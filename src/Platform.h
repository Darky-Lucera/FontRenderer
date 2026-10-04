#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

// What the library needs to know about the compiler and the processor it is built for, decided here once.

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #define FONTRENDERER_CPU_X86
#endif
#if defined(_M_ARM64) || defined(__aarch64__) || defined(_M_ARM) || defined(__arm__)
    #define FONTRENDERER_CPU_ARM
#endif

// Registers of 64 bits, also when pointers have 32 bits: x32, arm64_32 and WebAssembly.
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__) || defined(__wasm__) || \
    (defined(__riscv_xlen) && __riscv_xlen == 64)
    #define FONTRENDERER_CPU_64BIT
#endif

// The SIMD code is chosen when the library is built, not when it runs. Every x86 processor of 64 bits has SSE2.
// One of 32 bits gets it only if the compiler is told that it may use SSE2, which MSVC is by default.
// Every ARM64 processor has NEON. MSVC does not define __ARM_NEON, but it always has NEON on ARM64.
// FONTRENDERER_DISABLE_SIMD leaves only the scalar code.
#if !defined(FONTRENDERER_DISABLE_SIMD)
    #if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
        #define FONTRENDERER_SSE2
    #endif

    #if defined(_M_ARM64) || defined(__ARM_NEON)
        #define FONTRENDERER_NEON
    #endif
#endif

#if defined(_MSC_VER)
    #define FONTRENDERER_NO_INLINE      __declspec(noinline)
    #define FONTRENDERER_ALWAYS_INLINE  __forceinline
#else
    #define FONTRENDERER_NO_INLINE      __attribute__((noinline))
    #define FONTRENDERER_ALWAYS_INLINE  inline __attribute__((always_inline))
#endif
