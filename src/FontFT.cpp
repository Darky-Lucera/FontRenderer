//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontFT.h"
//-------------------------------------
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_PARAMETER_TAGS_H
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <math.h>      // ::lround, as DJGPP has no std::lround

using namespace MindShake;

//-------------------------------------
namespace {

    //---------------------------------
    FT_Int32
    GetLoadFlags(FontFT::EHinting hinting, bool monochrome) {
        // Bitmaps stored in the font only exist for some sizes, and FontRenderer scales to any size.
        FT_Int32       flags  = FT_LOAD_NO_BITMAP;
        const FT_Int32 target = monochrome ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL;
        switch (hinting) {
            case FontFT::EHinting::None:   flags |= FT_LOAD_NO_HINTING;              break;
            case FontFT::EHinting::Light:  flags |= FT_LOAD_TARGET_LIGHT;            break;
            case FontFT::EHinting::Normal: flags |= target;                          break;
            case FontFT::EHinting::Auto:   flags |= target | FT_LOAD_FORCE_AUTOHINT; break;
        }
        return flags;
    }

    //---------------------------------
    void
    DoneLibrary(FT_Library library) {
        FT_Done_FreeType(library);
    }

    //---------------------------------
    void
    DoneFace(FT_Face face) {
        FT_Done_Face(face);
    }

    // FreeType opens the first font of a collection. A collection starts with 'ttcf', and the offset of its first font is at byte 12.
    //---------------------------------
    size_t
    GetFirstFontOffset(const uint8_t *data, size_t size) {
        if (size < 16 || memcmp(data, "ttcf", 4) != 0) {
            return 0;
        }
        return (size_t(data[12]) << 24) | (size_t(data[13]) << 16) | (size_t(data[14]) << 8) | size_t(data[15]);
    }

} // end of namespace

//-------------------------------------
FontFT::FontFT(const char *fontName) : Font(fontName) {
    if (LoadFile(fontName) == false) {
        return;
    }

    if (mFontFile.GetSize() > size_t(std::numeric_limits<FT_Long>::max())) {
        mStatus = EStatus::InvalidFont;
        return;
    }

    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        mStatus = EStatus::OutOfMemory;
        return;
    }
    mLibrary = decltype(mLibrary)(library, DoneLibrary);

    FT_Face        face  = nullptr;
    const FT_Error error = FT_New_Memory_Face(library, mFontFile.GetData(), FT_Long(mFontFile.GetSize()), 0, &face);
    if (error != 0) {
        mStatus = (error == FT_Err_Out_Of_Memory) ? EStatus::OutOfMemory : EStatus::InvalidFont;
        return;
    }
    mFace = decltype(mFace)(face, DoneFace);

    // A font of bitmaps only has the sizes it stores.
    if (FT_IS_SCALABLE(face) == false) {
        mStatus = EStatus::InvalidFont;
        return;
    }

    if (InitPacker() == false) {
        return;
    }

    mUnitsPerEm = face->units_per_EM;
    mAscent     = face->ascender;
    mDescent    = face->descender;
    mLineGap    = face->height - (face->ascender - face->descender);

    mGposKerning.Load(mFontFile.GetData(), mFontFile.GetSize(), GetFirstFontOffset(mFontFile.GetData(), mFontFile.GetSize()));

    mStatus = EStatus::Ok;
}

//-------------------------------------
void
FontFT::SetHinting(EHinting hinting) {
    if (hinting == mHinting) {
        return;
    }

    mHinting = hinting;
    Reset();
}

//-------------------------------------
void
FontFT::SetMonochrome(bool set) {
    if (set == mMonochrome) {
        return;
    }

    mMonochrome = set;
    Reset();
}

//-------------------------------------
void
FontFT::SetStemDarkening(bool set) {
    if (set == mStemDarkening || mFace == nullptr) {
        return;
    }

    FT_Bool      value = set;
    FT_Parameter parameter;
    parameter.tag  = FT_PARAM_TAG_STEM_DARKENING;
    parameter.data = &value;
    if (FT_Face_Properties(mFace.get(), 1, &parameter) != 0) {
        return;
    }

    mStemDarkening = set;
    Reset();
}

