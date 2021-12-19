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

//#define kShowTexture
#define kShowTextureSTB
#define kShowTextureSFT

#define kShhowClippingLines

#define kShowBoundingBox

//-------------------------------------
static uint32_t g_width   = 512;
static uint32_t g_height  = 256;
static uint32_t *g_buffer = nullptr;
static int32_t  g_left   = 0;
static int32_t  g_top    = 0;
static int32_t  g_right  = g_width;
static int32_t  g_bottom = g_height;

//-------------------------------------
using Rect = MindShake::SkylineBinPack::Rect;

//-------------------------------------
void
resize(struct mfb_window *window, int width, int height) {
    (void) window;
    g_width  = width;
    g_height = height;
    g_buffer = (uint32_t *) realloc(g_buffer, g_width * g_height * 4);
}

//-------------------------------------
void
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
    std::string text1 = MindShake::UTF32_2_UTF8(U"P ¡Hola\nPepe!\n¿Cómo\nestás?");
    std::string text2 = MindShake::UTF32_2_UTF8(U"STB:\n") + text1;
    text1 = MindShake::UTF32_2_UTF8(U"SFT:\n") + text1;

    MindShake::FontSFT fontSFT("resources/Roboto-Regular.ttf");
    MindShake::FontSTB fontSTB("resources/Roboto-Regular.ttf");

    fontSFT.SetClipping(g_left, g_top, g_right, g_bottom);
    fontSTB.SetClipping(g_left, g_top, g_right, g_bottom);

    fontSFT.SetAntialias(true);
    fontSTB.SetAntialias(true);

    fontSFT.SetAntialiasWeight(16);
    fontSTB.SetAntialiasWeight(16);

    struct mfb_window *window = mfb_open_ex("Font Renderer", g_width, g_height, WF_RESIZABLE);
    if (!window) {
        fprintf(stderr, "Cannot create window!\n");
        return -1;
    }

    g_buffer = (uint32_t *) calloc(g_width * g_height * 4, 1);
    mfb_set_resize_callback(window, resize);

    mfb_update_state state;
    do {
        memset(g_buffer, 0, g_width*g_height*4);

#if defined(kShowTexture)
        uint32_t offset  = 0;
        uint32_t offsetT = 0;
    #if defined(kShowTextureSTB)
        uint32_t minX = std::min<uint32_t>(g_width, fontSTB.GetTextureWidth());
        uint32_t minY = std::min<uint32_t>(g_height, fontSTB.GetTextureHeight());
        uint8_t *texture = fontSTB.GetTexture();
    #else
        uint32_t minX = std::min<uint32_t>(g_width, fontSFT.GetTextureWidth());
        uint32_t minY = std::min<uint32_t>(g_height, fontSFT.GetTextureHeight());
        uint8_t *texture = fontSFT.GetTexture();
    #endif
        for(uint32_t y=0; y<minY; ++y) {
            for(uint32_t x=0; x<minX; ++x) {
                uint8_t pixel =texture[offsetT + x];
                g_buffer[offset + x] = MFB_RGB(pixel, pixel, pixel);
            }
            offset += g_width;
    #if defined(kShowTextureSTB)
            offsetT += fontSTB.GetTextureWidth();
    #else
            offsetT += fontSFT.GetTextureWidth();
    #endif
        }
#endif

#if defined(kShhowClippingLines)
        for(uint32_t x=0; x<g_width; ++x) {
            g_buffer[g_top    * g_width + x] = MFB_RGB(255, 0, 0);
            if(g_bottom < int32_t(g_height))
                g_buffer[g_bottom * g_width + x] = MFB_RGB(255, 0, 0);
        }

        for(uint32_t y=0; y<g_height; ++y) {
            g_buffer[y * g_width + g_left]  = MFB_RGB(255, 0, 0);
            if(g_right < int32_t(g_width))
                g_buffer[y * g_width + g_right] = MFB_RGB(255, 0, 0);
        }
#endif

        uint32_t posX1 = 10, posY1 = 10;
        uint32_t posX2 = 160, posY2 = 10;
        int32_t  left, top;

#if defined(kShowBoundingBox)
        Rect rectSFT{};
        fontSFT.GetTextBox(text1.c_str(), 32, &rectSFT);

        left = posX1 + rectSFT.x;
        top  = posY1 + rectSFT.y;
        for(uint32_t x=left; x<posX1 + rectSFT.width; ++x) {
            g_buffer[top * g_width + x] = MFB_RGB(0, 255, 0);
            if((top + rectSFT.height) < int32_t(g_height))
                g_buffer[(posY1 + rectSFT.height) * g_width + x] = MFB_RGB(0, 255, 0);
        }

        for(uint32_t y=top; y<posY1 + rectSFT.height; ++y) {
            g_buffer[y * g_width + left]  = MFB_RGB(0, 255, 0);
            if(left + rectSFT.width < int32_t(g_width))
                g_buffer[y * g_width + posX1 + rectSFT.width-1] = MFB_RGB(0, 255, 0);
        }
#endif

#if defined(kShowBoundingBox)
        Rect rectSTB{};
        fontSTB.GetTextBox(text2.c_str(), 32, &rectSTB);

        left = posX2 + rectSTB.x;
        top  = posY2 + rectSTB.y;
        for(uint32_t x=left; x<posX2 + rectSTB.width; ++x) {
            g_buffer[top * g_width + x] = MFB_RGB(0, 255, 0);
            if((top + rectSTB.height) < int32_t(g_height))
                g_buffer[(posY2 + rectSTB.height) * g_width + x] = MFB_RGB(0, 255, 0);
        }

        for(uint32_t y=top; y<posY2 + rectSTB.height; ++y) {
            g_buffer[y * g_width + left]  = MFB_RGB(0, 255, 0);
            if(left + rectSTB.width < int32_t(g_width))
                g_buffer[y * g_width + posX2 + rectSTB.width-1] = MFB_RGB(0, 255, 0);
        }
#endif
        size_t total = 1024;

        double s1 = mfb_timer_now(timer);
        for(size_t i=0; i<total; ++i) {
            fontSFT.DrawText(text1.c_str(), 32, 0xff7f7f, g_buffer, g_width, posX1, posY1);
        }
        double e1 = mfb_timer_now(timer);

        double s2 = mfb_timer_now(timer);
        for(size_t i=0; i<total; ++i) {
            fontSTB.DrawText(text2.c_str(), 32, 0x7f7fff, g_buffer, g_width, posX2, posY2);
        }
        double e2 = mfb_timer_now(timer);

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Time: %f - %f (%d)\n", float(e1 - s1), float(e2 - s2), int(total));

        fontSFT.DrawText(buffer, 20, 0xffffff, g_buffer, g_width, 0, g_height - 32);

        state = mfb_update_ex(window, g_buffer, g_width, g_height);
        if (state != STATE_OK) {
            window = nullptr;
            break;
        }
    } while(mfb_wait_sync(window));

    free(g_buffer);
    g_buffer = nullptr;

    return 0;
}
