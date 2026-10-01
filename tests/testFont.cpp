#include "testHelpers.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <random>
#include <string>
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

    // Counts texels closer than the spacing to a glyph, on any side, that are not empty or fall outside the texture.
    //---------------------------------
    template <class TFont>
    int
    CountSpacingErrors(const TFont &font) {
        const int      spacing = int(font.GetGlyphSpacing());
        const int      width   = int(font.GetTextureWidth());
        const int      height  = int(font.GetTextureHeight());
        const uint8_t  *texels = font.GetTexture();
        int            errors  = 0;

        for(const auto &entry : font.mCodePointHeightData) {
            if(entry.second.glyph <= 0 || entry.second.GetWidth() == 0) {
                continue;
            }

            const Font::Rect &rect = entry.second.rect;
            for(int y = rect.top() - spacing; y < rect.bottom() + spacing; ++y) {
                for(int x = rect.left() - spacing; x < rect.right() + spacing; ++x) {
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

        Inspectable<Test::DefaultFont> font;
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
TEST_CASE_TEMPLATE("Font keeps the spacing around every glyph empty", TFont, FONT_BACKENDS) {
    for(uint32_t spacing : { 0u, 1u, 3u }) {
        for(Font::ETextureGrowth growth : { Font::ETextureGrowth::Height, Font::ETextureGrowth::Width, Font::ETextureGrowth::Both }) {
            CAPTURE(spacing);
            CAPTURE(int(growth));

            TFont font;
            font.SetTextureGrowth(growth);
            font.SetAntialias(spacing == 3);
            font.SetAntialiasAllowEx(spacing == 3);
            RenderAscii(font, { 30 });

            // Changing the spacing discards the glyphs; 1 is the default, so it keeps them.
            REQUIRE(font.SetGlyphSpacing(spacing));
            CHECK(font.GetGlyphCount() == (spacing == 1 ? 94u : 0u));

            RenderAscii(font, { 8, 24, 40, 56, 72 });
            CHECK(CountSpacingErrors(font) == 0);
        }
    }
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font rejects a spacing that leaves no room", TFont, FONT_BACKENDS) {
    TFont font;

    CHECK_FALSE(font.SetGlyphSpacing(font.GetTextureHeight()));
    CHECK(font.GetGlyphSpacing() == 1);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font glyph padding grows every glyph without changing what is drawn", TFont, FONT_BACKENDS) {
    const uint32_t kLeft = 2, kTop = 3, kRight = 4, kBottom = 5;
    const int      kWidth = 900, kHeight = 400, kPosX = 30, kPosY = 20;

    for(bool antialias : { false, true }) {
        CAPTURE(antialias);
        TFont plain;
        TFont padded;
        for(TFont *font : { &plain, &padded }) {
            font->SetAntialias(antialias);
            font->SetAntialiasAllowEx(antialias);
        }
        REQUIRE(padded.SetGlyphPadding(kLeft, kTop, kRight, kBottom));
        CHECK(padded.GetGlyphPaddingLeft()   == kLeft);
        CHECK(padded.GetGlyphPaddingTop()    == kTop);
        CHECK(padded.GetGlyphPaddingRight()  == kRight);
        CHECK(padded.GetGlyphPaddingBottom() == kBottom);

        for(const char *text : kTexts) {
            for(int height : { 12, 57 }) {
                CAPTURE(text);
                CAPTURE(height);

                // Preload goes through its own packing path, so it is checked too.
                REQUIRE(padded.Preload(text, uint8_t(height)));

                // The padding is empty, so it draws nothing.
                std::vector<uint32_t> expected(kWidth * kHeight, 0);
                std::vector<uint32_t> buffer(kWidth * kHeight, 0);
                plain.DrawText(text, uint8_t(height), 0xffffffffu, expected.data(), kWidth, kPosX, kPosY);
                padded.DrawText(text, uint8_t(height), 0xffffffffu, buffer.data(), kWidth, kPosX, kPosY);
                CHECK(buffer == expected);

                std::vector<GlyphQuad> expectedQuads, quads;
                plain.GetGlyphQuads(text, uint8_t(height), expectedQuads);
                padded.GetGlyphQuads(text, uint8_t(height), quads);
                REQUIRE(quads.size() == expectedQuads.size());
                for(size_t i = 0; i < quads.size(); ++i) {
                    CHECK(quads[i].x      == expectedQuads[i].x - int32_t(kLeft));
                    CHECK(quads[i].y      == expectedQuads[i].y - int32_t(kTop));
                    CHECK(quads[i].width  == expectedQuads[i].width  + int32_t(kLeft + kRight));
                    CHECK(quads[i].height == expectedQuads[i].height + int32_t(kTop + kBottom));
                }

                Font::Rect expectedBox, box;
                plain.GetTextBox(text, uint8_t(height), &expectedBox);
                padded.GetTextBox(text, uint8_t(height), &box);
                CHECK(box.top()    == expectedBox.top()    - int32_t(kTop));
                CHECK(box.bottom() == expectedBox.bottom() + int32_t(kBottom));
                CHECK(box.left()   <= expectedBox.left());
                CHECK(box.right()  >= expectedBox.right());
            }
        }
        CHECK(CountSpacingErrors(padded) == 0);
    }
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font rejects a glyph padding that would not fit in any texture", TFont, FONT_BACKENDS) {
    TFont font;
    REQUIRE(font.SetGlyphPadding(1, 2, 3, 4));

    CHECK_FALSE(font.SetGlyphPadding(Font::kMaxTextureSize, 0, 0, 0));
    CHECK_FALSE(font.SetGlyphPadding(Font::kMaxTextureSize / 2, 0, Font::kMaxTextureSize / 2, 0));
    CHECK_FALSE(font.SetGlyphPadding(0, UINT32_MAX, 0, 1));
    CHECK(font.GetGlyphPaddingLeft()   == 1);
    CHECK(font.GetGlyphPaddingTop()    == 2);
    CHECK(font.GetGlyphPaddingRight()  == 3);
    CHECK(font.GetGlyphPaddingBottom() == 4);

    CHECK(font.SetGlyphPadding(Font::kMaxTextureSize / 2, 0, Font::kMaxTextureSize / 2 - 1, 0));
}

//-------------------------------------
TEST_CASE("Font without rotation never stores a glyph transposed") {
    // With the texture height fixed at 128, a glyph 200 pixels tall only fits rotated.
    Inspectable<Test::DefaultFont> font;
    font.SetTextureGrowth(Font::ETextureGrowth::Width);
    const std::vector<uint8_t> pixels(10 * 200, 255);
    CodePointHeightData        data {};
    CHECK(font.GetAllowRotation());
    CHECK(font.PackGlyph(pixels.data(), 10, 200, data));
    CHECK(data.rotated);

    font.Reset();
    font.SetAllowRotation(false);
    CHECK_FALSE(font.GetAllowRotation());
    CHECK_FALSE(font.PackGlyph(pixels.data(), 10, 200, data));

    RenderAscii(font, { 9, 23, 47, 64 });
    int rotated = 0;
    for(const auto &entry : font.mCodePointHeightData) {
        rotated += entry.second.rotated ? 1 : 0;
    }
    CHECK(rotated == 0);
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
    font.SetGlyphSpacing(font.GetGlyphSpacing());
    font.SetGlyphPadding(font.GetGlyphPaddingLeft(), font.GetGlyphPaddingTop(), font.GetGlyphPaddingRight(), font.GetGlyphPaddingBottom());
    font.SetAllowRotation(!font.GetAllowRotation());
    font.SetPackingHeuristic(Font::ELevelChoiceHeuristic::LevelMinWasteFit);
    CHECK(font.GetGlyphCount() == 94);

    font.SetGlyphPadding(0, 0, 0, 1);
    CHECK(font.GetGlyphCount() == 0);

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
        const int spacing    = int(font.GetGlyphSpacing());
        CHECK(usedWidth  <= int(font.GetTextureWidth()));
        CHECK(usedHeight <= int(font.GetTextureHeight()));

        int outside = 0;
        for(const auto &entry : font.mCodePointHeightData) {
            const Font::Rect &rect = entry.second.rect;
            if(entry.second.glyph > 0 && (rect.right() + spacing > usedWidth || rect.bottom() + spacing > usedHeight)) {
                ++outside;
            }
        }
        CHECK(outside == 0);
        CHECK(CountSpacingErrors(font) == 0);
    }
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font reads kerning from GPOS", TFont, FONT_BACKENDS) {
    // The test font only has kerning in GPOS.
    TFont          font;
    const uint32_t a = font.GetCodePointGlyph('A');
    const uint32_t v = font.GetCodePointGlyph('V');

    CHECK(font.GetKerning(a, v) < 0);
    CHECK(font.GetKerning(a, a) == 0);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font preload draws the same as rendering each glyph when it is drawn", TFont, FONT_BACKENDS) {
    std::string text;
    for(char c = ' '; c <= '~'; ++c) {
        text += c;
    }
    for(const char *sample : kTexts) {
        text += sample;
    }

    TFont preloaded;
    TFont lazy;
    for(TFont *font : { &preloaded, &lazy }) {
        font->SetAntialias(true);
        font->SetAntialiasAllowEx(true);
    }

    // At 90 pixels the glyphs do not fit in the first texture, so it has to grow.
    for(int height : { 17, 90 }) {
        CAPTURE(height);
        const uint32_t textureHeight = preloaded.GetTextureHeight();
        REQUIRE(preloaded.Preload(text.c_str(), uint8_t(height)));
        CHECK(preloaded.GetTextureHeight() >= textureHeight);

        const uint32_t version = preloaded.GetTextureVersion();
        for(const char *text : kTexts) {
            CAPTURE(text);
            std::vector<uint32_t> expected(900 * 400, 0);
            std::vector<uint32_t> buffer(900 * 400, 0);
            lazy.DrawText(text, uint8_t(height), 0xffffffffu, expected.data(), 900, 20, 20);
            preloaded.DrawText(text, uint8_t(height), 0xffffffffu, buffer.data(), 900, 20, 20);
            CHECK(buffer == expected);
        }
        CHECK(preloaded.GetTextureVersion() == version);
    }

    CHECK(preloaded.GetTextureHeight() > 128);
    // The 95 printable ASCII characters, and ¿, ó and á, at two heights.
    CHECK(preloaded.GetGlyphCount() == 2 * 98);
    CHECK(CountSpacingErrors(preloaded) == 0);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font skips the code points the font does not have", TFont, FONT_BACKENDS) {
    // U+E000 is private use, and U+10FFFF is not a character.
    const char *kMissing = "H\xee\x80\x80H\xf4\x8f\xbf\xbf";
    TFont      font;
    REQUIRE(font.GetCodePointDataForHeight(0xE000, 30).glyph == 0);
    REQUIRE(font.GetCodePointDataForHeight(0x10FFFF, 30).glyph == 0);

    CHECK(font.Preload(kMissing, 30));
    CHECK(font.GetGlyphCount() == 1);

    std::vector<uint32_t> expected(200 * 60, 0);
    std::vector<uint32_t> buffer(200 * 60, 0);
    font.DrawText("HH", 30, 0xffffffffu, expected.data(), 200, 4, 4);
    font.DrawText(kMissing, 30, 0xffffffffu, buffer.data(), 200, 4, 4);
    CHECK(buffer == expected);

    Font::Rect expectedBox, box;
    font.GetTextBox("HH", 30, &expectedBox);
    font.GetTextBox(kMissing, 30, &box);
    CHECK(box.x     == expectedBox.x);
    CHECK(box.width == expectedBox.width);

    std::vector<GlyphQuad> quads;
    font.GetGlyphQuads(kMissing, 30, quads);
    CHECK(quads.size() == 2);
    CHECK(font.GetGlyphCount() == 1);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font preload reports glyphs that do not fit", TFont, FONT_BACKENDS) {
    // With the texture height fixed at 128, a 255 pixel 'W' fits in neither orientation, while the dot does.
    TFont font;
    font.SetTextureGrowth(Font::ETextureGrowth::Width);

    CHECK_FALSE(font.Preload("W.", 255));
    CHECK(font.GetCodePointDataForHeight('W', 255).glyph > 0);
    CHECK(font.GetCodePointDataForHeight('W', 255).GetWidth() == 0);
    CHECK(font.GetCodePointDataForHeight('.', 255).GetWidth() > 0);
    CHECK(font.GetGlyphCount() == 2);

    CHECK(font.Preload("", 20));
    CHECK(font.Preload(nullptr, 20));
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font loads the kerning of every pair of rendered glyphs", TFont, FONT_BACKENDS) {
    TFont font;
    REQUIRE(font.Preload("AVTaeoy.,", 20));
    font.LoadAllKerningPairs();

    int kernedPairs = 0;
    for(const char left : std::string("AVTaeoy.,")) {
        for(const char right : std::string("AVTaeoy.,")) {
            CAPTURE(left);
            CAPTURE(right);
            const uint32_t leftGlyph  = font.GetCodePointGlyph(uint32_t(left));
            const uint32_t rightGlyph = font.GetCodePointGlyph(uint32_t(right));
            const int      kerning    = font.LookUpKerning(leftGlyph, rightGlyph);
            const auto     cached     = font.mKerningData.find((uint64_t(leftGlyph) << 32) | rightGlyph);
            // Pairs without kerning are not kept.
            CHECK((cached != font.mKerningData.end()) == (kerning != 0));
            if(cached != font.mKerningData.end()) {
                CHECK(cached->second == kerning);
                ++kernedPairs;
            }
        }
    }
    CHECK(kernedPairs > 10);
}

//-------------------------------------
TEST_CASE("Font texture version changes only with the texels") {
    Inspectable<Test::DefaultFont> font;
    std::vector<uint32_t>          buffer(64 * 64, 0);

    const uint32_t loaded = font.GetTextureVersion();
    font.DrawText("A", 20, 0xffffffffu, buffer.data(), 64, 0, 0);
    const uint32_t drawn = font.GetTextureVersion();
    CHECK(drawn != loaded);

    font.DrawText("A", 20, 0xffffffffu, buffer.data(), 64, 0, 0);
    font.DrawText(" ", 20, 0xffffffffu, buffer.data(), 64, 0, 0);
    CHECK(font.GetTextureVersion() == drawn);

    font.Reset();
    CHECK(font.GetTextureVersion() != drawn);
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font glyph quads place the texels where DrawText draws them", TFont, FONT_BACKENDS) {
    TFont font;
    font.SetAntialias(true);
    font.SetAntialiasAllowEx(true);

    const int kWidth = 900, kHeight = 400, kPosX = 30, kPosY = 20;
    int       rotated = 0;
    for(const char *text : kTexts) {
        for(int height : { 12, 32, 57 }) {
            CAPTURE(text);
            CAPTURE(height);

            std::vector<uint32_t> expected(kWidth * kHeight, 0);
            font.DrawText(text, uint8_t(height), 0xffffffffu, expected.data(), kWidth, kPosX, kPosY);

            // Blends white as DrawText does, so overlapping glyphs give the same result. DrawText also blends the alpha
            // of the buffer, which starts transparent, so the four channels of a pixel stay equal: not 255 in the alpha.
            std::vector<GlyphQuad> quads;
            font.GetGlyphQuads(text, uint8_t(height), quads);
            std::vector<uint32_t> buffer(kWidth * kHeight, 0);
            const uint8_t         *texels      = font.GetTexture();
            const size_t          textureWidth = font.GetTextureWidth();
            for(const GlyphQuad &quad : quads) {
                rotated += quad.rotated ? 1 : 0;
                for(int y = 0; y < quad.height; ++y) {
                    for(int x = 0; x < quad.width; ++x) {
                        const size_t  tx       = size_t(quad.textureRect.x + (quad.rotated ? y : x));
                        const size_t  ty       = size_t(quad.textureRect.y + (quad.rotated ? x : y));
                        const uint8_t coverage = texels[ty * textureWidth + tx];
                        if(coverage != 0) {
                            uint32_t       &pixel   = buffer[size_t(kPosY + quad.y + y) * kWidth + size_t(kPosX + quad.x + x)];
                            const uint32_t previous = pixel & 0xff;
                            // Divided by 255 and rounded once, as Blinn does.
                            const uint32_t sum      = 255 * coverage + previous * (255 - coverage) + 128;
                            const uint32_t channel  = (sum + (sum >> 8)) >> 8;
                            pixel = channel * 0x01010101u;
                        }
                    }
                }
            }
            CHECK(buffer == expected);
        }
    }

    CHECK(rotated > 0);
}

#if defined(FONT_OTHER_BACKENDS)

//-------------------------------------
TEST_CASE_TEMPLATE("Font backends place glyphs the same way", TFont, FONT_OTHER_BACKENDS) {
    Inspectable<Test::DefaultFont> reference;
    TFont                          font;

    for(int height = 10; height <= 70; height += 6) {
        for(uint32_t codePoint = 33; codePoint < 127; ++codePoint) {
            CAPTURE(height);
            CAPTURE(codePoint);

            const CodePointHeightData &a = reference.GetCodePointDataForHeight(codePoint, uint8_t(height));
            const CodePointHeightData &b = font.GetCodePointDataForHeight(codePoint, uint8_t(height));
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
TEST_CASE_TEMPLATE("Font backends draw every glyph in the same place", TFont, FONT_OTHER_BACKENDS) {
    Inspectable<Test::DefaultFont> reference(Test::kItalicFontPath);
    TFont                          font(Test::kItalicFontPath);
    REQUIRE(reference.GetStatus() == Font::EStatus::Ok);
    REQUIRE(font.GetStatus() == Font::EStatus::Ok);

    const int kWidth = 160, kHeight = 160, kPosX = 50, kPosY = 20;
    for(int height = 8; height <= 96; height += 4) {
        for(uint32_t codePoint = 33; codePoint < 127; ++codePoint) {
            CAPTURE(height);
            CAPTURE(codePoint);

            const char            text[] = { char(codePoint), 0 };
            std::vector<uint32_t> referenceBuffer(kWidth * kHeight, 0);
            std::vector<uint32_t> buffer(kWidth * kHeight, 0);
            reference.DrawText(text, uint8_t(height), 0xffffffffu, referenceBuffer.data(), kWidth, kPosX, kPosY);
            font.DrawText(text, uint8_t(height), 0xffffffffu, buffer.data(), kWidth, kPosX, kPosY);

            // Faint edge pixels, rounded differently by each rasterizer, barely move the center; a misplaced glyph moves it a pixel.
            double referenceX, referenceY, x, y;
            FindInkCenter(referenceBuffer, kWidth, referenceX, referenceY);
            FindInkCenter(buffer, kWidth, x, y);
            CHECK(std::abs(referenceX - x) < 0.5);
            CHECK(std::abs(referenceY - y) < 0.5);
        }
    }
}

//-------------------------------------
TEST_CASE_TEMPLATE("Font backends read the same kerning", TFont, FONT_OTHER_BACKENDS) {
    // The italic font has kerning in both GPOS and 'kern'.
    for (const char *fontPath : { Test::kFontPath, Test::kItalicFontPath }) {
        CAPTURE(fontPath);
        Inspectable<Test::DefaultFont> reference(fontPath);
        TFont                          font(fontPath);
        int                            kernedPairs = 0;
        for (uint32_t left = 32; left < 127; ++left) {
            for (uint32_t right = 32; right < 127; ++right) {
                CAPTURE(left);
                CAPTURE(right);
                const int kerning = reference.GetKerning(reference.GetCodePointGlyph(left), reference.GetCodePointGlyph(right));
                CHECK(kerning == font.GetKerning(font.GetCodePointGlyph(left), font.GetCodePointGlyph(right)));
                kernedPairs += (kerning != 0) ? 1 : 0;
            }
        }
        CHECK(kernedPairs > 100);
    }
}

#endif

#if defined(FONTRENDERER_USE_FREETYPE)

//-------------------------------------
namespace {

    // White text on a black buffer, so each pixel keeps the coverage of the text.
    //---------------------------------
    std::vector<uint32_t>
    DrawWhite(Font &font, const char *text, uint8_t height) {
        std::vector<uint32_t> buffer(200 * 60, 0);
        font.DrawText(text, height, 0xffffffffu, buffer.data(), 200, 4, 4);
        return buffer;
    }

    //---------------------------------
    uint64_t
    SumCoverage(const std::vector<uint32_t> &buffer) {
        uint64_t sum = 0;
        for(const uint32_t pixel : buffer) {
            sum += pixel & 0xff;
        }
        return sum;
    }

} // end of namespace

//-------------------------------------
TEST_CASE("FontFT settings discard the rendered glyphs only when they change") {
    Inspectable<FontFT> font;
    const auto          render = [&font] { font.GetCodePointDataForHeight('a', 20); };

    render();
    font.SetHinting(FontFT::EHinting::None);
    font.SetMonochrome(false);
    font.SetStemDarkening(false);
    CHECK(font.GetGlyphCount() == 1);

    font.SetHinting(FontFT::EHinting::Light);
    CHECK(font.GetHinting() == FontFT::EHinting::Light);
    CHECK(font.GetGlyphCount() == 0);

    render();
    font.SetMonochrome(true);
    CHECK(font.GetMonochrome());
    CHECK(font.GetGlyphCount() == 0);

    render();
    font.SetStemDarkening(true);
    CHECK(font.GetStemDarkening());
    CHECK(font.GetGlyphCount() == 0);
}

//-------------------------------------
TEST_CASE("FontFT monochrome draws pixels fully on or off") {
    Inspectable<FontFT> font;
    const auto          isGray = [](uint32_t pixel) { return (pixel & 0xff) != 0 && (pixel & 0xff) != 0xff; };

    std::vector<uint32_t> gray = DrawWhite(font, "Hamburg", 20);
    CHECK(std::count_if(gray.begin(), gray.end(), isGray) > 0);

    for(FontFT::EHinting hinting : { FontFT::EHinting::None, FontFT::EHinting::Light, FontFT::EHinting::Normal, FontFT::EHinting::Auto }) {
        CAPTURE(int(hinting));
        font.SetHinting(hinting);
        font.SetMonochrome(true);
        std::vector<uint32_t> mono = DrawWhite(font, "Hamburg", 20);
        CHECK(std::count_if(mono.begin(), mono.end(), isGray) == 0);
        CHECK(SumCoverage(mono) > 0);
        font.SetMonochrome(false);
    }
}

//-------------------------------------
TEST_CASE("FontFT hinting keeps the advances unless it works sideways") {
    Inspectable<FontFT> font;
    const float         outline = font.GetCodePointDataForHeight('a', 13).advanceWidth;
    REQUIRE(outline != std::floor(outline));

    font.SetHinting(FontFT::EHinting::Light);
    CHECK(font.GetCodePointDataForHeight('a', 13).advanceWidth == outline);

    for(FontFT::EHinting hinting : { FontFT::EHinting::Normal, FontFT::EHinting::Auto }) {
        CAPTURE(int(hinting));
        font.SetHinting(hinting);
        const float hinted = font.GetCodePointDataForHeight('a', 13).advanceWidth;
        CHECK(hinted == std::floor(hinted));
        CHECK(std::abs(hinted - outline) <= 1.0f);
    }
}

//-------------------------------------
TEST_CASE("FontFT hinting changes the glyphs at small sizes") {
    Inspectable<FontFT>         font;
    const std::vector<uint32_t> outline = DrawWhite(font, "Hamburgefonstiv", 11);

    for(FontFT::EHinting hinting : { FontFT::EHinting::Light, FontFT::EHinting::Normal, FontFT::EHinting::Auto }) {
        CAPTURE(int(hinting));
        font.SetHinting(hinting);
        CHECK(DrawWhite(font, "Hamburgefonstiv", 11) != outline);
    }
}

//-------------------------------------
TEST_CASE("FontFT stem darkening thickens light hinted text") {
    Inspectable<FontFT> font;
    font.SetHinting(FontFT::EHinting::Light);
    const uint64_t thin = SumCoverage(DrawWhite(font, "Hamburgefonstiv", 11));

    font.SetStemDarkening(true);
    CHECK(SumCoverage(DrawWhite(font, "Hamburgefonstiv", 11)) > thin);
}

#endif
