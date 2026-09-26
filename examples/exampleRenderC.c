#include <MiniFB.h>
#include <FontC.h>
//-------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
    #include <direct.h>
    #define chdir _chdir
#else
    #include <unistd.h>
#endif

//-------------------------------------
typedef enum clip_border {
    CLIP_BORDER_LEFT,
    CLIP_BORDER_RIGHT,
    CLIP_BORDER_TOP,
    CLIP_BORDER_BOTTOM
} clip_border;

//-------------------------------------
// It is also a VESA mode, which the DOS backend of MiniFB needs.
static uint32_t     g_width   = 800;
static uint32_t     g_height  = 600;
static uint32_t    *g_buffer  = NULL;
static int32_t      g_clip_left   = 0;
static int32_t      g_clip_top    = 0;
static int32_t      g_clip_right  = 800;
static int32_t      g_clip_bottom = 600;

static bool         g_show_bounding_box = true;
static bool         g_show_clipping     = true;
static bool         g_show_texture      = true;
static int          g_show_texture_id   = 1;
static bool         g_mean_weights      = false;
static clip_border  g_selected_border_h = CLIP_BORDER_LEFT;
static clip_border  g_selected_border_v = CLIP_BORDER_TOP;
static fr_font     *g_font_sft           = NULL;
static fr_font     *g_font_stb           = NULL;
static struct mfb_timer *g_fps_timer     = NULL;

//-------------------------------------
static const int32_t    k_margin        = 16;
static const int32_t    k_status_height = 44;

static const uint32_t   k_text_color   = UINT32_C(0xfff4ece1);
static const uint32_t   k_sft_color    = UINT32_C(0xffff9e80);
static const uint32_t   k_stb_color    = UINT32_C(0xff80cbff);
static const uint32_t   k_status_color = UINT32_C(0xffdcdcdc);
static const uint32_t   k_highlight    = UINT32_C(0xffffd54f);

static const char       *k_greeting = "¡Hola Pepe! ¿Cómo estás?";
static const char       *k_pangram  = "El veloz murciélago hindú\ncomía feliz cardillo y kiwi.\n"
                                      "La cigüeña tocaba el saxofón\ndetrás del palenque de paja.";
static const uint8_t    k_sizes[]   = { 12, 16, 20, 24, 32 };

//-------------------------------------
static int32_t
min_i32(int32_t a, int32_t b) {
    return a < b ? a : b;
}

//-------------------------------------
static int32_t
max_i32(int32_t a, int32_t b) {
    return a > b ? a : b;
}

//-------------------------------------
static void
resize(struct mfb_window *window, int width, int height) {
    uint32_t *buffer;
    (void) window;

    // realloc may free the buffer when asked for 0 bytes.
    if(width <= 0 || height <= 0) {
        return;
    }

    // If it fails, the old buffer is still valid and keeps being shown.
    buffer = (uint32_t *) realloc(g_buffer, (size_t) width * (size_t) height * sizeof(*buffer));
    if(buffer == NULL) {
        return;
    }

    // A clip border on the window edge follows the edge. Any other border only has to stay inside the new buffer.
    if(g_clip_right == (int32_t) g_width || g_clip_right > width) {
        g_clip_right = width;
    }
    if(g_clip_bottom == (int32_t) g_height || g_clip_bottom > height) {
        g_clip_bottom = height;
    }

    g_clip_left = min_i32(g_clip_left, g_clip_right);
    g_clip_top  = min_i32(g_clip_top, g_clip_bottom);
    g_buffer    = buffer;
    g_width     = (uint32_t) width;
    g_height    = (uint32_t) height;
}

//-------------------------------------
static uint32_t
blend(uint32_t dst, uint32_t src, uint32_t alpha) {
    const uint32_t inverse = 255 - alpha;
    const uint32_t r = (((src >> 16) & 0xff) * alpha + ((dst >> 16) & 0xff) * inverse) / 255;
    const uint32_t g = (((src >>  8) & 0xff) * alpha + ((dst >>  8) & 0xff) * inverse) / 255;
    const uint32_t b = (( src        & 0xff) * alpha + ( dst        & 0xff) * inverse) / 255;

    return UINT32_C(0xff000000) | (r << 16) | (g << 8) | b;
}

