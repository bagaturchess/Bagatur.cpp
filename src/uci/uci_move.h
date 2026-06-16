// UCI ↔ Bagatur internal move conversion.
//
// UCI uses long algebraic with A1=0, H1=7 (file index left→right).
// Bagatur's internal move encoding uses H1=0, A1=7 (file index reversed).
// Translation is a single XOR with 7 on each square index.

#pragma once

#include <string>

#include "../board/chess_board.h"
#include "../board/move_util.h"

namespace uci {

// Parse a UCI move ("e2e4", "e7e8q") given the current board position.
// Generates legal moves and returns the one matching from/to/promotion,
// or 0 if no match.
int uci_to_move(board::ChessBoard& cb, const std::string& uci_move);

// Format a Bagatur-encoded move as UCI long algebraic.
std::string move_to_uci(int move);

}  // namespace uci
