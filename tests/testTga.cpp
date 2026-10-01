#include "Tga.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

using namespace MindShake;

//-------------------------------------
namespace {

    constexpr const char *kTgaPath = FONT_RENDERER_TEST_OUTPUT "test.tga";

    //---------------------------------
    std::vector<uint8_t>
    ReadFile(const char *path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    //---------------------------------
    std::vector<uint8_t>
    WriteAndRead(const uint8_t *image, uint32_t width, uint32_t height, size_t stride, bool compress, uint32_t channels = 1) {
        REQUIRE(WriteTga(kTgaPath, image, width, height, stride, channels, compress));
        return ReadFile(kTgaPath);
    }

    //---------------------------------
    bool
    Read(const std::vector<uint8_t> &file, std::vector<uint8_t> &pixels, ETgaAlpha *alpha = nullptr) {
        uint32_t  width, height, channels;
        ETgaAlpha readAlpha;
        const bool read = ReadTga(file.data(), file.size(), pixels, width, height, channels, readAlpha);
        if(alpha != nullptr) {
            *alpha = readAlpha;
        }
        return read;
    }

    // An uncompressed or RLE file of the given pixels. The default descriptor stores the rows from the bottom up, without alpha bits.
    //---------------------------------
    std::vector<uint8_t>
    MakeFile(uint8_t type, uint32_t width, uint32_t height, std::vector<uint8_t> data, uint8_t bitsPerPixel = 8, uint8_t descriptor = 0) {
        std::vector<uint8_t> file(18, 0);
        file[2]  = type;
        file[12] = uint8_t(width);
        file[14] = uint8_t(height);
        file[16] = bitsPerPixel;
        file[17] = descriptor;
        file.insert(file.end(), data.begin(), data.end());
        return file;
    }

    // Adds the extension area and the footer of TGA 2.0, which only fill what the alpha holds.
    //---------------------------------
    std::vector<uint8_t>
    AddExtension(std::vector<uint8_t> file, uint8_t attributesType) {
        const size_t kExtensionSize = 495;
        const size_t offset         = file.size();
        file.resize(offset + kExtensionSize, 0);
        file[offset]       = uint8_t(kExtensionSize);
        file[offset + 1]   = uint8_t(kExtensionSize >> 8);
        file[offset + 494] = attributesType;

        for(int shift = 0; shift < 32; shift += 8) {
            file.push_back(uint8_t(offset >> shift));
        }
        file.insert(file.end(), 4, 0);                  // No developer area
        const char signature[] = "TRUEVISION-XFILE.";
        file.insert(file.end(), signature, signature + sizeof(signature));
        return file;
    }

    constexpr uint8_t kTopToBottom          = 0x20;
    constexpr uint8_t kTopToBottomWithAlpha = 0x28;

} // end of namespace

//-------------------------------------
TEST_CASE("TGA keeps the pixels it writes") {
    // Runs longer than a packet, and pixels that change at every step.
    const uint32_t kWidth = 300, kHeight = 7, kStride = 311;
    std::mt19937         rng(3);
    std::vector<uint8_t> image(kStride * kHeight);
    std::vector<uint8_t> expected;
    for(uint32_t y = 0; y < kHeight; ++y) {
        for(uint32_t x = 0; x < kStride; ++x) {
            const bool noise = (x / 40 + y) % 3 == 0;
            image[y * kStride + x] = noise ? uint8_t(rng()) : uint8_t(y * 10);
            if(x < kWidth) {
                expected.push_back(image[y * kStride + x]);
            }
        }
    }

    for(bool compress : { false, true }) {
        CAPTURE(compress);
        const std::vector<uint8_t> file = WriteAndRead(image.data(), kWidth, kHeight, kStride, compress);

        std::vector<uint8_t> pixels;
        uint32_t             width = 0, height = 0, channels = 0;
        ETgaAlpha            alpha;
        REQUIRE(ReadTga(file.data(), file.size(), pixels, width, height, channels, alpha));
        CHECK(width    == kWidth);
        CHECK(height   == kHeight);
        CHECK(channels == 1);
        CHECK(pixels   == expected);
        if(compress) {
            CHECK(file.size() < 18 + expected.size());
        }
        else {
            CHECK(file.size() == 18 + expected.size());
        }
    }
}

//-------------------------------------
TEST_CASE("TGA keeps the pixels of a 32-bit image") {
    const uint32_t kWidth = 200, kHeight = 5, kStride = 4 * 203;
    std::mt19937         rng(5);
    std::vector<uint8_t> image(kStride * kHeight);
    std::vector<uint8_t> expected;
    for(uint32_t y = 0; y < kHeight; ++y) {
        for(uint32_t x = 0; x < kStride; ++x) {
            // Runs of equal pixels, and pixels that change at every step.
            const bool noise = (x / 60 + y) % 2 == 0;
            image[y * kStride + x] = noise ? uint8_t(rng()) : uint8_t(y * 10 + x % 4);
            if(x < kWidth * 4) {
                expected.push_back(image[y * kStride + x]);
            }
        }
    }

    for(bool compress : { false, true }) {
        CAPTURE(compress);
        const std::vector<uint8_t> file = WriteAndRead(image.data(), kWidth, kHeight, kStride, compress, 4);
        CHECK(file[2]  == (compress ? 10 : 2));
        CHECK(file[16] == 32);
        CHECK(file[17] == 0x28);

        std::vector<uint8_t> pixels;
        uint32_t             width = 0, height = 0, channels = 0;
        ETgaAlpha            alpha;
        REQUIRE(ReadTga(file.data(), file.size(), pixels, width, height, channels, alpha));
        CHECK(width    == kWidth);
        CHECK(height   == kHeight);
        CHECK(channels == 4);
        CHECK(alpha    == ETgaAlpha::Straight);
        CHECK(pixels   == expected);
    }
}

//-------------------------------------
TEST_CASE("TGA marks a premultiplied alpha in the extension area of TGA 2.0") {
    const uint8_t image[] = { 20, 40, 60, 80,  0, 0, 0, 0 };

    // A straight alpha needs no extension area: the alpha bits of the header already say it.
    const std::vector<uint8_t> straight = WriteAndRead(image, 2, 1, 8, false, 4);
    CHECK(straight.size() == 18 + sizeof(image));

    REQUIRE(WriteTga(kTgaPath, image, 2, 1, 8, 4, false, ETgaAlpha::Premultiplied));
    const std::vector<uint8_t> premultiplied = ReadFile(kTgaPath);
    CHECK(premultiplied.size() == 18 + sizeof(image) + 495 + 26);

    std::vector<uint8_t> pixels;
    ETgaAlpha            alpha = ETgaAlpha::Straight;
    REQUIRE(Read(premultiplied, pixels, &alpha));
    CHECK(alpha  == ETgaAlpha::Premultiplied);
    CHECK(pixels == std::vector<uint8_t>(image, image + sizeof(image)));

    // Gray has no alpha to mark.
    REQUIRE(WriteTga(kTgaPath, image, 8, 1, 8, 1, false, ETgaAlpha::Premultiplied));
    CHECK(ReadFile(kTgaPath).size() == 18 + sizeof(image));
}

//-------------------------------------
TEST_CASE("TGA stores the color channels as they are in memory: blue, green, red and alpha") {
    const uint8_t              image[] = { 1, 2, 3, 4 };
    const std::vector<uint8_t> file    = WriteAndRead(image, 1, 1, 4, false, 4);

    CHECK(std::vector<uint8_t>(file.begin() + 18, file.end()) == std::vector<uint8_t> { 1, 2, 3, 4 });
}

//-------------------------------------
TEST_CASE("TGA reads 16-bit grayscale with alpha as color") {
    std::vector<uint8_t> pixels;
    uint32_t             width = 0, height = 0, channels = 0;
    ETgaAlpha            alpha;

    const std::vector<uint8_t> uncompressed = MakeFile(3, 2, 1, { 10, 20, 30, 40 }, 16, kTopToBottomWithAlpha);
    REQUIRE(ReadTga(uncompressed.data(), uncompressed.size(), pixels, width, height, channels, alpha));
    CHECK(channels == 4);
    CHECK(pixels == std::vector<uint8_t> { 10, 10, 10, 20, 30, 30, 30, 40 });

    const std::vector<uint8_t> compressed = MakeFile(11, 3, 1, { 0x81, 10, 20, 0x00, 30, 40 }, 16, kTopToBottomWithAlpha);
    REQUIRE(ReadTga(compressed.data(), compressed.size(), pixels, width, height, channels, alpha));
    CHECK(channels == 4);
    CHECK(pixels == std::vector<uint8_t> { 10, 10, 10, 20, 10, 10, 10, 20, 30, 30, 30, 40 });
}

//-------------------------------------
TEST_CASE("TGA rejects an alpha the image says it does not have, unless it varies") {
    std::vector<uint8_t> pixels;

    CHECK_FALSE(Read(MakeFile(2, 2, 1, { 1, 2, 3, 0, 4, 5, 6, 0 }, 32, kTopToBottom), pixels));
    CHECK_FALSE(Read(MakeFile(3, 2, 1, { 1, 255, 2, 255 }, 16, kTopToBottom), pixels));

    // Some programs do not declare the alpha they write.
    REQUIRE(Read(MakeFile(2, 2, 1, { 1, 2, 3, 0, 4, 5, 6, 9 }, 32, kTopToBottom), pixels));
    CHECK(pixels == std::vector<uint8_t> { 1, 2, 3, 0, 4, 5, 6, 9 });

    // A declared alpha is kept, even if it is the same in every pixel.
    REQUIRE(Read(MakeFile(2, 2, 1, { 1, 2, 3, 0, 4, 5, 6, 0 }, 32, kTopToBottomWithAlpha), pixels));
    CHECK(pixels == std::vector<uint8_t> { 1, 2, 3, 0, 4, 5, 6, 0 });
}

//-------------------------------------
TEST_CASE("TGA reads what the extension area of TGA 2.0 says about the alpha") {
    std::vector<uint8_t> pixels;
    ETgaAlpha            alpha = ETgaAlpha::Straight;

    // The pixels are not converted: whoever reads them decides what to do with a premultiplied alpha.
    const std::vector<uint8_t> premultiplied = AddExtension(MakeFile(2, 2, 1, { 0, 20, 40, 51, 0, 0, 0, 0 }, 32, kTopToBottomWithAlpha), 4);
    REQUIRE(Read(premultiplied, pixels, &alpha));
    CHECK(alpha  == ETgaAlpha::Premultiplied);
    CHECK(pixels == std::vector<uint8_t> { 0, 20, 40, 51, 0, 0, 0, 0 });

    const std::vector<uint8_t> straight = AddExtension(MakeFile(2, 1, 1, { 0, 20, 40, 51 }, 32, kTopToBottom), 3);
    REQUIRE(Read(straight, pixels, &alpha));
    CHECK(alpha  == ETgaAlpha::Straight);
    CHECK(pixels == std::vector<uint8_t> { 0, 20, 40, 51 });

    // The extension area is more precise than the alpha bits of the header.
    CHECK_FALSE(Read(AddExtension(MakeFile(2, 1, 1, { 1, 2, 3, 4 }, 32, kTopToBottomWithAlpha), 0), pixels));

    // An extension area outside the file is ignored, and then the header says there is alpha.
    std::vector<uint8_t> broken = AddExtension(MakeFile(2, 1, 1, { 1, 2, 3, 4 }, 32, kTopToBottomWithAlpha), 0);
    broken[broken.size() - 26 + 3] = 0x7f;
    CHECK(Read(broken, pixels));
}

//-------------------------------------
TEST_CASE("TGA compresses runs of two or more equal pixels") {
    const uint8_t              image[] = { 5, 5, 5, 1, 2, 3, 3 };
    const std::vector<uint8_t> file    = WriteAndRead(image, 7, 1, 7, true);

    const std::vector<uint8_t> packets(file.begin() + 18, file.end());
    CHECK(packets == std::vector<uint8_t> { 0x82, 5, 0x01, 1, 2, 0x81, 3 });

    // Two pixels that only differ in one channel are not a run.
    const uint8_t              color[] = { 1, 2, 3, 4,  1, 2, 3, 4,  1, 2, 3, 5 };
    const std::vector<uint8_t> colorFile = WriteAndRead(color, 3, 1, 12, true, 4);

    const std::vector<uint8_t> colorPackets(colorFile.begin() + 18, colorFile.end());
    CHECK(colorPackets == std::vector<uint8_t> { 0x81, 1, 2, 3, 4, 0x00, 1, 2, 3, 5 });
}

//-------------------------------------
TEST_CASE("TGA reads rows stored from the bottom up") {
    const std::vector<uint8_t> uncompressed = MakeFile(3, 3, 2, { 4, 5, 6, 1, 2, 3 });
    const std::vector<uint8_t> compressed   = MakeFile(11, 3, 2, { 0x02, 4, 5, 6, 0x82, 1 });

    std::vector<uint8_t> pixels;
    REQUIRE(Read(uncompressed, pixels));
    CHECK(pixels == std::vector<uint8_t> { 1, 2, 3, 4, 5, 6 });
    REQUIRE(Read(compressed, pixels));
    CHECK(pixels == std::vector<uint8_t> { 1, 1, 1, 4, 5, 6 });

    const std::vector<uint8_t> color = MakeFile(2, 1, 2, { 30, 20, 10, 40, 3, 2, 1, 4 }, 32);
    REQUIRE(Read(color, pixels));
    CHECK(pixels == std::vector<uint8_t> { 3, 2, 1, 4, 30, 20, 10, 40 });
}

//-------------------------------------
TEST_CASE("TGA reads packets that cross rows") {
    std::vector<uint8_t> pixels;
    REQUIRE(Read(MakeFile(11, 2, 3, { 0x84, 9, 0x00, 7 }), pixels));
    CHECK(pixels == std::vector<uint8_t> { 9, 7, 9, 9, 9, 9 });

    // The fewest bytes an image can take: only runs of 128 pixels.
    REQUIRE(Read(MakeFile(11, 16, 16, { 0xff, 7, 0xff, 7 }), pixels));
    CHECK(pixels == std::vector<uint8_t>(256, 7));
}

//-------------------------------------
TEST_CASE("TGA skips the image ID and ignores what follows the pixels") {
    const uint8_t image[] = { 7, 8 };
    for(bool compress : { false, true }) {
        CAPTURE(compress);
        std::vector<uint8_t> file = WriteAndRead(image, 2, 1, 2, compress);
        file[0] = 3;
        file.insert(file.begin() + 18, { 'I', 'D', '!' });
        file.insert(file.end(), { 'T', 'R', 'U', 'E', 'V', 'I', 'S', 'I', 'O', 'N' });

        std::vector<uint8_t> pixels;
        REQUIRE(Read(file, pixels));
        CHECK(pixels == std::vector<uint8_t> { 7, 8 });
    }
}

//-------------------------------------
TEST_CASE("TGA rejects the images it cannot read") {
    const uint8_t              image[] = { 1, 2, 3, 4 };
    const std::vector<uint8_t> valid   = WriteAndRead(image, 2, 2, 2, false);

    struct Change {
        size_t  offset;
        uint8_t value;
    };
    const Change changes[] = {
        {  1, 1    },   // Color map
        {  2, 1    },   // Image with a color map
        {  2, 2    },   // Color with 8 bits per pixel
        {  2, 10   },   // Compressed color with 8 bits per pixel
        { 16, 24   },   // Gray with 24 bits per pixel
        { 16, 32   },   // Gray with 32 bits per pixel
        { 16, 12   },   // Bits per pixel that are not whole bytes
        { 17, 0x30 },   // From right to left
        { 12, 0    },   // Width 0
        { 14, 0    },   // Height 0
        {  0, 200  },   // Image ID longer than the file
    };
    for(const Change &change : changes) {
        CAPTURE(change.offset);
        std::vector<uint8_t> file = valid;
        file[change.offset] = change.value;

        std::vector<uint8_t> pixels;
        CHECK_FALSE(Read(file, pixels));
    }

    std::vector<uint8_t> pixels;
    uint32_t             width, height, channels;
    ETgaAlpha            alpha;
    CHECK_FALSE(ReadTga(valid.data(), valid.size() - 1, pixels, width, height, channels, alpha));
    CHECK_FALSE(ReadTga(valid.data(), 17, pixels, width, height, channels, alpha));
    CHECK_FALSE(ReadTga(nullptr, 0, pixels, width, height, channels, alpha));

    // 32768 x 32768 pixels of 4 bytes are 4 GiB, which a 32-bit size_t counts as 0 bytes.
    std::vector<uint8_t> huge = MakeFile(2, 0, 0, {}, 32);
    huge[13] = 0x80;
    huge[15] = 0x80;
    CHECK_FALSE(Read(huge, pixels));

    // Compressed, it needs a packet for every 128 pixels. With a single one, it is rejected before reserving the 4 GiB.
    huge[2] = 10;
    huge.insert(huge.end(), { 0xff, 1, 2, 3, 4 });
    std::vector<uint8_t> untouched;
    CHECK_FALSE(Read(huge, untouched));
    CHECK(untouched.empty());

    // Color without alpha.
    CHECK_FALSE(Read(MakeFile(2, 1, 1, { 1, 2, 3 }, 24), pixels));
    CHECK(Read(MakeFile(2, 1, 1, { 1, 2, 3, 4 }, 32, kTopToBottomWithAlpha), pixels));
    // A color run needs the four bytes of its pixel.
    CHECK_FALSE(Read(MakeFile(10, 2, 1, { 0x81, 1, 2, 3 }, 32), pixels));
    CHECK_FALSE(Read(MakeFile(10, 2, 1, { 0x01, 1, 2, 3, 4, 5, 6, 7 }, 32), pixels));
}

//-------------------------------------
TEST_CASE("TGA rejects broken RLE packets") {
    std::vector<uint8_t> pixels;

    CHECK_FALSE(Read(MakeFile(11, 2, 2, { 0x83 }), pixels));                   // A run without its pixel
    CHECK_FALSE(Read(MakeFile(11, 2, 2, { 0x03, 1, 2, 3 }), pixels));          // A copy without all its pixels
    CHECK_FALSE(Read(MakeFile(11, 2, 2, { 0x84, 1 }), pixels));                // More pixels than the image
    CHECK_FALSE(Read(MakeFile(11, 2, 2, { 0x81, 1 }), pixels));                // Fewer pixels than the image
    CHECK(Read(MakeFile(11, 2, 2, { 0x81, 1, 0x01, 2, 3 }), pixels));
}

//-------------------------------------
TEST_CASE("TGA rejects the images it cannot write") {
    const uint8_t image[] = { 1, 2, 3, 4 };

    CHECK_FALSE(WriteTga(kTgaPath, image, 0, 1, 1, 1, false));
    CHECK_FALSE(WriteTga(kTgaPath, image, 1, 0, 1, 1, false));
    CHECK_FALSE(WriteTga(kTgaPath, image, 2, 1, 1, 1, false));
    CHECK_FALSE(WriteTga(kTgaPath, image, 1, 1, 3, 4, false));
    CHECK_FALSE(WriteTga(kTgaPath, image, 1, 1, 4, 3, false));
    CHECK_FALSE(WriteTga(kTgaPath, image, 1, 1, 4, 0, false));
    CHECK_FALSE(WriteTga(kTgaPath, nullptr, 1, 1, 1, 1, false));
    CHECK_FALSE(WriteTga(nullptr, image, 1, 1, 1, 1, false));
    CHECK_FALSE(WriteTga(FONT_RENDERER_TEST_OUTPUT "missing/test.tga", image, 1, 1, 1, 1, true));
}
