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
| Quiescence search at horizon     | `qsearch()`                            |
| TT lookup + non-PV cutoffs       | `env.getTPT().get(...)` + flag checks  |
| Mate distance pruning            | `alpha = max(alpha, -getMateVal(ply))` |
| Static null-move (reverse fut.)  | "Static null move pruning" block       |
| Null-move pruning + reduction    | "Verified null move pruning" block (sans verify) |
| Razoring                         | "Razoring" block                       |
| Late-move reductions             | `LMR_TABLE` + reduction formula        |
| Internal iterative reduction     | "Reduce depth if TT value is not presented" |
| Check extension                  | per-move extension on giving-check     |
| Move ordering: TT > caps > kil > hist | `PHASE_TT..PHASE_QUIET` loop       |
| Butterfly history                | `IHistoryTable` (registerGood/Bad)     |
| Killers (2/ply)                  | `env.getKillers()`                     |

### Dropped

| Feature                          | Why                                    |
| -------------------------------- | -------------------------------------- |
| Singular extension / multi-cut   | Needs `singular_move_search()` + heavy interaction with TT depth |
| ProbCut + TT-ProbCut             | Significant churn in pruning decisions |
| Continuation / capture history   | Three more tables × log + the move-listener plumbing |
| Pawn / material / volatility correction history | Needs incremental hash work |
| Endgame TB probing               | `probeTB()` — external Syzygy bridge   |
| Eval cache                       | NNUE already incremental               |
| Aspiration windows               | Driver simplification                  |
| Search statistics                | Stats class                            |
| SMP / lazy SMP                   | Would need work-splitting + shared TT  |
| ChannelManager / `IUCIDriver`    | No UCI loop yet — stdout instead       |

## File layout

```
src/search/
  types.h          score constants, TT flags, mate helpers
  tt.{h,cpp}       4-bucket transposition table
  history.h        butterfly history + killers (header-only)
  search.{h,cpp}   the Searcher class
  search_main.cpp  CLI driver
```

## API

```cpp
#include "search/search.h"

auto cb  = board::cbu::getNewCB(fen);
auto sr  = std::make_unique<search::Searcher>(*cb, /*tt_mb=*/64);  // heap-allocate

search::Limits lim;
lim.max_depth     = 12;
lim.max_time_secs = 5.0;        // optional

search::Result res = sr->go(lim);
// res.score, res.best_move, res.pv[..res.pv_length], res.nodes, ...
```

> **Heap-allocate the Searcher** — it owns a 256-deep accumulator-snapshot stack
> for incremental NNUE undo (≈3 MB). Stack-allocating it overflows MinGW's
> default 1 MB thread stack.

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
