#include "testHelpers.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <random>
#include <vector>

using namespace MindShake;
using MindShake::Test::Inspectable;

//-------------------------------------
namespace {

    const char *kTexts[] = {
        "Hello",
        "AVgjpq Wa 1f.T",
        "Line one\nsecond gy\n\nlast",
        "  x  ",
        "j",
        "\xc2\xbf" "C\xc3\xb3" "mo est\xc3\xa1s?",
    };

    //---------------------------------
    template <class TFont>
    void
    RenderAscii(TFont &font, std::initializer_list<int> heights) {
        for(int height : heights) {
            for(uint32_t codePoint = 33; codePoint < 127; ++codePoint)
                font.GetCodePointDataForHeight(codePoint, uint8_t(height));
        }
    }

    // Counts texels closer than the padding to a glyph, on any side, that are not empty or fall outside the texture.
    //---------------------------------
    template <class TFont>
    int
    CountPaddingErrors(const TFont &font) {
        const int      padding = int(font.GetGlyphPadding());
        const int      width   = int(font.GetTextureWidth());
        const int      height  = int(font.GetTextureHeight());
        const uint8_t  *texels = font.GetTexture();
        int            errors  = 0;

        for(const auto &entry : font.mCodePointHeightData) {
            if(entry.second.glyph <= 0) {
                continue;
            }

            const Font::Rect &rect = entry.second.rect;
            for(int y = rect.top() - padding; y < rect.bottom() + padding; ++y) {
                for(int x = rect.left() - padding; x < rect.right() + padding; ++x) {
                    if(x < 0 || y < 0 || x >= width || y >= height) {
                        ++errors;
                        continue;
                    }
                    const bool inGlyph = x >= rect.left() && x < rect.right() && y >= rect.top() && y < rect.bottom();
                    if(!inGlyph && texels[y * width + x] != 0) {
                        ++errors;
                    }
                }
            }
        }

        return errors;
    }

    // Coverage weighted center, for opaque white text drawn on a black buffer.
    //---------------------------------
    void
    FindInkCenter(const std::vector<uint32_t> &buffer, int width, double &x, double &y) {
        double sum = 0.0, sumX = 0.0, sumY = 0.0;
        for(size_t i = 0; i < buffer.size(); ++i) {
            const double coverage = double(buffer[i] & 0xff);
            sum  += coverage;
            sumX += coverage * double(i % size_t(width));
            sumY += coverage * double(i / size_t(width));
        }
        x = (sum > 0.0) ? sumX / sum : 0.0;
        y = (sum > 0.0) ? sumY / sum : 0.0;
    }

} // end of namespace

