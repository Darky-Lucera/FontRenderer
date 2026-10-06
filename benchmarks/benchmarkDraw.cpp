#include "benchmarkCommon.h"
#include "benchmarkSystem.h"
#if defined(FONTRENDERER_USE_STB)
    #include <FontSTB.h>
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
    #include <FontSFT.h>
#else
    #include <FontFT.h>
#endif
#if defined(FONTRENDERER_USE_BAKED)
    #include <FontBaked.h>
#endif
#include <Tga.h>
//-------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// Measures how long drawing text into a buffer in memory takes. It opens no window, so events and the frame rate
// do not disturb the times. Each variant draws the glyphs that GetGlyphQuads gives, and must leave the same pixels
// as FontBase::DrawText. When a variant leaves other pixels, it saves both images to show what the variant breaks.

using namespace MindShake;
using namespace Benchmark;

//-------------------------------------
namespace {

#if defined(FONTRENDERER_USE_STB)
    using RasterFont = FontSTB;
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
    using RasterFont = FontSFT;
#else
    using RasterFont = FontFT;
#endif

    constexpr const char *kResourcesPath    = FONT_RENDERER_BENCHMARK_RESOURCES;
    constexpr const char *kOutputPath       = FONT_RENDERER_BENCHMARK_OUTPUT;

    constexpr int32_t     kMargin           = 8;
    constexpr int         kSamples          = 15;
    constexpr double      kMinSampleSeconds = 0.02;

    // Not white, so that every channel of the texture is multiplied. An opaque text allows shortcuts that a
    // translucent one does not.
    constexpr struct {
        const char  *name;
        uint32_t    color;
    } kColors[] = {
        { "opaque",          0xffffc040u           },
        { "translucent",     0xe0ffc040u           },
    };

    //---------------------------------
    struct Variant {
        const char      *name;
        DrawFunction    draw;
    };

    // The variants to measure, in the order they are printed.
    using Selection = std::vector<const Variant *>;

    //---------------------------------
    struct Timing {
        double              median;     // Seconds per call
        double              min;
        std::vector<double> samples;    // Seconds per call, sorted
    };

    // Quoted, because the names of the scenarios have commas.
    //---------------------------------
    std::string
    CsvText(const std::string &text) {
        std::string quoted = "\"";
        for (char c : text) {
            quoted += (c == '"') ? std::string("\"\"") : std::string(1, c);
        }
        return quoted + "\"";
    }

    //---------------------------------
    void
    DrawReference(Scenario &scenario, uint32_t *dst) {
        scenario.font->DrawText(scenario.text.c_str(), scenario.size, scenario.color, dst, scenario.width, scenario.posX, scenario.posY);
    }

