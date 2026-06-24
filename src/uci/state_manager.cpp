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
// "info" lines directly to stdout.
//
// Mirrors Java SearchInfoUtils.buildMajorInfoCommand():
//   * append `lowerbound` / `upperbound` after the score
//   * skip the `pv` segment when the score is an upper bound (the best move
//     from a failed-low MTD probe is "the one that failed least" — printing
//     it as PV misleads the GUI).
//
// Rate-limit: emit at most once per ~80 ms unless the depth changes or a
// mate score is found. MTD bumps depth quickly at the shallow phase and
// the raw callback rate hit ~150 emissions/second early on. When a GUI
// runs under a kernel-level EDR (corporate AV, CrowdStrike, etc.) every
// stdout flush goes through a scan callback — at that volume the engine
// thread spends most of its time blocked on WriteFile and NPS collapses.
// Locally there is no EDR so the un-throttled output behaves fine, but
// for Arena/cutechess on managed Windows boxes the throttle is needed.
struct InfoEmitterState {
    int    last_depth          = -1;
    int    last_best_move      = 0;
    double last_emit_secs      = -1e9;
    bool   pv_emitted_at_depth = false;   // any non-upperbound emit at current depth?
};
InfoEmitterState g_info_state;

constexpr double kMinInfoIntervalSecs = 0.080;  // 80 ms ≈ 12 emissions/sec ceiling

void info_callback(const search::Result& r, void* /*user*/) {
    int  cur_best   = (r.pv_length > 0) ? r.pv[0] : 0;

    bool depth_changed      = (r.depth != g_info_state.last_depth);
    bool best_move_changed  = (cur_best != 0 && cur_best != g_info_state.last_best_move);
    bool is_mate            = (r.score >= search::MATE_THRESHOLD) || (r.score <= -search::MATE_THRESHOLD);
    bool overdue            = (r.time_secs - g_info_state.last_emit_secs) >= kMinInfoIntervalSecs;

    // Bound flag — see search::Result. lowerbound/exact carry PV; upperbound
    // omits it (per Java SearchInfoUtils convention).
    bool has_pv             = !r.upper_bound;

    // MTD often produces alternating upperbound (fail-low) and lowerbound
    // (fail-high) iterations at the same depth. The 80 ms throttle was
    // suppressing the second one, so depths that started with upperbound
    // never got a PV-bearing info line — the GUI was left with no PV for
    // every other depth ("info depth N skipped" symptom). Force-emit the
    // first PV-bearing iteration at each depth, regardless of throttle.
    if (depth_changed) g_info_state.pv_emitted_at_depth = false;
    bool needs_first_pv_at_depth = has_pv && !g_info_state.pv_emitted_at_depth;

    // CRITICAL: always emit when the chosen move changes, regardless of
    // throttle. Otherwise the GUI keeps an older PV in its display while
    // the engine internally migrates `Result::best_move` — we then ship
    // `bestmove X` to a GUI that's still showing PV starting with Y.
    if (!depth_changed && !best_move_changed && !is_mate
        && !overdue && !needs_first_pv_at_depth) return;

    g_info_state.last_depth      = r.depth;
    g_info_state.last_best_move  = cur_best;
    g_info_state.last_emit_secs  = r.time_secs;
    if (has_pv) g_info_state.pv_emitted_at_depth = true;

    const char* bound_suffix = r.lower_bound ? " lowerbound"
                             : r.upper_bound ? " upperbound"
                                             : "";

    // UCI: mate scores must be reported as `score mate N` (N in MOVES, signed
    // — positive = we deliver mate, negative = we get mated). Centipawn-format
    // is only for non-mate scores. GUIs (Arena, cutechess, Banksia) parse the
    // two as distinct outputs — printing `score cp 29996` for a forced mate
    // leaves the GUI's mate-indicator dark and breaks live mate-distance
    // displays during tournament play.
    std::printf("info depth %d seldepth %d nodes %llu time %.0f nps %llu",
                r.depth, r.seldepth,
                static_cast<unsigned long long>(r.nodes),
                r.time_secs * 1000.0,
                static_cast<unsigned long long>(r.nodes / std::max(r.time_secs, 1e-9)));
    if (search::is_mate_score(r.score)) {
        // plies = MAX_MATE - |score| (mate scoring convention in this engine).
        // UCI wants the move count rounded up: moves = (plies + 1) / 2.
        int plies = search::MAX_MATE - std::abs(r.score);
        int moves = (plies + 1) / 2;
        if (r.score < 0) moves = -moves;
        std::printf(" score mate %d%s", moves, bound_suffix);
    } else {
        std::printf(" score cp %d%s", r.score, bound_suffix);
    }
    if (!r.upper_bound) {
        std::printf(" pv");
        for (int i = 0; i < r.pv_length; ++i) {
            std::printf(" %s", move_to_uci(r.pv[i]).c_str());
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

void reset_info_emitter() {
    g_info_state.last_depth          = -1;
    g_info_state.last_best_move      = 0;
    g_info_state.last_emit_secs      = -1e9;
    g_info_state.pv_emitted_at_depth = false;
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

    // Board pointer changed. Re-bind the searcher and refresh NNUE
    // accumulators, but KEEP the searcher alive — its TT (512 MB) and
    // eval cache (128 MB) are expensive to allocate and carry useful
    // info across moves. Dropping them on every `position` command makes
    // NPS collapse ~10× under a GUI like Arena because every move pays
    // the 640 MB allocation again and starts from a cold TT.
    if (searcher_) {
        searcher_->set_board(*board_);
    }
}

void StateManager::cmd_go(const std::string& line) {
    stop_and_join_search();
    ensure_searcher();
    reset_info_emitter();   // every `go` starts a fresh emission cadence

    Go go = Go::parse(line);
    int colour_to_move = board_->colorToMove;
    search_side_       = colour_to_move;

    TimeBudget budget = compute(go, colour_to_move);

    search::Limits lim;
    lim.use_mtd       = true;
    lim.on_iteration  = info_callback;
    lim.callback_user = nullptr;

    if (budget.depth_limit > 0)  lim.max_depth = budget.depth_limit;
    if (budget.node_limit  > 0)  lim.max_nodes = static_cast<std::uint64_t>(budget.node_limit);
    // Dynamic time budget (Java MoveEvalInAccount + ConsumedTimeVSRemaining).
    // All four fields are 0 for INFINITE / FIXED_DEPTH / FIXED_NODES — the
    // searcher treats `min_move_secs == 0` as "no time cap".
    lim.min_move_secs         = budget.min_move_secs;
    lim.total_clock_secs      = budget.total_clock_secs;
    lim.max_usage_percent     = budget.max_usage_percent;
    lim.consumed_vs_remaining = budget.consumed_vs_remaining;

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
