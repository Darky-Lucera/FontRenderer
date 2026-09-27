#include "FontBaked.h"
#include "Tga.h"
#include "testHelpers.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace MindShake;

//-------------------------------------
namespace {

    constexpr uint32_t kWhite = 0xffffffffu;

    const char *kTexts[] = {
        "Hello",
        "AVgjpq Wa 1f.T",
        "Line one\nsecond gy\n\nlast",
        "  x  ",
        "\xc2\xbf" "C\xc3\xb3" "mo est\xc3\xa1s?",
    };

    const uint8_t kHeights[] = { 11, 30 };

    //---------------------------------
    std::string
    GetAllText() {
        std::string text;
        for(char c = ' '; c <= '~'; ++c) {
            text += c;
        }
        for(const char *sample : kTexts) {
            text += sample;
        }
        return text;
    }

    //---------------------------------
    std::vector<uint32_t>
    Draw(FontBase &font, const char *text, uint8_t height) {
        std::vector<uint32_t> buffer(400 * 160, 0);
        font.DrawText(text, height, kWhite, buffer.data(), 400, 10, 10);
        return buffer;
    }

    //---------------------------------
    std::vector<uint8_t>
    ReadFile(const char *path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    //---------------------------------
    void
    WriteFile(const char *path, const std::vector<uint8_t> &bytes) {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    }

    // Antialiased and extended, so the glyphs are not exactly what the rasterizer gives.
    //---------------------------------
    template <class TFont>
    void
    SaveAllText(TFont &font) {
        font.SetAntialias(true);
        font.SetAntialiasAllowEx(true);
        for(uint8_t height : kHeights) {
            REQUIRE(font.Preload(GetAllText().c_str(), height));
        }
        font.LoadAllKerningPairs();
        REQUIRE(font.SaveBaked(Test::kMetricsPath, Test::kTexturePath) == Font::EStatus::Ok);
    }

    //---------------------------------
    bool
    AreEqual(const std::vector<GlyphQuad> &a, const std::vector<GlyphQuad> &b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const GlyphQuad &x, const GlyphQuad &y) {
            return x.x == y.x && x.y == y.y && x.width == y.width && x.height == y.height && x.rotated == y.rotated &&
                   x.textureRect.x == y.textureRect.x && x.textureRect.y == y.textureRect.y &&
                   x.textureRect.width == y.textureRect.width && x.textureRect.height == y.textureRect.height;
        });
    }

} // end of namespace

//-------------------------------------
TEST_CASE_TEMPLATE("FontBaked draws exactly as the font it was saved from", TFont, FONT_BACKENDS) {
    TFont font;
    SaveAllText(font);

    FontBaked baked(Test::kMetricsPath, Test::kTexturePath);
    REQUIRE(baked.GetStatus() == Font::EStatus::Ok);
    CHECK(baked.GetFontName() == Test::kMetricsPath);
    CHECK(baked.GetTextureWidth()  == font.GetUsedTextureWidth());
    CHECK(baked.GetTextureHeight() == font.GetUsedTextureHeight());

    const uint32_t version = font.GetTextureVersion();
    for(const char *text : kTexts) {
        for(uint8_t height : kHeights) {
            CAPTURE(text);
            CAPTURE(int(height));

            CHECK(Draw(baked, text, height) == Draw(font, text, height));

            Font::Rect expected, box;
            font.GetTextBox(text, height, &expected);
            baked.GetTextBox(text, height, &box);
            CHECK(box.x      == expected.x);
            CHECK(box.y      == expected.y);
            CHECK(box.width  == expected.width);
            CHECK(box.height == expected.height);

            std::vector<GlyphQuad> expectedQuads, quads;
            font.GetGlyphQuads(text, height, expectedQuads);
            baked.GetGlyphQuads(text, height, quads);
            CHECK(AreEqual(quads, expectedQuads));
        }
    }
    // Every glyph drawn was already saved.
    CHECK(font.GetTextureVersion() == version);
}

