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
        mStatus = LoadMetrics(metricsFile);
        if (mStatus == EStatus::Ok) {
            mStatus = LoadTexture(textureFile);
        }
    }
    catch(const std::bad_alloc &) {
        mStatus = EStatus::OutOfMemory;
    }
    ClearIfNotLoaded();
}

//-------------------------------------
FontBaked::FontBaked(const char *metricsFile, const uint8_t *texture, uint32_t width, uint32_t height, ETextureFormat format) : FontBase(metricsFile) {
    try {
        mStatus = LoadMetrics(metricsFile);
        if (mStatus == EStatus::Ok) {
            mStatus = SetTexture(texture, width, height, format);
        }
    }
    catch(const std::bad_alloc &) {
        mStatus = EStatus::OutOfMemory;
    }
    ClearIfNotLoaded();
}

//-------------------------------------
void
FontBaked::ClearIfNotLoaded() {
    if (mStatus != EStatus::Ok) {
        mCodePointHeightData.Clear();
        mKerningData.Clear();
    }
}

//-------------------------------------
FontBaked::EStatus
FontBaked::LoadTexture(const char *textureFile) {
    if (textureFile == nullptr) {
        return EStatus::CannotOpenFile;
    }

    MappedFile    file;
    const EStatus status = GetFileStatus(file.Open(textureFile));
    if (status != EStatus::Ok) {
        return status;
    }

    std::vector<uint8_t> pixels;
    uint32_t             width, height, channels;
    ETgaAlpha            alpha;
    if (ReadTga(file.GetData(), file.GetSize(), pixels, width, height, channels, alpha) == false) {
        return EStatus::InvalidTexture;
    }

    ETextureFormat format = ETextureFormat::Alpha8;
    if (channels == 4) {
        format = (alpha == ETgaAlpha::Premultiplied) ? ETextureFormat::BGRA32Premultiplied : ETextureFormat::BGRA32;
    }

    return SetTexture(pixels.data(), width, height, format);
}

//-------------------------------------
FontBaked::EStatus
FontBaked::SetTexture(const uint8_t *texture, uint32_t width, uint32_t height, ETextureFormat format) {
    // The glyphs were checked against the size in the metrics file.
    if (width != mTextureWidth || height != mTextureHeight) {
        return EStatus::InvalidTexture;
    }

    // Without a texture, the format describes the copy the program keeps, like one in the GPU.
    if (texture != nullptr) {
        const size_t texelCount = size_t(width) * height;
        mTexture.assign(texture, texture + texelCount * GetBytesPerTexel(format));
        if (format == ETextureFormat::BGRA32) {
            PremultiplyTexels(mTexture.data(), texelCount);
            format = ETextureFormat::BGRA32Premultiplied;
        }
        else if (format == ETextureFormat::BGRA32Premultiplied) {
            // A broken texture can have a color brighter than its alpha allows, and DrawText needs it not to:
            // otherwise its blend overflows into the next channel.
            for (size_t i = 0; i < mTexture.size(); i += 4) {
                for (size_t channel = i; channel < i + 3; ++channel) {
                    mTexture[channel] = std::min(mTexture[channel], mTexture[i + 3]);
                }
            }
        }
    }
    mTextureFormat = format;

    return EStatus::Ok;
}

//-------------------------------------
HeightData
FontBaked::GetDataForHeight(uint8_t height) {
    const HeightData *data = mHeightData.Find(height);
    return (mStatus == EStatus::Ok && data != nullptr) ? *data : HeightData {};
}
