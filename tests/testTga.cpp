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
    WriteAndRead(const uint8_t *image, uint32_t width, uint32_t height, size_t stride, bool compress) {
        REQUIRE(WriteTga(kTgaPath, image, width, height, stride, compress));
        return ReadFile(kTgaPath);
    }

    //---------------------------------
    bool
    Read(const std::vector<uint8_t> &file, std::vector<uint8_t> &pixels) {
        uint32_t width, height;
        return ReadTga(file.data(), file.size(), pixels, width, height);
    }

    // An uncompressed or RLE file of the given pixels, with its rows from the bottom up.
    //---------------------------------
    std::vector<uint8_t>
    MakeFile(uint8_t type, uint32_t width, uint32_t height, std::vector<uint8_t> data) {
        std::vector<uint8_t> file(18, 0);
        file[2]  = type;
        file[12] = uint8_t(width);
        file[14] = uint8_t(height);
        file[16] = 8;
        file.insert(file.end(), data.begin(), data.end());
        return file;
    }

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
        uint32_t             width = 0, height = 0;
        REQUIRE(ReadTga(file.data(), file.size(), pixels, width, height));
        CHECK(width  == kWidth);
        CHECK(height == kHeight);
        CHECK(pixels == expected);
        if(compress) {
            CHECK(file.size() < 18 + expected.size());
        }
        else {
            CHECK(file.size() == 18 + expected.size());
        }
    }
}

//-------------------------------------
TEST_CASE("TGA compresses runs of two or more equal pixels") {
    const uint8_t              image[] = { 5, 5, 5, 1, 2, 3, 3 };
    const std::vector<uint8_t> file    = WriteAndRead(image, 7, 1, 7, true);

    const std::vector<uint8_t> packets(file.begin() + 18, file.end());
    CHECK(packets == std::vector<uint8_t> { 0x82, 5, 0x01, 1, 2, 0x81, 3 });
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
}

//-------------------------------------
TEST_CASE("TGA reads packets that cross rows") {
    std::vector<uint8_t> pixels;
    REQUIRE(Read(MakeFile(11, 2, 3, { 0x84, 9, 0x00, 7 }), pixels));
    CHECK(pixels == std::vector<uint8_t> { 9, 7, 9, 9, 9, 9 });
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
        {  2, 2    },   // Color
        {  2, 10   },   // Compressed color
        { 16, 16   },   // Bits per pixel
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
    uint32_t             width, height;
    CHECK_FALSE(ReadTga(valid.data(), valid.size() - 1, pixels, width, height));
    CHECK_FALSE(ReadTga(valid.data(), 17, pixels, width, height));
    CHECK_FALSE(ReadTga(nullptr, 0, pixels, width, height));
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

    CHECK_FALSE(WriteTga(kTgaPath, image, 0, 1, 1, false));
    CHECK_FALSE(WriteTga(kTgaPath, image, 1, 0, 1, false));
    CHECK_FALSE(WriteTga(kTgaPath, image, 2, 1, 1, false));
    CHECK_FALSE(WriteTga(kTgaPath, nullptr, 1, 1, 1, false));
    CHECK_FALSE(WriteTga(nullptr, image, 1, 1, 1, false));
    CHECK_FALSE(WriteTga(FONT_RENDERER_TEST_OUTPUT "missing/test.tga", image, 1, 1, 1, true));
}
