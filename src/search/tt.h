// Transposition table — 4-bucket replacement scheme.
//
// Each TT entry packs:
//   key32  (high 32 bits of zobrist for verification)
//   move   (Bagatur 22-bit move encoding)
//   score  (int16; ply-relative for mate scores)
//   eval   (int16; raw static eval at the node, for re-use)
//   depth  (int8)
//   flag   (int8 TTFlag)
//   gen    (int8 generation for aging)

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
    std::int8_t   depth;
    std::int8_t   flag;
    std::int8_t   gen;
    std::int8_t   pad;
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

    // Store. Replacement policy: prefer slot with same key32; otherwise the
    // entry with lowest (depth + 4 * (gen == cur ? 0 : 1)) — i.e. shallow
    // entries from older generations are replaced first.
    void store(board::BB key, int move, int score, int eval, int depth,
               TTFlag flag, int ply) noexcept;

private:
    std::vector<TTBucket> table_;
    std::size_t           mask_ = 0;
    std::int8_t           gen_  = 0;
};

}  // namespace search
