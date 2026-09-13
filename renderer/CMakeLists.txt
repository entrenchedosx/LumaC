# Luma Renderer: scene rendering on top of public LumaC only.
cmake_minimum_required(VERSION 3.15)

# Backend-independence guard (PART AG/C): library headers and sources
# (everything except tests/, which take the same white-box privilege
# as LumaC's own integration tests for readback/idle) must never name
# backend or LumaC-internal symbols. Fails configure loudly if the
# boundary rots. (This CMakeLists itself is excluded: it spells the
# forbidden pattern to enforce it.)
file(GLOB_RECURSE LR_AUDIT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(LR_AUDIT_FILE IN LISTS LR_AUDIT_FILES)
    file(READ "${LR_AUDIT_FILE}" LR_AUDIT_TEXT)
    if(LR_AUDIT_TEXT MATCHES "Vk[A-Z]|vulkan/|windows\\.h|X11/|Xlib\\.h|HWND|graphics_internal|lumac_internal|src/graphics|src/internal")
        message(FATAL_ERROR
            "Backend-independence violation in ${LR_AUDIT_FILE}: Luma "
            "Renderer library sources may use public lumac.h only.")
    endif()
endforeach()

# Tests register from here (the top level enables testing after this
# directory is added, so enable it locally too).
enable_testing()

# Compiled SPIR-V staging: committed binaries are used as-is; when
# glslc exists the GLSL sources are recompiled instead so embedded
# bytes never drift from source.
set(LR_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/luma_renderer_shaders")
file(MAKE_DIRECTORY "${LR_SPV_DIR}")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unlit.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unlit.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fulltri.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/eq2cube.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/irradiance.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/prefilter.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/brdf.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tonemap.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/post_tint.frag.spv"
    DESTINATION "${LR_SPV_DIR}")
find_program(LR_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(LR_GLSLC)
    add_custom_command(
        OUTPUT
            "${LR_SPV_DIR}/unlit.vert.spv"
            "${LR_SPV_DIR}/unlit.frag.spv"
            "${LR_SPV_DIR}/pbr.vert.spv"
            "${LR_SPV_DIR}/pbr.frag.spv"
            "${LR_SPV_DIR}/shadow.vert.spv"
            "${LR_SPV_DIR}/fulltri.vert.spv"
            "${LR_SPV_DIR}/eq2cube.frag.spv"
            "${LR_SPV_DIR}/irradiance.frag.spv"
            "${LR_SPV_DIR}/prefilter.frag.spv"
            "${LR_SPV_DIR}/brdf.frag.spv"
            "${LR_SPV_DIR}/sky.vert.spv"
            "${LR_SPV_DIR}/sky.frag.spv"
            "${LR_SPV_DIR}/tonemap.frag.spv"
            "${LR_SPV_DIR}/post_tint.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=vert -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unlit.vert"
            -o "${LR_SPV_DIR}/unlit.vert.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unlit.frag"
            -o "${LR_SPV_DIR}/unlit.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=vert -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr.vert"
            -o "${LR_SPV_DIR}/pbr.vert.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr.frag"
            -o "${LR_SPV_DIR}/pbr.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=vert -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.vert"
            -o "${LR_SPV_DIR}/shadow.vert.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=vert -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fulltri.vert"
            -o "${LR_SPV_DIR}/fulltri.vert.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/eq2cube.frag"
            -o "${LR_SPV_DIR}/eq2cube.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/irradiance.frag"
            -o "${LR_SPV_DIR}/irradiance.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/prefilter.frag"
            -o "${LR_SPV_DIR}/prefilter.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/brdf.frag"
            -o "${LR_SPV_DIR}/brdf.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=vert -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.vert"
            -o "${LR_SPV_DIR}/sky.vert.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.frag"
            -o "${LR_SPV_DIR}/sky.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tonemap.frag"
            -o "${LR_SPV_DIR}/tonemap.frag.spv"
        COMMAND "${LR_GLSLC}" -fshader-stage=frag -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/post_tint.frag"
            -o "${LR_SPV_DIR}/post_tint.frag.spv"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unlit.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unlit.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fulltri.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/eq2cube.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/irradiance.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/prefilter.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/brdf.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.vert"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tonemap.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/post_tint.frag"
        COMMENT "Compiling Luma Renderer shaders")
    add_custom_target(luma_renderer_shaders DEPENDS
        "${LR_SPV_DIR}/unlit.vert.spv"
        "${LR_SPV_DIR}/unlit.frag.spv"
        "${LR_SPV_DIR}/pbr.vert.spv"
        "${LR_SPV_DIR}/pbr.frag.spv"
        "${LR_SPV_DIR}/shadow.vert.spv"
        "${LR_SPV_DIR}/fulltri.vert.spv"
        "${LR_SPV_DIR}/eq2cube.frag.spv"
        "${LR_SPV_DIR}/irradiance.frag.spv"
        "${LR_SPV_DIR}/prefilter.frag.spv"
        "${LR_SPV_DIR}/brdf.frag.spv"
        "${LR_SPV_DIR}/sky.vert.spv"
        "${LR_SPV_DIR}/sky.frag.spv"
        "${LR_SPV_DIR}/tonemap.frag.spv"
        "${LR_SPV_DIR}/post_tint.frag.spv")
endif()

# Embedded SPIR-V bytes (portable hex dump, no host tools needed).
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_unlit_vert_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/unlit.vert.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_unlit_vert_spv.c"
        "-DSYM=lr_unlit_vert_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/unlit.vert.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_unlit_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/unlit.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_unlit_frag_spv.c"
        "-DSYM=lr_unlit_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/unlit.frag.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_pbr_vert_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/pbr.vert.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_pbr_vert_spv.c"
        "-DSYM=lr_pbr_vert_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/pbr.vert.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_pbr_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/pbr.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_pbr_frag_spv.c"
        "-DSYM=lr_pbr_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/pbr.frag.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_shadow_vert_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/shadow.vert.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_shadow_vert_spv.c"
        "-DSYM=lr_shadow_vert_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/shadow.vert.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_fulltri_vert_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/fulltri.vert.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_fulltri_vert_spv.c"
        "-DSYM=lr_fulltri_vert_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/fulltri.vert.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_eq2cube_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/eq2cube.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_eq2cube_frag_spv.c"
        "-DSYM=lr_eq2cube_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/eq2cube.frag.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_irradiance_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/irradiance.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_irradiance_frag_spv.c"
        "-DSYM=lr_irradiance_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/irradiance.frag.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_prefilter_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/prefilter.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_prefilter_frag_spv.c"
        "-DSYM=lr_prefilter_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/prefilter.frag.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_brdf_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/brdf.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_brdf_frag_spv.c"
        "-DSYM=lr_brdf_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/brdf.frag.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_sky_vert_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/sky.vert.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_sky_vert_spv.c"
        "-DSYM=lr_sky_vert_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/sky.vert.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_sky_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/sky.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_sky_frag_spv.c"
        "-DSYM=lr_sky_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/sky.frag.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_tonemap_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/tonemap.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_tonemap_frag_spv.c"
        "-DSYM=lr_tonemap_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/tonemap.frag.spv")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lr_post_tint_frag_spv.c"
    COMMAND "${CMAKE_COMMAND}"
        "-DIN=${LR_SPV_DIR}/post_tint.frag.spv"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/lr_post_tint_frag_spv.c"
        "-DSYM=lr_post_tint_frag_spv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hex_to_c.cmake"
    DEPENDS "${LR_SPV_DIR}/post_tint.frag.spv")

