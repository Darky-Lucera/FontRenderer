#include "benchmarkCommon.h"

// MoreJumpsPremultiplied with the loop that draws one glyph in a function that is never inlined. With -O2, GCC 15
// allocates the registers badly when that loop is inlined with the loops over the glyphs and the rows around it: it
// keeps values of the blend, and even the end of the row, on the stack, and reads them for every texel. On its own,
// the loop keeps everything in registers.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace MoreJumpsNoInline {

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
        NO_INLINE void
        DrawTexels(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                   uint32_t *dst, uint32_t dstStride, TBlend blend) {
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
        DrawQuads(const Scenario &scenario, uint32_t *dst, TBlend blend) {
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

        //-----------------------------
        void
        DrawAlpha8Opaque(const Scenario &scenario, uint32_t *dst, uint32_t premultiplied) {
            const uint32_t colorBlueRed    = premultiplied & kPairMask;
            const uint32_t colorGreenAlpha = (premultiplied >> 8) & kPairMask;
            DrawQuads<1>(scenario, dst, [premultiplied, colorBlueRed, colorGreenAlpha](const uint8_t *texel, uint32_t &pixel) {
                if (texel[0] == 255) {
                    pixel = premultiplied;
                }
                else if (texel[0] != 0) {
                    pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
                }
            });
        }

        //-----------------------------
        void
        DrawAlpha8(const Scenario &scenario, uint32_t *dst, uint32_t premultiplied, uint32_t colorAlpha) {
            const uint32_t colorBlueRed    = premultiplied & kPairMask;
            const uint32_t colorGreenAlpha = (premultiplied >> 8) & kPairMask;
            DrawQuads<1>(scenario, dst, [colorBlueRed, colorGreenAlpha, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                if (texel[0] != 0) {
                    pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], MulDiv255(texel[0], colorAlpha));
                }
            });
        }

        //-----------------------------
        void
        DrawBGRAOpaque(const Scenario &scenario, uint32_t *dst, uint32_t premultiplied) {
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

        //-----------------------------
        void
        DrawBGRA(const Scenario &scenario, uint32_t *dst, uint32_t premultiplied, uint32_t colorAlpha) {
            DrawQuads<4>(scenario, dst, [premultiplied, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                if (texel[3] != 0) {
                    const uint32_t bgra = LoadBGRA(texel);
                    pixel = Blend(pixel, MulPairByPair(bgra, premultiplied), MulPairByPair(bgra >> 8, premultiplied >> 8),
                                  MulDiv255(texel[3], colorAlpha));
                }
            });
        }

#undef NO_INLINE

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const uint32_t premultiplied = PremultiplyColor(scenario.color);
        const uint32_t colorAlpha    = scenario.color >> 24;
        const bool     opaque        = colorAlpha == 255;
        if (scenario.font->GetTextureFormat() == FontBase::ETextureFormat::Alpha8) {
            if (opaque) {
                DrawAlpha8Opaque(scenario, dst, premultiplied);
            }
            else {
                DrawAlpha8(scenario, dst, premultiplied, colorAlpha);
            }
        }
        else {
            if (opaque) {
                DrawBGRAOpaque(scenario, dst, premultiplied);
            }
            else {
                DrawBGRA(scenario, dst, premultiplied, colorAlpha);
            }
        }
    }

} // end of namespace MoreJumpsNoInline
} // end of namespace Benchmark
