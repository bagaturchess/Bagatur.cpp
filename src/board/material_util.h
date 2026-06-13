// Material-key bit-packing helpers. Mirrors MaterialUtil.java.
//
// 32-bit material key:
//   White: QQQRRRBBBNNNPPPP (bits 0..15)
//   Black: QQQRRRBBBNNNPPPP (bits 16..31)
// Each piece type gets 3-4 bits of count (max counts on board).

#pragma once

#include "types.h"

namespace board::material {

// VALUES[color][piece_index] -- bit-packed increment per piece.
inline constexpr std::array<std::array<int, 6>, 2> VALUES = {{
    // White: QQQRRRBBBNNNPPPP
    { 0, 1 << 0, 1 << 4, 1 << 7, 1 << 10, 1 << 13 },
    // Black: shift by 16
    { 0, 1 << 16, 1 << 20, 1 << 23, 1 << 26, 1 << 29 },
}};

inline constexpr std::array<int, 2> SHIFT = { 0, 16 };

namespace detail {
inline constexpr int MASK_MINOR_MAJOR_ALL          = 0xfff0fff0;
inline constexpr int MASK_MINOR_MAJOR_WHITE        = 0xfff0;
inline constexpr int MASK_MINOR_MAJOR_BLACK        = static_cast<int>(0xfff00000u);
inline constexpr std::array<int, 2> MASK_MINOR_MAJOR = { MASK_MINOR_MAJOR_WHITE, MASK_MINOR_MAJOR_BLACK };
inline constexpr std::array<int, 2> MASK_NON_NIGHTS  = { 0xff8f, static_cast<int>(0xff8f0000u) };
inline constexpr int MASK_SINGLE_BISHOPS           = 0x800080;
inline constexpr int MASK_SINGLE_BISHOP_NIGHT_W    = 0x90;
inline constexpr int MASK_SINGLE_BISHOP_NIGHT_B    = 0x900000;
inline constexpr std::array<int, 2> MASK_PAWNS_QUEENS = { 0xe00f, static_cast<int>(0xe00f0000u) };
inline constexpr std::array<int, 2> MASK_SLIDING      = { 0xff80, static_cast<int>(0xff800000u) };
inline constexpr std::array<int, 2> MASK_MATING       = { 0xff6f, static_cast<int>(0xff6f0000u) };
}  // namespace detail

BAGATUR_FORCE_INLINE bool contains_major_pieces(int mat) noexcept {
    return (mat & detail::MASK_MINOR_MAJOR_ALL) != 0;
}
BAGATUR_FORCE_INLINE bool has_non_pawn_pieces(int mat, int color) noexcept {
    return (mat & detail::MASK_MINOR_MAJOR[color]) != 0;
}
BAGATUR_FORCE_INLINE bool has_pawns_or_queens(int mat, int color) noexcept {
    return (mat & detail::MASK_PAWNS_QUEENS[color]) != 0;
}
BAGATUR_FORCE_INLINE bool has_only_knights(int mat, int color) noexcept {
    return (mat & detail::MASK_NON_NIGHTS[color]) == 0;
}
BAGATUR_FORCE_INLINE int  get_major_pieces(int mat, int color) noexcept {
    return static_cast<int>(static_cast<unsigned>(mat & detail::MASK_MINOR_MAJOR[color]) >> SHIFT[color]);
}
BAGATUR_FORCE_INLINE bool has_sliding_pieces(int mat, int color) noexcept {
    return (mat & detail::MASK_SLIDING[color]) != 0;
}
BAGATUR_FORCE_INLINE bool opposite_bishops(int mat) noexcept {
    return popcount(static_cast<BB>(mat & detail::MASK_MINOR_MAJOR_ALL)) == 2 &&
           popcount(static_cast<BB>(mat & detail::MASK_SINGLE_BISHOPS)) == 2;
}
BAGATUR_FORCE_INLINE bool is_kbnk(int mat) noexcept {
    return mat == 0x90 || mat == 0x900000;
}
BAGATUR_FORCE_INLINE bool is_krkp(int mat) noexcept {
    return mat == 0x10400 || mat == 0x4000001;
}

}  // namespace board::material
