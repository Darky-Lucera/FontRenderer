#include "exampleCommon.h"
#include <FontBaked.h>
#if defined(FONTRENDERER_USE_STB)
    #include <FontSTB.h>
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
    #include <FontSFT.h>
#else
    #include <FontFT.h>
#endif
//-------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdio>

// Draws with the font that exampleBakeEffects bakes, after tools/add_effects.py added an outline, a shadow and a bevel
// to its texture. Without that texture, it draws with the plain one that exampleBakeEffects saved.

//-------------------------------------
#if defined(FONTRENDERER_USE_STB)
using LabelFont = MindShake::FontSTB;
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
using LabelFont = MindShake::FontSFT;
#else
using LabelFont = MindShake::FontFT;
#endif

//-------------------------------------
struct Line {
    const char  *text;
    uint32_t    color;
    const char  *caption;
};

//-------------------------------------
static const char       *kMetricsPath        = "resources/effects.frb";
static const char       *kEffectsTexturePath = "resources/effectfx.tga";
static const char       *kPlainTexturePath   = "resources/effects.tga";
static const char       *kLabelFontPath      = "resources/Roboto-Regular.ttf";
static const uint8_t    kSize                = 32;
static const uint8_t    kLabelSize           = 13;

static const int32_t    kMargin              = 24;
static const int32_t    kStatusHeight        = 28;
static const uint32_t   kSkyTop              = 0xff141a33;
static const uint32_t   kSkyBottom           = 0xff3a1f52;
static const uint32_t   kLabelColor          = 0xffa3a8c8;

// White keeps the colors of the texture. Any other color multiplies them, so the dark outline stays dark.
static const Line       kLines[] = {
    { "FontRenderer",               0xffffffff, "color 0xffffffff keeps the colors of the texture"  },
    { "LEVEL 3: CRYSTAL CAVES",     0xff7cf2c4, "color 0xff7cf2c4 multiplies them"                  },
    { "SCORE 0012340",              0xffffd35a, "color 0xffffd35a"                                  },
    { "The quick brown fox jumps!", 0xff7cc8ff, "color 0xff7cc8ff"                                  },
};
static const char       *kBlinkingText       = "Press START";
static const uint32_t   kBlinkingColor       = 0x00ff7a7a;

static bool             gShowTexture         = true;

//-------------------------------------
static void
DrawBackground() {
    for (uint32_t y = 0; y < g_screen.height; ++y) {
        const uint32_t shade = (y * 255) / std::max(g_screen.height, 1u);
        const uint32_t color = example_blend(kSkyTop, kSkyBottom, shade);
        std::fill_n(&g_screen.buffer[size_t(y) * g_screen.width], g_screen.width, color);
    }

    // Stars, from a fixed seed so they stay still.
    uint32_t seed = 12345;
    auto random = [&seed](uint32_t range) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % range;
    };
    for (int i = 0; i < 90; ++i) {
        const int32_t  x          = int32_t(random(g_screen.width));
        const int32_t  y          = int32_t(random(g_screen.height));
        const uint32_t brightness = 40 + random(140);
        example_fill_rect(x, y, x + 1, y + 1, 0xffffffff, brightness);
    }
}

//-------------------------------------
static void
Keyboard(struct mfb_window *window, mfb_key key, mfb_key_mod mod, bool isPressed) {
    (void) mod;
    example_handle_key(window, key, isPressed);
    if (isPressed && key == MFB_KB_KEY_T) {
        gShowTexture = !gShowTexture;
    }
}

