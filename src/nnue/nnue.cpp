// nnue.cpp — implementation of the Bullet-style NNUE evaluator.
//
// The hot loops are written in AVX2 intrinsics when available; a scalar
// fallback is kept for portability and for verifying the SIMD results.

#include "nnue.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>

#include "../board/bitboard.h"
#include "../board/move_util.h"
#include "../board/types.h"

#if defined(__AVX2__)
#  include <immintrin.h>
#  define NNUE_HAVE_AVX2 1
#else
#  define NNUE_HAVE_AVX2 0
#endif

namespace nnue {

// ----------------------------------------------------------------------------
// Network singleton
// ----------------------------------------------------------------------------
namespace {

std::once_flag        g_network_loaded_flag;
std::unique_ptr<Network> g_network;

std::string default_network_path() {
    // network_bagatur.nnue is expected next to the executable, but for a
    // build directory we also fall back to the project-root copy.
    return "network_bagatur.nnue";
}

}  // anonymous

const Network& Network::instance() {
    std::call_once(g_network_loaded_flag, [] {
        g_network.reset(new Network());
        g_network->load(default_network_path());
    });
    return *g_network;
}

void Network::load_from(const std::string& path) {
    auto net = std::unique_ptr<Network>(new Network());
    net->load(path);
    // Force a single override — this is meant for tests.
    g_network = std::move(net);
}

namespace {

// Read a little-endian int16 from a streaming file. The Java loader does the
// equivalent (DataInputStream.readShort returns big-endian; the loader then
// calls `toLittleEndian` to swap to native LE). On x86 we just read raw.
inline std::int16_t read_le_i16(std::ifstream& f) {
    std::uint8_t b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(b[0]) |
                                     (static_cast<std::uint16_t>(b[1]) << 8));
}

}  // anonymous

[[noreturn]] static void die(const char* msg, const std::string& path) {
    std::fprintf(stderr, "nnue: %s '%s'\n", msg, path.c_str());
    std::abort();
}

void Network::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) die("cannot open", path);

    // L1Weights — flat, [feature + bucket * FEATURE_SIZE] indexed.
    l1_weights_.resize(static_cast<std::size_t>(FEATURE_SIZE) *
                       static_cast<std::size_t>(INPUT_BUCKET_SIZE) *
                       static_cast<std::size_t>(HIDDEN_SIZE));
    for (std::size_t i = 0; i < l1_weights_.size(); ++i)
        l1_weights_[i] = read_le_i16(f);

    // L1Biases
    for (int i = 0; i < HIDDEN_SIZE; ++i) l1_biases_[i] = read_le_i16(f);

    // L2Weights — interleaved storage in the file:
    //   for i in 0..HIDDEN_SIZE*2:
    //     for bucket in 0..OUTPUT_BUCKETS:
    //       read int16 -> L2Weights[bucket][i]
    // (Heap-allocated to keep the stack frame small.)
    auto l2_short = std::make_unique<std::array<std::array<std::int16_t, HIDDEN_SIZE * 2>, OUTPUT_BUCKETS>>();
    for (int i = 0; i < HIDDEN_SIZE * 2; ++i)
        for (int b = 0; b < OUTPUT_BUCKETS; ++b)
            (*l2_short)[b][i] = read_le_i16(f);

    for (int b = 0; b < OUTPUT_BUCKETS; ++b) output_biases_[b] = read_le_i16(f);

    // Widen L2 weights to int32 so the SIMD product code can keep everything
    // in int32 lanes without sign-extension every iteration.
    for (int b = 0; b < OUTPUT_BUCKETS; ++b)
        for (int i = 0; i < HIDDEN_SIZE * 2; ++i)
            l2_weights_int_[b][i] = (*l2_short)[b][i];

    if (!f.good()) die("short read on", path);
}

// ----------------------------------------------------------------------------
// Accumulator
// ----------------------------------------------------------------------------
void Accumulator::reset_to_biases() noexcept {
    const std::int16_t* src = Network::instance().l1_biases();
    std::memcpy(values, src, HIDDEN_SIZE * sizeof(std::int16_t));
}

// ----------------------------------------------------------------------------
// Vectorised L1 add / sub helpers
// ----------------------------------------------------------------------------
namespace {

inline int l1_offset(int feature_index, int bucket_index) noexcept {
    return (feature_index + bucket_index * FEATURE_SIZE) * HIDDEN_SIZE;
}

#if NNUE_HAVE_AVX2

// All three primitives operate on 16-lane int16 vectors (256-bit). The hidden
// layer is 1536 elements = 96 vectors, unrolled 4× per loop iteration.
inline void v_add(std::int16_t* __restrict v, const std::int16_t* __restrict w) {
    for (int i = 0; i < HIDDEN_SIZE; i += 16 * 4) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 16));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 32));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 48));
        v0 = _mm256_add_epi16(v0, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i)));
        v1 = _mm256_add_epi16(v1, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 16)));
        v2 = _mm256_add_epi16(v2, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 32)));
        v3 = _mm256_add_epi16(v3, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 48)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i),       v0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 16),  v1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 32),  v2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 48),  v3);
    }
}

