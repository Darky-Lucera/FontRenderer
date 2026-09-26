//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Font.h"
#include "UTF8_Utils.h"
//-------------------------------------
#include <algorithm>
#include <cassert>
#include <cmath>
#include <math.h>      // ::lround, as DJGPP has no std::lround
#include <cstdio>
#include <cstring>
#include <new>

using namespace MindShake;

// C++14 needs these definitions whenever the constants are bound to a reference.
constexpr uint32_t Font::kMaxTextureSize;
constexpr int32_t  Font::kMaxAntialiasWeight;

//-------------------------------------
Font::Font(const char *fontName) {
    mFontName = fontName;

    // Trash data
    mHeightData[0]          = {};
    mCodePointData[0]       = {};
    mCodePointHeightData[0] = {};
}

//-------------------------------------
void
Font::Reset() {
    mPacker.Reset();

    mCodePointHeightData.clear();
    mCodePointHeightData[0] = {};

    std::fill(mTexture.begin(), mTexture.end(), uint8_t(0));
}

//-------------------------------------
void
Font::SetSizeMode(ESizeMode mode) {
    if(mode == mSizeMode) {
        return;
    }

    mSizeMode = mode;

    // The scale of every height changes, so the cached metrics and the glyphs rendered with them are stale.
    mHeightData.clear();
    mHeightData[0] = {};
    Reset();
}

//-------------------------------------
bool
Font::LoadFile(const char *fileName) {
    switch(mFontFile.Open(fileName)) {
        case MappedFile::EError::None:
            return true;

        case MappedFile::EError::CannotOpen:
            fprintf(stderr, "Cannot open file: '%s'.\n", fileName);
            mStatus = EStatus::CannotOpenFile;
            return false;

        case MappedFile::EError::CannotRead:
            fprintf(stderr, "Cannot read file: '%s'.\n", fileName);
            mStatus = EStatus::CannotReadFile;
            return false;

        case MappedFile::EError::OutOfMemory:
            fprintf(stderr, "Not enough memory\n");
            mStatus = EStatus::OutOfMemory;
            return false;
    }

    return false;
}

//-------------------------------------
bool
Font::InitPacker() {
    mPacker.Init(512 - mGlyphPadding, 128 - mGlyphPadding, true);
    try {
        mTexture.assign(size_t(GetTextureWidth()) * GetTextureHeight(), 0);
    }
    catch(const std::bad_alloc &) {
        fprintf(stderr, "Not enough memory\n");
        mStatus = EStatus::OutOfMemory;
        return false;
    }

    return true;
}

//-------------------------------------
uint32_t
Font::GetUsedTextureWidth() const {
    const uint32_t used = mPacker.GetUsedWidth();
    return (used > 0) ? used + mGlyphPadding : 0;
}

//-------------------------------------
uint32_t
Font::GetUsedTextureHeight() const {
    const uint32_t used = mPacker.GetUsedHeight();
    return (used > 0) ? used + mGlyphPadding : 0;
}

//-------------------------------------
void
Font::SetAntialias(bool set) {
    if(set == mUseAntialias) {
        return;
    }

    mUseAntialias = set;
    Reset();
}

//-------------------------------------
void
Font::SetAntialiasAllowEx(bool set) {
    if(set == mAntialiasAllowEx) {
        return;
    }

    mAntialiasAllowEx = set;
    Reset();
}

//-------------------------------------
bool
Font::SetAntialiasWeights(int32_t center, int32_t border, int32_t corner) {
    auto isValid = [](int32_t weight) { return weight >= 0 && weight <= kMaxAntialiasWeight; };
    if(isValid(center) == false || isValid(border) == false || isValid(corner) == false || center + border + corner == 0) {
        return false;
    }

    if(center == mAACenter && border == mAABorder && corner == mAACorner) {
        return true;
    }

    mAACenter = center;
    mAABorder = border;
    mAACorner = corner;
    Reset();

    return true;
}

