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

# ------------------------------------------------------------------ binaries
add_executable(perft     src/perft/perft.cpp)
target_link_libraries(perft     PRIVATE bagatur)

add_executable(benchmark src/benchmark/benchmark.cpp)
target_link_libraries(benchmark PRIVATE bagatur)
