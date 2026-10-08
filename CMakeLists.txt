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
option(FONTRENDERER_BUILD_BENCHMARKS "Build the benchmark of the drawing code"              OFF)

option(FONTRENDERER_USE_STB          "Build the stb_truetype backend"                       ON)
option(FONTRENDERER_USE_LIBSCHRIFT   "Build the libschrift backend"                         ON)
option(FONTRENDERER_USE_FREETYPE     "Build the FreeType backend"                           OFF)
option(FONTRENDERER_USE_BAKED        "Build the backend that reads baked fonts"             ON)
option(FONTRENDERER_DISABLE_SIMD     "Draw only with the scalar code, no SSE2 or NEON"      OFF)

# x86-64-v2 adds SSE3, SSSE3, SSE4.1, SSE4.2 and POPCNT to the default x86-64 level, which already includes SSE and
# SSE2. Intel processors have it since Nehalem (2008), and AMD ones since Bulldozer (2011). The Steam Hardware Survey of
# September 2025 gives SSE4.2, the newest part of the level, on 99.8% of the computers. Steam users have newer computers
# than most people, but the level is common enough that Red Hat Enterprise Linux 9 and its derivatives require it.
#
# The gain is small. Against the drawing of the default level, the glyphs drew 6% faster on average over 17
# combinations of processor and compiler: 10% with an opaque text, 3% with a translucent one and 5% with a texture of
# BGRA32. Building the rest of the library for x86-64-v2 made no difference that could be measured.
#
# ON builds the whole library for x86-64-v2. On a processor without it, the library stops the program at start, with a
# message. OFF builds the library for the default x86-64 level, plus a second copy of the drawing for x86-64-v2, which
# it only uses when cpuid says that the processor has the level. Only 64-bit x86 builds use the option.
option(FONTRENDERER_X86_64_V2        "Build the library for x86-64-v2; OFF only uses it in the drawing, if the processor has it" ON)

# x86-64-v3 adds AVX, AVX2, BMI1, BMI2, FMA, F16C, LZCNT and MOVBE to x86-64-v2. Intel processors have it since Haswell
# (2013), except some Pentium and Celeron, and AMD ones since Excavator (2015). It is common in 2026, but less than
# x86-64-v2, so the option is off.
#
# The library has a copy of the drawing for x86-64-v3, which draws a glyph 8 pixels wide or more with registers of 256
# bits. Against the drawing for x86-64-v2, on three Intel processors (Haswell, Comet Lake and Alder Lake) with MSVC, GCC
# and clang, the glyphs of the measured texts drew between 4% slower and 12% faster at 14 pixels, between 3% and 35%
# faster at 24, between 11% and 50% at 40, and between 29% and 53% with BGRA32. Building the whole library for the
# level added little in the measures, and lets the compiler use FMA in the rasterizers, which can change a glyph by one
# level in some pixel.
#
# ON builds the whole library for x86-64-v3, whatever FONTRENDERER_X86_64_V2 says, and stops the program at start, with a
# message, on a processor without the level. OFF builds that copy for x86-64-v3 alone, and the library only uses it
# when cpuid says that the processor has the level. Only 64-bit x86 builds use the option.
option(FONTRENDERER_X86_64_V3        "Build the library for x86-64-v3; OFF only uses it in the drawing, if the processor has it" OFF)

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
# A program for Android or iOS runs on the device, so it stays in the build folder. In bin it would replace the
# program of the same name built for macOS or Linux, or for another processor of the device.
if (NOT ANDROID AND NOT IOS)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
    SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin)
    SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin)
endif()

# Library
#--------------------------------------
set(SRC_FontRenderer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontBase.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontC.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FontC.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDraw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDraw.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MappedFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MappedFile.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkylineBinPack.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Tga.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Tga.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UTF8_Utils.h
)

# Without an x86 processor GlyphDrawSSE2.cpp compiles to nothing, and the tool that builds the library on macOS
# warns about an object file without symbols. The check only passes when no architecture of the build is x86:
# a universal build of macOS, which has both, still needs the file.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
    #if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
        #error The target is x86
    #endif
    int main() { return 0; }
]=] FONTRENDERER_TARGET_WITHOUT_X86)

