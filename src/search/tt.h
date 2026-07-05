// Transposition table — 4-entry buckets, lock-free for SMP.
//
// Each entry is two 64-bit words accessed with std::atomic (relaxed ordering),
// so multiple search threads read and write the shared table WITHOUT locks or
// mutexes. A store writes the data word first, then commits the key word; a
// probe re-reads the key word to reject a torn read. A rare torn / colliding
// entry is harmless: probe verifies `key32`, and the search separately validates
// the TT move (isValidMove / isLegal) and treats the score only as a bound.
//
// Packing (16 bytes = one slot, four slots = one 64-byte cache-line bucket):
//   w0 : [ key32 : 32 ][ depth : 16 ][ flag : 8 ][ gen : 8 ]
//   w1 : [ move  : 32 ][ score : 16 ][ eval : 16 ]

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "../board/types.h"
#include "types.h"

namespace search {

// Decoded entry — the value type returned by probe() and consumed by the search.
// The on-wire storage is the atomic Slot below; this stays a plain struct.
struct TTEntry {
    std::uint32_t key32;
    std::int32_t  move;
    std::int16_t  score;
    std::int16_t  eval;
    std::int16_t  depth;
    std::int8_t   flag;
    std::uint8_t  gen;
};

class TranspositionTable {
public:
    explicit TranspositionTable(std::size_t mb = 512);
    void resize(std::size_t mb);
    void clear();
    void new_search() noexcept { ++gen_; }  // called between searches (no concurrent search)

    // UCI `hashfull`: how full the table is, in permill (0-1000). A sampled
    // estimate over the first 1000 slots — positions hash ~uniformly, so this
    // is cheap and representative. Safe to call concurrently with the search.
    int hashfull() const noexcept;

    // Probe: returns true if a matching, non-torn entry was found (sets `out`).
    // `score` is decoded as stored; the caller applies score_from_tt for ply.
    bool probe(board::BB key, TTEntry& out) const noexcept;

    // Store. Same replacement policy as before (empty slot → same key →
    // age-weighted shallowest), now writing the two atomic words lock-free.
    void store(board::BB key, int move, int score, int eval, int depth,
               TTFlag flag, int ply) noexcept;

private:
    // Two 64-bit atomic words per slot. atomic<uint64_t> is lock-free on x86-64.
    struct Slot {
        std::atomic<std::uint64_t> w0;
        std::atomic<std::uint64_t> w1;
    };
    struct Bucket {
        Slot e[4];
    };
    static_assert(sizeof(Slot)   == 16, "TT slot should be 16 bytes");
    static_assert(sizeof(Bucket) == 64, "TT bucket should be one cache line");

    // Atomics are non-copyable/non-movable, so a std::vector won't do — own a
    // raw bucket array instead.
    std::unique_ptr<Bucket[]> table_;
    std::size_t               num_buckets_ = 0;
    std::size_t               mask_        = 0;
    std::uint8_t              gen_         = 0;
};

}  // namespace search
