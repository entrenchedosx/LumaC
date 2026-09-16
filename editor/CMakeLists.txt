# Luma Editor: editor tooling over public Luma Engine + Renderer.
cmake_minimum_required(VERSION 3.15)

# Backend-independence guard (same rule as the engine): editor
# library headers and sources (everything except tests/, which take
# the same white-box privilege as the engine's own tests) must never
# name backend symbols, engine/renderer internals, or Lua spellings
# (the editor talks to scripts via le_script_* C API only). Fails
# configure loudly if the boundary rots. (This CMakeLists itself is
# excluded: it spells the forbidden pattern to enforce it.)
file(GLOB_RECURSE LED_AUDIT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(LED_AUDIT_FILE IN LISTS LED_AUDIT_FILES)
    file(READ "${LED_AUDIT_FILE}" LED_AUDIT_TEXT)
    if(LED_AUDIT_TEXT MATCHES "Vk[A-Z]|vulkan/|windows\\.h|X11/|Xlib\\.h|HWND|graphics_internal|lumac_internal|renderer_internal|engine_internal|src/graphics|src/internal|renderer/src|engine/src")
        message(FATAL_ERROR
            "Backend-independence violation in ${LED_AUDIT_FILE}: Luma "
            "Editor library sources may use public luma_editor.h, "
            "luma_engine.h, luma_renderer.h and lumac.h only.")
    endif()
    # Lua-confinement: no Lua API SPELLING outside engine/src/script/.
    if(LED_AUDIT_TEXT MATCHES "lua\\.h|lauxlib\\.h|lualib\\.h|lua_State|luaL_[a-z]|LUA_[A-Z]")
        message(FATAL_ERROR
            "Lua-confinement violation in ${LED_AUDIT_FILE}: Lua "
            "may appear only in engine/src/script/; the editor uses "
            "the le_script_* C API.")
    endif()
endforeach()

# Tests register from here (the top level enables testing after this
# directory is added, so enable it locally too).
enable_testing()

add_library(luma_editor STATIC
    src/editor_core.c
    src/editor_selection.c
    src/editor_reflect.c
    src/editor_hierarchy.c
    src/editor_inspector.c
    src/editor_commands.c
    src/editor_scene.c
    src/editor_play.c
    src/editor_viewport.c
    src/editor_gizmo.c
    src/editor_console.c
    src/editor_shortcuts.c
    src/editor_serialize.c
    src/project/project_core.c
    src/project/asset_db.c
    src/project/project_scan.c
    src/project/import_impl.c
    src/project/reimport.c
    src/project/browser.c
    src/project/project_files.c
    src/project/prefab_core.c
)

target_include_directories(luma_editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(luma_editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Downward layering only: editor -> engine (transitive assets,
# renderer, LumaC). The engine never links the editor.
target_link_libraries(luma_editor PUBLIC luma_engine)
if(UNIX AND NOT APPLE)
    target_link_libraries(luma_editor PRIVATE m)
endif()

if(MSVC)
    target_compile_options(luma_editor PRIVATE /W3)
else()
    target_compile_options(luma_editor PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Editor tests (CTest registration flows from the top level).
add_executable(test_editor_core tests/test_editor_core.c)
target_link_libraries(test_editor_core PRIVATE luma_editor)
target_include_directories(test_editor_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_core PRIVATE m)
endif()
add_test(NAME test_editor_core COMMAND test_editor_core)

add_executable(test_editor_history tests/test_editor_history.c)
target_link_libraries(test_editor_history PRIVATE luma_editor)
target_include_directories(test_editor_history PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_history PRIVATE m)
endif()
add_test(NAME test_editor_history COMMAND test_editor_history)

add_executable(test_editor_play tests/test_editor_play.c)
target_link_libraries(test_editor_play PRIVATE luma_editor)
target_include_directories(test_editor_play PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_play PRIVATE m)
endif()
add_test(NAME test_editor_play COMMAND test_editor_play)

add_executable(test_editor_viewport tests/test_editor_viewport.c)
target_link_libraries(test_editor_viewport PRIVATE luma_editor)
target_include_directories(test_editor_viewport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_viewport PRIVATE m)
endif()
add_test(NAME test_editor_viewport COMMAND test_editor_viewport)

add_executable(test_editor_reflect tests/test_editor_reflect.c)
target_link_libraries(test_editor_reflect PRIVATE luma_editor)
target_include_directories(test_editor_reflect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_reflect PRIVATE m)
endif()
add_test(NAME test_editor_reflect COMMAND test_editor_reflect)

# Phase 32 prefab smoke (headless, TEMP-backed project dir).
add_executable(test_prefab_smoke tests/test_prefab_smoke.c)
target_link_libraries(test_prefab_smoke PRIVATE luma_editor)
target_include_directories(test_prefab_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_prefab_smoke PRIVATE m)
endif()
add_test(NAME test_prefab_smoke COMMAND test_prefab_smoke)

# Phase 32 project system (manifest/paths/scan/identity/portability).
add_executable(test_project tests/test_project.c)
target_link_libraries(test_project PRIVATE luma_editor)
target_include_directories(test_project PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_project PRIVATE m)
endif()
add_test(NAME test_project COMMAND test_project)

# Phase 32 import/reimport/browser/delete.
add_executable(test_import tests/test_import.c)
target_link_libraries(test_import PRIVATE luma_editor)
target_include_directories(test_import PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_import PRIVATE m)
endif()
add_test(NAME test_import COMMAND test_import)

# Shared Windows builds place lumac.dll beside the top-level tests;
# editor tests live deeper, so extend their DLL search path
# explicitly (no-op for static builds).
if(WIN32)
    set_tests_properties(test_editor_core test_editor_history test_editor_play test_editor_viewport test_editor_reflect test_prefab_smoke test_project test_import PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:lumac>\;$ENV{PATH}")
endif()
