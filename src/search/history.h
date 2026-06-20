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


// ---------------------------------------------------------------------------
// CaptureHistory — separate butterfly for captures/promotions. Mirrors the
// dedicated capture history Java keeps alongside the quiet history.
//
// Quiet butterfly is `(color, from, to) → bonus`. For captures the (from, to)
// pair alone is too coarse — a Bishop on d3 capturing on e4 cares whether
// the victim was a Knight or a Queen — so we key by
// `(color, attacker_piece, to, captured_piece)`.
//
// Used to bias MVV-LVA ordering of good caps (and bad caps) so a capture
// that has historically refuted opponent lines floats above a same-victim
// capture by a worse attacker. Crucial near SEE-borderline captures where
// MVV-LVA alone gives no signal.
//
// Memory: 2 × 7 × 64 × 7 = 6,272 ints ≈ 25 KB. Fits comfortably in L1.
// ---------------------------------------------------------------------------
class CaptureHistory {
public:
    static constexpr int MAX_VAL = 16384;

    void clear() noexcept {
        for (auto& a : table_)
            for (auto& b : a)
                for (auto& c : b)
                    c.fill(0);
    }

    int get(int color, int move) const noexcept {
        int attacker = board::mv::source_piece_index(move);
        int to       = board::mv::to_index(move);
        int captured = board::mv::attacked_piece_index(move);
        return table_[color][attacker][to][captured];
    }

    void on_cutoff(int color, int move, int depth) noexcept {
        update(color, move, depth * depth);
    }
    void on_poor(int color, int move, int depth) noexcept {
        update(color, move, -depth);
    }

private:
    void update(int color, int move, int bonus) noexcept {
        int attacker = board::mv::source_piece_index(move);
        int to       = board::mv::to_index(move);
        int captured = board::mv::attacked_piece_index(move);
        std::int32_t& v = table_[color][attacker][to][captured];
        int clamped_bonus = std::clamp(bonus, -MAX_VAL, MAX_VAL);
        v += clamped_bonus - v * std::abs(clamped_bonus) / MAX_VAL;
    }

    // table_[color][attacker_piece][target_sq][captured_piece]
    std::array<std::array<std::array<std::array<std::int32_t, 7>, 64>, 7>, 2> table_{};
};


// ---------------------------------------------------------------------------
// ContinuationHistory — 1-ply follow-up bonus. Mirrors Java's
// `bagaturchess.search.impl.history.ContinuationHistory`. Indexed by
// (prev_piece, prev_to) → (cur_piece, cur_to); when a quiet move causes a
// β-cutoff in a child of `prev_move`, this table boosts the same follow-up
// next time we see `prev_move`. Conversely on a fail-low, the entry shrinks.
//
// Memory: 7 × 64 × 7 × 64 = 200,704 ints ≈ 800 KB. Fits in L2.
//
// Java's variant keys by `prev_move` only and stores a full HistoryTable
// per slot, costing ~14 MB. Stockfish-style (piece, to) → (piece, to) is
// equivalent in retrieval quality and an order of magnitude smaller.
// ---------------------------------------------------------------------------
class ContinuationHistory {
public:
    static constexpr int MAX_VAL = 16384;

    void clear() noexcept {
        for (auto& a : table_)
            for (auto& b : a)
                for (auto& c : b)
                    c.fill(0);
    }

    // `prev_move` = move that led to the CURRENT position (parent's choice).
    // `cur_move`  = candidate move from the current position.
    int get(int prev_move, int cur_move) const noexcept {
        int pp = board::mv::source_piece_index(prev_move);
        int pt = board::mv::to_index(prev_move);
        int cp = board::mv::source_piece_index(cur_move);
        int ct = board::mv::to_index(cur_move);
        return table_[pp][pt][cp][ct];
    }

    void on_cutoff(int prev_move, int cur_move, int depth) noexcept {
        update(prev_move, cur_move, depth * depth);
    }
    void on_poor(int prev_move, int cur_move, int depth) noexcept {
        update(prev_move, cur_move, -depth);
    }

private:
    void update(int prev_move, int cur_move, int bonus) noexcept {
        int pp = board::mv::source_piece_index(prev_move);
        int pt = board::mv::to_index(prev_move);
        int cp = board::mv::source_piece_index(cur_move);
        int ct = board::mv::to_index(cur_move);
        std::int32_t& v = table_[pp][pt][cp][ct];
        int clamped_bonus = std::clamp(bonus, -MAX_VAL, MAX_VAL);
        v += clamped_bonus - v * std::abs(clamped_bonus) / MAX_VAL;
    }

    // table_[prev_piece][prev_to][cur_piece][cur_to]
    std::array<std::array<std::array<std::array<std::int32_t, 64>, 7>, 64>, 7> table_{};
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
