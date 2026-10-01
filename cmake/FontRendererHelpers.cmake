function(fontrenderer_setup_target target)
    set_target_properties(${target}
    PROPERTIES
        C_STANDARD          11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS        ${FONTRENDERER_EXTENSIONS}
        CXX_EXTENSIONS      ${FONTRENDERER_EXTENSIONS}
    )

    target_compile_features(${target}
    PRIVATE
        cxx_std_14
    )

    target_compile_definitions(${target}
    PRIVATE
        "$<$<CONFIG:Debug>:DEBUG>"
        "$<$<CONFIG:Debug>:_DEBUG>"
        "$<$<CONFIG:Release>:NDEBUG>"
        "$<$<CONFIG:MinSizeRel>:NDEBUG>"
        "$<$<CONFIG:RelWithDebInfo>:NDEBUG>"
    )

    if (MSVC)
        target_compile_definitions(${target}
        PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )

        target_compile_options(${target}
        PRIVATE
            /W3
            /wd4244
            /wd26451
            /GS
            /Gy
            /fp:fast
            /Zc:__cplusplus
            /utf-8
        )

        if (MSVC_VERSION GREATER_EQUAL 1900)
            target_compile_options(${target}
            PRIVATE
                /sdl)
        endif()
    else()
        target_compile_options(${target}
        PRIVATE
            -Wall -Wextra
            -Wno-switch -Wno-unused-function -Wno-unused-parameter
            -ffast-math
        )
    endif()
endfunction()

function(fontrenderer_add_example target source_dir)
    set(sources
        ${ARGN}
    )

    source_group(
        TREE "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}"
        FILES ${sources}
    )

    add_executable(${target}
        ${sources}
    )

    fontrenderer_setup_target(${target})

    target_link_libraries(${target}
        fontRenderer
        minifb
    )
endfunction()

function(fontrenderer_add_test target source_dir)
    set(sources
        ${ARGN}
    )

    source_group(
        TREE "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}"
        FILES ${sources}
    )

    add_executable(${target}
        ${sources}
    )

    fontrenderer_setup_target(${target})

    target_include_directories(${target}
    PRIVATE
        ${source_dir}
        ${source_dir}/external
    )

    target_compile_definitions(${target}
    PRIVATE
        FONT_RENDERER_TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/bin/resources/"
        FONT_RENDERER_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/"
    )

    target_link_libraries(${target}
        fontRenderer
    )

    add_test(NAME ${target} COMMAND ${target})
endfunction()
