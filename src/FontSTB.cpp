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
FontSTB::FontSTB(const char *fileName) : Font(fileName) {
    if (LoadFile(fileName) == false) {
        return;
    }

    // stb_truetype does not check bounds: finding the offset reads 16 bytes, and -1 means it is not a font.
    const uint8_t *data   = mFontFile.GetData();
    const int32_t offset  = (mFontFile.GetSize() >= 16) ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
    if (offset < 0 || stbtt_InitFont(&mInfo, data, offset) == 0) {
        mStatus = EStatus::InvalidFont;
        return;
    }

    if (InitPacker() == false) {
        return;
    }

    // stb_truetype writes them through pointers to int, and the standard does not require int32_t to be int.
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&mInfo, &ascent, &descent, &lineGap);
    mAscent  = ascent;
    mDescent = descent;
    mLineGap = lineGap;
    mUnitsPerEm = int32_t(::lround(1.0f / stbtt_ScaleForMappingEmToPixels(&mInfo, 1.0f)));

    mGposKerning.Load(data, mFontFile.GetSize(), size_t(offset));

    mStatus = EStatus::Ok;
}

//-------------------------------------
CodePointData
FontSTB::GetCodePointData(uint32_t codePoint) {
    if (mStatus != EStatus::Ok) {
        return {};
    }

    const CodePointData *found = mCodePointData.Find(codePoint);
    if (found != nullptr) {
        return *found;
    }

    const int32_t glyph = stbtt_FindGlyphIndex(&mInfo, int(codePoint));
    if (glyph == 0) {
        return {};
    }

    CodePointData   codePointData;

    int advanceWidth;   // int, as the metrics of the constructor
    stbtt_GetGlyphHMetrics(&mInfo, glyph, &advanceWidth, nullptr);
    codePointData.glyph        = uint32_t(glyph);
    codePointData.advanceWidth = advanceWidth;

    mCodePointData.Insert(codePoint, codePointData);
    return codePointData;
}

//-------------------------------------
bool
FontSTB::RasterizeGlyph(const CodePointData &codePointData, uint8_t height, CodePointHeightData &data, GlyphBitmap &bitmap) {
    const float scale = GetScaleForHeight(height);
    int x1, y1, x2, y2;     // int, as the metrics of the constructor

    stbtt_GetGlyphBitmapBox(&mInfo, int(codePointData.glyph), scale, scale, &x1, &y1, &x2, &y2);

    data.x            = x1;
    data.y            = y1;
    data.advanceWidth = float(codePointData.advanceWidth) * scale;

    const int32_t w = (x2 - x1);
    const int32_t h = (y2 - y1);
    if (w > 0 && h > 0) {
        bitmap.pixels = std::make_unique<uint8_t[]>(size_t(w) * size_t(h));
        bitmap.width  = w;
        bitmap.height = h;
        stbtt_MakeGlyphBitmap(&mInfo, bitmap.pixels.get(), w, h, w, scale, scale, int(codePointData.glyph));
    }

    return true;
}

//-------------------------------------
int32_t
FontSTB::GetKernTableKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    // The public stbtt_GetGlyphKernAdvance ignores 'kern' when the font has GPOS.
    return stbtt__GetGlyphKernInfoAdvance(&mInfo, int32_t(leftGlyph), int32_t(rightGlyph));
}
