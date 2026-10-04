#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Platform.h"
//-------------------------------------
#include <cstddef>
#include <cstdint>

//-------------------------------------
namespace MindShake {
namespace GlyphDraw {

    // (a * b) / 255 rounded to the nearest integer, exact for any a and b from 0 to 255 (Jim Blinn).
    //---------------------------------
    inline uint32_t
    MulDiv255(uint32_t a, uint32_t b) {
        const uint32_t t = a * b + 0x80;
        return ((t >> 8) + t) >> 8;
    }

    // The 0xAARRGGBB color with red, green and blue multiplied by its alpha.
    //---------------------------------
    inline uint32_t
    PremultiplyColor(uint32_t color) {
        const uint32_t alpha = color >> 24;
        return (alpha << 24) | (MulDiv255((color >> 16) & 0xff, alpha) << 16) | (MulDiv255((color >> 8) & 0xff, alpha) << 8) |
               MulDiv255(color & 0xff, alpha);
    }

    // Blends the texels of one glyph, already clipped, over the pixels of dst. The glyph is width x height texels,
    // from the texel offset of the texture, with stepX texels between two of a row and stepY between two rows.
    // color is premultiplied, and alpha is the alpha of the color.
    using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                       uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

    // The function for a texture of 1 byte per texel, a coverage, or of 4, a premultiplied BGRA. The one for an
    // opaque text must only get a color with an alpha of 255. It is the fastest the library was built with.
    DrawGlyphFunction   GetDrawGlyphFunction(uint32_t bytesPerTexel, bool opaque);

    // The same function of each implementation. All of them leave the same pixels, which the tests check.
    DrawGlyphFunction   GetScalarDrawGlyphFunction(uint32_t bytesPerTexel, bool opaque);

#if defined(FONTRENDERER_SSE2)
    DrawGlyphFunction   GetSse2DrawGlyphFunction(uint32_t bytesPerTexel, bool opaque);
#endif

#if defined(FONTRENDERER_NEON)
    DrawGlyphFunction   GetNeonDrawGlyphFunction(uint32_t bytesPerTexel, bool opaque);
#endif

} // end of namespace GlyphDraw
} // end of namespace MindShake
