# Luma Editor: editor tooling over public Luma Engine + Renderer.
cmake_minimum_required(VERSION 3.15)

# Backend-independence guard (same rule as the engine): editor
# library headers and sources (everything except tests/, which take
# the same white-box privilege as the engine's own tests, and
# src/gui/, which is the isolated C++ ImGui layer audited by the
# GUI-confinement guard below) must never name backend symbols,
# engine/renderer internals, or Lua spellings (the editor talks to
# scripts via le_script_* C API only). Fails configure loudly if the
# boundary rots. (This CMakeLists itself is excluded: it spells the
# forbidden pattern to enforce it.)
file(GLOB_RECURSE LED_AUDIT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(LED_AUDIT_FILE IN LISTS LED_AUDIT_FILES)
    # The isolated GUI layer (src/gui/*) is exempt here: it is
    # audited by the GUI-confinement guard below instead.
    if(LED_AUDIT_FILE MATCHES "/src/gui/")
        continue()
    endif()
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
    # GUI-confinement: imgui spellings never leak outside src/gui/.
    if(LED_AUDIT_TEXT MATCHES "imgui\\.h|imgui_internal\\.h|ImGui[A-Z]|ImDraw|ImVec2|ImVec4|ImTextureID")
        message(FATAL_ERROR
            "GUI-confinement violation in ${LED_AUDIT_FILE}: Dear "
            "ImGui may appear only in editor/src/gui/; editor core "
            "stays GUI-framework-free.")
    endif()
endforeach()

# GUI-confinement guard for the isolated C++ layer: editor/src/gui/*
# is the ONLY place that may name ImGui. It must still never name
# backend symbols (Vk*, windows.h, X11), engine/renderer internals,
# or Lua spellings: it renders through PUBLIC LumaC (lc_*) only and
# feeds input from lc_window_event only. Fails configure loudly.
file(GLOB_RECURSE LED_GUI_AUDIT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/*")
foreach(LED_GUI_AUDIT_FILE IN LISTS LED_GUI_AUDIT_FILES)
    file(READ "${LED_GUI_AUDIT_FILE}" LED_GUI_AUDIT_TEXT)
    if(LED_GUI_AUDIT_TEXT MATCHES "Vk[A-Z]|vulkan/|windows\\.h|X11/|Xlib\\.h|HWND|graphics_internal|lumac_internal|renderer_internal|engine_internal|src/graphics|src/internal|renderer/src|engine/src")
        message(FATAL_ERROR
            "Backend-independence violation in ${LED_GUI_AUDIT_FILE}: "
            "the GUI layer renders through public LumaC (lc_*) only.")
    endif()
    if(LED_GUI_AUDIT_TEXT MATCHES "lua\\.h|lauxlib\\.h|lualib\\.h|lua_State|luaL_[a-z]|LUA_[A-Z]")
        message(FATAL_ERROR
            "Lua-confinement violation in ${LED_GUI_AUDIT_FILE}: the "
            "GUI layer never touches Lua spellings.")
    endif()
    if(LED_GUI_AUDIT_TEXT MATCHES "imgui_impl_vulkan|imgui_impl_dx|imgui_impl_opengl|imgui_impl_metal|imgui_impl_sdl|imgui_impl_glfw|imgui_impl_win32|imgui_impl_x11|#include.*imgui_impl_")
        message(FATAL_ERROR
            "GUI-confinement violation in ${LED_GUI_AUDIT_FILE}: no "
            "upstream GUI backend is used; the GUI layer owns its "
            "LumaC bridge (input + draw walk).")
    endif()
endforeach()

# Tests register from here (the top level enables testing after this
# directory is added, so enable it locally too).
enable_testing()

# Phase 33: C++ is required for the isolated GUI layer only (Dear
# ImGui is C++-only). Every other Luma target stays C11.
include(CheckCXXCompilerFlag)

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

# Phase 33: isolated GUI layer (C++ over public lc_* + led_* C ABIs).
# Dear ImGui sources are vendored unmodified under third_party/imgui;
# only the docking-branch tag is kept (see third_party/imgui/README).
set(LUMA_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui/imgui-1.92.9b-docking")
if(NOT EXISTS "${LUMA_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Missing vendored Dear ImGui at ${LUMA_IMGUI_DIR} "
        "(expected v1.92.9b-docking; see third_party/imgui/README.md).")
endif()

add_library(luma_editor_gui STATIC
    src/gui/gui_context.cpp
    src/gui/gui_input.cpp
    src/gui/gui_draw.cpp
    src/gui/gui_font.cpp
    src/gui/gui_viewport_tex.cpp
    src/gui/gui_panels.cpp
    src/gui/gui_inspector_widgets.cpp
    src/gui/gui_gizmo_overlay.cpp
    src/gui/gui_loop.cpp
    src/gui/gui_theme.cpp
    src/gui/gui_layout.cpp
    src/gui/gui_icons.cpp
    src/gui/gui_probe.cpp
    "${LUMA_IMGUI_DIR}/imgui.cpp"
    "${LUMA_IMGUI_DIR}/imgui_draw.cpp"
    "${LUMA_IMGUI_DIR}/imgui_tables.cpp"
    "${LUMA_IMGUI_DIR}/imgui_widgets.cpp"
    # NOTE: imgui_demo.cpp is intentionally NOT compiled (never ships).
)

# C++11 is enough for the vendored ImGui core (it uses nullptr but no
# newer library features); keep the floor low on purpose.
set_target_properties(luma_editor_gui PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(luma_editor_gui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# Vendored ImGui headers are PRIVATE: no consumer ever includes
# imgui.h through us (GUI confinement — enforced at configure).
target_include_directories(luma_editor_gui PRIVATE
    "${LUMA_IMGUI_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gui"
)
# Downward layering only: GUI -> editor core -> engine. ImGui never
# leaks through PUBLIC includes or link interfaces.
target_link_libraries(luma_editor_gui PUBLIC luma_editor)
if(UNIX AND NOT APPLE)
    target_link_libraries(luma_editor_gui PRIVATE m)
endif()

if(MSVC)
    # ImGui core warns under /W3 (conversion/4244); keep OUR files at
    # /W3 but drop vendored sources to /W1 so warnings stay ours.
    target_compile_options(luma_editor_gui PRIVATE /W3)
    set_source_files_properties(
        "${LUMA_IMGUI_DIR}/imgui.cpp"
        "${LUMA_IMGUI_DIR}/imgui_draw.cpp"
        "${LUMA_IMGUI_DIR}/imgui_tables.cpp"
        "${LUMA_IMGUI_DIR}/imgui_widgets.cpp"
        PROPERTIES COMPILE_OPTIONS "/W1")
else()
    target_compile_options(luma_editor_gui PRIVATE -Wall -Wextra)
    set_source_files_properties(
        "${LUMA_IMGUI_DIR}/imgui.cpp"
        "${LUMA_IMGUI_DIR}/imgui_draw.cpp"
        "${LUMA_IMGUI_DIR}/imgui_tables.cpp"
        "${LUMA_IMGUI_DIR}/imgui_widgets.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

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

# Phase 32 asset-database (determinism/conflicts/isolation).
add_executable(test_assetdb tests/test_assetdb.c)
target_link_libraries(test_assetdb PRIVATE luma_editor)
target_include_directories(test_assetdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_assetdb PRIVATE m)
endif()
add_test(NAME test_assetdb COMMAND test_assetdb)

# Phase 32 project<->editor integration (drops/assign/play/E2E).
add_executable(test_project_editor tests/test_project_editor.c)
target_link_libraries(test_project_editor PRIVATE luma_editor)
target_include_directories(test_project_editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_project_editor PRIVATE m)
endif()
add_test(NAME test_project_editor COMMAND test_project_editor)

# Pass 2B line-ending/checkout regression (headless): canonical
# text assets must not flip STALE on CRLF checkout (R-010 policy:
# LF writers + canonical fingerprints + CR-tolerant readers).
add_executable(test_lineend tests/test_lineend.c)
target_link_libraries(test_lineend PRIVATE luma_editor)
target_include_directories(test_lineend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_lineend PRIVATE m)
endif()
add_test(NAME test_lineend COMMAND test_lineend)

# Phase 32 glTF identity (Vulkan-gated; SKIP without a device).
add_executable(test_gltf_identity tests/test_gltf_identity.c)
target_link_libraries(test_gltf_identity PRIVATE luma_editor luma_assets luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_gltf_identity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(test_gltf_identity PRIVATE
    LE_BOX_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/BoxTextured.glb")
if(UNIX AND NOT APPLE)
    target_link_libraries(test_gltf_identity PRIVATE m)
endif()
add_test(NAME test_gltf_identity COMMAND test_gltf_identity)

# Phase 32 prefab depth (determinism/isolation/malformed/stress).
add_executable(test_prefab tests/test_prefab.c)
target_link_libraries(test_prefab PRIVATE luma_editor)
target_include_directories(test_prefab PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_prefab PRIVATE m)
endif()
add_test(NAME test_prefab COMMAND test_prefab)

# Phase 33 GUI confinement (headless, no window/GPU/renderer):
# imgui.h must not leak outside src/gui/; input mapping + draw-walk
# budgets + scissor clamp math run without a device.
add_executable(test_editor_gui tests/test_editor_gui.c)
target_link_libraries(test_editor_gui PRIVATE luma_editor_gui)
target_include_directories(test_editor_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_gui PRIVATE m)
endif()
add_test(NAME test_editor_gui COMMAND test_editor_gui)

# Phase 33 GUI-over-GPU proofs (Vulkan-gated; SKIP without a device):
# blended GUI draws, scissor clipping/restore, gizmo funnel,
# play/undo/redo through the GUI command funnel.
add_executable(test_editor_gui_gpu tests/test_editor_gui_gpu.c)
target_link_libraries(test_editor_gui_gpu PRIVATE luma_editor_gui luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_editor_gui_gpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_gui_gpu PRIVATE m)
endif()
add_test(NAME test_editor_gui_gpu COMMAND test_editor_gui_gpu)

# Phase 33 desktop app (real windowed application): window + device +
# swapchain + renderer + engine + world + session + GUI context.
# C11 over public lc_*/lr_*/le_*/led_*/leg_* ABIs only (never imgui.h
# — confinement audit exempts NOTHING outside src/gui, so app/ is
# covered by the same backend/Lua/GUI guards via the top audit only
# if placed under src/...; app/main.c lives beside tests/ instead and
# links luma_editor_gui for the C ABI).
add_executable(luma_editor_app app/main.c)
target_link_libraries(luma_editor_app PRIVATE luma_editor_gui luma_engine luma_renderer lumac)
target_include_directories(luma_editor_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools)
if(UNIX AND NOT APPLE)
    target_link_libraries(luma_editor_app PRIVATE m)
endif()

# Phase 33V negative tests (headless): failure paths must fail
# cleanly (bad project/scene paths, reparent cycles, stale handles,
# missing prefabs, play/save guards, empty drop payloads).
add_executable(test_editor_negative tests/test_editor_negative.c)
target_link_libraries(test_editor_negative PRIVATE luma_editor)
target_include_directories(test_editor_negative PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_negative PRIVATE m)
endif()
add_test(NAME test_editor_negative COMMAND test_editor_negative)

# Phase 34A headed interaction proofs (Vulkan-gated; SKIP without
# a device): real lc_window_inject_event input through the
# production queue + leg_probe_* observation + led/le asserts.
add_executable(test_editor_headed tests/test_editor_headed.c)
target_link_libraries(test_editor_headed PRIVATE luma_editor_gui luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_editor_headed PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_editor_headed PRIVATE m)
endif()
add_test(NAME test_editor_headed COMMAND test_editor_headed --shotdir "${CMAKE_CURRENT_BINARY_DIR}/shots34a")

# Pass 2B live prefab proof (Vulkan-gated; SKIP without a device):
# real prefab workflow with VISIBLE geometry through production
# paths (drop -> create -> instantiate x2 -> move/delete -> undo/
# redo -> save -> play/stop -> corrupt-load), composited + censused
# + shot. Shots land in build/shots2b (evidence bank copies the
# keepers into docs/verification/recovery/pass2b).
add_executable(test_prefab_live tests/test_prefab_live.c)
target_link_libraries(test_prefab_live PRIVATE luma_editor_gui luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_prefab_live PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_prefab_live PRIVATE m)
endif()
add_test(NAME test_prefab_live COMMAND test_prefab_live --shotdir "${CMAKE_CURRENT_BINARY_DIR}/shots2b" --project "${CMAKE_CURRENT_SOURCE_DIR}/../LumaRealityTest")

# Pass 2B live character-course proof (Vulkan-gated; SKIP without a
# device): real controller + real injected keys + real viewport
# composite over an obstacle course (wall/slide/step/stairs/jump/
# ceiling/trigger), with temporal before/mid/after screenshots.
add_executable(test_character_course tests/test_character_course.c)
target_link_libraries(test_character_course PRIVATE luma_editor_gui luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_character_course PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_character_course PRIVATE m)
endif()
add_test(NAME test_character_course COMMAND test_character_course --shotdir "${CMAKE_CURRENT_BINARY_DIR}/shots2b")

# Pass 2B live animation proof (Vulkan-gated; SKIP without a device):
# visible skinned arm (bind/idle/walk), crossfade both directions
# with no-collapse acceptance, loop-boundary continuity, ONCE
# semantics. Shots land in build/shots2b.
add_executable(test_animation_live tests/test_animation_live.c)
target_link_libraries(test_animation_live PRIVATE luma_editor_gui luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_animation_live PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_animation_live PRIVATE m)
endif()
add_test(NAME test_animation_live COMMAND test_animation_live --shotdir "${CMAKE_CURRENT_BINARY_DIR}/shots2b")

# Pass 2B PBR grid proof (Vulkan-gated; SKIP without a device):
# properly framed 5x3 roughness x metallic material matrix through
# the real viewport (R-013 orbit + R-012 env). Shot -> pass2b.
add_executable(test_pbr_grid tests/test_pbr_grid.c)
target_link_libraries(test_pbr_grid PRIVATE luma_editor_gui luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_pbr_grid PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_pbr_grid PRIVATE m)
endif()
add_test(NAME test_pbr_grid COMMAND test_pbr_grid --shotdir "${CMAKE_CURRENT_BINARY_DIR}/shots2b")

# Phase 34A mutation matrix (each arm breaks ONE headed proof while
# armed; the report rebuilds each arm and asserts the matching proof
# FAILS, then restores. Arms live in the SOURCES as LUMA34A_MUT_*
# (gui_panels/gizmo_overlay/gui_probe/viewport_tex/import_impl) —
# these OBJECT LIBRARIES recompile the same sources per arm so the
# production tree is never touched).
#
# Shared arm-name list (10 required mutations):
#   PLAY STOP UNDO SAVE PICK GIZMO COMPOSITE INPUT DROP IDENTITY
# Per-arm GUI object libraries (same sources, one extra define
# each; the headed main links the arm instead of the stock GUI
# lib). luma_editor (core) is shared — all arms except IDENTITY
# live in GUI TUs.
foreach(MUT IN ITEMS PLAY STOP UNDO SAVE PICK GIZMO COMPOSITE INPUT DROP)
    string(TOLOWER "${MUT}" MUT_LOWER)
    if(NOT TARGET luma_editor_gui_mut_${MUT_LOWER})
        add_library(luma_editor_gui_mut_${MUT_LOWER} STATIC
            src/gui/gui_context.cpp
            src/gui/gui_input.cpp
            src/gui/gui_draw.cpp
            src/gui/gui_font.cpp
            src/gui/gui_viewport_tex.cpp
            src/gui/gui_panels.cpp
            src/gui/gui_inspector_widgets.cpp
            src/gui/gui_gizmo_overlay.cpp
            src/gui/gui_loop.cpp
            src/gui/gui_theme.cpp
            src/gui/gui_layout.cpp
            src/gui/gui_icons.cpp
            src/gui/gui_probe.cpp
            "${LUMA_IMGUI_DIR}/imgui.cpp"
            "${LUMA_IMGUI_DIR}/imgui_draw.cpp"
            "${LUMA_IMGUI_DIR}/imgui_tables.cpp"
            "${LUMA_IMGUI_DIR}/imgui_widgets.cpp")
        set_target_properties(luma_editor_gui_mut_${MUT_LOWER} PROPERTIES
            CXX_STANDARD 11
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        target_include_directories(luma_editor_gui_mut_${MUT_LOWER} PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
        target_include_directories(luma_editor_gui_mut_${MUT_LOWER} PRIVATE
            "${LUMA_IMGUI_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gui")
        target_link_libraries(luma_editor_gui_mut_${MUT_LOWER} PUBLIC luma_editor)
        if(UNIX AND NOT APPLE)
            target_link_libraries(luma_editor_gui_mut_${MUT_LOWER} PRIVATE m)
        endif()
        if(MSVC)
            target_compile_options(luma_editor_gui_mut_${MUT_LOWER} PRIVATE /W3)
        else()
            target_compile_options(luma_editor_gui_mut_${MUT_LOWER} PRIVATE -Wall -Wextra)
        endif()
        target_compile_definitions(luma_editor_gui_mut_${MUT_LOWER} PRIVATE LUMA34A_MUT_${MUT}=1)
        set_source_files_properties(
            "${LUMA_IMGUI_DIR}/imgui.cpp"
            "${LUMA_IMGUI_DIR}/imgui_draw.cpp"
            "${LUMA_IMGUI_DIR}/imgui_tables.cpp"
            "${LUMA_IMGUI_DIR}/imgui_widgets.cpp"
            TARGET_DIRECTORY luma_editor_gui_mut_${MUT_LOWER}
            PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    if(NOT TARGET test_editor_headed_mut_${MUT_LOWER})
        add_executable(test_editor_headed_mut_${MUT_LOWER} tests/test_editor_headed.c)
        target_link_libraries(test_editor_headed_mut_${MUT_LOWER} PRIVATE luma_editor_gui_mut_${MUT_LOWER} luma_engine luma_renderer lumac Vulkan::Vulkan)
        target_include_directories(test_editor_headed_mut_${MUT_LOWER} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../include)
        if(UNIX AND NOT APPLE)
            target_link_libraries(test_editor_headed_mut_${MUT_LOWER} PRIVATE m)
        endif()
    endif()
endforeach()
# M-identity lives in the CORE import TU (not the GUI): separate
# core+GUI arm pair for it.
if(NOT TARGET luma_editor_mut_identity)
    add_library(luma_editor_mut_identity STATIC
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
        src/project/prefab_core.c)
    target_include_directories(luma_editor_mut_identity PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_include_directories(luma_editor_mut_identity PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(luma_editor_mut_identity PUBLIC luma_engine)
    if(UNIX AND NOT APPLE)
        target_link_libraries(luma_editor_mut_identity PRIVATE m)
    endif()
    if(MSVC)
        target_compile_options(luma_editor_mut_identity PRIVATE /W3)
    else()
        target_compile_options(luma_editor_mut_identity PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_compile_definitions(luma_editor_mut_identity PRIVATE LUMA34A_MUT_IDENTITY=1)
endif()
if(NOT TARGET luma_editor_gui_mut_identity)
    add_library(luma_editor_gui_mut_identity STATIC
        src/gui/gui_context.cpp
        src/gui/gui_input.cpp
        src/gui/gui_draw.cpp
        src/gui/gui_font.cpp
        src/gui/gui_viewport_tex.cpp
        src/gui/gui_panels.cpp
        src/gui/gui_inspector_widgets.cpp
        src/gui/gui_gizmo_overlay.cpp
        src/gui/gui_loop.cpp
        src/gui/gui_theme.cpp
        src/gui/gui_layout.cpp
        src/gui/gui_icons.cpp
        src/gui/gui_probe.cpp
        "${LUMA_IMGUI_DIR}/imgui.cpp"
        "${LUMA_IMGUI_DIR}/imgui_draw.cpp"
        "${LUMA_IMGUI_DIR}/imgui_tables.cpp"
        "${LUMA_IMGUI_DIR}/imgui_widgets.cpp")
    set_target_properties(luma_editor_gui_mut_identity PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(luma_editor_gui_mut_identity PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_include_directories(luma_editor_gui_mut_identity PRIVATE
        "${LUMA_IMGUI_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gui")
    target_link_libraries(luma_editor_gui_mut_identity PUBLIC luma_editor_mut_identity)
    if(UNIX AND NOT APPLE)
        target_link_libraries(luma_editor_gui_mut_identity PRIVATE m)
    endif()
    if(MSVC)
        target_compile_options(luma_editor_gui_mut_identity PRIVATE /W3)
    else()
        target_compile_options(luma_editor_gui_mut_identity PRIVATE -Wall -Wextra)
    endif()
    set_source_files_properties(
        "${LUMA_IMGUI_DIR}/imgui.cpp"
        "${LUMA_IMGUI_DIR}/imgui_draw.cpp"
        "${LUMA_IMGUI_DIR}/imgui_tables.cpp"
        "${LUMA_IMGUI_DIR}/imgui_widgets.cpp"
        TARGET_DIRECTORY luma_editor_gui_mut_identity
        PROPERTIES COMPILE_OPTIONS "-w")
endif()
if(NOT TARGET test_editor_headed_mut_identity)
    add_executable(test_editor_headed_mut_identity tests/test_editor_headed.c)
    target_link_libraries(test_editor_headed_mut_identity PRIVATE luma_editor_gui_mut_identity luma_engine luma_renderer lumac Vulkan::Vulkan)
    target_include_directories(test_editor_headed_mut_identity PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_editor_headed_mut_identity PRIVATE m)
    endif()
endif()

# Phase 34A portable identity (headless script section + Vulkan-gated
# model section): relocation/reimport/rename/key-strings.
add_executable(test_portable_identity tests/test_portable_identity.c)
target_link_libraries(test_portable_identity PRIVATE luma_editor luma_assets luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_portable_identity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(test_portable_identity PRIVATE
    LE_BOX_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/BoxTextured.glb")
if(UNIX AND NOT APPLE)
    target_link_libraries(test_portable_identity PRIVATE m)
endif()
add_test(NAME test_portable_identity COMMAND test_portable_identity)
# M-identity MATRIX PROOF (model section, not the headed script
# leg — scripts preserve the key half by design, so the arm cannot
# bite them): rebuild the SAME suite against the poisoned-key core
# lib; the forced model reimport ("engine ID stable across
# relocation + reimport") MUST move IDs while armed. CTest runs it
# as a will-fail check via the report script (not a passing test).
add_executable(test_portable_identity_mut_identity tests/test_portable_identity.c)
target_link_libraries(test_portable_identity_mut_identity PRIVATE luma_editor_mut_identity luma_assets luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_portable_identity_mut_identity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(test_portable_identity_mut_identity PRIVATE
    LE_BOX_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/BoxTextured.glb")
if(UNIX AND NOT APPLE)
    target_link_libraries(test_portable_identity_mut_identity PRIVATE m)
endif()

# Recovery acceptance-project builder (LumaRealityTest): GPU-backed
# (needs a device for glTF upload), run from the REPO ROOT so the
# project lands in the source tree. Staged through the public
# led/le API (same calls the GUI panels make), never hand-written
# scene bytes. Manual target (NOT in ctest): the auditor runs it
# explicitly between evidence passes.
add_executable(editor_recovery_project demo/recovery_project.c)
target_link_libraries(editor_recovery_project PRIVATE luma_editor luma_engine luma_renderer luma_assets lumac Vulkan::Vulkan)
target_include_directories(editor_recovery_project PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(UNIX AND NOT APPLE)
    target_link_libraries(editor_recovery_project PRIVATE m)
endif()

# Phase 33 demo project builder (headless, no window/GPU): builds
# editor/demo/Phase33Demo/ (manifest + Assets/Orbit.lua +
# Assets/Spinner.luprefab + Scenes/Main.luma_scene) through the
# public led_* API. CTest runs it from the REPO ROOT (WORKING_
# DIRECTORY) so the demo lands in the source tree.
add_executable(editor_demo_phase33 demo/phase33_builder.c)
target_link_libraries(editor_demo_phase33 PRIVATE luma_editor)
target_include_directories(editor_demo_phase33 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(UNIX AND NOT APPLE)
    target_link_libraries(editor_demo_phase33 PRIVATE m)
endif()
add_test(NAME editor_demo_phase33
    COMMAND editor_demo_phase33)
set_tests_properties(editor_demo_phase33 PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
# Shared Windows builds place lumac.dll beside the top-level tests;
# editor tests live deeper, so extend their DLL search path
# explicitly (no-op for static builds; set AFTER add_test so the
# demo target exists in every tree, incl. shared).
if(WIN32)
    set_tests_properties(test_editor_core test_editor_history test_editor_play test_editor_viewport test_editor_reflect test_prefab_smoke test_project test_import test_assetdb test_project_editor test_lineend test_gltf_identity test_prefab test_editor_gui test_editor_gui_gpu test_editor_negative test_editor_headed test_portable_identity editor_demo_phase33 PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:lumac>\;$ENV{PATH}")
endif()

# Backend-independence guard for the app (same rule as GUARD 1 but
# covering editor/app + editor/tests: no backend/engine-internal/Lua
# spellings — the app talks lc_*/lr_*/le_*/led_*/leg_* ONLY — and no
# imgui spellings outside src/gui (the app never includes imgui.h).
file(GLOB_RECURSE LED_APP_AUDIT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.h")
foreach(LED_APP_AUDIT_FILE IN LISTS LED_APP_AUDIT_FILES)
    file(READ "${LED_APP_AUDIT_FILE}" LED_APP_AUDIT_TEXT)
    if(LED_APP_AUDIT_TEXT MATCHES "Vk[A-Z]|vulkan/|windows\\.h|X11/|Xlib\\.h|HWND|graphics_internal|lumac_internal|renderer_internal|engine_internal|src/graphics|src/internal|renderer/src|engine/src")
        message(FATAL_ERROR
            "Backend-independence violation in ${LED_APP_AUDIT_FILE}: the "
            "editor app renders through public LumaC (lc_*) only.")
    endif()
    if(LED_APP_AUDIT_TEXT MATCHES "lua\\.h|lauxlib\\.h|lualib\\.h|lua_State|luaL_[a-z]|LUA_[A-Z]")
        message(FATAL_ERROR
            "Lua-confinement violation in ${LED_APP_AUDIT_FILE}: the "
            "editor app never touches Lua spellings.")
    endif()
    if(LED_APP_AUDIT_TEXT MATCHES "imgui\\.h|imgui_internal\\.h|ImGui[A-Z]|ImDraw|ImVec2|ImVec4|ImTextureID")
        message(FATAL_ERROR
            "GUI-confinement violation in ${LED_APP_AUDIT_FILE}: Dear "
            "ImGui may appear only in editor/src/gui/; the app uses "
            "the leg_* C ABI.")
    endif()
endforeach()
