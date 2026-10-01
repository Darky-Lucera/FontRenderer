cmake_minimum_required(VERSION 3.16)
project(FontRenderer VERSION 0.7.0)

# Options
#--------------------------------------
# A project that adds this one with add_subdirectory only builds the library, unless it asks for more.
# PROJECT_IS_TOP_LEVEL would do the same, but it needs CMake 3.21.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FONTRENDERER_IS_TOP_LEVEL ON)
else()
    set(FONTRENDERER_IS_TOP_LEVEL OFF)
endif()
option(FONTRENDERER_BUILD_EXAMPLES   "Build the examples, which need MiniFB"                ${FONTRENDERER_IS_TOP_LEVEL})
option(FONTRENDERER_BUILD_TESTS      "Build the unit tests"                                 ${FONTRENDERER_IS_TOP_LEVEL})
option(FONTRENDERER_USE_STB          "Build the stb_truetype backend"                       ON)
option(FONTRENDERER_USE_LIBSCHRIFT   "Build the libschrift backend"                         ON)
option(FONTRENDERER_USE_FREETYPE     "Build the FreeType backend"                           OFF)
option(FONTRENDERER_USE_BAKED        "Build the backend that reads baked fonts"             ON)

if (FONTRENDERER_USE_STB OR FONTRENDERER_USE_LIBSCHRIFT OR FONTRENDERER_USE_FREETYPE)
    set(FONTRENDERER_HAS_RASTERIZER ON)
else()
    set(FONTRENDERER_HAS_RASTERIZER OFF)
endif()

if (NOT FONTRENDERER_HAS_RASTERIZER AND NOT FONTRENDERER_USE_BAKED)
    message(FATAL_ERROR "FontRenderer needs FONTRENDERER_USE_STB, FONTRENDERER_USE_LIBSCHRIFT, FONTRENDERER_USE_FREETYPE or FONTRENDERER_USE_BAKED")
endif()

# The examples render glyphs from a font file.
if (NOT FONTRENDERER_HAS_RASTERIZER)
    set(FONTRENDERER_BUILD_EXAMPLES OFF)
endif()

# Dependencies
#--------------------------------------
include(FetchContent)

# MiniFB is only for the examples. A project that already has it, like MiniFB when it builds its own examples
# with FontRenderer, keeps its copy.
if (FONTRENDERER_BUILD_EXAMPLES AND NOT TARGET minifb)
    set(MINIFB_BUILD_EXAMPLES     OFF)
    set(MINIFB_BUILD_TESTS        OFF)
    set(MINIFB_BUILD_VERSION_INFO OFF)
    FetchContent_Declare(minifb
        GIT_REPOSITORY https://github.com/emoon/minifb.git
        GIT_TAG        v0.14.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(minifb)
endif()

# FreeType from the system if it has one, and else from its sources.
if (FONTRENDERER_USE_FREETYPE)
    find_package(Freetype QUIET)
    if (NOT TARGET Freetype::Freetype)
        # Otherwise FreeType links any zlib, libpng, HarfBuzz... it finds, and the build changes from one computer to another.
        set(FT_DISABLE_ZLIB     ON)
        set(FT_DISABLE_BZIP2    ON)
        set(FT_DISABLE_PNG      ON)
        set(FT_DISABLE_HARFBUZZ ON)
        set(FT_DISABLE_BROTLI   ON)
        set(SKIP_INSTALL_ALL    ON)
        FetchContent_Declare(freetype
            GIT_REPOSITORY https://github.com/freetype/freetype.git
            GIT_TAG        VER-2-14-3
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()
endif()

# Target options
#--------------------------------------
# DJGPP headers only declare POSIX functions like stat in GNU mode.
if (DJGPP)
    set(FONTRENDERER_EXTENSIONS ON)
else()
    set(FONTRENDERER_EXTENSIONS OFF)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([=[
        #if !defined(_M_IX86) && !defined(_M_X64)
            #error Edit and Continue is not supported on this architecture
        #endif
        int main() { return 0; }
    ]=] FONTRENDERER_MSVC_X86_OR_X64)

    # CMake 3.16 has no target property that can replace /Zi with /ZI.
    if (FONTRENDERER_MSVC_X86_OR_X64)
        foreach(lang C CXX)
            string(REGEX REPLACE "[/-]Z[7iI]" "/ZI" CMAKE_${lang}_FLAGS_DEBUG "${CMAKE_${lang}_FLAGS_DEBUG}")
            if (NOT CMAKE_${lang}_FLAGS_DEBUG MATCHES "[/-]ZI")
                string(APPEND CMAKE_${lang}_FLAGS_DEBUG " /ZI")
            endif()
        endforeach()
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FontRendererHelpers.cmake")

# Set output directory
#--------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin)
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin)

# Library
#--------------------------------------
set(SRC_FontRenderer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontBase.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontC.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontC.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MappedFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MappedFile.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkylineBinPack.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Tga.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Tga.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UTF8_Utils.h
)

if (FONTRENDERER_HAS_RASTERIZER)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Font.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Font.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GposKerning.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GposKerning.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SkylineBinPack.cpp
    )
