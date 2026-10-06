#include "benchmarkCommon.h"

#if defined(FONTRENDERER_SSE2)

#include <emmintrin.h>
//-------------------------------------
#include <cstring>

// The SSE2 code of the library (src/GlyphDrawSse2.cpp) with other ways to write Spread, which copies each coverage byte
// to the 2 lanes of 16 bits of its pixel. The same pixels as DrawText.
//
// Clang merges the two unpacks of the library into one byte shuffle and, with SSE2 alone, builds that shuffle with 8
// instructions instead of 2. Clang 19 does it after _mm_cvtsi32_si128, which leaves the upper 12 bytes at 0. The
// bands, whose 16 bytes are all coverage, come out fine. The other forms reach the same value through a shift or an OR,
// and clang then builds it with the 2 unpacks. GCC and MSVC build each form as it is written.

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
namespace Sse2Spread {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                           uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

        // How Spread copies each coverage byte to the 2 lanes of 16 bits of its pixel.
        enum class ESpread {
            Unpack,     // The bytes widened to 16 bits, then each lane doubled: the form of the library
            Shift,      // Each byte doubled, then each pair of bytes doubled, then the high byte of each lane shifted out
            Or,         // The bytes widened to 32 bits, then each lane ORed with itself shifted up by 16 bits
        };

        //-----------------------------
        struct UnpackOptions {
            static constexpr ESpread kSpread = ESpread::Unpack;
        };

        //-----------------------------
        struct ShiftOptions {
            static constexpr ESpread kSpread = ESpread::Shift;
        };

        //-----------------------------
        struct OrOptions {
            static constexpr ESpread kSpread = ESpread::Or;
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

