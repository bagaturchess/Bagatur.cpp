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

# ------------------------------------------------------------------ network
# If network_bagatur.nnue is present in the project root, copy it next to
# the perft_eval/benchmark_eval binaries so they find it without `cd`.
if (EXISTS "${CMAKE_SOURCE_DIR}/network_bagatur.nnue")
    foreach(tgt perft_eval benchmark_eval)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/network_bagatur.nnue"
                "$<TARGET_FILE_DIR:${tgt}>/network_bagatur.nnue")
    endforeach()
endif()