//-------------------------------------
TEST_CASE_TEMPLATE("Font loads the test font", TFont, FONT_BACKENDS) {
    TFont font;

    REQUIRE(font.GetStatus() == Font::EStatus::Ok);
    CHECK(font.GetTextureWidth()  == 512);
    CHECK(font.GetTextureHeight() == 128);
    CHECK(font.GetUsedTextureWidth()  == 0);
    CHECK(font.GetUsedTextureHeight() == 0);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font reports a missing file", TFont, FONT_BACKENDS) {
    TFont font("this-font-does-not-exist.ttf");

    CHECK(font.GetStatus() == Font::EStatus::CannotOpenFile);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font reports a file that is not a font", TFont, FONT_BACKENDS) {
    // Any file that is not a font, like this source file.
    TFont font(__FILE__);

    CHECK(font.GetStatus() == Font::EStatus::InvalidFont);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font reports a path that is not a file", TFont, FONT_BACKENDS) {
    TFont font(Test::kResourcesPath);

    CHECK(font.GetStatus() == Font::EStatus::CannotOpenFile);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font survives a font whose metrics give a zero scale", TFont, FONT_BACKENDS) {
    // As with a broken 'hhea' table: without line height every glyph is rendered at scale 0.
    TFont font;
    font.mAscent  = 0;
    font.mDescent = 0;
    font.SetAntialias(true);

    std::vector<uint32_t> buffer(64 * 64, 0);
    font.DrawText("Ag", 32, 0xffffffffu, buffer.data(), 64, 16, 16);
    Font::Rect box;
    font.GetTextBox("Ag", 32, &box);

    CHECK(std::count(buffer.begin(), buffer.end(), 0u) == std::ptrdiff_t(buffer.size()));
    CHECK(box.width  == 0);
    CHECK(box.height == 0);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font keeps the advance of a glyph that does not fit in the texture", TFont, FONT_BACKENDS) {
    // With the texture height fixed at 128, a 255 pixel 'W' fits in neither orientation, while the dot does.
    TFont fixedHeight;
    fixedHeight.SetTextureGrowth(Font::ETextureGrowth::Width);
    TFont growing;

    Font::Rect expected, box;
    growing.GetTextBox("W.", 255, &expected);
    fixedHeight.GetTextBox("W.", 255, &box);

    const CodePointHeightData &data = fixedHeight.GetCodePointDataForHeight('W', 255);
    CHECK(data.glyph > 0);
    CHECK(data.GetWidth() == 0);
    CHECK(box.right() == expected.right());
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font text box only grows sideways for glyphs without pixels", TFont, FONT_BACKENDS) {
    TFont      font;
    Font::Rect solid, pair, spaced, leading, trailing, blank;
    font.GetTextBox("HHH", 32, &solid);
    font.GetTextBox("HH", 32, &pair);
    font.GetTextBox("H H", 32, &spaced);
    font.GetTextBox("  HHH", 32, &leading);
    font.GetTextBox("HHH  ", 32, &trailing);
    font.GetTextBox("   ", 32, &blank);

    CHECK(spaced.top()      == solid.top());
    CHECK(spaced.bottom()   == solid.bottom());
    CHECK(spaced.right()    >  pair.right());
    CHECK(leading.left()    == 0);
    CHECK(leading.right()   >  solid.right());
    CHECK(leading.bottom()  == solid.bottom());
    CHECK(trailing.top()    == solid.top());
    CHECK(trailing.bottom() == solid.bottom());
    CHECK(trailing.right()  >  solid.right());
    CHECK(blank.width  == 0);
    CHECK(blank.height == 0);
}

