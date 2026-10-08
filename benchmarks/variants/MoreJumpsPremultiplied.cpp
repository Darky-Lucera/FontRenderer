#include "benchmarkCommon.h"

// MoreJumps on top of the current Baseline: with an opaque text, a texel with alpha 255 is not blended with the pixel.
// Alpha8 writes the color as it is. BGRA32 still tints the texel with the color, but does not read the pixel.
// Both are exact: with alpha 255 the blend adds pixel * 0. An opaque text also makes the alpha of the blend the
// alpha of the texel, which saves a multiplication. A translucent text can never take the shortcut, so it uses the
// loop of Baseline and does not pay for the branch.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace MoreJumpsPremultiplied {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        //-----------------------------
        template <size_t kBytesPerTexel, class TBlend>
        void
        DrawTexels(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                   uint32_t *dst, uint32_t dstStride, const TBlend &blend) {
            for (int32_t y = 0; y < height; ++y, offset += stepY) {
                uint32_t *row   = &dst[size_t(y) * dstStride];
                size_t   texel  = offset;
                for (int32_t x = 0; x < width; ++x, texel += stepX) {
                    blend(&texture[texel * kBytesPerTexel], row[x]);
                }
            }
        }

        //-----------------------------
        template <size_t kBytesPerTexel, class TBlend>
        void
        DrawQuads(const Scenario &scenario, uint32_t *dst, const TBlend &blend) {
            const uint8_t   *texture      = scenario.font->GetTexture();
            const size_t    textureWidth  = scenario.font->GetTextureWidth();

            for (const GlyphQuad &quad : scenario.quads) {
                const size_t stepX         = quad.rotated ? textureWidth : 1;
                const size_t stepY         = quad.rotated ? 1 : textureWidth;
                const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
                uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
                DrawTexels<kBytesPerTexel>(
                    texture,
                    offsetTexture,
                    stepX,
                    stepY,
                    quad.width,
                    quad.height,
                    dstGlyph,
                    scenario.width,
                    blend
                );
            }
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const uint32_t  premultiplied    = PremultiplyColor(scenario.color);
        const uint32_t  colorBlueRed     = premultiplied & kPairMask;
        const uint32_t  colorGreenAlpha  = (premultiplied >> 8) & kPairMask;
        const uint32_t  colorAlpha       = scenario.color >> 24;

        const bool opaque = colorAlpha == 255;
        if (scenario.font->GetTextureFormat() == FontBase::ETextureFormat::Alpha8) {
            if (opaque) {
                DrawQuads<1>(scenario, dst, [premultiplied, colorBlueRed, colorGreenAlpha](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[0] == 255) {
                        pixel = premultiplied;
                    }
                    else if (texel[0] != 0) {
                        pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
                    }
                });
            }
            else {
                DrawQuads<1>(scenario, dst, [colorBlueRed, colorGreenAlpha, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[0] != 0) {
                        pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], MulDiv255(texel[0], colorAlpha));
                    }
                });
            }
        }
        else {
            if (opaque) {
                DrawQuads<4>(scenario, dst, [premultiplied](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[3] == 255) {
                        const uint32_t bgra = LoadBGRA(texel);
                        pixel = Div255Pair(MulPairByPair(bgra, premultiplied)) | (Div255Pair(MulPairByPair(bgra >> 8, premultiplied >> 8)) << 8);
                    }
                    else if (texel[3] != 0) {
                        const uint32_t bgra = LoadBGRA(texel);
                        pixel = Blend(pixel, MulPairByPair(bgra, premultiplied), MulPairByPair(bgra >> 8, premultiplied >> 8), texel[3]);
                    }
                });
            }
            else {
                DrawQuads<4>(scenario, dst, [premultiplied, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[3] != 0) {
                        const uint32_t bgra = LoadBGRA(texel);
                        pixel = Blend(pixel, MulPairByPair(bgra, premultiplied), MulPairByPair(bgra >> 8, premultiplied >> 8),
                                      MulDiv255(texel[3], colorAlpha));
                    }
                });
            }
        }
    }

} // end of namespace MoreJumpsPremultiplied
} // end of namespace Benchmark
