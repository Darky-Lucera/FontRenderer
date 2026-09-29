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
    constexpr uint8_t  kUncompressedColor = 2;
    constexpr uint8_t  kUncompressedGray  = 3;
    constexpr uint8_t  kCompressedColor   = 10;
    constexpr uint8_t  kCompressedGray    = 11;
    constexpr uint8_t  kAlphaBits         = 8;
    constexpr uint8_t  kAlphaBitsMask     = 0x0f;
    constexpr uint8_t  kTopToBottom       = 0x20;
    constexpr uint8_t  kRightToLeft       = 0x10;
    constexpr uint32_t kMaxSize           = 0xffff;
    // A packet repeats one pixel, or copies up to this many pixels as they are.
    constexpr uint8_t  kRunPacket         = 0x80;
    constexpr size_t   kMaxPacketPixels   = 128;

    // TGA 2.0 ends with a footer that points to an extension area, which says what the alpha holds.
    constexpr char     kSignature[]             = "TRUEVISION-XFILE.";
    constexpr size_t   kFooterSize              = 8 + sizeof(kSignature);
    constexpr size_t   kExtensionSize           = 495;
    constexpr size_t   kAttributesTypeOffset    = 494;
    constexpr uint8_t  kAttributesUsefulAlpha   = 3;
    constexpr uint8_t  kAttributesPremultiplied = 4;

    //---------------------------------
    enum class EAlpha {
        None,
        Straight,
        Premultiplied
    };

    //---------------------------------
    uint16_t
    GetU16(const uint8_t *data) {
        return uint16_t(data[0] | (data[1] << 8));
    }

    //---------------------------------
    uint32_t
    GetU32(const uint8_t *data) {
        return uint32_t(GetU16(data)) | (uint32_t(GetU16(data + 2)) << 16);
    }

    //---------------------------------
    void
    SetU16(uint8_t *data, uint32_t value) {
        data[0] = uint8_t(value);
        data[1] = uint8_t(value >> 8);
    }

    // What the header and the extension area of TGA 2.0 say the alpha holds. The extension area is more precise.
    //---------------------------------
    EAlpha
    GetDeclaredAlpha(const uint8_t *data, size_t size) {
        if(size >= kHeaderSize + kFooterSize && memcmp(data + size - sizeof(kSignature), kSignature, sizeof(kSignature)) == 0) {
            const size_t offset = GetU32(data + size - kFooterSize);
            if(offset >= kHeaderSize && offset <= size - kFooterSize && size - kFooterSize - offset >= kExtensionSize &&
               GetU16(data + offset) == kExtensionSize) {
                const uint8_t attributes = data[offset + kAttributesTypeOffset];
                if(attributes == kAttributesUsefulAlpha) {
                    return EAlpha::Straight;
                }
                if(attributes == kAttributesPremultiplied) {
                    return EAlpha::Premultiplied;
                }
                return EAlpha::None;
            }
        }

        return ((data[17] & kAlphaBitsMask) != 0) ? EAlpha::Straight : EAlpha::None;
    }

    // Some programs write an alpha without declaring it. An alpha that is the same in every pixel holds nothing.
    //---------------------------------
    bool
    AlphaVaries(const std::vector<uint8_t> &pixels, size_t pixelSize) {
        for(size_t i = 2 * pixelSize - 1; i < pixels.size(); i += pixelSize) {
            if(pixels[i] != pixels[pixelSize - 1]) {
                return true;
            }
        }

        return false;
    }

    // From gray and alpha to blue, green, red and alpha.
    //---------------------------------
    void
    ConvertGrayToColor(std::vector<uint8_t> &pixels, size_t pixelCount) {
        std::vector<uint8_t> color(pixelCount * 4);
        for(size_t i = 0; i < pixelCount; ++i) {
            const uint8_t gray = pixels[i * 2];
            color[i * 4]     = gray;
            color[i * 4 + 1] = gray;
            color[i * 4 + 2] = gray;
            color[i * 4 + 3] = pixels[i * 2 + 1];
        }
        pixels.swap(color);
    }

    //---------------------------------
    void
    Unpremultiply(std::vector<uint8_t> &pixels) {
        for(size_t i = 0; i < pixels.size(); i += 4) {
            const uint32_t alpha = pixels[i + 3];
            for(size_t channel = i; channel < i + 3; ++channel) {
                // A broken file can have a color brighter than its alpha allows.
                const uint32_t color = (alpha == 0) ? 0 : (pixels[channel] * 255 + alpha / 2) / alpha;
                pixels[channel] = uint8_t(std::min<uint32_t>(color, 255));
            }
        }
    }

    //---------------------------------
    bool
    IsValidChannelCount(uint32_t channels) {
        return channels == 1 || channels == 4;
    }

    // The packets do not cross rows, as TGA 2.0 asks.
    //---------------------------------
    void
    CompressRow(const uint8_t *row, size_t width, size_t pixelSize, std::vector<uint8_t> &output) {
        auto areEqual = [=](size_t a, size_t b) {
            return memcmp(row + a * pixelSize, row + b * pixelSize, pixelSize) == 0;
        };

        size_t x = 0;
        while(x < width) {
            size_t run = 1;
            while(x + run < width && run < kMaxPacketPixels && areEqual(x + run, x)) {
                ++run;
            }

            if(run > 1) {
                output.push_back(uint8_t(kRunPacket | (run - 1)));
                output.insert(output.end(), row + x * pixelSize, row + (x + 1) * pixelSize);
                x += run;
                continue;
            }

            // Copies pixels until two equal ones start a run.
            size_t count = 1;
            while(x + count < width && count < kMaxPacketPixels &&
                  (x + count + 1 >= width || areEqual(x + count, x + count + 1) == false)) {
                ++count;
            }

            output.push_back(uint8_t(count - 1));
            output.insert(output.end(), row + x * pixelSize, row + (x + count) * pixelSize);
            x += count;
        }
    }

    // Other programs may let a packet cross rows, so the pixels are read as a single sequence.
    //---------------------------------
    bool
    Decompress(const uint8_t *data, size_t size, uint8_t *pixels, size_t pixelCount, size_t pixelSize) {
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
                if(size - read < pixelSize) {
                    return false;
                }
                for(size_t i = 0; i < count; ++i) {
                    memcpy(pixels + (written + i) * pixelSize, data + read, pixelSize);
                }
                read += pixelSize;
            }
            else {
                const size_t bytes = count * pixelSize;
                if(size - read < bytes) {
                    return false;
                }
                memcpy(pixels + written * pixelSize, data + read, bytes);
                read += bytes;
            }
            written += count;
        }

        return true;
    }

} // end of namespace

