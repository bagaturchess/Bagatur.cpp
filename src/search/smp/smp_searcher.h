// smp_searcher.h — Lazy-SMP coordinator.
//
// C++ port of bagaturchess MTDParallelSearch_ThreadsImpl + _BaseImpl: runs N
// independent `Searcher` workers, one per thread, on N private copies of the
// position, all sharing ONE lock-free transposition table (the only inter-thread
// cooperation). Each worker self-times with the same budget, so they finish
// together; the coordinator streams the merged info to the GUI and returns the
// merged best move. (RootSearchFirstMoveIndex diversification is intentionally
// not used, per the port's scope.)

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "../../board/chess_board.h"
#include "../search.h"
#include "../tt.h"
#include "searchers_merge.h"

namespace search::smp {

class SMPSearcher {
public:
    SMPSearcher(board::ChessBoard& root, TranspositionTable& shared_tt, int num_threads);

    Result go(const Limits& lim);
    void   stop() noexcept;
    void   set_board(board::ChessBoard& root);

    TranspositionTable& tt()      noexcept { return *tt_; }
    int                 threads() const noexcept { return num_threads_; }

private:
    struct WorkerCtx { SMPSearcher* self; int id; };
    static void worker_cb(const Result& r, void* user);

    TranspositionTable* tt_;
    int                 num_threads_;

    std::vector<std::unique_ptr<board::ChessBoard>> boards_;   // one private copy per worker
    std::vector<std::unique_ptr<Searcher>>          workers_;
    std::vector<WorkerCtx>                          ctx_;       // stable callback_user payloads

    SearchersMerge    merge_;
    std::atomic<bool> stop_{false};
    std::atomic<int>  running_{0};
};

}  // namespace search::smp
