# `uci` — UCI protocol layer

This package is the UCI driver for the C++ port of Bagatur. It speaks the
Universal Chess Interface on stdin/stdout, owns the board and the searcher,
and runs the search on a background thread so `stop` / `quit` stay
responsive.

The C++ design mirrors the Java original 1:1 where it can:

| C++                                | Java reference                                           |
| ---------------------------------- | -------------------------------------------------------- |
| `uci::StateManager`                | `bagaturchess.uci.impl.StateManager`                     |
| `uci::Go`                          | `bagaturchess.uci.api.commands.go.Go`                    |
| `uci::Position`                    | `bagaturchess.uci.api.commands.Position`                 |
| `uci::Protocol` (constants)        | `bagaturchess.uci.api.commands.Protocol`                 |
| `uci::TimeBudget` / `compute()`    | `bagaturchess.search.impl.uci_adaptor.timemanagement.*`  |
| `uci::uci_to_move` / `move_to_uci` | `bagaturchess.uci.api.utils.uci.UCIMoveConverter`        |

Three UCI options are exposed — `ThreadsCount`, `TTSize` and `UCI_Chess960`
(see [UCI options](#uci-options)). Otherwise the layer is deliberately minimal:
no ponder bookkeeping beyond accepting the flag, no channel abstraction (writes
go straight to stdout).

## Files

```
src/uci/
├── protocol.h          — engine name/version, all CMD_* / REPLY_* tokens
├── go.{h,cpp}          — parser for `go ...` (wtime, btime, movetime, depth, …)
├── position.{h,cpp}    — parser for `position [startpos|fen ...] [moves ...]`
├── uci_move.{h,cpp}    — UCI ⇄ Bagatur internal-move conversion
├── time_control.{h,cpp}— classify(go) + compute(go, side) → TimeBudget
├── state_manager.{h,cpp}— UCI command loop, threaded search, info-line stream
└── uci_main.cpp        — exe entry point (`Bagatur.cpp-x64`)
```

## Move conversion

Bagatur's internal square layout is file-reversed relative to standard UCI:

```
Bagatur:  H1 = 0,  G1 = 1, ..., A1 = 7,  H2 = 8, ...
UCI:      A1 = 0,  B1 = 1, ..., H1 = 7,  A2 = 8, ...
```

The translation is a single bit flip on the file index — `sq ^ 7`. This
happens once on the way in (UCI move → internal `int`) and once on the way
out (PV / bestmove printing).

Move parsing is done by generating the legal-move list for the current
position and matching by `(from, to, promotion)`. That is robust against
move-encoding differences and inherits castling/EP correctness from the
generator.

### Chess960 castling notation

Castling is encoded internally as the king's two-square target
(king→`G1`/`C1`/`G8`/`C8`), which prints as the classic `e1g1` / `e1c1`. With the
`UCI_Chess960` option on, the boundary uses **king-takes-rook** instead (`e1h1`,
or an arbitrary rook file in a shuffled start) — the unambiguous form GUIs use
for 960, where king and rook files vary:

* `move_to_uci(move, &castlingConfig)` maps the king-target back to the rook's
  start square for the printed destination.
* `uci_to_move(cb, str, /*frc=*/true)` accepts that king-takes-rook form — and,
  leniently, still the classic king-target, since a king never has a
  non-castling two-square move so the two forms can't collide.

The board mechanics underneath are FRC-capable **unconditionally**: FEN castling
rights parse in both `KQkq` and Shredder/X-FEN file-letter form, rook start
squares are detected from the position, and castle legality uses the 960
rook-shield rule. `UCI_Chess960` only switches the *notation* at this boundary.

## Time controllers

`time_control.cpp` reproduces Bagatur's `TimeControllerFactory` priority
ordering. `classify()` picks one of seven `TCType`s — `compute()` then
turns the `Go` into a single `TimeBudget` (depth limit / node limit /
move-time seconds) for the search driver.

| `TCType`               | When                                          | Budget               |
| ---------------------- | --------------------------------------------- | -------------------- |
| `INFINITE`             | `go infinite` (or no time fields at all)      | no limits — wait for `stop` |
| `FIXED_DEPTH`          | `go depth N`                                  | `max_depth = N`     |
| `FIXED_NODES`          | `go nodes N`                                  | `max_nodes = N`     |
| `TIME_PER_MOVE`        | `go movetime ms`                              | movetime − small safety margin |
| `INCREMENT_PER_MOVE`   | `go ... winc/binc > 0`                        | `clock / 35 + inc`  |
| `TOURNAMENT`           | `go ... movestogo N` (no increment)           | `clock / movestogo + inc` |
| `SUDDEN_DEATH`         | only `wtime/btime` (no inc, no movestogo)     | shaped by `FloatingTime` / emergency factor |

`SUDDEN_DEATH` uses the same emergency-factor curve as Java's
`TimeController_SuddenDeath`: 3× shrink at 0 ms remaining, linearly
relaxing to 1× by 20 000 ms, never below 1×.

## Search lifecycle

```
go ──► stop_and_join_search()
   ──► ThreadsCount == 1 ? ensure_searcher()   (single Searcher, shared TT)
                         : ensure_smp()          (SMPSearcher, N workers, shared TT)
   ──► compute(go, side) → TimeBudget
   ──► search_thread = thread {
            res = use_smp ? smp_->go(lim) : (tt_->new_search(), searcher_->go(lim));
            send "bestmove " + uci(res.best_move);
        }
```

`StateManager` owns one lock-free `TranspositionTable` (sized by `TTSize`) and
injects it into both the single `Searcher` and every SMP worker — see
[search/smp](../search/README.md#smp--lazy-smp) for the coordinator + merge.

* Each iteration of the search fires the `info_callback`, which streams an
  `info depth … pv …` line to stdout. Emission is throttled to ~12/s
  (`kMinInfoIntervalSecs = 80 ms`), but the first PV-bearing line of every
  depth is force-emitted so a GUI never misses a depth. Upperbound (fail-low)
  lines carry the `upperbound` tag and omit the PV; engine replies
  (`uciok` / `readyok` / `bestmove`) are serialized through `io_mutex_`.
* `position` rebuilds the board, replays the moves, then **rebinds** whichever
  searchers exist (single and/or SMP) to it via `set_board()` and refreshes the
  NNUE accumulators — but KEEPS them alive. Dropping the shared TT + eval cache
  on every move collapses NPS ~10× under a GUI like Arena, so the TT and the
  position-independent history tables are deliberately carried across the moves
  of a game. (Each SMP worker keeps its own private copy of the board.)
* `ucinewgame` drops both searchers and clears the shared TT, so the next game
  starts from an empty transposition table.

## UCI options

Both options are **dynamic** — they take effect at runtime, no engine restart
needed. `cmd_setoption` first joins any running search (per the UCI spec
`setoption` only arrives when idle, but a `TTSize` resize must not reallocate the
table out from under the worker threads).

| Option         | Type  | Default | Range             | Effect |
| -------------- | ----- | ------- | ----------------- | ------ |
| `ThreadsCount` | spin  | 1       | 1 … 2×CPU-cores   | SMP worker count. Applied on the next `go` (1 → single `Searcher`; > 1 → `SMPSearcher`, recreated when the count changes). |
| `TTSize`       | spin  | 512     | 1 … 65536 (MB)    | Shared transposition-table size. Resized immediately. |
| `UCI_Chess960` | check | false   | true / false      | Chess960 castling I/O. When `true`, castling is read and written as king-takes-rook (`e1h1`) instead of the classic king-target (`e1g1`) — see [Chess960 castling notation](#chess960-castling-notation). Board mechanics are FRC-capable regardless; this only switches the notation. |

The `max` for `ThreadsCount` is `2 × std::thread::hardware_concurrency()`.

## Network — embedded, not loaded from disk

The NNUE network (`network_bagatur.nnue`, ~16 MB) is linked **into**
`Bagatur.cpp-x64.exe` at build time via `cmake/embed_binary.cmake`. The
exe is fully self-contained: drop it into cutechess / Arena / ChessBase
without any companion files, and the working directory does not matter.

The CMake block that drives this is the only thing you need to touch if
you want to swap networks:

```cmake
set(BAGATUR_NETWORK_FILE "${CMAKE_SOURCE_DIR}/network_bagatur.nnue")
if (EXISTS "${BAGATUR_NETWORK_FILE}")
    embed_binary(...)
    target_compile_definitions(nnue PUBLIC BAGATUR_EMBEDDED_NETWORK=1)
endif()
```

If the file is absent at configure time, `BAGATUR_EMBEDDED_NETWORK` stays
undefined and the engine falls back to reading `./network_bagatur.nnue`
at startup — useful when iterating on networks without rebuilding.

## Building

```bash
cmake -B build -G Ninja
cmake --build build --target Bagatur.cpp-x64 --config Release
```

The resulting `build/Bagatur.cpp-x64.exe` (or `.../Bagatur.cpp-x64` on
POSIX) is the only artifact you need to ship.

## Smoke test

Manual handshake from a terminal:

```text
uci
id name Bagatur.cpp 0.1
id author Krasimir Topchiyski (Java original) / C++ port
uciok
isready
readyok
ucinewgame
position startpos moves e2e4 e7e5
go movetime 1000
info depth 1 ... pv g1f3
...
bestmove g1f3
quit
```

## Plugging into cutechess-cli

```bash
cutechess-cli \
  -engine cmd=./Bagatur.cpp-x64 name=Bagatur.cpp \
  -engine cmd=./bagatur.jar      name=BagaturJava proto=uci \
  -each tc=40/60+0.6 \
  -games 100 -concurrency 4 \
  -pgnout match.pgn
```

Set options per engine in cutechess with, e.g.,
`option.ThreadsCount=4 option.TTSize=256`; any other `setoption` is silently
accepted (per the UCI spec). For Chess960 matches cutechess sends
`setoption name UCI_Chess960 value true` automatically when the variant is
`fischerandom`, so castling moves round-trip as king-takes-rook.
