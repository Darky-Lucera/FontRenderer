#pragma once

#include <FontBase.h>
#include <Platform.h>
//-------------------------------------
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

//-------------------------------------
namespace Benchmark {

    //---------------------------------
    struct Scenario {
        std::string                         name;
        MindShake::FontBase                 *font {};
        std::string                         text;
        uint8_t                             size {};
        // The variants only know it at run time, as DrawText does. With a constant, the compiler
        // could simplify the blend of the variants, but not the one of DrawText.
        uint32_t                            color {};
        std::vector<MindShake::GlyphQuad>   quads;
        uint32_t                            width  {};      // Of the buffer, which fits the text and a margin
        uint32_t                            height {};
        int32_t                             posX   {};
        int32_t                             posY   {};
    };

    // Draws the quads of the scenario into dst, which is width x height pixels.
    using DrawFunction = void (*)(Scenario &scenario, uint32_t *dst);

    // The color with its red, green and blue multiplied by its alpha, rounded as Blinn does.
    uint32_t                PremultiplyColor(uint32_t color);

    // The helpers that several variants share. The namespace is unnamed so that each variant file has its own copy
    // of them, as when each file defined them itself: sharing them changes the code of no variant. A variant with
    // another version of one of them defines it in its own namespace, which hides the one here.
    //---------------------------------
    namespace {

        // Two channels of 0xAARRGGBB are blended at once, each in 16 bits of a uint32_t: blue and red, and green and alpha.
        constexpr uint32_t kPairMask = 0x00ff00ff;

        // (a * b) / 255 rounded to the nearest integer, exact for any a and b from 0 to 255 (Blinn).
        // SSE2 computes the same with _mm_mulhi_epu16 and 0x0101.
        //-----------------------------
        inline uint32_t
        MulDiv255(uint32_t a, uint32_t b) {
            const uint32_t t = a * b + 0x80;
            return ((t >> 8) + t) >> 8;
        }

        // The values in the bits 0 to 15 and 16 to 31 divided by 255, rounded as Blinn does. Exact for values up to 255 * 255.
        //-----------------------------
        inline uint32_t
        Div255Pair(uint32_t values) {
            const uint32_t t = values + 0x00800080;
            return ((t + ((t >> 8) & kPairMask)) >> 8) & kPairMask;
        }

        // The products, channel by channel, of the channels in the bits 0 to 7 and 16 to 23, in the bits 0 to 15 and 16 to 31.
        //-----------------------------
        inline uint32_t
        MulPairByPair(uint32_t x, uint32_t a) {
            return (x & 0xff) * (a & 0xff) | (x & 0x00ff0000) * ((a >> 16) & 0xff);
        }

        // (source + pixel * (255 - alpha)) / 255 for the 4 channels, with the source already multiplied: blue and red
        // in sourceBlueRed, green and alpha in sourceGreenAlpha, each in 16 bits.
        //-----------------------------
        inline uint32_t
        Blend(uint32_t pixel, uint32_t sourceBlueRed, uint32_t sourceGreenAlpha, uint32_t alpha) {
            const uint32_t inverse    = 255 - alpha;
            const uint32_t blueRed    = Div255Pair(sourceBlueRed    + (pixel & kPairMask) * inverse);
            const uint32_t greenAlpha = Div255Pair(sourceGreenAlpha + ((pixel >> 8) & kPairMask) * inverse);
            return blueRed | (greenAlpha << 8);
        }

        // The 4 bytes of a texel as 0xAARRGGBB, on any processor.
        //-----------------------------
        inline uint32_t
        LoadBGRA(const uint8_t *texel) {
            return uint32_t(texel[0]) | (uint32_t(texel[1]) << 8) | (uint32_t(texel[2]) << 16) | (uint32_t(texel[3]) << 24);
        }

        // Calls blend(texel, pixel) for each texel of a glyph, from offset, and the pixel of dst it goes to.
        //-----------------------------
        template <size_t kBytesPerTexel, class TBlend>
        void
        DrawTexels(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                   uint32_t *dst, uint32_t dstStride, TBlend blend) {
            for (int32_t y = 0; y < height; ++y, offset += stepY) {
                uint32_t *row   = &dst[size_t(y) * dstStride];
                size_t   texel  = offset;
                for (int32_t x = 0; x < width; ++x, texel += stepX) {
                    blend(&texture[texel * kBytesPerTexel], row[x]);
                }
            }
        }

        // The 4 channels of a pixel, each in 16 bits of a uint64_t.
        constexpr uint64_t kChannelMask = 0x00ff00ff00ff00ffull;

        // 0xAARRGGBB to 0x00AA00RR00GG00BB.
        //-----------------------------
        inline uint64_t
        Spread(uint32_t color) {
            uint64_t x = color;
            x = (x | (x << 16)) & 0x0000ffff0000ffffull;
            return (x | (x << 8)) & kChannelMask;
        }

