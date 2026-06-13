// Knight, king and pawn-attack tables — mirrors StaticMoves.java.
// Generated at compile time so they live in .rodata.

#pragma once

#include "types.h"

namespace bagatur::static_moves {

namespace detail {

constexpr bool is_knight_move(int from, int to) noexcept {
    int dr = (from >> 3) - (to >> 3);
    int dr_neg = -dr;
    if (dr == 1)      return from - 10 == to || from - 6  == to;
    if (dr_neg == 1)  return from + 10 == to || from + 6  == to;
    if (dr == 2)      return from - 17 == to || from - 15 == to;
    if (dr_neg == 2)  return from + 17 == to || from + 15 == to;
    return false;
}

constexpr bool is_king_move(int from, int to) noexcept {
    int dr = (from >> 3) - (to >> 3);
    if (dr == 0)  return (from - to) == -1 || (from - to) == 1;
    if (dr == 1)  return (from - to) ==  7 || (from - to) ==  8 || (from - to) ==  9;
    if (dr == -1) return (from - to) == -7 || (from - to) == -8 || (from - to) == -9;
    return false;
}

constexpr auto make_knight_moves() noexcept {
    std::array<BB, 64> a{};
    for (int from = 0; from < 64; ++from)
        for (int to = 0; to < 64; ++to)
            if (is_knight_move(from, to))
                a[from] |= 1ULL << to;
    return a;
}

constexpr auto make_king_moves() noexcept {
    std::array<BB, 64> a{};
    for (int from = 0; from < 64; ++from)
        for (int to = 0; to < 64; ++to)
            if (is_king_move(from, to))
                a[from] |= 1ULL << to;
    return a;
}

constexpr auto make_pawn_attacks() noexcept {
    std::array<std::array<BB, 64>, 2> a{};
    // Convention: file index 0 = H, 7 = A (Bagatur square layout).
    for (int from = 0; from < 64; ++from) {
        for (int to = 0; to < 64; ++to) {
            if (to == from + 7 && (to & 7) != 7) a[WHITE][from] |= 1ULL << to;
            if (to == from + 9 && (to & 7) != 0) a[WHITE][from] |= 1ULL << to;
            if (to == from - 7 && (to & 7) != 0) a[BLACK][from] |= 1ULL << to;
            if (to == from - 9 && (to & 7) != 7) a[BLACK][from] |= 1ULL << to;
        }
    }
    return a;
}

}  // namespace detail

inline constexpr std::array<BB, 64>                   KNIGHT_MOVES = detail::make_knight_moves();
inline constexpr std::array<BB, 64>                   KING_MOVES   = detail::make_king_moves();
inline constexpr std::array<std::array<BB, 64>, 2>    PAWN_ATTACKS = detail::make_pawn_attacks();

}  // namespace bagatur::static_moves
