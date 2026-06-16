// StateManager — UCI command loop. Mirrors bagaturchess.uci.impl.StateManager.
//
// Responsibilities:
//   * Read commands from stdin, write replies to stdout.
//   * Maintain the current ChessBoard (replaced on `position`).
//   * Spawn the search in a background thread on `go`.
//   * On `stop`: signal the search to terminate; the thread itself emits
//     `bestmove` once it returns.
//   * Stream `info ...` lines from the search through the iteration callback.
//
// Minimalist: no UCIOptions, no ponder bookkeeping beyond accepting the flag,
// no Channel abstraction (writes go straight to stdout).

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../board/chess_board.h"
#include "../search/search.h"

namespace uci {

class StateManager {
public:
    StateManager();
    ~StateManager();

    // Main loop. Returns when "quit" is received or stdin is closed.
    int run();

private:
    void  send(const std::string& s) const;
    void  send_hello() const;
    void  send_id_and_uciok() const;
    void  send_readyok() const;

    void  cmd_uci();
    void  cmd_isready();
    void  cmd_ucinewgame();
    void  cmd_position(const std::string& line);
    void  cmd_go(const std::string& line);
    void  cmd_stop();
    void  cmd_quit();

    void  reset_board_to_startpos();
    void  ensure_searcher();
    void  stop_and_join_search();

    std::unique_ptr<board::ChessBoard> board_;
    std::unique_ptr<search::Searcher>  searcher_;

    std::thread       search_thread_;
    std::atomic<bool> search_running_{false};
    int               search_side_;   // colour to move at search start
    int               last_best_move_ = 0;

    mutable std::mutex io_mutex_;
};

}  // namespace uci
