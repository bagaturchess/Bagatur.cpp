// Bitboard square constants and helper bitmasks.
// Mirrors Bitboard.java — names preserved 1:1.
//
// Convention: H1 = bit 0, A1 = bit 7, H8 = bit 56, A8 = bit 63.

#pragma once

#include "types.h"

namespace board::bb {

// --- rank 1 ---
inline constexpr BB H1 = 1ULL;
inline constexpr BB G1 = H1 << 1;
inline constexpr BB F1 = G1 << 1;
inline constexpr BB E1 = F1 << 1;
inline constexpr BB D1 = E1 << 1;
inline constexpr BB C1 = D1 << 1;
inline constexpr BB B1 = C1 << 1;
inline constexpr BB A1 = B1 << 1;
// --- rank 2 ---
inline constexpr BB H2 = A1 << 1;
inline constexpr BB G2 = H2 << 1;
inline constexpr BB F2 = G2 << 1;
inline constexpr BB E2 = F2 << 1;
inline constexpr BB D2 = E2 << 1;
inline constexpr BB C2 = D2 << 1;
inline constexpr BB B2 = C2 << 1;
inline constexpr BB A2 = B2 << 1;
// --- rank 3 ---
inline constexpr BB H3 = A2 << 1;
inline constexpr BB G3 = H3 << 1;
inline constexpr BB F3 = G3 << 1;
inline constexpr BB E3 = F3 << 1;
inline constexpr BB D3 = E3 << 1;
inline constexpr BB C3 = D3 << 1;
inline constexpr BB B3 = C3 << 1;
inline constexpr BB A3 = B3 << 1;
// --- rank 4 ---
inline constexpr BB H4 = A3 << 1;
inline constexpr BB G4 = H4 << 1;
inline constexpr BB F4 = G4 << 1;
inline constexpr BB E4 = F4 << 1;
inline constexpr BB D4 = E4 << 1;
inline constexpr BB C4 = D4 << 1;
inline constexpr BB B4 = C4 << 1;
inline constexpr BB A4 = B4 << 1;
// --- rank 5 ---
inline constexpr BB H5 = A4 << 1;
inline constexpr BB G5 = H5 << 1;
inline constexpr BB F5 = G5 << 1;
inline constexpr BB E5 = F5 << 1;
inline constexpr BB D5 = E5 << 1;
inline constexpr BB C5 = D5 << 1;
inline constexpr BB B5 = C5 << 1;
inline constexpr BB A5 = B5 << 1;
// --- rank 6 ---
inline constexpr BB H6 = A5 << 1;
inline constexpr BB G6 = H6 << 1;
inline constexpr BB F6 = G6 << 1;
inline constexpr BB E6 = F6 << 1;
inline constexpr BB D6 = E6 << 1;
inline constexpr BB C6 = D6 << 1;
inline constexpr BB B6 = C6 << 1;
inline constexpr BB A6 = B6 << 1;
// --- rank 7 ---
inline constexpr BB H7 = A6 << 1;
inline constexpr BB G7 = H7 << 1;
inline constexpr BB F7 = G7 << 1;
inline constexpr BB E7 = F7 << 1;
inline constexpr BB D7 = E7 << 1;
inline constexpr BB C7 = D7 << 1;
inline constexpr BB B7 = C7 << 1;
inline constexpr BB A7 = B7 << 1;
// --- rank 8 ---
inline constexpr BB H8 = A7 << 1;
inline constexpr BB G8 = H8 << 1;
inline constexpr BB F8 = G8 << 1;
inline constexpr BB E8 = F8 << 1;
inline constexpr BB D8 = E8 << 1;
inline constexpr BB C8 = D8 << 1;
inline constexpr BB B8 = C8 << 1;
inline constexpr BB A8 = B8 << 1;

// --- castling-helper square combinations (Bitboard.java lines 87-119) ---
inline constexpr BB A1_B1 = A1 | B1;
inline constexpr BB A1_D1 = A1 | D1;
inline constexpr BB B1_C1 = B1 | C1;
inline constexpr BB C1_D1 = C1 | D1;
inline constexpr BB C1_G1 = C1 | G1;
inline constexpr BB D1_F1 = D1 | F1;
inline constexpr BB F1_G1 = F1 | G1;
inline constexpr BB F1_H1 = F1 | H1;
inline constexpr BB G1_H1 = G1 | H1;
inline constexpr BB A8_B8 = A8 | B8;
inline constexpr BB A8_D8 = A8 | D8;
inline constexpr BB B8_C8 = B8 | C8;
inline constexpr BB C8_G8 = C8 | G8;
inline constexpr BB D8_F8 = D8 | F8;
inline constexpr BB F8_G8 = F8 | G8;
inline constexpr BB F8_H8 = F8 | H8;
inline constexpr BB G8_H8 = G8 | H8;

// --- ranks ---
inline constexpr BB RANK_1 = A1 | B1 | C1 | D1 | E1 | F1 | G1 | H1;
inline constexpr BB RANK_2 = A2 | B2 | C2 | D2 | E2 | F2 | G2 | H2;
inline constexpr BB RANK_3 = A3 | B3 | C3 | D3 | E3 | F3 | G3 | H3;
inline constexpr BB RANK_4 = A4 | B4 | C4 | D4 | E4 | F4 | G4 | H4;
inline constexpr BB RANK_5 = A5 | B5 | C5 | D5 | E5 | F5 | G5 | H5;
inline constexpr BB RANK_6 = A6 | B6 | C6 | D6 | E6 | F6 | G6 | H6;
inline constexpr BB RANK_7 = A7 | B7 | C7 | D7 | E7 | F7 | G7 | H7;
inline constexpr BB RANK_8 = A8 | B8 | C8 | D8 | E8 | F8 | G8 | H8;

inline constexpr BB RANK_23456  = RANK_2 | RANK_3 | RANK_4 | RANK_5 | RANK_6;
inline constexpr BB RANK_34567  = RANK_3 | RANK_4 | RANK_5 | RANK_6 | RANK_7;

inline constexpr std::array<BB, 2> RANK_PROMOTION     = { RANK_7, RANK_2 };
inline constexpr std::array<BB, 2> RANK_NON_PROMOTION = { ~RANK_PROMOTION[0], ~RANK_PROMOTION[1] };
inline constexpr std::array<BB, 2> RANK_FIRST         = { RANK_1, RANK_8 };

// --- files ---
inline constexpr BB FILE_A = A1 | A2 | A3 | A4 | A5 | A6 | A7 | A8;
inline constexpr BB FILE_B = B1 | B2 | B3 | B4 | B5 | B6 | B7 | B8;
inline constexpr BB FILE_C = C1 | C2 | C3 | C4 | C5 | C6 | C7 | C8;
inline constexpr BB FILE_D = D1 | D2 | D3 | D4 | D5 | D6 | D7 | D8;
inline constexpr BB FILE_E = E1 | E2 | E3 | E4 | E5 | E6 | E7 | E8;
inline constexpr BB FILE_F = F1 | F2 | F3 | F4 | F5 | F6 | F7 | F8;
inline constexpr BB FILE_G = G1 | G2 | G3 | G4 | G5 | G6 | G7 | G8;
inline constexpr BB FILE_H = H1 | H2 | H3 | H4 | H5 | H6 | H7 | H8;

inline constexpr BB NOT_FILE_A = ~FILE_A;
inline constexpr BB NOT_FILE_H = ~FILE_H;

inline constexpr std::array<BB, 8> RANKS = { RANK_1, RANK_2, RANK_3, RANK_4,
                                             RANK_5, RANK_6, RANK_7, RANK_8 };

// FILES uses Bagatur's reversed file index (0 = H ... 7 = A).
inline constexpr std::array<BB, 8> FILES = { FILE_H, FILE_G, FILE_F, FILE_E,
                                             FILE_D, FILE_C, FILE_B, FILE_A };

// Pawn-attack helpers (mirror Bitboard.java).
BAGATUR_FORCE_INLINE constexpr BB white_pawn_attacks(BB pawns) noexcept {
    return ((pawns << 9) & NOT_FILE_H) | ((pawns << 7) & NOT_FILE_A);
}

BAGATUR_FORCE_INLINE constexpr BB black_pawn_attacks(BB pawns) noexcept {
    return ((pawns >> 9) & NOT_FILE_A) | ((pawns >> 7) & NOT_FILE_H);
}

}  // namespace board::bb
