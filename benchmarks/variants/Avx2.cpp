#include "benchmarkCommon.h"

#include <immintrin.h>
//-------------------------------------
#include <cstring>
#include <type_traits>

// The code of X64v2 (the library code for x86-64-v2), with AVX2 registers of 256 bits, which hold 8 pixels. CMake builds
// only this file for x86-64-v3, the level that adds AVX and AVX2 to x86-64-v2. The same pixels as DrawText.
//
// The first three options of DrawOptions use the 8 pixels in one kind of glyph, and leave the others as X64v2 draws
// them. The last one changes where the color is kept:
//
// - kRows: a glyph that is not rotated, and is 8 pixels wide or more, draws its rows in blocks of 8, with the last
//   block overlapped and read before the row writes anything, as with blocks of 4. Narrower glyphs keep blocks of 4.
// - EBands::Wide: a rotated glyph 8 pixels wide or more draws bands of 4 rows in blocks of 8 columns. 8 reads of 4
//   bytes and two transpositions give the coverage, one block of 4 columns in each half of the register.
// - EBands::Tall: a rotated glyph 8 pixels tall or more draws bands of 8 rows in blocks of 4 columns. The texels down a
//   column are contiguous, so 4 reads of 8 bytes and a transposition give the coverage. A register holds a row of the
//   top half of the band and the row 4 below it, which are at different addresses: each block of 2 rows takes 2 reads
//   and 2 writes of 128 bits.
// - EBands::Hybrid: Wide for a rotated glyph 8 pixels wide or more, and Tall for a narrower one 8 pixels tall or more.
// - kSplit: the blocks of 4 pixels take the color from registers of 128 bits, and the color of 256 bits is only made
//   where a glyph is drawn in blocks of 8. Without it, MSVC keeps the color in memory in the functions that only draw
//   blocks of 4.
//
// The coverage of 8 pixels is copied to both halves of the register first, because _mm256_shuffle_epi8 only moves bytes
// within each half of 128 bits.
//
// The bands do not use the gather reads of AVX2: they are slow on the processors with the microcode for the Downfall
// flaw.

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
namespace Avx2 {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                           uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

        // How a rotated glyph of Alpha8 draws its bands.
        enum class EBands {
            Quad,       // 4 rows by 4 columns, as X64v2
            Wide,       // 4 rows by 8 columns
            Tall,       // 8 rows by 4 columns
            Hybrid,     // Wide when the glyph is 8 pixels wide or more, else Tall
        };

        // The choices of Draw. Avx2Rows, Avx2Wide, Avx2Tall and Avx2Split change one of them. With all of them off, this
        // is the code of X64v2, with the color in registers of 256 bits. With kSplit alone, it is the code of X64v2.
        //-----------------------------
        struct DrawOptions {
            static constexpr bool   kRows  = false;            // Rows of 8 pixels or more, in blocks of 8
            static constexpr EBands kBands = EBands::Quad;
            static constexpr bool   kSplit = false;            // Blocks of 4 with a color of 128 bits, Color4
            static constexpr bool   kNoUnroll = false;         // Keeps clang from unrolling the loop over the blocks of a row
        };

        //-----------------------------
        struct RowsOptions : DrawOptions {
            static constexpr bool   kRows  = true;
        };

        //-----------------------------
        struct WideOptions : DrawOptions {
            static constexpr EBands kBands = EBands::Wide;
        };

        //-----------------------------
        struct TallOptions : DrawOptions {
            static constexpr EBands kBands = EBands::Tall;
        };

        //-----------------------------
        struct SplitOptions : DrawOptions {
            static constexpr bool   kSplit = true;
        };

        // Rows only change the glyphs that are not rotated, and bands only the rotated ones, so the two add up.
        //-----------------------------
        struct SplitRowsWideOptions : SplitOptions {
            static constexpr bool   kRows  = true;
            static constexpr EBands kBands = EBands::Wide;
        };

        //-----------------------------
        struct SplitRowsTallOptions : SplitOptions {
            static constexpr bool   kRows  = true;
            static constexpr EBands kBands = EBands::Tall;
        };

        //-----------------------------
        struct SplitRowsWideNoUnrollOptions : SplitRowsWideOptions {
            static constexpr bool   kNoUnroll = true;
        };

        //-----------------------------
        struct SplitRowsTallNoUnrollOptions : SplitRowsTallOptions {
            static constexpr bool   kNoUnroll = true;
        };

        //-----------------------------
        struct SplitRowsHybridNoUnrollOptions : SplitRowsWideNoUnrollOptions {
            static constexpr EBands kBands = EBands::Hybrid;
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
        template <bool kOpaque>
        using UsesMadd = std::integral_constant<bool, kOpaque>;

        // Without kSplit, the blocks of 4 pixels take the low half of each member. In the functions that only use that
        // half, MSVC 19.44 keeps the members in memory and reads them for every block.
        //-----------------------------
        struct Color {
            __m256i     solid;      // The premultiplied color for 8 pixels
            __m256i     even;       // Its blue and red for 8 pixels, each in a 16-bit lane
            __m256i     odd;        // Its green and alpha for 8 pixels
            __m256i     alpha;      // The alpha of the color in every lane
            __m256i     evenMadd;   // even in the high byte of each lane and 0 in the low byte, both XORed with 0x80
            __m256i     oddMadd;    // The same for odd
        };

        // Color for 4 pixels, as X64v2 has it.
        //-----------------------------
        struct Color4 {
            __m128i     solid;
            __m128i     even;
            __m128i     odd;
            __m128i     alpha;
            __m128i     evenMadd;
            __m128i     oddMadd;
        };

        // The color of the blocks of 4 pixels.
        template <class O>
        using ColorOf4 = typename std::conditional<O::kSplit, Color4, Color>::type;

        //-----------------------------
        template <class TColor>
        TColor MakeColor(uint32_t premultiplied, uint32_t alpha);

