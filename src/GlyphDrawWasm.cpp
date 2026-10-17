//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

// The functions of GlyphDrawSSE2.cpp written with WebAssembly SIMD, and the same pixels. Emscripten builds them with
// -msimd128. The blocks, the bands and the shortcuts are those of GlyphDrawSSE2.cpp, which explains them.
//
// Emscripten can also build GlyphDrawSSE2.cpp with -msse2, but this code drew between 11 and 100 % faster, in browsers on
// x86 and on ARM, for two reasons:
// - WebAssembly has no _mm_mulhi_epu16, and Emscripten builds it from 7 instructions, with i32x4.mul, which x86 runs as
//   the slow pmulld. Div255 here uses only additions and shifts, as the scalar code does.
// - Emscripten makes _mm_movemask_epi8 an i8x16.bitmask, which ARM builds from several instructions. The shortcuts here
//   are tested with v128.any_true, which both have.
//
// This file has no code of pixman: the two lines that GlyphDrawSSE2.cpp took from it are replaced here. The notice stays
// because this code started from the ideas of the SSE2 code of pixman for text (over_n_8_8888).

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

#if defined(FONTRENDERER_WASM_SIMD)

#include <wasm_simd128.h>
//-------------------------------------
#include <cstring>

using namespace MindShake;

//-------------------------------------
namespace {

    constexpr int32_t kOpaqueSkipWidth      = 16;
    constexpr int32_t kTranslucentSkipWidth = 8;

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE bool
    UsesShortcuts(int32_t width) {
        return width >= (kOpaque ? kOpaqueSkipWidth : kTranslucentSkipWidth);
    }

    //---------------------------------
    struct Color {
        v128_t      solid;      // The premultiplied color for 4 pixels
        v128_t      even;       // Its blue and red for 4 pixels, each in a 16-bit lane
        v128_t      odd;        // Its green and alpha for 4 pixels
        v128_t      alpha;      // The alpha of the color in every lane
    };

    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE Color
    MakeColor(uint32_t premultiplied, uint32_t alpha) {
        Color color;
        color.solid = wasm_i32x4_splat(int32_t(premultiplied));
        color.even  = wasm_v128_and(color.solid, wasm_i16x8_const_splat(0x00ff));
        color.odd   = wasm_u16x8_shr(color.solid, 8);
        color.alpha = wasm_i16x8_splat(int16_t(alpha));
        return color;
    }

    // The 4 bytes at p as a number. WebAssembly is little-endian, so the first byte is the lowest.
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
    FONTRENDERER_ALWAYS_INLINE v128_t
    LoadQuad(const void *p) {
        return wasm_v128_load(p);
    }

