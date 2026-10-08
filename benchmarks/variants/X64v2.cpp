#include "benchmarkCommon.h"

#include <smmintrin.h>
//-------------------------------------
#include <cstring>
#include <type_traits>

// The SSE2 code of the library (src/GlyphDrawSse2.cpp) with instructions of x86-64-v2, the level that adds SSE3, SSSE3,
// SSE4.1, SSE4.2 and POPCNT, among others, to SSE2. CMake builds only this file for that level, so the compiler may use those instructions on
// its own too. The same pixels as DrawText.
//
// What changes, each one an option of DrawOptions:
//
// - The blend of an opaque text over Alpha8 uses _mm_maddubs_epi16, which multiplies the unsigned bytes of one register
//   by the signed bytes of another and adds each pair into a 16-bit lane. One register has 255 - m and m, and the other
//   the pixel and the color with 128 taken away so that they fit a signed byte. The sum of the blend then takes 2
//   multiplications instead of 4 multiplications and 2 additions. Under a translucent text, or over BGRA32, the two
//   weights of a lane do not add up to 255, so what the 128 takes away changes from pixel to pixel, and correcting it
//   costs more than the madd saves.
// - _mm_shuffle_epi8 copies each coverage, or the alpha of each texel, to the lanes of its pixel with one instruction
//   instead of two, and the coverage of the 4 rows of a band with 4 instead of 6.
// - _mm_testz_si128 and _mm_testc_si128 tell whether a band has no coverage or full coverage, and whether 4 texels are
//   transparent, without _mm_movemask_epi8 and a comparison.