//-------------------------------------
bool
Font::SetGlyphPadding(uint32_t padding) {
    if(padding == mGlyphPadding) {
        return true;
    }

    const uint32_t textureWidth  = GetTextureWidth();
    const uint32_t textureHeight = GetTextureHeight();
    if(padding >= textureWidth || padding >= textureHeight) {
        return false;
    }

    mGlyphPadding = padding;
    mPacker.Init(textureWidth - padding, textureHeight - padding, true);
    Reset();

    return true;
}

//-------------------------------------
bool
Font::PackGlyph(const uint8_t *pixels, uint32_t width, uint32_t height, CodePointHeightData &data) {
    data.rect    = {};
    data.rotated = false;

    // Fail early instead of growing the texture up to the limit for nothing.
    const bool canGrowWidth  = mTextureGrowth != ETextureGrowth::Height;
    const bool canGrowHeight = mTextureGrowth != ETextureGrowth::Width;
    auto canEverFit = [&](uint32_t w, uint32_t h) {
        return (canGrowWidth || w <= mPacker.GetWidth()) && (canGrowHeight || h <= mPacker.GetHeight());
    };
    const uint32_t paddedWidth  = width  + mGlyphPadding;
    const uint32_t paddedHeight = height + mGlyphPadding;
    if(canEverFit(paddedWidth, paddedHeight) == false && canEverFit(paddedHeight, paddedWidth) == false) {
        return false;
    }

    Rect &rect = data.rect;
    rect = mPacker.Insert(paddedWidth, paddedHeight, mPackingHeuristic);
    while(rect.width <= 0) {
        if(GrowTexture() == false) {
            return false;
        }

        rect = mPacker.Insert(paddedWidth, paddedHeight, mPackingHeuristic);
    }
    data.rotated = uint32_t(rect.width) != paddedWidth;

    // The padding reserved at the right and bottom of the glyph stays empty. Shifting by the padding moves
    // the glyph past the empty band along the top and left texture edges, so it has empty pixels on every side.
    rect.x      += int32_t(mGlyphPadding);
    rect.y      += int32_t(mGlyphPadding);
    rect.width  -= int32_t(mGlyphPadding);
    rect.height -= int32_t(mGlyphPadding);

    const size_t textureWidth = GetTextureWidth();
    const size_t stepX        = data.rotated ? textureWidth : 1;
    const size_t stepY        = data.rotated ? 1 : textureWidth;
    uint8_t      *row         = &mTexture[size_t(rect.y) * textureWidth + size_t(rect.x)];
    for(uint32_t y = 0; y < height; ++y, row += stepY) {
        uint8_t *dst = row;
        for(uint32_t x = 0; x < width; ++x, dst += stepX)
            *dst = *pixels++;
    }

    return true;
}

//-------------------------------------
bool
Font::GrowTexture() {
    const uint32_t oldWidth  = GetTextureWidth();
    const uint32_t oldHeight = GetTextureHeight();
    const uint32_t newWidth  = (mTextureGrowth != ETextureGrowth::Height) ? oldWidth  * 2 : oldWidth;
    const uint32_t newHeight = (mTextureGrowth != ETextureGrowth::Width)  ? oldHeight * 2 : oldHeight;
    if(newWidth > kMaxTextureSize || newHeight > kMaxTextureSize) {
        return false;
    }

    try {
        if(newWidth == oldWidth) {
            // Same stride: the existing rows are still valid after the resize.
            mTexture.resize(size_t(newWidth) * newHeight, 0);
        }
        else {
            std::vector<uint8_t> texture(size_t(newWidth) * newHeight, 0);
            for(uint32_t y = 0; y < oldHeight; ++y)
                memcpy(&texture[size_t(y) * newWidth], &mTexture[size_t(y) * oldWidth], oldWidth);

            mTexture.swap(texture);
        }
    }
    catch(const std::bad_alloc &) {
        return false;
    }

    // Cannot fail: the new size is bigger and within kMaxTextureSize.
    const bool resized = mPacker.ResizeBin(newWidth - mGlyphPadding, newHeight - mGlyphPadding);
    assert(resized);
    (void) resized;

    return true;
}

//-------------------------------------
float
Font::GetScaledKerning(int glyph, uint32_t nextCodePoint, float scale) {
    if(nextCodePoint == 0) {
        return 0.0f;
    }

    return float(GetKerning(uint32_t(glyph), GetCodePointGlyph(nextCodePoint))) * scale;
}

