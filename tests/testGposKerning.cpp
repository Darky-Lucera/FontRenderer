#include "GposKerning.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

using MindShake::GposKerning;

//-------------------------------------
namespace {

    constexpr uint16_t kXPlacement = 0x0001;
    constexpr uint16_t kYPlacement = 0x0002;
    constexpr uint16_t kXAdvance   = 0x0004;
    constexpr uint16_t kYAdvance   = 0x0008;
    constexpr uint16_t kXAdvDevice = 0x0040;
    constexpr uint16_t kAllFields  = 0x00ff;

    constexpr uint16_t kPairAdjustment = 2;
    constexpr uint16_t kChainedContext = 8;

    // Big endian bytes of a table under construction.
    //---------------------------------
    class Bytes {
        public:
            size_t          Size() const                    { return mData.size(); }
            const uint8_t * Data() const                    { return mData.data(); }
            uint8_t &       operator[](size_t pos)          { return mData[pos]; }

            void            U16(uint32_t value)             { mData.push_back(uint8_t(value >> 8)); mData.push_back(uint8_t(value)); }
            void            U32(uint32_t value)             { U16(value >> 16); U16(value & 0xffff); }
            void            Tag(const char *tag)            { mData.insert(mData.end(), tag, tag + 4); }
            void            Zeros(size_t count)             { mData.resize(mData.size() + count, 0); }
            void            Append(const Bytes &other)      { mData.insert(mData.end(), other.mData.begin(), other.mData.end()); }
            void            Truncate(size_t size)           { mData.resize(size); }

            // Reserves an offset, to point it later with PointToEnd.
            size_t          Offset16()                      { U16(0); return Size() - 2; }
            size_t          Offset32()                      { U32(0); return Size() - 4; }
            // Points the offset at pos, which counts from base, to the current end.
            void            PointToEnd16(size_t pos, size_t base) {
                const size_t offset = Size() - base;
                mData[pos]     = uint8_t(offset >> 8);
                mData[pos + 1] = uint8_t(offset);
            }
            void            PointToEnd32(size_t pos, size_t base) {
                const size_t offset = Size() - base;
                for (int i = 0; i < 4; ++i) {
                    mData[pos + i] = uint8_t(offset >> (24 - 8 * i));
                }
            }

        private:
            std::vector<uint8_t> mData;
    };

    // Every field besides the advance of x gets a value no test expects, so reading the wrong field shows up.
    //---------------------------------
    void
    AddValueRecord(Bytes &bytes, uint16_t valueFormat, int16_t xAdvance) {
        for (uint16_t field = 1; field <= 0x80; field <<= 1) {
            if (valueFormat & field) {
                bytes.U16(field == kXAdvance ? uint16_t(xAdvance) : 999);
            }
        }
    }

    // Writes a table in format 2: records of a first glyph, a last glyph and a value.
    // In a coverage table the value is the index of the first glyph, and the index grows along the range.
    // In a class definition the value is the class, and it is the same along the range.
    //---------------------------------
    void
    AddRangeTable(Bytes &bytes, const std::vector<std::pair<uint16_t, uint16_t>> &glyphValues, bool valueGrows) {
        std::vector<std::pair<uint16_t, uint16_t>> ranges;     // Indices into glyphValues of the first and last glyphs
        for (uint16_t i = 0; i < glyphValues.size(); ++i) {
            if (ranges.empty() == false) {
                const auto &last = glyphValues[ranges.back().second];
                if (glyphValues[i].first == last.first + 1 && glyphValues[i].second == last.second + (valueGrows ? 1 : 0)) {
                    ranges.back().second = i;
                    continue;
                }
            }
            ranges.push_back({ i, i });
        }

        bytes.U16(2);
        bytes.U16(uint32_t(ranges.size()));
        for (const auto &range : ranges) {
            bytes.U16(glyphValues[range.first].first);
            bytes.U16(glyphValues[range.second].first);
            bytes.U16(glyphValues[range.first].second);
        }
    }

    //---------------------------------
    struct Pair {
        uint16_t left;
        uint16_t right;
        int16_t  kerning;
    };

