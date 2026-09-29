//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Font.h"
#include "Tga.h"
#include "UTF8_Utils.h"
//-------------------------------------
#include <algorithm>
#include <cassert>
#include <cmath>
#include <math.h>      // ::lround, as DJGPP has no std::lround
#include <cstring>
#include <new>
#include <unordered_set>

using namespace MindShake;

// C++14 needs these definitions whenever the constants are bound to a reference.
constexpr int32_t Font::kMaxAntialiasWeight;

//-------------------------------------
Font::Font(const char *fontName) : FontBase(fontName) {
    // Trash data
    mCodePointData[0] = {};
}

//-------------------------------------
void
Font::Reset() {
    mPacker.Reset();

    mCodePointHeightData.clear();
    mCodePointHeightData[0] = {};

    std::fill(mTexture.begin(), mTexture.end(), uint8_t(0));
    ++mTextureVersion;
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
    const EStatus status = GetFileStatus(mFontFile.Open(fileName));
    if(status != EStatus::Ok) {
        mStatus = status;
        return false;
    }

    return true;
}

//-------------------------------------
bool
Font::InitPacker() {
    constexpr uint32_t kWidth  = 512;
    constexpr uint32_t kHeight = 128;

    mPacker.Init(kWidth - mGlyphSpacing, kHeight - mGlyphSpacing, mPacker.GetAllowRotation());
    try {
        mTexture.assign(size_t(kWidth) * kHeight, 0);
    }
    catch(const std::bad_alloc &) {
        mStatus = EStatus::OutOfMemory;
        return false;
    }

    mTextureWidth  = kWidth;
    mTextureHeight = kHeight;

    return true;
}

//-------------------------------------
uint32_t
Font::GetUsedTextureWidth() const {
    const uint32_t used = mPacker.GetUsedWidth();
    return (used > 0) ? used + mGlyphSpacing : 0;
}

//-------------------------------------
uint32_t
Font::GetUsedTextureHeight() const {
    const uint32_t used = mPacker.GetUsedHeight();
    return (used > 0) ? used + mGlyphSpacing : 0;
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
Font::SetGlyphSpacing(uint32_t spacing) {
    if(spacing == mGlyphSpacing) {
        return true;
    }

    if(spacing >= mTextureWidth || spacing >= mTextureHeight) {
        return false;
    }

    mGlyphSpacing = spacing;
    mPacker.Init(mTextureWidth - spacing, mTextureHeight - spacing, mPacker.GetAllowRotation());
    Reset();

    return true;
}

//-------------------------------------
bool
Font::SetGlyphPadding(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    if(left == mGlyphPaddingLeft && top == mGlyphPaddingTop && right == mGlyphPaddingRight && bottom == mGlyphPaddingBottom) {
        return true;
    }

    if(uint64_t(left) + right >= kMaxTextureSize || uint64_t(top) + bottom >= kMaxTextureSize) {
        return false;
    }

    mGlyphPaddingLeft   = left;
    mGlyphPaddingTop    = top;
    mGlyphPaddingRight  = right;
    mGlyphPaddingBottom = bottom;
    Reset();

    return true;
}

//-------------------------------------
bool
Font::CanEverFit(uint32_t spacedWidth, uint32_t spacedHeight) const {
    const bool canGrowWidth  = mTextureGrowth != ETextureGrowth::Height;
    const bool canGrowHeight = mTextureGrowth != ETextureGrowth::Width;
    auto fits = [&](uint32_t w, uint32_t h) {
        return (canGrowWidth || w <= mPacker.GetWidth()) && (canGrowHeight || h <= mPacker.GetHeight());
    };

    return fits(spacedWidth, spacedHeight) || (mPacker.GetAllowRotation() && fits(spacedHeight, spacedWidth));
}

//-------------------------------------
bool
Font::PackGlyph(const uint8_t *pixels, uint32_t width, uint32_t height, CodePointHeightData &data) {
    data.rect    = {};
    data.rotated = false;

    // Fail early instead of growing the texture up to the limit for nothing.
    const uint32_t spacedWidth  = width  + mGlyphSpacing;
    const uint32_t spacedHeight = height + mGlyphSpacing;
    if(CanEverFit(spacedWidth, spacedHeight) == false) {
        return false;
    }

    Rect rect = mPacker.Insert(spacedWidth, spacedHeight, mPackingHeuristic);
    while(rect.width <= 0) {
        if(GrowTexture() == false) {
            return false;
        }

        rect = mPacker.Insert(spacedWidth, spacedHeight, mPackingHeuristic);
    }

    CopyGlyph(pixels, width, height, rect, data);

    return true;
}

//-------------------------------------
void
Font::CopyGlyph(const uint8_t *pixels, uint32_t width, uint32_t height, const Rect &packed, CodePointHeightData &data) {
    Rect &rect = data.rect;
    rect         = packed;
    data.rotated = uint32_t(rect.width) != width + mGlyphSpacing;

    // The spacing reserved at the right and bottom of the glyph stays empty. Shifting by the spacing moves
    // the glyph past the empty band along the top and left texture edges, so it has empty pixels on every side.
    rect.x      += int32_t(mGlyphSpacing);
    rect.y      += int32_t(mGlyphSpacing);
    rect.width  -= int32_t(mGlyphSpacing);
    rect.height -= int32_t(mGlyphSpacing);

    const size_t textureWidth = mTextureWidth;
    const size_t stepX        = data.rotated ? textureWidth : 1;
    const size_t stepY        = data.rotated ? 1 : textureWidth;
    uint8_t      *row         = &mTexture[size_t(rect.y) * textureWidth + size_t(rect.x)];
    for(uint32_t y = 0; y < height; ++y, row += stepY) {
        uint8_t *dst = row;
        for(uint32_t x = 0; x < width; ++x, dst += stepX)
            *dst = *pixels++;
    }

    ++mTextureVersion;
}

//-------------------------------------
bool
Font::GrowTexture() {
    const uint32_t oldWidth  = mTextureWidth;
    const uint32_t oldHeight = mTextureHeight;
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

    mTextureWidth  = newWidth;
    mTextureHeight = newHeight;
    ++mTextureVersion;

    // Cannot fail: the new size is bigger and within kMaxTextureSize.
    const bool resized = mPacker.ResizeBin(newWidth - mGlyphSpacing, newHeight - mGlyphSpacing);
    assert(resized);
    (void) resized;

    return true;
}

//-------------------------------------
int
Font::GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    const uint64_t key = GetKerningKey(leftGlyph, rightGlyph);

    auto it = mKerningData.find(key);
    if (it == mKerningData.end()) {
        it = mKerningData.insert({ key, LookUpKerning(leftGlyph, rightGlyph) }).first;
    }

    return it->second;
}

