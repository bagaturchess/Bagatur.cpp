#include "chess_board.h"

#include "bitboard.h"
#include "castling_util.h"
#include "check_util.h"
#include "chess_constants.h"
#include "eval_constants.h"
#include "magic.h"
#include "material_util.h"
#include "move_util.h"
#include "static_moves.h"
#include "zobrist.h"

namespace board {

ChessBoard::ChessBoard()
    // Match the prime number used in the Java implementation so the table
    // size remains familiar to readers of the original code.
    : playedBoardStates(16384) {}

void ChessBoard::initGlobals() {
    cc::init();
    magic::init();
    zob::init();
}

void ChessBoard::updateKingValues(int kingColor, int sq) noexcept {
    if (sq == 64) {
        kingBishopRays[kingColor] = 0;
        kingRookRays[kingColor]   = 0;
        kingQueenRays[kingColor]  = 0;
        return;
    }
    kingIndex[kingColor]      = sq;
    kingArea[kingColor]       = cc::KING_AREA[kingColor][sq];
    kingBishopRays[kingColor] = magic::bishop_moves_empty(sq);
    kingRookRays[kingColor]   = magic::rook_moves_empty(sq);
    kingQueenRays[kingColor]  = kingBishopRays[kingColor] | kingRookRays[kingColor];
}

BB ChessBoard::checkingPiecesFull() const noexcept {
    int king_sq = kingIndex[colorToMove];
    return (pieces[colorToMoveInverse][NIGHT] & static_moves::KNIGHT_MOVES[king_sq])
         | ((pieces[colorToMoveInverse][ROOK]  | pieces[colorToMoveInverse][QUEEN]) & magic::rook_moves(king_sq, allPieces))
         | ((pieces[colorToMoveInverse][BISHOP]| pieces[colorToMoveInverse][QUEEN]) & magic::bishop_moves(king_sq, allPieces))
         | (pieces[colorToMoveInverse][PAWN]  & static_moves::PAWN_ATTACKS[colorToMove][king_sq]);
}

BB ChessBoard::checkingPiecesByPiece(int sourcePieceIndex) const noexcept {
    int king_sq = kingIndex[colorToMove];
    switch (sourcePieceIndex) {
        case PAWN:
            return pieces[colorToMoveInverse][PAWN] & static_moves::PAWN_ATTACKS[colorToMove][king_sq];
        case NIGHT:
            return pieces[colorToMoveInverse][NIGHT] & static_moves::KNIGHT_MOVES[king_sq];
        case BISHOP:
            return pieces[colorToMoveInverse][BISHOP] & magic::bishop_moves(king_sq, allPieces);
        case ROOK:
            return pieces[colorToMoveInverse][ROOK] & magic::rook_moves(king_sq, allPieces);
        case QUEEN:
            return pieces[colorToMoveInverse][QUEEN] & magic::queen_moves(king_sq, allPieces);
        default:
            return 0;  // king cannot check
    }
}

// FIDE insufficient-material draw test — 1:1 port of
// BoardImpl.hasSufficientMatingMaterial(int color). See chess_board.h for the
// per-condition spec.
bool ChessBoard::hasSufficientMatingMaterial(int color) const noexcept {
    // Light/dark-square masks in the H1=0 file-reversed indexing used by the
    // Java port:
    //   light = ((sq >> 3) + (sq & 7)) is even
    // → rank 1 light-bits 0,2,4,6 = 0x55; rank 2 light-bits 9,11,13,15 = 0xAA.
    constexpr BB LIGHT_SQUARES = 0x55AA55AA55AA55AAULL;
    constexpr BB DARK_SQUARES  = ~LIGHT_SQUARES;

    if (pieces[color][PAWN]  != 0) return true;
    if (pieces[color][QUEEN] != 0) return true;
    if (pieces[color][ROOK]  != 0) return true;

    const BB bishops = pieces[color][BISHOP];
    const BB knights = pieces[color][NIGHT];

    if (popcount(bishops) + popcount(knights) >= 3) return true;

    if (bishops != 0 &&
        (bishops & LIGHT_SQUARES) != 0 &&
        (bishops & DARK_SQUARES)  != 0) {
        return true;
    }

    if (popcount(bishops) == 1 && popcount(knights) == 1) return true;

    return false;
}

bool ChessBoard::hasSufficientMatingMaterial() const noexcept {
    return hasSufficientMatingMaterial(WHITE) || hasSufficientMatingMaterial(BLACK);
}

void ChessBoard::setPinnedAndDiscoPieces() noexcept {
    pinnedPieces     = 0;
    discoveredPieces = 0;

    for (int kingColor = WHITE; kingColor <= BLACK; ++kingColor) {
        int enemyColor = 1 - kingColor;
        if (!material::has_sliding_pieces(materialKey, enemyColor)) continue;

        BB enemyPiece = ((pieces[enemyColor][BISHOP] | pieces[enemyColor][QUEEN]) & kingBishopRays[kingColor]) |
                        ((pieces[enemyColor][ROOK]   | pieces[enemyColor][QUEEN]) & kingRookRays[kingColor]);
        while (enemyPiece) {
            BB ck = cc::IN_BETWEEN[kingIndex[kingColor]][trailing_zeros(enemyPiece)] & allPieces;
            if (popcount(ck) == 1) {
                pinnedPieces     |= ck & friendlyPieces[kingColor];
                discoveredPieces |= ck & friendlyPieces[enemyColor];
            }
            enemyPiece &= enemyPiece - 1;
        }
    }
}

void ChessBoard::updatePinnedAndDiscoPiecesAfterMove(int sourcePieceIndex, BB changedSquares) noexcept {
    if (sourcePieceIndex == KING) {
        pinnedPiecesDirty = true;
        return;
    }
    BB affected = kingQueenRays[WHITE] | kingQueenRays[BLACK];
    if (changedSquares & affected) pinnedPiecesDirty = true;
}

void ChessBoard::doNullMove() {
    if (pinnedPiecesDirty) { setPinnedAndDiscoPieces(); pinnedPiecesDirty = false; }
    pushHistoryValues(0);

    zobristKey ^= zob::sideToMove;
    if (epIndex != 0) {
        zobristKey ^= zob::epIndex[epIndex];
        epIndex = 0;
    }
    changeSideToMove();
    playedBoardStates.inc(zobristKey);
}

void ChessBoard::undoNullMove() {
    playedBoardStates.dec(zobristKey);
    popHistoryValues();
    pinnedPiecesDirty = false;
    changeSideToMove();
}

void ChessBoard::doMove(int move) {

    if (pinnedPiecesDirty) { setPinnedAndDiscoPieces(); pinnedPiecesDirty = false; }

    if (mv::is_castling(move)) { doCastling960(move); return; }

    int fromIndex          = mv::from_index(move);
    int toIndex            = mv::to_index(move);
    BB  toMask             = 1ULL << toIndex;
    BB  fromToMask         = (1ULL << fromIndex) ^ toMask;
    BB  changedSquares     = fromToMask;
    int sourcePieceIndex   = mv::source_piece_index(move);
    int attackedPieceIndex = mv::attacked_piece_index(move);

    pushHistoryValues(move);

    if (attackedPieceIndex != 0 || sourcePieceIndex == PAWN) lastCaptureOrPawnMoveBefore = 0;
    else                                                     lastCaptureOrPawnMoveBefore++;

    zobristKey ^= zob::piece[fromIndex][colorToMove][sourcePieceIndex]
               ^  zob::piece[toIndex][colorToMove][sourcePieceIndex]
               ^  zob::sideToMove;
    if (epIndex != 0) {
        zobristKey ^= zob::epIndex[epIndex];
        epIndex = 0;
    }

    friendlyPieces[colorToMove]            ^= fromToMask;
    pieceIndexes[fromIndex]                 = EMPTY;
    pieceIndexes[toIndex]                   = sourcePieceIndex;
    pieces[colorToMove][sourcePieceIndex]  ^= fromToMask;
    psqtScore_mg += eval::PSQT_MG[sourcePieceIndex][colorToMove][toIndex]
                  - eval::PSQT_MG[sourcePieceIndex][colorToMove][fromIndex];
    psqtScore_eg += eval::PSQT_EG[sourcePieceIndex][colorToMove][toIndex]
                  - eval::PSQT_EG[sourcePieceIndex][colorToMove][fromIndex];

    switch (sourcePieceIndex) {
    case PAWN: {
        pawnZobristKey ^= zob::piece[fromIndex][colorToMove][PAWN];
        if (mv::is_promotion(move)) {
            int promo = mv::move_type(move);
            if (colorToMove == WHITE) material_factor_white += eval::PHASE[promo];
            else                       material_factor_black += eval::PHASE[promo];
            materialKey += material::VALUES[colorToMove][promo] - material::VALUES[colorToMove][PAWN];
            pieces[colorToMove][PAWN]  ^= toMask;
            pieces[colorToMove][promo] |= toMask;
            pieceIndexes[toIndex]       = promo;
            zobristKey ^= zob::piece[toIndex][colorToMove][PAWN] ^ zob::piece[toIndex][colorToMove][promo];
            psqtScore_mg += eval::PSQT_MG[promo][colorToMove][toIndex] - eval::PSQT_MG[PAWN][colorToMove][toIndex];
            psqtScore_eg += eval::PSQT_EG[promo][colorToMove][toIndex] - eval::PSQT_EG[PAWN][colorToMove][toIndex];
        } else {
            pawnZobristKey ^= zob::piece[toIndex][colorToMove][PAWN];
            // Detect 2-square push that creates an EP target.
            BB inBetween = cc::IN_BETWEEN[fromIndex][toIndex];
            if (inBetween != 0) {
                int epSquare = trailing_zeros(inBetween);
                if ((static_moves::PAWN_ATTACKS[colorToMove][epSquare] & pieces[colorToMoveInverse][PAWN]) != 0) {
                    epIndex = epSquare;
                    zobristKey ^= zob::epIndex[epIndex];
                }
            }
        }
        break;
    }
    case ROOK:
        if (castlingRights != 0) {
            zobristKey ^= zob::castling[castlingRights];
            castlingRights = castling::getRookMovedOrAttackedCastlingRights(castlingRights, fromIndex, castlingConfig);
            zobristKey ^= zob::castling[castlingRights];
        }
        break;
    case KING:
        updateKingValues(colorToMove, toIndex);
        if (castlingRights != 0) {
            zobristKey ^= zob::castling[castlingRights];
            castlingRights = castling::getKingMovedCastlingRights(castlingRights, colorToMove, castlingConfig);
            zobristKey ^= zob::castling[castlingRights];
        }
        break;
    default:
        break;
    }

    // Capture handling.
    switch (attackedPieceIndex) {
    case EMPTY:
        break;
    case PAWN:
        if (mv::is_ep(move)) {
            toIndex += COLOR_FACTOR_8[colorToMoveInverse];
            toMask = 1ULL << toIndex;
            changedSquares |= toMask;
            pieceIndexes[toIndex] = EMPTY;
        }
        pawnZobristKey ^= zob::piece[toIndex][colorToMoveInverse][PAWN];
        psqtScore_mg -= eval::PSQT_MG[PAWN][colorToMoveInverse][toIndex];
        psqtScore_eg -= eval::PSQT_EG[PAWN][colorToMoveInverse][toIndex];
        friendlyPieces[colorToMoveInverse] ^= toMask;
        pieces[colorToMoveInverse][PAWN]   ^= toMask;
        zobristKey                          ^= zob::piece[toIndex][colorToMoveInverse][PAWN];
        materialKey -= material::VALUES[colorToMoveInverse][PAWN];
        break;
    case ROOK:
        if (castlingRights != 0) {
            zobristKey ^= zob::castling[castlingRights];
            castlingRights = castling::getRookMovedOrAttackedCastlingRights(castlingRights, toIndex, castlingConfig);
            zobristKey ^= zob::castling[castlingRights];
        }
        [[fallthrough]];
    default:
        if (colorToMoveInverse == WHITE) material_factor_white -= eval::PHASE[attackedPieceIndex];
        else                             material_factor_black -= eval::PHASE[attackedPieceIndex];
        psqtScore_mg -= eval::PSQT_MG[attackedPieceIndex][colorToMoveInverse][toIndex];
        psqtScore_eg -= eval::PSQT_EG[attackedPieceIndex][colorToMoveInverse][toIndex];
        friendlyPieces[colorToMoveInverse]            ^= toMask;
        pieces[colorToMoveInverse][attackedPieceIndex] ^= toMask;
        zobristKey ^= zob::piece[toIndex][colorToMoveInverse][attackedPieceIndex];
        materialKey -= material::VALUES[colorToMoveInverse][attackedPieceIndex];
        break;
    }

    allPieces   = friendlyPieces[colorToMove] | friendlyPieces[colorToMoveInverse];
    emptySpaces = ~allPieces;
    changeSideToMove();

    if (isDiscoveredMove(fromIndex)) {
        checkingPieces = checkingPiecesFull();
    } else if (mv::is_normal(move)) {
        checkingPieces = checkingPiecesByPiece(sourcePieceIndex);
    } else {
        checkingPieces = checkingPiecesFull();
    }

    updatePinnedAndDiscoPiecesAfterMove(sourcePieceIndex, changedSquares);
    playedBoardStates.inc(zobristKey);
}


void ChessBoard::doCastling960(int move) {
    pushHistoryValues(move);
    lastCaptureOrPawnMoveBefore++;

    int fromIndex_king = mv::from_index(move);
    int toIndex_king   = mv::to_index(move);

    auto rft = castling::getRookFromToSquareIDs(*this, toIndex_king);
    int fromIndex_rook = rft.from;
    int toIndex_rook   = rft.to;

    // The five-way switch handles the various overlap patterns that show up
    // in Chess960 castling (Java doCastling960 lines 415-490).
    if (fromIndex_king == toIndex_king) {
        BB bb = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMove][ROOK]   ^= bb;
        friendlyPieces[colorToMove] ^= bb;
        pieceIndexes[fromIndex_rook] = EMPTY;
        pieceIndexes[toIndex_rook]   = ROOK;
    } else if (fromIndex_rook == toIndex_rook) {
        BB bb = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMove][KING]   ^= bb;
        friendlyPieces[colorToMove] ^= bb;
        pieceIndexes[fromIndex_king] = EMPTY;
        pieceIndexes[toIndex_king]   = KING;
    } else if (fromIndex_rook == toIndex_king && toIndex_rook == fromIndex_king) {
        BB bb_king = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMove][KING] ^= bb_king;
        BB bb_rook = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMove][ROOK] ^= bb_rook;
        pieceIndexes[toIndex_rook] = ROOK;
        pieceIndexes[toIndex_king] = KING;
    } else if (fromIndex_rook == toIndex_king) {
        BB bb_king = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMove][KING] ^= bb_king;
        BB bb_rook = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMove][ROOK] ^= bb_rook;
        friendlyPieces[colorToMove] ^= ((1ULL << fromIndex_king) | (1ULL << toIndex_rook));
        pieceIndexes[toIndex_rook]   = ROOK;
        pieceIndexes[toIndex_king]   = KING;
        pieceIndexes[fromIndex_king] = EMPTY;
    } else if (toIndex_rook == fromIndex_king) {
        BB bb_king = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMove][KING] ^= bb_king;
        BB bb_rook = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMove][ROOK] ^= bb_rook;
        friendlyPieces[colorToMove] ^= ((1ULL << toIndex_king) | (1ULL << fromIndex_rook));
        pieceIndexes[toIndex_rook]   = ROOK;
        pieceIndexes[toIndex_king]   = KING;
        pieceIndexes[fromIndex_rook] = EMPTY;
    } else {
        BB bb_king = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMove][KING]   ^= bb_king;
        friendlyPieces[colorToMove] ^= bb_king;
        BB bb_rook = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMove][ROOK]   ^= bb_rook;
        friendlyPieces[colorToMove] ^= bb_rook;
        pieceIndexes[fromIndex_rook] = EMPTY;
        pieceIndexes[fromIndex_king] = EMPTY;
        pieceIndexes[toIndex_rook]   = ROOK;
        pieceIndexes[toIndex_king]   = KING;
    }

    updateKingValues(colorToMove, toIndex_king);

    zobristKey ^= zob::piece[fromIndex_king][colorToMove][KING] ^ zob::piece[toIndex_king][colorToMove][KING];
    zobristKey ^= zob::piece[fromIndex_rook][colorToMove][ROOK] ^ zob::piece[toIndex_rook][colorToMove][ROOK];

    if (epIndex != 0) { zobristKey ^= zob::epIndex[epIndex]; epIndex = 0; }
    zobristKey ^= zob::sideToMove;

    if (castlingRights != 0) {
        zobristKey ^= zob::castling[castlingRights];
        castlingRights = castling::getKingMovedCastlingRights(castlingRights, colorToMove, castlingConfig);
        zobristKey ^= zob::castling[castlingRights];
    }

    psqtScore_mg += eval::PSQT_MG[KING][colorToMove][toIndex_king] - eval::PSQT_MG[KING][colorToMove][fromIndex_king];
    psqtScore_eg += eval::PSQT_EG[KING][colorToMove][toIndex_king] - eval::PSQT_EG[KING][colorToMove][fromIndex_king];
    psqtScore_mg += eval::PSQT_MG[ROOK][colorToMove][toIndex_rook] - eval::PSQT_MG[ROOK][colorToMove][fromIndex_rook];
    psqtScore_eg += eval::PSQT_EG[ROOK][colorToMove][toIndex_rook] - eval::PSQT_EG[ROOK][colorToMove][fromIndex_rook];

    allPieces   = friendlyPieces[colorToMove] | friendlyPieces[colorToMoveInverse];
    emptySpaces = ~allPieces;
    changeSideToMove();

    checkingPieces = checkingPiecesFull();
    pinnedPiecesDirty = true;
    playedBoardStates.inc(zobristKey);
}


