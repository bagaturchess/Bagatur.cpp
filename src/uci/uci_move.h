// UCI ↔ Bagatur internal move conversion.
//
// UCI uses long algebraic with A1=0, H1=7 (file index left→right).
// Bagatur's internal move encoding uses H1=0, A1=7 (file index reversed).
// Translation is a single XOR with 7 on each square index.
//
// Castling notation depends on the mode:
//   * classic   — the king's two-square target  ("e1g1" / "e1c1")
//   * Chess960  — king-takes-rook                ("e1h1" / "e1a1" / arbitrary
//                 rook file), which is unambiguous when king/rook files vary.
// The engine always encodes castling internally as king→G1/C1/G8/C8; the
// Chess960 re-mapping happens only at this UCI boundary.

#pragma once

#include <string>

#include "../board/chess_board.h"
#include "../board/move_util.h"

namespace board { struct CastlingConfig; }

namespace uci {

// Parse a UCI move ("e2e4", "e7e8q") given the current board position.
// Generates legal moves and returns the one matching from/to/promotion, or 0
// if no match. In Chess960 mode (`frc = true`) a castling move is additionally
// accepted in king-takes-rook form (destination = the rook's start square).
int uci_to_move(board::ChessBoard& cb, const std::string& uci_move, bool frc = false);

// Format a Bagatur-encoded move as UCI long algebraic. When `frc_cfg` is
// non-null (Chess960 mode) a castling move is emitted as king-takes-rook using
// the rook start squares from that config; otherwise the classic king-target.
std::string move_to_uci(int move, const board::CastlingConfig* frc_cfg = nullptr);

}  // namespace uci
