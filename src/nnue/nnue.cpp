// nnue.cpp — implementation of the Bullet-style NNUE evaluator.
//
// The hot loops (accumulator add/sub and the SCReLU + dot output pass) have
// three implementations, one per file:
//   - AVX-512 (32 int16 lanes/vector) — nnue_kernels_avx512.cpp, fastest
//   - AVX2    (16 int16 lanes/vector) — nnue_kernels_avx2.cpp,   portable floor
//   - scalar                          — nnue_kernels_scalar.cpp, fallback/ref
// Selection is at RUNTIME by CPUID (nnue::kernels::active), so one distributable
// binary runs on any x86-64 CPU yet still uses AVX-512 where the host has it.
// This file calls the chosen set through function pointers.

#include "nnue.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <istream>
#include <mutex>
#include <streambuf>

#include "../board/bitboard.h"
#include "../board/move_util.h"
#include "../board/types.h"
#include "nnue_kernels.h"  // runtime-dispatched SIMD primitives (AVX-512/AVX2/scalar)

namespace nnue {

// ----------------------------------------------------------------------------
// Network singleton
// ----------------------------------------------------------------------------
namespace {

std::once_flag        g_network_loaded_flag;
std::unique_ptr<Network> g_network;

// Pre-loaded buffer pinned by `Network::load_from_memory(...)` before the
// first `instance()` call. When set, `instance()` uses the embedded blob
// instead of falling back to the file path.
const std::uint8_t*   g_embedded_data = nullptr;
std::size_t           g_embedded_size = 0;

std::string default_network_path() {
    return "network_bagatur.nnue";
}

// Read a little-endian int16. `std::istream&` lets the same parser serve
// `std::ifstream` (file path) and a zero-copy `imemstream` (embedded blob).
inline std::int16_t read_le_i16(std::istream& f) {
    std::uint8_t b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(b[0]) |
                                     (static_cast<std::uint16_t>(b[1]) << 8));
}

// Zero-copy istream over a contiguous byte buffer. We only need read()
// support — no putback, no seek beyond range — so the minimal setg() is
// enough.
struct imembuf : std::streambuf {
    imembuf(const std::uint8_t* p, std::size_t n) {
        char* base = const_cast<char*>(reinterpret_cast<const char*>(p));
        setg(base, base, base + n);
    }
};

void parse(std::istream& f, Network& net,
           std::vector<std::int16_t>&  l1_weights,
           std::array<std::int16_t, HIDDEN_SIZE>& l1_biases,
           std::array<std::array<std::int32_t, HIDDEN_SIZE * 2>, OUTPUT_BUCKETS>& l2_weights_int,
           std::array<std::int16_t, OUTPUT_BUCKETS>& output_biases) {
    (void)net;  // unused — accessor by reference for symmetry

    // L1Weights — flat, [feature + bucket * FEATURE_SIZE] indexed.
    l1_weights.resize(static_cast<std::size_t>(FEATURE_SIZE) *
                      static_cast<std::size_t>(INPUT_BUCKET_SIZE) *
                      static_cast<std::size_t>(HIDDEN_SIZE));
    for (std::size_t i = 0; i < l1_weights.size(); ++i)
        l1_weights[i] = read_le_i16(f);

    // L1Biases
    for (int i = 0; i < HIDDEN_SIZE; ++i) l1_biases[i] = read_le_i16(f);

    // L2Weights — interleaved storage in the file:
    //   for i in 0..HIDDEN_SIZE*2:
    //     for bucket in 0..OUTPUT_BUCKETS:
    //       read int16 -> L2Weights[bucket][i]
    auto l2_short = std::make_unique<std::array<std::array<std::int16_t, HIDDEN_SIZE * 2>, OUTPUT_BUCKETS>>();
    for (int i = 0; i < HIDDEN_SIZE * 2; ++i)
        for (int b = 0; b < OUTPUT_BUCKETS; ++b)
            (*l2_short)[b][i] = read_le_i16(f);

    for (int b = 0; b < OUTPUT_BUCKETS; ++b) output_biases[b] = read_le_i16(f);

    // Widen L2 weights to int32 so the SIMD product code can keep everything
    // in int32 lanes without sign-extension every iteration.
    for (int b = 0; b < OUTPUT_BUCKETS; ++b)
        for (int i = 0; i < HIDDEN_SIZE * 2; ++i)
            l2_weights_int[b][i] = (*l2_short)[b][i];
}

}  // anonymous

