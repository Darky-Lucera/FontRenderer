cmake_minimum_required(VERSION 3.10)
project(FontRenderer VERSION 0.1.0)

#dependencies
#--------------------------------------

# MiniFB
option(MINIFB_BUILD_EXAMPLES OFF)
add_subdirectory("dependencies/minifb" EXCLUDE_FROM_ALL)

# C++ version
#--------------------------------------
set(CMAKE_CXX_STANDARD 03)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


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
else()
    add_compile_options("$<$<CONFIG:Debug>:-g>")
    add_compile_options("$<IF:$<CONFIG:Debug>,-O0,-O2>")
    add_compile_options(-Wall -Wextra)
    add_compile_options(-Wno-switch -Wno-unused-function -Wno-unused-parameter)
    add_compile_options(-ffast-math)
endif()

set(SRC_FontRenderer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/Font.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/Font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/FontSFT.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/FontSFT.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/FontSTB.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/FontSTB.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/SkylineBinPack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/SkylineBinPack.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MindShake/UTF8_Utils.h
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SRC_FontRenderer})

add_executable(exampleRender
    ${CMAKE_CURRENT_SOURCE_DIR}/src/exampleRender.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libschrift/schrift.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libschrift/schrift.h
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/stb/stb_truetype.h
    ${SRC_FontRenderer}
)
target_include_directories(exampleRender PRIVATE src)
target_include_directories(exampleRender PRIVATE src/MindShake)
target_include_directories(exampleRender PRIVATE dependencies)
target_link_libraries(exampleRender minifb)

# Organize Visual Studio Solution Folders
#--------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
