#include "benchmarkCommon.h"

#if defined(FONTRENDERER_SSE2)

#include "benchmarkSse2.h"
//-------------------------------------
#include <cstring>
#include <emmintrin.h>

// Sse2RoundOnce with the BGRA32 texture and the color of the text premultiplied by their alpha, as
// RoundOnceSwarPremultiplied, and with the same pixels. The source of the blend is texel * color, which is already in
// the scale of color * alpha, so each 16-bit lane gets round((texel * color + destination * (255 - alpha)) / 255):
// two multiplications and a single rounding. The largest sum is 65152, which fits in 16 bits.
//
// Alpha8: the coverage multiplies the premultiplied color, as a premultiplied white texel would. The blocks of 4
// texels, the unaligned loads and the threshold for rotated glyphs are the same as in Sse2RoundOnce.
//
// BGRA32: a block of 4 transparent texels is skipped, and a block of 4 opaque texels under an opaque text is written
// without blending. A rotated glyph is drawn one pixel at a time: no scenario measures it.

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
namespace RoundOnceSse2Premultiplied {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        //-----------------------------
        struct Source {
            uint32_t    srca;
            __m128i     xmm_def;        // The color with an alpha of 255, for 4 pixels, written when the text is opaque
            __m128i     xmm_color;      // The premultiplied color, unpacked for 2 pixels
            __m128i     xmm_alpha;      // The alpha of the color in every lane
        };

        //-----------------------------
        inline void
        DrawPixel(const Source &source, uint8_t m, uint32_t *dst) {
            if (m) {
                const __m128i mask  = expand_pixel_8_1x128(m);
                const __m128i alpha = pix_multiply_1x128(mask, source.xmm_alpha);
                *dst = pack_1x128_32(blend_premultiplied(_mm_mullo_epi16(source.xmm_color, mask), alpha, unpack_32_1x128(*dst)));
            }
        }

