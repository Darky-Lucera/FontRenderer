#include "benchmarkCommon.h"

#if defined(FONTRENDERER_SSE2)

#include <cstring>
#include <emmintrin.h>
#include <type_traits>

// Sse2PerCase without pixels drawn one at a time, with the changes that made NeonPacked faster on ARM, each one measured
// again on x86. The same pixels as DrawText.
//
// A row of 4 pixels or more is drawn in blocks of 4. Its last block ends where the row ends, so it can overlap the block
// before it. Its pixels are read before the row writes anything: both blocks then compute the same values for the pixels
// they share, and the one that writes last changes nothing. Read after the block before it is written, they would wait
// until that write reaches the cache, because x86 cannot pass part of a recent write to a read. A row of 1 to 3 pixels
// is drawn 2 pixels at a time. Rotated glyphs of Alpha8 are drawn 4 rows at a time: the texels down a column of the glyph
// are contiguous in the texture, so 4 reads of 4 bytes and a transposition give the coverage of 4 rows in 4 columns. The
// 1 to 3 rows left are the end of a last band that overlaps the one above.
//
// The blend keeps the blue and red of 4 pixels in one register and their green and alpha in another, each channel in a
// 16-bit lane, as the scalar code does with two channels per uint32_t. The layout of pixman, with each channel of 2
// pixels in a 16-bit lane, needs 2 registers for 4 pixels and the coverage of each pixel copied over 4 lanes instead of
// 2. Separating the channels here takes an AND and a shift, which x86 runs on several ports, while unpacking bytes runs
// on a single one.
//
// Only glyphs at least 16 pixels wide, or 8 under a translucent text, ask whether a block can skip the blend, and a band
// asks once for its 4 rows. A block of opaque texels of BGRA32 is blended like any other: tinting it without the blend
// does not pay for the branch.
//
// Each of the other variants changes one choice of Draw. DrawOptions lists them.

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
namespace Sse2Overlap {

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

        // A compiler can stop inlining the helpers once a glyph function has many of them, and then pass the color through
        // memory for every block. ALWAYS_INLINE keeps them inside the function of the glyph.

        using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                           uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

        // How a row of 4 pixels or more draws the 1 to 3 pixels left after its blocks of 4.
        enum class ETail {
            Pixels,     // One at a time
            Overlap,    // With a last block that ends where the row ends, whose pixels are read before the row writes any
            Late,       // With the same block, read after the block before it is written. The pixels both blocks share get
                        // no coverage the second time, so that they are not blended twice.
        };

        // A glyph at least as wide as its skip width asks for each block whether it can skip the blend. Wide glyphs have
        // long runs of blocks without coverage or fully covered. In narrow glyphs those blocks are rare, and the branch can
        // cost more than the blends it saves. kSkipAlways and kSkipNever ask for every glyph or for none.
        constexpr int32_t kSkipAlways = 0;
        constexpr int32_t kSkipNever  = -1;

        // The choices of Draw. Each of the other variants changes one of them.
        //-----------------------------
        struct DrawOptions {
            static constexpr ETail   kTail                 = ETail::Overlap;
            static constexpr bool    kPairTail             = false;    // When 1 or 2 pixels are left, the last block has 2 pixels
            static constexpr bool    kNarrowRows           = true;     // A row of 1 to 3 pixels in blocks of 2, not one pixel at a time
            static constexpr bool    kBands                = true;     // Rotated glyphs of Alpha8 drawn 4 rows at a time
            static constexpr bool    kBandShortcuts        = true;     // A band asks once for its 4 rows whether it can skip the blend
            static constexpr bool    kSplit                = true;     // The split layout instead of the one of pixman
            static constexpr bool    kUprightApart         = false;    // Glyphs that are not rotated are drawn by a function of their own
            static constexpr bool    kOctets               = false;    // Blocks of 8 pixels while 8 fit, which ask once whether they can skip the blend
            static constexpr bool    kBgraTint             = false;    // A block of opaque texels under an opaque text is tinted without blending
            static constexpr bool    kWidths               = false;    // Upright glyphs without shortcuts pick the code of their row width once
            static constexpr bool    kBandWidths           = false;    // kWidths for the bands of rotated glyphs
            static constexpr bool    kWidthIfs             = false;    // kWidths and kBandWidths pick a case, but every case draws as Any
            static constexpr bool    kHideWidth            = false;    // With kWidthIfs, the compiler does not see the range of width of each case
            static constexpr int32_t kOpaqueSkipWidth      = 16;
            static constexpr int32_t kTranslucentSkipWidth = 8;        // Lower: the blend of a translucent text costs more
        };

        //-----------------------------
        struct PixelsOptions : DrawOptions {
            static constexpr ETail   kTail                 = ETail::Pixels;
        };

        //-----------------------------
        struct LateOptions : DrawOptions {
            static constexpr ETail   kTail                 = ETail::Late;
        };

        //-----------------------------
        struct PairTailOptions : DrawOptions {
            static constexpr bool    kPairTail             = true;
        };

        //-----------------------------
        struct NoNarrowRowsOptions : DrawOptions {
            static constexpr bool    kNarrowRows           = false;
        };

        //-----------------------------
        struct NoBandsOptions : DrawOptions {
            static constexpr bool    kBands                = false;
        };

        //-----------------------------
        struct RowShortcutsOptions : DrawOptions {
            static constexpr bool    kBandShortcuts        = false;
        };

        //-----------------------------
        struct PixmanOptions : DrawOptions {
            static constexpr bool    kSplit                = false;
        };

        //-----------------------------
        struct UprightApartOptions : DrawOptions {
            static constexpr bool    kUprightApart         = true;
        };

        //-----------------------------
        struct OctetsOptions : DrawOptions {
            static constexpr bool    kOctets               = true;
        };

        //-----------------------------
        struct BgraTintOptions : DrawOptions {
            static constexpr bool    kBgraTint             = true;
        };

        //-----------------------------
        struct SkipOptions : DrawOptions {
            static constexpr int32_t kOpaqueSkipWidth      = kSkipAlways;
            static constexpr int32_t kTranslucentSkipWidth = kSkipAlways;
        };

        //-----------------------------
        struct BranchlessOptions : DrawOptions {
            static constexpr int32_t kOpaqueSkipWidth      = kSkipNever;
            static constexpr int32_t kTranslucentSkipWidth = kSkipNever;
        };

        //-----------------------------
        struct Opaque8Options : DrawOptions {
            static constexpr int32_t kOpaqueSkipWidth      = 8;
        };

        //-----------------------------
        struct Opaque24Options : DrawOptions {
            static constexpr int32_t kOpaqueSkipWidth      = 24;
        };

        //-----------------------------
        struct Translucent0Options : DrawOptions {
            static constexpr int32_t kTranslucentSkipWidth = kSkipAlways;
        };

        //-----------------------------
        struct Translucent16Options : DrawOptions {
            static constexpr int32_t kTranslucentSkipWidth = 16;
        };

        //-----------------------------
        struct WidthsOptions : DrawOptions {
            static constexpr bool    kWidths               = true;
        };

