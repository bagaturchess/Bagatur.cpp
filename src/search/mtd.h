// MTD(f) γ-stepping bounds management.
//
// Direct port of:
//   bagaturchess.search.impl.rootsearch.sequential.mtd.IBetaGenerator
//   bagaturchess.search.impl.rootsearch.sequential.mtd.BetaGenerator
//   bagaturchess.search.impl.rootsearch.sequential.mtd.SearchManager
//
// Behaviour preserved 1:1 — same exponential trend multiplier, same "isLast"
// convergence condition with trustWindow, same finish-depth-on-converged.
// What's omitted (kept for a possible v2):
//   - dynamic TRUST_WINDOW_BEST_MOVE adjustment (this port uses the constant
//     minimum value of 8, matching Bagatur's initial setting). The Java code
//     lets it expand up to 16 when best_move is stable across depths.
//   - thread-safety / executor wrapping (single-threaded port).

#pragma once

#include <vector>

#include "types.h"

namespace search {

// IBetaGenerator equivalent — single concrete class, no virtual interface
// since we never swap implementations.
class BetaGenerator {
public:
    static constexpr int MAX_TREND_MULTIPLIER = 1'000'000;
    static constexpr int TREND_INIT =  0;
    static constexpr int TREND_UP   =  1;
    static constexpr int TREND_DOWN = -1;

    BetaGenerator(int initial_val, int betas_count, int initial_interval);

    void decreaseUpper(int val);
    void increaseLower(int val);

    // Returns a fresh list of `betas_count_` beta candidates appropriate for
    // the current state of the [lower, upper] bounds.
    std::vector<int> genBetas() const;

    int  getLowerBound() const noexcept { return lower_bound_; }
    int  getUpperBound() const noexcept { return upper_bound_; }
    int  getInitialInterval() const noexcept { return initial_interval_; }

private:
    int betas_count_;
    int initial_interval_;
    int lower_bound_;
    int upper_bound_;
    int trend_;
    int trend_multiplier_;
    int last_val_;
};


// SearchManager equivalent — sequencer for iterative deepening + bound
// narrowing. Tracks the current depth and the active BetaGenerator.
class MTDSearchManager {
public:
    static constexpr int TRUST_WINDOW_BEST_MOVE = 8;   // Java: TRUST_WINDOW_BEST_MOVE_MIN
    static constexpr int TRUST_WINDOW_MTD_STEP  = 8;   // Java: TRUST_WINDOW_MTD_STEP_MIN
    static constexpr int BETAS_COUNT            = 1;   // sequential (Java's BetaGeneratorFactory)

    MTDSearchManager(int start_depth, int max_iterations, int initial_value);

    // Pop the next beta to search. If the list is exhausted (search "instability"
    // — Java line 167) we regenerate via updateBetas() to recover.
    int  nextBeta();

    // Called after a null-window search at the current depth completes.
    //   eval >= beta  → lower bound (fail-high)
    //   eval <  beta  → upper bound (fail-low)
    // Returns true if the depth is now considered converged (caller should
    // record this as the iteration-complete signal for `info`).
    bool increaseLowerBound(int eval);
    bool decreaseUpperBound(int eval);

    bool isLast() const;

    int  getCurrentDepth()    const noexcept { return current_depth_; }
    int  getMaxIterations()   const noexcept { return max_iterations_; }
    int  getLowerBound()      const noexcept { return betas_gen_.getLowerBound(); }
    int  getUpperBound()      const noexcept { return betas_gen_.getUpperBound(); }

private:
    void initBetas();
    void updateBetas();
    void finishDepth();

    int               current_depth_;
    int               max_iterations_;
    int               initial_value_;
    BetaGenerator     betas_gen_;
    std::vector<int>  betas_;
};

}  // namespace search