//-------------------------------------
int
main(int argc, char *argv[]) {
    (void) argc;
    example_set_app_directory(argv[0]);

    // A separate script makes the texture with the effects, so it may not exist yet.
    const bool      hasEffects  = example_get_file_size(kEffectsTexturePath) > 0;
    const char      *texturePath = hasEffects ? kEffectsTexturePath : kPlainTexturePath;
    MindShake::FontBaked font(kMetricsPath, texturePath);
    if (font.GetStatus() != MindShake::Font::EStatus::Ok) {
        fprintf(stderr, "Cannot load %s and %s (status %d). Run exampleBakeEffects first.\n", kMetricsPath, texturePath, int(font.GetStatus()));
        return -1;
    }

    LabelFont labels(kLabelFontPath);
    if (labels.GetStatus() != MindShake::Font::EStatus::Ok) {
        fprintf(stderr, "Cannot load %s (status %d).\n", kLabelFontPath, int(labels.GetStatus()));
        return -1;
    }

    struct mfb_window *window = example_open_window("Font Renderer: baked font with effects");
    if (window == nullptr) {
        return -1;
    }
    mfb_set_keyboard_callback(window, Keyboard);
    struct mfb_timer *timer = mfb_timer_create();
    // The clip rectangle would frame the whole window. C still shows it.
    g_screen.show_clipping = false;

    const bool bgra = font.GetTextureFormat() == MindShake::FontBase::ETextureFormat::BGRA32;
    char       status[192];
    if (hasEffects) {
        snprintf(status, sizeof(status), u8"Texture: %s, %s · T atlas · Esc: exit", texturePath, bgra ? "BGRA32" : "Alpha8");
    }
    else {
        snprintf(status, sizeof(status), u8"Texture: %s, without effects: tools/add_effects.py makes %s · T atlas · Esc: exit",
                 texturePath, kEffectsTexturePath);
    }

    mfb_update_state state;
    do {
        DrawBackground();
        font.SetClipping(g_screen.clip_left, g_screen.clip_top, g_screen.clip_right, g_screen.clip_bottom);
        labels.SetClipping(g_screen.clip_left, g_screen.clip_top, g_screen.clip_right, g_screen.clip_bottom);

        int32_t y = kMargin + 8;
        for (const Line &line : kLines) {
            labels.DrawText(line.caption, kLabelSize, kLabelColor, g_screen.buffer, g_screen.width, kMargin, y);
            font.DrawText(line.text, kSize, line.color, g_screen.buffer, g_screen.width, kMargin, y + 16);
            y += 86;
        }

        const double   seconds = mfb_timer_now(timer);
        const uint32_t alpha   = uint32_t(150.0 + 105.0 * std::sin(seconds * 4.0));
        labels.DrawText("its alpha fades the text with its effects", kLabelSize, kLabelColor, g_screen.buffer, g_screen.width, kMargin, y);
        font.DrawText(kBlinkingText, kSize, (alpha << 24) | kBlinkingColor, g_screen.buffer, g_screen.width, kMargin, y + 16);

        if (gShowTexture) {
            const int32_t width  = int32_t(font.GetTextureWidth());
            const int32_t height = int32_t(font.GetTextureHeight());
            const int32_t left   = std::max(int32_t(g_screen.width) - kMargin - width, int32_t(480));
            char          label[64];
            snprintf(label, sizeof(label), u8"Atlas: %d × %d", int(width), int(height));
            labels.DrawText(label, kLabelSize, kLabelColor, g_screen.buffer, g_screen.width, left, kMargin + 8);
            example_draw_texture(font.GetTexture(), bgra, width, height, left, kMargin + 28,
                                 int32_t(g_screen.width) - kMargin, int32_t(g_screen.height) - kStatusHeight - kMargin);
        }

        example_draw_clipping();

        // The status bar is never clipped.
        const int32_t top = int32_t(g_screen.height) - kStatusHeight;
        example_fill_rect(0, top, int32_t(g_screen.width), int32_t(g_screen.height), 0xff000000, 170);
        labels.SetClipping(0, 0, int32_t(g_screen.width), int32_t(g_screen.height));
        labels.DrawText(status, kLabelSize, kLabelColor, g_screen.buffer, g_screen.width, kMargin, top + 6);

        state = mfb_update_ex(window, g_screen.buffer, g_screen.width, g_screen.height);
        if (state != MFB_STATE_OK) {
            window = nullptr;
            break;
        }
    } while(mfb_wait_sync(window));

    example_release();
    mfb_timer_destroy(timer);

    return 0;
}