    //---------------------------------
    const Variant kVariants[] = {
        { "Reference",                  DrawReference                     },
        { "Baseline",                   Baseline::Draw                    },
        { "BaselineScalar",             Baseline::DrawScalar              },
#if defined(FONTRENDERER_SSE2) && !defined(FONTRENDERER_X86_64_V2)
        { "BaselineSse2",               Baseline::DrawSse2                },
#endif
#if defined(FONTRENDERER_SSE2)
        { "RoundOnceSse2Premultiplied", RoundOnceSse2Premultiplied::Draw  },
        // Sse2PerCase is the SSE2 code the library had before; Sse2Overlap does what it has now.
        { "Sse2PerCase",                Sse2PerCase::Draw                 },
        { "Sse2PerCaseTail",            Sse2PerCase::DrawTail             },
        { "Sse2Overlap",                Sse2Overlap::Draw                 },
        { "Sse2OverlapWidths",          Sse2Overlap::DrawWidths           },
        { "Sse2OverlapBandWidths",      Sse2Overlap::DrawBandWidths       },
        { "Sse2OverlapWidthsBandWidths", Sse2Overlap::DrawWidthsBandWidths },
        // They pick the case of the width as Sse2OverlapWidthsBandWidths does, and then draw as Sse2Overlap does. In
        // WidthIfs the compiler can still build each case from the range of the width; in WidthIfsHidden it cannot.
        { "Sse2OverlapWidthIfs",        Sse2Overlap::DrawWidthIfs         },
        { "Sse2OverlapWidthIfsHidden",  Sse2Overlap::DrawWidthIfsHidden   },
#if defined(FONTRENDERER_BENCHMARK_ALIGNED)
        // The same code with its loops and jumps aligned.
        { "Sse2OverlapAligned",         Sse2OverlapAligned::Draw          },
        { "Sse2OverlapWidthsAligned",   Sse2OverlapAligned::DrawWidths    },
        { "Sse2OverlapBandWidthsAligned", Sse2OverlapAligned::DrawBandWidths },
        { "Sse2OverlapWidthsBandWidthsAligned", Sse2OverlapAligned::DrawWidthsBandWidths },
#endif
        // The library code with three ways to write Spread. SpreadUnpack is the one of the library.
        { "Sse2SpreadUnpack",           Sse2Spread::DrawUnpack            },
        { "Sse2SpreadShift",            Sse2Spread::DrawShift             },
        { "Sse2SpreadOr",               Sse2Spread::DrawOr                },
        // Each of these changes one choice of Sse2Overlap. Off to measure the others in less time.
        // { "Sse2OverlapPixels",          Sse2Overlap::DrawPixels           },
        // { "Sse2OverlapLate",            Sse2Overlap::DrawLate             },
        // { "Sse2OverlapPairTail",        Sse2Overlap::DrawPairTail         },
        // { "Sse2OverlapNoNarrowRows",    Sse2Overlap::DrawNoNarrowRows     },
        // { "Sse2OverlapNoBands",         Sse2Overlap::DrawNoBands          },
        // { "Sse2OverlapRowShortcuts",    Sse2Overlap::DrawRowShortcuts     },
        // { "Sse2OverlapPixman",          Sse2Overlap::DrawPixman           },
        // { "Sse2OverlapUprightApart",    Sse2Overlap::DrawUprightApart     },
        // { "Sse2OverlapOctets",          Sse2Overlap::DrawOctets           },
        // { "Sse2OverlapBgraTint",        Sse2Overlap::DrawBgraTint         },
        // { "Sse2OverlapSkip",            Sse2Overlap::DrawSkip             },
        // { "Sse2OverlapBranchless",      Sse2Overlap::DrawBranchless       },
        // { "Sse2OverlapOpaque8",         Sse2Overlap::DrawOpaque8          },
        // { "Sse2OverlapOpaque24",        Sse2Overlap::DrawOpaque24         },
        // { "Sse2OverlapTranslucent0",    Sse2Overlap::DrawTranslucent0     },
        // { "Sse2OverlapTranslucent16",   Sse2Overlap::DrawTranslucent16    },
        // They take the texture as straight or use the old formula, so they leave other pixels than DrawText.
        // { "Pixman",                     Pixman::Draw                      },
        // { "PixmanUnaligned",            PixmanUnaligned::Draw             },
        // { "PixmanRotated",              PixmanRotated::Draw               },
        // { "PixmanHybrid8",              PixmanHybrid::Draw8               },
        // { "PixmanHybrid16",             PixmanHybrid::Draw16              },
        // { "PixmanHybrid24",             PixmanHybrid::Draw24              },
        // { "Sse2",                       Sse2::Draw                        },
        // { "Sse2RoundOnce",              Sse2RoundOnce::Draw               },
#endif
#if defined(FONTRENDERER_BENCHMARK_X64V2)
        // Sse2Overlap with the instructions of x86-64-v2. X64v2Sse2 is the same code as Sse2Overlap, built for that level.
        { "X64v2",                      X64v2::Draw                       },
        { "X64v2NoMadd",                X64v2::DrawNoMadd                 },
        { "X64v2NoShuffle",             X64v2::DrawNoShuffle              },
        { "X64v2NoTest",                X64v2::DrawNoTest                 },
        { "X64v2Sse2",                  X64v2::DrawSse2                   },
        { "X64v2Widths",                X64v2::DrawWidths                 },
        { "X64v2BandWidths",            X64v2::DrawBandWidths             },
        { "X64v2WidthsBandWidths",      X64v2::DrawWidthsBandWidths       },
#if defined(FONTRENDERER_BENCHMARK_ALIGNED)
        { "X64v2Aligned",               X64v2Aligned::Draw                },
        { "X64v2WidthsAligned",         X64v2Aligned::DrawWidths          },
        { "X64v2BandWidthsAligned",     X64v2Aligned::DrawBandWidths      },
        { "X64v2WidthsBandWidthsAligned", X64v2Aligned::DrawWidthsBandWidths },
#endif
        // Slower than X64v2 with small glyphs rotated in the atlas, and with MSVC in every upright scenario. Off to
        // measure the others in less time.
        // { "X64v2Pairs",                 X64v2::DrawPairs                  },
        // { "X64v2WidthsPairs",           X64v2::DrawWidthsPairs            },
#endif
#if defined(FONTRENDERER_NEON)
        // NeonPerCase8 is the NEON code the library had before; NeonPacked does what it has now.
        { "NeonPacked",                 NeonPacked::Draw                  },
        { "NeonPackedLd4",              NeonPacked::DrawLd4               },
        { "NeonPackedNoQuad",           NeonPacked::DrawNoQuad            },
        { "NeonPackedNoNarrow",         NeonPacked::DrawNoNarrow          },
        { "NeonPackedBranchless",       NeonPacked::DrawBranchless        },
        { "NeonPackedSkip",             NeonPacked::DrawSkip              },
        // The steps before NeonPacked, which leave the same pixels as DrawText. Off to measure NeonPacked in less time.
        // { "NeonPerCase",                NeonPerCase::Draw                 },
        // { "NeonPerCase8",               NeonPerCase::Draw8                },
        // { "NeonPerCaseTail",            NeonPerCase::DrawTail             },
        // { "NeonPerCase8Tail",           NeonPerCase::Draw8Tail            },
        // { "NeonOverlap",                NeonOverlap::Draw                 },
        // { "NeonOverlapBranchless",      NeonOverlap::DrawBranchless       },
        // { "NeonOverlapUzp",             NeonOverlap::DrawUzp              },
        // { "NeonOverlapBands",           NeonOverlap::DrawBands            },
        // { "NeonOverlapBandsBranchless", NeonOverlap::DrawBandsBranchless  },
        // { "NeonOverlapBandsSkip32",     NeonOverlap::DrawBandsSkip32      },
#endif
        // The scalar variants that leave the same pixels as DrawText. Off to measure the SSE2 ones in less time.
        // { "MoreJumpsPerGlyph",          MoreJumpsPerGlyph::Draw           },
        // { "MoreJumpsPerCase",           MoreJumpsPerCase::Draw            },
        // { "MoreJumpsPerCaseAlpha8",     MoreJumpsPerCaseAlpha8::Draw      },
        // { "MoreJumpsPremultiplied",     MoreJumpsPremultiplied::Draw      },
        // { "MoreJumpsNoInline",          MoreJumpsNoInline::Draw           },
        // { "MoreJumpsNoInlineCast",      MoreJumpsNoInlineCast::Draw       },
        // { "MoreJumpsHybrid",            MoreJumpsHybrid::Draw             },
        // { "MoreJumpsAlpha8",            MoreJumpsAlpha8::Draw             },
        // { "MoreJumpsAlpha8Hoisted",     MoreJumpsAlpha8Hoisted::Draw      },
        // They take the texture as straight or use the old formula, so they leave other pixels than DrawText.
        // { "MoreJumps",                  MoreJumps::Draw                   },
        // { "NoJumps",                    NoJumps::Draw                     },
        // { "Pixel32",                    Pixel32::Draw                     },
        // { "Restrict",                   Restrict::Draw                    },
        // { "Div255",                     Div255::Draw                      },
        // { "RoundOnce",                  RoundOnce::Draw                   },
        // { "RoundEach",                  RoundEach::Draw                   },
        // { "Swar",                       Swar::Draw                        },
        // { "Premultiplied",              Premultiplied::Draw               },
        // { "Scalar",                     Scalar::Draw                      },
        // { "ScalarSwar",                 ScalarSwar::Draw                  },
        // { "Scalar64",                   Scalar64::Draw                    },
        // { "RoundOnceOver",              RoundOnceOver::Draw               },
        // { "RoundOnce64",                RoundOnce64::Draw                 },
        // { "RoundOnceSwar",              RoundOnceSwar::Draw               },
        // { "Linear",                     Linear::Draw                      },
    };

