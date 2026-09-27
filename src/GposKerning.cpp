//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "GposKerning.h"
//-------------------------------------
#include <algorithm>
#include <bitset>
#include <set>
#include <utility>

using namespace MindShake;

//-------------------------------------
namespace {

    constexpr uint32_t kTagGPOS = 0x47504F53;
    constexpr uint32_t kTagKern = 0x6B65726E;
    constexpr uint32_t kTagLatn = 0x6C61746E;
    constexpr uint32_t kTagDFLT = 0x44464C54;

    constexpr uint16_t kPairAdjustment = 2;
    constexpr uint16_t kExtension      = 9;

    constexpr uint16_t kXPlacement = 0x0001;
    constexpr uint16_t kYPlacement = 0x0002;
    constexpr uint16_t kXAdvance   = 0x0004;

    // Reads big endian values. Reading past the end gives 0 instead of failing, so a broken font
    // only gives wrong kerning or none, and the code that walks the tables needs no bounds checks of its own.
    //---------------------------------
    class Reader {
        public:
                        Reader(const uint8_t *data, size_t size) : mData(data), mSize(size) { }

            uint16_t    U16(size_t pos) const   { return Has(pos, 2) ? uint16_t((mData[pos] << 8) | mData[pos + 1]) : 0; }
            int16_t     S16(size_t pos) const   { return int16_t(U16(pos)); }
            uint32_t    U32(size_t pos) const   { return Has(pos, 4) ? (uint32_t(U16(pos)) << 16) | U16(pos + 2) : 0; }

            // A position past the end stays there, so every read from it gives 0.
            size_t      At(size_t base, uint64_t offset) const  { return Has(base, offset) ? base + size_t(offset) : mSize; }
            bool        Has(size_t pos, uint64_t bytes) const   { return pos <= mSize && bytes <= mSize - pos; }

        private:
            const uint8_t   *mData;
            size_t          mSize;
    };

    // Binary search in sorted records that start with a glyph id. Returns the index of the record, or -1.
    //---------------------------------
    int32_t
    FindGlyph(const Reader &reader, size_t records, uint16_t count, size_t recordSize, uint16_t glyph) {
        int32_t first = 0;
        int32_t last  = int32_t(count) - 1;
        while (first <= last) {
            const int32_t  middle = (first + last) / 2;
            const uint16_t id     = reader.U16(records + size_t(middle) * recordSize);
            if (glyph < id) {
                last = middle - 1;
            }
            else if (glyph > id) {
                first = middle + 1;
            }
            else {
                return middle;
            }
        }
        return -1;
    }

    // Binary search in sorted records of 6 bytes that start with the first and last glyph ids of a range.
    // Returns the index of the record, or -1.
    //---------------------------------
    int32_t
    FindRange(const Reader &reader, size_t records, uint16_t count, uint16_t glyph) {
        int32_t first = 0;
        int32_t last  = int32_t(count) - 1;
        while (first <= last) {
            const int32_t middle = (first + last) / 2;
            const size_t  record = records + size_t(middle) * 6;
            if (glyph < reader.U16(record)) {
                last = middle - 1;
            }
            else if (glyph > reader.U16(record + 2)) {
                first = middle + 1;
            }
            else {
                return middle;
            }
        }
        return -1;
    }

    // Returns -1 if the glyph is not covered.
    //---------------------------------
    int32_t
    GetCoverageIndex(const Reader &reader, size_t coverage, uint16_t glyph) {
        const uint16_t format = reader.U16(coverage);
        const uint16_t count  = reader.U16(coverage + 2);
        if (format == 1) {
            return FindGlyph(reader, coverage + 4, count, 2, glyph);
        }
        if (format == 2) {
            const int32_t index = FindRange(reader, coverage + 4, count, glyph);
            if (index >= 0) {
                const size_t record = coverage + 4 + size_t(index) * 6;
                return int32_t(reader.U16(record + 4)) + glyph - reader.U16(record);
            }
        }
        return -1;
    }

    // A glyph the table does not list is in class 0.
    //---------------------------------
    uint16_t
    GetClass(const Reader &reader, size_t classDef, uint16_t glyph) {
        const uint16_t format = reader.U16(classDef);
        if (format == 1) {
            const uint16_t startGlyph = reader.U16(classDef + 2);
            const uint16_t glyphCount = reader.U16(classDef + 4);
            if (glyph >= startGlyph && glyph - startGlyph < glyphCount) {
                return reader.U16(classDef + 6 + size_t(glyph - startGlyph) * 2);
            }
        }
        else if (format == 2) {
            const int32_t index = FindRange(reader, classDef + 4, reader.U16(classDef + 2), glyph);
            if (index >= 0) {
                return reader.U16(classDef + 4 + size_t(index) * 6 + 4);
            }
        }
        return 0;
    }

