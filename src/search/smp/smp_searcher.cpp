#include "smp_searcher.h"

#include <chrono>
#include <thread>

namespace search::smp {

SMPSearcher::SMPSearcher(board::ChessBoard& root, TranspositionTable& shared_tt, int num_threads)
    : tt_(&shared_tt),
      num_threads_(num_threads < 1 ? 1 : num_threads),
      merge_(num_threads_, 1.0 / num_threads_) {   // threshold: first worker to reach a depth

    boards_.reserve(num_threads_);
    workers_.reserve(num_threads_);
    ctx_.reserve(num_threads_);

    for (int i = 0; i < num_threads_; ++i) {
        boards_.push_back(std::make_unique<board::ChessBoard>(root));        // private copy
        workers_.push_back(std::make_unique<Searcher>(*boards_[i], shared_tt));
        ctx_.push_back(WorkerCtx{this, i});
    }
}

void SMPSearcher::set_board(board::ChessBoard& root) {
    for (int i = 0; i < num_threads_; ++i) {
        *boards_[i] = root;                    // copy-assign the position
        workers_[i]->set_board(*boards_[i]);   // rebind + refresh NNUE accumulators
    }
}

void SMPSearcher::stop() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    for (auto& w : workers_) w->stop();
}

void SMPSearcher::worker_cb(const Result& r, void* user) {
    auto* ctx = static_cast<WorkerCtx*>(user);
    ctx->self->merge_.update(ctx->id, r);
}

Result SMPSearcher::go(const Limits& lim) {
    stop_.store(false, std::memory_order_relaxed);
    merge_.reset(/*start_depth=*/1);
    tt_->new_search();                          // bump generation ONCE for the whole search
    running_.store(num_threads_, std::memory_order_relaxed);

    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };

    // Launch the workers. Each gets the same time budget (self-timing) but its
    // info goes to the merge, not the GUI.
    std::vector<std::thread> threads;
    threads.reserve(num_threads_);
    for (int i = 0; i < num_threads_; ++i) {
        threads.emplace_back([this, i, lim]() {
            Limits wl        = lim;
            wl.on_iteration  = &SMPSearcher::worker_cb;
            wl.callback_user = &ctx_[i];
            workers_[i]->go(wl);
            running_.fetch_sub(1, std::memory_order_relaxed);
        });
    }

    // Coordinator loop: stream merged info to the GUI until the workers finish
    // (they self-time) or an external stop arrives.
    auto send_if_new = [&](Result& last, bool& have_last) {
        Result m;
        if (!merge_.merged(m)) return;
        bool is_new = !have_last
                   || m.depth > last.depth
                   || (m.depth == last.depth
                       && (m.best_move != last.best_move || m.score != last.score));
        if (is_new) {
            m.time_secs = elapsed();
            if (lim.on_iteration) lim.on_iteration(m, lim.callback_user);
            last = m;
            have_last = true;
        }
    };

    Result last_sent{};
    bool   have_last = false;
    while (!stop_.load(std::memory_order_relaxed)
           && running_.load(std::memory_order_relaxed) > 0) {
        send_if_new(last_sent, have_last);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Make sure every worker is stopped (idempotent if they already finished).
    for (auto& w : workers_) w->stop();
    for (auto& t : threads) t.join();

    // Final merge after all workers have stopped.
    Result final_r;
    if (merge_.merged(final_r)) {
        final_r.time_secs = elapsed();
        if (lim.on_iteration) lim.on_iteration(final_r, lim.callback_user);
        return final_r;
    }
    return last_sent;   // fallback (e.g. instant stop before any iteration merged)
}

}  // namespace search::smp
