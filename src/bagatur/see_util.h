// SEE (Static Exchange Evaluation) — mirrors SEEUtil.java.

#pragma once

#include "types.h"

namespace bagatur {
class ChessBoard;
}

namespace bagatur::see {

int getSeeCaptureScore(const ChessBoard& cb, int move) noexcept;
int getSeeFieldScore(const ChessBoard& cb, int square_id) noexcept;

}  // namespace bagatur::see
