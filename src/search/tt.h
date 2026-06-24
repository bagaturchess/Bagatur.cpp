// Transposition table — 4-bucket replacement scheme.
//
// Each TT entry packs (16 bytes total, one bucket = one 64-byte cache line):
//   key32  (high 32 bits of zobrist for verification)
//   move   (Bagatur 22-bit move encoding)
//   score  (int16; ply-relative for mate scores)
//   eval   (int16; raw static eval at the node, for re-use)
//   depth  (int16; widened from int8 — SME double extension lets internal
//           depth exceed parent's depth by +1 per ply, and root cap is
//           MAX_PLY-1=127 so pathological chains can overflow int8)
//   flag   (int8 TTFlag)
//   gen    (uint8 generation for aging — unsigned so ++ wraps cleanly)

#pragma once

#include <cstdint>
#include <vector>

#include "../board/types.h"
#include "types.h"

namespace search {

struct TTEntry {
    std::uint32_t key32;
    std::int32_t  move;
    std::int16_t  score;
    std::int16_t  eval;
    std::int16_t  depth;
    std::int8_t   flag;
    std::uint8_t  gen;
};
static_assert(sizeof(TTEntry) == 16, "TT entry should be 16 bytes for clean bucketing");

struct TTBucket {
    TTEntry e[4];
};
static_assert(sizeof(TTBucket) == 64, "Bucket should be one cache line");

class TranspositionTable {
public:
    explicit TranspositionTable(std::size_t mb = 512);
    void resize(std::size_t mb);
    void clear();
    void new_search() noexcept { ++gen_; }

    // Probe: returns true if a matching entry was found (sets `out`).
    // `score` in the entry is converted from-tt at the caller via score_from_tt.
    bool probe(board::BB key, TTEntry& out) const noexcept;

    // Store. Replacement policy:
    //   1. Empty slot (flag == TT_NONE) — claim it.
    //   2. Same-key slot — preserve old move when new has none; keep the
    //      old entry instead of overwriting when the new store is much
    //      shallower AND non-exact (otherwise iterative-deepening / MTD
    //      probes would let a depth-0 qsearch UPPER erase a depth-12 EXACT).
    //   3. Otherwise pick the slot with the lowest age-weighted depth
    //      (`depth + (gen == cur ? 0 : -4)`).
    void store(board::BB key, int move, int score, int eval, int depth,
               TTFlag flag, int ply) noexcept;

private:
    std::vector<TTBucket> table_;
    std::size_t           mask_ = 0;
    std::uint8_t          gen_  = 0;
};

}  // namespace search
