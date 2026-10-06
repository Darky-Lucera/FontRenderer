#include "benchmarkCommon.h"
//-------------------------------------
#include <GlyphDraw.h>

// The functions FontBase::DrawText draws the glyphs with, called as it calls them: chosen once, and called for each
// glyph. They are the ones of the library, not a copy, so Baseline is always what the library does without the
// layout and the clipping. Draw uses the fastest the library was built with that this processor can run, which can be
// the copy for x86-64-v2. DrawSse2 uses the SSE2 ones, when the library has them, and DrawScalar the scalar ones.
// To try a change, start from Sse2Overlap.cpp, whose Draw does what the SSE2 code does, from X64v2.cpp, whose Draw does
// what the copy for x86-64-v2 does, from NeonPacked.cpp, whose Draw does what the NEON code does, or from
// MoreJumpsPerGlyph.cpp, a copy of the scalar code.

using namespace MindShake;

//-------------------------------------
namespace Benchmark {
namespace Baseline {

    //---------------------------------
    namespace {

        //-----------------------------
        void
        DrawQuads(Scenario &scenario, uint32_t *dst, GlyphDraw::DrawGlyphFunction drawGlyph) {
            const FontBase  &font         = *scenario.font;
            const uint8_t   *texture      = font.GetTexture();
            const size_t    textureWidth  = font.GetTextureWidth();
            const uint32_t  premultiplied = GlyphDraw::PremultiplyColor(scenario.color);
            const uint32_t  colorAlpha    = scenario.color >> 24;

            for (const GlyphQuad &quad : scenario.quads) {
                const size_t stepX         = quad.rotated ? textureWidth : 1;
                const size_t stepY         = quad.rotated ? 1 : textureWidth;
                const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
                uint32_t     *dstGlyph     = &dst[size_t(scenario.posY + quad.y) * scenario.width + size_t(scenario.posX + quad.x)];
                drawGlyph(texture, offsetTexture, stepX, stepY, quad.width, quad.height, dstGlyph, scenario.width, premultiplied, colorAlpha);
            }
        }

        //-----------------------------
        uint32_t
        GetBytesPerTexel(const Scenario &scenario) {
            return (scenario.font->GetTextureFormat() == FontBase::ETextureFormat::Alpha8) ? 1 : 4;
        }

    } // end of namespace

    //---------------------------------
    void
    Draw(Scenario &scenario, uint32_t *dst) {
        DrawQuads(scenario, dst, GlyphDraw::GetDrawGlyphFunction(GetBytesPerTexel(scenario), (scenario.color >> 24) == 255));
    }

    //---------------------------------
    void
    DrawScalar(Scenario &scenario, uint32_t *dst) {
        DrawQuads(scenario, dst, GlyphDraw::GetScalarDrawGlyphFunction(GetBytesPerTexel(scenario), (scenario.color >> 24) == 255));
    }

#if defined(FONTRENDERER_SSE2) && !defined(FONTRENDERER_X86_64_V2)
    //---------------------------------
    void
    DrawSse2(Scenario &scenario, uint32_t *dst) {
        DrawQuads(scenario, dst, GlyphDraw::GetSse2DrawGlyphFunction(GetBytesPerTexel(scenario), (scenario.color >> 24) == 255));
    }
#endif

} // end of namespace Baseline
} // end of namespace Benchmark
