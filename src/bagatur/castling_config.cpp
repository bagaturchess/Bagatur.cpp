#include "castling_config.h"

#include "chess_constants.h"

namespace bagatur {

void CastlingConfig::initFor(int from_king_w_, int from_rook_ks_w_, int from_rook_qs_w_,
                             int from_king_b_, int from_rook_ks_b_, int from_rook_qs_b_) {
    from_king_w    = from_king_w_;
    from_rook_ks_w = from_rook_ks_w_;
    from_rook_qs_w = from_rook_qs_w_;
    from_king_b    = from_king_b_;
    from_rook_ks_b = from_rook_ks_b_;
    from_rook_qs_b = from_rook_qs_b_;

    const auto& IB = cc::IN_BETWEEN;

    bb_inbetween_king_ks_w = IB[from_king_w][G1] | (1ULL << G1);
    bb_inbetween_king_qs_w = IB[from_king_w][C1] | (1ULL << C1);
    bb_inbetween_rook_ks_w = IB[from_rook_ks_w][F1] | (1ULL << F1);
    bb_inbetween_rook_qs_w = IB[from_rook_qs_w][D1] | (1ULL << D1);

    bb_inbetween_king_ks_b = IB[from_king_b][G8] | (1ULL << G8);
    bb_inbetween_king_qs_b = IB[from_king_b][C8] | (1ULL << C8);
    bb_inbetween_rook_ks_b = IB[from_rook_ks_b][F8] | (1ULL << F8);
    bb_inbetween_rook_qs_b = IB[from_rook_qs_b][D8] | (1ULL << D8);
}

CastlingConfig CastlingConfig::classic() {
    CastlingConfig c;
    c.initFor(E1, H1, A1, E8, H8, A8);
    return c;
}

}  // namespace bagatur
