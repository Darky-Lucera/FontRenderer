//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "CpuX86.h"

#if defined(FONTRENDERER_X86_64_V2) || defined(FONTRENDERER_X86_64_V2_AT_RUNTIME) || defined(FONTRENDERER_X86_64_V3_AT_RUNTIME)

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

    struct Registers {
        uint32_t    eax;
        uint32_t    ebx;
        uint32_t    ecx;
        uint32_t    edx;
    };

    // The registers that cpuid returns for leaf and subleaf, or 0 in all of them if the processor does not have that
    // leaf.
    //---------------------------------
    Registers
    Cpuid(uint32_t leaf, uint32_t subleaf = 0) {
        Registers result = { 0, 0, 0, 0 };
    #if defined(_MSC_VER)
        int registers[4];   // eax, ebx, ecx, edx
        __cpuid(registers, int(leaf & 0x80000000u));
        if (uint32_t(registers[0]) < leaf) {
            return result;
        }
        __cpuidex(registers, int(leaf), int(subleaf));
        result = { uint32_t(registers[0]), uint32_t(registers[1]), uint32_t(registers[2]), uint32_t(registers[3]) };
    #else
        unsigned eax, ebx, ecx, edx;
        if (__get_cpuid_count(leaf, subleaf, &eax, &ebx, &ecx, &edx)) {
            result = { eax, ebx, ecx, edx };
        }
    #endif
        return result;
    }

    // The features of x86-64-v2 beyond those of every x86-64 processor, as the x86-64 psABI lists them. The compiler
    // may use any of them in code built for that level, not only the instructions that the drawing asks for.
    //---------------------------------
    bool
    ProcessorHasX64v2() {
        constexpr uint32_t kSSE3       = 1u << 0;   // In ecx of leaf 1
        constexpr uint32_t kSSSE3      = 1u << 9;
        constexpr uint32_t kCMPXCHG16B = 1u << 13;
        constexpr uint32_t kSSE41      = 1u << 19;
        constexpr uint32_t kSSE42      = 1u << 20;
        constexpr uint32_t kPOPCNT     = 1u << 23;
        constexpr uint32_t kLAHFSAHF   = 1u << 0;   // In ecx of leaf 0x80000001
        constexpr uint32_t kLeaf1      = kSSE3 | kSSSE3 | kCMPXCHG16B | kSSE41 | kSSE42 | kPOPCNT;

        return (Cpuid(1).ecx & kLeaf1) == kLeaf1 && (Cpuid(0x80000001u).ecx & kLAHFSAHF) != 0;
    }

#if defined(FONTRENDERER_X86_64_V3) || defined(FONTRENDERER_X86_64_V3_AT_RUNTIME)
    // XCR0, the register that says which registers the operating system saves when it switches threads.
    //---------------------------------
    uint64_t
    ReadXcr0() {
    #if defined(_MSC_VER)
        return _xgetbv(0);
    #else
        // The intrinsic needs the XSAVE target, which the rest of the file does not have.
        uint32_t low, high;
        __asm__ volatile ("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        return (uint64_t(high) << 32) | low;
    #endif
    }

    // The features that x86-64-v3 adds to x86-64-v2, as the x86-64 psABI lists them, and an operating system that
    // saves the registers of 256 bits. Without that, AVX instructions fault even on a processor that has them.
    //---------------------------------
    bool
    ProcessorHasX64v3() {
        constexpr uint32_t kFMA     = 1u << 12;     // In ecx of leaf 1
        constexpr uint32_t kMOVBE   = 1u << 22;
        constexpr uint32_t kOSXSAVE = 1u << 27;
        constexpr uint32_t kAVX     = 1u << 28;
        constexpr uint32_t kF16C    = 1u << 29;
        constexpr uint32_t kBMI1    = 1u << 3;      // In ebx of leaf 7, subleaf 0
        constexpr uint32_t kAVX2    = 1u << 5;
        constexpr uint32_t kBMI2    = 1u << 8;
        constexpr uint32_t kLZCNT   = 1u << 5;      // In ecx of leaf 0x80000001
        constexpr uint32_t kXMMYMM  = 0x6;          // In XCR0: the state of the xmm registers and of the upper halves
        constexpr uint32_t kLeaf1   = kFMA | kMOVBE | kOSXSAVE | kAVX | kF16C;
        constexpr uint32_t kLeaf7   = kBMI1 | kAVX2 | kBMI2;

        if (ProcessorHasX64v2() == false || (Cpuid(1).ecx & kLeaf1) != kLeaf1) {
            return false;
        }
        // xgetbv only exists when OSXSAVE is set, which the test above checked.
        return (Cpuid(7).ebx & kLeaf7) == kLeaf7 && (Cpuid(0x80000001u).ecx & kLZCNT) != 0 && (ReadXcr0() & kXMMYMM) == kXMMYMM;
    }
#endif

} // end of namespace

#if defined(FONTRENDERER_X86_64_V2) || defined(FONTRENDERER_X86_64_V2_AT_RUNTIME)
//-------------------------------------
bool
CpuX86::HasX64v2() {
    static const bool has = ProcessorHasX64v2();
    return has;
}
#endif

#if defined(FONTRENDERER_X86_64_V3) || defined(FONTRENDERER_X86_64_V3_AT_RUNTIME)
//-------------------------------------
bool
CpuX86::HasX64v3() {
    static const bool has = ProcessorHasX64v3();
    return has;
}
#endif

#endif