add_library(luma_renderer STATIC
    src/renderer.c
    src/mesh.c
    src/mesh_primitives.c
    src/material.c
    src/camera.c
    src/draw_list.c
    src/environment.c
    src/post.c
    "${CMAKE_CURRENT_BINARY_DIR}/lr_unlit_vert_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_unlit_frag_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_pbr_vert_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_pbr_frag_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_shadow_vert_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_fulltri_vert_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_eq2cube_frag_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_irradiance_frag_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_prefilter_frag_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_brdf_frag_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_sky_vert_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_sky_frag_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_tonemap_frag_spv.c"
    "${CMAKE_CURRENT_BINARY_DIR}/lr_post_tint_frag_spv.c"
)
if(TARGET luma_renderer_shaders)
    add_dependencies(luma_renderer luma_renderer_shaders)
endif()

target_include_directories(luma_renderer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(luma_renderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Public LumaC dependency only (transitive: renderer headers name
# lc_* types). No backend, platform, or internal includes.
target_link_libraries(luma_renderer PUBLIC lumac)

if(MSVC)
    target_compile_options(luma_renderer PRIVATE /W3)
else()
    target_compile_options(luma_renderer PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(luma_renderer PRIVATE m)
endif()

# Renderer tests (CTest registration flows from the top level).
add_executable(test_renderer tests/test_renderer.c)
target_link_libraries(test_renderer PRIVATE luma_renderer)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_renderer PRIVATE m)
endif()
add_test(NAME test_renderer COMMAND test_renderer)

add_executable(test_renderer_vulkan tests/test_renderer_vulkan.c)
target_link_libraries(test_renderer_vulkan PRIVATE luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_renderer_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(TARGET luma_renderer_shaders)
    add_dependencies(test_renderer_vulkan luma_renderer_shaders)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_renderer_vulkan PRIVATE m)
endif()
add_test(NAME test_renderer_vulkan COMMAND test_renderer_vulkan)

# PBR tests (Phase 15): headless BRDF/math + Vulkan pixel/endurance.
add_executable(test_pbr tests/test_pbr.c)
target_link_libraries(test_pbr PRIVATE luma_renderer)
target_include_directories(test_pbr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_pbr PRIVATE m)
endif()
add_test(NAME test_pbr COMMAND test_pbr)

add_executable(test_pbr_vulkan tests/test_pbr_vulkan.c)
target_link_libraries(test_pbr_vulkan PRIVATE luma_renderer luma_assets lumac Vulkan::Vulkan)
target_include_directories(test_pbr_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_pbr_vulkan PRIVATE
    LA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets/tests/fixtures")
if(TARGET luma_renderer_shaders)
    add_dependencies(test_pbr_vulkan luma_renderer_shaders)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_pbr_vulkan PRIVATE m)
endif()
add_test(NAME test_pbr_vulkan COMMAND test_pbr_vulkan)

# Shadow tests (Phase 16): headless light-space math + Vulkan
# depth/pixel/endurance.
add_executable(test_shadow tests/test_shadow.c)
target_link_libraries(test_shadow PRIVATE luma_renderer)
target_include_directories(test_shadow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_shadow PRIVATE m)
endif()
add_test(NAME test_shadow COMMAND test_shadow)

add_executable(test_shadow_vulkan tests/test_shadow_vulkan.c)
target_link_libraries(test_shadow_vulkan PRIVATE luma_renderer luma_assets lumac Vulkan::Vulkan)
target_include_directories(test_shadow_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_shadow_vulkan PRIVATE
    LA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets/tests/fixtures")
if(TARGET luma_renderer_shaders)
    add_dependencies(test_shadow_vulkan luma_renderer_shaders)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_shadow_vulkan PRIVATE m)
endif()
add_test(NAME test_shadow_vulkan COMMAND test_shadow_vulkan)

# IBL tests (Phase 17): headless CPU references + Vulkan
# environment/HDR/tonemap pixel/endurance.
add_executable(test_ibl tests/test_ibl.c)
target_link_libraries(test_ibl PRIVATE luma_renderer)
target_include_directories(test_ibl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ibl PRIVATE m)
endif()
add_test(NAME test_ibl COMMAND test_ibl)

add_executable(test_ibl_vulkan tests/test_ibl_vulkan.c)
target_link_libraries(test_ibl_vulkan PRIVATE luma_renderer luma_assets lumac Vulkan::Vulkan)
target_include_directories(test_ibl_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_ibl_vulkan PRIVATE
    LA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets/tests/fixtures")
if(TARGET luma_renderer_shaders)
    add_dependencies(test_ibl_vulkan luma_renderer_shaders)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ibl_vulkan PRIVATE m)
endif()
add_test(NAME test_ibl_vulkan COMMAND test_ibl_vulkan)

# Shared Windows builds place lumac.dll beside the top-level tests;
# renderer tests live one directory deeper, so extend their DLL search
# path explicitly (no-op for static builds).
if(WIN32)
    set_tests_properties(test_renderer test_renderer_vulkan test_pbr
        test_pbr_vulkan test_shadow test_shadow_vulkan test_ibl
        test_ibl_vulkan PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:lumac>\;$ENV{PATH}")
endif()
