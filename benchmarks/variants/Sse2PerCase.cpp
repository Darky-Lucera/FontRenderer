#include "benchmarkCommon.h"

#if defined(FONTRENDERER_SSE2)

#include "benchmarkSse2.h"
//-------------------------------------
#include <cstring>
#include <emmintrin.h>

// RoundOnceSse2Premultiplied with the structure of the library, and the same pixels: a function draws one glyph, and
// there is one for each texture format and for an opaque or a translucent text, chosen once and never inlined.
// The function for an opaque text does not ask in each block whether the text is opaque, and it does not multiply
// the coverage by the alpha of the color, which is 255.
//
// DrawTail also changes the end of each row. A row is drawn in blocks of 4 pixels, and Draw draws the 1 to 3 pixels
// left one at a time. DrawTail draws them with one more block, moved back so that it ends where the row ends. The
// pixels of that block that are already drawn get a coverage of 0, or a transparent texel, which leaves them as they
// are: the blend gives round(pixel * 255 / 255). A glyph narrower than 4 pixels has no room for it.

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
namespace Sse2PerCase {

    // Unnamed, so that a helper of another variant with the same name is a different function.
    //---------------------------------
    namespace {

#if defined(_MSC_VER)
    #define NO_INLINE __declspec(noinline)
#else
    #define NO_INLINE __attribute__((noinline))
#endif

        using DrawGlyphFunction = void (*)(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                                           uint32_t *dst, uint32_t dstStride, uint32_t color, uint32_t alpha);

        // Below this width, putting together the 4 texels of a rotated glyph costs more than drawing them one by one.
        constexpr int32_t kMinRotatedBlockWidth = 8;

        // Each row keeps the texels from the first of these on, to clear the texels of a block that are already drawn.
        alignas(16) const uint32_t kKeepFrom[4][4] = {
            { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu },
            { 0x00000000u, 0xffffffffu, 0xffffffffu, 0xffffffffu },
            { 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu },
            { 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu },
        };

        //-----------------------------
        struct Alpha8Color {
            uint32_t    premultiplied;
            __m128i     xmm_solid;      // The color for 4 pixels, written when the text and the 4 texels are opaque
            __m128i     xmm_color;      // The premultiplied color, unpacked for 2 pixels
            __m128i     xmm_alpha;      // The alpha of the color in every lane
        };

        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawAlpha8Pixel(const Alpha8Color &color, uint8_t m, uint32_t *dst) {
            if (kOpaque && m == 255) {
                *dst = color.premultiplied;
            }
            else if (m) {
                const __m128i mask  = expand_pixel_8_1x128(m);
                const __m128i alpha = kOpaque ? mask : pix_multiply_1x128(mask, color.xmm_alpha);
                *dst = pack_1x128_32(blend_premultiplied(_mm_mullo_epi16(color.xmm_color, mask), alpha, unpack_32_1x128(*dst)));
            }
        }

