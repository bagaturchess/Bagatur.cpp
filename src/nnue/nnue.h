// NNUE evaluator — C++20 port of bagaturchess.nnue_v2.NNUE
// (used by bagaturchess.deeplearning.impl_nnue_v3.NNUEEvaluator).
//
// Network: bullet-style perspective NNUE with
//   - INPUT_BUCKET_SIZE       = 7    (king-square buckets)
//   - FEATURE_SIZE            = 768  (12 piece-classes × 64 squares)
//   - HIDDEN_SIZE             = 1536 (per accumulator)
//   - OUTPUT_BUCKETS          = 8    (piece-count buckets)
//   - SCALE / QA / QB         = 400 / 255 / 64
//
// Square-index convention conversion:
//   board layout (Bagatur):  H1=0, G1=1, ..., A1=7, ...
//   NNUE layout:             A1=0, B1=1, ...
// Translation is a single bit flip: `nnue_sq = board_sq ^ 7`
// (the file index is reversed and ranks are unchanged).
//
// Public API (in `board` namespace's sister `nnue` namespace):
//   Evaluator e;                       // loads network_bagatur.nnue once (lazy)
//   e.reset(board);                    // full refresh from a ChessBoard
//   e.after_make(board, move, side);   // call AFTER cb.doMove
//   e.after_unmake(board, move, side); // call AFTER cb.undoMove
//   int eval = e.evaluate(board);      // signed eval from side-to-move PoV

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../board/chess_board.h"

// C4324: "structure padded due to alignment specifier". Intentional — the
// alignas(64) on Accumulator (and transitively Evaluator::Frame) is there
// to keep the int16 lanes cache-line-aligned for SIMD.
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4324)
#endif

namespace nnue {

// Name of the SIMD kernel set the running CPU selected at startup
// ("AVX-512" / "AVX2" / "scalar"). Exposed for the UCI startup banner.
const char* active_simd_name();

inline constexpr int HIDDEN_SIZE       = 1536;
inline constexpr int FEATURE_SIZE      = 768;
inline constexpr int OUTPUT_BUCKETS    = 8;
inline constexpr int INPUT_BUCKET_SIZE = 7;
inline constexpr int SCALE             = 400;
inline constexpr int QA                = 255;
inline constexpr int QB                = 64;
inline constexpr int COLOR_STRIDE      = 64 * 6;
inline constexpr int PIECE_STRIDE      = 64;
inline constexpr int DIVISOR           = (32 + OUTPUT_BUCKETS - 1) / OUTPUT_BUCKETS;  // = 4

inline constexpr int WHITE = 0;
inline constexpr int BLACK = 1;

inline constexpr std::array<int, 64> INPUT_BUCKETS = {
    0, 0, 1, 1, 2, 2, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
};

// Network singleton.
//
// Default load order on first `instance()`:
//   1. If the binary was built with `BAGATUR_EMBEDDED_NETWORK` (CMake sees
//      `network_bagatur.nnue` in the project root), use the embedded blob.
//      This is the production path for distribution — no external file, no
//      working-directory surprises.
//   2. Otherwise fall back to `./network_bagatur.nnue` on disk.
//
// `load_from(path)` is a test-only override.
class Network {
public:
    static const Network& instance();

    // Loads from an explicit path (override). Use for tests.
    static void load_from(const std::string& path);

    // Loads from an in-memory blob (override). Use for tests.
    static void load_from_memory(const std::uint8_t* data, std::size_t size);

    // L1Weights stored flat: [(feature + bucket * FEATURE_SIZE) * HIDDEN_SIZE + j].
    const std::int16_t* l1_weights() const noexcept { return l1_weights_.data(); }
    const std::int16_t* l1_biases()  const noexcept { return l1_biases_.data(); }
    // L2Weights[bucket][i]
    const std::int32_t* l2_weights(int bucket) const noexcept { return l2_weights_int_[bucket].data(); }
    int                 output_bias(int bucket) const noexcept { return output_biases_[bucket]; }

private:
    Network() = default;
    void load(const std::string& path);
    void load_mem(const std::uint8_t* data, std::size_t size);

    std::vector<std::int16_t>                       l1_weights_;
    std::array<std::int16_t, HIDDEN_SIZE>           l1_biases_{};
    std::array<std::array<std::int32_t, HIDDEN_SIZE * 2>, OUTPUT_BUCKETS> l2_weights_int_{};
    std::array<std::int16_t, OUTPUT_BUCKETS>        output_biases_{};
};

// Per-perspective accumulator. The `values` array is the activations of
// the L1 hidden layer for one perspective.
struct alignas(64) Accumulator {
    std::int16_t values[HIDDEN_SIZE];
    int          bucket_index = -1;