    // The pairs must be sorted by glyph ids. The value records of the right glyphs hold an advance that must be ignored.
    //---------------------------------
    Bytes
    MakePairPos1(const std::vector<Pair> &pairs, uint16_t valueFormat1 = kXAdvance, uint16_t valueFormat2 = 0) {
        std::map<uint16_t, std::vector<Pair>> pairSets;
        for (const Pair &pair : pairs) {
            pairSets[pair.left].push_back(pair);
        }

        Bytes table;
        table.U16(1);
        const size_t coverage = table.Offset16();
        table.U16(valueFormat1);
        table.U16(valueFormat2);
        table.U16(uint32_t(pairSets.size()));
        std::vector<size_t> pairSetOffsets;
        for (size_t i = 0; i < pairSets.size(); ++i) {
            pairSetOffsets.push_back(table.Offset16());
        }

        size_t index = 0;
        for (const auto &pairSet : pairSets) {
            table.PointToEnd16(pairSetOffsets[index++], 0);
            table.U16(uint32_t(pairSet.second.size()));
            for (const Pair &pair : pairSet.second) {
                table.U16(pair.right);
                AddValueRecord(table, valueFormat1, pair.kerning);
                AddValueRecord(table, valueFormat2, 555);
            }
        }

        table.PointToEnd16(coverage, 0);
        table.U16(1);
        table.U16(uint32_t(pairSets.size()));
        for (const auto &pairSet : pairSets) {
            table.U16(pairSet.first);
        }
        return table;
    }

    //---------------------------------
    struct ClassKerning {
        std::vector<uint16_t>        coverage;          // Sorted
        std::map<uint16_t, uint16_t> classes1;          // Glyphs not listed are in class 0
        std::map<uint16_t, uint16_t> classes2;
        uint16_t                     class1Count;
        uint16_t                     class2Count;
        std::vector<int16_t>         kerning;           // class1Count rows of class2Count values
    };

    // The coverage and the second class definition are written in ranges, and the first class definition as an
    // array, so every format of both tables is read. The value records of the right glyphs hold an advance that must be ignored.
    //---------------------------------
    Bytes
    MakePairPos2(const ClassKerning &data, uint16_t valueFormat1 = kXAdvance, uint16_t valueFormat2 = 0) {
        Bytes table;
        table.U16(2);
        const size_t coverage = table.Offset16();
        table.U16(valueFormat1);
        table.U16(valueFormat2);
        const size_t classDef1 = table.Offset16();
        const size_t classDef2 = table.Offset16();
        table.U16(data.class1Count);
        table.U16(data.class2Count);
        for (const int16_t kerning : data.kerning) {
            AddValueRecord(table, valueFormat1, kerning);
            AddValueRecord(table, valueFormat2, 555);
        }

        table.PointToEnd16(coverage, 0);
        std::vector<std::pair<uint16_t, uint16_t>> coverageIndices;
        for (uint16_t i = 0; i < data.coverage.size(); ++i) {
            coverageIndices.push_back({ data.coverage[i], i });
        }
        AddRangeTable(table, coverageIndices, true);

        table.PointToEnd16(classDef1, 0);
        const uint16_t firstGlyph = data.classes1.begin()->first;
        const uint16_t lastGlyph  = data.classes1.rbegin()->first;
        table.U16(1);
        table.U16(firstGlyph);
        table.U16(uint32_t(lastGlyph - firstGlyph + 1));
        for (uint32_t glyph = firstGlyph; glyph <= lastGlyph; ++glyph) {
            const auto it = data.classes1.find(uint16_t(glyph));
            table.U16(it == data.classes1.end() ? 0 : it->second);
        }

        table.PointToEnd16(classDef2, 0);
        AddRangeTable(table, { data.classes2.begin(), data.classes2.end() }, false);
        return table;
    }

    //---------------------------------
    struct Lookup {
        std::vector<Bytes>       subtables;
        uint16_t                 type      = kPairAdjustment;
        bool                     extension = false;
        // The scripts whose 'kern' feature lists the lookup. Without any, only the 'mark' feature lists it.
        std::vector<std::string> scripts   = { "DFLT" };
    };

