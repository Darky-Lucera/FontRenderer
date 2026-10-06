#include "benchmarkCommon.h"
//-------------------------------------
#include <unordered_map>
#include <vector>

// Baseline with the BGRA texture premultiplied by its alpha, as FontBaked could store it. The blend then adds the
// color of the texel instead of multiplying it by its alpha first. The rounding changes, so the pixels may differ
// from DrawText by a level or two. An Alpha8 texture has no color to premultiply, so it is drawn as in Baseline.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace Premultiplied {

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

        // red, green and blue are already multiplied by alpha, so they are never above it and the sum never
        // goes above 255.
        //-----------------------------
        inline void
        BlendPremultiplied(Color32 &pixel, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
            const uint32_t inverse = 255 - alpha;
            pixel.b = uint8_t(blue  + (pixel.b * inverse) / 255);
            pixel.g = uint8_t(green + (pixel.g * inverse) / 255);
            pixel.r = uint8_t(red   + (pixel.r * inverse) / 255);
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

        // FontBaked would premultiply the texture once, when it loads it. Here it is done in the first call for each
        // texture, which is the call that checks the pixels, so the times do not include it.
        //-----------------------------
        const uint8_t *
        GetPremultipliedTexture(const FontBase &font) {
            static std::unordered_map<const uint8_t *, std::vector<uint8_t>> textures;

            const uint8_t *texture = font.GetTexture();
            auto          found    = textures.find(texture);
            if (found == textures.end()) {
                const size_t         size = size_t(font.GetTextureWidth()) * font.GetTextureHeight() * 4;
                std::vector<uint8_t> premultiplied(texture, texture + size);
                for (size_t i = 0; i < size; i += 4) {
                    const uint32_t alpha = premultiplied[i + 3];
                    premultiplied[i + 0] = uint8_t((premultiplied[i + 0] * alpha) / 255);
                    premultiplied[i + 1] = uint8_t((premultiplied[i + 1] * alpha) / 255);
                    premultiplied[i + 2] = uint8_t((premultiplied[i + 2] * alpha) / 255);
                }
                found = textures.emplace(texture, std::move(premultiplied)).first;
            }

            return found->second.data();
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase  &font         = *scenario.font;
        const bool      alpha8        = font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8;
        const uint8_t   *texture      = alpha8 ? font.GetTexture() : GetPremultipliedTexture(font);
        const size_t    textureWidth  = font.GetTextureWidth();
        Color32         fontColor;
        fontColor.color = scenario.color;

        // The color of the text is premultiplied once per call, like the texture.
        Color32         premultipliedColor;
        premultipliedColor.r = uint8_t((fontColor.r * fontColor.a) / 255);
        premultipliedColor.g = uint8_t((fontColor.g * fontColor.a) / 255);
        premultipliedColor.b = uint8_t((fontColor.b * fontColor.a) / 255);
        premultipliedColor.a = fontColor.a;

        for (const GlyphQuad &quad : scenario.quads) {
            const size_t stepX         = quad.rotated ? textureWidth : 1;
            const size_t stepY         = quad.rotated ? 1 : textureWidth;
            const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
            uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            if (alpha8) {
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
                    [premultipliedColor](const uint8_t *texel, Color32 &pixel) {
                        if (texel[3] != 0) {
                            BlendPremultiplied(
                                pixel,
                                (texel[2] * premultipliedColor.r) / 255,
                                (texel[1] * premultipliedColor.g) / 255,
                                (texel[0] * premultipliedColor.b) / 255,
                                (texel[3] * premultipliedColor.a) / 255
                            );
                        }
                    }
                );
            }
        }
    }

} // end of namespace Premultiplied
} // end of namespace Benchmark