        // The body of sse2_composite_over_n_8_8888 for 4 pixels, with the coverage of each in a byte of m.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawAlpha8Block(const Alpha8Color &color, uint32_t m, uint32_t *dst) {
            if (kOpaque && m == 0xffffffff) {
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), color.xmm_solid);
            }
            else if (m) {
                __m128i xmm_alpha = color.xmm_alpha;
                __m128i xmm_dst   = _mm_loadu_si128(reinterpret_cast<__m128i *>(dst));
                __m128i xmm_mask  = unpack_32_1x128(m);
                xmm_mask = _mm_unpacklo_epi8(xmm_mask, _mm_setzero_si128());

                __m128i xmm_dst_lo, xmm_dst_hi;
                __m128i xmm_mask_lo, xmm_mask_hi;
                unpack_128_2x128(xmm_dst, &xmm_dst_lo, &xmm_dst_hi);
                unpack_128_2x128(xmm_mask, &xmm_mask_lo, &xmm_mask_hi);

                expand_alpha_rev_2x128(xmm_mask_lo, xmm_mask_hi, &xmm_mask_lo, &xmm_mask_hi);

                const __m128i xmm_src_lo = _mm_mullo_epi16(color.xmm_color, xmm_mask_lo);
                const __m128i xmm_src_hi = _mm_mullo_epi16(color.xmm_color, xmm_mask_hi);
                if (kOpaque == false) {
                    pix_multiply_2x128(&xmm_mask_lo, &xmm_mask_hi, &xmm_alpha, &xmm_alpha, &xmm_mask_lo, &xmm_mask_hi);
                }
                xmm_dst_lo = blend_premultiplied(xmm_src_lo, xmm_mask_lo, xmm_dst_lo);
                xmm_dst_hi = blend_premultiplied(xmm_src_hi, xmm_mask_hi, xmm_dst_hi);

                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), pack_2x128_128(xmm_dst_lo, xmm_dst_hi));
            }
        }

        //-----------------------------
        template <bool kRotated, bool kOpaque, bool kTailBlock>
        inline void
        DrawAlpha8Row(const Alpha8Color &color, const uint8_t *mask, size_t step, uint32_t *dst, int32_t w) {
            const bool hasBlocks = w >= 4;
            while (w >= 4) {
                DrawAlpha8Block<kOpaque>(color, LoadMask<kRotated>(mask, step), dst);
                w    -= 4;
                dst  += 4;
                mask += 4 * step;
            }

            if (kTailBlock && hasBlocks && w > 0) {
                const int32_t  drawn = 4 - w;
                const uint32_t m     = LoadMask<kRotated>(mask - size_t(drawn) * step, step) & (0xffffffffu << (8 * drawn));
                DrawAlpha8Block<kOpaque>(color, m, dst - drawn);
            }
            else {
                while (w > 0) {
                    DrawAlpha8Pixel<kOpaque>(color, *mask, dst);
                    mask += step;
                    w--;
                    dst++;
                }
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kTailBlock>
        NO_INLINE void
        DrawAlpha8Glyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                        uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            Alpha8Color color;
            color.premultiplied = premultiplied;
            color.xmm_solid     = _mm_set1_epi32(int(premultiplied));
            color.xmm_color     = expand_pixel_32_1x128(premultiplied);
            color.xmm_alpha     = _mm_set1_epi16(int16_t(alpha));

            const uint8_t *mask = &texture[offset];
            if (stepX == 1) {
                for (int32_t y = 0; y < height; ++y) {
                    DrawAlpha8Row<false, kOpaque, kTailBlock>(color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride], width);
                }
            }
            else if (width < kMinRotatedBlockWidth) {
                for (int32_t y = 0; y < height; ++y) {
                    uint32_t *row = &dst[size_t(y) * dstStride];
                    for (int32_t x = 0; x < width; ++x) {
                        DrawAlpha8Pixel<kOpaque>(color, mask[size_t(y) * stepY + size_t(x) * stepX], &row[x]);
                    }
                }
            }
            else {
                for (int32_t y = 0; y < height; ++y) {
                    DrawAlpha8Row<true, kOpaque, kTailBlock>(color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride], width);
                }
            }
        }

        //-----------------------------
        struct BGRAColor {
            __m128i     xmm_color;      // The premultiplied color of the text, unpacked twice, for 2 pixels
            bool        white;
        };

        // The premultiplied texel times the premultiplied color, blended with the destination. The alpha of the blend
        // is that product in the alpha lane, rounded. Under an opaque text it is the alpha of the texel.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawBGRATexel(const BGRAColor &color, const uint8_t *texel, uint32_t *dst) {
            uint32_t t;
            memcpy(&t, texel, sizeof(uint32_t));
            if (t >> 24) {
                const __m128i unpacked = unpack_32_1x128(t);
                const __m128i source   = _mm_mullo_epi16(unpacked, color.xmm_color);
                const __m128i alpha    = kOpaque ? expand_alpha_1x128(unpacked) : div255_round(expand_alpha_1x128(source));
                *dst = pack_1x128_32(blend_premultiplied(source, alpha, unpack_32_1x128(*dst)));
            }
        }

        // 4 texels. A block of transparent texels is skipped, and a block of opaque texels under an opaque text is
        // written without blending.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawBGRABlock(const BGRAColor &color, __m128i xmm_src, uint32_t *dst) {
            if (is_transparent(xmm_src)) {
                return;
            }

            __m128i xmm_color = color.xmm_color;
            __m128i xmm_src_lo, xmm_src_hi;
            if (kOpaque && is_opaque(xmm_src)) {
                if (color.white) {
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), xmm_src);
                }
                else {
                    unpack_128_2x128(xmm_src, &xmm_src_lo, &xmm_src_hi);
                    pix_multiply_2x128(&xmm_src_lo, &xmm_src_hi, &xmm_color, &xmm_color, &xmm_src_lo, &xmm_src_hi);
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), pack_2x128_128(xmm_src_lo, xmm_src_hi));
                }
            }
            else {
                __m128i xmm_alpha_lo, xmm_alpha_hi;
                __m128i xmm_dst_lo, xmm_dst_hi;
                unpack_128_2x128(xmm_src, &xmm_src_lo, &xmm_src_hi);
                unpack_128_2x128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(dst)), &xmm_dst_lo, &xmm_dst_hi);

                if (kOpaque) {
                    expand_alpha_2x128(xmm_src_lo, xmm_src_hi, &xmm_alpha_lo, &xmm_alpha_hi);
                }
                xmm_src_lo = _mm_mullo_epi16(xmm_src_lo, xmm_color);
                xmm_src_hi = _mm_mullo_epi16(xmm_src_hi, xmm_color);
                if (kOpaque == false) {
                    expand_alpha_2x128(xmm_src_lo, xmm_src_hi, &xmm_alpha_lo, &xmm_alpha_hi);
                    xmm_alpha_lo = div255_round(xmm_alpha_lo);
                    xmm_alpha_hi = div255_round(xmm_alpha_hi);
                }
                xmm_dst_lo = blend_premultiplied(xmm_src_lo, xmm_alpha_lo, xmm_dst_lo);
                xmm_dst_hi = blend_premultiplied(xmm_src_hi, xmm_alpha_hi, xmm_dst_hi);

                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), pack_2x128_128(xmm_dst_lo, xmm_dst_hi));
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kTailBlock>
        inline void
        DrawBGRARow(const BGRAColor &color, const uint8_t *src, uint32_t *dst, int32_t w) {
            const bool hasBlocks = w >= 4;
            while (w >= 4) {
                DrawBGRABlock<kOpaque>(color, _mm_loadu_si128(reinterpret_cast<const __m128i *>(src)), dst);
                w   -= 4;
                dst += 4;
                src += 16;
            }

            if (kTailBlock && hasBlocks && w > 0) {
                const int32_t drawn   = 4 - w;
                const __m128i xmm_src = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src - size_t(drawn) * 4));
                const __m128i keep    = _mm_load_si128(reinterpret_cast<const __m128i *>(kKeepFrom[drawn]));
                DrawBGRABlock<kOpaque>(color, _mm_and_si128(xmm_src, keep), dst - drawn);
            }
            else {
                while (w > 0) {
                    DrawBGRATexel<kOpaque>(color, src, dst);
                    src += 4;
                    w--;
                    dst++;
                }
            }
        }

        // A rotated glyph is drawn one pixel at a time: no scenario measures it.
        //-----------------------------
        template <bool kOpaque, bool kTailBlock>
        NO_INLINE void
        DrawBGRAGlyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                      uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t /*alpha*/) {
            BGRAColor color;
            color.xmm_color = expand_pixel_32_1x128(premultiplied);
            color.white     = premultiplied == 0xffffffffu;

            const uint8_t *src = &texture[offset * 4];
            for (int32_t y = 0; y < height; ++y) {
                uint32_t *row = &dst[size_t(y) * dstStride];
                if (stepX == 1) {
                    DrawBGRARow<kOpaque, kTailBlock>(color, &src[size_t(y) * stepY * 4], row, width);
                }
                else {
                    for (int32_t x = 0; x < width; ++x) {
                        DrawBGRATexel<kOpaque>(color, &src[(size_t(y) * stepY + size_t(x) * stepX) * 4], &row[x]);
                    }
                }
            }
        }