//-------------------------------------
TEST_CASE("Font keeps packed glyphs intact while the texture grows") {
    for(Font::ETextureGrowth growth : { Font::ETextureGrowth::Height, Font::ETextureGrowth::Width, Font::ETextureGrowth::Both }) {
        CAPTURE(int(growth));

        Inspectable<FontSTB> font;
        REQUIRE(font.GetStatus() == Font::EStatus::Ok);
        font.SetTextureGrowth(growth);

        struct Packed {
            CodePointHeightData     data;
            uint32_t                width;
            uint32_t                height;
            std::vector<uint8_t>    pixels;
        };

        std::mt19937        rng(7);
        std::vector<Packed> packed;
        for(int i = 0; i < 1500; ++i) {
            Packed glyph {};
            glyph.width  = 1 + rng() % 40;
            glyph.height = 1 + rng() % 40;
            glyph.pixels.resize(glyph.width * glyph.height);
            for(uint8_t &pixel : glyph.pixels)
                pixel = uint8_t(1 + rng() % 255);

            REQUIRE(font.PackGlyph(glyph.pixels.data(), glyph.width, glyph.height, glyph.data));
            packed.push_back(std::move(glyph));
        }

        // Glyphs packed rotated are stored transposed.
        const size_t  textureWidth = font.GetTextureWidth();
        const uint8_t *texels      = font.GetTexture();
        int           wrongPixels  = 0;
        int           rotated      = 0;
        for(const Packed &glyph : packed) {
            const Font::Rect &rect = glyph.data.rect;
            rotated += glyph.data.rotated ? 1 : 0;
            for(uint32_t y = 0; y < glyph.height; ++y) {
                for(uint32_t x = 0; x < glyph.width; ++x) {
                    const size_t tx = size_t(rect.x) + (glyph.data.rotated ? y : x);
                    const size_t ty = size_t(rect.y) + (glyph.data.rotated ? x : y);
                    if(texels[ty * textureWidth + tx] != glyph.pixels[y * glyph.width + x]) {
                        ++wrongPixels;
                    }
                }
            }
        }

        CHECK(wrongPixels == 0);
        CHECK(rotated > 0);
        CHECK(font.GetTextureWidth()  <= Font::kMaxTextureSize);
        CHECK(font.GetTextureHeight() <= Font::kMaxTextureSize);
    }
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font keeps the padding around every glyph empty", TFont, FONT_BACKENDS) {
    for(uint32_t padding : { 0u, 1u, 3u }) {
        for(Font::ETextureGrowth growth : { Font::ETextureGrowth::Height, Font::ETextureGrowth::Width, Font::ETextureGrowth::Both }) {
            CAPTURE(padding);
            CAPTURE(int(growth));

            TFont font;
            font.SetTextureGrowth(growth);
            font.SetAntialias(padding == 3);
            font.SetAntialiasAllowEx(padding == 3);
            RenderAscii(font, { 30 });

            // Changing the padding discards the glyphs; 1 is the default, so it keeps them.
            REQUIRE(font.SetGlyphPadding(padding));
            CHECK(font.GetGlyphCount() == (padding == 1 ? 94u : 0u));

            RenderAscii(font, { 8, 24, 40, 56, 72 });
            CHECK(CountPaddingErrors(font) == 0);
        }
    }
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font rejects a padding that leaves no room", TFont, FONT_BACKENDS) {
    TFont font;

    CHECK_FALSE(font.SetGlyphPadding(font.GetTextureHeight()));
    CHECK(font.GetGlyphPadding() == 1);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font text box contains every drawn pixel", TFont, FONT_BACKENDS) {
    TFont font;
    font.SetAntialias(true);
    font.SetAntialiasAllowEx(true);

    const int kWidth = 900, kHeight = 400, kPosX = 100, kPosY = 80;
    for(const char *text : kTexts) {
        for(int height : { 12, 32, 57 }) {
            CAPTURE(text);
            CAPTURE(height);

            std::vector<uint32_t> buffer(kWidth * kHeight, 0);
            font.DrawText(text, uint8_t(height), 0xffffffffu, buffer.data(), kWidth, kPosX, kPosY);

            int minX = kWidth, minY = kHeight, maxX = -1, maxY = -1;
            for(int y = 0; y < kHeight; ++y) {
                for(int x = 0; x < kWidth; ++x) {
                    if((buffer[y * kWidth + x] & 0xffffff) != 0) {
                        minX = std::min(minX, x);
                        minY = std::min(minY, y);
                        maxX = std::max(maxX, x);
                        maxY = std::max(maxY, y);
                    }
                }
            }
            REQUIRE(maxX >= 0);

            Font::Rect box;
            font.GetTextBox(text, uint8_t(height), &box);
            CHECK(minX >= kPosX + box.left());
            CHECK(minY >= kPosY + box.top());
            CHECK(maxX <  kPosX + box.right());
            CHECK(maxY <  kPosY + box.bottom());
        }
    }
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font text box of an empty text is empty", TFont, FONT_BACKENDS) {
    TFont      font;
    Font::Rect box { 1, 2, 3, 4 };

    font.GetTextBox("", 32, &box);
    CHECK(box.width  == 0);
    CHECK(box.height == 0);

    font.GetTextBox("\n\n", 32, &box);
    CHECK(box.width  == 0);
    CHECK(box.height == 0);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font size modes", TFont, FONT_BACKENDS) {
    TFont font;
    REQUIRE(font.mUnitsPerEm > 0);

    for(int height = 8; height <= 96; height += 8) {
        CAPTURE(height);
        const HeightData &lineHeight = font.GetDataForHeight(uint8_t(height));
        // Ascent and descent are rounded outwards, so the line can be one pixel taller.
        CHECK(lineHeight.ascent - lineHeight.descent >= height);
        CHECK(lineHeight.ascent - lineHeight.descent <= height + 1);
    }

    font.SetSizeMode(Font::ESizeMode::EmSize);
    for(int height = 8; height <= 96; height += 8) {
        CAPTURE(height);
        CHECK(font.GetDataForHeight(uint8_t(height)).scale == doctest::Approx(float(height) / float(font.mUnitsPerEm)));
    }
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font discards glyphs only when a setting changes", TFont, FONT_BACKENDS) {
    TFont font;
    RenderAscii(font, { 20 });
    REQUIRE(font.GetGlyphCount() == 94);

    font.SetAntialias(font.GetAntialias());
    font.SetAntialiasAllowEx(font.GetAntialiasAllowEx());
    font.SetAntialiasWeights(font.GetAntialiasCenter(), font.GetAntialiasBorder(), font.GetAntialiasCorner());
    font.SetSizeMode(font.GetSizeMode());
    font.SetGlyphPadding(font.GetGlyphPadding());
    CHECK(font.GetGlyphCount() == 94);

    font.SetAntialias(!font.GetAntialias());
    CHECK(font.GetGlyphCount() == 0);

    RenderAscii(font, { 20 });
    font.SetAntialiasAllowEx(!font.GetAntialiasAllowEx());
    CHECK(font.GetGlyphCount() == 0);

    RenderAscii(font, { 20 });
    font.SetAntialiasWeights(font.GetAntialiasCenter() + 1, font.GetAntialiasBorder(), font.GetAntialiasCorner());
    CHECK(font.GetGlyphCount() == 0);

    RenderAscii(font, { 20 });
    font.SetSizeMode(Font::ESizeMode::EmSize);
    CHECK(font.GetGlyphCount() == 0);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font rejects antialias weights it cannot apply", TFont, FONT_BACKENDS) {
    TFont font;
    font.SetAntialias(true);
    RenderAscii(font, { 20 });
    const int32_t center = font.GetAntialiasCenter();
    const int32_t border = font.GetAntialiasBorder();
    const int32_t corner = font.GetAntialiasCorner();

    CHECK_FALSE(font.SetAntialiasWeights(0, 0, 0));
    CHECK_FALSE(font.SetAntialiasWeights(-1, 4, 1));
    CHECK_FALSE(font.SetAntialiasWeights(20, 4, Font::kMaxAntialiasWeight + 1));
    CHECK(font.GetAntialiasCenter() == center);
    CHECK(font.GetAntialiasBorder() == border);
    CHECK(font.GetAntialiasCorner() == corner);
    CHECK(font.GetGlyphCount() == 94);

    CHECK(font.SetAntialiasWeights(Font::kMaxAntialiasWeight, Font::kMaxAntialiasWeight, Font::kMaxAntialiasWeight));
    CHECK(font.GetGlyphCount() == 0);
    RenderAscii(font, { 20 });
    CHECK(font.GetGlyphCount() == 94);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font used texture size covers every glyph", TFont, FONT_BACKENDS) {
    for(Font::ELevelChoiceHeuristic heuristic : { Font::ELevelChoiceHeuristic::LevelBottomLeft, Font::ELevelChoiceHeuristic::LevelMinWasteFit }) {
        CAPTURE(int(heuristic));

        TFont font;
        font.SetPackingHeuristic(heuristic);
        RenderAscii(font, { 10, 20, 30, 40, 50 });

        const int usedWidth  = int(font.GetUsedTextureWidth());
        const int usedHeight = int(font.GetUsedTextureHeight());
        const int padding    = int(font.GetGlyphPadding());
        CHECK(usedWidth  <= int(font.GetTextureWidth()));
        CHECK(usedHeight <= int(font.GetTextureHeight()));

        int outside = 0;
        for(const auto &entry : font.mCodePointHeightData) {
            const Font::Rect &rect = entry.second.rect;
            if(entry.second.glyph > 0 && (rect.right() + padding > usedWidth || rect.bottom() + padding > usedHeight)) {
                ++outside;
            }
        }
        CHECK(outside == 0);
        CHECK(CountPaddingErrors(font) == 0);
    }
}

