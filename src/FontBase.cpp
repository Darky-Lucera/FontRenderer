//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontBase.h"
#include "CpuX86.h"
#include "GlyphDraw.h"
#include "UTF8_Utils.h"
//-------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <math.h>      // ::lround, as DJGPP has no std::lround
#include <unordered_set>

using namespace MindShake;

// C++14 needs these definitions whenever the constants are bound to a reference.
constexpr uint32_t FontBase::kMaxTextureSize;

#if defined(FONTRENDERER_X86_64_V2)
//-------------------------------------
namespace {

    // The library is built for x86-64-v2, so an older processor would stop at the first instruction of that level, with
    // an illegal instruction that does not say why. This object stops the program at start instead, with the reason.
    // It is in this file because every program that uses a font links it. The static objects of the program that are
    // built before this one still run first.
    struct X64v2Check {
        X64v2Check() {
            if(CpuX86::HasX64v2() == false) {
                std::fputs("FontRenderer was built for x86-64-v2 (FONTRENDERER_X86_64_V2), which this processor does not have.\n", stderr);
                std::abort();
            }
        }
    };

    const X64v2Check gX64v2Check;

} // end of namespace
#endif

// The metrics file is little-endian, and each record is packed without padding.
//-------------------------------------
namespace {

    constexpr uint8_t  kMetricsMagic[4]      = { 'F', 'R', 'B', 'F' };
    constexpr uint16_t kMetricsVersion       = 1;
    constexpr size_t   kMetricsHeaderSize    = 28;
    constexpr size_t   kHeightRecordSize     = 17;
    constexpr size_t   kGlyphRecordSize      = 42;
    constexpr size_t   kKerningRecordSize    = 12;
    constexpr uint32_t kMaxCodePoint         = 0x10FFFF;

    static_assert(sizeof(float) == sizeof(uint32_t), "The metrics file stores floats as 32 bits");

    //---------------------------------
    class ByteWriter {
        public:
            void    U8(uint8_t value)       { mBytes.push_back(value); }
            void    I32(int32_t value)      { U32(uint32_t(value));    }

            void    U16(uint16_t value) {
                U8(uint8_t(value));
                U8(uint8_t(value >> 8));
            }

            void    U32(uint32_t value) {
                U16(uint16_t(value));
                U16(uint16_t(value >> 16));
            }

            void    F32(float value) {
                uint32_t bits;
                memcpy(&bits, &value, sizeof(bits));
                U32(bits);
            }

            const std::vector<uint8_t> &    GetBytes() const    { return mBytes; }

        protected:
            std::vector<uint8_t>    mBytes;
    };

    // The caller checks the size of the data before reading it.
    //---------------------------------
    class ByteReader {
        public:
            explicit ByteReader(const uint8_t *data) : mData(data) { }

            uint8_t     U8()                { return *mData++;          }
            int32_t     I32()               { return int32_t(U32());    }

            uint16_t    U16() {
                const uint16_t low = U8();
                return uint16_t(low | (uint16_t(U8()) << 8));
            }

            uint32_t    U32() {
                const uint32_t low = U16();
                return low | (uint32_t(U16()) << 16);
            }

            float       F32() {
                const uint32_t bits = U32();
                float          value;
                memcpy(&value, &bits, sizeof(value));
                return value;
            }

        protected:
            const uint8_t   *mData;
    };

    //---------------------------------
    bool
    WriteFile(const char *fileName, const std::vector<uint8_t> &bytes) {
        FILE *file = fopen(fileName, "wb");
        if(file == nullptr) {
            return false;
        }

        const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        return (fclose(file) == 0) && written;
    }

} // end of namespace

//-------------------------------------
FontBase::FontBase(const char *fontName) {
    mFontName = fontName;

    // Trash data
    mHeightData[0]          = {};
    mCodePointHeightData[0] = {};
}

//-------------------------------------
uint32_t
FontBase::GetCodePointHeightKey(uint32_t codePoint, uint8_t height) {
    CodePointHeight cph;
    cph.codePoint = codePoint;
    cph.height    = height;
    return cph.value;
}

//-------------------------------------
uint64_t
FontBase::GetKerningKey(uint32_t leftGlyph, uint32_t rightGlyph) {
    return (uint64_t(leftGlyph) << 32) | uint64_t(rightGlyph);
}

