#include "syzygy.h"

#include "../board/move_generator.h"
#include "../board/move_util.h"
#include "tbprobe.h"

namespace syzygy {

namespace {

// Horizontal mirror: flip the file of every set bit (square s -> s ^ 7). This
// converts Bagatur's file-reversed layout (H1 = 0) to/from Fathom's standard
// A1 = 0. Self-inverse.
inline board::BB mirror_files(board::BB b) noexcept {
    b = ((b >> 1) & 0x5555555555555555ULL) | ((b & 0x5555555555555555ULL) << 1);
    b = ((b >> 2) & 0x3333333333333333ULL) | ((b & 0x3333333333333333ULL) << 2);
    b = ((b >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((b & 0x0F0F0F0F0F0F0F0FULL) << 4);
    return b;
}

// Fathom's positional arguments, in standard A1 = 0 orientation.
struct FathomPos {
    board::BB white, black, kings, queens, rooks, bishops, knights, pawns;
    unsigned  ep;
    bool      turn;   // true = white to move
};

FathomPos to_fathom(const board::ChessBoard& cb) {
    using namespace board;
    FathomPos p;
    p.white   = mirror_files(cb.friendlyPieces[WHITE]);
    p.black   = mirror_files(cb.friendlyPieces[BLACK]);
    p.kings   = mirror_files(cb.pieces[WHITE][KING]   | cb.pieces[BLACK][KING]);
    p.queens  = mirror_files(cb.pieces[WHITE][QUEEN]  | cb.pieces[BLACK][QUEEN]);
    p.rooks   = mirror_files(cb.pieces[WHITE][ROOK]   | cb.pieces[BLACK][ROOK]);
    p.bishops = mirror_files(cb.pieces[WHITE][BISHOP] | cb.pieces[BLACK][BISHOP]);
    p.knights = mirror_files(cb.pieces[WHITE][NIGHT]  | cb.pieces[BLACK][NIGHT]);
    p.pawns   = mirror_files(cb.pieces[WHITE][PAWN]   | cb.pieces[BLACK][PAWN]);
    p.ep      = cb.epIndex ? static_cast<unsigned>(cb.epIndex ^ 7) : 0;
    p.turn    = (cb.colorToMove == WHITE);
    return p;
}

// Find the legal Bagatur move matching the given (from, to, promotion) — the
// decoded Fathom root move. Mirrors the matching loop in uci::uci_to_move.
int match_move(board::ChessBoard& cb, int from_b, int to_b, int promo_piece) {
    board::MoveGenerator gen;
    gen.startPly();
    gen.generateMoves(cb);
    gen.generateAttacks(cb);
    int result = 0;
    while (gen.hasNext()) {
        int m = gen.next();
        if (!cb.isLegal(m)) continue;
        if (board::mv::from_index(m) != from_b) continue;
        if (board::mv::to_index(m)   != to_b)   continue;
        if (board::mv::is_promotion(m)) {
            if (promo_piece == 0)                       continue;
            if (board::mv::move_type(m) != promo_piece) continue;
        } else if (promo_piece != 0) {
            continue;
        }
        result = m;
        break;
    }
    gen.endPly();
    return result;
}

int fathom_promo_to_piece(unsigned promo) {
    switch (promo) {
        case TB_PROMOTES_QUEEN:  return board::QUEEN;
        case TB_PROMOTES_ROOK:   return board::ROOK;
        case TB_PROMOTES_BISHOP: return board::BISHOP;
        case TB_PROMOTES_KNIGHT: return board::NIGHT;
        default:                 return 0;
    }
}

}  // namespace

Syzygy& Syzygy::instance() {
    static Syzygy inst;
    return inst;
}

bool Syzygy::init(const std::string& path) {
    shutdown();
    if (path.empty() || path == "<empty>") return false;
    if (!tb_init(path.c_str())) return false;
    inited_     = true;
    max_pieces_ = static_cast<int>(TB_LARGEST);
    return max_pieces_ > 0;
}

void Syzygy::shutdown() {
    if (inited_) {
        tb_free();
        inited_ = false;
    }
    max_pieces_ = 0;
}

int Syzygy::probe_wdl(const board::ChessBoard& cb) const {
    if (max_pieces_ == 0)                            return WDL_FAIL;
    if (cb.castlingRights != 0)                      return WDL_FAIL;
    if (cb.lastCaptureOrPawnMoveBefore != 0)         return WDL_FAIL;  // tb_probe_wdl needs rule50 == 0
    if (board::popcount(cb.allPieces) > max_pieces_) return WDL_FAIL;

    FathomPos p = to_fathom(cb);
    unsigned r = tb_probe_wdl(p.white, p.black, p.kings, p.queens, p.rooks,
                              p.bishops, p.knights, p.pawns,
                              /*rule50=*/0, /*castling=*/0, p.ep, p.turn);
    if (r == TB_RESULT_FAILED) return WDL_FAIL;
    return static_cast<int>(r);   // TB_LOSS..TB_WIN
}

int Syzygy::probe_root(board::ChessBoard& cb, int& wdl_out) const {
    wdl_out = WDL_FAIL;
    if (max_pieces_ == 0)                            return 0;
    if (cb.castlingRights != 0)                      return 0;
    if (board::popcount(cb.allPieces) > max_pieces_) return 0;

    FathomPos p = to_fathom(cb);
    unsigned res = tb_probe_root(p.white, p.black, p.kings, p.queens, p.rooks,
                                 p.bishops, p.knights, p.pawns,
                                 static_cast<unsigned>(cb.lastCaptureOrPawnMoveBefore),
                                 /*castling=*/0, p.ep, p.turn, nullptr);
    if (res == TB_RESULT_FAILED)                            return 0;
    if (res == TB_RESULT_CHECKMATE || res == TB_RESULT_STALEMATE) return 0;

    int from_b      = static_cast<int>(TB_GET_FROM(res)) ^ 7;   // standard -> Bagatur
    int to_b        = static_cast<int>(TB_GET_TO(res))   ^ 7;
    int promo_piece = fathom_promo_to_piece(TB_GET_PROMOTES(res));

    int move = match_move(cb, from_b, to_b, promo_piece);
    if (move == 0) return 0;

    wdl_out = static_cast<int>(TB_GET_WDL(res));
    return move;
}

}  // namespace syzygy
