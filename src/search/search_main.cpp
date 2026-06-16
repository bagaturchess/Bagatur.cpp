// search_main — small UCI-style driver. Loads startpos (or a FEN given as
// command-line arg), runs iterative deepening, prints the PV at every depth.
//
// Usage:
//   search_main                       # startpos, depth 8 default
//   search_main 12                    # startpos, depth 12
//   search_main 12 "fen here ..."     # FEN, depth 12
//   search_main 12 "fen" 5            # FEN, depth 12, time-limit 5 s
//
// Output is one `info depth …` line per iteration and a final `bestmove`.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "../board/chess_board.h"
#include "../board/chess_board_util.h"
#include "../board/move_util.h"
#include "search.h"

namespace search {

// Render a Bagatur-encoded move as UCI long-algebraic notation. Bagatur uses
// H1=0, A1=7 file-reversed indexing; UCI uses A1=0, H1=7, so we flip files.
static std::string move_to_uci(int move) {
    if (move == 0) return "0000";
    int from_b = board::mv::from_index(move);
    int to_b   = board::mv::to_index(move);
    int from_n = from_b ^ 7;
    int to_n   = to_b   ^ 7;
    auto sq    = [](int s) {
        char f = static_cast<char>('a' + (s & 7));
        char r = static_cast<char>('1' + (s >> 3));
        return std::string{f, r};
    };
    std::string out = sq(from_n) + sq(to_n);
    if (board::mv::is_promotion(move)) {
        int p = board::mv::move_type(move);
        char c = '?';
        switch (p) {
            case board::NIGHT: c = 'n'; break;
            case board::BISHOP: c = 'b'; break;
            case board::ROOK: c = 'r'; break;
            case board::QUEEN: c = 'q'; break;
        }
        out.push_back(c);
    }
    return out;
}

int run(int argc, char** argv) {
    board::ChessBoard::initGlobals();
    nnue::Network::instance();  // pre-load network

    int        depth     = 8;
    std::string fen      = std::string(board::cbu::FEN_START);
    double     time_secs = 0.0;

    if (argc >= 2) depth = std::atoi(argv[1]);
    if (argc >= 3) fen   = argv[2];
    if (argc >= 4) time_secs = std::atof(argv[3]);

    std::printf("position fen: %s\n", fen.c_str());
    std::printf("limits: depth=%d  time=%.2fs\n\n", depth, time_secs);

    auto cb = board::cbu::getNewCB(fen);
    // Heap-allocate — Searcher carries multi-MB accumulator/PV stacks.
    auto sr = std::make_unique<Searcher>(*cb, /*tt_mb=*/64);

    Limits lim;
    lim.max_depth     = depth;
    lim.max_time_secs = time_secs;

    // Per-iteration printer — UCI-flavoured "info" line on every completed depth.
    lim.on_iteration  = [](const Result& r, void* /*user*/) {
        std::printf("info depth %d seldepth %d nodes %llu time %.0f nps %llu score cp %d pv",
                    r.depth, r.seldepth,
                    static_cast<unsigned long long>(r.nodes),
                    r.time_secs * 1000.0,
                    static_cast<unsigned long long>(r.nodes / std::max(r.time_secs, 1e-9)),
                    r.score);
        for (int i = 0; i < r.pv_length; ++i)
            std::printf(" %s", move_to_uci(r.pv[i]).c_str());
        std::printf("\n");
        std::fflush(stdout);
    };

    auto res = sr->go(lim);

    std::printf("bestmove %s\n", move_to_uci(res.best_move).c_str());
    return 0;
}

}  // namespace search

int main(int argc, char** argv) { return search::run(argc, argv); }
