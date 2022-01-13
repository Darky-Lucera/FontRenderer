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
#include <memory>

using namespace MindShake;

//-------------------------------------
FontSFT::FontSFT(const char *fontName) : Font(fontName) {
    mFont = sft_loadfile(fontName);
    if(mFont == nullptr) {
        fprintf(stderr, "Cannot open file: '%s'.\n", fontName);
        mStatus = -2;
        return;
    }

    if(InitPacker() == false) {
        mStatus = -5;
        return;
    }

    GetFontVMetrics();

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
void
FontSFT::GetFontVMetrics() {
    SFT sft {};
    sft.xScale = sft_unitsPerEm(mFont);
    sft.yScale = sft.xScale;
    sft.flags  = SFT_DOWNWARD_Y;
    sft.font   = mFont;
    SFT_LMetrics metrics {};
    sft_lmetrics(&sft, &metrics);

    mAscent  = metrics.ascender;
    mDescent = metrics.descender;
    mLineGap = metrics.lineGap;
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

        SFT sft {};
        sft.xScale = height;
        sft.yScale = height;
        sft.font   = mFont;
        sft.flags  = SFT_DOWNWARD_Y;
        SFT_GMetrics metrics{};
        if (sft_gmetrics(&sft, codePoint.glyph, &metrics) < 0) {
            return mCodePointHeightData[0];
        }

        int w = metrics.minWidth;
        int h = metrics.minHeight;

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

        auto pixels = std::make_unique<uint8_t[]>(w * h);
        if(isEmpty) {
            int offset = 0;
            for(int y=0; y<h; ++y) {
                for(int x=0; x<w; ++x) {
                    pixels[offset + x] = 0;
                }
                offset += mPacker.GetWidth();
            }
        }
        else {
            SFT_Image img {};
            img.width  = w;
            img.height = h;
	        img.pixels = pixels.get();
            if (sft_render(&sft, codePoint.glyph, img) < 0) {
                return mCodePointHeightData[0];
            }

            if(mUseAntialias) {
                if(mAntialiasAllowEx) {
                    auto dst = std::make_unique<uint8_t[]>((w + 2) * (h + 2));
                    AABlockEx(pixels.get(), w, h, dst.get(), w + 2);
                    std::swap(pixels, dst);
                    w += 2;
                    h += 2;
                }
                else {
                    auto dst = std::make_unique<uint8_t[]>(w * h);
                    AABlock(pixels.get(), w, h, dst.get(), w);
                    std::swap(pixels, dst);
                }
            }
        }

        CodePointHeightData codePointHeight;
        codePointHeight.glyph           = codePoint.glyph;
        codePointHeight.x               = 0;
        codePointHeight.y               = metrics.yOffset;
        codePointHeight.leftSideBearing = int(floor(metrics.leftSideBearing));
        codePointHeight.advanceWidth    = int(ceil( metrics.advanceWidth));

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

        size_t byteOffset   = (codePointHeight.rect.y) * mPacker.GetWidth() + codePointHeight.rect.x;
        size_t pixelsOffset = 0;
        for(int y=0; y<h; ++y) {
            memcpy(&mTexture[byteOffset], &pixels[pixelsOffset], w);
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
