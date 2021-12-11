//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontSFT.h"
//-------------------------------------
#include <cstdio>
#include <cmath>

using namespace MindShake;

//-------------------------------------
FontSFT::FontSFT(const char *fontName) {
    mFont = sft_loadfile(fontName);
    if(mFont == nullptr) {
        fprintf(stderr, "Cannot open file: '%s'.\n", fontName);
        mStatus = -2;
        return;
    }

    // Init packer
    mPacker.Init(512, 128, false);
    mTexture = (uint8_t *) calloc(mPacker.GetWidth() * mPacker.GetHeight(), 1);
    if(mTexture == nullptr) {
        fprintf(stderr, "Not enough memory\n");
        mStatus = -5;
        return;
    }

    SFT sft {};
    sft.xScale = sft_unitsPerEm(mFont);
    sft.yScale = sft.xScale;
    sft.flags  = SFT_DOWNWARD_Y;
    sft.font   = mFont;
    SFT_LMetrics metrics {};
    sft_lmetrics(&sft, &metrics);

    mFontName = fontName;
    mAscent   = metrics.ascender;
    mDescent  = metrics.descender;
    mLineGap  = metrics.lineGap;

    // Trash data
    mHeightData[0]          = {};
    mCodePointData[0]       = {};
    mCodePointHeightData[0] = {};

    mStatus = 1;
}

//-------------------------------------
FontSFT::~FontSFT() {
    if(mFont != nullptr) {
        sft_freefont(mFont);
        mFont = nullptr;
    }
    if(mTexture != nullptr) {
        free(mTexture);
        mTexture = nullptr;
    }
}

//-------------------------------------
const HeightData &
FontSFT::GetDataForHeight(uint8_t height) {
    if(mStatus < 0)
        return mHeightData[0];

    auto hd = mHeightData.find(height);
    if(hd == mHeightData.end()) {
        HeightData  heightData;

        //SFT sft {};
        //sft.xScale = height;
        //sft.yScale = height;
        //sft.font   = mFont;
        //sft.flags  = SFT_DOWNWARD_Y;
        //
        //SFT_LMetrics metrics;
        //sft_lmetrics(&sft, &metrics);

        heightData.scale   = float(height) / (mAscent - mDescent);//sft_unitsPerEm(mFont);
        heightData.ascent  = int(ceil(mAscent  * heightData.scale));
        heightData.descent = int(ceil(mDescent * heightData.scale));
        heightData.lineGap = int(ceil(mLineGap * heightData.scale));
        mHeightData[height] = heightData;

        hd = mHeightData.insert({height, heightData}).first;
    }

    return hd->second;
}

//-------------------------------------
const CodePointData &
FontSFT::GetCodePointData(uint32_t index) {
    if(mStatus < 0)
        return mCodePointData[0];

    auto cpd = mCodePointData.find(index);
    if(cpd == mCodePointData.end()) {
        SFT_Glyph gid {};
        SFT       sft {};
        sft.xScale = sft_unitsPerEm(mFont);
        sft.yScale = sft.xScale;
        sft.flags  = SFT_DOWNWARD_Y;
        sft.font   = mFont;
        if (sft_lookup(&sft, index, &gid) < 0) {
            return mCodePointData[0];
        }
        else {
            CodePointData   codePoint;
            SFT_GMetrics    metrics;

            if(sft_gmetrics(&sft, gid, &metrics) != 0)
                return mCodePointData[0];

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
    if(mStatus < 0)
        return mCodePointHeightData[0];

    CodePointHeight cph;
    cph.codePoint = index;
    cph.height    = height;

    auto cphd = mCodePointHeightData.find(cph.value);
    if(cphd == mCodePointHeightData.end()) {
        const CodePointData &codePoint = GetCodePointData(index);
        if(codePoint.glyph == 0) {
            return mCodePointHeightData[0];
        }

        CodePointHeightData codePointHeight;

        SFT sft {};
        sft.xScale = height;
        sft.yScale = height;
        sft.font   = mFont;
        sft.flags  = SFT_DOWNWARD_Y;
        SFT_GMetrics metrics{};
        if (sft_gmetrics(&sft, codePoint.glyph, &metrics) < 0) {
            return mCodePointHeightData[0];
        }

        // special case (' ')
        if(metrics.minWidth == 0)
            metrics.minWidth = 1;
        if(metrics.minHeight == 0)
            metrics.minHeight = 1;

        SFT_Image img {};
        img.width  = metrics.minWidth;
        img.height = metrics.minHeight;
        img.pixels = new uint8_t[img.width * img.height];
        if (sft_render(&sft, codePoint.glyph, img) < 0) {
            return mCodePointHeightData[0];
        }

        codePointHeight.glyph           = codePoint.glyph;
        codePointHeight.x               = 0;
        codePointHeight.y               = metrics.yOffset;
        codePointHeight.leftSideBearing = int(floor(metrics.leftSideBearing));
        codePointHeight.advanceWidth    = int(ceil( metrics.advanceWidth));

        int w = img.width;
        int h = img.height;

        codePointHeight.rect = mPacker.Insert(w, h, ELevelChoiceHeuristic::LevelBottomLeft);
        if(codePointHeight.rect.width <= 0) {
            mPacker.ResizeBin(mPacker.GetWidth(), mPacker.GetHeight() << 1);
            mTexture = (uint8_t *) realloc(mTexture, mPacker.GetWidth() * mPacker.GetHeight());
            if(mTexture == nullptr) {
                return mCodePointHeightData[0];
            }
            codePointHeight.rect = mPacker.Insert(w, h, ELevelChoiceHeuristic::LevelBottomLeft);
            if(codePointHeight.rect.width <= 0) {
                return mCodePointHeightData[0];
            }
        }

        size_t byteOffset   = (codePointHeight.rect.y) * mPacker.GetWidth() + codePointHeight.rect.x;
        size_t pixelsOffset = 0;
        for(int y=0; y<h; ++y) {
            memcpy(&mTexture[byteOffset], &reinterpret_cast<const uint8_t *>(img.pixels)[pixelsOffset], w);
            byteOffset   += mPacker.GetWidth();
            pixelsOffset += w;
        }

        cphd = mCodePointHeightData.insert({cph.value, codePointHeight}).first;
    }

    return cphd->second;
}

//-------------------------------------
int
FontSFT::GetKerning(uint32_t char1, uint32_t char2) {
    auto it = mKerningData.find((uint64_t(char1) << 32) | uint64_t(char2));
    if(it != mKerningData.end()) {
        return it->second;
    }
    else {
        SFT       sft {};
        sft.xScale = sft_unitsPerEm(mFont);
        sft.yScale = sft.xScale;
        sft.font   = mFont;
        sft.flags  = SFT_DOWNWARD_Y;
        SFT_Kerning kerning;
        sft_kerning(&sft, char1, char2, &kerning);
        //if(kerning.xShift != 0)
            mKerningData[(uint64_t(char1) << 32) | uint64_t(char2)] = int32_t(kerning.xShift);
    }
    return 0;
}
