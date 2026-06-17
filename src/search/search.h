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
    double         max_time_secs = 0.0;           // 0 = unlimited

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
    Searcher(board::ChessBoard& cb, std::size_t tt_mb = 512);

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

    TranspositionTable& tt() noexcept { return tt_; }

private:
    // Per-ply data used by the search routines.
    struct Stack {
        int  pv[MAX_PLY];
        int  pv_length = 0;
        int  static_eval = 0;
        int  killer1 = 0;
        int  killer2 = 0;
    };

    // Top-level drivers.
    Result goPVS(const Limits& lim);   // classic iterative deepening
    Result goMTD(const Limits& lim);   // MTD(f) γ-stepping

    // Core recursive routines.
    int search(int ply, int depth, int alpha, int beta, bool is_pv, bool cut_node);
    int qsearch(int ply, int alpha, int beta, bool is_pv);

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
    TranspositionTable  tt_;
    HistoryTable        history_;
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
    std::uint64_t                       node_check_= 0;
    int                                 root_depth_= 0;
    int                                 sel_depth_ = 0;
    std::chrono::steady_clock::time_point start_;
    double                              max_time_s_= 0.0;
    std::uint64_t                       max_nodes_ = 0;
    std::atomic<bool>                   stop_{false};
    bool                                aborted_   = false;
};

}  // namespace search