    //---------------------------------
    Lookup
    MakeLookup(std::vector<Bytes> subtables, std::vector<std::string> scripts = { "DFLT" }) {
        Lookup lookup;
        lookup.subtables = std::move(subtables);
        lookup.scripts   = std::move(scripts);
        return lookup;
    }

    // Each script has its own 'kern' feature, and a 'mark' feature that all of them share.
    // Scripts only have the default language system.
    //---------------------------------
    Bytes
    MakeGpos(const std::vector<Lookup> &lookups, uint16_t minorVersion = 0) {
        std::map<std::string, std::vector<uint16_t>> kernLookups;      // Sorted by tag, as the script list must be
        std::vector<uint16_t>                        markLookups;
        for (uint16_t i = 0; i < lookups.size(); ++i) {
            for (const std::string &script : lookups[i].scripts) {
                kernLookups[script].push_back(i);
            }
            if (lookups[i].scripts.empty()) {
                markLookups.push_back(i);
            }
        }
        const uint16_t markFeature = uint16_t(kernLookups.size());

        Bytes gpos;
        gpos.U16(1);
        gpos.U16(minorVersion);
        const size_t scriptList  = gpos.Offset16();
        const size_t featureList = gpos.Offset16();
        const size_t lookupList  = gpos.Offset16();
        if (minorVersion == 1) {
            gpos.U32(0);                                // No feature variations
        }

        gpos.PointToEnd16(scriptList, 0);
        const size_t        scriptListStart = gpos.Size();
        std::vector<size_t> scriptOffsets;
        gpos.U16(uint32_t(kernLookups.size()));
        for (const auto &script : kernLookups) {
            gpos.Tag(script.first.c_str());
            scriptOffsets.push_back(gpos.Offset16());
        }
        for (uint16_t i = 0; i < scriptOffsets.size(); ++i) {
            gpos.PointToEnd16(scriptOffsets[i], scriptListStart);
            gpos.U16(4);                                // The default language system follows
            gpos.U16(0);                                // No other language systems
            gpos.U16(0);                                // Lookup order
            gpos.U16(0xffff);                           // No required feature
            gpos.U16(2);
            gpos.U16(i);                                // Its 'kern' feature
            gpos.U16(markFeature);
        }

        gpos.PointToEnd16(featureList, 0);
        const size_t                       featureListStart = gpos.Size();
        std::vector<std::vector<uint16_t>> featureLookups;
        std::vector<size_t>                featureOffsets;
        gpos.U16(uint32_t(kernLookups.size() + 1));
        for (const auto &script : kernLookups) {
            gpos.Tag("kern");
            featureOffsets.push_back(gpos.Offset16());
            featureLookups.push_back(script.second);
        }
        gpos.Tag("mark");
        featureOffsets.push_back(gpos.Offset16());
        featureLookups.push_back(markLookups);
        for (size_t i = 0; i < featureOffsets.size(); ++i) {
            gpos.PointToEnd16(featureOffsets[i], featureListStart);
            gpos.U16(0);                                // No feature parameters
            gpos.U16(uint32_t(featureLookups[i].size()));
            for (const uint16_t index : featureLookups[i]) {
                gpos.U16(index);
            }
        }

        gpos.PointToEnd16(lookupList, 0);
        const size_t        lookupListStart = gpos.Size();
        std::vector<size_t> lookupOffsets;
        gpos.U16(uint32_t(lookups.size()));
        for (size_t i = 0; i < lookups.size(); ++i) {
            lookupOffsets.push_back(gpos.Offset16());
        }

        for (size_t i = 0; i < lookups.size(); ++i) {
            const Lookup &lookup = lookups[i];
            gpos.PointToEnd16(lookupOffsets[i], lookupListStart);
            const size_t lookupStart = gpos.Size();
            gpos.U16(lookup.extension ? 9 : lookup.type);
            gpos.U16(0);                                // Lookup flags
            gpos.U16(uint32_t(lookup.subtables.size()));
            std::vector<size_t> subtableOffsets;
            for (size_t j = 0; j < lookup.subtables.size(); ++j) {
                subtableOffsets.push_back(gpos.Offset16());
            }

            for (size_t j = 0; j < lookup.subtables.size(); ++j) {
                gpos.PointToEnd16(subtableOffsets[j], lookupStart);
                if (lookup.extension) {
                    const size_t extensionStart = gpos.Size();
                    gpos.U16(1);
                    gpos.U16(lookup.type);
                    const size_t offset = gpos.Offset32();
                    gpos.PointToEnd32(offset, extensionStart);
                }
                gpos.Append(lookup.subtables[j]);
            }
        }

        return gpos;
    }

