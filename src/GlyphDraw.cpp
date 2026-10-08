//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "GlyphDraw.h"
#include "CpuX86.h"

using namespace MindShake;

//-------------------------------------
namespace {

    // Two channels of 0xAARRGGBB are blended at once, each in 16 bits of a uint32_t: blue and red, and green and alpha.
    constexpr uint32_t kPairMask = 0x00ff00ff;

    // The values in the bits 0 to 15 and 16 to 31 divided by 255, rounded as MulDiv255 does. Exact up to 65662.
    //---------------------------------
    inline uint32_t
    Div255Pair(uint32_t values) {
        const uint32_t t = values + 0x00800080;
        return ((t + ((t >> 8) & kPairMask)) >> 8) & kPairMask;
    }

    // Multiplies the two channels of x, in its bits 0 to 7 and 16 to 23, by the same channels of a. Each product
    // takes 16 bits.
    //---------------------------------
    inline uint32_t
    MulPairByPair(uint32_t x, uint32_t a) {
        return (x & 0xff) * (a & 0xff) | (x & 0x00ff0000) * ((a >> 16) & 0xff);
    }

    // The source is premultiplied and not yet divided by 255. Adding the pixel before dividing rounds each channel
    // once. With a premultiplied texel, whose channels do not exceed its alpha, the sum is at most 65152, so it fits
    // in 16 bits and the result does not exceed 255.
    //---------------------------------
    inline uint32_t
    Blend(uint32_t pixel, uint32_t sourceBlueRed, uint32_t sourceGreenAlpha, uint32_t alpha) {
        const uint32_t inverse    = 255 - alpha;
        const uint32_t blueRed    = Div255Pair(sourceBlueRed    + (pixel & kPairMask) * inverse);
        const uint32_t greenAlpha = Div255Pair(sourceGreenAlpha + ((pixel >> 8) & kPairMask) * inverse);
        return blueRed | (greenAlpha << 8);
    }

    // Calls blend(texel, pixel) for each texel of a glyph, from offset, and the pixel of dst it goes to.
    //---------------------------------
    template <size_t kBytesPerTexel, class TBlend>
    inline void
    DrawTexels(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
               uint32_t *dst, uint32_t dstStride, const TBlend &blend) {
        for (int32_t y = 0; y < height; ++y, offset += stepY) {
            uint32_t *row   = &dst[size_t(y) * dstStride];
            size_t   texel  = offset;
            for (int32_t x = 0; x < width; ++x, texel += stepX) {
                blend(&texture[texel * kBytesPerTexel], row[x]);
            }
        }
    }

    // Each of the four loops stays in a function of its own. With several of them inlined into one function, the
    // compilers gave worse code to some of them.

    // A coverage m draws as the premultiplied white texel (m, m, m, m), so a white color texture draws the same.
    // With an opaque color, the alpha of the blend is the coverage, and a coverage of 255 leaves the color as it is:
    // the blend would add the pixel multiplied by 0.
    //---------------------------------
    FONTRENDERER_NO_INLINE void
    DrawAlpha8Opaque(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                     uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t /*alpha*/) {
        const uint32_t colorBlueRed    = color & kPairMask;
        const uint32_t colorGreenAlpha = (color >> 8) & kPairMask;
        DrawTexels<1>(texture, offset, stepX, stepY, width, height, dst, dstStride,
                      [color, colorBlueRed, colorGreenAlpha](const uint8_t *texel, uint32_t &pixel) {
            if (texel[0] == 255) {
                pixel = color;
            }
            else if (texel[0] != 0) {
                pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
            }
        });
    }