        //-----------------------------
        struct BandWidthsOptions : DrawOptions {
            static constexpr bool    kBandWidths           = true;
        };

        //-----------------------------
        struct WidthsBandWidthsOptions : DrawOptions {
            static constexpr bool    kWidths               = true;
            static constexpr bool    kBandWidths           = true;
        };

        //-----------------------------
        struct WidthIfsOptions : WidthsBandWidthsOptions {
            static constexpr bool    kWidthIfs             = true;
        };

        //-----------------------------
        struct WidthIfsHiddenOptions : WidthIfsOptions {
            static constexpr bool    kHideWidth            = true;
        };

        //-----------------------------
        template <class O, bool kOpaque>
        ALWAYS_INLINE bool
        UsesShortcuts(int32_t width) {
            constexpr int32_t kSkipWidth = kOpaque ? O::kOpaqueSkipWidth : O::kTranslucentSkipWidth;
            return kSkipWidth == kSkipAlways || (kSkipWidth != kSkipNever && width >= kSkipWidth);
        }

        //-----------------------------
        struct Color {
            __m128i     solid;          // The premultiplied color for 4 pixels
            __m128i     words;          // The same with each channel in a 16-bit lane, for 2 pixels
            __m128i     even;           // Its blue and red for 4 pixels, each in a 16-bit lane
            __m128i     odd;            // Its green and alpha for 4 pixels
            __m128i     alpha;          // The alpha of the color in every lane
            uint32_t    premultiplied;
            bool        white;
        };

        //-----------------------------
        ALWAYS_INLINE Color
        MakeColor(uint32_t premultiplied, uint32_t alpha) {
            Color color;
            color.solid         = _mm_set1_epi32(int(premultiplied));
            color.words         = _mm_unpacklo_epi8(color.solid, _mm_setzero_si128());
            color.even          = _mm_and_si128(color.solid, _mm_set1_epi16(0x00ff));
            color.odd           = _mm_srli_epi16(color.solid, 8);
            color.alpha         = _mm_set1_epi16(int16_t(alpha));
            color.premultiplied = premultiplied;
            color.white         = premultiplied == 0xffffffffu;
            return color;
        }