//-------------------------------------
FontBase::EStatus
FontBase::GetFileStatus(MappedFile::EError error) {
    switch(error) {
        case MappedFile::EError::None:
            return EStatus::Ok;

        case MappedFile::EError::CannotOpen:
            return EStatus::CannotOpenFile;

        case MappedFile::EError::CannotRead:
            return EStatus::CannotReadFile;

        case MappedFile::EError::OutOfMemory:
            return EStatus::OutOfMemory;
    }

    return EStatus::CannotReadFile;
}

//-------------------------------------
void
FontBase::PremultiplyTexels(uint8_t *texels, size_t texelCount) {
    for(size_t i = 0; i < texelCount * 4; i += 4) {
        const uint32_t alpha = texels[i + 3];
        texels[i + 0] = uint8_t(GlyphDraw::MulDiv255(texels[i + 0], alpha));
        texels[i + 1] = uint8_t(GlyphDraw::MulDiv255(texels[i + 1], alpha));
        texels[i + 2] = uint8_t(GlyphDraw::MulDiv255(texels[i + 2], alpha));
    }
}

//-------------------------------------
template <class TVisitor>
void
FontBase::LayoutText(const char *utf8, uint8_t textHeight, TVisitor visit) {
    const HeightData &heightData = GetDataForHeight(textHeight);

    float   penX     = 0.0f;
    int32_t baseline = heightData.ascent;

    const uint8_t *text = reinterpret_cast<const uint8_t *>(utf8);
    for(uint32_t codePoint = GetNextUTF32(&text), nextCodePoint; codePoint != 0; codePoint = nextCodePoint) {
        nextCodePoint = GetNextUTF32(&text);
        if(codePoint == '\n') {
            penX      = 0.0f;
            baseline += heightData.GetLineAdvance();
            continue;
        }

        const CodePointHeightData &data = GetCodePointDataForHeight(codePoint, textHeight);
        if(data.glyph > 0) {
            visit(data, penX, baseline);
            penX += data.advanceWidth + GetScaledKerning(data.glyph, nextCodePoint, heightData.scale);
        }
    }
}

//-------------------------------------
float
FontBase::GetScaledKerning(int glyph, uint32_t nextCodePoint, float scale) {
    if(nextCodePoint == 0) {
        return 0.0f;
    }

    return float(GetKerning(uint32_t(glyph), GetCodePointGlyph(nextCodePoint))) * scale;
}

//-------------------------------------
void
FontBase::DrawText(const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY) {
    if(utf8 == nullptr || textHeight == 0 || mTexture.empty()) {
        return;
    }

    const uint32_t                     premultiplied = GlyphDraw::PremultiplyColor(color);
    const uint32_t                     colorAlpha    = color >> 24;
    const GlyphDraw::DrawGlyphFunction drawGlyph     = GlyphDraw::GetDrawGlyphFunction(GetBytesPerTexel(mTextureFormat), colorAlpha == 255);

    LayoutText(utf8, textHeight, [&](const CodePointHeightData &data, float penX, int32_t baseline) {
        // Clip Top
        int32_t currentY = posY + baseline + data.y;
        int32_t minY     = 0;
        if(currentY < mTop) {
            minY    += mTop - currentY;
            currentY = mTop;
        }

        // Clip Bottom (if the beginning is beyond the bottom limit)
        if(currentY >= mBottom) {
            return;
        }

        // Clip Left
        int32_t currentX = posX + data.x + int32_t(::lround(penX));
        int32_t minX     = 0;
        if(currentX < mLeft) {
            minX    += mLeft - currentX;
            currentX = mLeft;
        }

        // Clip Right (if the beginning is beyond the right limit)
        if(currentX >= mRight) {
            return;
        }

        // Clip Right
        int32_t maxX = data.GetWidth();
        if(currentX + maxX - minX >= mRight) {
            maxX = minX + mRight - currentX;
        }

        // Clip Bottom
        int32_t maxY = data.GetHeight();
        if(currentY + maxY - minY >= mBottom) {
            maxY = minY + mBottom - currentY;
        }

        // Let's draw
        const size_t textureWidth  = mTextureWidth;
        const size_t stepX         = data.rotated ? textureWidth : 1;
        const size_t stepY         = data.rotated ? 1 : textureWidth;
        const size_t offsetTexture = size_t(data.rect.y) * textureWidth + size_t(data.rect.x) + size_t(minY) * stepY + size_t(minX) * stepX;
        uint32_t     *dstGlyph     = &dst[size_t(currentY) * dstStride + size_t(currentX)];
        drawGlyph(mTexture.data(), offsetTexture, stepX, stepY, maxX - minX, maxY - minY, dstGlyph, dstStride, premultiplied, colorAlpha);
    });
}

