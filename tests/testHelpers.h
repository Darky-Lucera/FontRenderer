#pragma once

#if defined(FONTRENDERER_USE_FREETYPE)
#include "FontFT.h"
#endif
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
#include "FontSFT.h"
#endif
#if defined(FONTRENDERER_USE_STB)
#include "FontSTB.h"
#endif
//-------------------------------------
#include <cstddef>

//-------------------------------------
namespace MindShake { namespace Test {

    // Set by CMake so the tests find the fonts wherever they are run from.
    constexpr const char *kResourcesPath  = FONT_RENDERER_TEST_RESOURCES;
    constexpr const char *kFontPath       = FONT_RENDERER_TEST_RESOURCES "Roboto-Regular.ttf";
    // Its stored bounding boxes do not always start at the left side bearing, and do not always contain the whole outline.
    constexpr const char *kItalicFontPath = FONT_RENDERER_TEST_RESOURCES "DejaVuSerifCondensed-BoldItalic.ttf";

    // Exposes the protected internals the tests need to inspect.
    //---------------------------------
    template <class TFont>
    class Inspectable : public TFont {
        public:
            explicit Inspectable(const char *fontName = kFontPath) : TFont(fontName) { }

            using TFont::GetCodePointDataForHeight;
            using TFont::GetCodePointGlyph;
            using TFont::GetDataForHeight;
            using TFont::GetKerning;
            using TFont::PackGlyph;
            using TFont::mAscent;
            using TFont::mCodePointHeightData;
            using TFont::mDescent;
            using TFont::mUnitsPerEm;

            // Entry 0 is the placeholder returned for missing glyphs.
            size_t GetGlyphCount() const { return mCodePointHeightData.size() - 1; }
    };

}} // end of namespace

// The backends the library has, and those besides DefaultFont, which the tests compare with it.
// TEST_CASE_TEMPLATE needs the types written out, so there is one list for each combination.
#define FONT_TEST_STB MindShake::Test::Inspectable<MindShake::FontSTB>
#define FONT_TEST_SFT MindShake::Test::Inspectable<MindShake::FontSFT>
#define FONT_TEST_FT  MindShake::Test::Inspectable<MindShake::FontFT>
#if defined(FONTRENDERER_USE_STB) && defined(FONTRENDERER_USE_LIBSCHRIFT) && defined(FONTRENDERER_USE_FREETYPE)
#define FONT_BACKENDS       FONT_TEST_STB, FONT_TEST_SFT, FONT_TEST_FT
#define FONT_OTHER_BACKENDS FONT_TEST_SFT, FONT_TEST_FT
#elif defined(FONTRENDERER_USE_STB) && defined(FONTRENDERER_USE_LIBSCHRIFT)
#define FONT_BACKENDS       FONT_TEST_STB, FONT_TEST_SFT
#define FONT_OTHER_BACKENDS FONT_TEST_SFT
#elif defined(FONTRENDERER_USE_STB) && defined(FONTRENDERER_USE_FREETYPE)
#define FONT_BACKENDS       FONT_TEST_STB, FONT_TEST_FT
#define FONT_OTHER_BACKENDS FONT_TEST_FT
#elif defined(FONTRENDERER_USE_LIBSCHRIFT) && defined(FONTRENDERER_USE_FREETYPE)
#define FONT_BACKENDS       FONT_TEST_SFT, FONT_TEST_FT
#define FONT_OTHER_BACKENDS FONT_TEST_FT
#elif defined(FONTRENDERER_USE_STB)
#define FONT_BACKENDS       FONT_TEST_STB
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
#define FONT_BACKENDS       FONT_TEST_SFT
#else
#define FONT_BACKENDS       FONT_TEST_FT
#endif

namespace MindShake { namespace Test {

    // For the tests of what Font does the same whatever the backend.
#if defined(FONTRENDERER_USE_STB)
    using DefaultFont = FontSTB;
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
    using DefaultFont = FontSFT;
#else
    using DefaultFont = FontFT;
#endif

}} // end of namespace
