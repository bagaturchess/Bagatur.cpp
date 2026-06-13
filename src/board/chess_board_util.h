// FEN parsing, position setup, and string formatting.
// Mirrors ChessBoardUtil.java.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "chess_board.h"

namespace board::cbu {

constexpr std::string_view FEN_START =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Creates an initialised board from a FEN string. Caller owns the returned
// board (returned by value — the type is move-constructible).
std::unique_ptr<ChessBoard> getNewCB();
std::unique_ptr<ChessBoard> getNewCB(std::string_view fen);

std::string toString(const ChessBoard& cb, bool add_ep = true);

}  // namespace board::cbu
