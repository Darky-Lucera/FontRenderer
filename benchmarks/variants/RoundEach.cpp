#include "benchmarkCommon.h"

// RoundOnce with each product of the blend rounded on its own, and then added, as pixman does. Unlike Scalar, it still
// sets the alpha to 255 and does not saturate the sum, which never goes above 255, so the cost of rounding each
// product can be told apart from the rest.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace RoundEach {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        //-----------------------------
        inline void
        Blend(Color32 &pixel, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
            const uint32_t inverse = 255 - alpha;
            pixel.b = uint8_t(Round255(blue  * alpha) + Round255(pixel.b * inverse));
            pixel.g = uint8_t(Round255(green * alpha) + Round255(pixel.g * inverse));
            pixel.r = uint8_t(Round255(red   * alpha) + Round255(pixel.r * inverse));
            pixel.a = 255;
        }

        //-----------------------------
        template <size_t kBytesPerTexel, class TBlend>
        void
        DrawTexels(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                   uint32_t *dst, uint32_t dstStride, TBlend blend) {
            for (int32_t y = 0; y < height; ++y, offset += stepY) {
                uint32_t *row   = &dst[size_t(y) * dstStride];
                size_t   texel  = offset;
                for (int32_t x = 0; x < width; ++x, texel += stepX) {
                    blend(&texture[texel * kBytesPerTexel], *reinterpret_cast<Color32 *>(&row[x]));
                }
            }
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
                    [fontColor](const uint8_t *texel, Color32 &pixel) {
                        if (texel[0] != 0) {
                            Blend(
                                pixel,
                                fontColor.r,
                                fontColor.g,
                                fontColor.b,
                                Round255(texel[0] * fontColor.a)
                            );
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
                    [fontColor](const uint8_t *texel, Color32 &pixel) {
                        if (texel[3] != 0) {
                            Blend(
                                pixel,
                                Round255(texel[2] * fontColor.r),
                                Round255(texel[1] * fontColor.g),
                                Round255(texel[0] * fontColor.b),
                                Round255(texel[3] * fontColor.a)
                            );
                        }
                    }
                );
            }
        }
    }

} // end of namespace RoundEach
} // end of namespace Benchmark
