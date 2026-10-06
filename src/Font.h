#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontBase.h"
#include "GposKerning.h"
#include "MappedFile.h"
#include "SkylineBinPack.h"
//-------------------------------------
#include <cstdint>
#include <memory>
#include <unordered_map>

//-------------------------------------
namespace MindShake {

    //---------------------------------
    struct CodePointData {
        int     glyph;
        int     advanceWidth;
        int     leftSideBearing;
    };

    // A font that renders its glyphs from a font file the first time they are drawn.
    //-------------------------------------
    class Font : public FontBase {
        protected:
            using MapCodePointData       = std::unordered_map<int32_t, CodePointData>;
            using SkylineBinPack         = MindShake::SkylineBinPack;

            //-------------------------
            struct GlyphBitmap {
                std::unique_ptr<uint8_t[]>  pixels;
                int                         width  {};
                int                         height {};
            };

        public:
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

            // Bigger weights could overflow the sums of the antialias filter.
            static constexpr int32_t    kMaxAntialiasWeight = 0xffff;

        public:
            explicit                    Font(const char *fileName);

            void                        Reset();                            // Remove all rendered glyphs and associated data!

            uint32_t                    GetUsedTextureWidth() const override;
            uint32_t                    GetUsedTextureHeight() const override;

            // Glyphs that already failed to fit stay empty until Reset.
            void                        SetTextureGrowth(ETextureGrowth growth) { mTextureGrowth = growth;              }
            ETextureGrowth              GetTextureGrowth() const            { return mTextureGrowth;                    }

            // Changing the mode discards every rendered glyph, like Reset.
            void                        SetSizeMode(ESizeMode mode);
            ESizeMode                   GetSizeMode() const                 { return mSizeMode;                         }

            // Empty pixels kept between the glyphs in the texture, so that bilinear filtering (e.g. OpenGL)
            // does not bleed neighbouring glyphs. Changing it discards every rendered glyph, like Reset.
            // Returns false if the spacing would leave no room in the texture.
            bool                        SetGlyphSpacing(uint32_t spacing);
            uint32_t                    GetGlyphSpacing() const             { return mGlyphSpacing;                     }

            // Empty pixels added to each side of every glyph, as part of the glyph: DrawText, GetTextBox and
            // GetGlyphQuads include them. They leave room to add effects, like a shadow or an outline, to the saved texture.
            // Changing it discards every rendered glyph, like Reset. Returns false if a padding would not fit in any texture.
            bool                        SetGlyphPadding(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);
            uint32_t                    GetGlyphPaddingLeft() const         { return mGlyphPaddingLeft;                 }
            uint32_t                    GetGlyphPaddingTop() const          { return mGlyphPaddingTop;                  }
            uint32_t                    GetGlyphPaddingRight() const        { return mGlyphPaddingRight;                }
            uint32_t                    GetGlyphPaddingBottom() const       { return mGlyphPaddingBottom;               }

            // Only affects glyphs packed afterwards.
            void                        SetPackingHeuristic(ELevelChoiceHeuristic heuristic) { mPackingHeuristic = heuristic; }
            ELevelChoiceHeuristic       GetPackingHeuristic() const         { return mPackingHeuristic;                 }

            // A rotated glyph is stored transposed. An effect added to the saved texture that is not symmetric
            // about the diagonal, like a vertical gradient, would look wrong on it. Only affects glyphs packed afterwards.
            void                        SetAllowRotation(bool enabled)      { mPacker.SetAllowRotation(enabled);        }
            bool                        GetAllowRotation() const            { return mPacker.GetAllowRotation();        }

            // Changing any antialias setting discards every rendered glyph, like Reset.
            void                        SetAntialias(bool enabled);
            bool                        GetAntialias() const                { return mAntialiasEnabled;                 }
            void                        SetAntialiasAllowEx(bool enabled);
            bool                        GetAntialiasAllowEx() const         { return mAntialiasAllowEx;                 }
            // Returns false, keeping the current weights, if any is outside [0, kMaxAntialiasWeight] or all are 0.
            bool                        SetAntialiasWeights(int32_t center, int32_t border, int32_t corner);
            int32_t                     GetAntialiasCenter() const          { return mAntialiasCenter;                  }
            int32_t                     GetAntialiasBorder() const          { return mAntialiasBorder;                  }
            int32_t                     GetAntialiasCorner() const          { return mAntialiasCorner;                  }

            // Renders every code point of the text at that height before it is drawn. Packing them all together
            // fills the texture better than packing them one by one. Returns false if any glyph did not fit in the texture.
            bool                        Preload(const char *utf8, uint8_t textHeight);

