// NPS benchmark — measures the do/undo + move-generation throughput on a
// fixed depth-6 perft from start position, plus a stress mix of mid-game
// positions. Use this to compare the Java baseline against the C++ port.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "board/chess_board.h"
#include "board/chess_board_util.h"
#include "board/move_generator.h"

using namespace board;
using clk = std::chrono::steady_clock;

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

struct Bench { const char* fen; int depth; const char* label; };

static const Bench kBench[] = {
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",         6, "startpos D6" },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", 5, "Kiwipete D5" },
    { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",                            7, "Position3 D7" },
    { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 5, "Position4 D5" },
    { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",        5, "Position5 D5" },
};

int main() {
    ChessBoard::initGlobals();

    std::uint64_t total_nodes = 0;
    double        total_time  = 0;
    std::printf("%-16s  %5s  %15s  %12s  %10s\n", "label", "depth", "nodes", "seconds", "Mnps");
    std::printf("---------------------------------------------------------------------\n");
    for (const Bench& b : kBench) {
        auto cb  = cbu::getNewCB(b.fen);
        MoveGenerator gen;
        auto t0 = clk::now();
        auto nodes = perft(*cb, gen, b.depth);
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        double nps = nodes / std::max(s, 1e-9);
        std::printf("%-16s  %5d  %15llu  %12.3f  %10.2f\n",
                    b.label, b.depth,
                    static_cast<unsigned long long>(nodes), s, nps / 1e6);
        total_nodes += nodes;
        total_time  += s;
    }
    std::printf("---------------------------------------------------------------------\n");
    std::printf("%-16s  %5s  %15llu  %12.3f  %10.2f\n", "TOTAL", "",
                static_cast<unsigned long long>(total_nodes),
                total_time, (total_nodes / std::max(total_time, 1e-9)) / 1e6);
    return 0;
}
