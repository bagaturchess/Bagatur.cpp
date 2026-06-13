// Bagatur.cpp — C++ port of bagaturchess.bitboard.impl1
//
// Foundational types: piece codes, colors, bit twiddling primitives.
// Mirrors ChessConstants.java / Util.java / EngineConstants.java.
//
// Square indexing matches the Java source EXACTLY:
//   H1 = 0, G1 = 1, ..., A1 = 7
//   H2 = 8, ..., A2 = 15
//   ...
//   H8 = 56, ..., A8 = 63
// File index (sq & 7): 0 = H, 7 = A.
// Rank index (sq >> 3): 0 = rank 1, 7 = rank 8.

#pragma once

#include <array>
#include <cstdint>

#if defined(__cpp_lib_bitops) || (defined(__cplusplus) && __cplusplus >= 202002L)
#  if __has_include(<bit>)
#    include <bit>
#    define BAGATUR_HAS_STD_BIT 1
#  endif
#endif

#if defined(_MSC_VER)
#  include <intrin.h>
#  define BAGATUR_FORCE_INLINE __forceinline
#else
#  define BAGATUR_FORCE_INLINE inline __attribute__((always_inline))
#endif

namespace board {

using BB = std::uint64_t;
using U32 = std::uint32_t;
using I32 = std::int32_t;
using U16 = std::uint16_t;
using I16 = std::int16_t;
using U8 = std::uint8_t;
using I8 = std::int8_t;

// ---- Piece codes (match ChessConstants.java) -------------------------------
inline constexpr int EMPTY  = 0;
inline constexpr int PAWN   = 1;
inline constexpr int NIGHT  = 2;   // "NIGHT" = knight; Java naming preserved
inline constexpr int BISHOP = 3;
inline constexpr int ROOK   = 4;
inline constexpr int QUEEN  = 5;
inline constexpr int KING   = 6;

inline constexpr int WHITE = 0;
inline constexpr int BLACK = 1;

inline constexpr int SCORE_NOT_RUNNING = 7777;

inline constexpr std::array<int, 2> COLOR_FACTOR   = { 1, -1 };
inline constexpr std::array<int, 2> COLOR_FACTOR_8 = { 8, -8 };

// ---- Engine-level toggles (match EngineConstants.java where relevant) ------
inline constexpr int  MAX_MOVES                = 2048;
inline constexpr int  MAX_PLIES                = 2 * MAX_MOVES;
inline constexpr bool GENERATE_BR_PROMOTIONS   = true;
inline constexpr bool ASSERT_ENABLED           = false;
inline constexpr bool DUMP_CASTLING            = false;
inline constexpr bool IS_FRC                   = false;

// ---- Bit twiddling (Long.numberOfTrailingZeros, lowestOneBit, bitCount) ----
BAGATUR_FORCE_INLINE int trailing_zeros(BB b) noexcept {
    // Java Long.numberOfTrailingZeros(0) returns 64.
#if defined(BAGATUR_HAS_STD_BIT)
    return b ? std::countr_zero(b) : 64;
#elif defined(_MSC_VER)
    unsigned long idx;
    return _BitScanForward64(&idx, b) ? static_cast<int>(idx) : 64;
#else
    return b ? __builtin_ctzll(b) : 64;
#endif
}

BAGATUR_FORCE_INLINE int popcount(BB b) noexcept {
#if defined(BAGATUR_HAS_STD_BIT)
    return std::popcount(b);
#elif defined(_MSC_VER)
    return static_cast<int>(__popcnt64(b));
#else
    return __builtin_popcountll(b);
#endif
}

BAGATUR_FORCE_INLINE BB lowest_bit(BB b) noexcept {
    // Equivalent of Long.lowestOneBit: isolate LSB.
    return b & static_cast<BB>(-static_cast<std::int64_t>(b));
}

BAGATUR_FORCE_INLINE BB pop_lsb(BB& b) noexcept {
    BB lsb = lowest_bit(b);
    b &= (b - 1);
    return lsb;
}

// ---- Util.POWER_LOOKUP equivalent -----------------------------------------
// In hot paths we use `1ULL << sq` directly (single shift, same code-gen as
// the Java POWER_LOOKUP[] array load). The array is exposed for fidelity:
inline constexpr std::array<BB, 64> POWER_LOOKUP = [] {
    std::array<BB, 64> a{};
    for (int i = 0; i < 64; ++i) a[i] = 1ULL << i;
    return a;
}();

BAGATUR_FORCE_INLINE constexpr BB sq_bb(int sq) noexcept {
    return 1ULL << sq;
}

BAGATUR_FORCE_INLINE constexpr int sq_file(int sq) noexcept {  // 0=H, 7=A
    return sq & 7;
}

BAGATUR_FORCE_INLINE constexpr int sq_rank(int sq) noexcept {  // 0..7
    return sq >> 3;
}

BAGATUR_FORCE_INLINE constexpr int sq_distance(int a, int b) noexcept {
    int rd = (a >> 3) - (b >> 3);
    int fd = (a & 7) - (b & 7);
    if (rd < 0) rd = -rd;
    if (fd < 0) fd = -fd;
    return rd > fd ? rd : fd;
}

BAGATUR_FORCE_INLINE constexpr BB mirror_horizontal(BB b) noexcept {
    constexpr BB k1 = 0x5555555555555555ULL;
    constexpr BB k2 = 0x3333333333333333ULL;
    constexpr BB k4 = 0x0f0f0f0f0f0f0f0fULL;
    b = ((b >> 1) & k1) | ((b & k1) << 1);
    b = ((b >> 2) & k2) | ((b & k2) << 2);
    b = ((b >> 4) & k4) | ((b & k4) << 4);
    return b;
}

BAGATUR_FORCE_INLINE BB mirror_vertical(BB b) noexcept {
#if defined(__cpp_lib_byteswap)
    return std::byteswap(b);
#elif defined(_MSC_VER)
    return _byteswap_uint64(b);
#else
    return __builtin_bswap64(b);
#endif
}

BAGATUR_FORCE_INLINE constexpr int flip_horizontal_index(int sq) noexcept {
    return (sq & 0xF8) | (7 - (sq & 7));
}

}  // namespace board
