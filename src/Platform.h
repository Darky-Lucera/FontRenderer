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

// The SIMD code is chosen when the library is built, not when it runs.
// Every x86 processor of 64 bits has SSE2.
// One x86 processor of 32 bits gets it only if the compiler is told that it may use SSE2, which MSVC 2012+ is by default.
// Every ARM64 processor has NEON.
// MSVC does not define __ARM_NEON, but it always has NEON on ARM64.
// FONTRENDERER_DISABLE_SIMD leaves only the scalar code.
#if !defined(FONTRENDERER_DISABLE_SIMD)
    #if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
        #define FONTRENDERER_SSE2
    #endif

    #if defined(_M_ARM64) || defined(__ARM_NEON)
        #define FONTRENDERER_NEON
    #endif
#endif

// x86-64 (default) includes SSE and SSE2.
// x86-64-v2 adds SSE3, SSSE3, SSE4.1, SSE4.2 and POPCNT, among others, to the default x86-64 level.
// CMake defines one of these two macros for every file:
// - FONTRENDERER_X86_64_V2: the whole library is built for x86-64-v2, so it only runs on a processor that has it.
// - FONTRENDERER_X86_64_V2_AT_RUNTIME: the library is built for the default x86-64 level, and only GlyphDrawX64v2.cpp
//   for x86-64-v2. The library draws with that file only when cpuid says that the processor has the level.
// Without CMake, define one of the two macros for every file.
//
// x86-64-v3 adds AVX, AVX2, BMI1, BMI2, FMA, F16C, LZCNT and MOVBE to x86-64-v2. GlyphDrawX64v3.cpp draws the glyphs of a
// BGRA32 texture with it. CMake can also define one of these:
// - FONTRENDERER_X86_64_V3: the whole library is built for x86-64-v3, so it only runs on a processor that has it.
//   FONTRENDERER_X86_64_V2 is then defined too.
// - FONTRENDERER_X86_64_V3_AT_RUNTIME: only GlyphDrawX64v3.cpp is built for x86-64-v3, and the library draws with it
//   only when cpuid says that the processor has the level.
// Without CMake, define one of them for every file, or neither.
//
// A universal build of macOS defines these macros for its ARM part too, where they mean nothing.
#if !(defined(FONTRENDERER_CPU_X86) && defined(FONTRENDERER_CPU_64BIT))
    #undef FONTRENDERER_X86_64_V2
    #undef FONTRENDERER_X86_64_V2_AT_RUNTIME
    #undef FONTRENDERER_X86_64_V3
    #undef FONTRENDERER_X86_64_V3_AT_RUNTIME
#endif
#if !defined(FONTRENDERER_SSE2)
    #undef FONTRENDERER_X86_64_V3_AT_RUNTIME
#endif

#if defined(_MSC_VER)
    #define FONTRENDERER_NO_INLINE      __declspec(noinline)
    #define FONTRENDERER_ALWAYS_INLINE  __forceinline
#else
    #define FONTRENDERER_NO_INLINE      __attribute__((noinline))
    #define FONTRENDERER_ALWAYS_INLINE  inline __attribute__((always_inline))
#endif
