# Bagatur.cpp

C++20 port of the [Bagatur chess engine](https://github.com/topce/Bagatur).

| Component                          | Mirrors Java                                            |
| ---------------------------------- | ------------------------------------------------------- |
| [src/board](src/board/README.md)   | `bagaturchess.bitboard.impl1` (board + move generation) |
| [src/nnue](src/nnue/README.md)     | `bagaturchess.nnue_v2` + `impl_nnue_v3.NNUEEvaluator`   |
| [src/search](src/search/README.md) | `bagaturchess.search.impl.alg.impl1.Search_PVS_NWS`     |

`network_bagatur.nnue` is expected at the project root and is auto-copied
next to the eval binaries by CMake on build.
