#pragma once
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64)
#include <pmmintrin.h>
#endif

namespace faustlens {
// Enable denormal flushing on the calling thread.
inline void EnableFlushToZero() {
#if defined(__aarch64__) || defined(_M_ARM64)
    // ARM FPCR FZ flushes both input and output denormals.
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
#elif defined(__x86_64__) || defined(_M_X64)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

} // namespace faustlens
