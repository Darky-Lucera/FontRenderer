//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

// The drawing of GlyphDrawSSE2.cpp built for x86-64-v3. CMake gives this file the options of that level, and the macros
// turn on the parts of GlyphDrawSSE2.cpp that use its instructions and those of x86-64-v2.
// Without one of the macros of x86-64-v3 that Platform.h explains, this file builds nothing.
#include "Platform.h"

#if defined(FONTRENDERER_SSE2) && (defined(FONTRENDERER_X86_64_V3) || defined(FONTRENDERER_X86_64_V3_AT_RUNTIME))
    #define FONTRENDERER_GLYPHDRAW_X64V2
    #define FONTRENDERER_GLYPHDRAW_X64V3
    #include "GlyphDrawSSE2.cpp"
#endif
