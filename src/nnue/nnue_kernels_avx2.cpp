// nnue_kernels_avx2.cpp — AVX2 implementation of the four NNUE SIMD primitives.
// Built with `-mavx2 -mfma` (see CMakeLists); each function carries
// target("avx2") so the ISA survives LTO. This is the path taken on any x86-64
// CPU from ~2013 on (Haswell / Excavator / Zen+) that lacks AVX-512.
//
// 16 int16 lanes per __m256i. HIDDEN_SIZE = 1536 = 96 vectors; the add/sub
// sweeps unroll 4× (= 64 lanes per iteration).
#include "nnue_kernels.h"

#include "nnue.h"  // HIDDEN_SIZE, QA

#include <immintrin.h>

namespace nnue::kernels {
namespace {

constexpr int HS = HIDDEN_SIZE;

NNUE_TARGET("avx2")
void v_add(std::int16_t* __restrict v, const std::int16_t* __restrict w) {
    for (int i = 0; i < HS; i += 16 * 4) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 16));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 32));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 48));
        v0 = _mm256_add_epi16(v0, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i)));
        v1 = _mm256_add_epi16(v1, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 16)));
        v2 = _mm256_add_epi16(v2, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 32)));
        v3 = _mm256_add_epi16(v3, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 48)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i),       v0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 16),  v1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 32),  v2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 48),  v3);
    }
}

NNUE_TARGET("avx2")
void v_sub(std::int16_t* __restrict v, const std::int16_t* __restrict w) {
    for (int i = 0; i < HS; i += 16 * 4) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 16));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 32));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 48));
        v0 = _mm256_sub_epi16(v0, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i)));
        v1 = _mm256_sub_epi16(v1, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 16)));
        v2 = _mm256_sub_epi16(v2, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 32)));
        v3 = _mm256_sub_epi16(v3, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i + 48)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i),       v0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 16),  v1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 32),  v2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 48),  v3);
    }
}

NNUE_TARGET("avx2")
void v_add_sub(std::int16_t* __restrict v,
               const std::int16_t* __restrict w_add,
               const std::int16_t* __restrict w_sub) {
    for (int i = 0; i < HS; i += 16 * 4) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 16));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 32));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i + 48));
        v0 = _mm256_add_epi16(v0, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_add + i)));
        v1 = _mm256_add_epi16(v1, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_add + i + 16)));
        v2 = _mm256_add_epi16(v2, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_add + i + 32)));
        v3 = _mm256_add_epi16(v3, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_add + i + 48)));
        v0 = _mm256_sub_epi16(v0, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_sub + i)));
        v1 = _mm256_sub_epi16(v1, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_sub + i + 16)));
        v2 = _mm256_sub_epi16(v2, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_sub + i + 32)));
        v3 = _mm256_sub_epi16(v3, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_sub + i + 48)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i),       v0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 16),  v1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 32),  v2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v + i + 48),  v3);
    }
}

// SCReLU + dot product. Returns the sum across HIDDEN_SIZE for one accumulator.
NNUE_TARGET("avx2")
std::int64_t v_screlu_dot(const std::int16_t* __restrict acc,
                          const std::int32_t* __restrict weights) {
    const __m256i lo = _mm256_setzero_si256();
    const __m256i hi = _mm256_set1_epi16(static_cast<std::int16_t>(QA));

    __m256i s0 = _mm256_setzero_si256();
    __m256i s1 = _mm256_setzero_si256();

    for (int i = 0; i < HS; i += 16) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + i));
        v = _mm256_max_epi16(v, lo);
        v = _mm256_min_epi16(v, hi);

        // Widen int16 -> int32 in two halves.
        __m128i v_lo = _mm256_castsi256_si128(v);
        __m128i v_hi = _mm256_extracti128_si256(v, 1);
        __m256i a0   = _mm256_cvtepi16_epi32(v_lo);
        __m256i a1   = _mm256_cvtepi16_epi32(v_hi);

        // Square (max 255^2 = 65025, fits in int32).
        __m256i sq0 = _mm256_mullo_epi32(a0, a0);
        __m256i sq1 = _mm256_mullo_epi32(a1, a1);

        __m256i w0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));
        __m256i w1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i + 8));

        s0 = _mm256_add_epi32(s0, _mm256_mullo_epi32(sq0, w0));
        s1 = _mm256_add_epi32(s1, _mm256_mullo_epi32(sq1, w1));
    }

    __m256i s = _mm256_add_epi32(s0, s1);

    // Horizontal reduce to int64 to avoid int32 overflow if the network grows.
    std::int32_t tmp[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(tmp), s);
    std::int64_t total = 0;
    for (int k = 0; k < 8; ++k) total += tmp[k];
    return total;
}

}  // namespace

const Kernels& avx2() {
    static const Kernels k{ &v_add, &v_sub, &v_add_sub, &v_screlu_dot, "AVX2" };
    return k;
}

}  // namespace nnue::kernels
