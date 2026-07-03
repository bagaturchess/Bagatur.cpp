// nnue_kernels.h — runtime-dispatched SIMD kernels for the NNUE hot loops.
//
// The four accumulator / output primitives each sweep the full HIDDEN_SIZE lane
// array (1536 int16), so they are the right granularity for CPU dispatch: one
// indirect call is amortized over 1536 lanes of work, i.e. the call overhead is
// noise. Three implementations are compiled into ONE binary — AVX-512, AVX2 and
// a scalar fallback — each in its own translation unit built at its own ISA
// level (see the per-file COMPILE_OPTIONS in CMakeLists). `active()` selects the
// fastest set the running CPU supports, once, via CPUID. This lets a single
// distributable run on any x86-64 machine yet still use AVX-512 where present.
//
// Each kernel also carries an explicit `target` attribute (NNUE_TARGET) so that
// LTO regenerates it at the intended ISA — the command-line `-mavx512*` flags
// alone are not reliably preserved across the link-time recompilation.
#pragma once

#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#  define NNUE_TARGET(isa) __attribute__((target(isa)))
#else
#  define NNUE_TARGET(isa)  // MSVC: ISA comes from the per-file /arch option
#endif

namespace nnue::kernels {

// A complete set of the four vectorised primitives for one ISA level.
//   v_add        : v += w                         (accumulator: feature on)
//   v_sub        : v -= w                         (accumulator: feature off)
//   v_add_sub    : v += w_add - w_sub             (accumulator: move a feature)
//   v_screlu_dot : sum_i clamp(acc_i)^2 * w_i     (output layer, one perspective)
struct Kernels {
    void         (*v_add)       (std::int16_t*, const std::int16_t*);
    void         (*v_sub)       (std::int16_t*, const std::int16_t*);
    void         (*v_add_sub)   (std::int16_t*, const std::int16_t*, const std::int16_t*);
    std::int64_t (*v_screlu_dot)(const std::int16_t*, const std::int32_t*);
    const char*  name;
};

const Kernels& avx512();  // nnue_kernels_avx512.cpp  — built with AVX-512F + BW
const Kernels& avx2();    // nnue_kernels_avx2.cpp    — built with AVX2 + FMA
const Kernels& scalar();  // nnue_kernels_scalar.cpp  — portable fallback

// The kernel set for the running CPU, chosen once by CPUID on first call.
const Kernels& active();

}  // namespace nnue::kernels
