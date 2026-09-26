#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2009 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>

//-------------------------------------
namespace MindShake {

    constexpr uint32_t kReplacementCharacter = 0xFFFD;

    // Returns 0 at the end of the text. Invalid or truncated sequences return kReplacementCharacter.
    //---------------------------------
    inline uint32_t
    GetNextUTF32(const uint8_t **text) {
        if(text == nullptr || *text == nullptr || **text == 0) {
            return 0;
        }

        const uint8_t   *bytes = *text;
        const uint8_t   lead   = bytes[0];
        uint32_t        codePoint;
        size_t          length;

        if(lead < 0x80) {
            *text += 1;
            return lead;
        }
        else if((lead & 0xE0) == 0xC0) {
            codePoint = lead & 0x1F;
            length    = 2;
        }
        else if((lead & 0xF0) == 0xE0) {
            codePoint = lead & 0x0F;
            length    = 3;
        }
        else if((lead & 0xF8) == 0xF0) {
            codePoint = lead & 0x07;
            length    = 4;
        }
        else {
            *text += 1;
            return kReplacementCharacter;
        }

        // Each byte is checked before reading the next one, so a truncated sequence stops at the terminating zero.
        for(size_t i = 1; i < length; ++i) {
            if((bytes[i] & 0xC0) != 0x80) {
                *text += i;
                return kReplacementCharacter;
            }
            codePoint = (codePoint << 6) | (bytes[i] & 0x3F);
        }

        // An overlong form could encode a zero, which would end the text early.
        const uint32_t minimum = (length == 2) ? 0x80 : (length == 3) ? 0x800 : 0x10000;
        if(codePoint < minimum || (codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) {
            codePoint = kReplacementCharacter;
        }

        *text += length;
        return codePoint;
    }

    //---------------------------------
    inline std::string
    UTF32_2_UTF8(const char32_t *utf32) {
        std::string utf8;

        for(; *utf32 != 0; ++utf32) {
            const uint32_t codePoint = *utf32;

            if(codePoint < 0x80) {
                utf8 += char(codePoint);
            }
            else if(codePoint < 0x800) {
                utf8 += char(0xC0 |  (codePoint >> 6));
                utf8 += char(0x80 |  (codePoint        & 0x3F));
            }
            else if(codePoint < 0x10000) {
                utf8 += char(0xE0 |  (codePoint >> 12));
                utf8 += char(0x80 | ((codePoint >> 6)  & 0x3F));
                utf8 += char(0x80 |  (codePoint        & 0x3F));
            }
            else {
                utf8 += char(0xF0 |  (codePoint >> 18));
                utf8 += char(0x80 | ((codePoint >> 12) & 0x3F));
                utf8 += char(0x80 | ((codePoint >> 6)  & 0x3F));
                utf8 += char(0x80 |  (codePoint        & 0x3F));
            }
        }

        return utf8;
    }

} // end of namespace
