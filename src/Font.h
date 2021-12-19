#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "SkylineBinPack.h"
//-------------------------------------
#include <cstdint>
#include <unordered_map>
#include <string>

//-------------------------------------
namespace MindShake {

    //---------------------------------
    struct HeightData {
        float   scale;
        int     ascent;
        int     descent;
        int     lineGap;
    };

    //---------------------------------
    struct CodePointData {
        int     glyph;
        int     advanceWidth;
        int     leftSideBearing;
    };

    //---------------------------------
    struct CodePointHeightData {
        using Rect = MindShake::SkylineBinPack::Rect;

        int     glyph;             // It's convenient
        int     advanceWidth;
        int     leftSideBearing;
        int     x, y;
        Rect    rect;
    };

    //-------------------------------------
    union Color32 {
        union {
            uint32_t    color;
            struct {
                uint8_t b, g, r, a;
            };
        };
    };

    //-------------------------------------
    union CodePointHeight {
        uint32_t     value;
        struct {
            uint32_t codePoint : 24;
            uint32_t height    :  8;
        };
    };


    //-------------------------------------
    class Font {
        protected:
            using MapHeightData          = std::unordered_map<uint32_t, HeightData>;
            using MapCodePointData       = std::unordered_map<int32_t, CodePointData>;
            using MapCodePointHeightData = std::unordered_map<uint32_t, CodePointHeightData>;
            using MapKerning             = std::unordered_map<uint64_t, int32_t>;
            using SkylineBinPack         = MindShake::SkylineBinPack;
            using Rect                   = SkylineBinPack::Rect;
            using ELevelChoiceHeuristic  = SkylineBinPack::ELevelChoiceHeuristic;

        public:
            uint8_t                     GetStatus() const                   { return mStatus;                           }

            const std::string &         GetFontName() const                 { return mFontName;                         }
            uint8_t *                   GetTexture() const                  { return mTexture;                          }
            uint32_t                    GetTextureWidth() const             { return mPacker.GetWidth();                }
            uint32_t                    GetTextureHeight() const            { return mPacker.GetHeight();               }

            void                        DrawText(const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY);
            void                        GetTextBox(const char *utf8, uint8_t textHeight, Rect *pRect);
            
            void                        SetClipping(int32_t left, int32_t top, int32_t right, int32_t bottom)   { mLeft = left; mRight = right; mTop = top; mBottom = bottom; }

            void                        SetAntialias(bool set)              { mUseAntialias = set;                      }
            bool                        GetAntialias() const                { return mUseAntialias;                     }
            void                        SetAntialiasWeight(int32_t value)   { mAntiAliasWeight = value;                 }
            int32_t                     GetAntialiasWeight() const          { return mAntiAliasWeight;                  }

        protected:
            float                       GetScaleForHeight(uint8_t height)   { return GetDataForHeight(height).scale;    }
            uint32_t                    GetCodePointGlyph(uint32_t index)   { return GetCodePointData(index).glyph;     }
            void                        AABlock(uint8_t *src, uint32_t width, uint32_t height, uint8_t *dst, uint32_t dstStride);

        protected:
            virtual int                         GetKerning(uint32_t char1, uint32_t char2) = 0;

            virtual const HeightData &          GetDataForHeight(uint8_t height) = 0;
            virtual const CodePointData &       GetCodePointData(uint32_t index) = 0;
            virtual const CodePointHeightData & GetCodePointDataForHeight(uint32_t index, uint8_t height) = 0;

        protected:
            std::string            mFontName;
            SkylineBinPack         mPacker;
            uint8_t                *mTexture {};
            int                    mAscent  {};
            int                    mDescent {};
            int                    mLineGap {};
            MapHeightData          mHeightData;
            MapCodePointData       mCodePointData;
            MapCodePointHeightData mCodePointHeightData;
            MapKerning             mKerningData;

            int32_t                mLeft   { -0xffff };
            int32_t                mTop    { -0xffff };
            int32_t                mRight  {  0xffff };
            int32_t                mBottom {  0xffff };

            int8_t                 mStatus { -1 };
            int32_t                mAntiAliasWeight { 44 };
            bool                   mUseAntialias { false };
    };

} // end of namespace
//-------------------------------------
