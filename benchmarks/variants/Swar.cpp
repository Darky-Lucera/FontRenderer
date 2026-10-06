#include "benchmarkCommon.h"

// Baseline that blends the red and the blue channels with the same multiplications, as two 16-bit numbers inside
// a uint32_t (SIMD within a register). A channel times an alpha is at most 255 * 255 = 65025, so it fits in 16 bits
// and does not overflow into the other number.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace Swar {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // Divides by 255 the 16-bit numbers in bits 0 to 15 and 16 to 31, with the same result as x / 255 for each one.
        // Each sum stays below 65536, so it does not carry into the other number.
        //-----------------------------
        inline uint32_t
        Div255Pair(uint32_t x) {
            return ((x + 0x00010001u + ((x >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
        }

        // pixel is 0xAARRGGBB, as Color32 on a little-endian CPU, and color is 0x00RRGGBB.
        //-----------------------------
        inline uint32_t
        Blend(uint32_t pixel, uint32_t color, uint32_t alpha) {
            const uint32_t inverse = 255 - alpha;
            const uint32_t redBlue = Div255Pair((color & 0x00ff00ffu) * alpha + (pixel & 0x00ff00ffu) * inverse);
            const uint32_t green   = (((color >> 8) & 0xff) * alpha + ((pixel >> 8) & 0xff) * inverse) / 255;
            return 0xff000000u | redBlue | (green << 8);
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
        const uint32_t  color         = fontColor.color & 0x00ffffffu;

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
                    [fontColor, color](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[0] != 0) {
                            pixel = Blend(
                                pixel,
                                color,
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
                    [fontColor](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[3] != 0) {
                            // Each channel has its own multiplier, so the color of the texel cannot use a single multiplication.
                            const uint32_t red   = (texel[2] * fontColor.r) / 255;
                            const uint32_t green = (texel[1] * fontColor.g) / 255;
                            const uint32_t blue  = (texel[0] * fontColor.b) / 255;
                            pixel = Blend(
                                pixel,
                                (red << 16) | (green << 8) | blue,
                                (texel[3] * fontColor.a) / 255
                            );
                        }
                    }
                );
            }
        }
    }

} // end of namespace Swar
} // end of namespace Benchmark
