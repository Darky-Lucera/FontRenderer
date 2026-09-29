#include "exampleCommon.h"
#if defined(FONTRENDERER_USE_STB)
    #include <FontSTB.h>
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
    #include <FontSFT.h>
#else
    #include <FontFT.h>
#endif
//-------------------------------------
#include <cstdio>
#include <string>

// Bakes the font that exampleEffects draws. The texture it saves is 8-bit grayscale: tools/add_effects.py, or an
// image program, adds the effects to it and saves the result as resources/effectfx.tga. See the README.

//-------------------------------------
#if defined(FONTRENDERER_USE_STB)
using BakedFont = MindShake::FontSTB;
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
using BakedFont = MindShake::FontSFT;
#else
using BakedFont = MindShake::FontFT;
#endif

//-------------------------------------
static const char       *kFontPath    = "resources/LilitaOne-Regular.ttf";
static const char       *kMetricsPath = "resources/effects.frb";
static const char       *kTexturePath = "resources/effects.tga";
static const uint8_t    kSize         = 32;

//-------------------------------------
int
main(int argc, char *argv[]) {
    (void) argc;
    example_set_app_directory(argv[0]);

    BakedFont font(kFontPath);
    if (font.GetStatus() != MindShake::Font::EStatus::Ok) {
        fprintf(stderr, "Cannot load %s (status %d).\n", kFontPath, int(font.GetStatus()));
        return -1;
    }

    // Room for the effects of tools/add_effects.py: a 2 pixel outline, and a shadow 4 pixels down whose blur spreads 4 more.
    font.SetGlyphPadding(6, 4, 6, 10);
    // The shadow and the light of the bevel come from above, so they would be wrong on a transposed glyph.
    font.SetAllowRotation(false);

    std::string ascii;
    for (char c = ' '; c <= '~'; ++c) {
        ascii += c;
    }
    if (font.Preload(ascii.c_str(), kSize) == false) {
        fprintf(stderr, "Some glyphs do not fit in the texture.\n");
        return -1;
    }
    font.LoadAllKerningPairs();

    const MindShake::Font::EStatus saved = font.SaveBaked(kMetricsPath, kTexturePath);
    if (saved != MindShake::Font::EStatus::Ok) {
        fprintf(stderr, "Cannot save the baked font (status %d).\n", int(saved));
        return -1;
    }

    printf("Saved %s and %s, %u x %u.\n", kMetricsPath, kTexturePath, unsigned(font.GetUsedTextureWidth()), unsigned(font.GetUsedTextureHeight()));
    return 0;
}
