//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontSFT.h"
//-------------------------------------
#include <cmath>
#include <cstring>
#include <memory>

using namespace MindShake;

//-------------------------------------
FontSFT::FontSFT(const char *fontName) : Font(fontName) {
    // sft_loadfile cannot tell a missing file from an invalid font.
    if(LoadFile(fontName) == false) {
        return;
    }

    mFont.reset(sft_loadmem(mFontFile.GetData(), mFontFile.GetSize()));
    if(mFont == nullptr) {
        mStatus = EStatus::InvalidFont;
        return;
    }

    if(InitPacker() == false) {
        return;
    }

    GetFontVMetrics();

    mStatus = EStatus::Ok;
}

//-------------------------------------
void
FontSFT::GetFontVMetrics() {
    mUnitsPerEm = sft_unitsPerEm(mFont.get());

    SFT sft {};
    sft.xScale = mUnitsPerEm;
    sft.yScale = sft.xScale;
    sft.flags  = SFT_DOWNWARD_Y;
    sft.font   = mFont.get();
    SFT_LMetrics metrics {};
    sft_lmetrics(&sft, &metrics);

    mAscent  = metrics.ascender;
    mDescent = metrics.descender;
    mLineGap = metrics.lineGap;
}

//-------------------------------------
const CodePointData &
FontSFT::GetCodePointData(uint32_t index) {
    if(mStatus != EStatus::Ok) {
        return mCodePointData[0];
    }

    auto cpd = mCodePointData.find(index);
    if(cpd == mCodePointData.end()) {
        SFT_Glyph gid {};
        SFT       sft {};
        sft.xScale = mUnitsPerEm;
        sft.yScale = sft.xScale;
        sft.flags  = SFT_DOWNWARD_Y;
        sft.font   = mFont.get();
        if (sft_lookup(&sft, index, &gid) < 0) {
            return mCodePointData[0];
        }
        else {
            CodePointData   codePoint;
            SFT_GMetrics    metrics;

            if(sft_gmetrics(&sft, gid, &metrics) != 0) {
                return mCodePointData[0];
            }

            codePoint.glyph = gid;
            codePoint.advanceWidth = metrics.advanceWidth;
            codePoint.leftSideBearing = metrics.leftSideBearing;

            cpd = mCodePointData.insert({index, codePoint}).first;
        }
    }

    return cpd->second;
}

//-------------------------------------
const CodePointHeightData &
FontSFT::GetCodePointDataForHeight(uint32_t index, uint8_t height) {
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

        SFT sft {};
        sft.xScale = double(GetScaleForHeight(height)) * mUnitsPerEm;
        sft.yScale = sft.xScale;
        sft.font   = mFont.get();
        sft.flags  = SFT_DOWNWARD_Y;
        SFT_GMetrics metrics{};
        if (sft_gmetrics(&sft, codePoint.glyph, &metrics) < 0) {
            return mCodePointHeightData[0];
        }

        CodePointHeightData codePointHeight;
        codePointHeight.glyph           = codePoint.glyph;
        codePointHeight.x               = metrics.xOffset;
        codePointHeight.y               = metrics.yOffset;
        codePointHeight.leftSideBearing = int(floor(metrics.leftSideBearing));
        codePointHeight.advanceWidth    = float(metrics.advanceWidth);

        int w = metrics.minWidth;
        int h = metrics.minHeight;
        // A glyph without an outline has no size. With a zero scale, only libschrift's extra row and column are left.
        if(w > 1 && h > 1) {
            auto pixels = std::make_unique<uint8_t[]>(size_t(w) * size_t(h));
            SFT_Image img {};
            img.width  = w;
            img.height = h;
            img.pixels = pixels.get();
            if (sft_render(&sft, codePoint.glyph, img) < 0) {
                return mCodePointHeightData[0];
            }

            // libschrift sizes the image from the bounding box stored in the font, plus one row and column
            // that stay empty unless the outline exceeds that box. Drop them so both backends clip the same way.
            const int trimmedWidth  = w - 1;
            const int trimmedHeight = h - 1;
            for(int y = 1; y < trimmedHeight; ++y)
                memmove(&pixels[size_t(y) * size_t(trimmedWidth)], &pixels[size_t(y) * size_t(w)], size_t(trimmedWidth));
            w = trimmedWidth;
            h = trimmedHeight;

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
FontSFT::GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    const uint64_t key = (uint64_t(leftGlyph) << 32) | uint64_t(rightGlyph);

    auto it = mKerningData.find(key);
    if(it == mKerningData.end()) {
        SFT sft {};
        sft.xScale = mUnitsPerEm;
        sft.yScale = sft.xScale;
        sft.font   = mFont.get();
        sft.flags  = SFT_DOWNWARD_Y;

        SFT_Kerning kerning {};
        const int32_t advance = (sft_kerning(&sft, leftGlyph, rightGlyph, &kerning) < 0) ? 0 : int32_t(kerning.xShift);
        it = mKerningData.insert({ key, advance }).first;
    }

    return it->second;
}
