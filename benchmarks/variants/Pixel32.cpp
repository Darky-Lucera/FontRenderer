#include "benchmarkCommon.h"

// Baseline that reads the destination pixel once as a uint32_t, blends in registers, and writes it back once,
// instead of reading and writing each channel as a byte.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace Pixel32 {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // pixel is 0xAARRGGBB, as Color32 on a little-endian CPU.
        //-----------------------------
        inline uint32_t
        Blend(uint32_t pixel, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
            const uint32_t inverse = 255 - alpha;
            const uint32_t b       = (blue  * alpha + ( pixel        & 0xff) * inverse) / 255;
            const uint32_t g       = (green * alpha + ((pixel >>  8) & 0xff) * inverse) / 255;
            const uint32_t r       = (red   * alpha + ((pixel >> 16) & 0xff) * inverse) / 255;
            return 0xff000000u | (r << 16) | (g << 8) | b;
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
                    [fontColor](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[0] != 0) {
                            pixel = Blend(
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
                    [fontColor](const uint8_t *texel, uint32_t &pixel) {
                        if (texel[3] != 0) {
                            pixel = Blend(
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

} // end of namespace Pixel32
} // end of namespace Benchmark
