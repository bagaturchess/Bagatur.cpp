// Time controllers — minimal port of
// bagaturchess.search.impl.uci_adaptor.timemanagement.TimeControllerFactory
// and the seven `TimeController_*` classes it dispatches.
//
// The detailed Java machinery — emergency factors, move-eval feedback,
// consumed-vs-remaining account etc. — is collapsed into a single budget
// computation. The branching is identical to TimeControllerFactory.getControllerType
// (line 57 of the Java) so cutechess-style time controls behave the same.

#pragma once

#include <cstdint>

#include "go.h"

namespace uci {

enum class TCType {
    INFINITE,
    FIXED_DEPTH,
    FIXED_NODES,
    TIME_PER_MOVE,
    INCREMENT_PER_MOVE,
    TOURNAMENT,
    SUDDEN_DEATH,
};

struct TimeBudget {
    TCType        type;
    int           depth_limit    = 0;       // for FIXED_DEPTH; 0 = unbounded
    std::int64_t  node_limit     = 0;       // for FIXED_NODES; 0 = unbounded

    // Dynamic time budget — mirrors Java's
    // TimeController_FloatingTime / MoveEvalInAccount /
    // ConsumedTimeVSRemainingTimeInAccount.
    //
    // Within one search:
    //   available = min_move_secs + total_clock_secs × usage_percent_dyn
    // `usage_percent_dyn` starts at 0 and grows up to `max_usage_percent`
    // as the searcher's per-iteration eval/best-move volatility builds up.
    //
    // Iteration-boundary terminate gate (newIteration() in Java):
    //   stop next iteration iff
    //     elapsed >= min_move_secs AND
    //     elapsed >  consumed_vs_remaining_pct × available
    //
    // For TIME_PER_MOVE / FIXED_DEPTH / FIXED_NODES / INFINITE the dynamic
    // mechanism collapses (max_usage_percent = 0 → available stays at
    // min_move_secs).
    double        min_move_secs           = 0.0;   // 0 = no time cap (depth/nodes/infinite)
    double        total_clock_secs        = 0.0;   // hard ceiling
    double        max_usage_percent       = 0.0;   // 0 / 0.125 (SuddenDeath) / 0.20 (default)
    double        consumed_vs_remaining   = 0.50;  // Java's `timeoptimization_cfg_consumed_time_vs_remaining_time`
};

// `colour_to_move`: 0 = WHITE, 1 = BLACK.
TCType      classify(const Go& go, int colour_to_move) noexcept;
TimeBudget  compute(const Go& go, int colour_to_move) noexcept;

}  // namespace uci
