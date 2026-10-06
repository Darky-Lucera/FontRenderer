#include "SkylineBinPack.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <random>
#include <vector>

using MindShake::SkylineBinPack;
using Rect      = SkylineBinPack::Rect;
using Size      = SkylineBinPack::Size;
using Heuristic = SkylineBinPack::ELevelChoiceHeuristic;

//-------------------------------------
namespace {

    constexpr Heuristic kHeuristics[] = { Heuristic::LevelBottomLeft, Heuristic::LevelMinWasteFit };

    //---------------------------------
    bool
    Overlap(const Rect &a, const Rect &b) {
        return a.Left() < b.Right() && b.Left() < a.Right() && a.Top() < b.Bottom() && b.Top() < a.Bottom();
    }

    //---------------------------------
    int
    CountOverlaps(const std::vector<Rect> &rects) {
        int overlaps = 0;
        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                if (Overlap(rects[i], rects[j])) {
                    ++overlaps;
                }
            }
        }
        return overlaps;
    }

    //---------------------------------
    int
    CountOutOfBin(const SkylineBinPack &packer, const std::vector<Rect> &rects) {
        int outside = 0;
        for (const Rect &rect : rects) {
            if (rect.Left() < 0 || rect.Top() < 0 || uint32_t(rect.Right()) > packer.GetWidth() || uint32_t(rect.Bottom()) > packer.GetHeight()) {
                ++outside;
            }
        }
        return outside;
    }

} // end of namespace

//-------------------------------------
TEST_CASE("SkylineBinPack rejects invalid sizes and keeps its state") {
    SkylineBinPack packer(64, 32);

    CHECK_FALSE(packer.Init(0, 10));
    CHECK_FALSE(packer.Init(10, 0));
    CHECK_FALSE(packer.Init(SkylineBinPack::kMaxBinSize + 1, 10));
    CHECK(packer.GetWidth()  == 64);
    CHECK(packer.GetHeight() == 32);

    CHECK_FALSE(packer.ResizeBin(32, 32));
    CHECK_FALSE(packer.ResizeBin(64, 16));
    CHECK(packer.ResizeBin(128, 64));
    CHECK(packer.GetWidth()  == 128);
    CHECK(packer.GetHeight() == 64);

    CHECK(packer.Insert(0, 5, Heuristic::LevelBottomLeft).width == 0);
    CHECK(packer.Insert(129, 1, Heuristic::LevelBottomLeft).width == 0);
    CHECK(packer.GetUsedSurfaceArea() == 0);
}

//-------------------------------------
TEST_CASE("SkylineBinPack places random rectangles inside the bin without overlaps") {
    std::mt19937 rng(1234);

    for (Heuristic heuristic : kHeuristics) {
        for (bool allowRotation : { false, true }) {
            CAPTURE(int(heuristic));
            CAPTURE(allowRotation);

            for (int bin = 0; bin < 40; ++bin) {
                // Separate statements: compilers evaluate function arguments in different orders.
                const uint32_t    binWidth     = 1 + rng() % 300;
                const uint32_t    binHeight    = 1 + rng() % 300;
                SkylineBinPack    packer(binWidth, binHeight, allowRotation);
                std::vector<Rect> rects;
                uint64_t          area         = 0;
                int               wrongSize    = 0;

                for (int i = 0; i < 200; ++i) {
                    if (rng() % 20 == 0) {
                        const uint32_t newWidth  = packer.GetWidth()  + rng() % 50;
                        const uint32_t newHeight = packer.GetHeight() + rng() % 50;
                        REQUIRE(packer.ResizeBin(newWidth, newHeight));
                    }

                    const uint32_t width  = 1 + rng() % 40;
                    const uint32_t height = 1 + rng() % 40;
                    const Rect     rect   = packer.Insert(width, height, heuristic);
                    if (rect.width <= 0) {
                        continue;
                    }

                    const bool same    = uint32_t(rect.width) == width  && uint32_t(rect.height) == height;
                    const bool rotated = uint32_t(rect.width) == height && uint32_t(rect.height) == width;
                    if (!same && !(allowRotation && rotated)) {
                        ++wrongSize;
                    }

                    rects.push_back(rect);
                    area += uint64_t(width) * height;
                }

                CHECK(wrongSize == 0);
                CHECK(CountOverlaps(rects) == 0);
                CHECK(CountOutOfBin(packer, rects) == 0);
                CHECK(packer.GetUsedSurfaceArea() == area);
            }
        }
    }
}

//-------------------------------------
TEST_CASE("SkylineBinPack batch insert keeps the input order") {
    SkylineBinPack          packer(64, 64, true);
    const std::vector<Size> kSizes = { { 10, 20 }, { 0, 5 }, { 100, 1 }, { 30, 30 }, { 5, 5 } };
    std::vector<Rect>       rects;

    const size_t placed = packer.Insert(kSizes, rects, Heuristic::LevelMinWasteFit);

    REQUIRE(rects.size() == kSizes.size());
    CHECK(placed == 3);
    CHECK(rects[1].width == 0);
    CHECK(rects[2].width == 0);
    for (size_t i : { size_t(0), size_t(3), size_t(4) }) {
        CAPTURE(i);
        CHECK(uint64_t(rects[i].width) * rects[i].height == uint64_t(kSizes[i].width) * kSizes[i].height);
    }
    CHECK(CountOverlaps({ rects[0], rects[3], rects[4] }) == 0);
}

//-------------------------------------
TEST_CASE("SkylineBinPack reports the used area") {
    SkylineBinPack packer(64, 32, false);
    CHECK(packer.GetUsedWidth()  == 0);
    CHECK(packer.GetUsedHeight() == 0);

    packer.Insert(10, 5, Heuristic::LevelBottomLeft);
    CHECK(packer.GetUsedWidth()  == 10);
    CHECK(packer.GetUsedHeight() == 5);

    packer.Reset();
    CHECK(packer.GetUsedWidth() == 0);
    CHECK(packer.GetUsedSurfaceArea() == 0);
}
