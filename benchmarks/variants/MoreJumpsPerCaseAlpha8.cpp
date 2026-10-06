#include "benchmarkCommon.h"

// MoreJumpsPerCase with a single function for a BGRA32 texture, without the shortcut for the texel of 255: there
// the shortcut must still tint the texel, and it measured faster with one compiler and slower with the other.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace MoreJumpsPerCaseAlpha8 {

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

    } // end of namespace

    //---------------------------------
    void
    DrawAlpha8Opaque(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font            = *scenario.font;
        const uint8_t   *texture         = font.GetTexture();
        const size_t    textureWidth     = font.GetTextureWidth();
        const uint32_t  premultiplied    = PremultiplyColor(scenario.color);
        const uint32_t  colorBlueRed     = premultiplied & kPairMask;
        const uint32_t  colorGreenAlpha  = (premultiplied >> 8) & kPairMask;

        for (const GlyphQuad &quad : scenario.quads) {
            const size_t stepX         = quad.rotated ? textureWidth : 1;
            const size_t stepY         = quad.rotated ? 1 : textureWidth;
            const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
            uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            DrawTexels<1>(
                texture,
                offsetTexture,
                stepX,
                stepY,
                quad.width,
                quad.height,
                dstGlyph,
                scenario.width,
                [colorBlueRed, colorGreenAlpha, premultiplied](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[0] == 255) {
                        pixel = premultiplied;
                    }
                    else if (texel[0] != 0) {
                        pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
                    }
                }
            );
        }
    }

    //---------------------------------
    void
    DrawAlpha8(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font            = *scenario.font;
        const uint8_t   *texture         = font.GetTexture();
        const size_t    textureWidth     = font.GetTextureWidth();
        const uint32_t  premultiplied    = PremultiplyColor(scenario.color);
        const uint32_t  colorBlueRed     = premultiplied & kPairMask;
        const uint32_t  colorGreenAlpha  = (premultiplied >> 8) & kPairMask;
        const uint32_t  colorAlpha       = scenario.color >> 24;

        for (const GlyphQuad &quad : scenario.quads) {
            const size_t stepX         = quad.rotated ? textureWidth : 1;
            const size_t stepY         = quad.rotated ? 1 : textureWidth;
            const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
            uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            DrawTexels<1>(
                texture,
                offsetTexture,
                stepX,
                stepY,
                quad.width,
                quad.height,
                dstGlyph,
                scenario.width,
                [colorBlueRed, colorGreenAlpha, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[0] != 0) {
                        pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], MulDiv255(texel[0], colorAlpha));
                    }
                }
            );
        }
    }

    //---------------------------------
    void
    DrawBgra(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font            = *scenario.font;
        const uint8_t   *texture         = font.GetTexture();
        const size_t    textureWidth     = font.GetTextureWidth();
        const uint32_t  premultiplied    = PremultiplyColor(scenario.color);
        const uint32_t  colorAlpha       = scenario.color >> 24;

        for (const GlyphQuad &quad : scenario.quads) {
            const size_t stepX         = quad.rotated ? textureWidth : 1;
            const size_t stepY         = quad.rotated ? 1 : textureWidth;
            const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
            uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            DrawTexels<4>(
                texture,
                offsetTexture,
                stepX,
                stepY,
                quad.width,
                quad.height,
                dstGlyph,
                scenario.width,
                [premultiplied, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
                    if (texel[3] != 0) {
                        const uint32_t bgra = uint32_t(texel[0]) | (uint32_t(texel[1]) << 8) | (uint32_t(texel[2]) << 16) | (uint32_t(texel[3]) << 24);
                        pixel = Blend(pixel, MulPairByPair(bgra, premultiplied), MulPairByPair(bgra >> 8, premultiplied >> 8),
                                        MulDiv255(texel[3], colorAlpha));
                    }
                }
            );
        }
    }

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const uint32_t  colorAlpha       = scenario.color >> 24;
        const bool      opaque           = colorAlpha == 255;
        const bool      isAlpha8         = scenario.font->GetTextureFormat() == FontBase::ETextureFormat::Alpha8;
        if (isAlpha8) {
            if (opaque) {
                DrawAlpha8Opaque(scenario, dst);
            }
            else {
                DrawAlpha8(scenario, dst);
            }
        }
        else {
            DrawBgra(scenario, dst);
        }
    }

} // end of namespace MoreJumpsPerCaseAlpha8
} // end of namespace Benchmark
