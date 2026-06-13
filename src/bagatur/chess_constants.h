// IN_BETWEEN, PINNED_MOVEMENT and KING_AREA tables.
// Mirrors the static-init blocks in ChessConstants.java.

#pragma once

#include "types.h"

namespace bagatur::cc {

void init();

extern std::array<std::array<BB, 64>, 64> IN_BETWEEN;
extern std::array<std::array<BB, 64>, 64> PINNED_MOVEMENT;
extern std::array<std::array<BB, 64>, 2>  KING_AREA;

}  // namespace bagatur::cc