void ChessBoard::undoMove(int move) {
    if (mv::is_castling(move)) { undoCastling960(move); return; }

    playedBoardStates.dec(zobristKey);

    int fromIndex          = mv::from_index(move);
    int toIndex            = mv::to_index(move);
    BB  toMask             = 1ULL << toIndex;
    BB  fromToMask         = (1ULL << fromIndex) ^ toMask;
    int sourcePieceIndex   = mv::source_piece_index(move);
    int attackedPieceIndex = mv::attacked_piece_index(move);

    popHistoryValues();
    pinnedPiecesDirty = false;

    friendlyPieces[colorToMoveInverse]            ^= fromToMask;
    pieceIndexes[fromIndex]                        = sourcePieceIndex;
    pieces[colorToMoveInverse][sourcePieceIndex] ^= fromToMask;
    psqtScore_mg += eval::PSQT_MG[sourcePieceIndex][colorToMoveInverse][fromIndex]
                  - eval::PSQT_MG[sourcePieceIndex][colorToMoveInverse][toIndex];
    psqtScore_eg += eval::PSQT_EG[sourcePieceIndex][colorToMoveInverse][fromIndex]
                  - eval::PSQT_EG[sourcePieceIndex][colorToMoveInverse][toIndex];

    switch (sourcePieceIndex) {
    case EMPTY:
        break;
    case PAWN: {
        pawnZobristKey ^= zob::piece[fromIndex][colorToMoveInverse][PAWN];
        if (mv::is_promotion(move)) {
            int promo = mv::move_type(move);
            if (colorToMoveInverse == WHITE) material_factor_white -= eval::PHASE[promo];
            else                             material_factor_black -= eval::PHASE[promo];
            materialKey -= material::VALUES[colorToMoveInverse][promo] - material::VALUES[colorToMoveInverse][PAWN];
            pieces[colorToMoveInverse][PAWN]  ^= toMask;
            pieces[colorToMoveInverse][promo] ^= toMask;
            psqtScore_mg += eval::PSQT_MG[PAWN][colorToMoveInverse][toIndex] - eval::PSQT_MG[promo][colorToMoveInverse][toIndex];
            psqtScore_eg += eval::PSQT_EG[PAWN][colorToMoveInverse][toIndex] - eval::PSQT_EG[promo][colorToMoveInverse][toIndex];
        } else {
            pawnZobristKey ^= zob::piece[toIndex][colorToMoveInverse][PAWN];
        }
        break;
    }
    case KING:
        updateKingValues(colorToMoveInverse, fromIndex);
        break;
    default:
        break;
    }

    // Undo capture.
    switch (attackedPieceIndex) {
    case EMPTY:
        break;
    case PAWN:
        if (mv::is_ep(move)) {
            pieceIndexes[toIndex] = EMPTY;
            toIndex += COLOR_FACTOR_8[colorToMove];
            toMask   = 1ULL << toIndex;
        }
        psqtScore_mg += eval::PSQT_MG[PAWN][colorToMove][toIndex];
        psqtScore_eg += eval::PSQT_EG[PAWN][colorToMove][toIndex];
        pawnZobristKey ^= zob::piece[toIndex][colorToMove][PAWN];
        pieces[colorToMove][PAWN]   |= toMask;
        friendlyPieces[colorToMove] |= toMask;
        materialKey += material::VALUES[colorToMove][PAWN];
        break;
    default:
        psqtScore_mg += eval::PSQT_MG[attackedPieceIndex][colorToMove][toIndex];
        psqtScore_eg += eval::PSQT_EG[attackedPieceIndex][colorToMove][toIndex];
        if (colorToMove == WHITE) material_factor_white += eval::PHASE[attackedPieceIndex];
        else                       material_factor_black += eval::PHASE[attackedPieceIndex];
        materialKey += material::VALUES[colorToMove][attackedPieceIndex];
        pieces[colorToMove][attackedPieceIndex] |= toMask;
        friendlyPieces[colorToMove]             |= toMask;
        break;
    }

    pieceIndexes[toIndex] = attackedPieceIndex;
    allPieces   = friendlyPieces[colorToMove] | friendlyPieces[colorToMoveInverse];
    emptySpaces = ~allPieces;
    changeSideToMove();
}


