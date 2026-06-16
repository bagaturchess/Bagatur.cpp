// HistoryTable + Killer moves storage.
//
// Bagatur uses a butterfly history indexed by [color][from][to] for quiet
// moves. The score is biased toward fail-high quiet moves.
//
// We use a slightly simplified "fail-soft" update:
//   on cutoff: history[c][from][to] += depth² (clamped)
//   on poor move: history[c][from][to] -= depth (clamped)
// Same shape as Bagatur's registerGood/registerBad — just lighter coupling.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "../board/move_util.h"
#include "../board/types.h"
#include "types.h"

namespace search {

class HistoryTable {
public:
    static constexpr int MAX_VAL = 16384;

    HistoryTable() { clear(); }

    void clear() noexcept {
        for (auto& c : table_) for (auto& f : c) f.fill(0);
    }

    int  get(int color, int move) const noexcept {
        return table_[color][board::mv::from_index(move)][board::mv::to_index(move)];
    }

    void on_cutoff(int color, int move, int depth) noexcept {
        update(color, move, depth * depth);
    }
    void on_poor(int color, int move, int depth) noexcept {
        update(color, move, -depth);
    }

private:
    void update(int color, int move, int bonus) noexcept {
        int from = board::mv::from_index(move);
        int to   = board::mv::to_index(move);
        std::int32_t& v = table_[color][from][to];
        // Gravitate-toward-zero update — keeps the table from saturating.
        int clamped_bonus = std::clamp(bonus, -MAX_VAL, MAX_VAL);
        v += clamped_bonus - v * std::abs(clamped_bonus) / MAX_VAL;
    }

    // 2 × 64 × 64 = 8192 ints = 32 KB. Fits comfortably in L2.
    std::array<std::array<std::array<std::int32_t, 64>, 64>, 2> table_{};
};


class Killers {
public:
    void clear() noexcept {
        for (auto& a : moves_) a.fill(0);
    }
    int  primary(int ply)   const noexcept { return moves_[ply][0]; }
    int  secondary(int ply) const noexcept { return moves_[ply][1]; }

    void on_cutoff(int ply, int move) noexcept {
        if (moves_[ply][0] == move) return;
        moves_[ply][1] = moves_[ply][0];
        moves_[ply][0] = move;
    }

private:
    std::array<std::array<int, 2>, MAX_PLY> moves_{};
};

}  // namespace search
