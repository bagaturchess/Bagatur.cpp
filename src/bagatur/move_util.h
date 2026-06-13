// Move encoding — header-only, mirrors MoveUtil.java.
//
// 22-bit encoding:
//   [ 0..5 ] from index   (6 bits)
//   [ 6..11] to   index   (6 bits)
//   [12..14] source piece (3 bits)
//   [15..17] attacked     (3 bits)
//   [18..20] move type    (3 bits)
//   [21    ] promotion    (1 bit)

#pragma once

#include "types.h"

namespace bagatur::mv {

inline constexpr int TYPE_NORMAL      = 0;
inline constexpr int TYPE_EP          = 1;
inline constexpr int TYPE_PROMOTION_N = NIGHT;
inline constexpr int TYPE_PROMOTION_B = BISHOP;
inline constexpr int TYPE_PROMOTION_R = ROOK;
inline constexpr int TYPE_PROMOTION_Q = QUEEN;
inline constexpr int TYPE_CASTLING    = 6;

inline constexpr int SHIFT_TO        = 6;
inline constexpr int SHIFT_SOURCE    = 12;
inline constexpr int SHIFT_ATTACK    = 15;
inline constexpr int SHIFT_MOVE_TYPE = 18;
inline constexpr int SHIFT_PROMOTION = 21;

inline constexpr int MASK_3_BITS  = 7;
inline constexpr int MASK_6_BITS  = 0x3f;
inline constexpr int MASK_12_BITS = 0xfff;
inline constexpr int MASK_ATTACK    = 7 << SHIFT_ATTACK;
inline constexpr int MASK_PROMOTION = 1 << SHIFT_PROMOTION;
inline constexpr int MASK_QUIET     = MASK_PROMOTION | MASK_ATTACK;

BAGATUR_FORCE_INLINE constexpr int from_index(int move) noexcept { return move & MASK_6_BITS; }
BAGATUR_FORCE_INLINE constexpr int to_index(int move)   noexcept { return (move >> SHIFT_TO) & MASK_6_BITS; }
BAGATUR_FORCE_INLINE constexpr int from_to_index(int move) noexcept { return move & MASK_12_BITS; }
BAGATUR_FORCE_INLINE constexpr int attacked_piece_index(int move) noexcept { return (move >> SHIFT_ATTACK) & MASK_3_BITS; }
BAGATUR_FORCE_INLINE constexpr int source_piece_index(int move) noexcept { return (move >> SHIFT_SOURCE) & MASK_3_BITS; }
BAGATUR_FORCE_INLINE constexpr int move_type(int move) noexcept { return (move >> SHIFT_MOVE_TYPE) & MASK_3_BITS; }

BAGATUR_FORCE_INLINE constexpr int create_move(int from, int to, int src_piece) noexcept {
    return (src_piece << SHIFT_SOURCE) | (to << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE constexpr int create_white_pawn_move(int from) noexcept {
    return (PAWN << SHIFT_SOURCE) | ((from + 8) << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE constexpr int create_black_pawn_move(int from) noexcept {
    return (PAWN << SHIFT_SOURCE) | ((from - 8) << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE constexpr int create_white_pawn_2_move(int from) noexcept {
    return (PAWN << SHIFT_SOURCE) | ((from + 16) << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE constexpr int create_black_pawn_2_move(int from) noexcept {
    return (PAWN << SHIFT_SOURCE) | ((from - 16) << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE constexpr int create_promotion(int promo_piece, int from, int to) noexcept {
    return (1 << SHIFT_PROMOTION) | (promo_piece << SHIFT_MOVE_TYPE) |
           (PAWN << SHIFT_SOURCE) | (to << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE constexpr int create_attack(int from, int to, int src_piece, int attacked_piece) noexcept {
    return (attacked_piece << SHIFT_ATTACK) | (src_piece << SHIFT_SOURCE) |
           (to << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE int create_see_attack(BB from_sq, int src_piece) noexcept {
    return (src_piece << SHIFT_SOURCE) | trailing_zeros(from_sq);
}
BAGATUR_FORCE_INLINE constexpr int create_promotion_attack(int promo_piece, int from, int to, int attacked_piece) noexcept {
    return (1 << SHIFT_PROMOTION) | (promo_piece << SHIFT_MOVE_TYPE) |
           (attacked_piece << SHIFT_ATTACK) | (PAWN << SHIFT_SOURCE) |
           (to << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE constexpr int create_ep(int from, int to) noexcept {
    return (TYPE_EP << SHIFT_MOVE_TYPE) | (PAWN << SHIFT_ATTACK) |
           (PAWN << SHIFT_SOURCE) | (to << SHIFT_TO) | from;
}
BAGATUR_FORCE_INLINE constexpr int create_castling(int from, int to) noexcept {
    return (TYPE_CASTLING << SHIFT_MOVE_TYPE) | (KING << SHIFT_SOURCE) |
           (to << SHIFT_TO) | from;
}

BAGATUR_FORCE_INLINE constexpr bool is_promotion(int move) noexcept { return (move & MASK_PROMOTION) != 0; }
BAGATUR_FORCE_INLINE constexpr bool is_quiet(int move)     noexcept { return (move & MASK_QUIET) == 0; }
BAGATUR_FORCE_INLINE constexpr bool is_normal(int move)    noexcept { return move_type(move) == TYPE_NORMAL; }
BAGATUR_FORCE_INLINE constexpr bool is_ep(int move)        noexcept { return move_type(move) == TYPE_EP; }
BAGATUR_FORCE_INLINE constexpr bool is_castling(int move)  noexcept { return move_type(move) == TYPE_CASTLING; }

}  // namespace bagatur::mv
