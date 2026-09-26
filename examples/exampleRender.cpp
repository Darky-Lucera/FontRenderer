#include <MiniFB.h>
#include <FontSFT.h>
#include <FontSTB.h>
#include <UTF8_Utils.h>
//-------------------------------------
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(_WIN32)
    #include <direct.h>
    #define chdir   _chdir
#else
    #include <unistd.h>
#endif

//-------------------------------------
enum class ClipBorder {
    Left,
    Right,
    Top,
    Bottom,
};

//-------------------------------------
// It is also a VESA mode, which the DOS backend of MiniFB needs.
static uint32_t     gWidth   = 800;
static uint32_t     gHeight  = 600;
static uint32_t     *gBuffer = nullptr;
static int32_t      gClipLeft   = 0;
static int32_t      gClipTop    = 0;
static int32_t      gClipRight  = gWidth;
static int32_t      gClipBottom = gHeight;

static bool         gShowBoundingBox = true;
static bool         gShowClipping    = true;
static bool         gShowTexture     = true;
static int          gShowTextureId   = 1;
static bool         gMeanWeights     = false;
static ClipBorder   gSelectedBorderH = ClipBorder::Left;
static ClipBorder   gSelectedBorderV = ClipBorder::Top;

//-------------------------------------
static const int32_t    kMargin       = 16;
static const int32_t    kStatusHeight = 44;

static const uint32_t   kTextColor    = 0xfff4ece1;
static const uint32_t   kSFTColor     = 0xffff9e80;
static const uint32_t   kSTBColor     = 0xff80cbff;
static const uint32_t   kStatusColor  = 0xffdcdcdc;
static const uint32_t   kHighlight    = 0xffffd54f;

static const char       *kGreeting    = u8"¡Hola Pepe! ¿Cómo estás?";
static const char       *kPangram     = u8"El veloz murciélago hindú\ncomía feliz cardillo y kiwi.\nLa cigüeña tocaba el saxofón\ndetrás del palenque de paja.";
static const uint8_t    kSizes[]      = { 12, 16, 20, 24, 32 };

//-------------------------------------
using Rect = MindShake::SkylineBinPack::Rect;

//-------------------------------------
static void
resize(struct mfb_window *window, int width, int height) {
    (void) window;
    // realloc may free the buffer when asked for 0 bytes.
    if (width <= 0 || height <= 0) {
        return;
    }

    // If it fails, the old buffer is still valid and keeps being shown.
    uint32_t *buffer = (uint32_t *) realloc(gBuffer, size_t(width) * size_t(height) * 4);
    if (buffer == nullptr) {
        return;
    }

    // A clip border on the window edge follows the edge. Any other border only has to stay inside the new buffer.
    if (gClipRight == int32_t(gWidth) || gClipRight > width) {
        gClipRight = width;
    }

    if (gClipBottom == int32_t(gHeight) || gClipBottom > height) {
        gClipBottom = height;
    }

    gClipLeft = std::min(gClipLeft, gClipRight);
    gClipTop  = std::min(gClipTop, gClipBottom);

    gBuffer = buffer;
    gWidth  = width;
    gHeight = height;
}

//-------------------------------------
static uint32_t
Blend(uint32_t dst, uint32_t src, uint32_t alpha) {
    const uint32_t inverse = 255 - alpha;
    const uint32_t r = (((src >> 16) & 0xff) * alpha + ((dst >> 16) & 0xff) * inverse) / 255;
    const uint32_t g = (((src >>  8) & 0xff) * alpha + ((dst >>  8) & 0xff) * inverse) / 255;
    const uint32_t b = (( src        & 0xff) * alpha + ( dst        & 0xff) * inverse) / 255;

    return 0xff000000 | (r << 16) | (g << 8) | b;
}

//-------------------------------------
static void
FillRect(int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t color, uint32_t alpha) {
    left   = std::max(left, int32_t(0));
    top    = std::max(top, int32_t(0));
    right  = std::min(right, int32_t(gWidth));
    bottom = std::min(bottom, int32_t(gHeight));

    for (int32_t y = top; y < bottom; ++y) {
        uint32_t *row = &gBuffer[size_t(y) * gWidth];
        for (int32_t x = left; x < right; ++x) {
            row[x] = Blend(row[x], color, alpha);
        }
    }
}

//-------------------------------------
static void
RenderBoundingBox(int32_t posX, int32_t posY, const Rect &rect) {
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }

    const int32_t left   = posX + rect.left();
    const int32_t top    = posY + rect.top();
    const int32_t right  = posX + rect.right();
    const int32_t bottom = posY + rect.bottom();

    FillRect(left,      top,        right,    top + 1,    0xffffffff, 64);
    FillRect(left,      bottom - 1, right,    bottom,     0xffffffff, 64);
    FillRect(left,      top + 1,    left + 1, bottom - 1, 0xffffffff, 64);
    FillRect(right - 1, top + 1,    right,    bottom - 1, 0xffffffff, 64);
}

