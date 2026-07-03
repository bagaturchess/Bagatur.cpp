// nnue_kernels_avx512.cpp — AVX-512F + AVX-512BW implementation of the four
// NNUE SIMD primitives. Built with `-mavx512f -mavx512bw -mavx2 -mfma` (see
// CMakeLists) so <immintrin.h> declares the _mm512 intrinsics; each function
// additionally carries target("avx512f,avx512bw") so the ISA survives LTO.
//
// 32 int16 lanes per __m512i. HIDDEN_SIZE = 1536 = 48 vectors; the add/sub
// sweeps unroll 4× (= 128 lanes per iteration, 12 iterations).
#include "nnue_kernels.h"

#include "nnue.h"  // HIDDEN_SIZE, QA

#include <immintrin.h>

namespace nnue::kernels {
namespace {

constexpr int HS = HIDDEN_SIZE;

NNUE_TARGET("avx512f,avx512bw")
void v_add(std::int16_t* __restrict v, const std::int16_t* __restrict w) {
    for (int i = 0; i < HS; i += 32 * 4) {
        __m512i v0 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i));
        __m512i v1 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 32));
        __m512i v2 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 64));
        __m512i v3 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 96));
        v0 = _mm512_add_epi16(v0, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w + i)));
        v1 = _mm512_add_epi16(v1, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w + i + 32)));
        v2 = _mm512_add_epi16(v2, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w + i + 64)));
        v3 = _mm512_add_epi16(v3, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w + i + 96)));
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i),      v0);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 32), v1);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 64), v2);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 96), v3);
    }
}

NNUE_TARGET("avx512f,avx512bw")
void v_sub(std::int16_t* __restrict v, const std::int16_t* __restrict w) {
    for (int i = 0; i < HS; i += 32 * 4) {
        __m512i v0 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i));
        __m512i v1 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 32));
        __m512i v2 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 64));
        __m512i v3 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 96));
        v0 = _mm512_sub_epi16(v0, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w + i)));
        v1 = _mm512_sub_epi16(v1, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w + i + 32)));
        v2 = _mm512_sub_epi16(v2, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w + i + 64)));
        v3 = _mm512_sub_epi16(v3, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w + i + 96)));
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i),      v0);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 32), v1);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 64), v2);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 96), v3);
    }
}

NNUE_TARGET("avx512f,avx512bw")
void v_add_sub(std::int16_t* __restrict v,
               const std::int16_t* __restrict w_add,
               const std::int16_t* __restrict w_sub) {
    for (int i = 0; i < HS; i += 32 * 4) {
        __m512i v0 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i));
        __m512i v1 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 32));
        __m512i v2 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 64));
        __m512i v3 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(v + i + 96));
        v0 = _mm512_add_epi16(v0, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w_add + i)));
        v1 = _mm512_add_epi16(v1, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w_add + i + 32)));
        v2 = _mm512_add_epi16(v2, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w_add + i + 64)));
        v3 = _mm512_add_epi16(v3, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w_add + i + 96)));
        v0 = _mm512_sub_epi16(v0, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w_sub + i)));
        v1 = _mm512_sub_epi16(v1, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w_sub + i + 32)));
        v2 = _mm512_sub_epi16(v2, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w_sub + i + 64)));
        v3 = _mm512_sub_epi16(v3, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(w_sub + i + 96)));
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i),      v0);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 32), v1);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 64), v2);
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(v + i + 96), v3);
    }
}

// SCReLU + dot. 32 int16 lanes per step, widened into 2×16 int32 lanes for the
// squared / weighted accumulation.
NNUE_TARGET("avx512f,avx512bw")
std::int64_t v_screlu_dot(const std::int16_t* __restrict acc,
                          const std::int32_t* __restrict weights) {
    const __m512i lo = _mm512_setzero_si512();
    const __m512i hi = _mm512_set1_epi16(static_cast<std::int16_t>(QA));

    __m512i s0 = _mm512_setzero_si512();
    __m512i s1 = _mm512_setzero_si512();

    for (int i = 0; i < HS; i += 32) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(acc + i));
        v = _mm512_max_epi16(v, lo);
        v = _mm512_min_epi16(v, hi);

        __m256i v_lo = _mm512_castsi512_si256(v);
        __m256i v_hi = _mm512_extracti64x4_epi64(v, 1);
        __m512i a0   = _mm512_cvtepi16_epi32(v_lo);
        __m512i a1   = _mm512_cvtepi16_epi32(v_hi);

        __m512i sq0 = _mm512_mullo_epi32(a0, a0);
        __m512i sq1 = _mm512_mullo_epi32(a1, a1);

        __m512i w0 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(weights + i));
        __m512i w1 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(weights + i + 16));

        s0 = _mm512_add_epi32(s0, _mm512_mullo_epi32(sq0, w0));
        s1 = _mm512_add_epi32(s1, _mm512_mullo_epi32(sq1, w1));
    }

    __m512i s = _mm512_add_epi32(s0, s1);
    return static_cast<std::int64_t>(_mm512_reduce_add_epi32(s));
}

}  // namespace

const Kernels& avx512() {
    static const Kernels k{ &v_add, &v_sub, &v_add_sub, &v_screlu_dot, "AVX-512" };
    return k;
}

}  // namespace nnue::kernels
