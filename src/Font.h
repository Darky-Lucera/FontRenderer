#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "MappedFile.h"
#include "SkylineBinPack.h"
//-------------------------------------
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

//-------------------------------------
namespace MindShake {

    //---------------------------------
    struct HeightData {
        float   scale;
        int     ascent;
        int     descent;
        int     lineGap;

        int     GetLineAdvance() const  { return ascent - descent + lineGap; }
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
        float   advanceWidth;       // Kept fractional so the pen position does not accumulate rounding errors
        int     leftSideBearing;
        int     x, y;
        Rect    rect;               // Area in the texture; a rotated glyph is stored transposed, so its width and height are swapped.
                                    // Empty for glyphs without pixels, like the space, and for glyphs that did not fit in the texture.
        bool    rotated {};

        int32_t GetWidth() const    { return rotated ? rect.height : rect.width;  }
        int32_t GetHeight() const   { return rotated ? rect.width  : rect.height; }
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

        public:
            using Rect                   = SkylineBinPack::Rect;
            using ELevelChoiceHeuristic  = SkylineBinPack::ELevelChoiceHeuristic;

            // Which dimensions of the texture are doubled when a glyph does not fit.
            // Growing the width forces a full copy of the texture, because the row stride changes.
            enum class ETextureGrowth {
                Height,
                Width,
                Both
            };

            // What the textHeight passed to DrawText and GetTextBox measures.
            enum class ESizeMode {
                LineHeight,     // From the highest ascender to the lowest descender
                EmSize          // Typographic size, as in CSS font-size; the real line height depends on the font
            };

            //-------------------------
            enum class EStatus : int8_t {
                Ok,
                NotLoaded,          // The constructor did not finish loading the font
                CannotOpenFile,
                CannotReadFile,
                InvalidFont,
                OutOfMemory
            };

            // Most GPUs cannot allocate bigger textures.
            static constexpr uint32_t   kMaxTextureSize = 16384;
            // Bigger weights could overflow the sums of the antialias filter.
            static constexpr int32_t    kMaxAntialiasWeight = 0xffff;

        public:
            explicit                    Font(const char *fontName);
            virtual                     ~Font() = default;

                                        Font(const Font &)                  = delete;
            Font &                      operator=(const Font &)             = delete;

            EStatus                     GetStatus() const                   { return mStatus;                           }
            void                        Reset();                            // Remove all rendered glyphs and associated data!

            const std::string &         GetFontName() const                 { return mFontName;                         }
            const uint8_t *             GetTexture() const                  { return mTexture.data();                   }
            // The packer only covers the texture minus a band of glyph padding along its top and left edges.
            uint32_t                    GetTextureWidth() const             { return mPacker.GetWidth()  + mGlyphPadding; }
            uint32_t                    GetTextureHeight() const            { return mPacker.GetHeight() + mGlyphPadding; }
            // Size of the top-left area of the texture that holds glyphs, to save a cropped atlas. 0 if it is empty.
            uint32_t                    GetUsedTextureWidth() const;
            uint32_t                    GetUsedTextureHeight() const;
            // Glyphs that already failed to fit stay empty until Reset.
            void                        SetTextureGrowth(ETextureGrowth growth) { mTextureGrowth = growth;              }
            ETextureGrowth              GetTextureGrowth() const            { return mTextureGrowth;                    }
            // Changing the mode discards every rendered glyph, like Reset.
            void                        SetSizeMode(ESizeMode mode);
            ESizeMode                   GetSizeMode() const                 { return mSizeMode;                         }
            // Empty pixels kept around every glyph in the texture, so that bilinear filtering (e.g. OpenGL)
            // does not bleed neighbouring glyphs. Changing it discards every rendered glyph, like Reset.
            // Returns false if the padding would leave no room in the texture.
            bool                        SetGlyphPadding(uint32_t padding);
            uint32_t                    GetGlyphPadding() const             { return mGlyphPadding;                     }
            // Only affects glyphs packed afterwards.
            void                        SetPackingHeuristic(ELevelChoiceHeuristic heuristic) { mPackingHeuristic = heuristic; }
            ELevelChoiceHeuristic       GetPackingHeuristic() const         { return mPackingHeuristic;                 }

