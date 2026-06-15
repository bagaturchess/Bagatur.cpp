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

Hot path uses AVX2 intrinsics (`<immintrin.h>`). The L1 add/sub kernels
process 64 lanes per loop iteration (`_mm256_add_epi16` × 4); the SCReLU/L2
kernel widens int16 → int32 with `_mm256_cvtepi16_epi32`, squares with
`_mm256_mullo_epi32`, and multiply-accumulates against int32 weights.

A scalar fallback exists for non-AVX2 targets but is significantly slower —
the network is sized for SIMD.

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

`g++ -O3 -march=native -mavx2 -flto`, eval at every visited node:

| Position    | Depth | Nodes        | Time     | NPS         |
| ----------- | :---: | -----------: | -------: | ----------: |
| startpos    |   5   |    4 865 609 |  6.20 s  | 0.79 Mnps   |
| Kiwipete    |   4   |    4 085 603 |  6.34 s  | 0.64 Mnps   |
| Position 3  |   5   |      674 624 |  0.82 s  | 0.82 Mnps   |
| Position 4  |   4   |      422 333 |  0.67 s  | 0.63 Mnps   |
| Position 5  |   4   |    2 103 487 |  2.87 s  | 0.73 Mnps   |

For reference, the plain `board::` perft (no eval) on the same mix runs at
≈ 20 Mnps, so the eval-per-node cost is ~25–35× the move-generation cost.
That ratio is dominated by SCReLU + L2 dot product on a 2 × 1536-wide
accumulator — there is no straightforward path to reduce it without
changing the network shape.