# x86-64-v2 only exists for 64-bit x86. A universal build of macOS also builds for ARM, which does not take the option,
# so it only goes to the x86_64 part.
set(FONTRENDERER_X86_64_V2_FLAGS "")
if (NOT FONTRENDERER_TARGET_WITHOUT_X86 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    include(CheckCXXCompilerFlag)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(FONTRENDERER_X86_64_V2_CANDIDATE "/arch:SSE4.2")
    elseif (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
        set(FONTRENDERER_X86_64_V2_CANDIDATE "-Xarch_x86_64;-march=x86-64-v2")
    else()
        # clang-cl does not take /arch:SSE4.2, but it takes this.
        set(FONTRENDERER_X86_64_V2_CANDIDATE "-march=x86-64-v2")
    endif()
    check_cxx_compiler_flag("${FONTRENDERER_X86_64_V2_CANDIDATE}" FONTRENDERER_HAS_X86_64_V2_FLAG)
    if (FONTRENDERER_HAS_X86_64_V2_FLAG)
        set(FONTRENDERER_X86_64_V2_FLAGS "${FONTRENDERER_X86_64_V2_CANDIDATE}")
    else()
        message(STATUS "FontRenderer: the compiler does not take ${FONTRENDERER_X86_64_V2_CANDIDATE}, so there is no code for x86-64-v2")
    endif()
endif()

# The same for x86-64-v3. MSVC has no option for the whole level: /arch:AVX2 also lets it use FMA and BMI.
set(FONTRENDERER_X86_64_V3_FLAGS "")
if (FONTRENDERER_X86_64_V2_FLAGS)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(FONTRENDERER_X86_64_V3_CANDIDATE "/arch:AVX2")
    elseif (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
        set(FONTRENDERER_X86_64_V3_CANDIDATE "-Xarch_x86_64;-march=x86-64-v3")
    else()
        set(FONTRENDERER_X86_64_V3_CANDIDATE "-march=x86-64-v3")
    endif()
    check_cxx_compiler_flag("${FONTRENDERER_X86_64_V3_CANDIDATE}" FONTRENDERER_HAS_X86_64_V3_FLAG)
    if (FONTRENDERER_HAS_X86_64_V3_FLAG)
        set(FONTRENDERER_X86_64_V3_FLAGS "${FONTRENDERER_X86_64_V3_CANDIDATE}")
    else()
        message(STATUS "FontRenderer: the compiler does not take ${FONTRENDERER_X86_64_V3_CANDIDATE}, so there is no code for x86-64-v3")
    endif()
endif()

# The level of the whole library: v3, v2, or empty for the default x86-64 level.
set(FONTRENDERER_LIBRARY_LEVEL "")
if (FONTRENDERER_X86_64_V3 AND FONTRENDERER_X86_64_V3_FLAGS)
    set(FONTRENDERER_LIBRARY_LEVEL "v3")
elseif (FONTRENDERER_X86_64_V2 AND FONTRENDERER_X86_64_V2_FLAGS)
    set(FONTRENDERER_LIBRARY_LEVEL "v2")
endif()

if (FONTRENDERER_X86_64_V2_FLAGS AND (FONTRENDERER_LIBRARY_LEVEL OR NOT FONTRENDERER_DISABLE_SIMD))
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CpuX86.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CpuX86.h
    )
endif()

if (NOT FONTRENDERER_DISABLE_SIMD AND NOT FONTRENDERER_TARGET_WITHOUT_X86)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawSSE2.cpp
    )
    if (FONTRENDERER_X86_64_V2_FLAGS)
        list(APPEND SRC_FontRenderer
            ${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawX64v2.cpp
        )
        # GlyphDrawX64v2.cpp includes GlyphDrawSSE2.cpp. Built in one unity file, both would define the same functions.
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawX64v2.cpp
        PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON
        )
        if (FONTRENDERER_LIBRARY_LEVEL)
            # With the whole library built for x86-64-v2 or later, GlyphDrawSSE2.cpp builds nothing by itself:
            # GlyphDrawX64v2.cpp includes it.
            set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawSSE2.cpp
            PROPERTIES
                HEADER_FILE_ONLY ON
            )
        else()
            set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawX64v2.cpp
            PROPERTIES
                COMPILE_OPTIONS "${FONTRENDERER_X86_64_V2_FLAGS}"
            )
        endif()
    endif()
    if (FONTRENDERER_X86_64_V3_FLAGS)
        list(APPEND SRC_FontRenderer
            ${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawX64v3.cpp
        )
        # Like GlyphDrawX64v2.cpp, it includes GlyphDrawSSE2.cpp.
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawX64v3.cpp
        PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON
        )
        if (NOT FONTRENDERER_LIBRARY_LEVEL STREQUAL "v3")
            set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawX64v3.cpp
            PROPERTIES
                COMPILE_OPTIONS "${FONTRENDERER_X86_64_V3_FLAGS}"
            )
        endif()
    endif()
