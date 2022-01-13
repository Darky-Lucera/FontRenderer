#include <MiniFB.h>
#include <FontSFT.h>
#include <FontSTB.h>
#include <UTF8_Utils.h>
//-------------------------------------
#if defined(_WIN32)
    #include <direct.h>
    #define chdir   _chdir
#else
    #include <unistd.h>
#endif

enum class ClipBorder {
    Left,
    Right,
    Top,
    Bottom,
};

//-------------------------------------
static uint32_t     gWidth   = 512;
static uint32_t     gHeight  = 256;
static uint32_t     *gBuffer = nullptr;
static int32_t      gClipLeft   = 0;
static int32_t      gClipTop    = 0;
static int32_t      gClipRight  = gWidth;
static int32_t      gClipBottom = gHeight;

static bool         gShowBoundingBox   = true;
static bool         gShowClippingLines = true;
static bool         gShowTexture       = false;
static int          gShowTextureId     = 1;
static ClipBorder   gSelectedBorderH = ClipBorder::Left;
static ClipBorder   gSelectedBorderV = ClipBorder::Top;

//-------------------------------------
using Rect = MindShake::SkylineBinPack::Rect;

//-------------------------------------
static void
resize(struct mfb_window *window, int width, int height) {
    (void) window;
    gWidth  = width;
    gHeight = height;
    gBuffer = (uint32_t *) realloc(gBuffer, gWidth * gHeight * 4);
}

//-------------------------------------
static void 
RenderBoundingBox(uint32_t posX1, uint32_t posY1, Rect &rect) {
    int32_t left = posX1 + rect.x;
    int32_t top  = posY1 + rect.y;

    for (uint32_t x = left; x < posX1 + rect.width; ++x) {
        gBuffer[top * gWidth + x] = MFB_RGB(0, 255, 0);
        if ((top + rect.height) < int32_t(gHeight))
            gBuffer[(posY1 + rect.height) * gWidth + x] = MFB_RGB(0, 255, 0);
    }

    for (uint32_t y = top; y < posY1 + rect.height; ++y) {
        gBuffer[y * gWidth + left] = MFB_RGB(0, 255, 0);
        if (left + rect.width < int32_t(gWidth))
            gBuffer[y * gWidth + posX1 + rect.width - 1] = MFB_RGB(0, 255, 0);
    }
}

//-------------------------------------
static void 
RenderTexture(const MindShake::FontSTB &fontSTB, const MindShake::FontSFT &fontSFT) {
    uint32_t offset = 0;
    uint32_t offsetT = 0;
    uint32_t minX, minY;
    uint8_t *texture = nullptr;

    if (gShowTextureId == 1) {
        minX = std::min<uint32_t>(gWidth, fontSTB.GetTextureWidth());
        minY = std::min<uint32_t>(gHeight, fontSTB.GetTextureHeight());
        texture = fontSTB.GetTexture();
    }
    else {
        minX = std::min<uint32_t>(gWidth, fontSFT.GetTextureWidth());
        minY = std::min<uint32_t>(gHeight, fontSFT.GetTextureHeight());
        texture = fontSFT.GetTexture();
    }

    for (uint32_t y = 0; y < minY; ++y) {
        for (uint32_t x = 0; x < minX; ++x) {
            uint8_t pixel = texture[offsetT + x];
            gBuffer[offset + x] = MFB_RGB(pixel, pixel, pixel);
        }
        offset += gWidth;
        if (gShowTextureId == 1)
            offsetT += fontSTB.GetTextureWidth();
        else
            offsetT += fontSFT.GetTextureWidth();
    }
}

//-------------------------------------
static void 
RenderClippingLines() {
    for (uint32_t x = 0; x < gWidth; ++x) {
        gBuffer[gClipTop * gWidth + x] = MFB_RGB(255, 0, 0);
        if (gClipBottom < int32_t(gHeight))
            gBuffer[gClipBottom * gWidth + x] = MFB_RGB(255, 0, 0);
    }

    for (uint32_t y = 0; y < gHeight; ++y) {
        gBuffer[y * gWidth + gClipLeft] = MFB_RGB(255, 0, 0);
        if (gClipRight < int32_t(gWidth))
            gBuffer[y * gWidth + gClipRight] = MFB_RGB(255, 0, 0);
    }
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
        fps = time * 60.0f;
    }

    return fps;
}

//-------------------------------------
static void
SetAppDirectory(const char *argv) {
    std::string path(argv);
#if defined(_WIN32)
    path = path.substr(0, path.rfind('\\'));
#else
    path = path.substr(0, path.rfind('/'));
#endif
    chdir(path.c_str());
}

