// CastlingConfig — FRC/Chess960 castling layout. Mirrors CastlingConfig.java.

#pragma once

#include "types.h"

namespace board {

struct CastlingConfig {
    // Reference square indices (Bagatur's reversed-file layout).
    static constexpr int A1 = 7;
    static constexpr int C1 = 5;
    static constexpr int D1 = 4;
    static constexpr int E1 = 3;
    static constexpr int F1 = 2;
    static constexpr int G1 = 1;
    static constexpr int H1 = 0;
    static constexpr int A8 = 63;
    static constexpr int C8 = 61;
    static constexpr int D8 = 60;
    static constexpr int E8 = 59;
    static constexpr int F8 = 58;
    static constexpr int G8 = 57;
    static constexpr int H8 = 56;

    int from_king_w        = E1;
    int from_rook_ks_w     = H1;  // kingside rook
    int from_rook_qs_w     = A1;  // queenside rook
    int from_king_b        = E8;
    int from_rook_ks_b     = H8;
    int from_rook_qs_b     = A8;

    BB bb_inbetween_king_ks_w = 0;
    BB bb_inbetween_king_qs_w = 0;
    BB bb_inbetween_rook_ks_w = 0;
    BB bb_inbetween_rook_qs_w = 0;
    BB bb_inbetween_king_ks_b = 0;
    BB bb_inbetween_king_qs_b = 0;
    BB bb_inbetween_rook_ks_b = 0;
    BB bb_inbetween_rook_qs_b = 0;

    void initFor(int from_king_w_, int from_rook_ks_w_, int from_rook_qs_w_,
                 int from_king_b_, int from_rook_ks_b_, int from_rook_qs_b_);

    static CastlingConfig classic();
};

}  // namespace board
