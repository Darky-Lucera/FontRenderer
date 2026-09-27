//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontBaked.h"
#include "Tga.h"
//-------------------------------------
#include <new>

using namespace MindShake;

//-------------------------------------
FontBaked::FontBaked(const char *metricsFile, const char *textureFile) : FontBase(metricsFile) {
    try {
        mStatus = LoadGlyphs(metricsFile);
        if(mStatus == EStatus::Ok) {
            mStatus = LoadTexture(textureFile);
        }
    }
    catch(const std::bad_alloc &) {
        mStatus = EStatus::OutOfMemory;
    }
}

//-------------------------------------
FontBaked::FontBaked(const char *metricsFile, const uint8_t *texture, uint32_t width, uint32_t height) : FontBase(metricsFile) {
    try {
        mStatus = LoadGlyphs(metricsFile);
        if(mStatus == EStatus::Ok) {
            mStatus = SetTexture(texture, width, height);
        }
    }
    catch(const std::bad_alloc &) {
        mStatus = EStatus::OutOfMemory;
    }
}

//-------------------------------------
FontBaked::EStatus
FontBaked::LoadGlyphs(const char *metricsFile) {
    const EStatus status = LoadMetrics(metricsFile);
    if(status != EStatus::Ok) {
        return status;
    }

    for(const auto &entry : mCodePointHeightData) {
        CodePointHeight cph;
        cph.value = entry.first;
        if(entry.second.glyph > 0) {
            mCodePointGlyphs[cph.codePoint] = uint32_t(entry.second.glyph);
        }
    }

    return EStatus::Ok;
}

//-------------------------------------
FontBaked::EStatus
FontBaked::LoadTexture(const char *textureFile) {
    if(textureFile == nullptr) {
        return EStatus::CannotOpenFile;
    }

    MappedFile    file;
    const EStatus status = GetFileStatus(file.Open(textureFile));
    if(status != EStatus::Ok) {
        return status;
    }

    std::vector<uint8_t> pixels;
    uint32_t             width, height;
    if(ReadTga(file.GetData(), file.GetSize(), pixels, width, height) == false) {
        return EStatus::InvalidFont;
    }

    return SetTexture(pixels.data(), width, height);
}

//-------------------------------------
FontBaked::EStatus
FontBaked::SetTexture(const uint8_t *texture, uint32_t width, uint32_t height) {
    // The glyphs were checked against the size in the metrics file.
    if(width != mTextureWidth || height != mTextureHeight) {
        return EStatus::InvalidFont;
    }

    if(texture != nullptr) {
        mTexture.assign(texture, texture + size_t(width) * height);
    }

    return EStatus::Ok;
}

//-------------------------------------
const HeightData &
FontBaked::GetDataForHeight(uint8_t height) {
    if(mStatus != EStatus::Ok) {
        return mHeightData[0];
    }

    auto hd = mHeightData.find(height);
    return (hd != mHeightData.end()) ? hd->second : mHeightData[0];
}

//-------------------------------------
const CodePointHeightData &
FontBaked::GetCodePointDataForHeight(uint32_t codePoint, uint8_t height) {
    if(mStatus != EStatus::Ok) {
        return mCodePointHeightData[0];
    }

    auto cphd = mCodePointHeightData.find(GetCodePointHeightKey(codePoint, height));
    return (cphd != mCodePointHeightData.end()) ? cphd->second : mCodePointHeightData[0];
}

//-------------------------------------
uint32_t
FontBaked::GetCodePointGlyph(uint32_t codePoint) {
    auto glyph = mCodePointGlyphs.find(codePoint);
    return (glyph != mCodePointGlyphs.end()) ? glyph->second : 0;
}

//-------------------------------------
int
FontBaked::GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    auto kerning = mKerningData.find(GetKerningKey(leftGlyph, rightGlyph));
    return (kerning != mKerningData.end()) ? kerning->second : 0;
}
