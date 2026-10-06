#include "benchmarkCommon.h"
//-------------------------------------
#include <cstring>

// RoundOnceSwarPremultiplied with the 4 channels of a pixel in a uint64_t, each in 16 bits: 0xAARRGGBB becomes
// 0x00AA00RR00GG00BB. It computes the same pixels as RoundOnceSwarPremultiplied and RoundOnceSse2Premultiplied.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace RoundOnce64Premultiplied {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // Each 16-bit value divided by 255, rounded as Blinn does. Exact up to 65662.
        //-----------------------------
        inline uint64_t
        Div255(uint64_t values) {
            const uint64_t t = values + 0x0080008000800080ull;
            return ((t + ((t >> 8) & kChannelMask)) >> 8) & kChannelMask;
        }

        // (source + pixel * (255 - alpha)) / 255 for the 4 channels, with the source already multiplied.
        //-----------------------------
        inline uint32_t
        Blend(uint32_t pixel, uint64_t source, uint32_t alpha) {
            return Pack(Div255(source + Spread(pixel) * (255 - alpha)));
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font         = *scenario.font;
        const bool      alpha8        = font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8;
        const uint8_t   *texture      = font.GetTexture();
        const size_t    textureWidth  = font.GetTextureWidth();
        Color32         color;
        color.color = PremultiplyColor(scenario.color);
        const uint64_t  spreadColor   = Spread(color.color);

        for (const GlyphQuad &quad : scenario.quads) {
            const size_t stepX         = quad.rotated ? textureWidth : 1;
            const size_t stepY         = quad.rotated ? 1 : textureWidth;
            const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
            uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            if (alpha8) {
                DrawTexels<1>(
                    texture,
                    offsetTexture,
                    stepX,
                    stepY,
                    quad.width,
                    quad.height,
                    dstGlyph,
                    scenario.width,
                    [spreadColor, color](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[0] != 0) {
                            pixel = Blend(pixel, spreadColor * texel[0], MulDiv255(texel[0], color.a));
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
                    [color](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[3] != 0) {
                            // Each channel has its own multiplier, so the source needs 4 multiplications.
                            const uint64_t source = uint64_t(texel[0] * color.b)         |
                                                    (uint64_t(texel[1] * color.g) << 16) |
                                                    (uint64_t(texel[2] * color.r) << 32) |
                                                    (uint64_t(texel[3] * color.a) << 48);
                            pixel = Blend(pixel, source, MulDiv255(texel[3], color.a));
                        }
                    }
                );
            }
        }
    }

} // end of namespace RoundOnce64Premultiplied
} // end of namespace Benchmark