// Returns the box of the text, relative to the window, and adds the time DrawText took to seconds.
//-------------------------------------
static Rect
RenderText(MindShake::Font &font, const char *text, uint8_t height, uint32_t color, int32_t x, int32_t y, struct mfb_timer *timer, double &seconds) {
    Rect box {};
    font.GetTextBox(text, height, &box);
    if (gShowBoundingBox) {
        RenderBoundingBox(x, y, box);
    }

    const double start = mfb_timer_now(timer);
    font.DrawText(text, height, color, gBuffer, gWidth, x, y);
    seconds += mfb_timer_now(timer) - start;

    box.x += x;
    box.y += y;
    return box;
}

// Returns the time the column took to draw, and its bottom in bottom.
//-------------------------------------
static double
RenderColumn(MindShake::Font &font, const char *name, uint32_t nameColor, int32_t left, struct mfb_timer *timer, int32_t &bottom) {
    double  seconds = 0.0;
    int32_t y       = kMargin;

    font.DrawText(name, 18, nameColor, gBuffer, gWidth, left, y);
    y += 30;

    for (uint8_t size : kSizes) {
        RenderText(font, kGreeting, size, kTextColor, left, y, timer, seconds);
        y += size + 6;
    }
    y += 10;

    bottom = RenderText(font, kPangram, 20, kTextColor, left, y, timer, seconds).bottom();

    return seconds;
}

//-------------------------------------
static void
RenderAtlas(MindShake::Font &font, const char *name, int32_t top) {
    const int32_t left   = kMargin;
    const int32_t right  = std::min(int32_t(gWidth) - kMargin, int32_t(gWidth));
    const int32_t bottom = std::min(int32_t(gHeight) - kStatusHeight - kMargin, int32_t(gHeight));

    char label[128];
    snprintf(label, sizeof(label), u8"Atlas of %s: %u × %u, %u × %u used", name,
             unsigned(font.GetTextureWidth()), unsigned(font.GetTextureHeight()),
             unsigned(font.GetUsedTextureWidth()), unsigned(font.GetUsedTextureHeight()));
    font.DrawText(label, 14, kStatusColor, gBuffer, gWidth, left, top);
    top += 22;

    // The checkerboard shows which texels are empty. A texture bigger than the panel is cropped.
    const uint8_t *texels       = font.GetTexture();
    const int32_t textureWidth  = int32_t(font.GetTextureWidth());
    const int32_t shownRight    = std::min(right, left + textureWidth);
    const int32_t shownBottom   = std::min(bottom, top + int32_t(font.GetTextureHeight()));
    for (int32_t y = std::max(top, int32_t(0)); y < shownBottom; ++y) {
        uint32_t *row = &gBuffer[size_t(y) * gWidth];
        for (int32_t x = left; x < shownRight; ++x) {
            const int32_t texelX = x - left;
            const int32_t texelY = y - top;
            const uint32_t square = ((texelX / 8 + texelY / 8) % 2 != 0) ? 0xff383838 : 0xff4a4a4a;
            row[x] = Blend(square, 0xffffffff, texels[size_t(texelY) * size_t(textureWidth) + size_t(texelX)]);
        }
    }

    RenderBoundingBox(left - 1, top - 1, Rect { 0, 0, shownRight - left + 2, shownBottom - top + 2 });
}

//-------------------------------------
static void
RenderClipping() {
    // Everything outside the clip rectangle is darkened.
    const int32_t width  = int32_t(gWidth);
    const int32_t height = int32_t(gHeight);
    FillRect(0,          0,           width,      gClipTop,    0xff000000, 150);
    FillRect(0,          gClipBottom, width,      height,      0xff000000, 150);
    FillRect(0,          gClipTop,    gClipLeft,  gClipBottom, 0xff000000, 150);
    FillRect(gClipRight, gClipTop,    width,      gClipBottom, 0xff000000, 150);

    // The borders the arrow keys move.
    const int32_t x = (gSelectedBorderH == ClipBorder::Left) ? gClipLeft : gClipRight - 2;
    const int32_t y = (gSelectedBorderV == ClipBorder::Top)  ? gClipTop  : gClipBottom - 2;
    FillRect(x,         gClipTop, x + 2,      gClipBottom, kHighlight, 255);
    FillRect(gClipLeft, y,        gClipRight, y + 2,       kHighlight, 255);
}

