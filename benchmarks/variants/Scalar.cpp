#include "benchmarkCommon.h"
//-------------------------------------
#include <algorithm>

// Baseline with the formula of pixman, which Sse2 computes 4 pixels at a time: each product is divided by 255 with
// rounding, and the blend is the over operator, which also blends the alpha of the destination.
// The alpha of the blend is computed first, and then the color is multiplied by it. pixman premultiplies the color
// of the text first, but that order would round a white BGRA32 texel and an Alpha8 texel with the same coverage
// in different ways.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace Scalar {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // The sums saturate as _mm_adds_epu8 does in the SSE2 code, although the rounding never takes them above 255.
        //-----------------------------
        inline void
        Blend(Color32 &pixel, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
            const uint32_t inverse = 255 - alpha;
            pixel.b = uint8_t(std::min<uint32_t>(MulDiv255(blue,  alpha) + MulDiv255(pixel.b, inverse), 255));
            pixel.g = uint8_t(std::min<uint32_t>(MulDiv255(green, alpha) + MulDiv255(pixel.g, inverse), 255));
            pixel.r = uint8_t(std::min<uint32_t>(MulDiv255(red,   alpha) + MulDiv255(pixel.r, inverse), 255));
            pixel.a = uint8_t(std::min<uint32_t>(alpha + MulDiv255(pixel.a, inverse), 255));
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
                                MulDiv255(texel[0], fontColor.a)
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
                                MulDiv255(texel[2], fontColor.r),
                                MulDiv255(texel[1], fontColor.g),
                                MulDiv255(texel[0], fontColor.b),
                                MulDiv255(texel[3], fontColor.a)
                            );
                        }
                    }
                );
            }
        }
    }

} // end of namespace Scalar
} // end of namespace Benchmark
