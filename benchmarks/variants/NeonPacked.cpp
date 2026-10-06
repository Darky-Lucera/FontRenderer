#include "benchmarkCommon.h"

#if defined(FONTRENDERER_NEON)

#include <arm_neon.h>
//-------------------------------------
#include <cstring>

// NeonOverlapBandsSkip32 with the pixels kept as they are in memory, BGRA after BGRA, instead of one register per
// channel. The coverage of each pixel, or the alpha of each texel, is copied over its 4 bytes with a table lookup. A row
// of 4 to 7 pixels then needs no reordering, and a block of 4 pixels costs half of a block of 8. vld4_u8 and vst4_u8,
// which reorder the channels, turned out slower on the Apple M1 than these lookups. The same pixels as DrawText.
//
// A row is drawn in blocks of 8 pixels. The 1 to 4 pixels left at its end are drawn with a block of 4 that ends where the
// row ends, and 5 to 7 with a block of 8. That last block overlaps the one before it, and both read their pixels before
// either writes, as in NeonOverlap. A row of 4 pixels is one block of 4, one of 5 or 6 a block of 4 and its last 2
// pixels, one of 7 its first 4 pixels and its last 4, and one of 1 to 3 is drawn 2 pixels at a time. Rotated glyphs of
// Alpha8 are drawn 4 rows at a time, as in NeonOverlapBands. Whether a block of BGRA32 can skip the blend is decided from
// the alpha bytes read as numbers, so that the branch does not wait for the NEON registers.
//
// Every instruction is in ARMv8.0, so any ARM64 processor runs it. AArch64 picks 16 bytes with one vqtbl1q_u8; 32-bit ARM
// needs two vtbl1_u8 or vtbl2_u8.
//
// Draw asks whether a block can skip the blend only in glyphs at least 32 pixels wide. Each of the other functions
// changes one thing. DrawLd4 draws the blocks of 8 with one register per channel, with vld4_u8 and vst4_u8, and BGRA32
// as NeonOverlap does. DrawNoQuad draws the 1 to 4 pixels left with a block of 8. DrawNoNarrow draws every row of 4 to 7
// pixels as its first 4 and its last 4. DrawBranchless never asks whether a block can skip the blend, and DrawSkip
// always asks.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace NeonPacked {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

#if defined(_MSC_VER)
    #define NO_INLINE     __declspec(noinline)
    #define ALWAYS_INLINE __forceinline
#else
    #define NO_INLINE     __attribute__((noinline))
    #define ALWAYS_INLINE inline __attribute__((always_inline))