//-------------------------------------
int
main(int argc, char *argv[]) {
    SetAppDirectory(argv[0]);
    struct mfb_timer *timer = mfb_timer_create();

    // Visual Studio does not convert multiple-byte characters with the u8 string literal.
    // This trick fixes some of them but, sadly, not all.
    std::string text1 = u8"¡Hola\nPepe!\n¿Cómo\nestás?";//MindShake::UTF32_2_UTF8(U"¡Hola\nPepe!\n¿Cómo\nestás?");
    std::string text2 = "STB:\n" + text1;
    text1 = "SFT:\n" + text1;

    MindShake::FontSFT fontSFT("resources/Roboto-Regular.ttf");
    MindShake::FontSTB fontSTB("resources/Roboto-Regular.ttf");

    fontSFT.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
    fontSTB.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);

    fontSFT.SetAntialias(true);
    fontSTB.SetAntialias(true);

    fontSFT.SetAntialiasWeights(20, 4, 1);
    fontSTB.SetAntialiasWeights(20, 4, 1);

    struct mfb_window *window = mfb_open_ex("Font Renderer", gWidth, gHeight, WF_RESIZABLE);
    if (!window) {
        fprintf(stderr, "Cannot create window!\n");
        return -1;
    }

    gBuffer = (uint32_t *) calloc(gWidth * gHeight * 4, 1);
    
    // Events
    mfb_set_resize_callback(window, resize);
    mfb_set_keyboard_callback([&fontSFT, &fontSTB](struct mfb_window *window, mfb_key key, mfb_key_mod mod, bool isPressed) {
        switch(key) {
            case KB_KEY_LEFT:
                if(gSelectedBorderH == ClipBorder::Left) {
                    if(gClipLeft > 0)
                        --gClipLeft;
                }
                else {
                    if(gClipRight > 0 && gClipRight > gClipLeft)
                        --gClipRight;
                }
                fontSFT.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
                fontSTB.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
                break;

            case KB_KEY_RIGHT:
                if(gSelectedBorderH == ClipBorder::Left) {
                    if(gClipLeft < gWidth && gClipLeft < gClipRight)
                        ++gClipLeft;
                }
                else {
                    if(gClipRight < gWidth)
                        ++gClipRight;
                }
                fontSFT.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
                fontSTB.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
                break;

            case KB_KEY_DOWN:
                if(gSelectedBorderV == ClipBorder::Top) {
                    if(gClipTop < gHeight && gClipTop < gClipBottom)
                        ++gClipTop;
                }
                else {
                    if(gClipBottom < gHeight)
                        ++gClipBottom;
                }
                fontSFT.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
                fontSTB.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
                break;

            case KB_KEY_UP:
                if(gSelectedBorderV == ClipBorder::Top) {
                    if(gClipTop > 0)
                        --gClipTop;
                }
                else {
                    if(gClipBottom > 0 && gClipBottom > gClipTop)
                        --gClipBottom;
                }
                fontSFT.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
                fontSTB.SetClipping(gClipLeft, gClipTop, gClipRight, gClipBottom);
                break;
        }

        if(isPressed) {
            switch(key) {
                case KB_KEY_ESCAPE:
                    mfb_close(window);
                    break;

                case KB_KEY_B:
                    gShowBoundingBox = !gShowBoundingBox;
                    break;

                case KB_KEY_C:
                    gShowClippingLines = !gShowClippingLines;
                    break;

                case KB_KEY_T:
                    gShowTexture = !gShowTexture;
                    break;

                case KB_KEY_1:
                    gShowTextureId = 1;
                    break;

                case KB_KEY_2:
                    gShowTextureId = 2;
                    break;

                case KB_KEY_3:
                    //gShowTextureId = 3;
                    break;

                case KB_KEY_A:
                    fontSFT.SetAntialias(!fontSFT.GetAntialias());
                    fontSTB.SetAntialias(!fontSTB.GetAntialias());
                    fontSFT.Reset();
                    fontSTB.Reset();
                    break;

                case KB_KEY_E:
                    fontSFT.SetAntialiasAllowEx(!fontSFT.GetAntialiasAllowEx());
                    fontSTB.SetAntialiasAllowEx(!fontSTB.GetAntialiasAllowEx());
                    fontSFT.Reset();
                    fontSTB.Reset();
                    break;

                case KB_KEY_TAB:
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

    mfb_update_state state;
    do {
        uint32_t *screen = gBuffer;
        for(uint32_t y=0; y<gHeight; ++y) {
            uint8_t aux = 32 + (y * 128) / gHeight;
            memset(screen, aux, gWidth * 4);
            screen += gWidth;
        }

        if(gShowTexture) {
            RenderTexture(fontSTB, fontSFT);
        }

        if(gShowClippingLines) {
            RenderClippingLines();
        }

        uint32_t posX1 = 10, posY1 = 10;
        uint32_t posX2 = gWidth / 2, posY2 = 10;

        if(gShowBoundingBox) {
            Rect rectSFT{};
            Rect rectSTB{};

            fontSFT.GetTextBox(text1.c_str(), 32, &rectSFT);
            fontSTB.GetTextBox(text2.c_str(), 32, &rectSTB);

            RenderBoundingBox(posX1, posY1, rectSFT);
            RenderBoundingBox(posX2, posY2, rectSTB);
        }

        double s1 = mfb_timer_now(timer);
        fontSFT.DrawText(text1.c_str(), 32, 0xffff7f7f, gBuffer, gWidth, posX1, posY1);
        double e1 = mfb_timer_now(timer);

        double s2 = mfb_timer_now(timer);
        fontSTB.DrawText(text2.c_str(), 32, 0xff7f7fff, gBuffer, gWidth, posX2, posY2);
        double e2 = mfb_timer_now(timer);

        char buffer[256];
        //snprintf(buffer, sizeof(buffer), "Time: %f ms - %f ms\n", float(e1 - s1) * 1000.0f, float(e2 - s2) * 1000.0f);
        //snprintf(buffer, sizeof(buffer), "Time: %f ms - %f ms\n", 0.0762f, 0.0624f); // For capturing pictures
        //fontSFT.DrawText(buffer, 20, 0xffffffff, gBuffer, gWidth, 0, gHeight - 32);

        Rect rect{};
        snprintf(buffer, sizeof(buffer), "FPS: %.2f", GetFPS());
        fontSFT.GetTextBox(buffer, 12, &rect);
        fontSFT.DrawText(buffer, 12, 0xff0f3f0f, gBuffer, gWidth, gWidth - rect.width - 1, gHeight - rect.height - 1);

        state = mfb_update_ex(window, gBuffer, gWidth, gHeight);
        if (state != STATE_OK) {
            window = nullptr;
            break;
        }
    } while(mfb_wait_sync(window));

    free(gBuffer);
    gBuffer = nullptr;

    return 0;
}
