#include "time_control.h"

#include <algorithm>

namespace uci {

namespace {

constexpr int    BAGATUR_DIVIDE_FACTOR_DEFAULT = 35;   // FloatingTime line 36
constexpr double TIME_BUFFER_PERCENT           = 0.10;  // FloatingTime line 38
constexpr double TIME_BUFFER_GAME_MS           = 5000;  // FloatingTime line 39
constexpr double TIME_BUFFER_MOVE_MS           = 1000;  // FloatingTime line 40

// SuddenDeath emergency factor — TimeController_SuddenDeath line 41-68.
// Linear interpolation: 3× at 0 ms remaining, 1× at 20 000 ms+.
double sudden_death_emergency_factor(double time_ms) noexcept {
    if (time_ms <= 0) return 3.0;
    double a = (3.0 - 1.0) / (0.0 - 20000.0);
    double b = 3.0 - a * 0.0;
    double f = a * time_ms + b;
    return f < 1.0 ? 1.0 : f;
}

// Per-TC `max_usage_percent` — taken from each Java time-controller's
// `TimeConfig` (`getMoveEvallDiff_MaxTotalTimeUsagePercent()`).
double max_usage_for(TCType type) noexcept {
    switch (type) {
        case TCType::SUDDEN_DEATH:        return 0.125;  // TimeConfig_SuddenDeath line 110
        case TCType::INCREMENT_PER_MOVE:  return 0.20;   // FloatingTime/MoveEvalInAccount default
        case TCType::TOURNAMENT:          return 0.20;
        default:                          return 0.0;    // fixed: no dynamic expansion
    }
}

// Components for the dynamic-budget model:
//   - min_move_ms = guaranteed search time (Java's `minMoveTime`)
//   - total_ms    = absolute clock ceiling for this move (Java's `totalClockTime`)
//
// Returned by reference; callers fill the rest of TimeBudget.
//
// For TIME_PER_MOVE: min == total == movetime so the dynamic component
// stays zero and we behave like a fixed budget.
void per_move_budget_components(TCType type, const Go& go, int colour,
                                double& min_move_ms, double& total_ms) noexcept {
    bool is_white = (colour == 0);
    double clock_ms = static_cast<double>(is_white ? go.wtime : go.btime);
    double inc_ms   = static_cast<double>(is_white ? go.winc  : go.binc);

    // FloatingTime line 53-63: shave a small buffer off the clock + increment.
    double clock_buf = std::min(TIME_BUFFER_GAME_MS, clock_ms * TIME_BUFFER_PERCENT);
    double inc_buf   = std::min(TIME_BUFFER_MOVE_MS, inc_ms   * TIME_BUFFER_PERCENT);
    double clock     = std::max(0.0, clock_ms - clock_buf);
    double inc       = std::max(0.0, inc_ms   - inc_buf);

    switch (type) {
        case TCType::TIME_PER_MOVE: {
            double mt = static_cast<double>(go.movetime);
            double budget = std::max(1.0, mt - std::min(50.0, mt * 0.05));
            min_move_ms = budget;
            total_ms    = budget;       // hard cap == budget; no expansion
            return;
        }
        case TCType::TOURNAMENT: {
            int    mtg  = std::max(1, go.movestogo);
            double base = clock / static_cast<double>(mtg + 1);
            min_move_ms = std::max(1.0, base + inc);
            total_ms    = clock;        // whole clock segment for these `movestogo` moves
            return;
        }
        case TCType::INCREMENT_PER_MOVE: {
            // FloatingTime.setupMinMoveTimeAndTotalClockTime() — emergency=1.
            double base = clock / BAGATUR_DIVIDE_FACTOR_DEFAULT;
            if (clock >= BAGATUR_DIVIDE_FACTOR_DEFAULT * inc) {
                min_move_ms = std::max(base, inc);
            } else {
                min_move_ms = std::min(base, inc);
            }
            min_move_ms = std::max(1.0, min_move_ms);
            total_ms    = clock;        // Java's `totalClockTime` for floating-time
            return;
        }
        case TCType::SUDDEN_DEATH: {
            double emergency = sudden_death_emergency_factor(clock);
            double base      = clock / (BAGATUR_DIVIDE_FACTOR_DEFAULT * emergency);
            double total     = clock / emergency;
            min_move_ms = std::max(1.0, base);
            total_ms    = std::max(min_move_ms, total);
            return;
        }
        default:
            min_move_ms = 0.0;
            total_ms    = 0.0;
            return;
    }
}

}  // namespace

TCType classify(const Go& go, int colour_to_move) noexcept {
    // Same priority order as TimeControllerFactory.getControllerType line 57-74.
    if (go.isAnalyzingMode())   return TCType::INFINITE;
    if (go.hasDepth())          return TCType::FIXED_DEPTH;
    if (go.hasNodes())          return TCType::FIXED_NODES;
    if (go.movetime != Go::UNDEF_MOVETIME) return TCType::TIME_PER_MOVE;

    bool is_white = (colour_to_move == 0);
    std::int64_t inc = is_white ? go.winc : go.binc;
    if (inc > 0)                return TCType::INCREMENT_PER_MOVE;
    if (go.movestogo != Go::UNDEF_MOVESTOGO) return TCType::TOURNAMENT;
    return TCType::SUDDEN_DEATH;
}

TimeBudget compute(const Go& go, int colour_to_move) noexcept {
    TimeBudget b;
    b.type = classify(go, colour_to_move);

    switch (b.type) {
        case TCType::INFINITE:
            // Engine runs until "stop". Driver passes no time/node/depth limits.
            break;
        case TCType::FIXED_DEPTH:
            b.depth_limit = go.depth;
            break;
        case TCType::FIXED_NODES:
            b.node_limit = go.nodes;
            break;
        case TCType::TIME_PER_MOVE:
        case TCType::INCREMENT_PER_MOVE:
        case TCType::TOURNAMENT:
        case TCType::SUDDEN_DEATH: {
            double min_move_ms = 0.0, total_ms = 0.0;
            per_move_budget_components(b.type, go, colour_to_move,
                                      min_move_ms, total_ms);
            b.min_move_secs         = min_move_ms / 1000.0;
            b.total_clock_secs      = total_ms    / 1000.0;
            b.max_usage_percent     = max_usage_for(b.type);
            // `consumed_vs_remaining` keeps its 0.50 default from the struct.
            break;
        }
    }
    return b;
}

}  // namespace uci
