#include "GlyphDraw.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

using namespace MindShake;

//-------------------------------------
namespace {

    constexpr int32_t kTextureWidth  = 32;
    constexpr int32_t kTextureHeight = 32;
    constexpr int32_t kWidth         = 40;      // Of the destination
    constexpr int32_t kHeight        = 16;
    constexpr int32_t kMaxGlyphWidth = 21;

    // Runs of transparent texels, of opaque ones and of texels in between, so that the code that draws 4 texels at a
    // time gets blocks of each kind, and mixed ones. A texel of 4 bytes is premultiplied: no color above its alpha.
    //---------------------------------
    std::vector<uint8_t>
    MakeTexture(std::mt19937 &rng, uint32_t bytesPerTexel, int32_t texels = kTextureWidth * kTextureHeight) {
        std::vector<uint8_t> texture;
        uint32_t             kind = 0, left = 0;
        for(int32_t i = 0; i < texels; ++i) {
            if(left == 0) {
                kind = rng() % 3;
                left = 1 + rng() % 9;
            }
            --left;

            const uint32_t alpha = (kind == 0) ? 0 : (kind == 1) ? 255 : 1 + rng() % 254;
            if(bytesPerTexel == 1) {
                texture.push_back(uint8_t(alpha));
            }
            else {
                for(int channel = 0; channel < 3; ++channel) {
                    texture.push_back(uint8_t(rng() % (alpha + 1)));
                }
                texture.push_back(uint8_t(alpha));
            }
        }

        return texture;
    }

    // Premultiplied pixels, opaque in the left half and of any alpha in the other.
    //---------------------------------
    std::vector<uint32_t>
    MakeDestination(std::mt19937 &rng, int32_t width = kWidth, int32_t height = kHeight) {
        std::vector<uint32_t> pixels;
        for(int32_t i = 0; i < width * height; ++i) {
            const uint32_t alpha = (i % width < width / 2) ? 255 : rng() % 256;
            uint32_t       pixel = alpha << 24;
            for(uint32_t shift = 0; shift < 24; shift += 8) {
                pixel |= (rng() % (alpha + 1)) << shift;
            }
            pixels.push_back(pixel);
        }

        return pixels;
    }

    // The blend, one channel at a time and with a division: the texel times the color, over the pixel, rounded once.
    // 255 is odd, so the division never ends in .5 and adding 127 rounds it.
    //---------------------------------
    uint32_t
    BlendReference(const uint8_t *texel, uint32_t bytesPerTexel, uint32_t color, uint32_t pixel) {
        const uint32_t texelAlpha = texel[bytesPerTexel - 1];
        if(texelAlpha == 0) {
            return pixel;
        }

        const uint32_t alpha  = (texelAlpha * (color >> 24) + 127) / 255;
        uint32_t       result = 0;
        for(uint32_t channel = 0; channel < 4; ++channel) {
            const uint32_t shift  = channel * 8;
            const uint32_t source = ((bytesPerTexel == 1) ? texelAlpha : texel[channel]) * ((color >> shift) & 0xff);
            result |= ((source + ((pixel >> shift) & 0xff) * (255 - alpha) + 127) / 255) << shift;
        }

        return result;
    }

} // end of namespace

//-------------------------------------
TEST_CASE("The functions that draw a glyph leave the pixels of the blend, the same with every implementation") {
    const int32_t kTextureX = 3, kTextureY = 2, kPosX = 5, kPosY = 4;

    std::mt19937 rng(11);
    for(uint32_t bytesPerTexel : { 1u, 4u }) {
        const std::vector<uint8_t>  texture     = MakeTexture(rng, bytesPerTexel);
        const std::vector<uint32_t> destination = MakeDestination(rng);

        // An alpha of 255 takes the functions for an opaque text. White has its own shortcut over a color texture.
        for(uint32_t color : { 0xffffffffu, 0xff40c080u, 0x80ff8040u, 0x01ffffffu, 0x00ffffffu }) {
            const uint32_t premultiplied = GlyphDraw::PremultiplyColor(color);
            const uint32_t alpha         = color >> 24;
            const bool     opaque        = alpha == 255;

            for(bool rotated : { false, true }) {
                // A rotated glyph is stored transposed: the pixel (x, y) is the texel (x0 + y, y0 + x).
                const size_t stepX  = rotated ? size_t(kTextureWidth) : 1;
                const size_t stepY  = rotated ? 1 : size_t(kTextureWidth);
                const size_t offset = size_t(kTextureY) * kTextureWidth + size_t(kTextureX);

                // A clipped glyph can get a width or a height of 0 or less, which must draw nothing. The SIMD code draws a
                // rotated glyph 4 rows at a time, and its 1 to 3 last rows with a band that overlaps the one above.
                for(int32_t width = -3; width <= kMaxGlyphWidth; ++width) {
                    for(int32_t height : { -2, 0, 1, 3, 4, 5, 7, 8, 9 }) {
                        CAPTURE(bytesPerTexel);
                        CAPTURE(color);
                        CAPTURE(rotated);
                        CAPTURE(width);
                        CAPTURE(height);

                        std::vector<uint32_t> expected = destination;
                        for(int32_t y = 0; y < height; ++y) {
                            for(int32_t x = 0; x < width; ++x) {
                                const uint8_t *texel = &texture[(offset + size_t(y) * stepY + size_t(x) * stepX) * bytesPerTexel];
                                uint32_t      &pixel = expected[size_t(kPosY + y) * kWidth + size_t(kPosX + x)];
                                pixel = BlendReference(texel, bytesPerTexel, premultiplied, pixel);
                            }
                        }

                        auto draw = [&](GlyphDraw::DrawGlyphFunction drawGlyph) {
                            std::vector<uint32_t> buffer = destination;
                            drawGlyph(texture.data(), offset, stepX, stepY, width, height, &buffer[size_t(kPosY) * kWidth + size_t(kPosX)],
                                      uint32_t(kWidth), premultiplied, alpha);
                            return buffer;
                        };

                        CHECK(draw(GlyphDraw::GetScalarDrawGlyphFunction(bytesPerTexel, opaque)) == expected);
                        CHECK(draw(GlyphDraw::GetDrawGlyphFunction(bytesPerTexel, opaque)) == expected);
#if defined(FONTRENDERER_SSE2)
                        CHECK(draw(GlyphDraw::GetSse2DrawGlyphFunction(bytesPerTexel, opaque)) == expected);
#endif
#if defined(FONTRENDERER_NEON)
                        CHECK(draw(GlyphDraw::GetNeonDrawGlyphFunction(bytesPerTexel, opaque)) == expected);
#endif
                        // The functions for a translucent text also draw an opaque one.
                        CHECK(draw(GlyphDraw::GetScalarDrawGlyphFunction(bytesPerTexel, false)) == expected);
                        CHECK(draw(GlyphDraw::GetDrawGlyphFunction(bytesPerTexel, false)) == expected);
                    }
                }
            }
        }
    }
}