        // 0x00AA00RR00GG00BB to 0xAARRGGBB.
        //-----------------------------
        inline uint32_t
        Pack(uint64_t x) {
            x = (x | (x >> 8)) & 0x0000ffff0000ffffull;
            return uint32_t(x | (x >> 16));
        }

        // x / 255 rounded to the nearest integer, exact for any x up to 255 * 255, the largest product of two channels.
        //-----------------------------
        inline uint32_t
        Round255(uint32_t x) {
            const uint32_t t = x + 128;
            return (t + (t >> 8)) >> 8;
        }

    } // end of namespace

    // Each variant is in its own file, variants/<Name>.cpp, and is added to kVariants in benchmarkDraw.cpp.
    // Every color texture is premultiplied, as FontBaked keeps it. The variants written before it was take the texture
    // as straight, so they leave other pixels in BGRA32, but their times still compare.
    namespace Baseline {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawScalar(Scenario &scenario, uint32_t *dst);
#if defined(FONTRENDERER_SSE2) && !defined(FONTRENDERER_X86_64_V2)
        void    DrawSse2(Scenario &scenario, uint32_t *dst);
#endif
#if defined(FONTRENDERER_SSE2) && (defined(FONTRENDERER_X86_64_V2) || defined(FONTRENDERER_X86_64_V2_AT_RUNTIME))
        void    DrawX64v2(Scenario &scenario, uint32_t *dst);
#endif
    }
    namespace MoreJumpsPerGlyph {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsPerCase {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsPerCaseAlpha8 {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Truncate {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumps {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsPremultiplied {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsPremultipliedMia {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsNoInline {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsNoInlineCast {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsHybrid {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsAlpha8 {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace MoreJumpsAlpha8Hoisted {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace NoJumps {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Pixel32 {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Restrict {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Div255 {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace RoundOnce {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace RoundEach {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Swar {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Premultiplied {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Scalar {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace ScalarSwar {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Scalar64 {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace RoundOnceOver {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace RoundOnce64 {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace RoundOnceSwar {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace RoundOnceSwarPremultiplied {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace RoundOnce64Premultiplied {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Linear {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }

    // The variants with SSE2, which are only built for a processor that has it.
#if defined(FONTRENDERER_SSE2)
    namespace RoundOnceSse2Premultiplied {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Sse2PerCase {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawTail(Scenario &scenario, uint32_t *dst);
    }
    namespace Sse2Overlap {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawPixels(Scenario &scenario, uint32_t *dst);
        void    DrawLate(Scenario &scenario, uint32_t *dst);
        void    DrawPairTail(Scenario &scenario, uint32_t *dst);
        void    DrawNoNarrowRows(Scenario &scenario, uint32_t *dst);
        void    DrawNoBands(Scenario &scenario, uint32_t *dst);
        void    DrawRowShortcuts(Scenario &scenario, uint32_t *dst);
        void    DrawPixman(Scenario &scenario, uint32_t *dst);
        void    DrawUprightApart(Scenario &scenario, uint32_t *dst);
        void    DrawOctets(Scenario &scenario, uint32_t *dst);
        void    DrawBGRATint(Scenario &scenario, uint32_t *dst);
        void    DrawSkip(Scenario &scenario, uint32_t *dst);
        void    DrawBranchless(Scenario &scenario, uint32_t *dst);
        void    DrawOpaque8(Scenario &scenario, uint32_t *dst);
        void    DrawOpaque24(Scenario &scenario, uint32_t *dst);
        void    DrawTranslucent0(Scenario &scenario, uint32_t *dst);
        void    DrawTranslucent16(Scenario &scenario, uint32_t *dst);
        void    DrawWidths(Scenario &scenario, uint32_t *dst);
        void    DrawBandWidths(Scenario &scenario, uint32_t *dst);
        void    DrawWidthsBandWidths(Scenario &scenario, uint32_t *dst);
        void    DrawWidthIfs(Scenario &scenario, uint32_t *dst);
        void    DrawWidthIfsHidden(Scenario &scenario, uint32_t *dst);
    }
#if defined(FONTRENDERER_BENCHMARK_ALIGNED)
    namespace Sse2OverlapAligned {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawWidths(Scenario &scenario, uint32_t *dst);
        void    DrawBandWidths(Scenario &scenario, uint32_t *dst);
        void    DrawWidthsBandWidths(Scenario &scenario, uint32_t *dst);
    }
#endif
    namespace Sse2Spread {
        void    DrawUnpack(Scenario &scenario, uint32_t *dst);
        void    DrawShift(Scenario &scenario, uint32_t *dst);
        void    DrawOr(Scenario &scenario, uint32_t *dst);
    }
    namespace Pixman {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace PixmanUnaligned {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace PixmanRotated {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace PixmanHybrid {
        void    Draw8(Scenario &scenario, uint32_t *dst);
        void    Draw16(Scenario &scenario, uint32_t *dst);
        void    Draw24(Scenario &scenario, uint32_t *dst);
    }
    namespace Sse2 {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
    namespace Sse2RoundOnce {
        void    Draw(Scenario &scenario, uint32_t *dst);
    }
#endif