void ChessBoard::undoCastling960(int move) {
    playedBoardStates.dec(zobristKey);
    popHistoryValues();
    pinnedPiecesDirty = false;

    int fromIndex_king = mv::from_index(move);
    int toIndex_king   = mv::to_index(move);
    auto rft = castling::getRookFromToSquareIDs(*this, toIndex_king);
    int fromIndex_rook = rft.from;
    int toIndex_rook   = rft.to;

    if (fromIndex_king == toIndex_king) {
        BB bb = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMoveInverse][ROOK]   ^= bb;
        friendlyPieces[colorToMoveInverse] ^= bb;
        pieceIndexes[fromIndex_rook] = ROOK;
        pieceIndexes[toIndex_rook]   = EMPTY;
    } else if (fromIndex_rook == toIndex_rook) {
        BB bb = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMoveInverse][KING]   ^= bb;
        friendlyPieces[colorToMoveInverse] ^= bb;
        pieceIndexes[fromIndex_king] = KING;
        pieceIndexes[toIndex_king]   = EMPTY;
    } else if (fromIndex_rook == toIndex_king && toIndex_rook == fromIndex_king) {
        BB bb_king = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMoveInverse][KING] ^= bb_king;
        BB bb_rook = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMoveInverse][ROOK] ^= bb_rook;
        pieceIndexes[toIndex_rook] = KING;
        pieceIndexes[toIndex_king] = ROOK;
    } else if (fromIndex_rook == toIndex_king) {
        BB bb_king = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMoveInverse][KING] ^= bb_king;
        BB bb_rook = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMoveInverse][ROOK] ^= bb_rook;
        friendlyPieces[colorToMoveInverse] ^= ((1ULL << fromIndex_king) | (1ULL << toIndex_rook));
        pieceIndexes[toIndex_rook]   = EMPTY;
        pieceIndexes[toIndex_king]   = ROOK;
        pieceIndexes[fromIndex_king] = KING;
    } else if (toIndex_rook == fromIndex_king) {
        BB bb_king = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMoveInverse][KING] ^= bb_king;
        BB bb_rook = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMoveInverse][ROOK] ^= bb_rook;
        friendlyPieces[colorToMoveInverse] ^= ((1ULL << toIndex_king) | (1ULL << fromIndex_rook));
        pieceIndexes[toIndex_rook]   = KING;
        pieceIndexes[toIndex_king]   = EMPTY;
        pieceIndexes[fromIndex_rook] = ROOK;
    } else {
        BB bb_king = (1ULL << fromIndex_king) | (1ULL << toIndex_king);
        pieces[colorToMoveInverse][KING]   ^= bb_king;
        friendlyPieces[colorToMoveInverse] ^= bb_king;
        BB bb_rook = (1ULL << fromIndex_rook) | (1ULL << toIndex_rook);
        pieces[colorToMoveInverse][ROOK]   ^= bb_rook;
        friendlyPieces[colorToMoveInverse] ^= bb_rook;
        pieceIndexes[fromIndex_rook] = ROOK;
        pieceIndexes[fromIndex_king] = KING;
        pieceIndexes[toIndex_rook]   = EMPTY;
        pieceIndexes[toIndex_king]   = EMPTY;
    }

    updateKingValues(colorToMoveInverse, fromIndex_king);

    psqtScore_mg += eval::PSQT_MG[KING][colorToMoveInverse][fromIndex_king] - eval::PSQT_MG[KING][colorToMoveInverse][toIndex_king];
    psqtScore_eg += eval::PSQT_EG[KING][colorToMoveInverse][fromIndex_king] - eval::PSQT_EG[KING][colorToMoveInverse][toIndex_king];
    psqtScore_mg += eval::PSQT_MG[ROOK][colorToMoveInverse][fromIndex_rook] - eval::PSQT_MG[ROOK][colorToMoveInverse][toIndex_rook];
    psqtScore_eg += eval::PSQT_EG[ROOK][colorToMoveInverse][fromIndex_rook] - eval::PSQT_EG[ROOK][colorToMoveInverse][toIndex_rook];

    allPieces   = friendlyPieces[colorToMove] | friendlyPieces[colorToMoveInverse];
    emptySpaces = ~allPieces;
    changeSideToMove();
}


