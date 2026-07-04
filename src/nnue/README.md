# nnue

C++20 port of `bagaturchess.deeplearning.impl_nnue_v3.NNUEEvaluator` and its
underlying `bagaturchess.nnue_v2.NNUE` network probe (~1300 lines of Java,
mostly Java Vector API SIMD code).

## What's in here

| File                        | Purpose                                                |
| --------------------------- | ------------------------------------------------------ |
| `nnue.h`                    | Public `Evaluator`, `Network`, `Accumulator` API       |
| `nnue.cpp`                  | Evaluator: refresh, incremental update, `evaluate()`; calls the SIMD kernels via function pointers |
| `nnue_kernels.h`            | `Kernels` dispatch table + `active()` (CPUID) selector |
| `nnue_kernels_avx512.cpp`   | AVX-512F+BW kernels (L1 add/sub/add-sub, SCReLU+L2 dot) |
| `nnue_kernels_avx2.cpp`     | AVX2 kernels — the shipped floor                       |
| `nnue_kernels_scalar.cpp`   | Scalar kernels + the runtime CPU dispatcher            |
| `perft_eval.cpp`            | Perft that runs `evaluate()` at every node (correctness + NPS) |
| `benchmark_eval.cpp`        | NPS benchmark on a mix of mid-game positions           |

## Network architecture (matches `nnue_v2.NNUE` 1:1)

- `INPUT_BUCKET_SIZE = 7`  — king-square buckets (mirrors black side)
- `FEATURE_SIZE     = 768` — (12 piece-classes × 64 squares) per perspective
- `HIDDEN_SIZE      = 1536` — accumulator size
- `OUTPUT_BUCKETS   = 8`   — piece-count buckets, `(count - 2) / 4`
- `SCALE / QA / QB  = 400 / 255 / 64`

Activation: SCReLU = `clamp(x, 0, QA)²`.

Total network size: ~16.5 MB (read from `./network_bagatur.nnue` next to the
exe on first `Evaluator` construction).

## Square-layout note

Bagatur's board uses `H1 = 0, A1 = 7` (file index reversed); Bullet-style NNUE
expects `A1 = 0, H1 = 7`. Translation is a single bit flip — `sq ^ 7`. For the
black perspective we vertical-flip on top of that — `sq ^ 0b111000`.

## API

```cpp
#include "nnue/nnue.h"
using namespace nnue;

Evaluator ev;
ev.reset(board);                      // walk the board, fill both perspectives

for (...) {
    int side_moved = board.colorToMove;
    board.doMove(m);
    ev.after_make(board, m, side_moved); // O(1) — applies incremental diff
    int score = ev.evaluate(board);      // side-to-move PoV
    ev.after_unmake(board, m, side_moved);
    board.undoMove(m);
}
```

Incremental updates are mandatory for any kind of perf:

- Quiet, capture, promotion → diff in-place (1–3 vector ops per perspective).
- Castling / EP / king crosses input-bucket boundary → full refresh, with a
  6 KB accumulator snapshot saved for `after_unmake`.

## SIMD — runtime CPU dispatch

The four hot-path primitives — accumulator `v_add` / `v_sub` / `v_add_sub` and the
SCReLU + L2 `v_screlu_dot` — ship in **three per-ISA translation units**, and the
engine selects the fastest the running CPU supports **at startup via CPUID**
(`nnue::kernels::active`). One distributable binary therefore runs on any x86-64
machine yet still uses AVX-512 where present — no per-ISA builds, no `SIGILL`.

| File                      | ISA     | Lane width | Selected when …                          |
| ------------------------- | ------- | :--------: | ---------------------------------------- |
| `nnue_kernels_avx512.cpp` | AVX-512 | 32 int16   | CPU has AVX-512F + AVX-512BW (OS-enabled) |
| `nnue_kernels_avx2.cpp`   | AVX2    | 16 int16   | CPU has AVX2 — the shipped floor          |
| `nnue_kernels_scalar.cpp` | scalar  | 1          | neither — portable reference              |

