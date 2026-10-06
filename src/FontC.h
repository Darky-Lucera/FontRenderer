#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
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
    FR_STATUS_INTERNAL_ERROR,
    FR_STATUS_CANNOT_WRITE_FILE,
    FR_STATUS_TEXTURE_FULL,         // Some glyph did not fit in the texture
    FR_STATUS_INVALID_TEXTURE       // A baked font cannot read the texture, or its size is not the one in the metrics file
};

//-------------------------------------
typedef int32_t fr_font_backend;
enum {
    FR_FONT_BACKEND_STB = 0,
    FR_FONT_BACKEND_SFT,
    FR_FONT_BACKEND_FT
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
typedef int32_t fr_font_texture_format;
enum {
    FR_FONT_TEXTURE_FORMAT_ALPHA8 = 0,           // One byte of coverage per texel, drawn with the color of the text
    FR_FONT_TEXTURE_FORMAT_BGRA32,               // Four bytes per texel: blue, green, red and alpha, not premultiplied
    FR_FONT_TEXTURE_FORMAT_BGRA32_PREMULTIPLIED, // As BGRA32, with blue, green and red multiplied by the alpha
    FR_FONT_TEXTURE_FORMAT_INVALID = -1
};

//-------------------------------------
typedef int32_t fr_font_ft_hinting;
enum {
    FR_FONT_FT_HINTING_NONE = 0,
    FR_FONT_FT_HINTING_LIGHT,
    FR_FONT_FT_HINTING_NORMAL,
    FR_FONT_FT_HINTING_AUTO,
    FR_FONT_FT_HINTING_INVALID = -1
};

//-------------------------------------
typedef struct fr_rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} fr_rect;

//-------------------------------------
typedef struct fr_glyph_quad {
    int32_t x;                      // Relative to the position passed to fr_font_draw_text
    int32_t y;
    int32_t width;
    int32_t height;
    // A rotated glyph is stored transposed: the pixel (x, y) of the quad is the texel (texture_rect.x + y, texture_rect.y + x).
    fr_rect texture_rect;
    bool    rotated;
} fr_glyph_quad;

#define FR_FONT_MAX_TEXTURE_SIZE     UINT32_C(16384)
#define FR_FONT_MAX_ANTIALIAS_WEIGHT INT32_C(65535)

// A font is not thread safe: use each one from a single thread at a time.
// With a NULL font, the getters return 0, false, NULL or the *_INVALID value.

// Creates a font using the selected rasterizer. On failure, *out_font is NULL.
// A backend the library was built without gives FR_STATUS_INVALID_BACKEND.
fr_status fr_font_create(const char *file_name, fr_font_backend backend, fr_font **out_font);
// Creates a font from the files fr_font_save_baked writes. It cannot render new glyphs, so the functions
// that change how glyphs are rendered give FR_STATUS_INVALID_BACKEND, and their getters give 0, false or *_INVALID.
// The texture can also be a 32-bit TGA with alpha, or a 16-bit grayscale TGA with alpha, which give an
// FR_FONT_TEXTURE_FORMAT_BGRA32_PREMULTIPLIED texture. The alpha is premultiplied unless the extension area of
// TGA 2.0 says that it already is. A texture it cannot use gives FR_STATUS_INVALID_TEXTURE.
// A library built without FONTRENDERER_USE_BAKED gives FR_STATUS_INVALID_BACKEND.
fr_status fr_font_create_baked(const char *metrics_file, const char *texture_file, fr_font **out_font);
// Takes the texture from memory instead of a TGA file, in rows of width texels. It is copied.
// An FR_FONT_TEXTURE_FORMAT_BGRA32 texture is premultiplied, so fr_font_get_texture_format gives
// FR_FONT_TEXTURE_FORMAT_BGRA32_PREMULTIPLIED. Without a texture, for one that is only in the GPU,
// fr_font_draw_text draws nothing and fr_font_get_texture_format gives format.
fr_status fr_font_create_baked_with_texture(const char *metrics_file, const uint8_t *texture, uint32_t width, uint32_t height,
                                            fr_font_texture_format format, fr_font **out_font);
void      fr_font_destroy(fr_font *font);

// Discards every rendered glyph. Changing the size mode, the glyph spacing, the glyph padding or any antialias setting does it too.
fr_status fr_font_reset(fr_font *font);

// Renders every code point of the text at that height before it is drawn. Packing them all together fills
// the texture better than packing them one by one. Gives FR_STATUS_TEXTURE_FULL if any glyph did not fit.
fr_status fr_font_preload(fr_font *font, const char *utf8, uint8_t text_height);
// Looks up the kerning of every pair of glyphs rendered so far, so that fr_font_save_baked saves it.
// It can be very slow: for n glyphs it looks up n * n pairs.
fr_status fr_font_load_all_kerning_pairs(fr_font *font);
// Saves what fr_font_create_baked needs to draw the glyphs rendered so far: their metrics and kerning in metrics_file,
// and the used area of the texture in texture_file, as an 8-bit grayscale TGA compressed with RLE.
// Only the kerning pairs already looked up are saved. Call fr_font_load_all_kerning_pairs first to save all of them.
fr_status fr_font_save_baked(const fr_font *font, const char *metrics_file, const char *texture_file);

