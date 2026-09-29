#include "exampleCommon.h"
#include <FontC.h>
//-------------------------------------
#include <stdio.h>

//-------------------------------------
typedef struct backend_info {
    const char *name;
    uint32_t    color;
    fr_font    *font;
    double      raster_milliseconds;    // Time to render the glyphs of the column
    bool        raster_measured;
} backend_info;

// The font of the shown atlas, saved with S and read back from its files. It keeps the settings it was saved with.
//-------------------------------------
typedef struct baked_info {
    fr_font *font;
    size_t   source;                    // The backend it was saved from
    long     metrics_bytes;
    long     texture_bytes;
} baked_info;

//-------------------------------------
static bool         g_show_bounding_box = true;
static bool         g_show_texture      = true;
static size_t       g_show_texture_id   = 0;
static bool         g_mean_weights      = false;
static backend_info g_backends[3];
static size_t       g_backend_count     = 0;
static baked_info   g_baked             = { NULL, 0, 0, 0 };
static double       g_draw_seconds      = 0.0;      // Time fr_font_draw_text took, summed over the timed frames
#if defined(FONTRENDERER_USE_FREETYPE)
static fr_font     *g_free_type          = NULL;
#endif

//-------------------------------------
static const int32_t    k_margin        = 16;
static const int32_t    k_status_height = 44;
#if defined(FONTRENDERER_USE_FREETYPE)
static const int32_t    k_status_line_height = 18;
#endif

static const uint32_t   k_text_color   = UINT32_C(0xfff4ece1);
static const uint32_t   k_status_color = UINT32_C(0xffdcdcdc);

static const char       *k_greeting       = "Hello! - ¡Hola! - Привет! - Olá!";
static const char       *k_more_greetings = "Grüß dich! - Γειά σου! - Ça va ? - Cześć!\n"
                                            "Xin chào! - Günaydın! - Hallå! - Dobrý den!\n"
                                            "Jó napot! - Hyvää päivää! - Bună ziua!\n"
                                            "Góðan dag! - Dobrý deň! - Добрий день!";
static const uint8_t    k_sizes[]         = { 12, 14, 16, 20, 24 };
static const char       *k_font_path      = "resources/Roboto-Regular.ttf";
#if defined(FONTRENDERER_USE_FREETYPE)
static const char       *k_hintings[]     = { "none", "light", "normal", "auto" };
#endif
static const char       *k_baked_metrics_path = "baked.frb";
static const char       *k_baked_texture_path = "baked.tga";
static const uint8_t    k_baked_size          = 18;

// Returns the box of the text, relative to the window, and adds the time fr_font_draw_text took to g_draw_seconds.
//-------------------------------------
static fr_rect
render_text(fr_font *font, const char *text, uint8_t height, uint32_t color, int32_t x, int32_t y,
            struct mfb_timer *timer) {
    fr_rect box = { 0, 0, 0, 0 };
    double start;
    fr_font_get_text_box(font, text, height, &box);
    if(g_show_bounding_box) {
        example_draw_box(x + box.x, y + box.y, box.width, box.height);
    }

    start = mfb_timer_now(timer);
    fr_font_draw_text(font, text, height, color, g_screen.buffer, g_screen.width, x, y);
    g_draw_seconds += mfb_timer_now(timer) - start;

    box.x += x;
    box.y += y;
    return box;
}

// Rendering the glyphs is the only work that depends on the backend. fr_font_draw_text copies them from the atlas,
// with the same code for every backend.
//-------------------------------------
static void
measure_raster(backend_info *backend, struct mfb_timer *timer) {
    fr_rect box;
    double  start;
    size_t  i;

    fr_font_reset(backend->font);
    start = mfb_timer_now(timer);
    for(i = 0; i < sizeof(k_sizes) / sizeof(k_sizes[0]); ++i) {
        fr_font_get_text_box(backend->font, k_greeting, k_sizes[i], &box);
    }
    fr_font_get_text_box(backend->font, k_more_greetings, 20, &box);
    backend->raster_milliseconds = (mfb_timer_now(timer) - start) * 1e3;
    backend->raster_measured     = true;
}