#endif

        // Clang stops inlining the helpers once a glyph function has several of them, and then passes the color through
        // memory for every block. ALWAYS_INLINE keeps them inside the function of the glyph.

        using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                           uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

        // A glyph at least kSkipWidth pixels wide asks for each block whether it can skip the blend. Wide glyphs have long
        // runs of blocks without coverage or fully covered. In narrow glyphs those blocks are rare, and the branch costs
        // more than the blends it saves. kSkipAlways and kSkipNever ask for every glyph or for none.
        constexpr int32_t kSkipAlways = 0;
        constexpr int32_t kSkipNever  = -1;

        //-----------------------------
        template <int32_t kSkipWidth>
        ALWAYS_INLINE bool
        UsesShortcuts(int32_t width) {
            return kSkipWidth == kSkipAlways || (kSkipWidth != kSkipNever && width >= kSkipWidth);
        }

        // Indexes for vtbl1_u8 on 2 pixels. The first copies the byte 0 over the 4 channels of the first pixel and the
        // byte 1 over the second, which spreads a coverage. The second copies the alpha of each pixel over its channels.
        const uint8_t kSpreadCoverage[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };
        const uint8_t kSpreadAlpha[8]    = { 3, 3, 3, 3, 7, 7, 7, 7 };

        // The same for 4 pixels: the bytes 0 to 3 of a register, or the bytes 4 to 7, each over the 4 channels of a pixel.
        const uint8_t kSpreadLow[16]  = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 };
        const uint8_t kSpreadHigh[16] = { 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7 };

        // The bytes 6 and 7 of a register over the 4 channels of 2 pixels.
        const uint8_t kSpreadLast[8] = { 6, 6, 6, 6, 7, 7, 7, 7 };

        // The alpha of each of 4 pixels over its 4 channels.
        const uint8_t kSpreadAlphas[16] = { 3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15 };

        // The alpha bytes of 2 texels read as a number.
        constexpr uint64_t kAlphaBytes = 0xff000000ff000000ull;

        //-----------------------------
        struct Color {
            uint8x8x4_t planes;         // Each channel of the premultiplied color in every lane
            uint8x16_t  quad;           // The premultiplied color for 4 pixels
            uint8x8_t   pair;           // The premultiplied color for 2 pixels
            uint8x8_t   spreadCoverage;
            uint8x8_t   spreadAlpha;
            uint8x16_t  spreadLow;
            uint8x16_t  spreadHigh;
            uint8x16_t  spreadAlphas;
            uint8x8_t   spreadLast;
            bool        white;
        };

        //-----------------------------
        ALWAYS_INLINE Color
        MakeColor(uint32_t premultiplied) {
            Color color;
            color.pair           = vreinterpret_u8_u32(vdup_n_u32(premultiplied));
            color.planes.val[0]  = vdup_lane_u8(color.pair, 0);
            color.planes.val[1]  = vdup_lane_u8(color.pair, 1);
            color.planes.val[2]  = vdup_lane_u8(color.pair, 2);
            color.planes.val[3]  = vdup_lane_u8(color.pair, 3);
            color.quad           = vreinterpretq_u8_u32(vdupq_n_u32(premultiplied));
            color.spreadCoverage = vld1_u8(kSpreadCoverage);
            color.spreadAlpha    = vld1_u8(kSpreadAlpha);
            color.spreadLow      = vld1q_u8(kSpreadLow);
            color.spreadHigh     = vld1q_u8(kSpreadHigh);
            color.spreadAlphas   = vld1q_u8(kSpreadAlphas);
            color.spreadLast     = vld1_u8(kSpreadLast);
            color.white          = premultiplied == 0xffffffffu;
            return color;
        }

        // Each 16-bit lane divided by 255 and rounded, as an 8-bit lane. Exact up to 65152, the largest sum of a blend.
        //-----------------------------
        ALWAYS_INLINE uint8x8_t
        Div255(uint16x8_t x) {
            return vraddhn_u16(x, vrshrq_n_u16(x, 8));
        }

        // Rounded (source + dst * (255 - alpha)) / 255 in each byte, with the source already multiplied.
        //-----------------------------
        ALWAYS_INLINE uint8x8_t
        Blend(uint16x8_t source, uint8x8_t dst, uint8x8_t alpha) {
            return Div255(vmlal_u8(source, dst, vmvn_u8(alpha)));
        }

        // The 8 bytes of a register as a number, to test them all at once.
        //-----------------------------
        ALWAYS_INLINE uint64_t
        GetLanes(uint8x8_t x) {
            return vget_lane_u64(vreinterpret_u64_u8(x), 0);
        }

        // 16 bytes picked from the 8 of x by indexes from 0 to 7. AArch64 does it with one instruction.
        //-----------------------------
        ALWAYS_INLINE uint8x16_t
        Pick(uint8x8_t x, uint8x16_t indexes) {
#if defined(__aarch64__) || defined(_M_ARM64)
            return vqtbl1q_u8(vcombine_u8(x, vcreate_u8(0)), indexes);
#else
            return vcombine_u8(vtbl1_u8(x, vget_low_u8(indexes)), vtbl1_u8(x, vget_high_u8(indexes)));
#endif
        }

        // 16 bytes picked from the 16 of x by indexes from 0 to 15.
        //-----------------------------
        ALWAYS_INLINE uint8x16_t
        Pick16(uint8x16_t x, uint8x16_t indexes) {
#if defined(__aarch64__) || defined(_M_ARM64)
            return vqtbl1q_u8(x, indexes);
#else
            uint8x8x2_t table;
            table.val[0] = vget_low_u8(x);
            table.val[1] = vget_high_u8(x);
            return vcombine_u8(vtbl2_u8(table, vget_low_u8(indexes)), vtbl2_u8(table, vget_high_u8(indexes)));
#endif
        }

        // The 4 bytes at p as a number. NEON is only used on little-endian processors, so the first byte is the lowest.
        //-----------------------------
        ALWAYS_INLINE uint32_t
        Load32(const uint8_t *p) {
            uint32_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        // The coverage of 4 pixels in a row, a byte each. The texels of a rotated glyph are step bytes apart.
        //-----------------------------
        template <bool kRotated>
        ALWAYS_INLINE uint32_t
        LoadCoverage4(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8) | (uint32_t(mask[2 * step]) << 16) | (uint32_t(mask[3 * step]) << 24);
            }

            return Load32(mask);
        }

        // The same for 8 pixels.
        //-----------------------------
        template <bool kRotated>
        ALWAYS_INLINE uint64_t
        LoadCoverage8(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint64_t(LoadCoverage4<true>(mask, step)) | (uint64_t(LoadCoverage4<true>(&mask[4 * step], step)) << 32);
            }

            uint64_t value;
            memcpy(&value, mask, sizeof(value));
            return value;
        }

        // 8 pixels, 4 in lo and 4 in hi, as one register per channel: blue, green, red and alpha.
        //-----------------------------
        ALWAYS_INLINE uint8x8x4_t
        Deinterleave(uint8x16_t lo, uint8x16_t hi) {
            const uint8x16x2_t pairs  = vuzpq_u8(lo, hi);                       // Blue and red, and green and alpha
            const uint8x16x2_t planes = vuzpq_u8(pairs.val[0], pairs.val[1]);   // Blue then green, and red then alpha
            uint8x8x4_t        result;
            result.val[0] = vget_low_u8(planes.val[0]);
            result.val[1] = vget_high_u8(planes.val[0]);
            result.val[2] = vget_low_u8(planes.val[1]);
            result.val[3] = vget_high_u8(planes.val[1]);
            return result;
        }

        // The opposite: one register per channel back to 2 registers of 4 pixels.
        //-----------------------------
        ALWAYS_INLINE uint8x16x2_t
        Interleave(uint8x8x4_t planes) {
            const uint8x16x2_t pairs = vzipq_u8(vcombine_u8(planes.val[0], planes.val[1]), vcombine_u8(planes.val[2], planes.val[3]));
            return vzipq_u8(pairs.val[0], pairs.val[1]);
        }

        // 8 pixels blended with 8 premultiplied sources: each channel of x times the same channel of the color. Under an
        // opaque text the alpha of the blend is opaqueAlpha. Under a translucent one it is the alpha of the source, rounded.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE uint8x8x4_t
        BlendPlanes(const Color &color, const uint8x8x4_t &x, uint8x8_t opaqueAlpha, uint8x8x4_t dst) {
            uint16x8_t source[4];
            for (int channel = 0; channel < 4; ++channel) {
                source[channel] = vmull_u8(x.val[channel], color.planes.val[channel]);
            }
            const uint8x8_t alpha = kOpaque ? opaqueAlpha : Div255(source[3]);
            for (int channel = 0; channel < 4; ++channel) {
                dst.val[channel] = Blend(source[channel], dst.val[channel], alpha);
            }
            return dst;
        }

        // A coverage m draws as the premultiplied white texel (m, m, m, m), the same in the 4 channels.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE uint8x8x4_t
        BlendCoverage(const Color &color, uint8x8_t coverage, const uint8x8x4_t &dst) {
            uint8x8x4_t x;
            x.val[0] = coverage;
            x.val[1] = coverage;
            x.val[2] = coverage;
            x.val[3] = coverage;
            return BlendPlanes<kOpaque>(color, x, coverage, dst);
        }

        // 4 pixels in a register, BGRA after BGRA, blended with the color, with the coverage and the alpha of the blend
        // already spread over the channels of each pixel.
        //-----------------------------
        ALWAYS_INLINE uint8x16_t
        BlendPacked(const Color &color, uint8x16_t coverage, uint8x16_t alpha, uint8x16_t pixels) {
            const uint8x16_t inverse = vmvnq_u8(alpha);
            const uint16x8_t lo      = vmlal_u8(vmull_u8(vget_low_u8(coverage),  vget_low_u8(color.quad)),  vget_low_u8(pixels),  vget_low_u8(inverse));
            const uint16x8_t hi      = vmlal_u8(vmull_u8(vget_high_u8(coverage), vget_high_u8(color.quad)), vget_high_u8(pixels), vget_high_u8(inverse));
            return vcombine_u8(Div255(lo), Div255(hi));
        }

        // The alpha of the blend for each coverage: the coverage itself under an opaque text.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE uint8x8_t
        GetAlpha(const Color &color, uint8x8_t coverage) {
            return kOpaque ? coverage : Div255(vmull_u8(coverage, color.planes.val[3]));
        }

        // Writes the 4 pixels at lo and the 4 at hi when the coverage of the 8 lets them skip the blend, and says so.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE bool
        DrawShortcut8(const Color &color, uint8x8_t coverage, uint8_t *lo, uint8_t *hi) {
            if (kSkip == false) {
                return false;
            }
            const uint64_t m = GetLanes(coverage);
            if (m == 0) {
                return true;
            }
            if (kOpaque && m == ~uint64_t(0)) {
                vst1q_u8(lo, color.quad);
                vst1q_u8(hi, color.quad);
                return true;
            }

            return false;
        }

        // 8 pixels already read, 4 at lo and 4 at hi, with the coverage of each in a lane. lo and hi can overlap.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawPairRead(const Color &color, uint8x8_t coverage, uint8x16_t loPixels, uint8x16_t hiPixels, uint8_t *lo, uint8_t *hi) {
            if (DrawShortcut8<kOpaque, kSkip>(color, coverage, lo, hi) == false) {
                const uint8x16_t loCoverage = Pick(coverage, color.spreadLow);
                const uint8x16_t hiCoverage = Pick(coverage, color.spreadHigh);
                uint8x16_t       loAlpha    = loCoverage;
                uint8x16_t       hiAlpha    = hiCoverage;
                if (kOpaque == false) {
                    const uint8x8_t alpha = GetAlpha<kOpaque>(color, coverage);
                    loAlpha = Pick(alpha, color.spreadLow);
                    hiAlpha = Pick(alpha, color.spreadHigh);
                }
                vst1q_u8(hi, BlendPacked(color, hiCoverage, hiAlpha, hiPixels));
                vst1q_u8(lo, BlendPacked(color, loCoverage, loAlpha, loPixels));
            }
        }

        // The same, reading the pixels. Both groups of 4 are read before either is written.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawPair(const Color &color, uint8x8_t coverage, uint8_t *lo, uint8_t *hi) {
            if (DrawShortcut8<kOpaque, kSkip>(color, coverage, lo, hi) == false) {
                DrawPairRead<kOpaque, false>(color, coverage, vld1q_u8(lo), vld1q_u8(hi), lo, hi);
            }
        }

        // 4 pixels already read, with their coverage in the lanes 0 to 3.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawQuadRead(const Color &color, uint8x8_t coverage, uint8x16_t pixels, uint8_t *bytes) {
            if (kSkip) {
                const uint32_t m = uint32_t(GetLanes(coverage));
                if (m == 0) {
                    return;
                }
                if (kOpaque && m == 0xffffffffu) {
                    vst1q_u8(bytes, color.quad);
                    return;
                }
            }
            const uint8x16_t spread = Pick(coverage, color.spreadLow);
            const uint8x16_t alpha  = kOpaque ? spread : Pick(GetAlpha<kOpaque>(color, coverage), color.spreadLow);
            vst1q_u8(bytes, BlendPacked(color, spread, alpha, pixels));
        }

        // A row of 4 to 7 pixels, with the coverage of its first 4 in the lanes 0 to 3 and of its last 4 in the lanes 4 to
        // 7. With kNarrow, 4 pixels are one block of 4, and 5 or 6 are a block of 4 and the last 2 pixels; without it, and
        // with 7, the first 4 and the last 4. The parts are read before any is written.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kNarrow>
        ALWAYS_INLINE void
        DrawShortRow(const Color &color, uint8x8_t coverage, uint8_t *bytes, int32_t w) {
            if (kNarrow == false || w == 7) {
                DrawPair<kOpaque, kSkip>(color, coverage, bytes, bytes + 4 * size_t(w - 4));
            }
            else if (w == 4) {
                DrawQuadRead<kOpaque, kSkip>(color, coverage, vld1q_u8(bytes), bytes);
            }
            else {
                uint8_t          *last      = bytes + 4 * size_t(w - 2);
                const uint8x8_t  lastPixels = vld1_u8(last);
                DrawQuadRead<kOpaque, kSkip>(color, coverage, vld1q_u8(bytes), bytes);
                const uint8x8_t  spread     = vtbl1_u8(coverage, color.spreadLast);
                vst1_u8(last, Blend(vmull_u8(color.pair, spread), lastPixels, GetAlpha<kOpaque>(color, spread)));
            }
        }

        // 8 contiguous pixels, with the coverage of each in a lane. With kPacked the pixels stay as they are in memory;
        // without it they are read with one register per channel.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked>
        ALWAYS_INLINE void
        DrawBlock(const Color &color, uint8x8_t coverage, uint8_t *bytes) {
            if (kPacked) {
                DrawPair<kOpaque, kSkip>(color, coverage, bytes, bytes + 16);
            }
            else if (DrawShortcut8<kOpaque, kSkip>(color, coverage, bytes, bytes + 16) == false) {
                vst4_u8(bytes, BlendCoverage<kOpaque>(color, coverage, vld4_u8(bytes)));
            }
        }

        // The last full block of a row and the last 8 pixels of the row, which overlap it. Both read their pixels before
        // either writes, so both compute the same values for the pixels they share.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked>
        ALWAYS_INLINE void
        DrawBlockAndLast(const Color &color, uint8x8_t coverage, uint8_t *bytes, uint8x8_t lastCoverage, uint8_t *last) {
            if (kPacked) {
                const uint8x16_t lastLo = vld1q_u8(last);
                const uint8x16_t lastHi = vld1q_u8(last + 16);
                DrawPair<kOpaque, kSkip>(color, coverage, bytes, bytes + 16);
                DrawPairRead<kOpaque, kSkip>(color, lastCoverage, lastLo, lastHi, last, last + 16);
            }
            else {
                const uint8x8x4_t lastPixels = vld4_u8(last);
                DrawBlock<kOpaque, kSkip, false>(color, coverage, bytes);
                if (DrawShortcut8<kOpaque, kSkip>(color, lastCoverage, last, last + 16) == false) {
                    vst4_u8(last, BlendCoverage<kOpaque>(color, lastCoverage, lastPixels));
                }
            }
        }

        // The last full block of a row and its last 4 pixels, which can overlap it.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked>
        ALWAYS_INLINE void
        DrawBlockAndQuad(const Color &color, uint8x8_t coverage, uint8_t *bytes, uint8x8_t tailCoverage, uint8_t *tail) {
            const uint8x16_t tailPixels = vld1q_u8(tail);
            DrawBlock<kOpaque, kSkip, kPacked>(color, coverage, bytes);
            DrawQuadRead<kOpaque, kSkip>(color, tailCoverage, tailPixels, tail);
        }

        // 2 pixels already read, with the coverage of each in a byte of m.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE uint8x8_t
        BlendAlpha8Pair(const Color &color, uint32_t m, uint8x8_t pixels) {
            const uint8x8_t coverage = vtbl1_u8(vreinterpret_u8_u32(vdup_n_u32(m)), color.spreadCoverage);
            return Blend(vmull_u8(color.pair, coverage), pixels, GetAlpha<kOpaque>(color, coverage));
        }

        // A row of 1 to 3 pixels. With 3, the pixels 0 and 1 and the pixels 1 and 2 are read before either pair is written.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE void
        DrawAlpha8Narrow(const Color &color, const uint8_t *mask, size_t step, uint32_t *dst, int32_t w) {
            if (w == 1) {
                const uint8x8_t result = BlendAlpha8Pair<kOpaque>(color, mask[0], vreinterpret_u8_u32(vld1_dup_u32(dst)));
                vst1_lane_u32(dst, vreinterpret_u32_u8(result), 0);
            }
            else if (w == 2) {
                const uint8x8_t result = BlendAlpha8Pair<kOpaque>(color, uint32_t(mask[0]) | (uint32_t(mask[step]) << 8), vreinterpret_u8_u32(vld1_u32(dst)));
                vst1_u32(dst, vreinterpret_u32_u8(result));
            }
            else if (w == 3) {
                const uint32_t  middle = mask[step];
                const uint8x8_t first  = vreinterpret_u8_u32(vld1_u32(dst));
                const uint8x8_t second = vreinterpret_u8_u32(vld1_u32(dst + 1));
                const uint8x8_t front  = BlendAlpha8Pair<kOpaque>(color, uint32_t(mask[0]) | (middle << 8), first);
                const uint8x8_t back   = BlendAlpha8Pair<kOpaque>(color, middle | (uint32_t(mask[2 * step]) << 8), second);
                vst1_u32(dst + 1, vreinterpret_u32_u8(back));
                vst1_u32(dst,     vreinterpret_u32_u8(front));
            }
        }

        // The clipping of DrawText can leave a width below 1, which draws nothing.
        //-----------------------------
        template <bool kRotated, bool kOpaque, bool kSkip, bool kPacked, bool kQuadTail, bool kNarrow>
        ALWAYS_INLINE void
        DrawAlpha8Row(const Color &color, const uint8_t *mask, size_t step, uint32_t *dst, int32_t w) {
            uint8_t *bytes = reinterpret_cast<uint8_t *>(dst);
            if (w >= 8) {
                const size_t full = size_t(w) & ~size_t(7);
                const size_t rest = size_t(w) & 7;
                size_t       x    = 0;
                for (; x + 8 < full; x += 8) {
                    DrawBlock<kOpaque, kSkip, kPacked>(color, vcreate_u8(LoadCoverage8<kRotated>(&mask[x * step], step)), bytes + 4 * x);
                }

                const uint8x8_t coverage = vcreate_u8(LoadCoverage8<kRotated>(&mask[x * step], step));
                if (rest == 0) {
                    DrawBlock<kOpaque, kSkip, kPacked>(color, coverage, bytes + 4 * x);
                }
                else if (kQuadTail && rest <= 4) {
                    const size_t tail = size_t(w) - 4;
                    DrawBlockAndQuad<kOpaque, kSkip, kPacked>(color, coverage, bytes + 4 * x, vcreate_u8(LoadCoverage4<kRotated>(&mask[tail * step], step)), bytes + 4 * tail);
                }
                else {
                    const size_t last = size_t(w) - 8;
                    DrawBlockAndLast<kOpaque, kSkip, kPacked>(color, coverage, bytes + 4 * x, vcreate_u8(LoadCoverage8<kRotated>(&mask[last * step], step)), bytes + 4 * last);
                }
            }
            else if (w >= 4) {
                const size_t   back = size_t(w - 4);
                const uint64_t m    = uint64_t(LoadCoverage4<kRotated>(mask, step)) | (uint64_t(LoadCoverage4<kRotated>(&mask[back * step], step)) << 32);
                DrawShortRow<kOpaque, kSkip, kNarrow>(color, vcreate_u8(m), bytes, w);
            }
            else {
                DrawAlpha8Narrow<kOpaque>(color, mask, step, dst, w);
            }
        }

        // The coverage of 4 rows in 4 columns of a rotated glyph, whose texels down a column are contiguous in the
        // texture: 4 bytes per column, column after column.
        //-----------------------------
        ALWAYS_INLINE uint8x16_t
        LoadColumns(const uint8_t *mask, size_t step) {
            const uint64_t first  = uint64_t(Load32(mask))            | (uint64_t(Load32(&mask[step]))     << 32);
            const uint64_t second = uint64_t(Load32(&mask[2 * step])) | (uint64_t(Load32(&mask[3 * step])) << 32);
            return vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(first), vcreate_u64(second)));
        }

        // The coverage of 4 rows in 8 columns, with the 8 of each row in a register: the same reordering as Deinterleave.
        //-----------------------------
        ALWAYS_INLINE uint8x8x4_t
        LoadBandCoverage(const uint8_t *mask, size_t step) {
            return Deinterleave(LoadColumns(mask, step), LoadColumns(&mask[4 * step], step));
        }

        // The coverage of 4 rows in 4 columns, with the 4 of each row in the lanes 0 to 3 of a register.
        //-----------------------------
        ALWAYS_INLINE uint8x8x4_t
        LoadBandQuadCoverage(const uint8_t *mask, size_t step) {
            const uint8x16_t columns = LoadColumns(mask, step);
            return Deinterleave(columns, columns);
        }

        // The functions below draw the same part of the 4 rows of a band, with the coverage of each row in a register. In a
        // partial band the rows before firstRow belong to the band above. The rows are written out because Clang keeps a
        // loop over them, with the coverage and the rows in memory.

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked, bool kPartial>
        ALWAYS_INLINE void
        DrawBandBlocks(const Color &color, const uint8x8x4_t &coverage, uint8_t *bytes, size_t rowBytes, int32_t firstRow) {
            if (kPartial == false || firstRow <= 0) {
                DrawBlock<kOpaque, kSkip, kPacked>(color, coverage.val[0], bytes);
            }
            if (kPartial == false || firstRow <= 1) {
                DrawBlock<kOpaque, kSkip, kPacked>(color, coverage.val[1], bytes + rowBytes);
            }
            if (kPartial == false || firstRow <= 2) {
                DrawBlock<kOpaque, kSkip, kPacked>(color, coverage.val[2], bytes + 2 * rowBytes);
            }
            DrawBlock<kOpaque, kSkip, kPacked>(color, coverage.val[3], bytes + 3 * rowBytes);
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked, bool kPartial>
        ALWAYS_INLINE void
        DrawBandBlocksAndLast(const Color &color, const uint8x8x4_t &coverage, uint8_t *bytes, const uint8x8x4_t &lastCoverage, uint8_t *last,
                              size_t rowBytes, int32_t firstRow) {
            if (kPartial == false || firstRow <= 0) {
                DrawBlockAndLast<kOpaque, kSkip, kPacked>(color, coverage.val[0], bytes, lastCoverage.val[0], last);
            }
            if (kPartial == false || firstRow <= 1) {
                DrawBlockAndLast<kOpaque, kSkip, kPacked>(color, coverage.val[1], bytes + rowBytes, lastCoverage.val[1], last + rowBytes);
            }
            if (kPartial == false || firstRow <= 2) {
                DrawBlockAndLast<kOpaque, kSkip, kPacked>(color, coverage.val[2], bytes + 2 * rowBytes, lastCoverage.val[2], last + 2 * rowBytes);
            }
            DrawBlockAndLast<kOpaque, kSkip, kPacked>(color, coverage.val[3], bytes + 3 * rowBytes, lastCoverage.val[3], last + 3 * rowBytes);
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked, bool kPartial>
        ALWAYS_INLINE void
        DrawBandBlocksAndQuads(const Color &color, const uint8x8x4_t &coverage, uint8_t *bytes, const uint8x8x4_t &tailCoverage, uint8_t *tail,
                               size_t rowBytes, int32_t firstRow) {
            if (kPartial == false || firstRow <= 0) {
                DrawBlockAndQuad<kOpaque, kSkip, kPacked>(color, coverage.val[0], bytes, tailCoverage.val[0], tail);
            }
            if (kPartial == false || firstRow <= 1) {
                DrawBlockAndQuad<kOpaque, kSkip, kPacked>(color, coverage.val[1], bytes + rowBytes, tailCoverage.val[1], tail + rowBytes);
            }
            if (kPartial == false || firstRow <= 2) {
                DrawBlockAndQuad<kOpaque, kSkip, kPacked>(color, coverage.val[2], bytes + 2 * rowBytes, tailCoverage.val[2], tail + 2 * rowBytes);
            }
            DrawBlockAndQuad<kOpaque, kSkip, kPacked>(color, coverage.val[3], bytes + 3 * rowBytes, tailCoverage.val[3], tail + 3 * rowBytes);
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kNarrow, bool kPartial>
        ALWAYS_INLINE void
        DrawBandShortRows(const Color &color, const uint8x8x4_t &coverage, uint8_t *bytes, int32_t w, size_t rowBytes, int32_t firstRow) {
            if (kPartial == false || firstRow <= 0) {
                DrawShortRow<kOpaque, kSkip, kNarrow>(color, coverage.val[0], bytes, w);
            }
            if (kPartial == false || firstRow <= 1) {
                DrawShortRow<kOpaque, kSkip, kNarrow>(color, coverage.val[1], bytes + rowBytes, w);
            }
            if (kPartial == false || firstRow <= 2) {
                DrawShortRow<kOpaque, kSkip, kNarrow>(color, coverage.val[2], bytes + 2 * rowBytes, w);
            }
            DrawShortRow<kOpaque, kSkip, kNarrow>(color, coverage.val[3], bytes + 3 * rowBytes, w);
        }

        // 4 rows of a rotated glyph at least 4 pixels wide, drawn as DrawAlpha8Row draws a row. The coverage of 8 columns
        // takes 8 reads of 4 bytes instead of 32 reads of a byte.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked, bool kQuadTail, bool kNarrow, bool kPartial>
        ALWAYS_INLINE void
        DrawAlpha8Band(const Color &color, const uint8_t *mask, size_t step, uint32_t *dst, size_t dstStride, int32_t w, int32_t firstRow) {
            uint8_t      *bytes    = reinterpret_cast<uint8_t *>(dst);
            const size_t rowBytes = 4 * dstStride;
            if (w >= 8) {
                const size_t full = size_t(w) & ~size_t(7);
                const size_t rest = size_t(w) & 7;
                size_t       x    = 0;
                for (; x + 8 < full; x += 8) {
                    DrawBandBlocks<kOpaque, kSkip, kPacked, kPartial>(color, LoadBandCoverage(&mask[x * step], step), bytes + 4 * x, rowBytes, firstRow);
                }

                const uint8x8x4_t coverage = LoadBandCoverage(&mask[x * step], step);
                if (rest == 0) {
                    DrawBandBlocks<kOpaque, kSkip, kPacked, kPartial>(color, coverage, bytes + 4 * x, rowBytes, firstRow);
                }
                else if (kQuadTail && rest <= 4) {
                    const size_t tail = size_t(w) - 4;
                    DrawBandBlocksAndQuads<kOpaque, kSkip, kPacked, kPartial>(color, coverage, bytes + 4 * x, LoadBandQuadCoverage(&mask[tail * step], step),
                                                                             bytes + 4 * tail, rowBytes, firstRow);
                }
                else {
                    const size_t last = size_t(w) - 8;
                    DrawBandBlocksAndLast<kOpaque, kSkip, kPacked, kPartial>(color, coverage, bytes + 4 * x, LoadBandCoverage(&mask[last * step], step),
                                                                            bytes + 4 * last, rowBytes, firstRow);
                }
            }
            else {
                const size_t back = size_t(w - 4);
                DrawBandShortRows<kOpaque, kSkip, kNarrow, kPartial>(color, Deinterleave(LoadColumns(mask, step), LoadColumns(&mask[back * step], step)),
                                                                     bytes, w, rowBytes, firstRow);
            }
        }

        // A rotated glyph is stored transposed. With the texels of a column contiguous, it is drawn 4 rows at a time. The
        // 1 to 3 rows left are the end of a last band of 4, which overlaps the one above.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked, bool kQuadTail, bool kNarrow>
        ALWAYS_INLINE void
        DrawAlpha8RotatedRows(const Color &color, const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                              uint32_t *dst, uint32_t dstStride) {
            if (stepY == 1 && width >= 4 && height >= 4) {
                int32_t y = 0;
                for (; y + 4 <= height; y += 4) {
                    DrawAlpha8Band<kOpaque, kSkip, kPacked, kQuadTail, kNarrow, false>(color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, width, 0);
                }
                if (y < height) {
                    const int32_t top = height - 4;
                    DrawAlpha8Band<kOpaque, kSkip, kPacked, kQuadTail, kNarrow, true>(color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, width, y - top);
                }
            }
            else {
                for (int32_t y = 0; y < height; ++y) {
                    DrawAlpha8Row<true, kOpaque, kSkip, kPacked, kQuadTail, kNarrow>(color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride], width);
                }
            }
        }

        // One function for each kSkip keeps each of them smaller, with fewer values that do not fit in the registers.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked, bool kQuadTail, bool kNarrow>
        NO_INLINE void
        DrawAlpha8Rotated(const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                          uint32_t *dst, uint32_t dstStride, uint32_t premultiplied) {
            DrawAlpha8RotatedRows<kOpaque, kSkip, kPacked, kQuadTail, kNarrow>(MakeColor(premultiplied), mask, stepX, stepY, width, height, dst, dstStride);
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked, bool kQuadTail, bool kNarrow>
        ALWAYS_INLINE void
        DrawAlpha8Rows(const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride) {
            for (int32_t y = 0; y < height; ++y) {
                DrawAlpha8Row<false, kOpaque, kSkip, kPacked, kQuadTail, kNarrow>(color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride], width);
            }
        }

        // A rotated glyph has a function of its own, so that its registers are not saved for every glyph.
        //-----------------------------
        template <bool kOpaque, int32_t kSkipWidth, bool kPacked, bool kQuadTail, bool kNarrow>
        NO_INLINE void
        DrawAlpha8Glyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                        uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t /*alpha*/) {
            const uint8_t *mask = &texture[offset];
            if (stepX != 1) {
                if (UsesShortcuts<kSkipWidth>(width)) {
                    DrawAlpha8Rotated<kOpaque, true, kPacked, kQuadTail, kNarrow>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied);
                }
                else {
                    DrawAlpha8Rotated<kOpaque, false, kPacked, kQuadTail, kNarrow>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied);
                }
                return;
            }

            const Color color = MakeColor(premultiplied);
            if (UsesShortcuts<kSkipWidth>(width)) {
                DrawAlpha8Rows<kOpaque, true, kPacked, kQuadTail, kNarrow>(color, mask, stepY, width, height, dst, dstStride);
            }
            else {
                DrawAlpha8Rows<kOpaque, false, kPacked, kQuadTail, kNarrow>(color, mask, stepY, width, height, dst, dstStride);
            }
        }

        // Each channel of the texels times the same channel of the color, rounded.
        //-----------------------------
        ALWAYS_INLINE uint8x8x4_t
        Tint(const Color &color, uint8x8x4_t texels) {
            for (int channel = 0; channel < 4; ++channel) {
                texels.val[channel] = Div255(vmull_u8(texels.val[channel], color.planes.val[channel]));
            }
            return texels;
        }

        // Writes 8 contiguous pixels when their texels let them skip the blend, and says so. Transparent texels leave the
        // pixels as they are. Opaque texels under an opaque text are written tinted, or as they are when the color is white.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE bool
        DrawBgraShortcut(const Color &color, const uint8x8x4_t &texels, uint8_t *bytes) {
            if (kSkip == false) {
                return false;
            }
            const uint64_t alphas = GetLanes(texels.val[3]);
            if (alphas == 0) {
                return true;
            }
            if (kOpaque && alphas == ~uint64_t(0)) {
                vst4_u8(bytes, color.white ? texels : Tint(color, texels));
                return true;
            }

            return false;
        }

        // The premultiplied texel times the premultiplied color, blended with the destination, for one pixel.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE void
        DrawBgraTexel(const Color &color, const uint8_t *texel, uint32_t *dst) {
            const uint32_t t = Load32(texel);
            if (t >> 24) {
                const uint8x8_t  texels = vreinterpret_u8_u32(vdup_n_u32(t));
                const uint16x8_t source = vmull_u8(texels, color.pair);
                const uint8x8_t  alpha  = vtbl1_u8(kOpaque ? texels : Div255(source), color.spreadAlpha);
                const uint8x8_t  result = Blend(source, vreinterpret_u8_u32(vld1_dup_u32(dst)), alpha);
                vst1_lane_u32(dst, vreinterpret_u32_u8(result), 0);
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawBgraRow(const Color &color, const uint8_t *src, uint32_t *dst, int32_t w) {
            uint8_t *bytes = reinterpret_cast<uint8_t *>(dst);
            if (w >= 8) {
                const size_t      last       = size_t(w - 8);
                uint8_t           *lastBytes = bytes + 4 * last;
                const uint8x8x4_t lastTexels = vld4_u8(src + 4 * last);
                const uint8x8x4_t lastPixels = vld4_u8(lastBytes);
                for (size_t x = 0; x < last; x += 8) {
                    uint8_t           *blockBytes = bytes + 4 * x;
                    const uint8x8x4_t texels      = vld4_u8(src + 4 * x);
                    if (DrawBgraShortcut<kOpaque, kSkip>(color, texels, blockBytes) == false) {
                        vst4_u8(blockBytes, BlendPlanes<kOpaque>(color, texels, texels.val[3], vld4_u8(blockBytes)));
                    }
                }
                if (DrawBgraShortcut<kOpaque, kSkip>(color, lastTexels, lastBytes) == false) {
                    vst4_u8(lastBytes, BlendPlanes<kOpaque>(color, lastTexels, lastTexels.val[3], lastPixels));
                }
            }
            else if (w >= 4) {
                const size_t      back       = size_t(w - 4);
                uint8_t           *backBytes = bytes + 4 * back;
                const uint8x16_t  texelsLo   = vld1q_u8(src);
                const uint8x16_t  texelsHi   = vld1q_u8(src + 4 * back);
                const uint8x8x4_t texels     = Deinterleave(texelsLo, texelsHi);
                const uint64_t    alphas     = GetLanes(texels.val[3]);
                if (kSkip && alphas == 0) {
                    return;
                }

                uint8x8x4_t planes;
                if (kSkip && kOpaque && alphas == ~uint64_t(0)) {
                    if (color.white) {
                        vst1q_u8(backBytes, texelsHi);
                        vst1q_u8(bytes,     texelsLo);
                        return;
                    }
                    planes = Tint(color, texels);
                }
                else {
                    planes = BlendPlanes<kOpaque>(color, texels, texels.val[3], Deinterleave(vld1q_u8(bytes), vld1q_u8(backBytes)));
                }
                const uint8x16x2_t pixels = Interleave(planes);
                vst1q_u8(backBytes, pixels.val[1]);
                vst1q_u8(bytes,     pixels.val[0]);
            }
            else {
                while (w > 0) {
                    DrawBgraTexel<kOpaque>(color, src, dst);
                    src += 4;
                    w--;
                    dst++;
                }
            }
        }

        // 4 texels and 4 pixels, BGRA after BGRA: each texel times the color, over its pixel. The alpha of the blend is the
        // alpha of the texel under an opaque text, and the alpha of the tinted texel under a translucent one.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE uint8x16_t
        BlendTexels(const Color &color, uint8x16_t texels, uint8x16_t pixels) {
            const uint16x8_t sourceLo = vmull_u8(vget_low_u8(texels),  vget_low_u8(color.quad));
            const uint16x8_t sourceHi = vmull_u8(vget_high_u8(texels), vget_high_u8(color.quad));
            const uint8x16_t alpha    = Pick16(kOpaque ? texels : vcombine_u8(Div255(sourceLo), Div255(sourceHi)), color.spreadAlphas);
            const uint8x16_t inverse  = vmvnq_u8(alpha);
            const uint16x8_t lo       = vmlal_u8(sourceLo, vget_low_u8(pixels),  vget_low_u8(inverse));
            const uint16x8_t hi       = vmlal_u8(sourceHi, vget_high_u8(pixels), vget_high_u8(inverse));
            return vcombine_u8(Div255(lo), Div255(hi));
        }

        // Writes the texels of 4 pixels when they let them skip the blend, and says so. Transparent texels leave the pixels
        // as they are. Opaque texels under an opaque text are written tinted, or as they are when the color is white.
        // anyAlpha and allAlpha are the alpha bytes of the texels, ORed and ANDed.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE bool
        DrawTexelsShortcut(const Color &color, uint64_t anyAlpha, uint64_t allAlpha, const uint8_t *src, uint8_t *bytes) {
            if (kSkip == false) {
                return false;
            }
            if (anyAlpha == 0) {
                return true;
            }
            if (kOpaque && allAlpha == kAlphaBytes) {
                const uint8x16_t texels = vld1q_u8(src);
                if (color.white) {
                    vst1q_u8(bytes, texels);
                }
                else {
                    const uint16x8_t lo = vmull_u8(vget_low_u8(texels),  vget_low_u8(color.quad));
                    const uint16x8_t hi = vmull_u8(vget_high_u8(texels), vget_high_u8(color.quad));
                    vst1q_u8(bytes, vcombine_u8(Div255(lo), Div255(hi)));
                }
                return true;
            }

            return false;
        }

        // 4 pixels already read, with their texels at src.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawTexelsRead(const Color &color, const uint8_t *src, uint8x16_t pixels, uint8_t *bytes) {
            if (kSkip) {
                uint64_t texels[2];
                memcpy(texels, src, sizeof(texels));
                if (DrawTexelsShortcut<kOpaque, true>(color, (texels[0] | texels[1]) & kAlphaBytes, texels[0] & texels[1] & kAlphaBytes, src, bytes)) {
                    return;
                }
            }
            vst1q_u8(bytes, BlendTexels<kOpaque>(color, vld1q_u8(src), pixels));
        }

        // 8 pixels already read, 4 at lo and 4 at hi, with their texels at srcLo and srcHi. When all 8 texels let the block
        // skip the blend, it is done for the 8 at once.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawTexelPairRead(const Color &color, const uint8_t *srcLo, const uint8_t *srcHi, uint8x16_t loPixels, uint8x16_t hiPixels, uint8_t *lo, uint8_t *hi) {
            if (kSkip) {
                uint64_t texelsLo[2], texelsHi[2];
                memcpy(texelsLo, srcLo, sizeof(texelsLo));
                memcpy(texelsHi, srcHi, sizeof(texelsHi));
                const uint64_t anyAlpha = (texelsLo[0] | texelsLo[1] | texelsHi[0] | texelsHi[1]) & kAlphaBytes;
                const uint64_t allAlpha = texelsLo[0] & texelsLo[1] & texelsHi[0] & texelsHi[1] & kAlphaBytes;
                if (anyAlpha == 0) {
                    return;
                }
                if (kOpaque && allAlpha == kAlphaBytes) {
                    DrawTexelsShortcut<kOpaque, true>(color, anyAlpha, allAlpha, srcHi, hi);
                    DrawTexelsShortcut<kOpaque, true>(color, anyAlpha, allAlpha, srcLo, lo);
                    return;
                }
            }
            vst1q_u8(hi, BlendTexels<kOpaque>(color, vld1q_u8(srcHi), hiPixels));
            vst1q_u8(lo, BlendTexels<kOpaque>(color, vld1q_u8(srcLo), loPixels));
        }

        // The same, reading the pixels. Both groups of 4 are read before either is written.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawTexelPair(const Color &color, const uint8_t *srcLo, const uint8_t *srcHi, uint8_t *lo, uint8_t *hi) {
            DrawTexelPairRead<kOpaque, kSkip>(color, srcLo, srcHi, vld1q_u8(lo), vld1q_u8(hi), lo, hi);
        }

        // A row of BGRA32 with the pixels as they are in memory, in the blocks of DrawAlpha8Row.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawBgraRowPacked(const Color &color, const uint8_t *src, uint32_t *dst, int32_t w) {
            uint8_t *bytes = reinterpret_cast<uint8_t *>(dst);
            if (w >= 8) {
                const size_t full = size_t(w) & ~size_t(7);
                const size_t rest = size_t(w) & 7;
                size_t       x    = 0;
                for (; x + 8 < full; x += 8) {
                    DrawTexelPair<kOpaque, kSkip>(color, src + 4 * x, src + 4 * x + 16, bytes + 4 * x, bytes + 4 * x + 16);
                }

                if (rest == 0) {
                    DrawTexelPair<kOpaque, kSkip>(color, src + 4 * x, src + 4 * x + 16, bytes + 4 * x, bytes + 4 * x + 16);
                }
                else if (rest <= 4) {
                    const size_t     tail       = 4 * (size_t(w) - 4);
                    const uint8x16_t tailPixels = vld1q_u8(bytes + tail);
                    DrawTexelPair<kOpaque, kSkip>(color, src + 4 * x, src + 4 * x + 16, bytes + 4 * x, bytes + 4 * x + 16);
                    DrawTexelsRead<kOpaque, kSkip>(color, src + tail, tailPixels, bytes + tail);
                }
                else {
                    const size_t     last   = 4 * (size_t(w) - 8);
                    const uint8x16_t lastLo = vld1q_u8(bytes + last);
                    const uint8x16_t lastHi = vld1q_u8(bytes + last + 16);
                    DrawTexelPair<kOpaque, kSkip>(color, src + 4 * x, src + 4 * x + 16, bytes + 4 * x, bytes + 4 * x + 16);
                    DrawTexelPairRead<kOpaque, kSkip>(color, src + last, src + last + 16, lastLo, lastHi, bytes + last, bytes + last + 16);
                }
            }
            else if (w >= 4) {
                const size_t back = 4 * size_t(w - 4);
                DrawTexelPair<kOpaque, kSkip>(color, src, src + back, bytes, bytes + back);
            }
            else {
                while (w > 0) {
                    DrawBgraTexel<kOpaque>(color, src, dst);
                    src += 4;
                    w--;
                    dst++;
                }
            }
        }

        // A rotated glyph is drawn one pixel at a time: no scenario measures it.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPacked>
        ALWAYS_INLINE void
        DrawBgraRows(const Color &color, const uint8_t *src, size_t stepX, size_t stepY, int32_t width, int32_t height,
                     uint32_t *dst, uint32_t dstStride) {
            for (int32_t y = 0; y < height; ++y) {
                uint32_t *row = &dst[size_t(y) * dstStride];
                if (stepX == 1 && kPacked) {
                    DrawBgraRowPacked<kOpaque, kSkip>(color, &src[size_t(y) * stepY * 4], row, width);
                }
                else if (stepX == 1) {
                    DrawBgraRow<kOpaque, kSkip>(color, &src[size_t(y) * stepY * 4], row, width);
                }
                else {
                    for (int32_t x = 0; x < width; ++x) {
                        DrawBgraTexel<kOpaque>(color, &src[(size_t(y) * stepY + size_t(x) * stepX) * 4], &row[x]);
                    }
                }
            }
        }

        //-----------------------------
        template <bool kOpaque, int32_t kSkipWidth, bool kPacked>
        NO_INLINE void
        DrawBgraGlyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                      uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t /*alpha*/) {
            const Color   color = MakeColor(premultiplied);
            const uint8_t *src  = &texture[offset * 4];
            if (UsesShortcuts<kSkipWidth>(width)) {
                DrawBgraRows<kOpaque, true, kPacked>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
            else {
                DrawBgraRows<kOpaque, false, kPacked>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
        }

#undef NO_INLINE
#undef ALWAYS_INLINE

        //-----------------------------
        template <int32_t kSkipWidth, bool kPacked, bool kQuadTail, bool kNarrow>
        DrawGlyphFunction
        GetDrawGlyphFunction(bool alpha8, bool opaque) {
            if (alpha8) {
                return opaque ? DrawAlpha8Glyph<true, kSkipWidth, kPacked, kQuadTail, kNarrow> : DrawAlpha8Glyph<false, kSkipWidth, kPacked, kQuadTail, kNarrow>;
            }

            return opaque ? DrawBgraGlyph<true, kSkipWidth, kPacked> : DrawBgraGlyph<false, kSkipWidth, kPacked>;
        }

        //-----------------------------
        template <int32_t kSkipWidth, bool kPacked, bool kQuadTail, bool kNarrow>
        inline void
        DrawQuads(Scenario &scenario, uint32_t *dst) {
            const FontBase          &font         = *scenario.font;
            const uint8_t           *texture      = font.GetTexture();
            const size_t            textureWidth  = font.GetTextureWidth();
            const uint32_t          premultiplied = PremultiplyColor(scenario.color);
            const uint32_t          colorAlpha    = scenario.color >> 24;
            const DrawGlyphFunction drawGlyph     = GetDrawGlyphFunction<kSkipWidth, kPacked, kQuadTail, kNarrow>(font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8,
                                                                                                         colorAlpha == 255);

            for (const GlyphQuad &quad : scenario.quads) {
                const size_t stepX         = quad.rotated ? textureWidth : 1;
                const size_t stepY         = quad.rotated ? 1 : textureWidth;
                const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
                uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
                drawGlyph(texture, offsetTexture, stepX, stepY, quad.width, quad.height, dstGlyph, scenario.width, premultiplied, colorAlpha);
            }
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        DrawQuads<32, true, true, true>(scenario, dst);
    }

    //---------------------------------
    void
    DrawLd4(Scenario &scenario, uint32_t *dst) {
        DrawQuads<32, false, true, true>(scenario, dst);
    }

    //---------------------------------
    void
    DrawNoQuad(Scenario &scenario, uint32_t *dst) {
        DrawQuads<32, true, false, true>(scenario, dst);
    }

    //---------------------------------
    void
    DrawNoNarrow(Scenario &scenario, uint32_t *dst) {
        DrawQuads<32, true, true, false>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBranchless(Scenario &scenario, uint32_t *dst) {
        DrawQuads<kSkipNever, true, true, true>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSkip(Scenario &scenario, uint32_t *dst) {
        DrawQuads<kSkipAlways, true, true, true>(scenario, dst);
    }

} // end of namespace NeonPacked
} // end of namespace Benchmark

#endif
