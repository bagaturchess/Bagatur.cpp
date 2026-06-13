#include "chess_board_util.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#include "bitboard.h"
#include "check_util.h"
#include "chess_constants.h"
#include "eval_constants.h"
#include "magic.h"
#include "material_util.h"
#include "static_moves.h"
#include "zobrist.h"

namespace board::cbu {

namespace {

constexpr const char* FEN_WHITE_PIECES[] = { "1", "P", "N", "B", "R", "Q", "K" };
constexpr const char* FEN_BLACK_PIECES[] = { "1", "p", "n", "b", "r", "q", "k" };
constexpr const char* FIELD_NAMES[] = {
    "h1","g1","f1","e1","d1","c1","b1","a1",
    "h2","g2","f2","e2","d2","c2","b2","a2",
    "h3","g3","f3","e3","d3","c3","b3","a3",
    "h4","g4","f4","e4","d4","c4","b4","a4",
    "h5","g5","f5","e5","d5","c5","b5","a5",
    "h6","g6","f6","e6","d6","c6","b6","a6",
    "h7","g7","f7","e7","d7","c7","b7","a7",
    "h8","g8","f8","e8","d8","c8","b8","a8",
};

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

void setPieces(ChessBoard& cb, const std::string& fenPieces) {
    for (int color = 0; color < 2; ++color)
        for (int piece = 1; piece <= KING; ++piece)
            cb.pieces[color][piece] = 0;

    int positionCount = 63;
    for (char ch : fenPieces) {
        switch (ch) {
        case '/': continue;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8':
            positionCount -= (ch - '0');
            break;
        case 'P': cb.pieces[WHITE][PAWN]   |= 1ULL << positionCount--; break;
        case 'N': cb.pieces[WHITE][NIGHT]  |= 1ULL << positionCount--; break;
        case 'B': cb.pieces[WHITE][BISHOP] |= 1ULL << positionCount--; break;
        case 'R': cb.pieces[WHITE][ROOK]   |= 1ULL << positionCount--; break;
        case 'Q': cb.pieces[WHITE][QUEEN]  |= 1ULL << positionCount--; break;
        case 'K': cb.pieces[WHITE][KING]   |= 1ULL << positionCount--; break;
        case 'p': cb.pieces[BLACK][PAWN]   |= 1ULL << positionCount--; break;
        case 'n': cb.pieces[BLACK][NIGHT]  |= 1ULL << positionCount--; break;
        case 'b': cb.pieces[BLACK][BISHOP] |= 1ULL << positionCount--; break;
        case 'r': cb.pieces[BLACK][ROOK]   |= 1ULL << positionCount--; break;
        case 'q': cb.pieces[BLACK][QUEEN]  |= 1ULL << positionCount--; break;
        case 'k': cb.pieces[BLACK][KING]   |= 1ULL << positionCount--; break;
        default: break;
        }
    }
}

void setFenValues(const std::vector<std::string>& fen, ChessBoard& cb) {
    cb.moveCounter = 0;
    setPieces(cb, fen[0]);

    cb.colorToMove = (fen.size() > 1 && fen[1] == "w") ? WHITE : BLACK;

    if (fen.size() > 3) {
        if (fen[3] == "-" || fen[3] == "\xE2\x80\x93") {  // hyphen or en-dash
            cb.epIndex = 0;
        } else {
            cb.epIndex = 104 - fen[3][0] + 8 * (std::stoi(fen[3].substr(1)) - 1);
        }
    }

    if (fen.size() > 5) {
        cb.lastCaptureOrPawnMoveBefore = (fen[4] == "-") ? 1 : std::stoi(fen[4]);
        cb.moveCounter = ((fen[5] == "-") ? 1 : std::stoi(fen[5])) * 2;
        if (cb.colorToMove == BLACK) ++cb.moveCounter;
    } else {
        int pawnsMoved = 16 - popcount(cb.pieces[WHITE][PAWN] & bb::RANK_2)
                            - popcount(cb.pieces[BLACK][PAWN] & bb::RANK_7);
        cb.moveCounter = pawnsMoved * 2;
    }
}

void setCastlingRights(const bool rights[4], ChessBoard& cb) {
    cb.castlingRights = 15;
    if (!rights[0]) cb.castlingRights &= 7;
    if (!rights[1]) cb.castlingRights &= 11;
    if (!rights[2]) cb.castlingRights &= 13;
    if (!rights[3]) cb.castlingRights &= 14;
}

void getCastlingRights(const std::string& s, const CastlingConfig& cfg, bool result[4]) {
    if (s.empty()) return;

    bool hasStandard = s.find_first_of("KQkq") != std::string::npos;
    if (hasStandard) {
        if (s.find('K') != std::string::npos) result[0] = true;
        if (s.find('Q') != std::string::npos) result[1] = true;
        if (s.find('k') != std::string::npos) result[2] = true;
        if (s.find('q') != std::string::npos) result[3] = true;
        return;
    }

    char ks_w = static_cast<char>(104 - cfg.from_rook_ks_w % 8);
    char qs_w = static_cast<char>(104 - cfg.from_rook_qs_w % 8);
    char ks_b = static_cast<char>(104 - cfg.from_rook_ks_b % 8);
    char qs_b = static_cast<char>(104 - cfg.from_rook_qs_b % 8);

    for (char c : s) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lc == ks_w) result[0] = true;
            if (lc == qs_w) result[1] = true;
        } else {
            if (c == ks_b) result[2] = true;
            if (c == qs_b) result[3] = true;
        }
    }
}

