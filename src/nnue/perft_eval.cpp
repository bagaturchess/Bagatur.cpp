// perft_eval — perft that also runs an NNUE evaluation at every visited
// node. Used to verify correctness AND measure NPS-with-eval.
//
// The node count must match the plain perft (eval is side-effect-free), so
// any drift indicates an accumulator bug.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "../board/chess_board.h"
#include "../board/chess_board_util.h"
#include "../board/move_generator.h"
#include "nnue.h"

namespace nnue {

// Sink for evaluation results so the optimiser cannot fold the call away.
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

struct Case {
    const char*   fen;
    int           depth;
    std::uint64_t expected;
    const char*   label;
};

static const Case kCases[] = {
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",       4,         197281, "startpos D4" },
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",       5,        4865609, "startpos D5" },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", 4,        4085603, "Kiwipete D4" },
    { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",                          5,         674624,  "Position3 D5" },
    { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4,         422333, "Position4 D4" },
    { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",        4,        2103487, "Position5 D4" },
};

int run(int argc, char** argv) {
    board::ChessBoard::initGlobals();
    Network::instance();  // load network now so we don't pay it inside timing

    if (argc >= 2 && std::string_view(argv[1]) == "single") {
        if (argc < 4) {
            std::printf("usage: perft_eval single \"FEN\" depth\n");
            return 1;
        }
        auto cb    = board::cbu::getNewCB(argv[2]);
        int  depth = std::atoi(argv[3]);
        board::MoveGenerator gen;
        Evaluator ev;
        ev.reset(*cb);
        auto t0 = std::chrono::steady_clock::now();
        auto nodes = perft_eval(*cb, gen, ev, depth);
        auto t1 = std::chrono::steady_clock::now();
        double s   = std::chrono::duration<double>(t1 - t0).count();
        double nps = nodes / std::max(s, 1e-9);
        std::printf("nodes = %llu  time = %.3fs  nps = %.2f Mnps  (sink=%lld)\n",
                    static_cast<unsigned long long>(nodes), s, nps / 1e6,
                    static_cast<long long>(g_sink.load()));
        return 0;
    }

    int failures = 0;
    for (const Case& c : kCases) {
        auto cb = board::cbu::getNewCB(c.fen);
        board::MoveGenerator gen;
        Evaluator ev;
        ev.reset(*cb);
        auto t0 = std::chrono::steady_clock::now();
        std::uint64_t nodes = perft_eval(*cb, gen, ev, c.depth);
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
    std::printf("\nAll perft_eval cases passed (sink=%lld).\n",
                static_cast<long long>(g_sink.load()));
    return 0;
}

}  // namespace nnue

int main(int argc, char** argv) { return nnue::run(argc, argv); }
