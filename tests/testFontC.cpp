#include "FontC.h"
#include "testHelpers.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <vector>

extern "C" int fr_test_c_api(const char *font_name, fr_font_backend backend);

//-------------------------------------
namespace {

    // For the tests of what the C API does the same whatever the backend.
#if defined(FONTRENDERER_USE_STB)
    constexpr fr_font_backend kBackend = FR_FONT_BACKEND_STB;
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
    constexpr fr_font_backend kBackend = FR_FONT_BACKEND_SFT;
#else
    constexpr fr_font_backend kBackend = FR_FONT_BACKEND_FT;
#endif

} // end of namespace

//-------------------------------------
TEST_CASE("Font C API is usable from C") {
#if defined(FONTRENDERER_USE_STB)
    CHECK(fr_test_c_api(MindShake::Test::kFontPath, FR_FONT_BACKEND_STB) == 0);
#endif
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
    CHECK(fr_test_c_api(MindShake::Test::kFontPath, FR_FONT_BACKEND_SFT) == 0);
#endif
#if defined(FONTRENDERER_USE_FREETYPE)
    CHECK(fr_test_c_api(MindShake::Test::kFontPath, FR_FONT_BACKEND_FT) == 0);
#endif
}

#if !defined(FONTRENDERER_USE_STB) || !defined(FONTRENDERER_USE_LIBSCHRIFT) || !defined(FONTRENDERER_USE_FREETYPE)
//-------------------------------------
TEST_CASE("Font C API rejects the backends the library was built without") {
    fr_font *font = nullptr;
#if !defined(FONTRENDERER_USE_STB)
    CHECK(fr_font_create(MindShake::Test::kFontPath, FR_FONT_BACKEND_STB, &font) == FR_STATUS_INVALID_BACKEND);
#endif
#if !defined(FONTRENDERER_USE_LIBSCHRIFT)
    CHECK(fr_font_create(MindShake::Test::kFontPath, FR_FONT_BACKEND_SFT, &font) == FR_STATUS_INVALID_BACKEND);
#endif
#if !defined(FONTRENDERER_USE_FREETYPE)
    CHECK(fr_font_create(MindShake::Test::kFontPath, FR_FONT_BACKEND_FT, &font) == FR_STATUS_INVALID_BACKEND);
#endif
    CHECK(font == nullptr);
}
#endif

