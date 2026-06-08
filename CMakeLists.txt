cmake_minimum_required(VERSION 3.28)

project(cloud_render LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenVDB CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

find_program(SLANGC_EXECUTABLE NAMES slangc REQUIRED)

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders")
set(SHADER_COMMON
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/phase.slang"
)
set(SHADER_PASSES
    renderVolume "${CMAKE_CURRENT_SOURCE_DIR}/shaders/render_volume.slang"
    temporalDenoise "${CMAKE_CURRENT_SOURCE_DIR}/shaders/temporal_denoise.slang"
    tonemap "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tonemap.slang"
)

while(SHADER_PASSES)
    list(POP_FRONT SHADER_PASSES entry source)
    set(output "${SHADER_OUTPUT_DIR}/${entry}.hlsl")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${SLANGC_EXECUTABLE}"
            -target hlsl
            -profile cs_5_0
            -entry "${entry}"
            -I "${CMAKE_CURRENT_SOURCE_DIR}"
            "${source}"
            -o "${output}"
        DEPENDS "${source}" ${SHADER_COMMON} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/PNanoVDB.h"
        VERBATIM
    )
    list(APPEND COMPILED_SHADERS "${output}")
endwhile()

add_custom_target(cloud_render_shaders ALL DEPENDS ${COMPILED_SHADERS})

add_executable(cloud_render
    src/main.cpp
)

add_dependencies(cloud_render cloud_render_shaders)

target_compile_definitions(cloud_render PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    NANOVDB_USE_OPENVDB
)

target_compile_options(cloud_render PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>"
    "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>"
)

target_link_libraries(cloud_render PRIVATE
    OpenVDB::openvdb
    glfw
    imgui::imgui
    d3d11
    d3dcompiler
    dxgi
)

add_custom_command(TARGET cloud_render POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:cloud_render>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${COMPILED_SHADERS} "$<TARGET_FILE_DIR:cloud_render>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_RUNTIME_DLLS:cloud_render>" "$<TARGET_FILE_DIR:cloud_render>"
    COMMAND_EXPAND_LISTS
)
