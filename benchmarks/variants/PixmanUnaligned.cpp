#include "benchmarkCommon.h"

#if defined(FONTRENDERER_SSE2)

#include "benchmarkSse2.h"
//-------------------------------------
#include <cstring>
#include <emmintrin.h>

// The SSE2 code that pixman uses to draw a solid color through an 8-bit mask over a 32-bit destination
// (sse2_composite_over_n_8_8888 in pixman-sse2.c), applied to each glyph. The helpers keep their names in pixman,
// to compare them with the original. It works with a premultiplied color and rounds its divisions by 255,
// so the pixels may differ from DrawText, which truncates, by a level or two.
// Only an Alpha8 texture uses it. A rotated glyph is drawn one texel at a time with the same helpers, and a
// BGRA32 texture is drawn by Baseline.
// Unlike pixman, it does not draw pixel by pixel until the destination is aligned to 16 bytes: it loads and stores
// unaligned, which costs about the same on current CPUs. A glyph row is often narrower than what that alignment skips.

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
namespace PixmanUnaligned {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        //-----------------------------
        struct Source {
            uint32_t    src;
            uint32_t    srca;
            __m128i     xmm_def;
            __m128i     xmm_src;
            __m128i     xmm_alpha;
        };

        //-----------------------------
        inline void
        DrawPixel(const Source &source, uint8_t m, uint32_t *dst) {
            if (m) {
                __m128i mmx_src   = source.xmm_src;
                __m128i mmx_alpha = source.xmm_alpha;
                __m128i mmx_mask  = expand_pixel_8_1x128(m);
                __m128i mmx_dest  = unpack_32_1x128(*dst);
                *dst = pack_1x128_32(in_over_1x128(&mmx_src, &mmx_alpha, &mmx_mask, &mmx_dest));
            }
        }

        // The body of sse2_composite_over_n_8_8888 for one row, without the loop that aligns dst.
        //-----------------------------
        void
        DrawRow(const Source &source, const uint8_t *mask, uint32_t *dst, int32_t w) {
            while (w >= 4) {
                uint32_t m;
                memcpy(&m, mask, sizeof(uint32_t));

                if (source.srca == 0xff && m == 0xffffffff) {
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), source.xmm_def);
                }
                else if (m) {
                    __m128i xmm_src   = source.xmm_src;
                    __m128i xmm_alpha = source.xmm_alpha;
                    __m128i xmm_dst   = _mm_loadu_si128(reinterpret_cast<__m128i *>(dst));
                    __m128i xmm_mask  = unpack_32_1x128(m);
                    xmm_mask = _mm_unpacklo_epi8(xmm_mask, _mm_setzero_si128());

                    __m128i xmm_dst_lo, xmm_dst_hi;
                    __m128i xmm_mask_lo, xmm_mask_hi;
                    unpack_128_2x128(xmm_dst, &xmm_dst_lo, &xmm_dst_hi);
                    unpack_128_2x128(xmm_mask, &xmm_mask_lo, &xmm_mask_hi);

                    expand_alpha_rev_2x128(xmm_mask_lo, xmm_mask_hi, &xmm_mask_lo, &xmm_mask_hi);

                    in_over_2x128(&xmm_src, &xmm_src, &xmm_alpha, &xmm_alpha, &xmm_mask_lo, &xmm_mask_hi, &xmm_dst_lo, &xmm_dst_hi);

                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), pack_2x128_128(xmm_dst_lo, xmm_dst_hi));
                }

                w    -= 4;
                dst  += 4;
                mask += 4;
            }

            while (w) {
                DrawPixel(source, *mask++, dst);
                w--;
                dst++;
            }
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        const FontBase &font = *scenario.font;
        if (font.GetTextureFormat() != FontBase::ETextureFormat::Alpha8) {
            Baseline::Draw(scenario, dst);
            return;
        }

        Source source;
        source.src = Premultiply(scenario.color);
        if (source.src == 0) {
            return;
        }
        source.srca      = source.src >> 24;
        source.xmm_def   = _mm_set1_epi32(int(source.src));
        source.xmm_src   = expand_pixel_32_1x128(source.src);
        source.xmm_alpha = expand_alpha_1x128(source.xmm_src);

        const uint8_t *texture      = font.GetTexture();
        const size_t  textureWidth  = font.GetTextureWidth();
        for (const GlyphQuad &quad : scenario.quads) {
            const uint8_t *mask     = &texture[size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x)];
            uint32_t      *dstGlyph = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            if (quad.rotated) {
                for (int32_t y = 0; y < quad.height; ++y) {
                    uint32_t *row = &dstGlyph[size_t(y) * scenario.width];
                    for (int32_t x = 0; x < quad.width; ++x) {
                        DrawPixel(source, mask[size_t(x) * textureWidth + size_t(y)], &row[x]);
                    }
                }
            }
            else {
                for (int32_t y = 0; y < quad.height; ++y) {
                    DrawRow(source, &mask[size_t(y) * textureWidth], &dstGlyph[size_t(y) * scenario.width], quad.width);
                }
            }
        }
    }

} // end of namespace PixmanUnaligned
} // end of namespace Benchmark

#endif