    // Writes the 4 pixels of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StoreQuad(void *p, v128_t x) {
        wasm_v128_store(p, x);
    }

    // 2 pixels or texels in the low half of a register, and 0 in the high half.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    LoadPair(const void *p) {
        return wasm_v128_load64_zero(p);
    }

    // Writes the 2 pixels in the low half of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StorePair(void *p, v128_t x) {
        wasm_v128_store64_lane(p, x, 0);
    }

    // Writes the 2 pixels in the high half of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StoreHighPair(void *p, v128_t x) {
        wasm_v128_store64_lane(p, x, 1);
    }

    // 1 pixel or texel in the low 32 bits of a register.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    LoadOne(const void *p) {
        return wasm_v128_load32_zero(p);
    }

    // Writes the pixel in the low 32 bits of x.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE void
    StoreOne(void *p, v128_t x) {
        wasm_v128_store32_lane(p, x, 0);
    }

    // A number in the low 32 bits of a register, and 0 in the rest.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    FromNumber(uint32_t value) {
        return wasm_i32x4_make(int32_t(value), 0, 0, 0);
    }

    // Each 16-bit lane divided by 255 and rounded. Exact up to 65152, the largest sum of a blend.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    Div255(v128_t x) {
        const v128_t t = wasm_i16x8_add(x, wasm_i16x8_const_splat(0x0080));
        return wasm_u16x8_shr(wasm_i16x8_add(t, wasm_u16x8_shr(t, 8)), 8);
    }

    // Rounded (source + pixels * (255 - alpha)) / 255 in each 16-bit lane, with the source already multiplied.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    Blend(v128_t source, v128_t alpha, v128_t pixels) {
        const v128_t inverse = wasm_v128_xor(alpha, wasm_i16x8_const_splat(0x00ff));
        return Div255(wasm_i16x8_add(source, wasm_i16x8_mul(pixels, inverse)));
    }

    // The bytes 0, 2, 4 and so on of x, each in a 16-bit lane: the blue and red of 4 pixels.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    EvenBytes(v128_t x) {
        return wasm_v128_and(x, wasm_i16x8_const_splat(0x00ff));
    }

    // The bytes 1, 3, 5 and so on of x, each in a 16-bit lane: the green and alpha of 4 pixels.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    OddBytes(v128_t x) {
        return wasm_u16x8_shr(x, 8);
    }

    // The opposite of EvenBytes and OddBytes: 4 pixels again.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    JoinBytes(v128_t even, v128_t odd) {
        return wasm_v128_or(even, wasm_i16x8_shl(odd, 8));
    }

    // The 4 coverage bytes in the low 32 bits of x, each copied to the 2 lanes of 16 bits of its pixel.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    Spread(v128_t x) {
        const v128_t twice = wasm_i8x16_shuffle(x, x, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
        return wasm_u16x8_shr(wasm_i16x8_shuffle(twice, twice, 0, 8, 1, 9, 2, 10, 3, 11), 8);
    }

    // Spread for each row of a band, whose coverage has a row in each 32-bit lane.
    //---------------------------------
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

    // 4 pixels blended with the color, with the coverage from Spread. Under an opaque text, the alpha of the blend is the
    // coverage.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE v128_t
    BlendCoverage(const Color &color, v128_t spread, v128_t pixels) {
        const v128_t alpha = kOpaque ? spread : Div255(wasm_i16x8_mul(spread, color.alpha));
        const v128_t even  = Blend(wasm_i16x8_mul(color.even, spread), alpha, EvenBytes(pixels));
        const v128_t odd   = Blend(wasm_i16x8_mul(color.odd,  spread), alpha, OddBytes(pixels));
        return JoinBytes(even, odd);
    }

    // The alpha of each of the 4 pixels of odd, which has their green and alpha, over the 2 lanes of its pixel.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    ExpandAlphaOdd(v128_t odd) {
        return wasm_i8x16_shuffle(odd, odd, 2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15);
    }

    // 4 premultiplied texels times the premultiplied color, blended with 4 pixels.
    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE v128_t
    BlendTexels(const Color &color, v128_t texels, v128_t pixels) {
        const v128_t odd       = OddBytes(texels);
        const v128_t sourceOdd = wasm_i16x8_mul(odd, color.odd);
        const v128_t alpha     = ExpandAlphaOdd(kOpaque ? odd : Div255(sourceOdd));
        const v128_t even      = Blend(wasm_i16x8_mul(EvenBytes(texels), color.even), alpha, EvenBytes(pixels));
        return JoinBytes(even, Blend(sourceOdd, alpha, OddBytes(pixels)));
    }

    // The alpha of the 4 texels is 0.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsTransparent(v128_t texels) {
        return wasm_v128_any_true(wasm_v128_and(texels, wasm_i32x4_const_splat(int32_t(0xff000000u)))) == false;
    }

    // The 16 coverage bytes of a band are 0.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsEmptyBand(v128_t band) {
        return wasm_v128_any_true(band) == false;
    }

    // The 16 coverage bytes of a band are 255.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE bool
    IsFullBand(v128_t band) {
        return wasm_v128_any_true(wasm_v128_not(band)) == false;
    }

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

    // The coverage of 4 rows in 4 columns of a rotated glyph, with a row in each 32-bit lane and row 0 in the lowest.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE v128_t
    LoadBand(const uint8_t *column0, const uint8_t *column1, const uint8_t *column2, const uint8_t *column3) {
        const v128_t first  = wasm_i8x16_shuffle(LoadOne(column0), LoadOne(column1), 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
        const v128_t second = wasm_i8x16_shuffle(LoadOne(column2), LoadOne(column3), 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
        return wasm_i16x8_shuffle(first, second, 0, 8, 1, 9, 2, 10, 3, 11);
    }

    //---------------------------------
    template <bool kOpaque, bool kSkip, bool kRead>
    FONTRENDERER_ALWAYS_INLINE void
    DrawCoverage(const Color &color, v128_t spread, bool none, bool full, uint32_t *p, v128_t pixels) {
        if (kSkip && none) {
            return;
        }
        if (kSkip && kOpaque && full) {
            StoreQuad(p, color.solid);
            return;
        }

        StoreQuad(p, BlendCoverage<kOpaque>(color, spread, kRead ? pixels : LoadQuad(p)));
    }

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_ALWAYS_INLINE void
    DrawTexel(const Color &color, const uint8_t *texel, uint32_t *p) {
        const uint32_t t = Load32(texel);
        if ((t >> 24) != 0) {
            StoreOne(p, BlendTexels<kOpaque>(color, FromNumber(t), LoadOne(p)));
        }
    }

    //---------------------------------
    template <bool kOpaque, bool kSkip, bool kRotated>
    struct Alpha8Row {
        const Color     &color;
        const uint8_t   *mask;
        size_t          step;
        uint32_t        *dst;

        //-----------------------------
        template <bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        Draw(size_t x, v128_t pixels) const {
            const uint32_t m = LoadCoverage<kRotated>(&mask[x * step], step);
            DrawCoverage<kOpaque, kSkip, kRead>(color, Spread(FromNumber(m)), m == 0, m == 0xffffffffu, &dst[x], pixels);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Quad(size_t x) const {
            Draw<false>(x, wasm_i64x2_const_splat(0));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        ReadQuad(size_t x) const {
            return LoadQuad(&dst[x]);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        QuadRead(size_t x, v128_t pixels) const {
            Draw<true>(x, pixels);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Narrow(int32_t w) const {
            if (w == 1) {
                StoreOne(dst, BlendCoverage<kOpaque>(color, Spread(FromNumber(mask[0])), LoadOne(dst)));
            }
            else if (w == 2) {
                StorePair(dst, BlendCoverage<kOpaque>(color, Spread(FromNumber(LoadCoveragePair<kRotated>(mask, step))), LoadPair(dst)));
            }
            else {
                const uint32_t m      = LoadCoveragePair<kRotated>(mask, step) | (LoadCoveragePair<kRotated>(&mask[step], step) << 16);
                const v128_t   pixels = wasm_i64x2_shuffle(LoadPair(dst), LoadPair(&dst[1]), 0, 2);
                const v128_t   result = BlendCoverage<kOpaque>(color, Spread(FromNumber(m)), pixels);
                StoreHighPair(&dst[1], result);
                StorePair(dst, result);
            }
        }
    };

    //---------------------------------
    template <bool kOpaque, bool kSkip, bool kPartial>
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
            const uint8_t *column = &mask[x * step];
            const v128_t  band    = LoadBand(column, &column[step], &column[2 * step], &column[3 * step]);
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

            v128_t spread0, spread1, spread2, spread3;
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
        const uint8_t   *src;
        uint32_t        *dst;

        //-----------------------------
        template <bool kRead>
        FONTRENDERER_ALWAYS_INLINE void
        Draw(size_t x, v128_t pixels) const {
            const v128_t texels = LoadQuad(&src[4 * x]);
            if (kSkip && IsTransparent(texels)) {
                return;
            }

            StoreQuad(&dst[x], BlendTexels<kOpaque>(color, texels, kRead ? pixels : LoadQuad(&dst[x])));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        Quad(size_t x) const {
            Draw<false>(x, wasm_i64x2_const_splat(0));
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE v128_t
        ReadQuad(size_t x) const {
            return LoadQuad(&dst[x]);
        }

        //-----------------------------
        FONTRENDERER_ALWAYS_INLINE void
        QuadRead(size_t x, v128_t pixels) const {
            Draw<true>(x, pixels);
        }

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
                const v128_t texels = wasm_i64x2_shuffle(LoadPair(src), LoadPair(&src[4]), 0, 2);
                const v128_t pixels = wasm_i64x2_shuffle(LoadPair(dst), LoadPair(&dst[1]), 0, 2);
                const v128_t result = BlendTexels<kOpaque>(color, texels, pixels);
                StoreHighPair(&dst[1], result);
                StorePair(dst, result);
            }
        }
    };

    //---------------------------------
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

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_NO_INLINE void
    DrawAlpha8(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
               uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
        const uint8_t *mask = &texture[offset];
        if (stepX != 1) {
            if (UsesShortcuts<kOpaque>(width)) {
                DrawAlpha8Rotated<kOpaque, true>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
            }
            else {
                DrawAlpha8Rotated<kOpaque, false>(mask, stepX, stepY, width, height, dst, dstStride, premultiplied, alpha);
            }
            return;
        }

        const Color color = MakeColor(premultiplied, alpha);
        if (UsesShortcuts<kOpaque>(width)) {
            DrawAlpha8Rows<kOpaque, true>(color, mask, stepY, width, height, dst, dstStride);
        }
        else {
            DrawAlpha8Rows<kOpaque, false>(color, mask, stepY, width, height, dst, dstStride);
        }
    }

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

    //---------------------------------
    template <bool kOpaque>
    FONTRENDERER_NO_INLINE void
    DrawBGRA(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
             uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
        const uint8_t *src  = &texture[offset * 4];
        const Color   color = MakeColor(premultiplied, alpha);
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
GlyphDraw::GetWasmDrawGlyphFunction(uint32_t bytesPerTexel, bool opaque) {
    if (bytesPerTexel == 1) {
        return opaque ? DrawAlpha8<true> : DrawAlpha8<false>;
    }

    return opaque ? DrawBGRA<true> : DrawBGRA<false>;
}

#endif
