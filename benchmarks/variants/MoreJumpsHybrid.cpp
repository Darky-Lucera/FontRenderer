#include "benchmarkCommon.h"

// MoreJumpsPremultiplied for an opaque text and MoreJumpsNoInline for a translucent one: the loop that draws one glyph
// is inlined only for an opaque text. It tests whether each kind of text can keep the form that measured faster with
// GCC 15.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace MoreJumpsHybrid {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

#if defined(_MSC_VER)
    #define NO_INLINE __declspec(noinline)
#else
    #define NO_INLINE __attribute__((noinline))
#endif

        //-----------------------------
        template <size_t kBytesPerTexel, class TBlend>
        inline void
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
        NO_INLINE void
        DrawTexelsNoInline(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                           uint32_t *dst, uint32_t dstStride, TBlend blend) {
            DrawTexels<kBytesPerTexel>(texture, offset, stepX, stepY, width, height, dst, dstStride, blend);
        }

        //-----------------------------
        template <size_t kBytesPerTexel, bool kInlineGlyph, class TBlend>
        void
        DrawQuads(const Scenario &scenario, uint32_t *dst, const TBlend &blend) {
            const uint8_t   *texture      = scenario.font->GetTexture();
            const size_t    textureWidth  = scenario.font->GetTextureWidth();

            for (const GlyphQuad &quad : scenario.quads) {
                const size_t stepX         = quad.rotated ? textureWidth : 1;
                const size_t stepY         = quad.rotated ? 1 : textureWidth;
                const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
                uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
                if (kInlineGlyph) {
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
                else {
                    DrawTexelsNoInline<kBytesPerTexel>(
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
        }

#undef NO_INLINE

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
                DrawQuads<1, true>(scenario, dst, [premultiplied, colorBlueRed, colorGreenAlpha](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[0] == 255) {
                        pixel = premultiplied;
                    }
                    else if (texel[0] != 0) {
                        pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
                    }
                });
            }
            else {
                DrawQuads<1, false>(scenario, dst, [colorBlueRed, colorGreenAlpha, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[0] != 0) {
                        pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], MulDiv255(texel[0], colorAlpha));
                    }
                });
            }
        }
        else {
            if (opaque) {
                DrawQuads<4, true>(scenario, dst, [premultiplied](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[3] == 255) {
                        const uint32_t bgra = LoadBgra(texel);
                        pixel = Div255Pair(MulPairByPair(bgra, premultiplied)) | (Div255Pair(MulPairByPair(bgra >> 8, premultiplied >> 8)) << 8);
                    }
                    else if (texel[3] != 0) {
                        const uint32_t bgra = LoadBgra(texel);
                        pixel = Blend(pixel, MulPairByPair(bgra, premultiplied), MulPairByPair(bgra >> 8, premultiplied >> 8), texel[3]);
                    }
                });
            }
            else {
                DrawQuads<4, false>(scenario, dst, [premultiplied, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[3] != 0) {
                        const uint32_t bgra = LoadBgra(texel);
                        pixel = Blend(pixel, MulPairByPair(bgra, premultiplied), MulPairByPair(bgra >> 8, premultiplied >> 8),
                                      MulDiv255(texel[3], colorAlpha));
                    }
                });
            }
        }
    }

} // end of namespace MoreJumpsHybrid
} // end of namespace Benchmark
