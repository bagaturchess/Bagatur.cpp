# search

C++17 port of `bagaturchess.search.impl.alg.impl1.Search_PVS_NWS` — the
PVS/NWS alpha-beta searcher that drives the Bagatur engine.

## Scope of the port

This is a **faithful-to-spirit** port, not a 1:1 mirror. The Java file is 2275
lines and pulls in ~4500 more across `tpt/`, `history/`, `pv/`, `movelists/`,
`env/`, etc. The port keeps the algorithmic heart while dropping infrastructure
that would each be its own multi-thousand-line port.

### Retained

| Feature                          | Where in Java                          |
| -------------------------------- | -------------------------------------- |
| PVS at PV nodes, NWS at non-PV   | `search()` main loop                   |
| MTD(f) γ-stepping root driver    | `SequentialSearch_MTD` + `BetaGenerator` + `SearchManager` |
| Singular Move Extension (SME)    | `singular_move_search()` + multi-cut   |
| Quiescence search at horizon     | `qsearch()`                            |
| TT lookup + non-PV cutoffs       | `env.getTPT().get(...)` + flag checks  |
| Mate distance pruning            | `alpha = max(alpha, -getMateVal(ply))` |
| Static null-move (reverse fut.)  | "Static null move pruning" block       |
| Null-move pruning + verification | "Verified null move pruning" block     |
| Razoring                         | "Razoring" block                       |
| Late-move reductions             | `LMR_TABLE` + reduction formula        |
| Internal iterative reduction     | "Reduce depth if TT value is not presented" |
| Check extension                  | per-move extension on giving-check     |
| Move ordering: TT > caps > kil > hist | `PHASE_TT..PHASE_QUIET` loop       |
| Butterfly history                | `IHistoryTable` (registerGood/Bad)     |
| Killers (2/ply)                  | `env.getKillers()`                     |
| Dynamic time budget              | `TimeController_FloatingTime` + `MoveEvalInAccount` + `ConsumedTimeVSRemainingTimeInAccount` |

### Dropped

| Feature                          | Why                                    |
| -------------------------------- | -------------------------------------- |
| ProbCut + TT-ProbCut             | Significant churn in pruning decisions |
| Continuation / capture history   | Three more tables × log + the move-listener plumbing |
| Pawn / material / volatility correction history | Needs incremental hash work |
| Endgame TB probing               | `probeTB()` — external Syzygy bridge   |
| Eval cache                       | NNUE already incremental               |
| Aspiration windows               | MTD(f) replaces them                   |
| Search statistics                | Stats class                            |
| SMP / lazy SMP                   | Would need work-splitting + shared TT  |

## File layout

```
src/search/
  types.h          score constants, TT flags, mate helpers
  tt.{h,cpp}       4-bucket transposition table
  history.h        butterfly history + killers (header-only)
  mtd.{h,cpp}      BetaGenerator + MTDSearchManager (γ-stepping bounds)
  search.{h,cpp}   the Searcher class — goMTD / goPVS / search / qsearch
  search_main.cpp  CLI driver
```

## API

```cpp
#include "search/search.h"

auto cb  = board::cbu::getNewCB(fen);
auto sr  = std::make_unique<search::Searcher>(*cb, /*tt_mb=*/512);  // heap-allocate

search::Limits lim;
lim.use_mtd     = true;        // MTD(f); set to false for classic PVS-only iterative deepening
lim.max_depth   = 12;
lim.max_nodes   = 0;           // 0 = unlimited

// Dynamic time budget — see "Dynamic time budget" below. Leave all zero for
// FIXED_DEPTH / FIXED_NODES / INFINITE.
lim.min_move_secs         = 1.0;   // guaranteed minimum
lim.total_clock_secs      = 60.0;  // hard ceiling
lim.max_usage_percent     = 0.20;  // dynamic-component cap
lim.consumed_vs_remaining = 0.50;  // iteration-boundary terminate factor

search::Result res = sr->go(lim);
// res.score, res.best_move, res.pv[..res.pv_length], res.nodes,
// res.lower_bound / res.upper_bound (MTD info)
```

> **Heap-allocate the Searcher** — it owns a 256-deep accumulator-snapshot stack
> for incremental NNUE undo (≈3 MB) plus a 512 MB TT by default. Stack-allocating
> it overflows MinGW's default 1 MB thread stack.

## MTD(f) γ-stepping

`Limits::use_mtd = true` (the default) drives the root with MTD(f) instead of
classic PVS aspiration. Direct port of
`bagaturchess.search.impl.rootsearch.sequential.SequentialSearch_MTD.negamax()`
+ `mtd/BetaGenerator.java` + `mtd/SearchManager.java`.

### The driver loop — `Searcher::goMTD()`

For each iteration:

1. **Pick `β`** from `MTDSearchManager::nextBeta()`. Internally:
   - First call: `β = initial_eval` (the static eval of the root).
   - Both bounds known: midpoint of `[lower, upper]` — classic MTD bisection.
   - One bound known: γ-step from that bound by `interval × trend_multiplier`,
     where `trend_multiplier` doubles on every consecutive same-direction step
     (exponential probe — recovers fast from a far-off seed).
2. **Clamp `β`** into the safe band `[mated_in(1) + 1, mate_in(2)]`. Without
   this clamp, root mate-distance pruning collapses the post-prune window to
   empty, `search()` returns immediately, and the bounds never tighten —
   the loop silently spins.
3. **Null-window search** at `[β-1, β]`. `is_pv = true` so the root PV stack
   gets populated; deeper recursion naturally enters non-PV branches.
