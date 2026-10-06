#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

// CMake defines FONTRENDERER_USE_FREETYPE when it builds this backend. Without CMake, define it for every file.
// Otherwise, a build without the backend would only fail at link time, with a missing symbol that says little.
#if !defined(FONTRENDERER_USE_FREETYPE)
    #error "FontFT needs FONTRENDERER_USE_FREETYPE. See the README."
#endif

#include "Font.h"
//-------------------------------------
#include <memory>

// Declared here so that the code which uses FontFT does not need the headers of FreeType.
struct FT_LibraryRec_;
struct FT_FaceRec_;

//-------------------------------------
namespace MindShake {

    //---------------------------------
    class FontFT : public Font {
        public:
            // How FreeType fits the outlines to the pixel grid. Hinting makes small text sharper.
            enum class EHinting {
                None,       // The outlines as they are, like the other backends
                Light,      // Only vertically, so the spacing of the text does not change. The best choice for most text.
                            // For TrueType outlines it is always the auto hinter
                Normal,     // The instructions of the font if it has them, and else the auto hinter. Advances become whole pixels.
                            // In gray, FreeType applies TrueType instructions mostly vertically, as ClearType does
                Auto        // The auto hinter, which only looks at the outlines and ignores the instructions of the font
            };

        public:
            explicit                    FontFT(const char *fileName);

            // Changing any of these settings discards every rendered glyph, like Reset.
            void                        SetHinting(EHinting hinting);
            EHinting                    GetHinting() const                  { return mHinting;                          }
            // Pixels fully on or off, without the gray levels of FreeType. Normal hinting was designed for it.
            void                        SetMonochrome(bool enabled);
            bool                        GetMonochrome() const               { return mMonochromeEnabled;                }
            // Thickens thin stems at small sizes. It only works with hinting: Light for TrueType outlines, any for CFF.
            void                        SetStemDarkening(bool enabled);
            bool                        GetStemDarkening() const            { return mStemDarkeningEnabled;             }

        protected:
            int                         GetKernTableKerning(uint32_t leftGlyph, uint32_t rightGlyph) override;

            const CodePointData &       GetCodePointData(uint32_t codePoint) override;
            bool                        RasterizeGlyph(const CodePointData &codePointData, uint8_t height, CodePointHeightData &data, GlyphBitmap &bitmap) override;

        protected:
            // Each font has its own library, so fonts can be used from different threads.
            std::unique_ptr<FT_LibraryRec_, void (*)(FT_LibraryRec_ *)> mLibrary { nullptr, nullptr };
            // Points into mFontFile. Declared after mLibrary, because it has to be destroyed first.
            std::unique_ptr<FT_FaceRec_,    void (*)(FT_FaceRec_ *)>    mFace    { nullptr, nullptr };

            EHinting                mHinting              { EHinting::None };
            bool                    mMonochromeEnabled    {};
            bool                    mStemDarkeningEnabled {};
    };

} // end of namespace
