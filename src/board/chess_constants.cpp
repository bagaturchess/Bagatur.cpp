#include "chess_constants.h"

#include "static_moves.h"

namespace board::cc {

std::array<std::array<BB, 64>, 64> IN_BETWEEN{};
std::array<std::array<BB, 64>, 64> PINNED_MOVEMENT{};
std::array<std::array<BB, 64>, 2>  KING_AREA{};

namespace {

void init_in_between() {
    // Horizontal / vertical (matches ChessConstants.java first static block).
    for (int from = 0; from < 64; ++from) {
        for (int to = from + 1; to < 64; ++to) {
            if (from / 8 == to / 8) {
                for (int i = to - 1; i > from; --i)
                    IN_BETWEEN[from][to] |= 1ULL << i;
            }
            if (from % 8 == to % 8) {
                for (int i = to - 8; i > from; i -= 8)
                    IN_BETWEEN[from][to] |= 1ULL << i;
            }
        }
    }
    // Diagonals (second static block).
    for (int from = 0; from < 64; ++from) {
        for (int to = from + 1; to < 64; ++to) {
            if ((to - from) % 9 == 0 && to % 8 > from % 8) {
                for (int i = to - 9; i > from; i -= 9)
                    IN_BETWEEN[from][to] |= 1ULL << i;
            }
            if ((to - from) % 7 == 0 && to % 8 < from % 8) {
                for (int i = to - 7; i > from; i -= 7)
                    IN_BETWEEN[from][to] |= 1ULL << i;
            }
        }
    }
    // Mirror to (to < from) cells.
    for (int from = 0; from < 64; ++from)
        for (int to = 0; to < from; ++to)
            IN_BETWEEN[from][to] = IN_BETWEEN[to][from];
}

void init_pinned_movement() {
    static constexpr int DIRECTION[8] = { -1, -7, -8, -9, 1, 7, 8, 9 };
    for (int pinned = 0; pinned < 64; ++pinned) {
        for (int king = 0; king < 64; ++king) {
            int correct = 0;
            for (int dir : DIRECTION) {
                if (correct != 0) break;
                int xray = king + dir;
                while (xray >= 0 && xray < 64) {
                    if (dir == -1 || dir == -9 || dir == 7) {
                        if ((xray & 7) == 7) break;
                    }
                    if (dir == 1 || dir == 9 || dir == -7) {
                        if ((xray & 7) == 0) break;
                    }
                    if (xray == pinned) {
                        correct = dir;
                        break;
                    }
                    xray += dir;
                }
            }
            if (correct != 0) {
                int xray = king + correct;
                while (xray >= 0 && xray < 64) {
                    if (correct == -1 || correct == -9 || correct == 7) {
                        if ((xray & 7) == 7) break;
                    }
                    if (correct == 1 || correct == 9 || correct == -7) {
                        if ((xray & 7) == 0) break;
                    }
                    PINNED_MOVEMENT[pinned][king] |= 1ULL << xray;
                    xray += correct;
                }
            }
        }
    }
}

void init_king_area() {
    using static_moves::KING_MOVES;

    // Base: king moves + own square.
    for (int i = 0; i < 64; ++i) {
        KING_AREA[WHITE][i] |= KING_MOVES[i] | (1ULL << i);
        KING_AREA[BLACK][i] |= KING_MOVES[i] | (1ULL << i);
        if (i > 15) KING_AREA[BLACK][i] |= KING_MOVES[i] >> 8;
        if (i < 48) KING_AREA[WHITE][i] |= KING_MOVES[i] << 8;
    }
    // Force 3-wide at the edges.
    for (int i = 0; i < 64; ++i) {
        for (int color = 0; color < 2; ++color) {
            if (i % 8 == 0)      KING_AREA[color][i] |= KING_AREA[color][i + 1];
            else if (i % 8 == 7) KING_AREA[color][i] |= KING_AREA[color][i - 1];
        }
    }
    // Force 4 long.
    for (int i = 0; i < 64; ++i) {
        if (i < 8) {
            KING_AREA[WHITE][i] = KING_AREA[WHITE][i + 8];
        } else if (i > 47) {
            KING_AREA[WHITE][i] = KING_AREA[WHITE][i > 55 ? i - 16 : i - 8];
        }
    }
    for (int i = 0; i < 64; ++i) {
        if (i > 55) {
            KING_AREA[BLACK][i] = KING_AREA[BLACK][i - 8];
        } else if (i < 16) {
            KING_AREA[BLACK][i] = KING_AREA[BLACK][i < 8 ? i + 16 : i + 8];
        }
    }
}

}  // anonymous

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    init_in_between();
    init_pinned_movement();
    init_king_area();
}

}  // namespace board::cc