4. **Update bounds**:
   - `eval >= β` → failed high → `increaseLowerBound(eval)`
   - `eval <  β` → failed low  → `decreaseUpperBound(eval)`
5. **Check convergence** (`MTDSearchManager::isLast()`):
   `lower + TRUST_WINDOW_BEST_MOVE >= upper`, with a fast-path when a mate
   value is bracketed. Once converged, the manager bumps `current_depth_`
   and resets the bounds for the next iteration.

### PV / `bestmove` commit gating — the subtle bit

After the search, the iteration may have *advanced* a bound (sharpened
`lower` or `upper`) or not. We only commit to the public `Result` when a
bound advanced (mirrors Java's `sentPV` gate on `SequentialSearch_MTD` line
227 / 264). But there's a further wrinkle:

  - **Lower-bound advances** (fail-high) emit a normal UCI `info ... pv ...`
    line — the move proved at least as good as `β`, so its PV is meaningful.
  - **Upper-bound advances** (fail-low) emit `info ... upperbound` *without*
    a `pv` segment, because the move that "failed least" is unreliable as a
    PV (the GUI would be misled).

If we committed `best.best_move` on *both* kinds of advance, then on a
fail-low iteration we'd silently overwrite the lower-bound-committed move
with a move the GUI never saw in any `info` line — and ship `bestmove X`
at the end of the search while the GUI's last displayed PV started with `Y`.
This was a real bug ("engine shows draw PV but plays losing move").

Fix: only commit `best.best_move` / `best.pv` on **lowerbound** advances
(or on the very first commit, so short searches never ship `bestmove 0000`):

```cpp
if (is_lower || best.best_move == 0) {
    best.best_move = stacks_[1].pv[0];
    std::memcpy(best.pv.data(), stacks_[1].pv, ...);
    best.pv_length = stacks_[1].pv_length;
}
best.score       = eval;
best.lower_bound = is_lower;
best.upper_bound = !is_lower;
```

The `Result::lower_bound` / `upper_bound` flags carry through to UCI, where
the driver appends `lowerbound` / `upperbound` to the `info` line and skips
the `pv` segment on upper.

### Why MTD(f)?

Two practical wins over classic PVS at the root:

  - **Tighter root windows** — every probe is a null-window search, which
    cuts ~2× faster than a full-window search. The total node count to
    finish a depth is competitive with PVS-aspiration once the TT warms up.
  - **Built-in resume-from-bound** — every probe ends with one bound
    sharpened. There is no "re-search at wider window after a fail",
    which the aspiration window approach needs.

The classic counter-argument (MTD thrashes on noisy evals) is largely
defused by the `trend_multiplier_` ramp in `BetaGenerator` — consecutive
same-direction failures double the step, so a far-off seed value converges
in `O(log Δ)` probes instead of `O(Δ / interval)`.

### Falling back to PVS

`Limits::use_mtd = false` drops to `goPVS()` — a vanilla iterative-deepening
loop with `[SCORE_MIN, SCORE_MAX]` at the root. Useful for A/B testing
search changes when you don't want MTD's narrowing dynamics in the mix.

## Dynamic time budget

`compute()` in `src/uci/time_control.cpp` produces a four-field budget that
is consumed by `Searcher` to gate iterations. 1:1 with Java's
`TimeController_FloatingTime` × `MoveEvalInAccount` ×
`ConsumedTimeVSRemainingTimeInAccount` chain:

```
available_time = min_move_secs + total_clock_secs × usage_pct_dyn
```

where `usage_pct_dyn` starts at 0 and grows toward `max_usage_percent` as
per-iteration eval/best-move volatility accumulates. Score-diff is decayed
on depth change, accumulated with a per-iteration `cur_diff`, and bumped by
a `SCORES_PENALTY_DIFF_MOVES` constant when the best move changes between
iterations.

Iteration-boundary terminate gate (Java `newIteration()`):

```
stop next iteration iff
  elapsed >= min_move_secs AND
  elapsed >  consumed_vs_remaining × available_time
```

Once set, `time_up()` returns true for the rest of the search. The effect:
the engine runs longer on volatile positions (eval swings, best-move
flips) and exits early on quiet end-games where it has already converged.

For `FIXED_DEPTH` / `FIXED_NODES` / `INFINITE`, leave `min_move_secs = 0` —
the searcher treats it as "no time cap" and only `max_depth` / `max_nodes`
/ external `stop()` end the search.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/Release/search_main 10                  # startpos, depth 10
./build/Release/search_main 8 "FEN..."          # custom position
./build/Release/search_main 12 "FEN..." 5       # with 5-second time limit
```

## Smoke-test output

```
$ search_main 10
position fen: rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
limits: depth=10  time=0.00s

info depth 10 seldepth 20 nodes 93464 time 80 nps 1165109
     score cp 67 pv d2d4 d7d5 c2c4 d5c4 e2e3 e7e5 f1c4 g8f6 b1c3 f8b4
bestmove d2d4
```

QGA mainline at depth 10, ~1.17 Mnps with full NNUE eval at every node.

On a tactical mate-in-2 puzzle (`r2qkb1r/pp2nppp/3p4/2pNN1B1/2BnP3/3P4/PPP2PPP/R2bK2R w KQkq - 1 0`):

```
info depth 5 ... score cp 29996 pv d5f6 g7f6 c4f7
bestmove d5f6
```

`29996 = MAX_MATE - 4 plies` → mate in 2.

Under MTD(f), `info` lines may also carry `lowerbound` / `upperbound` markers
after the `score` field. UCI mode (`StateManager`) drops the `pv` segment on
`upperbound` lines — see "PV / `bestmove` commit gating" above for why.