    void reset_to_biases() noexcept;
    void set_bucket(int b) noexcept { bucket_index = b; }
};

// EvalCache — small open-addressed table keyed by the upper 32 bits of the
// zobrist key. The NNUE network is purely a function of the piece layout +
// side-to-move, which the zobrist key captures exactly, so caching the
// final eval by zobrist is safe and saves the entire SCReLU + L2 pass on
// repeated visits.
//
// Entries are 8 bytes (4-byte key fragment + 4-byte score). `INT_MIN` is
// reserved as the empty sentinel — a real NNUE eval cannot reach that.
class EvalCache {
public:
    static constexpr int DEFAULT_MB = 128;

    explicit EvalCache(std::size_t mb = DEFAULT_MB) { resize(mb); }

    void resize(std::size_t mb);
    void clear() noexcept;

    bool probe(std::uint64_t key, int& score) const noexcept {
        const Entry& e = table_[static_cast<std::size_t>(key) & mask_];
        if (e.score == EMPTY_SCORE) return false;
        if (e.key32 != static_cast<std::uint32_t>(key >> 32)) return false;
        score = e.score;
        return true;
    }

    void store(std::uint64_t key, int score) noexcept {
        Entry& e = table_[static_cast<std::size_t>(key) & mask_];
        e.key32  = static_cast<std::uint32_t>(key >> 32);
        e.score  = score;
    }

private:
    static constexpr std::int32_t EMPTY_SCORE = (std::numeric_limits<std::int32_t>::min)();

    struct Entry {
        std::uint32_t key32;
        std::int32_t  score;
    };
    static_assert(sizeof(Entry) == 8, "EvalCache entry should be 8 bytes");

    std::vector<Entry> table_;
    std::size_t        mask_ = 0;
};

// Evaluator — holds white + black accumulators and a small undo stack so
// incremental updates can be rolled back when the search backs out.
class Evaluator {
public:
    Evaluator();

    // Recompute both accumulators from scratch.
    void reset(const board::ChessBoard& cb);

    // Update accumulators after a move was applied.
    // `side_moved` is the colour of the side that just moved (== 1 - cb.colorToMove).
    void after_make(const board::ChessBoard& cb, int move, int side_moved);

    // Roll back the most recent `after_make`.
    void after_unmake(const board::ChessBoard& cb, int move, int side_moved);

    // Evaluate the current position. Returns signed centipawn-like value
    // from the side-to-move PoV.
    int evaluate(const board::ChessBoard& cb);

    // Access to the embedded eval cache (resize / clear / stats).
    EvalCache&       eval_cache() noexcept       { return eval_cache_; }
    const EvalCache& eval_cache() const noexcept { return eval_cache_; }

private:
    void full_refresh(const board::ChessBoard& cb);
    void apply_diff(int move, int side_moved, bool forward);

    // Helpers — operate on accumulators directly.
    void acc_add_both(Accumulator& w, int wf_add, Accumulator& b, int bf_add);
    void acc_sub_both(Accumulator& w, int wf_sub, Accumulator& b, int bf_sub);
    void acc_add_sub_both(Accumulator& w, int wf_add, int wf_sub,
                          Accumulator& b, int bf_add, int bf_sub);

    Accumulator white_acc_;
    Accumulator black_acc_;

    // Per-ply frame. The accumulator snapshots are only filled when the move
    // forces a full refresh (castling / EP / king bucket change); the common
    // path stores only `move` and replays the diff in reverse on unmake.
    static constexpr int MAX_STACK = 256;
    struct Frame {
        int          move        = 0;
        int          side_moved  = 0;
        bool         refresh     = false;
        Accumulator  snap_white;
        Accumulator  snap_black;
    };
    std::array<Frame, MAX_STACK> stack_;
    int                          stack_top_ = 0;

    EvalCache                    eval_cache_{};
};

// Helpers exposed for the unit tests / verification.
namespace detail {

constexpr int convert_square(int board_sq) noexcept { return board_sq ^ 7; }
constexpr int convert_piece(int board_piece) noexcept { return board_piece - 1; }  // PAWN(1)->0

constexpr int choose_output_bucket(int piece_count) noexcept {
    return (piece_count - 2) / DIVISOR;
}

constexpr int choose_input_bucket(int king_sq_nnue, int side) noexcept {
    return side == WHITE ? INPUT_BUCKETS[king_sq_nnue]
                         : INPUT_BUCKETS[king_sq_nnue ^ 0b111000];
}

constexpr int get_feature_index(int square_nnue, int piece_side,
                                int piece_type_nnue, int perspective) noexcept {
    return perspective == WHITE
        ? piece_side       * COLOR_STRIDE + piece_type_nnue * PIECE_STRIDE + square_nnue
        : (piece_side ^ 1) * COLOR_STRIDE + piece_type_nnue * PIECE_STRIDE + (square_nnue ^ 0b111000);
}

}  // namespace detail

}  // namespace nnue

#ifdef _MSC_VER
#  pragma warning(pop)
#endif
