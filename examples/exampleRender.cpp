#include "exampleCommon.h"
#if defined(FONTRENDERER_USE_FREETYPE)
    #include <FontFT.h>
#endif
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
    #include <FontSFT.h>
#endif
#if defined(FONTRENDERER_USE_STB)
    #include <FontSTB.h>
#endif
//-------------------------------------
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

//-------------------------------------
struct Backend {
    const char                          *name;
    uint32_t                            color;
    std::unique_ptr<MindShake::Font>    font;
    double                              rasterMilliseconds {};  // Time to render the glyphs of the column
    bool                                rasterMeasured {};
};

//-------------------------------------
static bool                 gShowBoundingBox = true;
static bool                 gShowTexture     = true;
static size_t               gShowTextureId   = 0;
static bool                 gMeanWeights     = false;
static std::vector<Backend> gBackends;
static double               gDrawSeconds = 0.0;                 // Time DrawText took, summed over the timed frames
#if defined(FONTRENDERER_USE_FREETYPE)
static MindShake::FontFT    *gFreeType = nullptr;
#endif

//-------------------------------------
static const int32_t    kMargin       = 16;
static const int32_t    kStatusHeight = 44;
#if defined(FONTRENDERER_USE_FREETYPE)
static const int32_t    kStatusLineHeight = 18;
#endif

static const uint32_t   kTextColor    = 0xfff4ece1;
static const uint32_t   kStatusColor  = 0xffdcdcdc;

static const char       *kGreeting      = u8"Hello! - ¡Hola! - Привет! - Olá!";
static const char       *kMoreGreetings = u8"Grüß dich! - Γειά σου! - Ça va ? - Cześć!\n"
                                          u8"Xin chào! - Günaydın! - Hallå! - Dobrý den!\n"
                                          u8"Jó napot! - Hyvää päivää! - Bună ziua!\n"
                                          u8"Góðan dag! - Dobrý deň! - Добрий день!";
static const uint8_t    kSizes[]        = { 12, 14, 16, 20, 24 };
static const char       *kFontPath      = "resources/Roboto-Regular.ttf";
#if defined(FONTRENDERER_USE_FREETYPE)
static const char       *kHintings[]    = { "none", "light", "normal", "auto" };
#endif

//-------------------------------------
using Rect = MindShake::SkylineBinPack::Rect;

// Returns the box of the text, relative to the window, and adds the time DrawText took to gDrawSeconds.
//-------------------------------------
static Rect
RenderText(MindShake::Font &font, const char *text, uint8_t height, uint32_t color, int32_t x, int32_t y, struct mfb_timer *timer) {
    Rect box {};
    font.GetTextBox(text, height, &box);
    if (gShowBoundingBox) {
        example_draw_box(x + box.x, y + box.y, box.width, box.height);
    }

    const double start = mfb_timer_now(timer);
    font.DrawText(text, height, color, g_screen.buffer, g_screen.width, x, y);
    gDrawSeconds += mfb_timer_now(timer) - start;

    box.x += x;
    box.y += y;
    return box;
}

// Rendering the glyphs is the only work that depends on the backend. DrawText copies them from the atlas,
// with the same code for every backend.
//-------------------------------------
static void
MeasureRaster(Backend &backend, struct mfb_timer *timer) {
    MindShake::Font &font = *backend.font;
    Rect            box {};

    font.Reset();
    const double start = mfb_timer_now(timer);
    for (uint8_t size : kSizes) {
        font.GetTextBox(kGreeting, size, &box);
    }
    font.GetTextBox(kMoreGreetings, 20, &box);
    backend.rasterMilliseconds = (mfb_timer_now(timer) - start) * 1e3;
    backend.rasterMeasured     = true;
}

//-------------------------------------
static void
MeasureRasterAgain() {
    for (Backend &backend : gBackends) {
        backend.rasterMeasured = false;
    }
}

// Returns the bottom of the column.
//-------------------------------------
static int32_t
RenderColumn(Backend &backend, int32_t left, int32_t width, struct mfb_timer *timer) {
    MindShake::Font &font = *backend.font;
    int32_t         y     = kMargin;

    if (backend.rasterMeasured == false) {
        MeasureRaster(backend, timer);
    }

    // Besides the clip rectangle, each column only draws inside itself.
    font.SetClipping(std::max(g_screen.clip_left, left), g_screen.clip_top, std::min(g_screen.clip_right, left + width - kMargin), g_screen.clip_bottom);

    char title[64];
    snprintf(title, sizeof(title), u8"%s · raster %.2f ms", backend.name, backend.rasterMilliseconds);
    font.DrawText(title, 18, backend.color, g_screen.buffer, g_screen.width, left, y);
    y += 30;

    for (uint8_t size : kSizes) {
        RenderText(font, kGreeting, size, kTextColor, left, y, timer);
        y += size + 6;
    }
    y += 10;

    return RenderText(font, kMoreGreetings, 20, kTextColor, left, y, timer).bottom();
}

