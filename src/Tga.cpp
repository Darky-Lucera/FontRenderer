//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Tga.h"
//-------------------------------------
#include <algorithm>
#include <cstdio>
#include <cstring>

//-------------------------------------
namespace {

    constexpr size_t   kHeaderSize        = 18;
    constexpr uint8_t  kNoColorMap        = 0;
    constexpr uint8_t  kUncompressedGray  = 3;
    constexpr uint8_t  kCompressedGray    = 11;
    constexpr uint8_t  kBitsPerPixel      = 8;
    constexpr uint8_t  kTopToBottom       = 0x20;
    constexpr uint8_t  kRightToLeft       = 0x10;
    constexpr uint32_t kMaxSize           = 0xffff;
    // A packet repeats one pixel, or copies up to this many pixels as they are.
    constexpr uint8_t  kRunPacket         = 0x80;
    constexpr size_t   kMaxPacketPixels   = 128;

    //---------------------------------
    uint16_t
    GetU16(const uint8_t *data) {
        return uint16_t(data[0] | (data[1] << 8));
    }

    //---------------------------------
    void
    SetU16(uint8_t *data, uint32_t value) {
        data[0] = uint8_t(value);
        data[1] = uint8_t(value >> 8);
    }

    // The packets do not cross rows, as TGA 2.0 asks.
    //---------------------------------
    void
    CompressRow(const uint8_t *row, size_t width, std::vector<uint8_t> &output) {
        size_t x = 0;
        while(x < width) {
            size_t run = 1;
            while(x + run < width && run < kMaxPacketPixels && row[x + run] == row[x]) {
                ++run;
            }

            if(run > 1) {
                output.push_back(uint8_t(kRunPacket | (run - 1)));
                output.push_back(row[x]);
                x += run;
                continue;
            }

            // Copies pixels until two equal ones start a run.
            size_t count = 1;
            while(x + count < width && count < kMaxPacketPixels &&
                  (x + count + 1 >= width || row[x + count] != row[x + count + 1])) {
                ++count;
            }

            output.push_back(uint8_t(count - 1));
            output.insert(output.end(), row + x, row + x + count);
            x += count;
        }
    }

    // Other programs may let a packet cross rows, so the pixels are read as a single sequence.
    //---------------------------------
    bool
    Decompress(const uint8_t *data, size_t size, uint8_t *pixels, size_t pixelCount) {
        size_t read    = 0;
        size_t written = 0;
        while(written < pixelCount) {
            if(read >= size) {
                return false;
            }

            const uint8_t header = data[read++];
            const size_t  count  = size_t(header & ~kRunPacket) + 1;
            if(count > pixelCount - written) {
                return false;
            }

            if((header & kRunPacket) != 0) {
                if(read >= size) {
                    return false;
                }
                memset(pixels + written, data[read++], count);
            }
            else {
                if(size - read < count) {
                    return false;
                }
                memcpy(pixels + written, data + read, count);
                read += count;
            }
            written += count;
        }

        return true;
    }

} // end of namespace

//-------------------------------------
bool
MindShake::WriteTga(const char *fileName, const uint8_t *pixels, uint32_t width, uint32_t height, size_t stride, bool compress) {
    if(fileName == nullptr || pixels == nullptr || width == 0 || height == 0 || width > kMaxSize || height > kMaxSize || stride < width) {
        return false;
    }

    std::vector<uint8_t> output(kHeaderSize, 0);
    output[1]  = kNoColorMap;
    output[2]  = compress ? kCompressedGray : kUncompressedGray;
    SetU16(&output[12], width);
    SetU16(&output[14], height);
    output[16] = kBitsPerPixel;
    output[17] = kTopToBottom;

    for(uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = pixels + y * stride;
        if(compress) {
            CompressRow(row, width, output);
        }
        else {
            output.insert(output.end(), row, row + width);
        }
    }

    FILE *file = fopen(fileName, "wb");
    if(file == nullptr) {
        return false;
    }

    const bool written = fwrite(output.data(), 1, output.size(), file) == output.size();
    return (fclose(file) == 0) && written;
}

//-------------------------------------
bool
MindShake::ReadTga(const uint8_t *data, size_t size, std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height) {
    if(data == nullptr || size < kHeaderSize) {
        return false;
    }

    const size_t   idLength    = data[0];
    const uint8_t  type        = data[2];
    const uint8_t  descriptor  = data[17];
    const uint32_t imageWidth  = GetU16(&data[12]);
    const uint32_t imageHeight = GetU16(&data[14]);
    if(data[1] != kNoColorMap || (type != kUncompressedGray && type != kCompressedGray) || data[16] != kBitsPerPixel ||
       (descriptor & kRightToLeft) != 0 || imageWidth == 0 || imageHeight == 0 || size - kHeaderSize < idLength) {
        return false;
    }

    // Anything after the pixels, like the footer of TGA 2.0, is ignored.
    const uint8_t *source     = data + kHeaderSize + idLength;
    const size_t  sourceSize  = size - kHeaderSize - idLength;
    const size_t  pixelCount  = size_t(imageWidth) * imageHeight;
    if(type == kUncompressedGray) {
        if(sourceSize < pixelCount) {
            return false;
        }
        pixels.assign(source, source + pixelCount);
    }
    else {
        pixels.resize(pixelCount);
        if(Decompress(source, sourceSize, pixels.data(), pixelCount) == false) {
            return false;
        }
    }

    if((descriptor & kTopToBottom) == 0) {
        for(uint32_t y = 0; y < imageHeight / 2; ++y) {
            uint8_t *top    = &pixels[size_t(y) * imageWidth];
            uint8_t *bottom = &pixels[size_t(imageHeight - 1 - y) * imageWidth];
            std::swap_ranges(top, top + imageWidth, bottom);
        }
    }

    width  = imageWidth;
    height = imageHeight;

    return true;
}