//-------------------------------------
TEST_CASE("Font C API reports creation errors") {
    fr_font *font = nullptr;

    CHECK(fr_font_create(nullptr, kBackend, &font) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(font == nullptr);
    CHECK(fr_font_create(MindShake::Test::kFontPath, 99, &font) == FR_STATUS_INVALID_BACKEND);
    CHECK(font == nullptr);
    CHECK(fr_font_create("this-font-does-not-exist.ttf", kBackend, &font) == FR_STATUS_CANNOT_OPEN_FILE);
    CHECK(font == nullptr);
    CHECK(fr_font_create(MindShake::Test::kFontPath, kBackend, nullptr) == FR_STATUS_INVALID_ARGUMENT);

    // Any file that is not a font, like this source file.
#if defined(FONTRENDERER_USE_STB)
    CHECK(fr_font_create(__FILE__, FR_FONT_BACKEND_STB, &font) == FR_STATUS_INVALID_FONT);
    CHECK(font == nullptr);
#endif
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
    CHECK(fr_font_create(__FILE__, FR_FONT_BACKEND_SFT, &font) == FR_STATUS_INVALID_FONT);
    CHECK(font == nullptr);
#endif
#if defined(FONTRENDERER_USE_FREETYPE)
    CHECK(fr_font_create(__FILE__, FR_FONT_BACKEND_FT, &font) == FR_STATUS_INVALID_FONT);
    CHECK(font == nullptr);
#endif
}

//-------------------------------------
TEST_CASE("Font C API rejects invalid handles and options") {
    CHECK(fr_font_reset(nullptr) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_set_texture_growth(nullptr, FR_FONT_TEXTURE_GROWTH_HEIGHT) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_get_texture_growth(nullptr) == FR_FONT_TEXTURE_GROWTH_INVALID);
    CHECK(fr_font_set_size_mode(nullptr, FR_FONT_SIZE_MODE_LINE_HEIGHT) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_get_size_mode(nullptr) == FR_FONT_SIZE_MODE_INVALID);
    CHECK(fr_font_set_packing_heuristic(nullptr, FR_FONT_PACKING_LEVEL_BOTTOM_LEFT) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_get_packing_heuristic(nullptr) == FR_FONT_PACKING_INVALID);
    CHECK(fr_font_get_name(nullptr) == nullptr);
    CHECK(fr_font_get_texture(nullptr) == nullptr);
    CHECK(fr_font_get_texture_format(nullptr) == FR_FONT_TEXTURE_FORMAT_INVALID);
    CHECK(fr_font_set_glyph_spacing(nullptr, 1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_set_glyph_padding(nullptr, 1, 1, 1, 1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_set_allow_rotation(nullptr, true) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_get_allow_rotation(nullptr) == false);

    fr_rect box { 1, 2, 3, 4 };
    CHECK(fr_font_get_text_box(nullptr, "x", 16, &box) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(box.x == 0);
    CHECK(box.y == 0);
    CHECK(box.width == 0);
    CHECK(box.height == 0);

    fr_font *font = nullptr;
    REQUIRE(fr_font_create(MindShake::Test::kFontPath, kBackend, &font) == FR_STATUS_OK);
    CHECK(fr_font_set_texture_growth(font, -1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_set_size_mode(font, -1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_set_packing_heuristic(font, -1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_get_text_box(font, nullptr, 16, nullptr) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_draw_text(font, "x", 16, 0xffffffffu, nullptr, 0, 0, 0) == FR_STATUS_INVALID_ARGUMENT);
    fr_font_destroy(font);
}


//-------------------------------------
TEST_CASE("Font C API only applies the FreeType settings to FreeType fonts") {
    CHECK(fr_font_ft_set_hinting(nullptr, FR_FONT_FT_HINTING_LIGHT) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_ft_set_monochrome(nullptr, true) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_ft_set_stem_darkening(nullptr, true) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_ft_get_hinting(nullptr) == FR_FONT_FT_HINTING_INVALID);
    CHECK(fr_font_ft_get_monochrome(nullptr) == false);
    CHECK(fr_font_ft_get_stem_darkening(nullptr) == false);

#if defined(FONTRENDERER_USE_STB) || defined(FONTRENDERER_USE_LIBSCHRIFT)
    {
#if defined(FONTRENDERER_USE_STB)
        const fr_font_backend other = FR_FONT_BACKEND_STB;
#else
        const fr_font_backend other = FR_FONT_BACKEND_SFT;
#endif
        fr_font *font = nullptr;
        REQUIRE(fr_font_create(MindShake::Test::kFontPath, other, &font) == FR_STATUS_OK);
        CHECK(fr_font_ft_set_hinting(font, FR_FONT_FT_HINTING_LIGHT) == FR_STATUS_INVALID_BACKEND);
        CHECK(fr_font_ft_set_monochrome(font, true) == FR_STATUS_INVALID_BACKEND);
        CHECK(fr_font_ft_set_stem_darkening(font, true) == FR_STATUS_INVALID_BACKEND);
        CHECK(fr_font_ft_get_hinting(font) == FR_FONT_FT_HINTING_INVALID);
        CHECK(fr_font_ft_get_monochrome(font) == false);
        CHECK(fr_font_ft_get_stem_darkening(font) == false);
        fr_font_destroy(font);
    }
#endif

#if defined(FONTRENDERER_USE_FREETYPE)
    fr_font *font = nullptr;
    REQUIRE(fr_font_create(MindShake::Test::kFontPath, FR_FONT_BACKEND_FT, &font) == FR_STATUS_OK);
    CHECK(fr_font_ft_get_hinting(font) == FR_FONT_FT_HINTING_NONE);
    CHECK(fr_font_ft_set_hinting(font, FR_FONT_FT_HINTING_AUTO) == FR_STATUS_OK);
    CHECK(fr_font_ft_get_hinting(font) == FR_FONT_FT_HINTING_AUTO);
    CHECK(fr_font_ft_set_hinting(font, -1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_ft_get_hinting(font) == FR_FONT_FT_HINTING_AUTO);
    CHECK(fr_font_ft_set_monochrome(font, true) == FR_STATUS_OK);
    CHECK(fr_font_ft_get_monochrome(font));
    CHECK(fr_font_ft_set_stem_darkening(font, true) == FR_STATUS_OK);
    CHECK(fr_font_ft_get_stem_darkening(font));
    fr_font_destroy(font);
#endif
}

#if defined(FONTRENDERER_USE_BAKED)
//-------------------------------------
TEST_CASE("Font C API saves and loads baked fonts") {
    using MindShake::Test::kMetricsPath;
    using MindShake::Test::kTexturePath;

    const char *kText  = "Hello AV";
    fr_font    *font   = nullptr;
    REQUIRE(fr_font_create(MindShake::Test::kFontPath, kBackend, &font) == FR_STATUS_OK);
    REQUIRE(fr_font_preload(font, kText, 24) == FR_STATUS_OK);
    REQUIRE(fr_font_load_all_kerning_pairs(font) == FR_STATUS_OK);
    REQUIRE(fr_font_save_baked(font, kMetricsPath, kTexturePath) == FR_STATUS_OK);

    fr_font *baked = nullptr;
    REQUIRE(fr_font_create_baked(kMetricsPath, kTexturePath, &baked) == FR_STATUS_OK);
    std::vector<uint32_t> expected(128 * 64, 0);
    std::vector<uint32_t> buffer(128 * 64, 0);
    CHECK(fr_font_draw_text(font, kText, 24, 0xffffffffu, expected.data(), 128, 4, 4) == FR_STATUS_OK);
    CHECK(fr_font_draw_text(baked, kText, 24, 0xffffffffu, buffer.data(), 128, 4, 4) == FR_STATUS_OK);
    CHECK(buffer == expected);
    CHECK(fr_font_get_texture(baked) != nullptr);
    CHECK(fr_font_get_used_texture_width(baked) == fr_font_get_texture_width(baked));

    // A baked font cannot render new glyphs, so nothing about how they are rendered applies.
    CHECK(fr_font_reset(baked) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_preload(baked, "x", 24) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_load_all_kerning_pairs(baked) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_save_baked(baked, kMetricsPath, kTexturePath) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_texture_growth(baked, FR_FONT_TEXTURE_GROWTH_BOTH) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_size_mode(baked, FR_FONT_SIZE_MODE_EM_SIZE) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_glyph_spacing(baked, 2) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_glyph_padding(baked, 1, 1, 1, 1) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_packing_heuristic(baked, FR_FONT_PACKING_LEVEL_MIN_WASTE_FIT) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_allow_rotation(baked, false) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_antialias(baked, true) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_antialias_allow_ex(baked, true) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_set_antialias_weights(baked, 1, 1, 1) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_ft_set_hinting(baked, FR_FONT_FT_HINTING_LIGHT) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_get_texture_growth(baked) == FR_FONT_TEXTURE_GROWTH_INVALID);
    CHECK(fr_font_get_size_mode(baked) == FR_FONT_SIZE_MODE_INVALID);
    CHECK(fr_font_get_glyph_spacing(baked) == 0);
    CHECK(fr_font_get_glyph_padding_left(baked) == 0);
    CHECK(fr_font_get_packing_heuristic(baked) == FR_FONT_PACKING_INVALID);
    CHECK(fr_font_get_allow_rotation(baked) == false);
    CHECK(fr_font_get_texture_format(baked) == FR_FONT_TEXTURE_FORMAT_ALPHA8);
    CHECK(fr_font_get_antialias(baked) == false);
    CHECK(fr_font_get_antialias_allow_ex(baked) == false);
    CHECK(fr_font_get_antialias_center(baked) == 0);

    fr_font        *gpu   = nullptr;
    const uint32_t width  = fr_font_get_texture_width(baked);
    const uint32_t height = fr_font_get_texture_height(baked);
    REQUIRE(fr_font_create_baked_with_texture(kMetricsPath, nullptr, width, height, FR_FONT_TEXTURE_FORMAT_BGRA32, &gpu) == FR_STATUS_OK);
    CHECK(fr_font_get_texture(gpu) == nullptr);
    CHECK(fr_font_get_texture_format(gpu) == FR_FONT_TEXTURE_FORMAT_BGRA32);
    size_t count = 0;
    CHECK(fr_font_get_glyph_quads(gpu, "Hello", 24, nullptr, 0, &count) == FR_STATUS_OK);
    CHECK(count == 5);
    fr_font_destroy(gpu);

    // A premultiplied white texel (m, m, m, m) draws as the coverage m.
    const uint8_t        *coverage = fr_font_get_texture(baked);
    std::vector<uint8_t> white;
    for(size_t i = 0; i < size_t(width) * height; ++i) {
        white.insert(white.end(), 4, coverage[i]);
    }
    fr_font *color = nullptr;
    REQUIRE(fr_font_create_baked_with_texture(kMetricsPath, white.data(), width, height, FR_FONT_TEXTURE_FORMAT_BGRA32_PREMULTIPLIED, &color) == FR_STATUS_OK);
    CHECK(fr_font_get_texture(color) != nullptr);
    CHECK(fr_font_get_texture_format(color) == FR_FONT_TEXTURE_FORMAT_BGRA32_PREMULTIPLIED);
    std::vector<uint32_t> colorBuffer(128 * 64, 0);
    CHECK(fr_font_draw_text(color, kText, 24, 0xffffffffu, colorBuffer.data(), 128, 4, 4) == FR_STATUS_OK);
    CHECK(colorBuffer == expected);
    fr_font_destroy(color);

    CHECK(fr_font_create_baked_with_texture(kMetricsPath, nullptr, width + 1, height, FR_FONT_TEXTURE_FORMAT_ALPHA8, &gpu) == FR_STATUS_INVALID_TEXTURE);
    CHECK(gpu == nullptr);
    CHECK(fr_font_create_baked_with_texture(kMetricsPath, nullptr, width, height, FR_FONT_TEXTURE_FORMAT_BGRA32_PREMULTIPLIED + 1, &gpu) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_create_baked_with_texture(kMetricsPath, nullptr, width, height, FR_FONT_TEXTURE_FORMAT_INVALID, &gpu) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(gpu == nullptr);

    fr_font_destroy(baked);
    fr_font_destroy(font);
}

//-------------------------------------
TEST_CASE("Font C API reports baked font errors") {
    using MindShake::Test::kMetricsPath;
    using MindShake::Test::kTexturePath;

    fr_font *font = nullptr;
    CHECK(fr_font_create_baked(nullptr, kTexturePath, &font) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_create_baked(kMetricsPath, nullptr, &font) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_create_baked(kMetricsPath, kTexturePath, nullptr) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_create_baked_with_texture(nullptr, nullptr, 1, 1, FR_FONT_TEXTURE_FORMAT_ALPHA8, &font) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_create_baked("this-file-does-not-exist.frb", kTexturePath, &font) == FR_STATUS_CANNOT_OPEN_FILE);
    CHECK(font == nullptr);

    REQUIRE(fr_font_create(MindShake::Test::kFontPath, kBackend, &font) == FR_STATUS_OK);
    size_t        count = 1;
    fr_glyph_quad quad;
    CHECK(fr_font_preload(font, nullptr, 24) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_save_baked(font, nullptr, kTexturePath) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_save_baked(font, kMetricsPath, nullptr) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_save_baked(font, FONT_RENDERER_TEST_OUTPUT "missing/baked.frb", kTexturePath) == FR_STATUS_CANNOT_WRITE_FILE);
    CHECK(fr_font_get_glyph_quads(font, "x", 24, nullptr, 1, &count) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(count == 0);
    CHECK(fr_font_get_glyph_quads(font, "x", 24, &quad, 1, nullptr) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_get_glyph_quads(nullptr, "x", 24, &quad, 1, &count) == FR_STATUS_INVALID_ARGUMENT);

    // With the texture height fixed at 128, a 255 pixel 'W' fits in neither orientation.
    CHECK(fr_font_set_texture_growth(font, FR_FONT_TEXTURE_GROWTH_WIDTH) == FR_STATUS_OK);
    CHECK(fr_font_preload(font, "W", 255) == FR_STATUS_TEXTURE_FULL);
    fr_font_destroy(font);
}
#else
//-------------------------------------
TEST_CASE("Font C API rejects baked fonts without the baked backend") {
    fr_font *font = nullptr;
    CHECK(fr_font_create_baked(MindShake::Test::kMetricsPath, MindShake::Test::kTexturePath, &font) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_create_baked_with_texture(MindShake::Test::kMetricsPath, nullptr, 1, 1, FR_FONT_TEXTURE_FORMAT_ALPHA8, &font) == FR_STATUS_INVALID_BACKEND);
    CHECK(font == nullptr);
}
#endif