//-------------------------------------
static void
fill_rect(int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t color, uint32_t alpha) {
    int32_t y;
    left   = max_i32(left, 0);
    top    = max_i32(top, 0);
    right  = min_i32(right, (int32_t) g_width);
    bottom = min_i32(bottom, (int32_t) g_height);

    for(y = top; y < bottom; ++y) {
        int32_t x;
        uint32_t *row = &g_buffer[(size_t) y * g_width];
        for(x = left; x < right; ++x) {
            row[x] = blend(row[x], color, alpha);
        }
    }
}

//-------------------------------------
static void
render_bounding_box(int32_t pos_x, int32_t pos_y, const fr_rect *rect) {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    if(rect->width <= 0 || rect->height <= 0) {
        return;
    }

    left   = pos_x + rect->x;
    top    = pos_y + rect->y;
    right  = left + rect->width;
    bottom = top + rect->height;

    fill_rect(left,      top,        right,    top + 1,    UINT32_C(0xffffffff), 64);
    fill_rect(left,      bottom - 1, right,    bottom,     UINT32_C(0xffffffff), 64);
    fill_rect(left,      top + 1,    left + 1, bottom - 1, UINT32_C(0xffffffff), 64);
    fill_rect(right - 1, top + 1,    right,    bottom - 1, UINT32_C(0xffffffff), 64);
}

// Returns the box of the text, relative to the window, and adds the time fr_font_draw_text took to seconds.
//-------------------------------------
static fr_rect
render_text(fr_font *font, const char *text, uint8_t height, uint32_t color, int32_t x, int32_t y,
            struct mfb_timer *timer, double *seconds) {
    fr_rect box = { 0, 0, 0, 0 };
    double start;
    fr_font_get_text_box(font, text, height, &box);
    if(g_show_bounding_box) {
        render_bounding_box(x, y, &box);
    }

    start = mfb_timer_now(timer);
    fr_font_draw_text(font, text, height, color, g_buffer, g_width, x, y);
    *seconds += mfb_timer_now(timer) - start;

    box.x += x;
    box.y += y;
    return box;
}

// Returns the time the column took to draw, and its bottom in bottom.
//-------------------------------------
static double
render_column(fr_font *font, const char *name, uint32_t name_color, int32_t left,
              struct mfb_timer *timer, int32_t *bottom) {
    double  seconds = 0.0;
    int32_t y       = k_margin;
    size_t  i;

    fr_font_draw_text(font, name, 18, name_color, g_buffer, g_width, left, y);
    y += 30;

    for(i = 0; i < sizeof(k_sizes) / sizeof(k_sizes[0]); ++i) {
        render_text(font, k_greeting, k_sizes[i], k_text_color, left, y, timer, &seconds);
        y += k_sizes[i] + 6;
    }
    y += 10;

    {
        const fr_rect box = render_text(font, k_pangram, 20, k_text_color, left, y, timer, &seconds);
        *bottom = box.y + box.height;
    }
    return seconds;
}

//-------------------------------------
static void
render_atlas(fr_font *font, const char *name, int32_t top) {
    const int32_t left   = k_margin;
    const int32_t right  = min_i32((int32_t) g_width - k_margin, (int32_t) g_width);
    const int32_t bottom = min_i32((int32_t) g_height - k_status_height - k_margin, (int32_t) g_height);
    char          label[128];
    const uint8_t *texels;
    int32_t       texture_width;
    int32_t       shown_right;
    int32_t       shown_bottom;
    int32_t       y;

    snprintf(label, sizeof(label), "Atlas of %s: %u × %u, %u × %u used", name,
             (unsigned) fr_font_get_texture_width(font), (unsigned) fr_font_get_texture_height(font),
             (unsigned) fr_font_get_used_texture_width(font), (unsigned) fr_font_get_used_texture_height(font));
    fr_font_draw_text(font, label, 14, k_status_color, g_buffer, g_width, left, top);
    top += 22;

    // The checkerboard shows which texels are empty. A texture bigger than the panel is cropped.
    texels         = fr_font_get_texture(font);
    texture_width  = (int32_t) fr_font_get_texture_width(font);
    shown_right    = min_i32(right, left + texture_width);
    shown_bottom   = min_i32(bottom, top + (int32_t) fr_font_get_texture_height(font));
    for(y = max_i32(top, 0); y < shown_bottom; ++y) {
        int32_t x;
        uint32_t *row = &g_buffer[(size_t) y * g_width];
        for(x = left; x < shown_right; ++x) {
            const int32_t texel_x = x - left;
            const int32_t texel_y = y - top;
            const uint32_t square = ((texel_x / 8 + texel_y / 8) % 2 != 0) ? UINT32_C(0xff383838) : UINT32_C(0xff4a4a4a);
            row[x] = blend(square, UINT32_C(0xffffffff), texels[(size_t) texel_y * (size_t) texture_width + (size_t) texel_x]);
        }
    }

    {
        const fr_rect box = { 0, 0, shown_right - left + 2, shown_bottom - top + 2 };
        render_bounding_box(left - 1, top - 1, &box);
    }
}

