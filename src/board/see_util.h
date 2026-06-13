// SEE (Static Exchange Evaluation) — mirrors SEEUtil.java.

#pragma once

#include "types.h"

namespace board {
class ChessBoard;
}

namespace board::see {

int getSeeCaptureScore(const ChessBoard& cb, int move) noexcept;
int getSeeFieldScore(const ChessBoard& cb, int square_id) noexcept;

}  // namespace board::see
