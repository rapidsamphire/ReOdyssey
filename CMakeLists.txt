
# reodyssey - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(reodyssey LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Native renderer backend (Plume RHI: D3D12/Vulkan).
add_subdirectory(thirdparty/plume)

# zstd (static only) for decompressing the XenosRecomp shader cache.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/zstd/build/cmake zstd_build)

set(REODYSSEY_XENOSRECOMP_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp"
    CACHE PATH "Path to the XenosRecomp checkout used for shader tooling")

find_program(REODYSSEY_DXC_EXECUTABLE
    NAMES dxc dxc.exe dxc-linux dxc-macos
    HINTS
        "$ENV{DXC_DIR}/bin"
        "${REODYSSEY_XENOSRECOMP_DIR}/thirdparty/dxc-bin/bin/x64"
        "${REODYSSEY_XENOSRECOMP_DIR}/thirdparty/dxc-bin/bin/arm64"
)

if(NOT REODYSSEY_DXC_EXECUTABLE)
    message(FATAL_ERROR
        "DXC is required to build ReOdyssey's embedded blit shaders. "
        "Install dxc or configure with -DREODYSSEY_DXC_EXECUTABLE=<path-to-dxc>.")
endif()

set(REODYSSEY_GENERATED_SHADER_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
set(REODYSSEY_GENERATED_SHADER_BINARY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")

function(reodyssey_compile_dxil_shader OUT_VAR SOURCE_FILE PROFILE ARRAY_NAME)
    get_filename_component(shader_name "${SOURCE_FILE}" NAME)
    set(dxil_output
        "${REODYSSEY_GENERATED_SHADER_BINARY_DIR}/${shader_name}.dxil")
    set(header_output
        "${REODYSSEY_GENERATED_SHADER_INCLUDE_DIR}/render/shaders/${shader_name}.dxil.h")

    add_custom_command(
        OUTPUT "${dxil_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${REODYSSEY_GENERATED_SHADER_BINARY_DIR}"
        COMMAND "${REODYSSEY_DXC_EXECUTABLE}"
            -E main
            -T "${PROFILE}"
            -Fo "${dxil_output}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}"
        COMMENT "DXC ${SOURCE_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${header_output}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT_FILE=${dxil_output}"
            "-DOUTPUT_FILE=${header_output}"
            "-DARRAY_NAME=${ARRAY_NAME}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinaryHeader.cmake"
        DEPENDS
            "${dxil_output}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinaryHeader.cmake"
        COMMENT "Embedding ${shader_name}.dxil"
        VERBATIM
    )

    set_source_files_properties("${header_output}" PROPERTIES GENERATED TRUE)
    set(${OUT_VAR} ${${OUT_VAR}} "${header_output}" PARENT_SCOPE)
endfunction()

set(REODYSSEY_BLIT_SHADER_HEADERS)
reodyssey_compile_dxil_shader(REODYSSEY_BLIT_SHADER_HEADERS
    src/render/shaders/copy_vs.hlsl vs_6_0 g_copy_vs_dxil)
reodyssey_compile_dxil_shader(REODYSSEY_BLIT_SHADER_HEADERS
    src/render/shaders/copy_ps.hlsl ps_6_0 g_copy_ps_dxil)

add_custom_target(reodyssey_blit_shaders
    DEPENDS ${REODYSSEY_BLIT_SHADER_HEADERS})

# Sources
set(REODYSSEY_SOURCES
    src/main.cpp
    src/input/mouse_support.cpp
    src/render/render_patches.cpp
    src/input/reodyssey_mnk_input.cpp
    src/misc/math.cpp
    # Native renderer (hooks guest D3DDevice_* -> Plume).
    src/render/video.cpp
    src/render/d3d_hooks.cpp
    src/render/d3d_resource_hooks.cpp
    src/render/pipeline.cpp
    src/render/render_state.cpp
    # XenosRecomp-translated shader cache (DXIL/SPIRV blobs + entry table).
    generated/shader_cache.cpp
)

if(WIN32)
    add_executable(reodyssey WIN32 ${REODYSSEY_SOURCES})
else()
    add_executable(reodyssey ${REODYSSEY_SOURCES})
endif()

rexglue_setup_target(reodyssey)
add_dependencies(reodyssey reodyssey_blit_shaders)
target_sources(reodyssey PRIVATE ${REODYSSEY_BLIT_SHADER_HEADERS})
target_include_directories(reodyssey BEFORE PRIVATE
    "${REODYSSEY_GENERATED_SHADER_INCLUDE_DIR}")

# Link the native renderer backend. Plume exposes its headers as PUBLIC, so the
# include path for <plume_render_interface.h> propagates to reodyssey sources.
target_link_libraries(reodyssey PRIVATE plume)
if(WIN32)
    target_link_libraries(reodyssey PRIVATE d3d12 dxgi)
    target_compile_definitions(reodyssey PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(reodyssey PRIVATE "${REXSDK_DIR}/thirdparty/dxc/include")
    add_custom_command(TARGET reodyssey POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${REODYSSEY_XENOSRECOMP_DIR}/thirdparty/dxc-bin/bin/x64/dxcompiler.dll"
            "${REODYSSEY_XENOSRECOMP_DIR}/thirdparty/dxc-bin/bin/x64/dxil.dll"
            $<TARGET_FILE_DIR:reodyssey>
        COMMAND_EXPAND_LISTS
    )
endif()

# rex/hash.h includes <xxhash.h> (header-only via XXH_INLINE_ALL) but the SDK
# does not propagate the xxHash include dir to consumers.
target_include_directories(reodyssey PRIVATE "${REXSDK_DIR}/thirdparty/xxHash")

# zstd: decompress the shader cache.
target_link_libraries(reodyssey PRIVATE libzstd_static)
target_include_directories(reodyssey PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/lib")
