#include "benchmarkCommon.h"

#if defined(FONTRENDERER_NEON)

#include <arm_neon.h>
//-------------------------------------
#include <cstring>

// Sse2PerCase with NEON, and the same pixels: a function draws one glyph, and there is one for each texture format and
// for an opaque or a translucent text, chosen once and never inlined. The function for an opaque text does not
// multiply the coverage by the alpha of the color, which is 255.
//
// Draw draws 4 pixels at a time, as SSE2 does. The blend of a channel is round((source + dst * (255 - alpha)) / 255).
// vmull_u8 gives the product of the color and the coverage in 16 bits, vmlal_u8 adds dst * (255 - alpha) to it, and
// vrshrq_n_u16 with vraddhn_u16 divide that sum by 255 and round it once, as the scalar code does.
//
// Draw8 draws the glyphs of Alpha8 that are not rotated 8 pixels at a time, with the channels in separate registers
// (vld4_u8 and vst4_u8): the color is a constant per channel, so no register needs to spread the coverage.
//
// DrawTail and Draw8Tail also change the end of each row. Instead of drawing the 1 to 3 pixels left one at a time, they
// draw one more block of 4, moved back so that it ends where the row ends. The pixels of that block that are already
// drawn get a coverage of 0, or a transparent texel, which leaves them as they are: the blend gives
// round(pixel * 255 / 255). A glyph narrower than 4 pixels has no room for it.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace NeonPerCase {

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

        // Indexes for vtbl1_u8. The first two copy each of the 4 bytes of a register 4 times, the pixel of a byte
        // is in its 4 lanes. The last copies the alpha of each of 2 pixels over its 4 bytes.
        const uint8_t kSpreadLow[8]   = { 0, 0, 0, 0, 1, 1, 1, 1 };
        const uint8_t kSpreadHigh[8]  = { 2, 2, 2, 2, 3, 3, 3, 3 };
        const uint8_t kSpreadAlpha[8] = { 3, 3, 3, 3, 7, 7, 7, 7 };

        // Each row keeps the texels from the first of these on, to clear the texels of a block that are already drawn.
        const uint32_t kKeepFrom[4][4] = {
            { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu },
            { 0x00000000u, 0xffffffffu, 0xffffffffu, 0xffffffffu },
            { 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu },
            { 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu },
        };

        // Each 16-bit lane divided by 255 and rounded, as an 8-bit lane. Exact up to 65152 + 128, which the sums here never pass.
        //-----------------------------
        inline uint8x8_t
        Div255(uint16x8_t x) {
            return vraddhn_u16(x, vrshrq_n_u16(x, 8));
        }

        // Rounded (source + dst * (255 - alpha)) / 255 in each byte of 2 pixels, with the source already multiplied.
        //-----------------------------
        inline uint8x8_t
        BlendPair(uint16x8_t source, uint8x8_t dst, uint8x8_t alpha) {
            return Div255(vmlal_u8(source, dst, vmvn_u8(alpha)));
        }

        // The 4 bytes at p as a number, which is little-endian on the processors this code is built for.
        //-----------------------------
        inline uint32_t
        Load32(const uint8_t *p) {
            uint32_t value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        // The coverage of 4 pixels in a row. The texels of a rotated glyph are step bytes apart.
        //-----------------------------
        template <bool kRotated>
        inline uint32_t
        LoadMask(const uint8_t *mask, size_t step) {
            if (kRotated) {
                return uint32_t(mask[0]) | (uint32_t(mask[step]) << 8) | (uint32_t(mask[2 * step]) << 16) | (uint32_t(mask[3 * step]) << 24);
            }

            return Load32(mask);
        }

        //-----------------------------
        struct Alpha8Color {
            uint32_t    premultiplied;
            uint32_t    alpha;
            uint8x16_t  solid;          // The color for 4 pixels, written when the text and the 4 texels are opaque
            uint8x8_t   color;          // The premultiplied color, for 2 pixels
            uint8x8_t   alphaLanes;     // The alpha of the color in every lane
            uint8x8_t   spreadLow;      // The indexes that spread the coverage of pixels 0 and 1, and of 2 and 3
            uint8x8_t   spreadHigh;
            uint8x8_t   planes[4];      // Each channel of the color in every lane, for Draw8
        };

        //-----------------------------
        inline Alpha8Color
        MakeAlpha8Color(uint32_t premultiplied, uint32_t alpha) {
            Alpha8Color color;
            color.premultiplied = premultiplied;
            color.alpha         = alpha;
            color.solid         = vreinterpretq_u8_u32(vdupq_n_u32(premultiplied));
            color.color         = vreinterpret_u8_u32(vdup_n_u32(premultiplied));
            color.alphaLanes    = vdup_n_u8(uint8_t(alpha));
            color.spreadLow     = vld1_u8(kSpreadLow);
            color.spreadHigh    = vld1_u8(kSpreadHigh);
            for (uint32_t channel = 0; channel < 4; ++channel) {
                color.planes[channel] = vdup_n_u8(uint8_t(premultiplied >> (8 * channel)));
            }

            return color;
        }

        // One pixel of coverage m, with the shortcuts of the scalar code for an opaque text.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawAlpha8Pixel(const Alpha8Color &color, uint8_t m, uint32_t *dst) {
            if (kOpaque && m == 255) {
                *dst = color.premultiplied;
            }
            else if (m) {
                const uint8_t  alpha  = kOpaque ? m : uint8_t(MulDiv255(m, color.alpha));
                const uint8x8_t result = BlendPair(vmull_u8(color.color, vdup_n_u8(m)), vreinterpret_u8_u32(vld1_dup_u32(dst)), vdup_n_u8(alpha));
                vst1_lane_u32(dst, vreinterpret_u32_u8(result), 0);
            }
        }

        // Draws 4 pixels. Each byte of m is the coverage of one of them.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawAlpha8Block(const Alpha8Color &color, uint32_t m, uint32_t *dst) {
            uint8_t *bytes = reinterpret_cast<uint8_t *>(dst);
            if (kOpaque && m == 0xffffffff) {
                vst1q_u8(bytes, color.solid);
            }
            else if (m) {
                const uint8x8_t coverage = vreinterpret_u8_u32(vdup_n_u32(m));
                const uint8x8_t alpha    = kOpaque ? coverage : Div255(vmull_u8(coverage, color.alphaLanes));
                const uint8x16_t pixels  = vld1q_u8(bytes);

                const uint8x8_t low  = BlendPair(vmull_u8(color.color, vtbl1_u8(coverage, color.spreadLow)),
                                                 vget_low_u8(pixels), vtbl1_u8(alpha, color.spreadLow));
                const uint8x8_t high = BlendPair(vmull_u8(color.color, vtbl1_u8(coverage, color.spreadHigh)),
                                                 vget_high_u8(pixels), vtbl1_u8(alpha, color.spreadHigh));
                vst1q_u8(bytes, vcombine_u8(low, high));
            }
        }

        // Draws 8 pixels, with blue, green, red and alpha of the pixels in a register each.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawAlpha8Block8(const Alpha8Color &color, const uint8_t *mask, uint32_t *dst) {
            uint8_t         *bytes    = reinterpret_cast<uint8_t *>(dst);
            const uint8x8_t coverage  = vld1_u8(mask);
            const uint64_t  coverages = vget_lane_u64(vreinterpret_u64_u8(coverage), 0);
            if (kOpaque && coverages == ~uint64_t(0)) {
                vst1q_u8(bytes,      color.solid);
                vst1q_u8(bytes + 16, color.solid);
            }
            else if (coverages) {
                const uint8x8_t alpha = kOpaque ? coverage : Div255(vmull_u8(coverage, color.alphaLanes));
                uint8x8x4_t     pixels = vld4_u8(bytes);
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    pixels.val[channel] = BlendPair(vmull_u8(coverage, color.planes[channel]), pixels.val[channel], alpha);
                }
                vst4_u8(bytes, pixels);
            }
        }

        // The 1 to 3 pixels left at the end of a row are drawn one by one. The clipping of DrawText can leave a width
        // below 0, which draws nothing.
        //-----------------------------
        template <bool kRotated, bool kOpaque, bool kBlock8, bool kTailBlock>
        inline void
        DrawAlpha8Row(const Alpha8Color &color, const uint8_t *mask, size_t step, uint32_t *dst, int32_t w) {
            const bool hasBlocks = w >= 4;
            if (kBlock8 && kRotated == false) {
                while (w >= 8) {
                    DrawAlpha8Block8<kOpaque>(color, mask, dst);
                    w    -= 8;
                    dst  += 8;
                    mask += 8;
                }
            }

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
        template <bool kOpaque, bool kBlock8, bool kTailBlock>
        NO_INLINE void
        DrawAlpha8Glyph(const uint8_t *texture, size_t offset, size_t stepX, size_t stepY, int32_t width, int32_t height,
                        uint32_t *dst, uint32_t dstStride, uint32_t premultiplied, uint32_t alpha) {
            const Alpha8Color color = MakeAlpha8Color(premultiplied, alpha);

            const uint8_t *mask = &texture[offset];
            if (stepX == 1) {
                for (int32_t y = 0; y < height; ++y) {
                    DrawAlpha8Row<false, kOpaque, kBlock8, kTailBlock>(color, &mask[size_t(y) * stepY], 1, &dst[size_t(y) * dstStride], width);
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
                    DrawAlpha8Row<true, kOpaque, kBlock8, kTailBlock>(color, &mask[size_t(y) * stepY], stepX, &dst[size_t(y) * dstStride], width);
                }
            }
        }

        //-----------------------------
        struct BGRAColor {
            uint8x8_t   color;          // The premultiplied color of the text, for 2 pixels
            uint8x8_t   spreadAlpha;
            bool        white;
        };

        // The premultiplied texel times the premultiplied color, blended with the destination. The alpha of the blend
        // is that product in the alpha lane, rounded. Under an opaque text it is the alpha of the texel.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawBGRATexel(const BGRAColor &color, const uint8_t *texel, uint32_t *dst) {
            const uint32_t t = Load32(texel);
            if (t >> 24) {
                const uint8x8_t  texels = vreinterpret_u8_u32(vdup_n_u32(t));
                const uint16x8_t source = vmull_u8(texels, color.color);
                const uint8x8_t  alpha  = vtbl1_u8(kOpaque ? texels : Div255(source), color.spreadAlpha);
                const uint8x8_t  result = BlendPair(source, vreinterpret_u8_u32(vld1_dup_u32(dst)), alpha);
                vst1_lane_u32(dst, vreinterpret_u32_u8(result), 0);
            }
        }

        // The alpha of the 4 texels is 0.
        //-----------------------------
        inline bool
        IsTransparent(uint8x16_t texels) {
            const uint32x4_t alphas = vandq_u32(vreinterpretq_u32_u8(texels), vdupq_n_u32(0xff000000u));
            const uint64x1_t both   = vorr_u64(vget_low_u64(vreinterpretq_u64_u32(alphas)), vget_high_u64(vreinterpretq_u64_u32(alphas)));
            return vget_lane_u64(both, 0) == 0;
        }

        // The alpha of the 4 texels is 255.
        //-----------------------------
        inline bool
        IsOpaque(uint8x16_t texels) {
            const uint32x4_t alphas = vorrq_u32(vreinterpretq_u32_u8(texels), vdupq_n_u32(0x00ffffffu));
            const uint64x1_t both   = vand_u64(vget_low_u64(vreinterpretq_u64_u32(alphas)), vget_high_u64(vreinterpretq_u64_u32(alphas)));
            return vget_lane_u64(both, 0) == ~uint64_t(0);
        }

        // 4 texels. A block of transparent texels is skipped, and a block of opaque texels under an opaque text is
        // written without blending: tinted with the color, or as it is when the color is white.
        //-----------------------------
        template <bool kOpaque>
        inline void
        DrawBGRABlock(const BGRAColor &color, uint8x16_t texels, uint32_t *dst) {
            if (IsTransparent(texels)) {
                return;
            }

            uint8_t          *bytes     = reinterpret_cast<uint8_t *>(dst);
            const uint8x8_t  texelsLow  = vget_low_u8(texels);
            const uint8x8_t  texelsHigh = vget_high_u8(texels);
            const uint16x8_t sourceLow  = vmull_u8(texelsLow,  color.color);
            const uint16x8_t sourceHigh = vmull_u8(texelsHigh, color.color);
            if (kOpaque && IsOpaque(texels)) {
                vst1q_u8(bytes, color.white ? texels : vcombine_u8(Div255(sourceLow), Div255(sourceHigh)));
            }
            else {
                const uint8x16_t pixels = vld1q_u8(bytes);
                const uint8x8_t  low    = BlendPair(sourceLow,  vget_low_u8(pixels),
                                                    vtbl1_u8(kOpaque ? texelsLow  : Div255(sourceLow),  color.spreadAlpha));
                const uint8x8_t  high   = BlendPair(sourceHigh, vget_high_u8(pixels),
                                                    vtbl1_u8(kOpaque ? texelsHigh : Div255(sourceHigh), color.spreadAlpha));
                vst1q_u8(bytes, vcombine_u8(low, high));
            }
        }

        //-----------------------------
        template <bool kOpaque, bool kTailBlock>
        inline void
        DrawBGRARow(const BGRAColor &color, const uint8_t *src, uint32_t *dst, int32_t w) {
            const bool hasBlocks = w >= 4;
            while (w >= 4) {
                DrawBGRABlock<kOpaque>(color, vld1q_u8(src), dst);
                w   -= 4;
                dst += 4;
                src += 16;
            }

            if (kTailBlock && hasBlocks && w > 0) {
                const int32_t    drawn  = 4 - w;
                const uint8x16_t texels = vandq_u8(vld1q_u8(src - size_t(drawn) * 4), vld1q_u8(reinterpret_cast<const uint8_t *>(kKeepFrom[drawn])));
                DrawBGRABlock<kOpaque>(color, texels, dst - drawn);
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
            color.color       = vreinterpret_u8_u32(vdup_n_u32(premultiplied));
            color.spreadAlpha = vld1_u8(kSpreadAlpha);
            color.white       = premultiplied == 0xffffffffu;

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
        template <bool kBlock8, bool kTailBlock>
        DrawGlyphFunction
        GetDrawGlyphFunction(bool alpha8, bool opaque) {
            if (alpha8) {
                return opaque ? DrawAlpha8Glyph<true, kBlock8, kTailBlock> : DrawAlpha8Glyph<false, kBlock8, kTailBlock>;
            }

            return opaque ? DrawBGRAGlyph<true, kTailBlock> : DrawBGRAGlyph<false, kTailBlock>;
        }

        //-----------------------------
        template <bool kBlock8, bool kTailBlock>
        inline void
        DrawQuads(Scenario &scenario, uint32_t *dst) {
            const FontBase          &font         = *scenario.font;
            const uint8_t           *texture      = font.GetTexture();
            const size_t            textureWidth  = font.GetTextureWidth();
            const uint32_t          premultiplied = PremultiplyColor(scenario.color);
            const uint32_t          colorAlpha    = scenario.color >> 24;
            const DrawGlyphFunction drawGlyph     = GetDrawGlyphFunction<kBlock8, kTailBlock>(font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8, colorAlpha == 255);

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
        DrawQuads<false, false>(scenario, dst);
    }

    //---------------------------------
    void
    Draw8(Scenario &scenario, uint32_t *dst) {
        DrawQuads<true, false>(scenario, dst);
    }

    //---------------------------------
    void
    DrawTail(Scenario &scenario, uint32_t *dst) {
        DrawQuads<false, true>(scenario, dst);
    }

    //---------------------------------
    void
    Draw8Tail(Scenario &scenario, uint32_t *dst) {
        DrawQuads<true, true>(scenario, dst);
    }

} // end of namespace NeonPerCase
} // end of namespace Benchmark

#endif
