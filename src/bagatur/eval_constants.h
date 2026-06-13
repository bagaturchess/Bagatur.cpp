// Evaluation-table constants used by ChessBoard's incremental state update
// and SEE. Mirrors the subset of EvalConstants.java that the board itself
// needs.
//
// PSQT_MG / PSQT_EG are intentionally zero-initialised here: this port is
// focused on Board NPS, not full evaluation. ChessBoard still calls into the
// tables on every do/undo move (so the logic stays bit-identical to the Java
// one); plugging in real PSQT data is a one-line array swap with no code
// change.

#pragma once

#include "types.h"

namespace bagatur::eval {

inline constexpr std::array<int, 7> PHASE = { 0, 0, 3, 3, 5, 9, 0 };
//   index: EMPTY, PAWN, NIGHT, BISHOP, ROOK, QUEEN, KING

inline constexpr std::array<int, 7> MATERIAL_SEE     = { 0, 100, 300, 300, 500, 900, 3000 };

inline constexpr std::array<int, 7> PROMOTION_SCORE_SEE = {
    0,
    0,
    MATERIAL_SEE[NIGHT]  - MATERIAL_SEE[PAWN],
    MATERIAL_SEE[BISHOP] - MATERIAL_SEE[PAWN],
    MATERIAL_SEE[ROOK]   - MATERIAL_SEE[PAWN],
    MATERIAL_SEE[QUEEN]  - MATERIAL_SEE[PAWN],
    0,
};

// [piece][color][sq]. Zero-initialised — swap in real tables if needed.
inline constexpr std::array<std::array<std::array<int, 64>, 2>, 7> PSQT_MG{};
inline constexpr std::array<std::array<std::array<int, 64>, 2>, 7> PSQT_EG{};

}  // namespace bagatur::eval
