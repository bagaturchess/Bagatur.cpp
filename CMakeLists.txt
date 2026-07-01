cmake_minimum_required(VERSION 3.20)
project(BagaturCpp LANGUAGES CXX C)

# ------------------------------------------------------------------ language
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ------------------------------------------------------------------ flags
if (MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc)
    add_compile_options(/O2 /Oi /Ot /GS- /GL /fp:fast)
    add_link_options(/LTCG)
    # BMI2 / POPCNT
    add_compile_options(/arch:AVX2)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(-O3 -fno-exceptions -fno-rtti)
    # Portable ISA floor: Haswell (2013) / AMD Excavator+ / Zen+ — AVX2 + FMA +
    # BMI2 (pext for magic bitboards) + POPCNT, but NO AVX-512. `-march=native`
    # bakes in whatever the BUILD machine happens to have (e.g. AVX-512 on a
    # Tiger Lake box), which then faults with an illegal instruction on the many
    # CPUs without it (all AMD Zen 1–3, Intel 12–14 gen consumer, anything pre
    # ~2017). GCC 8.1 predates the `-march=x86-64-v3` alias, so `haswell` is the
    # equivalent named floor.
    add_compile_options(-march=haswell -mbmi -mbmi2 -mpopcnt)
    add_compile_options(-fno-trapping-math -fno-math-errno)
    # Statically link the C/C++/winpthread runtimes so the shipped exe needs no
    # MinGW DLLs (libstdc++-6, libgcc_s_seh-1, libwinpthread-1) — a target box
    # only needs the always-present Windows system DLLs (KERNEL32, msvcrt).
    add_link_options(-static -static-libgcc -static-libstdc++)

    # LTO is intentionally OFF on this GCC/MinGW toolchain. GCC 8.1's LTO
    # miscompiles the statically-linked libstdc++/libgcc combination above: the
    # binary corrupts and segfaults at startup (a spurious C++ throw during FEN
    # parsing, unwinding through -fno-exceptions frames). A self-contained,
    # portable exe (no MinGW DLLs) is worth more than LTO here, so LTO yields.
    # Non-static builds may re-enable it; a newer GCC (11+) also fixes LTO+static.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_ok)
    if (lto_ok AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# ------------------------------------------------------------------ library
add_library(bagatur STATIC
    src/board/magic.cpp
    src/board/chess_constants.cpp
    src/board/zobrist.cpp
    src/board/castling_config.cpp
    src/board/castling_util.cpp
    src/board/see_util.cpp
    src/board/chess_board.cpp
    src/board/chess_board_util.cpp
    src/board/move_generator.cpp
)
target_include_directories(bagatur PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ------------------------------------------------------------------ nnue lib
# Compile-time SIMD target:
#   default          -> AVX2 (Haswell / Excavator and newer)
#   -DBAGATUR_AVX512 -> AVX-512F + AVX-512BW (Skylake-X / Ice Lake / Zen 4+)
# Built binaries crash with illegal-instruction if run on a CPU that does not
# implement the selected ISA. Pick the lowest common denominator across your
# deployment targets.
option(BAGATUR_AVX512 "Compile NNUE with AVX-512 path (CPU must support AVX512F + AVX512BW)" OFF)

add_library(nnue STATIC src/nnue/nnue.cpp)
target_include_directories(nnue PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nnue PUBLIC bagatur)

# ------------------------------------------------------------------ embedded network
# If network_bagatur.nnue is present in the project root, embed it directly
# into the nnue library as a C array so the engine has no runtime file
# dependency. The generated TU lives under build/generated/ and is added
# to the `nnue` target sources.
set(BAGATUR_NETWORK_FILE "${CMAKE_SOURCE_DIR}/network_bagatur.nnue")
set(BAGATUR_HAS_EMBEDDED_NETWORK FALSE)
if (EXISTS "${BAGATUR_NETWORK_FILE}")
    include("${CMAKE_SOURCE_DIR}/cmake/embed_binary.cmake")
    set(BAGATUR_NETWORK_CPP "${CMAKE_BINARY_DIR}/generated/bagatur_network_embedded.cpp")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
    embed_binary("${BAGATUR_NETWORK_FILE}" "${BAGATUR_NETWORK_CPP}" "kBagaturNetwork")
    set(BAGATUR_HAS_EMBEDDED_NETWORK TRUE)
    message(STATUS "NNUE: network_bagatur.nnue will be embedded into Bagatur.cpp-x64")
else()
    message(STATUS "NNUE: no network_bagatur.nnue in project root — engine will read it from CWD at startup")
endif()

if (BAGATUR_AVX512)
    message(STATUS "NNUE: AVX-512 path enabled")
    if (MSVC)
        target_compile_options(nnue PRIVATE /arch:AVX512)
    else()
        target_compile_options(nnue PRIVATE -mavx512f -mavx512bw -mavx2 -mfma)
    endif()
else()
    if (MSVC)
        target_compile_options(nnue PRIVATE /arch:AVX2)
    else()
        target_compile_options(nnue PRIVATE -mavx2 -mfma)
    endif()
endif()

# ------------------------------------------------------------------ binaries
add_executable(perft     src/board/perft.cpp)
target_link_libraries(perft     PRIVATE bagatur)

add_executable(benchmark src/board/benchmark.cpp)
target_link_libraries(benchmark PRIVATE bagatur)

add_executable(perft_eval src/nnue/perft_eval.cpp)
target_link_libraries(perft_eval PRIVATE nnue)

add_executable(benchmark_eval src/nnue/benchmark_eval.cpp)
target_link_libraries(benchmark_eval PRIVATE nnue)

# ------------------------------------------------------------------ syzygy (Fathom)
# Self-contained Syzygy tablebase prober. tbprobe.c #includes tbchess.c, so it is
# the only C translation unit; syzygy.cpp is the C++ wrapper that the search and
# UCI layers call with a ChessBoard.
add_library(syzygy STATIC
    src/syzygy/tbprobe.c
    src/syzygy/syzygy.cpp
)
target_include_directories(syzygy PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/syzygy   # tbprobe.h does `#include <tbconfig.h>`
)
target_link_libraries(syzygy PUBLIC bagatur)

# ------------------------------------------------------------------ search
add_library(search STATIC
    src/search/tt.cpp
    src/search/mtd.cpp
    src/search/search.cpp
    src/search/smp/smp_searcher.cpp
)
target_include_directories(search PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(search PUBLIC nnue syzygy)

add_executable(search_main src/search/search_main.cpp)
target_link_libraries(search_main PRIVATE search)

# ------------------------------------------------------------------ uci
add_library(uci STATIC
    src/uci/go.cpp
    src/uci/position.cpp
    src/uci/time_control.cpp
    src/uci/uci_move.cpp
    src/uci/state_manager.cpp
)
target_include_directories(uci PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uci PUBLIC search)

# Bagatur.cpp-x64 — the UCI engine binary that plugs into chess GUIs.
# The network bytes are compiled directly into the exe (when the embedded
# network is available) so it stays self-contained — drop it into
# cutechess / arena / chessbase without any companion files.
add_executable(Bagatur.cpp-x64 src/uci/uci_main.cpp)
target_link_libraries(Bagatur.cpp-x64 PRIVATE uci)
if (NOT MSVC)
    target_link_libraries(Bagatur.cpp-x64 PRIVATE pthread)
endif()

if (BAGATUR_HAS_EMBEDDED_NETWORK)
    # Add the generated TU to the executable's own sources. Avoids the
    # static-library scan-order issue where the symbols would be discarded
    # before nnue.cpp's reference is observed. LTO is disabled for this TU —
    # a 16 MB constant array has nothing to optimize and slows the link.
    target_sources(Bagatur.cpp-x64 PRIVATE "${BAGATUR_NETWORK_CPP}")
    set_source_files_properties("${BAGATUR_NETWORK_CPP}"
        PROPERTIES COMPILE_FLAGS "-fno-lto" SKIP_LINTING ON)
    target_compile_definitions(Bagatur.cpp-x64 PRIVATE BAGATUR_EMBEDDED_NETWORK=1)
endif()
