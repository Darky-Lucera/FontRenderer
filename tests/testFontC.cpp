#include "FontC.h"
#include "testHelpers.h"
//-------------------------------------
#include <doctest/doctest.h>

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
