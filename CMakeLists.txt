cmake_minimum_required(VERSION 3.16)
project(FontRenderer VERSION 0.5.0)

# Options
#--------------------------------------
# A project that adds this one with add_subdirectory only builds the library, unless it asks for more.
# PROJECT_IS_TOP_LEVEL would do the same, but it needs CMake 3.21.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FONTRENDERER_IS_TOP_LEVEL ON)
else()
    set(FONTRENDERER_IS_TOP_LEVEL OFF)
endif()
option(FONTRENDERER_BUILD_EXAMPLES "Build the examples, which need MiniFB"                ${FONTRENDERER_IS_TOP_LEVEL})
option(FONTRENDERER_BUILD_TESTS    "Build the unit tests"                                ${FONTRENDERER_IS_TOP_LEVEL})

option(FONTRENDERER_USE_STB        "Build the stb_truetype backend"                      ON)
option(FONTRENDERER_USE_LIBSCHRIFT "Build the libschrift backend"                        ON)
option(FONTRENDERER_USE_FREETYPE   "Build the FreeType backend"                          OFF)

if(NOT FONTRENDERER_USE_STB AND NOT FONTRENDERER_USE_LIBSCHRIFT AND NOT FONTRENDERER_USE_FREETYPE)
    message(FATAL_ERROR "FontRenderer needs FONTRENDERER_USE_STB, FONTRENDERER_USE_LIBSCHRIFT or FONTRENDERER_USE_FREETYPE")
endif()

# Dependencies
#--------------------------------------
include(FetchContent)

# MiniFB is only for the examples. A project that already has it, like MiniFB when it builds its own examples
# with FontRenderer, keeps its copy.
if(FONTRENDERER_BUILD_EXAMPLES AND NOT TARGET minifb)
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
if(FONTRENDERER_USE_FREETYPE)
    find_package(Freetype QUIET)
    if(NOT TARGET Freetype::Freetype)
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

# C/C++ version
#--------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
# DJGPP headers only declare POSIX functions like stat in GNU mode.
if(DJGPP)
    set(CMAKE_CXX_EXTENSIONS ON)
    set(CMAKE_C_EXTENSIONS ON)
else()
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_C_EXTENSIONS OFF)
endif()

# Set output directory
#--------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin)
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin)

# Compiler flags
#--------------------------------------
if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)

    # Warning level
    add_compile_options(/W3)
    # add_compile_options(/wd4100)    # unused parameter
    # add_compile_options(/wd4324)    # structure was padded due to alignment specifier
    # add_compile_options(/wd4201)    # nonstandard extension used: nameless struct/union
    add_compile_options(/wd4244)    # 'conversion' conversion from 'type1' to 'type2', possible loss of data
    add_compile_options(/wd26451)   # arithmetic overflow
    # add_compile_options(/wd26812)   # arithmetic overflow
    # Security check
    add_compile_options(/GS)
    # Function level linking
    add_compile_options(/Gy)
    # Exceptions
    add_compile_options(/EHsc)
    # Enable Minimal Rebuild (required for Edit and Continue) (deprecated)
    if(MSVC_VERSION LESS_EQUAL 1920)
        add_compile_options(/Gm)
    endif()
    # SDL checks 2015+
    if(MSVC_VERSION GREATER_EQUAL 1900)
        add_compile_options(/sdl)
    endif()
    add_compile_options(/fp:fast)
    # Runtime library
    add_compile_options("$<IF:$<CONFIG:Debug>,/MDd,/MD>")
    # Program database for edit and continue
    add_compile_options("$<IF:$<CONFIG:Debug>,/ZI,/Zi>")
    # Optimizations
    add_compile_options("$<IF:$<CONFIG:Debug>,/Od,/O2>")
    # Inline function expansion
    add_compile_options("$<IF:$<CONFIG:Debug>,/Ob0,/Ob2>")
    # Basic runtime checks
    add_compile_options("$<$<CONFIG:Debug>:/RTC1>")
    # Enable intrinsics
    add_compile_options("$<$<CONFIG:RELEASE>:/Oi>")
    # Force Visual Studio to actualize __cplusplus version macro
    add_compile_options(/Zc:__cplusplus)
    # Read the sources as UTF-8 instead of the system code page
    add_compile_options(/utf-8)