void calculateZobristKeys(ChessBoard& cb) {
    cb.zobristKey = 0;
    for (int color = 0; color < 2; ++color) {
        for (int piece = PAWN; piece <= KING; ++piece) {
            BB p = cb.pieces[color][piece];
            while (p) {
                cb.zobristKey ^= zob::piece[trailing_zeros(p)][color][piece];
                p &= p - 1;
            }
        }
    }
    cb.zobristKey ^= zob::castling[cb.castlingRights];
    if (cb.colorToMove == WHITE) cb.zobristKey ^= zob::sideToMove;
    cb.zobristKey ^= zob::epIndex[cb.epIndex];
}

void calculatePawnZobristKeys(ChessBoard& cb) {
    cb.pawnZobristKey = 0;
    for (int color = 0; color < 2; ++color) {
        BB p = cb.pieces[color][PAWN];
        while (p) {
            cb.pawnZobristKey ^= zob::piece[trailing_zeros(p)][color][PAWN];
            p &= p - 1;
        }
    }
}

void calculateMaterialKey(ChessBoard& cb) {
    cb.materialKey = 0;
    for (int color = 0; color < 2; ++color)
        for (int piece = PAWN; piece <= QUEEN; ++piece)
            cb.materialKey += popcount(cb.pieces[color][piece]) * material::VALUES[color][piece];
}

void calculatePositionScores(ChessBoard& cb) {
    for (int color = 0; color < 2; ++color) {
        for (int piece = PAWN; piece <= KING; ++piece) {
            BB p = cb.pieces[color][piece];
            while (p) {
                int sq = trailing_zeros(p);
                cb.psqtScore_mg += eval::PSQT_MG[piece][color][sq];
                cb.psqtScore_eg += eval::PSQT_EG[piece][color][sq];
                p &= p - 1;
            }
        }
    }
}

void detectCastlingConfig(ChessBoard& cb) {
    using CC = CastlingConfig;

    int king_w = CC::E1, king_b = CC::E8;
    BB bb_w = cb.pieces[WHITE][KING];
    BB bb_b = cb.pieces[BLACK][KING];
    while (bb_w) { king_w = trailing_zeros(bb_w); bb_w &= bb_w - 1; }
    while (bb_b) { king_b = trailing_zeros(bb_b); bb_b &= bb_b - 1; }

    int rook_ks_w = CC::H1;
    for (int sq = king_w; sq >= CC::H1; --sq)
        if (cb.pieceIndexes[sq] == ROOK) { rook_ks_w = sq; break; }

    int rook_qs_w = CC::A1;
    for (int sq = king_w; sq <= CC::A1; ++sq)
        if (cb.pieceIndexes[sq] == ROOK) { rook_qs_w = sq; break; }

    int rook_ks_b = CC::H8;
    for (int sq = king_b; sq >= CC::H8; --sq)
        if (cb.pieceIndexes[sq] == ROOK) { rook_ks_b = sq; break; }

    int rook_qs_b = CC::A8;
    for (int sq = king_b; sq <= CC::A8; ++sq)
        if (cb.pieceIndexes[sq] == ROOK) { rook_qs_b = sq; break; }

    cb.castlingConfig.initFor(king_w, rook_ks_w, rook_qs_w, king_b, rook_ks_b, rook_qs_b);
}