    // Variants that only run when they are named, so that a run without names stays short.
    //---------------------------------
    const Variant kNamedOnly[] = {
        { "RoundOnceSwarPremultiplied", RoundOnceSwarPremultiplied::Draw  },
        { "RoundOnce64Premultiplied",   RoundOnce64Premultiplied::Draw    },
        // It takes the texture as straight and truncates, so it leaves other pixels than DrawText.
        { "Truncate",                   Truncate::Draw                    },
    };

    // A variant that must leave the same pixels as another variant, instead of the same as DrawText.
    //---------------------------------
    const struct {
        const char  *variant;
        const char  *sameAs;
    } kSameAs[] = {
        { "ScalarSwar",                 "Scalar"                     },
        { "Scalar64",                   "Scalar"                     },
        { "Sse2",                       "Scalar"                     },
        { "RoundOnce64",                "RoundOnceOver"              },
        { "RoundOnceSwar",              "RoundOnceOver"              },
        { "Sse2RoundOnce",              "RoundOnceOver"              },
    };

    //---------------------------------
    const Variant *
    FindVariant(const char *name) {
        for (const Variant &variant : kVariants) {
            if (strcmp(variant.name, name) == 0) {
                return &variant;
            }
        }
        for (const Variant &variant : kNamedOnly) {
            if (strcmp(variant.name, name) == 0) {
                return &variant;
            }
        }

        return nullptr;
    }

