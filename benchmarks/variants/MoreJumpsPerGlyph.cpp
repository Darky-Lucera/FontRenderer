#include "benchmarkCommon.h"

// A copy of the scalar functions of the library, in src/GlyphDraw.cpp, to try changes on: the blend of premultiplied
// alpha, rounded once as Blinn does, two channels at a time in a uint32_t, with a function for each texture format
// and for an opaque or a translucent text. The function is chosen once and called for each glyph.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace MoreJumpsPerGlyph {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // Each loop stays in a function of its own. With several of them inlined into one function, the compilers
        // gave worse code to some of them.
#if defined(_MSC_VER)
    #define NO_INLINE __declspec(noinline)
#else
    #define NO_INLINE __attribute__((noinline))
#endif

        using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                           uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

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

        // With an opaque color, the alpha of the blend is the coverage, and a coverage of 255 leaves the color as it
        // is: the blend would add the pixel multiplied by 0.
        //-----------------------------
        NO_INLINE void
        DrawAlpha8Opaque(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                         uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t /*alpha*/) {
            const uint32_t colorBlueRed    = color & kPairMask;
            const uint32_t colorGreenAlpha = (color >> 8) & kPairMask;
            DrawTexels<1>(texture, offset, stepX, stepY, width, height, dst, dstStride,
                          [color, colorBlueRed, colorGreenAlpha](const uint8_t *texel, uint32_t &pixel) {
                if (texel[0] == 255) {
                    pixel = color;
                }
                else if (texel[0] != 0) {
                    pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
                }
            });
        }

        //-----------------------------
        NO_INLINE void
        DrawAlpha8(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                   uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha) {
            const uint32_t colorBlueRed    = color & kPairMask;
            const uint32_t colorGreenAlpha = (color >> 8) & kPairMask;
            DrawTexels<1>(texture, offset, stepX, stepY, width, height, dst, dstStride,
                          [colorBlueRed, colorGreenAlpha, alpha](const uint8_t *texel, uint32_t &pixel) {
                if (texel[0] != 0) {
                    pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], MulDiv255(texel[0], alpha));
                }
            });
        }

        // An opaque texel under an opaque color is still tinted with the color, but it is not blended with the pixel.
        // The texel is read through a uint32_t pointer, as the library reads it.
        //-----------------------------
        NO_INLINE void
        DrawBGRAOpaque(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                       uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t /*alpha*/) {
            DrawTexels<4>(texture, offset, stepX, stepY, width, height, dst, dstStride,
                          [color](const uint8_t *texel, uint32_t &pixel) {
                if (texel[3] == 255) {
                    const uint32_t bgra = *reinterpret_cast<const uint32_t *>(texel);
                    pixel = Div255Pair(MulPairByPair(bgra, color)) | (Div255Pair(MulPairByPair(bgra >> 8, color >> 8)) << 8);
                }
                else if (texel[3] != 0) {
                    const uint32_t bgra = *reinterpret_cast<const uint32_t *>(texel);
                    pixel = Blend(pixel, MulPairByPair(bgra, color), MulPairByPair(bgra >> 8, color >> 8), texel[3]);
                }
            });
        }

        //-----------------------------
        NO_INLINE void
        DrawBGRA(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                 uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha) {
            DrawTexels<4>(texture, offset, stepX, stepY, width, height, dst, dstStride,
                          [color, alpha](const uint8_t *texel, uint32_t &pixel) {
                if (texel[3] != 0) {
                    const uint32_t bgra = *reinterpret_cast<const uint32_t *>(texel);
                    pixel = Blend(pixel, MulPairByPair(bgra, color), MulPairByPair(bgra >> 8, color >> 8), MulDiv255(texel[3], alpha));
                }
            });
        }

#undef NO_INLINE

        //-----------------------------
        DrawGlyphFunction
        GetDrawGlyphFunction(bool alpha8, bool opaque) {
            if (alpha8) {
                return opaque ? DrawAlpha8Opaque : DrawAlpha8;
            }

            return opaque ? DrawBGRAOpaque : DrawBGRA;
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase          &font         = *scenario.font;
        const uint8_t           *texture      = font.GetTexture();
        const size_t            textureWidth  = font.GetTextureWidth();
        const uint32_t          premultiplied = PremultiplyColor(scenario.color);
        const uint32_t          colorAlpha    = scenario.color >> 24;
        const DrawGlyphFunction drawGlyph     = GetDrawGlyphFunction(font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8, colorAlpha == 255);

        for (const GlyphQuad &quad : scenario.quads) {
            const size_t stepX         = quad.rotated ? textureWidth : 1;
            const size_t stepY         = quad.rotated ? 1 : textureWidth;
            const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
            uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            drawGlyph(texture, offsetTexture, stepX, stepY, quad.width, quad.height, dstGlyph, scenario.width, premultiplied, colorAlpha);
        }
    }

} // end of namespace MoreJumpsPerGlyph
} // end of namespace Benchmark
