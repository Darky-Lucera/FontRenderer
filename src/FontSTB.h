#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Font.h"
//-------------------------------------
#include <stb/stb_truetype.h>

//-------------------------------------
namespace MindShake {

    //---------------------------------
    class FontSTB : public Font {
        public:
            explicit                    FontSTB(const char *fontName);
            virtual                     ~FontSTB();

        protected:
            int                         GetKerning(uint32_t char1, uint32_t char2) override;

            const HeightData &          GetDataForHeight(uint8_t height) override;
            const CodePointData &       GetCodePointData(uint32_t index) override;
            const CodePointHeightData & GetCodePointDataForHeight(uint32_t index, uint8_t height) override;

        protected:
            stbtt_fontinfo  mInfo {};
            uint8_t         *mFontBuffer {};
    };

} // end of namespace