//-------------------------------------
TEST_CASE("FontBaked only draws the glyphs it has") {
    Test::DefaultFont font(Test::kFontPath);
    REQUIRE(font.Preload("A", 20));
    REQUIRE(font.SaveBaked(Test::kMetricsPath, Test::kTexturePath) == Font::EStatus::Ok);

    FontBaked baked(Test::kMetricsPath, Test::kTexturePath);
    REQUIRE(baked.GetStatus() == Font::EStatus::Ok);

    const std::vector<uint32_t> empty(400 * 160, 0);
    CHECK(Draw(baked, "A", 20) != empty);
    CHECK(Draw(baked, "B", 20) == empty);
    CHECK(Draw(baked, "A", 21) == empty);

    Font::Rect single, missing;
    baked.GetTextBox("A", 20, &single);
    baked.GetTextBox("AB", 20, &missing);
    CHECK(missing.width == single.width);
}

//-------------------------------------
TEST_CASE("FontBaked skips the code points the font does not have") {
    // U+E000 is private use: the font does not have it, so it was not saved either.
    const char        *kMissing = "H\xee\x80\x80H";
    Test::DefaultFont font(Test::kFontPath);
    REQUIRE(font.Preload(kMissing, 20));
    REQUIRE(font.SaveBaked(Test::kMetricsPath, Test::kTexturePath) == Font::EStatus::Ok);

    FontBaked baked(Test::kMetricsPath, Test::kTexturePath);
    REQUIRE(baked.GetStatus() == Font::EStatus::Ok);
    CHECK(Draw(baked, kMissing, 20) == Draw(font, kMissing, 20));
    CHECK(Draw(baked, kMissing, 20) == Draw(baked, "HH", 20));
}

//-------------------------------------
TEST_CASE("FontBaked takes the texture from memory") {
    Test::DefaultFont font(Test::kFontPath);
    SaveAllText(font);

    FontBaked                  reference(Test::kMetricsPath, Test::kTexturePath);
    const std::vector<uint8_t> file = ReadFile(Test::kTexturePath);
    std::vector<uint8_t>       texture;
    uint32_t                   width = 0, height = 0;
    REQUIRE(ReadTga(file.data(), file.size(), texture, width, height));

    FontBaked baked(Test::kMetricsPath, texture.data(), width, height);
    REQUIRE(baked.GetStatus() == Font::EStatus::Ok);
    CHECK(Draw(baked, kTexts[1], 30) == Draw(reference, kTexts[1], 30));

    // Only in the GPU: there is nothing to draw with, but the quads are the same.
    FontBaked gpu(Test::kMetricsPath, nullptr, width, height);
    REQUIRE(gpu.GetStatus() == Font::EStatus::Ok);
    CHECK(gpu.GetTexture() == nullptr);
    CHECK(Draw(gpu, kTexts[1], 30) == std::vector<uint32_t>(400 * 160, 0));

    std::vector<GlyphQuad> expected, quads;
    reference.GetGlyphQuads(kTexts[1], 30, expected);
    gpu.GetGlyphQuads(kTexts[1], 30, quads);
    CHECK(AreEqual(quads, expected));

    CHECK(FontBaked(Test::kMetricsPath, texture.data(), width + 1, height).GetStatus() == Font::EStatus::InvalidFont);
    CHECK(FontBaked(Test::kMetricsPath, nullptr, width, height - 1).GetStatus() == Font::EStatus::InvalidFont);
}

