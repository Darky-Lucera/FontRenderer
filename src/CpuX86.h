#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Platform.h"

//-------------------------------------
namespace MindShake {
namespace CpuX86 {

#if defined(FONTRENDERER_X86_64_V2) || defined(FONTRENDERER_X86_64_V2_AT_RUNTIME)
    // Whether this processor has x86-64-v2, from cpuid.
    bool    HasX64v2();
#endif

#if defined(FONTRENDERER_X86_64_V3) || defined(FONTRENDERER_X86_64_V3_AT_RUNTIME)
    // Whether this processor has x86-64-v3, which includes AVX2, and the operating system saves its registers.
    bool    HasX64v3();
#endif

} // end of namespace CpuX86
} // end of namespace MindShake
