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
    # ISA target for the general (non-NNUE) code. Default `haswell` (2013) /
    # AMD Excavator+ / Zen+ — AVX2 + FMA + BMI2 (pext for magic bitboards) +
    # POPCNT, but NO AVX-512 — the portable floor for the SHIPPED binary. GCC 8.1
    # predates the `-march=x86-64-v3` alias, so `haswell` is the equivalent floor.
    #
    # NOTE: the NNUE hot loops do NOT depend on this — they ship as separate
    # per-ISA kernels and pick AVX-512 at RUNTIME on capable CPUs (see the nnue
    # lib below), so the default haswell binary is already portable AND fast.
    #
    # `-DBAGATUR_MARCH=native` is only an optional private tweak: it lets the
    # compiler use host extensions + tuning for the SEARCH / MOVEGEN code too. It
    # then FAULTS with an illegal instruction on any CPU lacking those extensions
    # — never distribute a native build.
    set(BAGATUR_MARCH "haswell" CACHE STRING
        "ISA for -march on general code: 'haswell' = portable floor (ship this); 'native' = fastest on THIS host, not portable")
    add_compile_options(-march=${BAGATUR_MARCH} -mbmi -mbmi2 -mpopcnt)
    add_compile_options(-fno-trapping-math -fno-math-errno)
    # Statically link the C/C++/winpthread runtimes so the shipped exe needs no
    # MinGW DLLs (libstdc++-6, libgcc_s_seh-1, libwinpthread-1) — a target box
    # only needs the always-present Windows system DLLs (KERNEL32, msvcrt).
    add_link_options(-static -static-libgcc -static-libstdc++)

    # LTO. Requires a GCC where LTO + static linking is sound: GCC 8.1
    # miscompiled that combination into a startup crash, but a modern MinGW GCC
    # (11+, e.g. the winlibs GCC 16 UCRT toolchain) handles it. gcc-ar /
    # gcc-ranlib keep the LTO bytecode index inside the static archives — plain
    # `ar` silently drops it, which later surfaces as "undefined reference" at
    # the final link step.
    #
    # Measured on this port, LTO buys little for the hot loops (the NNUE SIMD
    # kernels are already isolated TUs reached via function pointer) — the big NPS
    # lever is the ISA of the NNUE kernel (AVX-512 vs AVX2, now runtime-selected),
    # not LTO. Kept ON by default but toggleable so the distribution build can be
    # reproduced or A/B'd without editing this file.
    option(BAGATUR_LTO "Enable link-time optimization (needs GCC 11+ for sound static+LTO)" ON)
    if (BAGATUR_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT lto_ok)
        if (lto_ok)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
            if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                find_program(GCC_AR_BIN     gcc-ar)
                find_program(GCC_RANLIB_BIN gcc-ranlib)
                if (GCC_AR_BIN AND GCC_RANLIB_BIN)
                    set(CMAKE_AR     "${GCC_AR_BIN}"     CACHE FILEPATH "GCC LTO-aware ar"     FORCE)
                    set(CMAKE_RANLIB "${GCC_RANLIB_BIN}" CACHE FILEPATH "GCC LTO-aware ranlib" FORCE)
                endif()
            endif()
        endif()
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
# The NNUE SIMD hot loops ship in THREE per-ISA translation units — AVX-512,
# AVX2 and a scalar fallback — and the engine picks the fastest the running CPU
# supports at startup via CPUID (see src/nnue/nnue_kernels.h). So a single
# distributable runs on any x86-64 box yet still uses AVX-512 where present; no
# separate per-ISA binaries. nnue.cpp and the scalar kernel stay at the global
# portable floor; only the two SIMD kernel files are raised, and each kernel also
# carries a target() attribute so the ISA survives LTO (the per-file -m flags
# alone are not reliably preserved across the link-time recompile).
add_library(nnue STATIC
    src/nnue/nnue.cpp
    src/nnue/nnue_kernels_avx512.cpp
    src/nnue/nnue_kernels_avx2.cpp
    src/nnue/nnue_kernels_scalar.cpp
)
target_include_directories(nnue PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nnue PUBLIC bagatur)

if (MSVC)
    set_source_files_properties(src/nnue/nnue_kernels_avx512.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    set_source_files_properties(src/nnue/nnue_kernels_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
else()
    set_source_files_properties(src/nnue/nnue_kernels_avx512.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx2;-mfma")
    set_source_files_properties(src/nnue/nnue_kernels_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()

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

# Distributable filename carries version + arch only. No ISA tag: the binary
# runtime-dispatches the NNUE kernels from AVX2 up to AVX-512 per CPU
# (src/nnue/nnue_kernels.h), so a single file is both portable and fast — an
# `-avx2`/`-avx512` suffix would misdescribe it. The CMake TARGET stays
# `Bagatur.cpp-x64` (so `--target Bagatur.cpp-x64` and the docs keep working);
# only the produced file is renamed.
set_target_properties(Bagatur.cpp-x64 PROPERTIES
    OUTPUT_NAME "Bagatur.cpp_1.0-x86_64")

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
