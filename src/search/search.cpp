// search.cpp — PVS + NWS implementation.

#include "search.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../board/move_util.h"
#include "../board/see_util.h"

namespace search {

namespace {

// LMR_TABLE — same shape as Search_PVS_NWS.LMR_TABLE.
struct LMRTable {
    double t[64][64];
    constexpr LMRTable() : t{} {
        for (int d = 1; d < 64; ++d) {
            for (int m = 1; m < 64; ++m) {
                double r = std::log((double)d) * std::log((double)m) / 2.0;
                if (r < 1.0) r = 1.0;
                r *= 1.555;  // REDUCTION_AGGRESSIVENESS
                t[d][m] = r;
            }
        }
    }
};
// constexpr would require constexpr std::log, which is non-portable; use
// const + runtime-initialise once.
const LMRTable kLMR{};

// MVV-LVA piece values for move ordering on captures.
constexpr int PIECE_VAL[7] = { 0, 100, 320, 330, 500, 900, 20000 };

constexpr int SCORE_TT_MOVE    = 1 << 30;
constexpr int SCORE_GOOD_CAP   = 1 << 28;
constexpr int SCORE_KILLER_1   = 1 << 26;
constexpr int SCORE_KILLER_2   = (1 << 26) - 1;
constexpr int SCORE_BAD_CAP    = -(1 << 28);

}  // namespace

Searcher::Searcher(board::ChessBoard& cb, std::size_t tt_mb)
    : cb_(cb), tt_(tt_mb) {
    eval_.reset(cb);
}

bool Searcher::time_up() noexcept {
    if (max_nodes_ != 0 && nodes_ >= max_nodes_) return true;
    if (max_time_s_ <= 0.0) return false;
    auto now = std::chrono::steady_clock::now();
    double s = std::chrono::duration<double>(now - start_).count();
    return s >= max_time_s_;
}

bool Searcher::check_for_stop() {
    // Avoid clock syscalls on every node — sample every 4096 nodes.
    if ((++node_check_ & 4095) != 0) return false;
    if (stop_.load(std::memory_order_relaxed)) return true;
    if (time_up()) return true;
    return false;
}

int Searcher::evaluate() {
    return eval_.evaluate(cb_);
}

void Searcher::update_pv(int ply, int move) {
    Stack& s = stacks_[ply];
    s.pv[0] = move;
    Stack& next = stacks_[ply + 1];
    std::memcpy(&s.pv[1], next.pv, next.pv_length * sizeof(int));
    s.pv_length = next.pv_length + 1;
}

int Searcher::score_capture(int move) const noexcept {
    int victim   = board::mv::attacked_piece_index(move);
    int attacker = board::mv::source_piece_index(move);
    // MVV-LVA: prioritise high-value victim, then low-value attacker.
    return PIECE_VAL[victim] * 16 - PIECE_VAL[attacker];
}

// ---------------------------------------------------------------------------
// qsearch — captures + promotions only, prunes losing captures by SEE<0.
// Mirrors bagaturchess.Search_PVS_NWS.qsearch(): no delta/futility pruning
// inside the loop; legality of remaining moves is the SEE filter alone.
// ---------------------------------------------------------------------------
int Searcher::qsearch(int ply, int alpha, int beta) {
    ++nodes_;
    if (ply > sel_depth_) sel_depth_ = ply;

    if (check_for_stop()) {
        aborted_ = true;
        return alpha;
    }
    if (ply >= MAX_PLY) return evaluate();

    // Repetition / 50-move
    if (cb_.lastCaptureOrPawnMoveBefore >= 100) return SCORE_DRAW;
    if (cb_.getRepetition() >= 2) return SCORE_DRAW;

    const int alpha_orig = alpha;   // saved for the alpha-restore step at the end

    int stand = evaluate();
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;

    int best = stand;

    gen_.startPly();
    gen_.generateAttacks(cb_);

    // Score captures by MVV-LVA inline (no full sort — picks max each iter).
    int  buf_moves[256];
    int  buf_scores[256];
    int  n = 0;
    while (gen_.hasNext() && n < 256) {
        int m = gen_.next();
        if (!cb_.isLegal(m)) continue;
        buf_moves[n]  = m;
        buf_scores[n] = score_capture(m);
        ++n;
    }
    gen_.endPly();

    for (int picked = 0; picked < n; ++picked) {
        int best_i = picked;
        for (int j = picked + 1; j < n; ++j)
            if (buf_scores[j] > buf_scores[best_i]) best_i = j;
        if (best_i != picked) {
            std::swap(buf_moves[picked],  buf_moves[best_i]);
            std::swap(buf_scores[picked], buf_scores[best_i]);
        }
        int m = buf_moves[picked];

        // SEE pruning — skip captures that lose material on the static exchange.
        // Java line 2079-2084: int see = ...; if (see < 0) continue;
        if (board::see::getSeeCaptureScore(cb_, m) < 0) continue;

        int side = cb_.colorToMove;
        cb_.doMove(m);
        eval_.after_make(cb_, m, side);
        int score = -qsearch(ply + 1, -beta, -alpha);
        eval_.after_unmake(cb_, m, side);
        cb_.undoMove(m);

        if (aborted_) return alpha;

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                if (score >= beta) break;
            }
        }
    }

    // Alpha-restore (Java line 2163-2175, `isOther_UseAlphaOptimizationInQSearch`).
    // If neither stand-pat nor any capture exceeded the *input* alpha, clamp
    // the return value back up to alpha_orig — turns fail-soft below alpha
    // into fail-hard at alpha. Makes the TT bound stored at the caller tighter.
    if (alpha_orig > best) best = alpha_orig;

    return best;
}

