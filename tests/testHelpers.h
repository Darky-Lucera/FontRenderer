#pragma once

#include "FontSFT.h"
#include "FontSTB.h"
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

#define FONT_BACKENDS MindShake::Test::Inspectable<MindShake::FontSTB>, MindShake::Test::Inspectable<MindShake::FontSFT>
