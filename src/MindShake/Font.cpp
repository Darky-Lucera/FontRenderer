//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Font.h"
#include "UTF8_Utils.h"

using namespace MindShake;

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

    Color32 &fontColor = *reinterpret_cast<Color32 *>(&color);

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
                                uint32_t grey    = uint32_t(mTexture[offsetTexture + texX]) + 1;
                                uint32_t invGrey = 256 - grey;

                                Color32  &dstColor = *reinterpret_cast<Color32 *>(&dst[offsetDst + dstX]);
                                dstColor.b = ((fontColor.b * grey) + (dstColor.b * invGrey)) >> 8;
                                dstColor.g = ((fontColor.g * grey) + (dstColor.g * invGrey)) >> 8;
                                dstColor.r = ((fontColor.r * grey) + (dstColor.r * invGrey)) >> 8;
                                dstColor.a = ((fontColor.a * grey) + (dstColor.a * invGrey)) >> 8;
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
            right = data.rect.width;
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
