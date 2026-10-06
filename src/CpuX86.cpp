//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "CpuX86.h"

#if defined(FONTRENDERER_X86_64_V2) || defined(FONTRENDERER_X86_64_V2_AT_RUNTIME)

#if defined(_MSC_VER)
    #include <intrin.h>
#else
    #include <cpuid.h>
#endif
//-------------------------------------
#include <cstdint>

using namespace MindShake;

//-------------------------------------
namespace {

    // The ecx register that cpuid returns for leaf, or 0 if the processor does not have that leaf.
    //---------------------------------
    uint32_t
    CpuidEcx(uint32_t leaf) {
    #if defined(_MSC_VER)
        int registers[4];   // eax, ebx, ecx, edx
        __cpuid(registers, int(leaf & 0x80000000u));
        if (uint32_t(registers[0]) < leaf) {
            return 0;
        }
        __cpuid(registers, int(leaf));
        return uint32_t(registers[2]);
    #else
        unsigned eax, ebx, ecx, edx;
        return __get_cpuid(leaf, &eax, &ebx, &ecx, &edx) ? ecx : 0;
    #endif
    }

    // The features of x86-64-v2 beyond those of every x86-64 processor, as the x86-64 psABI lists them. The compiler
    // may use any of them in code built for that level, not only the instructions that the drawing asks for.
    //---------------------------------
    bool
    ProcessorHasX64v2() {
        constexpr uint32_t kSse3       = 1u << 0;   // In ecx of leaf 1
        constexpr uint32_t kSsse3      = 1u << 9;
        constexpr uint32_t kCmpxchg16b = 1u << 13;
        constexpr uint32_t kSse41      = 1u << 19;
        constexpr uint32_t kSse42      = 1u << 20;
        constexpr uint32_t kPopcnt     = 1u << 23;
        constexpr uint32_t kLahfSahf   = 1u << 0;   // In ecx of leaf 0x80000001
        constexpr uint32_t kLeaf1      = kSse3 | kSsse3 | kCmpxchg16b | kSse41 | kSse42 | kPopcnt;

        return (CpuidEcx(1) & kLeaf1) == kLeaf1 && (CpuidEcx(0x80000001u) & kLahfSahf) != 0;
    }

} // end of namespace

//-------------------------------------
bool
CpuX86::HasX64v2() {
    static const bool has = ProcessorHasX64v2();
    return has;
}

#endif
