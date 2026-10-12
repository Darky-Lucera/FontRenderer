#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FlatMap.h"
#include "Platform.h"
//-------------------------------------
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

//-------------------------------------
namespace MindShake {

    // A FlatMap whose values never move once added, so that a reference to one stays valid. The layout keeps the
    // data of a glyph while it looks up the next one, which can add it.
    // std::unordered_map also keeps references valid, but it is slow for the reasons FlatMap explains, so do not
    // replace this with it without measuring with benchmarkLayout. A lookup here is the one of FlatMap, plus one read
    // to find the block: the values are kept in blocks of 32, so an index finds its value with a shift and a mask.
    //---------------------------------
    template <class TValue>
    class StableMap {
        public:
            //-------------------------
            FONTRENDERER_ALWAYS_INLINE const TValue *
            Find(uint32_t key) const {
                const uint32_t *index = mIndices.Find(key);
                return (index != nullptr) ? &GetEntry(*index).value : nullptr;
            }

            // As std::unordered_map::insert: it does not change the value of a key already in the map.
            // Returns the value in the map.
            //-------------------------
            const TValue &
            Insert(uint32_t key, const TValue &value) {
                const uint32_t *index = mIndices.Find(key);
                if (index != nullptr) {
                    return GetEntry(*index).value;
                }

                if ((mCount & kBlockMask) == 0) {
                    mBlocks.emplace_back(new Entry[kBlockSize]);
                }
                Entry &entry = mBlocks.back()[mCount & kBlockMask];
                entry.key   = key;
                entry.value = value;
                mIndices.Insert(key, uint32_t(mCount));
                ++mCount;
                return entry.value;
            }

            //-------------------------
            void
            Clear() {
                mIndices.Clear();
                mBlocks.clear();
                mCount = 0;
            }

            size_t      GetCount() const    { return mCount; }

            // Calls visit(key, value) for each key, in the order they were added.
            //-------------------------
            template <class TVisitor>
            void
            ForEach(TVisitor visit) const {
                for (size_t i = 0; i < mCount; ++i) {
                    const Entry &entry = GetEntry(uint32_t(i));
                    visit(entry.key, entry.value);
                }
            }

        protected:
            //-------------------------
            struct Entry {
                uint32_t    key {};
                TValue      value {};
            };

            static constexpr uint32_t   kBlockBits = 5;
            static constexpr uint32_t   kBlockSize = 1u << kBlockBits;
            static constexpr uint32_t   kBlockMask = kBlockSize - 1;

            const Entry &   GetEntry(uint32_t index) const  { return mBlocks[index >> kBlockBits][index & kBlockMask]; }

        protected:
            FlatMap<uint32_t>                       mIndices;   // Index of each key in the blocks
            std::vector<std::unique_ptr<Entry[]>>   mBlocks;
            size_t                                  mCount {};
    };

} // end of namespace