    // The variant whose pixels the given one must leave, or nullptr for DrawText.
    //---------------------------------
    const Variant *
    GetSameAs(const Variant &variant) {
        for (const auto &sameAs : kSameAs) {
            if (strcmp(sameAs.variant, variant.name) == 0) {
                return FindVariant(sameAs.sameAs);
            }
        }

        return nullptr;
    }

    //---------------------------------
    std::string
    MakeText(int lines, int columns) {
        static const std::string kWords = "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs! 0123456789 ";

        std::string text;
        size_t      next = 0;
        for (int line = 0; line < lines; ++line) {
            for (int column = 0; column < columns; ++column) {
                text += kWords[next];
                next  = (next + 1) % kWords.size();
            }
            if (line + 1 < lines) {
                text += '\n';
            }
        }

        return text;
    }

    // The text is drawn at a position that leaves the margin around its box, so no glyph needs clipping.
    //---------------------------------
    bool
    Prepare(Scenario &scenario) {
        FontBase::Rect box;
        scenario.font->GetTextBox(scenario.text.c_str(), scenario.size, &box);
        if (box.width <= 0 || box.height <= 0) {
            return false;
        }

        scenario.posX   = kMargin - box.x;
        scenario.posY   = kMargin - box.y;
        scenario.width  = uint32_t(box.width  + 2 * kMargin);
        scenario.height = uint32_t(box.height + 2 * kMargin);
        scenario.font->GetGlyphQuads(scenario.text.c_str(), scenario.size, scenario.quads);

        return scenario.quads.empty() == false;
    }

