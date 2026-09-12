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

# Vulkan graphics backend (device, surface, swapchain, frame presentation).
# The SDK is located normally (VULKAN_SDK env / standard install paths);
# no SDK paths are hard-coded and nothing is vendored into the repo.
find_package(Vulkan REQUIRED)

# Library sources (platform-neutral + graphics + one Vulkan backend).
set(LUMAC_SOURCES
    src/lumac.c
    src/window.c
    src/graphics/graphics.c
    src/graphics/surface.c
    src/graphics/swapchain.c
    src/graphics/frame.c
    src/graphics/shader.c
    src/graphics/pipeline.c
    src/graphics/vulkan/vulkan_backend.c
    src/graphics/vulkan/vulkan_surface.c
    src/graphics/vulkan/vulkan_swapchain.c
    src/graphics/vulkan/vulkan_frame.c
    src/graphics/vulkan/vulkan_shader.c
    src/graphics/vulkan/vulkan_pipeline.c
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
    target_compile_definitions(lumac PRIVATE VK_USE_PLATFORM_XLIB_KHR)
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

# Triangle shaders: committed SPIR-V is always staged into the build
# tree; when glslc exists it recompiles the GLSL sources instead, so
# the binaries never drift from source. Only the triangle example and
# its test consume these files; the library never needs a compiler.
set(LC_SPV_DIR "${CMAKE_BINARY_DIR}/lumac_shaders")
file(MAKE_DIRECTORY "${LC_SPV_DIR}")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.frag.spv"
    DESTINATION "${LC_SPV_DIR}")
find_program(LC_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(LC_GLSLC)
    add_custom_command(
        OUTPUT
            "${LC_SPV_DIR}/triangle.vert.spv"
            "${LC_SPV_DIR}/triangle.frag.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=vert
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.vert"
            -o "${LC_SPV_DIR}/triangle.vert.spv"
        COMMAND "${LC_GLSLC}" -fshader-stage=frag
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.frag"
            -o "${LC_SPV_DIR}/triangle.frag.spv"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle/shaders/triangle.frag"
        COMMENT "Compiling LumaC triangle shaders")
    add_custom_target(lumac_triangle_shaders DEPENDS
        "${LC_SPV_DIR}/triangle.vert.spv"
        "${LC_SPV_DIR}/triangle.frag.spv")
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
endif()
