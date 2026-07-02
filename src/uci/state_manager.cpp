#include "state_manager.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "../board/castling_config.h"
#include "../board/chess_board_util.h"
#include "../nnue/nnue.h"
#include "../syzygy/syzygy.h"
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

void info_callback(const search::Result& r, void* user) {
    // `user` carries the Chess960 castling config (king-takes-rook notation) or
    // nullptr for classic king-target notation. The castling layout is fixed for
    // the whole game, so the root board's config is valid for every PV move.
    const auto* frc_cfg = static_cast<const board::CastlingConfig*>(user);

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
    std::printf("info depth %d seldepth %d nodes %llu time %.0f nps %llu tbhits %llu",
                r.depth, r.seldepth,
                static_cast<unsigned long long>(r.nodes),
                r.time_secs * 1000.0,
                static_cast<unsigned long long>(r.nodes / std::max(r.time_secs, 1e-9)),
                static_cast<unsigned long long>(r.tbhits));
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
            std::printf(" %s", move_to_uci(r.pv[i], frc_cfg).c_str());
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
    send("option name ThreadsCount type spin default 1 min 1 max " + std::to_string(max_threads()));
    send("option name TTSize type spin default " + std::to_string(kDefaultTTSizeMb) + " min 1 max 65536");
    send("option name UCI_Chess960 type check default false");
    send("option name SyzygyPath type string default <empty>");
    send(REPLY_UCIOK);
}

void StateManager::send_readyok() const {
    send(REPLY_READYOK);
}

void StateManager::reset_board_to_startpos() {
    board_ = board::cbu::getNewCB();
}

int StateManager::max_threads() const {
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 1;
    return 2 * static_cast<int>(hc);
}

void StateManager::ensure_tt() {
    if (!tt_) tt_ = std::make_unique<search::TranspositionTable>(static_cast<std::size_t>(tt_size_mb_));
}

void StateManager::ensure_searcher() {
    ensure_tt();
    if (!searcher_) searcher_ = std::make_unique<search::Searcher>(*board_, *tt_);
}

void StateManager::ensure_smp() {
    ensure_tt();
    // Recreate when the thread count changed (worker count is fixed per object).
    if (!smp_ || smp_->threads() != threads_count_)
        smp_ = std::make_unique<search::smp::SMPSearcher>(*board_, *tt_, threads_count_);
}

