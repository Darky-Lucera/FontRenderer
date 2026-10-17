#include "benchmarkCommon.h"

#if defined(__wasm_simd128__)

#include <cstring>
#include <type_traits>
#include <wasm_simd128.h>

// The SSE2 code of the library (GlyphDrawSSE2.cpp) written with the intrinsics of WebAssembly SIMD. Draw does what
// GlyphDrawWasm.cpp does. The same pixels as DrawText.
//
// The blocks, the bands and the shortcuts are those of GlyphDrawSSE2.cpp, which explains them. Built with -msse2,
// Emscripten translates each SSE2 intrinsic into WebAssembly, and three of them come out worse than they need to:
// - _mm_mulhi_epu16 has no WebAssembly instruction. Emscripten widens both registers to 32 bits, multiplies twice and
//   shuffles: 7 instructions, with i32x4.mul, which x86 runs as the slow pmulld. Div255 here is the formula of the scalar
//   code, with additions and shifts.
// - _mm_movemask_epi8 becomes i8x16.bitmask. x86 has it, but ARM does not, and the browser builds it from several
//   instructions there. The tests of the shortcuts here use v128.any_true, which both have.
// - _mm_shufflelo_epi16 and _mm_shufflehi_epi16 become two shuffles. ExpandAlphaOdd here is one.
//
// DrawMulhi and DrawBitmask undo the first and the second change, to tell what each one gives.

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
namespace Wasm {

    //---------------------------------
    namespace {

        // The choices of Draw. DrawMulhi and DrawBitmask each change one of them.
        //-----------------------------
        struct DrawOptions {
            static constexpr bool   kMulhi   = false;   // Div255 as Emscripten translates _mm_mulhi_epu16
            static constexpr bool   kBitmask = false;   // The shortcuts tested as Emscripten translates _mm_movemask_epi8
        };

        //-----------------------------
        struct MulhiOptions : DrawOptions {
            static constexpr bool   kMulhi   = true;
        };

