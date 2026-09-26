#include "FontC.h"
#include "testHelpers.h"
//-------------------------------------
#include <doctest/doctest.h>

extern "C" int fr_test_c_api(const char *font_name, fr_font_backend backend);

//-------------------------------------
TEST_CASE("Font C API is usable from C") {
    CHECK(fr_test_c_api(MindShake::Test::kFontPath, FR_FONT_BACKEND_STB) == 0);
    CHECK(fr_test_c_api(MindShake::Test::kFontPath, FR_FONT_BACKEND_SFT) == 0);
}

//-------------------------------------
TEST_CASE("Font C API reports creation errors") {
    fr_font *font = nullptr;

    CHECK(fr_font_create(nullptr, FR_FONT_BACKEND_STB, &font) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(font == nullptr);
    CHECK(fr_font_create(MindShake::Test::kFontPath, 99, &font) == FR_STATUS_INVALID_BACKEND);
    CHECK(font == nullptr);
    CHECK(fr_font_create("this-font-does-not-exist.ttf", FR_FONT_BACKEND_STB, &font) == FR_STATUS_CANNOT_OPEN_FILE);
    CHECK(font == nullptr);
    CHECK(fr_font_create(MindShake::Test::kFontPath, FR_FONT_BACKEND_STB, nullptr) == FR_STATUS_INVALID_ARGUMENT);

    // Any file that is not a font, like this source file.
    CHECK(fr_font_create(__FILE__, FR_FONT_BACKEND_STB, &font) == FR_STATUS_INVALID_FONT);
    CHECK(font == nullptr);
    CHECK(fr_font_create(__FILE__, FR_FONT_BACKEND_SFT, &font) == FR_STATUS_INVALID_FONT);
    CHECK(font == nullptr);
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
    REQUIRE(fr_font_create(MindShake::Test::kFontPath, FR_FONT_BACKEND_STB, &font) == FR_STATUS_OK);
    CHECK(fr_font_set_texture_growth(font, -1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_set_size_mode(font, -1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_set_packing_heuristic(font, -1) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_get_text_box(font, nullptr, 16, nullptr) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_draw_text(font, "x", 16, 0xffffffffu, nullptr, 0, 0, 0) == FR_STATUS_INVALID_ARGUMENT);
    fr_font_destroy(font);
}