//-------------------------------------
const CodePointData &
FontFT::GetCodePointData(uint32_t index) {
    if (mStatus != EStatus::Ok) {
        return mCodePointData[0];
    }

    auto cpd = mCodePointData.find(index);
    if (cpd == mCodePointData.end()) {
        FT_Face       face  = mFace.get();
        const FT_UInt glyph = FT_Get_Char_Index(face, index);
        if (glyph == 0 || FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE) != 0) {
            cpd = mCodePointData.find(0);   // Trash
            cpd->second = {};
        }
        else {
            CodePointData codePoint;
            codePoint.glyph           = int(glyph);
            codePoint.advanceWidth    = int(face->glyph->metrics.horiAdvance);
            codePoint.leftSideBearing = int(face->glyph->metrics.horiBearingX);

            cpd = mCodePointData.insert({index, codePoint}).first;
        }
    }

    return cpd->second;
}

//-------------------------------------
const CodePointHeightData &
FontFT::GetCodePointDataForHeight(uint32_t index, uint8_t height) {
    if (mStatus != EStatus::Ok) {
        return mCodePointHeightData[0];
    }

    CodePointHeight cph;
    cph.codePoint = index;
    cph.height    = height;

    auto cphd = mCodePointHeightData.find(cph.value);
    if (cphd == mCodePointHeightData.end()) {
        const CodePointData &codePoint = GetCodePointData(index);
        if (codePoint.glyph == 0) {
            return mCodePointHeightData[0];
        }

        // The scale goes to FreeType as it is, in 16.16 fixed point, and it gives pixels in 26.6 fixed point.
        const float        scale   = GetScaleForHeight(height);
        FT_Size_RequestRec request {};
        request.type   = FT_SIZE_REQUEST_TYPE_SCALES;
        request.width  = FT_Long(::lround(double(scale) * 64.0 * 65536.0));
        request.height = request.width;

        FT_Face face = mFace.get();
        const FT_Render_Mode renderMode = mMonochrome ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
        if (FT_Request_Size(face, &request) != 0 || FT_Load_Glyph(face, FT_UInt(codePoint.glyph), GetLoadFlags(mHinting, mMonochrome)) != 0 ||
            FT_Render_Glyph(face->glyph, renderMode) != 0) {
            return mCodePointHeightData[0];
        }

        const FT_GlyphSlot slot   = face->glyph;
        const FT_Bitmap    &bitmap = slot->bitmap;

        CodePointHeightData codePointHeight;
        codePointHeight.glyph           = codePoint.glyph;
        codePointHeight.x               = slot->bitmap_left;
        codePointHeight.y               = -slot->bitmap_top;
        codePointHeight.leftSideBearing = int(floor(codePoint.leftSideBearing * scale));
        // Normal and Auto hinting fit each glyph to an advance of whole pixels, so the text must use that advance.
        // Light hinting leaves the fractional advance of the outline, as FreeType recommends.
        const bool roundsAdvances       = (mHinting == EHinting::Normal || mHinting == EHinting::Auto);
        codePointHeight.advanceWidth    = roundsAdvances ? float(slot->advance.x) / 64.0f : float(codePoint.advanceWidth) * scale;

        int w = int(bitmap.width);
        int h = int(bitmap.rows);
        if (w > 0 && h > 0) {
            // A FreeType bitmap can pad its rows, and a negative pitch stores them from the bottom up.
            // A monochrome bitmap has one bit per pixel, the leftmost pixel in the highest bit.
            auto         pixels = std::make_unique<uint8_t[]>(size_t(w) * size_t(h));
            const size_t pitch  = size_t(std::abs(bitmap.pitch));
            for (int y = 0; y < h; ++y) {
                const uint8_t *source = bitmap.buffer + size_t((bitmap.pitch >= 0) ? y : h - 1 - y) * pitch;
                uint8_t       *target = &pixels[size_t(y) * size_t(w)];
                if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                    for (int x = 0; x < w; ++x) {
                        target[x] = (source[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
                    }
                }
                else {
                    memcpy(target, source, size_t(w));
                }
            }

            const int grown = ApplyAntialias(pixels, w, h);
            codePointHeight.x -= grown;
            codePointHeight.y -= grown;
            PackGlyph(pixels.get(), uint32_t(w), uint32_t(h), codePointHeight);
        }

        cphd = mCodePointHeightData.insert({cph.value, codePointHeight}).first;
    }

    return cphd->second;
}

//-------------------------------------
int
FontFT::GetKernTableKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
    FT_Vector kerning {};
    if (FT_Get_Kerning(mFace.get(), leftGlyph, rightGlyph, FT_KERNING_UNSCALED, &kerning) != 0) {
        return 0;
    }
    return int(kerning.x);
}
