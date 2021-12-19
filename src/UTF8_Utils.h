#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2009 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include <string>

//-------------------------------------
namespace MindShake {

    //---------------------------------
    inline uint32_t
    GetNextUTF32(const uint8_t **text) {
        static const uint8_t  UTF8_Bytes[256] = {
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,

            2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
            2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,

            3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,

            4,4,4,4,4,4,4,4,5,5,5,5,6,6,6,6,
        };

        //static const uint32_t gOffsetsFromUTF8[6] = {
        //    0x00000000UL, 0x00003080UL, 0x000E2080UL, 0x03C82080UL, 0xFA082080UL, 0x82082080UL
        //};

        if(text == nullptr || *text == nullptr || **text == 0)
            return 0;

        size_t      length = UTF8_Bytes[**text];
        uint32_t    codePoint = 0;
        switch(length) {
            case 1:
                codePoint = uint32_t(*(*text + 0) & ~0x80);
                (*text) += 1;
                break;

            case 2:
                codePoint  = uint32_t(*(*text + 0) & ~0xE0) <<  6;
                codePoint |= uint32_t(*(*text + 1) & ~0xC0);
                (*text) += 2;
                break;

            case 3:
                codePoint  = uint32_t(*(*text + 0) & ~0xF0) << 12;
                codePoint |= uint32_t(*(*text + 1) & ~0xC0) <<  6;
                codePoint |= uint32_t(*(*text + 2) & ~0xC0);
                (*text) += 3;
                break;

            case 4:
                codePoint  = uint32_t(*(*text + 0) & ~0xF8) << 18;
                codePoint |= uint32_t(*(*text + 1) & ~0xC0) << 12;
                codePoint |= uint32_t(*(*text + 2) & ~0xC0) <<  6;
                codePoint |= uint32_t(*(*text + 3) & ~0xC0);
                (*text) += 4;
                break;

            // Error
            default:
                codePoint = 0;
                break;
        }

        return codePoint;
    }

    //---------------------------------
    inline std::string
    UTF32_2_UTF8(const char32_t *utf32) {
        std::string utf8;

        while(*utf32 != 0) {
            if (*utf32 < 0x80)
                utf8 += static_cast<uint8_t>(*utf32++);
            else if (*utf32 < 0x800) {
                utf8 += static_cast<uint8_t>((*utf32++ >> 6)          | 0xc0);
                utf8 += static_cast<uint8_t>((*utf32++ & 0x3f)        | 0x80);
            }
            else if (*utf32 < 0x10000) {
                utf8 += static_cast<uint8_t>((*utf32++ >> 12)         | 0xe0);
                utf8 += static_cast<uint8_t>(((*utf32++ >> 6) & 0x3f) | 0x80);
                utf8 += static_cast<uint8_t>((*utf32++ & 0x3f)        | 0x80);
            }
            else {
                utf8 += static_cast<uint8_t>((*utf32++ >> 18)         | 0xf0);
                utf8 += static_cast<uint8_t>(((*utf32++ >> 12) & 0x3f)| 0x80);
                utf8 += static_cast<uint8_t>(((*utf32++ >> 6) & 0x3f) | 0x80);
                utf8 += static_cast<uint8_t>((*utf32++ & 0x3f)        | 0x80);
            }
        }

        return utf8;
    }


} // end of namespace
