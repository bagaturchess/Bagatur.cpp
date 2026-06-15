# nnue

C++20 port of `bagaturchess.deeplearning.impl_nnue_v3.NNUEEvaluator` and its
underlying `bagaturchess.nnue_v2.NNUE` network probe (~1300 lines of Java,
mostly Java Vector API SIMD code).

## What's in here

| File                  | Purpose                                                |
| --------------------- | ------------------------------------------------------ |
| `nnue.h`              | Public `Evaluator`, `Network`, `Accumulator` API       |
| `nnue.cpp`            | AVX2 hot path: L1 add/sub/add-sub, SCReLU + L2 dot     |
| `perft_eval.cpp`      | Perft that runs `evaluate()` at every node (correctness + NPS) |
| `benchmark_eval.cpp`  | NPS benchmark on a mix of mid-game positions           |

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

## SIMD

Three hot-path implementations are present in `nnue.cpp`; the build picks one
at compile time based on the standard `__AVX512BW__` / `__AVX2__` predefines:

| ISA      | Lane width | When chosen                                  |
| -------- | :--------: | -------------------------------------------- |
| AVX-512  | 32 int16   | `__AVX512F__` + `__AVX512BW__` defined       |
| AVX-2    | 16 int16   | `__AVX2__` defined                           |
| scalar   | 1          | neither — portable fallback for verification |

The L1 add/sub/add-sub kernels process 64 (AVX2) or 128 (AVX-512) lanes per
loop iteration. The SCReLU + L2 dot kernel widens int16 → int32 via the
respective `cvtepi16_epi32`, squares with `mullo_epi32`, and multiply-
accumulates against int32 weights. The AVX-512 path uses `_mm512_reduce_add_epi32`
for the horizontal sum.

### Enabling AVX-512

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBAGATUR_AVX512=ON
cmake --build build -j
```

Or directly with g++:

```bash
g++ -std=c++17 -O3 -march=native -mavx512f -mavx512bw -mavx2 -mfma -flto ...
```

**Caveat:** an AVX-512 build crashes with `SIGILL` if executed on a CPU that
does not implement AVX-512F + AVX-512BW. Target CPUs:

- AMD Zen 4 / Zen 5 (Ryzen 7000 / 9000)
- Intel server: Skylake-X, Cascade Lake, Ice Lake-SP, Sapphire Rapids
- Intel desktop: Ice Lake / Tiger Lake mobile; **disabled on Alder/Raptor Lake**

For mixed deployments, leave `BAGATUR_AVX512=OFF` (default) — the AVX2 path
runs on everything from Intel Haswell (2013) / AMD Excavator (2015) onwards.

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

Or directly with g++:

```bash
g++ -std=c++17 -O3 -march=native -mavx2 -flto \
    src/board/*.cpp src/nnue/nnue.cpp src/nnue/perft_eval.cpp \
    -o perft_eval
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