//-------------------------------------
static int32_t
GetStatusHeight() {
#if defined(FONTRENDERER_USE_FREETYPE)
    // One more line for the settings of FreeType.
    return kStatusHeight + kStatusLineHeight;
#else
    return kStatusHeight;
#endif
}

//-------------------------------------
static void
RenderAtlas(MindShake::Font &font, const char *name, int32_t top) {
    const int32_t left   = kMargin;
    const int32_t right  = std::min(int32_t(g_screen.width) - kMargin, int32_t(g_screen.width));
    const int32_t bottom = std::min(int32_t(g_screen.height) - GetStatusHeight() - kMargin, int32_t(g_screen.height));

    char label[128];
    snprintf(label, sizeof(label), u8"Atlas of %s: %u × %u, %u × %u used", name,
             unsigned(font.GetTextureWidth()), unsigned(font.GetTextureHeight()),
             unsigned(font.GetUsedTextureWidth()), unsigned(font.GetUsedTextureHeight()));
    font.DrawText(label, 14, kStatusColor, g_screen.buffer, g_screen.width, left, top);
    top += 22;

    // The checkerboard shows which texels are empty. A texture bigger than the panel is cropped.
    const uint8_t *texels       = font.GetTexture();
    const int32_t textureWidth  = int32_t(font.GetTextureWidth());
    const int32_t shownRight    = std::min(right, left + textureWidth);
    const int32_t shownBottom   = std::min(bottom, top + int32_t(font.GetTextureHeight()));
    for (int32_t y = std::max(top, int32_t(0)); y < shownBottom; ++y) {
        uint32_t *row = &g_screen.buffer[size_t(y) * g_screen.width];
        for (int32_t x = left; x < shownRight; ++x) {
            const int32_t texelX = x - left;
            const int32_t texelY = y - top;
            const uint32_t square = ((texelX / 8 + texelY / 8) % 2 != 0) ? 0xff383838 : 0xff4a4a4a;
            row[x] = example_blend(square, 0xffffffff, texels[size_t(texelY) * size_t(textureWidth) + size_t(texelX)]);
        }
    }

    example_draw_box(left - 1, top - 1, shownRight - left + 2, shownBottom - top + 2);
}

//-------------------------------------
static void
RenderStatusBar(MindShake::Font &font, double drawMicroseconds, float fps) {
    const int32_t top = int32_t(g_screen.height) - GetStatusHeight();
    example_fill_rect(0, top, int32_t(g_screen.width), int32_t(g_screen.height), 0xff000000, 170);

    // The status bar is never clipped.
    font.SetClipping(0, 0, int32_t(g_screen.width), int32_t(g_screen.height));

    auto onOff = [](bool value) { return value ? "ON" : "OFF"; };

    char line[256];
    snprintf(line, sizeof(line), u8"A antialias: %s · E extend: %s · W weights: %s · B boxes: %s · C clip view: %s · T atlas: %s",
             onOff(font.GetAntialias()), onOff(font.GetAntialiasAllowEx()), gMeanWeights ? "mean" : "gaussian",
             onOff(gShowBoundingBox), onOff(g_screen.show_clipping), onOff(gShowTexture));
    font.DrawText(line, 13, kStatusColor, g_screen.buffer, g_screen.width, kMargin, top + 6);

    // One number key for each backend, as in "1, 2, 3".
    const int keysLength = int(gBackends.size()) * 3 - 2;
    snprintf(line, sizeof(line), u8"Tab: clip corner · Arrows: move clip · %.*s: atlas font · Esc: exit", keysLength, "1, 2, 3");
    font.DrawText(line, 13, kStatusColor, g_screen.buffer, g_screen.width, kMargin, top + 24);

    Rect box {};
    snprintf(line, sizeof(line), u8"DrawText %.0f µs · %.0f FPS", drawMicroseconds, fps);
    font.GetTextBox(line, 13, &box);
    font.DrawText(line, 13, kStatusColor, g_screen.buffer, g_screen.width, int32_t(g_screen.width) - kMargin - box.right(), top + 24);

#if defined(FONTRENDERER_USE_FREETYPE)
    snprintf(line, sizeof(line), u8"FreeType · H hinting: %s · M monochrome: %s · D stem darkening: %s",
             kHintings[int(gFreeType->GetHinting())], onOff(gFreeType->GetMonochrome()), onOff(gFreeType->GetStemDarkening()));
    font.DrawText(line, 13, kStatusColor, g_screen.buffer, g_screen.width, kMargin, top + 24 + kStatusLineHeight);
#endif
}