//-------------------------------------
void
FontBase::GetTextBox(const char *utf8, uint8_t textHeight, Rect *pRect) {
    if(pRect == nullptr) {
        return;
    }

    *pRect = {};
    if(utf8 == nullptr || textHeight == 0) {
        return;
    }

    int32_t minX = INT32_MAX, maxX = INT32_MIN;
    int32_t minY = INT32_MAX, maxY = INT32_MIN;

    LayoutText(utf8, textHeight, [&](const CodePointHeightData &data, float penX, int32_t baseline) {
        const int32_t roundedPenX = int32_t(::lround(penX));
        if(data.GetWidth() > 0) {
            const int32_t left = roundedPenX + data.x;
            const int32_t top  = baseline + data.y;

            minX = std::min(minX, left);
            maxX = std::max(maxX, left + data.GetWidth());
            minY = std::min(minY, top);
            maxY = std::max(maxY, top + data.GetHeight());
        }
        else {
            minX = std::min(minX, roundedPenX);
        }
        // The advance counts too, so trailing spaces widen the box.
        maxX = std::max(maxX, int32_t(::lround(penX + data.advanceWidth)));
    });

    if(minX > maxX || minY > maxY) {
        return;
    }

    *pRect = { minX, minY, maxX - minX, maxY - minY };
}

//-------------------------------------
void
FontBase::GetGlyphQuads(const char *utf8, uint8_t textHeight, std::vector<GlyphQuad> &quads) {
    quads.clear();
    if(utf8 == nullptr || textHeight == 0) {
        return;
    }

    LayoutText(utf8, textHeight, [&](const CodePointHeightData &data, float penX, int32_t baseline) {
        if(data.GetWidth() <= 0) {
            return;
        }

        GlyphQuad quad;
        quad.x           = int32_t(::lround(penX)) + data.x;
        quad.y           = baseline + data.y;
        quad.width       = data.GetWidth();
        quad.height      = data.GetHeight();
        quad.textureRect = data.rect;
        quad.rotated     = data.rotated;
        quads.push_back(quad);
    });
}

//-------------------------------------
FontBase::EStatus
FontBase::SaveMetrics(const char *fileName, uint32_t textureWidth, uint32_t textureHeight) const {
    std::vector<uint32_t> heights;
    for(const auto &entry : mHeightData) {
        if(entry.first != 0) {
            heights.push_back(entry.first);
        }
    }
    std::sort(heights.begin(), heights.end());

    // A glyph rendered at a height without metrics, as with a font whose metrics give a zero scale, cannot be drawn.
    struct Glyph {
        uint32_t                    codePoint;
        uint8_t                     height;
        const CodePointHeightData   *data;
    };
    std::vector<Glyph>           glyphs;
    std::unordered_set<uint32_t> glyphIndices;
    for(const auto &entry : mCodePointHeightData) {
        CodePointHeight cph;
        cph.value = entry.first;
        if(entry.second.glyph > 0 && mHeightData.count(cph.height) != 0) {
            glyphs.push_back({ uint32_t(cph.codePoint), uint8_t(cph.height), &entry.second });
            glyphIndices.insert(uint32_t(entry.second.glyph));
        }
    }
    std::sort(glyphs.begin(), glyphs.end(), [](const Glyph &a, const Glyph &b) {
        return (a.codePoint != b.codePoint) ? a.codePoint < b.codePoint : a.height < b.height;
    });

    std::vector<std::pair<uint64_t, int32_t>> kerning;
    for(const auto &entry : mKerningData) {
        const uint32_t left  = uint32_t(entry.first >> 32);
        const uint32_t right = uint32_t(entry.first);
        if(entry.second != 0 && glyphIndices.count(left) != 0 && glyphIndices.count(right) != 0) {
            kerning.push_back(entry);
        }
    }
    std::sort(kerning.begin(), kerning.end());

    ByteWriter writer;
    for(uint8_t byte : kMetricsMagic) {
        writer.U8(byte);
    }
    writer.U16(kMetricsVersion);
    writer.U16(0);
    writer.U32(textureWidth);
    writer.U32(textureHeight);
    writer.U32(uint32_t(heights.size()));
    writer.U32(uint32_t(glyphs.size()));
    writer.U32(uint32_t(kerning.size()));

    for(uint32_t height : heights) {
        const HeightData &data = mHeightData.at(height);
        writer.U8(uint8_t(height));
        writer.F32(data.scale);
        writer.I32(data.ascent);
        writer.I32(data.descent);
        writer.I32(data.lineGap);
    }

    for(const Glyph &glyph : glyphs) {
        const CodePointHeightData &data = *glyph.data;
        writer.U32(glyph.codePoint);
        writer.U8(glyph.height);
        writer.I32(data.glyph);
        writer.F32(data.advanceWidth);
        writer.I32(data.leftSideBearing);
        writer.I32(data.x);
        writer.I32(data.y);
        writer.I32(data.rect.x);
        writer.I32(data.rect.y);
        writer.I32(data.rect.width);
        writer.I32(data.rect.height);
        writer.U8(data.rotated ? 1 : 0);
    }

    for(const auto &entry : kerning) {
        writer.U32(uint32_t(entry.first >> 32));
        writer.U32(uint32_t(entry.first));
        writer.I32(entry.second);
    }

    return WriteFile(fileName, writer.GetBytes()) ? EStatus::Ok : EStatus::CannotWriteFile;
}

