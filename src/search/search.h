// search/search.h — PVS + NWS searcher.
//
// Faithful-to-spirit port of bagaturchess.search.impl.alg.impl1.Search_PVS_NWS,
// minus the SMP / UCI mediator / TB probing / correction histories /
// singular-extension / probcut layers, which would each pull in another
// thousand lines of Java infrastructure. Heuristics retained:
//
//   - PVS at PV nodes; null-window search at non-PV
//   - TT lookup with non-PV cutoffs (exact / lower / upper)
//   - Mate distance pruning
//   - Null-move pruning (with depth reduction)
//   - Static null-move (reverse futility)
//   - Razoring at shallow depths
//   - Late-move reductions (Bagatur LMR_TABLE shape)
//   - Check extension
//   - Move ordering phases: TT > captures > killers > history-quiets
//   - Killers (2 per ply), butterfly history
//   - Quiescence search at horizon
//
// Eval is delegated to the C++ NNUE evaluator.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "../board/chess_board.h"
#include "../board/move_generator.h"
#include "../nnue/nnue.h"
#include "history.h"
#include "tt.h"
#include "types.h"

namespace search {

struct Result;  // fwd

struct Limits {
    int            max_depth     = MAX_PLY - 1;
    std::uint64_t  max_nodes     = 0;             // 0 = unlimited

    // Dynamic time budget — 1:1 with Java's
    // TimeController_FloatingTime / MoveEvalInAccount /
    // ConsumedTimeVSRemainingTimeInAccount.
    //   available_time = min_move_secs + total_clock_secs × usage_pct_dyn
    //   usage_pct_dyn starts at 0, grows toward `max_usage_percent` as
    //   per-iteration eval/best-move volatility builds up.
    //
    // Iteration-boundary terminate gate (Java newIteration()):
    //   stop next iteration iff
    //     elapsed >= min_move_secs AND
    //     elapsed >  consumed_vs_remaining × available_time
    //
    // For FIXED_DEPTH / FIXED_NODES / INFINITE leave `min_move_secs = 0` to
    // disable the time machinery.
    double         min_move_secs           = 0.0;
    double         total_clock_secs        = 0.0;
    double         max_usage_percent       = 0.0;
    double         consumed_vs_remaining   = 0.50;

    // Root-search algorithm. When `true`, runs MTD(f) γ-stepping over null
    // windows. When `false`, runs the classic PVS iterative-deepening loop.
    bool           use_mtd       = true;

    // Optional: fired at every refined info (every ID iteration in PVS mode,
    // every tightened bound in MTD mode).
    using InfoCallback = void(*)(const Result&, void* user);
    InfoCallback   on_iteration  = nullptr;
    void*          callback_user = nullptr;
};

struct Result {
    int            score      = SCORE_DRAW;
    int            best_move  = 0;
    int            depth      = 0;
    int            seldepth   = 0;
    std::uint64_t  nodes      = 0;
    std::uint64_t  tbhits     = 0;   // Syzygy tablebase probe hits this search
    double         time_secs  = 0.0;
    std::array<int, MAX_PLY> pv{};
    int            pv_length  = 0;

    // MTD(f) reports the score as either a lower or upper bound after each
    // null-window probe. Mirrors Java's `info.setLowerBound/UpperBound`. UCI
    // convention: append `lowerbound`/`upperbound` to the info line; skip the
    // `pv` segment on upper-bound (the move that "failed least" is unreliable).
    bool           lower_bound = false;
    bool           upper_bound = false;
};

class Searcher {
public:
    // Owns its own transposition table (single-threaded / standalone use).
    Searcher(board::ChessBoard& cb, std::size_t tt_mb = 512);
    // Shares an externally-owned transposition table — used by the SMP search,
    // where every worker thread probes/stores into one lock-free shared table.
    Searcher(board::ChessBoard& cb, TranspositionTable& shared_tt);

    // Run iterative deepening up to the given limits. Returns the best move &
    // score found at the deepest fully completed iteration.
    Result go(const Limits& lim);

    // Request the running search to stop ASAP.
    void stop() noexcept { stop_.store(true, std::memory_order_relaxed); }

    // Re-bind the searcher to a new board (e.g. after a UCI `position`
    // command replaces the StateManager's board). Refreshes the NNUE
    // accumulators for the new position but PRESERVES the TT and eval
    // cache — they carry useful info across moves of a game.
    void set_board(board::ChessBoard& cb) {
        cb_ = &cb;
        eval_.reset(cb);
    }

