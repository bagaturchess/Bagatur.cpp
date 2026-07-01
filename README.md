# Bagatur.cpp

C++20 port of the [Bagatur chess engine](https://github.com/topce/Bagatur).

| Component                          | Mirrors Java                                            |
| ---------------------------------- | ------------------------------------------------------- |
| [src/board](src/board/README.md)   | `bagaturchess.bitboard.impl1` (board + move generation) |
| [src/nnue](src/nnue/README.md)     | `bagaturchess.nnue_v2` + `impl_nnue_v3.NNUEEvaluator`   |
| [src/search](src/search/README.md) | `bagaturchess.search.impl.alg.impl1.Search_PVS_NWS`     |
| [src/uci](src/uci/README.md)        | `bagaturchess.uci.impl` (UCI protocol driver)           |
| [src/syzygy](src/syzygy/README.md) | `bagaturchess.egtb.syzygy` (Fathom tablebase prober)    |

`network_bagatur.nnue` (project root) is **embedded into `Bagatur.cpp-x64`**
at build time via `cmake/embed_binary.cmake`, so the UCI engine ships as a
single self-contained exe — drop it into any GUI with no companion file. The
helper binaries (`search_main`, `perft_eval`, `benchmark_eval`) do **not**
embed it; they read `network_bagatur.nnue` from the current working directory
at startup. If the network is absent at configure time, even the engine falls
back to reading it from the working directory.

Syzygy endgame tablebase support ([src/syzygy](src/syzygy/README.md)) is
built in but **off by default** — the engine only probes once the `SyzygyPath`
UCI option points at a directory of `.rtbw`/`.rtbz` files (a separate download).
The prober itself (Fathom) is vendored in-tree, so the build stays
dependency-free.

## Building

Plain C++20 with **no external dependencies** — only the standard library. The
NNUE network ships in the repo and is embedded at build time, so a build
produces a single self-contained engine binary.

**Requirements**

- A C++20 compiler — GCC, Clang, or MSVC.
- CMake ≥ 3.20.
- An x86-64 CPU with **AVX2 + BMI2 + POPCNT** (Haswell / Excavator and newer) —
  the default ISA target.

**Build**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Bagatur.cpp-x64
```

The result, `build/Bagatur.cpp-x64[.exe]`, is the UCI engine — drop it into any
GUI. Omit `--target` to build every target. Pick a generator with `-G` if you
like (e.g. `-G Ninja`); CMake otherwise auto-selects one (Make / MSBuild).

**What gets compiled** (CMake targets)

| Target                                | Kind | What it is                                              |
| ------------------------------------- | ---- | ------------------------------------------------------- |
| `Bagatur.cpp-x64`                     | exe  | UCI engine, network embedded — the artifact to ship    |
| `search_main`                         | exe  | CLI search driver (reads `network_bagatur.nnue` from CWD) |
| `perft` / `perft_eval`                | exe  | move-generation / evaluation perft correctness         |
| `benchmark` / `benchmark_eval`        | exe  | speed benchmarks                                       |
| `bagatur` / `nnue` / `syzygy` / `search` / `uci` | lib | static libraries the executables link            |

**Optimisation flags** (set by CMake in a Release build)

- GCC / Clang: `-O3 -march=native -mbmi -mbmi2 -mpopcnt -fno-exceptions
  -fno-rtti -fno-trapping-math -fno-math-errno`, plus LTO where supported.
- MSVC: `/O2 /Oi /Ot /GS- /GL /fp:fast /arch:AVX2 /LTCG`.

`-march=native` tunes the binary to the **build machine's** CPU. Rebuild on the
target host (or drop `-march=native`) if you ship to different hardware, or the
engine may hit an illegal-instruction fault on an older CPU.

**AVX-512** — off by default (the NNUE uses an AVX2 path). Configure with
`-DBAGATUR_AVX512=ON` to compile the AVX-512 NNUE path; the CPU must then
support AVX512F + AVX512BW (Skylake-X / Ice Lake / Zen 4+).