//-------------------------------------
int
Font::LookUpKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    // GPOS wins over the 'kern' table when it has kerning, as in HarfBuzz. FreeType prefers 'kern', but fonts keep it
    // for old software, and it often has only part of the pairs, because it cannot store classes of glyphs.
    // stb_truetype also prefers GPOS, but whenever the font has the table, even without kerning in it.
    return mGposKerning.HasKerning() ? mGposKerning.GetKerning(leftGlyph, rightGlyph) : GetKernTableKerning(leftGlyph, rightGlyph);
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

//-------------------------------------
const CodePointHeightData &
Font::GetCodePointDataForHeight(uint32_t codePoint, uint8_t height) {
    if(mStatus != EStatus::Ok) {
        return mCodePointHeightData[0];
    }

    const uint32_t key = GetCodePointHeightKey(codePoint, height);

    auto cphd = mCodePointHeightData.find(key);
    if(cphd == mCodePointHeightData.end()) {
        CodePointHeightData data;
        GlyphBitmap         bitmap;
        if(RenderGlyph(codePoint, height, data, bitmap) == false) {
            return mCodePointHeightData[0];
        }

        if(bitmap.pixels != nullptr) {
            PackGlyph(bitmap.pixels.get(), uint32_t(bitmap.width), uint32_t(bitmap.height), data);
        }

        cphd = mCodePointHeightData.insert({key, data}).first;
    }

    return cphd->second;
}

//-------------------------------------
bool
Font::RenderGlyph(uint32_t codePoint, uint8_t height, CodePointHeightData &data, GlyphBitmap &bitmap) {
    const CodePointData &codePointData = GetCodePointData(codePoint);
    if(codePointData.glyph == 0) {
        return false;
    }

    if(RasterizeGlyph(codePointData, height, data, bitmap) == false) {
        return false;
    }

    data.glyph = codePointData.glyph;
    if(bitmap.pixels != nullptr) {
        const int grown = ApplyAntialias(bitmap.pixels, bitmap.width, bitmap.height);
        data.x -= grown;
        data.y -= grown;
        ApplyPadding(bitmap, data);
    }

    return true;
}

//-------------------------------------
void
Font::ApplyPadding(GlyphBitmap &bitmap, CodePointHeightData &data) {
    const int left   = int(mGlyphPaddingLeft);
    const int top    = int(mGlyphPaddingTop);
    const int width  = bitmap.width  + left + int(mGlyphPaddingRight);
    const int height = bitmap.height + top  + int(mGlyphPaddingBottom);
    if(width == bitmap.width && height == bitmap.height) {
        return;
    }

    auto pixels = std::make_unique<uint8_t[]>(size_t(width) * size_t(height));
    for(int y = 0; y < bitmap.height; ++y) {
        memcpy(&pixels[size_t(y + top) * size_t(width) + size_t(left)], &bitmap.pixels[size_t(y) * size_t(bitmap.width)], size_t(bitmap.width));
    }

    bitmap.pixels = std::move(pixels);
    bitmap.width  = width;
    bitmap.height = height;
    data.x -= left;
    data.y -= top;
}

