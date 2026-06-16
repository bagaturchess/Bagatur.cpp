#include "state_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "../board/chess_board_util.h"
#include "../nnue/nnue.h"
#include "go.h"
#include "position.h"
#include "protocol.h"
#include "time_control.h"
#include "uci_move.h"

namespace uci {

namespace {

// Per-iteration info callback — runs on the search thread and prints UCI
// "info" lines directly to stdout. Atomic flush after each line keeps GUIs
// like cutechess responsive.
void info_callback(const search::Result& r, void* /*user*/) {
    std::printf("info depth %d seldepth %d nodes %llu time %.0f nps %llu score cp %d pv",
                r.depth, r.seldepth,
                static_cast<unsigned long long>(r.nodes),
                r.time_secs * 1000.0,
                static_cast<unsigned long long>(r.nodes / std::max(r.time_secs, 1e-9)),
                r.score);
    for (int i = 0; i < r.pv_length; ++i) {
        std::printf(" %s", move_to_uci(r.pv[i]).c_str());
    }
    std::printf("\n");
    std::fflush(stdout);
}

}  // namespace

StateManager::StateManager() {
    board::ChessBoard::initGlobals();
    nnue::Network::instance();  // pre-load the NNUE network so the first `go` is responsive
    reset_board_to_startpos();
}

StateManager::~StateManager() {
    stop_and_join_search();
}

void StateManager::send(const std::string& s) const {
    std::lock_guard<std::mutex> g(io_mutex_);
    std::fputs(s.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void StateManager::send_hello() const {
    // Skipped — cutechess will reject any chatter before `id name` / `uciok`.
}

void StateManager::send_id_and_uciok() const {
    std::string idn = std::string(REPLY_ID_NAME) + " " + ENGINE_NAME + " " + ENGINE_VERSION;
    send(idn);
    send(std::string(REPLY_ID_AUTHOR) + " " + ENGINE_AUTHOR);
    send(REPLY_UCIOK);
}

void StateManager::send_readyok() const {
    send(REPLY_READYOK);
}

void StateManager::reset_board_to_startpos() {
    board_ = board::cbu::getNewCB();
}

void StateManager::ensure_searcher() {
    if (!searcher_) {
        searcher_ = std::make_unique<search::Searcher>(*board_, /*tt_mb=*/512);
    }
}

void StateManager::stop_and_join_search() {
    if (search_thread_.joinable()) {
        if (searcher_) searcher_->stop();
        search_thread_.join();
    }
    search_running_.store(false, std::memory_order_relaxed);
}

void StateManager::cmd_uci() {
    send_id_and_uciok();
}

void StateManager::cmd_isready() {
    send_readyok();
}

void StateManager::cmd_ucinewgame() {
    stop_and_join_search();
    reset_board_to_startpos();
    // Drop the searcher so it gets a fresh TT for the new game.
    searcher_.reset();
}

void StateManager::cmd_position(const std::string& line) {
    stop_and_join_search();

    Position pos = Position::parse(line);
    board_ = pos.fen.empty() ? board::cbu::getNewCB()
                              : board::cbu::getNewCB(pos.fen);

    for (const std::string& mv : pos.moves) {
        int m = uci_to_move(*board_, mv);
        if (m == 0) {
            // Illegal / unrecognised — refuse to advance further but keep
            // the legal prefix that already applied.
            break;
        }
        board_->doMove(m);
    }

    // Board changed → searcher's bound Searcher::cb_ reference is still
    // valid (we own *board_), but NNUE accumulators need to be refreshed
    // before the next search. Easiest: rebuild the searcher.
    searcher_.reset();
}

void StateManager::cmd_go(const std::string& line) {
    stop_and_join_search();
    ensure_searcher();

    Go go = Go::parse(line);
    int colour_to_move = board_->colorToMove;
    search_side_       = colour_to_move;

    TimeBudget budget = compute(go, colour_to_move);

    search::Limits lim;
    lim.use_mtd       = true;
    lim.on_iteration  = info_callback;
    lim.callback_user = nullptr;

    if (budget.depth_limit > 0)        lim.max_depth     = budget.depth_limit;
    if (budget.node_limit  > 0)        lim.max_nodes     = static_cast<std::uint64_t>(budget.node_limit);
    if (budget.move_time_secs > 0.0)   lim.max_time_secs = budget.move_time_secs;

    search_running_.store(true, std::memory_order_relaxed);
    search_thread_ = std::thread([this, lim]() {
        search::Result res = searcher_->go(lim);
        last_best_move_ = res.best_move;

        std::string reply = std::string(REPLY_BESTMOVE) + " " + move_to_uci(res.best_move);
        send(reply);
        search_running_.store(false, std::memory_order_relaxed);
    });
}

void StateManager::cmd_stop() {
    stop_and_join_search();
}

void StateManager::cmd_quit() {
    stop_and_join_search();
}

int StateManager::run() {
    send_hello();
    std::string line;
    while (std::getline(std::cin, line)) {
        // Strip a trailing CR (Windows pipes).
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

        std::string cmd;
        std::size_t sp = line.find(' ');
        cmd = (sp == std::string::npos) ? line : line.substr(0, sp);

        if      (cmd == CMD_UCI)         cmd_uci();
        else if (cmd == CMD_ISREADY)     cmd_isready();
        else if (cmd == CMD_UCINEWGAME)  cmd_ucinewgame();
        else if (cmd == CMD_POSITION)    cmd_position(line);
        else if (cmd == CMD_GO)          cmd_go(line);
        else if (cmd == CMD_STOP)        cmd_stop();
        else if (cmd == CMD_QUIT)      { cmd_quit(); break; }
        else if (cmd == CMD_PONDERHIT) { /* pondering not supported in this minimal port */ }
        else if (cmd == CMD_SETOPTION) { /* no options defined — silently accept */ }
        // unknown commands are silently ignored, per UCI spec
    }
    return 0;
}

}  // namespace uci