    // A font whose only table is GPOS. With a prefix, the font starts after it, like a font inside a collection.
    //---------------------------------
    Bytes
    MakeFont(const Bytes &gpos, size_t prefixSize = 0) {
        Bytes font;
        font.Zeros(prefixSize);
        font.U32(0x00010000);
        font.U16(1);                                    // Number of tables
        font.U16(16);                                   // Search range
        font.U16(0);                                    // Entry selector
        font.U16(0);                                    // Range shift
        font.Tag("GPOS");
        font.U32(0);                                    // Checksum
        font.U32(uint32_t(font.Size() + 8));            // Offsets of tables count from the start of the file
        font.U32(uint32_t(gpos.Size()));
        font.Append(gpos);
        return font;
    }

    // Keeps a copy of the font as long as the kerning that points into it.
    // The copy has the exact size, so AddressSanitizer catches any read past its end.
    //---------------------------------
    struct TestFont {
        explicit TestFont(const Bytes &font, size_t fontOffset = 0) : data(new uint8_t[font.Size()]) {
            std::copy(font.Data(), font.Data() + font.Size(), data.get());
            loaded = kerning.Load(data.get(), font.Size(), fontOffset);
        }

        std::unique_ptr<uint8_t[]> data;
        GposKerning                kerning;
        bool                       loaded;
    };

    // Right glyphs other than rightGlyph are in class 0, and get otherKerning.
    //---------------------------------
    ClassKerning
    MakeClassKerning(uint16_t leftGlyph, uint16_t rightGlyph, int16_t kerning, int16_t otherKerning = 0) {
        ClassKerning data;
        data.coverage    = { leftGlyph };
        data.classes1    = { { leftGlyph, 1 } };
        data.classes2    = { { rightGlyph, 1 } };
        data.class1Count = 2;
        data.class2Count = 2;
        data.kerning     = { 0, 0, otherKerning, kerning };
        return data;
    }

} // end of namespace

//-------------------------------------
TEST_CASE("GposKerning reads pairs of glyphs") {
    TestFont font(MakeFont(MakeGpos({ MakeLookup({ MakePairPos1({ { 10, 20, -50 }, { 10, 21, -30 }, { 11, 20, 15 } }) }) })));
    REQUIRE(font.loaded);
    REQUIRE(font.kerning.HasKerning());

    CHECK(font.kerning.GetKerning(10, 20) == -50);
    CHECK(font.kerning.GetKerning(10, 21) == -30);
    CHECK(font.kerning.GetKerning(11, 20) == 15);
    CHECK(font.kerning.GetKerning(11, 21) == 0);
    CHECK(font.kerning.GetKerning(12, 20) == 0);
    CHECK(font.kerning.GetKerning(20, 10) == 0);
    // Glyph ids in OpenType have 16 bits.
    CHECK(font.kerning.GetKerning(10 + 0x10000, 20) == 0);
    CHECK(font.kerning.GetKerning(10, 20 + 0x10000) == 0);
}