        //-----------------------------
        struct BitmaskOptions : DrawOptions {
            static constexpr bool   kBitmask = true;
        };

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
            v128_t      solid;      // The premultiplied color for 4 pixels
            v128_t      even;       // Its blue and red for 4 pixels, each in a 16-bit lane
            v128_t      odd;        // Its green and alpha for 4 pixels
            v128_t      alpha;      // The alpha of the color in every lane
        };

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE Color
        MakeColor(uint32_t premultiplied, uint32_t alpha) {
            Color color;
            color.solid = wasm_i32x4_splat(int32_t(premultiplied));
            color.even  = wasm_v128_and(color.solid, wasm_i16x8_const_splat(0x00ff));
            color.odd   = wasm_u16x8_shr(color.solid, 8);
            color.alpha = wasm_i16x8_splat(int16_t(alpha));
            return color;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE uint32_t
        Load32(const uint8_t *p) {
            uint32_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE uint32_t
        Load16(const uint8_t *p) {
            uint16_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        LoadQuad(const void *p) {
            return wasm_v128_load(p);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StoreQuad(void *p, v128_t x) {
            wasm_v128_store(p, x);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        LoadPair(const void *p) {
            return wasm_v128_load64_zero(p);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StorePair(void *p, v128_t x) {
            wasm_v128_store64_lane(p, x, 0);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StoreHighPair(void *p, v128_t x) {
            wasm_v128_store64_lane(p, x, 1);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        LoadOne(const void *p) {
            return wasm_v128_load32_zero(p);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        StoreOne(void *p, v128_t x) {
            wasm_v128_store32_lane(p, x, 0);
        }

        // A number in the low 32 bits of a register, and 0 in the rest.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        FromNumber(uint32_t value) {
            return wasm_i32x4_make(int32_t(value), 0, 0, 0);
        }

        // Each 16-bit lane divided by 255 and rounded. Exact up to 65152, the largest sum of a blend.
        //-----------------------------
        template <class TOptions>
        FONTRENDERER_ALWAYS_INLINE v128_t
        Div255(v128_t x) {
            const v128_t t = wasm_i16x8_add(x, wasm_i16x8_const_splat(0x0080));
            if (TOptions::kMulhi) {
                // (t * 257) >> 16, as Emscripten builds _mm_mulhi_epu16.
                const v128_t factor = wasm_i16x8_const_splat(0x0101);
                const v128_t low    = wasm_u32x4_extmul_low_u16x8(t, factor);
                const v128_t high   = wasm_u32x4_extmul_high_u16x8(t, factor);
                return wasm_i16x8_shuffle(low, high, 1, 3, 5, 7, 9, 11, 13, 15);
            }

            return wasm_u16x8_shr(wasm_i16x8_add(t, wasm_u16x8_shr(t, 8)), 8);
        }

        // Rounded (source + pixels * (255 - alpha)) / 255 in each 16-bit lane, with the source already multiplied.
        //-----------------------------
        template <class TOptions>
        FONTRENDERER_ALWAYS_INLINE v128_t
        Blend(v128_t source, v128_t alpha, v128_t pixels) {
            const v128_t inverse = wasm_v128_xor(alpha, wasm_i16x8_const_splat(0x00ff));
            return Div255<TOptions>(wasm_i16x8_add(source, wasm_i16x8_mul(pixels, inverse)));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        EvenBytes(v128_t x) {
            return wasm_v128_and(x, wasm_i16x8_const_splat(0x00ff));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        OddBytes(v128_t x) {
            return wasm_u16x8_shr(x, 8);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        JoinBytes(v128_t even, v128_t odd) {
            return wasm_v128_or(even, wasm_i16x8_shl(odd, 8));
        }

        // The 4 coverage bytes in the low 32 bits of x, each copied to the 2 lanes of 16 bits of its pixel.
        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        Spread(v128_t x) {
            const v128_t twice = wasm_i8x16_shuffle(x, x, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
            return wasm_u16x8_shr(wasm_i16x8_shuffle(twice, twice, 0, 8, 1, 9, 2, 10, 3, 11), 8);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        SpreadBand(v128_t band, v128_t &row0, v128_t &row1, v128_t &row2, v128_t &row3) {
            const v128_t zero = wasm_i64x2_const_splat(0);
            const v128_t low  = wasm_i8x16_shuffle(band, zero, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
            const v128_t high = wasm_i8x16_shuffle(band, zero, 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31);
            row0 = wasm_i16x8_shuffle(low,  low,  0, 8, 1, 9, 2, 10, 3, 11);
            row1 = wasm_i16x8_shuffle(low,  low,  4, 12, 5, 13, 6, 14, 7, 15);
            row2 = wasm_i16x8_shuffle(high, high, 0, 8, 1, 9, 2, 10, 3, 11);
            row3 = wasm_i16x8_shuffle(high, high, 4, 12, 5, 13, 6, 14, 7, 15);
        }

        //-----------------------------
        template <bool kOpaque, class TOptions>
        FONTRENDERER_ALWAYS_INLINE v128_t
        BlendCoverage(const Color &color, v128_t spread, v128_t pixels) {
            const v128_t alpha = kOpaque ? spread : Div255<TOptions>(wasm_i16x8_mul(spread, color.alpha));
            const v128_t even  = Blend<TOptions>(wasm_i16x8_mul(color.even, spread), alpha, EvenBytes(pixels));
            const v128_t odd   = Blend<TOptions>(wasm_i16x8_mul(color.odd,  spread), alpha, OddBytes(pixels));
            return JoinBytes(even, odd);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        ExpandAlphaOdd(v128_t odd) {
            return wasm_i8x16_shuffle(odd, odd, 2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15);
        }

        //-----------------------------
        template <bool kOpaque, class TOptions>
        FONTRENDERER_ALWAYS_INLINE v128_t
        BlendTexels(const Color &color, v128_t texels, v128_t pixels) {
            const v128_t odd       = OddBytes(texels);
            const v128_t sourceOdd = wasm_i16x8_mul(odd, color.odd);
            const v128_t alpha     = ExpandAlphaOdd(kOpaque ? odd : Div255<TOptions>(sourceOdd));
            const v128_t even      = Blend<TOptions>(wasm_i16x8_mul(EvenBytes(texels), color.even), alpha, EvenBytes(pixels));
            return JoinBytes(even, Blend<TOptions>(sourceOdd, alpha, OddBytes(pixels)));
        }

        //-----------------------------
        template <class TOptions>
        FONTRENDERER_ALWAYS_INLINE bool
        IsTransparent(v128_t texels) {
            if (TOptions::kBitmask) {
                return (wasm_i8x16_bitmask(wasm_i8x16_eq(texels, wasm_i64x2_const_splat(0))) & 0x8888) == 0x8888;
            }

            return wasm_v128_any_true(wasm_v128_and(texels, wasm_i32x4_const_splat(int32_t(0xff000000u)))) == false;
        }

        //-----------------------------
        template <class TOptions>
        FONTRENDERER_ALWAYS_INLINE bool
        IsEmptyBand(v128_t band) {
            if (TOptions::kBitmask) {
                return wasm_i8x16_bitmask(wasm_i8x16_eq(band, wasm_i64x2_const_splat(0))) == 0xffff;
            }

            return wasm_v128_any_true(band) == false;
        }

        //-----------------------------
        template <class TOptions>
        FONTRENDERER_ALWAYS_INLINE bool
        IsFullBand(v128_t band) {
            if (TOptions::kBitmask) {
                return wasm_i8x16_bitmask(wasm_i8x16_eq(band, wasm_i8x16_const_splat(-1))) == 0xffff;
            }

            return wasm_v128_any_true(wasm_v128_not(band)) == false;
        }

        //-----------------------------
        template <bool kRotated>
        FONTRENDERER_ALWAYS_INLINE uint32_t
        LoadCoverage(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8) | (uint32_t(mask[2 * step]) << 16) | (uint32_t(mask[3 * step]) << 24);
            }

            return Load32(mask);
        }

        //-----------------------------
        template <bool kRotated>
        FONTRENDERER_ALWAYS_INLINE uint32_t
        LoadCoveragePair(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8);
            }

            return Load16(mask);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        LoadBand(const uint8_t *column0, const uint8_t *column1, const uint8_t *column2, const uint8_t *column3) {
            const v128_t first  = wasm_i8x16_shuffle(LoadOne(column0), LoadOne(column1), 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
            const v128_t second = wasm_i8x16_shuffle(LoadOne(column2), LoadOne(column3), 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
            return wasm_i16x8_shuffle(first, second, 0, 8, 1, 9, 2, 10, 3, 11);
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kRead, class TOptions>
        FONTRENDERER_ALWAYS_INLINE void
        DrawCoverage(const Color &color, v128_t spread, bool none, bool full, uint32_t *p, v128_t pixels) {
            if (kSkip && none) {
                return;
            }
            if (kSkip && kOpaque && full) {
                StoreQuad(p, color.solid);
                return;
            }

            StoreQuad(p, BlendCoverage<kOpaque, TOptions>(color, spread, kRead ? pixels : LoadQuad(p)));
        }

        //-----------------------------
        template <bool kOpaque, class TOptions>
        FONTRENDERER_ALWAYS_INLINE void
        DrawTexel(const Color &color, const uint8_t *texel, uint32_t *p) {
            const uint32_t t = Load32(texel);
            if ((t >> 24) != 0) {
                StoreOne(p, BlendTexels<kOpaque, TOptions>(color, FromNumber(t), LoadOne(p)));
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kRotated, class TOptions>
        struct Alpha8Row {
            const Color     &color;
            const uint8_t   *mask;
            size_t          step;
            uint32_t        *dst;

            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, v128_t pixels) const {
                const uint32_t m = LoadCoverage<kRotated>(&mask[x * step], step);
                DrawCoverage<kOpaque, kSkip, kRead, TOptions>(color, Spread(FromNumber(m)), m == 0, m == 0xffffffffu, &dst[x], pixels);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Quad(size_t x) const {
                Draw<false>(x, wasm_i64x2_const_splat(0));
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE v128_t
            ReadQuad(size_t x) const {
                return LoadQuad(&dst[x]);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            QuadRead(size_t x, v128_t pixels) const {
                Draw<true>(x, pixels);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Narrow(int32_t w) const {
                if (w == 1) {
                    StoreOne(dst, BlendCoverage<kOpaque, TOptions>(color, Spread(FromNumber(mask[0])), LoadOne(dst)));
                }
                else if (w == 2) {
                    StorePair(dst, BlendCoverage<kOpaque, TOptions>(color, Spread(FromNumber(LoadCoveragePair<kRotated>(mask, step))), LoadPair(dst)));
                }
                else {
                    const uint32_t m      = LoadCoveragePair<kRotated>(mask, step) | (LoadCoveragePair<kRotated>(&mask[step], step) << 16);
                    const v128_t   pixels = wasm_i64x2_shuffle(LoadPair(dst), LoadPair(&dst[1]), 0, 2);
                    const v128_t   result = BlendCoverage<kOpaque, TOptions>(color, Spread(FromNumber(m)), pixels);
                    StoreHighPair(&dst[1], result);
                    StorePair(dst, result);
                }
            }
        };

        //-----------------------------
        template <bool kOpaque, bool kSkip, bool kPartial, class TOptions>
        struct Alpha8Band {
            const Color     &color;
            const uint8_t   *mask;
            size_t          step;
            uint32_t        *dst;
            size_t          dstStride;
            int32_t         firstRow;

            struct Rows {
                v128_t      row0;
                v128_t      row1;
                v128_t      row2;
                v128_t      row3;
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
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, const Rows &pixels) const {
                const uint8_t *column = &mask[x * step];
                const v128_t  band    = LoadBand(column, &column[step], &column[2 * step], &column[3 * step]);
                if (kSkip) {
                    if (IsEmptyBand<TOptions>(band)) {
                        return;
                    }
                    if (kOpaque && IsFullBand<TOptions>(band)) {
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

                v128_t spread0, spread1, spread2, spread3;
                SpreadBand(band, spread0, spread1, spread2, spread3);
                if (Draws(0)) {
                    DrawCoverage<kOpaque, false, kRead, TOptions>(color, spread0, false, false, At(0, x), pixels.row0);
                }
                if (Draws(1)) {
                    DrawCoverage<kOpaque, false, kRead, TOptions>(color, spread1, false, false, At(1, x), pixels.row1);
                }
                if (Draws(2)) {
                    DrawCoverage<kOpaque, false, kRead, TOptions>(color, spread2, false, false, At(2, x), pixels.row2);
                }
                DrawCoverage<kOpaque, false, kRead, TOptions>(color, spread3, false, false, At(3, x), pixels.row3);
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
        template <bool kOpaque, bool kSkip, class TOptions>
        struct BGRARow {
            const Color     &color;
            const uint8_t   *src;
            uint32_t        *dst;

            //-------------------------
            template <bool kRead>
            FONTRENDERER_ALWAYS_INLINE void
            Draw(size_t x, v128_t pixels) const {
                const v128_t texels = LoadQuad(&src[4 * x]);
                if (kSkip && IsTransparent<TOptions>(texels)) {
                    return;
                }

                StoreQuad(&dst[x], BlendTexels<kOpaque, TOptions>(color, texels, kRead ? pixels : LoadQuad(&dst[x])));
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Quad(size_t x) const {
                Draw<false>(x, wasm_i64x2_const_splat(0));
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE v128_t
            ReadQuad(size_t x) const {
                return LoadQuad(&dst[x]);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            QuadRead(size_t x, v128_t pixels) const {
                Draw<true>(x, pixels);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE void
            Narrow(int32_t w) const {
                if (w == 1) {
                    StoreOne(dst, BlendTexels<kOpaque, TOptions>(color, LoadOne(src), LoadOne(dst)));
                }
                else if (w == 2) {
                    StorePair(dst, BlendTexels<kOpaque, TOptions>(color, LoadPair(src), LoadPair(dst)));
                }
                else {
                    const v128_t texels = wasm_i64x2_shuffle(LoadPair(src), LoadPair(&src[4]), 0, 2);
                    const v128_t pixels = wasm_i64x2_shuffle(LoadPair(dst), LoadPair(&dst[1]), 0, 2);
                    const v128_t result = BlendTexels<kOpaque, TOptions>(color, texels, pixels);
                    StoreHighPair(&dst[1], result);
                    StorePair(dst, result);
                }
            }
        };

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

        //-----------------------------
        template <bool kOpaque, bool kSkip, class TOptions>
        FONTRENDERER_NO_INLINE void
        DrawAlpha8Rotated(const uint8_t *mask, size_t stepX, size_t stepY, int32_t width, int32_t height,
                          uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const Color color = MakeColor(premultiplied, alpha);
            if (stepY == 1 && width >= 4 && height >= 4) {
                int32_t y = 0;
                for (; y + 4 <= height; y += 4) {
                    DrawBlocks(Alpha8Band<kOpaque, kSkip, false, TOptions> { color, &mask[size_t(y)], stepX, &dst[size_t(y) * dstStride], dstStride, 0 }, width);
                }
                if (y < height) {
                    const int32_t top = height - 4;
                    DrawBlocks(Alpha8Band<kOpaque, kSkip, true, TOptions> { color, &mask[size_t(top)], stepX, &dst[size_t(top) * dstStride], dstStride, y - top }, width);
                }
            }
            else {
                for (int32_t y = 0; y < height; ++y) {
                    DrawRow(Alpha8Row<kOpaque, kSkip, true, TOptions> { color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride] }, width);
                }
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, class TOptions>
        FONTRENDERER_ALWAYS_INLINE void
        DrawAlpha8Rows(const Color &color, const uint8_t *mask, size_t stepY, int32_t width, int32_t height, uint32_t *dst, uint32_t dstStride) {
            for (int32_t y = 0; y < height; ++y) {
                DrawRow(Alpha8Row<kOpaque, kSkip, false, TOptions> { color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride] }, width);
            }
        }

        //-----------------------------
        template <bool kOpaque, class TOptions>
        FONTRENDERER_NO_INLINE void
        DrawAlpha8(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                   uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const uint8_t *mask = &texture[offset];
            if (stepX != 1) {
                if (UsesShortcuts<kOpaque>(width)) {
                    DrawAlpha8Rotated<kOpaque, true, TOptions>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
                }
                else {
                    DrawAlpha8Rotated<kOpaque, false, TOptions>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
                }
                return;
            }

            const Color color = MakeColor(premultiplied, alpha);
            if (UsesShortcuts<kOpaque>(width)) {
                DrawAlpha8Rows<kOpaque, true, TOptions>(color, mask, stepY, width, height, dst, dstStride);
            }
            else {
                DrawAlpha8Rows<kOpaque, false, TOptions>(color, mask, stepY, width, height, dst, dstStride);
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kSkip, class TOptions>
        FONTRENDERER_ALWAYS_INLINE void
        DrawBGRARows(const Color &color, const uint8_t *src, size_t stepX, size_t stepY, int32_t width, int32_t height,
                     uint32_t *dst, uint32_t dstStride) {
            if (stepX != 1) {
                for (int32_t y = 0; y < height; ++y) {
                    uint32_t *row = &dst[size_t(y) * dstStride];
                    for (int32_t x = 0; x < width; ++x) {
                        DrawTexel<kOpaque, TOptions>(color, &src[(size_t(y) * stepY + size_t(x) * stepX) * 4], &row[x]);
                    }
                }
                return;
            }

            for (int32_t y = 0; y < height; ++y) {
                DrawRow(BGRARow<kOpaque, kSkip, TOptions> { color, &src[size_t(y) * stepY * 4], &dst[size_t(y) * dstStride] }, width);
            }
        }

        //-----------------------------
        template <bool kOpaque, class TOptions>
        FONTRENDERER_NO_INLINE void
        DrawBGRA(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                 uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const uint8_t *src  = &texture[offset * 4];
            const Color   color = MakeColor(premultiplied, alpha);
            if (UsesShortcuts<kOpaque>(width)) {
                DrawBGRARows<kOpaque, true, TOptions>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
            else {
                DrawBGRARows<kOpaque, false, TOptions>(color, src, stepX, stepY, width, height, dst, dstStride);
            }
        }

        // Chosen once and called for each glyph, as Baseline does with the functions of the library.
        //-----------------------------
        template <class TOptions>
        void
        DrawQuads(Scenario &scenario, uint32_t *dst) {
            using DrawGlyph = void (*)(const uint8_t *, size_t, size_t, size_t, int32_t, int32_t, uint32_t *, uint32_t, uint32_t, uint32_t);

            const FontBase  &font         = *scenario.font;
            const uint8_t   *texture      = font.GetTexture();
            const size_t    textureWidth  = font.GetTextureWidth();
            const uint32_t  premultiplied = PremultiplyColor(scenario.color);
            const uint32_t  colorAlpha    = scenario.color >> 24;
            const bool      opaque        = colorAlpha == 255;
            const bool      alpha8        = font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8;

            DrawGlyph drawGlyph;
            if (alpha8) {
                drawGlyph = opaque ? DrawAlpha8<true, TOptions> : DrawAlpha8<false, TOptions>;
            }
            else {
                drawGlyph = opaque ? DrawBGRA<true, TOptions> : DrawBGRA<false, TOptions>;
            }

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
    DrawMulhi(Scenario &scenario, uint32_t *dst) {
        DrawQuads<MulhiOptions>(scenario, dst);
    }

    //---------------------------------
    void
    DrawBitmask(Scenario &scenario, uint32_t *dst) {
        DrawQuads<BitmaskOptions>(scenario, dst);
    }

} // end of namespace Wasm
} // end of namespace Benchmark

#endif