// The texture and the destination are copies of exactly the texels and the pixels of the glyph, so that a read or a
// write past them leaves the buffer, which a build with a sanitizer reports. Glyphs up to 70 x 60 get many blocks per
// row and many bands of rows.
//-------------------------------------
TEST_CASE("The functions that draw a glyph only read and write the texels and the pixels of the glyph") {
    const int32_t  kSide     = 80;
    const uint32_t kColors[] = { 0xffffffffu, 0xff40c080u, 0x80ff8040u, 0x01ffffffu, 0x00ffffffu };

    std::mt19937 rng(29);
    for(uint32_t bytesPerTexel : { 1u, 4u }) {
        const std::vector<uint8_t> texture = MakeTexture(rng, bytesPerTexel, kSide * kSide);
        for(int32_t i = 0; i < 300; ++i) {
            const bool     rotated       = rng() % 2 == 0;
            const int32_t  width         = int32_t(rng() % 72) - 2;
            const int32_t  height        = int32_t(rng() % 62) - 2;
            const uint32_t color         = kColors[rng() % 5];
            const uint32_t premultiplied = GlyphDraw::PremultiplyColor(color);
            const uint32_t alpha         = color >> 24;
            CAPTURE(bytesPerTexel);
            CAPTURE(color);
            CAPTURE(rotated);
            CAPTURE(width);
            CAPTURE(height);

            // The glyph in the bottom right corner of the texture. Rotated, it takes height columns and width rows.
            const bool    empty   = width <= 0 || height <= 0;
            const int32_t columns = empty ? 0 : (rotated ? height : width);
            const int32_t rows    = empty ? 0 : (rotated ? width : height);
            const size_t  stepX   = rotated ? size_t(kSide) : 1;
            const size_t  stepY   = rotated ? 1 : size_t(kSide);
            const size_t  offset  = size_t(kSide - rows) * kSide + size_t(kSide - columns);
            const size_t  last    = empty ? offset : offset + size_t(height - 1) * stepY + size_t(width - 1) * stepX;

            std::vector<uint8_t> glyphTexels;
            if(empty == false) {
                glyphTexels.assign(texture.begin() + std::ptrdiff_t(offset * bytesPerTexel), texture.begin() + std::ptrdiff_t((last + 1) * bytesPerTexel));
            }

            const uint32_t              dstStride   = uint32_t(empty ? 0 : width) + rng() % 4;
            const size_t                pixelCount  = empty ? 0 : size_t(height - 1) * dstStride + size_t(width);
            const std::vector<uint32_t> destination = MakeDestination(rng, int32_t(pixelCount), 1);

            std::vector<uint32_t> expected = destination;
            for(int32_t y = 0; y < height; ++y) {
                for(int32_t x = 0; x < width; ++x) {
                    const uint8_t *texel = &glyphTexels[(size_t(y) * stepY + size_t(x) * stepX) * bytesPerTexel];
                    uint32_t      &pixel = expected[size_t(y) * dstStride + size_t(x)];
                    pixel = BlendReference(texel, bytesPerTexel, premultiplied, pixel);
                }
            }

            auto draw = [&](GlyphDraw::DrawGlyphFunction drawGlyph) {
                std::vector<uint32_t> buffer = destination;
                drawGlyph(glyphTexels.data(), 0, stepX, stepY, width, height, buffer.data(), dstStride, premultiplied, alpha);
                return buffer;
            };

            const bool opaque = alpha == 255;
            CHECK(draw(GlyphDraw::GetScalarDrawGlyphFunction(bytesPerTexel, opaque)) == expected);
            CHECK(draw(GlyphDraw::GetDrawGlyphFunction(bytesPerTexel, opaque)) == expected);
#if defined(FONTRENDERER_SSE2)
            CHECK(draw(GlyphDraw::GetSse2DrawGlyphFunction(bytesPerTexel, opaque)) == expected);
            CHECK(draw(GlyphDraw::GetSse2DrawGlyphFunction(bytesPerTexel, false)) == expected);
#endif
#if defined(FONTRENDERER_NEON)
            CHECK(draw(GlyphDraw::GetNeonDrawGlyphFunction(bytesPerTexel, opaque)) == expected);
            CHECK(draw(GlyphDraw::GetNeonDrawGlyphFunction(bytesPerTexel, false)) == expected);
#endif
        }
    }
}
