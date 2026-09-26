#include "UTF8_Utils.h"
//-------------------------------------
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace MindShake;

//-------------------------------------
namespace {

    // Decodes from an exact-size copy, so a sanitizer catches any read past the terminating zero.
    std::vector<uint32_t>
    Decode(const std::string &bytes) {
        std::vector<uint8_t> buffer(bytes.begin(), bytes.end());
        buffer.push_back(0);

        std::vector<uint32_t> codePoints;
        const uint8_t         *text = buffer.data();
        for(uint32_t codePoint = GetNextUTF32(&text); codePoint != 0; codePoint = GetNextUTF32(&text))
            codePoints.push_back(codePoint);

        return codePoints;
    }

} // end of namespace

//-------------------------------------
TEST_CASE("UTF-8 round trip for 1 to 4 byte sequences") {
    const char32_t utf32[] = U"Aa¡Hola! ñ€中\U0001D11E\U0001F600";

    const std::string utf8 = UTF32_2_UTF8(utf32);
    CHECK(utf8 == "Aa\xc2\xa1Hola! \xc3\xb1\xe2\x82\xac\xe4\xb8\xad\xf0\x9d\x84\x9e\xf0\x9f\x98\x80");

    const std::vector<uint32_t> expected(std::begin(utf32), std::end(utf32) - 1);
    CHECK(Decode(utf8) == expected);
}

//-------------------------------------
TEST_CASE("UTF-8 invalid or truncated sequences give the replacement character") {
    const uint32_t R = kReplacementCharacter;

    CHECK(Decode("\xc3")               == std::vector<uint32_t> { R });
    CHECK(Decode("\xe2\x82")           == std::vector<uint32_t> { R });
    CHECK(Decode("\xf0\x9f\x98")       == std::vector<uint32_t> { R });
    CHECK(Decode("\x80" "A")           == std::vector<uint32_t> { R, 'A' });
    CHECK(Decode("\xc3" "A")           == std::vector<uint32_t> { R, 'A' });
    CHECK(Decode("\xff" "B")           == std::vector<uint32_t> { R, 'B' });
    CHECK(Decode("\xe2\x82" "C")       == std::vector<uint32_t> { R, 'C' });
}

//-------------------------------------
TEST_CASE("UTF-8 overlong forms, surrogates and values beyond Unicode give the replacement character") {
    const uint32_t R = kReplacementCharacter;

    CHECK(Decode("A\xc0\x80" "B")      == std::vector<uint32_t> { 'A', R, 'B' });
    CHECK(Decode("\xe0\x80\xaf")       == std::vector<uint32_t> { R });
    CHECK(Decode("\xf0\x80\x80\xaf")   == std::vector<uint32_t> { R });
    CHECK(Decode("\xed\xa0\x80")       == std::vector<uint32_t> { R });
    CHECK(Decode("\xed\xbf\xbf")       == std::vector<uint32_t> { R });
    CHECK(Decode("\xf4\x90\x80\x80")   == std::vector<uint32_t> { R });

    CHECK(Decode("\xc2\x80" "\xdf\xbf" "\xe0\xa0\x80" "\xed\x9f\xbf" "\xee\x80\x80" "\xef\xbf\xbf" "\xf0\x90\x80\x80" "\xf4\x8f\xbf\xbf")
          == std::vector<uint32_t> { 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF });
}

//-------------------------------------
TEST_CASE("UTF-8 decoding stops at the end of the text") {
    const uint8_t *null = nullptr;
    CHECK(GetNextUTF32(nullptr) == 0);
    CHECK(GetNextUTF32(&null)   == 0);

    const uint8_t  empty[] = { 0 };
    const uint8_t *text    = empty;
    CHECK(GetNextUTF32(&text) == 0);
    CHECK(text == empty);
}
