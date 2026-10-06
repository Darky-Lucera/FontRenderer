#include "benchmarkCommon.h"

// The old baseline: a copy of the loop of FontBase::DrawText before it rounded and premultiplied. It truncates each
// division, sets the alpha to 255 and takes the texture as straight, so in BGRA32 it leaves other pixels. It is kept to
// compare the speed of the old loop with the new one.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace Truncate {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        //-----------------------------
        inline void
        Blend(Color32 &pixel, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
            const uint32_t inverse = 255 - alpha;
            pixel.b = uint8_t((blue  * alpha + pixel.b * inverse) / 255);
            pixel.g = uint8_t((green * alpha + pixel.g * inverse) / 255);
            pixel.r = uint8_t((red   * alpha + pixel.r * inverse) / 255);
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
                    [fontColor](const uint8_t *texel, Color32 &pixel) {
                        if (texel[3] != 0) {
                            Blend(
                                pixel,
                                (texel[2] * fontColor.r) / 255,
                                (texel[1] * fontColor.g) / 255,
                                (texel[0] * fontColor.b) / 255,
                                (texel[3] * fontColor.a) / 255
                            );
                        }
                    }
                );
            }
        }
    }

} // end of namespace Truncate
} // end of namespace Benchmark
