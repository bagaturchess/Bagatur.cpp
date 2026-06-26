# Bagatur.cpp

C++20 port of the [Bagatur chess engine](https://github.com/topce/Bagatur).

| Component                          | Mirrors Java                                            |
| ---------------------------------- | ------------------------------------------------------- |
| [src/board](src/board/README.md)   | `bagaturchess.bitboard.impl1` (board + move generation) |
| [src/nnue](src/nnue/README.md)     | `bagaturchess.nnue_v2` + `impl_nnue_v3.NNUEEvaluator`   |
| [src/search](src/search/README.md) | `bagaturchess.search.impl.alg.impl1.Search_PVS_NWS`     |
| [src/uci](src/uci/README.md)        | `bagaturchess.uci.impl` (UCI protocol driver)           |

`network_bagatur.nnue` (project root) is **embedded into `Bagatur.cpp-x64`**
at build time via `cmake/embed_binary.cmake`, so the UCI engine ships as a
single self-contained exe — drop it into any GUI with no companion file. The
helper binaries (`search_main`, `perft_eval`, `benchmark_eval`) do **not**
embed it; they read `network_bagatur.nnue` from the current working directory
at startup. If the network is absent at configure time, even the engine falls
back to reading it from the working directory.