    // A background that changes from pixel to pixel, so that a variant that reads the wrong destination pixel,
    // or ignores it, leaves other pixels.
    //---------------------------------
    void
    FillBackground(std::vector<uint32_t> &pixels, uint32_t width) {
        for (size_t i = 0; i < pixels.size(); ++i) {
            const uint32_t x = uint32_t(i % width);
            const uint32_t y = uint32_t(i / width);
            pixels[i] = 0xff000000u | (((x * 3) & 0xff) << 16) | (((y * 5) & 0xff) << 8) | ((x ^ y) & 0xff);
        }
    }

    // How much two images differ, to tell a rounding difference of a level from a broken variant.
    //---------------------------------
    struct Difference {
        size_t      pixels    {};
        uint32_t    maxLevels {};   // The largest difference in one channel
    };

    //---------------------------------
    Difference
    Compare(const std::vector<uint32_t> &pixels, const std::vector<uint32_t> &reference) {
        Difference difference;
        for (size_t i = 0; i < pixels.size(); ++i) {
            if (pixels[i] == reference[i]) {
                continue;
            }

            ++difference.pixels;
            for (uint32_t shift = 0; shift < 32; shift += 8) {
                const int32_t levels = std::abs(int32_t((pixels[i] >> shift) & 0xff) - int32_t((reference[i] >> shift) & 0xff));
                difference.maxLevels = std::max(difference.maxLevels, uint32_t(levels));
            }
        }

        return difference;
    }

    //---------------------------------
    std::string
    SaveImage(const std::vector<uint32_t> &pixels, uint32_t width, uint32_t height, const std::string &name) {
        std::string fileName;
        for (char c : name) {
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            fileName += keep ? c : '_';
        }
        const std::string path = std::string(kOutputPath) + "bench_" + fileName + ".tga";

        return WriteTga(path.c_str(), reinterpret_cast<const uint8_t *>(pixels.data()), width, height, size_t(width) * 4, 4, true) ? path : std::string("(cannot write it)");
    }

