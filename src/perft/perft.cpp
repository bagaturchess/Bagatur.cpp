// Perft correctness driver — verifies move generation against well-known
// reference counts from the chessprogramming wiki.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "bagatur/chess_board.h"
#include "bagatur/chess_board_util.h"
#include "bagatur/move_generator.h"

using namespace bagatur;

static std::uint64_t perft(ChessBoard& cb, MoveGenerator& gen, int depth) {
    if (depth == 0) return 1;
    gen.startPly();
    gen.generateMoves(cb);
    gen.generateAttacks(cb);

    std::uint64_t nodes = 0;
    while (gen.hasNext()) {
        int move = gen.next();
        if (!cb.isLegal(move)) continue;
        cb.doMove(move);
        nodes += perft(cb, gen, depth - 1);
        cb.undoMove(move);
    }
    gen.endPly();
    return nodes;
}

struct Case {
    const char*   fen;
    int           depth;
    std::uint64_t expected;
    const char*   label;
};

static const Case kCases[] = {
    // Position 1 — startpos (Wiki)
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",       5,        4865609, "startpos D5" },
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",       6,      119060324, "startpos D6" },

    // Position 2 — "Kiwipete" (Peter McKenzie)
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", 5,      193690690, "Kiwipete D5" },

    // Position 3 — endgame
    { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",                          6,       11030083, "Position3 D6" },

    // Position 4 — mirrored
    { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 5,       15833292, "Position4 D5" },

    // Position 5 — Steven Edwards
    { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",        5,       89941194, "Position5 D5" },

    // Position 6 — Marcel van Kervinck
    { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - -", 5,    164075551, "Position6 D5" },
};

int main(int argc, char** argv) {
    ChessBoard::initGlobals();

    if (argc >= 2 && std::string_view(argv[1]) == "single") {
        if (argc < 4) {
            std::printf("usage: perft single \"FEN\" depth\n");
            return 1;
        }
        auto cb  = cbu::getNewCB(argv[2]);
        int  depth = std::atoi(argv[3]);
        MoveGenerator gen;
        auto t0 = std::chrono::steady_clock::now();
        auto nodes = perft(*cb, gen, depth);
        auto t1 = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        double nps = nodes / std::max(s, 1e-9);
        std::printf("nodes = %llu  time = %.3fs  nps = %.2f Mnps\n",
                    static_cast<unsigned long long>(nodes), s, nps / 1e6);
        return 0;
    }

    int failures = 0;
    for (const Case& c : kCases) {
        auto cb = cbu::getNewCB(c.fen);
        MoveGenerator gen;
        auto t0 = std::chrono::steady_clock::now();
        std::uint64_t nodes = perft(*cb, gen, c.depth);
        auto t1 = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        double nps = nodes / std::max(s, 1e-9);
        bool ok = nodes == c.expected;
        std::printf("[%s] %-20s depth=%d  nodes=%llu  expected=%llu  time=%.3fs  nps=%.2f Mnps\n",
                    ok ? "OK  " : "FAIL", c.label, c.depth,
                    static_cast<unsigned long long>(nodes),
                    static_cast<unsigned long long>(c.expected),
                    s, nps / 1e6);
        if (!ok) ++failures;
    }
    if (failures != 0) {
        std::printf("\n%d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("\nAll perft cases passed.\n");
    return 0;
}
