#include "benchmarkCommon.h"

#if defined(FONTRENDERER_NEON)

#include <arm_neon.h>
//-------------------------------------
#include <cstring>

// NeonPerCase8 without pixels drawn one at a time in a row of 4 pixels or more. Every block has 8 pixels, with the
// channels apart: one register for the blue of the 8 pixels, one for the green, and so on. The same pixels as DrawText.
// NeonPacked starts from this file.
//
// A row of 8 pixels or more is drawn in blocks of 8, and its last block ends where the row ends, so it can overlap the
// block before it. It reads its pixels before that block writes them. Both blocks then compute the same values for the
// pixels they share, from the same coverage and the same pixel, and the one that writes last changes nothing.
// A row of 4 to 7 pixels is one block of 8 lanes made of its first 4 pixels and its last 4, which can overlap in the same
// way. A row of 1 to 3 pixels is drawn 2 pixels at a time. Rotated glyphs of BGRA32 are still drawn one pixel at a time:
// no scenario measures them.
//
// Draw draws the rows of a rotated glyph of Alpha8 like the others, with the coverage gathered byte by byte. DrawBands
// draws them 4 rows at a time: the texels of a column of the glyph are contiguous in the texture, so 8 reads of 4 bytes
// give the coverage of 4 rows in 8 columns. The 1 to 3 rows left are the end of a last band that overlaps the one above.
//
// pixman draws its blocks of 8 without asking whether the coverage is all 0 or all 255. DrawBranchless and
// DrawBandsBranchless do the same, and DrawBandsSkip32 only asks in glyphs at least 32 pixels wide. DrawUzp reads and
// writes the blocks of 8 with vld1q_u8 and vst1q_u8 and separates the channels with vuzpq_u8 and vzipq_u8, instead of
// vld4_u8 and vst4_u8.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace NeonOverlap {

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

        // Clang stops inlining the functions of a row once a glyph function has two of them, and then passes the color
        // through memory for every row. ALWAYS_INLINE keeps them inside the function of the glyph.
        //
        // A glyph at least kSkipWidth pixels wide asks for each block whether it can skip the blend. Wide glyphs have long
        // runs of blocks without coverage or fully covered. In narrow glyphs those blocks are rare, and the branch costs
        // more than the blends it saves. kSkipAlways and kSkipNever ask for every glyph or for none.
        constexpr int32_t kSkipAlways = 0;
        constexpr int32_t kSkipNever  = -1;

        //-----------------------------
        template <int32_t kSkipWidth>
        inline bool
        UsesShortcuts(int32_t width) {
            return kSkipWidth == kSkipAlways || (kSkipWidth != kSkipNever && width >= kSkipWidth);
        }

        using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                           uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

        // Indexes for vtbl1_u8 on 2 pixels. The first copies the byte 0 over the 4 channels of the first pixel and the
        // byte 1 over the second, which spreads a coverage. The second copies the alpha of each pixel over its channels.
        const uint8_t kSpreadCoverage[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };
        const uint8_t kSpreadAlpha[8]    = { 3, 3, 3, 3, 7, 7, 7, 7 };

        //-----------------------------
        struct Color {
            uint8x8x4_t planes;         // Each channel of the premultiplied color in every lane
            uint8x8_t   pair;           // The premultiplied color for 2 pixels
            uint8x16_t  solid;          // The color for 4 pixels, written when the text and the texels are opaque
            uint8x8_t   spreadCoverage;
            uint8x8_t   spreadAlpha;
            bool        white;
        };

        //-----------------------------
        inline Color
        MakeColor(uint32_t premultiplied) {
            Color color;
            color.pair           = vreinterpret_u8_u32(vdup_n_u32(premultiplied));
            color.planes.val[0]  = vdup_lane_u8(color.pair, 0);
            color.planes.val[1]  = vdup_lane_u8(color.pair, 1);
            color.planes.val[2]  = vdup_lane_u8(color.pair, 2);
            color.planes.val[3]  = vdup_lane_u8(color.pair, 3);
            color.solid          = vreinterpretq_u8_u32(vdupq_n_u32(premultiplied));
            color.spreadCoverage = vld1_u8(kSpreadCoverage);
            color.spreadAlpha    = vld1_u8(kSpreadAlpha);
            color.white          = premultiplied == 0xffffffffu;
            return color;
        }

        // Each 16-bit lane divided by 255 and rounded, as an 8-bit lane. Exact up to 65152, the largest sum of a blend.
        //-----------------------------
        inline uint8x8_t
        Div255(uint16x8_t x) {
            return vraddhn_u16(x, vrshrq_n_u16(x, 8));
        }

        // Rounded (source + dst * (255 - alpha)) / 255 in each byte, with the source already multiplied.
        //-----------------------------
        inline uint8x8_t
        Blend(uint16x8_t source, uint8x8_t dst, uint8x8_t alpha) {
            return Div255(vmlal_u8(source, dst, vmvn_u8(alpha)));
        }

        // The 8 bytes of a register as a number, to test them all at once.
        //-----------------------------
        inline uint64_t
        GetLanes(uint8x8_t x) {
            return vget_lane_u64(vreinterpret_u64_u8(x), 0);
        }

        // The 4 bytes at p as a number. NEON is only used on little-endian processors, so the first byte is the lowest.
        //-----------------------------
        inline uint32_t
        Load32(const uint8_t *p) {
            uint32_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        // The coverage of 4 pixels in a row, a byte each. The texels of a rotated glyph are step bytes apart.
        //-----------------------------
        template <bool kRotated>
        inline uint32_t
        LoadCoverage4(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8) | (uint32_t(mask[2 * step]) << 16) | (uint32_t(mask[3 * step]) << 24);
            }

            return Load32(mask);
        }

        // The same for 8 pixels.
        //-----------------------------
        template <bool kRotated>
        inline uint64_t
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
        inline uint8x8x4_t
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
        inline uint8x16x2_t
        Interleave(uint8x8x4_t planes) {
            const uint8x16x2_t pairs = vzipq_u8(vcombine_u8(planes.val[0], planes.val[1]), vcombine_u8(planes.val[2], planes.val[3]));
            return vzipq_u8(pairs.val[0], pairs.val[1]);
        }

        // 8 contiguous pixels as one register per channel.
        //-----------------------------
        template <bool kLd4>
        inline uint8x8x4_t
        Load8(const uint8_t *p) {
            if (kLd4) {
                return vld4_u8(p);
            }

            return Deinterleave(vld1q_u8(p), vld1q_u8(p + 16));
        }

        // The opposite.
        //-----------------------------
        template <bool kLd4>
        inline void
        Store8(uint8_t *p, uint8x8x4_t planes) {
            if (kLd4) {
                vst4_u8(p, planes);
            }
            else {
                const uint8x16x2_t pixels = Interleave(planes);
                vst1q_u8(p,      pixels.val[0]);
                vst1q_u8(p + 16, pixels.val[1]);
            }
        }

        // 8 pixels blended with 8 premultiplied sources: each channel of x times the same channel of the color. Under an
        // opaque text the alpha of the blend is opaqueAlpha. Under a translucent one it is the alpha of the source, rounded.
        //-----------------------------
        template <bool kOpaque>
        inline uint8x8x4_t
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
        inline uint8x8x4_t
        BlendCoverage(const Color &color, uint8x8_t coverage, const uint8x8x4_t &dst) {
            uint8x8x4_t x;
            x.val[0] = coverage;
            x.val[1] = coverage;
            x.val[2] = coverage;
            x.val[3] = coverage;
            return BlendPlanes<kOpaque>(color, x, coverage, dst);
        }

        // Writes the 4 pixels at lo and the 4 at hi when the coverage of the 8 lets them skip the blend, and says so.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        inline bool
        DrawAlpha8Shortcut(const Color &color, uint8x8_t coverage, uint8_t *lo, uint8_t *hi) {
            if (kSkip == false) {
                return false;
            }
            const uint64_t m = GetLanes(coverage);
            if (m == 0) {
                return true;
            }
            if (kOpaque && m == ~uint64_t(0)) {
                vst1q_u8(lo, color.solid);
                vst1q_u8(hi, color.solid);
                return true;
            }

            return false;
        }

        // 8 contiguous pixels, with the coverage of each in a lane.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4>
        inline void
        DrawAlpha8Block(const Color &color, uint8x8_t coverage, uint8_t *bytes) {
            if (DrawAlpha8Shortcut<kOpaque, kSkip>(color, coverage, bytes, bytes + 16) == false) {
                Store8<kLd4>(bytes, BlendCoverage<kOpaque>(color, coverage, Load8<kLd4>(bytes)));
            }
        }

        // The same, with the pixels already read: the last block of a row reads them before the block it overlaps writes.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4>
        inline void
        DrawAlpha8BlockRead(const Color &color, uint8x8_t coverage, const uint8x8x4_t &pixels, uint8_t *bytes) {
            if (DrawAlpha8Shortcut<kOpaque, kSkip>(color, coverage, bytes, bytes + 16) == false) {
                Store8<kLd4>(bytes, BlendCoverage<kOpaque>(color, coverage, pixels));
            }
        }

        // 4 pixels at bytes and 4 at backBytes, which can overlap them, with the coverage of the first 4 in the lanes 0
        // to 3 and of the other 4 in the lanes 4 to 7. Both are read before either is written.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        inline void
        DrawAlpha8Halves(const Color &color, uint8x8_t coverage, uint8_t *bytes, uint8_t *backBytes) {
            if (DrawAlpha8Shortcut<kOpaque, kSkip>(color, coverage, bytes, backBytes) == false) {
                const uint8x16x2_t pixels = Interleave(BlendCoverage<kOpaque>(color, coverage, Deinterleave(vld1q_u8(bytes), vld1q_u8(backBytes))));
                vst1q_u8(backBytes, pixels.val[1]);
                vst1q_u8(bytes,     pixels.val[0]);
            }
        }

        // 2 pixels already read, with the coverage of each in a byte of m.
        //-----------------------------
        template <bool kOpaque>
        inline uint8x8_t
        BlendAlpha8Pair(const Color &color, uint32_t m, uint8x8_t pixels) {
            const uint8x8_t coverage = vtbl1_u8(vreinterpret_u8_u32(vdup_n_u32(m)), color.spreadCoverage);
            const uint8x8_t alpha    = kOpaque ? coverage : Div255(vmull_u8(coverage, color.planes.val[3]));
            return Blend(vmull_u8(color.pair, coverage), pixels, alpha);
        }

        // A row of 1 to 3 pixels. With 3, the pixels 0 and 1 and the pixels 1 and 2 are read before either pair is written.
        //-----------------------------
        template <bool kOpaque>
        inline void
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
        template <bool kRotated, bool kOpaque, bool kSkip, bool kLd4>
        ALWAYS_INLINE void
        DrawAlpha8Row(const Color &color, const uint8_t *mask, size_t step, uint32_t *dst, int32_t w) {
            uint8_t *bytes = reinterpret_cast<uint8_t *>(dst);
            if (w >= 8) {
                const size_t      last         = size_t(w - 8);
                uint8_t           *lastBytes   = bytes + 4 * last;
                const uint8x8_t   lastCoverage = vcreate_u8(LoadCoverage8<kRotated>(&mask[last * step], step));
                const uint8x8x4_t lastPixels   = Load8<kLd4>(lastBytes);
                for (size_t x = 0; x < last; x += 8) {
                    DrawAlpha8Block<kOpaque, kSkip, kLd4>(color, vcreate_u8(LoadCoverage8<kRotated>(&mask[x * step], step)), bytes + 4 * x);
                }
                DrawAlpha8BlockRead<kOpaque, kSkip, kLd4>(color, lastCoverage, lastPixels, lastBytes);
            }
            else if (w >= 4) {
                const size_t   back = size_t(w - 4);
                const uint64_t m    = uint64_t(LoadCoverage4<kRotated>(mask, step)) | (uint64_t(LoadCoverage4<kRotated>(&mask[back * step], step)) << 32);
                DrawAlpha8Halves<kOpaque, kSkip>(color, vcreate_u8(m), bytes, bytes + 4 * back);
            }
            else {
                DrawAlpha8Narrow<kOpaque>(color, mask, step, dst, w);
            }
        }

        // The coverage of 4 rows in 4 columns of a rotated glyph, whose texels down a column are contiguous in the
        // texture: 4 bytes per column, column after column.
        //-----------------------------
        inline uint8x16_t
        LoadColumns(const uint8_t *mask, size_t step) {
            const uint64_t first  = uint64_t(Load32(mask))            | (uint64_t(Load32(&mask[step]))     << 32);
            const uint64_t second = uint64_t(Load32(&mask[2 * step])) | (uint64_t(Load32(&mask[3 * step])) << 32);
            return vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(first), vcreate_u64(second)));
        }

        // The coverage of 4 rows in 8 columns, with the 8 of each row in a register: the same reordering as Deinterleave.
        //-----------------------------
        inline uint8x8x4_t
        LoadBandCoverage(const uint8_t *mask, size_t step) {
            return Deinterleave(LoadColumns(mask, step), LoadColumns(&mask[4 * step], step));
        }

        // The block at the same column of the 4 rows of a band, with the coverage of each row in a register. In a partial
        // band the rows before firstRow belong to the band above. The rows are written out because Clang keeps a loop over
        // them, with the coverage and the rows in memory.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4, bool kPartial>
        ALWAYS_INLINE void
        DrawAlpha8BandBlocks(const Color &color, const uint8x8x4_t &coverage, uint8_t *bytes, size_t rowBytes, int32_t firstRow) {
            if (kPartial == false || firstRow <= 0) {
                DrawAlpha8Block<kOpaque, kSkip, kLd4>(color, coverage.val[0], bytes);
            }
            if (kPartial == false || firstRow <= 1) {
                DrawAlpha8Block<kOpaque, kSkip, kLd4>(color, coverage.val[1], bytes + rowBytes);
            }
            if (kPartial == false || firstRow <= 2) {
                DrawAlpha8Block<kOpaque, kSkip, kLd4>(color, coverage.val[2], bytes + 2 * rowBytes);
            }
            DrawAlpha8Block<kOpaque, kSkip, kLd4>(color, coverage.val[3], bytes + 3 * rowBytes);
        }

        // A block and the last block of a row, which overlaps it: both read their pixels before either writes.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4>
        ALWAYS_INLINE void
        DrawAlpha8Overlapping(const Color &color, uint8x8_t coverage, uint8x8_t lastCoverage, uint8_t *bytes, uint8_t *lastBytes) {
            const uint8x8x4_t lastPixels = Load8<kLd4>(lastBytes);
            DrawAlpha8Block<kOpaque, kSkip, kLd4>(color, coverage, bytes);
            DrawAlpha8BlockRead<kOpaque, kSkip, kLd4>(color, lastCoverage, lastPixels, lastBytes);
        }

        // 4 rows of a rotated glyph at least 4 pixels wide, drawn as DrawAlpha8Row draws a row. The coverage of 8 columns
        // takes 8 reads of 4 bytes instead of 32 reads of a byte.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4, bool kPartial>
        ALWAYS_INLINE void
        DrawAlpha8Band(const Color &color, const uint8_t *mask, size_t step, uint32_t *dst, size_t dstStride, int32_t w, int32_t firstRow) {
            uint8_t      *bytes    = reinterpret_cast<uint8_t *>(dst);
            const size_t rowBytes = 4 * dstStride;
            if (w >= 8) {
                const size_t last = size_t(w - 8);
                size_t       x    = 0;
                for (; x + 8 <= last; x += 8) {
                    DrawAlpha8BandBlocks<kOpaque, kSkip, kLd4, kPartial>(color, LoadBandCoverage(&mask[x * step], step), bytes + 4 * x, rowBytes, firstRow);
                }

                const uint8x8x4_t lastCoverage = LoadBandCoverage(&mask[last * step], step);
                if (x < last) {
                    const uint8x8x4_t coverage   = LoadBandCoverage(&mask[x * step], step);
                    uint8_t           *block     = bytes + 4 * x;
                    uint8_t           *lastBlock = bytes + 4 * last;
                    if (kPartial == false || firstRow <= 0) {
                        DrawAlpha8Overlapping<kOpaque, kSkip, kLd4>(color, coverage.val[0], lastCoverage.val[0], block, lastBlock);
                    }
                    if (kPartial == false || firstRow <= 1) {
                        DrawAlpha8Overlapping<kOpaque, kSkip, kLd4>(color, coverage.val[1], lastCoverage.val[1], block + rowBytes, lastBlock + rowBytes);
                    }
                    if (kPartial == false || firstRow <= 2) {
                        DrawAlpha8Overlapping<kOpaque, kSkip, kLd4>(color, coverage.val[2], lastCoverage.val[2], block + 2 * rowBytes, lastBlock + 2 * rowBytes);
                    }
                    DrawAlpha8Overlapping<kOpaque, kSkip, kLd4>(color, coverage.val[3], lastCoverage.val[3], block + 3 * rowBytes, lastBlock + 3 * rowBytes);
                }
                else {
                    DrawAlpha8BandBlocks<kOpaque, kSkip, kLd4, kPartial>(color, lastCoverage, bytes + 4 * last, rowBytes, firstRow);
                }
            }
            else {
                const size_t      back      = size_t(w - 4);
                uint8_t           *backBytes = bytes + 4 * back;
                const uint8x8x4_t coverage  = Deinterleave(LoadColumns(mask, step), LoadColumns(&mask[back * step], step));
                if (kPartial == false || firstRow <= 0) {
                    DrawAlpha8Halves<kOpaque, kSkip>(color, coverage.val[0], bytes, backBytes);
                }
                if (kPartial == false || firstRow <= 1) {
                    DrawAlpha8Halves<kOpaque, kSkip>(color, coverage.val[1], bytes + rowBytes, backBytes + rowBytes);
                }
                if (kPartial == false || firstRow <= 2) {
                    DrawAlpha8Halves<kOpaque, kSkip>(color, coverage.val[2], bytes + 2 * rowBytes, backBytes + 2 * rowBytes);
                }
                DrawAlpha8Halves<kOpaque, kSkip>(color, coverage.val[3], bytes + 3 * rowBytes, backBytes + 3 * rowBytes);
            }
        }

        // A rotated glyph is stored transposed. With bands, and the texels of a column contiguous, it is drawn 4 rows at a
        // time. The 1 to 3 rows left are the end of a last band of 4, which overlaps the one above.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4>
        ALWAYS_INLINE void
        DrawAlpha8RotatedRows(const Color &color, const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                              uint32_t *dst, uint32_t dstStride, bool bands) {
            if (bands && stepY == 1 && width >= 4 && height >= 4) {
                int32_t y = 0;
                for (; y + 4 <= height; y += 4) {
                    DrawAlpha8Band<kOpaque, kSkip, kLd4, false>(color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, width, 0);
                }
                if (y < height) {
                    const int32_t top = height - 4;
                    DrawAlpha8Band<kOpaque, kSkip, kLd4, true>(color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, width, y - top);
                }
            }
            else {
                for (int32_t y = 0; y < height; ++y) {
                    DrawAlpha8Row<true, kOpaque, kSkip, kLd4>(color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride], width);
                }
            }
        }

        //-----------------------------
        template <bool kOpaque, int32_t kSkipWidth, bool kLd4, bool kBands>
        NO_INLINE void
        DrawAlpha8Rotated(const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                          uint32_t *dst, uint32_t dstStride, uint32_t premultiplied) {
            const Color color = MakeColor(premultiplied);
            if (UsesShortcuts<kSkipWidth>(width)) {
                DrawAlpha8RotatedRows<kOpaque, true, kLd4>(color, mask, stepX, stepY, width, height, dst, dstStride, kBands);
            }
            else {
                DrawAlpha8RotatedRows<kOpaque, false, kLd4>(color, mask, stepX, stepY, width, height, dst, dstStride, kBands);
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4>
        ALWAYS_INLINE void
        DrawAlpha8Rows(const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride) {
            for (int32_t y = 0; y < height; ++y) {
                DrawAlpha8Row<false, kOpaque, kSkip, kLd4>(color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride], width);
            }
        }

        // A rotated glyph has a function of its own, so that its registers are not saved for every glyph.
        //-----------------------------
        template <bool kOpaque, int32_t kSkipWidth, bool kLd4, bool kBands>
        NO_INLINE void
        DrawAlpha8Glyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                        uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t /*alpha*/) {
            const uint8_t *mask = &texture[offset];
            if (stepX != 1) {
                DrawAlpha8Rotated<kOpaque, kSkipWidth, kLd4, kBands>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied);
                return;
            }

            const Color color = MakeColor(premultiplied);
            if (UsesShortcuts<kSkipWidth>(width)) {
                DrawAlpha8Rows<kOpaque, true, kLd4>(color, mask, stepY, width, height, dst, dstStride);
            }
            else {
                DrawAlpha8Rows<kOpaque, false, kLd4>(color, mask, stepY, width, height, dst, dstStride);
            }
        }

        // Each channel of the texels times the same channel of the color, rounded.
        //-----------------------------
        inline uint8x8x4_t
        Tint(const Color &color, uint8x8x4_t texels) {
            for (int channel = 0; channel < 4; ++channel) {
                texels.val[channel] = Div255(vmull_u8(texels.val[channel], color.planes.val[channel]));
            }
            return texels;
        }

        // Writes 8 contiguous pixels when their texels let them skip the blend, and says so. Transparent texels leave the
        // pixels as they are. Opaque texels under an opaque text are written tinted, or as they are when the color is white.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4>
        inline bool
        DrawBGRAShortcut(const Color &color, const uint8x8x4_t &texels, uint8_t *bytes) {
            if (kSkip == false) {
                return false;
            }
            const uint64_t alphas = GetLanes(texels.val[3]);
            if (alphas == 0) {
                return true;
            }
            if (kOpaque && alphas == ~uint64_t(0)) {
                Store8<kLd4>(bytes, color.white ? texels : Tint(color, texels));
                return true;
            }

            return false;
        }

        // The premultiplied texel times the premultiplied color, blended with the destination, for one pixel.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawBGRATexel(const Color &color, const uint8_t *texel, uint32_t *dst) {
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
        template <bool kOpaque, bool kSkip, bool kLd4>
        ALWAYS_INLINE void
        DrawBGRARow(const Color &color, const uint8_t *src, uint32_t *dst, int32_t w) {
            uint8_t *bytes = reinterpret_cast<uint8_t *>(dst);
            if (w >= 8) {
                const size_t      last       = size_t(w - 8);
                uint8_t           *lastBytes = bytes + 4 * last;
                const uint8x8x4_t lastTexels = Load8<kLd4>(src + 4 * last);
                const uint8x8x4_t lastPixels = Load8<kLd4>(lastBytes);
                for (size_t x = 0; x < last; x += 8) {
                    uint8_t           *blockBytes = bytes + 4 * x;
                    const uint8x8x4_t texels      = Load8<kLd4>(src + 4 * x);
                    if (DrawBGRAShortcut<kOpaque, kSkip, kLd4>(color, texels, blockBytes) == false) {
                        Store8<kLd4>(blockBytes, BlendPlanes<kOpaque>(color, texels, texels.val[3], Load8<kLd4>(blockBytes)));
                    }
                }
                if (DrawBGRAShortcut<kOpaque, kSkip, kLd4>(color, lastTexels, lastBytes) == false) {
                    Store8<kLd4>(lastBytes, BlendPlanes<kOpaque>(color, lastTexels, lastTexels.val[3], lastPixels));
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
                    DrawBGRATexel<kOpaque>(color, src, dst);
                    src += 4;
                    w--;
                    dst++;
                }
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kLd4>
        ALWAYS_INLINE void
        DrawBGRARows(const Color &color, const uint8_t *src, size_t stepX, size_t stepY, int32_t width, int32_t height,
                     uint32_t *dst, uint32_t dstStride) {
            for (int32_t y = 0; y < height; ++y) {
                uint32_t *row = &dst[size_t(y) * dstStride];
                if (stepX == 1) {
                    DrawBGRARow<kOpaque, kSkip, kLd4>(color, &src[size_t(y) * stepY * 4], row, width);
                }
                else {
                    for (int32_t x = 0; x < width; ++x) {
                        DrawBGRATexel<kOpaque>(color, &src[(size_t(y) * stepY + size_t(x) * stepX) * 4], &row[x]);
                    }
                }
            }
        }

        //-----------------------------
        template <bool kOpaque, int32_t kSkipWidth, bool kLd4>
        NO_INLINE void
        DrawBGRAGlyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                      uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t /*alpha*/) {
            const Color   color = MakeColor(premultiplied);
            const uint8_t *src  = &texture[offset * 4];
            if (UsesShortcuts<kSkipWidth>(width)) {
                DrawBGRARows<kOpaque, true, kLd4>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
            else {
                DrawBGRARows<kOpaque, false, kLd4>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
        }

#undef NO_INLINE
#undef ALWAYS_INLINE

        //-----------------------------
        template <int32_t kSkipWidth, bool kLd4, bool kBands>
        DrawGlyphFunction
        GetDrawGlyphFunction(bool alpha8, bool opaque) {
            if (alpha8) {
                return opaque ? DrawAlpha8Glyph<true, kSkipWidth, kLd4, kBands> : DrawAlpha8Glyph<false, kSkipWidth, kLd4, kBands>;
            }

            return opaque ? DrawBGRAGlyph<true, kSkipWidth, kLd4> : DrawBGRAGlyph<false, kSkipWidth, kLd4>;
        }

        //-----------------------------
        template <int32_t kSkipWidth, bool kLd4, bool kBands>
        inline void
        DrawQuads(Scenario &scenario, uint32_t *dst) {
            const FontBase          &font         = *scenario.font;
            const uint8_t           *texture      = font.GetTexture();
            const size_t            textureWidth  = font.GetTextureWidth();
            const uint32_t          premultiplied = PremultiplyColor(scenario.color);
            const uint32_t          colorAlpha    = scenario.color >> 24;
            const DrawGlyphFunction drawGlyph     = GetDrawGlyphFunction<kSkipWidth, kLd4, kBands>(font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8, colorAlpha == 255);

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
        DrawQuads<kSkipAlways, true, false>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBranchless(Scenario &scenario, uint32_t *dst) {
        DrawQuads<kSkipNever, true, false>(scenario, dst);
    }

    //---------------------------------
    void
    DrawUzp(Scenario &scenario, uint32_t *dst) {
        DrawQuads<kSkipAlways, false, false>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBands(Scenario &scenario, uint32_t *dst) {
        DrawQuads<kSkipAlways, true, true>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBandsBranchless(Scenario &scenario, uint32_t *dst) {
        DrawQuads<kSkipNever, true, true>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBandsSkip32(Scenario &scenario, uint32_t *dst) {
        DrawQuads<32, true, true>(scenario, dst);
    }

} // end of namespace NeonOverlap
} // end of namespace Benchmark

#endif
