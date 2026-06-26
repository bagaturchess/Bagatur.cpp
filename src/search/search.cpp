// search.cpp — PVS + NWS implementation.

#include "search.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../board/move_util.h"
#include "../board/see_util.h"
#include "mtd.h"

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
    : cb_(&cb), tt_(tt_mb) {
    eval_.reset(cb);
}

bool Searcher::time_up() noexcept {
    if (max_nodes_ != 0 && nodes_ >= max_nodes_) return true;
    // `min_move_secs_ == 0` is the "no time limit" sentinel used by
    // INFINITE / FIXED_DEPTH / FIXED_NODES.
    if (min_move_secs_ <= 0.0) return false;
    if (terminate_search_)     return true;
    auto now = std::chrono::steady_clock::now();
    double s = std::chrono::duration<double>(now - start_).count();
    if (s >= total_clock_secs_) return true;   // hard cap (Java's clock ceiling)
    // Strict in-recursion soft cap. Earlier this was relaxed to `2×
    // available_secs()` on the theory that the iteration-boundary terminate
    // gate already prevents over-spend; but at ultra-short TC (8s + 0.08)
    // the gate sets `terminate_search_` AT the iteration boundary, not
    // before — an iteration that *starts* just under the gate at elapsed
    // = 0.5×available can run to 2× without aborting. Over 40 moves this
    // doubles per-move time and burns the clock. The strict 1× check
    // matches the Java behavior the engine was tuned against.
    if (s >= available_secs()) return true;
    return false;
}

void Searcher::update_volatility(int eval, int move, int depth) noexcept {
    // 1:1 port of MoveEvalInAccount.newPVLine() (line 60-86).
    // Constants from the Java source:
    static constexpr double SCORES_DIFF_DEVIDER       = 2.0;
    static constexpr int    SCORES_PENALTY_DIFF_MOVES = 50;
    static constexpr int    MAX_SCORES_DIFF           = 150;

    // Java skips the volatility accumulation when a mate has been found —
    // we don't want to keep burning clock once mate is known.
    if (is_mate_score(eval)) return;

    if (vol_last_move_ != 0) {
        int cur_diff = std::abs(vol_last_eval_ - eval);
        if (vol_last_depth_ != depth) {
            // Decay on new depth — old volatility shrinks.
            vol_accum_score_diff_ /= SCORES_DIFF_DEVIDER;
        }
        vol_accum_score_diff_ += cur_diff;
        if (vol_last_move_ != move) {
            vol_accum_score_diff_ += SCORES_PENALTY_DIFF_MOVES;
        }
        double clamped = std::min(static_cast<double>(MAX_SCORES_DIFF),
                                  vol_accum_score_diff_);
        vol_usage_pct_ = max_usage_percent_ * (clamped / MAX_SCORES_DIFF);
    }

    vol_last_move_  = move;
    vol_last_eval_  = eval;
    vol_last_depth_ = depth;
}

bool Searcher::check_for_stop() {
    // `max_nodes_` is checked per-node so `go nodes N` is exact, BUT only
    // once we're past the first iteration (`root_depth_ > 1`). At depth=1
    // we must let the root finish its first legal move's evaluation —
    // otherwise `go nodes 1` aborts the very first recursive call and we
    // ship `bestmove 0000`. After depth 1 has produced a fallback PV, any
    // further abort is safe (the previous depth's bestmove survives).
    if (max_nodes_ != 0 && nodes_ >= max_nodes_ && root_depth_ > 1) return true;

    // Avoid clock syscalls on every node — sample every 4096 nodes. This
    // also throttles the depth=1 node-limit check (the iteration completes
    // ~20 nodes before time_up()'s node check fires).
    if ((++node_check_ & 4095) != 0) return false;
    if (stop_.load(std::memory_order_relaxed)) return true;
    if (time_up()) return true;
    return false;
}

int Searcher::evaluate() {
    int eval = eval_.evaluate(*cb_);

    // Insufficient-material cap — Java SearchImpl.evaluate() line 258:
    //   if (!hasSufficientMatingMaterial(stm)) eval = min(getDrawScores(-1), eval);
    //
    // When the side-to-move has only K (or K + a single minor) it cannot
    // FORCE a win even if NNUE returns a large positive score. Capping at
    // 0 stops the search from pursuing a phantom "winning plan" — same
    // effect as the draw cut in `isDraw`, but specific to a one-sided lack
    // of mating material (the symmetric case is already a hard draw).
    if (eval > SCORE_DRAW
        && !cb_->hasSufficientMatingMaterial(cb_->colorToMove)) {
        eval = SCORE_DRAW;
    }
    return eval;
}

void Searcher::update_pv(int ply, int move) {
    Stack& s = stacks_[ply];
    s.pv[0] = move;
    Stack& next = stacks_[ply + 1];
    // Defensive cap: Stack.pv has MAX_PLY slots (indices 0..MAX_PLY-1). We
    // need room for the move at [0] plus up to (MAX_PLY-1) ints from the
    // child. Without this, deep extension chains can produce a pv_length
    // that overruns the inline array — Windows surfaces it as a segfault.
    int copy = next.pv_length;
    if (copy > MAX_PLY - 1) copy = MAX_PLY - 1;
    if (copy > 0) {
        std::memcpy(&s.pv[1], next.pv, copy * sizeof(int));
    }
    s.pv_length = copy + 1;
}

int Searcher::score_capture(int move) const noexcept {
    int victim   = board::mv::attacked_piece_index(move);
    int attacker = board::mv::source_piece_index(move);
    // MVV-LVA: prioritise high-value victim, then low-value attacker.
    int mvv_lva  = PIECE_VAL[victim] * 16 - PIECE_VAL[attacker];
    // Promotion gain. Without this a quiet queen promotion scores ~ -100
    // (EMPTY victim, PAWN attacker) and sorts BELOW ordinary captures, even
    // though SEE rightly classifies it as a strong move. `move_type` returns
    // the promoted-piece index (NIGHT..QUEEN) for promotions; add its value on
    // the same MVV-LVA scale so a queen promo orders near a queen capture and
    // capture-promotions float above their plain-capture MVV-LVA rank.
    if (board::mv::is_promotion(move)) {
        mvv_lva += PIECE_VAL[board::mv::move_type(move)] * 16;
    }
    // Blend in capture history. The MAX_VAL scale (~16k) is comparable to
    // a half-pawn in MVV-LVA terms, so the history weight nudges ordering
    // without dominating it.
    int hist     = cap_history_.get(cb_->colorToMove, move);
    return mvv_lva + hist;
}

