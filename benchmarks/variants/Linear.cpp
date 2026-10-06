#include "benchmarkCommon.h"
//-------------------------------------
#include <cmath>

// Baseline that blends in linear light instead of blending the sRGB values of the pixels, which is the correct way:
// with sRGB values, the edges of the glyphs look thinner or bolder than they are, depending on the colors.
// It measures what that costs, so its pixels differ from DrawText on purpose.
// The coverage of a glyph is already linear, so only the colors are converted, through two tables.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace Linear {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // 8 bits are not enough for linear values: the dark sRGB values would fall on a few of them, and the
        // gradients would show steps. 12 bits keep the table back to sRGB in 4 KB.
        constexpr uint32_t kLinearMax = 4095;

        //-----------------------------
        struct Tables {
            uint16_t    toLinear[256];
            uint8_t     toSrgb[kLinearMax + 1];

            Tables() {
                for (uint32_t i = 0; i < 256; ++i) {
                    const double srgb   = i / 255.0;
                    const double linear = (srgb <= 0.04045) ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
                    toLinear[i] = uint16_t(std::lround(linear * kLinearMax));
                }

                for (uint32_t i = 0; i <= kLinearMax; ++i) {
                    const double linear = double(i) / kLinearMax;
                    const double srgb   = (linear <= 0.0031308) ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
                    toSrgb[i] = uint8_t(std::lround(srgb * 255.0));
                }
            }
        };

        //-----------------------------
        const Tables &
        GetTables() {
            static const Tables tables;
            return tables;
        }

        // red, green and blue are linear, from 0 to kLinearMax.
        //-----------------------------
        inline void
        Blend(const Tables &tables, Color32 &pixel, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
            const uint32_t inverse = 255 - alpha;
            pixel.b = tables.toSrgb[(blue  * alpha + tables.toLinear[pixel.b] * inverse) / 255];
            pixel.g = tables.toSrgb[(green * alpha + tables.toLinear[pixel.g] * inverse) / 255];
            pixel.r = tables.toSrgb[(red   * alpha + tables.toLinear[pixel.r] * inverse) / 255];
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

        const Tables    *tables       = &GetTables();
        const uint32_t  linearRed     = tables->toLinear[fontColor.r];
        const uint32_t  linearGreen   = tables->toLinear[fontColor.g];
        const uint32_t  linearBlue    = tables->toLinear[fontColor.b];

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
                    [fontColor, tables, linearRed, linearGreen, linearBlue](const uint8_t *texel, Color32 &pixel) {
                        if (texel[0] != 0) {
                            Blend(
                                *tables,
                                pixel,
                                linearRed,
                                linearGreen,
                                linearBlue,
                                (texel[0] * fontColor.a) / 255
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
                    [fontColor, tables, linearRed, linearGreen, linearBlue](const uint8_t *texel, Color32 &pixel) {
                        if (texel[3] != 0) {
                            Blend(
                                *tables,
                                pixel,
                                (tables->toLinear[texel[2]] * linearRed)   / kLinearMax,
                                (tables->toLinear[texel[1]] * linearGreen) / kLinearMax,
                                (tables->toLinear[texel[0]] * linearBlue)  / kLinearMax,
                                (texel[3] * fontColor.a) / 255
                            );
                        }
                    }
                );
            }
        }
    }

} // end of namespace Linear
} // end of namespace Benchmark
