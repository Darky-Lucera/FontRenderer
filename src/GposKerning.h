#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <vector>

//-------------------------------------
namespace MindShake {

    // Kerning of a pair of glyphs from the 'kern' feature of the OpenType GPOS table.
    // It only reads the pair adjustment lookups. The contextual ones need the whole text and a shaper like HarfBuzz.
    //---------------------------------
    class GposKerning {
        public:
            // fontOffset is where the font starts inside a font collection, and 0 for a single font.
            // The font data must outlive this object. Returns false if there is no kerning it can read.
            bool        Load(const uint8_t *font, size_t size, size_t fontOffset = 0);
            bool        HasKerning() const      { return mScripts.empty() == false; }

            // In font units. Only the advance of the first glyph counts, which is where kerning goes in horizontal text.
            int         GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) const;

        protected:
            const uint8_t                       *mGpos {};
            size_t                              mGposSize {};
            // Positions in GPOS of the pair adjustment subtables of each lookup.
            std::vector<std::vector<uint32_t>>  mLookups;
            // Indices into mLookups of the 'kern' feature of each script, in the order they are tried.
            std::vector<std::vector<uint16_t>>  mScripts;
    };

} // end of namespace