inline void v_sub(std::int16_t* __restrict v, const std::int16_t* __restrict w) {
    for (int i = 0; i < HIDDEN_SIZE; i += 16 * 4) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 16));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 32));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 48));
        v0 = _mm256_sub_epi16(v0, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i)));
        v1 = _mm256_sub_epi16(v1, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 16)));
        v2 = _mm256_sub_epi16(v2, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 32)));
        v3 = _mm256_sub_epi16(v3, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 48)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i),       v0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 16),  v1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 32),  v2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 48),  v3);
    }
}

inline void v_add_sub(std::int16_t* __restrict v,
                      const std::int16_t* __restrict w_add,
                      const std::int16_t* __restrict w_sub) {
    for (int i = 0; i < HIDDEN_SIZE; i += 16 * 4) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 16));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 32));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 48));
        v0 = _mm256_add_epi16(v0, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_add + i)));
        v1 = _mm256_add_epi16(v1, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_add + i + 16)));
        v2 = _mm256_add_epi16(v2, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_add + i + 32)));
        v3 = _mm256_add_epi16(v3, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_add + i + 48)));
        v0 = _mm256_sub_epi16(v0, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_sub + i)));
        v1 = _mm256_sub_epi16(v1, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_sub + i + 16)));
        v2 = _mm256_sub_epi16(v2, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_sub + i + 32)));
        v3 = _mm256_sub_epi16(v3, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_sub + i + 48)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i),       v0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 16),  v1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 32),  v2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 48),  v3);
    }
}

// SCReLU + dot product. Returns the sum across HIDDEN_SIZE for one accumulator.
inline std::int64_t v_screlu_dot(const std::int16_t* __restrict acc,
                                 const std::int32_t* __restrict weights) {
    const __m256i lo  = _mm256_setzero_si256();
    const __m256i hi  = _mm256_set1_epi16(static_cast<std::int16_t>(QA));

    __m256i s0 = _mm256_setzero_si256();
    __m256i s1 = _mm256_setzero_si256();

    for (int i = 0; i < HIDDEN_SIZE; i += 16) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + i));
        v = _mm256_max_epi16(v, lo);
        v = _mm256_min_epi16(v, hi);

        // Widen int16 -> int32 in two halves.
        __m128i v_lo = _mm256_castsi256_si128(v);
        __m128i v_hi = _mm256_extracti128_si256(v, 1);
        __m256i a0   = _mm256_cvtepi16_epi32(v_lo);
        __m256i a1   = _mm256_cvtepi16_epi32(v_hi);

        // Square (max 255^2 = 65025, fits in int32).
        __m256i sq0 = _mm256_mullo_epi32(a0, a0);
        __m256i sq1 = _mm256_mullo_epi32(a1, a1);

        __m256i w0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));
        __m256i w1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i + 8));

        s0 = _mm256_add_epi32(s0, _mm256_mullo_epi32(sq0, w0));
        s1 = _mm256_add_epi32(s1, _mm256_mullo_epi32(sq1, w1));
    }

    __m256i s = _mm256_add_epi32(s0, s1);

    // Horizontal reduce to int64 to avoid int32 overflow if the network grows.
    std::int32_t tmp[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(tmp), s);
    std::int64_t total = 0;
    for (int k = 0; k < 8; ++k) total += tmp[k];
    return total;
}

#else  // NNUE_HAVE_AVX2 == 0

inline void v_add(std::int16_t* v, const std::int16_t* w) {
    for (int i = 0; i < HIDDEN_SIZE; ++i) v[i] = static_cast<std::int16_t>(v[i] + w[i]);
}
inline void v_sub(std::int16_t* v, const std::int16_t* w) {
    for (int i = 0; i < HIDDEN_SIZE; ++i) v[i] = static_cast<std::int16_t>(v[i] - w[i]);
}
inline void v_add_sub(std::int16_t* v, const std::int16_t* a, const std::int16_t* s) {
    for (int i = 0; i < HIDDEN_SIZE; ++i)
        v[i] = static_cast<std::int16_t>(v[i] + a[i] - s[i]);
}
inline std::int64_t v_screlu_dot(const std::int16_t* acc, const std::int32_t* weights) {
    std::int64_t s = 0;
    for (int i = 0; i < HIDDEN_SIZE; ++i) {
        int v = acc[i];
        if (v < 0) v = 0;
        else if (v > QA) v = QA;
        s += static_cast<std::int64_t>(v) * v * weights[i];
    }
    return s;
}

