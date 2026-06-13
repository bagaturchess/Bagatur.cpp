// Zobrist hashing tables — mirrors Zobrist.java.
//
// The raw 64-bit number stream is identical to the Java source, so a board
// hashed by this implementation produces the same key as the Java one.

#pragma once

#include "types.h"

namespace bagatur::zob {

void init();

extern BB                                              sideToMove;
extern std::array<BB, 16>                              castling;
extern std::array<BB, 48>                              epIndex;
extern std::array<std::array<std::array<BB, 7>, 2>, 64> piece;  // [square][color][piece]

}  // namespace bagatur::zob