else()
    add_compile_options("$<$<CONFIG:Debug>:-g>")
    add_compile_options("$<IF:$<CONFIG:Debug>,-O0,-O2>")
    add_compile_options(-Wall -Wextra)
    add_compile_options(-Wno-switch -Wno-unused-function -Wno-unused-parameter)
    add_compile_options(-ffast-math)
endif()

#--------------------------------------
set(SRC_FontRenderer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Font.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontC.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontC.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GposKerning.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GposKerning.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MappedFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MappedFile.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkylineBinPack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkylineBinPack.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UTF8_Utils.h
)
if(FONTRENDERER_USE_STB)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontSTB.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontSTB.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/external/stb/stb_truetype.h
    )
endif()
if(FONTRENDERER_USE_LIBSCHRIFT)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontSFT.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontSFT.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/external/libschrift/schrift.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/external/libschrift/schrift.h
    )
endif()
if(FONTRENDERER_USE_FREETYPE)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontFT.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontFT.h
    )
endif()
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SRC_FontRenderer})

add_library(fontRenderer STATIC
    ${SRC_FontRenderer}
)
target_include_directories(fontRenderer PUBLIC src)
target_include_directories(fontRenderer PUBLIC src/external)
# Public, so that code which uses the library knows which backends it has.
# The header of a backend stops the build without its macro, instead of failing later at link time.
if(FONTRENDERER_USE_STB)
    target_compile_definitions(fontRenderer PUBLIC FONTRENDERER_USE_STB)
endif()
if(FONTRENDERER_USE_LIBSCHRIFT)
    target_compile_definitions(fontRenderer PUBLIC FONTRENDERER_USE_LIBSCHRIFT)
endif()
if(FONTRENDERER_USE_FREETYPE)
    target_compile_definitions(fontRenderer PUBLIC FONTRENDERER_USE_FREETYPE)
    target_link_libraries(fontRenderer PRIVATE Freetype::Freetype)
endif()

#--------------------------------------
if(FONTRENDERER_BUILD_EXAMPLES)
    set(SRC_ExampleCommon
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleCommon.c
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleCommon.h
    )

    set(SRC_ExampleRender
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleRender.cpp
        ${SRC_ExampleCommon}
    )
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/examples" FILES ${SRC_ExampleRender})

    add_executable(exampleRender
        ${SRC_ExampleRender}
    )
    target_link_libraries(exampleRender fontRenderer)
    target_link_libraries(exampleRender minifb)

    set(SRC_ExampleRenderC
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/exampleRenderC.c
        ${SRC_ExampleCommon}
    )
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/examples" FILES ${SRC_ExampleRenderC})

    add_executable(exampleRenderC
        ${SRC_ExampleRenderC}
    )
    target_link_libraries(exampleRenderC fontRenderer)
    target_link_libraries(exampleRenderC minifb)

    # A DJGPP program reopens its own file through a truncated path, so the name must already fit in 8.3.
    if(DJGPP)
        set_target_properties(exampleRender PROPERTIES OUTPUT_NAME "example")
        set_target_properties(exampleRenderC PROPERTIES OUTPUT_NAME "examplec")
    endif()
endif()

# Unit tests
#--------------------------------------
if(FONTRENDERER_BUILD_TESTS)
    enable_testing()

    set(SRC_UnitTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testMain.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testHelpers.h
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testDrawText.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testFont.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testFontC.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testFontC.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testGposKerning.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testSkylineBinPack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testUTF8.cpp
    )
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/tests" FILES ${SRC_UnitTests})

    add_executable(unitTests
        ${SRC_UnitTests}
    )
    target_include_directories(unitTests PRIVATE tests tests/external)
    target_compile_definitions(unitTests PRIVATE FONT_RENDERER_TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/bin/resources/")
    target_link_libraries(unitTests fontRenderer)
    # The tests render with FreeType itself, to check that FontFT draws the same.
    if(FONTRENDERER_USE_FREETYPE)
        target_link_libraries(unitTests Freetype::Freetype)
    endif()
    # DJGPP (DOS) has neither the threads nor the POSIX signal stacks doctest uses by default.
    if(DJGPP)
        target_compile_definitions(unitTests PRIVATE DOCTEST_CONFIG_NO_MULTITHREADING DOCTEST_CONFIG_NO_POSIX_SIGNALS)
        set_target_properties(unitTests PROPERTIES OUTPUT_NAME "unittest")
    endif()

    add_test(NAME unitTests COMMAND unitTests)
endif()

# Organize Visual Studio Solution Folders
#--------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
