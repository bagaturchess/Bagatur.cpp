// mtd.cpp — BetaGenerator + MTDSearchManager implementation.
//
// Line numbers in comments refer to the Java source under
//   Search/src/bagaturchess/search/impl/rootsearch/sequential/mtd/

#include "mtd.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "types.h"

namespace search {

// ============================================================================
// BetaGenerator (BetaGenerator.java)
// ============================================================================
BetaGenerator::BetaGenerator(int initial_val, int betas_count, int initial_interval)
    : betas_count_(betas_count),
      initial_interval_(initial_interval),
      lower_bound_(SCORE_MIN),
      upper_bound_(SCORE_MAX),
      trend_(TREND_INIT),
      trend_multiplier_(1),
      last_val_(initial_val) {}

void BetaGenerator::decreaseUpper(int val) {
    // BetaGenerator.java line 71-86
    if (val < upper_bound_) {
        if (trend_ == TREND_DOWN) {
            if (trend_multiplier_ < MAX_TREND_MULTIPLIER) trend_multiplier_ *= 2;
        } else {
            trend_multiplier_ = 1;
        }
        upper_bound_ = val;
        trend_       = TREND_DOWN;
    }
}

void BetaGenerator::increaseLower(int val) {
    // BetaGenerator.java line 91-107
    if (val > lower_bound_) {
        if (trend_ == TREND_UP) {
            if (trend_multiplier_ < MAX_TREND_MULTIPLIER) trend_multiplier_ *= 2;
        } else {
            trend_multiplier_ = 1;
        }
        lower_bound_ = val;
        trend_       = TREND_UP;
    }
}

std::vector<int> BetaGenerator::genBetas() const {
    // BetaGenerator.java line 113-173
    std::vector<int> betas;
    betas.reserve(betas_count_);

    if (lower_bound_ != SCORE_MIN && upper_bound_ != SCORE_MAX) {
        // Both bounds known → split evenly. For betas_count_ == 1 this picks
        // the midpoint (classic MTD bisection).
        int win = std::abs(upper_bound_ - lower_bound_) / (betas_count_ + 1);
        if (win <= 0) {
            // Degenerate: bounds touch. Fall back to a single beta at the
            // upper bound — the caller will quickly resolve via the SearchManager.
            betas.push_back(upper_bound_);
            return betas;
        }
        for (int i = 1; i <= betas_count_; ++i) {
            betas.push_back(lower_bound_ + i * win);
        }
        return betas;
    }

    bool first_time = (lower_bound_ == SCORE_MIN && upper_bound_ == SCORE_MAX);

    if (first_time) {
        // First call: seed beta is the initial eval.
        betas.push_back(last_val_);

        int start_val = last_val_ - (betas_count_ / 2) * initial_interval_;
        for (int i = 2; i <= betas_count_; ++i) {
            int beta = start_val + i * initial_interval_;
            if (beta != last_val_) betas.push_back(beta);
        }
        return betas;
    }

    // One bound known — γ-step from it by `interval × trend_multiplier`.
    if (lower_bound_ != SCORE_MIN) {
        for (int i = 1; i <= betas_count_; ++i) {
            betas.push_back(lower_bound_ + i * initial_interval_ * trend_multiplier_);
        }
    } else {
        // upper_bound_ != SCORE_MAX
        for (int i = 1; i <= betas_count_; ++i) {
            betas.push_back(upper_bound_ - i * initial_interval_ * trend_multiplier_);
        }
    }
    return betas;
}


// ============================================================================
// MTDSearchManager (SearchManager.java)
// ============================================================================
MTDSearchManager::MTDSearchManager(int start_depth, int max_iterations, int initial_value)
    : current_depth_(start_depth),
      max_iterations_(max_iterations),
      initial_value_(initial_value),
      betas_gen_(initial_value, BETAS_COUNT, TRUST_WINDOW_MTD_STEP) {
    initBetas();
}

void MTDSearchManager::initBetas() {
    // SearchManager.java line 102-118 — recreate the generator and refresh
    // the beta queue. On the very first call, betas_gen_ already exists from
    // the ctor; subsequent calls (after finishDepth()) replace it.
    betas_gen_ = BetaGenerator(/*initial_val=*/betas_gen_.getLowerBound() == SCORE_MIN
                                                  ? initial_value_
                                                  : static_cast<int>(betas_gen_.getLowerBound()),
                               BETAS_COUNT, TRUST_WINDOW_MTD_STEP);
    betas_ = betas_gen_.genBetas();
}

void MTDSearchManager::updateBetas() {
    // SearchManager.java line 142-160 — refresh betas with current bounds.
    betas_ = betas_gen_.genBetas();
}

int MTDSearchManager::nextBeta() {
    // SearchManager.java line 162-208 — pop the head; on exhaustion regenerate.
    if (betas_.empty()) {
        updateBetas();
        if (betas_.empty()) {
            // Should not happen in practice. Fall back to a midpoint guess.
            int lb = getLowerBound() == SCORE_MIN ? initial_value_ - 1 : getLowerBound();
            int ub = getUpperBound() == SCORE_MAX ? initial_value_ + 1 : getUpperBound();
            return lb + (ub - lb) / 2;
        }
    }
    int b = betas_.front();
    betas_.erase(betas_.begin());
    return b;
}

bool MTDSearchManager::isLast() const {
    // SearchManager.java line 337-362 — converged when bounds collapsed within
    // TRUST_WINDOW_BEST_MOVE, with mate-special-cases.
    int lb = betas_gen_.getLowerBound();
    int ub = betas_gen_.getUpperBound();

    bool last = (lb + TRUST_WINDOW_BEST_MOVE >= ub);

    if (!last) {
        // Mate-score early-exit: once we've found a mate value within the
        // current window, no need to keep narrowing.
        if (lb >= MATE_THRESHOLD && is_mate_score(lb) && (ub - lb < MATE_THRESHOLD)) {
            last = true;
        }
        if (ub <= -MATE_THRESHOLD && is_mate_score(ub) && (ub - lb < MATE_THRESHOLD)) {
            last = true;
        }
    }
    return last;
}

void MTDSearchManager::finishDepth() {
    // SearchManager.java line 290-322
    ++current_depth_;
}

bool MTDSearchManager::increaseLowerBound(int eval) {
    // SearchManager.java line 223-257. Returns true if depth converged.
    if (eval >= betas_gen_.getLowerBound()) {
        betas_gen_.increaseLower(eval);
    }
    bool converged = isLast();
    if (converged) {
        finishDepth();
        initBetas();
    } else {
        updateBetas();
    }
    return converged;
}

bool MTDSearchManager::decreaseUpperBound(int eval) {
    // SearchManager.java line 260-287
    if (eval <= betas_gen_.getUpperBound()) {
        betas_gen_.decreaseUpper(eval);
    }
    bool converged = isLast();
    if (converged) {
        finishDepth();
        initBetas();
    } else {
        updateBetas();
    }
    return converged;
}

}  // namespace search
