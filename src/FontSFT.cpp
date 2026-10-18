//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#if defined(FONTRENDERER_USE_LIBSCHRIFT)

#include "FontSFT.h"
//-------------------------------------
#include <cmath>
#include <cstring>
#include <memory>

using namespace MindShake;

//-------------------------------------
FontSFT::FontSFT(const char *fileName) : Font(fileName) {
    // sft_loadfile cannot tell a missing file from an invalid font.
    if (LoadFile(fileName) == false) {
        return;
    }

    mFont.reset(sft_loadmem(mFontFile.GetData(), mFontFile.GetSize()));
    if (mFont == nullptr) {
        mStatus = EStatus::InvalidFont;
        return;
    }

    if (InitPacker() == false) {
        return;
    }

    GetFontVMetrics();

    mGposKerning.Load(mFontFile.GetData(), mFontFile.GetSize());

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
CodePointData
FontSFT::GetCodePointData(uint32_t codePoint) {
    if (mStatus != EStatus::Ok) {
        return {};
    }

    const CodePointData *found = mCodePointData.Find(codePoint);
    if (found != nullptr) {
        return *found;
    }

    SFT_Glyph gid {};
    SFT       sft {};
    sft.xScale = mUnitsPerEm;
    sft.yScale = sft.xScale;
    sft.flags  = SFT_DOWNWARD_Y;
    sft.font   = mFont.get();
    if (sft_lookup(&sft, codePoint, &gid) < 0) {
        return {};
    }

    CodePointData   codePointData;
    SFT_GMetrics    metrics;

    if (sft_gmetrics(&sft, gid, &metrics) != 0) {
        return {};
    }

    codePointData.glyph = uint32_t(gid);
    codePointData.advanceWidth = metrics.advanceWidth;

    mCodePointData.Insert(codePoint, codePointData);
    return codePointData;
}

//-------------------------------------
bool
FontSFT::RasterizeGlyph(const CodePointData &codePointData, uint8_t height, CodePointHeightData &data, GlyphBitmap &bitmap) {
    SFT sft {};
    sft.xScale = double(GetScaleForHeight(height)) * mUnitsPerEm;
    sft.yScale = sft.xScale;
    sft.font   = mFont.get();
    sft.flags  = SFT_DOWNWARD_Y;
    SFT_GMetrics metrics{};
    if (sft_gmetrics(&sft, codePointData.glyph, &metrics) < 0) {
        return false;
    }

    data.x            = metrics.xOffset;
    data.y            = metrics.yOffset;
    data.advanceWidth = float(metrics.advanceWidth);

    int32_t w = metrics.minWidth;
    int32_t h = metrics.minHeight;
    // A glyph without an outline has no size. With a zero scale, only libschrift's extra row and column are left.
    if (w > 1 && h > 1) {
        auto pixels = std::make_unique<uint8_t[]>(size_t(w) * size_t(h));
        SFT_Image img {};
        img.width  = w;
        img.height = h;
        img.pixels = pixels.get();
        if (sft_render(&sft, codePointData.glyph, img) < 0) {
            return false;
        }

        // libschrift sizes the image from the bounding box stored in the font, plus one row and column
        // that stay empty unless the outline exceeds that box. Drop them so both backends clip the same way.
        const int32_t trimmedWidth  = w - 1;
        const int32_t trimmedHeight = h - 1;
        for (int32_t y = 1; y < trimmedHeight; ++y) {
            memmove(&pixels[size_t(y) * size_t(trimmedWidth)], &pixels[size_t(y) * size_t(w)], size_t(trimmedWidth));
        }

        bitmap.pixels = std::move(pixels);
        bitmap.width  = trimmedWidth;
        bitmap.height = trimmedHeight;
    }

    return true;
}

//-------------------------------------
int32_t
FontSFT::GetKernTableKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    SFT sft {};
    sft.xScale = mUnitsPerEm;
    sft.yScale = sft.xScale;
    sft.font   = mFont.get();
    sft.flags  = SFT_DOWNWARD_Y;

    SFT_Kerning kerning {};
    return (sft_kerning(&sft, leftGlyph, rightGlyph, &kerning) < 0) ? 0 : int32_t(kerning.xShift);
}

#endif