#endif

}  // anonymous

// ----------------------------------------------------------------------------
// Evaluator implementation
// ----------------------------------------------------------------------------
Evaluator::Evaluator() {
    Network::instance();  // trigger lazy load now so we don't pay on first eval
}

void Evaluator::acc_add_both(Accumulator& w, int wf_add, Accumulator& b, int bf_add) {
    const auto* base = Network::instance().l1_weights();
    v_add(w.values, base + l1_offset(wf_add, w.bucket_index));
    v_add(b.values, base + l1_offset(bf_add, b.bucket_index));
}

void Evaluator::acc_sub_both(Accumulator& w, int wf_sub, Accumulator& b, int bf_sub) {
    const auto* base = Network::instance().l1_weights();
    v_sub(w.values, base + l1_offset(wf_sub, w.bucket_index));
    v_sub(b.values, base + l1_offset(bf_sub, b.bucket_index));
}

void Evaluator::acc_add_sub_both(Accumulator& w, int wf_add, int wf_sub,
                                 Accumulator& b, int bf_add, int bf_sub) {
    const auto* base = Network::instance().l1_weights();
    v_add_sub(w.values,
              base + l1_offset(wf_add, w.bucket_index),
              base + l1_offset(wf_sub, w.bucket_index));
    v_add_sub(b.values,
              base + l1_offset(bf_add, b.bucket_index),
              base + l1_offset(bf_sub, b.bucket_index));
}

// ----------------------------------------------------------------------------
// Full refresh — walk the board's piece bitboards, build feature indices,
// and add each one to the white & black accumulators. Mirrors the Java
// NNUEProbeUtils.fillInput + Accumulators.fullAccumulatorUpdate pair.
// ----------------------------------------------------------------------------
void Evaluator::full_refresh(const board::ChessBoard& cb) {
    int wk = board::trailing_zeros(cb.pieces[board::WHITE][board::KING]) ^ 7;
    int bk = board::trailing_zeros(cb.pieces[board::BLACK][board::KING]) ^ 7;

    white_acc_.reset_to_biases();
    black_acc_.reset_to_biases();
    white_acc_.set_bucket(detail::choose_input_bucket(wk, WHITE));
    black_acc_.set_bucket(detail::choose_input_bucket(bk, BLACK));

    auto add_pieces = [&](int board_color, int board_piece) {
        int nnue_color = board_color;                 // WHITE/BLACK constants match
        int nnue_piece = board_piece - 1;             // PAWN(1)..KING(6) → 0..5
        board::BB bb = cb.pieces[board_color][board_piece];
        while (bb) {
            int board_sq = board::trailing_zeros(bb);
            int sq_nnue  = board_sq ^ 7;
            acc_add_both(white_acc_,
                         detail::get_feature_index(sq_nnue, nnue_color, nnue_piece, WHITE),
                         black_acc_,
                         detail::get_feature_index(sq_nnue, nnue_color, nnue_piece, BLACK));
            bb &= bb - 1;
        }
    };

    for (int color = board::WHITE; color <= board::BLACK; ++color)
        for (int piece = board::PAWN; piece <= board::KING; ++piece)
            add_pieces(color, piece);
}

void Evaluator::reset(const board::ChessBoard& cb) {
    full_refresh(cb);
    stack_top_ = 0;
}

