// Magic bitboards for sliding pieces — mirrors MagicUtil.java.
//
// Same magic numbers as the Java source, so the layout/index math matches 1:1.
// Tables live in .bss and are populated by magic::init() — called once from
// chess_board::init().

#pragma once

#include "types.h"

namespace bagatur::magic {

void init();

// Internal arrays — exposed so the hot-path getters can be inline.
extern std::array<BB, 64>     rook_movement_masks;
extern std::array<BB, 64>     bishop_movement_masks;
extern std::array<U32, 64>    rook_shifts;
extern std::array<U32, 64>    bishop_shifts;
extern std::array<BB, 64>     rook_magic_numbers;
extern std::array<BB, 64>     bishop_magic_numbers;
extern std::array<BB*, 64>    rook_magic_moves;     // index -> table for that square
extern std::array<BB*, 64>    bishop_magic_moves;

BAGATUR_FORCE_INLINE BB rook_moves(int from, BB all_pieces) noexcept {
    BB occ = all_pieces & rook_movement_masks[from];
    U32 idx = static_cast<U32>((occ * rook_magic_numbers[from]) >> rook_shifts[from]);
    return rook_magic_moves[from][idx];
}

BAGATUR_FORCE_INLINE BB bishop_moves(int from, BB all_pieces) noexcept {
    BB occ = all_pieces & bishop_movement_masks[from];
    U32 idx = static_cast<U32>((occ * bishop_magic_numbers[from]) >> bishop_shifts[from]);
    return bishop_magic_moves[from][idx];
}

BAGATUR_FORCE_INLINE BB queen_moves(int from, BB all_pieces) noexcept {
    return rook_moves(from, all_pieces) | bishop_moves(from, all_pieces);
}

BAGATUR_FORCE_INLINE BB rook_moves_empty(int from) noexcept {
    return rook_magic_moves[from][0];
}

BAGATUR_FORCE_INLINE BB bishop_moves_empty(int from) noexcept {
    return bishop_magic_moves[from][0];
}

BAGATUR_FORCE_INLINE BB queen_moves_empty(int from) noexcept {
    return rook_magic_moves[from][0] | bishop_magic_moves[from][0];
}

}  // namespace bagatur::magic