    // The variants with the instructions of x86-64-v2, which CMake only builds when the compiler can target that level.
#if defined(FONTRENDERER_BENCHMARK_X64V2)
    namespace X64v2 {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawNoMadd(Scenario &scenario, uint32_t *dst);
        void    DrawNoShuffle(Scenario &scenario, uint32_t *dst);
        void    DrawNoTest(Scenario &scenario, uint32_t *dst);
        void    DrawNoUnroll(Scenario &scenario, uint32_t *dst);
        void    DrawSse2(Scenario &scenario, uint32_t *dst);
        void    DrawWidths(Scenario &scenario, uint32_t *dst);
        void    DrawPairs(Scenario &scenario, uint32_t *dst);
        void    DrawWidthsPairs(Scenario &scenario, uint32_t *dst);
        void    DrawBandWidths(Scenario &scenario, uint32_t *dst);
        void    DrawWidthsBandWidths(Scenario &scenario, uint32_t *dst);
    }
#if defined(FONTRENDERER_BENCHMARK_ALIGNED)
    namespace X64v2Aligned {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawWidths(Scenario &scenario, uint32_t *dst);
        void    DrawBandWidths(Scenario &scenario, uint32_t *dst);
        void    DrawWidthsBandWidths(Scenario &scenario, uint32_t *dst);
    }
#endif
#endif

    // The variants with the instructions of x86-64-v3, which CMake only builds when the compiler can target that level.
#if defined(FONTRENDERER_BENCHMARK_X64V3)
    namespace X64v3Vex {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawNoMadd(Scenario &scenario, uint32_t *dst);
        void    DrawNoShuffle(Scenario &scenario, uint32_t *dst);
        void    DrawNoTest(Scenario &scenario, uint32_t *dst);
        void    DrawNoUnroll(Scenario &scenario, uint32_t *dst);
        void    DrawSse2(Scenario &scenario, uint32_t *dst);
        void    DrawWidths(Scenario &scenario, uint32_t *dst);
        void    DrawPairs(Scenario &scenario, uint32_t *dst);
        void    DrawWidthsPairs(Scenario &scenario, uint32_t *dst);
        void    DrawBandWidths(Scenario &scenario, uint32_t *dst);
        void    DrawWidthsBandWidths(Scenario &scenario, uint32_t *dst);
    }
    namespace Avx2 {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawRows(Scenario &scenario, uint32_t *dst);
        void    DrawWide(Scenario &scenario, uint32_t *dst);
        void    DrawTall(Scenario &scenario, uint32_t *dst);
        void    DrawSplit(Scenario &scenario, uint32_t *dst);
        void    DrawSplitRowsWide(Scenario &scenario, uint32_t *dst);
        void    DrawSplitRowsTall(Scenario &scenario, uint32_t *dst);
        void    DrawSplitRowsWideNoUnroll(Scenario &scenario, uint32_t *dst);
        void    DrawSplitRowsTallNoUnroll(Scenario &scenario, uint32_t *dst);
        void    DrawSplitRowsHybridNoUnroll(Scenario &scenario, uint32_t *dst);
    }
#endif

    // The variants with NEON, which are only built for a processor that has it.
#if defined(FONTRENDERER_NEON)
    namespace NeonPerCase {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    Draw8(Scenario &scenario, uint32_t *dst);
        void    DrawTail(Scenario &scenario, uint32_t *dst);
        void    Draw8Tail(Scenario &scenario, uint32_t *dst);
    }
    namespace NeonOverlap {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawBranchless(Scenario &scenario, uint32_t *dst);
        void    DrawUzp(Scenario &scenario, uint32_t *dst);
        void    DrawBands(Scenario &scenario, uint32_t *dst);
        void    DrawBandsBranchless(Scenario &scenario, uint32_t *dst);
        void    DrawBandsSkip32(Scenario &scenario, uint32_t *dst);
    }
    namespace NeonPacked {
        void    Draw(Scenario &scenario, uint32_t *dst);
        void    DrawLd4(Scenario &scenario, uint32_t *dst);
        void    DrawNoQuad(Scenario &scenario, uint32_t *dst);
        void    DrawNoNarrow(Scenario &scenario, uint32_t *dst);
        void    DrawBranchless(Scenario &scenario, uint32_t *dst);
        void    DrawSkip(Scenario &scenario, uint32_t *dst);
    }
#endif

} // end of namespace