// ---------------------------------------------------------------------------
// Move ordering for the main loop. We pull moves in phases:
//   1. TT move (if legal at this position)
//   2. Captures + promotions, sorted by MVV-LVA
//   3. Killers (2)
//   4. Quiet moves, sorted by butterfly-history
// ---------------------------------------------------------------------------
void Searcher::score_quiet_moves(int ply, int* moves, int* scores, int n) {
    int color   = cb_.colorToMove;
    int killer1 = killers_.primary(ply);
    int killer2 = killers_.secondary(ply);
    for (int i = 0; i < n; ++i) {
        int m = moves[i];
        if (m == killer1)      scores[i] = SCORE_KILLER_1;
        else if (m == killer2) scores[i] = SCORE_KILLER_2;
        else                   scores[i] = history_.get(color, m);
    }
}

int Searcher::pick_next_quiet(int* /*moves*/, int* scores, int start, int n) {
    int best_i = start;
    for (int j = start + 1; j < n; ++j)
        if (scores[j] > scores[best_i]) best_i = j;
    return best_i;
}

// ---------------------------------------------------------------------------
// search — main negamax routine. ply >= 1 (root is run by go()).
// ---------------------------------------------------------------------------
int Searcher::search(int ply, int depth, int alpha, int beta, bool is_pv, bool cut_node) {
    stacks_[ply].pv_length = 0;

    if (check_for_stop()) {
        aborted_ = true;
        return alpha;
    }

    if (ply >= MAX_PLY) return evaluate();

    // Repetition / 50-move
    if (cb_.lastCaptureOrPawnMoveBefore >= 100) return SCORE_DRAW;
    if (cb_.getRepetition() >= 2) return SCORE_DRAW;

    // Mate distance pruning.
    int mate_alpha = std::max(alpha, mated_in(ply));
    int mate_beta  = std::min(beta,  mate_in(ply + 1));
    if (mate_alpha >= mate_beta) return mate_alpha;
    alpha = mate_alpha;
    beta  = mate_beta;

    // qsearch at horizon
    if (depth <= 0) return qsearch(ply, alpha, beta);

    ++nodes_;
    if (ply > sel_depth_) sel_depth_ = ply;

    bool in_check = (cb_.checkingPieces != 0);
    int  alpha_orig = alpha;

    // TT probe
    int     tt_move  = 0;
    int     tt_score = SCORE_DRAW;
    TTEntry tte;
    if (tt_.probe(cb_.zobristKey, tte)) {
        tt_move = tte.move;
        tt_score = score_from_tt(tte.score, ply);

        if (!is_pv && tte.depth >= depth) {
            if (tte.flag == TT_EXACT) return tt_score;
            if (tte.flag == TT_LOWER && tt_score >= beta)  return tt_score;
            if (tte.flag == TT_UPPER && tt_score <= alpha) return tt_score;
        }
    }

    Stack& st = stacks_[ply];
    // SCORE_MIN serves as the "no static eval available" sentinel — set when
    // we're in check (eval is meaningless there). Mirrors Java's IEvaluator.MIN_EVAL.
    int static_eval = in_check ? SCORE_MIN : evaluate();
    st.static_eval = static_eval;

    // `improving` — true when our static eval climbed since our previous turn
    // (two plies back). Used to gate / scale several pruning heuristics.
    bool improving = false;
    if (!in_check && ply >= 3 && stacks_[ply - 2].static_eval != SCORE_MIN) {
        improving = static_eval > stacks_[ply - 2].static_eval;
    }

    // CNIR — Consecutive Not-Improving Reduction: when position has been
    // deteriorating for two consecutive same-colour ply pairs, reduce more.
    // Mirrors Search_PVS_NWS.java around line 1355.
    bool not_improving_twice = !improving
        && ply >= 5
        && stacks_[ply - 2].static_eval != SCORE_MIN
        && stacks_[ply - 4].static_eval != SCORE_MIN
        && stacks_[ply - 2].static_eval <= stacks_[ply - 4].static_eval;

    // ----------------------------------------------------------------
    // Pruning at non-PV nodes
    // ----------------------------------------------------------------
    if (!is_pv && !in_check && !is_mate_score(alpha) && !is_mate_score(beta)) {

        // ------------------------------------------------------------
        // Fail-high pruning for non-PV nodes — fires when the static
        // eval is comfortably above beta and the position therefore
        // "looks won" already.
        // ------------------------------------------------------------
        if (static_eval >= beta + 35) {

            // Static null move (reverse futility)
            if (depth <= 7 && static_eval - 80 * depth >= beta) {
                return static_eval;
            }

            // Null move pruning
            if (depth >= 3) {
                int r = 3 + depth / 4 + std::min(3, (static_eval - beta) / 80);
                cb_.doNullMove();
                int score = -search(ply + 1, depth - r, -beta, -beta + 1, false, !cut_node);
                cb_.undoNullMove();
                if (aborted_) return alpha;
                if (score >= beta) {
                    if (is_mate_score(score)) score = beta;  // don't return false mate
                    return score;
                }
            }
        }

        // ------------------------------------------------------------
        // Fail-low pruning for non-PV nodes — fires when the static
        // eval is below alpha and the position therefore "looks lost"
        // unless a tactical resource flips it.
        // ------------------------------------------------------------
        if (static_eval <= alpha) {

            // Razoring — confirm the fail-low with a shallow qsearch.
            if (depth <= 4) {
                int razor_margin = 260 * depth;
                if (static_eval + razor_margin < alpha) {
                    int v = qsearch(ply, alpha, alpha + 1);
                    if (v < alpha) return v;
                }
            }
        }
    }

    // Internal iterative reduction: if we got nothing from the TT and we're
    // at non-shallow depth, reducing here often pays for itself.
    if (depth >= 4 && tt_move == 0) --depth;

    // ----------------------------------------------------------------
    // Collect & order moves
    // ----------------------------------------------------------------
    int caps[64],     cap_scores[64],     n_caps = 0;
    int quiets[200],  quiet_scores[200],  n_quiets = 0;

    gen_.startPly();
    gen_.generateMoves(cb_);
    gen_.generateAttacks(cb_);
    while (gen_.hasNext()) {
        int m = gen_.next();
        if (!cb_.isLegal(m)) continue;
        if (board::mv::is_quiet(m)) {
            if (n_quiets < (int)(sizeof(quiets)/sizeof(quiets[0]))) {
                quiets[n_quiets++] = m;
            }
        } else {
            if (n_caps < (int)(sizeof(caps)/sizeof(caps[0]))) {
                caps[n_caps++] = m;
            }
        }
    }
    gen_.endPly();

    for (int i = 0; i < n_caps; ++i) cap_scores[i] = score_capture(caps[i]);
    score_quiet_moves(ply, quiets, quiet_scores, n_quiets);

    // ----------------------------------------------------------------
    // Move loop. Phases: TT > captures (sorted) > quiets (sorted)
    // ----------------------------------------------------------------
    int best_score = SCORE_MIN;
    int best_move  = 0;
    int move_count = 0;

    // ----------------------------------------------------------------
    // Fail-low pruning for non-PV nodes — fires per-move before the
    // move is actually played. Mirrors the equivalent block in
    // bagaturchess Search_PVS_NWS.search() lines 1204-1255.
    //
    // Skips:
    //   - LMP   (Late Move Pruning): once we've tried enough moves
    //   - Futility: static eval too far below alpha to recover
    //   - SEE: bad captures at shallow depths
    // ----------------------------------------------------------------
    auto should_skip_pre_move = [&](int m, bool is_quiet_move) -> bool {
        if (is_pv || in_check || move_count <= 1) return false;
        if (is_mate_score(alpha) || is_mate_score(beta)) return false;

        if (is_quiet_move) {
            // LMP: at high move counts, remaining quiet moves are unlikely to top
            // the best we already have. `improving` widens the gate.
            int lmp_count = (3 + depth * depth / (improving ? 1 : 2)) * 3 / 4;  // /PRUNING_AGGR (=1.333)
            if (move_count >= lmp_count) return true;

            // Futility: skip quiet moves that can't possibly bring static_eval
            // up to alpha at this depth (≈ 60 cp/ply slack).
            if (depth <= 7) {
                int futility = depth * 80 * 3 / 4;
                if (static_eval + futility <= alpha) return true;
            }
        } else {
            // SEE-pruning for captures with negative static exchange.
            if (depth <= 7) {
                int see = board::see::getSeeCaptureScore(cb_, m);
                int see_thresh = -80 * depth * 3 / 4;
                if (see < see_thresh) return true;
            }
        }
        return false;
    };

    auto try_move = [&](int m, bool is_quiet_move) -> bool {
        int side = cb_.colorToMove;

        cb_.doMove(m);
        eval_.after_make(cb_, m, side);

        // Check extension (cheap proxy: are we now in check?)
        int extension = (cb_.checkingPieces != 0) ? 1 : 0;

        int new_depth = depth - 1 + extension;
        int score;

        // PVS / LMR
        if (move_count == 1) {
            score = -search(ply + 1, new_depth, -beta, -alpha, is_pv, false);
        } else {
            int reduction = 0;
            if (is_quiet_move && depth >= 3 && move_count > 1 && !in_check && extension == 0) {
                int rd = std::min(63, depth);
                int rm = std::min(63, move_count);
                reduction = (int)kLMR.t[rd][rm];

                // Mirror Search_PVS_NWS LMR adjustments:
                //   if (!isPv)             reduction += 1;
                //   if (cutNode)           reduction += 1;
                //   if (notImprovingTwice) reduction += 1;   // CNIR
                // (non-PV nodes, expected-fail-high nodes, and deteriorating-
                //  position branches all get reduced more aggressively)
                if (!is_pv)              reduction += 1;
                if (cut_node)            reduction += 1;
                if (not_improving_twice) reduction += 1;

                if (reduction >= new_depth) reduction = new_depth - 1;
                if (reduction < 0) reduction = 0;
            }
            // Null-window reduced search
            score = -search(ply + 1, new_depth - reduction, -alpha - 1, -alpha, false, true);
            // Re-search if reduced search came back above alpha
            if (score > alpha && reduction > 0) {
                score = -search(ply + 1, new_depth, -alpha - 1, -alpha, false, !cut_node);
            }
            // PV re-search
            if (score > alpha && score < beta && is_pv) {
                score = -search(ply + 1, new_depth, -beta, -alpha, true, false);
            }
        }

        eval_.after_unmake(cb_, m, side);
        cb_.undoMove(m);

        if (aborted_) return false;  // do not finalise

        if (score > best_score) {
            best_score = score;
            best_move  = m;
            if (score > alpha) {
                alpha = score;
                if (is_pv) update_pv(ply, m);
            }
            if (alpha >= beta) {
                if (is_quiet_move) {
                    killers_.on_cutoff(ply, m);
                    history_.on_cutoff(cb_.colorToMove, m, depth);
                }
                return true;  // beta cutoff
            }
        } else if (is_quiet_move) {
            history_.on_poor(cb_.colorToMove, m, depth);
        }
        return false;
    };

    // Phase 1: TT move first — never pruned
    if (tt_move != 0 && cb_.isValidMove(tt_move)) {
        bool is_quiet_move = board::mv::is_quiet(tt_move);
        ++move_count;
        if (try_move(tt_move, is_quiet_move)) goto done;
        if (aborted_) return alpha;
    }

    // Phase 2: captures sorted by MVV-LVA
    for (int picked = 0; picked < n_caps; ++picked) {
        int best_i = picked;
        for (int j = picked + 1; j < n_caps; ++j)
            if (cap_scores[j] > cap_scores[best_i]) best_i = j;
        if (best_i != picked) {
            std::swap(caps[picked],       caps[best_i]);
            std::swap(cap_scores[picked], cap_scores[best_i]);
        }
        int m = caps[picked];
        if (m == tt_move) continue;
        ++move_count;
        if (should_skip_pre_move(m, /*is_quiet=*/false)) continue;
        if (try_move(m, /*is_quiet=*/false)) goto done;
        if (aborted_) return alpha;
    }

    // Phase 3: quiet moves sorted by killer + history
    for (int picked = 0; picked < n_quiets; ++picked) {
        int best_i = pick_next_quiet(quiets, quiet_scores, picked, n_quiets);
        if (best_i != picked) {
            std::swap(quiets[picked],       quiets[best_i]);
            std::swap(quiet_scores[picked], quiet_scores[best_i]);
        }
        int m = quiets[picked];
        if (m == tt_move) continue;
        ++move_count;
        if (should_skip_pre_move(m, /*is_quiet=*/true)) continue;
        if (try_move(m, /*is_quiet=*/true)) goto done;
        if (aborted_) return alpha;
    }

done:
    if (aborted_) return alpha;

    // No legal moves played → mate or stalemate
    if (move_count == 0) {
        return in_check ? mated_in(ply) : SCORE_DRAW;
    }

    // TT store
    TTFlag flag = (best_score >= beta) ? TT_LOWER
                 : (best_score > alpha_orig ? TT_EXACT : TT_UPPER);
    tt_.store(cb_.zobristKey, best_move, best_score, static_eval, depth, flag, ply);
    return best_score;
}

