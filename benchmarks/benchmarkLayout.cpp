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
#include <GlyphDraw.h>
#include <UTF8_Utils.h>
//-------------------------------------
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <math.h>      // ::lround, as DJGPP has no std::lround
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Measures the layout of FontBase: what DrawText does for each character besides drawing the glyph. It compares the
// layout of the library with copies of it:
//
//   Old          The layout before FlatMap: the tables in std::unordered_map, a virtual call for each lookup, the glyph
//                of the next character looked up again for the kerning, and lround, which MSVC and clang-cl call in the
//                C runtime. It keeps tables of its own, filled from the library.
//   Copy         The layout of the library, to tell how much of a difference comes from where the code is placed.
//   ByValue        Copy, with the visitor of DrawText taking its variables by value instead of by reference. MSVC does
//                  not inline LayoutText into DrawText, and then reads each variable through a pointer for each glyph.
//   Inline         Copy, with LayoutText always inline, as GCC does on its own.
//   Branchy        Copy, with the lookups in tables of its own whose Find exits its loop when it finds the key. Written
//                  so, clang keeps the jumps; with FlatMap::Find it turns them into conditional moves, so the value
//                  cannot be read until the comparison ends.
//   InlineBranchy  Inline and Branchy.
//   InlineBranchyByValue  Inline, Branchy and ByValue.
//
// Each layout is measured alone, as GetTextBox does it, and inside DrawText. The "labels" scenarios draw the same text
// one line per call, as a user interface draws its labels. The last rows measure the tables alone, with the keys the
// text looks up: a loop that only looks up keys lets the processor overlap the lookups more than the layout does.

using namespace MindShake;
using namespace Benchmark;

// Not an anonymous namespace: with one, GCC knows that no class derives from LayoutFont, and calls the functions of the
// font directly, which the library and Old call through the virtual table.
//-------------------------------------
namespace LayoutBenchmark {

#if defined(FONTRENDERER_USE_STB)
    using RasterFont = FontSTB;
#elif defined(FONTRENDERER_USE_LIBSCHRIFT)
    using RasterFont = FontSFT;
#else
    using RasterFont = FontFT;
#endif

    constexpr const char *kResourcesPath    = FONT_RENDERER_BENCHMARK_RESOURCES;

    constexpr int32_t     kMargin           = 8;
    constexpr int         kSamples          = 15;
    constexpr double      kMinSampleSeconds = 0.02;
    // Opaque, as most text is. It only matters for the rows that draw.
    constexpr uint32_t    kColor            = 0xffffc040u;

    // Keeps the sums of the lookups, so that the compiler cannot drop the loops.
    volatile uint64_t     gSink;

    //---------------------------------
    enum class ELayout {
        Old,
        Copy,
        ByValue,
        Inline,
        Branchy,
        InlineBranchy,
        InlineBranchyByValue
    };

    template <ELayout kLayout>
    using LayoutTag = std::integral_constant<ELayout, kLayout>;