// ----------------------------------------------------------------------------
// Incremental updates around make/unmake.
//
// Common case (quiet move, capture, promotion): apply the diff forward on
// make and again — with add/sub swapped — on unmake. No per-ply copy.
//
// Refresh case (castling / EP / king crosses input-bucket boundary):
// snapshot both accumulators before the refresh and restore on unmake.
// ----------------------------------------------------------------------------
void Evaluator::apply_diff(int move, int side_moved, bool forward) {
    using namespace board;

    int from_n  = mv::from_index(move) ^ 7;
    int to_n    = mv::to_index(move)   ^ 7;
    int piece_n = mv::source_piece_index(move) - 1;
    int cap     = mv::attacked_piece_index(move);
    int mover   = side_moved;
    int op      = 1 - mover;
    bool is_promo = mv::is_promotion(move);

    if (is_promo) {
        int promo_n   = mv::move_type(move) - 1;
        int wf_pawn   = detail::get_feature_index(from_n, mover, 0, WHITE);
        int bf_pawn   = detail::get_feature_index(from_n, mover, 0, BLACK);
        int wf_promo  = detail::get_feature_index(to_n,   mover, promo_n, WHITE);
        int bf_promo  = detail::get_feature_index(to_n,   mover, promo_n, BLACK);
        if (forward) {
            acc_sub_both(white_acc_, wf_pawn,  black_acc_, bf_pawn);
            acc_add_both(white_acc_, wf_promo, black_acc_, bf_promo);
        } else {
            acc_add_both(white_acc_, wf_pawn,  black_acc_, bf_pawn);
            acc_sub_both(white_acc_, wf_promo, black_acc_, bf_promo);
        }
        if (cap != 0) {
            int cap_n  = cap - 1;
            int wf_cap = detail::get_feature_index(to_n, op, cap_n, WHITE);
            int bf_cap = detail::get_feature_index(to_n, op, cap_n, BLACK);
            if (forward) acc_sub_both(white_acc_, wf_cap, black_acc_, bf_cap);
            else         acc_add_both(white_acc_, wf_cap, black_acc_, bf_cap);
        }
        return;
    }

    int wf_from = detail::get_feature_index(from_n, mover, piece_n, WHITE);
    int bf_from = detail::get_feature_index(from_n, mover, piece_n, BLACK);
    int wf_to   = detail::get_feature_index(to_n,   mover, piece_n, WHITE);
    int bf_to   = detail::get_feature_index(to_n,   mover, piece_n, BLACK);

    if (forward) {
        acc_add_sub_both(white_acc_, wf_to, wf_from, black_acc_, bf_to, bf_from);
    } else {
        acc_add_sub_both(white_acc_, wf_from, wf_to, black_acc_, bf_from, bf_to);
    }

    if (cap != 0) {
        int cap_n  = cap - 1;
        int wf_cap = detail::get_feature_index(to_n, op, cap_n, WHITE);
        int bf_cap = detail::get_feature_index(to_n, op, cap_n, BLACK);
        if (forward) acc_sub_both(white_acc_, wf_cap, black_acc_, bf_cap);
        else         acc_add_both(white_acc_, wf_cap, black_acc_, bf_cap);
    }
}

void Evaluator::after_make(const board::ChessBoard& cb, int move, int side_moved) {
    using namespace board;

    Frame& frame   = stack_[stack_top_++];
    frame.move     = move;
    frame.side_moved = side_moved;

    bool is_castling = mv::is_castling(move);
    bool is_ep       = mv::is_ep(move);
    int  piece       = mv::source_piece_index(move);
    int  from_n      = mv::from_index(move) ^ 7;
    int  to_n        = mv::to_index(move)   ^ 7;

    bool king_bucket_change = false;
    if (piece == KING) {
        king_bucket_change =
            detail::choose_input_bucket(from_n, side_moved) !=
            detail::choose_input_bucket(to_n,   side_moved);
    }

    if (is_castling || is_ep || king_bucket_change) {
        frame.snap_white = white_acc_;
        frame.snap_black = black_acc_;
        frame.refresh    = true;
        full_refresh(cb);
        return;
    }

    frame.refresh = false;
    apply_diff(move, side_moved, /*forward=*/true);
}

void Evaluator::after_unmake(const board::ChessBoard& /*cb*/, int /*move*/, int /*side_moved*/) {
    if (stack_top_ == 0) return;
    Frame& f = stack_[--stack_top_];
    if (f.refresh) {
        white_acc_ = f.snap_white;
        black_acc_ = f.snap_black;
    } else {
        apply_diff(f.move, f.side_moved, /*forward=*/false);
    }
}

// ----------------------------------------------------------------------------
// Final eval: SCReLU + dot product through L2.
// ----------------------------------------------------------------------------
int Evaluator::evaluate(const board::ChessBoard& cb) {
    int piece_count = board::popcount(cb.allPieces);
    int bucket      = detail::choose_output_bucket(piece_count);

    const Accumulator& us   = (cb.colorToMove == board::WHITE) ? white_acc_ : black_acc_;
    const Accumulator& them = (cb.colorToMove == board::WHITE) ? black_acc_ : white_acc_;

    const auto& net = Network::instance();
    const std::int32_t* w_us   = net.l2_weights(bucket);
    const std::int32_t* w_them = w_us + HIDDEN_SIZE;

    std::int64_t sum = v_screlu_dot(us.values,   w_us) + v_screlu_dot(them.values, w_them);

    sum /= QA;
    sum += net.output_bias(bucket);
    sum *= SCALE;
    sum /= (QA * QB);
    return static_cast<int>(sum);
}

}  // namespace nnue