            // Looks up the kerning of every pair of glyphs rendered so far, at any height, so that SaveBaked saves it.
            // It can be very slow: for n glyphs it looks up n * n pairs.
            void                        LoadAllKerningPairs();

            // Saves what FontBaked needs to draw the glyphs rendered so far: their metrics and kerning in metricsFile,
            // and the used area of the texture in textureFile, as an 8-bit grayscale TGA compressed with RLE.
            // FontBaked also reads the texture as a 32-bit TGA, so an image program can add color and effects to it.
            // Only the kerning pairs already looked up are saved. Call LoadAllKerningPairs first to save all of them.
            EStatus                     SaveBaked(const char *metricsFile, const char *textureFile) const;

        protected:
            // On failure it sets the status and returns false.
            bool                        LoadFile(const char *fileName);
            bool                        InitPacker();
            // Copies the glyph into the texture, growing it if needed. Returns false, leaving data.rect empty, if it does not fit.
            bool                        PackGlyph(const uint8_t *pixels, uint32_t width, uint32_t height, CodePointHeightData &data);
            bool                        CanEverFit(uint32_t paddedWidth, uint32_t paddedHeight) const;
            void                        CopyGlyph(const uint8_t *pixels, uint32_t width, uint32_t height, const Rect &packed, CodePointHeightData &data);
            bool                        GrowTexture();
            // Returns false if the font has no glyph for the code point, or the backend cannot render it.
            bool                        RenderGlyph(uint32_t codePoint, uint8_t height, CodePointHeightData &data, GlyphBitmap &bitmap);
            float                       GetScaleForHeight(uint8_t height)   { return GetDataForHeight(height).scale;    }
            // Without the cache of mKerningData.
            int                         LookUpKerning(uint32_t leftGlyph, uint32_t rightGlyph);
            // Returns how many pixels the glyph grew on each side.
            int                         ApplyAntialias(std::unique_ptr<uint8_t[]> &pixels, int &width, int &height);
            void                        ApplyPadding(GlyphBitmap &bitmap, CodePointHeightData &data);
            void                        AntialiasBlock(uint8_t *src, uint32_t width, uint32_t height, uint8_t *dst, uint32_t dstStride);
            void                        AntialiasBlockEx(uint8_t *src, uint32_t width, uint32_t height, uint8_t *dst, uint32_t dstStride);

            const HeightData &          GetDataForHeight(uint8_t height) override;
            const CodePointHeightData & GetCodePointDataForHeight(uint32_t codePoint, uint8_t height) override;
            uint32_t                    GetCodePointGlyph(uint32_t codePoint) override { return GetCodePointData(codePoint).glyph; }
            int                         GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) override;

        protected:
            // Only used when GposKerning has nothing to read.
            virtual int                         GetKernTableKerning(uint32_t leftGlyph, uint32_t rightGlyph) = 0;

            virtual const CodePointData &       GetCodePointData(uint32_t codePoint) = 0;
            // Fills the position and the advance of data, and the coverage of the glyph in bitmap,
            // which stays empty for a glyph without pixels. Returns false if it cannot render the glyph.
            virtual bool                        RasterizeGlyph(const CodePointData &codePointData, uint8_t height, CodePointHeightData &data, GlyphBitmap &bitmap) = 0;

        protected:
            MappedFile             mFontFile;       // The backends point into it. As a base class member, it is destroyed after their members
            SkylineBinPack         mPacker;
            int                    mAscent  {};
            int                    mDescent {};
            int                    mLineGap {};
            int                    mUnitsPerEm {};
            MapCodePointData       mCodePointData;
            GposKerning            mGposKerning;    // Points into mFontFile

            ETextureGrowth         mTextureGrowth { ETextureGrowth::Height };
            ESizeMode              mSizeMode { ESizeMode::LineHeight };
            uint32_t               mGlyphSpacing { 1 };
            uint32_t               mGlyphPaddingLeft   {};
            uint32_t               mGlyphPaddingTop    {};
            uint32_t               mGlyphPaddingRight  {};
            uint32_t               mGlyphPaddingBottom {};
            ELevelChoiceHeuristic  mPackingHeuristic { ELevelChoiceHeuristic::LevelBottomLeft };
            int32_t                mAntialiasCenter { 20 };
            int32_t                mAntialiasBorder {  4 };
            int32_t                mAntialiasCorner {  1 };
            bool                   mAntialiasEnabled { false };
            bool                   mAntialiasAllowEx { false };
    };

} // end of namespace
//-------------------------------------
