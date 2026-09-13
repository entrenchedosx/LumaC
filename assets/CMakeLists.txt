# Luma Assets: glTF model ingestion on top of public Luma Renderer +
# LumaC only.
cmake_minimum_required(VERSION 3.15)

# Backend-independence guard: library headers and sources (everything
# except tests/, which take the same white-box privilege as LumaC's
# own integration tests) must never name backend symbols, LumaC or
# renderer internals, or glTF concepts outside the importer. Fails
# configure loudly if the boundary rots. (This CMakeLists itself is
# excluded: it spells the forbidden pattern to enforce it.)
file(GLOB_RECURSE LA_AUDIT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(LA_AUDIT_FILE IN LISTS LA_AUDIT_FILES)
    file(READ "${LA_AUDIT_FILE}" LA_AUDIT_TEXT)
    if(LA_AUDIT_TEXT MATCHES "Vk[A-Z]|vulkan/|windows\\.h|X11/|Xlib\\.h|HWND|graphics_internal|lumac_internal|renderer_internal|src/graphics|src/internal|renderer/src")
        message(FATAL_ERROR
            "Backend-independence violation in ${LA_AUDIT_FILE}: Luma "
            "Assets library sources may use public luma_renderer.h and "
            "lumac.h only (plus vendored cgltf/stb_image in importer).")
    endif()
endforeach()

# Tests register from here (the top level enables testing after this
# directory is added, so enable it locally too).
enable_testing()

add_library(luma_assets STATIC
    src/asset_manager.c
    src/model.c
    src/texture.c
    src/hdr_image.c
    src/gltf_import.c
    src/fs.c
    src/cgltf_impl.c
    src/stb_image_impl.c
)

target_include_directories(luma_assets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(luma_assets PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Public renderer (+ transitive LumaC) dependency only. No backend,
# platform, or internal includes.
target_link_libraries(luma_assets PUBLIC luma_renderer)

if(MSVC)
    target_compile_options(luma_assets PRIVATE /W3)
    # Third-party single-TU builds stay warning-quiet.
    set_source_files_properties(src/cgltf_impl.c src/stb_image_impl.c
        PROPERTIES COMPILE_OPTIONS "/W0")
else()
    target_compile_options(luma_assets PRIVATE -Wall -Wextra -Wpedantic)
    set_source_files_properties(src/cgltf_impl.c src/stb_image_impl.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(luma_assets PRIVATE m)
endif()

# Asset tests (CTest registration flows from the top level).
add_executable(test_assets tests/test_assets.c)
target_link_libraries(test_assets PRIVATE luma_assets)
target_include_directories(test_assets PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_assets PRIVATE
    LA_MALFORMED_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/malformed"
    LA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
if(UNIX AND NOT APPLE)
    target_link_libraries(test_assets PRIVATE m)
endif()
add_test(NAME test_assets COMMAND test_assets)

add_executable(test_assets_vulkan tests/test_assets_vulkan.c)
target_link_libraries(test_assets_vulkan PRIVATE luma_assets luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_assets_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_assets_vulkan PRIVATE
    LA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    LA_MALFORMED_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/malformed"
    LA_BOX_PATH="${CMAKE_CURRENT_SOURCE_DIR}/BoxTextured.glb")
if(UNIX AND NOT APPLE)
    target_link_libraries(test_assets_vulkan PRIVATE m)
endif()
add_test(NAME test_assets_vulkan COMMAND test_assets_vulkan)

# Shared Windows builds place lumac.dll beside the top-level tests;
# asset tests live one directory deeper, so extend their DLL search
# path explicitly (no-op for static builds).
if(WIN32)
    set_tests_properties(test_assets test_assets_vulkan PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:lumac>\;$ENV{PATH}")
endif()