//-------------------------------------
bool
Font::Preload(const char *utf8, uint8_t textHeight) {
    if(mStatus != EStatus::Ok) {
        return false;
    }

    if(utf8 == nullptr || textHeight == 0) {
        return true;
    }

    struct Pending {
        uint32_t            key;
        CodePointHeightData data;
        GlyphBitmap         bitmap;
    };

    std::vector<Pending>         pending;
    std::unordered_set<uint32_t> seen;
    const uint8_t *text = reinterpret_cast<const uint8_t *>(utf8);
    for(uint32_t codePoint = GetNextUTF32(&text); codePoint != 0; codePoint = GetNextUTF32(&text)) {
        const uint32_t key = GetCodePointHeightKey(codePoint, textHeight);
        if(codePoint == '\n' || mCodePointHeightData.count(key) != 0 || seen.insert(key).second == false) {
            continue;
        }

        Pending glyph {};
        glyph.key = key;
        if(RenderGlyph(codePoint, textHeight, glyph.data, glyph.bitmap) == false) {
            continue;
        }

        if(glyph.bitmap.pixels == nullptr) {
            mCodePointHeightData.insert({ key, glyph.data });
        }
        else {
            pending.push_back(std::move(glyph));
        }
    }

    bool                              allFit = true;
    std::vector<SkylineBinPack::Size> sizes;
    std::vector<Pending *>            packing;
    for(Pending &glyph : pending) {
        const SkylineBinPack::Size size { uint32_t(glyph.bitmap.width) + mGlyphSpacing, uint32_t(glyph.bitmap.height) + mGlyphSpacing };
        if(CanEverFit(size.width, size.height)) {
            sizes.push_back(size);
            packing.push_back(&glyph);
        }
        else {
            allFit = false;
        }
    }

    std::vector<Rect> rects;
    while(sizes.empty() == false) {
        mPacker.Insert(sizes, rects, mPackingHeuristic);

        std::vector<SkylineBinPack::Size> unplacedSizes;
        std::vector<Pending *>            unplaced;
        for(size_t i = 0; i < sizes.size(); ++i) {
            Pending &glyph = *packing[i];
            if(rects[i].width > 0) {
                CopyGlyph(glyph.bitmap.pixels.get(), uint32_t(glyph.bitmap.width), uint32_t(glyph.bitmap.height), rects[i], glyph.data);
            }
            else {
                unplacedSizes.push_back(sizes[i]);
                unplaced.push_back(&glyph);
            }
        }
        sizes.swap(unplacedSizes);
        packing.swap(unplaced);

        if(sizes.empty() == false && GrowTexture() == false) {
            allFit = false;
            break;
        }
    }

    for(const Pending &glyph : pending) {
        mCodePointHeightData.insert({ glyph.key, glyph.data });
    }

    return allFit;
}

//-------------------------------------
void
Font::LoadAllKerningPairs() {
    if(mStatus != EStatus::Ok) {
        return;
    }

    std::unordered_set<uint32_t> glyphs;
    for(const auto &entry : mCodePointHeightData) {
        if(entry.second.glyph > 0) {
            glyphs.insert(uint32_t(entry.second.glyph));
        }
    }

    // Pairs without kerning are not cached: n * n entries would take too much memory.
    for(uint32_t left : glyphs) {
        for(uint32_t right : glyphs) {
            const uint64_t key = GetKerningKey(left, right);
            if(mKerningData.count(key) != 0) {
                continue;
            }

            const int kerning = LookUpKerning(left, right);
            if(kerning != 0) {
                mKerningData.insert({ key, kerning });
            }
        }
    }
}

//-------------------------------------
Font::EStatus
Font::SaveBaked(const char *metricsFile, const char *textureFile) const {
    if(mStatus != EStatus::Ok) {
        return mStatus;
    }

    if(metricsFile == nullptr || textureFile == nullptr) {
        return EStatus::CannotWriteFile;
    }

    // A TGA image cannot be empty, and the texture is when no glyph has pixels.
    const uint32_t width  = std::max<uint32_t>(GetUsedTextureWidth(), 1);
    const uint32_t height = std::max<uint32_t>(GetUsedTextureHeight(), 1);
    if(WriteTga(textureFile, mTexture.data(), width, height, mTextureWidth, 1, true) == false) {
        return EStatus::CannotWriteFile;
    }

    return SaveMetrics(metricsFile, width, height);
}