//-------------------------------------
TEST_CASE("FontBaked reports files it cannot use") {
    Test::DefaultFont font(Test::kFontPath);
    REQUIRE(font.Preload("Hello", 20));
    font.LoadAllKerningPairs();
    REQUIRE(font.SaveBaked(Test::kMetricsPath, Test::kTexturePath) == Font::EStatus::Ok);
    const std::vector<uint8_t> metrics = ReadFile(Test::kMetricsPath);
    const std::vector<uint8_t> texture = ReadFile(Test::kTexturePath);
    REQUIRE(FontBaked(Test::kMetricsPath, Test::kTexturePath).GetStatus() == Font::EStatus::Ok);

    CHECK(FontBaked("this-file-does-not-exist.frb", Test::kTexturePath).GetStatus() == Font::EStatus::CannotOpenFile);
    CHECK(FontBaked(Test::kMetricsPath, "this-file-does-not-exist.tga").GetStatus() == Font::EStatus::CannotOpenFile);
    // Any file that is not what it should be, like this source file.
    CHECK(FontBaked(__FILE__, Test::kTexturePath).GetStatus() == Font::EStatus::InvalidFont);
    CHECK(FontBaked(Test::kMetricsPath, __FILE__).GetStatus() == Font::EStatus::InvalidFont);

    // The texture is smaller than the one the glyphs were placed in.
    std::vector<uint8_t> small = texture;
    small[12] = uint8_t(small[12] - 1);
    small.resize(small.size() - 1);
    WriteFile(Test::kTexturePath, small);
    CHECK(FontBaked(Test::kMetricsPath, Test::kTexturePath).GetStatus() == Font::EStatus::InvalidFont);
    WriteFile(Test::kTexturePath, texture);

    struct Change {
        const char  *what;
        size_t      offset;
        uint8_t     value;
    };
    // A header of 28 bytes, one height of 17 bytes, and then the glyphs, 'H' first. Numbers are little-endian.
    const size_t firstGlyph = 28 + 17;
    const Change changes[] = {
        { "magic",                  0,                  'X'  },
        { "version",                4,                  2    },
        { "texture width",          11,                 0x7f },
        { "texture height",         15,                 0x7f },
        { "height count",           16,                 2    },
        { "height 0",               28,                 0    },
        { "code point 0",           firstGlyph,         0    },
        { "code point too big",     firstGlyph + 3,     0x7f },
        { "height without metrics", firstGlyph + 4,     21   },
        { "negative glyph",         firstGlyph + 8,     0x80 },
        { "rect outside",           firstGlyph + 28,    0x7f },
        { "rotated 2",              firstGlyph + 41,    2    },
    };
    for(const Change &change : changes) {
        CAPTURE(change.what);
        std::vector<uint8_t> file = metrics;
        REQUIRE(file[change.offset] != change.value);
        file[change.offset] = change.value;
        WriteFile(Test::kMetricsPath, file);
        CHECK(FontBaked(Test::kMetricsPath, Test::kTexturePath).GetStatus() == Font::EStatus::InvalidFont);
    }

    std::vector<uint8_t> truncated = metrics;
    truncated.pop_back();
    WriteFile(Test::kMetricsPath, truncated);
    CHECK(FontBaked(Test::kMetricsPath, Test::kTexturePath).GetStatus() == Font::EStatus::InvalidFont);
}

//-------------------------------------
TEST_CASE("Font reports files it cannot write") {
    Test::DefaultFont font(Test::kFontPath);
    REQUIRE(font.Preload("A", 20));

    const char *missing = FONT_RENDERER_TEST_OUTPUT "missing/baked.frb";
    CHECK(font.SaveBaked(missing, Test::kTexturePath) == Font::EStatus::CannotWriteFile);
    CHECK(font.SaveBaked(Test::kMetricsPath, missing) == Font::EStatus::CannotWriteFile);
    CHECK(font.SaveBaked(nullptr, Test::kTexturePath) == Font::EStatus::CannotWriteFile);
    CHECK(font.SaveBaked(Test::kMetricsPath, nullptr) == Font::EStatus::CannotWriteFile);
}

//-------------------------------------
TEST_CASE("Font saves a font without pixels") {
    Test::DefaultFont font(Test::kFontPath);
    REQUIRE(font.Preload(" ", 20));
    REQUIRE(font.SaveBaked(Test::kMetricsPath, Test::kTexturePath) == Font::EStatus::Ok);

    FontBaked baked(Test::kMetricsPath, Test::kTexturePath);
    REQUIRE(baked.GetStatus() == Font::EStatus::Ok);
    CHECK(baked.GetTextureWidth()  == 1);
    CHECK(baked.GetTextureHeight() == 1);
}