void StateManager::stop_and_join_search() {
    if (search_thread_.joinable()) {
        if (searcher_) searcher_->stop();
        if (smp_)      smp_->stop();
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
    // Fresh game: drop the searchers and clear the shared TT (kept allocated).
    searcher_.reset();
    smp_.reset();
    if (tt_) tt_->clear();
}

void StateManager::cmd_position(const std::string& line) {
    stop_and_join_search();

    Position pos = Position::parse(line);
    board_ = pos.fen.empty() ? board::cbu::getNewCB()
                              : board::cbu::getNewCB(pos.fen);

    for (const std::string& mv : pos.moves) {
        int m = uci_to_move(*board_, mv, frc960_);
        if (m == 0) {
            // Illegal / unrecognised — refuse to advance further but keep
            // the legal prefix that already applied.
            break;
        }
        board_->doMove(m);
    }

    // Board pointer changed. Re-bind the searcher and refresh NNUE
    // accumulators, but KEEP the searcher alive — its TT (1024 MB by
    // default) and eval cache (128 MB) are expensive to allocate and carry
    // useful info across moves. Dropping them on every `position` command makes
    // NPS collapse ~10× under a GUI like Arena because every move pays
    // the ~1.1 GB allocation again and starts from a cold TT.
    if (searcher_) searcher_->set_board(*board_);
    if (smp_)      smp_->set_board(*board_);
}

void StateManager::cmd_go(const std::string& line) {
    stop_and_join_search();
    reset_info_emitter();   // every `go` starts a fresh emission cadence

    // Root Syzygy probe (DTZ). Runs single-threaded here, BEFORE any worker
    // thread starts — tb_probe_root is not thread-safe. On a hit the optimal
    // move (respecting the 50-move rule) is known immediately; emit one info
    // line + bestmove and skip the search entirely.
    if (syzygy::Syzygy::instance().available()) {
        int wdl = 0;
        int tb_move = syzygy::Syzygy::instance().probe_root(*board_, wdl);
        if (tb_move != 0) {
            int score = (wdl == syzygy::WDL_WIN)  ?  search::TB_WIN_SCORE
                      : (wdl == syzygy::WDL_LOSS) ? -search::TB_WIN_SCORE
                      :  search::SCORE_DRAW;
            const board::CastlingConfig* cfg = frc960_ ? &board_->castlingConfig : nullptr;
            search::Result r;
            r.depth = 1; r.seldepth = 1; r.score = score;
            r.best_move = tb_move; r.pv[0] = tb_move; r.pv_length = 1;
            r.tbhits = 1;
            info_callback(r, const_cast<board::CastlingConfig*>(cfg));
            last_best_move_ = tb_move;
            send(std::string(REPLY_BESTMOVE) + " " + move_to_uci(tb_move, cfg));
            return;
        }
    }

    const bool use_smp = threads_count_ > 1;
    if (use_smp) ensure_smp(); else ensure_searcher();

    Go go = Go::parse(line);
    int colour_to_move = board_->colorToMove;
    search_side_       = colour_to_move;

    TimeBudget budget = compute(go, colour_to_move);

    search::Limits lim;
    lim.use_mtd       = true;
    lim.on_iteration  = info_callback;
    // In Chess960 mode the info callback formats castling as king-takes-rook;
    // pass the (game-stable) castling config through, nullptr ⇒ classic.
    lim.callback_user = frc960_ ? &board_->castlingConfig : nullptr;

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
    search_thread_ = std::thread([this, lim, use_smp]() {
        search::Result res;
        if (use_smp) {
            res = smp_->go(lim);     // SMP bumps the TT generation internally
        } else {
            tt_->new_search();       // single shared-TT searcher won't bump itself
            res = searcher_->go(lim);
        }
        last_best_move_ = res.best_move;

        const board::CastlingConfig* bm_cfg = frc960_ ? &board_->castlingConfig : nullptr;
        std::string reply = std::string(REPLY_BESTMOVE) + " " + move_to_uci(res.best_move, bm_cfg);
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

void StateManager::cmd_setoption(const std::string& line) {
    // Options must not mutate shared state (TT resize, worker recreation) while a
    // search is live — a concurrent TTSize resize would reallocate the table out
    // from under the worker threads. Per UCI, setoption only arrives when idle,
    // but guard anyway.
    stop_and_join_search();

    // setoption name <Name> value <V>
    std::size_t name_pos  = line.find("name ");
    std::size_t value_pos = line.find(" value ");
    if (name_pos == std::string::npos || value_pos == std::string::npos) return;

    std::string name = line.substr(name_pos + 5, value_pos - (name_pos + 5));
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();

    // Raw value string (spin options atoi it, the check option compares it).
    std::string val = line.substr(value_pos + 7);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());

    if (name == "ThreadsCount") {
        int v = std::atoi(val.c_str());
        if (v < 1) v = 1;
        if (v > max_threads()) v = max_threads();
        threads_count_ = v;
    } else if (name == "TTSize") {
        int v = std::atoi(val.c_str());
        if (v < 1)     v = 1;
        if (v > 65536) v = 65536;
        tt_size_mb_ = v;
        if (tt_) tt_->resize(static_cast<std::size_t>(tt_size_mb_));
    } else if (name == "UCI_Chess960") {
        frc960_ = (val == "true" || val == "1");
    } else if (name == "SyzygyPath") {
        // Load (or unload, on "<empty>") the Syzygy tablebases. Done while the
        // search is idle (joined above) since tb_init reallocates global state.
        syzygy::Syzygy::instance().init(val);
    }
    // Unknown options are silently ignored (per UCI).
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
        else if (cmd == CMD_SETOPTION) cmd_setoption(line);
        // unknown commands are silently ignored, per UCI spec
    }
    return 0;
}

}  // namespace uci