        //-----------------------------
        template <>
        FONTRENDERER_ALWAYS_INLINE Color
        MakeColor<Color>(uint32_t premultiplied, uint32_t alpha) {
            Color color;
            color.solid    = _mm256_set1_epi32(int(premultiplied));
            color.even     = _mm256_and_si256(color.solid, _mm256_set1_epi16(0x00ff));
            color.odd      = _mm256_srli_epi16(color.solid, 8);
            color.alpha    = _mm256_set1_epi16(int16_t(alpha));
            color.evenMadd = _mm256_xor_si256(_mm256_slli_epi16(color.even, 8), _mm256_set1_epi8(-128));
            color.oddMadd  = _mm256_xor_si256(_mm256_slli_epi16(color.odd,  8), _mm256_set1_epi8(-128));
            return color;
        }

        //-----------------------------
        template <>
        FONTRENDERER_ALWAYS_INLINE Color4
        MakeColor<Color4>(uint32_t premultiplied, uint32_t alpha) {
            Color4 color;
            color.solid    = _mm_set1_epi32(int(premultiplied));
            color.even     = _mm_and_si128(color.solid, _mm_set1_epi16(0x00ff));
            color.odd      = _mm_srli_epi16(color.solid, 8);
            color.alpha    = _mm_set1_epi16(int16_t(alpha));
            color.evenMadd = _mm_xor_si128(_mm_slli_epi16(color.even, 8), _mm_set1_epi8(-128));
            color.oddMadd  = _mm_xor_si128(_mm_slli_epi16(color.odd,  8), _mm_set1_epi8(-128));
            return color;
        }

        // The low 128 bits of x. It costs no instruction.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Low(__m256i x) {
            return _mm256_castsi256_si128(x);
        }

