// MoveGenerator — pseudo-legal/legal-king move generation.
// Mirrors MoveGenerator.java (search-side helpers like setHHScores /
// setRootScores are intentionally omitted; they belong with the Search code).

#pragma once

#include "chess_board.h"
#include "types.h"

namespace board {

class MoveGenerator {
public:
    static constexpr int MAX_BUFFER = 30000;

    MoveGenerator();

    BAGATUR_FORCE_INLINE void clearHistoryHeuristics() noexcept { currentPly_ = 0; }

    BAGATUR_FORCE_INLINE void startPly() noexcept {
        nextToGenerate_[currentPly_ + 1] = nextToGenerate_[currentPly_];
        nextToMove_[currentPly_ + 1]     = nextToGenerate_[currentPly_];
        ++currentPly_;
    }

    BAGATUR_FORCE_INLINE void endPly() noexcept { --currentPly_; }

    BAGATUR_FORCE_INLINE int  next()     noexcept { return moves_[nextToMove_[currentPly_]++]; }
    BAGATUR_FORCE_INLINE int  previous() noexcept {
        int idx = nextToMove_[currentPly_] - 1;
        return idx < 0 ? 0 : moves_[idx];
    }
    BAGATUR_FORCE_INLINE bool hasNext()  const noexcept {
        return nextToGenerate_[currentPly_] != nextToMove_[currentPly_];
    }
    BAGATUR_FORCE_INLINE std::int64_t getScore() const noexcept {
        return moveScores_[nextToMove_[currentPly_] - 1];
    }
    BAGATUR_FORCE_INLINE int  getCountMoves() const noexcept {
        return nextToGenerate_[currentPly_] - nextToMove_[currentPly_];
    }

    BAGATUR_FORCE_INLINE void addMove(int move) noexcept {
        moves_[nextToGenerate_[currentPly_]++] = move;
    }

    void generateMoves(ChessBoard& cb);
    void generateAttacks(ChessBoard& cb);

    void setMVVLVAScores(const ChessBoard& cb) noexcept;
    void setSEEScores(const ChessBoard& cb) noexcept;

    int getCountGoodAttacks(const ChessBoard& cb) noexcept;
    int getCountEqualAttacks(const ChessBoard& cb) noexcept;
    int getCountBadAttacks(const ChessBoard& cb) noexcept;
    int getCountGoodAndEqualAttacks(const ChessBoard& cb) noexcept;

    void sort() noexcept;  // insertion sort by descending score

private:
    int           moves_[MAX_BUFFER]            = {};
    std::int64_t  moveScores_[MAX_BUFFER]       = {};
    int           nextToGenerate_[MAX_PLIES * 2] = {};
    int           nextToMove_[MAX_PLIES * 2]    = {};
    int           currentPly_                   = 0;

    void generateNotInCheckMoves(ChessBoard& cb);
    void generateOutOfSlidingCheckMoves(ChessBoard& cb);
    void generateNotInCheckAttacks(ChessBoard& cb);
    void generateOutOfCheckAttacks(ChessBoard& cb);

    void addPawnAttacksAndPromotions(BB pawns, ChessBoard& cb, BB enemies, BB emptySpaces);
    void addBishopAttacks(BB piece, ChessBoard& cb, BB possible);
    void addRookAttacks(BB piece, ChessBoard& cb, BB possible);
    void addQueenAttacks(BB piece, ChessBoard& cb, BB possible);

    void addBishopMoves(BB piece, BB all_pieces, BB possible);
    void addQueenMoves(BB piece, BB all_pieces, BB possible);
    void addRookMoves(BB piece, BB all_pieces, BB possible);
    void addNightMoves(BB piece, BB possible);
    void addPawnMoves(BB pawns, ChessBoard& cb, BB possible);

    void addKingMoves(ChessBoard& cb);
    void addKingAttacks(ChessBoard& cb);
    void addNightAttacks(BB piece, const int* pieceIndexes, BB possible);
    void addEpAttacks(ChessBoard& cb);

    void addPromotionMove(int fromIndex, int toIndex);
    void addPromotionAttacks(BB moves, int fromIndex, const int* pieceIndexes);
};

}  // namespace board
