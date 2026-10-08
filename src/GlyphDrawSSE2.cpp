//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

// The functions of GlyphDraw.cpp with SSE2, and the same pixels.
//
// A row is drawn in blocks of 4 pixels. Its last block ends where the row ends, so it can overlap the block before it.
// Its pixels are read before the row writes anything: both blocks then compute the same values for the pixels they
// share, and the one that writes last changes nothing. Read after the block before it is written, they would wait until
// that write reaches the cache, because x86 cannot pass part of a recent write to a read. A row of 1 to 3 pixels is
// drawn 2 pixels at a time.
//
// A rotated glyph is stored transposed, so the texels down a column of the glyph are contiguous in the texture. Its
// rows are drawn 4 at a time: 4 reads of 4 bytes and a transposition give the coverage of 4 rows in 4 columns. The 1 to
// 3 rows left are the end of a last band of 4, which overlaps the one above.
//
// The blend keeps the blue and red of 4 pixels in one register and their green and alpha in another, each channel in a
// 16-bit lane, as the scalar code does with two channels per uint32_t. Separating the channels takes an AND and a shift,
// which x86 runs on several ports. Unpacking each channel to 16 bits, as pixman does, runs on a single one. Each channel
// is round((source + destination * (255 - alpha)) / 255), with a source that is not yet divided by 255, so it is rounded
// once, as in the scalar code.
//
// The division by 255 with _mm_mulhi_epu16 and the test of the alpha of 4 texels with _mm_movemask_epi8 come from pixman.
//
// GlyphDrawX64v2.cpp builds this file again for x86-64-v2, with FONTRENDERER_GLYPHDRAW_X64V2 defined. That build uses
// instructions that SSE2 lacks in three places, each behind that macro:
// - The blend of an opaque text over Alpha8 uses _mm_maddubs_epi16 (SSSE3), which multiplies the unsigned bytes of one
//   register by the signed bytes of another and adds each pair into a 16-bit lane. One register has 255 - m and m, and
//   the other the pixel and the color with 128 taken away, so that they fit a signed byte. The sum of the blend then
//   takes 2 multiplications instead of 4 multiplications and 2 additions. Under a translucent text, or with a texture
//   of BGRA32, the two weights of a lane do not add up to 255, so what the 128 takes away changes from pixel to pixel,
//   and correcting it costs more than the multiplication saves.
// - _mm_shuffle_epi8 (SSSE3) copies each coverage, or the alpha of each texel, to the lanes of its pixel with one
//   instruction instead of two, and the coverage of the 4 rows of a band with 4 instead of 6.
// - _mm_testz_si128 and _mm_testc_si128 (SSE4.1) tell whether a band has no coverage or full coverage, and whether 4
//   texels are transparent, without _mm_movemask_epi8 and a comparison.
//
// GlyphDrawX64v3.cpp builds this file a third time, for x86-64-v3, with FONTRENDERER_GLYPHDRAW_X64V3 defined too. In
// that build a glyph 8 pixels wide or more is drawn in blocks of 8 pixels in registers of 256 bits, as the blocks of 4
// are, and a narrower one as in the build for x86-64-v2. A rotated glyph of Alpha8 draws bands of 4 rows by 8 columns:
// two transpositions of 4 columns, one in each half of the register.

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

#include "GlyphDraw.h"

// With the whole library built for x86-64-v2, only the build of GlyphDrawX64v2.cpp draws.
#if defined(FONTRENDERER_SSE2) && (defined(FONTRENDERER_GLYPHDRAW_X64V2) || !defined(FONTRENDERER_X86_64_V2))

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    #include <immintrin.h>      // AVX2, which includes SSE4.1
#elif defined(FONTRENDERER_GLYPHDRAW_X64V2)
    #include <smmintrin.h>      // SSE4.1, which includes SSSE3
#else
    #include <emmintrin.h>
#endif
//-------------------------------------
#include <cstring>
#include <type_traits>

using namespace MindShake;

//-------------------------------------
namespace {

    // A compiler can stop inlining the helpers once a glyph function has many of them, and then pass the color through
    // memory for every block. FONTRENDERER_ALWAYS_INLINE keeps them inside the function of the glyph.

    // A glyph at least this wide asks for each block whether it can skip the blend: a block without coverage, or a fully
    // covered one under an opaque text. Wide glyphs have long runs of such blocks. In narrow glyphs they are rare, and the
    // branch costs more than the blends it saves. The blend of a translucent text costs more, so it pays off sooner.
    constexpr int32_t kOpaqueSkipWidth      = 16;
    constexpr int32_t kTranslucentSkipWidth = 8;

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE bool
    UsesShortcuts(int32_t width) {
        return width >= (kOpaque ? kOpaqueSkipWidth : kTranslucentSkipWidth);
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
    constexpr bool kX64v2 = true;
#else
    constexpr bool kX64v2 = false;
#endif

    // std::true_type when the blend of the glyph is BlendWeights, with _mm_maddubs_epi16.
    template <bool kOpaque>
    using UsesMadd = std::integral_constant<bool, kX64v2 && kOpaque>;

    //---------------------------------
    struct Color {
        __m128i     solid;      // The premultiplied color for 4 pixels
        __m128i     even;       // Its blue and red for 4 pixels, each in a 16-bit lane
        __m128i     odd;        // Its green and alpha for 4 pixels
        __m128i     alpha;      // The alpha of the color in every lane
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        __m128i     evenMadd;   // even in the high byte of each lane and 0 in the low byte, both XORed with 0x80
        __m128i     oddMadd;    // The same for odd
#endif
    };

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE Color
    MakeColor(uint32_t premultiplied, uint32_t alpha) {
        Color color;
        color.solid    = _mm_set1_epi32(int(premultiplied));
        color.even     = _mm_and_si128(color.solid, _mm_set1_epi16(0x00ff));
        color.odd      = _mm_srli_epi16(color.solid, 8);
        color.alpha    = _mm_set1_epi16(int16_t(alpha));
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        color.evenMadd = _mm_xor_si128(_mm_slli_epi16(color.even, 8), _mm_set1_epi8(-128));
        color.oddMadd  = _mm_xor_si128(_mm_slli_epi16(color.odd,  8), _mm_set1_epi8(-128));
#endif
        return color;
    }

    // The 4 bytes at p as a number. x86 is little-endian, so the first byte is the lowest.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE uint32_t
    Load32(const uint8_t *p) {
        uint32_t value;
        memcpy(&value, p, sizeof(value));
        return value;
    }

    // The same for 2 bytes.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE uint32_t
    Load16(const uint8_t *p) {
        uint16_t value;
        memcpy(&value, p, sizeof(value));
        return value;
    }

    // 4 pixels or texels, as they are in memory.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    LoadQuad(const void *p) {
        return _mm_loadu_si128(static_cast<const __m128i *>(p));
    }