const Network& Network::instance() {
    std::call_once(g_network_loaded_flag, [] {
        g_network.reset(new Network());
        if (g_embedded_data != nullptr) {
            g_network->load_mem(g_embedded_data, g_embedded_size);
        } else {
            g_network->load(default_network_path());
        }
    });
    return *g_network;
}

void Network::load_from(const std::string& path) {
    auto net = std::unique_ptr<Network>(new Network());
    net->load(path);
    g_network = std::move(net);
}

// Two roles:
//   1. Called before any `instance()` call (e.g. from main()): pins the
//      buffer so the lazy load uses it. The exe stays self-contained.
//   2. Called after instance() already initialised (tests, late swap):
//      replaces the live network with the new buffer.
void Network::load_from_memory(const std::uint8_t* data, std::size_t size) {
    g_embedded_data = data;
    g_embedded_size = size;
    if (g_network) {
        g_network->load_mem(data, size);
    }
}

[[noreturn]] static void die(const char* msg, const std::string& path) {
    std::fprintf(stderr, "nnue: %s '%s'\n", msg, path.c_str());
    std::abort();
}

void Network::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) die("cannot open", path);
    parse(f, *this, l1_weights_, l1_biases_, l2_weights_int_, output_biases_);
    if (!f.good()) die("short read on", path);
}

void Network::load_mem(const std::uint8_t* data, std::size_t size) {
    imembuf buf(data, size);
    std::istream f(&buf);
    parse(f, *this, l1_weights_, l1_biases_, l2_weights_int_, output_biases_);
    if (!f.good()) die("short read on", "<embedded>");
}

// ----------------------------------------------------------------------------
// Accumulator
// ----------------------------------------------------------------------------
void Accumulator::reset_to_biases() noexcept {
    const std::int16_t* src = Network::instance().l1_biases();
    std::memcpy(values, src, HIDDEN_SIZE * sizeof(std::int16_t));
}

// ----------------------------------------------------------------------------
// EvalCache
// ----------------------------------------------------------------------------
namespace {
inline std::size_t round_down_pow2(std::size_t v) noexcept {
    std::size_t r = 1;
    while ((r << 1) <= v) r <<= 1;
    return r;
}
}  // namespace

void EvalCache::resize(std::size_t mb) {
    std::size_t bytes   = mb * (1ull << 20);
    std::size_t entries = round_down_pow2(bytes / sizeof(Entry));
    if (entries < 1024) entries = 1024;
    table_.assign(entries, Entry{0, EMPTY_SCORE});
    mask_ = entries - 1;
}

void EvalCache::clear() noexcept {
    for (Entry& e : table_) { e.key32 = 0; e.score = EMPTY_SCORE; }
}

// ----------------------------------------------------------------------------
// L1 add / sub helpers — thin wrappers over the runtime-selected SIMD kernels
// ----------------------------------------------------------------------------
namespace {

inline int l1_offset(int feature_index, int bucket_index) noexcept {
    return (feature_index + bucket_index * FEATURE_SIZE) * HIDDEN_SIZE;
}

// The active SIMD kernel set (AVX-512 / AVX2 / scalar), chosen once by CPUID at
// static-init time. Each primitive sweeps the whole HIDDEN_SIZE lane array, so
// dispatching through a function pointer costs one indirect call amortized over
// 1536 lanes — negligible — while keeping the binary portable. See nnue_kernels.h.
const kernels::Kernels g_kernels = kernels::active();

inline void v_add(std::int16_t* v, const std::int16_t* w) {
    g_kernels.v_add(v, w);
}
inline void v_sub(std::int16_t* v, const std::int16_t* w) {
    g_kernels.v_sub(v, w);
}
inline void v_add_sub(std::int16_t* v, const std::int16_t* w_add, const std::int16_t* w_sub) {
    g_kernels.v_add_sub(v, w_add, w_sub);
}
inline std::int64_t v_screlu_dot(const std::int16_t* acc, const std::int32_t* weights) {
    return g_kernels.v_screlu_dot(acc, weights);
}

}  // anonymous

// Name of the SIMD kernel set the running CPU selected (for the startup banner).
const char* active_simd_name() { return g_kernels.name; }

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
// Goes through the embedded EvalCache — if the position's zobrist is already
// in the cache, the entire SCReLU + L2 pass is skipped.
// ----------------------------------------------------------------------------
int Evaluator::evaluate(const board::ChessBoard& cb) {
    int cached;
    if (eval_cache_.probe(cb.zobristKey, cached)) return cached;

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
    int eval = static_cast<int>(sum);

    eval_cache_.store(cb.zobristKey, eval);
    return eval;
}

}  // namespace nnue