//-------------------------------------
TEST_CASE("GposKerning reads classes of glyphs") {
    ClassKerning data;
    data.coverage    = { 10, 11, 12, 30 };
    data.classes1    = { { 10, 1 }, { 11, 1 }, { 12, 2 } };
    data.classes2    = { { 20, 1 }, { 21, 2 }, { 22, 2 }, { 40, 1 } };
    data.class1Count = 3;
    data.class2Count = 3;
    data.kerning     = {  0,  -5,  -6,
                         -7, -40, -20,
                         -8,  25,   0 };
    TestFont font(MakeFont(MakeGpos({ MakeLookup({ MakePairPos2(data) }) })));
    REQUIRE(font.loaded);

    CHECK(font.kerning.GetKerning(10, 20) == -40);
    CHECK(font.kerning.GetKerning(11, 21) == -20);
    CHECK(font.kerning.GetKerning(11, 22) == -20);
    CHECK(font.kerning.GetKerning(12, 20) == 25);
    CHECK(font.kerning.GetKerning(12, 40) == 25);
    // A glyph without a class is in class 0, but the first glyph must still be in the coverage.
    CHECK(font.kerning.GetKerning(10, 99) == -7);
    CHECK(font.kerning.GetKerning(30, 21) == -6);
    CHECK(font.kerning.GetKerning(13, 20) == 0);
}

//-------------------------------------
TEST_CASE("GposKerning finds the advance in any value format") {
    const uint16_t formats1[] = { kXAdvance, kXPlacement | kXAdvance, kYPlacement | kXAdvance | kXAdvDevice, kXPlacement | kYPlacement | kXAdvance | kYAdvance, kAllFields };
    const uint16_t formats2[] = { 0, kXPlacement, kXAdvance, kAllFields };
    for (const uint16_t format1 : formats1) {
        for (const uint16_t format2 : formats2) {
            CAPTURE(format1);
            CAPTURE(format2);
            const Bytes pairs   = MakePairPos1({ { 10, 20, -50 }, { 10, 21, -30 } }, format1, format2);
            const Bytes classes = MakePairPos2(MakeClassKerning(11, 21, -40), format1, format2);
            TestFont    font(MakeFont(MakeGpos({ MakeLookup({ pairs, classes }) })));
            REQUIRE(font.loaded);

            CHECK(font.kerning.GetKerning(10, 20) == -50);
            CHECK(font.kerning.GetKerning(10, 21) == -30);
            CHECK(font.kerning.GetKerning(11, 21) == -40);
        }
    }
}

//-------------------------------------
TEST_CASE("GposKerning only applies the first subtable of a lookup that has the pair") {
    SUBCASE("A pair missing in a subtable of pairs goes on to the next subtable") {
        const Bytes pairs   = MakePairPos1({ { 10, 20, -50 } });
        const Bytes classes = MakePairPos2(MakeClassKerning(10, 21, -30, -10));
        TestFont    font(MakeFont(MakeGpos({ MakeLookup({ pairs, classes }) })));

        CHECK(font.kerning.GetKerning(10, 20) == -50);
        CHECK(font.kerning.GetKerning(10, 21) == -30);
        CHECK(font.kerning.GetKerning(10, 22) == -10);
    }

    SUBCASE("A pair without an advance still stops the lookup") {
        const Bytes pairs   = MakePairPos1({ { 10, 20, -50 } }, kXPlacement);
        const Bytes classes = MakePairPos2(MakeClassKerning(10, 20, -30));
        TestFont    font(MakeFont(MakeGpos({ MakeLookup({ pairs, classes }) })));

        CHECK(font.kerning.GetKerning(10, 20) == 0);
    }

    SUBCASE("Class 0 of the right glyph stops the lookup") {
        const Bytes classes = MakePairPos2(MakeClassKerning(10, 20, -30));
        const Bytes pairs   = MakePairPos1({ { 10, 21, -50 } });
        TestFont    font(MakeFont(MakeGpos({ MakeLookup({ classes, pairs }) })));

        CHECK(font.kerning.GetKerning(10, 20) == -30);
        CHECK(font.kerning.GetKerning(10, 21) == 0);
    }

    SUBCASE("A class out of range goes on to the next subtable") {
        ClassKerning badClass2 = MakeClassKerning(10, 20, -30);
        badClass2.classes2[20] = 2;
        ClassKerning badClass1 = MakeClassKerning(11, 20, -30);
        badClass1.classes1[11] = 2;
        const Bytes pairs = MakePairPos1({ { 10, 20, -50 }, { 11, 20, -60 } });
        TestFont    font(MakeFont(MakeGpos({ MakeLookup({ MakePairPos2(badClass2), MakePairPos2(badClass1), pairs }) })));

        CHECK(font.kerning.GetKerning(10, 20) == -50);
        CHECK(font.kerning.GetKerning(11, 20) == -60);
    }
}