        // The body of sse2_composite_over_n_8_8888 for one row, without the loop that aligns dst.
        //-----------------------------
        template <bool kRotated>
        void
        DrawRow(const Source &source, const uint8_t *mask, size_t step, uint32_t *dst, int32_t w) {
            while (w >= 4) {
                const uint32_t m = LoadMask<kRotated>(mask, step);

                if (source.srca == 0xff && m == 0xffffffff) {
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), source.xmm_def);
                }
                else if (m) {
                    __m128i xmm_color = source.xmm_color;
                    __m128i xmm_alpha = source.xmm_alpha;
                    __m128i xmm_dst   = _mm_loadu_si128(reinterpret_cast<__m128i *>(dst));
                    __m128i xmm_mask  = unpack_32_1x128(m);
                    xmm_mask = _mm_unpacklo_epi8(xmm_mask, _mm_setzero_si128());

                    __m128i xmm_dst_lo, xmm_dst_hi;
                    __m128i xmm_mask_lo, xmm_mask_hi;
                    unpack_128_2x128(xmm_dst, &xmm_dst_lo, &xmm_dst_hi);
                    unpack_128_2x128(xmm_mask, &xmm_mask_lo, &xmm_mask_hi);

                    expand_alpha_rev_2x128(xmm_mask_lo, xmm_mask_hi, &xmm_mask_lo, &xmm_mask_hi);

                    const __m128i xmm_src_lo = _mm_mullo_epi16(xmm_color, xmm_mask_lo);
                    const __m128i xmm_src_hi = _mm_mullo_epi16(xmm_color, xmm_mask_hi);
                    pix_multiply_2x128(&xmm_mask_lo, &xmm_mask_hi, &xmm_alpha, &xmm_alpha, &xmm_mask_lo, &xmm_mask_hi);
                    xmm_dst_lo = blend_premultiplied(xmm_src_lo, xmm_mask_lo, xmm_dst_lo);
                    xmm_dst_hi = blend_premultiplied(xmm_src_hi, xmm_mask_hi, xmm_dst_hi);

                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), pack_2x128_128(xmm_dst_lo, xmm_dst_hi));
                }

                w    -= 4;
                dst  += 4;
                mask += 4 * step;
            }

            while (w) {
                DrawPixel(source, *mask, dst);
                mask += step;
                w--;
                dst++;
            }
        }

        //-----------------------------
        struct Tint {
            __m128i     color;      // The premultiplied color of the text, unpacked twice, for 2 pixels
            bool        opaque;
            bool        white;
        };

        // The premultiplied texel times the premultiplied color, blended with the destination. The alpha of the blend is
        // that product in the alpha lane, rounded.
        //-----------------------------
        inline __m128i
        tint_over_1x128(const Tint &tint, uint32_t texel, uint32_t dst) {
            const __m128i source = _mm_mullo_epi16(unpack_32_1x128(texel), tint.color);
            const __m128i alpha  = div255_round(expand_alpha_1x128(source));
            return blend_premultiplied(source, alpha, unpack_32_1x128(dst));
        }

        //-----------------------------
        inline void
        DrawTexel(const Tint &tint, const uint8_t *texel, uint32_t *dst) {
            uint32_t t;
            memcpy(&t, texel, sizeof(uint32_t));
            if (t >> 24) {
                *dst = pack_1x128_32(tint_over_1x128(tint, t, *dst));
            }
        }

        //-----------------------------
        void
        DrawTexelRow(const Tint &tint, const uint8_t *src, uint32_t *dst, int32_t w) {
            while (w >= 4) {
                const __m128i xmm_src = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src));
                if (is_transparent(xmm_src) == false) {
                    __m128i xmm_src_lo, xmm_src_hi;
                    if (tint.opaque && is_opaque(xmm_src)) {
                        if (tint.white) {
                            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), xmm_src);
                        }
                        else {
                            __m128i color = tint.color;
                            unpack_128_2x128(xmm_src, &xmm_src_lo, &xmm_src_hi);
                            pix_multiply_2x128(&xmm_src_lo, &xmm_src_hi, &color, &color, &xmm_src_lo, &xmm_src_hi);
                            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), pack_2x128_128(xmm_src_lo, xmm_src_hi));
                        }
                    }
                    else {
                        __m128i color = tint.color;
                        __m128i xmm_alpha_lo, xmm_alpha_hi;
                        __m128i xmm_dst_lo, xmm_dst_hi;
                        unpack_128_2x128(xmm_src, &xmm_src_lo, &xmm_src_hi);
                        unpack_128_2x128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(dst)), &xmm_dst_lo, &xmm_dst_hi);

                        xmm_src_lo = _mm_mullo_epi16(xmm_src_lo, color);
                        xmm_src_hi = _mm_mullo_epi16(xmm_src_hi, color);
                        expand_alpha_2x128(xmm_src_lo, xmm_src_hi, &xmm_alpha_lo, &xmm_alpha_hi);
                        xmm_alpha_lo = div255_round(xmm_alpha_lo);
                        xmm_alpha_hi = div255_round(xmm_alpha_hi);
                        xmm_dst_lo = blend_premultiplied(xmm_src_lo, xmm_alpha_lo, xmm_dst_lo);
                        xmm_dst_hi = blend_premultiplied(xmm_src_hi, xmm_alpha_hi, xmm_dst_hi);

                        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), pack_2x128_128(xmm_dst_lo, xmm_dst_hi));
                    }
                }

                w   -= 4;
                dst += 4;
                src += 16;
            }

            while (w) {
                DrawTexel(tint, src, dst);
                src += 4;
                w--;
                dst++;
            }
        }

    } // end of namespace

    // Below this width, putting together the 4 texels of a rotated glyph costs more than drawing them one by one.
    constexpr int32_t kMinRotatedBlockWidth = 8;

    //---------------------------------
    void
    DrawAlpha8(Scenario &scenario, uint32_t *dst) {
        Source source;
        source.srca = scenario.color >> 24;
        if (source.srca == 0) {
            return;
        }
        // When the text is opaque, the premultiplied color is the color itself.
        source.xmm_def   = _mm_set1_epi32(int(scenario.color | 0xff000000u));
        source.xmm_color = expand_pixel_32_1x128(PremultiplyColor(scenario.color));
        source.xmm_alpha = _mm_set1_epi16(int16_t(source.srca));

        const uint8_t *texture      = scenario.font->GetTexture();
        const size_t  textureWidth  = scenario.font->GetTextureWidth();
        for (const GlyphQuad &quad : scenario.quads) {
            const uint8_t *mask     = &texture[size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x)];
            uint32_t      *dstGlyph = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            if (quad.rotated && quad.width < kMinRotatedBlockWidth) {
                for (int32_t y = 0; y < quad.height; ++y) {
                    uint32_t *row = &dstGlyph[size_t(y) * scenario.width];
                    for (int32_t x = 0; x < quad.width; ++x) {
                        DrawPixel(source, mask[size_t(x) * textureWidth + size_t(y)], &row[x]);
                    }
                }
            }
            else if (quad.rotated) {
                for (int32_t y = 0; y < quad.height; ++y) {
                    DrawRow<true>(source, &mask[size_t(y)], textureWidth, &dstGlyph[size_t(y) * scenario.width], quad.width);
                }
            }
            else {
                for (int32_t y = 0; y < quad.height; ++y) {
                    DrawRow<false>(source, &mask[size_t(y) * textureWidth], 1, &dstGlyph[size_t(y) * scenario.width], quad.width);
                }
            }
        }
    }

    //---------------------------------
    void
    DrawBgra(Scenario &scenario, uint32_t *dst) {
        Color32 fontColor;
        fontColor.color = scenario.color;

        // When the texel and the text are opaque, both premultiplied values are the values themselves, so the opaque
        // blocks keep the shortcuts of Sse2RoundOnce.
        Tint tint;
        tint.color  = expand_pixel_32_1x128(PremultiplyColor(scenario.color));
        tint.opaque = fontColor.a == 255;
        tint.white  = scenario.color == 0xffffffffu;

        const uint8_t *texture      = scenario.font->GetTexture();
        const size_t  textureWidth  = scenario.font->GetTextureWidth();
        for (const GlyphQuad &quad : scenario.quads) {
            const uint8_t *src      = &texture[(size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x)) * 4];
            uint32_t      *dstGlyph = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
            for (int32_t y = 0; y < quad.height; ++y) {
                uint32_t *row = &dstGlyph[size_t(y) * scenario.width];
                if (quad.rotated) {
                    for (int32_t x = 0; x < quad.width; ++x) {
                        DrawTexel(tint, &src[(size_t(x) * textureWidth + size_t(y)) * 4], &row[x]);
                    }
                }
                else {
                    DrawTexelRow(tint, &src[size_t(y) * textureWidth * 4], row, quad.width);
                }
            }
        }
    }

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        if (scenario.font->GetTextureFormat() == FontBase::ETextureFormat::Alpha8) {
            DrawAlpha8(scenario, dst);
        }
        else {
            DrawBgra(scenario, dst);
        }
    }

} // end of namespace RoundOnceSse2Premultiplied
} // end of namespace Benchmark

#endif