//-------------------------------------
TEST_CASE("Font backends place glyphs the same way") {
    Inspectable<FontSTB> stb;
    Inspectable<FontSFT> sft;

    for(int height = 10; height <= 70; height += 6) {
        for(uint32_t codePoint = 33; codePoint < 127; ++codePoint) {
            CAPTURE(height);
            CAPTURE(codePoint);

            const CodePointHeightData &a = stb.GetCodePointDataForHeight(codePoint, uint8_t(height));
            const CodePointHeightData &b = sft.GetCodePointDataForHeight(codePoint, uint8_t(height));
            REQUIRE(a.glyph > 0);
            REQUIRE(b.glyph > 0);

            // Each rasterizer rounds the glyph bounds on its own, so an edge exactly on a pixel boundary may differ by one.
            CHECK(std::abs(a.x - b.x) <= 1);
            CHECK(std::abs(a.y - b.y) <= 1);
            CHECK(std::abs(a.GetWidth()  - b.GetWidth())  <= 1);
            CHECK(std::abs(a.GetHeight() - b.GetHeight()) <= 1);
            CHECK(a.advanceWidth == doctest::Approx(b.advanceWidth).epsilon(0.001));
        }
    }
}

//-------------------------------------
TEST_CASE("Font backends draw every glyph in the same place") {
    Inspectable<FontSTB> stb(Test::kItalicFontPath);
    Inspectable<FontSFT> sft(Test::kItalicFontPath);
    REQUIRE(stb.GetStatus() == Font::EStatus::Ok);
    REQUIRE(sft.GetStatus() == Font::EStatus::Ok);

    const int kWidth = 160, kHeight = 160, kPosX = 50, kPosY = 20;
    for(int height = 8; height <= 96; height += 4) {
        for(uint32_t codePoint = 33; codePoint < 127; ++codePoint) {
            CAPTURE(height);
            CAPTURE(codePoint);

            const char            text[] = { char(codePoint), 0 };
            std::vector<uint32_t> bufferSTB(kWidth * kHeight, 0);
            std::vector<uint32_t> bufferSFT(kWidth * kHeight, 0);
            stb.DrawText(text, uint8_t(height), 0xffffffffu, bufferSTB.data(), kWidth, kPosX, kPosY);
            sft.DrawText(text, uint8_t(height), 0xffffffffu, bufferSFT.data(), kWidth, kPosX, kPosY);

            // Faint edge pixels, rounded differently by each rasterizer, barely move the center; a misplaced glyph moves it a pixel.
            double stbX, stbY, sftX, sftY;
            FindInkCenter(bufferSTB, kWidth, stbX, stbY);
            FindInkCenter(bufferSFT, kWidth, sftX, sftY);
            CHECK(std::abs(stbX - sftX) < 0.5);
            CHECK(std::abs(stbY - sftY) < 0.5);
        }
    }
}

//-------------------------------------
TEST_CASE("FontSTB reads kerning from GPOS") {
    // The test font only has kerning in GPOS, which libschrift cannot read, so only STB is checked.
    Inspectable<FontSTB> font;
    const uint32_t       a = font.GetCodePointGlyph('A');
    const uint32_t       v = font.GetCodePointGlyph('V');

    CHECK(font.GetKerning(a, v) < 0);
    CHECK(font.GetKerning(a, a) == 0);
}
