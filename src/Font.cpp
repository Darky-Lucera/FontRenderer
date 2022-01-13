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
#include <cmath>
#include <cstring>

using namespace MindShake;

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

    memset(mTexture, 0, mPacker.GetWidth() * mPacker.GetHeight());
}

//-------------------------------------
bool
Font::InitPacker() {
    mPacker.Init(512, 128, false);
    mTexture = (uint8_t *) calloc(mPacker.GetWidth() * mPacker.GetHeight(), 1);
    if (mTexture == nullptr) {
        fprintf(stderr, "Not enough memory\n");
        mStatus = -5;
        return false;
    }

    return true;
}

//-------------------------------------
void
Font::DrawText(const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY) {
    if(utf8 == nullptr || textHeight == 0)
        return;

    uint32_t codePoint;
    uint32_t offsetDst, offsetTexture;
    uint32_t offsetTextX, offsetTextY;
    int32_t  currentX, currentY;
    int32_t  minX, maxX, minY, maxY;

    Color32 fontColor = *reinterpret_cast<Color32 *>(&color);

    const HeightData &heightData = GetDataForHeight(textHeight);

    posY += heightData.ascent; // baseline

    offsetTextX = 0;
    offsetTextY = 0;
    while((codePoint = GetNextUTF32(reinterpret_cast<const uint8_t **>(&utf8))) != 0) {
        if(codePoint == '\n') {
            offsetTextX = 0;
            offsetTextY += (heightData.ascent - heightData.descent);
            continue;
        }

        const CodePointHeightData &data = GetCodePointDataForHeight(codePoint, textHeight);
        if(data.glyph > 0) {
            // Clip Top
            currentY = posY + data.y + offsetTextY;
            minY = data.rect.top();
            if(currentY < mTop) {
                minY    += mTop - currentY;
                currentY = mTop;
            }

            // Clip Bottom (if the beginning is beyond the bottom limit)
            if(currentY < mBottom) {
                // Clip Left
                currentX = posX + data.x + offsetTextX;
                minX = data.rect.left();
                if(currentX < mLeft) {
                    minX    += mLeft - currentX;
                    currentX = mLeft;
                }

                // Clip Right (if the beginning is beyond the right limit)
                if(currentX < mRight) {
                    // Clip Right
                    maxX = data.rect.right();
                    if(currentX + maxX - minX >= mRight) {
                        maxX = minX + mRight - currentX;
                    }

                    // Clip Bottom
                    maxY = data.rect.bottom();
                    if(currentY + maxY - minY >= mBottom) {
                        maxY = minY + mBottom - currentY;
                    }

                    // Let's draw
                    offsetTexture = minY * mPacker.GetWidth();
                    offsetDst     = currentY * dstStride + currentX;
                    for(int texY=minY; texY<maxY; ++texY) {
                        for(int texX=minX, dstX=0; texX<maxX; ++texX, ++dstX) {
                            if(mTexture[offsetTexture + texX] != 0) {
                                uint32_t grey    = uint32_t((mTexture[offsetTexture + texX] * fontColor.a) / 255);
                                uint32_t invGrey = 255 - grey;

                                Color32  &dstColor = *reinterpret_cast<Color32 *>(&dst[offsetDst + dstX]);
                                dstColor.b = ((fontColor.b * grey) + (dstColor.b * invGrey)) / 255;
                                dstColor.g = ((fontColor.g * grey) + (dstColor.g * invGrey)) / 255;
                                dstColor.r = ((fontColor.r * grey) + (dstColor.r * invGrey)) / 255;
                                dstColor.a = 255;
                            }
                        }
                        offsetTexture += mPacker.GetWidth();
                        offsetDst     += dstStride;
                    }
                }
            }
            offsetTextX += data.advanceWidth + uint32_t(GetKerning(data.glyph, GetCodePointGlyph(*utf8)) * heightData.scale);
        }
    }
}

//-------------------------------------
void
Font::GetTextBox(const char *utf8, uint8_t textHeight, Rect *pRect) {
    if(utf8 == nullptr || textHeight == 0)
        return;

    uint32_t codePoint;
    int32_t  offsetTextX, offsetTextY;
    int32_t  currentX, currentY;
    int32_t  right, bottom;
    int32_t  minX, maxX;
    int32_t  minY, maxY;

    const HeightData &heightData = GetDataForHeight(textHeight);

    minX = minY = 0xffff;
    maxX = maxY = 0;
    offsetTextX = 0;
    offsetTextY = 0;
    while((codePoint = GetNextUTF32(reinterpret_cast<const uint8_t **>(&utf8))) != 0) {
        if(codePoint == '\n') {
            offsetTextX = 0;
            offsetTextY += (heightData.ascent - heightData.descent);
            maxY = 0;
            continue;
        }

        const CodePointHeightData &data = GetCodePointDataForHeight(codePoint, textHeight);
        if(data.glyph > 0) {
            currentY = heightData.ascent + data.y + offsetTextY;
            bottom = data.rect.height;
            if(maxY < currentY + bottom)
                maxY = currentY + bottom;
            if(minY > currentY)
                minY = currentY;

            currentX = data.x + offsetTextX;
            right = std::max(data.rect.width, data.advanceWidth);
            if(maxX < currentX + right)
                maxX = currentX + right;
            if(minX > currentX)
                minX = currentX;

            offsetTextX += data.advanceWidth + uint32_t(GetKerning(data.glyph, GetCodePointGlyph(*utf8)) * heightData.scale);
        }
    }

    if(pRect != nullptr) {
        pRect->x      = minX;
        pRect->y      = minY;
        pRect->width  = maxX - 1;
        pRect->height = maxY - 1;
    }
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
    if(mStatus < 0)
        return mHeightData[0];

    auto hd = mHeightData.find(height);
    if(hd == mHeightData.end()) {
        HeightData  heightData;

        heightData.scale   = float(height) / (mAscent - mDescent);
        heightData.ascent  = int(std::ceil(mAscent  * heightData.scale));
        heightData.descent = int(std::ceil(mDescent * heightData.scale));
        heightData.lineGap = int(std::ceil(mLineGap * heightData.scale));
        mHeightData[height] = heightData;

        hd = mHeightData.insert({height, heightData}).first;
    }

    return hd->second;
}
