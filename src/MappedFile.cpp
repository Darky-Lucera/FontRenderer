//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "MappedFile.h"
//-------------------------------------
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__DJGPP__)
    #include <cstdio>
    #include <new>
    #include <sys/stat.h>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

using namespace MindShake;

#if defined(_WIN32)

//-------------------------------------
static bool
IsOutOfMemory(DWORD error) {
    return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY || error == ERROR_COMMITMENT_LIMIT;
}

//-------------------------------------
MappedFile::EError
MappedFile::Open(const char *fileName) {
    Close();

    HANDLE file = CreateFileA(fileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE) {
        return EError::CannotOpen;
    }

    LARGE_INTEGER size;
    if(GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0) {
        CloseHandle(file);
        return EError::CannotRead;
    }
    if(uint64_t(size.QuadPart) > SIZE_MAX) {
        CloseHandle(file);
        return EError::OutOfMemory;
    }

    // The mapping keeps its own reference to the file.
    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if(mapping == nullptr) {
        return IsOutOfMemory(GetLastError()) ? EError::OutOfMemory : EError::CannotRead;
    }

    const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if(view == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(mapping);
        return IsOutOfMemory(error) ? EError::OutOfMemory : EError::CannotRead;
    }

    mData   = static_cast<const uint8_t *>(view);
    mSize   = size_t(size.QuadPart);
    mHandle = mapping;

    return EError::None;
}

//-------------------------------------
void
MappedFile::Close() {
    if(mData != nullptr) {
        UnmapViewOfFile(mData);
    }
    if(mHandle != nullptr) {
        CloseHandle(mHandle);
    }

    mData   = nullptr;
    mSize   = 0;
    mHandle = nullptr;
}

#elif defined(__DJGPP__)

//-------------------------------------
MappedFile::EError
MappedFile::Open(const char *fileName) {
    Close();

    struct stat info;
    if(stat(fileName, &info) != 0 || S_ISREG(info.st_mode) == false) {
        return EError::CannotOpen;
    }
    if(info.st_size <= 0) {
        return EError::CannotRead;
    }

    FILE *file = fopen(fileName, "rb");
    if(file == nullptr) {
        return EError::CannotOpen;
    }

    uint8_t *data = new (std::nothrow) uint8_t[size_t(info.st_size)];
    if(data == nullptr) {
        fclose(file);
        return EError::OutOfMemory;
    }

    const size_t read = fread(data, size_t(info.st_size), 1, file);
    fclose(file);
    if(read != 1) {
        delete[] data;
        return EError::CannotRead;
    }

    mData = data;
    mSize = size_t(info.st_size);

    return EError::None;
}

//-------------------------------------
void
MappedFile::Close() {
    delete[] mData;

    mData = nullptr;
    mSize = 0;
}

#else

//-------------------------------------
MappedFile::EError
MappedFile::Open(const char *fileName) {
    Close();

    const int file = open(fileName, O_RDONLY);
    if(file < 0) {
        return EError::CannotOpen;
    }

    // Unlike Windows, open() accepts a directory, so reject it here too.
    struct stat info;
    if(fstat(file, &info) != 0 || S_ISREG(info.st_mode) == false) {
        close(file);
        return EError::CannotOpen;
    }
    if(info.st_size <= 0) {
        close(file);
        return EError::CannotRead;
    }
    if(uint64_t(info.st_size) > SIZE_MAX) {
        close(file);
        return EError::OutOfMemory;
    }

    // The mapping stays valid after closing the file.
    void *data = mmap(nullptr, size_t(info.st_size), PROT_READ, MAP_PRIVATE, file, 0);
    const int error = errno;
    close(file);
    if(data == MAP_FAILED) {
        return (error == ENOMEM) ? EError::OutOfMemory : EError::CannotRead;
    }

    mData = static_cast<const uint8_t *>(data);
    mSize = size_t(info.st_size);

    return EError::None;
}

//-------------------------------------
void
MappedFile::Close() {
    if(mData != nullptr) {
        munmap(const_cast<uint8_t *>(mData), mSize);
    }

    mData = nullptr;
    mSize = 0;
}

#endif