//-------------------------------------
static void
measure_raster_again(void) {
    size_t i;
    for(i = 0; i < g_backend_count; ++i) {
        g_backends[i].raster_measured = false;
    }
}

// Returns the bottom of the column.
//-------------------------------------
static int32_t
render_column(backend_info *backend, int32_t left, int32_t width, struct mfb_timer *timer) {
    fr_font *font = backend->font;
    int32_t  y    = k_margin;
    char     title[64];
    size_t   i;

    if(backend->raster_measured == false) {
        measure_raster(backend, timer);
    }

    // Besides the clip rectangle, each column only draws inside itself.
    fr_font_set_clipping(font, example_max(g_screen.clip_left, left), g_screen.clip_top,
                         example_min(g_screen.clip_right, left + width - k_margin), g_screen.clip_bottom);

    snprintf(title, sizeof(title), "%s · raster %.2f ms", backend->name, backend->raster_milliseconds);
    fr_font_draw_text(font, title, 18, backend->color, g_screen.buffer, g_screen.width, left, y);
    y += 30;

    for(i = 0; i < sizeof(k_sizes) / sizeof(k_sizes[0]); ++i) {
        render_text(font, k_greeting, k_sizes[i], k_text_color, left, y, timer);
        y += k_sizes[i] + 6;
    }
    y += 10;

    {
        const fr_rect box = render_text(font, k_more_greetings, 20, k_text_color, left, y, timer);
        return box.y + box.height;
    }
}

//-------------------------------------
static int32_t
get_status_height(void) {
#if defined(FONTRENDERER_USE_FREETYPE)
    // One more line for the settings of FreeType.
    return k_status_height + k_status_line_height;
#else
    return k_status_height;
#endif
}

// text_font writes the label, because a baked font only has the glyphs it was saved with.
//-------------------------------------
static void
render_atlas(fr_font *font, fr_font *text_font, const char *name, int32_t top) {
    const int32_t left   = k_margin;
    const int32_t right  = example_min((int32_t) g_screen.width - k_margin, (int32_t) g_screen.width);
    const int32_t bottom = example_min((int32_t) g_screen.height - get_status_height() - k_margin, (int32_t) g_screen.height);
    char          label[128];

    snprintf(label, sizeof(label), "Atlas of %s: %u × %u, %u × %u used", name,
             (unsigned) fr_font_get_texture_width(font), (unsigned) fr_font_get_texture_height(font),
             (unsigned) fr_font_get_used_texture_width(font), (unsigned) fr_font_get_used_texture_height(font));
    fr_font_draw_text(text_font, label, 14, k_status_color, g_screen.buffer, g_screen.width, left, top);
    top += 22;

    example_draw_texture(fr_font_get_texture(font), fr_font_get_texture_format(font) == FR_FONT_TEXTURE_FORMAT_BGRA32,
                         (int32_t) fr_font_get_texture_width(font), (int32_t) fr_font_get_texture_height(font), left, top, right, bottom);
}

//-------------------------------------
static size_t
get_atlas_count(void) {
    return (g_baked.font != NULL) ? g_backend_count + 1 : g_backend_count;
}

//-------------------------------------
static void
render_shown_atlas(int32_t top) {
    backend_info *shown;

    if(g_show_texture_id == g_backend_count) {
        char name[64];
        shown = &g_backends[g_baked.source];
        snprintf(name, sizeof(name), "baked %s", shown->name);
        fr_font_set_clipping(shown->font, g_screen.clip_left, g_screen.clip_top, g_screen.clip_right, g_screen.clip_bottom);
        render_atlas(g_baked.font, shown->font, name, top);
        return;
    }

    shown = &g_backends[g_show_texture_id];
    fr_font_set_clipping(shown->font, g_screen.clip_left, g_screen.clip_top, g_screen.clip_right, g_screen.clip_bottom);
    render_atlas(shown->font, shown->font, shown->name, top);
}

