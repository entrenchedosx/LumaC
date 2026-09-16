cmake_minimum_required(VERSION 3.15)

project(LumaC
    VERSION 0.1.0
    DESCRIPTION "A lightweight cross-platform graphics API written in C"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Options
option(LUMAC_BUILD_SHARED "Build LumaC as shared library" OFF)
option(LUMAC_BUILD_EXAMPLES "Build LumaC examples" ON)
option(LUMAC_BUILD_TESTS "Build LumaC tests" ON)
option(LUMA_BUILD_RENDERER "Build the Luma Renderer library (lr_*)" ON)
option(LUMA_BUILD_ASSETS "Build the Luma Assets library (la_*)" ON)

# Assets layer on the renderer layer; disabling the renderer while
# asking for assets is a configuration error, not a silent skip.
if(LUMA_BUILD_ASSETS AND NOT LUMA_BUILD_RENDERER)
    message(FATAL_ERROR "LUMA_BUILD_ASSETS requires LUMA_BUILD_RENDERER")
endif()

# Vulkan graphics backend (device, surface, swapchain, frame presentation).
# The SDK is located normally (VULKAN_SDK env / standard install paths);
# no SDK paths are hard-coded and nothing is vendored into the repo.
find_package(Vulkan REQUIRED)

# Library sources (platform-neutral + graphics + one Vulkan backend).
set(LUMAC_SOURCES
    src/lumac.c
    src/clock.c
    src/window.c
    src/graphics/graphics.c
    src/graphics/surface.c
    src/graphics/swapchain.c
    src/graphics/frame.c
    src/graphics/shader.c
    src/graphics/pipeline.c
    src/graphics/compute_pipeline.c
    src/graphics/buffer.c
    src/graphics/vulkan/vulkan_backend.c
    src/graphics/vulkan/vulkan_surface.c
    src/graphics/vulkan/vulkan_swapchain.c
    src/graphics/vulkan/vulkan_frame.c
    src/graphics/vulkan/vulkan_shader.c
    src/graphics/vulkan/vulkan_pipeline.c
    src/graphics/vulkan/vulkan_format.c
    src/graphics/vulkan/vulkan_buffer.c
    src/graphics/image.c
    src/graphics/sampler.c
    src/graphics/vulkan/vulkan_image.c
    src/graphics/vulkan/vulkan_sampler.c
    src/graphics/image_view.c
    src/graphics/binding.c
    src/graphics/vulkan/vulkan_binding.c
    src/graphics/render_target.c
    src/graphics/readback.c
    src/graphics/resource_id.c
    src/graphics/vulkan/vulkan_sync.c
    src/graphics/vulkan/vulkan_memory.c
    src/graphics/encoder.c
    src/graphics/vulkan/vulkan_render_target.c
    src/graphics/vulkan/vulkan_encoder.c
    src/graphics/vulkan/vulkan_worker.c
    src/graphics/vulkan/vulkan_transfer.c
    src/graphics/vulkan/vulkan_compute.c
)

# Platform backend selection (only the current OS backend is compiled)
if(WIN32)
    list(APPEND LUMAC_SOURCES src/platform/platform_win32.c)
elseif(UNIX AND NOT APPLE)
    list(APPEND LUMAC_SOURCES src/platform/platform_x11.c)
else()
    message(FATAL_ERROR "LumaC supports only Windows (Win32) and Linux (X11)")
endif()

# Library
if(LUMAC_BUILD_SHARED)
    add_library(lumac SHARED ${LUMAC_SOURCES})
    target_compile_definitions(lumac PRIVATE LUMAC_EXPORTS)
    target_compile_definitions(lumac PUBLIC LUMAC_BUILD_SHARED)
else()
    add_library(lumac STATIC ${LUMAC_SOURCES})
endif()

target_include_directories(lumac PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(lumac PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Vulkan is PRIVATE: consumers of lumac.h never need Vulkan headers.
target_link_libraries(lumac PRIVATE Vulkan::Vulkan)

# Platform Vulkan surface declarations, PRIVATE so they never leak to user
# projects. Only the current platform's macro is defined; Win32 and Xlib
# surface APIs are never enabled together. The backend keeps native
# include order (windows.h / Xlib.h before vulkan.h) in
# src/graphics/graphics_internal.h.
if(WIN32)
    target_compile_definitions(lumac PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
    # pthread_mutexattr_settype/PTHREAD_MUTEX_RECURSIVE are POSIX interfaces
    # hidden by glibc under strict -std=c11 unless a POSIX feature level is
    # requested before system headers are included.
    target_compile_definitions(lumac PRIVATE
        VK_USE_PLATFORM_XLIB_KHR
        _POSIX_C_SOURCE=200809L
    )
endif()

# Platform dependencies (no external windowing frameworks)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    if(TARGET X11::X11)
        target_link_libraries(lumac PRIVATE X11::X11)
    else()
        target_include_directories(lumac PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(lumac PRIVATE ${X11_LIBRARIES})
    endif()
    # Allocator mutex (PART AL); pthreads live in libc on modern
    # systems, Threads::Threads covers the rest.
    find_package(Threads REQUIRED)
    target_link_libraries(lumac PRIVATE Threads::Threads)
endif()

# Compiler warnings (not fatal globally)
if(MSVC)
    target_compile_options(lumac PRIVATE /W3)
else()
    target_compile_options(lumac PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Visibility for shared builds on GCC/Clang
if(LUMAC_BUILD_SHARED AND NOT MSVC)
    set_target_properties(lumac PROPERTIES C_VISIBILITY_PRESET hidden)
    # Public API uses LC_API with default visibility
endif()

# Example shaders: committed SPIR-V is always staged into the build
# tree; when glslc exists it recompiles the GLSL sources instead, so
# the binaries never drift from source. Only the examples and their
# tests consume these files; the library never needs a compiler.
set(LC_SPV_DIR "${CMAKE_BINARY_DIR}/lumac_shaders")
file(MAKE_DIRECTORY "${LC_SPV_DIR}")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/vertex_triangle/shaders/vb_triangle.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured_quad/shaders/tex_quad.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured_quad/shaders/tex_quad.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/cube_3d/shaders/cube.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/cube_3d/shaders/cube.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/quad.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/quad.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/checker.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/mrt.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/chain_copy.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/chain_copy.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_double.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_append.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_storage.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/count_test.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/count_test.frag.spv"
    DESTINATION "${LC_SPV_DIR}")
find_program(LC_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(LC_GLSLC)
    add_custom_command(
        OUTPUT
            "${LC_SPV_DIR}/triangle.vert.spv"
            "${LC_SPV_DIR}/triangle.frag.spv"
            "${LC_SPV_DIR}/chain_copy.vert.spv"
            "${LC_SPV_DIR}/chain_copy.frag.spv"
            "${LC_SPV_DIR}/test_double.comp.spv"
            "${LC_SPV_DIR}/test_append.comp.spv"
            "${LC_SPV_DIR}/test_storage.comp.spv"
            "${LC_SPV_DIR}/count_test.vert.spv"
            "${LC_SPV_DIR}/count_test.frag.spv"
            "${LC_SPV_DIR}/vb_triangle.vert.spv"
            "${LC_SPV_DIR}/tex_quad.vert.spv"
            "${LC_SPV_DIR}/tex_quad.frag.spv"
            "${LC_SPV_DIR}/cube.vert.spv"
            "${LC_SPV_DIR}/cube.frag.spv"
            "${LC_SPV_DIR}/quad.vert.spv"
            "${LC_SPV_DIR}/quad.frag.spv"
            "${LC_SPV_DIR}/checker.frag.spv"
            "${LC_SPV_DIR}/mrt.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=vert
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.vert"
            -o "${LC_SPV_DIR}/triangle.vert.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.frag"
            -o "${LC_SPV_DIR}/triangle.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=vert
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/chain_copy.vert"
            -o "${LC_SPV_DIR}/chain_copy.vert.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/chain_copy.frag"
            -o "${LC_SPV_DIR}/chain_copy.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=vert
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/vertex_triangle/shaders/vb_triangle.vert"
            -o "${LC_SPV_DIR}/vb_triangle.vert.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=vert
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured_quad/shaders/tex_quad.vert"
            -o "${LC_SPV_DIR}/tex_quad.vert.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured_quad/shaders/tex_quad.frag"
            -o "${LC_SPV_DIR}/tex_quad.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=vert
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/cube_3d/shaders/cube.vert"
            -o "${LC_SPV_DIR}/cube.vert.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/cube_3d/shaders/cube.frag"
            -o "${LC_SPV_DIR}/cube.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=vert
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/quad.vert"
            -o "${LC_SPV_DIR}/quad.vert.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/quad.frag"
            -o "${LC_SPV_DIR}/quad.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/checker.frag"
            -o "${LC_SPV_DIR}/checker.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/mrt.frag"
            -o "${LC_SPV_DIR}/mrt.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=compute -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_double.comp"
            -o "${LC_SPV_DIR}/test_double.comp.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=compute -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_append.comp"
            -o "${LC_SPV_DIR}/test_append.comp.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=compute -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_storage.comp"
            -o "${LC_SPV_DIR}/test_storage.comp.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=vert
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/count_test.vert"
            -o "${LC_SPV_DIR}/count_test.vert.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/count_test.frag"
            -o "${LC_SPV_DIR}/count_test.frag.spv"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/chain_copy.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/chain_copy.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_double.comp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_append.comp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/test_storage.comp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/count_test.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/count_test.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/vertex_triangle/shaders/vb_triangle.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured_quad/shaders/tex_quad.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured_quad/shaders/tex_quad.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/cube_3d/shaders/cube.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/cube_3d/shaders/cube.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/quad.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/quad.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/checker.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/render_to_texture/shaders/mrt.frag"
        COMMENT "Compiling LumaC example shaders")
    add_custom_target(lumac_triangle_shaders DEPENDS
        "${LC_SPV_DIR}/triangle.vert.spv"
        "${LC_SPV_DIR}/triangle.frag.spv"
        "${LC_SPV_DIR}/chain_copy.vert.spv"
        "${LC_SPV_DIR}/chain_copy.frag.spv"
        "${LC_SPV_DIR}/test_double.comp.spv"
        "${LC_SPV_DIR}/test_append.comp.spv"
        "${LC_SPV_DIR}/test_storage.comp.spv"
        "${LC_SPV_DIR}/count_test.vert.spv"
        "${LC_SPV_DIR}/count_test.frag.spv"
        "${LC_SPV_DIR}/vb_triangle.vert.spv"
        "${LC_SPV_DIR}/tex_quad.vert.spv"
        "${LC_SPV_DIR}/tex_quad.frag.spv"
        "${LC_SPV_DIR}/cube.vert.spv"
        "${LC_SPV_DIR}/cube.frag.spv"
        "${LC_SPV_DIR}/quad.vert.spv"
        "${LC_SPV_DIR}/quad.frag.spv"
        "${LC_SPV_DIR}/checker.frag.spv"
        "${LC_SPV_DIR}/mrt.frag.spv")
endif()

# Examples
if(LUMAC_BUILD_EXAMPLES)
    add_subdirectory(examples/basic_init)
    add_subdirectory(examples/window)
    add_subdirectory(examples/device_info)
    add_subdirectory(examples/surface_info)
    add_subdirectory(examples/swapchain_info)
    add_subdirectory(examples/clear_screen)
    add_subdirectory(examples/triangle)
    add_subdirectory(examples/vertex_triangle)
    add_subdirectory(examples/textured_quad)
    add_subdirectory(examples/texture_upload)
    add_subdirectory(examples/cube_3d)
    add_subdirectory(examples/render_to_texture)
    if(LUMA_BUILD_RENDERER)
        add_subdirectory(examples/renderer_scene)
        add_subdirectory(examples/shadow_scene)
        add_subdirectory(examples/ibl_scene)
        add_subdirectory(examples/gpu_driven_scene)
        add_subdirectory(examples/visibility_scene)
        if(LUMA_BUILD_ENGINE)
            add_subdirectory(examples/engine_scene)
            add_subdirectory(examples/scene_roundtrip)
            add_subdirectory(examples/lua_scene)
            add_subdirectory(examples/lua_input)
        endif()
    endif()
    if(LUMA_BUILD_ASSETS)
        add_subdirectory(examples/model_viewer)
        add_subdirectory(examples/pbr_scene)
    endif()
endif()

# Luma Engine (separate library on public Luma Renderer + LumaC).
option(LUMA_BUILD_ENGINE "Build the Luma Engine library (le_*)" ON)
if(LUMA_BUILD_ENGINE AND NOT LUMA_BUILD_RENDERER)
    message(FATAL_ERROR "LUMA_BUILD_ENGINE requires LUMA_BUILD_RENDERER")
endif()

# Luma Renderer (separate library on public LumaC only).
if(LUMA_BUILD_RENDERER)
    add_subdirectory(renderer)
endif()

# Luma Assets (separate library on public Luma Renderer + LumaC).
if(LUMA_BUILD_ASSETS)
    add_subdirectory(assets)
endif()

# Luma Engine (generational scene objects on public Luma Renderer).
if(LUMA_BUILD_ENGINE)
    add_subdirectory(engine)
endif()

# Tests
if(LUMAC_BUILD_TESTS)
    enable_testing()
    add_executable(test_init tests/test_init.c)
    target_link_libraries(test_init PRIVATE lumac)
    target_include_directories(test_init PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_init COMMAND test_init)

    # Headless-safe: validates argument handling only, never creates OS windows
    add_executable(test_window tests/test_window.c)
    target_link_libraries(test_window PRIVATE lumac)
    target_include_directories(test_window PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_window COMMAND test_window)

    # Headless-safe: validates device argument handling, never touches Vulkan
    add_executable(test_device tests/test_device.c)
    target_link_libraries(test_device PRIVATE lumac)
    target_include_directories(test_device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_device COMMAND test_device)

    # Integration: creates a real Vulkan device; requires a Vulkan
    # runtime + GPU, so it is expected to run where those exist.
    add_executable(test_device_vulkan tests/test_device_vulkan.c)
    target_link_libraries(test_device_vulkan PRIVATE lumac)
    target_include_directories(test_device_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_device_vulkan COMMAND test_device_vulkan)

    # Headless-safe: validates surface argument handling, never touches
    # Vulkan or the window system
    add_executable(test_surface tests/test_surface.c)
    target_link_libraries(test_surface PRIVATE lumac)
    target_include_directories(test_surface PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_surface COMMAND test_surface)

    # Integration: creates real windows, devices, and VkSurfaceKHR objects;
    # needs a display server + Vulkan runtime + GPU, skips otherwise.
    add_executable(test_surface_vulkan tests/test_surface_vulkan.c)
    target_link_libraries(test_surface_vulkan PRIVATE lumac)
    target_include_directories(test_surface_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_surface_vulkan COMMAND test_surface_vulkan)

    # Headless-safe: validates swapchain argument handling, never touches
    # Vulkan or the window system
    add_executable(test_swapchain tests/test_swapchain.c)
    target_link_libraries(test_swapchain PRIVATE lumac)
    target_include_directories(test_swapchain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_swapchain COMMAND test_swapchain)

    # Integration: creates real swapchains + image views and exercises
    # recreate/destroy ordering; needs display + Vulkan + GPU, skips
    # otherwise.
    add_executable(test_swapchain_vulkan tests/test_swapchain_vulkan.c)
    target_link_libraries(test_swapchain_vulkan PRIVATE lumac)
    target_include_directories(test_swapchain_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_swapchain_vulkan COMMAND test_swapchain_vulkan)

    # Headless-safe: validates frame argument handling, never touches
    # Vulkan or the window system
    add_executable(test_frame tests/test_frame.c)
    target_link_libraries(test_frame PRIVATE lumac)
    target_include_directories(test_frame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_frame COMMAND test_frame)

    # Integration: renders real frames (acquire/clear/submit/present),
    # resize stress, minimize/restore, two-window rendering; needs
    # display + Vulkan + GPU, skips otherwise.
    add_executable(test_frame_vulkan tests/test_frame_vulkan.c)
    target_link_libraries(test_frame_vulkan PRIVATE lumac)
    target_include_directories(test_frame_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_frame_vulkan COMMAND test_frame_vulkan)

    # Headless-safe: validates shader argument/SPIR-V handling, never
    # touches Vulkan
    add_executable(test_shader tests/test_shader.c)
    target_link_libraries(test_shader PRIVATE lumac)
    target_include_directories(test_shader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_shader COMMAND test_shader)

    # Headless-safe: validates pipeline argument handling, never touches
    # Vulkan
    add_executable(test_pipeline tests/test_pipeline.c)
    target_link_libraries(test_pipeline PRIVATE lumac)
    target_include_directories(test_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_pipeline COMMAND test_pipeline)

    # Integration: persistent pipeline cache (two-generation
    # load/save/reuse, corrupt/truncated/junk blobs, unwritable
    # path, disabled mode); device-only (no display needed), skips
    # otherwise. Timings reported, never asserted.
    add_executable(test_pipeline_cache tests/test_pipeline_cache.c)
    target_link_libraries(test_pipeline_cache PRIVATE lumac)
    target_include_directories(test_pipeline_cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(test_pipeline_cache PRIVATE LC_CACHE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_pipeline_cache lumac_triangle_shaders)
    endif()
    add_test(NAME test_pipeline_cache COMMAND test_pipeline_cache)

    # Integration: real SPIR-V modules, pipeline creation, triangle
    # rendering (120 frames, resize, minimize, two windows); needs
    # display + Vulkan + GPU, skips otherwise.
    add_executable(test_triangle_vulkan tests/test_triangle_vulkan.c)
    target_link_libraries(test_triangle_vulkan PRIVATE lumac)
    target_include_directories(test_triangle_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(test_triangle_vulkan PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_triangle_vulkan lumac_triangle_shaders)
    endif()
    add_test(NAME test_triangle_vulkan COMMAND test_triangle_vulkan)

    # Headless-safe: validates buffer argument handling, never touches
    # Vulkan or the window system
    add_executable(test_buffer tests/test_buffer.c)
    target_link_libraries(test_buffer PRIVATE lumac)
    target_include_directories(test_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_buffer COMMAND test_buffer)

    # Integration: real allocation/mapping/staging/round-trip plus
    # vertex-buffer rendering; needs display + Vulkan + GPU, skips
    # otherwise. Uses internal headers for the copy path (no public
    # copy API exists by design), so it needs src + Vulkan includes.
    add_executable(test_buffer_vulkan tests/test_buffer_vulkan.c)
    target_link_libraries(test_buffer_vulkan PRIVATE lumac Vulkan::Vulkan)
    target_include_directories(test_buffer_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(test_buffer_vulkan PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_buffer_vulkan lumac_triangle_shaders)
    endif()
    add_test(NAME test_buffer_vulkan COMMAND test_buffer_vulkan)

    # Headless-safe: validates image/sampler argument handling, never
    # touches Vulkan or the window system
    add_executable(test_image tests/test_image.c)
    target_link_libraries(test_image PRIVATE lumac)
    target_include_directories(test_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_image COMMAND test_image)

    add_executable(test_sampler tests/test_sampler.c)
    target_link_libraries(test_sampler PRIVATE lumac)
    target_include_directories(test_sampler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_sampler COMMAND test_sampler)

    # Integration: real image allocation/upload/round-trip, mipmaps,
    # arrays, cube structure, samplers; needs a Vulkan runtime + GPU
    # (no display needed), skips otherwise. Uses internal headers for
    # the copy path (no public copy API exists by design).
    add_executable(test_image_vulkan tests/test_image_vulkan.c)
    target_link_libraries(test_image_vulkan PRIVATE lumac Vulkan::Vulkan)
    target_include_directories(test_image_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test_image_vulkan COMMAND test_image_vulkan)

    # Integration: PUBLIC image readback only (no white-box copy),
    # pattern/float/mip/layer/depth reads, error matrix, ABA
    # identity stress; device-only (no display needed), skips
    # otherwise.
    add_executable(test_readback_vulkan tests/test_readback_vulkan.c)
    target_link_libraries(test_readback_vulkan PRIVATE lumac)
    target_include_directories(test_readback_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_readback_vulkan COMMAND test_readback_vulkan)

    # Integration: semantic states, explicit transitions, chained
    # write->read visibility (~1000 transitions), depth, upload/
    # copy paths, mixed mip states; windowed (display needed),
    # skips otherwise.
    add_executable(test_sync_vulkan tests/test_sync_vulkan.c)
    target_link_libraries(test_sync_vulkan PRIVATE lumac)
    target_include_directories(test_sync_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(test_sync_vulkan PRIVATE LC_SYNC_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_sync_vulkan lumac_triangle_shaders)
    endif()
    add_test(NAME test_sync_vulkan COMMAND test_sync_vulkan)

    # Integration: GPU memory allocator stress (10k buffers, images,
    # fragmentation, dedicated, reclamation, alignment, mapping,
    # OOM rollback, benchmarks, scale workload); device-only (no
    # display needed), skips otherwise.
    add_executable(test_memory tests/test_memory.c)
    target_link_libraries(test_memory PRIVATE lumac)
    target_include_directories(test_memory PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_memory COMMAND test_memory)

    # Phase 20: device-only async uploads/readback, completion values,
    # staging pressure/oversize policy, and caller-data ownership.
    add_executable(test_phase20_transfer_vulkan tests/test_phase20_transfer_vulkan.c)
    target_link_libraries(test_phase20_transfer_vulkan PRIVATE lumac Vulkan::Vulkan)
    target_include_directories(test_phase20_transfer_vulkan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test_phase20_transfer_vulkan COMMAND test_phase20_transfer_vulkan)

    # Phase 21: compute fundamentals (pipelines, dispatch, worker
    # compute lists, atomics, buffer states, isolated queue submit).
    add_executable(test_phase21_compute_vulkan tests/test_phase21_compute_vulkan.c)
    target_link_libraries(test_phase21_compute_vulkan PRIVATE lumac Vulkan::Vulkan)
    target_include_directories(test_phase21_compute_vulkan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(test_phase21_compute_vulkan PRIVATE LC_COMPUTE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_phase21_compute_vulkan lumac_triangle_shaders)
    endif()
    add_test(NAME test_phase21_compute_vulkan COMMAND test_phase21_compute_vulkan)

    # Phase 23: GPU-written indirect draw counts (native where
    # supported, zero-instance fallback otherwise) with pixel proof.
    add_executable(test_indirect_count_vulkan tests/test_indirect_count_vulkan.c)
    target_link_libraries(test_indirect_count_vulkan PRIVATE lumac Vulkan::Vulkan)
    target_include_directories(test_indirect_count_vulkan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(test_indirect_count_vulkan PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_indirect_count_vulkan lumac_triangle_shaders)
    endif()
    add_test(NAME test_indirect_count_vulkan COMMAND test_indirect_count_vulkan)

    # Phase 20: four native threads, independent command pools, 50k
    # real draws, batch execution, three frames-in-flight, pixel proof.
    add_executable(test_phase20_recording_vulkan tests/test_phase20_recording_vulkan.c)
    target_link_libraries(test_phase20_recording_vulkan PRIVATE lumac)
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_phase20_recording_vulkan PRIVATE Threads::Threads)
    endif()
    target_include_directories(test_phase20_recording_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(test_phase20_recording_vulkan PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_phase20_recording_vulkan lumac_triangle_shaders)
    endif()
    add_test(NAME test_phase20_recording_vulkan COMMAND test_phase20_recording_vulkan)

    # Headless-safe: validates image-view argument handling, never
    # touches Vulkan or the window system
    add_executable(test_image_view tests/test_image_view.c)
    target_link_libraries(test_image_view PRIVATE lumac)
    target_include_directories(test_image_view PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_image_view COMMAND test_image_view)

    # Headless-safe: validates binding argument handling, never touches
    # Vulkan or the window system
    add_executable(test_binding tests/test_binding.c)
    target_link_libraries(test_binding PRIVATE lumac)
    target_include_directories(test_binding PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_binding COMMAND test_binding)

    # Headless-safe: pins the nanosecond clock contract (monotonicity,
    # frequency identity, sleep-delta sanity), no GPU involved
    add_executable(test_clock tests/test_clock.c)
    target_link_libraries(test_clock PRIVATE lumac)
    target_include_directories(test_clock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(UNIX AND NOT APPLE)
        # nanosleep is POSIX-gated under strict -std=c11 (same rule
        # as the lumac POSIX feature level above).
        target_compile_definitions(test_clock PRIVATE
            _POSIX_C_SOURCE=200809L)
    endif()
    add_test(NAME test_clock COMMAND test_clock)

    # Integration: real views, layouts, sets, updates, allocator stress,
    # textured rendering (120 frames, resize, two windows, cube bind);
    # needs display + Vulkan + GPU, skips otherwise. White-box for the
    # copy path, like the buffer test.
    add_executable(test_textured_vulkan tests/test_textured_vulkan.c)
    target_link_libraries(test_textured_vulkan PRIVATE lumac Vulkan::Vulkan)
    target_include_directories(test_textured_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(test_textured_vulkan PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_textured_vulkan lumac_triangle_shaders)
    endif()
    add_test(NAME test_textured_vulkan COMMAND test_textured_vulkan)

    # Headless-safe: validates Phase 11 index/instancing/push/depth
    # argument handling, never touches Vulkan or the window system
    add_executable(test_3d tests/test_3d.c)
    target_link_libraries(test_3d PRIVATE lumac)
    target_include_directories(test_3d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_3d COMMAND test_3d)

    # Headless-safe: validates Phase 12 render-target/encoder/pass
    # argument handling, never touches Vulkan or the window system
    add_executable(test_render_target tests/test_render_target.c)
    target_link_libraries(test_render_target PRIVATE lumac)
    target_include_directories(test_render_target PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_render_target COMMAND test_render_target)

    # Integration: real indexed/instanced cube rendering with depth,
    # push constants, raster state, and canonical signatures (validation
    # checks, signature-compatible binds, 60 frames, resize stress);
    # needs display + Vulkan + GPU, skips otherwise.
    add_executable(test_cube_vulkan tests/test_cube_vulkan.c)
    target_link_libraries(test_cube_vulkan PRIVATE lumac Vulkan::Vulkan)
    target_include_directories(test_cube_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(test_cube_vulkan PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_cube_vulkan lumac_triangle_shaders)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_cube_vulkan PRIVATE m)
    endif()
    add_test(NAME test_cube_vulkan COMMAND test_cube_vulkan)

    # Integration: real offscreen targets, MRT pixels, two-pass
    # render-to-texture sampling, sharing/incompatibility, resize
    # independence, multi-window, 120-frame cube run; needs display +
    # Vulkan + GPU, skips otherwise. White-box for readback.
    add_executable(test_rendertarget_vulkan tests/test_rendertarget_vulkan.c)
    target_link_libraries(test_rendertarget_vulkan PRIVATE lumac Vulkan::Vulkan)
    target_include_directories(test_rendertarget_vulkan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(test_rendertarget_vulkan PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
    if(TARGET lumac_triangle_shaders)
        add_dependencies(test_rendertarget_vulkan lumac_triangle_shaders)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_rendertarget_vulkan PRIVATE m)
    endif()
    add_test(NAME test_rendertarget_vulkan COMMAND test_rendertarget_vulkan)
endif()