//-------------------------------------
static void
render_clipping(void) {
    const int32_t width  = (int32_t) g_width;
    const int32_t height = (int32_t) g_height;
    const int32_t x = (g_selected_border_h == CLIP_BORDER_LEFT) ? g_clip_left : g_clip_right - 2;
    const int32_t y = (g_selected_border_v == CLIP_BORDER_TOP)  ? g_clip_top  : g_clip_bottom - 2;

    // Everything outside the clip rectangle is darkened.
    fill_rect(0,            0,             width,        g_clip_top,    UINT32_C(0xff000000), 150);
    fill_rect(0,            g_clip_bottom, width,        height,        UINT32_C(0xff000000), 150);
    fill_rect(0,            g_clip_top,    g_clip_left,  g_clip_bottom, UINT32_C(0xff000000), 150);
    fill_rect(g_clip_right, g_clip_top,    width,        g_clip_bottom, UINT32_C(0xff000000), 150);

    // The borders the arrow keys move.
    fill_rect(x,           g_clip_top, x + 2,        g_clip_bottom, k_highlight, 255);
    fill_rect(g_clip_left, y,          g_clip_right, y + 2,        k_highlight, 255);
}

//-------------------------------------
static const char *
on_off(bool value) {
    return value ? "ON" : "OFF";
}

//-------------------------------------
static void
render_status_bar(fr_font *font, double sft_microseconds, double stb_microseconds, float fps) {
    const int32_t top = (int32_t) g_height - k_status_height;
    char          line[256];
    fr_rect       box = { 0, 0, 0, 0 };

    fill_rect(0, top, (int32_t) g_width, (int32_t) g_height, UINT32_C(0xff000000), 170);

    // The status bar is never clipped.
    fr_font_set_clipping(font, 0, 0, (int32_t) g_width, (int32_t) g_height);

    snprintf(line, sizeof(line),
             "A antialias: %s · E extend: %s · W weights: %s · B boxes: %s · C clip view: %s · T atlas: %s",
             on_off(fr_font_get_antialias(font)), on_off(fr_font_get_antialias_allow_ex(font)),
             g_mean_weights ? "mean" : "gaussian", on_off(g_show_bounding_box),
             on_off(g_show_clipping), on_off(g_show_texture));
    fr_font_draw_text(font, line, 13, k_status_color, g_buffer, g_width, k_margin, top + 6);

    fr_font_draw_text(font, "Tab: clip corner · Arrows: move clip · 1, 2: atlas font · Esc: exit",
                      13, k_status_color, g_buffer, g_width, k_margin, top + 24);

    snprintf(line, sizeof(line), "libschrift %.0f µs · stb_truetype %.0f µs · %.0f FPS",
             sft_microseconds, stb_microseconds, fps);
    fr_font_get_text_box(font, line, 13, &box);
    fr_font_draw_text(font, line, 13, k_status_color, g_buffer, g_width,
                      (int32_t) g_width - k_margin - (box.x + box.width), top + 24);
}

//-------------------------------------
static float
get_fps(void) {
    static uint32_t frame_count = 0;
    static float fps = 60.0f;

    ++frame_count;
    if(frame_count >= 60) {
        const double time = mfb_timer_now(g_fps_timer);
        frame_count = 0;
        mfb_timer_reset(g_fps_timer);
        fps = (float) (60.0 / time);
    }

    return fps;
}

