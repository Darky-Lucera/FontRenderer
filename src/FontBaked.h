#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

// CMake defines FONTRENDERER_USE_BAKED when it builds this backend. Without CMake, define it for every file.
// Otherwise, a build without the backend would only fail at link time, with a missing symbol that says little.
#if !defined(FONTRENDERER_USE_BAKED)
    #error "FontBaked needs FONTRENDERER_USE_BAKED. See the README."
#endif

#include "FontBase.h"
//-------------------------------------
#include <cstdint>
#include <unordered_map>

//-------------------------------------
namespace MindShake {

    // A font that only has the glyphs Font::SaveBaked saved. It cannot render new ones:
    // a code point or a height that is not in the files is not drawn.
    //---------------------------------
    class FontBaked : public FontBase {
        public:
            // The texture is read from an 8-bit grayscale TGA, like the one Font::SaveBaked writes, which gives an Alpha8
            // texture, or from a 32-bit TGA with alpha or a 16-bit grayscale TGA with alpha, which give a BGRA32Premultiplied
            // texture. The alpha is premultiplied unless the extension area of TGA 2.0 says that it already is.
            // A texture it cannot use, like a color TGA without alpha, gives InvalidTexture.
            FontBaked(const char *metricsFile, const char *textureFile);
            // The texture is in rows of width texels, and it is copied. Its size must be the one Font::SaveBaked saved.
            // A BGRA32 texture is premultiplied, so GetTextureFormat gives BGRA32Premultiplied.
            // Without a texture, for one that is only in the GPU, DrawText draws nothing and GetTextureFormat gives format.
            FontBaked(const char *metricsFile, const uint8_t *texture, uint32_t width, uint32_t height, ETextureFormat format);

        protected:
            // LoadMetrics, and the glyph of each code point, which GetCodePointGlyph needs.
            EStatus                     LoadGlyphs(const char *metricsFile);
            EStatus                     LoadTexture(const char *textureFile);
            EStatus                     SetTexture(const uint8_t *texture, uint32_t width, uint32_t height, ETextureFormat format);

            const HeightData &          GetDataForHeight(uint8_t height) override;
            const CodePointHeightData & GetCodePointDataForHeight(uint32_t codePoint, uint8_t height) override;
            uint32_t                    GetCodePointGlyph(uint32_t codePoint) override;
            int                         GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) override;

        protected:
            std::unordered_map<uint32_t, uint32_t>  mCodePointGlyphs;
    };

} // end of namespace
