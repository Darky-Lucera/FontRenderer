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
#include <string>
#include <unordered_map>
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
    struct CodePointHeightData {
        using Rect = MindShake::SkylineBinPack::Rect;

        int     glyph;              // It's convenient
        float   advanceWidth;       // Kept fractional so the pen position does not accumulate rounding errors
        int     leftSideBearing;
        int     x, y;
        Rect    rect;               // Area in the texture; a rotated glyph is stored transposed, so its width and height are swapped.
                                    // Empty for glyphs without pixels, like the space, and for glyphs that did not fit in the texture.
        bool    rotated {};

        int32_t GetWidth() const    { return rotated ? rect.height : rect.width;  }
        int32_t GetHeight() const   { return rotated ? rect.width  : rect.height; }
    };

    //---------------------------------
    struct GlyphQuad {
        using Rect = MindShake::SkylineBinPack::Rect;

        int32_t x, y;               // Relative to the position passed to DrawText
        int32_t width, height;
        // A rotated glyph is stored transposed: the pixel (x, y) of the quad is the texel (textureRect.x + y, textureRect.y + x).
        Rect    textureRect;
        bool    rotated;
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

    // What every font can do with the glyphs it already has: lay out and draw text, and give the texture.
    //-------------------------------------
    class FontBase {
        protected:
            using MapHeightData          = std::unordered_map<uint32_t, HeightData>;
            using MapCodePointHeightData = std::unordered_map<uint32_t, CodePointHeightData>;
            using MapKerning             = std::unordered_map<uint64_t, int32_t>;

        public:
            using Rect                   = SkylineBinPack::Rect;

            //-------------------------
            enum class EStatus : int8_t {
                Ok,
                NotLoaded,          // The constructor did not finish loading the font
                CannotOpenFile,
                CannotReadFile,
                InvalidFont,
                OutOfMemory,
                CannotWriteFile,
                InvalidTexture      // FontBaked cannot read the texture, or its size is not the one in the metrics file
            };

            //-------------------------
            enum class ETextureFormat : int8_t {
                Alpha8,             // One byte of coverage per texel, drawn with the color of the text
                BGRA32,             // Four bytes per texel: blue, green, red and alpha, not premultiplied, as Color32
                BGRA32Premultiplied // As BGRA32, with blue, green and red multiplied by the alpha
            };

            // Most GPUs cannot allocate bigger textures.
            static constexpr uint32_t   kMaxTextureSize = 16384;

        public:
            explicit                    FontBase(const char *fileName);
            virtual                     ~FontBase() = default;

                                        FontBase(const FontBase &)          = delete;
            FontBase &                  operator=(const FontBase &)         = delete;

            EStatus                     GetStatus() const                   { return mStatus;                           }

            const std::string &         GetFileName() const                 { return mFileName;                         }

            // Rows of GetTextureWidth texels, in the format GetTextureFormat gives. nullptr if the font has no copy of the texture.
            const uint8_t *             GetTexture() const                  { return mTexture.empty() ? nullptr : mTexture.data(); }
            // Font always renders Alpha8. Only FontBaked can have a color texture, and it premultiplies the ones it copies,
            // so a texture it has is BGRA32Premultiplied. Draw it on the GPU with the blend of premultiplied alpha.
            ETextureFormat              GetTextureFormat() const            { return mTextureFormat;                    }
            uint32_t                    GetTextureWidth() const             { return mTextureWidth;                     }
            uint32_t                    GetTextureHeight() const            { return mTextureHeight;                    }
            // Size of the top-left area of the texture that holds glyphs, to save a cropped atlas. 0 if it is empty.
            virtual uint32_t            GetUsedTextureWidth() const         { return mTextureWidth;                     }
            virtual uint32_t            GetUsedTextureHeight() const        { return mTextureHeight;                    }
            // Changes whenever the texels change, so that a copy of the texture, like one in the GPU, knows when to update.
            uint32_t                    GetTextureVersion() const           { return mTextureVersion;                   }

            // DrawText does not know the size of dst: without SetClipping the text has to fit inside it.
            // With a color texture, the color of each texel is multiplied by color, as a GPU does with the color of a vertex.
            // It blends the alpha of dst too, as the over operator of Porter and Duff does. Unlike color, dst must have
            // premultiplied alpha, and so does the result. An opaque pixel is the same either way, and an opaque dst
            // stays opaque.
            void                        DrawText(const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY);
            // Box covering every glyph DrawText would draw, relative to the position passed to it.
            // A text with nothing to draw, like an empty one, gives an empty box.
            void                        GetTextBox(const char *utf8, uint8_t textHeight, Rect *rect);
            // The glyphs DrawText would draw, to draw them in another way, like with the GPU. The clipping does not apply.
            // It can add glyphs to the texture, so check GetTextureVersion afterwards.
            void                        GetGlyphQuads(const char *utf8, uint8_t textHeight, std::vector<GlyphQuad> &quads);

            void                        SetClipping(int32_t left, int32_t top, int32_t right, int32_t bottom)   { mLeft = left; mRight = right; mTop = top; mBottom = bottom; }

        protected:
            // Calls visit(data, penX, baseline) for each glyph of the text, with the pen position relative to the top-left of the text.
            template <class TVisitor>
            void                        LayoutText(const char *utf8, uint8_t textHeight, TVisitor visit);
            float                       GetScaledKerning(int glyph, uint32_t nextCodePoint, float scale);

            static uint32_t             GetCodePointHeightKey(uint32_t codePoint, uint8_t height);
            static uint64_t             GetKerningKey(uint32_t leftGlyph, uint32_t rightGlyph);
            static EStatus              GetFileStatus(MappedFile::EError error);
            static uint32_t             GetBytesPerTexel(ETextureFormat format) { return (format == ETextureFormat::Alpha8) ? 1 : 4; }
            // Multiplies blue, green and red of each BGRA32 texel by its alpha, rounded as DrawText rounds.
            static void                 PremultiplyTexels(uint8_t *texels, size_t texelCount);

            // Only what DrawText uses: the heights, the glyphs, and the kerning between the glyphs.
            EStatus                     SaveMetrics(const char *fileName, uint32_t textureWidth, uint32_t textureHeight) const;
            // Also sets the texture size. It does not load the texture.
            EStatus                     LoadMetrics(const char *fileName);

        protected:
            virtual const HeightData &          GetDataForHeight(uint8_t height) = 0;
            virtual const CodePointHeightData & GetCodePointDataForHeight(uint32_t codePoint, uint8_t height) = 0;
            virtual uint32_t                    GetCodePointGlyph(uint32_t codePoint) = 0;
            // In font units.
            virtual int                         GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) = 0;

        protected:
            std::string            mFileName;
            std::vector<uint8_t>   mTexture;
            ETextureFormat         mTextureFormat  { ETextureFormat::Alpha8 };
            uint32_t               mTextureWidth   {};
            uint32_t               mTextureHeight  {};
            uint32_t               mTextureVersion {};
            MapHeightData          mHeightData;
            MapCodePointHeightData mCodePointHeightData;
            MapKerning             mKerningData;

            int32_t                mLeft   { -0xffff };
            int32_t                mTop    { -0xffff };
            int32_t                mRight  {  0xffff };
            int32_t                mBottom {  0xffff };

            EStatus                mStatus { EStatus::NotLoaded };
    };

} // end of namespace
//-------------------------------------