    // Writes the 4 pixels of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StoreQuad(void *p, __m128i x) {
        _mm_storeu_si128(static_cast<__m128i *>(p), x);
    }

    // 2 pixels or texels in the low half of a register, and 0 in the high half.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    LoadPair(const void *p) {
        return _mm_loadl_epi64(static_cast<const __m128i *>(p));
    }

    // Writes the 2 pixels in the low half of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StorePair(void *p, __m128i x) {
        _mm_storel_epi64(static_cast<__m128i *>(p), x);
    }

    // Writes the 2 pixels in the high half of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StoreHighPair(void *p, __m128i x) {
        _mm_storeh_pd(static_cast<double *>(p), _mm_castsi128_pd(x));
    }

    // 1 pixel or texel in the low 32 bits of a register.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    LoadOne(const void *p) {
        return _mm_cvtsi32_si128(int(Load32(static_cast<const uint8_t *>(p))));
    }

    // Writes the pixel in the low 32 bits of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StoreOne(void *p, __m128i x) {
        const uint32_t value = uint32_t(_mm_cvtsi128_si32(x));
        memcpy(p, &value, sizeof(value));
    }

    // Each 16-bit lane divided by 255 and rounded: (x + 128) * 257 >> 16. Exact up to 65152, the largest sum of a blend.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    Div255(__m128i x) {
        return _mm_mulhi_epu16(_mm_add_epi16(x, _mm_set1_epi16(0x0080)), _mm_set1_epi16(0x0101));
    }

    // Rounded (source + pixels * (255 - alpha)) / 255 in each 16-bit lane, with the source already multiplied.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    Blend(__m128i source, __m128i alpha, __m128i pixels) {
        const __m128i inverse = _mm_xor_si128(alpha, _mm_set1_epi16(0x00ff));
        return Div255(_mm_add_epi16(source, _mm_mullo_epi16(pixels, inverse)));
    }

    // The bytes 0, 2, 4 and so on of x, each in a 16-bit lane: the blue and red of 4 pixels.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    EvenBytes(__m128i x) {
        return _mm_and_si128(x, _mm_set1_epi16(0x00ff));
    }

    // The bytes 1, 3, 5 and so on of x, each in a 16-bit lane: the green and alpha of 4 pixels.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    OddBytes(__m128i x) {
        return _mm_srli_epi16(x, 8);
    }

    // The opposite of EvenBytes and OddBytes: 4 pixels again.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    JoinBytes(__m128i even, __m128i odd) {
        return _mm_or_si128(even, _mm_slli_epi16(odd, 8));
    }

    // The 4 coverage bytes in the low 32 bits of x, each copied to the 2 lanes of 16 bits of its pixel: m0 m0 m1 m1 ...
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    Spread(__m128i x) {
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        // A -1 in the mask of _mm_shuffle_epi8 gives a byte of 0.
        return _mm_shuffle_epi8(x, _mm_setr_epi8(0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3, -1));
#elif defined(__clang__)
        // Clang 19 merges the two unpacks of the other form into one byte shuffle and, with SSE2 alone, builds it with
        // 8 instructions. It builds this form, the same value through a shift, with 2 unpacks. Clang 14 builds both with
        // 2. GCC and MSVC build each form as written, so the shift would cost them one more instruction.
        const __m128i twice = _mm_unpacklo_epi8(x, x);
        return _mm_srli_epi16(_mm_unpacklo_epi16(twice, twice), 8);
#else
        const __m128i words = _mm_unpacklo_epi8(x, _mm_setzero_si128());
        return _mm_unpacklo_epi16(words, words);
#endif
    }

    // Spread for each row of a band, whose coverage has a row in each 32-bit lane.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    SpreadBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        row0 = _mm_shuffle_epi8(band, _mm_setr_epi8( 0, -1,  0, -1,  1, -1,  1, -1,  2, -1,  2, -1,  3, -1,  3, -1));
        row1 = _mm_shuffle_epi8(band, _mm_setr_epi8( 4, -1,  4, -1,  5, -1,  5, -1,  6, -1,  6, -1,  7, -1,  7, -1));
        row2 = _mm_shuffle_epi8(band, _mm_setr_epi8( 8, -1,  8, -1,  9, -1,  9, -1, 10, -1, 10, -1, 11, -1, 11, -1));
        row3 = _mm_shuffle_epi8(band, _mm_setr_epi8(12, -1, 12, -1, 13, -1, 13, -1, 14, -1, 14, -1, 15, -1, 15, -1));
#else
        const __m128i low  = _mm_unpacklo_epi8(band, _mm_setzero_si128());
        const __m128i high = _mm_unpackhi_epi8(band, _mm_setzero_si128());
        row0 = _mm_unpacklo_epi16(low, low);
        row1 = _mm_unpackhi_epi16(low, low);
        row2 = _mm_unpacklo_epi16(high, high);
        row3 = _mm_unpackhi_epi16(high, high);
#endif
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
    // The weights of BlendWeights from the 4 coverage bytes in the low 32 bits of x: in the 2 lanes of 16 bits of each
    // pixel, 255 - m in the low byte and m in the high byte.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    Weights(__m128i x) {
        const __m128i bytes = _mm_shuffle_epi8(x, _mm_setr_epi8(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3));
        return _mm_xor_si128(bytes, _mm_set1_epi16(0x00ff));
    }

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    WeightsBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
        const __m128i inverse = _mm_set1_epi16(0x00ff);
        row0 = _mm_xor_si128(_mm_shuffle_epi8(band, _mm_setr_epi8( 0,  0,  0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  3)), inverse);
        row1 = _mm_xor_si128(_mm_shuffle_epi8(band, _mm_setr_epi8( 4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7)), inverse);
        row2 = _mm_xor_si128(_mm_shuffle_epi8(band, _mm_setr_epi8( 8,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 11, 11, 11, 11)), inverse);
        row3 = _mm_xor_si128(_mm_shuffle_epi8(band, _mm_setr_epi8(12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15)), inverse);
    }