//-------------------------------------
static void
Keyboard(struct mfb_window *window, mfb_key key, mfb_key_mod mod, bool isPressed) {
    example_handle_key(window, key, isPressed);
    if (isPressed == false) {
        return;
    }

    switch(key) {
        case MFB_KB_KEY_B:
            gShowBoundingBox = !gShowBoundingBox;
            break;

        case MFB_KB_KEY_T:
            gShowTexture = !gShowTexture;
            break;

        case MFB_KB_KEY_1:
        case MFB_KB_KEY_2:
        case MFB_KB_KEY_3:
            if(size_t(key - MFB_KB_KEY_1) < gBackends.size()) {
                gShowTextureId = size_t(key - MFB_KB_KEY_1);
            }
            break;

        case MFB_KB_KEY_A:
            for(Backend &backend : gBackends) {
                backend.font->SetAntialias(!backend.font->GetAntialias());
            }
            MeasureRasterAgain();
            break;

        case MFB_KB_KEY_E:
            for(Backend &backend : gBackends) {
                backend.font->SetAntialiasAllowEx(!backend.font->GetAntialiasAllowEx());
            }
            MeasureRasterAgain();
            break;

        case MFB_KB_KEY_W:
            gMeanWeights = !gMeanWeights;
            for(Backend &backend : gBackends) {
                if(gMeanWeights) {
                    backend.font->SetAntialiasWeights(1, 1, 1);
                }
                else {
                    backend.font->SetAntialiasWeights(20, 4, 1);
                }
            }
            MeasureRasterAgain();
            break;

#if defined(FONTRENDERER_USE_FREETYPE)
        case MFB_KB_KEY_H:
            gFreeType->SetHinting(MindShake::FontFT::EHinting((int(gFreeType->GetHinting()) + 1) % 4));
            MeasureRasterAgain();
            break;

        case MFB_KB_KEY_M:
            gFreeType->SetMonochrome(!gFreeType->GetMonochrome());
            MeasureRasterAgain();
            break;

        case MFB_KB_KEY_D:
            gFreeType->SetStemDarkening(!gFreeType->GetStemDarkening());
            MeasureRasterAgain();
            break;
#endif

        default:
            break;
    }
}

//-------------------------------------
template <class TFont>
static bool
AddBackend(const char *name, uint32_t color) {
    std::unique_ptr<MindShake::Font> font = std::make_unique<TFont>(kFontPath);
    if (font->GetStatus() != MindShake::Font::EStatus::Ok) {
        fprintf(stderr, "Cannot load %s with %s (status %d).\n", kFontPath, name, int(font->GetStatus()));
        return false;
    }

    font->SetAntialias(true);
    font->SetAntialiasWeights(20, 4, 1);
    gBackends.push_back({ name, color, std::move(font) });
    return true;
}

//-------------------------------------
int
main(int argc, char *argv[]) {
    example_set_app_directory(argv[0]);
    struct mfb_timer *timer = mfb_timer_create();

    bool loaded = true;
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
    loaded = loaded && AddBackend<MindShake::FontSFT>("libschrift", 0xffff9e80);
#endif
#if defined(FONTRENDERER_USE_STB)
    loaded = loaded && AddBackend<MindShake::FontSTB>("stb_truetype", 0xff80cbff);
#endif
#if defined(FONTRENDERER_USE_FREETYPE)
    loaded = loaded && AddBackend<MindShake::FontFT>("FreeType", 0xffa5d6a7);
    if (loaded) {
        // Light hinting makes small text sharper without changing its spacing.
        gFreeType = static_cast<MindShake::FontFT *>(gBackends.back().font.get());
        gFreeType->SetHinting(MindShake::FontFT::EHinting::Light);
    }
#endif
    if (loaded == false) {
        return -1;
    }

    struct mfb_window *window = example_open_window("Font Renderer");
    if (window == nullptr) {
        return -1;
    }
    mfb_set_keyboard_callback(window, Keyboard);

    // A single frame varies too much to be read, so the time of DrawText is an average.
    const int kTimedFrames     = 30;
    int       timedFrames      = 0;
    double    drawMicroseconds = 0.0;

    mfb_update_state state;
    do {
        example_clear();

        // The keys and the resize callback only change the globals, so each column applies the clipping every frame.
        const int32_t columnWidth = (int32_t(g_screen.width) - kMargin) / int32_t(gBackends.size());
        int32_t       bottom      = 0;
        for (size_t i = 0; i < gBackends.size(); ++i) {
            bottom = std::max(bottom, RenderColumn(gBackends[i], kMargin + int32_t(i) * columnWidth, columnWidth, timer));
        }
        if (++timedFrames == kTimedFrames) {
            drawMicroseconds = gDrawSeconds / kTimedFrames * 1e6;
            gDrawSeconds     = 0.0;
            timedFrames      = 0;
        }

        if (gShowTexture) {
            Backend &backend = gBackends[gShowTextureId];
            backend.font->SetClipping(g_screen.clip_left, g_screen.clip_top, g_screen.clip_right, g_screen.clip_bottom);
            RenderAtlas(*backend.font, backend.name, bottom + kMargin);
        }
        example_draw_clipping();

        RenderStatusBar(*gBackends[0].font, drawMicroseconds, example_get_fps());

        state = mfb_update_ex(window, g_screen.buffer, g_screen.width, g_screen.height);
        if (state != MFB_STATE_OK) {
            window = nullptr;
            break;
        }
    } while(mfb_wait_sync(window));

    example_release();
    gBackends.clear();
    mfb_timer_destroy(timer);

    return 0;
}
