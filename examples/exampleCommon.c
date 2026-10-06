#include "exampleCommon.h"
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
// The DOS backend of MiniFB needs a VESA mode. Elsewhere the window is only as tall as its content.
#define WINDOW_WIDTH    1024
#if defined(__DJGPP__)
    #define WINDOW_HEIGHT   768
#else
    #define WINDOW_HEIGHT   512
#endif

example_screen g_screen = { NULL, WINDOW_WIDTH, WINDOW_HEIGHT, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, true };

// Tab swaps the top left corner for the bottom right one.
static bool              g_bottom_right_selected = false;
static struct mfb_timer *g_fps_timer             = NULL;

static const uint32_t    k_highlight = UINT32_C(0xffffd54f);

//-------------------------------------
static void
resize(struct mfb_window *window, int width, int height) {
    uint32_t *buffer;
    (void) window;

    // realloc may free the buffer when asked for 0 bytes.
    if (width <= 0 || height <= 0) {
        return;
    }

    // If it fails, the old buffer is still valid and keeps being shown.
    buffer = (uint32_t *) realloc(g_screen.buffer, (size_t) width * (size_t) height * sizeof(*buffer));
    if (buffer == NULL) {
        return;
    }

    // A clip border on the window edge follows the edge. Any other border only has to stay inside the new buffer.
    if (g_screen.clip_right == (int32_t) g_screen.width || g_screen.clip_right > width) {
        g_screen.clip_right = width;
    }
    if (g_screen.clip_bottom == (int32_t) g_screen.height || g_screen.clip_bottom > height) {
        g_screen.clip_bottom = height;
    }

    g_screen.clip_left = example_min(g_screen.clip_left, g_screen.clip_right);
    g_screen.clip_top  = example_min(g_screen.clip_top, g_screen.clip_bottom);
    g_screen.buffer    = buffer;
    g_screen.width     = (uint32_t) width;
    g_screen.height    = (uint32_t) height;
}

//-------------------------------------
void
example_set_app_directory(const char *argv0) {
    const char *slash     = strrchr(argv0, '/');
    const char *backslash = strrchr(argv0, '\\');
    const char *separator = slash;
    size_t      length;
    char       *directory;

    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    if (separator == NULL) {
        return;
    }

    // Keep the separator: "C:" alone is the current directory of drive C, and "/app" would give "".
    length = (size_t) (separator - argv0) + 1;
    directory = (char *) malloc(length + 1);
    if (directory == NULL) {
        return;
    }
    memcpy(directory, argv0, length);
    directory[length] = '\0';
    chdir(directory);
    free(directory);
}

//-------------------------------------
struct mfb_window *
example_open_window(const char *title) {
    struct mfb_window *window = mfb_open_ex(title, g_screen.width, g_screen.height, MFB_WF_RESIZABLE);
    if (window == NULL) {
        fprintf(stderr, "Cannot create window!\n");
        return NULL;
    }

    g_screen.buffer = (uint32_t *) calloc((size_t) g_screen.width * g_screen.height, sizeof(*g_screen.buffer));
    if (g_screen.buffer == NULL) {
        fprintf(stderr, "Cannot allocate the display buffer!\n");
        mfb_close(window);
        return NULL;
    }

    g_fps_timer = mfb_timer_create();
    mfb_set_resize_callback(window, resize);
    return window;
}

//-------------------------------------
void
example_release(void) {
    free(g_screen.buffer);
    g_screen.buffer = NULL;
    mfb_timer_destroy(g_fps_timer);
    g_fps_timer = NULL;
}

//-------------------------------------
void
example_clear(void) {
    uint32_t *row = g_screen.buffer;
    uint32_t  x, y;
    for (y = 0; y < g_screen.height; ++y) {
        const uint32_t shade = 32 + (y * 128) / g_screen.height;
        const uint32_t color = UINT32_C(0xff000000) | (shade * UINT32_C(0x010101));
        for (x = 0; x < g_screen.width; ++x) {
            row[x] = color;
        }
        row += g_screen.width;
    }
}