#endif

    // What the blend of 4 pixels needs from their 4 coverage bytes, in the low 32 bits of x: Weights for BlendWeights,
    // and Spread for BlendCoverage.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m128i
    Prepare(__m128i x) {
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        return UsesMadd<kOpaque>::value ? Weights(x) : Spread(x);
#else
        return Spread(x);
#endif
    }

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE void
    PrepareBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        if (UsesMadd<kOpaque>::value) {
            WeightsBand(band, row0, row1, row2, row3);
            return;
        }
#endif
        SpreadBand(band, row0, row1, row2, row3);
    }

    // 4 pixels blended with the color, with the coverage from Spread. A coverage m draws as the premultiplied white texel
    // (m, m, m, m), so a white color texture draws the same. Under an opaque text, the alpha of the blend is the coverage.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m128i
    BlendCoverage(const Color &color, __m128i spread, __m128i pixels) {
        const __m128i alpha = kOpaque ? spread : Div255(_mm_mullo_epi16(spread, color.alpha));
        const __m128i even  = Blend(_mm_mullo_epi16(color.even, spread), alpha, EvenBytes(pixels));
        const __m128i odd   = Blend(_mm_mullo_epi16(color.odd,  spread), alpha, OddBytes(pixels));
        return JoinBytes(even, odd);
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
    // BlendCoverage for an opaque text, with the weights from Weights. XORed with 0x80, a byte from 0 to 255 is a signed
    // byte from -128 to 127 that is 128 less. So the sum in each lane is (255 - m) * (pixel - 128) + m * (color - 128),
    // which is the sum of the blend minus 128 * 255, and lies between -32640 and 32385: _mm_maddubs_epi16 never
    // saturates it. Flipping its bit 15 adds 32768: those 128 * 255, and the 128 that rounds the division by 255.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    BlendWeights(const Color &color, __m128i weights, __m128i pixels) {
        const __m128i bias = _mm_set1_epi16(int16_t(0x8000));
        const __m128i even = _mm_maddubs_epi16(weights, _mm_xor_si128(EvenBytes(pixels), color.evenMadd));
        const __m128i odd  = _mm_maddubs_epi16(weights, _mm_xor_si128(OddBytes(pixels),  color.oddMadd));
        return JoinBytes(_mm_mulhi_epu16(_mm_xor_si128(even, bias), _mm_set1_epi16(0x0101)),
                         _mm_mulhi_epu16(_mm_xor_si128(odd,  bias), _mm_set1_epi16(0x0101)));
    }
#endif

    // 4 pixels blended with the color, with the coverage from Prepare. An overload picks the blend, not an if: when the
    // blend that is not used stays in the glyph function, even behind a constant condition, MSVC 19.44 keeps the color
    // in memory instead of in registers.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m128i
    BlendPrepared(std::false_type, const Color &color, __m128i spread, __m128i pixels) {
        return BlendCoverage<kOpaque>(color, spread, pixels);
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m128i
    BlendPrepared(std::true_type, const Color &color, __m128i weights, __m128i pixels) {
        return BlendWeights(color, weights, pixels);
    }
#endif

    // The alpha of each of the 4 pixels of odd, which has their green and alpha, over the 2 lanes of its pixel.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    ExpandAlphaOdd(__m128i odd) {
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        return _mm_shuffle_epi8(odd, _mm_setr_epi8(2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15));
#else
        return _mm_shufflehi_epi16(_mm_shufflelo_epi16(odd, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
#endif
    }

    // 4 premultiplied texels times the premultiplied color, blended with 4 pixels. The alpha of the blend is the alpha of
    // the texel under an opaque text, and the alpha of the texel times the color, rounded, under a translucent one.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m128i
    BlendTexels(const Color &color, __m128i texels, __m128i pixels) {
        const __m128i odd       = OddBytes(texels);
        const __m128i sourceOdd = _mm_mullo_epi16(odd, color.odd);
        const __m128i alpha     = ExpandAlphaOdd(kOpaque ? odd : Div255(sourceOdd));
        const __m128i even      = Blend(_mm_mullo_epi16(EvenBytes(texels), color.even), alpha, EvenBytes(pixels));
        return JoinBytes(even, Blend(sourceOdd, alpha, OddBytes(pixels)));
    }

    // The alpha of the 4 texels is 0. _mm_testz_si128 tells whether the AND of its two registers is all 0.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsTransparent(__m128i texels) {
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        return _mm_testz_si128(texels, _mm_set1_epi32(int(0xff000000u))) != 0;
#else
        return (_mm_movemask_epi8(_mm_cmpeq_epi8(texels, _mm_setzero_si128())) & 0x8888) == 0x8888;
#endif
    }

    // The 16 coverage bytes of a band are 0.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsEmptyBand(__m128i band) {
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        return _mm_testz_si128(band, band) != 0;
#else
        return _mm_movemask_epi8(_mm_cmpeq_epi8(band, _mm_setzero_si128())) == 0xffff;
#endif
    }

    // The 16 coverage bytes of a band are 255. _mm_testc_si128(a, b) tells whether b has no bit set that a lacks.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsFullBand(__m128i band) {
#if defined(FONTRENDERER_GLYPHDRAW_X64V2)
        return _mm_testc_si128(band, _mm_set1_epi8(-1)) != 0;
#else
        return _mm_movemask_epi8(_mm_cmpeq_epi8(band, _mm_set1_epi8(-1))) == 0xffff;