//-------------------------------------
static void
bake_font(size_t source) {
    fr_font   *font  = g_backends[source].font;
    fr_font   *baked = NULL;
    fr_status status;
    char      glyphs[256];

    // The label of the row needs the name of the backend and the digits of the sizes.
    snprintf(glyphs, sizeof(glyphs), "%s Baked from %s · files: 0123456789 + bytes", k_greeting, g_backends[source].name);
    fr_font_preload(font, glyphs, k_baked_size);
    fr_font_load_all_kerning_pairs(font);

    status = fr_font_save_baked(font, k_baked_metrics_path, k_baked_texture_path);
    if(status != FR_STATUS_OK) {
        fprintf(stderr, "Cannot save the baked font (status %d).\n", (int) status);
        return;
    }

    status = fr_font_create_baked(k_baked_metrics_path, k_baked_texture_path, &baked);
    if(status != FR_STATUS_OK) {
        fprintf(stderr, "Cannot load the baked font (status %d).\n", (int) status);
        return;
    }

    fr_font_destroy(g_baked.font);
    g_baked.font          = baked;
    g_baked.source        = source;
    g_baked.metrics_bytes = example_get_file_size(k_baked_metrics_path);
    g_baked.texture_bytes = example_get_file_size(k_baked_texture_path);
    g_show_texture_id     = g_backend_count;
}

// Returns the bottom of the row.
//-------------------------------------
static int32_t
render_baked_row(int32_t top, struct mfb_timer *timer) {
    const backend_info *source   = &g_backends[g_baked.source];
    fr_rect            label_box = { 0, 0, 0, 0 };
    fr_rect            greeting;
    char               label[96];

    fr_font_set_clipping(g_baked.font, g_screen.clip_left, g_screen.clip_top, g_screen.clip_right, g_screen.clip_bottom);

    snprintf(label, sizeof(label), "Baked from %s · files: %ld + %ld bytes", source->name, g_baked.metrics_bytes, g_baked.texture_bytes);
    fr_font_get_text_box(g_baked.font, label, k_baked_size, &label_box);
    fr_font_draw_text(g_baked.font, label, k_baked_size, source->color, g_screen.buffer, g_screen.width, k_margin, top);

    greeting = render_text(g_baked.font, k_greeting, k_baked_size, k_text_color, k_margin + label_box.x + label_box.width + 2 * k_margin, top, timer);
    return example_max(top + label_box.y + label_box.height, greeting.y + greeting.height);
}

//-------------------------------------
static const char *
on_off(bool value) {
    return value ? "ON" : "OFF";
}

//-------------------------------------
static void
render_status_bar(fr_font *font, double draw_microseconds, float fps) {
    const int32_t top         = (int32_t) g_screen.height - get_status_height();
    // One number key for each atlas, as in "1, 2, 3".
    const int     keys_length = (int) get_atlas_count() * 3 - 2;
#if defined(FONTRENDERER_USE_BAKED)
    const char    *bake_key   = " · S: bake atlas font";
#else
    const char    *bake_key   = "";
#endif
    char          line[256];
    fr_rect       box = { 0, 0, 0, 0 };

    example_fill_rect(0, top, (int32_t) g_screen.width, (int32_t) g_screen.height, UINT32_C(0xff000000), 170);

    // The status bar is never clipped.
    fr_font_set_clipping(font, 0, 0, (int32_t) g_screen.width, (int32_t) g_screen.height);

    snprintf(line, sizeof(line),
             "A antialias: %s · E extend: %s · W weights: %s · B boxes: %s · C clip view: %s · T atlas: %s",
             on_off(fr_font_get_antialias(font)), on_off(fr_font_get_antialias_allow_ex(font)),
             g_mean_weights ? "mean" : "gaussian", on_off(g_show_bounding_box),
             on_off(g_screen.show_clipping), on_off(g_show_texture));
    fr_font_draw_text(font, line, 13, k_status_color, g_screen.buffer, g_screen.width, k_margin, top + 6);

    snprintf(line, sizeof(line), "Tab: clip corner · Arrows: move clip · %.*s: atlas font%s · Esc: exit", keys_length, "1, 2, 3, 4", bake_key);
    fr_font_draw_text(font, line, 13, k_status_color, g_screen.buffer, g_screen.width, k_margin, top + 24);

    snprintf(line, sizeof(line), "DrawText %.0f µs · %.0f FPS", draw_microseconds, fps);
    fr_font_get_text_box(font, line, 13, &box);
    fr_font_draw_text(font, line, 13, k_status_color, g_screen.buffer, g_screen.width,
                      (int32_t) g_screen.width - k_margin - (box.x + box.width), top + 24);

#if defined(FONTRENDERER_USE_FREETYPE)
    snprintf(line, sizeof(line), "FreeType · H hinting: %s · M monochrome: %s · D stem darkening: %s",
             k_hintings[fr_font_ft_get_hinting(g_free_type)], on_off(fr_font_ft_get_monochrome(g_free_type)),
             on_off(fr_font_ft_get_stem_darkening(g_free_type)));
    fr_font_draw_text(font, line, 13, k_status_color, g_screen.buffer, g_screen.width, k_margin, top + 24 + k_status_line_height);
#endif
}

