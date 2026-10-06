#pragma once

#include <cstdint>
#include <cstring>
#include <emmintrin.h>

// The SSE2 helpers that several variants share. Those of pixman keep their names in pixman, to compare them with
// the original. div255_round, blend_premultiplied, LoadMask and Premultiply are not from pixman.

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

//-------------------------------------
namespace Benchmark {

    // Unnamed, so that each variant file has its own copy, as when each file defined them itself.
    //---------------------------------
    namespace {

        const __m128i mask_0080 = _mm_set1_epi16(0x0080);
        const __m128i mask_00ff = _mm_set1_epi16(0x00ff);
        const __m128i mask_0101 = _mm_set1_epi16(0x0101);

        //-----------------------------
        inline __m128i
        unpack_32_1x128(uint32_t data) {
            return _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(data)), _mm_setzero_si128());
        }

        //-----------------------------
        inline void
        unpack_128_2x128(__m128i data, __m128i *data_lo, __m128i *data_hi) {
            *data_lo = _mm_unpacklo_epi8(data, _mm_setzero_si128());
            *data_hi = _mm_unpackhi_epi8(data, _mm_setzero_si128());
        }

        //-----------------------------
        inline __m128i
        pack_2x128_128(__m128i lo, __m128i hi) {
            return _mm_packus_epi16(lo, hi);
        }

        //-----------------------------
        inline __m128i
        expand_pixel_32_1x128(uint32_t data) {
            return _mm_shuffle_epi32(unpack_32_1x128(data), _MM_SHUFFLE(1, 0, 1, 0));
        }

        //-----------------------------
        inline __m128i
        expand_alpha_1x128(__m128i data) {
            return _mm_shufflehi_epi16(_mm_shufflelo_epi16(data, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
        }

        //-----------------------------
        inline void
        expand_alpha_2x128(__m128i data_lo, __m128i data_hi, __m128i *alpha_lo, __m128i *alpha_hi) {
            const __m128i lo = _mm_shufflelo_epi16(data_lo, _MM_SHUFFLE(3, 3, 3, 3));
            const __m128i hi = _mm_shufflelo_epi16(data_hi, _MM_SHUFFLE(3, 3, 3, 3));
            *alpha_lo = _mm_shufflehi_epi16(lo, _MM_SHUFFLE(3, 3, 3, 3));
            *alpha_hi = _mm_shufflehi_epi16(hi, _MM_SHUFFLE(3, 3, 3, 3));
        }

        // The alpha of the 4 pixels is 255.
        //-----------------------------
        inline bool
        is_opaque(__m128i x) {
            const __m128i ffs = _mm_cmpeq_epi8(x, x);
            return (_mm_movemask_epi8(_mm_cmpeq_epi8(x, ffs)) & 0x8888) == 0x8888;
        }

        // The alpha of the 4 pixels is 0.
        //-----------------------------
        inline bool
        is_transparent(__m128i x) {
            return (_mm_movemask_epi8(_mm_cmpeq_epi8(x, _mm_setzero_si128())) & 0x8888) == 0x8888;
        }

        //-----------------------------
        inline void
        expand_alpha_rev_2x128(__m128i data_lo, __m128i data_hi, __m128i *alpha_lo, __m128i *alpha_hi) {
            const __m128i lo = _mm_shufflelo_epi16(data_lo, _MM_SHUFFLE(0, 0, 0, 0));
            const __m128i hi = _mm_shufflelo_epi16(data_hi, _MM_SHUFFLE(0, 0, 0, 0));
            *alpha_lo = _mm_shufflehi_epi16(lo, _MM_SHUFFLE(0, 0, 0, 0));
            *alpha_hi = _mm_shufflehi_epi16(hi, _MM_SHUFFLE(0, 0, 0, 0));
        }

        // (data * alpha) / 255, rounded: (t + 128) * 257 >> 16 is Blinn's exact rounding.
        //-----------------------------
        inline void
        pix_multiply_2x128(__m128i *data_lo, __m128i *data_hi, __m128i *alpha_lo, __m128i *alpha_hi, __m128i *ret_lo, __m128i *ret_hi) {
            __m128i lo = _mm_mullo_epi16(*data_lo, *alpha_lo);
            __m128i hi = _mm_mullo_epi16(*data_hi, *alpha_hi);
            lo = _mm_adds_epu16(lo, mask_0080);
            hi = _mm_adds_epu16(hi, mask_0080);
            *ret_lo = _mm_mulhi_epu16(lo, mask_0101);
            *ret_hi = _mm_mulhi_epu16(hi, mask_0101);
        }

        //-----------------------------
        inline void
        negate_2x128(__m128i data_lo, __m128i data_hi, __m128i *neg_lo, __m128i *neg_hi) {
            *neg_lo = _mm_xor_si128(data_lo, mask_00ff);
            *neg_hi = _mm_xor_si128(data_hi, mask_00ff);
        }

        //-----------------------------
        inline void
        over_2x128(__m128i *src_lo, __m128i *src_hi, __m128i *alpha_lo, __m128i *alpha_hi, __m128i *dst_lo, __m128i *dst_hi) {
            __m128i t1, t2;
            negate_2x128(*alpha_lo, *alpha_hi, &t1, &t2);
            pix_multiply_2x128(dst_lo, dst_hi, &t1, &t2, dst_lo, dst_hi);
            *dst_lo = _mm_adds_epu8(*src_lo, *dst_lo);
            *dst_hi = _mm_adds_epu8(*src_hi, *dst_hi);
        }

        //-----------------------------
        inline __m128i
        expand_pixel_8_1x128(uint8_t data) {
            return _mm_shufflelo_epi16(unpack_32_1x128(uint32_t(data)), _MM_SHUFFLE(0, 0, 0, 0));
        }

        //-----------------------------
        inline __m128i
        pix_multiply_1x128(__m128i data, __m128i alpha) {
            return _mm_mulhi_epu16(_mm_adds_epu16(_mm_mullo_epi16(data, alpha), mask_0080), mask_0101);
        }

        //-----------------------------
        inline __m128i
        negate_1x128(__m128i data) {
            return _mm_xor_si128(data, mask_00ff);
        }

        //-----------------------------
        inline __m128i
        over_1x128(__m128i src, __m128i alpha, __m128i dst) {
            return _mm_adds_epu8(src, pix_multiply_1x128(dst, negate_1x128(alpha)));
        }

        //-----------------------------
        inline uint32_t
        pack_1x128_32(__m128i data) {
            return uint32_t(_mm_cvtsi128_si32(_mm_packus_epi16(data, _mm_setzero_si128())));
        }

        // Each 16-bit lane divided by 255, rounded as pix_multiply does.
        //-----------------------------
        inline __m128i
        div255_round(__m128i x) {
            return _mm_mulhi_epu16(_mm_add_epi16(x, mask_0080), mask_0101);
        }

        // Rounded (source + dst * (255 - alpha)) / 255 in each 16-bit lane, with the source already multiplied.
        //-----------------------------
        inline __m128i
        blend_premultiplied(__m128i source, __m128i alpha, __m128i dst) {
            return div255_round(_mm_add_epi16(source, _mm_mullo_epi16(dst, negate_1x128(alpha))));
        }

        // The texels of 4 pixels in a row. Those of a rotated glyph are step bytes apart.
        //-----------------------------
        template <bool kRotated>
        inline uint32_t
        LoadMask(const uint8_t *mask, size_t step) {
            uint32_t m;
            if (kRotated) {
                m = uint32_t(mask[0]) | (uint32_t(mask[step]) << 8) | (uint32_t(mask[2 * step]) << 16) | (uint32_t(mask[3 * step]) << 24);
            }
            else {
                memcpy(&m, mask, sizeof(uint32_t));
            }

            return m;
        }

        //-----------------------------
        inline void
        in_over_2x128(__m128i *src_lo, __m128i *src_hi, __m128i *alpha_lo, __m128i *alpha_hi,
                      __m128i *mask_lo, __m128i *mask_hi, __m128i *dst_lo, __m128i *dst_hi) {
            __m128i s_lo, s_hi;
            __m128i a_lo, a_hi;
            pix_multiply_2x128(src_lo, src_hi, mask_lo, mask_hi, &s_lo, &s_hi);
            pix_multiply_2x128(alpha_lo, alpha_hi, mask_lo, mask_hi, &a_lo, &a_hi);
            over_2x128(&s_lo, &s_hi, &a_lo, &a_hi, dst_lo, dst_hi);
        }

        //-----------------------------
        inline __m128i
        in_over_1x128(__m128i *src, __m128i *alpha, __m128i *mask, __m128i *dst) {
            return over_1x128(pix_multiply_1x128(*src, *mask), pix_multiply_1x128(*alpha, *mask), *dst);
        }

        // pixman expects the color premultiplied by its alpha. It rounds, as its own conversions do.
        //-----------------------------
        uint32_t
        Premultiply(uint32_t color) {
            const uint32_t alpha  = color >> 24;
            uint32_t       result = alpha << 24;
            for (uint32_t shift = 0; shift < 24; shift += 8) {
                const uint32_t t = ((color >> shift) & 0xff) * alpha + 0x80;
                result |= (((t >> 8) + t) >> 8) << shift;
            }

            return result;
        }

    } // end of namespace

} // end of namespace
