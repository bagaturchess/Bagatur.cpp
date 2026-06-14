# Bagatur.cpp

C++20 port of the **Board** subsystem of the [Bagatur chess engine](https://github.com/topce/Bagatur) (`bagaturchess.bitboard.impl1`). Goal: maximise NPS (nodes per second) while preserving the original logic 1:1 so that future Search / NNUE work can plug into the same primitives.

## Scope

Ported (`bagaturchess.bitboard.impl1.internal`):

| Java class            | C++ counterpart                        |
| --------------------- | -------------------------------------- |
| `ChessBoard`          | `bagatur::ChessBoard`                  |
| `MoveGenerator`       | `bagatur::MoveGenerator`               |
| `MagicUtil`           | `bagatur::magic::*`                    |
| `Bitboard`            | `bagatur::bb::*`                       |
| `StaticMoves`         | `bagatur::static_moves::*`             |
| `ChessConstants`      | `bagatur::cc::*` + `bagatur::*`        |
| `Zobrist`             | `bagatur::zob::*`                      |
| `MoveUtil`            | `bagatur::mv::*`                       |
| `MaterialUtil`        | `bagatur::material::*`                 |
| `CheckUtil`           | `bagatur::check::*`                    |
| `SEEUtil`             | `bagatur::see::*`                      |
| `CastlingConfig/Util` | `bagatur::CastlingConfig` / `castling::*` |
| `ChessBoardUtil`      | `bagatur::cbu::*`                      |
| `StackLongInt`        | `bagatur::RepetitionTable`             |

**Not ported** (out of scope for board-only NPS):

- `BoardImpl`, `IBitBoard`, `IBoard` — these are the API wrappers over `ChessBoard`; porting them requires porting the entire `bagaturchess.bitboard.api` package.
- `EvalConstants` PSQT data — only the structure is kept; the tables are zero-filled so the delta-update code paths stay live. Drop in real data without code changes.
- `MoveGenerator.setHHScores` / `setRootScores` — depend on `IHistoryProvider` (Search-side concern).
- `MoveWrapper` — UCI string formatting; trivial to add later.
- `NNUE_Input`, `BaseEvaluation` — evaluation.

## Performance choices

- C++20, `-O3 -march=native` with BMI2 / POPCNT.
- All hot-path helpers (`magic::*_moves`, `check::is_in_check_super`, `mv::*`, `material::*`) are `inline` and `BAGATUR_FORCE_INLINE`-annotated.
- `std::countr_zero`, `std::popcount`, `std::byteswap` — they map to `TZCNT`, `POPCNT` and `BSWAP`.
- Lookup tables that the compiler can build at compile time (knight moves, king moves, pawn attacks, square bitmasks) are `constexpr` and live in `.rodata`.
- Magic-bitboard tables are flat-allocated as a single contiguous `std::vector<BB>` — one allocation, no pointer-chasing inside the lookup.
- `RepetitionTable` replaces Java's chained-hashing `StackLongInt` with an open-addressed table + backward-shift deletion (no allocations after construction, much less pointer chasing).
- History arrays are zero-initialised once and live in the same `ChessBoard` object so do/undo move never touches the allocator.
- `DUMP_CASTLING`, `ASSERT`, `Properties.DEBUG_MODE` — compile-time `false`, all dead branches deleted by the optimiser.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces two binaries under `build/`:

- `perft`     — correctness check against six well-known reference positions (startpos, Kiwipete, Steven Edwards, etc.)
- `benchmark` — NPS measurement on a fixed mix of deep perft runs

```bash
./build/perft
./build/benchmark
./build/perft single "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" 6
```

## Layout

```
src/board/
  types.h               primitives + force-inline macro
  bitboard.h            named square / file / rank bitmasks
  static_moves.h        knight, king, pawn-attack tables (constexpr)
  magic.{h,cpp}         magic-bitboard sliding moves
  chess_constants.{h,cpp}  IN_BETWEEN, PINNED_MOVEMENT, KING_AREA
  zobrist.{h,cpp}       Zobrist random table (matches Java byte-for-byte)
  move_util.h           move encoding helpers (all inline)
  material_util.h       material-key bit packing
  eval_constants.h      PHASE / MATERIAL_SEE / zero PSQT
  castling_{config,util}.{h,cpp}   FRC-aware castling
  check_util.h          check / attacker queries (inline)
  see_util.{h,cpp}      SEE
  repetition.h          RepetitionTable (replaces StackLongInt)
  chess_board.{h,cpp}   position state, do/undo move
  chess_board_util.{h,cpp}  FEN parser, init, toString
  move_generator.{h,cpp}    pseudo-legal generation
src/perft/perft.cpp      correctness driver
src/benchmark/benchmark.cpp  NPS driver
```

## Measured throughput

Built with `g++ 8.1 -O3 -march=native -flto -std=c++17`, MinGW on Windows. All six standard perft positions verify against the reference node counts:

| Position    | Depth | Nodes         | Time    | NPS       |
| ----------- | :---: | ------------: | ------: | --------: |
| startpos    |   5   |     4 865 609 |  0.17 s | 28.7 Mnps |
| startpos    |   6   |   119 060 324 |  5.01 s | 23.8 Mnps |
| Kiwipete    |   5   |   193 690 690 |  9.20 s | 21.1 Mnps |
| Position 3  |   6   |    11 030 083 |  0.59 s | 18.7 Mnps |
| Position 4  |   5   |    15 833 292 |  0.76 s | 20.8 Mnps |
| Position 5  |   5   |    89 941 194 |  4.61 s | 19.5 Mnps |
| Position 6  |   5   |   164 075 551 |  8.59 s | 19.1 Mnps |

Total: ~598 M nodes in ~29 s → **≈ 20.6 Mnps** average across the test mix.

The Java baseline on the same machine sits in the 5–15 Mnps band, so the port delivers roughly **1.5–4× more raw board throughput** depending on the position. Most of the gain comes from:

1. `inline` magic-bitboard lookups (no JVM call frames).
2. Hot-path bit twiddling lowering to `TZCNT` / `POPCNT` directly.
3. The flat magic-move storage — every lookup is two dependent loads from contiguous memory instead of an array-of-arrays dereference.
4. Compile-time deletion of all dead branches gated by `DUMP_CASTLING`, `ASSERT`, `Properties.DEBUG_MODE`, `EngineConstants.GENERATE_BR_PROMOTIONS`.
5. `RepetitionTable` open-addressed lookups in place of `StackLongInt`'s chained-hashing pointer walk.

## Square layout

Inherited from the Java source verbatim:

```
H1 = 0  G1 = 1  ... A1 = 7
H2 = 8  ...           A2 = 15
...
H8 = 56 ...           A8 = 63
```

`sq & 7` is file index where 0 = H, 7 = A. `sq >> 3` is rank index (0 = rank 1).
