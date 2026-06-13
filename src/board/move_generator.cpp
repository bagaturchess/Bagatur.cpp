#include "move_generator.h"

#include "bitboard.h"
#include "castling_util.h"
#include "check_util.h"
#include "chess_constants.h"
#include "magic.h"
#include "material_util.h"
#include "move_util.h"
#include "see_util.h"
#include "static_moves.h"

namespace board {

MoveGenerator::MoveGenerator() { clearHistoryHeuristics(); }

void MoveGenerator::setMVVLVAScores(const ChessBoard& /*cb*/) noexcept {
    constexpr std::int64_t SCALE = 100;
    int start = nextToMove_[currentPly_];
    int end   = nextToGenerate_[currentPly_];
    for (int j = start; j < end; ++j) {
        int move  = moves_[j];
        int score = 6 * mv::attacked_piece_index(move) - mv::source_piece_index(move);
        if (mv::is_promotion(move)) score += mv::move_type(move);
        moveScores_[j] = SCALE * score;
    }
}

void MoveGenerator::setSEEScores(const ChessBoard& cb) noexcept {
    int start = nextToMove_[currentPly_];
    int end   = nextToGenerate_[currentPly_];
    for (int j = start; j < end; ++j)
        moveScores_[j] = see::getSeeCaptureScore(cb, moves_[j]);
}

int MoveGenerator::getCountGoodAttacks(const ChessBoard& cb) noexcept {
    int count = 0;
    for (int j = nextToMove_[currentPly_]; j < nextToGenerate_[currentPly_]; ++j)
        if (see::getSeeCaptureScore(cb, moves_[j]) > 0) ++count;
    return count;
}

int MoveGenerator::getCountEqualAttacks(const ChessBoard& cb) noexcept {
    int count = 0;
    for (int j = nextToMove_[currentPly_]; j < nextToGenerate_[currentPly_]; ++j)
        if (see::getSeeCaptureScore(cb, moves_[j]) == 0) ++count;
    return count;
}

int MoveGenerator::getCountBadAttacks(const ChessBoard& cb) noexcept {
    int count = 0;
    for (int j = nextToMove_[currentPly_]; j < nextToGenerate_[currentPly_]; ++j)
        if (see::getSeeCaptureScore(cb, moves_[j]) < 0) ++count;
    return count;
}

int MoveGenerator::getCountGoodAndEqualAttacks(const ChessBoard& cb) noexcept {
    int count = 0;
    for (int j = nextToMove_[currentPly_]; j < nextToGenerate_[currentPly_]; ++j)
        if (see::getSeeCaptureScore(cb, moves_[j]) >= 0) ++count;
    return count;
}

void MoveGenerator::sort() noexcept {
    int start = nextToMove_[currentPly_];
    int end   = nextToGenerate_[currentPly_] - 1;
    // Insertion sort by descending score, identical to MoveGenerator.java
    // (the randomisation pass from the Java version is omitted — it exists
    // for SMP diversification, which is outside the scope of this port).
    for (int i = start, j = i; i < end; j = ++i) {
        std::int64_t score = moveScores_[i + 1];
        int          move  = moves_[i + 1];
        while (score > moveScores_[j]) {
            moveScores_[j + 1] = moveScores_[j];
            moves_[j + 1]      = moves_[j];
            if (j-- == start) break;
        }
        moveScores_[j + 1] = score;
        moves_[j + 1]      = move;
    }
}

// ---------------------------------------------------------------------------
// Move generation. The top-level dispatch (single-check / double-check / out-
// of-check) follows MoveGenerator.java line-for-line.
// ---------------------------------------------------------------------------

void MoveGenerator::generateMoves(ChessBoard& cb) {
    if (cb.pinnedPiecesDirty) {
        cb.setPinnedAndDiscoPieces();
        cb.pinnedPiecesDirty = false;
    }
    int checkers = popcount(cb.checkingPieces);
    if (checkers == 0) {
        generateNotInCheckMoves(cb);
    } else if (checkers == 1) {
        int checker = cb.pieceIndexes[trailing_zeros(cb.checkingPieces)];
        if (checker == PAWN || checker == NIGHT) {
            addKingMoves(cb);
        } else {
            generateOutOfSlidingCheckMoves(cb);
        }
    } else {
        addKingMoves(cb);
    }
}

void MoveGenerator::generateAttacks(ChessBoard& cb) {
    if (cb.pinnedPiecesDirty) {
        cb.setPinnedAndDiscoPieces();
        cb.pinnedPiecesDirty = false;
    }
    int checkers = popcount(cb.checkingPieces);
    if (checkers == 0)      generateNotInCheckAttacks(cb);
    else if (checkers == 1) generateOutOfCheckAttacks(cb);
    else                    addKingAttacks(cb);
}

void MoveGenerator::generateNotInCheckMoves(ChessBoard& cb) {
    int us = cb.colorToMove;

    addKingMoves(cb);
    addQueenMoves(cb.pieces[us][QUEEN]  & ~cb.pinnedPieces, cb.allPieces, cb.emptySpaces);
    addRookMoves (cb.pieces[us][ROOK]   & ~cb.pinnedPieces, cb.allPieces, cb.emptySpaces);
    addBishopMoves(cb.pieces[us][BISHOP] & ~cb.pinnedPieces, cb.allPieces, cb.emptySpaces);
    addNightMoves(cb.pieces[us][NIGHT]  & ~cb.pinnedPieces, cb.emptySpaces);
    addPawnMoves (cb.pieces[us][PAWN]   & ~cb.pinnedPieces, cb, cb.emptySpaces);

    BB pinned_pieces = cb.friendlyPieces[us] & cb.pinnedPieces;
    while (pinned_pieces) {
        int sq = trailing_zeros(pinned_pieces);
        BB sq_bb = lowest_bit(pinned_pieces);
        BB pinned_movement = cc::PINNED_MOVEMENT[sq][cb.kingIndex[us]];
        switch (cb.pieceIndexes[sq]) {
            case PAWN:   addPawnMoves(sq_bb, cb, cb.emptySpaces & pinned_movement); break;
            case BISHOP: addBishopMoves(sq_bb, cb.allPieces, cb.emptySpaces & pinned_movement); break;
            case ROOK:   addRookMoves(sq_bb, cb.allPieces, cb.emptySpaces & pinned_movement); break;
            case QUEEN:  addQueenMoves(sq_bb, cb.allPieces, cb.emptySpaces & pinned_movement); break;
        }
        pinned_pieces &= pinned_pieces - 1;
    }
}

void MoveGenerator::generateOutOfSlidingCheckMoves(ChessBoard& cb) {
    int us = cb.colorToMove;
    BB inBetween = cc::IN_BETWEEN[cb.kingIndex[us]][trailing_zeros(cb.checkingPieces)];
    if (inBetween != 0) {
        addNightMoves (cb.pieces[us][NIGHT]  & ~cb.pinnedPieces, inBetween);
        addBishopMoves(cb.pieces[us][BISHOP] & ~cb.pinnedPieces, cb.allPieces, inBetween);
        addRookMoves  (cb.pieces[us][ROOK]   & ~cb.pinnedPieces, cb.allPieces, inBetween);
        addQueenMoves (cb.pieces[us][QUEEN]  & ~cb.pinnedPieces, cb.allPieces, inBetween);
        addPawnMoves  (cb.pieces[us][PAWN]   & ~cb.pinnedPieces, cb, inBetween);
    }
    addKingMoves(cb);
}

void MoveGenerator::generateNotInCheckAttacks(ChessBoard& cb) {
    int us = cb.colorToMove;
    BB  enemies = cb.friendlyPieces[cb.colorToMoveInverse];

    addEpAttacks(cb);
    addPawnAttacksAndPromotions(cb.pieces[us][PAWN]   & ~cb.pinnedPieces, cb, enemies, cb.emptySpaces);
    addNightAttacks(cb.pieces[us][NIGHT]   & ~cb.pinnedPieces, cb.pieceIndexes, enemies);
    addRookAttacks (cb.pieces[us][ROOK]    & ~cb.pinnedPieces, cb, enemies);
    addBishopAttacks(cb.pieces[us][BISHOP] & ~cb.pinnedPieces, cb, enemies);
    addQueenAttacks(cb.pieces[us][QUEEN]   & ~cb.pinnedPieces, cb, enemies);
    addKingAttacks(cb);

    BB pinned_pieces = cb.friendlyPieces[us] & cb.pinnedPieces;
    while (pinned_pieces) {
        int sq = trailing_zeros(pinned_pieces);
        BB sq_bb = lowest_bit(pinned_pieces);
        BB ray = enemies & cc::PINNED_MOVEMENT[sq][cb.kingIndex[us]];
        switch (cb.pieceIndexes[sq]) {
            case PAWN:   addPawnAttacksAndPromotions(sq_bb, cb, ray, 0); break;
            case BISHOP: addBishopAttacks(sq_bb, cb, ray); break;
            case ROOK:   addRookAttacks(sq_bb, cb, ray); break;
            case QUEEN:  addQueenAttacks(sq_bb, cb, ray); break;
        }
        pinned_pieces &= pinned_pieces - 1;
    }
}

void MoveGenerator::generateOutOfCheckAttacks(ChessBoard& cb) {
    int us = cb.colorToMove;

    addEpAttacks(cb);

    // Promotion pushes are legal only when they block a single sliding check.
    BB promotionPushTargets = 0;
    if ((cb.checkingPieces & (cb.checkingPieces - 1)) == 0) {
        int checkerSq = trailing_zeros(cb.checkingPieces);
        int checker   = cb.pieceIndexes[checkerSq];
        if (checker == BISHOP || checker == ROOK || checker == QUEEN) {
            promotionPushTargets = cb.emptySpaces & cc::IN_BETWEEN[cb.kingIndex[us]][checkerSq];
        }
    }

    addPawnAttacksAndPromotions(cb.pieces[us][PAWN]   & ~cb.pinnedPieces, cb, cb.checkingPieces, promotionPushTargets);
    addNightAttacks(cb.pieces[us][NIGHT]  & ~cb.pinnedPieces, cb.pieceIndexes, cb.checkingPieces);
    addBishopAttacks(cb.pieces[us][BISHOP] & ~cb.pinnedPieces, cb, cb.checkingPieces);
    addRookAttacks (cb.pieces[us][ROOK]   & ~cb.pinnedPieces, cb, cb.checkingPieces);
    addQueenAttacks(cb.pieces[us][QUEEN]  & ~cb.pinnedPieces, cb, cb.checkingPieces);
    addKingAttacks(cb);
}

// ---- Pawn -----------------------------------------------------------------

void MoveGenerator::addPawnAttacksAndPromotions(BB pawns, ChessBoard& cb, BB enemies, BB emptySpaces) {
    if (pawns == 0) return;

    if (cb.colorToMove == WHITE) {
        // non-promoting attacks
        BB piece = pawns & bb::RANK_NON_PROMOTION[WHITE] & bb::black_pawn_attacks(enemies);
        while (piece) {
            int from = trailing_zeros(piece);
            BB targets = static_moves::PAWN_ATTACKS[WHITE][from] & enemies;
            while (targets) {
                int to = trailing_zeros(targets);
                addMove(mv::create_attack(from, to, PAWN, cb.pieceIndexes[to]));
                targets &= targets - 1;
            }
            piece &= piece - 1;
        }
        // promoting
        piece = pawns & bb::RANK_7;
        while (piece) {
            int from = trailing_zeros(piece);
            BB lsb = lowest_bit(piece);
            if (((lsb << 8) & emptySpaces) != 0) addPromotionMove(from, from + 8);
            addPromotionAttacks(static_moves::PAWN_ATTACKS[WHITE][from] & enemies, from, cb.pieceIndexes);
            piece &= piece - 1;
        }
    } else {
        BB piece = pawns & bb::RANK_NON_PROMOTION[BLACK] & bb::white_pawn_attacks(enemies);
        while (piece) {
            int from = trailing_zeros(piece);
            BB targets = static_moves::PAWN_ATTACKS[BLACK][from] & enemies;
            while (targets) {
                int to = trailing_zeros(targets);
                addMove(mv::create_attack(from, to, PAWN, cb.pieceIndexes[to]));
                targets &= targets - 1;
            }
            piece &= piece - 1;
        }
        piece = pawns & bb::RANK_2;
        while (piece) {
            int from = trailing_zeros(piece);
            BB lsb = lowest_bit(piece);
            if (((lsb >> 8) & emptySpaces) != 0) addPromotionMove(from, from - 8);
            addPromotionAttacks(static_moves::PAWN_ATTACKS[BLACK][from] & enemies, from, cb.pieceIndexes);
            piece &= piece - 1;
        }
    }
}

void MoveGenerator::addPawnMoves(BB pawns, ChessBoard& cb, BB possible) {
    if (pawns == 0) return;
    if (cb.colorToMove == WHITE) {
        // 1-step pushes
        BB piece = pawns & (possible >> 8) & bb::RANK_23456;
        while (piece) {
            addMove(mv::create_white_pawn_move(trailing_zeros(piece)));
            piece &= piece - 1;
        }
        // 2-step pushes
        piece = pawns & (possible >> 16) & bb::RANK_2;
        while (piece) {
            BB lsb = lowest_bit(piece);
            if ((cb.emptySpaces & (lsb << 8)) != 0)
                addMove(mv::create_white_pawn_2_move(trailing_zeros(piece)));
            piece &= piece - 1;
        }
    } else {
        BB piece = pawns & (possible << 8) & bb::RANK_34567;
        while (piece) {
            addMove(mv::create_black_pawn_move(trailing_zeros(piece)));
            piece &= piece - 1;
        }
        piece = pawns & (possible << 16) & bb::RANK_7;
        while (piece) {
            BB lsb = lowest_bit(piece);
            if ((cb.emptySpaces & (lsb >> 8)) != 0)
                addMove(mv::create_black_pawn_2_move(trailing_zeros(piece)));
            piece &= piece - 1;
        }
    }
}

// ---- Sliding pieces -------------------------------------------------------

void MoveGenerator::addBishopMoves(BB piece, BB allPieces, BB possible) {
    while (piece) {
        int from = trailing_zeros(piece);
        BB moves = magic::bishop_moves(from, allPieces) & possible;
        while (moves) {
            addMove(mv::create_move(from, trailing_zeros(moves), BISHOP));
            moves &= moves - 1;
        }
        piece &= piece - 1;
    }
}

void MoveGenerator::addRookMoves(BB piece, BB allPieces, BB possible) {
    while (piece) {
        int from = trailing_zeros(piece);
        BB moves = magic::rook_moves(from, allPieces) & possible;
        while (moves) {
            addMove(mv::create_move(from, trailing_zeros(moves), ROOK));
            moves &= moves - 1;
        }
        piece &= piece - 1;
    }
}

void MoveGenerator::addQueenMoves(BB piece, BB allPieces, BB possible) {
    while (piece) {
        int from = trailing_zeros(piece);
        BB moves = magic::queen_moves(from, allPieces) & possible;
        while (moves) {
            addMove(mv::create_move(from, trailing_zeros(moves), QUEEN));
            moves &= moves - 1;
        }
        piece &= piece - 1;
    }
}

void MoveGenerator::addBishopAttacks(BB piece, ChessBoard& cb, BB possible) {
    while (piece) {
        int from = trailing_zeros(piece);
        BB moves = magic::bishop_moves(from, cb.allPieces) & possible;
        while (moves) {
            int to = trailing_zeros(moves);
            addMove(mv::create_attack(from, to, BISHOP, cb.pieceIndexes[to]));
            moves &= moves - 1;
        }
        piece &= piece - 1;
    }
}

void MoveGenerator::addRookAttacks(BB piece, ChessBoard& cb, BB possible) {
    while (piece) {
        int from = trailing_zeros(piece);
        BB moves = magic::rook_moves(from, cb.allPieces) & possible;
        while (moves) {
            int to = trailing_zeros(moves);
            addMove(mv::create_attack(from, to, ROOK, cb.pieceIndexes[to]));
            moves &= moves - 1;
        }
        piece &= piece - 1;
    }
}

void MoveGenerator::addQueenAttacks(BB piece, ChessBoard& cb, BB possible) {
    while (piece) {
        int from = trailing_zeros(piece);
        BB moves = magic::queen_moves(from, cb.allPieces) & possible;
        while (moves) {
            int to = trailing_zeros(moves);
            addMove(mv::create_attack(from, to, QUEEN, cb.pieceIndexes[to]));
            moves &= moves - 1;
        }
        piece &= piece - 1;
    }
}

// ---- Knight ---------------------------------------------------------------

void MoveGenerator::addNightMoves(BB piece, BB possible) {
    while (piece) {
        int from = trailing_zeros(piece);
        BB moves = static_moves::KNIGHT_MOVES[from] & possible;
        while (moves) {
            addMove(mv::create_move(from, trailing_zeros(moves), NIGHT));
            moves &= moves - 1;
        }
        piece &= piece - 1;
    }
}

void MoveGenerator::addNightAttacks(BB piece, const int* pieceIndexes, BB possible) {
    while (piece) {
        int from = trailing_zeros(piece);
        BB moves = static_moves::KNIGHT_MOVES[from] & possible;
        while (moves) {
            int to = trailing_zeros(moves);
            addMove(mv::create_attack(from, to, NIGHT, pieceIndexes[to]));
            moves &= moves - 1;
        }
        piece &= piece - 1;
    }
}

// ---- King -----------------------------------------------------------------

void MoveGenerator::addKingMoves(ChessBoard& cb) {
    int from = cb.kingIndex[cb.colorToMove];
    BB  occupiedWithoutKing = cb.allPieces ^ (1ULL << from);
    const BB* enemy = cb.pieces[cb.colorToMoveInverse];
    int enemyMajor = material::get_major_pieces(cb.materialKey, cb.colorToMoveInverse);

    BB moves = static_moves::KING_MOVES[from] & cb.emptySpaces;
    while (moves) {
        int to = trailing_zeros(moves);
        if (!check::is_in_check_including_king(to, cb.colorToMove, enemy, occupiedWithoutKing, enemyMajor)) {
            addMove(mv::create_move(from, to, KING));
        }
        moves &= moves - 1;
    }

    if (cb.checkingPieces == 0) {
        BB castlingIndexes = castling::getCastlingIndexes(cb.colorToMove, cb.castlingRights, cb.castlingConfig);
        while (castlingIndexes) {
            int to_king = trailing_zeros(castlingIndexes);
            if (castling::isValidCastlingMove(cb, from, to_king)) {
                addMove(mv::create_castling(from, to_king));
            }
            castlingIndexes &= castlingIndexes - 1;
        }
    }
}

void MoveGenerator::addKingAttacks(ChessBoard& cb) {
    int from = cb.kingIndex[cb.colorToMove];
    BB  occupiedWithoutKing = cb.allPieces ^ (1ULL << from);
    const BB* enemy = cb.pieces[cb.colorToMoveInverse];
    int enemyMajor = material::get_major_pieces(cb.materialKey, cb.colorToMoveInverse);

    BB moves = static_moves::KING_MOVES[from] & cb.friendlyPieces[cb.colorToMoveInverse];
    while (moves) {
        int to = trailing_zeros(moves);
        if (!check::is_in_check_including_king(to, cb.colorToMove, enemy, occupiedWithoutKing, enemyMajor)) {
            addMove(mv::create_attack(from, to, KING, cb.pieceIndexes[to]));
        }
        moves &= moves - 1;
    }
}

// ---- EP -------------------------------------------------------------------

void MoveGenerator::addEpAttacks(ChessBoard& cb) {
    if (cb.epIndex == 0) return;
    BB piece = cb.pieces[cb.colorToMove][PAWN] & static_moves::PAWN_ATTACKS[cb.colorToMoveInverse][cb.epIndex];
    while (piece) {
        int from = trailing_zeros(piece);
        if (cb.isLegalEPMove(from)) {
            addMove(mv::create_ep(from, cb.epIndex));
        }
        piece &= piece - 1;
    }
}

// ---- Promotions -----------------------------------------------------------

void MoveGenerator::addPromotionMove(int from, int to) {
    addMove(mv::create_promotion(mv::TYPE_PROMOTION_Q, from, to));
    addMove(mv::create_promotion(mv::TYPE_PROMOTION_N, from, to));
    if constexpr (GENERATE_BR_PROMOTIONS) {
        addMove(mv::create_promotion(mv::TYPE_PROMOTION_B, from, to));
        addMove(mv::create_promotion(mv::TYPE_PROMOTION_R, from, to));
    }
}

void MoveGenerator::addPromotionAttacks(BB moves, int from, const int* pieceIndexes) {
    while (moves) {
        int to = trailing_zeros(moves);
        int captured = pieceIndexes[to];
        addMove(mv::create_promotion_attack(mv::TYPE_PROMOTION_Q, from, to, captured));
        addMove(mv::create_promotion_attack(mv::TYPE_PROMOTION_N, from, to, captured));
        if constexpr (GENERATE_BR_PROMOTIONS) {
            addMove(mv::create_promotion_attack(mv::TYPE_PROMOTION_B, from, to, captured));
            addMove(mv::create_promotion_attack(mv::TYPE_PROMOTION_R, from, to, captured));
        }
        moves &= moves - 1;
    }
}

}  // namespace board
