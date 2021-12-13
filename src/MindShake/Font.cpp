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

// TODO: Think where put these funcs...
//---------------------------------
static inline uint8_t
GetAAColorClip(uint32_t _x, uint32_t _y, uint32_t _width, uint32_t _height, const uint8_t *_pImg, uint32_t _stride, int32_t _multiplier) {
    uint32_t  offset;
    uint32_t  color;
    int32_t   multiplier;
    int32_t   divisor;

    offset = _x + _y * _stride;

    multiplier = _multiplier;
    divisor    = _multiplier + 4*4 + 1*4;

    color = 0;
    if(_x > 0) {
        color += 4 * _pImg[offset-1];

        if(_y > 0) {
            color += _pImg[offset-_stride - 1];
        }

        if(_y < _height-2) {
            color += _pImg[offset+_stride - 1];
        }
    }
//    else
//        multiplier += 4;

    if(_x < _width - 2) {
        color += 4 * _pImg[offset+1];

        if(_y > 0) {
            color += _pImg[offset-_stride + 1];
        }

        if(_y < _height-2) {
            color += _pImg[offset+_stride + 1];
        }
    }
//    else
//        multiplier += 4;

    if(_y > 0) {
        color += 4 * _pImg[offset-_stride];
    }
//    else
//        multiplier += 2;

    if(_y < _height-2) {
        color += 4 * _pImg[offset+_stride];
    }
//    else
//        multiplier += 2;

    color += multiplier * _pImg[offset];
    color /= divisor;

    return uint8_t(color);
}

//---------------------------------
static inline uint8_t
GetAAColor(uint32_t _x, uint32_t _y, uint32_t _width, uint32_t _height, const uint8_t *_pImg, uint32_t _stride, int32_t _multiplier) {
    uint32_t  offset;
    uint32_t  color;
    int32_t   multiplier;
    int32_t   divisor;

    offset = _x + _y * _stride;

    multiplier = _multiplier;
    divisor    = _multiplier + 4*4 + 1*4;

    color = 0;
    color += 4 * _pImg[offset - 1];
    color +=     _pImg[offset - _stride - 1];
    color +=     _pImg[offset + _stride - 1];
    color += 4 * _pImg[offset + 1];
    color +=     _pImg[offset - _stride + 1];
    color +=     _pImg[offset + _stride + 1];
    color += 4 * _pImg[offset - _stride];
    color += 4 * _pImg[offset + _stride];
    color += multiplier * _pImg[offset];
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
        dst[x] = GetAAColorClip(x, 0, width, height, src, width, mAntiAliasWeight);
        dst[offset + x] = GetAAColorClip(x, height - 1, width, height, src, width, mAntiAliasWeight);
    }
                
    offset = dstStride;
    for (y = 1; y < height - 1; ++y) {
        dst[offset] = GetAAColorClip(0, y, width, height, src, width, mAntiAliasWeight);
        for (x = 1; x < width - 1; ++x) {
            dst[offset + x] = GetAAColor(x, y, width, height, src, width, mAntiAliasWeight);
        }
        dst[offset + width - 1] = GetAAColorClip(width - 1, y, width, height, src, width, mAntiAliasWeight);
        offset += dstStride;
    }
}
