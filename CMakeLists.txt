cmake_minimum_required(VERSION 3.20)
project(BagaturCpp LANGUAGES CXX)

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
    add_compile_options(-march=native -mbmi -mbmi2 -mpopcnt)
    add_compile_options(-fno-trapping-math -fno-math-errno)
    # LTO
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_ok)
    if (lto_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        # GCC LTO needs the wrapper ar / ranlib to keep static archives
        # readable by the LTO linker plugin. Plain `ar` silently drops the
        # bytecode index, which surfaces later as "undefined reference" at
        # the final link step.
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

# ------------------------------------------------------------------ search
add_library(search STATIC
    src/search/tt.cpp
    src/search/mtd.cpp
    src/search/search.cpp
    src/search/smp/smp_searcher.cpp
)
target_include_directories(search PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(search PUBLIC nnue)

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
