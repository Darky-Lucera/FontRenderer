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
#include <cstdio>

using namespace MindShake;

//-------------------------------------
FontSTB::FontSTB(const char *fontName) {
    uint32_t    size;

    // Read font into memory
    FILE *fontFile = fopen(fontName, "rb");
    if(fontFile == nullptr) {
        fprintf(stderr, "Cannot open file: '%s'.\n", fontName);
        mStatus = -2;
        return;
    }

    fseek(fontFile, 0, SEEK_END);
    size = ftell(fontFile);
    fseek(fontFile, 0, SEEK_SET);

    mFontBuffer = (uint8_t *) malloc(size);
    if(mFontBuffer == nullptr) {
        fprintf(stderr, "Not enough memory\n");
        mStatus = -3;
        return;
    }

    fread(mFontBuffer, size, 1, fontFile);
    fclose(fontFile);
    fontFile = nullptr;

    // Init Font
    if (!stbtt_InitFont(&mInfo, mFontBuffer, stbtt_GetFontOffsetForIndex(mFontBuffer, 0))) {
        fprintf(stderr, "Init font failed\n");
        mStatus = -4;
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

    mFontName = fontName;
    stbtt_GetFontVMetrics(&mInfo, &mAscent, &mDescent, &mLineGap);

    // Trash data
    mHeightData[0]          = {};
    mCodePointData[0]       = {};
    mCodePointHeightData[0] = {};

    int length = stbtt_GetKerningTableLength(&mInfo);
    if(length > 0) {
        stbtt_kerningentry *kernings = new stbtt_kerningentry[length];
        stbtt_GetKerningTable(&mInfo, kernings, length);
        mKerningData.reserve(size_t(length));
        for(int k=0; k<length; ++k) {
            auto &current = kernings[k];
            mKerningData[(uint64_t(current.glyph1) << 32) | uint64_t(current.glyph2)] = current.advance;
        }
        delete [] kernings;
    }

    mStatus = 1;
}

//-------------------------------------
FontSTB::~FontSTB() {
    if(mFontBuffer != nullptr) {
        free(mFontBuffer);
        mFontBuffer = nullptr;
    }
    if(mTexture != nullptr) {
        free(mTexture);
        mTexture = nullptr;
    }
}

//-------------------------------------
const HeightData &
FontSTB::GetDataForHeight(uint8_t height) {
    if(mStatus < 0)
        return mHeightData[0];

    auto hd = mHeightData.find(height);
    if(hd == mHeightData.end()) {
        HeightData  heightData;

        heightData.scale   = float(height) / (mAscent - mDescent);//stbtt_ScaleForPixelHeight(&mInfo, float(height));
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
FontSTB::GetCodePointData(uint32_t index) {
    if(mStatus < 0)
        return mCodePointData[0];

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

        float scale = GetScaleForHeight(height);
        int x1, y1, x2, y2, w, h;

        stbtt_GetGlyphBitmapBox(&mInfo, codePoint.glyph, scale, scale, &x1, &y1, &x2, &y2);

        codePointHeight.glyph           = codePoint.glyph;
        codePointHeight.x               = x1;
        codePointHeight.y               = y1;
        codePointHeight.leftSideBearing = int(floor(codePoint.leftSideBearing * scale));
        codePointHeight.advanceWidth    = int(ceil( codePoint.advanceWidth    * scale));


        w = codePointHeight.advanceWidth - codePointHeight.leftSideBearing;
        h = (y2 - y1);

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

        size_t byteOffset = (codePointHeight.rect.y) * mPacker.GetWidth() + codePointHeight.rect.x;
        stbtt_MakeGlyphBitmap(&mInfo, mTexture + byteOffset, x2 - x1, y2 - y1, mPacker.GetWidth(), scale, scale, codePoint.glyph);

        cphd = mCodePointHeightData.insert({cph.value, codePointHeight}).first;
    }

    return cphd->second;
}

//-------------------------------------
int
FontSTB::GetKerning(uint32_t char1, uint32_t char2) {
    auto it = mKerningData.find((uint64_t(char1) << 32) | uint64_t(char2));
    if(it != mKerningData.end()) {
        return it->second;
    }
    return 0;
}