//-------------------------------------
uint32_t
example_blend(uint32_t dst, uint32_t src, uint32_t alpha) {
    const uint32_t inverse = 255 - alpha;
    const uint32_t r = (((src >> 16) & 0xff) * alpha + ((dst >> 16) & 0xff) * inverse) / 255;
    const uint32_t g = (((src >>  8) & 0xff) * alpha + ((dst >>  8) & 0xff) * inverse) / 255;
    const uint32_t b = (( src        & 0xff) * alpha + ( dst        & 0xff) * inverse) / 255;

    return UINT32_C(0xff000000) | (r << 16) | (g << 8) | b;
}

//-------------------------------------
void
example_fill_rect(int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t color, uint32_t alpha) {
    int32_t y;
    left   = example_max(left, 0);
    top    = example_max(top, 0);
    right  = example_min(right, (int32_t) g_screen.width);
    bottom = example_min(bottom, (int32_t) g_screen.height);

    for (y = top; y < bottom; ++y) {
        int32_t  x;
        uint32_t *row = &g_screen.buffer[(size_t) y * g_screen.width];
        for (x = left; x < right; ++x) {
            row[x] = example_blend(row[x], color, alpha);
        }
    }
}

//-------------------------------------
void
example_draw_box(int32_t left, int32_t top, int32_t width, int32_t height) {
    const int32_t right  = left + width;
    const int32_t bottom = top + height;
    if (width <= 0 || height <= 0) {
        return;
    }

    example_fill_rect(left,      top,        right,    top + 1,    UINT32_C(0xffffffff), 64);
    example_fill_rect(left,      bottom - 1, right,    bottom,     UINT32_C(0xffffffff), 64);
    example_fill_rect(left,      top + 1,    left + 1, bottom - 1, UINT32_C(0xffffffff), 64);
    example_fill_rect(right - 1, top + 1,    right,    bottom - 1, UINT32_C(0xffffffff), 64);
}

//-------------------------------------
void
example_draw_texture(const uint8_t *texels, bool bgra, int32_t width, int32_t height, int32_t left, int32_t top, int32_t right, int32_t bottom) {
    const int32_t shown_right  = example_min(right, left + width);
    const int32_t shown_bottom = example_min(bottom, top + height);
    int32_t       y;

    for (y = example_max(top, 0); y < shown_bottom; ++y) {
        int32_t  x;
        uint32_t *row = &g_screen.buffer[(size_t) y * g_screen.width];
        for (x = example_max(left, 0); x < shown_right; ++x) {
            const int32_t  texel_x = x - left;
            const int32_t  texel_y = y - top;
            const uint32_t square  = ((texel_x / 8 + texel_y / 8) % 2 != 0) ? UINT32_C(0xff383838) : UINT32_C(0xff4a4a4a);
            const size_t   texel   = (size_t) texel_y * (size_t) width + (size_t) texel_x;
            if (bgra) {
                const uint8_t  *bgra_texel = &texels[texel * 4];
                const uint32_t inverse     = 255u - bgra_texel[3];
                const uint32_t r           = bgra_texel[2] + (((square >> 16) & 0xff) * inverse) / 255;
                const uint32_t g           = bgra_texel[1] + (((square >>  8) & 0xff) * inverse) / 255;
                const uint32_t b           = bgra_texel[0] + (( square        & 0xff) * inverse) / 255;
                row[x] = UINT32_C(0xff000000) | (r << 16) | (g << 8) | b;
            }
            else {
                row[x] = example_blend(square, UINT32_C(0xffffffff), texels[texel]);
            }
        }
    }

    example_draw_box(left - 1, top - 1, shown_right - left + 2, shown_bottom - top + 2);
}

