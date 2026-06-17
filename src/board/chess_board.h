// ChessBoard — full position state + do/undo move. Mirrors ChessBoard.java.
//
// Layout choices for cache locality:
//   - The 6 hot-path scalars (colorToMove, colorToMoveInverse, epIndex,
//     castlingRights, materialKey, lastCaptureOrPawnMoveBefore) sit at the
//     top, in one cache line.
//   - `pieces`, `friendlyPieces`, `allPieces`, `emptySpaces` follow them so
//     the entire occupancy footprint fits in two cache lines.
//   - History arrays (`*History[MAX_MOVES]`) live below — only touched in
//     push/pop at the beginning/end of do/undo move.

#pragma once

#include "castling_config.h"
#include "repetition.h"
#include "types.h"

namespace board {

class ChessBoard {
public:
    ChessBoard();

    // --- Hot-path state -----------------------------------------------------
    // [color][piece] bitboards. piece slot 0 (EMPTY) is unused but kept so
    // that all code that reads `pieces[c][piece_type]` for piece_type returned
    // from `getMoveType(move)` still indexes correctly.
    BB    pieces[2][7]      = {};
    BB    friendlyPieces[2] = { 0, 0 };
    BB    allPieces         = 0;
    BB    emptySpaces       = ~0ULL;

    int   castlingRights        = 0;
    int   colorToMove           = WHITE;
    int   colorToMoveInverse    = BLACK;
    int   epIndex               = 0;       // 0 = no EP
    int   materialKey           = 0;
    int   material_factor_white = 0;
    int   material_factor_black = 0;

    // PSQT delta-tracked scores. Kept for state fidelity; arrays are zero-
    // filled in this port so the additions are no-ops in eval terms but the
    // state is still updated symmetrically (so resetting between games is
    // trivial).
    int   psqtScore_mg = 0;
    int   psqtScore_eg = 0;

    BB    zobristKey      = 0;
    BB    pawnZobristKey  = 0;
    BB    checkingPieces  = 0;
    BB    pinnedPieces    = 0;
    BB    discoveredPieces = 0;

    // pieceIndexes[sq] = piece code (PAWN..KING) or EMPTY.
    int   pieceIndexes[64] = {};
    int   kingIndex[2]     = { 0, 0 };
    BB    kingArea[2]      = { 0, 0 };
    BB    kingBishopRays[2] = { 0, 0 };
    BB    kingRookRays[2]   = { 0, 0 };
    BB    kingQueenRays[2]  = { 0, 0 };

    // --- History (move counter & undo info) ---------------------------------
    int   moveCounter        = 0;
    int   playedMovesCount   = 0;
    int   lastCaptureOrPawnMoveBefore = 0;
    bool  pinnedPiecesDirty  = false;

    int   castlingHistory[MAX_MOVES]                    = {};
    int   epIndexHistory[MAX_MOVES]                     = {};
    BB    zobristKeyHistory[MAX_MOVES]                  = {};
    BB    checkingPiecesHistory[MAX_MOVES]              = {};
    BB    pinnedPiecesHistory[MAX_MOVES]                = {};
    BB    discoveredPiecesHistory[MAX_MOVES]            = {};
    int   lastCaptureOrPawnMoveBeforeHistory[MAX_MOVES] = {};
    int   playedMoves[MAX_MOVES]                        = {};

    CastlingConfig    castlingConfig = CastlingConfig::classic();
    RepetitionTable   playedBoardStates;

    // --- API ---------------------------------------------------------------
    BAGATUR_FORCE_INLINE void changeSideToMove() noexcept {
        colorToMove        = colorToMoveInverse;
        colorToMoveInverse = 1 - colorToMove;
    }

    void doMove(int move);
    void undoMove(int move);
    void doNullMove();
    void undoNullMove();

    void setPinnedAndDiscoPieces() noexcept;
    void updateKingValues(int kingColor, int sq) noexcept;

    bool isLegal(int move) noexcept;
    bool isLegalEPMove(int fromIndex) noexcept;
    bool isValidMove(int move) noexcept;

    BAGATUR_FORCE_INLINE bool isDiscoveredMove(int fromIndex) noexcept {
        if (pinnedPiecesDirty) {
            setPinnedAndDiscoPieces();
            pinnedPiecesDirty = false;
        }
        return (discoveredPieces & (1ULL << fromIndex)) != 0;
    }

    int  getRepetition() const noexcept {
        int v = playedBoardStates.get(zobristKey);
        return v == RepetitionTable::NO_VALUE ? 0 : v;
    }

    // FIDE insufficient-material draw test — mirrors BoardImpl.java.
    //
    // A position is a draw by insufficient material iff NEITHER side has the
    // material to force mate. Per-color test returns true if THAT side could
    // (in principle) mate a bare king:
    //   - any pawn, queen, or rook                           → true
    //   - ≥ 3 minor pieces (bishop + knight count)           → true
    //   - bishops on BOTH square colors                      → true
    //   - exactly 1 bishop AND 1 knight                      → true
    //   - otherwise (lone king / K+N / K+B / KN+KN, etc.)    → false
    //
    // The KN+KN case is technically winnable with perfect play but cannot be
    // FORCED — FIDE treats it as drawn material; Java does too.
    bool hasSufficientMatingMaterial() const noexcept;
    bool hasSufficientMatingMaterial(int color) const noexcept;

    // One-time initialisation of all lookup tables (magic, zobrist, etc.).
    // Idempotent and thread-safe to call multiple times.
    static void initGlobals();

private:
    void doCastling960(int move);
    void undoCastling960(int move);

    BAGATUR_FORCE_INLINE void pushHistoryValues(int move) noexcept {
        castlingHistory[moveCounter]                    = castlingRights;
        epIndexHistory[moveCounter]                     = epIndex;
        zobristKeyHistory[moveCounter]                  = zobristKey;
        pinnedPiecesHistory[moveCounter]                = pinnedPieces;
        discoveredPiecesHistory[moveCounter]            = discoveredPieces;
        checkingPiecesHistory[moveCounter]              = checkingPieces;
        lastCaptureOrPawnMoveBeforeHistory[moveCounter] = lastCaptureOrPawnMoveBefore;
        ++moveCounter;
        playedMoves[playedMovesCount++] = move;
    }

    BAGATUR_FORCE_INLINE void popHistoryValues() noexcept {
        --playedMovesCount;
        --moveCounter;
        epIndex                       = epIndexHistory[moveCounter];
        zobristKey                    = zobristKeyHistory[moveCounter];
        castlingRights                = castlingHistory[moveCounter];
        pinnedPieces                  = pinnedPiecesHistory[moveCounter];
        discoveredPieces              = discoveredPiecesHistory[moveCounter];
        checkingPieces                = checkingPiecesHistory[moveCounter];
        lastCaptureOrPawnMoveBefore   = lastCaptureOrPawnMoveBeforeHistory[moveCounter];
    }

    void updatePinnedAndDiscoPiecesAfterMove(int sourcePieceIndex, BB changedSquares) noexcept;

    BB checkingPiecesFull() const noexcept;
    BB checkingPiecesByPiece(int sourcePieceIndex) const noexcept;

    bool isLegalKingMove(int move) noexcept;
    bool isLegalNonKingMove(int move) noexcept;
};

}  // namespace board
