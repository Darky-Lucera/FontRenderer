#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "testHelpers.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <vector>
#if defined(FONTRENDERER_USE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstring>
#include <math.h>      // ::lround, as DJGPP has no std::lround
#endif

using namespace MindShake;

//-------------------------------------
namespace {

    // Opaque white over a black buffer leaves the glyph coverage in every color channel.
    constexpr uint32_t kWhite = 0xffffffffu;

    //---------------------------------
    struct Bitmap {
        std::vector<uint8_t>    pixels;
        int                     width  {};
        int                     height {};
        int                     x      {};     // Offset from the pen position on the baseline
        int                     y      {};
    };

    //---------------------------------
    std::vector<uint8_t>
    ReadFile(const char *path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

#if defined(FONTRENDERER_USE_STB)
    //---------------------------------
    Bitmap
    Rasterize(const FontSTB &, uint32_t codePoint, float scale) {
        static const std::vector<uint8_t> data = ReadFile(Test::kFontPath);
        static const stbtt_fontinfo       info = [] {
            stbtt_fontinfo fontInfo {};
            stbtt_InitFont(&fontInfo, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0));
            return fontInfo;
        }();

        Bitmap    bitmap;
        const int glyph = stbtt_FindGlyphIndex(&info, int(codePoint));
        int       x1, y1, x2, y2;
        stbtt_GetGlyphBitmapBox(&info, glyph, scale, scale, &x1, &y1, &x2, &y2);
        bitmap.width  = x2 - x1;
        bitmap.height = y2 - y1;
        bitmap.x      = x1;
        bitmap.y      = y1;
        bitmap.pixels.resize(size_t(bitmap.width) * size_t(bitmap.height));
        if(bitmap.pixels.empty() == false) {
            stbtt_MakeGlyphBitmap(&info, bitmap.pixels.data(), bitmap.width, bitmap.height, bitmap.width, scale, scale, glyph);
        }

        return bitmap;
    }
#endif

#if defined(FONTRENDERER_USE_LIBSCHRIFT)
    //---------------------------------
    Bitmap
    Rasterize(const FontSFT &, uint32_t codePoint, float scale) {
        static const std::vector<uint8_t>                             data = ReadFile(Test::kFontPath);
        static const std::unique_ptr<SFT_Font, void (*)(SFT_Font *)> font { sft_loadmem(data.data(), data.size()), sft_freefont };

        // Same expression as FontSFT, so both renders are bit identical.
        SFT sft {};
        sft.xScale = double(scale) * sft_unitsPerEm(font.get());
        sft.yScale = sft.xScale;
        sft.flags  = SFT_DOWNWARD_Y;
        sft.font   = font.get();

        Bitmap       bitmap;
        SFT_Glyph    glyph {};
        SFT_GMetrics metrics {};
        sft_lookup(&sft, codePoint, &glyph);
        sft_gmetrics(&sft, glyph, &metrics);
        bitmap.width  = metrics.minWidth;
        bitmap.height = metrics.minHeight;
        bitmap.x      = metrics.xOffset;
        bitmap.y      = metrics.yOffset;
        bitmap.pixels.resize(size_t(bitmap.width) * size_t(bitmap.height));
        if(bitmap.pixels.empty() == false) {
            SFT_Image image {};
            image.pixels = bitmap.pixels.data();
            image.width  = bitmap.width;
            image.height = bitmap.height;
            sft_render(&sft, glyph, image);
        }

        return bitmap;
    }
#endif

#if defined(FONTRENDERER_USE_FREETYPE)
    //---------------------------------
    Bitmap
    Rasterize(const FontFT &, uint32_t codePoint, float scale) {
        static const std::vector<uint8_t> data    = ReadFile(Test::kFontPath);
        static const FT_Library           library = [] {
            FT_Library value = nullptr;
            FT_Init_FreeType(&value);
            return value;
        }();
        static const FT_Face              face    = [] {
            FT_Face value = nullptr;
            FT_New_Memory_Face(library, data.data(), FT_Long(data.size()), 0, &value);
            return value;
        }();

        // Same request and flags as FontFT, so both renders are bit identical.
        FT_Size_RequestRec request {};
        request.type   = FT_SIZE_REQUEST_TYPE_SCALES;
        request.width  = FT_Long(::lround(double(scale) * 64.0 * 65536.0));
        request.height = request.width;
        FT_Request_Size(face, &request);
        FT_Load_Glyph(face, FT_Get_Char_Index(face, codePoint), FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);

        const FT_Bitmap &source = face->glyph->bitmap;
        Bitmap           bitmap;
        bitmap.width  = int(source.width);
        bitmap.height = int(source.rows);
        bitmap.x      = face->glyph->bitmap_left;
        bitmap.y      = -face->glyph->bitmap_top;
        bitmap.pixels.resize(size_t(bitmap.width) * size_t(bitmap.height));
        for(int y = 0; y < bitmap.height; ++y) {
            memcpy(&bitmap.pixels[size_t(y) * size_t(bitmap.width)], source.buffer + size_t(y) * size_t(source.pitch), size_t(bitmap.width));
        }

        return bitmap;
    }
#endif

} // end of namespace