// ---------------------------------------------------------------------------
// qsearch — captures + promotions only, prunes losing captures by SEE<0.
// Mirrors bagaturchess.Search_PVS_NWS.qsearch(): no delta/futility pruning
// inside the loop; legality of remaining moves is the SEE filter alone.
//
// Probes and stores the transposition table with depth=0 — Java does the same
// (line 2178-2181). Entries placed by qsearch share the table with search()
// entries; depth-comparison logic naturally keeps higher-depth entries when
// the bucket is contested.
// ---------------------------------------------------------------------------
int Searcher::qsearch(int ply, int alpha, int beta, bool is_pv) {
    ++nodes_;
    if (ply > sel_depth_) sel_depth_ = ply;

    if (check_for_stop()) {
        aborted_ = true;
        return alpha;
    }
    if (ply >= MAX_PLY) return evaluate();

    // Repetition / 50-move / insufficient material — mirrors SearchImpl.isDraw():
    //   non-PV: 2nd occurrence is enough (a side that can force a repeat
    //           can drive towards the actual 3-fold; bound the score now).
    //   PV:     wait for the real 3-fold so we don't print a phantom PV.
    // Without this, drawn endgames spin in the MTD(f) loop — every search
    // call returns instantly (no draw cut), depth bumps without bound, and
    // no `on_iteration` fires because the score never sharpens past the
    // initial seed — so the GUI sees no PV at all.
    // `ply > 1` — never short-circuit at root: even in a true 3-fold /
    // 50-move / insufficient-material draw the engine must still emit a
    // legal move (UCI forbids `bestmove 0000` in a non-terminal position).
    // At root we let the search pick some legal move; at deeper plies the
    // cut prevents drawn-endgame spin in the MTD(f) loop. Previously the
    // 50-move and repetition cuts fired at root too — MTD then raced
    // through depths with no PV (each iteration "finished" instantly with
    // no legal move searched) and shipped `bestmove 0000`. Reproduces with
    // e.g. `position startpos moves g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1 f6g8`
    // then `go depth 5`.
    if (ply > 1) {
        if (cb_->lastCaptureOrPawnMoveBefore >= 100) return SCORE_DRAW;
        if (cb_->getRepetition() >= (is_pv ? 3 : 2)) return SCORE_DRAW;
        if (!cb_->hasSufficientMatingMaterial())     return SCORE_DRAW;
    }

    const int alpha_orig = alpha;   // saved for the alpha-restore step at the end

    // ----------------------------------------------------------------
    // TT probe (Java qsearch lines 1928-1978). Reused for both the
    // cutoff (non-PV nodes only) and to bias move ordering by TT move.
    //
    // Same near-draw safeguard as in search() — entries written when the
    // position had a different repetition / 50-move context carry stale
    // draw scores. Skip the cutoff but keep the TT move for ordering.
    // ----------------------------------------------------------------
    // `getRepetition()` counts the current visit too: 1 = first occurrence,
    // 2 = second occurrence (a transposition back to this position has just
    // happened), 3 = FIDE three-fold. We disable TT cutoff at 2+ visits —
    // a stored entry from the *first* visit could not see the draw lines
    // that become forcible on the *second* visit, so its score is stale.
    // Same family of staleness for 50-move counter — zobrist omits it, so
    // an entry stored at low halfmove is misleading near the limit. qsearch
    // tactical sequences are short (and captures reset the counter) so the
    // margin can be tight — 90 leaves ~10 plies of slack.
    // TT move is still pulled for ordering; only the cutoff is suppressed.
    bool tt_score_unreliable_for_cutoff =
        (cb_->getRepetition() >= 2)
        || (cb_->lastCaptureOrPawnMoveBefore >= 90);

    int     tt_move  = 0;
    TTEntry tte;
    if (tt_.probe(cb_->zobristKey, tte)) {
        int tt_score = score_from_tt(tte.score, ply);
        tt_move      = tte.move;

        if (!is_pv && !tt_score_unreliable_for_cutoff) {
            if (tte.flag == TT_EXACT) return tt_score;
            if (tte.flag == TT_LOWER && tt_score >= beta)  return tt_score;
            if (tte.flag == TT_UPPER && tt_score <= alpha) return tt_score;
        }
    }

    // When the side to move is in check, stand-pat is illegal — the attack on
    // the king MUST be answered. Standing pat here would (a) return a static
    // NNUE score for a position that might be checkmate, and (b) generate only
    // captures, missing the king moves / blocks that are often the sole legal
    // evasions — so qsearch could fail high on a lost position or miss a forced
    // mate. In check we therefore: never stand-pat, generate ALL legal moves,
    // never SEE-prune (a losing capture or a quiet block / king retreat can be
    // the only way out), and report mate when no legal move exists.
    //
    // This node is reachable in check via two paths the check-extension does
    // NOT cover: a capture inside qsearch that gives check (recurses straight
    // into qsearch), and a TT move searched without check extension dropping to
    // depth 0. `generateMoves` is itself check-aware (king escapes +
    // interpositions); `generateAttacks` adds capture-the-checker / capture-
    // promotion evasions. Together they yield the full evasion set.
    bool in_check = (cb_->checkingPieces != 0);

    int stand;
    int best;
    int best_move = 0;
    int played    = 0;   // legal moves actually searched (for mate detection)

    if (in_check) {
        stand = SCORE_MIN;   // sentinel: no valid static eval while in check
        best  = SCORE_MIN;
    } else {
        stand = evaluate();
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
        best = stand;
    }

    gen_.startPly();
    if (in_check) gen_.generateMoves(*cb_);   // king moves + interpositions
    gen_.generateAttacks(*cb_);

    // Score captures by MVV-LVA inline (no full sort — picks max each iter).
    // Boost the TT move so it's always tried first when present (Java's
    // PHASE_TT applies the same idea, but only when ttMove is a capture or
    // promotion — qsearch otherwise wouldn't reach it).
    int  buf_moves[256];
    int  buf_scores[256];
    int  n = 0;
    while (gen_.hasNext() && n < 256) {
        int m = gen_.next();
        if (!cb_->isLegal(m)) continue;
        buf_moves[n]  = m;
        buf_scores[n] = (m == tt_move) ? SCORE_TT_MOVE : score_capture(m);
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

        // SEE pruning — skip captures that lose material on the static
        // exchange (Java line 2079-2084). NOT applied to check evasions: a
        // losing capture (or a negative-SEE quiet block / king retreat) can be
        // the only legal way out of check.
        if (!in_check && board::see::getSeeCaptureScore(*cb_, m) < 0) continue;

        int side = cb_->colorToMove;
        cb_->doMove(m);
        eval_.after_make(*cb_, m, side);
        int score = -qsearch(ply + 1, -beta, -alpha, is_pv);
        eval_.after_unmake(*cb_, m, side);
        cb_->undoMove(m);

        if (aborted_) return alpha;
        ++played;

        if (score > best) {
            best      = score;
            best_move = m;
            if (score > alpha) {
                alpha = score;
                if (score >= beta) break;
            }
        }
    }

    // In check with no legal move played → checkmate. Stalemate is impossible
    // while in check, so this is always mate (never a draw). Without this the
    // alpha-restore below would return alpha_orig for a mated position.
    if (in_check && played == 0) return mated_in(ply);

    // Alpha-restore (Java line 2163-2175, `isOther_UseAlphaOptimizationInQSearch`).
    // If neither stand-pat nor any capture exceeded the *input* alpha, clamp
    // the return value back up to alpha_orig — turns fail-soft below alpha
    // into fail-hard at alpha. Makes the TT bound stored at the caller tighter.
    if (alpha_orig > best) {
        best      = alpha_orig;
        best_move = 0;
    }

    // ----------------------------------------------------------------
    // TT store at depth 0 (Java line 2178-2181). Same flag formula as
    // search()'s end-of-node store:
    //   best >= beta        → TT_LOWER (fail-high)
    //   best > alpha_orig   → TT_EXACT
    //   otherwise           → TT_UPPER (fail-low / alpha-restore)
    // ----------------------------------------------------------------
    TTFlag flag = (best >= beta) ? TT_LOWER
                 : (best > alpha_orig ? TT_EXACT : TT_UPPER);
    tt_.store(cb_->zobristKey, best_move, best, stand, /*depth=*/0, flag, ply);

    return best;
}

