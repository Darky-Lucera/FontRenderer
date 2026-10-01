//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontBaked.h"
#include "Tga.h"
//-------------------------------------
#include <algorithm>
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
FontBaked::FontBaked(const char *metricsFile, const uint8_t *texture, uint32_t width, uint32_t height, ETextureFormat format) : FontBase(metricsFile) {
    try {
        mStatus = LoadGlyphs(metricsFile);
        if(mStatus == EStatus::Ok) {
            mStatus = SetTexture(texture, width, height, format);
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
    uint32_t             width, height, channels;
    ETgaAlpha            alpha;
    if(ReadTga(file.GetData(), file.GetSize(), pixels, width, height, channels, alpha) == false) {
        return EStatus::InvalidTexture;
    }

    ETextureFormat format = ETextureFormat::Alpha8;
    if(channels == 4) {
        format = (alpha == ETgaAlpha::Premultiplied) ? ETextureFormat::BGRA32Premultiplied : ETextureFormat::BGRA32;
    }

    return SetTexture(pixels.data(), width, height, format);
}

//-------------------------------------
FontBaked::EStatus
FontBaked::SetTexture(const uint8_t *texture, uint32_t width, uint32_t height, ETextureFormat format) {
    // The glyphs were checked against the size in the metrics file.
    if(width != mTextureWidth || height != mTextureHeight) {
        return EStatus::InvalidTexture;
    }

    // Without a texture, the format describes the copy the program keeps, like one in the GPU.
    if(texture != nullptr) {
        const size_t texelCount = size_t(width) * height;
        mTexture.assign(texture, texture + texelCount * GetBytesPerTexel(format));
        if(format == ETextureFormat::BGRA32) {
            PremultiplyTexels(mTexture.data(), texelCount);
            format = ETextureFormat::BGRA32Premultiplied;
        }
        else if(format == ETextureFormat::BGRA32Premultiplied) {
            // A broken texture can have a color brighter than its alpha allows, and DrawText needs it not to:
            // otherwise its blend overflows into the next channel.
            for(size_t i = 0; i < mTexture.size(); i += 4) {
                for(size_t channel = i; channel < i + 3; ++channel) {
                    mTexture[channel] = std::min(mTexture[channel], mTexture[i + 3]);
                }
            }
        }
    }
    mTextureFormat = format;

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