//-------------------------------------
static void
keyboard(struct mfb_window *window, mfb_key key, mfb_key_mod mod, bool is_pressed) {
    size_t i;
    (void) mod;

    example_handle_key(window, key, is_pressed);
    if(!is_pressed) {
        return;
    }

    switch(key) {
        case MFB_KB_KEY_B:
            g_show_bounding_box = !g_show_bounding_box;
            break;
        case MFB_KB_KEY_T:
            g_show_texture = !g_show_texture;
            break;
        case MFB_KB_KEY_1:
        case MFB_KB_KEY_2:
        case MFB_KB_KEY_3:
        case MFB_KB_KEY_4:
            if((size_t) (key - MFB_KB_KEY_1) < get_atlas_count()) {
                g_show_texture_id = (size_t) (key - MFB_KB_KEY_1);
            }
            break;
#if defined(FONTRENDERER_USE_BAKED)
        case MFB_KB_KEY_S:
            // With the baked atlas shown, its backend is saved again.
            bake_font((g_show_texture_id < g_backend_count) ? g_show_texture_id : g_baked.source);
            break;
#endif
        case MFB_KB_KEY_A:
            for(i = 0; i < g_backend_count; ++i) {
                fr_font_set_antialias(g_backends[i].font, !fr_font_get_antialias(g_backends[i].font));
            }
            measure_raster_again();
            break;
        case MFB_KB_KEY_E:
            for(i = 0; i < g_backend_count; ++i) {
                fr_font_set_antialias_allow_ex(g_backends[i].font, !fr_font_get_antialias_allow_ex(g_backends[i].font));
            }
            measure_raster_again();
            break;
        case MFB_KB_KEY_W:
            g_mean_weights = !g_mean_weights;
            for(i = 0; i < g_backend_count; ++i) {
                if(g_mean_weights) {
                    fr_font_set_antialias_weights(g_backends[i].font, 1, 1, 1);
                }
                else {
                    fr_font_set_antialias_weights(g_backends[i].font, 20, 4, 1);
                }
            }
            measure_raster_again();
            break;
#if defined(FONTRENDERER_USE_FREETYPE)
        case MFB_KB_KEY_H:
            fr_font_ft_set_hinting(g_free_type, (fr_font_ft_get_hinting(g_free_type) + 1) % 4);
            measure_raster_again();
            break;
        case MFB_KB_KEY_M:
            fr_font_ft_set_monochrome(g_free_type, !fr_font_ft_get_monochrome(g_free_type));
            measure_raster_again();
            break;
        case MFB_KB_KEY_D:
            fr_font_ft_set_stem_darkening(g_free_type, !fr_font_ft_get_stem_darkening(g_free_type));
            measure_raster_again();
            break;
#endif
        default:
            break;
    }
}