//-------------------------------------
TEST_CASE_TEMPLATE("Font draws every glyph exactly as its rasterizer renders it", TFont, FONT_BACKENDS) {
    struct Atlas {
        uint32_t                        spacing;
        Font::ELevelChoiceHeuristic     heuristic;
        Font::ETextureGrowth            growth;
    };

    // Each layout packs, and so rotates, a different set of glyphs.
    const Atlas atlases[] = {
        { 0, Font::ELevelChoiceHeuristic::LevelMinWasteFit, Font::ETextureGrowth::Height },
        { 1, Font::ELevelChoiceHeuristic::LevelBottomLeft,  Font::ETextureGrowth::Width  },
        { 3, Font::ELevelChoiceHeuristic::LevelMinWasteFit, Font::ETextureGrowth::Both   },
    };

    const int kWidth = 128, kHeight = 128, kPosX = 40, kPosY = 20;
    int       rotated = 0;
    for(const Atlas &atlas : atlases) {
        TFont font;
        REQUIRE(font.SetGlyphSpacing(atlas.spacing));
        font.SetPackingHeuristic(atlas.heuristic);
        font.SetTextureGrowth(atlas.growth);

        for(int height : { 9, 23, 47, 64 }) {
            const HeightData &heightData = font.GetDataForHeight(uint8_t(height));
            for(uint32_t codePoint = 33; codePoint < 127; ++codePoint) {
                CAPTURE(atlas.spacing);
                CAPTURE(height);
                CAPTURE(codePoint);

                const char            text[] = { char(codePoint), 0 };
                std::vector<uint32_t> buffer(kWidth * kHeight, 0);
                font.DrawText(text, uint8_t(height), kWhite, buffer.data(), kWidth, kPosX, kPosY);
                rotated += font.GetCodePointDataForHeight(codePoint, uint8_t(height)).rotated ? 1 : 0;

                const Bitmap glyph   = Rasterize(font, codePoint, heightData.scale);
                const int    originX = kPosX + glyph.x;
                const int    originY = kPosY + heightData.ascent + glyph.y;
                int          wrongPixels = 0;
                for(int y = 0; y < kHeight; ++y) {
                    for(int x = 0; x < kWidth; ++x) {
                        const int     glyphX   = x - originX;
                        const int     glyphY   = y - originY;
                        const bool    inGlyph  = glyphX >= 0 && glyphY >= 0 && glyphX < glyph.width && glyphY < glyph.height;
                        const uint8_t expected = inGlyph ? glyph.pixels[size_t(glyphY) * size_t(glyph.width) + size_t(glyphX)] : 0;
                        if((buffer[size_t(y) * kWidth + size_t(x)] & 0xff) != expected) {
                            ++wrongPixels;
                        }
                    }
                }
                CHECK(wrongPixels == 0);
            }
        }
    }

    CHECK(rotated > 0);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font clipping draws exactly the part of the text inside the clip rectangle", TFont, FONT_BACKENDS) {
    const char *kText   = "AVgjpq Wa 1f.T\nLine two gy";
    const int  kWidth   = 420, kHeight = 120, kPosX = 20, kPosY = 10;
    const auto kTextHeight = uint8_t(36);

    TFont font;
    font.SetAntialias(true);
    font.SetAntialiasAllowEx(true);

    std::vector<uint32_t> reference(kWidth * kHeight, 0);
    font.DrawText(kText, kTextHeight, kWhite, reference.data(), kWidth, kPosX, kPosY);
    REQUIRE(std::count(reference.begin(), reference.end(), 0u) < std::ptrdiff_t(reference.size()));

    std::mt19937 rng(5);
    for(int i = 0; i < 200; ++i) {
        // DrawText does not know the buffer size, so the clip rectangle has to keep it inside.
        int32_t left   = int32_t(rng() % (kWidth  + 1));
        int32_t right  = int32_t(rng() % (kWidth  + 1));
        int32_t top    = int32_t(rng() % (kHeight + 1));
        int32_t bottom = int32_t(rng() % (kHeight + 1));
        // Some rectangles are left inverted, which must draw nothing.
        if(i % 4 != 0) {
            if(left > right) {
                std::swap(left, right);
            }
            if(top > bottom) {
                std::swap(top, bottom);
            }
        }
        CAPTURE(left);
        CAPTURE(top);
        CAPTURE(right);
        CAPTURE(bottom);

        std::vector<uint32_t> buffer(kWidth * kHeight, 0);
        font.SetClipping(left, top, right, bottom);
        font.DrawText(kText, kTextHeight, kWhite, buffer.data(), kWidth, kPosX, kPosY);

        int wrongPixels = 0;
        for(int y = 0; y < kHeight; ++y) {
            for(int x = 0; x < kWidth; ++x) {
                const bool     inside   = x >= left && x < right && y >= top && y < bottom;
                const size_t   offset   = size_t(y) * kWidth + size_t(x);
                const uint32_t expected = inside ? reference[offset] : 0u;
                if(buffer[offset] != expected) {
                    ++wrongPixels;
                }
            }
        }
        CHECK(wrongPixels == 0);
    }
}
