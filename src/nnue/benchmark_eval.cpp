// benchmark_eval — NPS benchmark that runs an NNUE evaluation at every
// visited node. Counterpart to board/benchmark.cpp (which only does move
// generation + do/undo).
//
// Comparing the two numbers gives a direct read on how much eval costs in
// the search-loop hot path.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "../board/chess_board.h"
#include "../board/chess_board_util.h"
#include "../board/move_generator.h"
#include "nnue.h"

namespace nnue {

using clk = std::chrono::steady_clock;

static std::atomic<std::int64_t> g_sink{0};

static std::uint64_t perft_eval(board::ChessBoard& cb, board::MoveGenerator& gen,
                                Evaluator& ev, int depth) {
    if (depth == 0) return 1;
    gen.startPly();
    gen.generateMoves(cb);
    gen.generateAttacks(cb);

    std::uint64_t nodes = 0;
    while (gen.hasNext()) {
        int move = gen.next();
        if (!cb.isLegal(move)) continue;

        int side_moved = cb.colorToMove;
        cb.doMove(move);
        ev.after_make(cb, move, side_moved);

        int score = ev.evaluate(cb);
        g_sink.fetch_add(score, std::memory_order_relaxed);

        nodes += perft_eval(cb, gen, ev, depth - 1);

        ev.after_unmake(cb, move, side_moved);
        cb.undoMove(move);
    }
    gen.endPly();
    return nodes;
}

struct Bench { const char* fen; int depth; const char* label; };

static const Bench kBench[] = {
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",         5, "startpos D5" },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", 4, "Kiwipete D4" },
    { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",                            6, "Position3 D6" },
    { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, "Position4 D4" },
    { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",        4, "Position5 D4" },
};

int run() {
    board::ChessBoard::initGlobals();
    Network::instance();  // pre-load network so it doesn't taint timings

    std::uint64_t total_nodes = 0;
    double        total_time  = 0;
    std::printf("%-16s  %5s  %15s  %12s  %10s\n", "label", "depth", "nodes", "seconds", "Mnps");
    std::printf("---------------------------------------------------------------------\n");
    for (const Bench& b : kBench) {
        auto cb = board::cbu::getNewCB(b.fen);
        board::MoveGenerator gen;
        Evaluator ev;
        ev.reset(*cb);
        auto t0 = clk::now();
        auto nodes = perft_eval(*cb, gen, ev, b.depth);
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
    std::printf("(sink = %lld — keeps the optimiser honest)\n",
                static_cast<long long>(g_sink.load()));
    return 0;
}

}  // namespace nnue

int main() { return nnue::run(); }
