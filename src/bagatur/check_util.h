// Check / attacker queries. Header-only — these functions are on every hot
// path (move generation legality, do/undo bookkeeping).
//
// Mirrors CheckUtil.java 1:1.

#pragma once

#include "magic.h"
#include "static_moves.h"
#include "types.h"

namespace bagatur::check {

BAGATUR_FORCE_INLINE bool is_in_check_super(int king_sq, int color,
                                            const BB enemy[7], BB all_pieces) noexcept {
    return (enemy[NIGHT]                  & static_moves::KNIGHT_MOVES[king_sq])
         | ((enemy[ROOK]  | enemy[QUEEN]) & magic::rook_moves(king_sq, all_pieces))
         | ((enemy[BISHOP]| enemy[QUEEN]) & magic::bishop_moves(king_sq, all_pieces))
         | (enemy[PAWN]                   & static_moves::PAWN_ATTACKS[color][king_sq]);
}

BAGATUR_FORCE_INLINE bool is_in_check_including_king(int king_sq, int color,
                                                     const BB enemy[7], BB all_pieces,
                                                     int enemy_major_pieces) noexcept {
    if (enemy_major_pieces == 0) {
        return ((enemy[PAWN] & static_moves::PAWN_ATTACKS[color][king_sq])
              | (enemy[KING] & static_moves::KING_MOVES[king_sq])) != 0;
    }
    return ((enemy[NIGHT]                 & static_moves::KNIGHT_MOVES[king_sq])
          | ((enemy[ROOK]  | enemy[QUEEN])& magic::rook_moves(king_sq, all_pieces))
          | ((enemy[BISHOP]| enemy[QUEEN])& magic::bishop_moves(king_sq, all_pieces))
          | (enemy[PAWN]                  & static_moves::PAWN_ATTACKS[color][king_sq])
          | (enemy[KING]                  & static_moves::KING_MOVES[king_sq])) != 0;
}

BAGATUR_FORCE_INLINE bool is_in_check_including_king(int king_sq, int color,
                                                     const BB enemy[7], BB all_pieces) noexcept {
    return ((enemy[NIGHT]                 & static_moves::KNIGHT_MOVES[king_sq])
          | ((enemy[ROOK]  | enemy[QUEEN])& magic::rook_moves(king_sq, all_pieces))
          | ((enemy[BISHOP]| enemy[QUEEN])& magic::bishop_moves(king_sq, all_pieces))
          | (enemy[PAWN]                  & static_moves::PAWN_ATTACKS[color][king_sq])
          | (enemy[KING]                  & static_moves::KING_MOVES[king_sq])) != 0;
}

}  // namespace bagatur::check