Each kernel is built at its own ISA level (per-file `COMPILE_OPTIONS` in
CMakeLists) **and** carries a `target()` attribute so the ISA survives LTO (the
command-line `-mavx512*` flags alone are not preserved across the link-time
recompile). `nnue.cpp` calls the selected set through function pointers; because
every primitive sweeps the full 1536-lane hidden layer, the single indirect call
per primitive is amortized to noise.

The L1 add/sub/add-sub kernels process 64 (AVX2) or 128 (AVX-512) lanes per loop
iteration. The SCReLU + L2 dot kernel widens int16 → int32 via the respective
`cvtepi16_epi32`, squares with `mullo_epi32`, and multiply-accumulates against
int32 weights; the AVX-512 path reduces with `_mm512_reduce_add_epi32`.

### Which path am I on?

The engine prints the selected set once after `uciok`:

```text
info string NNUE SIMD: AVX-512
```

To force a path — to exercise all three in one binary, or as a fallback if a host
mis-detects — set an environment variable before launching (`avx512`/`avx2` trust
the caller; forcing an ISA the CPU lacks faults exactly like a native build):

```bash
BAGATUR_SIMD=avx2 ./Bagatur.cpp_1.0-x86_64
```

Measured on an AVX-512 host, the AVX-512 kernel runs ~15–20 % more NPS than AVX2
on the same search tree; on a CPU without AVX-512 the AVX2 path is chosen
automatically and runs on everything from Intel Haswell (2013) / AMD Excavator
(2015) onward.

## Building

CMake (preferred):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# copy the network next to the binaries
cp <wherever-you-keep>/network_bagatur.nnue build/Release/
./build/Release/perft_eval
./build/Release/benchmark_eval
```

Or directly with g++ — the two SIMD kernels compile at their own ISA (each also
carries a `target()` attribute, so this stays correct under `-flto`); everything
else stays at the portable floor:

```bash
g++ -std=c++20 -O3 -march=haswell -mavx512f -mavx512bw -mavx2 -mfma -flto \
    -c src/nnue/nnue_kernels_avx512.cpp -o k_avx512.o
g++ -std=c++20 -O3 -march=haswell -mavx2 -mfma -flto \
    -c src/nnue/nnue_kernels_avx2.cpp -o k_avx2.o
g++ -std=c++20 -O3 -march=haswell -flto \
    -c src/nnue/nnue_kernels_scalar.cpp -o k_scalar.o
g++ -std=c++20 -O3 -march=haswell -flto \
    src/board/*.cpp src/nnue/nnue.cpp src/nnue/perft_eval.cpp \
    k_avx512.o k_avx2.o k_scalar.o -o perft_eval
```

## Measured throughput

Eval at every visited node, `g++ -O3 -march=native -flto`:

| Position    | Depth | Nodes        | AVX2 NPS  | AVX-512 NPS |
| ----------- | :---: | -----------: | --------: | ----------: |
| startpos    |   4   |      197 281 | 1.12 Mnps | 1.24 Mnps   |
| startpos    |   5   |    4 865 609 | 1.08 Mnps | 1.17 Mnps   |
| Kiwipete    |   4   |    4 085 603 | 0.92 Mnps | 0.97 Mnps   |
| Position 3  |   5   |      674 624 | 1.11 Mnps | 1.22 Mnps   |
| Position 4  |   4   |      422 333 | 0.90 Mnps | 0.93 Mnps   |
| Position 5  |   4   |    2 103 487 | 1.03 Mnps | 1.05 Mnps   |

AVX-512 gains ~6-10% across the mix on this machine. The win is smaller than
the theoretical 2× because make/unmake is partially memory-bound on L1 weight
reads (one `add_sub` reads ~6 KB of weights, which fits in L2 but is still
the latency floor). The SCReLU + L2 dot loop, where AVX-512 helps most,
contributes ~30-40% of total time, so a 1.5× win there shows up as ~10%
end-to-end.

For reference, the plain `board::` perft (no eval) on the same mix runs at
≈ 20 Mnps, so the eval-per-node cost is ~25–35× the move-generation cost.
That ratio is dominated by SCReLU + L2 dot product on a 2 × 1536-wide
accumulator — there is no straightforward path to reduce it without
changing the network shape.