            // DrawText does not know the size of dst: without SetClipping the text has to fit inside it.
            void                        DrawText(const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY);
            // Box covering every glyph DrawText would draw, relative to the position passed to it.
            // A text with nothing to draw, like an empty one, gives an empty box.
            void                        GetTextBox(const char *utf8, uint8_t textHeight, Rect *pRect);

            void                        SetClipping(int32_t left, int32_t top, int32_t right, int32_t bottom)   { mLeft = left; mRight = right; mTop = top; mBottom = bottom; }

            // Changing any antialias setting discards every rendered glyph, like Reset.
            void                        SetAntialias(bool set);
            bool                        GetAntialias() const                { return mUseAntialias;                     }
            void                        SetAntialiasAllowEx(bool set);
            bool                        GetAntialiasAllowEx() const         { return mAntialiasAllowEx;                 }
            // Returns false, keeping the current weights, if any is outside [0, kMaxAntialiasWeight] or all are 0.
            bool                        SetAntialiasWeights(int32_t center, int32_t border, int32_t corner);
            int32_t                     GetAntialiasCenter() const          { return mAACenter;                         }
            int32_t                     GetAntialiasBorder() const          { return mAABorder;                         }
            int32_t                     GetAntialiasCorner() const          { return mAACorner;                         }

        protected:
            // On failure it sets the status and returns false.
            bool                        LoadFile(const char *fileName);
            bool                        InitPacker();
            // Copies the glyph into the texture, growing it if needed. Returns false, leaving data.rect empty, if it does not fit.
            bool                        PackGlyph(const uint8_t *pixels, uint32_t width, uint32_t height, CodePointHeightData &data);
            bool                        GrowTexture();
            float                       GetScaleForHeight(uint8_t height)   { return GetDataForHeight(height).scale;    }
            uint32_t                    GetCodePointGlyph(uint32_t index)   { return GetCodePointData(index).glyph;     }
            float                       GetScaledKerning(int glyph, uint32_t nextCodePoint, float scale);
            // Returns how many pixels the glyph grew on each side.
            int                         ApplyAntialias(std::unique_ptr<uint8_t[]> &pixels, int &width, int &height);
            void                        AABlock(uint8_t *src, uint32_t width, uint32_t height, uint8_t *dst, uint32_t dstStride);
            void                        AABlockEx(uint8_t *src, uint32_t width, uint32_t height, uint8_t *dst, uint32_t dstStride);
            const HeightData &          GetDataForHeight(uint8_t height);

        protected:
            virtual int                         GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) = 0;

            virtual const CodePointData &       GetCodePointData(uint32_t index) = 0;
            virtual const CodePointHeightData & GetCodePointDataForHeight(uint32_t index, uint8_t height) = 0;

        protected:
            std::string            mFontName;
            MappedFile             mFontFile;       // The backends point into it. As a base class member, it is destroyed after their members
            SkylineBinPack         mPacker;
            std::vector<uint8_t>   mTexture;
            int                    mAscent  {};
            int                    mDescent {};
            int                    mLineGap {};
            int                    mUnitsPerEm {};
            MapHeightData          mHeightData;
            MapCodePointData       mCodePointData;
            MapCodePointHeightData mCodePointHeightData;
            MapKerning             mKerningData;

            int32_t                mLeft   { -0xffff };
            int32_t                mTop    { -0xffff };
            int32_t                mRight  {  0xffff };
            int32_t                mBottom {  0xffff };

            ETextureGrowth         mTextureGrowth { ETextureGrowth::Height };
            ESizeMode              mSizeMode { ESizeMode::LineHeight };
            uint32_t               mGlyphPadding { 1 };
            ELevelChoiceHeuristic  mPackingHeuristic { ELevelChoiceHeuristic::LevelBottomLeft };
            EStatus                mStatus { EStatus::NotLoaded };
            int32_t                mAACenter { 20 };
            int32_t                mAABorder {  4 };
            int32_t                mAACorner {  1 };
            bool                   mUseAntialias { false };
            bool                   mAntialiasAllowEx { false };
    };

} // end of namespace
//-------------------------------------