//-------------------------------------
int
Font::ApplyAntialias(std::unique_ptr<uint8_t[]> &pixels, int &width, int &height) {
    assert(width > 0 && height > 0);
    if(mUseAntialias == false) {
        return 0;
    }

    if(mAntialiasAllowEx) {
        auto dst = std::make_unique<uint8_t[]>(size_t(width + 2) * size_t(height + 2));
        AABlockEx(pixels.get(), uint32_t(width), uint32_t(height), dst.get(), uint32_t(width + 2));
        pixels  = std::move(dst);
        width  += 2;
        height += 2;
        return 1;
    }

    auto dst = std::make_unique<uint8_t[]>(size_t(width) * size_t(height));
    AABlock(pixels.get(), uint32_t(width), uint32_t(height), dst.get(), uint32_t(width));
    pixels = std::move(dst);
    return 0;
}

//-------------------------------------
void
Font::DrawText(const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY) {
    if(utf8 == nullptr || textHeight == 0) {
        return;
    }

    uint32_t offsetDst, offsetTexture;
    float    offsetTextX;
    int32_t  offsetTextY;
    int32_t  currentX, currentY;
    int32_t  minX, maxX, minY, maxY;

    Color32 fontColor = *reinterpret_cast<Color32 *>(&color);

    const HeightData &heightData = GetDataForHeight(textHeight);

    posY += heightData.ascent; // baseline

    offsetTextX = 0.0f;
    offsetTextY = 0;
    const uint8_t *text = reinterpret_cast<const uint8_t *>(utf8);
    for(uint32_t codePoint = GetNextUTF32(&text), nextCodePoint; codePoint != 0; codePoint = nextCodePoint) {
        nextCodePoint = GetNextUTF32(&text);
        if(codePoint == '\n') {
            offsetTextX = 0.0f;
            offsetTextY += heightData.GetLineAdvance();
            continue;
        }

        const CodePointHeightData &data = GetCodePointDataForHeight(codePoint, textHeight);
        if(data.glyph > 0) {
            // Clip Top
            currentY = posY + data.y + offsetTextY;
            minY = 0;
            if(currentY < mTop) {
                minY    += mTop - currentY;
                currentY = mTop;
            }

            // Clip Bottom (if the beginning is beyond the bottom limit)
            if(currentY < mBottom) {
                // Clip Left
                currentX = posX + data.x + int32_t(::lround(offsetTextX));
                minX = 0;
                if(currentX < mLeft) {
                    minX    += mLeft - currentX;
                    currentX = mLeft;
                }

                // Clip Right (if the beginning is beyond the right limit)
                if(currentX < mRight) {
                    // Clip Right
                    maxX = data.GetWidth();
                    if(currentX + maxX - minX >= mRight) {
                        maxX = minX + mRight - currentX;
                    }

                    // Clip Bottom
                    maxY = data.GetHeight();
                    if(currentY + maxY - minY >= mBottom) {
                        maxY = minY + mBottom - currentY;
                    }

                    // Let's draw
                    const uint32_t textureWidth = GetTextureWidth();
                    const uint32_t stepX        = data.rotated ? textureWidth : 1;
                    const uint32_t stepY        = data.rotated ? 1 : textureWidth;
                    offsetTexture = data.rect.y * textureWidth + data.rect.x + minY * stepY + minX * stepX;
                    offsetDst     = currentY * dstStride + currentX;
                    for(int glyphY=minY; glyphY<maxY; ++glyphY) {
                        uint32_t texel = offsetTexture;
                        for(int glyphX=minX, dstX=0; glyphX<maxX; ++glyphX, ++dstX, texel += stepX) {
                            if(mTexture[texel] != 0) {
                                uint32_t grey    = uint32_t((mTexture[texel] * fontColor.a) / 255);
                                uint32_t invGrey = 255 - grey;

                                Color32  &dstColor = *reinterpret_cast<Color32 *>(&dst[offsetDst + dstX]);
                                dstColor.b = ((fontColor.b * grey) + (dstColor.b * invGrey)) / 255;
                                dstColor.g = ((fontColor.g * grey) + (dstColor.g * invGrey)) / 255;
                                dstColor.r = ((fontColor.r * grey) + (dstColor.r * invGrey)) / 255;
                                dstColor.a = 255;
                            }
                        }
                        offsetTexture += stepY;
                        offsetDst     += dstStride;
                    }
                }
            }
            offsetTextX += data.advanceWidth + GetScaledKerning(data.glyph, nextCodePoint, heightData.scale);
        }
    }
}

