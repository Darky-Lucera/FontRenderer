#include "benchmarkCommon.h"
//-------------------------------------
#include <cstring>

// RoundOnceOver with the 4 channels of a pixel in a uint64_t, each in 16 bits: 0xAARRGGBB becomes 0x00AA00RR00GG00BB.
// color * alpha + destination * (255 - alpha) is at most 255 * 255, so it fits in 16 bits: two multiplications of
// 64 bits and a single rounding do the 4 channels. It computes the same pixels as RoundOnceOver and Sse2RoundOnce.
// A CPU of 32 bits needs several multiplications for one of 64 bits, so there RoundOnceSwar should be faster.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace RoundOnce64 {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // Each 16-bit value divided by 255, rounded as Blinn does. Exact for values up to 255 * 255.
        //-----------------------------
        inline uint64_t
        Div255(uint64_t products) {
            const uint64_t t = products + 0x0080008000800080ull;
            return ((t + ((t >> 8) & kChannelMask)) >> 8) & kChannelMask;
        }

        // (color * alpha + pixel * (255 - alpha)) / 255 for the 4 channels.
        //-----------------------------
        inline uint32_t
        Blend(uint32_t pixel, uint64_t color, uint32_t alpha) {
            return Pack(Div255(color * alpha + Spread(pixel) * (255 - alpha)));
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font         = *scenario.font;
        const uint8_t   *texture      = font.GetTexture();
        const size_t    textureWidth  = font.GetTextureWidth();
        Color32         fontColor;
        fontColor.color = scenario.color;
        // With an alpha of 255, color times the alpha of the blend leaves that alpha, as over needs.
        const uint64_t  opaqueColor   = Spread(scenario.color | 0xff000000u);

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
                    [opaqueColor, fontColor](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[0] != 0) {
                            pixel = Blend(pixel, opaqueColor, MulDiv255(texel[0], fontColor.a));
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
                            // Each channel has its own multiplier, so the tint needs 4 multiplications.
                            const uint64_t products = uint64_t(texel[0] * fontColor.b)         |
                                                      (uint64_t(texel[1] * fontColor.g) << 16) |
                                                      (uint64_t(texel[2] * fontColor.r) << 32) |
                                                      (uint64_t(texel[3] * fontColor.a) << 48);
                            const uint64_t tinted   = Div255(products);
                            const uint32_t alpha    = uint32_t(tinted >> 48);
                            pixel = Blend(pixel, tinted | 0x00ff000000000000ull, alpha);
                        }
                    }
                );
            }
        }
    }

} // end of namespace RoundOnce64
} // end of namespace Benchmark
