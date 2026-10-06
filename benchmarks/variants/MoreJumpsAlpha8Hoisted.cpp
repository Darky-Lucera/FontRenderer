#include "benchmarkCommon.h"

// MoreJumpsAlpha8 with the check of the texture format out of the loop over the glyphs, which is written once for
// each format. The check of the opaque text stays inside the loop of Alpha8.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace MoreJumpsAlpha8Hoisted {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // The drawing of an opaque text over an Alpha8 texture, written in one piece to read it: no template, no
        // lambda and no helper. Nothing calls it; Draw does the same through DrawTexels. The alpha of the color must
        // be 255, so the color is already premultiplied.
        //-----------------------------
        void
        DrawAlpha8Opaque(const Scenario &scenario, uint32_t *dst) {
            constexpr uint32_t kMask  = 0x00ff00ff;     // Two channels, each in the low byte of 16 bits
            constexpr uint32_t kRound = 0x00800080;     // 128 for each of the two channels

            const uint8_t   *texture        = scenario.font->GetTexture();
            const size_t    textureWidth    = scenario.font->GetTextureWidth();
            const uint32_t  color           = scenario.color;
            const uint32_t  colorBlueRed    = color & kMask;
            const uint32_t  colorGreenAlpha = (color >> 8) & kMask;

            for (const GlyphQuad &quad : scenario.quads) {
                const size_t stepX  = quad.rotated ? textureWidth : 1;
                const size_t stepY  = quad.rotated ? 1 : textureWidth;
                size_t       offset = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
                uint32_t     *row   = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];

                for (int32_t y = 0; y < quad.height; ++y, offset += stepY, row += scenario.width) {
                    size_t texel = offset;
                    for (int32_t x = 0; x < quad.width; ++x, texel += stepX) {
                        const uint32_t coverage = texture[texel];
                        if (coverage == 255) {
                            row[x] = color;
                        }
                        else if (coverage != 0) {
                            const uint32_t pixel   = row[x];
                            const uint32_t inverse = 255 - coverage;

                            // color * coverage + pixel * (255 - coverage), which is at most 255 * 255 per channel.
                            uint32_t blueRed    = colorBlueRed    * coverage + (pixel & kMask) * inverse;
                            uint32_t greenAlpha = colorGreenAlpha * coverage + ((pixel >> 8) & kMask) * inverse;

                            // The division by 255, rounded as Blinn does: ((v + 128) + ((v + 128) >> 8)) >> 8.
                            blueRed    += kRound;
                            greenAlpha += kRound;
                            blueRed    = ((blueRed    + ((blueRed    >> 8) & kMask)) >> 8) & kMask;
                            greenAlpha = ((greenAlpha + ((greenAlpha >> 8) & kMask)) >> 8) & kMask;

                            row[x] = blueRed | (greenAlpha << 8);
                        }
                    }
                }
            }
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font            = *scenario.font;
        const uint8_t   *texture         = font.GetTexture();
        const size_t    textureWidth     = font.GetTextureWidth();
        const uint32_t  premultiplied    = PremultiplyColor(scenario.color);
        const uint32_t  colorBlueRed     = premultiplied & kPairMask;
        const uint32_t  colorGreenAlpha  = (premultiplied >> 8) & kPairMask;
        const uint32_t  colorAlpha       = scenario.color >> 24;
        const bool      opaque           = colorAlpha == 255;

        if (font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8) {
            for (const GlyphQuad &quad : scenario.quads) {
                const size_t stepX         = quad.rotated ? textureWidth : 1;
                const size_t stepY         = quad.rotated ? 1 : textureWidth;
                const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
                uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
                if (opaque) {
                    DrawTexels<1>(
                        texture,
                        offsetTexture,
                        stepX,
                        stepY,
                        quad.width,
                        quad.height,
                        dstGlyph,
                        scenario.width,
                        [premultiplied, colorBlueRed, colorGreenAlpha](const uint8_t *texel, uint32_t &pixel) {
                            if (texel[0] == 255) {
                                pixel = premultiplied;
                            }
                            else if (texel[0] != 0) {
                                pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
                            }
                        }
                    );
                }
                else {
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
        }
        else {
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

        //for (const GlyphQuad &quad : scenario.quads) {
        //    const size_t stepX         = quad.rotated ? textureWidth : 1;
        //    const size_t stepY         = quad.rotated ? 1 : textureWidth;
        //    const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
        //    uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
        //    if (font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8) {
        //        if (opaque) {
        //            DrawTexels<1>(
        //                texture,
        //                offsetTexture,
        //                stepX,
        //                stepY,
        //                quad.width,
        //                quad.height,
        //                dstGlyph,
        //                scenario.width,
        //                [premultiplied, colorBlueRed, colorGreenAlpha](const uint8_t *texel, uint32_t &pixel) {
        //                    if (texel[0] == 255) {
        //                        pixel = premultiplied;
        //                    }
        //                    else if (texel[0] != 0) {
        //                        pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], texel[0]);
        //                    }
        //                }
        //            );
        //        }
        //        else {
        //            DrawTexels<1>(
        //                texture,
        //                offsetTexture,
        //                stepX,
        //                stepY,
        //                quad.width,
        //                quad.height,
        //                dstGlyph,
        //                scenario.width,
        //                [colorBlueRed, colorGreenAlpha, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
        //                    if (texel[0] != 0) {
        //                        pixel = Blend(pixel, colorBlueRed * texel[0], colorGreenAlpha * texel[0], MulDiv255(texel[0], colorAlpha));
        //                    }
        //                }
        //            );
        //        }
        //    }
        //    else {
        //        DrawTexels<4>(
        //            texture,
        //            offsetTexture,
        //            stepX,
        //            stepY,
        //            quad.width,
        //            quad.height,
        //            dstGlyph,
        //            scenario.width,
        //            [premultiplied, colorAlpha](const uint8_t *texel, uint32_t &pixel) {
        //                if (texel[3] != 0) {
        //                    const uint32_t bgra = uint32_t(texel[0]) | (uint32_t(texel[1]) << 8) | (uint32_t(texel[2]) << 16) | (uint32_t(texel[3]) << 24);
        //                    pixel = Blend(pixel, MulPairByPair(bgra, premultiplied), MulPairByPair(bgra >> 8, premultiplied >> 8),
        //                                  MulDiv255(texel[3], colorAlpha));
        //                }
        //            }
        //        );
        //    }
        //}
    }

} // end of namespace MoreJumpsAlpha8Hoisted
} // end of namespace Benchmark