endif()

# The same for GlyphDrawNeon.cpp and ARM: a universal build of macOS needs the file for its ARM architecture.
check_cxx_source_compiles([=[
    #if defined(_M_ARM64) || defined(__aarch64__) || defined(_M_ARM) || defined(__arm__)
        #error The target is ARM
    #endif
    int main() { return 0; }
]=] FONTRENDERER_TARGET_WITHOUT_ARM)

if (NOT FONTRENDERER_DISABLE_SIMD AND NOT FONTRENDERER_TARGET_WITHOUT_ARM)
    list(APPEND SRC_FontRenderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GlyphDrawNeon.cpp
    )
endif()

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

# Public, because the header that declares the drawing functions leaves out the SSE2 and NEON ones with it.
if (FONTRENDERER_DISABLE_SIMD)
    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_DISABLE_SIMD
    )
endif()

# Public, so that the tests and the benchmark know which drawing functions GlyphDraw.h declares. Platform.h explains
# the macros.
if (FONTRENDERER_LIBRARY_LEVEL STREQUAL "v3")
    target_compile_options(fontRenderer
    PRIVATE
        ${FONTRENDERER_X86_64_V3_FLAGS}
    )

    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_X86_64_V2
        FONTRENDERER_X86_64_V3
    )
elseif (FONTRENDERER_LIBRARY_LEVEL STREQUAL "v2")
    target_compile_options(fontRenderer
    PRIVATE
        ${FONTRENDERER_X86_64_V2_FLAGS}
    )

    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_X86_64_V2
    )
elseif (FONTRENDERER_X86_64_V2_FLAGS AND NOT FONTRENDERER_DISABLE_SIMD)
    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_X86_64_V2_AT_RUNTIME
    )
endif()

