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

// Compute the per-move budget in milliseconds from the white/black clock
// shape that the FloatingTime / SuddenDeath / IncrementPerMove / Tournament
// chain produces. We collapse all of them onto a single budget for the
// driver — full fidelity tracking is not needed for cutechess.
double per_move_budget_ms(TCType type, const Go& go, int colour) noexcept {
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
            // Use raw movetime with a small safety margin.
            double mt = static_cast<double>(go.movetime);
            return std::max(1.0, mt - std::min(50.0, mt * 0.05));
        }
        case TCType::TOURNAMENT: {
            // Java TimeController_Tournament uses time/movestogo (Tournament.java).
            int mtg = std::max(1, go.movestogo);
            // Distribute most of the remaining clock across the moves.
            return std::max(1.0, clock / mtg + inc);
        }
        case TCType::INCREMENT_PER_MOVE: {
            // FloatingTime divides by 35 and reserves the increment.
            double base = clock / BAGATUR_DIVIDE_FACTOR_DEFAULT;
            return std::max(1.0, base + inc);
        }
        case TCType::SUDDEN_DEATH: {
            // FloatingTime / SuddenDeath:
            //   totalClockTime = clock / emergency_factor(clock)
            //   minMoveTime    ≈ clock / (35 × emergency_factor)
            //   availableTime  = minMoveTime + totalClockTime × 0.125
            double emergency  = sudden_death_emergency_factor(clock);
            double total      = clock / emergency;
            double min_move   = clock / (BAGATUR_DIVIDE_FACTOR_DEFAULT * emergency);
            double available  = min_move + total * 0.125;   // SuddenDeath.java line 110
            return std::clamp(available, 1.0, total);
        }
        default:
            return 0.0;
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
        case TCType::SUDDEN_DEATH:
            b.move_time_secs = per_move_budget_ms(b.type, go, colour_to_move) / 1000.0;
            break;
    }
    return b;
}

}  // namespace uci