// Returned pointers are owned by the font. The texture pointer may be invalidated by
// any operation that renders a glyph, because the texture can grow.
// The texture has rows of fr_font_get_texture_width texels, in the format fr_font_get_texture_format gives.
// Fonts created with fr_font_create always have an FR_FONT_TEXTURE_FORMAT_ALPHA8 texture.
const char    *fr_font_get_file_name(const fr_font *font);
const uint8_t *fr_font_get_texture(const fr_font *font);
fr_font_texture_format fr_font_get_texture_format(const fr_font *font);
uint32_t       fr_font_get_texture_width(const fr_font *font);
uint32_t       fr_font_get_texture_height(const fr_font *font);
uint32_t       fr_font_get_used_texture_width(const fr_font *font);
uint32_t       fr_font_get_used_texture_height(const fr_font *font);
// Changes whenever the texels change, so that a copy of the texture, like one in the GPU, knows when to update.
uint32_t       fr_font_get_texture_version(const fr_font *font);

fr_status              fr_font_set_texture_growth(fr_font *font, fr_font_texture_growth growth);
fr_font_texture_growth fr_font_get_texture_growth(const fr_font *font);

fr_status          fr_font_set_size_mode(fr_font *font, fr_font_size_mode mode);
fr_font_size_mode  fr_font_get_size_mode(const fr_font *font);

// Empty pixels kept between the glyphs in the texture, so that bilinear filtering does not bleed neighbouring glyphs.
fr_status fr_font_set_glyph_spacing(fr_font *font, uint32_t spacing);
uint32_t  fr_font_get_glyph_spacing(const fr_font *font);

// Empty pixels added to each side of every glyph, as part of the glyph: fr_font_draw_text, fr_font_get_text_box and
// fr_font_get_glyph_quads include them. They leave room to add effects, like a shadow or an outline, to the saved texture.
fr_status fr_font_set_glyph_padding(fr_font *font, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);
uint32_t  fr_font_get_glyph_padding_left(const fr_font *font);
uint32_t  fr_font_get_glyph_padding_top(const fr_font *font);
uint32_t  fr_font_get_glyph_padding_right(const fr_font *font);
uint32_t  fr_font_get_glyph_padding_bottom(const fr_font *font);

// Only affects glyphs packed afterwards.
fr_status                    fr_font_set_packing_heuristic(fr_font *font, fr_font_packing_heuristic heuristic);
fr_font_packing_heuristic    fr_font_get_packing_heuristic(const fr_font *font);

// A rotated glyph is stored transposed. An effect added to the saved texture that is not symmetric
// about the diagonal, like a vertical gradient, would look wrong on it. Only affects glyphs packed afterwards.
fr_status fr_font_set_allow_rotation(fr_font *font, bool enabled);
bool      fr_font_get_allow_rotation(const fr_font *font);

// The destination size is not known. Set clipping so every written pixel lies inside
// the destination buffer. dst_stride is measured in uint32_t pixels, not bytes.
// color is ARGB, and its alpha is used: with alpha 0 the text is invisible. With a color texture, the color of
// each texel is multiplied by color, as a GPU does with the color of a vertex. The alpha of dst is blended too,
// as the over operator of Porter and Duff does. Unlike color, dst must have premultiplied alpha, and so does the
// result. An opaque pixel is the same either way.
// text_height is the line height or the em size, as the size mode says. With 0 nothing is drawn.
fr_status fr_font_draw_text(fr_font *font, const char *utf8, uint8_t text_height, uint32_t color,
                            uint32_t *dst, uint32_t dst_stride, int32_t pos_x, int32_t pos_y);
// Box of the pixels fr_font_draw_text would draw, relative to its position. On any failure, *rect is all 0.
fr_status fr_font_get_text_box(fr_font *font, const char *utf8, uint8_t text_height, fr_rect *rect);
// The glyphs fr_font_draw_text would draw, to draw them in another way, like with the GPU. The clipping does not apply.
// Writes up to capacity quads, and sets *count to the number of quads of the whole text, so a bigger
// buffer can be passed again. quads can be NULL when capacity is 0.
// It can add glyphs to the texture, so check fr_font_get_texture_version afterwards.
fr_status fr_font_get_glyph_quads(fr_font *font, const char *utf8, uint8_t text_height,
                                  fr_glyph_quad *quads, size_t capacity, size_t *count);

fr_status fr_font_set_clipping(fr_font *font, int32_t left, int32_t top, int32_t right, int32_t bottom);

fr_status fr_font_set_antialias(fr_font *font, bool enabled);
bool      fr_font_get_antialias(const fr_font *font);
fr_status fr_font_set_antialias_allow_ex(fr_font *font, bool enabled);
bool      fr_font_get_antialias_allow_ex(const fr_font *font);
fr_status fr_font_set_antialias_weights(fr_font *font, int32_t center, int32_t border, int32_t corner);
int32_t   fr_font_get_antialias_center(const fr_font *font);
int32_t   fr_font_get_antialias_border(const fr_font *font);
int32_t   fr_font_get_antialias_corner(const fr_font *font);

// Only for fonts created with FR_FONT_BACKEND_FT. Other fonts give FR_STATUS_INVALID_BACKEND,
// and the getters give false or FR_FONT_FT_HINTING_INVALID. See FontFT.h for what each setting does.
// Changing any of them discards every rendered glyph, like fr_font_reset.
fr_status          fr_font_ft_set_hinting(fr_font *font, fr_font_ft_hinting hinting);
fr_font_ft_hinting fr_font_ft_get_hinting(const fr_font *font);
fr_status          fr_font_ft_set_monochrome(fr_font *font, bool enabled);
bool               fr_font_ft_get_monochrome(const fr_font *font);
fr_status          fr_font_ft_set_stem_darkening(fr_font *font, bool enabled);
bool               fr_font_ft_get_stem_darkening(const fr_font *font);

#ifdef __cplusplus
} // extern "C"
#endif

