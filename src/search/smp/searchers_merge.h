// searchers_merge.h — merges per-thread results into one reported result.
//
// C++ port of bagaturchess SearchersInfo (the SMP result merge). Each worker
// reports its per-iteration Result; the merge:
//   * ignores fail-low (upperbound) iterations and PV-less ones,
//   * advances a consensus depth once `next_depth_threshold` fraction of the
//     workers have reached the next depth,
//   * at the consensus depth, groups the workers' results by best move and
//     aggregates the eval per move (best mate value for mates, otherwise the
//     AVERAGE across the agreeing workers),
//   * picks the move with the highest aggregated eval; sums node counts.
//
// Only the transposition table must be lock-free; this merge is touched a few
// times per millisecond, so a plain mutex is fine.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../search.h"   // Result
#include "../types.h"    // is_mate_score, MAX_PLY

namespace search::smp {

class SearchersMerge {
public:
    SearchersMerge(int num_workers, double next_depth_threshold)
        : num_workers_(num_workers), threshold_(next_depth_threshold) {
        workers_.resize(num_workers);
    }

    void reset(int start_depth) {
        std::lock_guard<std::mutex> g(mu_);
        cur_depth_ = start_depth;
        for (auto& w : workers_) { w.by_depth.clear(); w.nodes = 0; }
    }

    // Called by each worker (on its own thread) after a completed iteration.
    void update(int worker_id, const Result& r) {
        if (r.upper_bound || r.pv_length < 1 || r.best_move == 0) return;
        std::lock_guard<std::mutex> g(mu_);
        Worker& w = workers_[worker_id];
        w.nodes = r.nodes;
        w.by_depth[r.depth] = r;
    }

    // Build the merged result for the consensus depth. Returns false if nothing
    // is reportable yet.
    bool merged(Result& out) {
        std::lock_guard<std::mutex> g(mu_);
        if (has_depth(cur_depth_ + 1)) ++cur_depth_;
        return accumulate(cur_depth_, out);
    }

private:
    struct Worker {
        std::map<int, Result> by_depth;   // depth -> last reported Result
        std::uint64_t         nodes = 0;
    };

    bool has_depth(int depth) const {
        int responded = 0;
        for (const auto& w : workers_)
            if (w.by_depth.count(depth)) ++responded;
        return responded / double(num_workers_) >= threshold_;
    }

    std::uint64_t total_nodes() const {
        std::uint64_t n = 0;
        for (const auto& w : workers_) n += w.nodes;
        return n;
    }

    // Per-candidate-move aggregation (mirror of Java MoveInfo).
    struct MoveAgg {
        long long sum = 0;
        int       cnt = 0;
        int       best_eval = SCORE_MIN;
        bool      is_mate = false;
        int       mate_val = SCORE_MIN;
        const Result* best = nullptr;

        void add(const Result& r) {
            int e = r.score;
            if (is_mate) {
                if (is_mate_score(e) && e > mate_val) { mate_val = e; best = &r; }
            } else if (is_mate_score(e)) {
                is_mate = true; mate_val = e; best = &r;
            } else {
                if (e > best_eval) { best_eval = e; best = &r; }
                sum += e;
            }
            ++cnt;
        }
        int eval() const { return is_mate ? mate_val : int(sum / cnt); }
    };

    bool accumulate(int depth, Result& out) {
        std::unordered_map<int, MoveAgg> by_move;   // best_move -> aggregate
        int max_seldepth = 0;

        for (const auto& w : workers_) {
            auto it = w.by_depth.find(depth);
            if (it == w.by_depth.end()) continue;
            const Result& r = it->second;
            by_move[r.best_move].add(r);
            if (r.seldepth > max_seldepth) max_seldepth = r.seldepth;
        }

        const MoveAgg* winner = nullptr;
        for (const auto& kv : by_move) {
            if (winner == nullptr || kv.second.eval() > winner->eval()) winner = &kv.second;
        }
        if (winner == nullptr || winner->best == nullptr) return false;

        const Result& rep = *winner->best;
        out = Result{};
        out.depth     = rep.depth;
        out.seldepth  = max_seldepth;
        out.score     = winner->eval();
        out.best_move = rep.best_move;
        out.pv_length = rep.pv_length;
        out.pv        = rep.pv;
        out.nodes     = total_nodes();
        return true;
    }

    mutable std::mutex  mu_;
    int                 num_workers_;
    double              threshold_;
    int                 cur_depth_ = 1;
    std::vector<Worker> workers_;
};

}  // namespace search::smp