//-------------------------------------
void
Font::GetTextBox(const char *utf8, uint8_t textHeight, Rect *pRect) {
    if(pRect == nullptr) {
        return;
    }

    *pRect = {};
    if(utf8 == nullptr || textHeight == 0) {
        return;
    }

    const HeightData &heightData = GetDataForHeight(textHeight);

    int32_t minX = INT32_MAX, maxX = INT32_MIN;
    int32_t minY = INT32_MAX, maxY = INT32_MIN;
    float   offsetTextX = 0.0f;
    int32_t offsetTextY = 0;

    const uint8_t *text = reinterpret_cast<const uint8_t *>(utf8);
    for(uint32_t codePoint = GetNextUTF32(&text), nextCodePoint; codePoint != 0; codePoint = nextCodePoint) {
        nextCodePoint = GetNextUTF32(&text);
        if(codePoint == '\n') {
            offsetTextX = 0.0f;
            offsetTextY += heightData.GetLineAdvance();
            continue;
        }

        const CodePointHeightData &data = GetCodePointDataForHeight(codePoint, textHeight);
        if(data.glyph > 0) {
            const int32_t penX = int32_t(::lround(offsetTextX));
            if(data.GetWidth() > 0) {
                const int32_t left = penX + data.x;
                const int32_t top  = heightData.ascent + data.y + offsetTextY;

                minX = std::min(minX, left);
                maxX = std::max(maxX, left + data.GetWidth());
                minY = std::min(minY, top);
                maxY = std::max(maxY, top + data.GetHeight());
            }
            else {
                minX = std::min(minX, penX);
            }
            // The advance counts too, so trailing spaces widen the box.
            maxX = std::max(maxX, int32_t(::lround(offsetTextX + data.advanceWidth)));

            offsetTextX += data.advanceWidth + GetScaledKerning(data.glyph, nextCodePoint, heightData.scale);
        }
    }

    if(minX > maxX || minY > maxY) {
        return;
    }

    *pRect = { minX, minY, maxX - minX, maxY - minY };
}

// TODO: Think where put these funcs...
//---------------------------------
static inline uint8_t
GetAAColorClip(int32_t x, int32_t y, int32_t width, int32_t height, const uint8_t *pImg, int32_t stride, int32_t center, int32_t border, int32_t corner) {
    int32_t   offset;
    uint32_t  color;
    int32_t   divisor;

    offset  = x + y * stride;

    divisor = center + border * 4 + corner * 4;

    color = (divisor >> 1);
    if(x > 0 && x <= width) {
        if(y >= 0 && y < height) {
            color += border * pImg[offset - 1];             // Left
        }
        if(y > 0 && y <= height) {
            color += corner * pImg[offset - stride - 1];    // Top Left
        }
        if(y >= -1 && y < height - 1) {
            color += corner * pImg[offset + stride - 1];    // Bottom Left
        }
    }

    if(x >= -1 && x < width - 1) {
        if(y >= 0 && y < height) {
            color += border * pImg[offset + 1];             // Right
        }
        if(y > 0 && y <= height) {
            color += corner * pImg[offset - stride + 1];    // Top Right
        }
        if(y >= -1 && y < height - 1) {
            color += corner * pImg[offset + stride + 1];    // Bottom Right
        }
    }

    if(x >= 0 && x < width) {
        if(y >= 0 && y < height) {
            color += center * pImg[offset];                 // Center
        }
        if(y > 0 && y <= height) {
            color += border * pImg[offset - stride];        // Top
        }
        if(y >= -1 && y < height - 1) {
            color += border * pImg[offset + stride];        // Bottom
        }
    }
    color /= divisor;

    return uint8_t(color);
}

