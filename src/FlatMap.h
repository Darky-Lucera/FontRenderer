#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2026 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "Platform.h"
//-------------------------------------
#include <cstddef>
#include <cstdint>
#include <vector>

//-------------------------------------
namespace MindShake {

    // A hash map from 32-bit keys, for the tables of the fonts. The layout looks up two of them for each character.
    //
    // Do not replace it with std::unordered_map or std::map without measuring with benchmarkLayout. With text of
    // 14 pixels, on an i7-10850H with GCC, MSVC and clang-cl, changing only std::unordered_map for FlatMap made the
    // layout alone 53 to 237 % faster, and DrawText 11 to 48 % faster. std::unordered_map is slow here because
    // libstdc++ finds the bucket with a 64-bit division (35 to 88 cycles on that i7), and one more for each extra node
    // it visits; MSVC hashes the key byte by byte; and both follow a pointer to a node of its own for each key.
    // std::map was not measured, but it compares the key with about log2(n) nodes, each of them a separate allocation.
    // Here a lookup is a multiplication, a shift and, almost always, one read of a contiguous array.
    //
    // It uses open addressing with linear probing, and never erases a single key, so it needs no marks for erased slots.
    // It stays at most half full, so most lookups, found or not, end at the first or the second slot. The slot comes
    // from Fibonacci hashing: the high bits of the key multiplied by 2^32 / phi, which spread keys that differ only in
    // their low bits, like consecutive code points.
    // Inserting can move every value, so a pointer that Find returns is valid only until the next Insert.
    // Find is always inline: the layout calls it for each character, and some compilers did not inline it there.
    //---------------------------------
    template <class TValue>
    class FlatMap {
        public:
            // The callers never use this key: it marks an empty slot.
            static constexpr uint32_t   kEmpty = 0xffffffffu;

            FlatMap() {
                Rebuild(kMinSlots);
            }

            // Finding the key ends the loop, and an empty slot returns from inside it. Written so, clang keeps both as
            // jumps. With both returns inside the loop, it turned them into conditional moves, and the value could not
            // be read until the comparison ended: the layout alone was about 30 % slower.
            //-------------------------
            FONTRENDERER_ALWAYS_INLINE const TValue *
            Find(uint32_t key) const {
                uint32_t slot = SlotOf(key);
                while (mSlots[slot].key != key) {
                    if (mSlots[slot].key == kEmpty) {
                        return nullptr;
                    }
                    slot = (slot + 1) & mMask;
                }
                return &mSlots[slot].value;
            }

            // As std::unordered_map::insert: it does not change the value of a key already in the map, and then returns false.
            //-------------------------
            bool
            Insert(uint32_t key, const TValue &value) {
                if ((mCount + 1) * 2 > mSlots.size()) {
                    Rebuild(mSlots.size() * 2);
                }
                if (Place(key, value) == false) {
                    return false;
                }
                ++mCount;
                return true;
            }

            //-------------------------
            void
            Clear() {
                mSlots.clear();
                mCount = 0;
                Rebuild(kMinSlots);
            }

            size_t      GetCount() const    { return mCount; }

            // Calls visit(key, value) for each key, in no particular order.
            //-------------------------
            template <class TVisitor>
            void
            ForEach(TVisitor visit) const {
                for (const Slot &entry : mSlots) {
                    if (entry.key != kEmpty) {
                        visit(entry.key, entry.value);
                    }
                }
            }

        protected:
            //-------------------------
            struct Slot {
                uint32_t    key;
                TValue      value;
            };

            static constexpr size_t     kMinSlots = 16;

            //-------------------------
            uint32_t
            SlotOf(uint32_t key) const {
                return (key * 2654435769u) >> mShift;
            }

            //-------------------------
            bool
            Place(uint32_t key, const TValue &value) {
                uint32_t slot = SlotOf(key);
                while (mSlots[slot].key != kEmpty) {
                    if (mSlots[slot].key == key) {
                        return false;
                    }
                    slot = (slot + 1) & mMask;
                }
                mSlots[slot] = { key, value };
                return true;
            }

            // slotCount is a power of two.
            //-------------------------
            void
            Rebuild(size_t slotCount) {
                std::vector<Slot> old(slotCount, Slot { kEmpty, TValue() });
                old.swap(mSlots);

                mMask  = uint32_t(slotCount - 1);
                mShift = 32;
                for (size_t count = slotCount; count > 1; count >>= 1) {
                    --mShift;
                }

                for (const Slot &entry : old) {
                    if (entry.key != kEmpty) {
                        Place(entry.key, entry.value);
                    }
                }
            }

        protected:
            std::vector<Slot>   mSlots;
            size_t              mCount {};
            uint32_t            mMask  {};
            uint32_t            mShift {};
    };

} // end of namespace
