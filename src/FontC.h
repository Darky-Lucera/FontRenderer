#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//-------------------------------------
typedef struct fr_font fr_font;

//-------------------------------------
typedef int32_t fr_status;
enum {
    FR_STATUS_OK = 0,
    FR_STATUS_NOT_LOADED,
    FR_STATUS_CANNOT_OPEN_FILE,
    FR_STATUS_CANNOT_READ_FILE,
    FR_STATUS_INVALID_FONT,
    FR_STATUS_OUT_OF_MEMORY,
    FR_STATUS_INVALID_ARGUMENT,
    FR_STATUS_INVALID_BACKEND,
    FR_STATUS_INTERNAL_ERROR
};

//-------------------------------------
typedef int32_t fr_font_backend;
enum {
    FR_FONT_BACKEND_STB = 0,
    FR_FONT_BACKEND_SFT
};

//-------------------------------------
typedef int32_t fr_font_texture_growth;
enum {
    FR_FONT_TEXTURE_GROWTH_HEIGHT = 0,
    FR_FONT_TEXTURE_GROWTH_WIDTH,
    FR_FONT_TEXTURE_GROWTH_BOTH,
    FR_FONT_TEXTURE_GROWTH_INVALID = -1
};

//-------------------------------------
typedef int32_t fr_font_size_mode;
enum {
    FR_FONT_SIZE_MODE_LINE_HEIGHT = 0,
    FR_FONT_SIZE_MODE_EM_SIZE,
    FR_FONT_SIZE_MODE_INVALID = -1
};

//-------------------------------------
typedef int32_t fr_font_packing_heuristic;
enum {
    FR_FONT_PACKING_LEVEL_BOTTOM_LEFT = 0,
    FR_FONT_PACKING_LEVEL_MIN_WASTE_FIT,
    FR_FONT_PACKING_INVALID = -1
};

//-------------------------------------
typedef struct fr_rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} fr_rect;

#define FR_FONT_MAX_TEXTURE_SIZE     UINT32_C(16384)
#define FR_FONT_MAX_ANTIALIAS_WEIGHT INT32_C(65535)

// A font is not thread safe: use each one from a single thread at a time.
// With a NULL font, the getters return 0, false, NULL or the *_INVALID value.

// Creates a font using the selected rasterizer. On failure, *out_font is NULL.
fr_status fr_font_create(const char *font_name, fr_font_backend backend, fr_font **out_font);
void      fr_font_destroy(fr_font *font);

// Discards every rendered glyph. Changing the size mode, the glyph padding or any antialias setting does it too.
fr_status fr_font_reset(fr_font *font);

// Returned pointers are owned by the font. The texture pointer may be invalidated by
// any operation that renders a glyph, because the texture can grow.
// The texture has one byte of coverage per texel, and its rows are fr_font_get_texture_width bytes long.
const char    *fr_font_get_name(const fr_font *font);
const uint8_t *fr_font_get_texture(const fr_font *font);
uint32_t       fr_font_get_texture_width(const fr_font *font);
uint32_t       fr_font_get_texture_height(const fr_font *font);
uint32_t       fr_font_get_used_texture_width(const fr_font *font);
uint32_t       fr_font_get_used_texture_height(const fr_font *font);

fr_status              fr_font_set_texture_growth(fr_font *font, fr_font_texture_growth growth);
fr_font_texture_growth fr_font_get_texture_growth(const fr_font *font);

fr_status          fr_font_set_size_mode(fr_font *font, fr_font_size_mode mode);
fr_font_size_mode  fr_font_get_size_mode(const fr_font *font);

fr_status fr_font_set_glyph_padding(fr_font *font, uint32_t padding);
uint32_t  fr_font_get_glyph_padding(const fr_font *font);

fr_status                    fr_font_set_packing_heuristic(fr_font *font, fr_font_packing_heuristic heuristic);
fr_font_packing_heuristic    fr_font_get_packing_heuristic(const fr_font *font);

// The destination size is not known. Set clipping so every written pixel lies inside
// the destination buffer. dst_stride is measured in uint32_t pixels, not bytes.
// color is ARGB, and its alpha is used: with alpha 0 the text is invisible.
// text_height is the line height or the em size, as the size mode says. With 0 nothing is drawn.
fr_status fr_font_draw_text(fr_font *font, const char *utf8, uint8_t text_height, uint32_t color,
                            uint32_t *dst, uint32_t dst_stride, int32_t pos_x, int32_t pos_y);
// Box of the pixels fr_font_draw_text would draw, relative to its position. On any failure, *rect is all 0.
fr_status fr_font_get_text_box(fr_font *font, const char *utf8, uint8_t text_height, fr_rect *rect);

fr_status fr_font_set_clipping(fr_font *font, int32_t left, int32_t top, int32_t right, int32_t bottom);

fr_status fr_font_set_antialias(fr_font *font, bool enabled);
bool      fr_font_get_antialias(const fr_font *font);
fr_status fr_font_set_antialias_allow_ex(fr_font *font, bool enabled);
bool      fr_font_get_antialias_allow_ex(const fr_font *font);
fr_status fr_font_set_antialias_weights(fr_font *font, int32_t center, int32_t border, int32_t corner);
int32_t   fr_font_get_antialias_center(const fr_font *font);
int32_t   fr_font_get_antialias_border(const fr_font *font);
int32_t   fr_font_get_antialias_corner(const fr_font *font);

#ifdef __cplusplus
} // extern "C"
#endif

