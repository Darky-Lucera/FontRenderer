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

    // Only TGA images without a color map, uncompressed or compressed with RLE. In memory, the pixels go in rows
    // from top to bottom, with 1 channel, gray, or 4 channels: blue, green, red and alpha, the order of the file.

    //---------------------------------
    enum class ETgaAlpha : uint8_t {
        Straight,
        Premultiplied       // Blue, green and red are multiplied by the alpha
    };

    // Writes 8-bit grayscale for 1 channel, and 32-bit color with alpha for 4. A premultiplied alpha is marked in
    // the extension area of TGA 2.0; a program that does not read it takes the alpha as straight.
    // stride is the distance in bytes between the starts of two rows. Width and height go from 1 to 65535.
    bool    WriteTga(const char *fileName, const uint8_t *pixels, uint32_t width, uint32_t height, size_t stride, uint32_t channels,
                     bool compress, ETgaAlpha alpha = ETgaAlpha::Straight);
    // Reads 8-bit grayscale with 1 channel. Reads 16-bit grayscale with alpha, and 32-bit color with alpha, with 4 channels.
    // Rows stored from the bottom up, as many programs write them, are flipped. alpha is Premultiplied only when the
    // extension area of TGA 2.0 says so, and the pixels are not converted. Returns false if the data is not such an image,
    // or if it has no alpha: when the image says it has none and every pixel has the same alpha. An alpha that varies
    // is used, because some programs do not declare it.
    bool    ReadTga(const uint8_t *data, size_t size, std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height, uint32_t &channels,
                    ETgaAlpha &alpha);

} // end of namespace