bool ChessBoard::isLegalKingMove(int move) noexcept {
    return !check::is_in_check_including_king(mv::to_index(move), colorToMove,
                                              pieces[colorToMoveInverse],
                                              allPieces ^ (1ULL << mv::from_index(move)));
}

bool ChessBoard::isLegalNonKingMove(int move) noexcept {
    return !check::is_in_check_super(kingIndex[colorToMove], colorToMove,
                                     pieces[colorToMoveInverse],
                                     allPieces ^ (1ULL << mv::from_index(move)) ^ (1ULL << mv::to_index(move)));
}

bool ChessBoard::isLegal(int move) noexcept {
    if (mv::source_piece_index(move) == KING) {
        return !check::is_in_check_including_king(mv::to_index(move), colorToMove,
                                                  pieces[colorToMoveInverse],
                                                  allPieces ^ (1ULL << mv::from_index(move)),
                                                  material::get_major_pieces(materialKey, colorToMoveInverse));
    }
    if (mv::attacked_piece_index(move) != 0) {
        if (mv::is_ep(move)) return isLegalEPMove(mv::from_index(move));
        return true;
    }
    if (checkingPieces != 0) {
        return !check::is_in_check_super(kingIndex[colorToMove], colorToMove,
                                         pieces[colorToMoveInverse],
                                         allPieces ^ (1ULL << mv::from_index(move)) ^ (1ULL << mv::to_index(move)));
    }
    return true;
}

