//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "FontSTB.h"
//-------------------------------------
#include <cmath>
#include <math.h>      // ::lround, as DJGPP has no std::lround
#include <memory>

using namespace MindShake;

//-------------------------------------
FontSTB::FontSTB(const char *fontName) : Font(fontName) {
    if(LoadFile(fontName) == false) {
        return;
    }

    // stb_truetype does not check bounds: finding the offset reads 16 bytes, and -1 means it is not a font.
    const uint8_t *data   = mFontFile.GetData();
    const int     offset  = (mFontFile.GetSize() >= 16) ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
    if(offset < 0 || stbtt_InitFont(&mInfo, data, offset) == 0) {
        mStatus = EStatus::InvalidFont;
        return;
    }

    if(InitPacker() == false) {
        return;
    }

    stbtt_GetFontVMetrics(&mInfo, &mAscent, &mDescent, &mLineGap);
    mUnitsPerEm = int(::lround(1.0f / stbtt_ScaleForMappingEmToPixels(&mInfo, 1.0f)));

    mGposKerning.Load(data, mFontFile.GetSize(), size_t(offset));

    mStatus = EStatus::Ok;
}

//-------------------------------------
const CodePointData &
FontSTB::GetCodePointData(uint32_t index) {
    if(mStatus != EStatus::Ok) {
        return mCodePointData[0];
    }

    auto cpd = mCodePointData.find(index);
    if(cpd == mCodePointData.end()) {
        int glyph = stbtt_FindGlyphIndex(&mInfo, index);
        if(glyph == 0) {
            cpd = mCodePointData.find(0);   // Trash
            cpd->second = {};
        }
        else {
            CodePointData   codePoint;

            codePoint.glyph = glyph;
            stbtt_GetGlyphHMetrics(&mInfo, glyph, &codePoint.advanceWidth, &codePoint.leftSideBearing);

            cpd = mCodePointData.insert({index, codePoint}).first;
        }
    }

    return cpd->second;
}

//-------------------------------------
bool
FontSTB::RasterizeGlyph(const CodePointData &codePoint, uint8_t height, CodePointHeightData &data, GlyphBitmap &bitmap) {
    const float scale = GetScaleForHeight(height);
    int x1, y1, x2, y2;

    stbtt_GetGlyphBitmapBox(&mInfo, codePoint.glyph, scale, scale, &x1, &y1, &x2, &y2);

    data.x               = x1;
    data.y               = y1;
    data.leftSideBearing = int(floor(codePoint.leftSideBearing * scale));
    data.advanceWidth    = float(codePoint.advanceWidth) * scale;

    const int w = (x2 - x1);
    const int h = (y2 - y1);
    if(w > 0 && h > 0) {
        bitmap.pixels = std::make_unique<uint8_t[]>(size_t(w) * size_t(h));
        bitmap.width  = w;
        bitmap.height = h;
        stbtt_MakeGlyphBitmap(&mInfo, bitmap.pixels.get(), w, h, w, scale, scale, codePoint.glyph);
    }

    return true;
}

//-------------------------------------
int
FontSTB::GetKernTableKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    // The public stbtt_GetGlyphKernAdvance ignores 'kern' when the font has GPOS.
    return stbtt__GetGlyphKernInfoAdvance(&mInfo, int(leftGlyph), int(rightGlyph));
}