// ---------------------------------------------------------------------------
// Quiet-move scoring used by `singular_move_search()`. SME does NOT use the
// phase split (TT/good/killer/quiet/bad), so killers are boosted to the top
// of the quiet list here so they're picked first.
//
// The main `search()` loop pulls killers as their own phases and scores quiets
// inline by history alone — see Java Search_PVS_NWS PHASE_* convention:
//   PHASE_TT > PHASE_ATTACKING_GOOD > PHASE_KILLER_1 > PHASE_KILLER_2
//     > PHASE_QUIET > PHASE_ATTACKING_BAD
// ---------------------------------------------------------------------------
void Searcher::score_quiet_moves(int ply, int* moves, int* scores, int n) {
    int color   = cb_->colorToMove;
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
// `use_sme` gates singular-move extension recursion: `singular_move_search`
// passes false down so we never SME inside SME (1:1 with Java).
// ---------------------------------------------------------------------------
int Searcher::search(int ply, int depth, int alpha, int beta,
                     bool is_pv, bool cut_node, bool use_sme) {
    stacks_[ply].pv_length = 0;

    if (check_for_stop()) {
        aborted_ = true;
        return alpha;
    }

    if (ply >= MAX_PLY) return evaluate();

    // Repetition / 50-move / insufficient material — non-PV bails on 2nd
    // visit. See qsearch() above for the full rationale; same convention.
    // All three are guarded by `ply > 1` — the root MUST emit a legal move
    // (UCI forbids `bestmove 0000`). Without the guard, `goMTD` would race
    // through depths with no PV when the root is a 3-fold / 50-move draw.
    if (ply > 1) {
        if (cb_->lastCaptureOrPawnMoveBefore >= 100) return SCORE_DRAW;
        if (cb_->getRepetition() >= (is_pv ? 3 : 2)) return SCORE_DRAW;
        if (!cb_->hasSufficientMatingMaterial())     return SCORE_DRAW;
    }

    // Mate distance pruning.
    int mate_alpha = std::max(alpha, mated_in(ply));
    int mate_beta  = std::min(beta,  mate_in(ply + 1));
    if (mate_alpha >= mate_beta) return mate_alpha;
    alpha = mate_alpha;
    beta  = mate_beta;

    // qsearch at horizon
    if (depth <= 0) return qsearch(ply, alpha, beta, is_pv);

    ++nodes_;
    if (ply > sel_depth_) sel_depth_ = ply;

    bool in_check = (cb_->checkingPieces != 0);
    int  alpha_orig = alpha;

    // TT probe.
    //
    // The zobrist key captures only the piece layout + side-to-move +
    // castling + EP. It does NOT include the repetition count or the
    // 50-move counter — but the *search score* in the entry implicitly
    // depends on both (draw scores propagate up through the tree).
    //
    // When the *current* position has any repetition history or is close
    // to the 50-move rule, an entry stored during a *previous* visit was
    // computed under a different game-state context — its score may be
    // wildly wrong (e.g. the engine saw +50 cp on the first visit but
    // the second visit makes a 3-fold draw forcible, so the true score
    // is 0). Cutting on such an entry locks in the stale score and the
    // engine refuses to take the draw — exactly the symptom that crops
    // up after several moves of a game and not from a cold `position fen`.
    //
    // Mitigation: still pull the TT *move* for ordering, but don't take
    // the cutoff when the current game-state could shift the true score.
    // `getRepetition()` counts the current visit too: 1 = first occurrence,
    // 2 = second occurrence (a transposition back to this position has just
    // happened), 3 = FIDE three-fold. We disable TT cutoff at 2+ visits —
    // a stored entry from the *first* visit could not see the draw lines
    // that become forcible on the *second* visit, so its score is stale.
    //
    // Same family of staleness: the zobrist key omits the 50-move half-move
    // counter, so an entry stored when the counter was low (lots of plies
    // before draw) gives a misleading cutoff when revisited near the limit
    // (the score was computed under a wider non-draw horizon than we now
    // actually have). Mark the cutoff as unreliable when our current search
    // depth could push us past the 50-move boundary from this node.
    // TT move is still pulled for ordering; only the cutoff is suppressed.
    bool tt_score_unreliable_for_cutoff =
        (cb_->getRepetition() >= 2)
        || (cb_->lastCaptureOrPawnMoveBefore + depth >= 100);

    int     tt_move  = 0;
    int     tt_score = SCORE_DRAW;
    int     tt_depth = -1;
    TTFlag  tt_flag  = TT_NONE;
    TTEntry tte;
    if (tt_.probe(cb_->zobristKey, tte)) {
        tt_move  = tte.move;
        tt_score = score_from_tt(tte.score, ply);
        tt_depth = tte.depth;
        tt_flag  = static_cast<TTFlag>(tte.flag);

        if (!is_pv && tt_depth >= depth && !tt_score_unreliable_for_cutoff) {
            if (tt_flag == TT_EXACT) return tt_score;
            if (tt_flag == TT_LOWER && tt_score >= beta)  return tt_score;
            if (tt_flag == TT_UPPER && tt_score <= alpha) return tt_score;
        }
    }

    // SME gates (Search_PVS_NWS.java line 570-571).
    bool tt_is_lower_or_exact         = (tt_flag == TT_LOWER) || (tt_flag == TT_EXACT);
    bool tt_depth_enough_for_singular = tt_depth >= depth - 3;

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

    // `hasAtLeastOnePiece` — Java line 678 (computed once per node). True
    // when at least one side has a non-pawn piece. Pawn-only positions are
    // zugzwang-prone, so Java disables the entire non-PV pruning block
    // (static null / NMP / razoring) AND the per-move LMP/futility/SEE
    // gate when this is false. PHASE values match Java's: N/B=3, R=5, Q=9.
    bool has_at_least_one_piece = (cb_->material_factor_white >= 3)
                               || (cb_->material_factor_black >= 3);

    // `mateThreat` — set by NMP when probe/verify returns a mate score
    // against us at depth >= 2. Java line 786-790, 794-798. Used later to
    // disable LMP/futility/SEE-quiet (outer gate, Java line 1206) and LMR
    // (Java line 1301). Conservative: when NMP suggests a mate threat
    // exists, we keep full-depth searches on candidate moves.
    bool mate_threat = false;

    // Internal iterative reduction (IIR) — 1:1 with Java Search_PVS_NWS:
    //   line 695 (non-PV IIR, INSIDE the non-PV block, BEFORE NMP/razoring)
    //   line 996 (PV IIR, after SME)
    // When `tt_move == 0 && depth >= 2`, reduce depth by 1. We collapse
    // both into a single check placed BEFORE the non-PV pruning block,
    // so the reduced depth feeds into static-null / NMP / razoring depth
    // gates (matching Java's non-PV IIR ordering). For PV nodes the same
    // reduction fires here — Java's PV IIR is functionally equivalent
    // since the pruning block below skips PV anyway.
    if (depth >= 2 && tt_move == 0) --depth;

    // ----------------------------------------------------------------
    // Pruning at non-PV nodes — Java Search_PVS_NWS.java lines 714-834
    //
    // Constants and divisor `PRUNING_AGGRESSIVENESS = 1.333` (≈ * 3/4)
    // are taken from Search_PVS_NWS. The volatility-history term is
    // skipped (we don't track it); equivalent to volatility = 0.
    // ----------------------------------------------------------------
    if (!is_pv && !in_check && has_at_least_one_piece
        && !is_mate_score(alpha) && !is_mate_score(beta)) {

        // ------------------------------------------------------------
        // Fail-high pruning — `static_eval >= beta + 35` gate matches
        // Java's reverse-futility eligibility check.
        // ------------------------------------------------------------
        if (static_eval >= beta + 35) {

            // Static null move pruning — Java line 718-737.
            //   margin = depth * 80 - (improving ? 70 : 0)
            //   cut if static_eval - margin/1.333 >= beta
            //   return (2*beta + static_eval) / 3   (soft lower bound)
            if (depth <= 7) {
                int snm_margin = (depth * 80 - (improving ? 70 : 0)) * 3 / 4;
                if (static_eval - snm_margin >= beta) {
                    return (2 * beta + static_eval) / 3;
                }
            }

            // Verified null move pruning — Java line 740-799.
            //   r = max(1.5, depth/3 + 3 + min(3, max(0, static_eval-beta)/80)) * 1.555
            //   probe with `[beta-1, beta]` at ply+1
            //   if probe fails high, VERIFY at same ply, [beta-1, beta], depth-r
            //   only cut when both pass — prevents NMP zugzwang artifacts
            if (depth >= 3) {
                double r_raw   = depth / 3.0 + 3.0
                               + std::min(3.0, std::max(0, static_eval - beta) / 80.0);
                double r_d     = std::max(1.5, r_raw) * 1.555;
                int    r       = static_cast<int>(r_d);
                int    nd      = depth - r;

                // Null-move marker for the child's prev_move: 0 = no
                // parent move (continuation history skipped at ply+1).
                stacks_[ply].current_move = 0;
                cb_->doNullMove();
                int score = (nd <= 0)
                    ? -qsearch(ply + 1, -beta, -beta + 1, /*is_pv=*/false)
                    : -search(ply + 1, nd, -beta, -beta + 1, /*is_pv=*/false, !cut_node, use_sme);
                cb_->undoNullMove();
                if (aborted_) return alpha;

                if (score >= beta) {
                    // Verification — same ply, same window, reduced depth.
                    int verify = (nd <= 0)
                        ?  qsearch(ply, beta - 1, beta, /*is_pv=*/false)
                        :  search(ply, nd, beta - 1, beta, /*is_pv=*/false, /*cut_node=*/true, use_sme);
                    if (aborted_) return alpha;

                    if (verify >= beta) {
                        if (is_mate_score(verify)) verify = beta;
                        return verify;
                    }

                    // Java line 786-790: verify returned a mate score against
                    // us → mate threat exists, disable LMP/LMR below.
                    if (verify < -MATE_THRESHOLD && depth >= 2) {
                        mate_threat = true;
                    }
                }

                // Java line 794-798: probe returned a mate score against us →
                // mate threat exists, disable LMP/LMR below.
                if (score < -MATE_THRESHOLD && depth >= 2) {
                    mate_threat = true;
                }
            }
        }

        // ------------------------------------------------------------
        // Fail-low pruning — razoring with Java's scaled margin.
        //   margin = RAZORING_MARGIN(260) * depth / 1.333
        // ------------------------------------------------------------
        if (static_eval <= alpha) {
            if (depth <= 4) {
                int razor_margin = 260 * depth * 3 / 4;
                if (static_eval + razor_margin < alpha) {
                    int v = qsearch(ply, alpha, alpha + 1, /*is_pv=*/false);
                    if (v < alpha) return v;
                }
            }
        }
    }

    // ----------------------------------------------------------------
    // Singular-move extension + multi-cut — Java Search_PVS_NWS.java
    // line 927-992. Test whether the TT move is uniquely good at this
    // position. If yes, extend it. If a multi-cut is detected (≥2 moves
    // beat β at reduced depth), cut immediately.
    //
    // `tt_move_extension` semantics (Java line 1276):
    //    +2  → double extension (alternatives much weaker than TT move)
    //    +1  → single extension (TT move is singular)
    //     0  → no extension (SME not triggered)
    //    -2  → multi-cut hint: TT move not singular, search it shallower
    // ----------------------------------------------------------------
    int tt_move_extension = 0;
    if (use_sme
        && !in_check
        && depth >= 6
        && ply < MAX_PLY / 2                    // keep room for extensions
        && !is_mate_score(alpha)
        && !is_mate_score(beta)
        && tt_is_lower_or_exact
        && !is_mate_score(tt_score)             // avoid singular-margin math on mate scores
        && !tt_score_unreliable_for_cutoff      // tt_score may be stale near draw → SME would lie too
        && tt_depth_enough_for_singular
        && tt_move != 0
        && cb_->isValidMove(tt_move)) {

        int singular_margin = is_pv ? (depth * depth / 12)
                                    : (126 * depth / 55);
        int singular_beta   = tt_score - singular_margin;
        int singular_depth  = std::max(1, (depth - 1) / 2);

        int singular_value = singular_move_search(
            ply, singular_depth,
            singular_beta - 1, singular_beta,
            tt_move, cut_node);

        if (aborted_) return alpha;

        if (singular_value < singular_beta) {
            // TT move is singular — extend
            if (singular_value < singular_beta - singular_margin) {
                tt_move_extension = 2;   // double extension
            } else {
                tt_move_extension = 1;
            }
        } else if (!is_pv) {
            // Multi-cut: an alternative also beats β
            if (singular_value > beta
                && !is_mate_score(alpha)
                && !is_mate_score(beta)) {
                return singular_value;
            }
            tt_move_extension = -2;      // demote TT move depth
        }
    }

    // (IIR moved to before the non-PV pruning block — see above.)

    // ----------------------------------------------------------------
    // Collect & order moves
    //
    // Buckets follow Java's Search_PVS_NWS PHASE_* groups:
    //   good_caps = PHASE_ATTACKING_GOOD (captures/promotions, SEE >= 0)
    //   bad_caps  = PHASE_ATTACKING_BAD  (captures/promotions, SEE <  0)
    //   quiets    = PHASE_QUIET          (non-capture non-promotion)
    //
    // SEE is computed once per capture during the split. Java's phase loop
    // re-generates captures per phase and recomputes SEE on filter; we
    // collapse that into a single pass and cache `see` alongside bad caps
    // so the PHASE_ATTACKING_BAD pruning threshold (Java line 1247) uses
    // the precomputed value.
    // ----------------------------------------------------------------
    // Worst-case legal-move count in chess is 218 (constructed position,
    // R.D. Nunn). Caps bucket ≤ 64 covers the worst-case captures-only case
    // (≤ 16 victims × bounded attackers). Quiets bucket sized to 256 so we
    // never silently drop legal quiets in pathological multi-queen / very
    // crowded positions.
    int good_caps[64];   int good_cap_scores[64];                            int n_good_caps = 0;
    int bad_caps[64];    int bad_cap_scores[64];    int bad_cap_sees[64];    int n_bad_caps  = 0;
    int quiets[256];     int quiet_scores[256];                              int n_quiets    = 0;

    gen_.startPly();
    gen_.generateMoves(*cb_);
    gen_.generateAttacks(*cb_);
    while (gen_.hasNext()) {
        int m = gen_.next();
        if (!cb_->isLegal(m)) continue;
        if (board::mv::is_quiet(m)) {
            if (n_quiets < (int)(sizeof(quiets)/sizeof(quiets[0]))) {
                quiets[n_quiets++] = m;
            }
        } else {
            int see = board::see::getSeeCaptureScore(*cb_, m);
            if (see >= 0) {
                if (n_good_caps < (int)(sizeof(good_caps)/sizeof(good_caps[0]))) {
                    good_caps[n_good_caps]       = m;
                    good_cap_scores[n_good_caps] = score_capture(m);
                    ++n_good_caps;
                }
            } else {
                if (n_bad_caps < (int)(sizeof(bad_caps)/sizeof(bad_caps[0]))) {
                    bad_caps[n_bad_caps]       = m;
                    bad_cap_scores[n_bad_caps] = score_capture(m);
                    bad_cap_sees[n_bad_caps]   = see;
                    ++n_bad_caps;
                }
            }
        }
    }
    gen_.endPly();

    // Quiet scoring: butterfly history + 1-ply continuation. Killers are
    // tried in their own phases (PHASE_KILLER_1 / PHASE_KILLER_2) before
    // this list is iterated, and are skipped from the list via
    // `m == killer1 / killer2` in the QUIET phase.
    //
    // `prev_move` is the move PARENT chose to land on the current position.
    // 0 means the parent ply was a null-move (or we're at root) — in that
    // case the continuation table can't be probed and we fall back to
    // butterfly history only.
    int prev_move = (ply > 1) ? stacks_[ply - 1].current_move : 0;
    {
        int color = cb_->colorToMove;
        for (int i = 0; i < n_quiets; ++i) {
            int m = quiets[i];
            int score = history_.get(color, m);
            if (prev_move != 0) score += cont_history_.get(prev_move, m);
            quiet_scores[i] = score;
        }
    }

    int killer1 = killers_.primary(ply);
    int killer2 = killers_.secondary(ply);

    // ----------------------------------------------------------------
    // Move loop. Phases (Java Search_PVS_NWS PHASE_*):
    //   TT > ATTACKING_GOOD > KILLER_1 > KILLER_2 > QUIET > ATTACKING_BAD
    // ----------------------------------------------------------------
    int best_score   = SCORE_MIN;
    int best_move    = 0;

    // played_count = number of moves *actually played* at this node so far,
    // BEFORE the current iteration. Mirrors Java's
    // `movesPerformed_attacks + movesPerformed_quiet` semantics. The
    // previous code used an enumeration counter that bumped on every
    // skipped move too — that desynced both the LMP threshold and the
    // LMR table index by 1+ per pruned move, over-reducing late refute
    // moves in tactical positions.
    int played_count = 0;

    // ----------------------------------------------------------------
    // Fail-low pruning helpers — fired only for PHASE_QUIET (LMP +
    // futility) and PHASE_ATTACKING_BAD (SEE threshold). Java
    // Search_PVS_NWS line 1216 / 1247. Good captures and killers are
    // NEVER pre-move pruned — same gate Java applies.
    // ----------------------------------------------------------------
    auto should_skip_quiet_pre = [&](int m) -> bool {
        // Outer gate — Java line 1205-1213:
        //   !isPv && !mateThreat && !ssi.in_check && !isCheckMove
        //     && mP > 1 && hasAtLeastOnePiece
        //     && !isMateVal(alpha) && !isMateVal(beta)
        // `!isCheckMove` we approximate by `extension == 0` in the LMR path
        // only (we don't have a check-move predicate before doMove here);
        // pawn-only and mate-threat positions are zugzwang-prone, skip all
        // the LMP / futility / SEE-quiet pruning there.
        if (is_pv || in_check || mate_threat || !has_at_least_one_piece) return false;
        if (played_count < 1) return false;
        if (is_mate_score(alpha) || is_mate_score(beta)) return false;

        // LMP (Java line 1221): movesPerformed >= threshold → skip.
        int lmp_count = (3 + depth * depth / (improving ? 1 : 2)) * 3 / 4;
        if (played_count >= lmp_count) return true;

        // Futility (Java line 1229): static eval too low to recover.
        if (depth <= 7) {
            int futility = depth * 80 * 3 / 4;
            if (static_eval + futility <= alpha) return true;
        }

        // SEE pruning for non-captures (Java line 1238-1245). From the 4th
        // played move onwards at shallow depth, skip quiets that lose
        // material on the static exchange. Same -SEE_MARGIN*depth/AGGR
        // threshold as bad-cap SEE prune. `getSeeCaptureScore` handles
        // quiets correctly: MATERIAL_SEE[EMPTY]=0 makes the formula collapse
        // to `0 - opponent_recapture_value`, which is exactly the loss we
        // want to detect.
        if (played_count >= 3 && depth <= 7) {
            int see = board::see::getSeeCaptureScore(*cb_, m);
            int see_thresh = -80 * depth * 3 / 4;
            if (see < see_thresh) return true;
        }
        return false;
    };

    auto should_skip_bad_cap_pre = [&](int see) -> bool {
        // Same Java-aligned gate as the quiet variant — apply from 2nd move on.
        if (is_pv || in_check || mate_threat || !has_at_least_one_piece) return false;
        if (played_count < 1) return false;
        if (is_mate_score(alpha) || is_mate_score(beta)) return false;
        if (depth > 7) return false;
        // SEE pruning for captures (Java line 1247-1253).
        int see_thresh = -80 * depth * 3 / 4;
        return see < see_thresh;
    };

    // `forced_extension` is meaningful only for the TT move: it carries
    // the SME result (-2 / 0 / +1 / +2). Mirrors Java line 1273-1277
    // where TT move's new_depth uses tt_move_extension exclusively,
    // bypassing the check-extension branch.
    auto try_move = [&](int m, bool is_quiet_move, bool is_tt_move = false,
                        int forced_extension = 0) -> bool {
        int side = cb_->colorToMove;

        // Record THIS ply's chosen move BEFORE descending — child reads it
        // as its `prev_move` for continuation-history lookups.
        stacks_[ply].current_move = m;

        cb_->doMove(m);
        eval_.after_make(*cb_, m, side);

        // Check extension (cheap proxy: did this move leave the opponent in
        // check?). `cb_->doMove` above already flipped the side, so
        // checkingPieces now reflects a check GIVEN by `m`.
        int check_extension = (cb_->checkingPieces != 0) ? 1 : 0;

        int extension;
        if (is_tt_move) {
            // `forced_extension` carries the SME verdict (-2 / 0 / +1 / +2).
            // Java feeds it through exclusively, but that lets a checking TT
            // move — usually the best move — be searched a ply SHALLOWER than
            // the same move in a later phase (which gets the check extension),
            // and at depth 1 it drops the child straight into qsearch while in
            // check. Preserve an explicit SME demotion (negative), but never
            // discard the check extension when SME did not extend.
            extension = forced_extension;
            if (extension >= 0 && check_extension > extension) {
                extension = check_extension;
            }
        } else {
            extension = check_extension;
        }

        int new_depth = depth - 1 + extension;
        int score;

        // PVS / LMR — `played_count` is the index of THIS move in the
        // played sequence (0 for first, 1 for second, …), matching Java's
        // pre-increment `movesPerformed` semantics.
        if (played_count == 0) {
            // First played move — full window, propagate parent's is_pv.
            score = -search(ply + 1, new_depth, -beta, -alpha, is_pv, false, use_sme);
        } else {
            int reduction = 0;
            // LMR — Java doLMR gate (Search_PVS_NWS.java line 1301-1307):
            //   !mateThreat && new_depth >= 2 && !in_check && !isCheckMove
            //     && mP > 1 && isQuiet
            // `mate_threat` set by NMP if a mate score returned at this node
            // (zugzwang-style mate threat) — keep full-depth searches there.
            if (is_quiet_move && new_depth >= 2 && played_count >= 1
                && !in_check && extension == 0 && !mate_threat) {

                int rd = std::min(63, new_depth);
                int rm = std::min(63, played_count);
                double red = kLMR.t[rd][rm];

                if (!is_pv)              red += 1.0;
                if (cut_node)            red += 1.0;
                if (improving)           red -= 1.0;
                if (not_improving_twice) red += 1.0;

                // History feedback (Java line 1330-1335). `side` is the
                // mover (captured before doMove flipped colorToMove).
                // Butterfly history and continuation history both bias the
                // reduction: a quiet that's historically been a cutoff
                // (high `hist`/`cont`) gets searched deeper.
                int hist = history_.get(side, m);
                red -= 0.5 * static_cast<double>(hist) / HistoryTable::MAX_VAL;
                if (prev_move != 0) {
                    int cont = cont_history_.get(prev_move, m);
                    red -= 0.5 * static_cast<double>(cont) / ContinuationHistory::MAX_VAL;
                }

                if (red < 1.0)            red = 1.0;
                if (red > new_depth - 1)  red = new_depth - 1;
                reduction = static_cast<int>(red);
            }
            // Null-window reduced search
            score = -search(ply + 1, new_depth - reduction, -alpha - 1, -alpha, false, true, use_sme);
            // Re-search at full depth if the reduced search beat alpha
            if (score > alpha && reduction > 0) {
                score = -search(ply + 1, new_depth, -alpha - 1, -alpha, false, !cut_node, use_sme);
            }
            // PV re-search — Java Search_PVS_NWS line 1457:
            //   if (isPv && (moveNumber == 1 || score > bestScore))
            // The `played_count == 0` half is dead code here (handled by
            // the separate first-move branch above) but kept for 1:1
            // fidelity with Java. The `score > best_score` half is the
            // meaningful trigger: re-search at full window only when this
            // move actually IMPROVES the running best — looser than the
            // old `score > alpha` (alpha may have advanced past best_score
            // via the null-window re-search, masking the improvement).
            if (is_pv && (played_count == 0 || score > best_score)) {
                score = -search(ply + 1, new_depth, -beta, -alpha, true, false, use_sme);
            }
        }

        eval_.after_unmake(*cb_, m, side);
        cb_->undoMove(m);

        // Count this move as played before deciding on success/failure —
        // subsequent moves see the updated count for LMR/LMP.
        ++played_count;

        if (aborted_) return false;

        if (score > best_score) {
            best_score = score;
            best_move  = m;
            update_pv(ply, m);
            if (score > alpha) {
                alpha = score;
            }
            if (alpha >= beta) {
                if (is_quiet_move) {
                    killers_.on_cutoff(ply, m);
                    history_.on_cutoff(cb_->colorToMove, m, depth);
                    if (prev_move != 0) {
                        cont_history_.on_cutoff(prev_move, m, depth);
                    }
                } else {
                    // Capture/promotion cutoff — update capture history so
                    // future visits float this (attacker, to, captured) tuple
                    // above same-victim alternatives.
                    cap_history_.on_cutoff(cb_->colorToMove, m, depth);
                }
                return true;  // beta cutoff
            }
        } else if (is_quiet_move) {
            history_.on_poor(cb_->colorToMove, m, depth);
            if (prev_move != 0) {
                cont_history_.on_poor(prev_move, m, depth);
            }
        } else {
            cap_history_.on_poor(cb_->colorToMove, m, depth);
        }
        return false;
    };

    // PHASE_TT — TT move first. Never pruned; depth carries the SME extension.
    if (tt_move != 0 && cb_->isValidMove(tt_move)) {
        bool is_quiet_move = board::mv::is_quiet(tt_move);
        if (try_move(tt_move, is_quiet_move,
                     /*is_tt_move=*/true, tt_move_extension)) goto done;
        if (aborted_) return alpha;
    }

    // PHASE_ATTACKING_GOOD — SEE >= 0 captures/promotions, sorted by MVV-LVA.
    // No pre-move pruning (Java does not prune good captures).
    for (int picked = 0; picked < n_good_caps; ++picked) {
        int best_i = picked;
        for (int j = picked + 1; j < n_good_caps; ++j)
            if (good_cap_scores[j] > good_cap_scores[best_i]) best_i = j;
        if (best_i != picked) {
            std::swap(good_caps[picked],       good_caps[best_i]);
            std::swap(good_cap_scores[picked], good_cap_scores[best_i]);
        }
        int m = good_caps[picked];
        if (m == tt_move) continue;
        if (try_move(m, /*is_quiet=*/false)) goto done;
        if (aborted_) return alpha;
    }

    // PHASE_KILLER_1 — never pre-move pruned. Killers persist across nodes at
    // the same ply, so the move may be invalid/illegal in this position;
    // mirror Java's `isPossible` = `isValidMove && isLegal`.
    if (killer1 != 0 && killer1 != tt_move
        && cb_->isValidMove(killer1) && cb_->isLegal(killer1)) {
        if (try_move(killer1, /*is_quiet=*/true)) goto done;
        if (aborted_) return alpha;
    }

    // PHASE_KILLER_2 — same as above, plus must differ from killer1.
    if (killer2 != 0 && killer2 != tt_move && killer2 != killer1
        && cb_->isValidMove(killer2) && cb_->isLegal(killer2)) {
        if (try_move(killer2, /*is_quiet=*/true)) goto done;
        if (aborted_) return alpha;
    }

    // PHASE_QUIET — sorted by history. Skip TT and killers (already tried).
    for (int picked = 0; picked < n_quiets; ++picked) {
        int best_i = pick_next_quiet(quiets, quiet_scores, picked, n_quiets);
        if (best_i != picked) {
            std::swap(quiets[picked],       quiets[best_i]);
            std::swap(quiet_scores[picked], quiet_scores[best_i]);
        }
        int m = quiets[picked];
        if (m == tt_move || m == killer1 || m == killer2) continue;
        if (should_skip_quiet_pre(m)) continue;
        if (try_move(m, /*is_quiet=*/true)) goto done;
        if (aborted_) return alpha;
    }

    // PHASE_ATTACKING_BAD — SEE < 0 captures, sorted by MVV-LVA. SEE pruning
    // uses the value cached during the split.
    for (int picked = 0; picked < n_bad_caps; ++picked) {
        int best_i = picked;
        for (int j = picked + 1; j < n_bad_caps; ++j)
            if (bad_cap_scores[j] > bad_cap_scores[best_i]) best_i = j;
        if (best_i != picked) {
            std::swap(bad_caps[picked],       bad_caps[best_i]);
            std::swap(bad_cap_scores[picked], bad_cap_scores[best_i]);
            std::swap(bad_cap_sees[picked],   bad_cap_sees[best_i]);
        }
        int m = bad_caps[picked];
        if (m == tt_move) continue;
        if (should_skip_bad_cap_pre(bad_cap_sees[picked])) continue;
        if (try_move(m, /*is_quiet=*/false)) goto done;
        if (aborted_) return alpha;
    }

done:
    if (aborted_) return alpha;

    // No legal moves played → mate or stalemate
    if (played_count == 0) {
        return in_check ? mated_in(ply) : SCORE_DRAW;
    }

    // TT store
    TTFlag flag = (best_score >= beta) ? TT_LOWER
                 : (best_score > alpha_orig ? TT_EXACT : TT_UPPER);
    tt_.store(cb_->zobristKey, best_move, best_score, static_eval, depth, flag, ply);
    return best_score;
}

// ---------------------------------------------------------------------------
// singular_move_search — Java private singular_move_search() (line 1616).
//
// Searches all moves EXCEPT `tt_move_excl` with a null window at reduced
// depth. Returns the best score among alternatives. Used by the SME block
// in `search()` to decide if the TT move is uniquely good (extend it) or
// if multiple moves beat β (multi-cut).
//
// The TT lookup uses a *perturbed* hash key (xor with rotated tt_move
// scrambled bits) so we never collide with the parent node's TT entry —
// otherwise we'd just return tt_score and learn nothing about the
// singularity of tt_move.
//
// Quiet-move limit: only the first `1 + depth/2` non-check quiet moves
// are considered. Captures, promotions and check-givers are always tried.
// ---------------------------------------------------------------------------
int Searcher::singular_move_search(int ply, int depth, int alpha, int beta,
                                   int tt_move_excl, bool cut_node) {
    // Perturbed hash — mirrors Java line 1622:
    //   hashkey ^ Long.rotateLeft(((long) ttMove1) * 0x9E3779B97F4A7C15L, 32)
    auto rotl64 = [](std::uint64_t x, int n) noexcept {
        return (x << n) | (x >> (64 - n));
    };
    std::uint64_t scrambled = static_cast<std::uint64_t>(tt_move_excl)
                              * 0x9E3779B97F4A7C15ULL;
    std::uint64_t hashkey   = cb_->zobristKey ^ rotl64(scrambled, 32);

    // TT probe with the perturbed key. Same staleness guard as main search /
    // qsearch — entries stored under a different repetition / 50-move
    // context carry scores that don't apply to the current node.
    bool sme_tt_unreliable_for_cutoff =
        (cb_->getRepetition() >= 2)
        || (cb_->lastCaptureOrPawnMoveBefore + depth >= 100);
    TTEntry tte;
    if (tt_.probe(hashkey, tte)) {
        if (tte.depth >= depth && !sme_tt_unreliable_for_cutoff) {
            int tts = score_from_tt(tte.score, ply);
            if (tte.flag == TT_EXACT)                      return tts;
            if (tte.flag == TT_LOWER && tts >= beta)       return tts;
            if (tte.flag == TT_UPPER && tts <= alpha)      return tts;
        }
    }

    const int alpha_orig = alpha;

    // Generate every move at this position
    int caps[64],    cap_scores[64],    n_caps   = 0;
    int quiets[256], quiet_scores[256], n_quiets = 0;

    gen_.startPly();
    gen_.generateMoves(*cb_);
    gen_.generateAttacks(*cb_);
    while (gen_.hasNext()) {
        int m = gen_.next();
        if (!cb_->isLegal(m)) continue;
        if (m == tt_move_excl) continue;     // skip the excluded TT move
        if (board::mv::is_quiet(m)) {
            if (n_quiets < (int)(sizeof(quiets) / sizeof(quiets[0])))
                quiets[n_quiets++] = m;
        } else {
            if (n_caps < (int)(sizeof(caps) / sizeof(caps[0])))
                caps[n_caps++] = m;
        }
    }
    gen_.endPly();

    for (int i = 0; i < n_caps;   ++i) cap_scores[i] = score_capture(caps[i]);
    score_quiet_moves(ply, quiets, quiet_scores, n_quiets);

    const int quiet_limit = 1 + depth / 2;
    int       quiet_noncheck_played = 0;
    int       best_score = SCORE_MIN;
    int       best_move  = 0;

    auto try_alt = [&](int m, bool is_quiet_move) -> bool {
        int side = cb_->colorToMove;

        // Record THIS ply's chosen alternative move so the child reads it
        // as `prev_move` for continuation-history lookups.
        stacks_[ply].current_move = m;

        cb_->doMove(m);
        eval_.after_make(*cb_, m, side);

        bool gives_check = (cb_->checkingPieces != 0);

        // Quiet-move limit (Java line 1837-1845): cap the number of quiet
        // non-check moves we explore. Captures, promotions and checks are
        // always searched.
        bool skip = false;
        if (is_quiet_move && !gives_check) {
            ++quiet_noncheck_played;
            if (quiet_noncheck_played > quiet_limit) skip = true;
        }

        int score = SCORE_MIN;
        if (!skip) {
            // Nested SME is disabled — pass use_sme=false.
            score = -search(ply + 1, depth - 1, -beta, -alpha,
                            /*is_pv=*/false, cut_node, /*use_sme=*/false);
        }

        eval_.after_unmake(*cb_, m, side);
        cb_->undoMove(m);

        if (aborted_) return false;
        if (skip) return false;

        if (score > best_score) {
            best_score = score;
            best_move  = m;
        }
        if (score > alpha) {
            alpha = score;
            if (alpha >= beta) return true;   // β cutoff
        }
        return false;
    };

    // Captures sorted by MVV-LVA
    for (int picked = 0; picked < n_caps; ++picked) {
        int best_i = picked;
        for (int j = picked + 1; j < n_caps; ++j)
            if (cap_scores[j] > cap_scores[best_i]) best_i = j;
        if (best_i != picked) {
            std::swap(caps[picked],       caps[best_i]);
            std::swap(cap_scores[picked], cap_scores[best_i]);
        }
        if (try_alt(caps[picked], /*is_quiet=*/false)) goto sm_done;
        if (aborted_) return alpha;
    }

    // Quiets sorted by killer + history
    for (int picked = 0; picked < n_quiets; ++picked) {
        int best_i = pick_next_quiet(quiets, quiet_scores, picked, n_quiets);
        if (best_i != picked) {
            std::swap(quiets[picked],       quiets[best_i]);
            std::swap(quiet_scores[picked], quiet_scores[best_i]);
        }
        if (try_alt(quiets[picked], /*is_quiet=*/true)) goto sm_done;
        if (aborted_) return alpha;
    }

sm_done:
    if (aborted_) return alpha;

    // Store in the perturbed TT slot so subsequent SME calls at this node
    // hit the cached result.
    if (best_move != 0) {
        TTFlag f = (best_score >= beta)       ? TT_LOWER
                 : (best_score > alpha_orig)  ? TT_EXACT
                                              : TT_UPPER;
        tt_.store(hashkey, best_move, best_score, /*eval=*/0,
                  depth, f, ply);
    }

    return best_score;
}

// ---------------------------------------------------------------------------
// Top-level dispatch
// ---------------------------------------------------------------------------
Result Searcher::go(const Limits& lim) {
    return lim.use_mtd ? goMTD(lim) : goPVS(lim);
}

// ---------------------------------------------------------------------------
// Classic iterative deepening (PVS-only). Used when `Limits::use_mtd == false`.
// ---------------------------------------------------------------------------
Result Searcher::goPVS(const Limits& lim) {
    nodes_      = 0;
    node_check_ = 0;
    sel_depth_  = 0;
    aborted_    = false;
    stop_.store(false, std::memory_order_relaxed);
    max_nodes_  = lim.max_nodes;
    min_move_secs_         = lim.min_move_secs;
    total_clock_secs_      = lim.total_clock_secs;
    max_usage_percent_     = lim.max_usage_percent;
    consumed_vs_remaining_ = lim.consumed_vs_remaining;
    vol_last_eval_         = 0;
    vol_last_move_         = 0;
    vol_last_depth_        = 0;
    vol_accum_score_diff_  = 0.0;
    vol_usage_pct_         = 0.0;
    terminate_search_      = false;
    start_      = std::chrono::steady_clock::now();
    tt_.new_search();
    // Killers ARE cleared per go() — they're keyed by ply alone, not by
    // position. After the position changed (1+ moves were played between
    // searches) most of the previous killers point to moves that are
    // illegal here, AND the `m == killerN` skip in PHASE_QUIET would
    // silently drop a legitimate quiet that happens to share encoding
    // with a stale killer. Carrying them across positions tanks ordering
    // quality more than the cold-start cost saves. (history tables key by
    // [color][from][to] so they're position-independent and we KEEP those.)
    killers_.clear();
    // Note: eval_.reset() is NOT called here — `StateManager::cmd_position`
    // already invokes `set_board()` which refreshes NNUE accumulators.
    // search_main.cpp's CLI driver creates a fresh Searcher (whose ctor
    // calls eval_.reset) so the first go() is consistent there too.
    // Redundant reset costs ~hundreds-of-microseconds per move = noticeable
    // on 200 ms / move time controls.

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

        // Volatility update — same mechanism as goMTD.
        update_volatility(score, best.best_move, depth);

        if (min_move_secs_ > 0.0) {
            auto t_now = std::chrono::steady_clock::now();
            double tillNow = std::chrono::duration<double>(t_now - start_).count();
            if (tillNow >= min_move_secs_ &&
                tillNow >  consumed_vs_remaining_ * available_secs()) {
                terminate_search_ = true;
            }
        }

        if (time_up() || stop_.load(std::memory_order_relaxed)) break;
        if (is_mate_score(score)) break;  // already mated, no point going deeper
    }
    return best;
}

