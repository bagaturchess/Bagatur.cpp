// nnue_kernels_scalar.cpp — portable scalar fallback for the four NNUE SIMD
// primitives, plus the runtime CPU dispatcher `active()`. Built with no special
// ISA flags, so it is safe to compile and run on any x86-64 target.
//
// The scalar path is a correctness reference; on the shipped build it is only
// ever selected on a CPU without AVX2, which cannot occur in practice because
// the rest of the engine is compiled to the Haswell floor (needs BMI2/AVX2).
// It is kept so the dispatch table is total and the kernels are verifiable.
#include "nnue_kernels.h"

#include "nnue.h"  // HIDDEN_SIZE, QA

#include <cstdlib>      // std::getenv
#include <string_view>

#if defined(_MSC_VER)
#  include <intrin.h>  // __cpuid, __cpuidex, _xgetbv
#endif

namespace nnue::kernels {
namespace {

constexpr int HS = HIDDEN_SIZE;

void v_add(std::int16_t* v, const std::int16_t* w) {
    for (int i = 0; i < HS; ++i) v[i] = static_cast<std::int16_t>(v[i] + w[i]);
}

void v_sub(std::int16_t* v, const std::int16_t* w) {
    for (int i = 0; i < HS; ++i) v[i] = static_cast<std::int16_t>(v[i] - w[i]);
}

void v_add_sub(std::int16_t* v, const std::int16_t* a, const std::int16_t* s) {
    for (int i = 0; i < HS; ++i)
        v[i] = static_cast<std::int16_t>(v[i] + a[i] - s[i]);
}

std::int64_t v_screlu_dot(const std::int16_t* acc, const std::int32_t* weights) {
    std::int64_t s = 0;
    for (int i = 0; i < HS; ++i) {
        int v = acc[i];
        if (v < 0) v = 0;
        else if (v > QA) v = QA;
        s += static_cast<std::int64_t>(v) * v * weights[i];
    }
    return s;
}

// Picks the richest kernel set the running CPU (and OS) actually supports.
const Kernels& select() {
    // Explicit override, mainly for testing all paths in one binary and as a
    // fallback if a host mis-detects. `avx512`/`avx2` trust the caller — forcing
    // an ISA the CPU lacks will fault, exactly as a native build would.
    if (const char* forced = std::getenv("BAGATUR_SIMD")) {
        const std::string_view f{forced};
        if (f == "scalar") return scalar();
        if (f == "avx2")   return avx2();
        if (f == "avx512") return avx512();
    }
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw"))
        return avx512();
    if (__builtin_cpu_supports("avx2"))
        return avx2();
    return scalar();
#elif defined(_MSC_VER)
    int r[4];
    __cpuid(r, 0);
    const int max_leaf = r[0];
    __cpuidex(r, 1, 0);
    const bool osxsave = (r[2] & (1 << 27)) != 0;
    const unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0ull;
    const bool ymm_ok = (xcr0 & 0x6)  == 0x6;    // XMM + YMM state saved by OS
    const bool zmm_ok = (xcr0 & 0xE6) == 0xE6;   // + opmask + ZMM_hi256 + hi16_ZMM
    bool has_avx2 = false, has_avx512f = false, has_avx512bw = false;
    if (max_leaf >= 7) {
        __cpuidex(r, 7, 0);
        has_avx2     = (r[1] & (1 << 5))  != 0;
        has_avx512f  = (r[1] & (1 << 16)) != 0;
        has_avx512bw = (r[1] & (1 << 30)) != 0;
    }
    if (zmm_ok && has_avx512f && has_avx512bw) return avx512();
    if (ymm_ok && has_avx2)                    return avx2();
    return scalar();
#else
    return scalar();
#endif
}

}  // namespace

const Kernels& scalar() {
    static const Kernels k{ &v_add, &v_sub, &v_add_sub, &v_screlu_dot, "scalar" };
    return k;
}

const Kernels& active() {
    static const Kernels& sel = select();  // computed once, thread-safe (C++11)
    return sel;
}

}  // namespace nnue::kernels