/*
 * Copyright © 2008 Rodrigo Kumpera
 * Copyright © 2008 André Tupinambá
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Red Hat not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  Red Hat makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 *
 * Author:  Rodrigo Kumpera (kumpera@gmail.com)
 *          André Tupinambá (andrelrt@gmail.com)
 *
 * Based on work by Owen Taylor and Søren Sandmann
 */

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace X64v2 {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                           uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

        // The choices of Draw. Each of the other variants changes one of them, except X64v2Sse2, which turns off the three
        // that use instructions of x86-64-v2, and X64v2WidthsPairs, which turns on both options for rows.
        //-----------------------------
        struct DrawOptions {
            static constexpr bool   kMadd       = true;     // The blend of an opaque text over Alpha8 with _mm_maddubs_epi16
            static constexpr bool   kShuffle    = true;     // Coverage and alpha copied to their lanes with _mm_shuffle_epi8
            static constexpr bool   kTest       = true;     // The shortcuts of a band and of BGRA32 tested with _mm_testz_si128
            static constexpr bool   kWidths     = false;    // Glyphs without shortcuts pick the code of their row width once
            static constexpr bool   kPairs      = false;    // Rows of a glyph of Alpha8 that is not rotated drawn two at a time
            static constexpr bool   kBandWidths = false;    // kWidths for the bands of rotated glyphs
            static constexpr bool   kNoUnroll   = false;    // Keeps clang from unrolling the loop over the blocks of a row
        };

        //-----------------------------
        struct WidthsOptions : DrawOptions {
            static constexpr bool   kWidths     = true;
        };

        //-----------------------------
        struct BandWidthsOptions : DrawOptions {
            static constexpr bool   kBandWidths = true;
        };

        //-----------------------------
        struct WidthsBandWidthsOptions : DrawOptions {
            static constexpr bool   kWidths     = true;
            static constexpr bool   kBandWidths = true;
        };

        //-----------------------------
        struct PairsOptions : DrawOptions {
            static constexpr bool   kPairs      = true;
        };

        //-----------------------------
        struct WidthsPairsOptions : DrawOptions {
            static constexpr bool   kWidths     = true;
            static constexpr bool   kPairs      = true;
        };

        //-----------------------------
        struct NoMaddOptions : DrawOptions {
            static constexpr bool   kMadd       = false;
        };

        //-----------------------------
        struct NoShuffleOptions : DrawOptions {
            static constexpr bool   kShuffle    = false;
        };

        //-----------------------------
        struct NoTestOptions : DrawOptions {
            static constexpr bool   kTest       = false;
        };

        //-----------------------------
        struct NoUnrollOptions : DrawOptions {
            static constexpr bool   kNoUnroll   = true;
        };

        //-----------------------------
        struct Sse2Options {
            static constexpr bool   kMadd       = false;
            static constexpr bool   kShuffle    = false;
            static constexpr bool   kTest       = false;
            static constexpr bool   kWidths     = false;
            static constexpr bool   kPairs      = false;
            static constexpr bool   kBandWidths = false;
            static constexpr bool   kNoUnroll   = false;
        };

        // A compiler can stop inlining the helpers once a glyph function has many of them, and then pass the color through
        // memory for every block. FONTRENDERER_ALWAYS_INLINE keeps them inside the function of the glyph.

        // A glyph at least this wide asks for each block whether it can skip the blend: a block without coverage, or a fully
        // covered one under an opaque text. Wide glyphs have long runs of such blocks. In narrow glyphs they are rare, and the
        // branch costs more than the blends it saves. The blend of a translucent text costs more, so it pays off sooner.
        constexpr int32_t kOpaqueSkipWidth      = 16;
        constexpr int32_t kTranslucentSkipWidth = 8;

        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE bool
        UsesShortcuts(int32_t width) {
            return width >= (kOpaque ? kOpaqueSkipWidth : kTranslucentSkipWidth);
        }

        // std::true_type when the glyph is blended with BlendWeights.
        //-----------------------------
        template <class O, bool kOpaque>
        using UsesMadd = std::integral_constant<bool, O::kMadd && kOpaque>;

        //-----------------------------
        struct Color {
            __m128i     solid;      // The premultiplied color for 4 pixels
            __m128i     even;       // Its blue and red for 4 pixels, each in a 16-bit lane
            __m128i     odd;        // Its green and alpha for 4 pixels
            __m128i     alpha;      // The alpha of the color in every lane
            __m128i     evenMadd;   // even in the high byte of each lane and 0 in the low byte, both XORed with 0x80
            __m128i     oddMadd;    // The same for odd
        };

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE Color
        MakeColor(uint32_t premultiplied, uint32_t alpha) {
            Color color;
            color.solid    = _mm_set1_epi32(int(premultiplied));
            color.even     = _mm_and_si128(color.solid, _mm_set1_epi16(0x00ff));
            color.odd      = _mm_srli_epi16(color.solid, 8);
            color.alpha    = _mm_set1_epi16(int16_t(alpha));
            color.evenMadd = _mm_xor_si128(_mm_slli_epi16(color.even, 8), _mm_set1_epi8(-128));
            color.oddMadd  = _mm_xor_si128(_mm_slli_epi16(color.odd,  8), _mm_set1_epi8(-128));
            return color;
        }

        // The 4 bytes at p as a number. x86 is little-endian, so the first byte is the lowest.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE uint32_t
        Load32(const uint8_t *p) {
            uint32_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        // The same for 2 bytes.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE uint32_t
        Load16(const uint8_t *p) {
            uint16_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        // 4 pixels or texels, as they are in memory.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        LoadQuad(const void *p) {
            return _mm_loadu_si128(static_cast<const __m128i *>(p));
        }

        // Writes the 4 pixels of x.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StoreQuad(void *p, __m128i x) {
            _mm_storeu_si128(static_cast<__m128i *>(p), x);
        }

        // 2 pixels or texels in the low half of a register, and 0 in the high half.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        LoadPair(const void *p) {
            return _mm_loadl_epi64(static_cast<const __m128i *>(p));
        }

        // Writes the 2 pixels in the low half of x.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StorePair(void *p, __m128i x) {
            _mm_storel_epi64(static_cast<__m128i *>(p), x);
        }

        // Writes the 2 pixels in the high half of x.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StoreHighPair(void *p, __m128i x) {
            _mm_storeh_pd(static_cast<double *>(p), _mm_castsi128_pd(x));
        }

        // 1 pixel or texel in the low 32 bits of a register.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        LoadOne(const void *p) {
            return _mm_cvtsi32_si128(int(Load32(static_cast<const uint8_t *>(p))));
        }

        // Writes the pixel in the low 32 bits of x.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StoreOne(void *p, __m128i x) {
            const uint32_t value = uint32_t(_mm_cvtsi128_si32(x));
            memcpy(p, &value, sizeof(value));
        }

        // Each 16-bit lane divided by 255 and rounded: (x + 128) * 257 >> 16. Exact up to 65152, the largest sum of a blend.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Div255(__m128i x) {
            return _mm_mulhi_epu16(_mm_add_epi16(x, _mm_set1_epi16(0x0080)), _mm_set1_epi16(0x0101));
        }

        // Rounded (source + pixels * (255 - alpha)) / 255 in each 16-bit lane, with the source already multiplied.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Blend(__m128i source, __m128i alpha, __m128i pixels) {
            const __m128i inverse = _mm_xor_si128(alpha, _mm_set1_epi16(0x00ff));
            return Div255(_mm_add_epi16(source, _mm_mullo_epi16(pixels, inverse)));
        }

        // The bytes 0, 2, 4 and so on of x, each in a 16-bit lane: the blue and red of 4 pixels.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        EvenBytes(__m128i x) {
            return _mm_and_si128(x, _mm_set1_epi16(0x00ff));
        }

        // The bytes 1, 3, 5 and so on of x, each in a 16-bit lane: the green and alpha of 4 pixels.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        OddBytes(__m128i x) {
            return _mm_srli_epi16(x, 8);
        }

        // The opposite of EvenBytes and OddBytes: 4 pixels again.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        JoinBytes(__m128i even, __m128i odd) {
            return _mm_or_si128(even, _mm_slli_epi16(odd, 8));
        }

        // The 4 coverage bytes in the low 32 bits of x, each copied to the 2 lanes of 16 bits of its pixel: m0 m0 m1 m1 ...
        // A -1 in the mask of _mm_shuffle_epi8 gives a byte of 0.
        //-----------------------------
        template <class O>
        FONTRENDERER_ALWAYS_INLINE __m128i
        Spread(__m128i x) {
            if (O::kShuffle) {
                return _mm_shuffle_epi8(x, _mm_setr_epi8(0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3, -1));
            }

            const __m128i words = _mm_unpacklo_epi8(x, _mm_setzero_si128());
            return _mm_unpacklo_epi16(words, words);
        }

        // Spread for each row of a band, whose coverage has a row in each 32-bit lane.
        //-----------------------------
        template <class O>
        FONTRENDERER_ALWAYS_INLINE void
        SpreadBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
            if (O::kShuffle) {
                row0 = _mm_shuffle_epi8(band, _mm_setr_epi8( 0, -1,  0, -1,  1, -1,  1, -1,  2, -1,  2, -1,  3, -1,  3, -1));
                row1 = _mm_shuffle_epi8(band, _mm_setr_epi8( 4, -1,  4, -1,  5, -1,  5, -1,  6, -1,  6, -1,  7, -1,  7, -1));
                row2 = _mm_shuffle_epi8(band, _mm_setr_epi8( 8, -1,  8, -1,  9, -1,  9, -1, 10, -1, 10, -1, 11, -1, 11, -1));
                row3 = _mm_shuffle_epi8(band, _mm_setr_epi8(12, -1, 12, -1, 13, -1, 13, -1, 14, -1, 14, -1, 15, -1, 15, -1));
                return;
            }

            const __m128i low  = _mm_unpacklo_epi8(band, _mm_setzero_si128());
            const __m128i high = _mm_unpackhi_epi8(band, _mm_setzero_si128());
            row0 = _mm_unpacklo_epi16(low, low);
            row1 = _mm_unpackhi_epi16(low, low);
            row2 = _mm_unpacklo_epi16(high, high);
            row3 = _mm_unpackhi_epi16(high, high);
        }

        // The weights of BlendWeights from the 4 coverage bytes in the low 32 bits of x: in the 2 lanes of 16 bits of each
        // pixel, 255 - m in the low byte and m in the high byte.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Weights(__m128i x) {
            const __m128i bytes = _mm_shuffle_epi8(x, _mm_setr_epi8(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3));
            return _mm_xor_si128(bytes, _mm_set1_epi16(0x00ff));
        }

        // Weights for each row of a band.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        WeightsBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
            const __m128i inverse = _mm_set1_epi16(0x00ff);
            row0 = _mm_xor_si128(_mm_shuffle_epi8(band, _mm_setr_epi8( 0,  0,  0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  3)), inverse);
            row1 = _mm_xor_si128(_mm_shuffle_epi8(band, _mm_setr_epi8( 4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7)), inverse);
            row2 = _mm_xor_si128(_mm_shuffle_epi8(band, _mm_setr_epi8( 8,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 11, 11, 11, 11)), inverse);
            row3 = _mm_xor_si128(_mm_shuffle_epi8(band, _mm_setr_epi8(12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15)), inverse);
        }

        // What the blend of 4 pixels needs from their 4 coverage bytes, in the low 32 bits of x: Weights for BlendWeights,
        // and Spread for BlendCoverage.
        //-----------------------------
        template <class O, bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m128i
        Prepare(__m128i x) {
            return UsesMadd<O, kOpaque>() ? Weights(x) : Spread<O>(x);
        }

        // Prepare for each row of a band.
        //-----------------------------
        template <class O, bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE void
        PrepareBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
            if (UsesMadd<O, kOpaque>()) {
                WeightsBand(band, row0, row1, row2, row3);
            }
            else {
                SpreadBand<O>(band, row0, row1, row2, row3);
            }
        }

        // 4 pixels blended with the color, with the coverage from Spread. A coverage m draws as the premultiplied white texel
        // (m, m, m, m), so a white color texture draws the same. Under an opaque text, the alpha of the blend is the coverage.
        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendCoverage(const Color &color, __m128i spread, __m128i pixels) {
            const __m128i alpha = kOpaque ? spread : Div255(_mm_mullo_epi16(spread, color.alpha));
            const __m128i even  = Blend(_mm_mullo_epi16(color.even, spread), alpha, EvenBytes(pixels));
            const __m128i odd   = Blend(_mm_mullo_epi16(color.odd,  spread), alpha, OddBytes(pixels));
            return JoinBytes(even, odd);
        }

        // BlendCoverage for an opaque text, with the weights from Weights. XORed with 0x80, a byte from 0 to 255 is a signed
        // byte from -128 to 127 that is 128 less. So the sum in each lane is (255 - m) * (pixel - 128) + m * (color - 128),
        // which is the sum of the blend minus 128 * 255, and lies between -32640 and 32385: _mm_maddubs_epi16 never
        // saturates it. Flipping its bit 15 adds 32768: those 128 * 255, and the 128 that rounds the division by 255.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendWeights(const Color &color, __m128i weights, __m128i pixels) {
            const __m128i bias = _mm_set1_epi16(int16_t(0x8000));
            const __m128i even = _mm_maddubs_epi16(weights, _mm_xor_si128(EvenBytes(pixels), color.evenMadd));
            const __m128i odd  = _mm_maddubs_epi16(weights, _mm_xor_si128(OddBytes(pixels),  color.oddMadd));
            return JoinBytes(_mm_mulhi_epu16(_mm_xor_si128(even, bias), _mm_set1_epi16(0x0101)),
                             _mm_mulhi_epu16(_mm_xor_si128(odd,  bias), _mm_set1_epi16(0x0101)));
        }

        // 4 pixels blended with the color, with the coverage from Prepare. An overload picks the blend, not an if: when the
        // blend that is not used stays in the glyph function, even behind a constant condition, MSVC 19.44 keeps the color
        // in memory instead of in registers.
        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendPrepared(std::false_type, const Color &color, __m128i spread, __m128i pixels) {
            return BlendCoverage<kOpaque>(color, spread, pixels);
        }

        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendPrepared(std::true_type, const Color &color, __m128i weights, __m128i pixels) {
            return BlendWeights(color, weights, pixels);
        }

        // The alpha of each of the 4 pixels of odd, which has their green and alpha, over the 2 lanes of its pixel.
        //-----------------------------
        template <class O>
        FONTRENDERER_ALWAYS_INLINE __m128i
        ExpandAlphaOdd(__m128i odd) {
            if (O::kShuffle) {
                return _mm_shuffle_epi8(odd, _mm_setr_epi8(2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15));
            }

            return _mm_shufflehi_epi16(_mm_shufflelo_epi16(odd, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
        }

        // 4 premultiplied texels times the premultiplied color, blended with 4 pixels. The alpha of the blend is the alpha of
        // the texel under an opaque text, and the alpha of the texel times the color, rounded, under a translucent one.
        //-----------------------------
        template <class O, bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendTexels(const Color &color, __m128i texels, __m128i pixels) {
            const __m128i odd       = OddBytes(texels);
            const __m128i sourceOdd = _mm_mullo_epi16(odd, color.odd);
            const __m128i alpha     = ExpandAlphaOdd<O>(kOpaque ? odd : Div255(sourceOdd));
            const __m128i even      = Blend(_mm_mullo_epi16(EvenBytes(texels), color.even), alpha, EvenBytes(pixels));
            return JoinBytes(even, Blend(sourceOdd, alpha, OddBytes(pixels)));
        }

        // The alpha of the 4 texels is 0. _mm_testz_si128 tells whether the AND of its two registers is all 0.
        //-----------------------------
        template <class O>
        FONTRENDERER_ALWAYS_INLINE bool
        IsTransparent(__m128i texels) {
            if (O::kTest) {
                return _mm_testz_si128(texels, _mm_set1_epi32(int(0xff000000u))) != 0;
            }

            return (_mm_movemask_epi8(_mm_cmpeq_epi8(texels, _mm_setzero_si128())) & 0x8888) == 0x8888;
        }

        // The 16 coverage bytes of a band are 0.
        //-----------------------------
        template <class O>
        FONTRENDERER_ALWAYS_INLINE bool
        IsEmptyBand(__m128i band) {
            if (O::kTest) {
                return _mm_testz_si128(band, band) != 0;
            }

            return _mm_movemask_epi8(_mm_cmpeq_epi8(band, _mm_setzero_si128())) == 0xffff;
        }

        // The 16 coverage bytes of a band are 255. _mm_testc_si128(a, b) tells whether b has no bit set that a lacks.
        //-----------------------------
        template <class O>
        FONTRENDERER_ALWAYS_INLINE bool
        IsFullBand(__m128i band) {
            if (O::kTest) {
                return _mm_testc_si128(band, _mm_set1_epi8(-1)) != 0;
            }

            return _mm_movemask_epi8(_mm_cmpeq_epi8(band, _mm_set1_epi8(-1))) == 0xffff;
        }

        // The coverage of 4 pixels in a row, a byte each. The texels of a rotated glyph are step bytes apart.
        //-----------------------------
        template <bool kRotated>
        FONTRENDERER_ALWAYS_INLINE uint32_t
        LoadCoverage(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8) | (uint32_t(mask[2 * step]) << 16) | (uint32_t(mask[3 * step]) << 24);
            }

            return Load32(mask);
        }

        // The same for 2 pixels.
        //-----------------------------
        template <bool kRotated>
        FONTRENDERER_ALWAYS_INLINE uint32_t
        LoadCoveragePair(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8);
            }

            return Load16(mask);
        }

        // The coverage of 4 rows in 4 columns of a rotated glyph: one read of 4 bytes per column, and a transposition. Each
        // 32-bit lane has the 4 columns of a row, with row 0 in the lowest.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        LoadBand(const uint8_t *column0, const uint8_t *column1, const uint8_t *column2, const uint8_t *column3) {
            const __m128i first  = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(Load32(column0))), _mm_cvtsi32_si128(int(Load32(column1))));
            const __m128i second = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(Load32(column2))), _mm_cvtsi32_si128(int(Load32(column3))));
            return _mm_unpacklo_epi16(first, second);
        }

        // 4 pixels at p with their coverage, from Prepare. none and full say that the 4 coverages are 0 or 255. With kSkip,
        // such a block is left as it is, or gets the color under an opaque text. With kRead, pixels are the pixels at p,
        // read before the row wrote anything. Without it, they are read here, after the shortcuts.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        DrawCoverage(const Color &color, __m128i prepared, bool none, bool full, uint32_t *p, __m128i pixels) {
            if (kSkip && none) {
                return;
            }
            if (kSkip && kOpaque && full) {
                StoreQuad(p, color.solid);
                return;
            }

            StoreQuad(p, BlendPrepared<kOpaque>(UsesMadd<O, kOpaque>(), color, prepared, kRead ? pixels : LoadQuad(p)));
        }

        // One texel over one pixel. A transparent texel leaves the pixel as it is.
        //-----------------------------
        template <class O, bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE void
        DrawTexel(const Color &color, const uint8_t *texel, uint32_t *p) {
            const uint32_t t = Load32(texel);
            if ((t >> 24) != 0) {
                StoreOne(p, BlendTexels<O, kOpaque>(color, _mm_cvtsi32_si128(int(t)), LoadOne(p)));
            }
        }

        // DrawBlocks draws a row of a glyph with the functions of one of these: a row of Alpha8, 4 rows of a rotated glyph
        // of Alpha8, or a row of BGRA32. ReadQuad returns the pixels of a block before the row writes anything.

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, bool kRotated>
        struct Alpha8Row {
            static constexpr bool kNoUnroll = O::kNoUnroll;

            const Color     &color;
            const uint8_t   *mask;      // The coverage of the first pixel of the row
            size_t          step;       // Between two pixels of the row
            uint32_t        *dst;

            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, __m128i pixels) const {
                const uint32_t m = LoadCoverage<kRotated>(&mask[x * step], step);
                DrawCoverage<O, kOpaque, kSkip, kRead>(color, Prepare<O, kOpaque>(_mm_cvtsi32_si128(int(m))), m == 0, m == 0xffffffffu, &dst[x], pixels);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Quad(size_t x) const {
                Draw<false>(x, _mm_setzero_si128());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE __m128i
            ReadQuad(size_t x) const {
                return LoadQuad(&dst[x]);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            QuadRead(size_t x, __m128i pixels) const {
                Draw<true>(x, pixels);
            }

            // A row of 1 to 3 pixels. With 3, the pixels 0 and 1 and the pixels 1 and 2 are read first and blended as one
            // block of 4.
            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Narrow(int32_t w) const {
                if (w == 1) {
                    StoreOne(dst, BlendPrepared<kOpaque>(UsesMadd<O, kOpaque>(), color, Prepare<O, kOpaque>(_mm_cvtsi32_si128(int(mask[0]))), LoadOne(dst)));
                }
                else if (w == 2) {
                    StorePair(dst, BlendPrepared<kOpaque>(UsesMadd<O, kOpaque>(), color, Prepare<O, kOpaque>(_mm_cvtsi32_si128(int(LoadCoveragePair<kRotated>(mask, step)))), LoadPair(dst)));
                }
                else {
                    const uint32_t m      = LoadCoveragePair<kRotated>(mask, step) | (LoadCoveragePair<kRotated>(&mask[step], step) << 16);
                    const __m128i  pixels = _mm_unpacklo_epi64(LoadPair(dst), LoadPair(&dst[1]));
                    const __m128i  result = BlendPrepared<kOpaque>(UsesMadd<O, kOpaque>(), color, Prepare<O, kOpaque>(_mm_cvtsi32_si128(int(m))), pixels);
                    StoreHighPair(&dst[1], result);
                    StorePair(dst, result);
                }
            }
        };

        // The rows are written out because a loop over them, with a variable index, can make a compiler keep the coverage
        // and the rows in memory.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, bool kPartial>
        struct Alpha8Band {
            static constexpr bool kNoUnroll = O::kNoUnroll;

            const Color     &color;
            const uint8_t   *mask;      // The coverage of the first pixel of the band
            size_t          step;       // Between two columns of the glyph
            uint32_t        *dst;
            size_t          dstStride;
            int32_t         firstRow;   // In a partial band, the rows before it belong to the band above

            struct Rows {
                __m128i     row0;
                __m128i     row1;
                __m128i     row2;
                __m128i     row3;
            };

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE bool
            Draws(int32_t row) const {
                return kPartial == false || row >= firstRow;
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE uint32_t *
            At(int32_t row, size_t x) const {
                return &dst[size_t(row) * dstStride + x];
            }

            // With kSkip, the 4 rows skip the blend together or not at all, with one branch instead of one per row. In a
            // partial band, the test includes the rows of the band above, which can only make the shortcut rarer.
            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, const Rows &pixels) const {
                const uint8_t *column = &mask[x * step];
                const __m128i band    = LoadBand(column, &column[step], &column[2 * step], &column[3 * step]);
                if (kSkip) {
                    if (IsEmptyBand<O>(band)) {
                        return;
                    }
                    if (kOpaque && IsFullBand<O>(band)) {
                        if (Draws(0)) {
                            StoreQuad(At(0, x), color.solid);
                        }
                        if (Draws(1)) {
                            StoreQuad(At(1, x), color.solid);
                        }
                        if (Draws(2)) {
                            StoreQuad(At(2, x), color.solid);
                        }
                        StoreQuad(At(3, x), color.solid);
                        return;
                    }
                }

                __m128i prepared0, prepared1, prepared2, prepared3;
                PrepareBand<O, kOpaque>(band, prepared0, prepared1, prepared2, prepared3);
                if (Draws(0)) {
                    DrawCoverage<O, kOpaque, false, kRead>(color, prepared0, false, false, At(0, x), pixels.row0);
                }
                if (Draws(1)) {
                    DrawCoverage<O, kOpaque, false, kRead>(color, prepared1, false, false, At(1, x), pixels.row1);
                }
                if (Draws(2)) {
                    DrawCoverage<O, kOpaque, false, kRead>(color, prepared2, false, false, At(2, x), pixels.row2);
                }
                DrawCoverage<O, kOpaque, false, kRead>(color, prepared3, false, false, At(3, x), pixels.row3);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Quad(size_t x) const {
                Draw<false>(x, Rows());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE Rows
            ReadQuad(size_t x) const {
                Rows pixels;
                pixels.row0 = LoadQuad(At(0, x));
                pixels.row1 = LoadQuad(At(1, x));
                pixels.row2 = LoadQuad(At(2, x));
                pixels.row3 = LoadQuad(At(3, x));
                return pixels;
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            QuadRead(size_t x, const Rows &pixels) const {
                Draw<true>(x, pixels);
            }
        };

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        struct BGRARow {
            static constexpr bool kNoUnroll = O::kNoUnroll;

            const Color     &color;
            const uint8_t   *src;       // The texel of the first pixel of the row
            uint32_t        *dst;

            // With kSkip, a block of transparent texels is left as it is. A block of opaque texels under an opaque text is
            // blended like any other: tinting it without the blend did not pay for the branch.
            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, __m128i pixels) const {
                const __m128i texels = LoadQuad(&src[4 * x]);
                if (kSkip && IsTransparent<O>(texels)) {
                    return;
                }

                StoreQuad(&dst[x], BlendTexels<O, kOpaque>(color, texels, kRead ? pixels : LoadQuad(&dst[x])));
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Quad(size_t x) const {
                Draw<false>(x, _mm_setzero_si128());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE __m128i
            ReadQuad(size_t x) const {
                return LoadQuad(&dst[x]);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            QuadRead(size_t x, __m128i pixels) const {
                Draw<true>(x, pixels);
            }

            // A row of 1 to 3 pixels, as Alpha8Row::Narrow draws it.
            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Narrow(int32_t w) const {
                if (w == 1) {
                    StoreOne(dst, BlendTexels<O, kOpaque>(color, LoadOne(src), LoadOne(dst)));
                }
                else if (w == 2) {
                    StorePair(dst, BlendTexels<O, kOpaque>(color, LoadPair(src), LoadPair(dst)));
                }
                else {
                    const __m128i texels = _mm_unpacklo_epi64(LoadPair(src), LoadPair(&src[4]));
                    const __m128i pixels = _mm_unpacklo_epi64(LoadPair(dst), LoadPair(&dst[1]));
                    const __m128i result = BlendTexels<O, kOpaque>(color, texels, pixels);
                    StoreHighPair(&dst[1], result);
                    StorePair(dst, result);
                }
            }
        };

        // The blocks of a row before its last one.
        //-----------------------------
        template <class TLine>
        FONTRENDERER_ALWAYS_INLINE void
        DrawQuadsBefore(std::false_type, const TLine &line, size_t tail) {
            for (size_t x = 0; x < tail; x += 4) {
                line.Quad(x);
            }
        }

        // Built for x86-64-v3, clang 19 unrolls the loop above by two, and a row then needs more instructions and
        // branches before its first block. It does not unroll it for x86-64-v2.
        //-----------------------------
        template <class TLine>
        FONTRENDERER_ALWAYS_INLINE void
        DrawQuadsBefore(std::true_type, const TLine &line, size_t tail) {
#if defined(__clang__)
            #pragma clang loop unroll(disable)
#endif
            for (size_t x = 0; x < tail; x += 4) {
                line.Quad(x);
            }
        }

        // A row of at least 4 pixels, or a band of 4 such rows. The last block is read before the row writes anything, and
        // the blocks before it are drawn as they come, so a row needs no branch on how many pixels are left.
        //-----------------------------
        template <class TLine>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBlocks(const TLine &line, int32_t w) {
            const size_t tail   = size_t(w) - 4;
            const auto   pixels = line.ReadQuad(tail);
            DrawQuadsBefore(std::integral_constant<bool, TLine::kNoUnroll>(), line, tail);
            line.QuadRead(tail, pixels);
        }

        // The clipping of DrawText can leave a width below 1, which draws nothing.
        //-----------------------------
        template <class TRow>
        FONTRENDERER_ALWAYS_INLINE void
        DrawRow(const TRow &row, int32_t w) {
            if (w >= 4) {
                DrawBlocks(row, w);
            }
            else if (w > 0) {
                row.Narrow(w);
            }
        }

        // The widths of row that a glyph can pick once for all its rows, so that they need no branch on the width.
        enum class EWidth {
            Any,        // DrawRow decides for each row
            Narrow,     // 1 to 3 pixels
            One,        // 4 pixels: one block
            Two,        // 5 to 8 pixels: two blocks, which overlap below 8
        };

        template <EWidth kWidth>
        using WidthTag = std::integral_constant<EWidth, kWidth>;

        //-----------------------------
        template <class TRow>
        FONTRENDERER_ALWAYS_INLINE void
        DrawRowOf(WidthTag<EWidth::Any>, const TRow &row, int32_t w) {
            DrawRow(row, w);
        }

        //-----------------------------
        template <class TRow>
        FONTRENDERER_ALWAYS_INLINE void
        DrawRowOf(WidthTag<EWidth::Narrow>, const TRow &row, int32_t w) {
            row.Narrow(w);
        }

        //-----------------------------
        template <class TRow>
        FONTRENDERER_ALWAYS_INLINE void
        DrawRowOf(WidthTag<EWidth::One>, const TRow &row, int32_t) {
            row.QuadRead(0, row.ReadQuad(0));
        }

        // As DrawBlocks, with one block before the last one.
        //-----------------------------
        template <class TRow>
        FONTRENDERER_ALWAYS_INLINE void
        DrawRowOf(WidthTag<EWidth::Two>, const TRow &row, int32_t w) {
            const size_t tail   = size_t(w) - 4;
            const auto   pixels = row.ReadQuad(tail);
            row.Quad(0);
            row.QuadRead(tail, pixels);
        }

        // Two rows of a glyph, drawn as one: their blocks share the loop, and the processor gets two chains of work that
        // do not wait for each other.
        //-----------------------------
        template <class TRow>
        struct RowPair {
            static constexpr bool kNoUnroll = TRow::kNoUnroll;

            TRow    first;
            TRow    second;

            struct Pixels {
                __m128i     first;
                __m128i     second;
            };

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Quad(size_t x) const {
                first.Quad(x);
                second.Quad(x);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE Pixels
            ReadQuad(size_t x) const {
                Pixels pixels;
                pixels.first  = first.ReadQuad(x);
                pixels.second = second.ReadQuad(x);
                return pixels;
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            QuadRead(size_t x, const Pixels &pixels) const {
                first.QuadRead(x, pixels.first);
                second.QuadRead(x, pixels.second);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Narrow(int32_t w) const {
                first.Narrow(w);
                second.Narrow(w);
            }
        };

        // DrawRowOf for a band, which is always at least 4 pixels wide.
        //-----------------------------
        template <class TBand>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBandOf(WidthTag<EWidth::Any>, const TBand &band, int32_t w) {
            DrawBlocks(band, w);
        }

        //-----------------------------
        template <class TBand>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBandOf(WidthTag<EWidth::One> tag, const TBand &band, int32_t w) {
            DrawRowOf(tag, band, w);
        }

        //-----------------------------
        template <class TBand>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBandOf(WidthTag<EWidth::Two> tag, const TBand &band, int32_t w) {
            DrawRowOf(tag, band, w);
        }

        // The bands of a rotated glyph at least 4 pixels wide and tall. kWidth says which widths it can have.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, EWidth kWidth>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBandsOf(const Color &color, const uint8_t *mask, size_t stepX, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride) {
            int32_t y = 0;
            for (; y + 4 <= height; y += 4) {
                DrawBandOf(WidthTag<kWidth>(), Alpha8Band<O, kOpaque, kSkip, false> { color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, 0 }, width);
            }
            if (y < height) {
                const int32_t top = height - 4;
                DrawBandOf(WidthTag<kWidth>(), Alpha8Band<O, kOpaque, kSkip, true> { color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, y - top }, width);
            }
        }

        // The bands of a rotated glyph, with any width, or, with the first tag, with the code of their width.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBands(std::false_type, const Color &color, const uint8_t *mask, size_t stepX, int32_t width, int32_t height,
                  uint32_t *dst, uint32_t dstStride) {
            DrawBandsOf<O, kOpaque, kSkip, EWidth::Any>(color, mask, stepX, width, height, dst, dstStride);
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBands(std::true_type, const Color &color, const uint8_t *mask, size_t stepX, int32_t width, int32_t height,
                  uint32_t *dst, uint32_t dstStride) {
            if (width > 8) {
                DrawBandsOf<O, kOpaque, kSkip, EWidth::Any>(color, mask, stepX, width, height, dst, dstStride);
            }
            else if (width > 4) {
                DrawBandsOf<O, kOpaque, kSkip, EWidth::Two>(color, mask, stepX, width, height, dst, dstStride);
            }
            else {
                DrawBandsOf<O, kOpaque, kSkip, EWidth::One>(color, mask, stepX, width, height, dst, dstStride);
            }
        }

        // A rotated glyph has a function of its own, one for each kSkip, so that the glyph function does not save the
        // registers it uses for every glyph. Its rows are drawn 4 at a time when the texels of a column are contiguous.
        // As with kWidths, only glyphs without shortcuts pick the code of their width.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_NO_INLINE void
        DrawAlpha8Rotated(const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                          uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const Color color = MakeColor(premultiplied, alpha);
            if (stepY == 1 && width >= 4 && height >= 4) {
                using BandWidths = std::integral_constant<bool, O::kBandWidths && !kSkip>;
                DrawBands<O, kOpaque, kSkip>(BandWidths(), color, mask, stepX, width, height, dst, dstStride);
            }
            else {
                for (int32_t y = 0; y < height; ++y) {
                    DrawRow(Alpha8Row<O, kOpaque, kSkip, true> { color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride] }, width);
                }
            }
        }

        // The rows of a glyph that is not rotated, one at a time or, with the first tag, two at a time. kWidth says which
        // widths they can have.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, EWidth kWidth>
        FONTRENDERER_ALWAYS_INLINE void
        DrawAlpha8RowsOf(std::false_type, const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height,
                         uint32_t *dst, uint32_t dstStride) {
            for (int32_t y = 0; y < height; ++y) {
                DrawRowOf(WidthTag<kWidth>(), Alpha8Row<O, kOpaque, kSkip, false> { color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride] }, width);
            }
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, EWidth kWidth>
        FONTRENDERER_ALWAYS_INLINE void
        DrawAlpha8RowsOf(std::true_type, const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height,
                         uint32_t *dst, uint32_t dstStride) {
            using Row = Alpha8Row<O, kOpaque, kSkip, false>;
            int32_t y = 0;
            for (; y + 2 <= height; y += 2) {
                const Row first  { color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride] };
                const Row second { color, &mask[size_t(y + 1) * stepY], 1, &dst[size_t(y + 1) * dstStride] };
                DrawRowOf(WidthTag<kWidth>(), RowPair<Row> { first, second }, width);
            }
            if (y < height) {
                DrawRowOf(WidthTag<kWidth>(), Row { color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride] }, width);
            }
        }

        // The rows of a glyph that is not rotated, with any width, or, with the first tag, with the code of their width.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawAlpha8Rows(std::false_type, const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height,
                       uint32_t *dst, uint32_t dstStride) {
            using Pairs = std::integral_constant<bool, O::kPairs>;
            DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::Any>(Pairs(), color, mask, stepY, width, height, dst, dstStride);
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawAlpha8Rows(std::true_type, const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height,
                       uint32_t *dst, uint32_t dstStride) {
            using Pairs = std::integral_constant<bool, O::kPairs>;
            if (width > 8) {
                DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::Any>(Pairs(), color, mask, stepY, width, height, dst, dstStride);
            }
            else if (width > 4) {
                DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::Two>(Pairs(), color, mask, stepY, width, height, dst, dstStride);
            }
            else if (width == 4) {
                DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::One>(Pairs(), color, mask, stepY, width, height, dst, dstStride);
            }
            else if (width > 0) {
                DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::Narrow>(Pairs(), color, mask, stepY, width, height, dst, dstStride);
            }
        }

        // src/GlyphDraw.cpp explains why these functions are never inlined.
        //-----------------------------
        template <class O, bool kOpaque>
        FONTRENDERER_NO_INLINE void
        DrawAlpha8(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                   uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const uint8_t *mask = &texture[offset];
            if (stepX != 1) {
                if (UsesShortcuts<kOpaque>(width)) {
                    DrawAlpha8Rotated<O, kOpaque, true>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
                }
                else {
                    DrawAlpha8Rotated<O, kOpaque, false>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
                }
                return;
            }

            // The glyphs that ask for the shortcuts are 8 pixels wide or more, where the cases of width barely apply.
            const Color color = MakeColor(premultiplied, alpha);
            if (UsesShortcuts<kOpaque>(width)) {
                DrawAlpha8Rows<O, kOpaque, true>(std::false_type(), color, mask, stepY, width, height, dst, dstStride);
            }
            else {
                using Widths = std::integral_constant<bool, O::kWidths>;
                DrawAlpha8Rows<O, kOpaque, false>(Widths(), color, mask, stepY, width, height, dst, dstStride);
            }
        }

        // A rotated glyph is drawn one pixel at a time: none of the measured texts with a color texture had one.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBGRARows(const Color &color, const uint8_t *src, size_t stepX, size_t stepY, int32_t width, int32_t height,
                     uint32_t *dst, uint32_t dstStride) {
            if (stepX != 1) {
                for (int32_t y = 0; y < height; ++y) {
                    uint32_t *row = &dst[size_t(y) * dstStride];
                    for (int32_t x = 0; x < width; ++x) {
                        DrawTexel<O, kOpaque>(color, &src[(size_t(y) * stepY + size_t(x) * stepX) * 4], &row[x]);
                    }
                }
                return;
            }

            for (int32_t y = 0; y < height; ++y) {
                DrawRow(BGRARow<O, kOpaque, kSkip> { color, &src[size_t(y) * stepY * 4], &dst[size_t(y) * dstStride] }, width);
            }
        }

        //-----------------------------
        template <class O, bool kOpaque>
        FONTRENDERER_NO_INLINE void
        DrawBGRA(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                 uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const Color   color = MakeColor(premultiplied, alpha);
            const uint8_t *src  = &texture[offset * 4];
            if (UsesShortcuts<kOpaque>(width)) {
                DrawBGRARows<O, kOpaque, true>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
            else {
                DrawBGRARows<O, kOpaque, false>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
        }

        //-----------------------------
        template <class O>
        DrawGlyphFunction
        GetDrawGlyphFunction(bool alpha8, bool opaque) {
            if (alpha8) {
                return opaque ? DrawAlpha8<O, true> : DrawAlpha8<O, false>;
            }

            return opaque ? DrawBGRA<O, true> : DrawBGRA<O, false>;
        }

        //-----------------------------
        template <class O>
        inline void
        DrawQuads(Scenario &scenario, uint32_t *dst) {
            const FontBase          &font         = *scenario.font;
            const uint8_t           *texture      = font.GetTexture();
            const size_t            textureWidth  = font.GetTextureWidth();
            const uint32_t          premultiplied = PremultiplyColor(scenario.color);
            const uint32_t          colorAlpha    = scenario.color >> 24;
            const DrawGlyphFunction drawGlyph     = GetDrawGlyphFunction<O>(font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8, colorAlpha == 255);

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
        DrawQuads<DrawOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawNoMadd(Scenario &scenario, uint32_t *dst) {
        DrawQuads<NoMaddOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawNoShuffle(Scenario &scenario, uint32_t *dst) {
        DrawQuads<NoShuffleOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawNoTest(Scenario &scenario, uint32_t *dst) {
        DrawQuads<NoTestOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawNoUnroll(Scenario &scenario, uint32_t *dst) {
        DrawQuads<NoUnrollOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSse2(Scenario &scenario, uint32_t *dst) {
        DrawQuads<Sse2Options>(scenario, dst);
    }

    //---------------------------------
    void
    DrawWidths(Scenario &scenario, uint32_t *dst) {
        DrawQuads<WidthsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawPairs(Scenario &scenario, uint32_t *dst) {
        DrawQuads<PairsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawWidthsPairs(Scenario &scenario, uint32_t *dst) {
        DrawQuads<WidthsPairsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBandWidths(Scenario &scenario, uint32_t *dst) {
        DrawQuads<BandWidthsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawWidthsBandWidths(Scenario &scenario, uint32_t *dst) {
        DrawQuads<WidthsBandWidthsOptions>(scenario, dst);
    }

} // end of namespace X64v2
} // end of namespace Benchmark
