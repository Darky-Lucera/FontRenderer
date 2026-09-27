#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

// CMake defines FONTRENDERER_USE_STB when it builds this backend. Without CMake, define it for every file.
// Otherwise, a build without the backend would only fail at link time, with a missing symbol that says little.
#if !defined(FONTRENDERER_USE_STB)
    #error "FontSTB needs FONTRENDERER_USE_STB. See the README."
#endif

#include "Font.h"
//-------------------------------------
#include <stb/stb_truetype.h>

//-------------------------------------
namespace MindShake {

    //---------------------------------
    class FontSTB : public Font {
        public:
            explicit                    FontSTB(const char *fontName);

        protected:
            int                         GetKernTableKerning(uint32_t leftGlyph, uint32_t rightGlyph) override;

            const CodePointData &       GetCodePointData(uint32_t index) override;
            const CodePointHeightData & GetCodePointDataForHeight(uint32_t index, uint8_t height) override;

        protected:
            stbtt_fontinfo          mInfo {};       // Points into mFontFile
    };

} // end of namespace