    // A copy of GetNextUTF32 that is always inline, as it is in the layout of the library. GCC did not inline it into
    // the copies of the layout.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE uint32_t
    NextCodePoint(const uint8_t **text) {
        if (text == nullptr || *text == nullptr || **text == 0) {
            return 0;
        }

        const uint8_t   *bytes = *text;
        const uint8_t   lead   = bytes[0];
        uint32_t        codePoint;
        size_t          length;

        if (lead < 0x80) {
            *text += 1;
            return lead;
        }
        else if ((lead & 0xE0) == 0xC0) {
            codePoint = lead & 0x1F;
            length    = 2;
        }
        else if ((lead & 0xF0) == 0xE0) {
            codePoint = lead & 0x0F;
            length    = 3;
        }
        else if ((lead & 0xF8) == 0xF0) {
            codePoint = lead & 0x07;
            length    = 4;
        }
        else {
            *text += 1;
            return kReplacementCharacter;
        }

        for (size_t i = 1; i < length; ++i) {
            if ((bytes[i] & 0xC0) != 0x80) {
                *text += i;
                return kReplacementCharacter;
            }
            codePoint = (codePoint << 6) | (bytes[i] & 0x3F);
        }

        const uint32_t minimum = (length == 2) ? 0x80 : (length == 3) ? 0x800 : 0x10000;
        if (codePoint < minimum || (codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) {
            codePoint = kReplacementCharacter;
        }

        *text += length;
        return codePoint;
    }

    // A copy of the rounding of FontBase.cpp.
    //---------------------------------
    FONTRENDERER_ALWAYS_INLINE int32_t
    RoundPen(float penX) {
        return int32_t(penX + ((penX < 0.0f) ? -0.49999997f : 0.49999997f));
    }

    // FlatMap with the Find of Branchy.
    //---------------------------------
    template <class TValue>
    class BranchyFlatMap : public FlatMap<TValue> {
        public:
            //-------------------------
            FONTRENDERER_ALWAYS_INLINE const TValue *
            Find(uint32_t key) const {
                uint32_t slot = this->SlotOf(key);
                while (this->mSlots[slot].key != key) {
                    if (this->mSlots[slot].key == FlatMap<TValue>::kEmpty) {
                        return nullptr;
                    }
                    slot = (slot + 1) & this->mMask;
                }
                return &this->mSlots[slot].value;
            }
    };

    // StableMap with the Find of Branchy. The key must not be in the map when it is inserted.
    //---------------------------------
    template <class TValue>
    class BranchyStableMap {
        public:
            //-------------------------
            FONTRENDERER_ALWAYS_INLINE const TValue *
            Find(uint32_t key) const {
                const uint32_t *index = mIndices.Find(key);
                return (index != nullptr) ? &mBlocks[*index >> 5][*index & 31].value : nullptr;
            }

            //-------------------------
            const TValue &
            Insert(uint32_t key, const TValue &value) {
                if ((mCount & 31) == 0) {
                    mBlocks.emplace_back(new Entry[32]);
                }
                Entry &entry = mBlocks.back()[mCount & 31];
                entry.key   = key;
                entry.value = value;
                mIndices.Insert(key, uint32_t(mCount));
                ++mCount;
                return entry.value;
            }

        protected:
            //-------------------------
            struct Entry {
                uint32_t    key {};
                TValue      value {};
            };

        protected:
            BranchyFlatMap<uint32_t>                mIndices;
            std::vector<std::unique_ptr<Entry[]>>   mBlocks;
            size_t                                  mCount {};
    };

    // The lookups of the old layout, which were virtual functions of FontBase.
    //---------------------------------
    class OldTables {
        public:
            virtual                             ~OldTables() = default;

            virtual const CodePointHeightData & GetOldCodePointData(uint32_t codePoint, uint8_t height) = 0;
            virtual uint32_t                    GetOldCodePointGlyph(uint32_t codePoint) = 0;
            virtual int                         GetOldKerning(uint32_t leftGlyph, uint32_t rightGlyph) = 0;
    };

    //---------------------------------
    class LayoutProbe {
        public:
            virtual                     ~LayoutProbe() = default;

            virtual FontBase &          GetFont() = 0;
            virtual int32_t             GetLineAdvance(uint8_t textHeight) = 0;
            virtual void                Box(ELayout layout, const char *utf8, uint8_t textHeight, FontBase::Rect *rect) = 0;
            virtual void                Quads(ELayout layout, const char *utf8, uint8_t textHeight, std::vector<GlyphQuad> &quads) = 0;
            virtual void                Draw(ELayout layout, const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY) = 0;

            // The keys the layout looks up: one per character but '\n', and one pair per two glyphs in a row.
            virtual void                RecordKeys(const char *utf8, uint8_t textHeight, std::vector<uint32_t> &glyphKeys, std::vector<uint32_t> &kerningPairs) = 0;
            virtual uint64_t            SumStdGlyphs(const std::vector<uint32_t> &glyphKeys) const = 0;
            virtual uint64_t            SumFlatGlyphs(const std::vector<uint32_t> &glyphKeys) const = 0;
            virtual uint64_t            SumStdKerning(const std::vector<uint32_t> &kerningPairs) const = 0;
            virtual uint64_t            SumFlatKerning(const std::vector<uint32_t> &kerningPairs) const = 0;
    };

    //---------------------------------
    template <class TFont>
    class LayoutFont : public TFont, public LayoutProbe, public OldTables {
        public:
            // kerningComplete tells that the font has every kerning pair that is not 0, as FontBaked has: the old
            // FontBaked did not add the pairs it did not find, and the old Font did.
            template <class... TArgs>
            explicit LayoutFont(bool kerningComplete, TArgs &&...args) : TFont(std::forward<TArgs>(args)...), mKerningComplete(kerningComplete) {
                if (kerningComplete) {
                    this->mKerningData.ForEach([this](uint32_t key, int32_t kerning) {
                        mOldKerning[(uint64_t(key >> 16) << 32) | (key & 0xffff)] = kerning;
                        mBranchyKerning.Insert(key, kerning);
                    });
                }
            }

            FontBase &  GetFont() override                          { return *this;                                     }
            int32_t     GetLineAdvance(uint8_t textHeight) override { return this->GetDataForHeight(textHeight).GetLineAdvance(); }

            //-------------------------
            void
            Box(ELayout layout, const char *utf8, uint8_t textHeight, FontBase::Rect *rect) override {
                switch (layout) {
                    case ELayout::Old:
                        BoxWith(LayoutTag<ELayout::Old>(), utf8, textHeight, rect);
                        break;

                    case ELayout::Copy:
                        BoxWith(LayoutTag<ELayout::Copy>(), utf8, textHeight, rect);
                        break;

                    case ELayout::ByValue:
                        BoxWith(LayoutTag<ELayout::ByValue>(), utf8, textHeight, rect);
                        break;

                    case ELayout::Inline:
                        BoxWith(LayoutTag<ELayout::Inline>(), utf8, textHeight, rect);
                        break;

                    case ELayout::Branchy:
                        BoxWith(LayoutTag<ELayout::Branchy>(), utf8, textHeight, rect);
                        break;

                    case ELayout::InlineBranchy:
                        BoxWith(LayoutTag<ELayout::InlineBranchy>(), utf8, textHeight, rect);
                        break;

                    case ELayout::InlineBranchyByValue:
                        BoxWith(LayoutTag<ELayout::InlineBranchyByValue>(), utf8, textHeight, rect);
                        break;
                }
            }

            //-------------------------
            void
            Quads(ELayout layout, const char *utf8, uint8_t textHeight, std::vector<GlyphQuad> &quads) override {
                switch (layout) {
                    case ELayout::Old:
                        QuadsWith(LayoutTag<ELayout::Old>(), utf8, textHeight, quads);
                        break;

                    case ELayout::Copy:
                        QuadsWith(LayoutTag<ELayout::Copy>(), utf8, textHeight, quads);
                        break;

                    case ELayout::ByValue:
                        QuadsWith(LayoutTag<ELayout::ByValue>(), utf8, textHeight, quads);
                        break;

                    case ELayout::Inline:
                        QuadsWith(LayoutTag<ELayout::Inline>(), utf8, textHeight, quads);
                        break;

                    case ELayout::Branchy:
                        QuadsWith(LayoutTag<ELayout::Branchy>(), utf8, textHeight, quads);
                        break;

                    case ELayout::InlineBranchy:
                        QuadsWith(LayoutTag<ELayout::InlineBranchy>(), utf8, textHeight, quads);
                        break;

                    case ELayout::InlineBranchyByValue:
                        QuadsWith(LayoutTag<ELayout::InlineBranchyByValue>(), utf8, textHeight, quads);
                        break;
                }
            }

            //-------------------------
            void
            Draw(ELayout layout, const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY) override {
                switch (layout) {
                    case ELayout::Old:
                        DrawWith(LayoutTag<ELayout::Old>(), utf8, textHeight, color, dst, dstStride, posX, posY);
                        break;

                    case ELayout::Copy:
                        DrawWith(LayoutTag<ELayout::Copy>(), utf8, textHeight, color, dst, dstStride, posX, posY);
                        break;

                    case ELayout::ByValue:
                        DrawWith(LayoutTag<ELayout::ByValue>(), utf8, textHeight, color, dst, dstStride, posX, posY);
                        break;

                    case ELayout::Inline:
                        DrawWith(LayoutTag<ELayout::Inline>(), utf8, textHeight, color, dst, dstStride, posX, posY);
                        break;

                    case ELayout::Branchy:
                        DrawWith(LayoutTag<ELayout::Branchy>(), utf8, textHeight, color, dst, dstStride, posX, posY);
                        break;

                    case ELayout::InlineBranchy:
                        DrawWith(LayoutTag<ELayout::InlineBranchy>(), utf8, textHeight, color, dst, dstStride, posX, posY);
                        break;

                    case ELayout::InlineBranchyByValue:
                        DrawWith(LayoutTag<ELayout::InlineBranchyByValue>(), utf8, textHeight, color, dst, dstStride, posX, posY);
                        break;
                }
            }

            //-------------------------
            void
            RecordKeys(const char *utf8, uint8_t textHeight, std::vector<uint32_t> &glyphKeys, std::vector<uint32_t> &kerningPairs) override {
                const uint8_t *text          = reinterpret_cast<const uint8_t *>(utf8);
                uint32_t      previousGlyph  = 0;
                for (uint32_t codePoint = GetNextUTF32(&text); codePoint != 0; codePoint = GetNextUTF32(&text)) {
                    if (codePoint == '\n') {
                        previousGlyph = 0;
                        continue;
                    }

                    glyphKeys.push_back(FontBase::GetCodePointHeightKey(codePoint, textHeight));
                    const uint32_t glyph = this->GetCodePointDataForHeight(codePoint, textHeight).glyph;
                    if (glyph > 0 && previousGlyph > 0) {
                        kerningPairs.push_back(FontBase::GetKerningKey(previousGlyph, glyph));
                    }
                    previousGlyph = glyph;
                }
            }

            //-------------------------
            uint64_t
            SumStdGlyphs(const std::vector<uint32_t> &glyphKeys) const override {
                uint64_t sum = 0;
                for (uint32_t key : glyphKeys) {
                    const auto entry = mOldCodePointData.find(key);
                    if (entry != mOldCodePointData.end()) {
                        sum += entry->second.glyph;
                    }
                }
                return sum;
            }

            //-------------------------
            uint64_t
            SumFlatGlyphs(const std::vector<uint32_t> &glyphKeys) const override {
                uint64_t sum = 0;
                for (uint32_t key : glyphKeys) {
                    const CodePointHeightData *data = this->mCodePointHeightData.Find(key);
                    if (data != nullptr) {
                        sum += data->glyph;
                    }
                }
                return sum;
            }

            //-------------------------
            uint64_t
            SumStdKerning(const std::vector<uint32_t> &kerningPairs) const override {
                uint64_t sum = 0;
                for (uint32_t pair : kerningPairs) {
                    const auto entry = mOldKerning.find((uint64_t(pair >> 16) << 32) | (pair & 0xffff));
                    if (entry != mOldKerning.end()) {
                        sum += uint32_t(entry->second);
                    }
                }
                return sum;
            }

            //-------------------------
            uint64_t
            SumFlatKerning(const std::vector<uint32_t> &kerningPairs) const override {
                uint64_t sum = 0;
                for (uint32_t pair : kerningPairs) {
                    const int32_t *kerning = this->mKerningData.Find(pair);
                    if (kerning != nullptr) {
                        sum += uint32_t(*kerning);
                    }
                }
                return sum;
            }

            //-------------------------
            const CodePointHeightData &
            GetOldCodePointData(uint32_t codePoint, uint8_t height) override {
                const uint32_t key   = FontBase::GetCodePointHeightKey(codePoint, height);
                const auto     entry = mOldCodePointData.find(key);
                if (entry != mOldCodePointData.end()) {
                    return entry->second;
                }

                mOldHeight = height;
                return mOldCodePointData.insert({ key, this->GetCodePointDataForHeight(codePoint, height) }).first->second;
            }

            // The old Font looked it up without the height, in a table of its own.
            //-------------------------
            uint32_t
            GetOldCodePointGlyph(uint32_t codePoint) override {
                const auto entry = mOldCodePointGlyphs.find(codePoint);
                if (entry != mOldCodePointGlyphs.end()) {
                    return entry->second;
                }

                const uint32_t glyph = uint32_t(this->GetCodePointDataForHeight(codePoint, mOldHeight).glyph);
                mOldCodePointGlyphs.insert({ codePoint, glyph });
                return glyph;
            }

            //-------------------------
            int
            GetOldKerning(uint32_t leftGlyph, uint32_t rightGlyph) override {
                const uint64_t key   = (uint64_t(leftGlyph) << 32) | rightGlyph;
                const auto     entry = mOldKerning.find(key);
                if (entry != mOldKerning.end()) {
                    return entry->second;
                }
                if (mKerningComplete) {
                    return 0;
                }

                const int kerning = this->GetKerning(leftGlyph, rightGlyph);
                mOldKerning.insert({ key, kerning });
                return kerning;
            }

        protected:
            //-------------------------
            FONTRENDERER_ALWAYS_INLINE const CodePointHeightData &
            GetBranchyData(uint32_t codePoint, uint8_t height) {
                const uint32_t            key  = FontBase::GetCodePointHeightKey(codePoint, height);
                const CodePointHeightData *data = mBranchyCodePointData.Find(key);
                return (data != nullptr) ? *data : AddBranchyData(key, codePoint, height);
            }

            //-------------------------
            FONTRENDERER_NO_INLINE const CodePointHeightData &
            AddBranchyData(uint32_t key, uint32_t codePoint, uint8_t height) {
                return mBranchyCodePointData.Insert(key, this->GetCodePointDataForHeight(codePoint, height));
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE int
            GetBranchyKerning(uint32_t leftGlyph, uint32_t rightGlyph) {
                const uint32_t key      = FontBase::GetKerningKey(leftGlyph, rightGlyph);
                const int32_t  *kerning = mBranchyKerning.Find(key);
                return (kerning != nullptr) ? *kerning : AddBranchyKerning(key, leftGlyph, rightGlyph);
            }

            //-------------------------
            FONTRENDERER_NO_INLINE int
            AddBranchyKerning(uint32_t key, uint32_t leftGlyph, uint32_t rightGlyph) {
                if (mKerningComplete) {
                    return 0;
                }
                const int kerning = this->GetKerning(leftGlyph, rightGlyph);
                mBranchyKerning.Insert(key, kerning);
                return kerning;
            }

            //-------------------------
            template <ELayout kLayout>
            FONTRENDERER_ALWAYS_INLINE const CodePointHeightData &
            LookUpData(LayoutTag<kLayout>, uint32_t codePoint, uint8_t height) {
                return this->GetCodePointDataForHeight(codePoint, height);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE const CodePointHeightData &
            LookUpData(LayoutTag<ELayout::Branchy>, uint32_t codePoint, uint8_t height) {
                return GetBranchyData(codePoint, height);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE const CodePointHeightData &
            LookUpData(LayoutTag<ELayout::InlineBranchy>, uint32_t codePoint, uint8_t height) {
                return GetBranchyData(codePoint, height);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE const CodePointHeightData &
            LookUpData(LayoutTag<ELayout::InlineBranchyByValue>, uint32_t codePoint, uint8_t height) {
                return GetBranchyData(codePoint, height);
            }

            //-------------------------
            template <ELayout kLayout>
            FONTRENDERER_ALWAYS_INLINE int
            LookUpKerning(LayoutTag<kLayout>, uint32_t leftGlyph, uint32_t rightGlyph) {
                return this->GetKerning(leftGlyph, rightGlyph);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE int
            LookUpKerning(LayoutTag<ELayout::Branchy>, uint32_t leftGlyph, uint32_t rightGlyph) {
                return GetBranchyKerning(leftGlyph, rightGlyph);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE int
            LookUpKerning(LayoutTag<ELayout::InlineBranchy>, uint32_t leftGlyph, uint32_t rightGlyph) {
                return GetBranchyKerning(leftGlyph, rightGlyph);
            }

            //-------------------------
            FONTRENDERER_ALWAYS_INLINE int
            LookUpKerning(LayoutTag<ELayout::InlineBranchyByValue>, uint32_t leftGlyph, uint32_t rightGlyph) {
                return GetBranchyKerning(leftGlyph, rightGlyph);
            }

            // A copy of the old FontBase::LayoutText and FontBase::GetScaledKerning. The lookups go through OldTables,
            // as they went through the virtual functions of FontBase.
            //-------------------------
            template <class TVisitor>
            void
            Layout(LayoutTag<ELayout::Old>, const char *utf8, uint8_t textHeight, TVisitor visit) {
                OldTables        &old        = *mOld;
                const HeightData heightData = this->GetDataForHeight(textHeight);

                float   penX     = 0.0f;
                int32_t baseline = heightData.ascent;

                const uint8_t *text = reinterpret_cast<const uint8_t *>(utf8);
                for (uint32_t codePoint = NextCodePoint(&text), nextCodePoint; codePoint != 0; codePoint = nextCodePoint) {
                    nextCodePoint = NextCodePoint(&text);
                    if (codePoint == '\n') {
                        penX      = 0.0f;
                        baseline += heightData.GetLineAdvance();
                        continue;
                    }

                    const CodePointHeightData &data = old.GetOldCodePointData(codePoint, textHeight);
                    if (data.glyph > 0) {
                        visit(data, penX, baseline);
                        const float kerning = (nextCodePoint == 0) ? 0.0f :
                                              float(old.GetOldKerning(data.glyph, old.GetOldCodePointGlyph(nextCodePoint))) * heightData.scale;
                        penX += data.advanceWidth + kerning;
                    }
                }
            }

            // The compiler decides whether it inlines it, as with FontBase::LayoutText.
            //-------------------------
            template <ELayout kLayout, class TVisitor>
            void
            Layout(LayoutTag<kLayout> tag, const char *utf8, uint8_t textHeight, TVisitor visit) {
                LayoutBody(tag, utf8, textHeight, visit);
            }

            //-------------------------
            template <class TVisitor>
            FONTRENDERER_ALWAYS_INLINE void
            Layout(LayoutTag<ELayout::Inline> tag, const char *utf8, uint8_t textHeight, TVisitor visit) {
                LayoutBody(tag, utf8, textHeight, visit);
            }

            //-------------------------
            template <class TVisitor>
            FONTRENDERER_ALWAYS_INLINE void
            Layout(LayoutTag<ELayout::InlineBranchy> tag, const char *utf8, uint8_t textHeight, TVisitor visit) {
                LayoutBody(tag, utf8, textHeight, visit);
            }

            //-------------------------
            template <class TVisitor>
            FONTRENDERER_ALWAYS_INLINE void
            Layout(LayoutTag<ELayout::InlineBranchyByValue> tag, const char *utf8, uint8_t textHeight, TVisitor visit) {
                LayoutBody(tag, utf8, textHeight, visit);
            }

            // A copy of FontBase::LayoutText, with the lookups of LookUpData and LookUpKerning.
            //-------------------------
            template <ELayout kLayout, class TVisitor>
            FONTRENDERER_ALWAYS_INLINE void
            LayoutBody(LayoutTag<kLayout> tag, const char *utf8, uint8_t textHeight, TVisitor &visit) {
                const HeightData heightData = this->GetDataForHeight(textHeight);

                float   penX     = 0.0f;
                int32_t baseline = heightData.ascent;

                auto lookUp = [&](uint32_t codePoint) -> const CodePointHeightData * {
                    return (codePoint != 0 && codePoint != '\n') ? &LookUpData(tag, codePoint, textHeight) : nullptr;
                };

                const uint8_t             *text     = reinterpret_cast<const uint8_t *>(utf8);
                uint32_t                  codePoint = NextCodePoint(&text);
                const CodePointHeightData *data     = lookUp(codePoint);
                while (codePoint != 0) {
                    const uint32_t            nextCodePoint = NextCodePoint(&text);
                    const CodePointHeightData *nextData     = lookUp(nextCodePoint);
                    if (codePoint == '\n') {
                        penX      = 0.0f;
                        baseline += heightData.GetLineAdvance();
                    }
                    else if (data->glyph > 0) {
                        visit(*data, penX, baseline);
                        float kerning = 0.0f;
                        if (nextData != nullptr && nextData->glyph > 0) {
                            kerning = float(LookUpKerning(tag, data->glyph, nextData->glyph)) * heightData.scale;
                        }
                        penX += data->advanceWidth + kerning;
                    }

                    codePoint = nextCodePoint;
                    data      = nextData;
                }
            }

            //-------------------------
            static int32_t
            Round(LayoutTag<ELayout::Old>, float penX) {
                return int32_t(::lround(penX));
            }

            //-------------------------
            template <ELayout kLayout>
            static int32_t
            Round(LayoutTag<kLayout>, float penX) {
                return RoundPen(penX);
            }

            // A copy of FontBase::GetTextBox.
            //-------------------------
            template <ELayout kLayout>
            FONTRENDERER_NO_INLINE void
            BoxWith(LayoutTag<kLayout> tag, const char *utf8, uint8_t textHeight, FontBase::Rect *rect) {
                if (rect == nullptr) {
                    return;
                }

                *rect = {};
                if (utf8 == nullptr || textHeight == 0) {
                    return;
                }

                int32_t minX = INT32_MAX, maxX = INT32_MIN;
                int32_t minY = INT32_MAX, maxY = INT32_MIN;

                Layout(tag, utf8, textHeight, [&](const CodePointHeightData &data, float penX, int32_t baseline) {
                    const int32_t roundedPenX = Round(tag, penX);
                    if (data.GetWidth() > 0) {
                        const int32_t left = roundedPenX + data.x;
                        const int32_t top  = baseline + data.y;

                        minX = std::min(minX, left);
                        maxX = std::max(maxX, left + data.GetWidth());
                        minY = std::min(minY, top);
                        maxY = std::max(maxY, top + data.GetHeight());
                    }
                    else {
                        minX = std::min(minX, roundedPenX);
                    }
                    maxX = std::max(maxX, Round(tag, penX + data.advanceWidth));
                });

                if (minX > maxX || minY > maxY) {
                    return;
                }

                *rect = { minX, minY, maxX - minX, maxY - minY };
            }

            // A copy of FontBase::GetGlyphQuads.
            //-------------------------
            template <ELayout kLayout>
            FONTRENDERER_NO_INLINE void
            QuadsWith(LayoutTag<kLayout> tag, const char *utf8, uint8_t textHeight, std::vector<GlyphQuad> &quads) {
                quads.clear();
                if (utf8 == nullptr || textHeight == 0) {
                    return;
                }

                Layout(tag, utf8, textHeight, [&](const CodePointHeightData &data, float penX, int32_t baseline) {
                    if (data.GetWidth() <= 0) {
                        return;
                    }

                    GlyphQuad quad;
                    quad.x           = Round(tag, penX) + data.x;
                    quad.y           = baseline + data.y;
                    quad.width       = data.GetWidth();
                    quad.height      = data.GetHeight();
                    quad.textureRect = data.rect;
                    quad.rotated     = data.rotated;
                    quads.push_back(quad);
                });
            }

            // A copy of FontBase::DrawText.
            //-------------------------
            template <ELayout kLayout>
            FONTRENDERER_NO_INLINE void
            DrawWith(LayoutTag<kLayout> tag, const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY) {
                if (utf8 == nullptr || textHeight == 0 || this->mTexture.empty()) {
                    return;
                }

                const uint32_t                     premultiplied = GlyphDraw::PremultiplyColor(color);
                const uint32_t                     colorAlpha    = color >> 24;
                const GlyphDraw::DrawGlyphFunction drawGlyph     = GlyphDraw::GetDrawGlyphFunction(FontBase::GetBytesPerTexel(this->mTextureFormat), colorAlpha == 255);

                Layout(tag, utf8, textHeight, [&](const CodePointHeightData &data, float penX, int32_t baseline) {
                    int32_t currentY = posY + baseline + data.y;
                    int32_t minY     = 0;
                    if (currentY < this->mTop) {
                        minY    += this->mTop - currentY;
                        currentY = this->mTop;
                    }

                    if (currentY >= this->mBottom) {
                        return;
                    }

                    int32_t currentX = posX + data.x + Round(tag, penX);
                    int32_t minX     = 0;
                    if (currentX < this->mLeft) {
                        minX    += this->mLeft - currentX;
                        currentX = this->mLeft;
                    }

                    if (currentX >= this->mRight) {
                        return;
                    }

                    int32_t maxX = data.GetWidth();
                    if (currentX + maxX - minX >= this->mRight) {
                        maxX = minX + this->mRight - currentX;
                    }

                    int32_t maxY = data.GetHeight();
                    if (currentY + maxY - minY >= this->mBottom) {
                        maxY = minY + this->mBottom - currentY;
                    }

                    const size_t textureWidth  = this->mTextureWidth;
                    const size_t stepX         = data.rotated ? textureWidth : 1;
                    const size_t stepY         = data.rotated ? 1 : textureWidth;
                    const size_t offsetTexture = size_t(data.rect.y) * textureWidth + size_t(data.rect.x) + size_t(minY) * stepY + size_t(minX) * stepX;
                    uint32_t     *dstGlyph     = &dst[size_t(currentY) * dstStride + size_t(currentX)];
                    drawGlyph(this->mTexture.data(), offsetTexture, stepX, stepY, maxX - minX, maxY - minY, dstGlyph, dstStride, premultiplied, colorAlpha);
                });
            }

            // DrawWith, with the visitor taking its variables by value.
            //-------------------------
            FONTRENDERER_NO_INLINE void
            DrawWith(LayoutTag<ELayout::ByValue> tag, const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY) {
                if (utf8 == nullptr || textHeight == 0 || this->mTexture.empty()) {
                    return;
                }

                const uint32_t                     premultiplied = GlyphDraw::PremultiplyColor(color);
                const uint32_t                     colorAlpha    = color >> 24;
                const GlyphDraw::DrawGlyphFunction drawGlyph     = GlyphDraw::GetDrawGlyphFunction(FontBase::GetBytesPerTexel(this->mTextureFormat), colorAlpha == 255);

                Layout(tag, utf8, textHeight, [this, tag, dst, dstStride, posX, posY, premultiplied, colorAlpha, drawGlyph](const CodePointHeightData &data, float penX, int32_t baseline) {
                    int32_t currentY = posY + baseline + data.y;
                    int32_t minY     = 0;
                    if (currentY < this->mTop) {
                        minY    += this->mTop - currentY;
                        currentY = this->mTop;
                    }

                    if (currentY >= this->mBottom) {
                        return;
                    }

                    int32_t currentX = posX + data.x + Round(tag, penX);
                    int32_t minX     = 0;
                    if (currentX < this->mLeft) {
                        minX    += this->mLeft - currentX;
                        currentX = this->mLeft;
                    }

                    if (currentX >= this->mRight) {
                        return;
                    }

                    int32_t maxX = data.GetWidth();
                    if (currentX + maxX - minX >= this->mRight) {
                        maxX = minX + this->mRight - currentX;
                    }

                    int32_t maxY = data.GetHeight();
                    if (currentY + maxY - minY >= this->mBottom) {
                        maxY = minY + this->mBottom - currentY;
                    }

                    const size_t textureWidth  = this->mTextureWidth;
                    const size_t stepX         = data.rotated ? textureWidth : 1;
                    const size_t stepY         = data.rotated ? 1 : textureWidth;
                    const size_t offsetTexture = size_t(data.rect.y) * textureWidth + size_t(data.rect.x) + size_t(minY) * stepY + size_t(minX) * stepX;
                    uint32_t     *dstGlyph     = &dst[size_t(currentY) * dstStride + size_t(currentX)];
                    drawGlyph(this->mTexture.data(), offsetTexture, stepX, stepY, maxX - minX, maxY - minY, dstGlyph, dstStride, premultiplied, colorAlpha);
                });
            }

            // The DrawWith of ByValue, for InlineBranchyByValue.
            //-------------------------
            FONTRENDERER_NO_INLINE void
            DrawWith(LayoutTag<ELayout::InlineBranchyByValue> tag, const char *utf8, uint8_t textHeight, uint32_t color, uint32_t *dst, uint32_t dstStride, int32_t posX, int32_t posY) {
                if (utf8 == nullptr || textHeight == 0 || this->mTexture.empty()) {
                    return;
                }

                const uint32_t                     premultiplied = GlyphDraw::PremultiplyColor(color);
                const uint32_t                     colorAlpha    = color >> 24;
                const GlyphDraw::DrawGlyphFunction drawGlyph     = GlyphDraw::GetDrawGlyphFunction(FontBase::GetBytesPerTexel(this->mTextureFormat), colorAlpha == 255);

                Layout(tag, utf8, textHeight, [this, tag, dst, dstStride, posX, posY, premultiplied, colorAlpha, drawGlyph](const CodePointHeightData &data, float penX, int32_t baseline) {
                    int32_t currentY = posY + baseline + data.y;
                    int32_t minY     = 0;
                    if (currentY < this->mTop) {
                        minY    += this->mTop - currentY;
                        currentY = this->mTop;
                    }

                    if (currentY >= this->mBottom) {
                        return;
                    }

                    int32_t currentX = posX + data.x + Round(tag, penX);
                    int32_t minX     = 0;
                    if (currentX < this->mLeft) {
                        minX    += this->mLeft - currentX;
                        currentX = this->mLeft;
                    }

                    if (currentX >= this->mRight) {
                        return;
                    }

                    int32_t maxX = data.GetWidth();
                    if (currentX + maxX - minX >= this->mRight) {
                        maxX = minX + this->mRight - currentX;
                    }

                    int32_t maxY = data.GetHeight();
                    if (currentY + maxY - minY >= this->mBottom) {
                        maxY = minY + this->mBottom - currentY;
                    }

                    const size_t textureWidth  = this->mTextureWidth;
                    const size_t stepX         = data.rotated ? textureWidth : 1;
                    const size_t stepY         = data.rotated ? 1 : textureWidth;
                    const size_t offsetTexture = size_t(data.rect.y) * textureWidth + size_t(data.rect.x) + size_t(minY) * stepY + size_t(minX) * stepX;
                    uint32_t     *dstGlyph     = &dst[size_t(currentY) * dstStride + size_t(currentX)];
                    drawGlyph(this->mTexture.data(), offsetTexture, stepX, stepY, maxX - minX, maxY - minY, dstGlyph, dstStride, premultiplied, colorAlpha);
                });
            }

        protected:
            // Through a pointer, so that the compiler calls them through the virtual table, as the old layout did.
            OldTables                                       *mOld { this };
            std::unordered_map<uint32_t, CodePointHeightData> mOldCodePointData;
            std::unordered_map<uint32_t, uint32_t>          mOldCodePointGlyphs;
            std::unordered_map<uint64_t, int32_t>           mOldKerning;
            uint8_t                                         mOldHeight {};
            bool                                            mKerningComplete;

            BranchyStableMap<CodePointHeightData>           mBranchyCodePointData;
            BranchyFlatMap<int32_t>                         mBranchyKerning;
    };

    //---------------------------------
    struct Scenario {
        std::string                     name;
        std::unique_ptr<LayoutProbe>    probe;
        std::string                     text;
        uint8_t                         size;
        bool                            labels;     // One call per line of the text
    };

    //---------------------------------
    struct Item {
        const char              *name;
        size_t                  comparedWith;   // Index of the row its speed is compared with
        std::function<void()>   run;
        std::string             check;
    };

    //---------------------------------
    struct Timing {
        double  median;     // Seconds per call
        double  min;
    };

    //---------------------------------
    struct NamedLayout {
        const char  *box;
        const char  *draw;
        ELayout     layout;
    };

    const NamedLayout kLayouts[] = {
        { "BoxOld",           "DrawOld",           ELayout::Old           },
        { "BoxCopy",          "DrawCopy",          ELayout::Copy          },
        { "BoxByValue",       "DrawByValue",       ELayout::ByValue       },
        { "BoxInline",        "DrawInline",        ELayout::Inline        },
        { "BoxBranchy",       "DrawBranchy",       ELayout::Branchy       },
        { "BoxInlineBranchy", "DrawInlineBranchy", ELayout::InlineBranchy },
        { "BoxInlineBranchyByValue", "DrawInlineBranchyByValue", ELayout::InlineBranchyByValue },
    };

    // The same text as benchmarkDraw.
    //---------------------------------
    std::string
    MakeLatinText(int lines, int columns) {
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

    // Words of 6 lowercase Cyrillic and Greek letters, picked at random but always the same. Every code point is above
    // 255, and the random pairs ask the font for many more kerning pairs than the Latin text.
    //---------------------------------
    std::string
    MakeNonLatinText(int lines, int columns) {
        std::vector<uint32_t> letters;
        for (uint32_t c = 0x430; c <= 0x44f; ++c) {
            letters.push_back(c);
        }
        for (uint32_t c = 0x3b1; c <= 0x3c9; ++c) {
            letters.push_back(c);
        }

        std::string text;
        uint32_t    random = 1;
        for (int line = 0; line < lines; ++line) {
            for (int column = 0; column < columns; ++column) {
                if (column % 7 == 6) {
                    text += ' ';
                    continue;
                }

                random = random * 1664525u + 1013904223u;
                const uint32_t letter = letters[(random >> 16) % letters.size()];
                text += char(0xc0 | (letter >> 6));
                text += char(0x80 | (letter & 0x3f));
            }
            if (line + 1 < lines) {
                text += '\n';
            }
        }

        return text;
    }

    //---------------------------------
    std::vector<std::string>
    SplitLines(const std::string &text) {
        std::vector<std::string> lines;
        size_t                   start = 0;
        while (true) {
            const size_t end = text.find('\n', start);
            lines.push_back(text.substr(start, end - start));
            if (end == std::string::npos) {
                return lines;
            }
            start = end + 1;
        }
    }

    //---------------------------------
    bool
    FileExists(const std::string &path) {
        return std::ifstream(path, std::ios::binary).good();
    }

    //---------------------------------
    void
    FillBackground(std::vector<uint32_t> &pixels, uint32_t width) {
        for (size_t i = 0; i < pixels.size(); ++i) {
            const uint32_t x = uint32_t(i % width);
            const uint32_t y = uint32_t(i / width);
            pixels[i] = 0xff000000u | (((x * 3) & 0xff) << 16) | (((y * 5) & 0xff) << 8) | ((x ^ y) & 0xff);
        }
    }

    //---------------------------------
    bool
    SameQuads(const std::vector<GlyphQuad> &a, const std::vector<GlyphQuad> &b) {
        if (a.size() != b.size()) {
            return false;
        }

        for (size_t i = 0; i < a.size(); ++i) {
            const GlyphQuad &p = a[i];
            const GlyphQuad &q = b[i];
            if (p.x != q.x || p.y != q.y || p.width != q.width || p.height != q.height || p.rotated != q.rotated ||
               p.textureRect.x != q.textureRect.x || p.textureRect.y != q.textureRect.y ||
               p.textureRect.width != q.textureRect.width || p.textureRect.height != q.textureRect.height) {
                return false;
            }
        }

        return true;
    }

    //---------------------------------
    bool
    SameRect(const FontBase::Rect &a, const FontBase::Rect &b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }

    //---------------------------------
    double
    SecondsPerCall(const std::function<void()> &run, uint32_t calls) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < calls; ++i) {
            run();
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        return elapsed.count() / calls;
    }

    // As in benchmarkDraw: the calls of a sample add up to kMinSampleSeconds, and the samples of the rows take turns,
    // so that a moment in which the system is busy slows down all of them alike.
    //---------------------------------
    std::vector<Timing>
    Measure(const std::vector<Item> &items) {
        std::vector<uint32_t> calls(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            calls[i] = 1;
            while (SecondsPerCall(items[i].run, calls[i]) * calls[i] < kMinSampleSeconds) {
                calls[i] *= 2;
            }
        }

        std::vector<std::vector<double>> samples(items.size(), std::vector<double>(kSamples));
        for (int sample = 0; sample < kSamples; ++sample) {
            for (size_t i = 0; i < items.size(); ++i) {
                samples[i][sample] = SecondsPerCall(items[i].run, calls[i]);
            }
        }

        std::vector<Timing> timings;
        for (std::vector<double> &itemSamples : samples) {
            std::sort(itemSamples.begin(), itemSamples.end());
            timings.push_back({ itemSamples[kSamples / 2], itemSamples[0] });
        }

        return timings;
    }

    //---------------------------------
    void
    Run(Scenario &scenario) {
        LayoutProbe    &probe = *scenario.probe;
        FontBase       &font  = probe.GetFont();
        const uint8_t  size   = scenario.size;

        FontBase::Rect box;
        font.GetTextBox(scenario.text.c_str(), size, &box);
        if (box.width <= 0 || box.height <= 0) {
            printf("%s: nothing to draw\n\n", scenario.name.c_str());
            return;
        }

        // The text is drawn at a position that leaves the margin around its box, so no glyph needs clipping. Each line
        // of the labels is drawn where it is in the whole text.
        const int32_t  posX   = kMargin - box.x;
        const int32_t  posY   = kMargin - box.y;
        const uint32_t width  = uint32_t(box.width  + 2 * kMargin);
        const uint32_t height = uint32_t(box.height + 2 * kMargin);

        const std::vector<std::string> texts       = scenario.labels ? SplitLines(scenario.text) : std::vector<std::string>(1, scenario.text);
        const int32_t                  lineAdvance = probe.GetLineAdvance(size);

        std::vector<uint32_t> reference(size_t(width) * height);
        std::vector<uint32_t> pixels(reference.size());
        FillBackground(reference, width);
        for (size_t i = 0; i < texts.size(); ++i) {
            font.DrawText(texts[i].c_str(), size, kColor, reference.data(), width, posX, posY + int32_t(i) * lineAdvance);
        }

        // The checks also fill the tables of Old and Branchy, so the times leave out their first lookups, which ask the library.
        std::vector<std::string> boxChecks;
        std::vector<std::string> drawChecks;
        size_t                   glyphCount = 0;
        for (const NamedLayout &named : kLayouts) {
            bool same = true;
            for (const std::string &text : texts) {
                std::vector<GlyphQuad> quads;
                std::vector<GlyphQuad> layoutQuads;
                FontBase::Rect         textBox;
                FontBase::Rect         layoutBox;
                font.GetGlyphQuads(text.c_str(), size, quads);
                font.GetTextBox(text.c_str(), size, &textBox);
                probe.Quads(named.layout, text.c_str(), size, layoutQuads);
                probe.Box(named.layout, text.c_str(), size, &layoutBox);
                same       = same && SameQuads(quads, layoutQuads) && SameRect(textBox, layoutBox);
                glyphCount = (named.layout == ELayout::Old) ? glyphCount + quads.size() : glyphCount;
            }
            boxChecks.push_back(same ? "same" : "DIFFERENT glyphs");

            FillBackground(pixels, width);
            for (size_t i = 0; i < texts.size(); ++i) {
                probe.Draw(named.layout, texts[i].c_str(), size, kColor, pixels.data(), width, posX, posY + int32_t(i) * lineAdvance);
            }
            drawChecks.push_back((pixels == reference) ? "same" : "DIFFERENT pixels");
        }

        std::vector<uint32_t> glyphKeys;
        std::vector<uint32_t> kerningPairs;
        probe.RecordKeys(scenario.text.c_str(), size, glyphKeys, kerningPairs);
        const bool sameGlyphs  = probe.SumStdGlyphs(glyphKeys) == probe.SumFlatGlyphs(glyphKeys);
        const bool sameKerning = probe.SumStdKerning(kerningPairs) == probe.SumFlatKerning(kerningPairs);

        std::unordered_set<uint32_t> distinctGlyphs(glyphKeys.begin(), glyphKeys.end());
        std::unordered_set<uint32_t> distinctPairs(kerningPairs.begin(), kerningPairs.end());
        printf("%s: %zu calls, %zu characters, %zu glyphs with pixels, %zu distinct code points, %zu kerning pairs (%zu distinct)\n",
               scenario.name.c_str(), texts.size(), glyphKeys.size(), glyphCount, distinctGlyphs.size(), kerningPairs.size(), distinctPairs.size());

        // The buffer is not cleared between calls: blending over the previous text costs the same as over the background.
        std::vector<Item> items;
        items.push_back({ "GetTextBox", 0, [&]() {
            for (const std::string &text : texts) {
                FontBase::Rect r;
                font.GetTextBox(text.c_str(), size, &r);
            }
        }, "" });
        for (size_t i = 0; i < sizeof(kLayouts) / sizeof(kLayouts[0]); ++i) {
            const ELayout layout = kLayouts[i].layout;
            items.push_back({ kLayouts[i].box, 0, [&probe, &texts, layout, size]() {
                for (const std::string &text : texts) {
                    FontBase::Rect r;
                    probe.Box(layout, text.c_str(), size, &r);
                }
            }, boxChecks[i] });
        }

        const size_t drawText = items.size();
        items.push_back({ "DrawText", drawText, [&]() {
            for (size_t line = 0; line < texts.size(); ++line) {
                font.DrawText(texts[line].c_str(), size, kColor, pixels.data(), width, posX, posY + int32_t(line) * lineAdvance);
            }
        }, "" });
        for (size_t i = 0; i < sizeof(kLayouts) / sizeof(kLayouts[0]); ++i) {
            const ELayout layout = kLayouts[i].layout;
            uint32_t      *dst   = pixels.data();
            items.push_back({ kLayouts[i].draw, drawText, [&probe, &texts, layout, size, dst, width, posX, posY, lineAdvance]() {
                for (size_t line = 0; line < texts.size(); ++line) {
                    probe.Draw(layout, texts[line].c_str(), size, kColor, dst, width, posX, posY + int32_t(line) * lineAdvance);
                }
            }, drawChecks[i] });
        }

        // What Baseline of benchmarkDraw measures: the glyphs of GetGlyphQuads drawn with the functions of the library,
        // without layout and without clipping, in one call.
        std::vector<GlyphQuad> quads;
        font.GetGlyphQuads(scenario.text.c_str(), size, quads);
        items.push_back({ "DrawOnly", drawText, [&]() {
            const uint8_t                      *texture      = font.GetTexture();
            const size_t                       textureWidth  = font.GetTextureWidth();
            const uint32_t                     premultiplied = GlyphDraw::PremultiplyColor(kColor);
            const uint32_t                     colorAlpha    = kColor >> 24;
            const uint32_t                     bytesPerTexel = (font.GetTextureFormat() == FontBase::ETextureFormat::Alpha8) ? 1 : 4;
            const GlyphDraw::DrawGlyphFunction drawGlyph     = GlyphDraw::GetDrawGlyphFunction(bytesPerTexel, colorAlpha == 255);
            for (const GlyphQuad &quad : quads) {
                const size_t stepX         = quad.rotated ? textureWidth : 1;
                const size_t stepY         = quad.rotated ? 1 : textureWidth;
                const size_t offsetTexture = size_t(quad.textureRect.y) * textureWidth + size_t(quad.textureRect.x);
                uint32_t     *dstGlyph     = &pixels[size_t(posY + quad.y) * width + size_t(posX + quad.x)];
                drawGlyph(texture, offsetTexture, stepX, stepY, quad.width, quad.height, dstGlyph, width, premultiplied, colorAlpha);
            }
        }, "" });

        const size_t stdGlyphs = items.size();
        items.push_back({ "StdGlyphs",   stdGlyphs,   [&]() { gSink = probe.SumStdGlyphs(glyphKeys);   }, "" });
        items.push_back({ "FlatGlyphs",  stdGlyphs,   [&]() { gSink = probe.SumFlatGlyphs(glyphKeys);  }, sameGlyphs ? "same" : "DIFFERENT" });
        const size_t stdKerning = items.size();
        items.push_back({ "StdKerning",  stdKerning,  [&]() { gSink = probe.SumStdKerning(kerningPairs);  }, "" });
        items.push_back({ "FlatKerning", stdKerning,  [&]() { gSink = probe.SumFlatKerning(kerningPairs); }, sameKerning ? "same" : "DIFFERENT" });

        const std::vector<Timing> timings = Measure(items);

        printf("    %-28s %12s %12s %10s  %s\n", "Variant", "Median us", "Min us", "vs first", "Check");
        for (size_t i = 0; i < items.size(); ++i) {
            if (i == drawText || i == stdGlyphs) {
                printf("\n");
            }
            printf("    %-28s %12.1f %12.1f %10.2f  %s\n", items[i].name, timings[i].median * 1e6, timings[i].min * 1e6,
                   timings[items[i].comparedWith].median / timings[i].median, items[i].check.c_str());
        }
        printf("\n");
    }

} // end of namespace LayoutBenchmark

using namespace LayoutBenchmark;

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
    printf("Each group is compared with its first row: the library (GetTextBox, DrawText) or std::unordered_map.\n\n");

    std::vector<Scenario> scenarios;

    const std::string robotoPath = std::string(kResourcesPath) + "Roboto-Regular.ttf";
    const std::string dejaVuPath = std::string(kResourcesPath) + "DejaVuSerifCondensed-BoldItalic.ttf";
    const struct {
        const std::string   *path;
        const char          *name;
        bool                latin;
        bool                labels;
        uint8_t             size;
        int                 lines;
        int                 columns;
    } kRasterTexts[] = {
        { &robotoPath, "Roboto",                        true,  false, 14,  60, 100 },
        { &robotoPath, "Roboto",                        true,  false, 24,  35,  60 },
        { &robotoPath, "Roboto",                        true,  false, 40,  20,  40 },
        { &robotoPath, "Roboto",                        true,  false, 96,   8,  24 },
        { &dejaVuPath, "DejaVu Serif, Cyrillic, Greek", false, false, 14,  60, 100 },
        { &dejaVuPath, "DejaVu Serif, Cyrillic, Greek", false, false, 24,  35,  60 },
        { &robotoPath, "Roboto labels of 12",           true,  true,  14, 300,  12 },
    };

    for (const auto &rasterText : kRasterTexts) {
        std::unique_ptr<LayoutFont<RasterFont>> font(new LayoutFont<RasterFont>(false, rasterText.path->c_str()));
        if (font->GetStatus() != FontBase::EStatus::Ok) {
            fprintf(stderr, "Cannot load %s (status %d).\n", rasterText.path->c_str(), int(font->GetStatus()));
            return -1;
        }

        Scenario scenario;
        scenario.name   = std::string(rasterText.name) + " " + std::to_string(rasterText.size) + " px";
        scenario.text   = rasterText.latin ? MakeLatinText(rasterText.lines, rasterText.columns) : MakeNonLatinText(rasterText.lines, rasterText.columns);
        scenario.size   = rasterText.size;
        scenario.labels = rasterText.labels;
        // Packing every glyph at once, as a baked font does, gives the atlas its final size before the times start.
        if (font->Preload(scenario.text.c_str(), scenario.size) == false) {
            fprintf(stderr, "%s: some glyphs do not fit in the texture.\n", scenario.name.c_str());
            return -1;
        }
        scenario.probe = std::move(font);
        scenarios.push_back(std::move(scenario));
    }

#if defined(FONTRENDERER_USE_BAKED)
    const std::string metricsPath = std::string(kResourcesPath) + "effects.frb";
    const std::string effectsPath = std::string(kResourcesPath) + "effectfx.tga";
    const std::string plainPath   = std::string(kResourcesPath) + "effects.tga";
    const std::string texturePath = FileExists(effectsPath) ? effectsPath : plainPath;
    for (bool labels : { false, true }) {
        std::unique_ptr<LayoutFont<FontBaked>> baked(new LayoutFont<FontBaked>(true, metricsPath.c_str(), texturePath.c_str()));
        if (baked->GetStatus() != FontBase::EStatus::Ok) {
            printf("Cannot load %s and %s (status %d): the baked font is not measured.\n\n",
                   metricsPath.c_str(), texturePath.c_str(), int(baked->GetStatus()));
            break;
        }

        Scenario scenario;
        scenario.name   = labels ? "LilitaOne labels of 12 32 px baked" : "LilitaOne 32 px baked";
        scenario.text   = labels ? MakeLatinText(100, 12) : MakeLatinText(20, 60);
        scenario.size   = 32;
        scenario.labels = labels;
        scenario.probe  = std::move(baked);
        scenarios.push_back(std::move(scenario));
    }
#endif

    for (Scenario &scenario : scenarios) {
        Run(scenario);
    }

    return 0;
}
