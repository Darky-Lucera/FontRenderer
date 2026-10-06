//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

// The drawing of GlyphDrawSse2.cpp built for x86-64-v2. CMake gives this file the options of that level, and the macro
// turns on the parts of GlyphDrawSse2.cpp that use its instructions. Including the file keeps a single copy of the code.
// Without one of the macros that Platform.h explains, this file builds nothing.
#include "Platform.h"

#if defined(FONTRENDERER_X86_64_V2) || defined(FONTRENDERER_X86_64_V2_AT_RUNTIME)
    #define FONTRENDERER_GLYPHDRAW_X64V2
    #include "GlyphDrawSse2.cpp"
#endif