//-------------------------------------
TEST_CASE("GposKerning adds the kerning of every lookup of the kern feature") {
    const Lookup notKerning = MakeLookup({ MakePairPos1({ { 10, 20, -1000 } }) }, {});
    TestFont font(MakeFont(MakeGpos({ MakeLookup({ MakePairPos1({ { 10, 20, -50 } }) }),
                                      notKerning,
                                      MakeLookup({ MakePairPos2(MakeClassKerning(10, 20, -7)) }) })));
    REQUIRE(font.loaded);

    CHECK(font.kerning.GetKerning(10, 20) == -57);
}

//-------------------------------------
TEST_CASE("GposKerning takes the kerning of the first script that has the pair") {
    TestFont font(MakeFont(MakeGpos({ MakeLookup({ MakePairPos1({ { 10, 20, -50 } }) }, { "latn" }),
                                      MakeLookup({ MakePairPos1({ { 10, 20, -30 }, { 11, 21, -7 }, { 14, 24, -8 } }) }, { "hebr" }),
                                      MakeLookup({ MakePairPos1({ { 10, 20, -99 }, { 12, 22, -3 }, { 14, 24, -4 } }) }, { "DFLT" }),
                                      MakeLookup({ MakePairPos1({ { 13, 23, -11 } }) }, { "DFLT", "hebr", "latn" }) })));
    REQUIRE(font.loaded);

    // Latin goes first, then the default script, then the rest.
    CHECK(font.kerning.GetKerning(10, 20) == -50);
    CHECK(font.kerning.GetKerning(12, 22) == -3);
    CHECK(font.kerning.GetKerning(14, 24) == -4);
    CHECK(font.kerning.GetKerning(11, 21) == -7);
    // A lookup can belong to several scripts.
    CHECK(font.kerning.GetKerning(13, 23) == -11);
}

//-------------------------------------
TEST_CASE("GposKerning reads extension lookups") {
    Lookup lookup = MakeLookup({ MakePairPos1({ { 10, 20, -50 } }), MakePairPos2(MakeClassKerning(11, 21, -40)) });
    lookup.extension = true;
    TestFont font(MakeFont(MakeGpos({ lookup })));
    REQUIRE(font.loaded);

    CHECK(font.kerning.GetKerning(10, 20) == -50);
    CHECK(font.kerning.GetKerning(11, 21) == -40);
}

//-------------------------------------
TEST_CASE("GposKerning skips the lookups that need more than a pair of glyphs") {
    // The subtable is never read, so any bytes do.
    Lookup contextual = MakeLookup({ MakePairPos1({ { 10, 20, -1000 } }) });
    contextual.type = kChainedContext;
    Lookup extension = contextual;
    extension.extension = true;

    SUBCASE("Without pair adjustments there is no kerning") {
        TestFont font(MakeFont(MakeGpos({ contextual, extension })));

        CHECK(font.loaded == false);
        CHECK(font.kerning.HasKerning() == false);
        CHECK(font.kerning.GetKerning(10, 20) == 0);
    }

    SUBCASE("The pair adjustments still apply") {
        TestFont font(MakeFont(MakeGpos({ contextual, MakeLookup({ MakePairPos1({ { 10, 20, -50 } }) }), extension })));

        REQUIRE(font.loaded);
        CHECK(font.kerning.GetKerning(10, 20) == -50);
    }
}

//-------------------------------------
TEST_CASE("GposKerning reads GPOS 1.1") {
    TestFont font(MakeFont(MakeGpos({ MakeLookup({ MakePairPos1({ { 10, 20, -50 } }) }) }, 1)));
    REQUIRE(font.loaded);

    CHECK(font.kerning.GetKerning(10, 20) == -50);
}

