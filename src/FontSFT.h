#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Font.h"
//-------------------------------------
#include <libschrift/schrift.h>
#include <memory>

//-------------------------------------
namespace MindShake {

    //---------------------------------
    class FontSFT : public Font {
        public:
            explicit                    FontSFT(const char *fontName);

        protected:
            void                        GetFontVMetrics();
            int                         GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) override;

            const CodePointData &       GetCodePointData(uint32_t index) override;
            const CodePointHeightData & GetCodePointDataForHeight(uint32_t index, uint8_t height) override;

        protected:
            std::unique_ptr<SFT_Font, void (*)(SFT_Font *)> mFont { nullptr, sft_freefont };   // Points into mFontFile
    };

} // end of namespace