//-------------------------------------
static void
set_app_directory(const char *argv0) {
    const char *slash     = strrchr(argv0, '/');
    const char *backslash = strrchr(argv0, '\\');
    const char *separator = slash;
    size_t      length;
    char       *directory;

    if(backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    if(separator == NULL) {
        return;
    }

    // Keep the separator: "C:" alone is the current directory of drive C, and "/app" would give "".
    length = (size_t) (separator - argv0) + 1;
    directory = (char *) malloc(length + 1);
    if(directory == NULL) {
        return;
    }
    memcpy(directory, argv0, length);
    directory[length] = '\0';
    chdir(directory);
    free(directory);
}

//-------------------------------------
static void
keyboard(struct mfb_window *window, mfb_key key, mfb_key_mod mod, bool is_pressed) {
    (void) mod;

    switch(key) {
        case MFB_KB_KEY_LEFT:
            if(g_selected_border_h == CLIP_BORDER_LEFT) {
                if(g_clip_left > 0) {
                    --g_clip_left;
                }
            }
            else if(g_clip_right > 0 && g_clip_right > g_clip_left) {
                --g_clip_right;
            }
            break;

        case MFB_KB_KEY_RIGHT:
            if(g_selected_border_h == CLIP_BORDER_LEFT) {
                if(g_clip_left < (int32_t) g_width && g_clip_left < g_clip_right) {
                    ++g_clip_left;
                }
            }
            else if(g_clip_right < (int32_t) g_width) {
                ++g_clip_right;
            }
            break;

        case MFB_KB_KEY_DOWN:
            if(g_selected_border_v == CLIP_BORDER_TOP) {
                if(g_clip_top < (int32_t) g_height && g_clip_top < g_clip_bottom) {
                    ++g_clip_top;
                }
            }
            else if(g_clip_bottom < (int32_t) g_height) {
                ++g_clip_bottom;
            }
            break;

        case MFB_KB_KEY_UP:
            if(g_selected_border_v == CLIP_BORDER_TOP) {
                if(g_clip_top > 0) {
                    --g_clip_top;
                }
            }
            else if(g_clip_bottom > 0 && g_clip_bottom > g_clip_top) {
                --g_clip_bottom;
            }
            break;
    }

    if(!is_pressed) {
        return;
    }

    switch(key) {
        case MFB_KB_KEY_ESCAPE:
            mfb_close(window);
            break;
        case MFB_KB_KEY_B:
            g_show_bounding_box = !g_show_bounding_box;
            break;
        case MFB_KB_KEY_C:
            g_show_clipping = !g_show_clipping;
            break;
        case MFB_KB_KEY_T:
            g_show_texture = !g_show_texture;
            break;
        case MFB_KB_KEY_1:
            g_show_texture_id = 1;
            break;
        case MFB_KB_KEY_2:
            g_show_texture_id = 2;
            break;
        case MFB_KB_KEY_A:
            fr_font_set_antialias(g_font_sft, !fr_font_get_antialias(g_font_sft));
            fr_font_set_antialias(g_font_stb, !fr_font_get_antialias(g_font_stb));
            break;
        case MFB_KB_KEY_E:
            fr_font_set_antialias_allow_ex(g_font_sft, !fr_font_get_antialias_allow_ex(g_font_sft));
            fr_font_set_antialias_allow_ex(g_font_stb, !fr_font_get_antialias_allow_ex(g_font_stb));
            break;
        case MFB_KB_KEY_W:
            g_mean_weights = !g_mean_weights;
            if(g_mean_weights) {
                fr_font_set_antialias_weights(g_font_sft, 1, 1, 1);
                fr_font_set_antialias_weights(g_font_stb, 1, 1, 1);
            }
            else {
                fr_font_set_antialias_weights(g_font_sft, 20, 4, 1);
                fr_font_set_antialias_weights(g_font_stb, 20, 4, 1);
            }
            break;
        case MFB_KB_KEY_TAB:
            if(g_selected_border_h == CLIP_BORDER_LEFT) {
                g_selected_border_h = CLIP_BORDER_RIGHT;
                g_selected_border_v = CLIP_BORDER_BOTTOM;
            }
            else {
                g_selected_border_h = CLIP_BORDER_LEFT;
                g_selected_border_v = CLIP_BORDER_TOP;
            }
            break;
    }
}

//-------------------------------------
int
main(int argc, char *argv[]) {
    struct mfb_timer  *timer;
    struct mfb_window *window;
    mfb_update_state   state;
    const int          timed_frame_count = 30;
    double             sum_sft           = 0.0;
    double             sum_stb           = 0.0;
    double             shown_sft         = 0.0;
    double             shown_stb         = 0.0;
    int                timed_frames      = 0;
    fr_status          status;
    (void) argc;

    set_app_directory(argv[0]);
    timer       = mfb_timer_create();
    g_fps_timer = mfb_timer_create();

    status = fr_font_create("resources/Roboto-Regular.ttf", FR_FONT_BACKEND_SFT, &g_font_sft);
    if(status != FR_STATUS_OK) {
        fprintf(stderr, "Cannot create the libschrift font (status %d).\n", (int) status);
        return -1;
    }
    status = fr_font_create("resources/Roboto-Regular.ttf", FR_FONT_BACKEND_STB, &g_font_stb);
    if(status != FR_STATUS_OK) {
        fprintf(stderr, "Cannot create the stb_truetype font (status %d).\n", (int) status);
        fr_font_destroy(g_font_sft);
        return -1;
    }

    fr_font_set_antialias(g_font_sft, true);
    fr_font_set_antialias(g_font_stb, true);
    fr_font_set_antialias_weights(g_font_sft, 20, 4, 1);
    fr_font_set_antialias_weights(g_font_stb, 20, 4, 1);

    window = mfb_open_ex("Font Renderer C API", g_width, g_height, MFB_WF_RESIZABLE);
    if(window == NULL) {
        fprintf(stderr, "Cannot create window!\n");
        fr_font_destroy(g_font_stb);
        fr_font_destroy(g_font_sft);
        return -1;
    }

    g_buffer = (uint32_t *) calloc((size_t) g_width * g_height, sizeof(*g_buffer));
    if(g_buffer == NULL) {
        fprintf(stderr, "Cannot allocate the display buffer!\n");
        mfb_close(window);
        fr_font_destroy(g_font_stb);
        fr_font_destroy(g_font_sft);
        return -1;
    }

    mfb_set_resize_callback(window, resize);
    mfb_set_keyboard_callback(window, keyboard);

    do {
        uint32_t *screen = g_buffer;
        uint32_t  y;
        int32_t   bottom_sft;
        int32_t   bottom_stb;

        for(y = 0; y < g_height; ++y) {
            const uint8_t shade = (uint8_t) (32 + (y * 128) / g_height);
            memset(screen, shade, (size_t) g_width * sizeof(*screen));
            screen += g_width;
        }

        // The keys and the resize callback only change the globals, so the clipping is applied here every frame.
        fr_font_set_clipping(g_font_sft, g_clip_left, g_clip_top, g_clip_right, g_clip_bottom);
        fr_font_set_clipping(g_font_stb, g_clip_left, g_clip_top, g_clip_right, g_clip_bottom);

        sum_sft += render_column(g_font_sft, "libschrift", k_sft_color, k_margin, timer, &bottom_sft);
        sum_stb += render_column(g_font_stb, "stb_truetype", k_stb_color,
                                 (int32_t) (g_width / 2) + k_margin, timer, &bottom_stb);
        if(++timed_frames == timed_frame_count) {
            shown_sft   = sum_sft / timed_frame_count * 1e6;
            shown_stb   = sum_stb / timed_frame_count * 1e6;
            sum_sft     = 0.0;
            sum_stb     = 0.0;
            timed_frames = 0;
        }

        if(g_show_texture) {
            render_atlas(g_show_texture_id == 1 ? g_font_sft : g_font_stb,
                         g_show_texture_id == 1 ? "libschrift" : "stb_truetype",
                         max_i32(bottom_sft, bottom_stb) + k_margin);
        }
        if(g_show_clipping) {
            render_clipping();
        }

        render_status_bar(g_font_stb, shown_sft, shown_stb, get_fps());

        state = mfb_update_ex(window, g_buffer, g_width, g_height);
        if(state != MFB_STATE_OK) {
            window = NULL;
            break;
        }
    } while(mfb_wait_sync(window));

    free(g_buffer);
    g_buffer = NULL;
    fr_font_destroy(g_font_stb);
    fr_font_destroy(g_font_sft);
    mfb_timer_destroy(g_fps_timer);
    mfb_timer_destroy(timer);
    return 0;
}