        //-----------------------------
        struct Color {
            __m128i     solid;      // The premultiplied color for 4 pixels
            __m128i     even;       // Its blue and red for 4 pixels, each in a 16-bit lane
            __m128i     odd;        // Its green and alpha for 4 pixels
            __m128i     alpha;      // The alpha of the color in every lane
        };

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE Color
        MakeColor(uint32_t premultiplied, uint32_t alpha) {
            Color color;
            color.solid = _mm_set1_epi32(int(premultiplied));
            color.even  = _mm_and_si128(color.solid, _mm_set1_epi16(0x00ff));
            color.odd   = _mm_srli_epi16(color.solid, 8);
            color.alpha = _mm_set1_epi16(int16_t(alpha));
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
        //-----------------------------
        template <class O>
        FONTRENDERER_ALWAYS_INLINE __m128i
        Spread(__m128i x) {
            if (O::kSpread == ESpread::Shift) {
                const __m128i twice = _mm_unpacklo_epi8(x, x);
                return _mm_srli_epi16(_mm_unpacklo_epi16(twice, twice), 8);
            }
            if (O::kSpread == ESpread::Or) {
                const __m128i dwords = _mm_unpacklo_epi16(_mm_unpacklo_epi8(x, _mm_setzero_si128()), _mm_setzero_si128());
                return _mm_or_si128(dwords, _mm_slli_epi32(dwords, 16));
            }

            const __m128i words = _mm_unpacklo_epi8(x, _mm_setzero_si128());
            return _mm_unpacklo_epi16(words, words);
        }

        // Spread for each row of a band, whose coverage has a row in each 32-bit lane.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        SpreadBand(__m128i band, __m128i &row0, __m128i &row1, __m128i &row2, __m128i &row3) {
            const __m128i low  = _mm_unpacklo_epi8(band, _mm_setzero_si128());
            const __m128i high = _mm_unpackhi_epi8(band, _mm_setzero_si128());
            row0 = _mm_unpacklo_epi16(low, low);
            row1 = _mm_unpackhi_epi16(low, low);
            row2 = _mm_unpacklo_epi16(high, high);
            row3 = _mm_unpackhi_epi16(high, high);
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

        // The alpha of each of the 4 pixels of odd, which has their green and alpha, over the 2 lanes of its pixel.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE __m128i
        ExpandAlphaOdd(__m128i odd) {
            return _mm_shufflehi_epi16(_mm_shufflelo_epi16(odd, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
        }

        // 4 premultiplied texels times the premultiplied color, blended with 4 pixels. The alpha of the blend is the alpha of
        // the texel under an opaque text, and the alpha of the texel times the color, rounded, under a translucent one.
        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_ALWAYS_INLINE __m128i
        BlendTexels(const Color &color, __m128i texels, __m128i pixels) {
            const __m128i odd       = OddBytes(texels);
            const __m128i sourceOdd = _mm_mullo_epi16(odd, color.odd);
            const __m128i alpha     = ExpandAlphaOdd(kOpaque ? odd : Div255(sourceOdd));
            const __m128i even      = Blend(_mm_mullo_epi16(EvenBytes(texels), color.even), alpha, EvenBytes(pixels));
            return JoinBytes(even, Blend(sourceOdd, alpha, OddBytes(pixels)));
        }

        // The alpha of the 4 texels is 0.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE bool
        IsTransparent(__m128i texels) {
            return (_mm_movemask_epi8(_mm_cmpeq_epi8(texels, _mm_setzero_si128())) & 0x8888) == 0x8888;
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

        // 4 pixels at p with their coverage, spread for the blend. none and full say that the 4 coverages are 0 or 255. With
        // kSkip, such a block is left as it is, or gets the color under an opaque text. With kRead, pixels are the pixels at
        // p, read before the row wrote anything. Without it, they are read here, after the shortcuts.
        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        DrawCoverage(const Color &color, __m128i spread, bool none, bool full, uint32_t *p, __m128i pixels) {
            if (kSkip && none) {
                return;
            }
            if (kSkip && kOpaque && full) {
                StoreQuad(p, color.solid);
                return;
            }

            StoreQuad(p, BlendCoverage<kOpaque>(color, spread, kRead ? pixels : LoadQuad(p)));
        }

        // One texel over one pixel. A transparent texel leaves the pixel as it is.
        //-----------------------------
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

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip, bool kRotated>
        struct Alpha8Row {
            const Color     &color;
            const uint8_t   *mask;      // The coverage of the first pixel of the row
            size_t          step;       // Between two pixels of the row
            uint32_t        *dst;

            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, __m128i pixels) const {
                const uint32_t m = LoadCoverage<kRotated>(&mask[x * step], step);
                DrawCoverage<kOpaque, kSkip, kRead>(color, Spread<O>(_mm_cvtsi32_si128(int(m))), m == 0, m == 0xffffffffu, &dst[x], pixels);
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
                    StoreOne(dst, BlendCoverage<kOpaque>(color, Spread<O>(_mm_cvtsi32_si128(int(mask[0]))), LoadOne(dst)));
                }
                else if (w == 2) {
                    StorePair(dst, BlendCoverage<kOpaque>(color, Spread<O>(_mm_cvtsi32_si128(int(LoadCoveragePair<kRotated>(mask, step)))), LoadPair(dst)));
                }
                else {
                    const uint32_t m      = LoadCoveragePair<kRotated>(mask, step) | (LoadCoveragePair<kRotated>(&mask[step], step) << 16);
                    const __m128i  pixels = _mm_unpacklo_epi64(LoadPair(dst), LoadPair(&dst[1]));
                    const __m128i  result = BlendCoverage<kOpaque>(color, Spread<O>(_mm_cvtsi32_si128(int(m))), pixels);
                    StoreHighPair(&dst[1], result);
                    StorePair(dst, result);
                }
            }
        };

        // The rows are written out because a loop over them, with a variable index, can make a compiler keep the coverage
        // and the rows in memory.
        //-----------------------------
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
                    if (_mm_movemask_epi8(_mm_cmpeq_epi8(band, _mm_setzero_si128())) == 0xffff) {
                        return;
                    }
                    if (kOpaque && _mm_movemask_epi8(_mm_cmpeq_epi8(band, _mm_set1_epi8(-1))) == 0xffff) {
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
                SpreadBand(band, spread0, spread1, spread2, spread3);
                if (Draws(0)) {
                    DrawCoverage<kOpaque, false, kRead>(color, spread0, false, false, At(0, x), pixels.row0);
                }
                if (Draws(1)) {
                    DrawCoverage<kOpaque, false, kRead>(color, spread1, false, false, At(1, x), pixels.row1);
                }
                if (Draws(2)) {
                    DrawCoverage<kOpaque, false, kRead>(color, spread2, false, false, At(2, x), pixels.row2);
                }
                DrawCoverage<kOpaque, false, kRead>(color, spread3, false, false, At(3, x), pixels.row3);
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
        template <bool kOpaque, bool kSkip>
        struct BgraRow {
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
                if (kSkip && IsTransparent(texels)) {
                    return;
                }

                StoreQuad(&dst[x], BlendTexels<kOpaque>(color, texels, kRead ? pixels : LoadQuad(&dst[x])));
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

        // A row of at least 4 pixels, or a band of 4 such rows. The last block is read before the row writes anything, and
        // the blocks before it are drawn as they come, so a row needs no branch on how many pixels are left.
        //-----------------------------
        template <class TLine>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBlocks(const TLine &line, int32_t w) {
            const size_t tail   = size_t(w) - 4;
            const auto   pixels = line.ReadQuad(tail);
            for (size_t x = 0; x < tail; x += 4) {
                line.Quad(x);
            }
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

        // A rotated glyph has a function of its own, one for each kSkip, so that the glyph function does not save the
        // registers it uses for every glyph. Its rows are drawn 4 at a time when the texels of a column are contiguous.
        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
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
                    DrawRow(Alpha8Row<O, kOpaque, kSkip, true> { color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride] }, width);
                }
            }
        }

        //-----------------------------
        template <class O, bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawAlpha8Rows(const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride) {
            for (int32_t y = 0; y < height; ++y) {
                DrawRow(Alpha8Row<O, kOpaque, kSkip, false> { color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride] }, width);
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

            const Color color = MakeColor(premultiplied, alpha);
            if (UsesShortcuts<kOpaque>(width)) {
                DrawAlpha8Rows<O, kOpaque, true>(color, mask, stepY, width, height, dst, dstStride);
            }
            else {
                DrawAlpha8Rows<O, kOpaque, false>(color, mask, stepY, width, height, dst, dstStride);
            }
        }

        // A rotated glyph is drawn one pixel at a time: none of the measured texts with a color texture had one.
        //-----------------------------
        template <bool kOpaque, bool kSkip>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBgraRows(const Color &color, const uint8_t *src, size_t stepX, size_t stepY, int32_t width, int32_t height,
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
                DrawRow(BgraRow<kOpaque, kSkip> { color, &src[size_t(y) * stepY * 4], &dst[size_t(y) * dstStride] }, width);
            }
        }

        // BGRA32 does not use Spread, so every option set shares these.
        //-----------------------------
        template <bool kOpaque>
        FONTRENDERER_NO_INLINE void
        DrawBgra(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                 uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const Color   color = MakeColor(premultiplied, alpha);
            const uint8_t *src  = &texture[offset * 4];
            if (UsesShortcuts<kOpaque>(width)) {
                DrawBgraRows<kOpaque, true>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
            else {
                DrawBgraRows<kOpaque, false>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
        }

        //-----------------------------
        template <class O>
        DrawGlyphFunction
        GetDrawGlyphFunction(bool alpha8, bool opaque) {
            if (alpha8) {
                return opaque ? DrawAlpha8<O, true> : DrawAlpha8<O, false>;
            }

            return opaque ? DrawBgra<true> : DrawBgra<false>;
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
    DrawUnpack(Scenario &scenario, uint32_t *dst) {
        DrawQuads<UnpackOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawShift(Scenario &scenario, uint32_t *dst) {
        DrawQuads<ShiftOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawOr(Scenario &scenario, uint32_t *dst) {
        DrawQuads<OrOptions>(scenario, dst);
    }

} // end of namespace Sse2Spread
} // end of namespace Benchmark

#endif