    //---------------------------------
    double
    SecondsPerCall(Scenario &scenario, DrawFunction draw, uint32_t *dst, uint32_t calls) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < calls; ++i) {
            draw(scenario, dst);
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        return elapsed.count() / calls;
    }

    // How many calls make a sample of at least kMinSampleSeconds. Doubling them until then also warms up the caches.
    //---------------------------------
    uint32_t
    CallsPerSample(Scenario &scenario, DrawFunction draw, uint32_t *dst) {
        uint32_t calls = 1;
        while (SecondsPerCall(scenario, draw, dst, calls) * calls < kMinSampleSeconds) {
            calls *= 2;
        }

        return calls;
    }

    // The samples of the variants take turns, so that a moment in which the system is busy slows down all of them
    // alike, and does not change how they compare. The median ignores the samples that the system interrupted.
    // The buffer is not cleared between calls: blending over the previous text costs the same as over the background.
    //---------------------------------
    std::vector<Timing>
    Measure(Scenario &scenario, uint32_t *dst, const Selection &variants) {
        const size_t          variantCount = variants.size();
        std::vector<uint32_t> calls(variantCount);
        for (size_t i = 0; i < variantCount; ++i) {
            calls[i] = CallsPerSample(scenario, variants[i]->draw, dst);
        }

        std::vector<std::vector<double>> samples(variantCount, std::vector<double>(kSamples));
        for (int sample = 0; sample < kSamples; ++sample) {
            for (size_t i = 0; i < variantCount; ++i) {
                samples[i][sample] = SecondsPerCall(scenario, variants[i]->draw, dst, calls[i]);
            }
        }

        std::vector<Timing> timings;
        for (std::vector<double> &variantSamples : samples) {
            std::sort(variantSamples.begin(), variantSamples.end());
            timings.push_back({ variantSamples[kSamples / 2], variantSamples[0], variantSamples });
        }

        return timings;
    }

    //---------------------------------
    void
    Run(Scenario &scenario, const Selection &variants, FILE *csv, const std::string &compiler, const std::string &processor) {
        if (Prepare(scenario) == false) {
            printf("%s: nothing to draw\n\n", scenario.name.c_str());
            return;
        }

        size_t pixelCount   = 0;
        size_t rotatedCount = 0;
        for (const GlyphQuad &quad : scenario.quads) {
            pixelCount   += size_t(quad.width) * size_t(quad.height);
            rotatedCount += quad.rotated ? 1 : 0;
        }

        const bool alpha8 = scenario.font->GetTextureFormat() == FontBase::ETextureFormat::Alpha8;
        printf("%s: %s, %zu glyphs (%zu%% rotated), %zu pixels, atlas %u x %u, buffer %u x %u\n",
               scenario.name.c_str(), alpha8 ? "Alpha8" : "BGRA32Premultiplied", scenario.quads.size(), (rotatedCount * 100) / scenario.quads.size(),
               pixelCount, unsigned(scenario.font->GetUsedTextureWidth()), unsigned(scenario.font->GetUsedTextureHeight()),
               unsigned(scenario.width), unsigned(scenario.height));
        std::vector<uint32_t> reference(size_t(scenario.width) * scenario.height);
        FillBackground(reference, scenario.width);
        DrawReference(scenario, reference.data());

        std::vector<uint32_t>    pixels(reference.size());
        std::vector<uint32_t>    other(reference.size());
        std::vector<std::string> results;
        std::vector<std::string> targets;
        std::vector<Difference>  differences;
        for (const Variant *selected : variants) {
            const Variant &variant = *selected;
            FillBackground(pixels, scenario.width);
            variant.draw(scenario, pixels.data());

            const Variant               *sameAs   = GetSameAs(variant);
            const std::vector<uint32_t> *expected = &reference;
            std::string                 target    = "Reference";
            if (sameAs != nullptr) {
                FillBackground(other, scenario.width);
                sameAs->draw(scenario, other.data());
                expected = &other;
                target   = sameAs->name;
            }

            std::string      result     = (sameAs != nullptr) ? "same as " + target : "same";
            const Difference difference = Compare(pixels, *expected);
            if (difference.pixels != 0) {
                result  = "DIFFERENT";
                result += (sameAs != nullptr) ? " from " + target : std::string();
                result += ": " + std::to_string(difference.pixels) + " pixels, up to " + std::to_string(difference.maxLevels) + " levels, ";
                result += SaveImage(pixels, scenario.width, scenario.height, scenario.name + "_" + variant.name);
                result += " and " + SaveImage(*expected, scenario.width, scenario.height, scenario.name + "_" + target);
            }

            results.push_back(result);
            targets.push_back(target);
            differences.push_back(difference);
        }

        const std::vector<Timing> timings        = Measure(scenario, pixels.data(), variants);
        double                    baselineMedian = 0.0;
        for (size_t i = 0; i < timings.size(); ++i) {
            // By name: the linker may merge two variants with the same machine code into one address.
            if (strcmp(variants[i]->name, "Baseline") == 0) {
                baselineMedian = timings[i].median;
            }
        }

        printf("    %-28s %12s %12s %12s  %s\n", "Variant", "Median us", "Min us", "vs Baseline", "Pixels");
        for (size_t i = 0; i < timings.size(); ++i) {
            const Timing &timing = timings[i];
            printf("    %-28s %12.1f %12.1f %12.2f  %s\n", variants[i]->name, timing.median * 1e6, timing.min * 1e6,
                   baselineMedian / timing.median, results[i].c_str());
        }
        printf("\n");

        if (csv == nullptr) {
            return;
        }
        for (size_t i = 0; i < timings.size(); ++i) {
            const Timing &timing = timings[i];
            fprintf(csv, "%s,%s,%s,%s,%zu,%zu,%s,%.3f,%.3f,%.2f,%s,%s,%zu,%u,\"",
                    CsvText(compiler).c_str(), CsvText(processor).c_str(), CsvText(scenario.name).c_str(), alpha8 ? "Alpha8" : "BGRA32Premultiplied",
                    scenario.quads.size(), pixelCount, variants[i]->name, timing.median * 1e6, timing.min * 1e6,
                    (baselineMedian / timing.median - 1.0) * 100.0, (differences[i].pixels == 0) ? "same" : "different",
                    targets[i].c_str(), differences[i].pixels, unsigned(differences[i].maxLevels));
            for (size_t sample = 0; sample < timing.samples.size(); ++sample) {
                fprintf(csv, "%s%.3f", (sample == 0) ? "" : " ", timing.samples[sample] * 1e6);
            }
            fprintf(csv, "\"\n");
        }
    }

    //---------------------------------
    bool
    FileExists(const std::string &path) {
        return std::ifstream(path, std::ios::binary).good();
    }

    //---------------------------------
    void
    PrintUsage() {
        printf("Usage: benchmarkDraw [variant...] [--scenario text]... [--csv file]\n"
               "       benchmarkDraw --name\n"
               "  Without variants, it measures all of them but the ones that only run when named. Reference and Baseline\n"
               "  are always measured.\n"
               "  --scenario keeps the scenarios whose name has the text. With several, a scenario must have all of them.\n"
               "  --csv also writes the results, with every sample, for benchmarks/scripts/plot_benchmark.py.\n"
               "  --name prints a name for the files of the results, made of the system, the compiler and the processor.\n"
               "Variants:");
        for (const Variant &variant : kVariants) {
            printf(" %s", variant.name);
        }
        printf("\nOnly when named:");
        for (const Variant &variant : kNamedOnly) {
            printf(" %s", variant.name);
        }
        printf("\n");
    }

    // An unknown variant stops the program, so that a typo does not measure something else without notice.
    //---------------------------------
    bool
    ParseArguments(int argc, char *argv[], Selection &variants, std::vector<std::string> &scenarioFilters, std::string &csvPath) {
        Selection named;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--scenario") == 0 || strcmp(argv[i], "--csv") == 0) {
                if (i + 1 >= argc) {
                    return false;
                }
                if (strcmp(argv[i], "--csv") == 0) {
                    csvPath = argv[++i];
                }
                else {
                    scenarioFilters.push_back(argv[++i]);
                }
                continue;
            }

            const Variant *variant = FindVariant(argv[i]);
            if (variant == nullptr) {
                fprintf(stderr, "Unknown variant: %s\n", argv[i]);
                return false;
            }
            named.push_back(variant);
        }

        if (named.empty()) {
            for (const Variant &variant : kVariants) {
                variants.push_back(&variant);
            }
            return true;
        }

        for (const char *always : { "Reference", "Baseline" }) {
            variants.push_back(FindVariant(always));
        }
        for (const Variant *variant : named) {
            if (std::find(variants.begin(), variants.end(), variant) == variants.end()) {
                variants.push_back(variant);
            }
        }

        return true;
    }

    //---------------------------------
    bool
    MatchesAll(const std::string &name, const std::vector<std::string> &filters) {
        for (const std::string &filter : filters) {
            if (name.find(filter) == std::string::npos) {
                return false;
            }
        }

        return true;
    }

} // end of namespace