        // low and high as the two halves of one register.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        Combine(__m128i low, __m128i high) {
            return _mm256_inserti128_si256(_mm256_castsi128_si256(low), high, 1);
        }

        // A member x of a color, for a block of the size of the first argument, which is only there for its type.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Fit(__m128i, __m256i x) {
            return Low(x);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Fit(__m128i, __m128i x) {
            return x;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        Fit(__m256i, __m256i x) {
            return x;
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

        // The same for 8 bytes.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE uint64_t
        Load64(const uint8_t *p) {
            uint64_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        // A block of 4 or 8 pixels or texels, as they are in memory.
        //-----------------------------
        template <class V>
        V LoadBlock(const void *p);

        //-----------------------------
        template <>
        FONTRENDERER_ALWAYS_INLINE __m128i
        LoadBlock<__m128i>(const void *p) {
            return _mm_loadu_si128(static_cast<const __m128i *>(p));
        }

        //-----------------------------
        template <>
        FONTRENDERER_ALWAYS_INLINE __m256i
        LoadBlock<__m256i>(const void *p) {
            return _mm256_loadu_si256(static_cast<const __m256i *>(p));
        }

        // Writes the 4 pixels of x.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StoreBlock(void *p, __m128i x) {
            _mm_storeu_si128(static_cast<__m128i *>(p), x);
        }

        // Writes the 8 pixels of x.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StoreBlock(void *p, __m256i x) {
            _mm256_storeu_si256(static_cast<__m256i *>(p), x);
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

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        Div255(__m256i x) {
            return _mm256_mulhi_epu16(_mm256_add_epi16(x, _mm256_set1_epi16(0x0080)), _mm256_set1_epi16(0x0101));
        }

        // Rounded (source + pixels * (255 - alpha)) / 255 in each 16-bit lane, with the source already multiplied.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Blend(__m128i source, __m128i alpha, __m128i pixels) {
            const __m128i inverse = _mm_xor_si128(alpha, _mm_set1_epi16(0x00ff));
            return Div255(_mm_add_epi16(source, _mm_mullo_epi16(pixels, inverse)));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        Blend(__m256i source, __m256i alpha, __m256i pixels) {
            const __m256i inverse = _mm256_xor_si256(alpha, _mm256_set1_epi16(0x00ff));
            return Div255(_mm256_add_epi16(source, _mm256_mullo_epi16(pixels, inverse)));
        }

        // The bytes 0, 2, 4 and so on of x, each in a 16-bit lane: the blue and red of each pixel.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        EvenBytes(__m128i x) {
            return _mm_and_si128(x, _mm_set1_epi16(0x00ff));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        EvenBytes(__m256i x) {
            return _mm256_and_si256(x, _mm256_set1_epi16(0x00ff));
        }

        // The bytes 1, 3, 5 and so on of x, each in a 16-bit lane: the green and alpha of each pixel.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        OddBytes(__m128i x) {
            return _mm_srli_epi16(x, 8);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        OddBytes(__m256i x) {
            return _mm256_srli_epi16(x, 8);
        }

        // The opposite of EvenBytes and OddBytes: the pixels again.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        JoinBytes(__m128i even, __m128i odd) {
            return _mm_or_si128(even, _mm_slli_epi16(odd, 8));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        JoinBytes(__m256i even, __m256i odd) {
            return _mm256_or_si256(even, _mm256_slli_epi16(odd, 8));
        }

        // A mask of _mm256_shuffle_epi8 that does in each half what mask does in a register of 128 bits.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        BothHalves(__m128i mask) {
            return _mm256_broadcastsi128_si256(mask);
        }

        // The 4 coverage bytes in the low 32 bits of x, each copied to the 2 lanes of 16 bits of its pixel: m0 m0 m1 m1 ...
        // A -1 in the mask of _mm_shuffle_epi8 gives a byte of 0.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Spread(__m128i x) {
            return _mm_shuffle_epi8(x, _mm_setr_epi8(0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3, -1));
        }

        // Spread for 8 coverage bytes, copied first to both halves.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        Spread8(uint64_t m) {
            return _mm256_shuffle_epi8(_mm256_set1_epi64x(int64_t(m)),
                                       _mm256_setr_epi8(0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3, -1,
                                                        4, -1, 4, -1, 5, -1, 5, -1, 6, -1, 6, -1, 7, -1, 7, -1));
        }

        // The masks of _mm_shuffle_epi8 that take row 0, 1, 2 or 3 of a band, which has a row in each 32-bit lane: as
        // Spread does, or as Weights does before its XOR.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        SpreadMask(int row) {
            const char b = char(4 * row);
            return _mm_setr_epi8(b, -1, b, -1, char(b + 1), -1, char(b + 1), -1, char(b + 2), -1, char(b + 2), -1, char(b + 3), -1, char(b + 3), -1);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        WeightsMask(int row) {
            const char b = char(4 * row);
            return _mm_setr_epi8(b, b, b, b, char(b + 1), char(b + 1), char(b + 1), char(b + 1),
                                 char(b + 2), char(b + 2), char(b + 2), char(b + 2), char(b + 3), char(b + 3), char(b + 3), char(b + 3));
        }

        // The weights of BlendWeights from the 4 coverage bytes in the low 32 bits of x: in the 2 lanes of 16 bits of each
        // pixel, 255 - m in the low byte and m in the high byte.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        Weights(__m128i x) {
            return _mm_xor_si128(_mm_shuffle_epi8(x, WeightsMask(0)), _mm_set1_epi16(0x00ff));
        }

        // Weights for 8 coverage bytes.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        Weights8(uint64_t m) {
            const __m256i bytes = _mm256_shuffle_epi8(_mm256_set1_epi64x(int64_t(m)),
                                                      _mm256_setr_epi8(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                                                       4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7));
            return _mm256_xor_si256(bytes, _mm256_set1_epi16(0x00ff));
        }

        // What the blend of 4 pixels needs from their 4 coverage bytes, in the low 32 bits of x: Weights for BlendWeights,
        // and Spread for BlendCoverage.
        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m128i
        Prepare(__m128i x) {
            return UsesMadd<kOpaque>::value ? Weights(x) : Spread(x);
        }

        // Prepare for 8 coverage bytes.
        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m256i
        Prepare8(uint64_t m) {
            return UsesMadd<kOpaque>::value ? Weights8(m) : Spread8(m);
        }

        // Prepare for each row of a band of 4 rows, whose coverage has a row in each 32-bit lane.
        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE void
        PrepareBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
            if (UsesMadd<kOpaque>::value) {
                const __m128i inverse = _mm_set1_epi16(0x00ff);
                row0 = _mm_xor_si128(_mm_shuffle_epi8(band, WeightsMask(0)), inverse);
                row1 = _mm_xor_si128(_mm_shuffle_epi8(band, WeightsMask(1)), inverse);
                row2 = _mm_xor_si128(_mm_shuffle_epi8(band, WeightsMask(2)), inverse);
                row3 = _mm_xor_si128(_mm_shuffle_epi8(band, WeightsMask(3)), inverse);
            }
            else {
                row0 = _mm_shuffle_epi8(band, SpreadMask(0));
                row1 = _mm_shuffle_epi8(band, SpreadMask(1));
                row2 = _mm_shuffle_epi8(band, SpreadMask(2));
                row3 = _mm_shuffle_epi8(band, SpreadMask(3));
            }
        }

        // PrepareBand for two bands, one in each half.
        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE void
        PrepareBand(__m256i band, __m256i &row0, __m256i &row1, __m256i &row2, __m256i &row3) {
            if (UsesMadd<kOpaque>::value) {
                const __m256i inverse = _mm256_set1_epi16(0x00ff);
                row0 = _mm256_xor_si256(_mm256_shuffle_epi8(band, BothHalves(WeightsMask(0))), inverse);
                row1 = _mm256_xor_si256(_mm256_shuffle_epi8(band, BothHalves(WeightsMask(1))), inverse);
                row2 = _mm256_xor_si256(_mm256_shuffle_epi8(band, BothHalves(WeightsMask(2))), inverse);
                row3 = _mm256_xor_si256(_mm256_shuffle_epi8(band, BothHalves(WeightsMask(3))), inverse);
            }
            else {
                row0 = _mm256_shuffle_epi8(band, BothHalves(SpreadMask(0)));
                row1 = _mm256_shuffle_epi8(band, BothHalves(SpreadMask(1)));
                row2 = _mm256_shuffle_epi8(band, BothHalves(SpreadMask(2)));
                row3 = _mm256_shuffle_epi8(band, BothHalves(SpreadMask(3)));
            }
        }

        // The pixels blended with the color, with the coverage from Spread. A coverage m draws as the premultiplied white
        // texel (m, m, m, m), so a white color texture draws the same. Under an opaque text, the alpha of the blend is the
        // coverage.
        //-----------------------------
        template <bool kOpaque, class TColor>
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendCoverage(const TColor &color, __m128i spread, __m128i pixels) {
            const __m128i alpha = kOpaque ? spread : Div255(_mm_mullo_epi16(spread, Fit(spread, color.alpha)));
            const __m128i even  = Blend(_mm_mullo_epi16(Fit(spread, color.even), spread), alpha, EvenBytes(pixels));
            const __m128i odd   = Blend(_mm_mullo_epi16(Fit(spread, color.odd),  spread), alpha, OddBytes(pixels));
            return JoinBytes(even, odd);
        }

        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m256i
        BlendCoverage(const Color &color, __m256i spread, __m256i pixels) {
            const __m256i alpha = kOpaque ? spread : Div255(_mm256_mullo_epi16(spread, color.alpha));
            const __m256i even  = Blend(_mm256_mullo_epi16(color.even, spread), alpha, EvenBytes(pixels));
            const __m256i odd   = Blend(_mm256_mullo_epi16(color.odd,  spread), alpha, OddBytes(pixels));
            return JoinBytes(even, odd);
        }

        // BlendCoverage for an opaque text, with the weights from Weights. XORed with 0x80, a byte from 0 to 255 is a signed
        // byte from -128 to 127 that is 128 less. So the sum in each lane is (255 - m) * (pixel - 128) + m * (color - 128),
        // which is the sum of the blend minus 128 * 255, and lies between -32640 and 32385: _mm_maddubs_epi16 never
        // saturates it. Flipping its bit 15 adds 32768: those 128 * 255, and the 128 that rounds the division by 255.
        //-----------------------------
        template <class TColor>
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendWeights(const TColor &color, __m128i weights, __m128i pixels) {
            const __m128i bias = _mm_set1_epi16(int16_t(0x8000));
            const __m128i even = _mm_maddubs_epi16(weights, _mm_xor_si128(EvenBytes(pixels), Fit(weights, color.evenMadd)));
            const __m128i odd  = _mm_maddubs_epi16(weights, _mm_xor_si128(OddBytes(pixels),  Fit(weights, color.oddMadd)));
            return JoinBytes(_mm_mulhi_epu16(_mm_xor_si128(even, bias), _mm_set1_epi16(0x0101)),
                             _mm_mulhi_epu16(_mm_xor_si128(odd,  bias), _mm_set1_epi16(0x0101)));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        BlendWeights(const Color &color, __m256i weights, __m256i pixels) {
            const __m256i bias = _mm256_set1_epi16(int16_t(0x8000));
            const __m256i even = _mm256_maddubs_epi16(weights, _mm256_xor_si256(EvenBytes(pixels), color.evenMadd));
            const __m256i odd  = _mm256_maddubs_epi16(weights, _mm256_xor_si256(OddBytes(pixels),  color.oddMadd));
            return JoinBytes(_mm256_mulhi_epu16(_mm256_xor_si256(even, bias), _mm256_set1_epi16(0x0101)),
                             _mm256_mulhi_epu16(_mm256_xor_si256(odd,  bias), _mm256_set1_epi16(0x0101)));
        }

        // The pixels blended with the color, with the coverage from Prepare. An overload picks the blend, not an if: when
        // the blend that is not used stays in the glyph function, even behind a constant condition, MSVC 19.44 keeps the
        // color in memory instead of in registers.
        //-----------------------------
        template <bool kOpaque, class TColor, class V>
        FONTRENDERER_ALWAYS_INLINE V
        BlendPrepared(std::false_type, const TColor &color, V spread, V pixels) {
            return BlendCoverage<kOpaque>(color, spread, pixels);
        }

        //-----------------------------
        template <bool kOpaque, class TColor, class V>
        FONTRENDERER_ALWAYS_INLINE V
        BlendPrepared(std::true_type, const TColor &color, V weights, V pixels) {
            return BlendWeights(color, weights, pixels);
        }

        // The alpha of each pixel of odd, which has their green and alpha, over the 2 lanes of its pixel.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        ExpandAlphaOdd(__m128i odd) {
            return _mm_shuffle_epi8(odd, _mm_setr_epi8(2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        ExpandAlphaOdd(__m256i odd) {
            return _mm256_shuffle_epi8(odd, BothHalves(_mm_setr_epi8(2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15)));
        }

        // Premultiplied texels times the premultiplied color, blended with the pixels. The alpha of the blend is the alpha
        // of the texel under an opaque text, and the alpha of the texel times the color, rounded, under a translucent one.
        //-----------------------------
        template <bool kOpaque, class TColor>
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendTexels(const TColor &color, __m128i texels, __m128i pixels) {
            const __m128i odd       = OddBytes(texels);
            const __m128i sourceOdd = _mm_mullo_epi16(odd, Fit(odd, color.odd));
            const __m128i alpha     = ExpandAlphaOdd(kOpaque ? odd : Div255(sourceOdd));
            const __m128i even      = Blend(_mm_mullo_epi16(EvenBytes(texels), Fit(odd, color.even)), alpha, EvenBytes(pixels));
            return JoinBytes(even, Blend(sourceOdd, alpha, OddBytes(pixels)));
        }

        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m256i
        BlendTexels(const Color &color, __m256i texels, __m256i pixels) {
            const __m256i odd       = OddBytes(texels);
            const __m256i sourceOdd = _mm256_mullo_epi16(odd, color.odd);
            const __m256i alpha     = ExpandAlphaOdd(kOpaque ? odd : Div255(sourceOdd));
            const __m256i even      = Blend(_mm256_mullo_epi16(EvenBytes(texels), color.even), alpha, EvenBytes(pixels));
            return JoinBytes(even, Blend(sourceOdd, alpha, OddBytes(pixels)));
        }

        // The alpha of the texels is 0. _mm_testz_si128 tells whether the AND of its two registers is all 0.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        IsTransparent(__m128i texels) {
            return _mm_testz_si128(texels, _mm_set1_epi32(int(0xff000000u))) != 0;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        IsTransparent(__m256i texels) {
            return _mm256_testz_si256(texels, _mm256_set1_epi32(int(0xff000000u))) != 0;
        }

        // The coverage bytes of a band are 0.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        IsEmptyBand(__m128i band) {
            return _mm_testz_si128(band, band) != 0;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        IsEmptyBand(__m256i band) {
            return _mm256_testz_si256(band, band) != 0;
        }

        // The coverage bytes of a band are 255. _mm_testc_si128(a, b) tells whether b has no bit set that a lacks.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        IsFullBand(__m128i band) {
            return _mm_testc_si128(band, _mm_set1_epi8(-1)) != 0;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        IsFullBand(__m256i band) {
            return _mm256_testc_si256(band, _mm256_set1_epi8(-1)) != 0;
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

        // The coverage of 8 rows in 4 columns: one read of 8 bytes per column, and a transposition. The low half has rows
        // 0 to 3 as LoadBand has them, and the high half rows 4 to 7.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m256i
        LoadTallBand(const uint8_t *column0, const uint8_t *column1, const uint8_t *column2, const uint8_t *column3) {
            const __m128i first  = _mm_unpacklo_epi8(LoadPair(column0), LoadPair(column1));
            const __m128i second = _mm_unpacklo_epi8(LoadPair(column2), LoadPair(column3));
            return Combine(_mm_unpacklo_epi16(first, second), _mm_unpackhi_epi16(first, second));
        }

        // A block at p with its coverage, from Prepare. none and full say that every coverage is 0 or 255. With kSkip, such
        // a block is left as it is, or gets the color under an opaque text. With kRead, pixels are the pixels at p, read
        // before the row wrote anything. Without it, they are read here, after the shortcuts.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kRead, class TColor, class V>
        FONTRENDERER_ALWAYS_INLINE void
        DrawCoverage(const TColor &color, V prepared, bool none, bool full, uint32_t *p, V pixels) {
            if (kSkip && none) {
                return;
            }
            if (kSkip && kOpaque && full) {
                StoreBlock(p, Fit(prepared, color.solid));
                return;
            }

            StoreBlock(p, BlendPrepared<kOpaque>(UsesMadd<kOpaque>(), color, prepared, kRead ? pixels : LoadBlock<V>(p)));
        }

        // One texel over one pixel. A transparent texel leaves the pixel as it is.
        //-----------------------------
        template <bool kOpaque, class TColor>
        FONTRENDERER_ALWAYS_INLINE void
        DrawTexel(const TColor &color, const uint8_t *texel, uint32_t *p) {
            const uint32_t t = Load32(texel);
            if ((t >> 24) != 0) {
                StoreOne(p, BlendTexels<kOpaque>(color, _mm_cvtsi32_si128(int(t)), LoadOne(p)));
            }
        }

        // DrawBlocks draws a row of a glyph, or a band of rows, in blocks of kPixels pixels, with the functions of one of
        // the structs below. ReadBlock returns the pixels of a block before the row writes anything.

        //-----------------------------
        template <class TColor, bool kOpaque, bool kSkip, bool kRotated>
        struct Alpha8Row {
            static constexpr size_t kPixels = 4;

            const TColor    &color;
            const uint8_t   *mask;      // The coverage of the first pixel of the row
            size_t          step;       // Between two pixels of the row
            uint32_t        *dst;

            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, __m128i pixels) const {
                const uint32_t m = LoadCoverage<kRotated>(&mask[x * step], step);
                DrawCoverage<kOpaque, kSkip, kRead>(color, Prepare<kOpaque>(_mm_cvtsi32_si128(int(m))), m == 0, m == 0xffffffffu, &dst[x], pixels);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Block(size_t x) const {
                Draw<false>(x, _mm_setzero_si128());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE __m128i
            ReadBlock(size_t x) const {
                return LoadBlock<__m128i>(&dst[x]);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            BlockRead(size_t x, __m128i pixels) const {
                Draw<true>(x, pixels);
            }

            // A row of 1 to 3 pixels. With 3, the pixels 0 and 1 and the pixels 1 and 2 are read first and blended as one
            // block of 4.
            //-------------------------
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

        // A row of a glyph that is not rotated, in blocks of 8.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        struct Alpha8Row8 {
            static constexpr size_t kPixels = 8;

            const Color     &color;
            const uint8_t   *mask;      // The coverage of the first pixel of the row
            uint32_t        *dst;

            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, __m256i pixels) const {
                const uint64_t m = Load64(&mask[x]);
                DrawCoverage<kOpaque, kSkip, kRead>(color, Prepare8<kOpaque>(m), m == 0, m == ~uint64_t(0), &dst[x], pixels);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Block(size_t x) const {
                Draw<false>(x, _mm256_setzero_si256());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE __m256i
            ReadBlock(size_t x) const {
                return LoadBlock<__m256i>(&dst[x]);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            BlockRead(size_t x, __m256i pixels) const {
                Draw<true>(x, pixels);
            }
        };

        // The rows are written out because a loop over them, with a variable index, can make a compiler keep the coverage
        // and the rows in memory.
        //-----------------------------
        template <class TColor, bool kOpaque, bool kSkip, bool kPartial>
        struct Alpha8Band {
            static constexpr size_t kPixels = 4;

            const TColor    &color;
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
                    if (IsEmptyBand(band)) {
                        return;
                    }
                    if (kOpaque && IsFullBand(band)) {
                        if (Draws(0)) {
                            StoreBlock(At(0, x), Fit(band, color.solid));
                        }
                        if (Draws(1)) {
                            StoreBlock(At(1, x), Fit(band, color.solid));
                        }
                        if (Draws(2)) {
                            StoreBlock(At(2, x), Fit(band, color.solid));
                        }
                        StoreBlock(At(3, x), Fit(band, color.solid));
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

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Block(size_t x) const {
                Draw<false>(x, Rows());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE Rows
            ReadBlock(size_t x) const {
                Rows pixels;
                pixels.row0 = LoadBlock<__m128i>(At(0, x));
                pixels.row1 = LoadBlock<__m128i>(At(1, x));
                pixels.row2 = LoadBlock<__m128i>(At(2, x));
                pixels.row3 = LoadBlock<__m128i>(At(3, x));
                return pixels;
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            BlockRead(size_t x, const Rows &pixels) const {
                Draw<true>(x, pixels);
            }
        };

        // A band of 4 rows in blocks of 8 columns. The low half of the coverage has the first 4 columns, as LoadBand gives
        // them, and the high half the other 4, so each row is one block of 8 pixels.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPartial>
        struct Alpha8WideBand {
            static constexpr size_t kPixels = 8;

            const Color     &color;
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

            // The shortcuts as in Alpha8Band.
            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, const Rows &pixels) const {
                const uint8_t *column = &mask[x * step];
                const __m256i band    = Combine(LoadBand(column, &column[step], &column[2 * step], &column[3 * step]),
                                                LoadBand(&column[4 * step], &column[5 * step], &column[6 * step], &column[7 * step]));
                if (kSkip) {
                    if (IsEmptyBand(band)) {
                        return;
                    }
                    if (kOpaque && IsFullBand(band)) {
                        if (Draws(0)) {
                            StoreBlock(At(0, x), color.solid);
                        }
                        if (Draws(1)) {
                            StoreBlock(At(1, x), color.solid);
                        }
                        if (Draws(2)) {
                            StoreBlock(At(2, x), color.solid);
                        }
                        StoreBlock(At(3, x), color.solid);
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

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Block(size_t x) const {
                Draw<false>(x, Rows());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE Rows
            ReadBlock(size_t x) const {
                Rows pixels;
                pixels.row0 = LoadBlock<__m256i>(At(0, x));
                pixels.row1 = LoadBlock<__m256i>(At(1, x));
                pixels.row2 = LoadBlock<__m256i>(At(2, x));
                pixels.row3 = LoadBlock<__m256i>(At(3, x));
                return pixels;
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            BlockRead(size_t x, const Rows &pixels) const {
                Draw<true>(x, pixels);
            }
        };

        // A band of 8 rows in blocks of 4 columns. Each register holds row r in its low half and row r + 4 in its high
        // half, as the coverage from LoadTallBand does.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPartial>
        struct Alpha8TallBand {
            static constexpr size_t kPixels = 4;

            const Color     &color;
            const uint8_t   *mask;      // The coverage of the first pixel of the band
            size_t          step;       // Between two columns of the glyph
            uint32_t        *dst;
            size_t          dstStride;
            int32_t         firstRow;   // In a partial band, the rows before it belong to the band above

            struct Rows {
                __m256i     rows04;
                __m256i     rows15;
                __m256i     rows26;
                __m256i     rows37;
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

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE __m256i
            ReadRows(int32_t row, size_t x) const {
                return Combine(LoadBlock<__m128i>(At(row, x)), LoadBlock<__m128i>(At(row + 4, x)));
            }

            // Row 7 is never in the band above.
            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            DrawRows(int32_t row, size_t x, __m256i prepared, __m256i pixels) const {
                const __m256i result = BlendPrepared<kOpaque>(UsesMadd<kOpaque>(), color, prepared, kRead ? pixels : ReadRows(row, x));
                if (Draws(row)) {
                    StoreBlock(At(row, x), Low(result));
                }
                if (row + 4 == 7 || Draws(row + 4)) {
                    StoreBlock(At(row + 4, x), _mm256_extracti128_si256(result, 1));
                }
            }

            // The shortcuts as in Alpha8Band, with one branch for the 8 rows.
            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, const Rows &pixels) const {
                const uint8_t *column = &mask[x * step];
                const __m256i band    = LoadTallBand(column, &column[step], &column[2 * step], &column[3 * step]);
                if (kSkip) {
                    if (IsEmptyBand(band)) {
                        return;
                    }
                    if (kOpaque && IsFullBand(band)) {
                        for (int32_t row = 0; row < 7; ++row) {
                            if (Draws(row)) {
                                StoreBlock(At(row, x), Low(color.solid));
                            }
                        }
                        StoreBlock(At(7, x), Low(color.solid));
                        return;
                    }
                }

                __m256i prepared04, prepared15, prepared26, prepared37;
                PrepareBand<kOpaque>(band, prepared04, prepared15, prepared26, prepared37);
                DrawRows<kRead>(0, x, prepared04, pixels.rows04);
                DrawRows<kRead>(1, x, prepared15, pixels.rows15);
                DrawRows<kRead>(2, x, prepared26, pixels.rows26);
                DrawRows<kRead>(3, x, prepared37, pixels.rows37);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Block(size_t x) const {
                Draw<false>(x, Rows());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE Rows
            ReadBlock(size_t x) const {
                Rows pixels;
                pixels.rows04 = ReadRows(0, x);
                pixels.rows15 = ReadRows(1, x);
                pixels.rows26 = ReadRows(2, x);
                pixels.rows37 = ReadRows(3, x);
                return pixels;
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            BlockRead(size_t x, const Rows &pixels) const {
                Draw<true>(x, pixels);
            }
        };

        //-----------------------------
        template <class TColor, bool kOpaque, bool kSkip>
        struct BGRARow {
            static constexpr size_t kPixels = 4;

            const TColor    &color;
            const uint8_t   *src;       // The texel of the first pixel of the row
            uint32_t        *dst;

            // With kSkip, a block of transparent texels is left as it is. A block of opaque texels under an opaque text is
            // blended like any other: tinting it without the blend did not pay for the branch.
            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, __m128i pixels) const {
                const __m128i texels = LoadBlock<__m128i>(&src[4 * x]);
                if (kSkip && IsTransparent(texels)) {
                    return;
                }

                StoreBlock(&dst[x], BlendTexels<kOpaque>(color, texels, kRead ? pixels : LoadBlock<__m128i>(&dst[x])));
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Block(size_t x) const {
                Draw<false>(x, _mm_setzero_si128());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE __m128i
            ReadBlock(size_t x) const {
                return LoadBlock<__m128i>(&dst[x]);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            BlockRead(size_t x, __m128i pixels) const {
                Draw<true>(x, pixels);
            }

            // A row of 1 to 3 pixels, as Alpha8Row::Narrow draws it.
            //-------------------------
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

        // A row of BGRA32 in blocks of 8.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        struct BGRARow8 {
            static constexpr size_t kPixels = 8;

            const Color     &color;
            const uint8_t   *src;       // The texel of the first pixel of the row
            uint32_t        *dst;

            // The shortcut as in BGRARow.
            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, __m256i pixels) const {
                const __m256i texels = LoadBlock<__m256i>(&src[4 * x]);
                if (kSkip && IsTransparent(texels)) {
                    return;
                }

                StoreBlock(&dst[x], BlendTexels<kOpaque>(color, texels, kRead ? pixels : LoadBlock<__m256i>(&dst[x])));
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Block(size_t x) const {
                Draw<false>(x, _mm256_setzero_si256());
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE __m256i
            ReadBlock(size_t x) const {
                return LoadBlock<__m256i>(&dst[x]);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            BlockRead(size_t x, __m256i pixels) const {
                Draw<true>(x, pixels);
            }
        };

        // The blocks of a row before its last one.
        //-----------------------------
        template <class TLine>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBlocksBefore(std::false_type, const TLine &line, size_t tail) {
            for (size_t x = 0; x < tail; x += TLine::kPixels) {
                line.Block(x);
            }
        }

        // Built for x86-64-v3, clang 19 unrolls the loop above by two, and a row then needs more instructions and
        // branches before its first block. It does not unroll it for x86-64-v2.
        //-----------------------------
        template <class TLine>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBlocksBefore(std::true_type, const TLine &line, size_t tail) {
#if defined(__clang__)
            #pragma clang loop unroll(disable)
#endif
            for (size_t x = 0; x < tail; x += TLine::kPixels) {
                line.Block(x);
            }
        }

        // A row of at least kPixels pixels, or a band of such rows. The last block is read before the row writes anything,
        // and the blocks before it are drawn as they come, so a row needs no branch on how many pixels are left.
        //-----------------------------
        template <class O, class TLine>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBlocks(const TLine &line, int32_t w) {
            const size_t tail   = size_t(w) - TLine::kPixels;
            const auto   pixels = line.ReadBlock(tail);
            DrawBlocksBefore(std::integral_constant<bool, O::kNoUnroll>(), line, tail);
            line.BlockRead(tail, pixels);
        }

        // The clipping of DrawText can leave a width below 1, which draws nothing.
        //-----------------------------
        template <class O, class TRow>
        FONTRENDERER_ALWAYS_INLINE void
        DrawRow(const TRow &row, int32_t w) {
            if (w >= 4) {
                DrawBlocks<O>(row, w);
            }
            else if (w > 0) {
                row.Narrow(w);
            }
        }

        template <EBands kBands>
        using BandsTag = std::integral_constant<EBands, kBands>;

        // The bands of a rotated glyph at least 4 pixels wide and tall, 4 rows by 4 columns. Each way of drawing makes
        // the color it needs, so that a glyph drawn in blocks of 4 has no color of 256 bits with kSplit.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBands(BandsTag<EBands::Quad>, uint32_t premultiplied, uint32_t alpha, const uint8_t *mask, size_t stepX, int32_t width, int32_t height,
                  uint32_t *dst, uint32_t dstStride) {
            using TColor = ColorOf4<O>;
            const TColor color = MakeColor<TColor>(premultiplied, alpha);
            int32_t y = 0;
            for (; y + 4 <= height; y += 4) {
                DrawBlocks<O>(Alpha8Band<TColor, kOpaque, kSkip, false> { color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, 0 }, width);
            }
            if (y < height) {
                const int32_t top = height - 4;
                DrawBlocks<O>(Alpha8Band<TColor, kOpaque, kSkip, true> { color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, y - top }, width);
            }
        }

        // 4 rows by 8 columns when the glyph is 8 pixels wide or more.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBands(BandsTag<EBands::Wide>, uint32_t premultiplied, uint32_t alpha, const uint8_t *mask, size_t stepX, int32_t width, int32_t height,
                  uint32_t *dst, uint32_t dstStride) {
            if (width < 8) {
                DrawBands<O, kOpaque, kSkip>(BandsTag<EBands::Quad>(), premultiplied, alpha, mask, stepX, width, height, dst, dstStride);
                return;
            }

            const Color color = MakeColor<Color>(premultiplied, alpha);
            int32_t y = 0;
            for (; y + 4 <= height; y += 4) {
                DrawBlocks<O>(Alpha8WideBand<kOpaque, kSkip, false> { color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, 0 }, width);
            }
            if (y < height) {
                const int32_t top = height - 4;
                DrawBlocks<O>(Alpha8WideBand<kOpaque, kSkip, true> { color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, y - top }, width);
            }
        }

        // 8 rows by 4 columns when the glyph is 8 pixels tall or more.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBands(BandsTag<EBands::Tall>, uint32_t premultiplied, uint32_t alpha, const uint8_t *mask, size_t stepX, int32_t width, int32_t height,
                  uint32_t *dst, uint32_t dstStride) {
            if (height < 8) {
                DrawBands<O, kOpaque, kSkip>(BandsTag<EBands::Quad>(), premultiplied, alpha, mask, stepX, width, height, dst, dstStride);
                return;
            }

            const Color color = MakeColor<Color>(premultiplied, alpha);
            int32_t y = 0;
            for (; y + 8 <= height; y += 8) {
                DrawBlocks<O>(Alpha8TallBand<kOpaque, kSkip, false> { color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, 0 }, width);
            }
            if (y < height) {
                const int32_t top = height - 8;
                DrawBlocks<O>(Alpha8TallBand<kOpaque, kSkip, true> { color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, y - top }, width);
            }
        }

        // Wide gained most with wide glyphs, and Tall with narrow and tall ones.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBands(BandsTag<EBands::Hybrid>, uint32_t premultiplied, uint32_t alpha, const uint8_t *mask, size_t stepX, int32_t width, int32_t height,
                  uint32_t *dst, uint32_t dstStride) {
            if (width >= 8) {
                DrawBands<O, kOpaque, kSkip>(BandsTag<EBands::Wide>(), premultiplied, alpha, mask, stepX, width, height, dst, dstStride);
            }
            else {
                DrawBands<O, kOpaque, kSkip>(BandsTag<EBands::Tall>(), premultiplied, alpha, mask, stepX, width, height, dst, dstStride);
            }
        }

        // A rotated glyph has a function of its own, one for each kSkip, so that the glyph function does not save the
        // registers it uses for every glyph. Its rows are drawn in bands when the texels of a column are contiguous.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_NO_INLINE void
        DrawAlpha8Rotated(const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                          uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            if (stepY == 1 && width >= 4 && height >= 4) {
                DrawBands<O, kOpaque, kSkip>(BandsTag<O::kBands>(), premultiplied, alpha, mask, stepX, width, height, dst, dstStride);
                return;
            }

            using TColor = ColorOf4<O>;
            const TColor color = MakeColor<TColor>(premultiplied, alpha);
            for (int32_t y = 0; y < height; ++y) {
                DrawRow<O>(Alpha8Row<TColor, kOpaque, kSkip, true> { color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride] }, width);
            }
        }

        // The rows of a glyph that is not rotated, in blocks of 4 or, with the first tag and 8 pixels or more, of 8.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawAlpha8Rows(std::false_type, uint32_t premultiplied, uint32_t alpha, const uint8_t *mask, size_t stepY, int32_t width, int32_t height,
                       uint32_t *dst, uint32_t dstStride) {
            using TColor = ColorOf4<O>;
            const TColor color = MakeColor<TColor>(premultiplied, alpha);
            for (int32_t y = 0; y < height; ++y) {
                DrawRow<O>(Alpha8Row<TColor, kOpaque, kSkip, false> { color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride] }, width);
            }
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawAlpha8Rows(std::true_type, uint32_t premultiplied, uint32_t alpha, const uint8_t *mask, size_t stepY, int32_t width, int32_t height,
                       uint32_t *dst, uint32_t dstStride) {
            if (width < 8) {
                DrawAlpha8Rows<O, kOpaque, kSkip>(std::false_type(), premultiplied, alpha, mask, stepY, width, height, dst, dstStride);
                return;
            }

            const Color color = MakeColor<Color>(premultiplied, alpha);
            for (int32_t y = 0; y < height; ++y) {
                DrawBlocks<O>(Alpha8Row8<kOpaque, kSkip> { color, &mask[size_t(y) * stepY], &dst[size_t(y) * dstStride] }, width);
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

            using Rows = std::integral_constant<bool, O::kRows>;
            if (UsesShortcuts<kOpaque>(width)) {
                DrawAlpha8Rows<O, kOpaque, true>(Rows(), premultiplied, alpha, mask, stepY, width, height, dst, dstStride);
            }
            else {
                DrawAlpha8Rows<O, kOpaque, false>(Rows(), premultiplied, alpha, mask, stepY, width, height, dst, dstStride);
            }
        }

        // The rows of BGRA32, in blocks of 4 or, with the first tag and 8 pixels or more, of 8.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBGRARows(std::false_type, uint32_t premultiplied, uint32_t alpha, const uint8_t *src, size_t stepY, int32_t width, int32_t height,
                     uint32_t *dst, uint32_t dstStride) {
            using TColor = ColorOf4<O>;
            const TColor color = MakeColor<TColor>(premultiplied, alpha);
            for (int32_t y = 0; y < height; ++y) {
                DrawRow<O>(BGRARow<TColor, kOpaque, kSkip> { color, &src[size_t(y) * stepY * 4], &dst[size_t(y) * dstStride] }, width);
            }
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBGRARows(std::true_type, uint32_t premultiplied, uint32_t alpha, const uint8_t *src, size_t stepY, int32_t width, int32_t height,
                     uint32_t *dst, uint32_t dstStride) {
            if (width < 8) {
                DrawBGRARows<O, kOpaque, kSkip>(std::false_type(), premultiplied, alpha, src, stepY, width, height, dst, dstStride);
                return;
            }

            const Color color = MakeColor<Color>(premultiplied, alpha);
            for (int32_t y = 0; y < height; ++y) {
                DrawBlocks<O>(BGRARow8<kOpaque, kSkip> { color, &src[size_t(y) * stepY * 4], &dst[size_t(y) * dstStride] }, width);
            }
        }

        // A rotated glyph is drawn one pixel at a time: none of the measured texts with a color texture had one.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBGRAGlyph(uint32_t premultiplied, uint32_t alpha, const uint8_t *src, size_t stepX, size_t stepY, int32_t width, int32_t height,
                      uint32_t *dst, uint32_t dstStride) {
            if (stepX != 1) {
                using TColor = ColorOf4<O>;
                const TColor color = MakeColor<TColor>(premultiplied, alpha);
                for (int32_t y = 0; y < height; ++y) {
                    uint32_t *row = &dst[size_t(y) * dstStride];
                    for (int32_t x = 0; x < width; ++x) {
                        DrawTexel<kOpaque>(color, &src[(size_t(y) * stepY + size_t(x) * stepX) * 4], &row[x]);
                    }
                }
                return;
            }

            using Rows = std::integral_constant<bool, O::kRows>;
            DrawBGRARows<O, kOpaque, kSkip>(Rows(), premultiplied, alpha, src, stepY, width, height, dst, dstStride);
        }

        //-----------------------------
        template <class O, bool kOpaque>
        FONTRENDERER_NO_INLINE void
        DrawBGRA(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                 uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const uint8_t *src = &texture[offset * 4];
            if (UsesShortcuts<kOpaque>(width)) {
                DrawBGRAGlyph<O, kOpaque, true>(premultiplied, alpha, src, stepX, stepY, width, height, dst, dstStride);
            }
            else {
                DrawBGRAGlyph<O, kOpaque, false>(premultiplied, alpha, src, stepX, stepY, width, height, dst, dstStride);
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
    DrawRows(Scenario &scenario, uint32_t *dst) {
        DrawQuads<RowsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawWide(Scenario &scenario, uint32_t *dst) {
        DrawQuads<WideOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawTall(Scenario &scenario, uint32_t *dst) {
        DrawQuads<TallOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSplit(Scenario &scenario, uint32_t *dst) {
        DrawQuads<SplitOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSplitRowsWide(Scenario &scenario, uint32_t *dst) {
        DrawQuads<SplitRowsWideOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSplitRowsTall(Scenario &scenario, uint32_t *dst) {
        DrawQuads<SplitRowsTallOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSplitRowsWideNoUnroll(Scenario &scenario, uint32_t *dst) {
        DrawQuads<SplitRowsWideNoUnrollOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSplitRowsTallNoUnroll(Scenario &scenario, uint32_t *dst) {
        DrawQuads<SplitRowsTallNoUnrollOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSplitRowsHybridNoUnroll(Scenario &scenario, uint32_t *dst) {
        DrawQuads<SplitRowsHybridNoUnrollOptions>(scenario, dst);
    }

} // end of namespace Avx2
} // end of namespace Benchmark