#endif
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    // Color for 8 pixels, made only for the glyphs drawn in blocks of 8. Color stays of 128 bits: when the blocks of 4
    // took the low halves of registers of 256 bits, MSVC 19.44 kept the color in memory.
    //---------------------------------
    struct Color8 {
        __m256i     solid;
        __m256i     even;
        __m256i     odd;
        __m256i     alpha;
        __m256i     evenMadd;
        __m256i     oddMadd;
    };

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE Color8
    MakeColor8(uint32_t premultiplied, uint32_t alpha) {
        Color8 color;
        color.solid    = _mm256_set1_epi32(int(premultiplied));
        color.even     = _mm256_and_si256(color.solid, _mm256_set1_epi16(0x00ff));
        color.odd      = _mm256_srli_epi16(color.solid, 8);
        color.alpha    = _mm256_set1_epi16(int16_t(alpha));
        color.evenMadd = _mm256_xor_si256(_mm256_slli_epi16(color.even, 8), _mm256_set1_epi8(-128));
        color.oddMadd  = _mm256_xor_si256(_mm256_slli_epi16(color.odd,  8), _mm256_set1_epi8(-128));
        return color;
    }

    // The 8 bytes at p as a number.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE uint64_t
    Load64(const uint8_t *p) {
        uint64_t value;
        memcpy(&value, p, sizeof(value));
        return value;
    }

    // 8 pixels or texels, as they are in memory.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    LoadOctet(const void *p) {
        return _mm256_loadu_si256(static_cast<const __m256i *>(p));
    }

    // Writes the 8 pixels of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StoreOctet(void *p, __m256i x) {
        _mm256_storeu_si256(static_cast<__m256i *>(p), x);
    }

    // Div255, Blend, EvenBytes, OddBytes and JoinBytes for 8 pixels.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    Div255(__m256i x) {
        return _mm256_mulhi_epu16(_mm256_add_epi16(x, _mm256_set1_epi16(0x0080)), _mm256_set1_epi16(0x0101));
    }

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    Blend(__m256i source, __m256i alpha, __m256i pixels) {
        const __m256i inverse = _mm256_xor_si256(alpha, _mm256_set1_epi16(0x00ff));
        return Div255(_mm256_add_epi16(source, _mm256_mullo_epi16(pixels, inverse)));
    }

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    EvenBytes(__m256i x) {
        return _mm256_and_si256(x, _mm256_set1_epi16(0x00ff));
    }

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    OddBytes(__m256i x) {
        return _mm256_srli_epi16(x, 8);
    }

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    JoinBytes(__m256i even, __m256i odd) {
        return _mm256_or_si256(even, _mm256_slli_epi16(odd, 8));
    }

    // ExpandAlphaOdd for 8 pixels. _mm256_shuffle_epi8 moves bytes within each half, which here is enough.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    ExpandAlphaOdd(__m256i odd) {
        return _mm256_shuffle_epi8(odd, _mm256_setr_epi8(2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15,
                                                         2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15));
    }

    // BlendTexels for 8 pixels.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m256i
    BlendTexels(const Color8 &color, __m256i texels, __m256i pixels) {
        const __m256i odd       = OddBytes(texels);
        const __m256i sourceOdd = _mm256_mullo_epi16(odd, color.odd);
        const __m256i alpha     = ExpandAlphaOdd(kOpaque ? odd : Div255(sourceOdd));
        const __m256i even      = Blend(_mm256_mullo_epi16(EvenBytes(texels), color.even), alpha, EvenBytes(pixels));
        return JoinBytes(even, Blend(sourceOdd, alpha, OddBytes(pixels)));
    }

    // The alpha of the 8 texels is 0.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsTransparent(__m256i texels) {
        return _mm256_testz_si256(texels, _mm256_set1_epi32(int(0xff000000u))) != 0;
    }

    // A mask of _mm256_shuffle_epi8, which moves bytes only within each half, that does in each half what mask does in
    // a register of 128 bits.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    BothHalves(__m128i mask) {
        return _mm256_broadcastsi128_si256(mask);
    }

    // Spread for 8 coverage bytes, copied first to both halves.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    Spread8(uint64_t m) {
        return _mm256_shuffle_epi8(_mm256_set1_epi64x(int64_t(m)),
                                   _mm256_setr_epi8(0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3, -1,
                                                    4, -1, 4, -1, 5, -1, 5, -1, 6, -1, 6, -1, 7, -1, 7, -1));
    }

    // Weights for 8 coverage bytes.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    Weights8(uint64_t m) {
        const __m256i bytes = _mm256_shuffle_epi8(_mm256_set1_epi64x(int64_t(m)),
                                                  _mm256_setr_epi8(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                                                   4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7));
        return _mm256_xor_si256(bytes, _mm256_set1_epi16(0x00ff));
    }

    // Prepare for 8 coverage bytes.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m256i
    Prepare8(uint64_t m) {
        return UsesMadd<kOpaque>::value ? Weights8(m) : Spread8(m);
    }

    // PrepareBand for two bands of 4 columns, one in each half.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE void
    PrepareBand(__m256i band, __m256i &row0, __m256i &row1, __m256i &row2, __m256i &row3) {
        if (UsesMadd<kOpaque>::value) {
            const __m256i inverse = _mm256_set1_epi16(0x00ff);
            row0 = _mm256_xor_si256(_mm256_shuffle_epi8(band, BothHalves(_mm_setr_epi8( 0,  0,  0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  3))), inverse);
            row1 = _mm256_xor_si256(_mm256_shuffle_epi8(band, BothHalves(_mm_setr_epi8( 4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7))), inverse);
            row2 = _mm256_xor_si256(_mm256_shuffle_epi8(band, BothHalves(_mm_setr_epi8( 8,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 11, 11, 11, 11))), inverse);
            row3 = _mm256_xor_si256(_mm256_shuffle_epi8(band, BothHalves(_mm_setr_epi8(12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15))), inverse);
        }
        else {
            row0 = _mm256_shuffle_epi8(band, BothHalves(_mm_setr_epi8( 0, -1,  0, -1,  1, -1,  1, -1,  2, -1,  2, -1,  3, -1,  3, -1)));
            row1 = _mm256_shuffle_epi8(band, BothHalves(_mm_setr_epi8( 4, -1,  4, -1,  5, -1,  5, -1,  6, -1,  6, -1,  7, -1,  7, -1)));
            row2 = _mm256_shuffle_epi8(band, BothHalves(_mm_setr_epi8( 8, -1,  8, -1,  9, -1,  9, -1, 10, -1, 10, -1, 11, -1, 11, -1)));
            row3 = _mm256_shuffle_epi8(band, BothHalves(_mm_setr_epi8(12, -1, 12, -1, 13, -1, 13, -1, 14, -1, 14, -1, 15, -1, 15, -1)));
        }
    }

    // BlendCoverage, BlendWeights and BlendPrepared for 8 pixels.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m256i
    BlendCoverage(const Color8 &color, __m256i spread, __m256i pixels) {
        const __m256i alpha = kOpaque ? spread : Div255(_mm256_mullo_epi16(spread, color.alpha));
        const __m256i even  = Blend(_mm256_mullo_epi16(color.even, spread), alpha, EvenBytes(pixels));
        const __m256i odd   = Blend(_mm256_mullo_epi16(color.odd,  spread), alpha, OddBytes(pixels));
        return JoinBytes(even, odd);
    }

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    BlendWeights(const Color8 &color, __m256i weights, __m256i pixels) {
        const __m256i bias = _mm256_set1_epi16(int16_t(0x8000));
        const __m256i even = _mm256_maddubs_epi16(weights, _mm256_xor_si256(EvenBytes(pixels), color.evenMadd));
        const __m256i odd  = _mm256_maddubs_epi16(weights, _mm256_xor_si256(OddBytes(pixels),  color.oddMadd));
        return JoinBytes(_mm256_mulhi_epu16(_mm256_xor_si256(even, bias), _mm256_set1_epi16(0x0101)),
                         _mm256_mulhi_epu16(_mm256_xor_si256(odd,  bias), _mm256_set1_epi16(0x0101)));
    }

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m256i
    BlendPrepared(std::false_type, const Color8 &color, __m256i spread, __m256i pixels) {
        return BlendCoverage<kOpaque>(color, spread, pixels);
    }

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE __m256i
    BlendPrepared(std::true_type, const Color8 &color, __m256i weights, __m256i pixels) {
        return BlendWeights(color, weights, pixels);
    }

    // IsEmptyBand and IsFullBand for 32 coverage bytes.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsEmptyBand(__m256i band) {
        return _mm256_testz_si256(band, band) != 0;
    }

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsFullBand(__m256i band) {
        return _mm256_testc_si256(band, _mm256_set1_epi8(-1)) != 0;
    }
