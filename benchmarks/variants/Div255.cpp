#include "benchmarkCommon.h"

// Baseline with the divisions by 255 written as additions and shifts. The compiler already turns x / 255 into a
// multiplication and a shift, so this mostly checks the formula that the SIMD variants need, as SIMD has no integer division.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace Div255 {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // The same result as x / 255 for any x up to 255 * 255, the largest product of two channels.
        //-----------------------------
        inline uint32_t
        Div255(uint32_t x) {
            return (x + 1 + (x >> 8)) >> 8;
        }

        //-----------------------------
        inline void
        Blend(Color32 &pixel, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
            const uint32_t inverse = 255 - alpha;
            pixel.b = uint8_t(Div255(blue  * alpha + pixel.b * inverse));
            pixel.g = uint8_t(Div255(green * alpha + pixel.g * inverse));
            pixel.r = uint8_t(Div255(red   * alpha + pixel.r * inverse));
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
                                Div255(texel[0] * fontColor.a)
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
                                Div255(texel[2] * fontColor.r),
                                Div255(texel[1] * fontColor.g),
                                Div255(texel[0] * fontColor.b),
                                Div255(texel[3] * fontColor.a)
                            );
                        }
                    }
                );
            }
        }
    }

} // end of namespace Div255
} // end of namespace Benchmark