    // Each field of a value record takes 2 bytes, in the order of its bit. Only the 8 low bits are fields.
    //---------------------------------
    size_t
    GetValueRecordSize(uint16_t valueFormat) {
        return std::bitset<8>(valueFormat).count() * 2;
    }

    //---------------------------------
    int
    GetXAdvance(const Reader &reader, size_t valueRecord, uint16_t valueFormat) {
        if ((valueFormat & kXAdvance) == 0) {
            return 0;
        }
        return reader.S16(valueRecord + GetValueRecordSize(valueFormat & (kXPlacement | kYPlacement)));
    }

    // Returns false if the subtable does not have the pair, and then the next subtable of the lookup is tried.
    //---------------------------------
    bool
    GetPairKerning(const Reader &reader, size_t subtable, uint16_t leftGlyph, uint16_t rightGlyph, int &kerning) {
        const int32_t coverageIndex = GetCoverageIndex(reader, reader.At(subtable, reader.U16(subtable + 2)), leftGlyph);
        if (coverageIndex < 0) {
            return false;
        }

        const uint16_t valueFormat1 = reader.U16(subtable + 4);
        const uint16_t valueFormat2 = reader.U16(subtable + 6);
        const size_t   recordSize   = GetValueRecordSize(valueFormat1) + GetValueRecordSize(valueFormat2);

        size_t valueRecord;
        if (reader.U16(subtable) == 1) {
            if (coverageIndex >= reader.U16(subtable + 8)) {
                return false;
            }

            const size_t  pairSet       = reader.At(subtable, reader.U16(subtable + 10 + size_t(coverageIndex) * 2));
            const size_t  pairValueSize = 2 + recordSize;
            const int32_t index         = FindGlyph(reader, pairSet + 2, reader.U16(pairSet), pairValueSize, rightGlyph);
            if (index < 0) {
                return false;
            }
            valueRecord = pairSet + 2 + size_t(index) * pairValueSize + 2;
        }
        else {
            const uint16_t class1      = GetClass(reader, reader.At(subtable, reader.U16(subtable + 8)),  leftGlyph);
            const uint16_t class2      = GetClass(reader, reader.At(subtable, reader.U16(subtable + 10)), rightGlyph);
            const uint16_t class1Count = reader.U16(subtable + 12);
            const uint16_t class2Count = reader.U16(subtable + 14);
            if (class1 >= class1Count || class2 >= class2Count) {
                return false;
            }
            valueRecord = reader.At(subtable, 16 + (uint64_t(class1) * class2Count + class2) * recordSize);
        }

        kerning = GetXAdvance(reader, valueRecord, valueFormat1);
        return true;
    }

    // Returns the positions of the pair adjustment subtables of the lookup, which may be inside extension subtables.
    //---------------------------------
    std::vector<uint32_t>
    GetPairSubtables(const Reader &gpos, size_t lookup, size_t &budget) {
        std::vector<uint32_t> subtables;
        const uint16_t        lookupType    = gpos.U16(lookup);
        const uint16_t        subtableCount = gpos.U16(lookup + 4);
        if (gpos.Has(lookup, 6 + uint64_t(subtableCount) * 2) == false || subtableCount > budget) {
            return subtables;
        }
        budget -= subtableCount;

        for (uint16_t i = 0; i < subtableCount; ++i) {
            size_t   subtable     = gpos.At(lookup, gpos.U16(lookup + 6 + size_t(i) * 2));
            uint16_t subtableType = lookupType;
            if (lookupType == kExtension) {
                subtableType = gpos.U16(subtable + 2);
                subtable     = gpos.At(subtable, gpos.U32(subtable + 4));
            }

            const uint16_t format = gpos.U16(subtable);
            if (subtableType == kPairAdjustment && (format == 1 || format == 2)) {
                subtables.push_back(uint32_t(subtable));
            }
        }
        return subtables;
    }

} // end of namespace