//-------------------------------------
bool
MindShake::WriteTga(const char *fileName, const uint8_t *pixels, uint32_t width, uint32_t height, size_t stride, uint32_t channels, bool compress) {
    if(fileName == nullptr || pixels == nullptr || width == 0 || height == 0 || width > kMaxSize || height > kMaxSize ||
       IsValidChannelCount(channels) == false || stride < size_t(width) * channels) {
        return false;
    }

    const bool           gray = channels == 1;
    std::vector<uint8_t> output(kHeaderSize, 0);
    output[1] = kNoColorMap;
    if(gray) {
        output[2] = compress ? kCompressedGray : kUncompressedGray;
    }
    else {
        output[2] = compress ? kCompressedColor : kUncompressedColor;
    }
    SetU16(&output[12], width);
    SetU16(&output[14], height);
    output[16] = uint8_t(channels * 8);
    output[17] = uint8_t(kTopToBottom | (gray ? 0 : kAlphaBits));

    const size_t rowSize = size_t(width) * channels;
    for(uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = pixels + y * stride;
        if(compress) {
            CompressRow(row, width, channels, output);
        }
        else {
            output.insert(output.end(), row, row + rowSize);
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
MindShake::ReadTga(const uint8_t *data, size_t size, std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height, uint32_t &channels) {
    if(data == nullptr || size < kHeaderSize) {
        return false;
    }

    const size_t   idLength    = data[0];
    const uint8_t  type        = data[2];
    const uint8_t  descriptor  = data[17];
    const uint32_t imageWidth  = GetU16(&data[12]);
    const uint32_t imageHeight = GetU16(&data[14]);
    const bool     gray        = type == kUncompressedGray || type == kCompressedGray;
    const bool     color       = type == kUncompressedColor || type == kCompressedColor;
    const uint32_t pixelSize   = data[16] / 8u;
    // A color image without alpha is rejected: a font texture needs the alpha.
    const bool     validSize   = (data[16] % 8 == 0) && (gray ? (pixelSize == 1 || pixelSize == 2) : pixelSize == 4);
    if(data[1] != kNoColorMap || (gray == false && color == false) || validSize == false ||
       (descriptor & kRightToLeft) != 0 || imageWidth == 0 || imageHeight == 0 || size - kHeaderSize < idLength) {
        return false;
    }

    // Anything after the pixels is ignored, except what the extension area of TGA 2.0 says about the alpha.
    const uint8_t *source     = data + kHeaderSize + idLength;
    const size_t  sourceSize  = size - kHeaderSize - idLength;
    const size_t  pixelCount  = size_t(imageWidth) * imageHeight;
    const size_t  byteCount   = pixelCount * pixelSize;
    if(type == kUncompressedGray || type == kUncompressedColor) {
        if(sourceSize < byteCount) {
            return false;
        }
        pixels.assign(source, source + byteCount);
    }
    else {
        pixels.resize(byteCount);
        if(Decompress(source, sourceSize, pixels.data(), pixelCount, pixelSize) == false) {
            return false;
        }
    }

    if((descriptor & kTopToBottom) == 0) {
        const size_t rowSize = size_t(imageWidth) * pixelSize;
        for(uint32_t y = 0; y < imageHeight / 2; ++y) {
            uint8_t *top    = &pixels[size_t(y) * rowSize];
            uint8_t *bottom = &pixels[size_t(imageHeight - 1 - y) * rowSize];
            std::swap_ranges(top, top + rowSize, bottom);
        }
    }

    if(pixelSize > 1) {
        EAlpha alpha = GetDeclaredAlpha(data, size);
        if(alpha == EAlpha::None) {
            if(AlphaVaries(pixels, pixelSize) == false) {
                return false;
            }
            alpha = EAlpha::Straight;
        }

        if(pixelSize == 2) {
            ConvertGrayToColor(pixels, pixelCount);
        }
        if(alpha == EAlpha::Premultiplied) {
            Unpremultiply(pixels);
        }
    }

    width    = imageWidth;
    height   = imageHeight;
    channels = (pixelSize > 1) ? 4 : 1;

    return true;
}
