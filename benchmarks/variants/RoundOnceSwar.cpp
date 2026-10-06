#include "benchmarkCommon.h"
//-------------------------------------
#include <cstring>

// RoundOnceOver with the tricks of the C code of pixman (pixman-combine32.h): two channels at a time in a uint32_t,
// blue and red in one, green and alpha in another, each in 16 bits. color * alpha + destination * (255 - alpha) is at
// most 255 * 255, so it fits in 16 bits and is rounded once. It computes the same pixels as RoundOnceOver and
// Sse2RoundOnce, with multiplications of 32 bits, for a CPU of 32 bits.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace RoundOnceSwar {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // (x * a) / 255, rounded, channel by channel, for the channels in the bits 0 to 7 and 16 to 23.
        // UN8_rb_MUL_UN8_rb in pixman.
        //-----------------------------
        inline uint32_t
        MulDiv255PairByPair(uint32_t x, uint32_t a) {
            uint32_t t = (x & 0xff) * (a & 0xff);
            t |= (x & 0x00ff0000) * ((a >> 16) & 0xff);
            t += 0x00800080;
            return ((t + ((t >> 8) & kPairMask)) >> 8) & kPairMask;
        }

        // (color * alpha + pixel * (255 - alpha)) / 255 for the 4 channels, with 0xAARRGGBB values.
        //-----------------------------
        inline uint32_t
        Blend(uint32_t pixel, uint32_t color, uint32_t alpha) {
            const uint32_t inverse    = 255 - alpha;
            const uint32_t blueRed    = Div255Pair((color & kPairMask) * alpha + (pixel & kPairMask) * inverse);
            const uint32_t greenAlpha = Div255Pair(((color >> 8) & kPairMask) * alpha + ((pixel >> 8) & kPairMask) * inverse);
            return blueRed | (greenAlpha << 8);
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font         = *scenario.font;
        const uint8_t   *texture      = font.GetTexture();
        const size_t    textureWidth  = font.GetTextureWidth();
        // With an alpha of 255, color times the alpha of the blend leaves that alpha, as over needs.
        const uint32_t  fontColor     = scenario.color;
        const uint32_t  opaqueColor   = fontColor | 0xff000000u;
        const uint32_t  fontAlpha     = fontColor >> 24;

        for (const GlyphQuad &quad : scenario.quads) {
            const size_t stepX         = quad.rotated ? textureWidth : 1;
            const size_t stepY         = quad.rotated ? 1 : textureWidth;
            const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
            uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            if (font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8) {
                DrawTexels<1>(
                    texture,
                    offsetTexture,
                    stepX,
                    stepY,
                    quad.width,
                    quad.height,
                    dstGlyph,
                    scenario.width,
                    [opaqueColor, fontAlpha](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[0] != 0) {
                            pixel = Blend(pixel, opaqueColor, MulDiv255(texel[0], fontAlpha));
                        }
                    }
                );
            }
            else {
                DrawTexels<4>(
                    texture,
                    offsetTexture,
                    stepX,
                    stepY,
                    quad.width,
                    quad.height,
                    dstGlyph,
                    scenario.width,
                    [fontColor](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[3] != 0) {
                            uint32_t bgra;
                            memcpy(&bgra, texel, sizeof(uint32_t));
                            // The texel and the color are both 0xAARRGGBB, so the pairs line up.
                            const uint32_t tinted = MulDiv255PairByPair(bgra, fontColor) | (MulDiv255PairByPair(bgra >> 8, fontColor >> 8) << 8);
                            pixel = Blend(pixel, tinted | 0xff000000u, tinted >> 24);
                        }
                    }
                );
            }
        }
    }

} // end of namespace RoundOnceSwar
} // end of namespace Benchmark
