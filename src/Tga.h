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

    // Only 8-bit grayscale TGA images, without a color map, uncompressed or compressed with RLE.
    // The pixels go in rows from top to bottom.

    // stride is the distance in bytes between the starts of two rows. Width and height go from 1 to 65535.
    bool    WriteTga(const char *fileName, const uint8_t *pixels, uint32_t width, uint32_t height, size_t stride, bool compress);
    // Rows stored from the bottom up, as many programs write them, are flipped. Returns false if the data is not such an image.
    bool    ReadTga(const uint8_t *data, size_t size, std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height);

} // end of namespace
