#pragma once

//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>

//-------------------------------------
namespace MindShake {

    // Read-only view of a whole file. It maps the file instead of copying it, except on DOS, which cannot map files.
    //---------------------------------
    class MappedFile {
        public:
            enum class EError {
                None,
                CannotOpen,
                CannotRead,
                OutOfMemory
            };

        public:
                                MappedFile() = default;
                                ~MappedFile()                               { Close();          }

                                MappedFile(const MappedFile &)              = delete;
            MappedFile &        operator=(const MappedFile &)               = delete;

            // An empty file cannot be mapped, so it gives CannotRead.
            EError              Open(const char *fileName);
            void                Close();

            const uint8_t *     GetData() const                             { return mData;     }
            size_t              GetSize() const                             { return mSize;     }

        protected:
            const uint8_t       *mData   {};
            size_t              mSize    {};
            void                *mHandle {};    // The Windows mapping. It is a void * to keep windows.h, and its DrawText macro, out of this header
    };

} // end of namespace