//-------------------------------------
static bool
add_backend(fr_font_backend id, const char *name, uint32_t color) {
    backend_info    *backend = &g_backends[g_backend_count];
    const fr_status status   = fr_font_create(k_font_path, id, &backend->font);
    if(status != FR_STATUS_OK) {
        fprintf(stderr, "Cannot load %s with %s (status %d).\n", k_font_path, name, (int) status);
        return false;
    }

    backend->name                = name;
    backend->color               = color;
    backend->raster_milliseconds = 0.0;
    backend->raster_measured     = false;
    fr_font_set_antialias(backend->font, true);
    fr_font_set_antialias_weights(backend->font, 20, 4, 1);
    ++g_backend_count;
    return true;
}

//-------------------------------------
static void
destroy_backends(void) {
    size_t i;
    fr_font_destroy(g_baked.font);
    g_baked.font = NULL;
    for(i = 0; i < g_backend_count; ++i) {
        fr_font_destroy(g_backends[i].font);
    }
    g_backend_count = 0;
}

//-------------------------------------
int
main(int argc, char *argv[]) {
    struct mfb_timer  *timer;
    struct mfb_window *window;
    mfb_update_state   state;
    const int          timed_frame_count = 30;
    int                timed_frames      = 0;
    double             draw_microseconds = 0.0;
    bool               loaded            = true;
    (void) argc;

    example_set_app_directory(argv[0]);
    timer = mfb_timer_create();

#if defined(FONTRENDERER_USE_LIBSCHRIFT)
    loaded = loaded && add_backend(FR_FONT_BACKEND_SFT, "libschrift", UINT32_C(0xffff9e80));
#endif
#if defined(FONTRENDERER_USE_STB)
    loaded = loaded && add_backend(FR_FONT_BACKEND_STB, "stb_truetype", UINT32_C(0xff80cbff));
#endif
#if defined(FONTRENDERER_USE_FREETYPE)
    loaded = loaded && add_backend(FR_FONT_BACKEND_FT, "FreeType", UINT32_C(0xffa5d6a7));
    if(loaded) {
        // Light hinting makes small text sharper without changing its spacing.
        g_free_type = g_backends[g_backend_count - 1].font;
        fr_font_ft_set_hinting(g_free_type, FR_FONT_FT_HINTING_LIGHT);
    }
#endif
    if(loaded == false) {
        destroy_backends();
        return -1;
    }

    window = example_open_window("Font Renderer C API");
    if(window == NULL) {
        destroy_backends();
        return -1;
    }
    mfb_set_keyboard_callback(window, keyboard);

    do {
        const int32_t column_width = ((int32_t) g_screen.width - k_margin) / (int32_t) g_backend_count;
        int32_t       bottom       = 0;
        size_t        i;

        example_clear();

        // The keys and the resize callback only change the globals, so each column applies the clipping every frame.
        for(i = 0; i < g_backend_count; ++i) {
            bottom = example_max(bottom, render_column(&g_backends[i], k_margin + (int32_t) i * column_width, column_width, timer));
        }
        if(++timed_frames == timed_frame_count) {
            draw_microseconds = g_draw_seconds / timed_frame_count * 1e6;
            g_draw_seconds    = 0.0;
            timed_frames      = 0;
        }

        if(g_baked.font != NULL) {
            bottom = render_baked_row(bottom + k_margin, timer);
        }

        if(g_show_texture) {
            render_shown_atlas(bottom + k_margin);
        }
        example_draw_clipping();

        render_status_bar(g_backends[0].font, draw_microseconds, example_get_fps());

        state = mfb_update_ex(window, g_screen.buffer, g_screen.width, g_screen.height);
        if(state != MFB_STATE_OK) {
            window = NULL;
            break;
        }
    } while(mfb_wait_sync(window));

    example_release();
    destroy_backends();
    mfb_timer_destroy(timer);
    return 0;
}