    TranspositionTable& tt() noexcept { return *tt_; }

private:
    // Per-ply data used by the search routines.
    struct Stack {
        int  pv[MAX_PLY];
        int  pv_length = 0;
        int  static_eval = 0;
        int  killer1 = 0;
        int  killer2 = 0;
        // The move CHOSEN at this ply during the current traversal — written
        // by `try_move` before recursing, read by the child as its "previous
        // move" for continuation-history lookups. 0 = null-move ply / unset.
        int  current_move = 0;
    };

    // Top-level drivers.
    Result goPVS(const Limits& lim);   // classic iterative deepening
    Result goMTD(const Limits& lim);   // MTD(f) γ-stepping

    // Core recursive routines.
    int search(int ply, int depth, int alpha, int beta,
               bool is_pv, bool cut_node, bool use_sme = true);
    int qsearch(int ply, int alpha, int beta, bool is_pv);

    // Singular-move verification — Java private singular_move_search().
    // Returns the best score among all moves OTHER than `tt_move_excl`,
    // searched at a reduced depth with a perturbed hash so we never hit
    // the parent TT entry. Caller compares the result to `singular_beta`:
    //   < singular_beta              → TT move is singular, extend it
    //   ≥ singular_beta && > β       → multi-cut: ≥2 moves beat β, cut now
    //   ≥ singular_beta              → no extension (demote TT move)
    int singular_move_search(int ply, int depth, int alpha, int beta,
                             int tt_move_excl, bool cut_node);

    // Helpers
    int  evaluate();
    bool time_up() noexcept;
    void update_pv(int ply, int move);
    bool check_for_stop();
    int  score_capture(int move) const noexcept;
    void score_quiet_moves(int ply, int* moves, int* scores, int n);
    int  pick_next_quiet(int* moves, int* scores, int start, int n);

    // Non-owning pointer (was a reference; pointer so the StateManager can
    // re-bind to a fresh ChessBoard after every UCI `position` command
    // without dropping the searcher — and with it the 512 MB TT and the
    // 128 MB eval cache, which are expensive to allocate and useful to
    // carry across moves of a game).
    board::ChessBoard*  cb_;
    nnue::Evaluator     eval_;
    // `tt_` points at either `owned_tt_` (standalone) or an external shared
    // table (SMP). The lock-free TT makes shared concurrent access safe.
    std::unique_ptr<TranspositionTable> owned_tt_;
    TranspositionTable*                 tt_;
    HistoryTable        history_;
    CaptureHistory      cap_history_;
    ContinuationHistory cont_history_;
    Killers             killers_;

    // Per-ply data. Size is `MAX_PLY + 1` because `update_pv(ply, move)`
    // reads `stacks_[ply + 1].pv` to build the child PV chain — search() at
    // ply = MAX_PLY - 1 (last legal ply) needs stacks_[MAX_PLY] to exist.
    // Without the +1, deep extension chains (e.g. perpetual-check positions
    // where extensions keep `depth` from decrementing) segfault.
    std::array<Stack, MAX_PLY + 1> stacks_{};

    // One move-generator buffer per ply (caller doesn't recurse with the same
    // gen — MoveGenerator already supports startPly/endPly stacking).
    board::MoveGenerator gen_;

    // Running counters.
    std::uint64_t                       nodes_     = 0;
    std::uint64_t                       tb_hits_   = 0;
    std::uint64_t                       node_check_= 0;
    int                                 root_depth_= 0;
    int                                 sel_depth_ = 0;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t                       max_nodes_ = 0;
    std::atomic<bool>                   stop_{false};
    bool                                aborted_   = false;

    // --- Dynamic time budget (Java MoveEvalInAccount +
    //     ConsumedTimeVSRemainingTimeInAccount, mirror of) ---
    // Set once per `go()` from the Limits passed in.
    double  min_move_secs_         = 0.0;
    double  total_clock_secs_      = 0.0;
    double  max_usage_percent_     = 0.0;
    double  consumed_vs_remaining_ = 0.50;

    // Per-iteration tracking — Java MoveEvalInAccount fields. Reset on
    // every `go()`.
    int     vol_last_eval_       = 0;
    int     vol_last_move_       = 0;
    int     vol_last_depth_      = 0;
    double  vol_accum_score_diff_= 0.0;
    double  vol_usage_pct_       = 0.0;     // dynamic component, [0, max_usage_percent_]
    bool    terminate_search_    = false;   // set by iteration-boundary check

    // Mirror of MoveEvalInAccount.newPVLine() — called after each MTD
    // iteration that emitted a fresh `eval/best_move/depth`. Updates the
    // dynamic usage percent so that available_time can grow on volatile
    // searches.
    void update_volatility(int eval, int move, int depth) noexcept;

    // Current dynamic available time = min_move + total × usage_pct.
    double available_secs() const noexcept {
        return min_move_secs_ + total_clock_secs_ * vol_usage_pct_;
    }
};

}  // namespace search