        // The 4 bytes at p as a number. x86 is little-endian, so the first byte is the lowest.
        //-----------------------------
        ALWAYS_INLINE uint32_t
        Load32(const uint8_t *p) {
            uint32_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        // The same for 2 bytes.
        //-----------------------------
        ALWAYS_INLINE uint32_t
        Load16(const uint8_t *p) {
            uint16_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        // 4 pixels or texels, as they are in memory.
        //-----------------------------
        ALWAYS_INLINE __m128i
        LoadQuad(const void *p) {
            return _mm_loadu_si128(static_cast<const __m128i *>(p));
        }

        // Writes the 4 pixels of x.
        //-----------------------------
        ALWAYS_INLINE void
        StoreQuad(void *p, __m128i x) {
            _mm_storeu_si128(static_cast<__m128i *>(p), x);
        }

        // 2 pixels or texels in the low half of a register, and 0 in the high half.
        //-----------------------------
        ALWAYS_INLINE __m128i
        LoadPair(const void *p) {
            return _mm_loadl_epi64(static_cast<const __m128i *>(p));
        }

        // Writes the 2 pixels in the low half of x.
        //-----------------------------
        ALWAYS_INLINE void
        StorePair(void *p, __m128i x) {
            _mm_storel_epi64(static_cast<__m128i *>(p), x);
        }

        // Writes the 2 pixels in the high half of x.
        //-----------------------------
        ALWAYS_INLINE void
        StoreHighPair(void *p, __m128i x) {
            _mm_storeh_pd(static_cast<double *>(p), _mm_castsi128_pd(x));
        }

        // 1 pixel or texel in the low 32 bits of a register.
        //-----------------------------
        ALWAYS_INLINE __m128i
        LoadOne(const void *p) {
            return _mm_cvtsi32_si128(int(Load32(static_cast<const uint8_t *>(p))));
        }

        // Writes the pixel in the low 32 bits of x.
        //-----------------------------
        ALWAYS_INLINE void
        StoreOne(void *p, __m128i x) {
            const uint32_t value = uint32_t(_mm_cvtsi128_si32(x));
            memcpy(p, &value, sizeof(value));
        }

        // Each 16-bit lane divided by 255 and rounded: (x + 128) * 257 >> 16. Exact up to 65152, the largest sum of a blend.
        //-----------------------------
        ALWAYS_INLINE __m128i
        Div255(__m128i x) {
            return _mm_mulhi_epu16(_mm_add_epi16(x, _mm_set1_epi16(0x0080)), _mm_set1_epi16(0x0101));
        }

        // Rounded (source + pixels * (255 - alpha)) / 255 in each 16-bit lane, with the source already multiplied.
        //-----------------------------
        ALWAYS_INLINE __m128i
        Blend(__m128i source, __m128i alpha, __m128i pixels) {
            const __m128i inverse = _mm_xor_si128(alpha, _mm_set1_epi16(0x00ff));
            return Div255(_mm_add_epi16(source, _mm_mullo_epi16(pixels, inverse)));
        }

        // The bytes 0, 2, 4 and so on of x, each in a 16-bit lane: the blue and red of 4 pixels.
        //-----------------------------
        ALWAYS_INLINE __m128i
        EvenBytes(__m128i x) {
            return _mm_and_si128(x, _mm_set1_epi16(0x00ff));
        }

        // The bytes 1, 3, 5 and so on of x, each in a 16-bit lane: the green and alpha of 4 pixels.
        //-----------------------------
        ALWAYS_INLINE __m128i
        OddBytes(__m128i x) {
            return _mm_srli_epi16(x, 8);
        }

        // The opposite of EvenBytes and OddBytes: 4 pixels again.
        //-----------------------------
        ALWAYS_INLINE __m128i
        JoinBytes(__m128i even, __m128i odd) {
            return _mm_or_si128(even, _mm_slli_epi16(odd, 8));
        }

        // The 4 coverage bytes in the low 32 bits of x, each copied to the lanes of its pixel: over its 4 bytes in the
        // layout of pixman (m0 m0 m0 m0 m1 m1 m1 m1 ...), and over 2 lanes of 16 bits in the split one (m0 m0 m1 m1 ...).
        //-----------------------------
        template <bool kSplit>
        ALWAYS_INLINE __m128i
        Spread(__m128i x) {
#if defined(__clang__)
            // The same form as Spread in src/GlyphDrawSse2.cpp, for the same reason.
            if (kSplit) {
                const __m128i twice = _mm_unpacklo_epi8(x, x);
                return _mm_srli_epi16(_mm_unpacklo_epi16(twice, twice), 8);
            }
#endif
            const __m128i doubled = _mm_unpacklo_epi8(x, kSplit ? _mm_setzero_si128() : x);
            return _mm_unpacklo_epi16(doubled, doubled);
        }

        // Spread for each row of a band, whose coverage has a row in each 32-bit lane.
        //-----------------------------
        template <bool kSplit>
        ALWAYS_INLINE void
        SpreadBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
            const __m128i other = kSplit ? _mm_setzero_si128() : band;
            const __m128i low   = _mm_unpacklo_epi8(band, other);
            const __m128i high  = _mm_unpackhi_epi8(band, other);
            row0 = _mm_unpacklo_epi16(low, low);
            row1 = _mm_unpackhi_epi16(low, low);
            row2 = _mm_unpacklo_epi16(high, high);
            row3 = _mm_unpackhi_epi16(high, high);
        }

        // 2 pixels with each channel in a 16-bit lane, blended with the color. The coverage of each pixel is in its 4 lanes.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE __m128i
        BlendCoverageWords(const Color &color, __m128i coverage, __m128i pixels) {
            const __m128i alpha = kOpaque ? coverage : Div255(_mm_mullo_epi16(coverage, color.alpha));
            return Blend(_mm_mullo_epi16(color.words, coverage), alpha, pixels);
        }

        // 4 pixels blended with the color, with the coverage from Spread. A coverage m draws as the premultiplied white
        // texel (m, m, m, m). Under an opaque text, the alpha of the blend is the coverage itself.
        //-----------------------------
        template <bool kSplit, bool kOpaque>
        ALWAYS_INLINE __m128i
        BlendCoverage(const Color &color, __m128i spread, __m128i pixels) {
            if (kSplit) {
                const __m128i alpha = kOpaque ? spread : Div255(_mm_mullo_epi16(spread, color.alpha));
                const __m128i even  = Blend(_mm_mullo_epi16(color.even, spread), alpha, EvenBytes(pixels));
                const __m128i odd   = Blend(_mm_mullo_epi16(color.odd,  spread), alpha, OddBytes(pixels));
                return JoinBytes(even, odd);
            }

            const __m128i zero = _mm_setzero_si128();
            const __m128i lo   = BlendCoverageWords<kOpaque>(color, _mm_unpacklo_epi8(spread, zero), _mm_unpacklo_epi8(pixels, zero));
            const __m128i hi   = BlendCoverageWords<kOpaque>(color, _mm_unpackhi_epi8(spread, zero), _mm_unpackhi_epi8(pixels, zero));
            return _mm_packus_epi16(lo, hi);
        }

        // The low 2 pixels of BlendCoverage. In the layout of pixman they take half the work.
        //-----------------------------
        template <bool kSplit, bool kOpaque>
        ALWAYS_INLINE __m128i
        BlendCoveragePair(const Color &color, __m128i spread, __m128i pixels) {
            if (kSplit) {
                return BlendCoverage<kSplit, kOpaque>(color, spread, pixels);
            }

            const __m128i zero = _mm_setzero_si128();
            const __m128i lo   = BlendCoverageWords<kOpaque>(color, _mm_unpacklo_epi8(spread, zero), _mm_unpacklo_epi8(pixels, zero));
            return _mm_packus_epi16(lo, lo);
        }

        // The alpha of each of the 2 pixels of x, which has each channel in a 16-bit lane, over the 4 lanes of its pixel.
        //-----------------------------
        ALWAYS_INLINE __m128i
        ExpandAlphaWords(__m128i x) {
            return _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        }

        // The alpha of each of the 4 pixels of odd, which has their green and alpha, over the 2 lanes of its pixel.
        //-----------------------------
        ALWAYS_INLINE __m128i
        ExpandAlphaOdd(__m128i odd) {
            return _mm_shufflehi_epi16(_mm_shufflelo_epi16(odd, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
        }

        // 2 texels with each channel in a 16-bit lane, times the color and blended with 2 pixels. The alpha of the blend is
        // the alpha of the texel under an opaque text, and the alpha of the texel times the color, rounded, under a
        // translucent one.
        //-----------------------------
        template <bool kOpaque>
        ALWAYS_INLINE __m128i
        BlendTexelsWords(const Color &color, __m128i texels, __m128i pixels) {
            const __m128i source = _mm_mullo_epi16(texels, color.words);
            const __m128i alpha  = kOpaque ? ExpandAlphaWords(texels) : Div255(ExpandAlphaWords(source));
            return Blend(source, alpha, pixels);
        }

        // The same for 4 texels and 4 pixels.
        //-----------------------------
        template <bool kSplit, bool kOpaque>
        ALWAYS_INLINE __m128i
        BlendTexels(const Color &color, __m128i texels, __m128i pixels) {
            if (kSplit) {
                const __m128i odd       = OddBytes(texels);
                const __m128i sourceOdd = _mm_mullo_epi16(odd, color.odd);
                const __m128i alpha     = ExpandAlphaOdd(kOpaque ? odd : Div255(sourceOdd));
                const __m128i even      = Blend(_mm_mullo_epi16(EvenBytes(texels), color.even), alpha, EvenBytes(pixels));
                return JoinBytes(even, Blend(sourceOdd, alpha, OddBytes(pixels)));
            }

            const __m128i zero = _mm_setzero_si128();
            const __m128i lo   = BlendTexelsWords<kOpaque>(color, _mm_unpacklo_epi8(texels, zero), _mm_unpacklo_epi8(pixels, zero));
            const __m128i hi   = BlendTexelsWords<kOpaque>(color, _mm_unpackhi_epi8(texels, zero), _mm_unpackhi_epi8(pixels, zero));
            return _mm_packus_epi16(lo, hi);
        }

        // The low 2 pixels of BlendTexels.
        //-----------------------------
        template <bool kSplit, bool kOpaque>
        ALWAYS_INLINE __m128i
        BlendTexelsPair(const Color &color, __m128i texels, __m128i pixels) {
            if (kSplit) {
                return BlendTexels<kSplit, kOpaque>(color, texels, pixels);
            }

            const __m128i zero = _mm_setzero_si128();
            const __m128i lo   = BlendTexelsWords<kOpaque>(color, _mm_unpacklo_epi8(texels, zero), _mm_unpacklo_epi8(pixels, zero));
            return _mm_packus_epi16(lo, lo);
        }

        // Each channel of 4 texels times the same channel of the color, rounded: the blend of opaque texels under an opaque
        // text, which does not depend on the pixels.
        //-----------------------------
        template <bool kSplit>
        ALWAYS_INLINE __m128i
        Tint(const Color &color, __m128i texels) {
            if (kSplit) {
                return JoinBytes(Div255(_mm_mullo_epi16(EvenBytes(texels), color.even)), Div255(_mm_mullo_epi16(OddBytes(texels), color.odd)));
            }

            const __m128i zero = _mm_setzero_si128();
            const __m128i lo   = Div255(_mm_mullo_epi16(_mm_unpacklo_epi8(texels, zero), color.words));
            const __m128i hi   = Div255(_mm_mullo_epi16(_mm_unpackhi_epi8(texels, zero), color.words));
            return _mm_packus_epi16(lo, hi);
        }

        // The alpha of the 4 texels is 0.
        //-----------------------------
        ALWAYS_INLINE bool
        IsTransparent(__m128i texels) {
            return (_mm_movemask_epi8(_mm_cmpeq_epi8(texels, _mm_setzero_si128())) & 0x8888) == 0x8888;
        }

        // The alpha of the 4 texels is 255.
        //-----------------------------
        ALWAYS_INLINE bool
        IsOpaque(__m128i texels) {
            return (_mm_movemask_epi8(_mm_cmpeq_epi8(texels, _mm_set1_epi8(-1))) & 0x8888) == 0x8888;
        }

        // All ones in the 32-bit lanes from drawn on, and 0 in the ones before: the texels of a block that are not drawn yet.
        //-----------------------------
        ALWAYS_INLINE __m128i
        KeepFrom(size_t drawn) {
            return _mm_cmpgt_epi32(_mm_setr_epi32(1, 2, 3, 4), _mm_set1_epi32(int(drawn)));
        }

        // The coverage of 4 pixels in a row, a byte each. The texels of a rotated glyph are step bytes apart.
        //-----------------------------
        template <bool kRotated>
        ALWAYS_INLINE uint32_t
        LoadCoverage(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8) | (uint32_t(mask[2 * step]) << 16) | (uint32_t(mask[3 * step]) << 24);
            }

            return Load32(mask);
        }

        // The same for 2 pixels.
        //-----------------------------
        template <bool kRotated>
        ALWAYS_INLINE uint32_t
        LoadCoveragePair(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8);
            }

