#include "FontC.h"
//-------------------------------------
#include <doctest/doctest.h>

// Only built without a backend that renders glyphs. The library then has no Font, and this
// test would not link if the C API still used it.

//-------------------------------------
TEST_CASE("Font C API only creates baked fonts without a backend that renders glyphs") {
    fr_font *font = nullptr;

    CHECK(fr_font_create(FONT_RENDERER_TEST_RESOURCES "Roboto-Regular.ttf", FR_FONT_BACKEND_STB, &font) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_create(FONT_RENDERER_TEST_RESOURCES "Roboto-Regular.ttf", FR_FONT_BACKEND_SFT, &font) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_create(FONT_RENDERER_TEST_RESOURCES "Roboto-Regular.ttf", FR_FONT_BACKEND_FT, &font) == FR_STATUS_INVALID_BACKEND);
    CHECK(fr_font_create_baked("this-file-does-not-exist.frb", "this-file-does-not-exist.tga", &font) == FR_STATUS_CANNOT_OPEN_FILE);
    CHECK(font == nullptr);

    CHECK(fr_font_reset(nullptr) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_preload(nullptr, "x", 20) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_load_all_kerning_pairs(nullptr) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_save_baked(nullptr, "a.frb", "a.tga") == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_set_antialias(nullptr, true) == FR_STATUS_INVALID_ARGUMENT);
    CHECK(fr_font_get_antialias(nullptr) == false);
}