endif()

if (FONTRENDERER_USE_STB)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontSTB.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontSTB.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/external/stb/stb_truetype.h
    )
endif()

if (FONTRENDERER_USE_LIBSCHRIFT)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontSFT.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontSFT.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/external/libschrift/schrift.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/external/libschrift/schrift.h
    )
endif()

if (FONTRENDERER_USE_FREETYPE)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontFT.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontFT.h
    )
endif()

if (FONTRENDERER_USE_BAKED)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontBaked.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontBaked.h
    )
endif()
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SRC_FontRenderer})

add_library(fontRenderer
STATIC
    ${SRC_FontRenderer}
)

add_library(FontRenderer::FontRenderer ALIAS fontRenderer)

fontrenderer_setup_target(fontRenderer)

target_include_directories(fontRenderer
PUBLIC
    src
)

target_include_directories(fontRenderer
PUBLIC
    src/external
)

# Public, so that code which uses the library knows which backends it has.
# The header of a backend stops the build without its macro, instead of failing later at link time.
if (FONTRENDERER_USE_STB)
    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_USE_STB
    )
endif()

if (FONTRENDERER_USE_LIBSCHRIFT)
    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_USE_LIBSCHRIFT
    )
endif()

if (FONTRENDERER_USE_FREETYPE)
    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_USE_FREETYPE
    )

    target_link_libraries(fontRenderer
    PRIVATE
        Freetype::Freetype
    )
endif()

if (FONTRENDERER_USE_BAKED)
    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_USE_BAKED
    )
endif()

#--------------------------------------
if (FONTRENDERER_BUILD_EXAMPLES)
    set(SRC_ExampleCommon
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleCommon.c
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleCommon.h
    )

    fontrenderer_add_example(exampleRender examples
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleRender.cpp
        ${SRC_ExampleCommon}
    )

    fontrenderer_add_example(exampleRenderC examples
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleRenderC.c
        ${SRC_ExampleCommon}
    )

    # A DJGPP program reopens its own file through a truncated path, so the name must already fit in 8.3.
    if (DJGPP)
        set_target_properties(exampleRender  PROPERTIES OUTPUT_NAME "example")
        set_target_properties(exampleRenderC PROPERTIES OUTPUT_NAME "examplec")
    endif()

    if (FONTRENDERER_USE_BAKED)
        fontrenderer_add_example(exampleBakeEffects examples
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleBakeEffects.cpp
            ${SRC_ExampleCommon}
        )

        fontrenderer_add_example(exampleEffects examples
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleEffects.cpp
            ${SRC_ExampleCommon}
        )

        if (DJGPP)
            set_target_properties(exampleBakeEffects PROPERTIES OUTPUT_NAME "bakefx")
            set_target_properties(exampleEffects     PROPERTIES OUTPUT_NAME "effects")
        endif()
    endif()
endif()

# Unit tests
#--------------------------------------
if (FONTRENDERER_BUILD_TESTS)
    enable_testing()

    set(SRC_UnitTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testMain.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testTga.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testUTF8.cpp
    )

    # The other tests render glyphs from a font file, also to create the files of FontBaked.
    if (FONTRENDERER_HAS_RASTERIZER)
        list(APPEND SRC_UnitTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/testHelpers.h
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/testDrawText.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/testFont.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/testFontC.c
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/testFontC.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/testGposKerning.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/testSkylineBinPack.cpp
        )

        if (FONTRENDERER_USE_BAKED)
            list(APPEND SRC_UnitTests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/testFontBaked.cpp
            )
        endif()
    else()
        list(APPEND SRC_UnitTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/testFontCBakedOnly.cpp
        )
    endif()
    fontrenderer_add_test(unitTests tests
        ${SRC_UnitTests}
    )

    # The tests render with FreeType itself, to check that FontFT draws the same.
    if (FONTRENDERER_USE_FREETYPE)
        target_link_libraries(unitTests
            Freetype::Freetype
        )
    endif()

    # DJGPP (DOS) has neither the threads nor the POSIX signal stacks doctest uses by default.
    if (DJGPP)
        target_compile_definitions(unitTests PRIVATE DOCTEST_CONFIG_NO_MULTITHREADING DOCTEST_CONFIG_NO_POSIX_SIGNALS)
        set_target_properties(unitTests PROPERTIES OUTPUT_NAME "unittest")
    endif()
endif()

# Organize Visual Studio Solution Folders
#--------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
