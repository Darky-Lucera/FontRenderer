#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Font.h"
//-------------------------------------
#include "libschrift/schrift.h"

//-------------------------------------
namespace MindShake {

    //---------------------------------
    class FontSFT : public Font {
        public:
            explicit                    FontSFT(const char *fontName);
            virtual                     ~FontSFT();

        protected:
            int                         GetKerning(uint32_t char1, uint32_t char2) override;

            const HeightData &          GetDataForHeight(uint8_t height) override;
            const CodePointData &       GetCodePointData(uint32_t index) override;
            const CodePointHeightData & GetCodePointDataForHeight(uint32_t index, uint8_t height) override;

        protected:
            SFT_Font    *mFont {};
    };

} // end of namespace