            return Load16(mask);
        }

        // The coverage of 4 rows in 4 columns of a rotated glyph, whose texels down a column are contiguous: one read of 4
        // bytes per column, and a transposition. Each 32-bit lane has the 4 columns of a row, with row 0 in the lowest.
        //-----------------------------
        ALWAYS_INLINE __m128i
        LoadBand(const uint8_t *column0, const uint8_t *column1, const uint8_t *column2, const uint8_t *column3) {
            const __m128i first  = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(Load32(column0))), _mm_cvtsi32_si128(int(Load32(column1))));
            const __m128i second = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(Load32(column2))), _mm_cvtsi32_si128(int(Load32(column3))));
            return _mm_unpacklo_epi16(first, second);
        }

        // 4 pixels at p with their coverage, spread for the blend. none and full say that the 4 coverages are 0 or 255.
        // With kSkip, such a block is left as it is, or gets the color under an opaque text. With kRead, pixels are the
        // pixels at p, read before the row wrote anything. Without it, they are read here, after the shortcuts.
        //-----------------------------
        template <bool kSplit, bool kOpaque, bool kSkip, bool kRead>
        ALWAYS_INLINE void
        DrawCoverage(const Color &color, __m128i spread, bool none, bool full, uint32_t *p, __m128i pixels) {
            if (kSkip && none) {
                return;
            }
            if (kSkip && kOpaque && full) {
                StoreQuad(p, color.solid);
                return;
            }

            StoreQuad(p, BlendCoverage<kSplit, kOpaque>(color, spread, kRead ? pixels : LoadQuad(p)));
        }

        // One pixel of coverage m, with the shortcuts of the scalar code.
        //-----------------------------
        template <bool kSplit, bool kOpaque>
        ALWAYS_INLINE void
        DrawCoveragePixel(const Color &color, uint32_t m, uint32_t *p) {
            if (kOpaque && m == 255) {
                *p = color.premultiplied;
            }
            else if (m != 0) {
                StoreOne(p, BlendCoveragePair<kSplit, kOpaque>(color, Spread<kSplit>(_mm_cvtsi32_si128(int(m))), LoadOne(p)));
            }
        }

        // One texel over one pixel. A transparent texel leaves the pixel as it is.
        //-----------------------------
        template <bool kSplit, bool kOpaque>
        ALWAYS_INLINE void
        DrawTexelPixel(const Color &color, const uint8_t *texel, uint32_t *p) {
            const uint32_t t = Load32(texel);
            if ((t >> 24) != 0) {
                StoreOne(p, BlendTexelsPair<kSplit, kOpaque>(color, _mm_cvtsi32_si128(int(t)), LoadOne(p)));
            }
        }

        // DrawLine draws a row of a glyph with the functions of one of these: a row of Alpha8, 4 rows of a rotated glyph of
        // Alpha8, or a row of BGRA32. Read functions return the pixels of a block before the row writes anything. After
        // functions draw a block whose first `drawn` pixels are already drawn, so they give them no coverage.

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, bool kRotated>
        struct Alpha8Row {
            const Color     &color;
            const uint8_t   *mask;      // The coverage of the first pixel of the row
            size_t          step;       // Between two pixels of the row
            uint32_t        *dst;

            //-------------------------
            ALWAYS_INLINE uint32_t
            Coverage(size_t x) const {
                return LoadCoverage<kRotated>(&mask[x * step], step);
            }

            //-------------------------
            template <bool kRead>
            ALWAYS_INLINE void
            Draw(size_t x, uint32_t m, __m128i pixels) const {
                const __m128i spread = Spread<O::kSplit>(_mm_cvtsi32_si128(int(m)));
                DrawCoverage<O::kSplit, kOpaque, kSkip, kRead>(color, spread, m == 0, m == 0xffffffffu, &dst[x], pixels);
            }

            //-------------------------
            ALWAYS_INLINE void
            Quad(size_t x) const {
                Draw<false>(x, Coverage(x), _mm_setzero_si128());
            }

            // With kSkip, the 8 pixels ask once whether they can skip the blend, and then are blended without asking again.
            //-------------------------
            ALWAYS_INLINE void
            Octet(size_t x) const {
                const uint32_t low  = Coverage(x);
                const uint32_t high = Coverage(x + 4);
                if (kSkip && (low | high) == 0) {
                    return;
                }
                if (kSkip && kOpaque && (low & high) == 0xffffffffu) {
                    StoreQuad(&dst[x], color.solid);
                    StoreQuad(&dst[x + 4], color.solid);
                    return;
                }

                DrawCoverage<O::kSplit, kOpaque, false, false>(color, Spread<O::kSplit>(_mm_cvtsi32_si128(int(low))), false, false, &dst[x], _mm_setzero_si128());
                DrawCoverage<O::kSplit, kOpaque, false, false>(color, Spread<O::kSplit>(_mm_cvtsi32_si128(int(high))), false, false, &dst[x + 4], _mm_setzero_si128());
            }

            //-------------------------
            ALWAYS_INLINE __m128i
            ReadQuad(size_t x) const {
                return LoadQuad(&dst[x]);
            }

            //-------------------------
            ALWAYS_INLINE void
            QuadRead(size_t x, __m128i pixels) const {
                Draw<true>(x, Coverage(x), pixels);
            }

            //-------------------------
            ALWAYS_INLINE void
            QuadAfter(size_t x, size_t drawn) const {
                Draw<false>(x, Coverage(x) & (0xffffffffu << (8 * drawn)), _mm_setzero_si128());
            }

            //-------------------------
            ALWAYS_INLINE __m128i
            ReadPair(size_t x) const {
                return LoadPair(&dst[x]);
            }

            //-------------------------
            ALWAYS_INLINE void
            DrawPair(size_t x, uint32_t m, __m128i pixels) const {
                StorePair(&dst[x], BlendCoveragePair<O::kSplit, kOpaque>(color, Spread<O::kSplit>(_mm_cvtsi32_si128(int(m))), pixels));
            }

            //-------------------------
            ALWAYS_INLINE void
            PairRead(size_t x, __m128i pixels) const {
                DrawPair(x, LoadCoveragePair<kRotated>(&mask[x * step], step), pixels);
            }

            //-------------------------
            ALWAYS_INLINE void
            PairAfter(size_t x, size_t drawn) const {
                DrawPair(x, LoadCoveragePair<kRotated>(&mask[x * step], step) & (0xffffu << (8 * drawn)), LoadPair(&dst[x]));
            }

            //-------------------------
            ALWAYS_INLINE void
            Pixels(size_t from, size_t to) const {
                for (size_t x = from; x < to; ++x) {
                    DrawCoveragePixel<O::kSplit, kOpaque>(color, mask[x * step], &dst[x]);
                }
            }

            // A row of 1 to 3 pixels. With 3, the pixels 0 and 1 and the pixels 1 and 2 are read first and blended as one
            // block of 4.
            //-------------------------
            ALWAYS_INLINE void
            Narrow(int32_t w) const {
                if (w == 1) {
                    StoreOne(dst, BlendCoveragePair<O::kSplit, kOpaque>(color, Spread<O::kSplit>(_mm_cvtsi32_si128(int(mask[0]))), LoadOne(dst)));
                }
                else if (w == 2) {
                    DrawPair(0, LoadCoveragePair<kRotated>(mask, step), LoadPair(dst));
                }
                else {
                    const uint32_t m      = LoadCoveragePair<kRotated>(mask, step) | (LoadCoveragePair<kRotated>(&mask[step], step) << 16);
                    const __m128i  pixels = _mm_unpacklo_epi64(LoadPair(dst), LoadPair(&dst[1]));
                    const __m128i  result = BlendCoverage<O::kSplit, kOpaque>(color, Spread<O::kSplit>(_mm_cvtsi32_si128(int(m))), pixels);
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
            ALWAYS_INLINE bool
            Draws(int32_t row) const {
                return kPartial == false || row >= firstRow;
            }

            //-------------------------
            ALWAYS_INLINE uint32_t *
            At(int32_t row, size_t x) const {
                return &dst[size_t(row) * dstStride + x];
            }

            //-------------------------
            ALWAYS_INLINE __m128i
            Coverage(size_t x) const {
                const uint8_t *column = &mask[x * step];
                return LoadBand(column, &column[step], &column[2 * step], &column[3 * step]);
            }

            // The movemask has a bit per coverage, 4 for each row. With kBandShortcuts, the 4 rows skip the blend together
            // or not at all, with one branch instead of one per row. In a partial band, the test includes the rows of the
            // band above, which can only make the shortcut rarer.
            //-------------------------
            template <bool kRead>
            ALWAYS_INLINE void
            Draw(size_t x, __m128i band, const Rows &pixels) const {
                constexpr bool kRowShortcuts = kSkip && O::kBandShortcuts == false;
                const uint32_t none = kSkip ? uint32_t(_mm_movemask_epi8(_mm_cmpeq_epi8(band, _mm_setzero_si128()))) : 0;
                const uint32_t full = (kSkip && kOpaque) ? uint32_t(_mm_movemask_epi8(_mm_cmpeq_epi8(band, _mm_set1_epi8(-1)))) : 0;
                if (kSkip && O::kBandShortcuts) {
                    if (none == 0xffff) {
                        return;
                    }
                    if (kOpaque && full == 0xffff) {
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

                __m128i spread0, spread1, spread2, spread3;
                SpreadBand<O::kSplit>(band, spread0, spread1, spread2, spread3);
                if (Draws(0)) {
                    DrawCoverage<O::kSplit, kOpaque, kRowShortcuts, kRead>(color, spread0, (none & 0x000f) == 0x000f, (full & 0x000f) == 0x000f, At(0, x), pixels.row0);
                }
                if (Draws(1)) {
                    DrawCoverage<O::kSplit, kOpaque, kRowShortcuts, kRead>(color, spread1, (none & 0x00f0) == 0x00f0, (full & 0x00f0) == 0x00f0, At(1, x), pixels.row1);
                }
                if (Draws(2)) {
                    DrawCoverage<O::kSplit, kOpaque, kRowShortcuts, kRead>(color, spread2, (none & 0x0f00) == 0x0f00, (full & 0x0f00) == 0x0f00, At(2, x), pixels.row2);
                }
                DrawCoverage<O::kSplit, kOpaque, kRowShortcuts, kRead>(color, spread3, (none & 0xf000) == 0xf000, (full & 0xf000) == 0xf000, At(3, x), pixels.row3);
            }

            //-------------------------
            ALWAYS_INLINE void
            Quad(size_t x) const {
                Draw<false>(x, Coverage(x), Rows());
            }

            //-------------------------
            ALWAYS_INLINE void
            Octet(size_t x) const {
                Quad(x);
                Quad(x + 4);
            }

            //-------------------------
            ALWAYS_INLINE Rows
            ReadQuad(size_t x) const {
                Rows pixels;
                pixels.row0 = LoadQuad(At(0, x));
                pixels.row1 = LoadQuad(At(1, x));
                pixels.row2 = LoadQuad(At(2, x));
                pixels.row3 = LoadQuad(At(3, x));
                return pixels;
            }

            //-------------------------
            ALWAYS_INLINE void
            QuadRead(size_t x, const Rows &pixels) const {
                Draw<true>(x, Coverage(x), pixels);
            }

            //-------------------------
            ALWAYS_INLINE void
            QuadAfter(size_t x, size_t drawn) const {
                Draw<false>(x, _mm_and_si128(Coverage(x), _mm_set1_epi32(int(0xffffffffu << (8 * drawn)))), Rows());
            }

            //-------------------------
            ALWAYS_INLINE Rows
            ReadPair(size_t x) const {
                Rows pixels;
                pixels.row0 = LoadPair(At(0, x));
                pixels.row1 = LoadPair(At(1, x));
                pixels.row2 = LoadPair(At(2, x));
                pixels.row3 = LoadPair(At(3, x));
                return pixels;
            }

            // The coverage of 2 columns is read as the 4 columns x, x + 1, x and x + 1.
            //-------------------------
            template <bool kRead>
            ALWAYS_INLINE void
            DrawPairs(size_t x, __m128i band, const Rows &pixels) const {
                __m128i spread0, spread1, spread2, spread3;
                SpreadBand<O::kSplit>(band, spread0, spread1, spread2, spread3);
                if (Draws(0)) {
                    StorePair(At(0, x), BlendCoveragePair<O::kSplit, kOpaque>(color, spread0, kRead ? pixels.row0 : LoadPair(At(0, x))));
                }
                if (Draws(1)) {
                    StorePair(At(1, x), BlendCoveragePair<O::kSplit, kOpaque>(color, spread1, kRead ? pixels.row1 : LoadPair(At(1, x))));
                }
                if (Draws(2)) {
                    StorePair(At(2, x), BlendCoveragePair<O::kSplit, kOpaque>(color, spread2, kRead ? pixels.row2 : LoadPair(At(2, x))));
                }
                StorePair(At(3, x), BlendCoveragePair<O::kSplit, kOpaque>(color, spread3, kRead ? pixels.row3 : LoadPair(At(3, x))));
            }

            //-------------------------
            ALWAYS_INLINE __m128i
            PairCoverage(size_t x) const {
                const uint8_t *column = &mask[x * step];
                return LoadBand(column, &column[step], column, &column[step]);
            }

            //-------------------------
            ALWAYS_INLINE void
            PairRead(size_t x, const Rows &pixels) const {
                DrawPairs<true>(x, PairCoverage(x), pixels);
            }

            //-------------------------
            ALWAYS_INLINE void
            PairAfter(size_t x, size_t drawn) const {
                DrawPairs<false>(x, _mm_and_si128(PairCoverage(x), _mm_set1_epi32(int(0xffffffffu << (8 * drawn)))), Rows());
            }

            //-------------------------
            ALWAYS_INLINE void
            Pixels(size_t from, size_t to) const {
                for (size_t x = from; x < to; ++x) {
                    const uint8_t *column = &mask[x * step];
                    if (Draws(0)) {
                        DrawCoveragePixel<O::kSplit, kOpaque>(color, column[0], At(0, x));
                    }
                    if (Draws(1)) {
                        DrawCoveragePixel<O::kSplit, kOpaque>(color, column[1], At(1, x));
                    }
                    if (Draws(2)) {
                        DrawCoveragePixel<O::kSplit, kOpaque>(color, column[2], At(2, x));
                    }
                    DrawCoveragePixel<O::kSplit, kOpaque>(color, column[3], At(3, x));
                }
            }

            // A band is at least 4 pixels wide, so this is never called.
            //-------------------------
            ALWAYS_INLINE void
            Narrow(int32_t w) const {
                Pixels(0, size_t(w));
            }
        };

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        struct BgraRow {
            const Color     &color;
            const uint8_t   *src;       // The texel of the first pixel of the row
            uint32_t        *dst;

            //-------------------------
            ALWAYS_INLINE const uint8_t *
            Texel(size_t x) const {
                return &src[4 * x];
            }

            // With kSkip, a block of transparent texels is left as it is, and one of opaque texels under an opaque text is
            // written without blending: tinted with the color, or as it is when the color is white.
            //-------------------------
            template <bool kRead>
            ALWAYS_INLINE void
            Draw(size_t x, __m128i texels, __m128i pixels) const {
                if (kSkip && IsTransparent(texels)) {
                    return;
                }
                if (kSkip && kOpaque && O::kBgraTint && IsOpaque(texels)) {
                    StoreQuad(&dst[x], color.white ? texels : Tint<O::kSplit>(color, texels));
                    return;
                }

                StoreQuad(&dst[x], BlendTexels<O::kSplit, kOpaque>(color, texels, kRead ? pixels : LoadQuad(&dst[x])));
            }

            //-------------------------
            ALWAYS_INLINE void
            Quad(size_t x) const {
                Draw<false>(x, LoadQuad(Texel(x)), _mm_setzero_si128());
            }

            // As Alpha8Row::Octet. The alpha of (low | high) is 0 when both alphas are 0, and the alpha of (low & high) is
            // 255 when both are 255.
            //-------------------------
            ALWAYS_INLINE void
            Octet(size_t x) const {
                const __m128i low  = LoadQuad(Texel(x));
                const __m128i high = LoadQuad(Texel(x + 4));
                if (kSkip && IsTransparent(_mm_or_si128(low, high))) {
                    return;
                }
                if (kSkip && kOpaque && O::kBgraTint && IsOpaque(_mm_and_si128(low, high))) {
                    StoreQuad(&dst[x], color.white ? low : Tint<O::kSplit>(color, low));
                    StoreQuad(&dst[x + 4], color.white ? high : Tint<O::kSplit>(color, high));
                    return;
                }

                StoreQuad(&dst[x], BlendTexels<O::kSplit, kOpaque>(color, low, LoadQuad(&dst[x])));
                StoreQuad(&dst[x + 4], BlendTexels<O::kSplit, kOpaque>(color, high, LoadQuad(&dst[x + 4])));
            }

            //-------------------------
            ALWAYS_INLINE __m128i
            ReadQuad(size_t x) const {
                return LoadQuad(&dst[x]);
            }

            //-------------------------
            ALWAYS_INLINE void
            QuadRead(size_t x, __m128i pixels) const {
                Draw<true>(x, LoadQuad(Texel(x)), pixels);
            }

            //-------------------------
            ALWAYS_INLINE void
            QuadAfter(size_t x, size_t drawn) const {
                Draw<false>(x, _mm_and_si128(LoadQuad(Texel(x)), KeepFrom(drawn)), _mm_setzero_si128());
            }

            //-------------------------
            ALWAYS_INLINE __m128i
            ReadPair(size_t x) const {
                return LoadPair(&dst[x]);
            }

            //-------------------------
            ALWAYS_INLINE void
            PairRead(size_t x, __m128i pixels) const {
                StorePair(&dst[x], BlendTexelsPair<O::kSplit, kOpaque>(color, LoadPair(Texel(x)), pixels));
            }

            //-------------------------
            ALWAYS_INLINE void
            PairAfter(size_t x, size_t drawn) const {
                StorePair(&dst[x], BlendTexelsPair<O::kSplit, kOpaque>(color, _mm_and_si128(LoadPair(Texel(x)), KeepFrom(drawn)), LoadPair(&dst[x])));
            }

            //-------------------------
            ALWAYS_INLINE void
            Pixels(size_t from, size_t to) const {
                for (size_t x = from; x < to; ++x) {
                    DrawTexelPixel<O::kSplit, kOpaque>(color, Texel(x), &dst[x]);
                }
            }

            // A row of 1 to 3 pixels, as Alpha8Row::Narrow draws it.
            //-------------------------
            ALWAYS_INLINE void
            Narrow(int32_t w) const {
                if (w == 1) {
                    StoreOne(dst, BlendTexelsPair<O::kSplit, kOpaque>(color, LoadOne(src), LoadOne(dst)));
                }
                else if (w == 2) {
                    StorePair(dst, BlendTexelsPair<O::kSplit, kOpaque>(color, LoadPair(src), LoadPair(dst)));
                }
                else {
                    const __m128i texels = _mm_unpacklo_epi64(LoadPair(src), LoadPair(Texel(1)));
                    const __m128i pixels = _mm_unpacklo_epi64(LoadPair(dst), LoadPair(&dst[1]));
                    const __m128i result = BlendTexels<O::kSplit, kOpaque>(color, texels, pixels);
                    StoreHighPair(&dst[1], result);
                    StorePair(dst, result);
                }
            }
        };

        // The blocks of a row from its first pixel until end. The last block can go up to 3 pixels past end. With kOctets,
        // the blocks have 8 pixels while the second half of one starts before end. Returns where the last block ends.
        //-----------------------------
        template <class O, class TLine>
        ALWAYS_INLINE size_t
        DrawBlocks(const TLine &line, size_t end) {
            size_t x = 0;
            if (O::kOctets) {
                for (; x + 4 < end; x += 8) {
                    line.Octet(x);
                }
            }
            for (; x < end; x += 4) {
                line.Quad(x);
            }
            return x;
        }

        // A row of w pixels, or a band of 4 rows. The last block of a row is read before the row writes anything, and the
        // blocks before it are drawn as they come, so a row needs no branch on how many pixels are left. The clipping of
        // DrawText can leave a width below 1, which draws nothing.
        //-----------------------------
        template <class O, class TLine>
        ALWAYS_INLINE void
        DrawLine(const TLine &line, int32_t w) {
            if (w >= 4) {
                const size_t width = size_t(w);
                const size_t rest  = width & 3;
                if (O::kTail == ETail::Pixels) {
                    const size_t full = width - rest;
                    DrawBlocks<O>(line, full);
                    line.Pixels(full, width);
                }
                else if (O::kPairTail && (rest == 1 || rest == 2)) {
                    const size_t full = width - rest;
                    const size_t tail = width - 2;
                    if (O::kTail == ETail::Late) {
                        DrawBlocks<O>(line, full);
                        line.PairAfter(tail, full - tail);
                    }
                    else {
                        const auto pixels = line.ReadPair(tail);
                        DrawBlocks<O>(line, full);
                        line.PairRead(tail, pixels);
                    }
                }
                else {
                    const size_t tail = width - 4;
                    if (O::kTail == ETail::Late) {
                        const size_t drawn = DrawBlocks<O>(line, tail) - tail;
                        line.QuadAfter(tail, drawn);
                    }
                    else {
                        const auto pixels = line.ReadQuad(tail);
                        DrawBlocks<O>(line, tail);
                        line.QuadRead(tail, pixels);
                    }
                }
            }
            else if (w > 0) {
                if (O::kNarrowRows) {
                    line.Narrow(w);
                }
                else {
                    line.Pixels(0, size_t(w));
                }
            }
        }

        // The widths of row that a glyph can pick once for all its rows, so that they need no branch on the width. They
        // draw as DrawLine does with the choices of DrawOptions.
        enum class EWidth {
            Any,        // DrawLine decides for each row
            Narrow,     // 1 to 3 pixels
            One,        // 4 pixels: one block
            Two,        // 5 to 8 pixels: two blocks, which overlap below 8
        };

        template <EWidth kWidth>
        using WidthTag = std::integral_constant<EWidth, kWidth>;

        // The code that draws the rows of a glyph whose width picked kWidth. With kWidthIfs it is always the code for any
        // width.
        template <class O, EWidth kWidth>
        using RowWidthTag = WidthTag<O::kWidthIfs ? EWidth::Any : kWidth>;

        // From the branch that picked the case, GCC knows the range of the width, and it builds from the code for any
        // width the same code as the case written for that range. The empty asm hides the width, so the code stays the
        // one for any width. MSVC has no inline assembly for x64, so with it the width is never hidden.
        //-----------------------------
        template <class O>
        ALWAYS_INLINE int32_t
        HideWidth(int32_t width) {
        #if defined(__GNUC__) || defined(__clang__)
            if (O::kHideWidth) {
                __asm__("" : "+r"(width));
            }
        #endif
            return width;
        }

        //-----------------------------
        template <class O, class TRow>
        ALWAYS_INLINE void
        DrawRowOf(WidthTag<EWidth::Any>, const TRow &row, int32_t w) {
            DrawLine<O>(row, w);
        }

        //-----------------------------
        template <class O, class TRow>
        ALWAYS_INLINE void
        DrawRowOf(WidthTag<EWidth::Narrow>, const TRow &row, int32_t w) {
            row.Narrow(w);
        }

        //-----------------------------
        template <class O, class TRow>
        ALWAYS_INLINE void
        DrawRowOf(WidthTag<EWidth::One>, const TRow &row, int32_t) {
            row.QuadRead(0, row.ReadQuad(0));
        }

        //-----------------------------
        template <class O, class TRow>
        ALWAYS_INLINE void
        DrawRowOf(WidthTag<EWidth::Two>, const TRow &row, int32_t w) {
            const size_t tail   = size_t(w) - 4;
            const auto   pixels = row.ReadQuad(tail);
            row.Quad(0);
            row.QuadRead(tail, pixels);
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, EWidth kWidth>
        ALWAYS_INLINE void
        DrawAlpha8RowsOf(const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride) {
            width = HideWidth<O>(width);
            for (int32_t y = 0; y < height; ++y) {
                DrawRowOf<O>(RowWidthTag<O, kWidth>(), Alpha8Row<O, kOpaque, kSkip, false> { color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride] }, width);
            }
        }

        // The rows of an upright glyph, with any width, or, with the first tag, with the code of their width.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawAlpha8Rows(std::false_type, const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height,
                       uint32_t *dst, uint32_t dstStride) {
            DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::Any>(color, mask, stepY, width, height, dst, dstStride);
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawAlpha8Rows(std::true_type, const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height,
                       uint32_t *dst, uint32_t dstStride) {
            if (width > 8) {
                DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::Any>(color, mask, stepY, width, height, dst, dstStride);
            }
            else if (width > 4) {
                DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::Two>(color, mask, stepY, width, height, dst, dstStride);
            }
            else if (width == 4) {
                DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::One>(color, mask, stepY, width, height, dst, dstStride);
            }
            else if (width > 0) {
                DrawAlpha8RowsOf<O, kOpaque, kSkip, EWidth::Narrow>(color, mask, stepY, width, height, dst, dstStride);
            }
        }

        // The glyphs that ask for the shortcuts are 8 pixels wide or more, where the cases of width barely apply.
        //-----------------------------
        template <class O, bool kSkip>
        using UsesWidths = std::integral_constant<bool, O::kWidths && !kSkip>;

        // The bands of a rotated glyph at least 4 pixels wide and tall. kWidth says which widths it can have.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, EWidth kWidth>
        ALWAYS_INLINE void
        DrawBandsOf(const Color &color, const uint8_t *mask, size_t stepX, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride) {
            width = HideWidth<O>(width);
            int32_t y = 0;
            for (; y + 4 <= height; y += 4) {
                DrawRowOf<O>(RowWidthTag<O, kWidth>(), Alpha8Band<O, kOpaque, kSkip, false> { color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, 0 }, width);
            }
            if (y < height) {
                const int32_t top = height - 4;
                DrawRowOf<O>(RowWidthTag<O, kWidth>(), Alpha8Band<O, kOpaque, kSkip, true> { color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, y - top }, width);
            }
        }

        // The bands of a rotated glyph, with any width, or, with the first tag, with the code of their width.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawBands(std::false_type, const Color &color, const uint8_t *mask, size_t stepX, int32_t width, int32_t height,
                  uint32_t *dst, uint32_t dstStride) {
            DrawBandsOf<O, kOpaque, kSkip, EWidth::Any>(color, mask, stepX, width, height, dst, dstStride);
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
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

        // A rotated glyph is stored transposed. When the texels of a column are contiguous, it is drawn 4 rows at a time.
        // The 1 to 3 rows left are the end of a last band of 4, which overlaps the one above. It is a separate function,
        // one for each kSkip, so that the glyph function does not save the registers it uses for every glyph. As with
        // kWidths, only glyphs without shortcuts pick the code of their width.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        NO_INLINE void
        DrawAlpha8Rotated(const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                          uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const Color color = MakeColor(premultiplied, alpha);
            if (O::kBands && stepY == 1 && width >= 4 && height >= 4) {
                using BandWidths = std::integral_constant<bool, O::kBandWidths && !kSkip>;
                DrawBands<O, kOpaque, kSkip>(BandWidths(), color, mask, stepX, width, height, dst, dstStride);
            }
            else {
                for (int32_t y = 0; y < height; ++y) {
                    DrawLine<O>(Alpha8Row<O, kOpaque, kSkip, true> { color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride] }, width);
                }
            }
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        NO_INLINE void
        DrawAlpha8Upright(const uint8_t *mask, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride,
                          uint32_t premultiplied, uint32_t alpha) {
            DrawAlpha8Rows<O, kOpaque, kSkip>(UsesWidths<O, kSkip>(), MakeColor(premultiplied, alpha), mask, stepY, width, height, dst, dstStride);
        }

        // GlyphDraw.cpp explains why the glyph functions are never inlined. With kUprightApart, this one only picks the
        // function that draws the glyph. GCC saves the SSE registers that a function uses when it starts, so otherwise a
        // rotated glyph saves them here and again in DrawAlpha8Rotated.
        //-----------------------------
        template <class O, bool kOpaque>
        NO_INLINE void
        DrawAlpha8Glyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                        uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const uint8_t *mask = &texture[offset];
            if (stepX != 1) {
                if (UsesShortcuts<O, kOpaque>(width)) {
                    DrawAlpha8Rotated<O, kOpaque, true>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
                }
                else {
                    DrawAlpha8Rotated<O, kOpaque, false>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
                }
                return;
            }

            if (O::kUprightApart) {
                if (UsesShortcuts<O, kOpaque>(width)) {
                    DrawAlpha8Upright<O, kOpaque, true>(mask, stepY, width, height, dst, dstStride, premultiplied, alpha);
                }
                else {
                    DrawAlpha8Upright<O, kOpaque, false>(mask, stepY, width, height, dst, dstStride, premultiplied, alpha);
                }
                return;
            }

            const Color color = MakeColor(premultiplied, alpha);
            if (UsesShortcuts<O, kOpaque>(width)) {
                DrawAlpha8Rows<O, kOpaque, true>(UsesWidths<O, true>(), color, mask, stepY, width, height, dst, dstStride);
            }
            else {
                DrawAlpha8Rows<O, kOpaque, false>(UsesWidths<O, false>(), color, mask, stepY, width, height, dst, dstStride);
            }
        }

        // A rotated glyph is drawn one pixel at a time: no scenario measures it.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        ALWAYS_INLINE void
        DrawBgraRows(const Color &color, const uint8_t *src, size_t stepX, size_t stepY, int32_t width, int32_t height,
                     uint32_t *dst, uint32_t dstStride) {
            if (stepX != 1) {
                for (int32_t y = 0; y < height; ++y) {
                    uint32_t *row = &dst[size_t(y) * dstStride];
                    for (int32_t x = 0; x < width; ++x) {
                        DrawTexelPixel<O::kSplit, kOpaque>(color, &src[(size_t(y) * stepY + size_t(x) * stepX) * 4], &row[x]);
                    }
                }
                return;
            }

            for (int32_t y = 0; y < height; ++y) {
                DrawLine<O>(BgraRow<O, kOpaque, kSkip> { color, &src[size_t(y) * stepY * 4], &dst[size_t(y) * dstStride] }, width);
            }
        }

        //-----------------------------
        template <class O, bool kOpaque>
        NO_INLINE void
        DrawBgraGlyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                      uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const Color   color = MakeColor(premultiplied, alpha);
            const uint8_t *src  = &texture[offset * 4];
            if (UsesShortcuts<O, kOpaque>(width)) {
                DrawBgraRows<O, kOpaque, true>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
            else {
                DrawBgraRows<O, kOpaque, false>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
        }

#undef NO_INLINE
#undef ALWAYS_INLINE

        //-----------------------------
        template <class O>
        DrawGlyphFunction
        GetDrawGlyphFunction(bool alpha8, bool opaque) {
            if (alpha8) {
                return opaque ? DrawAlpha8Glyph<O, true> : DrawAlpha8Glyph<O, false>;
            }

            return opaque ? DrawBgraGlyph<O, true> : DrawBgraGlyph<O, false>;
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
    DrawPixels(Scenario &scenario, uint32_t *dst) {
        DrawQuads<PixelsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawLate(Scenario &scenario, uint32_t *dst) {
        DrawQuads<LateOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawPairTail(Scenario &scenario, uint32_t *dst) {
        DrawQuads<PairTailOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawNoNarrowRows(Scenario &scenario, uint32_t *dst) {
        DrawQuads<NoNarrowRowsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawNoBands(Scenario &scenario, uint32_t *dst) {
        DrawQuads<NoBandsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawRowShortcuts(Scenario &scenario, uint32_t *dst) {
        DrawQuads<RowShortcutsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawPixman(Scenario &scenario, uint32_t *dst) {
        DrawQuads<PixmanOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawUprightApart(Scenario &scenario, uint32_t *dst) {
        DrawQuads<UprightApartOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawOctets(Scenario &scenario, uint32_t *dst) {
        DrawQuads<OctetsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBgraTint(Scenario &scenario, uint32_t *dst) {
        DrawQuads<BgraTintOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawSkip(Scenario &scenario, uint32_t *dst) {
        DrawQuads<SkipOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBranchless(Scenario &scenario, uint32_t *dst) {
        DrawQuads<BranchlessOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawOpaque8(Scenario &scenario, uint32_t *dst) {
        DrawQuads<Opaque8Options>(scenario, dst);
    }

    //---------------------------------
    void
    DrawOpaque24(Scenario &scenario, uint32_t *dst) {
        DrawQuads<Opaque24Options>(scenario, dst);
    }

    //---------------------------------
    void
    DrawTranslucent0(Scenario &scenario, uint32_t *dst) {
        DrawQuads<Translucent0Options>(scenario, dst);
    }

    //---------------------------------
    void
    DrawTranslucent16(Scenario &scenario, uint32_t *dst) {
        DrawQuads<Translucent16Options>(scenario, dst);
    }

    //---------------------------------
    void
    DrawWidths(Scenario &scenario, uint32_t *dst) {
        DrawQuads<WidthsOptions>(scenario, dst);
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

    //---------------------------------
    void
    DrawWidthIfs(Scenario &scenario, uint32_t *dst) {
        DrawQuads<WidthIfsOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawWidthIfsHidden(Scenario &scenario, uint32_t *dst) {
        DrawQuads<WidthIfsHiddenOptions>(scenario, dst);
    }

} // end of namespace Sse2Overlap
} // end of namespace Benchmark

#endif