void initBoard(ChessBoard& cb, const bool rights[4]) {
    calculateMaterialKey(cb);

    cb.updateKingValues(WHITE, trailing_zeros(cb.pieces[WHITE][KING]));
    cb.updateKingValues(BLACK, trailing_zeros(cb.pieces[BLACK][KING]));

    cb.colorToMoveInverse = 1 - cb.colorToMove;
    cb.friendlyPieces[WHITE] = cb.pieces[WHITE][PAWN]   | cb.pieces[WHITE][BISHOP]
                              | cb.pieces[WHITE][NIGHT] | cb.pieces[WHITE][KING]
                              | cb.pieces[WHITE][ROOK]  | cb.pieces[WHITE][QUEEN];
    cb.friendlyPieces[BLACK] = cb.pieces[BLACK][PAWN]   | cb.pieces[BLACK][BISHOP]
                              | cb.pieces[BLACK][NIGHT] | cb.pieces[BLACK][KING]
                              | cb.pieces[BLACK][ROOK]  | cb.pieces[BLACK][QUEEN];
    cb.allPieces   = cb.friendlyPieces[WHITE] | cb.friendlyPieces[BLACK];
    cb.emptySpaces = ~cb.allPieces;

    for (int i = 0; i < 64; ++i) cb.pieceIndexes[i] = EMPTY;
    for (int color = 0; color < 2; ++color) {
        for (int piece = 1; piece <= KING; ++piece) {
            BB p = cb.pieces[color][piece];
            while (p) {
                cb.pieceIndexes[trailing_zeros(p)] = piece;
                p &= p - 1;
            }
        }
    }

    // checkingPieces needs the colorToMoveInverse already set.
    int king_sq = cb.kingIndex[cb.colorToMove];
    cb.checkingPieces =
          (cb.pieces[cb.colorToMoveInverse][NIGHT]                                    & static_moves::KNIGHT_MOVES[king_sq])
        | ((cb.pieces[cb.colorToMoveInverse][ROOK]  | cb.pieces[cb.colorToMoveInverse][QUEEN]) & magic::rook_moves(king_sq, cb.allPieces))
        | ((cb.pieces[cb.colorToMoveInverse][BISHOP]| cb.pieces[cb.colorToMoveInverse][QUEEN]) & magic::bishop_moves(king_sq, cb.allPieces))
        | (cb.pieces[cb.colorToMoveInverse][PAWN]                                     & static_moves::PAWN_ATTACKS[cb.colorToMove][king_sq]);

    cb.setPinnedAndDiscoPieces();

    cb.psqtScore_mg = 0;
    cb.psqtScore_eg = 0;
    calculatePositionScores(cb);

    cb.material_factor_white = popcount(cb.pieces[WHITE][NIGHT])  * eval::PHASE[NIGHT]
                              + popcount(cb.pieces[WHITE][BISHOP]) * eval::PHASE[BISHOP]
                              + popcount(cb.pieces[WHITE][ROOK])   * eval::PHASE[ROOK]
                              + popcount(cb.pieces[WHITE][QUEEN])  * eval::PHASE[QUEEN];
    cb.material_factor_black = popcount(cb.pieces[BLACK][NIGHT])  * eval::PHASE[NIGHT]
                              + popcount(cb.pieces[BLACK][BISHOP]) * eval::PHASE[BISHOP]
                              + popcount(cb.pieces[BLACK][ROOK])   * eval::PHASE[ROOK]
                              + popcount(cb.pieces[BLACK][QUEEN])  * eval::PHASE[QUEEN];

    setCastlingRights(rights, cb);
    calculatePawnZobristKeys(cb);
    calculateZobristKeys(cb);
}

}  // anonymous

std::unique_ptr<ChessBoard> getNewCB() { return getNewCB(FEN_START); }

std::unique_ptr<ChessBoard> getNewCB(std::string_view fen) {
    ChessBoard::initGlobals();

    auto cb = std::make_unique<ChessBoard>();
    auto parts = split(fen);
    setFenValues(parts, *cb);

    bool rights[4] = { false, false, false, false };
    initBoard(*cb, rights);
    cb->playedBoardStates.inc(cb->zobristKey);

    // Detect castling config (FRC-friendly) from current board.
    detectCastlingConfig(*cb);

    if (parts.size() > 2) {
        cb->playedBoardStates.dec(cb->zobristKey);
        cb->zobristKey ^= zob::castling[cb->castlingRights];
        getCastlingRights(parts[2], cb->castlingConfig, rights);
        setCastlingRights(rights, *cb);
        cb->zobristKey ^= zob::castling[cb->castlingRights];
        cb->playedBoardStates.inc(cb->zobristKey);
    }
    return cb;
}

std::string toString(const ChessBoard& cb, bool add_ep) {
    std::string sb;
    sb.reserve(96);
    for (int i = 63; i >= 0; --i) {
        if ((cb.friendlyPieces[WHITE] & (1ULL << i)) != 0) {
            sb += FEN_WHITE_PIECES[cb.pieceIndexes[i]];
        } else {
            sb += FEN_BLACK_PIECES[cb.pieceIndexes[i]];
        }
        if (i % 8 == 0 && i != 0) sb += '/';
    }
    sb += ' ';
    sb += (cb.colorToMove == WHITE ? 'w' : 'b');
    sb += ' ';

    if (cb.castlingRights == 0) {
        sb += '-';
    } else {
        if (cb.castlingRights & 8) sb += 'K';
        if (cb.castlingRights & 4) sb += 'Q';
        if (cb.castlingRights & 2) sb += 'k';
        if (cb.castlingRights & 1) sb += 'q';
    }

    // Compact consecutive '1's into rank-skip digits.
    for (int n = 8; n >= 2; --n) {
        std::string from(static_cast<std::size_t>(n), '1');
        std::string to(1, static_cast<char>('0' + n));
        std::size_t pos = 0;
        while ((pos = sb.find(from, pos)) != std::string::npos) {
            sb.replace(pos, from.size(), to);
            ++pos;
        }
    }

    sb += ' ';
    sb += (add_ep && cb.epIndex != 0) ? FIELD_NAMES[cb.epIndex] : "-";
    sb += ' ';
    sb += std::to_string(cb.lastCaptureOrPawnMoveBefore);
    sb += ' ';
    sb += std::to_string((cb.playedMovesCount + 1) / 2 + 1);
    return sb;
}

}  // namespace board::cbu