//---------------------------------
static inline uint8_t
GetAAColor(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *pImg, uint32_t stride, int32_t center, int32_t border, int32_t corner) {
    uint32_t  offset;
    uint32_t  color;
    int32_t   divisor;

    offset  = x + y * stride;

    divisor = center + border * 4 + corner * 4;

    color = 0;
    color += corner * pImg[offset - stride - 1];    // Top Left
    color += border * pImg[offset - stride    ];    // Top
    color += corner * pImg[offset - stride + 1];    // Top Right

    color += border * pImg[offset - 1];             // Left
    color += border * pImg[offset + 1];             // Right
    color += center * pImg[offset    ];             // Center

    color += corner * pImg[offset + stride - 1];    // Bottom Left
    color += border * pImg[offset + stride    ];    // Bottom
    color += corner * pImg[offset + stride + 1];    // Bottom Right
    color /= divisor;

    return uint8_t(color);
}

//-------------------------------------
void
Font::AABlock(uint8_t *src, uint32_t width, uint32_t height, uint8_t *dst, uint32_t dstStride) {
    uint32_t x, y;
    uint32_t offset;

    offset = (height - 1) * dstStride;
    for (x = 0; x < width; ++x) {
        dst[x] = GetAAColorClip(x, 0, width, height, src, width, mAACenter, mAABorder, mAACorner);
        dst[offset + x] = GetAAColorClip(x, height - 1, width, height, src, width, mAACenter, mAABorder, mAACorner);
    }

    offset = dstStride;
    for (y = 1; y < height - 1; ++y) {
        dst[offset] = GetAAColorClip(0, y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        for (x = 1; x < width - 1; ++x) {
            dst[offset + x] = GetAAColor(x, y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        }
        dst[offset + width - 1] = GetAAColorClip(width - 1, y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        offset += dstStride;
    }
}

//-------------------------------------
void
Font::AABlockEx(uint8_t *src, uint32_t width, uint32_t height, uint8_t *dst, uint32_t dstStride) {
    int32_t x, y;
    int32_t offset, offsetY;

    //memset(src, 255, width*height);
    offset = (height) * dstStride;
    for (y = -1; y < 1; ++y) {
        offsetY = dstStride * (y + 1);
        for (x = -1; x <= int32_t(width); ++x) {
            dst[offsetY +          x + 1] = GetAAColorClip(x,          y, width, height, src, width, mAACenter, mAABorder, mAACorner);
            dst[offsetY + offset + x + 1] = GetAAColorClip(x, height + y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        }
    }

    offset = dstStride * 2;
    for (y = 1; y < int32_t(height - 1); ++y) {
        dst[offset    ] = GetAAColorClip(-1, y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        dst[offset + 1] = GetAAColorClip( 0, y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        for (x = 1; x < int32_t(width - 1); ++x) {
            dst[offset + x + 1] = GetAAColor(x, y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        }
        dst[offset + dstStride - 2] = GetAAColorClip(width - 1, y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        dst[offset + dstStride - 1] = GetAAColorClip(width    , y, width, height, src, width, mAACenter, mAABorder, mAACorner);
        offset += dstStride;
    }
}

//-------------------------------------
const HeightData &
Font::GetDataForHeight(uint8_t height) {
    if(mStatus != EStatus::Ok) {
        return mHeightData[0];
    }

    auto hd = mHeightData.find(height);
    if(hd == mHeightData.end()) {
        const int reference = (mSizeMode == ESizeMode::EmSize) ? mUnitsPerEm : (mAscent - mDescent);
        if(reference <= 0) {
            return mHeightData[0];
        }

        HeightData  heightData;

        heightData.scale   = float(height) / float(reference);
        heightData.ascent  = int(std::ceil(float(mAscent)  * heightData.scale));
        // The descent is negative: floor rounds it outwards so descenders are not clipped by the next line.
        heightData.descent = int(std::floor(float(mDescent) * heightData.scale));
        heightData.lineGap = int(::lround(float(mLineGap) * heightData.scale));

        hd = mHeightData.insert({height, heightData}).first;
    }

    return hd->second;
}