//-------------------------------------
bool
GposKerning::Load(const uint8_t *font, size_t size, size_t fontOffset) {
    mGpos     = nullptr;
    mGposSize = 0;
    mLookups.clear();
    mScripts.clear();

    const Reader   file(font, size);
    const uint16_t numTables = file.U16(file.At(fontOffset, 4));
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t record = file.At(fontOffset, 12 + uint64_t(i) * 16);
        if (file.U32(record) == kTagGPOS) {
            const uint32_t offset = file.U32(record + 8);
            const uint32_t length = file.U32(record + 12);
            if (offset > size || length > size - offset) {
                return false;
            }
            mGpos     = font + offset;
            mGposSize = length;
            break;
        }
    }

    const Reader gpos(mGpos, mGposSize);
    // Version 1.1 only adds feature variations, which do not change the default instance of a variable font.
    if (gpos.U16(0) != 1) {
        return false;
    }

    const size_t   scriptList   = gpos.At(0, gpos.U16(4));
    const size_t   featureList  = gpos.At(0, gpos.U16(6));
    const size_t   lookupList   = gpos.At(0, gpos.U16(8));
    const uint16_t scriptCount  = gpos.U16(scriptList);
    const uint16_t featureCount = gpos.U16(featureList);
    const uint16_t lookupCount  = gpos.U16(lookupList);

    // The script of the text is unknown. Latin goes first, as the most likely one, then the default script, then the rest.
    std::vector<size_t> scripts;
    for (const uint32_t tag : { kTagLatn, kTagDFLT, 0u }) {
        for (uint16_t i = 0; i < scriptCount; ++i) {
            const size_t   record    = scriptList + 2 + size_t(i) * 6;
            const uint32_t scriptTag = gpos.U32(record);
            const bool     isChosen  = (tag != 0) ? scriptTag == tag : (scriptTag != kTagLatn && scriptTag != kTagDFLT);
            if (isChosen) {
                scripts.push_back(gpos.At(scriptList, gpos.U16(record + 4)));
            }
        }
    }

    // A real font takes far fewer steps to read than bytes has its GPOS. This stops a broken font whose tables
    // point into each other, so that the same few bytes would be read millions of times.
    size_t budget = mGposSize;

    constexpr int32_t               kNotRead = -2;
    constexpr int32_t               kNoPairs = -1;
    std::vector<int32_t>            lookupSlots(lookupCount, kNotRead);     // Index into mLookups, or one of the values above
    std::set<std::vector<uint16_t>> seenScripts;
    for (const size_t script : scripts) {
        // Only the default language system is read, as HarfBuzz does when it does not know the language.
        const uint16_t langSysOffset = gpos.U16(script);
        if (langSysOffset == 0) {
            continue;
        }

        const size_t          langSys           = gpos.At(script, langSysOffset);
        const uint16_t        requiredFeature   = gpos.U16(langSys + 2);
        const uint16_t        featureIndexCount = gpos.U16(langSys + 4);
        std::vector<uint16_t> featureIndices;
        if (requiredFeature != 0xffff) {
            featureIndices.push_back(requiredFeature);
        }
        for (uint16_t i = 0; i < featureIndexCount && budget > 0; ++i, --budget) {
            featureIndices.push_back(gpos.U16(langSys + 6 + size_t(i) * 2));
        }

        std::vector<uint16_t> lookups;
        for (const uint16_t featureIndex : featureIndices) {
            const size_t record = featureList + 2 + size_t(featureIndex) * 6;
            if (featureIndex >= featureCount || gpos.U32(record) != kTagKern) {
                continue;
            }

            const size_t   feature          = gpos.At(featureList, gpos.U16(record + 4));
            const uint16_t lookupIndexCount = gpos.U16(feature + 2);
            for (uint16_t i = 0; i < lookupIndexCount && budget > 0; ++i, --budget) {
                const uint16_t lookupIndex = gpos.U16(feature + 4 + size_t(i) * 2);
                if (lookupIndex >= lookupCount) {
                    continue;
                }

                int32_t &slot = lookupSlots[lookupIndex];
                if (slot == kNotRead) {
                    const size_t          lookup    = gpos.At(lookupList, gpos.U16(lookupList + 2 + size_t(lookupIndex) * 2));
                    std::vector<uint32_t> subtables = GetPairSubtables(gpos, lookup, budget);
                    slot = subtables.empty() ? kNoPairs : int32_t(mLookups.size());
                    if (slot != kNoPairs) {
                        mLookups.push_back(std::move(subtables));
                    }
                }
                if (slot != kNoPairs) {
                    lookups.push_back(uint16_t(slot));
                }
            }
        }

        // A lookup is applied once, even if several features list it.
        std::sort(lookups.begin(), lookups.end());
        lookups.erase(std::unique(lookups.begin(), lookups.end()), lookups.end());
        if (lookups.empty() == false && seenScripts.insert(lookups).second) {
            mScripts.push_back(std::move(lookups));
        }
    }

    return HasKerning();
}

//-------------------------------------
int
GposKerning::GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) const {
    if (leftGlyph > 0xffff || rightGlyph > 0xffff) {
        return 0;
    }

    // Adding the kerning of every script would apply twice the pairs that the lookups of two scripts share.
    // So the first script whose lookups have the pair decides, even if it gives 0.
    const Reader gpos(mGpos, mGposSize);
    for (const auto &lookups : mScripts) {
        int  kerning = 0;
        bool hasPair = false;
        for (const uint16_t lookup : lookups) {
            // Inside a lookup only the first subtable that has the pair applies, even if its value is 0.
            for (const uint32_t subtable : mLookups[lookup]) {
                int value;
                if (GetPairKerning(gpos, subtable, uint16_t(leftGlyph), uint16_t(rightGlyph), value)) {
                    kerning += value;
                    hasPair  = true;
                    break;
                }
            }
        }
        if (hasPair) {
            return kerning;
        }
    }

    return 0;
}
