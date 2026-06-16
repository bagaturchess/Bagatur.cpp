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
    double        move_time_secs = 0.0;     // for time-based modes; 0 = unbounded
};

// `colour_to_move`: 0 = WHITE, 1 = BLACK.
TCType      classify(const Go& go, int colour_to_move) noexcept;
TimeBudget  compute(const Go& go, int colour_to_move) noexcept;

}  // namespace uci