#endif

    // The coverage of 4 pixels in a row, a byte each. The texels of a rotated glyph are step bytes apart.
    //---------------------------------
    template <bool kRotated>
    FONTRENDERER_ALWAYS_INLINE uint32_t
    LoadCoverage(const uint8_t *mask, size_t step) {
        if (kRotated) {
            return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8) | (uint32_t(mask[2 * step]) << 16) | (uint32_t(mask[3 * step]) << 24);
        }

        return Load32(mask);
    }

    // The same for 2 pixels.
    //---------------------------------
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
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m128i
    LoadBand(const uint8_t *column0, const uint8_t *column1, const uint8_t *column2, const uint8_t *column3) {
        const __m128i first  = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(Load32(column0))), _mm_cvtsi32_si128(int(Load32(column1))));
        const __m128i second = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(Load32(column2))), _mm_cvtsi32_si128(int(Load32(column3))));
        return _mm_unpacklo_epi16(first, second);
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    // The coverage of 4 rows in 8 columns: LoadBand of the first 4 columns in the low half, and of the other 4 in the
    // high half.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE __m256i
    LoadWideBand(const uint8_t *column0, size_t step) {
        const __m128i low  = LoadBand(column0, &column0[step], &column0[2 * step], &column0[3 * step]);
        const __m128i high = LoadBand(&column0[4 * step], &column0[5 * step], &column0[6 * step], &column0[7 * step]);
        return _mm256_inserti128_si256(_mm256_castsi128_si256(low), high, 1);
    }