    //---------------------------------
    FONTRENDERER_NO_INLINE void
    DrawAlpha8(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
               uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha) {
        const uint32_t colorBlueRed    = color & kPairMask;
        const uint32_t colorGreenAlpha = (color >> 8) & kPairMask;
        DrawTexels<1>(texture, offset, stepX, stepY, width, height, dst, dstStride,
                      [colorBlueRed, colorGreenAlpha, alpha](const uint8_t *texel, uint32_t &pixel) {
            if (texel[0] != 0) {
                pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], GlyphDraw::MulDiv255(texel[0], alpha));
            }
        });
    }

    // An opaque texel under an opaque color is still tinted with the color, but it is not blended with the pixel.
    //---------------------------------
    FONTRENDERER_NO_INLINE void
    DrawBGRAOpaque(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                   uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t /*alpha*/) {
        DrawTexels<4>(texture, offset, stepX, stepY, width, height, dst, dstStride,
                      [color](const uint8_t *texel, uint32_t &pixel) {
            // Reading the texel through a uint32_t pointer, here and in DrawBGRA, is undefined behavior: it breaks
            // the aliasing rule, and it gives the channels in another order on a big-endian processor. It works with
            // GCC and MSVC on x86. The portable load is:
            //   uint32_t(texel[0]) | (uint32_t(texel[1]) << 8) | (uint32_t(texel[2]) << 16) | (uint32_t(texel[3]) << 24)
            if (texel[3] == 255) {
                const uint32_t bgra = *reinterpret_cast<const uint32_t *>(texel);
                pixel = Div255Pair(MulPairByPair(bgra, color)) | (Div255Pair(MulPairByPair(bgra >> 8, color >> 8)) << 8);
            }
            else if (texel[3] != 0) {
                const uint32_t bgra = *reinterpret_cast<const uint32_t *>(texel);
                pixel = Blend(pixel, MulPairByPair(bgra, color), MulPairByPair(bgra >> 8, color >> 8), texel[3]);
            }
        });
    }

    //---------------------------------
    FONTRENDERER_NO_INLINE void
    DrawBGRA(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
             uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha) {
        DrawTexels<4>(texture, offset, stepX, stepY, width, height, dst, dstStride,
                      [color, alpha](const uint8_t *texel, uint32_t &pixel) {
            if (texel[3] != 0) {
                const uint32_t bgra = *reinterpret_cast<const uint32_t *>(texel);
                pixel = Blend(pixel, MulPairByPair(bgra, color), MulPairByPair(bgra >> 8, color >> 8), GlyphDraw::MulDiv255(texel[3], alpha));
            }
        });
    }

#if defined(FONTRENDERER_SSE2) && defined(FONTRENDERER_X86_64_V2_AT_RUNTIME)
    bool gAllowX64v2 = true;
#endif

#if defined(FONTRENDERER_SSE2) && defined(FONTRENDERER_X86_64_V3_AT_RUNTIME)
    bool gAllowX64v3 = true;
#endif

} // end of namespace

#if defined(FONTRENDERER_SSE2) && defined(FONTRENDERER_X86_64_V2_AT_RUNTIME)
//-------------------------------------
void
GlyphDraw::AllowX64v2(bool allow) {
    gAllowX64v2 = allow;
}
#endif

#if defined(FONTRENDERER_SSE2) && defined(FONTRENDERER_X86_64_V3_AT_RUNTIME)
//-------------------------------------
void
GlyphDraw::AllowX64v3(bool allow) {
    gAllowX64v3 = allow;
}
#endif

//-------------------------------------
GlyphDraw::DrawGlyphFunction
GlyphDraw::GetScalarDrawGlyphFunction(uint32_t bytesPerTexel, bool opaque) {
    if (bytesPerTexel == 1) {
        return opaque ? DrawAlpha8Opaque : DrawAlpha8;
    }

    return opaque ? DrawBGRAOpaque : DrawBGRA;
}

//-------------------------------------
GlyphDraw::DrawGlyphFunction
GlyphDraw::GetDrawGlyphFunction(uint32_t bytesPerTexel, bool opaque) {
#if defined(FONTRENDERER_SSE2) && defined(FONTRENDERER_X86_64_V3_AT_RUNTIME)
    if (gAllowX64v3 && CpuX86::HasX64v3()) {
        return GetX64v3DrawGlyphFunction(bytesPerTexel, opaque);
    }
#endif

#if defined(FONTRENDERER_SSE2) && defined(FONTRENDERER_X86_64_V3)
    return GetX64v3DrawGlyphFunction(bytesPerTexel, opaque);
#elif defined(FONTRENDERER_SSE2) && defined(FONTRENDERER_X86_64_V2)
    return GetX64v2DrawGlyphFunction(bytesPerTexel, opaque);
#elif defined(FONTRENDERER_SSE2) && defined(FONTRENDERER_X86_64_V2_AT_RUNTIME)
    if (gAllowX64v2 && CpuX86::HasX64v2()) {
        return GetX64v2DrawGlyphFunction(bytesPerTexel, opaque);
    }

    return GetSSE2DrawGlyphFunction(bytesPerTexel, opaque);
#elif defined(FONTRENDERER_SSE2)
    return GetSSE2DrawGlyphFunction(bytesPerTexel, opaque);
#elif defined(FONTRENDERER_NEON)
    return GetNeonDrawGlyphFunction(bytesPerTexel, opaque);
#else
    return GetScalarDrawGlyphFunction(bytesPerTexel, opaque);
#endif
}