//-------------------------------------
static void
RenderStatusBar(MindShake::Font &font, double sftMicroseconds, double stbMicroseconds, float fps) {
    const int32_t top = int32_t(gHeight) - kStatusHeight;
    FillRect(0, top, int32_t(gWidth), int32_t(gHeight), 0xff000000, 170);

    // The status bar is never clipped.
    font.SetClipping(0, 0, int32_t(gWidth), int32_t(gHeight));

    auto onOff = [](bool value) { return value ? "ON" : "OFF"; };

    char line[256];
    snprintf(line, sizeof(line), u8"A antialias: %s · E extend: %s · W weights: %s · B boxes: %s · C clip view: %s · T atlas: %s",
             onOff(font.GetAntialias()), onOff(font.GetAntialiasAllowEx()), gMeanWeights ? "mean" : "gaussian",
             onOff(gShowBoundingBox), onOff(gShowClipping), onOff(gShowTexture));
    font.DrawText(line, 13, kStatusColor, gBuffer, gWidth, kMargin, top + 6);

    font.DrawText(u8"Tab: clip corner · Arrows: move clip · 1, 2: atlas font · Esc: exit", 13, kStatusColor, gBuffer, gWidth, kMargin, top + 24);

    Rect box {};
    snprintf(line, sizeof(line), u8"libschrift %.0f µs · stb_truetype %.0f µs · %.0f FPS", sftMicroseconds, stbMicroseconds, fps);
    font.GetTextBox(line, 13, &box);
    font.DrawText(line, 13, kStatusColor, gBuffer, gWidth, int32_t(gWidth) - kMargin - box.right(), top + 24);
}

//-------------------------------------
static float
GetFPS() {
    static struct mfb_timer *timer = mfb_timer_create();
    static uint32_t frameCount = 0;
    static float fps = 60.0f;

    ++frameCount;
    if(frameCount >= 60) {
        frameCount = 0;
        double time = mfb_timer_now(timer);
        mfb_timer_reset(timer);
        fps = float(60.0 / time);
    }

    return fps;
}

//-------------------------------------
static void
SetAppDirectory(const char *argv) {
    const std::string path(argv);

#if defined(_WIN32)
    // Windows accepts both separators, and cmd keeps a '/' in argv[0].
    const size_t separator = path.find_last_of("\\/");
#else
    const size_t separator = path.rfind('/');
#endif

    // Keep the separator: "C:" alone is the current directory of drive C, and "/app" would give "".
    if (separator != std::string::npos) {
        chdir(path.substr(0, separator + 1).c_str());
    }
}

