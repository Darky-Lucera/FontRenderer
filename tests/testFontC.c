#include "FontC.h"
//-------------------------------------
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Returns the failing line, after destroying the font.
#define CHECK_C(condition) do { if (!(condition)) { result = __LINE__; goto cleanup; } } while (0)

//-------------------------------------
int
fr_test_c_api(const char *file_name, fr_font_backend backend) {
    fr_font         *font = NULL;
    fr_rect         box;
    fr_glyph_quad   quads[2];
    uint32_t        buffer[64 * 64] = { 0 };
    uint32_t        version;
    size_t          count;
    size_t          i;
    bool            has_pixels = false;
    int             result     = 0;

    CHECK_C(fr_font_create(file_name, backend, &font) == FR_STATUS_OK);
    CHECK_C(font != NULL);
    CHECK_C(strcmp(fr_font_get_file_name(font), file_name) == 0);
    CHECK_C(fr_font_get_texture(font) != NULL);
    CHECK_C(fr_font_get_texture_width(font) == 512);
    CHECK_C(fr_font_get_texture_height(font) == 128);
    CHECK_C(fr_font_get_used_texture_width(font) == 0);
    CHECK_C(fr_font_get_used_texture_height(font) == 0);

    CHECK_C(fr_font_set_texture_growth(font, FR_FONT_TEXTURE_GROWTH_BOTH) == FR_STATUS_OK);
    CHECK_C(fr_font_get_texture_growth(font) == FR_FONT_TEXTURE_GROWTH_BOTH);
    CHECK_C(fr_font_set_size_mode(font, FR_FONT_SIZE_MODE_EM_SIZE) == FR_STATUS_OK);
    CHECK_C(fr_font_get_size_mode(font) == FR_FONT_SIZE_MODE_EM_SIZE);
    CHECK_C(fr_font_set_size_mode(font, FR_FONT_SIZE_MODE_LINE_HEIGHT) == FR_STATUS_OK);
    CHECK_C(fr_font_get_texture_format(font) == FR_FONT_TEXTURE_FORMAT_ALPHA8);
    CHECK_C(fr_font_set_glyph_spacing(font, 2) == FR_STATUS_OK);
    CHECK_C(fr_font_get_glyph_spacing(font) == 2);
    CHECK_C(fr_font_set_glyph_spacing(font, fr_font_get_texture_height(font)) == FR_STATUS_INVALID_ARGUMENT);
    CHECK_C(fr_font_set_glyph_padding(font, 1, 2, 3, 4) == FR_STATUS_OK);
    CHECK_C(fr_font_get_glyph_padding_left(font) == 1);
    CHECK_C(fr_font_get_glyph_padding_top(font) == 2);
    CHECK_C(fr_font_get_glyph_padding_right(font) == 3);
    CHECK_C(fr_font_get_glyph_padding_bottom(font) == 4);
    CHECK_C(fr_font_set_glyph_padding(font, FR_FONT_MAX_TEXTURE_SIZE, 0, 0, 0) == FR_STATUS_INVALID_ARGUMENT);
    CHECK_C(fr_font_get_glyph_padding_left(font) == 1);
    CHECK_C(fr_font_set_packing_heuristic(font, FR_FONT_PACKING_LEVEL_MIN_WASTE_FIT) == FR_STATUS_OK);
    CHECK_C(fr_font_get_packing_heuristic(font) == FR_FONT_PACKING_LEVEL_MIN_WASTE_FIT);
    CHECK_C(fr_font_get_allow_rotation(font));
    CHECK_C(fr_font_set_allow_rotation(font, false) == FR_STATUS_OK);
    CHECK_C(!fr_font_get_allow_rotation(font));

    CHECK_C(fr_font_set_antialias(font, true) == FR_STATUS_OK);
    CHECK_C(fr_font_get_antialias(font));
    CHECK_C(fr_font_set_antialias_allow_ex(font, true) == FR_STATUS_OK);
    CHECK_C(fr_font_get_antialias_allow_ex(font));
    CHECK_C(fr_font_set_antialias_weights(font, 24, 5, 1) == FR_STATUS_OK);
    CHECK_C(fr_font_get_antialias_center(font) == 24);
    CHECK_C(fr_font_get_antialias_border(font) == 5);
    CHECK_C(fr_font_get_antialias_corner(font) == 1);
    CHECK_C(fr_font_set_antialias_weights(font, 0, 0, 0) == FR_STATUS_INVALID_ARGUMENT);

    CHECK_C(fr_font_get_text_box(font, "Ag", 24, &box) == FR_STATUS_OK);
    CHECK_C(box.width > 0);
    CHECK_C(box.height > 0);
    CHECK_C(fr_font_set_clipping(font, 0, 0, 64, 64) == FR_STATUS_OK);
    version = fr_font_get_texture_version(font);
    CHECK_C(fr_font_draw_text(font, "Ag", 24, UINT32_C(0xffffffff), buffer, 64, 8, 8) == FR_STATUS_OK);
    CHECK_C(fr_font_get_texture_version(font) == version);
    for (i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i) {
        if (buffer[i] != 0) {
            has_pixels = true;
            break;
        }
    }
    CHECK_C(has_pixels);
    CHECK_C(fr_font_get_used_texture_width(font) > 0);
    CHECK_C(fr_font_get_used_texture_height(font) > 0);

    CHECK_C(fr_font_get_glyph_quads(font, "Ag", 24, NULL, 0, &count) == FR_STATUS_OK);
    CHECK_C(count == 2);
    CHECK_C(fr_font_get_glyph_quads(font, "Ag", 24, quads, 2, &count) == FR_STATUS_OK);
    CHECK_C(quads[0].width > 0 && quads[0].texture_rect.width > 0);
    CHECK_C(quads[1].x > quads[0].x);

    version = fr_font_get_texture_version(font);
    CHECK_C(fr_font_preload(font, "xyz", 24) == FR_STATUS_OK);
    CHECK_C(fr_font_get_texture_version(font) != version);

    CHECK_C(fr_font_reset(font) == FR_STATUS_OK);
    CHECK_C(fr_font_get_used_texture_width(font) == 0);
    CHECK_C(fr_font_get_used_texture_height(font) == 0);

cleanup:
    fr_font_destroy(font);
    return result;
}
