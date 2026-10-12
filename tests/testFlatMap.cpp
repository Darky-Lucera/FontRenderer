#include "FlatMap.h"
#include "StableMap.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <cstdint>
#include <map>
#include <vector>

using MindShake::FlatMap;
using MindShake::StableMap;

//-------------------------------------
namespace {

    // Keys as the layout makes them: consecutive code points at a few heights, and pairs of glyphs.
    //---------------------------------
    std::vector<uint32_t>
    MakeKeys() {
        std::vector<uint32_t> keys;
        for (uint32_t height : { 14u, 24u, 96u }) {
            for (uint32_t codePoint = 32; codePoint < 600; ++codePoint) {
                keys.push_back((height << 24) | codePoint);
            }
        }
        for (uint32_t left = 1; left < 40; ++left) {
            for (uint32_t right = 1; right < 40; ++right) {
                keys.push_back((left << 16) | right);
            }
        }
        return keys;
    }

} // end of namespace

//-------------------------------------
TEST_CASE("FlatMap finds what was inserted and nothing else") {
    const std::vector<uint32_t> keys = MakeKeys();
    FlatMap<int32_t>            map;
    std::map<uint32_t, int32_t> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        CHECK(map.Insert(keys[i], int32_t(i)));
        reference[keys[i]] = int32_t(i);
    }
    CHECK(map.GetCount() == keys.size());

    for (const auto &entry : reference) {
        const int32_t *value = map.Find(entry.first);
        REQUIRE(value != nullptr);
        CHECK(*value == entry.second);
    }
    CHECK(map.Find(0) == nullptr);
    CHECK(map.Find((200u << 24) | 'A') == nullptr);

    size_t visited = 0;
    map.ForEach([&](uint32_t key, int32_t value) {
        CHECK(reference.at(key) == value);
        ++visited;
    });
    CHECK(visited == keys.size());
}

//-------------------------------------
TEST_CASE("FlatMap keeps the first value of a key") {
    FlatMap<int32_t> map;
    CHECK(map.Insert(7, 1));
    CHECK_FALSE(map.Insert(7, 2));
    CHECK(*map.Find(7) == 1);
    CHECK(map.GetCount() == 1);
}

// Clear shrinks the slots to those of an empty map, so it must drop the keys: there would be no room for them.
//-------------------------------------
TEST_CASE("FlatMap is empty after Clear, whatever it held") {
    const std::vector<uint32_t> keys = MakeKeys();
    FlatMap<int32_t>            map;
    for (int round = 0; round < 2; ++round) {
        for (uint32_t key : keys) {
            map.Insert(key, 1);
        }
        map.Clear();
        CHECK(map.GetCount() == 0);
        CHECK(map.Find(keys[0]) == nullptr);
        CHECK(map.Find(keys.back()) == nullptr);
    }

    CHECK(map.Insert(keys[0], 5));
    CHECK(*map.Find(keys[0]) == 5);
}

//-------------------------------------
TEST_CASE("StableMap keeps its values in place while it grows") {
    const std::vector<uint32_t> keys = MakeKeys();
    StableMap<uint64_t>         map;
    std::vector<const uint64_t *> addresses;
    for (uint32_t key : keys) {
        addresses.push_back(&map.Insert(key, uint64_t(key) * 3));
    }
    CHECK(map.GetCount() == keys.size());

    for (size_t i = 0; i < keys.size(); ++i) {
        const uint64_t *value = map.Find(keys[i]);
        REQUIRE(value != nullptr);
        CHECK(value == addresses[i]);
        CHECK(*value == uint64_t(keys[i]) * 3);
    }

    // Inserting a key that is there gives the value already in the map.
    CHECK(&map.Insert(keys[10], 0) == addresses[10]);
    CHECK(*addresses[10] == uint64_t(keys[10]) * 3);

    size_t index = 0;
    map.ForEach([&](uint32_t key, const uint64_t &value) {
        REQUIRE(index < keys.size());
        CHECK(key == keys[index]);
        CHECK(&value == addresses[index]);
        ++index;
    });
    CHECK(index == keys.size());

    map.Clear();
    CHECK(map.GetCount() == 0);
    CHECK(map.Find(keys[0]) == nullptr);
    CHECK(map.Insert(keys[0], 9) == 9);
}