//-------------------------------------
void
example_draw_clipping(void) {
    const int32_t width  = (int32_t) g_screen.width;
    const int32_t height = (int32_t) g_screen.height;
    const int32_t left   = g_screen.clip_left;
    const int32_t top    = g_screen.clip_top;
    const int32_t right  = g_screen.clip_right;
    const int32_t bottom = g_screen.clip_bottom;
    const int32_t x      = g_bottom_right_selected ? right - 2  : left;
    const int32_t y      = g_bottom_right_selected ? bottom - 2 : top;
    if (g_screen.show_clipping == false) {
        return;
    }

    // Everything outside the clip rectangle is darkened.
    example_fill_rect(0,     0,      width, top,    UINT32_C(0xff000000), 150);
    example_fill_rect(0,     bottom, width, height, UINT32_C(0xff000000), 150);
    example_fill_rect(0,     top,    left,  bottom, UINT32_C(0xff000000), 150);
    example_fill_rect(right, top,    width, bottom, UINT32_C(0xff000000), 150);

    // The borders the arrow keys move.
    example_fill_rect(x,    top, x + 2, bottom, k_highlight, 255);
    example_fill_rect(left, y,   right, y + 2,  k_highlight, 255);
}

//-------------------------------------
void
example_handle_key(struct mfb_window *window, mfb_key key, bool is_pressed) {
    switch (key) {
        case MFB_KB_KEY_LEFT:
            if (g_bottom_right_selected == false) {
                if (g_screen.clip_left > 0) {
                    --g_screen.clip_left;
                }
            }
            else if (g_screen.clip_right > 0 && g_screen.clip_right > g_screen.clip_left) {
                --g_screen.clip_right;
            }
            break;

        case MFB_KB_KEY_RIGHT:
            if (g_bottom_right_selected == false) {
                if (g_screen.clip_left < (int32_t) g_screen.width && g_screen.clip_left < g_screen.clip_right) {
                    ++g_screen.clip_left;
                }
            }
            else if (g_screen.clip_right < (int32_t) g_screen.width) {
                ++g_screen.clip_right;
            }
            break;

        case MFB_KB_KEY_DOWN:
            if (g_bottom_right_selected == false) {
                if (g_screen.clip_top < (int32_t) g_screen.height && g_screen.clip_top < g_screen.clip_bottom) {
                    ++g_screen.clip_top;
                }
            }
            else if (g_screen.clip_bottom < (int32_t) g_screen.height) {
                ++g_screen.clip_bottom;
            }
            break;

        case MFB_KB_KEY_UP:
            if (g_bottom_right_selected == false) {
                if (g_screen.clip_top > 0) {
                    --g_screen.clip_top;
                }
            }
            else if (g_screen.clip_bottom > 0 && g_screen.clip_bottom > g_screen.clip_top) {
                --g_screen.clip_bottom;
            }
            break;

        default:
            break;
    }

    if (is_pressed == false) {
        return;
    }

    switch (key) {
        case MFB_KB_KEY_ESCAPE:
            mfb_close(window);
            break;
        case MFB_KB_KEY_TAB:
            g_bottom_right_selected = !g_bottom_right_selected;
            break;
        case MFB_KB_KEY_C:
            g_screen.show_clipping = !g_screen.show_clipping;
            break;
        default:
            break;
    }
}

//-------------------------------------
float
example_get_fps(void) {
    static uint32_t frame_count = 0;
    static float    fps         = 60.0f;

    ++frame_count;
    if (frame_count >= 60) {
        const double time = mfb_timer_now(g_fps_timer);
        frame_count = 0;
        mfb_timer_reset(g_fps_timer);
        fps = (float) (60.0 / time);
    }

    return fps;
}

//-------------------------------------
long
example_get_file_size(const char *file_name) {
    FILE *file = fopen(file_name, "rb");
    long size;

    if (file == NULL) {
        return 0;
    }

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fclose(file);
    return size;
}