#endif

    // 4 pixels at p with their coverage, from Prepare. none and full say that the 4 coverages are 0 or 255. With kSkip,
    // such a block is left as it is, or gets the color under an opaque text. With kRead, pixels are the pixels at p, read
    // before the row wrote anything. Without it, they are read here, after the shortcuts.
    //---------------------------------
    template <bool kOpaque, bool kSkip, bool kRead>
    FONTRENDERER_ALWAYS_INLINE void
    DrawCoverage(const Color &color, __m128i prepared, bool none, bool full, uint32_t *p, __m128i pixels) {
        if (kSkip && none) {
            return;
        }
        if (kSkip && kOpaque && full) {
            StoreQuad(p, color.solid);
            return;
        }

        StoreQuad(p, BlendPrepared<kOpaque>(UsesMadd<kOpaque>(), color, prepared, kRead ? pixels : LoadQuad(p)));
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    // DrawCoverage for 8 pixels.
    //---------------------------------
    template <bool kOpaque, bool kSkip, bool kRead>
    FONTRENDERER_ALWAYS_INLINE void
    DrawCoverage(const Color8 &color, __m256i prepared, bool none, bool full, uint32_t *p, __m256i pixels) {
        if (kSkip && none) {
            return;
        }
        if (kSkip && kOpaque && full) {
            StoreOctet(p, color.solid);
            return;
        }

        StoreOctet(p, BlendPrepared<kOpaque>(UsesMadd<kOpaque>(), color, prepared, kRead ? pixels : LoadOctet(p)));
    }
#endif

    // One texel over one pixel. A transparent texel leaves the pixel as it is.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE void
    DrawTexel(const Color &color, const uint8_t *texel, uint32_t *p) {
        const uint32_t t = Load32(texel);
        if ((t >> 24) != 0) {
            StoreOne(p, BlendTexels<kOpaque>(color, _mm_cvtsi32_si128(int(t)), LoadOne(p)));
        }
    }

    // DrawBlocks draws a row of a glyph with the functions of one of these: a row of Alpha8, 4 rows of a rotated glyph
    // of Alpha8, or a row of BGRA32. ReadQuad returns the pixels of a block before the row writes anything.

    //---------------------------------
    template <bool kOpaque, bool kSkip, bool kRotated>
    struct Alpha8Row {
        const Color     &color;
        const uint8_t   *mask;      // The coverage of the first pixel of the row
        size_t          step;       // Between two pixels of the row
        uint32_t        *dst;

        //-----------------------------
        template <bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        Draw(size_t x, __m128i pixels) const {
            const uint32_t m = LoadCoverage<kRotated>(&mask[x * step], step);
            DrawCoverage<kOpaque, kSkip, kRead>(color, Prepare<kOpaque>(_mm_cvtsi32_si128(int(m))), m == 0, m == 0xffffffffu, &dst[x], pixels);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Quad(size_t x) const {
            Draw<false>(x, _mm_setzero_si128());
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        ReadQuad(size_t x) const {
            return LoadQuad(&dst[x]);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        QuadRead(size_t x, __m128i pixels) const {
            Draw<true>(x, pixels);
        }

        // A row of 1 to 3 pixels. With 3, the pixels 0 and 1 and the pixels 1 and 2 are read first and blended as one
        // block of 4.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Narrow(int32_t w) const {
            if (w == 1) {
                StoreOne(dst, BlendPrepared<kOpaque>(UsesMadd<kOpaque>(), color, Prepare<kOpaque>(_mm_cvtsi32_si128(int(mask[0]))), LoadOne(dst)));
            }
            else if (w == 2) {
                StorePair(dst, BlendPrepared<kOpaque>(UsesMadd<kOpaque>(), color, Prepare<kOpaque>(_mm_cvtsi32_si128(int(LoadCoveragePair<kRotated>(mask, step)))), LoadPair(dst)));
            }
            else {
                const uint32_t m      = LoadCoveragePair<kRotated>(mask, step) | (LoadCoveragePair<kRotated>(&mask[step], step) << 16);
                const __m128i  pixels = _mm_unpacklo_epi64(LoadPair(dst), LoadPair(&dst[1]));
                const __m128i  result = BlendPrepared<kOpaque>(UsesMadd<kOpaque>(), color, Prepare<kOpaque>(_mm_cvtsi32_si128(int(m))), pixels);
                StoreHighPair(&dst[1], result);
                StorePair(dst, result);
            }
        }
    };

    // The rows are written out because a loop over them, with a variable index, can make a compiler keep the coverage
    // and the rows in memory.
    //---------------------------------
    template <bool kOpaque, bool kSkip, bool kPartial>
    struct Alpha8Band {
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

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        Draws(int32_t row) const {
            return kPartial == false || row >= firstRow;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE uint32_t *
        At(int32_t row, size_t x) const {
            return &dst[size_t(row) * dstStride + x];
        }

        // With kSkip, the 4 rows skip the blend together or not at all, with one branch instead of one per row. In a
        // partial band, the test includes the rows of the band above, which can only make the shortcut rarer.
        //-----------------------------
        template <bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        Draw(size_t x, const Rows &pixels) const {
            const uint8_t *column = &mask[x * step];
            const __m128i band    = LoadBand(column, &column[step], &column[2 * step], &column[3 * step]);
            if (kSkip) {
                if (IsEmptyBand(band)) {
                    return;
                }
                if (kOpaque && IsFullBand(band)) {
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
            PrepareBand<kOpaque>(band, prepared0, prepared1, prepared2, prepared3);
            if (Draws(0)) {
                DrawCoverage<kOpaque, false, kRead>(color, prepared0, false, false, At(0, x), pixels.row0);
            }
            if (Draws(1)) {
                DrawCoverage<kOpaque, false, kRead>(color, prepared1, false, false, At(1, x), pixels.row1);
            }
            if (Draws(2)) {
                DrawCoverage<kOpaque, false, kRead>(color, prepared2, false, false, At(2, x), pixels.row2);
            }
            DrawCoverage<kOpaque, false, kRead>(color, prepared3, false, false, At(3, x), pixels.row3);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Quad(size_t x) const {
            Draw<false>(x, Rows());
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE Rows
        ReadQuad(size_t x) const {
            Rows pixels;
            pixels.row0 = LoadQuad(At(0, x));
            pixels.row1 = LoadQuad(At(1, x));
            pixels.row2 = LoadQuad(At(2, x));
            pixels.row3 = LoadQuad(At(3, x));
            return pixels;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        QuadRead(size_t x, const Rows &pixels) const {
            Draw<true>(x, pixels);
        }
    };

    //---------------------------------
    template <bool kOpaque, bool kSkip>
    struct BGRARow {
        const Color     &color;
        const uint8_t   *src;       // The texel of the first pixel of the row
        uint32_t        *dst;

        // With kSkip, a block of transparent texels is left as it is. A block of opaque texels under an opaque text is
        // blended like any other: tinting it without the blend did not pay for the branch.
        //-----------------------------
        template <bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        Draw(size_t x, __m128i pixels) const {
            const __m128i texels = LoadQuad(&src[4 * x]);
            if (kSkip && IsTransparent(texels)) {
                return;
            }

            StoreQuad(&dst[x], BlendTexels<kOpaque>(color, texels, kRead ? pixels : LoadQuad(&dst[x])));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Quad(size_t x) const {
            Draw<false>(x, _mm_setzero_si128());
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        ReadQuad(size_t x) const {
            return LoadQuad(&dst[x]);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        QuadRead(size_t x, __m128i pixels) const {
            Draw<true>(x, pixels);
        }

        // A row of 1 to 3 pixels, as Alpha8Row::Narrow draws it.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Narrow(int32_t w) const {
            if (w == 1) {
                StoreOne(dst, BlendTexels<kOpaque>(color, LoadOne(src), LoadOne(dst)));
            }
            else if (w == 2) {
                StorePair(dst, BlendTexels<kOpaque>(color, LoadPair(src), LoadPair(dst)));
            }
            else {
                const __m128i texels = _mm_unpacklo_epi64(LoadPair(src), LoadPair(&src[4]));
                const __m128i pixels = _mm_unpacklo_epi64(LoadPair(dst), LoadPair(&dst[1]));
                const __m128i result = BlendTexels<kOpaque>(color, texels, pixels);
                StoreHighPair(&dst[1], result);
                StorePair(dst, result);
            }
        }
    };

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    // A row of BGRA32 in blocks of 8, as BGRARow draws it in blocks of 4.
    //---------------------------------
    template <bool kOpaque, bool kSkip>
    struct BGRAOctetRow {
        const Color8    &color;
        const uint8_t   *src;       // The texel of the first pixel of the row
        uint32_t        *dst;

        //-----------------------------
        template <bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        Draw(size_t x, __m256i pixels) const {
            const __m256i texels = LoadOctet(&src[4 * x]);
            if (kSkip && IsTransparent(texels)) {
                return;
            }

            StoreOctet(&dst[x], BlendTexels<kOpaque>(color, texels, kRead ? pixels : LoadOctet(&dst[x])));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Octet(size_t x) const {
            Draw<false>(x, _mm256_setzero_si256());
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        ReadOctet(size_t x) const {
            return LoadOctet(&dst[x]);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        OctetRead(size_t x, __m256i pixels) const {
            Draw<true>(x, pixels);
        }
    };

    // A row of Alpha8 that is not rotated, in blocks of 8.
    //---------------------------------
    template <bool kOpaque, bool kSkip>
    struct Alpha8OctetRow {
        const Color8    &color;
        const uint8_t   *mask;      // The coverage of the first pixel of the row
        uint32_t        *dst;

        //-----------------------------
        template <bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        Draw(size_t x, __m256i pixels) const {
            const uint64_t m = Load64(&mask[x]);
            DrawCoverage<kOpaque, kSkip, kRead>(color, Prepare8<kOpaque>(m), m == 0, m == ~uint64_t(0), &dst[x], pixels);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Octet(size_t x) const {
            Draw<false>(x, _mm256_setzero_si256());
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        ReadOctet(size_t x) const {
            return LoadOctet(&dst[x]);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        OctetRead(size_t x, __m256i pixels) const {
            Draw<true>(x, pixels);
        }
    };

    // Alpha8Band in blocks of 8 columns, so each row of the band is one block of 8 pixels.
    //---------------------------------
    template <bool kOpaque, bool kSkip, bool kPartial>
    struct Alpha8WideBand {
        const Color8    &color;
        const uint8_t   *mask;      // The coverage of the first pixel of the band
        size_t          step;       // Between two columns of the glyph
        uint32_t        *dst;
        size_t          dstStride;
        int32_t         firstRow;   // In a partial band, the rows before it belong to the band above

        struct Rows {
            __m256i     row0;
            __m256i     row1;
            __m256i     row2;
            __m256i     row3;
        };

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        Draws(int32_t row) const {
            return kPartial == false || row >= firstRow;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE uint32_t *
        At(int32_t row, size_t x) const {
            return &dst[size_t(row) * dstStride + x];
        }

        //-----------------------------
        template <bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        Draw(size_t x, const Rows &pixels) const {
            const __m256i band = LoadWideBand(&mask[x * step], step);
            if (kSkip) {
                if (IsEmptyBand(band)) {
                    return;
                }
                if (kOpaque && IsFullBand(band)) {
                    if (Draws(0)) {
                        StoreOctet(At(0, x), color.solid);
                    }
                    if (Draws(1)) {
                        StoreOctet(At(1, x), color.solid);
                    }
                    if (Draws(2)) {
                        StoreOctet(At(2, x), color.solid);
                    }
                    StoreOctet(At(3, x), color.solid);
                    return;
                }
            }

            __m256i prepared0, prepared1, prepared2, prepared3;
            PrepareBand<kOpaque>(band, prepared0, prepared1, prepared2, prepared3);
            if (Draws(0)) {
                DrawCoverage<kOpaque, false, kRead>(color, prepared0, false, false, At(0, x), pixels.row0);
            }
            if (Draws(1)) {
                DrawCoverage<kOpaque, false, kRead>(color, prepared1, false, false, At(1, x), pixels.row1);
            }
            if (Draws(2)) {
                DrawCoverage<kOpaque, false, kRead>(color, prepared2, false, false, At(2, x), pixels.row2);
            }
            DrawCoverage<kOpaque, false, kRead>(color, prepared3, false, false, At(3, x), pixels.row3);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Octet(size_t x) const {
            Draw<false>(x, Rows());
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE Rows
        ReadOctet(size_t x) const {
            Rows pixels;
            pixels.row0 = LoadOctet(At(0, x));
            pixels.row1 = LoadOctet(At(1, x));
            pixels.row2 = LoadOctet(At(2, x));
            pixels.row3 = LoadOctet(At(3, x));
            return pixels;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        OctetRead(size_t x, const Rows &pixels) const {
            Draw<true>(x, pixels);
        }
    };
#endif

    // A row of at least 4 pixels, or a band of 4 such rows. The last block is read before the row writes anything, and
    // the blocks before it are drawn as they come, so a row needs no branch on how many pixels are left.
    //---------------------------------
    template <class TLine>
    FONTRENDERER_ALWAYS_INLINE void
    DrawBlocks(const TLine &line, int32_t w) {
        const size_t tail   = size_t(w) - 4;
        const auto   pixels = line.ReadQuad(tail);
        // Built for x86-64-v3, clang 19 unrolls this loop by two, and the rows of small text get slower. It does not
        // unroll it for the other levels.
#if defined(__clang__)
        #pragma clang loop unroll(disable)
#endif
        for (size_t x = 0; x < tail; x += 4) {
            line.Quad(x);
        }
        line.QuadRead(tail, pixels);
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    // DrawBlocks in blocks of 8, for a row of at least 8 pixels or a band of such rows.
    //---------------------------------
    template <class TLine>
    FONTRENDERER_ALWAYS_INLINE void
    DrawOctets(const TLine &line, int32_t w) {
        const size_t tail   = size_t(w) - 8;
        const auto   pixels = line.ReadOctet(tail);
#if defined(__clang__)
        #pragma clang loop unroll(disable)
#endif
        for (size_t x = 0; x < tail; x += 8) {
            line.Octet(x);
        }
        line.OctetRead(tail, pixels);
    }
#endif

    // The clipping of DrawText can leave a width below 1, which draws nothing.
    //---------------------------------
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

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    // The bands of a rotated glyph at least 8 pixels wide and 4 tall, in blocks of 8 columns. Inside DrawAlpha8Rotated,
    // next to the bands of 4 columns, MSVC 19.44 kept both colors in memory and read them for every block.
    //---------------------------------
    template <bool kOpaque, bool kSkip>
    FONTRENDERER_NO_INLINE void
    DrawAlpha8WideBands(const uint8_t *mask, size_t stepX, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride,
                        uint32_t premultiplied, uint32_t alpha) {
        const Color8 color = MakeColor8(premultiplied, alpha);
        int32_t y = 0;
        for (; y + 4 <= height; y += 4) {
            DrawOctets(Alpha8WideBand<kOpaque, kSkip, false> { color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, 0 }, width);
        }
        if (y < height) {
            const int32_t top = height - 4;
            DrawOctets(Alpha8WideBand<kOpaque, kSkip, true> { color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, y - top }, width);
        }
    }
#endif

    // A rotated glyph has a function of its own, one for each kSkip, so that the glyph function does not save the
    // registers it uses for every glyph. Its rows are drawn 4 at a time when the texels of a column are contiguous.
    //---------------------------------
    template <bool kOpaque, bool kSkip>
    FONTRENDERER_NO_INLINE void
    DrawAlpha8Rotated(const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                      uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
        const Color color = MakeColor(premultiplied, alpha);
        if (stepY == 1 && width >= 4 && height >= 4) {
            int32_t y = 0;
            for (; y + 4 <= height; y += 4) {
                DrawBlocks(Alpha8Band<kOpaque, kSkip, false> { color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, 0 }, width);
            }
            if (y < height) {
                const int32_t top = height - 4;
                DrawBlocks(Alpha8Band<kOpaque, kSkip, true> { color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, y - top }, width);
            }
        }
        else {
            for (int32_t y = 0; y < height; ++y) {
                DrawRow(Alpha8Row<kOpaque, kSkip, true> { color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride] }, width);
            }
        }
    }

    //---------------------------------
    template <bool kOpaque, bool kSkip>
    FONTRENDERER_ALWAYS_INLINE void
    DrawAlpha8Rows(const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride) {
        for (int32_t y = 0; y < height; ++y) {
            DrawRow(Alpha8Row<kOpaque, kSkip, false> { color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride] }, width);
        }
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    // The rows of a glyph that is not rotated and is at least 8 pixels wide, in blocks of 8.
    //---------------------------------
    template <bool kOpaque, bool kSkip>
    FONTRENDERER_ALWAYS_INLINE void
    DrawAlpha8OctetRows(const uint8_t *mask, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride,
                        uint32_t premultiplied, uint32_t alpha) {
        const Color8 color = MakeColor8(premultiplied, alpha);
        for (int32_t y = 0; y < height; ++y) {
            DrawOctets(Alpha8OctetRow<kOpaque, kSkip> { color, &mask[size_t(y) * stepY], &dst[size_t(y) * dstStride] }, width);
        }
    }
#endif

    // GlyphDraw.cpp explains why these functions are never inlined.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_NO_INLINE void
    DrawAlpha8(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
               uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
        const uint8_t *mask = &texture[offset];
#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
        if (stepX != 1 && stepY == 1 && width >= 8 && height >= 4) {
            if (UsesShortcuts<kOpaque>(width)) {
                DrawAlpha8WideBands<kOpaque, true>(mask, stepX, width, height, dst, dstStride, premultiplied, alpha);
            }
            else {
                DrawAlpha8WideBands<kOpaque, false>(mask, stepX, width, height, dst, dstStride, premultiplied, alpha);
            }
            return;
        }
#endif
        if (stepX != 1) {
            if (UsesShortcuts<kOpaque>(width)) {
                DrawAlpha8Rotated<kOpaque, true>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
            }
            else {
                DrawAlpha8Rotated<kOpaque, false>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
            }
            return;
        }

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
        if (width >= 8) {
            if (UsesShortcuts<kOpaque>(width)) {
                DrawAlpha8OctetRows<kOpaque, true>(mask, stepY, width, height, dst, dstStride, premultiplied, alpha);
            }
            else {
                DrawAlpha8OctetRows<kOpaque, false>(mask, stepY, width, height, dst, dstStride, premultiplied, alpha);
            }
            return;
        }
#endif

        const Color color = MakeColor(premultiplied, alpha);
        if (UsesShortcuts<kOpaque>(width)) {
            DrawAlpha8Rows<kOpaque, true>(color, mask, stepY, width, height, dst, dstStride);
        }
        else {
            DrawAlpha8Rows<kOpaque, false>(color, mask, stepY, width, height, dst, dstStride);
        }
    }

    // A rotated glyph is drawn one pixel at a time: none of the measured texts with a color texture had one.
    //---------------------------------
    template <bool kOpaque, bool kSkip>
    FONTRENDERER_ALWAYS_INLINE void
    DrawBGRARows(const Color &color, const uint8_t *src, size_t stepX, size_t stepY, int32_t width, int32_t height,
                 uint32_t *dst, uint32_t dstStride) {
        if (stepX != 1) {
            for (int32_t y = 0; y < height; ++y) {
                uint32_t *row = &dst[size_t(y) * dstStride];
                for (int32_t x = 0; x < width; ++x) {
                    DrawTexel<kOpaque>(color, &src[(size_t(y) * stepY + size_t(x) * stepX) * 4], &row[x]);
                }
            }
            return;
        }

        for (int32_t y = 0; y < height; ++y) {
            DrawRow(BGRARow<kOpaque, kSkip> { color, &src[size_t(y) * stepY * 4], &dst[size_t(y) * dstStride] }, width);
        }
    }

#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
    // The rows of a glyph that is not rotated and is at least 8 pixels wide, in blocks of 8.
    //---------------------------------
    template <bool kOpaque, bool kSkip>
    FONTRENDERER_ALWAYS_INLINE void
    DrawBGRAOctetRows(const uint8_t *src, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride,
                      uint32_t premultiplied, uint32_t alpha) {
        const Color8 color = MakeColor8(premultiplied, alpha);
        for (int32_t y = 0; y < height; ++y) {
            DrawOctets(BGRAOctetRow<kOpaque, kSkip> { color, &src[size_t(y) * stepY * 4], &dst[size_t(y) * dstStride] }, width);
        }
    }
#endif

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_NO_INLINE void
    DrawBGRA(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
             uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
        const uint8_t *src = &texture[offset * 4];
#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
        if (stepX == 1 && width >= 8) {
            if (UsesShortcuts<kOpaque>(width)) {
                DrawBGRAOctetRows<kOpaque, true>(src, stepY, width, height, dst, dstStride, premultiplied, alpha);
            }
            else {
                DrawBGRAOctetRows<kOpaque, false>(src, stepY, width, height, dst, dstStride, premultiplied, alpha);
            }
            return;
        }
#endif

        const Color color = MakeColor(premultiplied, alpha);
        if (UsesShortcuts<kOpaque>(width)) {
            DrawBGRARows<kOpaque, true>(color, src, stepX, stepY, width, height, dst, dstStride);
        }
        else {
            DrawBGRARows<kOpaque, false>(color, src, stepX, stepY, width, height, dst, dstStride);
        }
    }

} // end of namespace

//-------------------------------------
GlyphDraw::DrawGlyphFunction
#if defined(FONTRENDERER_GLYPHDRAW_X64V3)
GlyphDraw::GetX64v3DrawGlyphFunction(uint32_t bytesPerTexel, bool opaque) {
#elif defined(FONTRENDERER_GLYPHDRAW_X64V2)
GlyphDraw::GetX64v2DrawGlyphFunction(uint32_t bytesPerTexel, bool opaque) {
#else
GlyphDraw::GetSSE2DrawGlyphFunction(uint32_t bytesPerTexel, bool opaque) {
#endif
    if (bytesPerTexel == 1) {
        return opaque ? DrawAlpha8<true> : DrawAlpha8<false>;
    }

    return opaque ? DrawBGRA<true> : DrawBGRA<false>;
}

#endif