// ---------------------------------------------------------------------------
// MTD(f) γ-stepping driver
//
// Direct port of the loop in
// bagaturchess.search.impl.rootsearch.sequential.SequentialSearch_MTD.negamax()
// + bagaturchess.search.impl.rootsearch.sequential.mtd.NullwinSearchTask.run().
//
// Each iteration of the inner loop is one "NullwinSearchTask":
//   1. Pop the next beta from MTDSearchManager.
//   2. Run a null-window search at the manager's current depth: [beta-1, beta].
//   3. Update lower/upper bound based on whether the search failed high/low.
//   4. The manager auto-detects convergence and bumps the depth when bounds
//      have collapsed within TRUST_WINDOW_BEST_MOVE.
// ---------------------------------------------------------------------------
Result Searcher::goMTD(const Limits& lim) {
    nodes_      = 0;
    node_check_ = 0;
    sel_depth_  = 0;
    aborted_    = false;
    stop_.store(false, std::memory_order_relaxed);
    max_nodes_  = lim.max_nodes;
    min_move_secs_         = lim.min_move_secs;
    total_clock_secs_      = lim.total_clock_secs;
    max_usage_percent_     = lim.max_usage_percent;
    consumed_vs_remaining_ = lim.consumed_vs_remaining;
    vol_last_eval_         = 0;
    vol_last_move_         = 0;
    vol_last_depth_        = 0;
    vol_accum_score_diff_  = 0.0;
    vol_usage_pct_         = 0.0;
    terminate_search_      = false;
    start_      = std::chrono::steady_clock::now();
    tt_.new_search();
    // Killers ARE cleared per go() — they're keyed by ply alone, not by
    // position. After the position changed (1+ moves were played between
    // searches) most of the previous killers point to moves that are
    // illegal here, AND the `m == killerN` skip in PHASE_QUIET would
    // silently drop a legitimate quiet that happens to share encoding
    // with a stale killer. Carrying them across positions tanks ordering
    // quality more than the cold-start cost saves. (history tables key by
    // [color][from][to] so they're position-independent and we KEEP those.)
    killers_.clear();
    // Note: eval_.reset() is NOT called here — `StateManager::cmd_position`
    // already invokes `set_board()` which refreshes NNUE accumulators.
    // search_main.cpp's CLI driver creates a fresh Searcher (whose ctor
    // calls eval_.reset) so the first go() is consistent there too.
    // Redundant reset costs ~hundreds-of-microseconds per move = noticeable
    // on 200 ms / move time controls.

    // Initial seed value — full static eval at the root. Same scale as what
    // search() returns (side-to-move PoV in negamax).
    int initial_val = eval_.evaluate(*cb_);

    MTDSearchManager mgr(/*start_depth=*/1, /*max_iterations=*/lim.max_depth, initial_val);

    Result best{};
    best.score = initial_val;
    best.depth = 0;

    while (!stop_.load(std::memory_order_relaxed) &&
           mgr.getCurrentDepth() <= mgr.getMaxIterations()) {

        int depth = mgr.getCurrentDepth();
        int beta  = mgr.nextBeta();
        root_depth_ = depth;

        // Clamp beta so the null window [β-1, β] survives mate-distance
        // pruning at root (ply=1). search() does:
        //
        //   mate_alpha = max(alpha, mated_in(1) = -MAX_MATE+1)
        //   mate_beta  = min(beta,  mate_in(2)  =  MAX_MATE-2)
        //   if (mate_alpha >= mate_beta) return mate_alpha;
        //
        // The previous clamp left β = SCORE_MIN+2 = -MAX_MATE+1 (or
        // SCORE_MAX-2 at the high end). Both collapse the post-prune window
        // to empty, so search() returns immediately — no PV is built, the
        // MTD bounds never tighten, and the iteration silently spins.
        //
        // Safe band: `mated_in(1) + 1 ≤ β ≤ mate_in(2)`.
        if (beta <= mated_in(1)) beta = mated_in(1) + 1;
        if (beta >  mate_in(2))  beta = mate_in(2);

        // Reset the root PV stack so we can grab a fresh PV after the search.
        stacks_[1].pv_length = 0;

        // Null-window search at [beta-1, beta]. is_pv stays true so the root
        // PV gets populated; deeper recursion naturally enters non-PV branches
        // because the window is null.
        int eval = search(/*ply=*/1, depth, beta - 1, beta,
                          /*is_pv=*/true, /*cut_node=*/false);

        if (aborted_) break;

        // Check whether THIS eval would actually sharpen the bound *before*
        // calling the manager — `increaseLowerBound` / `decreaseUpperBound`
        // may reset bounds to [MIN, MAX] when they trigger isLast() and bump
        // the depth. Mirrors Java's `sentPV = (eval >= getLowerBound())` /
        // `sentPV = (eval <= getUpperBound())` test on line 227 / 264.
        //
        // `depth_converged` is true when the manager bumped `current_depth_`
        // inside the call — i.e. the MTD bisection narrowed to within the
        // trust window and the next probe will be at a new depth. Used to
        // gate the dynamic-time volatility update: per-probe bound changes
        // are MTD noise (a fail-low followed by a fail-high at the same
        // depth is one converging probe, not two volatile events), so we
        // only feed depth-to-depth transitions into the volatility model.
        int pre_depth = mgr.getCurrentDepth();
        bool advanced;
        bool is_lower;   // failed-high → eval is a lower bound on the true score
        if (eval >= beta) {
            advanced = (eval >= mgr.getLowerBound());
            mgr.increaseLowerBound(eval);
            is_lower = true;
        } else {
            advanced = (eval <= mgr.getUpperBound());
            mgr.decreaseUpperBound(eval);
            is_lower = false;
        }
        bool depth_converged = (mgr.getCurrentDepth() != pre_depth);

        // Only update the public Result and fire the info callback when the
        // bound was actually tightened. Mirrors Java's "sentPV" gate.
        if (advanced) {
            Stack& s = stacks_[1];
            if (s.pv_length > 0) {
                // CRITICAL — only commit best_move/pv on a LOWERBOUND
                // (fail-high) advance. Upperbound (fail-low) advances
                // suppress the PV in the UCI line (per Java's convention),
                // so committing them silently desyncs the bestmove sent at
                // the end from the last PV the GUI saw — that is exactly
                // the "engine shows draw PV but plays losing move" symptom.
                // Always allow the very first commit (so `bestmove 0000`
                // is never sent on very short searches).
                if (is_lower || best.best_move == 0) {
                    best.best_move = s.pv[0];
                    std::memcpy(best.pv.data(), s.pv, s.pv_length * sizeof(int));
                    best.pv_length = s.pv_length;
                }
            }
            best.score       = eval;
            best.lower_bound = is_lower;
            best.upper_bound = !is_lower;
            // The MTDSearchManager may have just incremented current_depth_
            // if this call converged. Report the depth we actually searched.
            best.depth = depth;

            auto now      = std::chrono::steady_clock::now();
            best.time_secs= std::chrono::duration<double>(now - start_).count();
            best.nodes    = nodes_;
            best.seldepth = sel_depth_;

            if (lim.on_iteration) lim.on_iteration(best, lim.callback_user);

            // Volatility update — only when the depth actually converged.
            // Earlier this fired on every `advanced` probe (per-bound), so
            // MTD bisection noise within a single depth (fail-low followed
            // by fail-high) inflated `vol_accum_score_diff_` and pulled the
            // dynamic time budget up for the wrong reason. The Java
            // counterpart in MoveEvalInAccount.newPVLine() is fed by a
            // committed depth-result, not by intermediate bounds — gating
            // on `depth_converged` here matches that semantic.
            if (depth_converged) {
                update_volatility(best.score, best.best_move, depth);
            }
        }

        // Iteration-boundary terminate gate — Java newIteration() in
        // ConsumedTimeVSRemainingTimeInAccount line 56-58:
        //   terminate = (tillNow >= minMoveTime &&
        //                tillNow >  consumedVsRemaining × availableTime)
        // Once set, time_up() will return true for the rest of the search
        // and the next iteration won't start.
        if (min_move_secs_ > 0.0) {
            auto now = std::chrono::steady_clock::now();
            double tillNow = std::chrono::duration<double>(now - start_).count();
            if (tillNow >= min_move_secs_ &&
                tillNow >  consumed_vs_remaining_ * available_secs()) {
                terminate_search_ = true;
            }
        }

        if (time_up() || stop_.load(std::memory_order_relaxed)) break;

        // Mate found in the proved interval — no value in chasing more depth.
        if (is_mate_score(mgr.getLowerBound()) && mgr.getLowerBound() > 0) break;
        if (is_mate_score(mgr.getUpperBound()) && mgr.getUpperBound() < 0) break;
    }
    return best;
}

}  // namespace search
