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

    GetKerningTable();

    mStatus = EStatus::Ok;
}

//-------------------------------------
void
FontSTB::GetKerningTable() {
    int length = stbtt_GetKerningTableLength(&mInfo);
    if (length > 0) {
        std::vector<stbtt_kerningentry> kernings(static_cast<size_t>(length));
        stbtt_GetKerningTable(&mInfo, kernings.data(), length);
        mKerningData.reserve(size_t(length));
        for (int k = 0; k < length; ++k) {
            auto &current = kernings[k];
            mKerningData[(uint64_t(current.glyph1) << 32) | uint64_t(current.glyph2)] = current.advance;
        }
    }
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
const CodePointHeightData &
FontSTB::GetCodePointDataForHeight(uint32_t index, uint8_t height) {
    if(mStatus != EStatus::Ok) {
        return mCodePointHeightData[0];
    }

    CodePointHeight cph;
    cph.codePoint = index;
    cph.height    = height;

    auto cphd = mCodePointHeightData.find(cph.value);
    if(cphd == mCodePointHeightData.end()) {
        const CodePointData &codePoint = GetCodePointData(index);
        if(codePoint.glyph == 0) {
            return mCodePointHeightData[0];
        }

        float scale = GetScaleForHeight(height);
        int x1, y1, x2, y2;

        stbtt_GetGlyphBitmapBox(&mInfo, codePoint.glyph, scale, scale, &x1, &y1, &x2, &y2);

        CodePointHeightData codePointHeight;
        codePointHeight.glyph           = codePoint.glyph;
        codePointHeight.x               = x1;
        codePointHeight.y               = y1;
        codePointHeight.leftSideBearing = int(floor(codePoint.leftSideBearing * scale));
        codePointHeight.advanceWidth    = float(codePoint.advanceWidth) * scale;

        int w = (x2 - x1);
        int h = (y2 - y1);
        if(w > 0 && h > 0) {
            auto pixels = std::make_unique<uint8_t[]>(size_t(w) * size_t(h));
            stbtt_MakeGlyphBitmap(&mInfo, pixels.get(), w, h, w, scale, scale, codePoint.glyph);

            const int grown = ApplyAntialias(pixels, w, h);
            codePointHeight.x -= grown;
            codePointHeight.y -= grown;
            PackGlyph(pixels.get(), uint32_t(w), uint32_t(h), codePointHeight);
        }

        cphd = mCodePointHeightData.insert({cph.value, codePointHeight}).first;
    }

    return cphd->second;
}

//-------------------------------------
int
FontSTB::GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    const uint64_t key = (uint64_t(leftGlyph) << 32) | uint64_t(rightGlyph);

    auto it = mKerningData.find(key);
    if(it == mKerningData.end()) {
        // The 'kern' pairs are preloaded, so this only adds what stb can read from GPOS.
        // stb reads GPOS alone when the font has it, and it only understands part of that table,
        // so it cannot replace the 'kern' lookup: fonts with both tables would lose their kerning.
        const int advance = stbtt_GetGlyphKernAdvance(&mInfo, int(leftGlyph), int(rightGlyph));
        it = mKerningData.insert({ key, advance }).first;
    }

    return it->second;
}
