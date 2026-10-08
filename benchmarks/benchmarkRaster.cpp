#include "benchmarkSystem.h"

#if defined(FONTRENDERER_USE_STB)
    #include <FontSTB.h>
#endif
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
    #include <FontSFT.h>
#endif
#if defined(FONTRENDERER_USE_FREETYPE)
    #include <FontFT.h>
#endif
//-------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Measures how long Font takes to render the glyphs of a text, with each backend: the work that benchmarkDraw leaves
// out, because it renders the glyphs before it measures. Each call clears the rendered glyphs with Reset and renders them
// again with Preload, so opening the file is not measured.
//
// The checksum of the texture tells whether two builds render the same pixels. A build for another x86-64 level, or
// another compiler, may give other pixels, because the project builds with fast floating point and the compiler may
// then fuse a multiplication and an addition.

using namespace MindShake;
using namespace Benchmark;

//-------------------------------------
namespace {

    constexpr const char *kResourcesPath = FONT_RENDERER_BENCHMARK_RESOURCES;
    constexpr int         kSamples       = 15;
    constexpr double      kSampleSeconds = 0.02;

    //---------------------------------
    struct Backend {
        const char  *name;
        Font        *(*create)(const char *fileName);
    };

    //---------------------------------
    const Backend kBackends[] = {
#if defined(FONTRENDERER_USE_STB)
        { "stb_truetype", [](const char *fileName) -> Font * { return new FontSTB(fileName); } },
#endif
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
        { "libschrift",   [](const char *fileName) -> Font * { return new FontSFT(fileName); } },
#endif
#if defined(FONTRENDERER_USE_FREETYPE)
        { "FreeType",     [](const char *fileName) -> Font * { return new FontFT(fileName); } },
#endif
    };

    const char    *kFonts[] = { "Roboto-Regular.ttf", "DejaVuSerifCondensed-BoldItalic.ttf" };
    const uint8_t kSizes[]  = { 14, 24, 40, 96 };

    // The printable characters of ASCII and Latin-1, in UTF-8.
    //---------------------------------
    std::string
    MakeText() {
        std::string text;
        for (uint32_t c = 0x20; c <= 0xff; ++c) {
            if (c >= 0x7f && c < 0xa0) {
                continue;
            }
            if (c < 0x80) {
                text += char(c);
            }
            else {
                text += char(0xc0 | (c >> 6));
                text += char(0x80 | (c & 0x3f));
            }
        }
        return text;
    }

    // FNV-1a over the rows of the texture that hold glyphs.
    //---------------------------------
    uint32_t
    Checksum(const Font &font) {
        const uint8_t *texture = font.GetTexture();
        if (texture == nullptr) {
            return 0;
        }

        const size_t bytes = size_t(font.GetTextureWidth()) * font.GetUsedTextureHeight();
        uint32_t     hash  = 2166136261u;
        for (size_t i = 0; i < bytes; ++i) {
            hash = (hash ^ texture[i]) * 16777619u;
        }
        return hash;
    }

    //---------------------------------
    double
    Seconds(Font &font, const std::string &text, uint8_t size, int calls) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < calls; ++i) {
            font.Reset();
            font.Preload(text.c_str(), size);
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        return elapsed.count();
    }

} // end of namespace

//-------------------------------------
int
main(int argc, char *argv[]) {
    if (argc == 2 && strcmp(argv[1], "--name") == 0) {
        printf("%s\n", GetRunName().c_str());
        return 0;
    }

    KeepSystemAwake();

    const std::string processor = GetProcessor();
    printf("Compiler: %s\n", GetCompiler().c_str());
    printf("Processor: %s\n\n", processor.empty() ? "unknown" : processor.c_str());
#if !defined(NDEBUG)
    printf("This is not a Release build: the times say little about the real speed.\n\n");
#endif

    const std::string text = MakeText();
    printf("Each call renders the 191 printable characters of ASCII and Latin-1 with Preload, after Reset.\n\n");
    printf("    %-42s  %10s  %10s  %8s\n", "Font, size and backend", "Median us", "Min us", "Checksum");

    for (const char *fontName : kFonts) {
        const std::string path = std::string(kResourcesPath) + fontName;
        for (const Backend &backend : kBackends) {
            std::unique_ptr<Font> font(backend.create(path.c_str()));
            if (font->GetStatus() != FontBase::EStatus::Ok) {
                printf("    %s cannot open %s\n", backend.name, path.c_str());
                continue;
            }

            for (uint8_t size : kSizes) {
                // One call to warm up and to count how many calls fill a sample.
                const double once  = std::max(Seconds(*font, text, size, 1), 1e-6);
                const int    calls = std::max(1, int(kSampleSeconds / once));

                std::vector<double> samples;
                for (int s = 0; s < kSamples; ++s) {
                    samples.push_back(Seconds(*font, text, size, calls) / calls);
                }
                std::sort(samples.begin(), samples.end());

                char label[96];
                snprintf(label, sizeof(label), "%s %d px, %s", fontName, int(size), backend.name);
                printf("    %-42s  %10.1f  %10.1f  %08x\n", label, samples[kSamples / 2] * 1e6, samples[0] * 1e6, Checksum(*font));
            }
        }
    }
    return 0;
}