//-------------------------------------
int
main(int argc, char *argv[]) {
    if (argc == 2 && strcmp(argv[1], "--name") == 0) {
        printf("%s\n", GetRunName().c_str());
        return 0;
    }

    Selection                variants;
    std::vector<std::string> scenarioFilters;
    std::string              csvPath;
    if (ParseArguments(argc, argv, variants, scenarioFilters, csvPath) == false) {
        PrintUsage();
        return -1;
    }

    KeepSystemAwake();

    const std::string compiler  = GetCompiler();
    const std::string processor = GetProcessor();
    printf("Compiler: %s\n", compiler.c_str());
    printf("Processor: %s\n\n", processor.empty() ? "unknown" : processor.c_str());
#if !defined(NDEBUG)
    printf("This is not a Release build: the times say little about the real speed.\n\n");
#endif

    std::unique_ptr<FILE, int (*)(FILE *)> csv(nullptr, fclose);
    if (csvPath.empty() == false) {
        csv.reset(fopen(csvPath.c_str(), "w"));
        if (csv == nullptr) {
            fprintf(stderr, "Cannot write %s.\n", csvPath.c_str());
            return -1;
        }
        fprintf(csv.get(), "compiler,processor,scenario,format,glyphs,pixels,variant,median_us,min_us,speed_pct,pixels_check,compared_with,"
                           "different_pixels,different_levels,samples_us\n");
    }

    const std::string robotoPath = std::string(kResourcesPath) + "Roboto-Regular.ttf";

    // One font per scenario, so that each atlas only has the glyphs of its scenario and their sizes can be compared.
    std::vector<std::unique_ptr<FontBase>> fonts;
    std::vector<Scenario>                  scenarios;
    const struct {
        uint8_t size;
        int     lines;
        int     columns;
    } kRasterTexts[] = {
        { 14, 60, 100 },
        { 24, 35,  60 },
        { 40, 20,  40 },
        { 96,  8,  24 },
    };

    for (const auto &rasterText : kRasterTexts) {
        for (bool rotation : { true, false }) {
            std::unique_ptr<RasterFont> font(new RasterFont(robotoPath.c_str()));
            if (font->GetStatus() != FontBase::EStatus::Ok) {
                fprintf(stderr, "Cannot load %s (status %d).\n", robotoPath.c_str(), int(font->GetStatus()));
                return -1;
            }

            Scenario scenario;
            scenario.name = "Roboto " + std::to_string(rasterText.size) + " px, " + (rotation ? "rotation" : "no rotation");
            scenario.text = MakeText(rasterText.lines, rasterText.columns);
            scenario.size = rasterText.size;
            font->SetAllowRotation(rotation);
            // Packing every glyph at once, as a baked font does, gives the atlas its final size before the times start.
            if (font->Preload(scenario.text.c_str(), scenario.size) == false) {
                fprintf(stderr, "%s: some glyphs do not fit in the texture.\n", scenario.name.c_str());
                return -1;
            }
            scenario.font = font.get();
            fonts.push_back(std::move(font));
            scenarios.push_back(std::move(scenario));
        }
    }

#if defined(FONTRENDERER_USE_BAKED)
    // exampleBakeEffects bakes this font without rotation, and tools/add_effects.py adds the effects to its texture.
    const std::string metricsPath = std::string(kResourcesPath) + "effects.frb";
    const std::string effectsPath = std::string(kResourcesPath) + "effectfx.tga";
    const std::string plainPath   = std::string(kResourcesPath) + "effects.tga";
    const std::string texturePath = FileExists(effectsPath) ? effectsPath : plainPath;
    std::unique_ptr<FontBaked> baked(new FontBaked(metricsPath.c_str(), texturePath.c_str()));
    if (baked->GetStatus() == FontBase::EStatus::Ok) {
        Scenario scenario;
        scenario.name = "LilitaOne 32 px baked, no rotation";
        scenario.text = MakeText(20, 60);
        scenario.size = 32;
        scenario.font = baked.get();
        fonts.push_back(std::move(baked));
        scenarios.push_back(std::move(scenario));
    }
    else {
        printf("Cannot load %s and %s (status %d): the baked font is not measured.\n\n",
               metricsPath.c_str(), texturePath.c_str(), int(baked->GetStatus()));
    }
#endif

    int measured = 0;
    for (const Scenario &scenario : scenarios) {
        for (const auto &color : kColors) {
            Scenario colored = scenario;
            colored.name    += std::string(", ") + color.name;
            colored.color    = color.color;
            if (MatchesAll(colored.name, scenarioFilters)) {
                Run(colored, variants, csv.get(), compiler, processor);
                ++measured;
            }
        }
    }
    if (measured == 0) {
        fprintf(stderr, "No scenario has every --scenario text.\n");
        return -1;
    }

    return 0;
}