if (FONTRENDERER_X86_64_V3_FLAGS AND NOT FONTRENDERER_LIBRARY_LEVEL STREQUAL "v3" AND NOT FONTRENDERER_DISABLE_SIMD)
    target_compile_definitions(fontRenderer
    PUBLIC
        FONTRENDERER_X86_64_V3_AT_RUNTIME
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
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/testGlyphDraw.cpp
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

    # Some tests rasterize with their own copy of a backend and expect the same pixels. With fast floating point, a
    # build for x86-64-v3 may fuse a multiplication and an addition into FMA, and give another value in some pixel.
    if (FONTRENDERER_LIBRARY_LEVEL STREQUAL "v3")
        target_compile_options(unitTests
        PRIVATE
            ${FONTRENDERER_X86_64_V3_FLAGS}
        )
    elseif (FONTRENDERER_LIBRARY_LEVEL STREQUAL "v2")
        target_compile_options(unitTests
        PRIVATE
            ${FONTRENDERER_X86_64_V2_FLAGS}
        )
    endif()

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

# Benchmarks
#--------------------------------------
# They render glyphs from a font file. Their times only mean something in a Release build.
if (FONTRENDERER_BUILD_BENCHMARKS AND FONTRENDERER_HAS_RASTERIZER)
    set(SRC_Benchmarks
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkCommon.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkCommon.h
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkDraw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkSse2.h
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkSystem.h
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Baseline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsPerCase.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsPerGlyph.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsPerCaseAlpha8.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Div255.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsPremultiplied.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsNoInline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsNoInlineCast.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsHybrid.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsAlpha8.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/MoreJumpsAlpha8Hoisted.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/NoJumps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Linear.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Pixel32.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Premultiplied.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Restrict.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/RoundEach.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/RoundOnce.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/RoundOnce64.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/RoundOnce64Premultiplied.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/RoundOnceOver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/RoundOnceSwar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/RoundOnceSwarPremultiplied.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Scalar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Scalar64.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/ScalarSwar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Swar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Truncate.cpp
        # The variants with SSE2. Each of these files compiles to nothing for a processor without it.
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Pixman.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/PixmanHybrid.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/PixmanRotated.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/PixmanUnaligned.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/RoundOnceSse2Premultiplied.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Sse2.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Sse2Overlap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Sse2PerCase.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Sse2RoundOnce.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Sse2Spread.cpp
        # The variants with NEON. Each of these files compiles to nothing for a processor without it.
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/NeonOverlap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/NeonPacked.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/NeonPerCase.cpp
    )

    # Only the file of the X64v2 variants is built for x86-64-v2, so that the other variants stay SSE2 code. The processor
    # that runs the benchmark must have that level: the file never asks the processor for it, and the linker can take
    # from it the copy of an inline function that other files share.
    set(FONTRENDERER_X64V2_FLAG "")
    if (NOT FONTRENDERER_DISABLE_SIMD)
        set(FONTRENDERER_X64V2_FLAG "${FONTRENDERER_X86_64_V2_FLAGS}")
    endif()

    if (FONTRENDERER_X64V2_FLAG)
        list(APPEND SRC_Benchmarks
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/X64v2.cpp
        )
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/X64v2.cpp
        PROPERTIES
            COMPILE_OPTIONS "${FONTRENDERER_X64V2_FLAG}"
        )
    endif()

    # The same for the files of the variants for x86-64-v3.
    set(FONTRENDERER_X64V3_FLAG "")
    if (NOT FONTRENDERER_DISABLE_SIMD)
        set(FONTRENDERER_X64V3_FLAG "${FONTRENDERER_X86_64_V3_FLAGS}")
    endif()

    if (FONTRENDERER_X64V3_FLAG)
        list(APPEND SRC_Benchmarks
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/X64v3Vex.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Avx2.cpp
        )
        set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/X64v3Vex.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Avx2.cpp
        PROPERTIES
            COMPILE_OPTIONS "${FONTRENDERER_X64V3_FLAG}"
        )
    endif()

    # The Aligned variants build Sse2Overlap.cpp and X64v2.cpp again with their loops aligned to 32 bytes and no jump
    # that crosses or ends on a 32-byte boundary. MSVC has no option to align loops, so with it they only move the jumps.
    # They are left out unless the compiler takes every flag.
    set(FONTRENDERER_ALIGN_FLAGS "")
    if (NOT FONTRENDERER_DISABLE_SIMD AND NOT FONTRENDERER_TARGET_WITHOUT_X86)
        include(CheckCXXCompilerFlag)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            set(FONTRENDERER_ALIGN_FLAGS "/QIntel-jcc-erratum")
        elseif (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            set(FONTRENDERER_ALIGN_FLAGS "/clang:-falign-loops=32" "/clang:-mbranches-within-32B-boundaries")
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(FONTRENDERER_ALIGN_FLAGS "-falign-loops=32" "-mbranches-within-32B-boundaries")
        else()
            # GCC leaves the jumps to the assembler, which has the option since binutils 2.34.
            set(FONTRENDERER_ALIGN_FLAGS "-falign-loops=32" "-Wa,-mbranches-within-32B-boundaries")
        endif()
        foreach (flag IN LISTS FONTRENDERER_ALIGN_FLAGS)
            string(MAKE_C_IDENTIFIER "FONTRENDERER_HAS_FLAG_${flag}" flag_result)
            check_cxx_compiler_flag("${flag}" ${flag_result})
            if (NOT ${flag_result})
                set(FONTRENDERER_ALIGN_FLAGS "")
                break()
            endif()
        endforeach()
    endif()

    if (FONTRENDERER_ALIGN_FLAGS)
        list(APPEND SRC_Benchmarks
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Sse2OverlapAligned.cpp
        )
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/Sse2OverlapAligned.cpp
        PROPERTIES
            COMPILE_OPTIONS "${FONTRENDERER_ALIGN_FLAGS}"
        )
        if (FONTRENDERER_X64V2_FLAG)
            list(APPEND SRC_Benchmarks
                ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/X64v2Aligned.cpp
            )
            set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/variants/X64v2Aligned.cpp
            PROPERTIES
                COMPILE_OPTIONS "${FONTRENDERER_X64V2_FLAG};${FONTRENDERER_ALIGN_FLAGS}"
            )
        endif()
    endif()
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks" FILES ${SRC_Benchmarks})

    add_executable(benchmarkDraw
        ${SRC_Benchmarks}
    )

    fontrenderer_setup_target(benchmarkDraw)

    target_include_directories(benchmarkDraw
    PRIVATE
        benchmarks
    )

    # On Android the benchmark runs on the device, from the folder where benchmarks/scripts/benchmarkAndroid.bat
    # copies it and its resources. Both paths can be given to CMake. Relative ones, such as "resources/" and "./",
    # start at the folder the benchmark runs from, so that benchmark can be taken to another computer.
    if (ANDROID)
        set(FONTRENDERER_DEFAULT_RESOURCES "/data/local/tmp/fontrenderer/resources/")
        set(FONTRENDERER_DEFAULT_OUTPUT    "/data/local/tmp/fontrenderer/")
    else()
        set(FONTRENDERER_DEFAULT_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/bin/resources/")
        set(FONTRENDERER_DEFAULT_OUTPUT    "${CMAKE_CURRENT_BINARY_DIR}/")
    endif()
    if (NOT DEFINED FONTRENDERER_BENCHMARK_RESOURCES)
        set(FONTRENDERER_BENCHMARK_RESOURCES "${FONTRENDERER_DEFAULT_RESOURCES}")
    endif()
    if (NOT DEFINED FONTRENDERER_BENCHMARK_OUTPUT)
        set(FONTRENDERER_BENCHMARK_OUTPUT    "${FONTRENDERER_DEFAULT_OUTPUT}")
    endif()

    target_compile_definitions(benchmarkDraw
    PRIVATE
        FONT_RENDERER_BENCHMARK_RESOURCES="${FONTRENDERER_BENCHMARK_RESOURCES}"
    )

    target_compile_definitions(benchmarkDraw
    PRIVATE
        FONT_RENDERER_BENCHMARK_OUTPUT="${FONTRENDERER_BENCHMARK_OUTPUT}"
    )

    target_link_libraries(benchmarkDraw
    PRIVATE
        fontRenderer)

    if (FONTRENDERER_X64V2_FLAG)
        target_compile_definitions(benchmarkDraw
        PRIVATE
            FONTRENDERER_BENCHMARK_X64V2
        )
    endif()

    if (FONTRENDERER_X64V3_FLAG)
        target_compile_definitions(benchmarkDraw
        PRIVATE
            FONTRENDERER_BENCHMARK_X64V3
        )
    endif()

    if (FONTRENDERER_ALIGN_FLAGS)
        target_compile_definitions(benchmarkDraw
        PRIVATE
            FONTRENDERER_BENCHMARK_ALIGNED
        )
    endif()

    if (DJGPP)
        set_target_properties(benchmarkDraw PROPERTIES OUTPUT_NAME "benchdrw")
    endif()

    # How long Font takes to render the glyphs, which benchmarkDraw leaves out.
    if (FONTRENDERER_HAS_RASTERIZER)
        add_executable(benchmarkRaster
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkRaster.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkSystem.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmarkSystem.h
        )

        fontrenderer_setup_target(benchmarkRaster)

        target_compile_definitions(benchmarkRaster
        PRIVATE
            FONT_RENDERER_BENCHMARK_RESOURCES="${FONTRENDERER_BENCHMARK_RESOURCES}"
        )

        target_link_libraries(benchmarkRaster
        PRIVATE
            fontRenderer
        )

        if (DJGPP)
            set_target_properties(benchmarkRaster PROPERTIES OUTPUT_NAME "benchrst")
        endif()
    endif()
endif()

# Organize Visual Studio Solution Folders
#--------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