//-------------------------------------
int
main(int argc, char *argv[]) {
    SetAppDirectory(argv[0]);
    struct mfb_timer *timer = mfb_timer_create();

    MindShake::FontSFT fontSFT("resources/Roboto-Regular.ttf");
    MindShake::FontSTB fontSTB("resources/Roboto-Regular.ttf");

    fontSFT.SetAntialias(true);
    fontSTB.SetAntialias(true);

    fontSFT.SetAntialiasWeights(20, 4, 1);
    fontSTB.SetAntialiasWeights(20, 4, 1);

    struct mfb_window *window = mfb_open_ex("Font Renderer", gWidth, gHeight, MFB_WF_RESIZABLE);
    if (!window) {
        fprintf(stderr, "Cannot create window!\n");
        return -1;
    }

    gBuffer = (uint32_t *) calloc(gWidth * gHeight * 4, 1);

    // Events
    mfb_set_resize_callback(window, resize);
    mfb_set_keyboard_callback(
        [&fontSFT, &fontSTB](struct mfb_window *window, mfb_key key, mfb_key_mod mod, bool isPressed) {
            switch(key) {
                case MFB_KB_KEY_LEFT:
                    if(gSelectedBorderH == ClipBorder::Left) {
                        if(gClipLeft > 0) {
                            --gClipLeft;
                        }
                    }
                    else {
                        if(gClipRight > 0 && gClipRight > gClipLeft) {
                            --gClipRight;
                        }
                    }
                    break;

                case MFB_KB_KEY_RIGHT:
                    if(gSelectedBorderH == ClipBorder::Left) {
                        if(gClipLeft < int32_t(gWidth) && gClipLeft < gClipRight) {
                            ++gClipLeft;
                        }
                    }
                    else {
                        if(gClipRight < int32_t(gWidth)) {
                            ++gClipRight;
                        }
                    }
                    break;

                case MFB_KB_KEY_DOWN:
                    if(gSelectedBorderV == ClipBorder::Top) {
                        if(gClipTop < int32_t(gHeight) && gClipTop < gClipBottom) {
                            ++gClipTop;
                        }
                    }
                    else {
                        if(gClipBottom < int32_t(gHeight)) {
                            ++gClipBottom;
                        }
                    }
                    break;

                case MFB_KB_KEY_UP:
                    if(gSelectedBorderV == ClipBorder::Top) {
                        if(gClipTop > 0) {
                            --gClipTop;
                        }
                    }
                    else {
                        if(gClipBottom > 0 && gClipBottom > gClipTop) {
                            --gClipBottom;
                        }
                    }
                    break;
            }

            if(isPressed) {
                switch(key) {
                    case MFB_KB_KEY_ESCAPE:
                        mfb_close(window);
                        break;

                    case MFB_KB_KEY_B:
                        gShowBoundingBox = !gShowBoundingBox;
                        break;

                    case MFB_KB_KEY_C:
                        gShowClipping = !gShowClipping;
                        break;

                    case MFB_KB_KEY_T:
                        gShowTexture = !gShowTexture;
                        break;

                    case MFB_KB_KEY_1:
                        gShowTextureId = 1;
                        break;

                    case MFB_KB_KEY_2:
                        gShowTextureId = 2;
                        break;

                    case MFB_KB_KEY_A:
                        fontSFT.SetAntialias(!fontSFT.GetAntialias());
                        fontSTB.SetAntialias(!fontSTB.GetAntialias());
                        break;

                    case MFB_KB_KEY_E:
                        fontSFT.SetAntialiasAllowEx(!fontSFT.GetAntialiasAllowEx());
                        fontSTB.SetAntialiasAllowEx(!fontSTB.GetAntialiasAllowEx());
                        break;

                    case MFB_KB_KEY_W:
                        gMeanWeights = !gMeanWeights;
                        if(gMeanWeights) {
                            fontSFT.SetAntialiasWeights(1, 1, 1);
                            fontSTB.SetAntialiasWeights(1, 1, 1);
                        }
                        else {
                            fontSFT.SetAntialiasWeights(20, 4, 1);
                            fontSTB.SetAntialiasWeights(20, 4, 1);
                        }
                        break;

                    case MFB_KB_KEY_TAB:
                        if(gSelectedBorderH == ClipBorder::Left) {
                            gSelectedBorderH = ClipBorder::Right;
                            gSelectedBorderV = ClipBorder::Bottom;
                        }
                        else {
                            gSelectedBorderH = ClipBorder::Left;
                            gSelectedBorderV = ClipBorder::Top;
                        }
                        break;
                }
            }
        }, window);

    // A single frame varies too much to be read, so the times shown are averages.
    const int kTimedFrames = 30;
    double    sumSFT       = 0.0;
    double    sumSTB       = 0.0;
    double    shownSFT     = 0.0;
    double    shownSTB     = 0.0;
    int       timedFrames  = 0;

    mfb_update_state state;
    do {
        uint32_t *screen = gBuffer;
        for(uint32_t y=0; y<gHeight; ++y) {
            uint8_t aux = 32 + (y * 128) / gHeight;
            memset(screen, aux, gWidth * 4);
            screen += gWidth;
        }

        // The keys and the resize callback only change the globals, so the clipping is applied here every frame.
        fontSFT.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
        fontSTB.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);

        int32_t bottomSFT, bottomSTB;
        sumSFT += RenderColumn(fontSFT, "libschrift",   kSFTColor, kMargin,                    timer, bottomSFT);
        sumSTB += RenderColumn(fontSTB, "stb_truetype", kSTBColor, int32_t(gWidth / 2) + kMargin, timer, bottomSTB);
        if (++timedFrames == kTimedFrames) {
            shownSFT    = sumSFT / kTimedFrames * 1e6;
            shownSTB    = sumSTB / kTimedFrames * 1e6;
            sumSFT      = 0.0;
            sumSTB      = 0.0;
            timedFrames = 0;
        }

        if (gShowTexture) {
            MindShake::Font &font = (gShowTextureId == 1) ? static_cast<MindShake::Font &>(fontSFT) : fontSTB;
            RenderAtlas(font, (gShowTextureId == 1) ? "libschrift" : "stb_truetype", std::max(bottomSFT, bottomSTB) + kMargin);
        }

        if (gShowClipping) {
            RenderClipping();
        }

        RenderStatusBar(fontSTB, shownSFT, shownSTB, GetFPS());

        state = mfb_update_ex(window, gBuffer, gWidth, gHeight);
        if (state != MFB_STATE_OK) {
            window = nullptr;
            break;
        }
    } while(mfb_wait_sync(window));

    free(gBuffer);
    gBuffer = nullptr;

    return 0;
}
