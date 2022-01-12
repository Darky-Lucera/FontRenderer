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
#include <memory>

using namespace MindShake;

//-------------------------------------
FontSTB::FontSTB(const char *fontName) : Font(fontName) {
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
        fclose(fontFile);
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

    if(InitPacker() == false) {
        mStatus = -5;
        return;
    }

    stbtt_GetFontVMetrics(&mInfo, &mAscent, &mDescent, &mLineGap);

    GetKerningTable();

    mStatus = 1;
}

//-------------------------------------
void 
FontSTB::GetKerningTable() {
    int length = stbtt_GetKerningTableLength(&mInfo);
    if (length > 0) {
        stbtt_kerningentry *kernings = new stbtt_kerningentry[length];
        stbtt_GetKerningTable(&mInfo, kernings, length);
        mKerningData.reserve(size_t(length));
        for (int k = 0; k < length; ++k) {
            auto &current = kernings[k];
            mKerningData[(uint64_t(current.glyph1) << 32) | uint64_t(current.glyph2)] = current.advance;
        }
        delete[] kernings;
    }
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

        w = (x2 - x1);//codePointHeight.advanceWidth - codePointHeight.leftSideBearing;
        h = (y2 - y1);

        // special case (' ')
        bool isEmpty = false;
        if(w == 0) {
            w = 1;
            isEmpty = true;
        }
        if(h == 0) {
            h = 1;
            isEmpty = true;
        }

        codePointHeight.rect = mPacker.Insert(w, h, ELevelChoiceHeuristic::LevelBottomLeft);
        if(codePointHeight.rect.width <= 0) {
            mPacker.ResizeBin(mPacker.GetWidth(), mPacker.GetHeight() << 1);
            uint8_t *aux = (uint8_t *) realloc(mTexture, mPacker.GetWidth() * mPacker.GetHeight());
            if(aux == nullptr) {
                return mCodePointHeightData[0];
            }
            mTexture = aux;
            codePointHeight.rect = mPacker.Insert(w, h, ELevelChoiceHeuristic::LevelBottomLeft);
            if(codePointHeight.rect.width <= 0) {
                return mCodePointHeightData[0];
            }
        }

        size_t byteOffset = (codePointHeight.rect.y) * mPacker.GetWidth() + codePointHeight.rect.x;
        uint8_t *dst = mTexture + byteOffset;
        if(isEmpty) {
            int offset = 0;
            for(int y=0; y<h; ++y) {
                for(int x=0; x<w; ++x) {
                    dst[offset + x] = 0;
                }
                offset += mPacker.GetWidth();
            }
        }
        else if(mUseAntialias == false) {
            stbtt_MakeGlyphBitmap(&mInfo, dst, w, h, mPacker.GetWidth(), scale, scale, codePoint.glyph);
        }
        else {
            auto src = std::make_unique<uint8_t[]>(w * h); 
            stbtt_MakeGlyphBitmap(&mInfo, src.get(), w, h, w, scale, scale, codePoint.glyph);
            AABlock(src.get(), w, h, dst, mPacker.GetWidth());
        }

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
