#include "benchmarkCommon.h"
//-------------------------------------
#include <cstring>

// RoundOnceSwar with the BGRA32 texture and the color of the text premultiplied by their alpha. The source of the blend
// is then texel * color, which is already in the scale of color * alpha, so the blend is
// round((texel * color + destination * (255 - alpha)) / 255), with a single rounding and two multiplications per
// channel instead of three. The largest sum is 65152, which fits in 16 bits, and the result is never above 255.
// An Alpha8 texel of coverage m is drawn as the premultiplied white texel (m, m, m, m), so both textures still
// leave the same pixels. It computes the same pixels as RoundOnce64Premultiplied and RoundOnceSse2Premultiplied.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace RoundOnceSwarPremultiplied {

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font           = *scenario.font;
        const bool      alpha8          = font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8;
        const uint8_t   *texture        = font.GetTexture();
        const size_t    textureWidth    = font.GetTextureWidth();
        const uint32_t  color           = PremultiplyColor(scenario.color);
        const uint32_t  colorBlueRed    = color & kPairMask;
        const uint32_t  colorGreenAlpha = (color >> 8) & kPairMask;
        const uint32_t  fontAlpha       = color >> 24;

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
                    [colorBlueRed, colorGreenAlpha, fontAlpha](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[0] != 0) {
                            pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], MulDiv255(texel[0], fontAlpha));
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
                    [color, fontAlpha](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[3] != 0) {
                            uint32_t bgra;
                            memcpy(&bgra, texel, sizeof(uint32_t));
                            // The texel and the color are both 0xAARRGGBB, so the pairs line up.
                            pixel = Blend(
                                pixel,
                                MulPairByPair(bgra, color),
                                MulPairByPair(bgra >> 8, color >> 8),
                                MulDiv255(texel[3], fontAlpha)
                            );
                        }
                    }
                );
            }
        }
    }

} // end of namespace RoundOnceSwarPremultiplied
} // end of namespace Benchmark
