#include "benchmarkCommon.h"

// Baseline with one more loop, for an opaque text over an Alpha8 texture: a texel of 255 is not blended with the
// pixel, the color is written as it is. It is exact: with an alpha of 255 the blend adds pixel * 0. An opaque text
// also makes the alpha of the blend the alpha of the texel, which saves a multiplication. A translucent text and a
// BGRA32 texture keep the loops of Baseline: the first can never take the shortcut, and the second must still tint
// the texel, so it measured no faster.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace MoreJumpsAlpha8 {

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font            = *scenario.font;
        const uint8_t   *texture         = font.GetTexture();
        const size_t    textureWidth     = font.GetTextureWidth();
        const uint32_t  premultiplied    = PremultiplyColor(scenario.color);
        const uint32_t  colorBlueRed     = premultiplied & kPairMask;
        const uint32_t  colorGreenAlpha  = (premultiplied >> 8) & kPairMask;
        const uint32_t  colorAlpha       = scenario.color >> 24;
        const bool      opaque           = colorAlpha == 255;

        for (const GlyphQuad &quad : scenario.quads) {
            const size_t stepX         = quad.rotated ? textureWidth : 1;
            const size_t stepY         = quad.rotated ? 1 : textureWidth;
            const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
            uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            if (font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8) {
                if (opaque) {
                    DrawTexels<1>(
                        texture,
                        offsetTexture,
                        stepX,
                        stepY,
                        quad.width,
                        quad.height,
                        dstGlyph,
                        scenario.width,
                        [premultiplied, colorBlueRed, colorGreenAlpha](const uint8_t *texel, uint32_t &pixel) {
                            if (texel[0] == 255) {
                                pixel = premultiplied;
                            }
                            else if (texel[0] != 0) {
                                pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
                            }
                        }
                    );
                }
                else {
                    DrawTexels<1>(
                        texture,
                        offsetTexture,
                        stepX,
                        stepY,
                        quad.width,
                        quad.height,
                        dstGlyph,
                        scenario.width,
                        [colorBlueRed, colorGreenAlpha, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                            if (texel[0] != 0) {
                                pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], MulDiv255(texel[0], colorAlpha));
                            }
                        }
                    );
                }
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
                    [premultiplied, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[3] != 0) {
                            const uint32_t bgra = uint32_t(texel[0]) | (uint32_t(texel[1]) << 8) | (uint32_t(texel[2]) << 16) | (uint32_t(texel[3]) << 24);
                            pixel = Blend(pixel, MulPairByPair(bgra, premultiplied), MulPairByPair(bgra >> 8, premultiplied >> 8),
                                          MulDiv255(texel[3], colorAlpha));
                        }
                    }
                );
            }
        }
    }

} // end of namespace MoreJumpsAlpha8
} // end of namespace Benchmark
