#include "tt.h"

#include <algorithm>
#include <cstring>

namespace search {

namespace {
inline std::size_t round_down_pow2(std::size_t v) noexcept {
    std::size_t r = 1;
    while ((r << 1) <= v) r <<= 1;
    return r;
}
}  // namespace

TranspositionTable::TranspositionTable(std::size_t mb) { resize(mb); }

void TranspositionTable::resize(std::size_t mb) {
    std::size_t bytes   = mb * (1ull << 20);
    std::size_t buckets = round_down_pow2(bytes / sizeof(TTBucket));
    if (buckets < 1024) buckets = 1024;
    table_.assign(buckets, TTBucket{});
    mask_ = buckets - 1;
}

void TranspositionTable::clear() {
    std::fill(table_.begin(), table_.end(), TTBucket{});
    gen_ = 0;
}

bool TranspositionTable::probe(board::BB key, TTEntry& out) const noexcept {
    const TTBucket& b = table_[static_cast<std::size_t>(key) & mask_];
    auto k32 = static_cast<std::uint32_t>(key >> 32);
    for (const TTEntry& e : b.e) {
        if (e.key32 == k32 && e.flag != TT_NONE) {
            out = e;
            return true;
        }
    }
    return false;
}

void TranspositionTable::store(board::BB key, int move, int score, int eval,
                               int depth, TTFlag flag, int ply) noexcept {
    TTBucket& b   = table_[static_cast<std::size_t>(key) & mask_];
    auto      k32 = static_cast<std::uint32_t>(key >> 32);

    TTEntry* victim = &b.e[0];
    int      worst  = 999;

    for (TTEntry& e : b.e) {
        if (e.flag == TT_NONE) {
            victim = &e;
            break;
        }
        if (e.key32 == k32) {
            // Same key — preserve the old move when the new store has none.
            // Common case: qsearch's alpha-restore branch stores best_move=0;
            // without this, that erases a deeper full-search's move and
            // wrecks move ordering / SME on the next visit.
            if (move == 0) move = e.move;

            // Don't let a shallow non-exact entry clobber a much deeper one.
            // Iterative deepening / MTD probes hit the same key many times;
            // a depth-0 qsearch UPPER must not erase a depth-12 EXACT for
            // the same position. Threshold of 4 plies matches the standard
            // chess-engine policy (Stockfish, Crafty).
            if (flag != TT_EXACT && depth + 4 < e.depth) {
                // The old entry stays; still refresh its move if the old
                // slot had none and the new probe found one (cheap upgrade
                // for move ordering on the next visit).
                if (move != 0 && e.move == 0) e.move = move;
                return;
            }

            victim = &e;
            break;
        }
        // age-weighted depth — older generations more replaceable
        int score_for_replace = e.depth + (e.gen == gen_ ? 0 : -4);
        if (score_for_replace < worst) {
            worst  = score_for_replace;
            victim = &e;
        }
    }

    int stored_score = score_to_tt(score, ply);
    // Defensive clamp — NNUE can produce eval spikes outside int16 range
    // in pathological positions, and consumers (correction histories,
    // improving detection, future TT-eval pruning) would silently see the
    // wrapped value otherwise.
    int clamped_eval = std::clamp(eval, -32768, 32767);
    victim->key32 = k32;
    victim->move  = move;
    victim->score = static_cast<std::int16_t>(stored_score);
    victim->eval  = static_cast<std::int16_t>(clamped_eval);
    victim->depth = static_cast<std::int16_t>(depth);
    victim->flag  = static_cast<std::int8_t>(flag);
    victim->gen   = gen_;
}

}  // namespace search
