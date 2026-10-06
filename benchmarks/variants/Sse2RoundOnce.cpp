#include "benchmarkCommon.h"

#if defined(FONTRENDERER_SSE2)

#include "benchmarkSse2.h"
//-------------------------------------
#include <cstring>
#include <emmintrin.h>

// Sse2 with the blend of RoundOnceOver, and the same pixels: color * alpha + destination * (255 - alpha), with both
// products added before a single rounding division by 255. The sum is at most 255 * 255, so it fits in 16 bits.
// pixman rounds each product on its own and then adds them with saturation; one rounding is more exact and needs
// fewer instructions. The alpha of the blend is still rounded on its own, as in RoundOnceOver.
//
// Alpha8: PixmanHybrid with its threshold of 8 pixels. It is the code that pixman uses to draw a solid color through
// an 8-bit mask (sse2_composite_over_n_8_8888), with unaligned loads and stores instead of drawing pixel by pixel
// until the destination is aligned, and with rotated glyphs of 8 pixels or wider also drawn 4 pixels at a time.
// pixman premultiplies the color first; here the coverage times the alpha of the text gives the alpha of the blend,
// and the color is multiplied by it, with the same number of multiplications.
//
// BGRA32: pixman has no operation that tints a texture with a color, so the texel is multiplied by the color of the
// text, channel by channel, then premultiplied by its own alpha, and then blended with over, as pixman does.
// A block of 4 transparent texels is skipped, and a block of 4 opaque texels under an opaque text is written without
// blending. A rotated glyph is drawn one pixel at a time: no scenario measures it, as the effects font is not rotated.

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
namespace Sse2RoundOnce {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

        // 255 in the alpha of each unpacked pixel, the 16-bit lanes 3 and 7.
        const __m128i mask_alpha = _mm_set_epi32(0x00ff0000, 0x00000000, 0x00ff0000, 0x00000000);

        // Rounded (color * alpha + dst * (255 - alpha)) / 255 in each 16-bit lane, with the rounding of pix_multiply.
        //-----------------------------
        inline __m128i
        blend_once(__m128i color, __m128i alpha, __m128i dst) {
            const __m128i sum = _mm_add_epi16(_mm_mullo_epi16(color, alpha), _mm_mullo_epi16(dst, negate_1x128(alpha)));
            return _mm_mulhi_epu16(_mm_add_epi16(sum, mask_0080), mask_0101);
        }

        //-----------------------------
        struct Source {
            uint32_t    srca;
            __m128i     xmm_def;        // The color with an alpha of 255, for 4 pixels
            __m128i     xmm_color;      // The same, unpacked for 2 pixels, so that color times alpha keeps the alpha
            __m128i     xmm_alpha;      // The alpha of the color in every lane
        };

        //-----------------------------
        inline void
        DrawPixel(const Source &source, uint8_t m, uint32_t *dst) {
            if (m) {
                const __m128i alpha = pix_multiply_1x128(expand_pixel_8_1x128(m), source.xmm_alpha);
                *dst = pack_1x128_32(blend_once(source.xmm_color, alpha, unpack_32_1x128(*dst)));
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

                    pix_multiply_2x128(&xmm_mask_lo, &xmm_mask_hi, &xmm_alpha, &xmm_alpha, &xmm_mask_lo, &xmm_mask_hi);
                    xmm_dst_lo = blend_once(xmm_color, xmm_mask_lo, xmm_dst_lo);
                    xmm_dst_hi = blend_once(xmm_color, xmm_mask_hi, xmm_dst_hi);

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
            __m128i     color;      // The color of the text, unpacked twice, for 2 pixels
            bool        opaque;
            bool        white;
        };

        // The texel times the color, blended with the destination. The alpha of the tinted texel is set to 255 before the
        // blend, so that the alpha of the result is the alpha of the blend, as over needs.
        //-----------------------------
        inline __m128i
        tint_over_1x128(const Tint &tint, uint32_t texel, uint32_t dst) {
            const __m128i tinted = pix_multiply_1x128(unpack_32_1x128(texel), tint.color);
            const __m128i alpha  = expand_alpha_1x128(tinted);
            return blend_once(_mm_or_si128(tinted, mask_alpha), alpha, unpack_32_1x128(dst));
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

                        pix_multiply_2x128(&xmm_src_lo, &xmm_src_hi, &color, &color, &xmm_src_lo, &xmm_src_hi);
                        expand_alpha_2x128(xmm_src_lo, xmm_src_hi, &xmm_alpha_lo, &xmm_alpha_hi);
                        xmm_dst_lo = blend_once(_mm_or_si128(xmm_src_lo, mask_alpha), xmm_alpha_lo, xmm_dst_lo);
                        xmm_dst_hi = blend_once(_mm_or_si128(xmm_src_hi, mask_alpha), xmm_alpha_hi, xmm_dst_hi);

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
        const uint32_t opaqueColor = scenario.color | 0xff000000u;
        source.xmm_def   = _mm_set1_epi32(int(opaqueColor));
        source.xmm_color = expand_pixel_32_1x128(opaqueColor);
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

        Tint tint;
        tint.color  = expand_pixel_32_1x128(scenario.color);
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

} // end of namespace Sse2RoundOnce
} // end of namespace Benchmark

#endif