//-------------------------------------
FontBase::EStatus
FontBase::LoadMetrics(const char *fileName) {
    MappedFile     file;
    const EStatus  status = GetFileStatus(file.Open(fileName));
    if(status != EStatus::Ok) {
        return status;
    }

    const uint8_t *data = file.GetData();
    const size_t  size  = file.GetSize();
    if(size < kMetricsHeaderSize || memcmp(data, kMetricsMagic, sizeof(kMetricsMagic)) != 0) {
        return EStatus::InvalidFont;
    }

    ByteReader reader(data + sizeof(kMetricsMagic));
    const uint16_t version       = reader.U16();
    reader.U16();
    const uint32_t textureWidth  = reader.U32();
    const uint32_t textureHeight = reader.U32();
    const uint32_t heightCount   = reader.U32();
    const uint32_t glyphCount    = reader.U32();
    const uint32_t kerningCount  = reader.U32();

    const uint64_t expectedSize = kMetricsHeaderSize + uint64_t(heightCount) * kHeightRecordSize +
                                  uint64_t(glyphCount) * kGlyphRecordSize + uint64_t(kerningCount) * kKerningRecordSize;
    if(version != kMetricsVersion || expectedSize != size ||
       textureWidth == 0 || textureWidth > kMaxTextureSize || textureHeight == 0 || textureHeight > kMaxTextureSize) {
        return EStatus::InvalidFont;
    }

    for(uint32_t i = 0; i < heightCount; ++i) {
        const uint8_t height = reader.U8();
        HeightData    heightData;
        heightData.scale   = reader.F32();
        heightData.ascent  = reader.I32();
        heightData.descent = reader.I32();
        heightData.lineGap = reader.I32();
        if(height == 0 || std::isfinite(heightData.scale) == false || heightData.scale <= 0.0f ||
           mHeightData.insert({ height, heightData }).second == false) {
            return EStatus::InvalidFont;
        }
    }

    for(uint32_t i = 0; i < glyphCount; ++i) {
        const uint32_t      codePoint = reader.U32();
        const uint8_t       height    = reader.U8();
        CodePointHeightData glyph;
        glyph.glyph           = reader.I32();
        glyph.advanceWidth    = reader.F32();
        glyph.leftSideBearing = reader.I32();
        glyph.x               = reader.I32();
        glyph.y               = reader.I32();
        glyph.rect.x          = reader.I32();
        glyph.rect.y          = reader.I32();
        glyph.rect.width      = reader.I32();
        glyph.rect.height     = reader.I32();
        const uint8_t rotated = reader.U8();
        glyph.rotated         = rotated != 0;

        const Rect &rect  = glyph.rect;
        const bool  empty = rect.width == 0 && rect.height == 0;
        const bool  fits  = rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
                            uint64_t(rect.x) + uint64_t(rect.width) <= textureWidth && uint64_t(rect.y) + uint64_t(rect.height) <= textureHeight;
        if(codePoint == 0 || codePoint > kMaxCodePoint || mHeightData.count(height) == 0 || glyph.glyph <= 0 ||
           std::isfinite(glyph.advanceWidth) == false || rotated > 1 || (empty == false && fits == false) ||
           mCodePointHeightData.insert({ GetCodePointHeightKey(codePoint, height), glyph }).second == false) {
            return EStatus::InvalidFont;
        }
    }

    for(uint32_t i = 0; i < kerningCount; ++i) {
        const uint32_t left    = reader.U32();
        const uint32_t right   = reader.U32();
        const int32_t  kerning = reader.I32();
        if(mKerningData.insert({ GetKerningKey(left, right), kerning }).second == false) {
            return EStatus::InvalidFont;
        }
    }

    mTextureWidth  = textureWidth;
    mTextureHeight = textureHeight;

    return EStatus::Ok;
}