// ---------------------------------------------------------------------------
// Iterative deepening driver
// ---------------------------------------------------------------------------
Result Searcher::go(const Limits& lim) {
    nodes_      = 0;
    node_check_ = 0;
    sel_depth_  = 0;
    aborted_    = false;
    stop_.store(false, std::memory_order_relaxed);
    max_time_s_ = lim.max_time_secs;
    max_nodes_  = lim.max_nodes;
    start_      = std::chrono::steady_clock::now();
    tt_.new_search();
    killers_.clear();
    eval_.reset(cb_);

    Result best{};
    int alpha = SCORE_MIN, beta = SCORE_MAX;

    for (int depth = 1; depth <= lim.max_depth; ++depth) {
        root_depth_ = depth;
        stacks_[0].pv_length = 0;

        int score = search(/*ply=*/1, depth, alpha, beta, /*is_pv=*/true, /*cut_node=*/false);

        if (aborted_) break;

        Stack& s0 = stacks_[1];  // search() writes its PV to ply=1's stack
        if (s0.pv_length > 0) {
            best.score     = score;
            best.best_move = s0.pv[0];
            best.depth     = depth;
            best.pv_length = s0.pv_length;
            std::memcpy(best.pv.data(), s0.pv, s0.pv_length * sizeof(int));
        }
        auto now      = std::chrono::steady_clock::now();
        best.time_secs= std::chrono::duration<double>(now - start_).count();
        best.nodes    = nodes_;
        best.seldepth = sel_depth_;

        if (lim.on_iteration) lim.on_iteration(best, lim.callback_user);

        if (time_up() || stop_.load(std::memory_order_relaxed)) break;
        if (is_mate_score(score)) break;  // already mated, no point going deeper
    }
    return best;
}

}  // namespace search
