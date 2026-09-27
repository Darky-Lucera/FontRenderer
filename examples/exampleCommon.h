#pragma once

// The window, drawing and clip rectangle code that both examples share, so that each example only shows FontRenderer.

#include <MiniFB.h>
//-------------------------------------
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The buffer the examples draw into, which follows the size of the window, and the clip rectangle, which the arrow keys move.
//-------------------------------------
typedef struct example_screen {
    uint32_t *buffer;
    uint32_t  width;
    uint32_t  height;
    int32_t   clip_left;
    int32_t   clip_top;
    int32_t   clip_right;
    int32_t   clip_bottom;
    bool      show_clipping;
} example_screen;

extern example_screen g_screen;

// So that the program finds its resources wherever it is run from.
void               example_set_app_directory(const char *argv0);
// Returns NULL, after printing why, if the window or its buffer cannot be created.
struct mfb_window *example_open_window(const char *title);
// Frees what example_open_window created, once the window is closed.
void               example_release(void);

void               example_clear(void);
uint32_t           example_blend(uint32_t dst, uint32_t src, uint32_t alpha);
void               example_fill_rect(int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t color, uint32_t alpha);
void               example_draw_box(int32_t left, int32_t top, int32_t width, int32_t height);
// Only while show_clipping is on.
void               example_draw_clipping(void);

// The keys of every example: the arrows move the selected borders of the clip rectangle, Tab selects the other two,
// C shows or hides the clip rectangle, and Esc closes the window.
void               example_handle_key(struct mfb_window *window, mfb_key key, bool is_pressed);
float              example_get_fps(void);

static inline int32_t example_min(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t example_max(int32_t a, int32_t b) { return a > b ? a : b; }

#ifdef __cplusplus
} // extern "C"
#endif
