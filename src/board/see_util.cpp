#include "see_util.h"

#include <algorithm>

#include "bitboard.h"
#include "chess_board.h"
#include "eval_constants.h"
#include "magic.h"
#include "move_util.h"
#include "static_moves.h"

namespace board::see {

namespace {

BAGATUR_FORCE_INLINE int getSmallestAttackSeeMove(const BB pieces[7], int colorToMove,
                                                  int toIndex, BB allPieces, BB slidingMask) noexcept {
    BB attackMove;

    // pawn non-promotion attacks
    attackMove = static_moves::PAWN_ATTACKS[1 - colorToMove][toIndex] & pieces[PAWN] &
                 allPieces & bb::RANK_NON_PROMOTION[colorToMove];
    if (attackMove) return mv::create_see_attack(attackMove, PAWN);

    // knight attacks
    attackMove = pieces[NIGHT] & static_moves::KNIGHT_MOVES[toIndex] & allPieces;
    if (attackMove) return mv::create_see_attack(attackMove, NIGHT);

    // bishop attacks
    if (pieces[BISHOP] & slidingMask) {
        attackMove = pieces[BISHOP] & magic::bishop_moves(toIndex, allPieces) & allPieces;
        if (attackMove) return mv::create_see_attack(attackMove, BISHOP);
    }
    // rook attacks
    if (pieces[ROOK] & slidingMask) {
        attackMove = pieces[ROOK] & magic::rook_moves(toIndex, allPieces) & allPieces;
        if (attackMove) return mv::create_see_attack(attackMove, ROOK);
    }
    // queen attacks
    if (pieces[QUEEN] & slidingMask) {
        attackMove = pieces[QUEEN] & magic::queen_moves(toIndex, allPieces) & allPieces;
        if (attackMove) return mv::create_see_attack(attackMove, QUEEN);
    }

    // pawn-promotion attacks
    if (pieces[PAWN] & bb::RANK_PROMOTION[colorToMove]) {
        attackMove = static_moves::PAWN_ATTACKS[1 - colorToMove][toIndex] & pieces[PAWN] &
                     allPieces & bb::RANK_PROMOTION[colorToMove];
        if (attackMove)
            return mv::create_promotion_attack(mv::TYPE_PROMOTION_Q,
                                               trailing_zeros(attackMove), toIndex, 0);
    }

    // king attacks
    attackMove = pieces[KING] & static_moves::KING_MOVES[toIndex];
    if (attackMove) return mv::create_see_attack(attackMove, KING);

    return 0;
}

int getSeeScore(const ChessBoard& cb, int colorToMove, int toIndex,
                int attackedPieceIndex, BB allPieces, BB slidingMask) noexcept {

    int move = getSmallestAttackSeeMove(cb.pieces[colorToMove], colorToMove, toIndex, allPieces, slidingMask);
    if (move == 0) return 0;
    if (attackedPieceIndex == KING) return 3000;

    allPieces  ^= 1ULL << mv::from_index(move);
    slidingMask &= allPieces;

    if (mv::is_promotion(move)) {
        return std::max(0, eval::PROMOTION_SCORE_SEE[QUEEN] +
                              eval::MATERIAL_SEE[attackedPieceIndex] -
                              getSeeScore(cb, 1 - colorToMove, toIndex, QUEEN, allPieces, slidingMask));
    }
    return std::max(0, eval::MATERIAL_SEE[attackedPieceIndex] -
                          getSeeScore(cb, 1 - colorToMove, toIndex,
                                      mv::source_piece_index(move), allPieces, slidingMask));
}

}  // anonymous

int getSeeCaptureScore(const ChessBoard& cb, int move) noexcept {
    int index   = mv::to_index(move);
    BB allPieces = cb.allPieces & ~(1ULL << mv::from_index(move));
    BB slidingMask = magic::queen_moves_empty(index) & allPieces;

    if (mv::is_promotion(move)) {
        return eval::PROMOTION_SCORE_SEE[mv::move_type(move)] +
               eval::MATERIAL_SEE[mv::attacked_piece_index(move)] -
               getSeeScore(cb, cb.colorToMoveInverse, index,
                           mv::move_type(move), allPieces, slidingMask);
    }
    return eval::MATERIAL_SEE[mv::attacked_piece_index(move)] -
           getSeeScore(cb, cb.colorToMoveInverse, index,
                       mv::source_piece_index(move), allPieces, slidingMask);
}

int getSeeFieldScore(const ChessBoard& cb, int square_id) noexcept {
    BB allPieces = cb.allPieces & ~(1ULL << square_id);
    BB slidingMask = magic::queen_moves_empty(square_id) & allPieces;
    return -getSeeScore(cb, cb.colorToMoveInverse, square_id,
                        cb.pieceIndexes[square_id], allPieces, slidingMask);
}

}  // namespace board::see