#undef NO_INLINE

        //-----------------------------
        template <bool kTailBlock>
        DrawGlyphFunction
        GetDrawGlyphFunction(bool alpha8, bool opaque) {
            if (alpha8) {
                return opaque ? DrawAlpha8Glyph<true, kTailBlock> : DrawAlpha8Glyph<false, kTailBlock>;
            }

            return opaque ? DrawBGRAGlyph<true, kTailBlock> : DrawBGRAGlyph<false, kTailBlock>;
        }

        //-----------------------------
        template <bool kTailBlock>
        inline void
        DrawQuads(Scenario &scenario, uint32_t *dst) {
            const FontBase          &font         = *scenario.font;
            const uint8_t           *texture      = font.GetTexture();
            const size_t            textureWidth  = font.GetTextureWidth();
            const uint32_t          premultiplied = PremultiplyColor(scenario.color);
            const uint32_t          colorAlpha    = scenario.color >> 24;
            const DrawGlyphFunction drawGlyph     = GetDrawGlyphFunction<kTailBlock>(font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8, colorAlpha == 255);

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
        DrawQuads<false>(scenario, dst);
    }

    //---------------------------------
    void
    DrawTail(Scenario &scenario, uint32_t *dst) {
        DrawQuads<true>(scenario, dst);
    }

} // end of namespace Sse2PerCase
} // end of namespace Benchmark

#endif