//-------------------------------------
TEST_CASE("GposKerning finds GPOS in a font collection") {
    const size_t kFontOffset = 36;
    TestFont     font(MakeFont(MakeGpos({ MakeLookup({ MakePairPos1({ { 10, 20, -50 } }) }) }), kFontOffset), kFontOffset);
    REQUIRE(font.loaded);

    CHECK(font.kerning.GetKerning(10, 20) == -50);
}

//-------------------------------------
TEST_CASE("GposKerning needs a GPOS table it knows") {
    const Bytes gpos = MakeGpos({ MakeLookup({ MakePairPos1({ { 10, 20, -50 } }) }) });
    const Bytes good = MakeFont(gpos);
    REQUIRE(TestFont(good).loaded);

    SUBCASE("Without a font") {
        GposKerning kerning;
        CHECK(kerning.Load(nullptr, 0) == false);
        CHECK(kerning.GetKerning(10, 20) == 0);
    }

    SUBCASE("Without GPOS") {
        Bytes font = good;
        font[12] = 'X';
        CHECK(TestFont(font).loaded == false);
    }

    SUBCASE("GPOS past the end of the font") {
        Bytes font = MakeFont(gpos);
        font.Truncate(font.Size() - 1);
        CHECK(TestFont(font).loaded == false);
    }

    SUBCASE("An unknown major version") {
        Bytes newer = gpos;
        newer[1] = 2;
        CHECK(TestFont(MakeFont(newer)).loaded == false);
    }

    SUBCASE("Loading again forgets the previous font") {
        TestFont font(good);
        Bytes    empty;
        CHECK(font.kerning.Load(empty.Data(), 0) == false);
        CHECK(font.kerning.HasKerning() == false);
        CHECK(font.kerning.GetKerning(10, 20) == 0);
    }
}

//-------------------------------------
TEST_CASE("GposKerning never reads outside a broken GPOS table") {
    // Every piece it reads: extension lookups, both formats of pair adjustment, coverage and class definition,
    // and value records with several fields.
    Lookup extension = MakeLookup({ MakePairPos1({ { 10, 20, -50 }, { 12, 22, -5 } }, kXPlacement | kXAdvance, kXAdvance) });
    extension.extension = true;
    ClassKerning classes;
    classes.coverage    = { 10, 11, 12, 30 };
    classes.classes1    = { { 10, 1 }, { 11, 1 }, { 12, 2 } };
    classes.classes2    = { { 20, 1 }, { 21, 2 }, { 22, 2 }, { 40, 1 } };
    classes.class1Count = 3;
    classes.class2Count = 3;
    classes.kerning     = { 0, -5, -6, -7, -40, -20, -8, 25, 0 };
    const Bytes gpos = MakeGpos({ extension, MakeLookup({ MakePairPos2(classes, kAllFields) }) });

    // The results do not matter: this only has to finish without reading outside the font.
    // Out of bounds reads only fail with AddressSanitizer or similar tools.
    const auto queryPairs = [](const GposKerning &kerning) {
        int sum = 0;
        for (uint32_t left = 0; left < 48; ++left) {
            for (uint32_t right = 0; right < 48; ++right) {
                sum += kerning.GetKerning(left, right);
            }
        }
        return sum;
    };

    SUBCASE("Cut at every size") {
        for (size_t size = 0; size <= gpos.Size(); ++size) {
            Bytes cut = gpos;
            cut.Truncate(size);
            TestFont font(MakeFont(cut));
            (void) queryPairs(font.kerning);
        }
    }

    SUBCASE("Random bytes changed") {
        std::mt19937                            random(1234);
        std::uniform_int_distribution<size_t>   position(0, gpos.Size() - 1);
        std::uniform_int_distribution<uint32_t> byte(0, 255);
        std::uniform_int_distribution<int>      changes(1, 4);
        for (int i = 0; i < 2000; ++i) {
            Bytes     broken = gpos;
            const int count  = changes(random);
            for (int j = 0; j < count; ++j) {
                broken[position(random)] = uint8_t(byte(random));
            }
            TestFont font(MakeFont(broken));
            (void) queryPairs(font.kerning);
        }
    }
}
