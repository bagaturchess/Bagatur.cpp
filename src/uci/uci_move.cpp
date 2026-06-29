#include "uci_move.h"

#include "../board/castling_config.h"
#include "../board/move_generator.h"

namespace uci {

namespace {

// Chess960: map a castling move's king-target (G1/C1/G8/C8) to the rook's
// start square, which is the destination square used in king-takes-rook
// notation. Falls through to the input for any non-castling target.
int rook_from_for_king_target(const board::CastlingConfig& c, int king_to) {
    using CC = board::CastlingConfig;
    switch (king_to) {
        case CC::G1: return c.from_rook_ks_w;
        case CC::C1: return c.from_rook_qs_w;
        case CC::G8: return c.from_rook_ks_b;
        case CC::C8: return c.from_rook_qs_b;
    }
    return king_to;
}

}  // namespace

int uci_to_move(board::ChessBoard& cb, const std::string& uci, bool frc) {
    if (uci.size() < 4) return 0;

    // UCI: A1 = file 0, rank 0; H1 = file 7, rank 0; ...
    int from_n = (uci[0] - 'a') + (uci[1] - '1') * 8;
    int to_n   = (uci[2] - 'a') + (uci[3] - '1') * 8;

    // Convert to Bagatur layout (file index reversed): H1 = 0, A1 = 7.
    int from_b = from_n ^ 7;
    int to_b   = to_n   ^ 7;

    int promo = 0;
    if (uci.size() >= 5) {
        switch (uci[4]) {
            case 'n': promo = board::NIGHT;  break;
            case 'b': promo = board::BISHOP; break;
            case 'r': promo = board::ROOK;   break;
            case 'q': promo = board::QUEEN;  break;
            default: break;
        }
    }

    board::MoveGenerator gen;
    gen.startPly();
    gen.generateMoves(cb);
    gen.generateAttacks(cb);
    int result = 0;
    while (gen.hasNext()) {
        int m = gen.next();
        if (!cb.isLegal(m)) continue;
        if (board::mv::from_index(m) != from_b) continue;

        int  m_to       = board::mv::to_index(m);
        bool to_matches = (m_to == to_b);

        // Chess960: castling arrives as king-takes-rook, so the input's
        // destination is the rook's start square, not the king's two-square
        // target. (Classic "e1g1" still matches above; in 960 positions where
        // the rook sits on the king-target square the two forms coincide.)
        if (!to_matches && frc && board::mv::is_castling(m)) {
            if (rook_from_for_king_target(cb.castlingConfig, m_to) == to_b)
                to_matches = true;
        }
        if (!to_matches) continue;

        if (board::mv::is_promotion(m)) {
            if (promo == 0)                            continue;
            if (board::mv::move_type(m) != promo)      continue;
        } else if (promo != 0) {
            continue;
        }
        result = m;
        break;
    }
    gen.endPly();
    return result;
}

std::string move_to_uci(int move, const board::CastlingConfig* frc_cfg) {
    if (move == 0) return "0000";

    int from_b = board::mv::from_index(move);
    int to_b   = board::mv::to_index(move);

    // Chess960: emit castling as king-takes-rook (destination = rook square).
    if (frc_cfg != nullptr && board::mv::is_castling(move)) {
        to_b = rook_from_for_king_target(*frc_cfg, to_b);
    }

    int from_n = from_b ^ 7;
    int to_n   = to_b   ^ 7;

    auto sq_str = [](int s) {
        char f = static_cast<char>('a' + (s & 7));
        char r = static_cast<char>('1' + (s >> 3));
        return std::string{f, r};
    };

    std::string out = sq_str(from_n) + sq_str(to_n);
    if (board::mv::is_promotion(move)) {
        char c = '?';
        switch (board::mv::move_type(move)) {
            case board::NIGHT:  c = 'n'; break;
            case board::BISHOP: c = 'b'; break;
            case board::ROOK:   c = 'r'; break;
            case board::QUEEN:  c = 'q'; break;
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace uci
