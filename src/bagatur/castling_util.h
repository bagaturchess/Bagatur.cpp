// CastlingUtil — mirrors CastlingUtil.java (FRC-aware).

#pragma once

#include "castling_config.h"
#include "types.h"

namespace bagatur {
class ChessBoard;
}

namespace bagatur::castling {

BB   getCastlingIndexes(int colorToMove, int castlingRights, const CastlingConfig& cfg) noexcept;
int  getRookMovedOrAttackedCastlingRights(int castlingRights, int rook_sq, const CastlingConfig& cfg) noexcept;
int  getKingMovedCastlingRights(int castlingRights, int color, const CastlingConfig& cfg) noexcept;

bool isValidCastlingMove(const ChessBoard& cb, int fromIndex, int toIndex) noexcept;

struct RookFromTo { int from; int to; };
RookFromTo getRookFromToSquareIDs(const ChessBoard& cb, int kingToIndex) noexcept;

}  // namespace bagatur::castling
