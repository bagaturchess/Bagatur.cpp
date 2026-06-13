#include "castling_util.h"

#include "bitboard.h"
#include "check_util.h"
#include "chess_board.h"

namespace bagatur::castling {

BB getCastlingIndexes(int colorToMove, int castlingRights, const CastlingConfig& /*cfg*/) noexcept {
    if (castlingRights == 0) return 0;
    if (colorToMove == WHITE) {
        switch (castlingRights) {
            case 0: case 1: case 2: case 3:     return 0;
            case 4: case 5: case 6: case 7:     return bb::C1;
            case 8: case 9: case 10: case 11:   return bb::G1;
            case 12: case 13: case 14: case 15: return bb::C1_G1;
        }
    } else {
        switch (castlingRights) {
            case 0: case 4: case 8: case 12:    return 0;
            case 1: case 5: case 9: case 13:    return bb::C8;
            case 2: case 6: case 10: case 14:   return bb::G8;
            case 3: case 7: case 11: case 15:   return bb::C8_G8;
        }
    }
    return 0;
}

int getRookMovedOrAttackedCastlingRights(int castlingRights, int rook_sq, const CastlingConfig& cfg) noexcept {
    if (rook_sq == cfg.from_rook_ks_w) return castlingRights & 7;
    if (rook_sq == cfg.from_rook_qs_w) return castlingRights & 11;
    if (rook_sq == cfg.from_rook_ks_b) return castlingRights & 13;
    if (rook_sq == cfg.from_rook_qs_b) return castlingRights & 14;
    return castlingRights;
}

int getKingMovedCastlingRights(int castlingRights, int color, const CastlingConfig& /*cfg*/) noexcept {
    return color == WHITE ? (castlingRights & 3) : (castlingRights & 12);
}

bool isValidCastlingMove(const ChessBoard& cb, int fromIndex, int toIndex) noexcept {
    if (cb.checkingPieces != 0) return false;

    BB bb_RookInBetween;
    BB bb_KingInBetween;
    BB bb_all_pieces_no_king_no_rook;

    using CC = CastlingConfig;

    switch (toIndex) {
        case CC::G1:
            bb_RookInBetween = cb.castlingConfig.bb_inbetween_rook_ks_w;
            bb_KingInBetween = cb.castlingConfig.bb_inbetween_king_ks_w;
            bb_all_pieces_no_king_no_rook =
                cb.allPieces & ~((1ULL << cb.castlingConfig.from_king_w) |
                                 (1ULL << cb.castlingConfig.from_rook_ks_w));
            break;
        case CC::C1:
            bb_RookInBetween = cb.castlingConfig.bb_inbetween_rook_qs_w;
            bb_KingInBetween = cb.castlingConfig.bb_inbetween_king_qs_w;
            bb_all_pieces_no_king_no_rook =
                cb.allPieces & ~((1ULL << cb.castlingConfig.from_king_w) |
                                 (1ULL << cb.castlingConfig.from_rook_qs_w));
            break;
        case CC::G8:
            bb_RookInBetween = cb.castlingConfig.bb_inbetween_rook_ks_b;
            bb_KingInBetween = cb.castlingConfig.bb_inbetween_king_ks_b;
            bb_all_pieces_no_king_no_rook =
                cb.allPieces & ~((1ULL << cb.castlingConfig.from_king_b) |
                                 (1ULL << cb.castlingConfig.from_rook_ks_b));
            break;
        case CC::C8:
            bb_RookInBetween = cb.castlingConfig.bb_inbetween_rook_qs_b;
            bb_KingInBetween = cb.castlingConfig.bb_inbetween_king_qs_b;
            bb_all_pieces_no_king_no_rook =
                cb.allPieces & ~((1ULL << cb.castlingConfig.from_king_b) |
                                 (1ULL << cb.castlingConfig.from_rook_qs_b));
            break;
        default:
            return false;
    }

    if ((bb_all_pieces_no_king_no_rook & bb_KingInBetween) != 0 ||
        (bb_all_pieces_no_king_no_rook & bb_RookInBetween) != 0) {
        return false;
    }

    int king_color = ((1ULL << fromIndex) & cb.pieces[WHITE][KING]) != 0 ? WHITE : BLACK;

    while (bb_KingInBetween != 0) {
        int sq = trailing_zeros(bb_KingInBetween);
        // The king must not cross an attacked square. For Chess960 we exclude
        // the friendly rook from the occupancy because it may shield squares
        // the king passes through.
        if (check::is_in_check_including_king(sq, king_color,
                                              cb.pieces[1 - king_color],
                                              bb_all_pieces_no_king_no_rook)) {
            return false;
        }
        bb_KingInBetween &= bb_KingInBetween - 1;
    }
    return true;
}

RookFromTo getRookFromToSquareIDs(const ChessBoard& cb, int kingToIndex) noexcept {
    using CC = CastlingConfig;
    switch (kingToIndex) {
        case CC::G1: return { cb.castlingConfig.from_rook_ks_w, CC::F1 };
        case CC::C1: return { cb.castlingConfig.from_rook_qs_w, CC::D1 };
        case CC::G8: return { cb.castlingConfig.from_rook_ks_b, CC::F8 };
        case CC::C8: return { cb.castlingConfig.from_rook_qs_b, CC::D8 };
    }
    return { 0, 0 };
}

}  // namespace bagatur::castling