bool ChessBoard::isLegalEPMove(int fromIndex) noexcept {
    BB fromToMask = (1ULL << fromIndex) ^ (1ULL << epIndex);

    friendlyPieces[colorToMove]        ^= fromToMask;
    pieces[colorToMoveInverse][PAWN]   ^= 1ULL << (epIndex + COLOR_FACTOR_8[colorToMoveInverse]);
    allPieces = friendlyPieces[colorToMove]
              | (friendlyPieces[colorToMoveInverse] ^ (1ULL << (epIndex + COLOR_FACTOR_8[colorToMoveInverse])));

    bool inCheck = checkingPiecesFull() != 0;

    friendlyPieces[colorToMove]        ^= fromToMask;
    pieces[colorToMoveInverse][PAWN]   |= 1ULL << (epIndex + COLOR_FACTOR_8[colorToMoveInverse]);
    allPieces = friendlyPieces[colorToMove] | friendlyPieces[colorToMoveInverse];

    return !inCheck;
}


bool ChessBoard::isValidMove(int move) noexcept {
    if (pinnedPiecesDirty) { setPinnedAndDiscoPieces(); pinnedPiecesDirty = false; }

    int fromIndex = mv::from_index(move);
    BB  fromSquare = 1ULL << fromIndex;
    if ((pieces[colorToMove][mv::source_piece_index(move)] & fromSquare) == 0) return false;

    int toIndex   = mv::to_index(move);
    BB  toSquare  = 1ULL << toIndex;
    int attacked  = mv::attacked_piece_index(move);
    if (attacked == 0) {
        if (mv::is_castling(move)) {
            if (pieceIndexes[toIndex] != EMPTY &&
                pieceIndexes[toIndex] != ROOK &&
                pieceIndexes[toIndex] != KING) return false;
        } else if (pieceIndexes[toIndex] != EMPTY) {
            return false;
        }
    } else {
        if ((pieces[colorToMoveInverse][attacked] & toSquare) == 0 && !mv::is_ep(move)) return false;
    }

    switch (mv::source_piece_index(move)) {
    case PAWN:
        if (mv::is_ep(move)) {
            if (toIndex != epIndex) return false;
            return isLegalEPMove(fromIndex);
        }
        if (colorToMove == WHITE) {
            if (fromIndex > toIndex) return false;
            if (toIndex - fromIndex == 16 && (allPieces & (1ULL << (fromIndex + 8))) != 0) return false;
        } else {
            if (fromIndex < toIndex) return false;
            if (fromIndex - toIndex == 16 && (allPieces & (1ULL << (fromIndex - 8))) != 0) return false;
        }
        break;
    case NIGHT:
        break;
    case BISHOP: case ROOK: case QUEEN:
        if ((cc::IN_BETWEEN[fromIndex][toIndex] & allPieces) != 0) return false;
        break;
    case KING:
        if (mv::is_castling(move)) {
            BB castlingIndexes = castling::getCastlingIndexes(colorToMove, castlingRights, castlingConfig);
            while (castlingIndexes != 0) {
                if (toIndex == trailing_zeros(castlingIndexes)) {
                    return castling::isValidCastlingMove(*this, fromIndex, toIndex);
                }
                castlingIndexes &= castlingIndexes - 1;
            }
            return false;
        }
        return isLegalKingMove(move);
    }

    if ((fromSquare & pinnedPieces) != 0) {
        if ((cc::PINNED_MOVEMENT[fromIndex][kingIndex[colorToMove]] & toSquare) == 0) return false;
    }
    if (checkingPieces != 0) {
        if (attacked == 0) return isLegalNonKingMove(move);
        if (popcount(checkingPieces) >= 2) return false;
        return (toSquare & checkingPieces) != 0;
    }
    return true;
}

}  // namespace board
