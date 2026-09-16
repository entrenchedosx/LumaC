# Luma Engine: scene objects on top of public Luma Renderer + LumaC only.
cmake_minimum_required(VERSION 3.15)

# Backend-independence guard: engine library headers and sources
# (everything except tests/, which take the same white-box privilege
# as LumaC's own integration tests) must never name backend symbols,
# LumaC internals, or renderer internals. src/script/*.c is covered
# too — but Lua is allowed there ONLY (vendored backend, confined to
# the script runtime): the audit permits lua/LUA/lua_ spellings in
# src/script/ while still forbidding them everywhere else. Fails
# configure loudly if the boundary rots. (This CMakeLists itself is
# excluded: it spells the forbidden pattern to enforce it.)
file(GLOB_RECURSE LE_AUDIT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(LE_AUDIT_FILE IN LISTS LE_AUDIT_FILES)
    file(READ "${LE_AUDIT_FILE}" LE_AUDIT_TEXT)
    if(LE_AUDIT_TEXT MATCHES "Vk[A-Z]|vulkan/|windows\\.h|X11/|Xlib\\.h|HWND|graphics_internal|lumac_internal|renderer_internal|src/graphics|src/internal|renderer/src")
        message(FATAL_ERROR
            "Backend-independence violation in ${LE_AUDIT_FILE}: Luma "
            "Engine library sources may use public luma_renderer.h and "
            "lumac.h only.")
    endif()
    if(NOT LE_AUDIT_FILE MATCHES "/src/script/")
        # VM-confinement: no Lua API SPELLING (headers, types, aux
        # entry points, macro families) outside src/script/. Plain
        # prose ("Lua bindings", "lua_bytes" fields, "UTF-8 Lua
        # bytes") is documentation, not a dependency — so the
        # patterns below anchor on code spellings only.
        if(LE_AUDIT_TEXT MATCHES "lua\\.h|lauxlib\\.h|lualib\\.h|lua_State|luaL_[a-z]|LUA_[A-Z]")
            message(FATAL_ERROR
                "Lua-confinement violation in ${LE_AUDIT_FILE}: Lua "
                "may appear only in engine/src/script/ (private "
                "backend); public headers and general engine sources "
                "must stay VM-independent.")
        endif()
    endif()
endforeach()

# Tests register from here (the top level enables testing after this
# directory is added, so enable it locally too).
enable_testing()

# Vendored Lua 5.4.8 (third_party/lua, UNMODIFIED upstream): private
# static library for the Phase 26 script backend. lua.c/luac.c (the
# standalone interpreters) are excluded — only the library. No Lua
# header ever crosses the public engine API (audit below covers only
# engine/ sources; third_party/ is intentionally outside it), and no
# Lua symbol is exported from luma_engine.
file(GLOB LUMA_LUA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lapi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lauxlib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lbaselib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lcode.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lcorolib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lctype.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/ldblib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/ldebug.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/ldo.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/ldump.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lfunc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lgc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/linit.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/liolib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/llex.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lmathlib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lmem.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/loadlib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lobject.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lopcodes.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/loslib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lparser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lstate.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lstring.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lstrlib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/ltable.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/ltablib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/ltm.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lundump.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lutf8lib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lvm.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lzio.c")
add_library(luma_lua STATIC ${LUMA_LUA_SOURCES})
target_include_directories(luma_lua PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua>
    $<INSTALL_INTERFACE:include>)
if(MSVC)
    target_compile_options(luma_lua PRIVATE /W3)
else()
    target_compile_options(luma_lua PRIVATE -Wall -Wextra)
endif()

add_library(luma_engine STATIC
    src/engine.c
    src/world.c
    src/object.c
    src/transform.c
    src/component.c
    src/math.c
    src/sync.c
    src/asset.c
    src/reparent.c
    src/scene.c
    src/serialize.c
    src/gltf_bridge.c
    src/lifecycle.c
    src/input/input.c
    src/input/actions.c
    src/time/time.c
    src/physics/physics.c
    src/physics/body.c
    src/physics/dynamics.c
    src/physics/collider_pose.c
    src/physics/broadphase.c
    src/physics/narrowphase.c
    src/physics/solver.c
    src/physics/step.c
    src/physics/events.c
    src/physics/query.c
    src/physics/scene_physics.c
    src/physics/cast.c
    src/physics/ccd.c
    src/physics/character.c
    src/animation/anim_asset.c
    src/animation/anim_sample.c
    src/animation/anim_pose.c
    src/animation/animator.c
    src/animation/anim_playback.c
    src/animation/anim_step.c
    src/animation/anim_api.c
    src/animation/anim_gltf.c
    src/animation/anim_serialize.c
    src/gltf_bridge_anim.c
    src/script/script_bind_anim.c
    src/script/script_bind_character.c
    src/script/script_bind_physics.c
    src/script/script_runtime.c
    src/script/script_lua.c
    src/script/script_bind_world.c
    src/script/script_bind_object.c
    src/script/script_bind_input.c
    src/script/script_asset.c
    src/script/script_step.c
)

target_include_directories(luma_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(luma_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua
)

# Public renderer (+ transitive LumaC) and assets (glTF bridge)
# dependencies only. No backend, platform, or internal includes.
# Layering stays downward: engine -> assets -> renderer -> LumaC.
# luma_lua is PRIVATE: no Lua type crosses luma_engine.h, and no
# consumer of the engine ever needs Lua headers or symbols.
target_link_libraries(luma_engine PUBLIC luma_renderer luma_assets)
target_link_libraries(luma_engine PRIVATE luma_lua)
target_include_directories(luma_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
)

if(MSVC)
    target_compile_options(luma_engine PRIVATE /W3)
else()
    target_compile_options(luma_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(luma_engine PRIVATE m)
endif()

# Engine tests (CTest registration flows from the top level).
add_executable(test_engine tests/test_engine.c)
target_link_libraries(test_engine PRIVATE luma_engine)
target_include_directories(test_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_engine PRIVATE m)
endif()
add_test(NAME test_engine COMMAND test_engine)

add_executable(test_engine_stress tests/test_engine_stress.c)
target_link_libraries(test_engine_stress PRIVATE luma_engine)
target_include_directories(test_engine_stress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_engine_stress PRIVATE m)
endif()
add_test(NAME test_engine_stress COMMAND test_engine_stress)

add_executable(test_engine_vulkan tests/test_engine_vulkan.c)
target_link_libraries(test_engine_vulkan PRIVATE luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_engine_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_engine_vulkan PRIVATE m)
endif()
add_test(NAME test_engine_vulkan COMMAND test_engine_vulkan)

add_executable(test_scene tests/test_scene.c)
target_link_libraries(test_scene PRIVATE luma_engine)
target_include_directories(test_scene PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_scene PRIVATE m)
endif()
add_test(NAME test_scene COMMAND test_scene)

# Phase 26: headless Lua scripting runtime tests (no GPU/window).
add_executable(test_script tests/test_script.c)
target_link_libraries(test_script PRIVATE luma_engine)
target_include_directories(test_script PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_script PRIVATE m)
endif()
add_test(NAME test_script COMMAND test_script)

add_executable(test_scene_vulkan tests/test_scene_vulkan.c)
target_link_libraries(test_scene_vulkan PRIVATE luma_engine luma_assets luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_scene_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_scene_vulkan PRIVATE
    LE_BOX_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/BoxTextured.glb")
if(UNIX AND NOT APPLE)
    target_link_libraries(test_scene_vulkan PRIVATE m)
endif()
add_test(NAME test_scene_vulkan COMMAND test_scene_vulkan)

# Phase 26: scripted renderables through the render path
# (lifecycle, mutation, errors, serialization, fixed-step pixels).
add_executable(test_script_vulkan tests/test_script_vulkan.c)
target_link_libraries(test_script_vulkan PRIVATE luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_script_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_script_vulkan PRIVATE m)
endif()
add_test(NAME test_script_vulkan COMMAND test_script_vulkan)

# Phase 27: headless input/time/lifecycle tests (no GPU/window;
# injection + explicit-delta stepping only).
add_executable(test_input tests/test_input.c)
target_link_libraries(test_input PRIVATE luma_engine)
target_include_directories(test_input PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_input PRIVATE m)
endif()
add_test(NAME test_input COMMAND test_input)

# Phase 27: input/time -> Lua -> transform integration (headless).
add_executable(test_input_script tests/test_input_script.c)
target_link_libraries(test_input_script PRIVATE luma_engine)
target_include_directories(test_input_script PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_input_script PRIVATE m)
endif()
add_test(NAME test_input_script COMMAND test_input_script)

# Phase 27: input-driven scripts through the render path
# (inject -> Lua -> transform -> GPU -> readback pixel proof).
add_executable(test_input_vulkan tests/test_input_vulkan.c)
target_link_libraries(test_input_vulkan PRIVATE luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_input_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_input_vulkan PRIVATE m)
endif()
add_test(NAME test_input_vulkan COMMAND test_input_vulkan)

# Phase 27 micro-benchmarks (informative; always exits 0).
add_executable(test_input_bench tests/test_input_bench.c)
target_link_libraries(test_input_bench PRIVATE luma_engine)
target_include_directories(test_input_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_input_bench PRIVATE m)
endif()
add_test(NAME test_input_bench COMMAND test_input_bench)

# Phase 28: headless physics & collision foundation tests.
add_executable(test_physics tests/test_physics.c)
target_link_libraries(test_physics PRIVATE luma_engine)
target_include_directories(test_physics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_physics PRIVATE m)
endif()
add_test(NAME test_physics COMMAND test_physics)

# Phase 28: Lua physics bindings + collision callbacks (headless).
add_executable(test_physics_script tests/test_physics_script.c)
target_link_libraries(test_physics_script PRIVATE luma_engine)
target_include_directories(test_physics_script PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_physics_script PRIVATE m)
endif()
add_test(NAME test_physics_script COMMAND test_physics_script)

# Phase 28: physics-driven renderables through the render path.
add_executable(test_physics_vulkan tests/test_physics_vulkan.c)
target_link_libraries(test_physics_vulkan PRIVATE luma_engine luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_physics_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_physics_vulkan PRIVATE m)
endif()
add_test(NAME test_physics_vulkan COMMAND test_physics_vulkan)

# Phase 28 micro-benchmarks (informative; always exits 0).
add_executable(test_physics_bench tests/test_physics_bench.c)
target_link_libraries(test_physics_bench PRIVATE luma_engine)
target_include_directories(test_physics_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_physics_bench PRIVATE m)
endif()
add_test(NAME test_physics_bench COMMAND test_physics_bench)

# Phase 30: capsule collider tests (headless).
add_executable(test_capsule tests/test_capsule.c)
target_link_libraries(test_capsule PRIVATE luma_engine)
target_include_directories(test_capsule PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_capsule PRIVATE m)
endif()
add_test(NAME test_capsule COMMAND test_capsule)

# Phase 30: shape-cast / sweep tests (headless).
add_executable(test_cast tests/test_cast.c)
target_link_libraries(test_cast PRIVATE luma_engine)
target_include_directories(test_cast PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_cast PRIVATE m)
endif()
add_test(NAME test_cast COMMAND test_cast)

# Phase 30: CCD foundation tests (headless).
add_executable(test_ccd tests/test_ccd.c)
target_link_libraries(test_ccd PRIVATE luma_engine)
target_include_directories(test_ccd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ccd PRIVATE m)
endif()
add_test(NAME test_ccd COMMAND test_ccd)

# Phase 30: character controller tests (headless).
add_executable(test_character tests/test_character.c)
target_link_libraries(test_character PRIVATE luma_engine)
target_include_directories(test_character PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_character PRIVATE m)
endif()
add_test(NAME test_character COMMAND test_character)

# Phase 30: character platform/trigger/push tests (headless).
add_executable(test_character_platform
    tests/test_character_platform.c)
target_link_libraries(test_character_platform PRIVATE luma_engine)
target_include_directories(test_character_platform PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_character_platform PRIVATE m)
endif()
add_test(NAME test_character_platform
    COMMAND test_character_platform)

# Phase 30: Lua character gameplay tests (headless).
add_executable(test_character_script
    tests/test_character_script.c)
target_link_libraries(test_character_script PRIVATE luma_engine)
target_include_directories(test_character_script PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_character_script PRIVATE m)
endif()
add_test(NAME test_character_script
    COMMAND test_character_script)

# Phase 29: headless animation & skinning foundation tests.
add_executable(test_animation tests/test_animation.c)
target_link_libraries(test_animation PRIVATE luma_engine)
target_include_directories(test_animation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_animation PRIVATE m)
endif()
add_test(NAME test_animation COMMAND test_animation)

# Phase 29: Lua animation playback bindings (headless).
add_executable(test_animation_script tests/test_animation_script.c)
target_link_libraries(test_animation_script PRIVATE luma_engine)
target_include_directories(test_animation_script PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_animation_script PRIVATE m)
endif()
add_test(NAME test_animation_script COMMAND test_animation_script)

# Phase 29: animated + skinned renderables through the render
# path (static regression, motion proof, CPU/GPU oracle, glTF
# bridge import over the skinned fixture).
add_executable(test_animation_vulkan tests/test_animation_vulkan.c)
target_link_libraries(test_animation_vulkan PRIVATE luma_engine luma_assets luma_renderer lumac Vulkan::Vulkan)
target_include_directories(test_animation_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_animation_vulkan PRIVATE
    LE_ANIM_GLB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/tests/fixtures/skinned.glb")
if(UNIX AND NOT APPLE)
    target_link_libraries(test_animation_vulkan PRIVATE m)
endif()
add_test(NAME test_animation_vulkan COMMAND test_animation_vulkan)

# Shared Windows builds place lumac.dll beside the top-level tests;
# engine tests live one directory deeper, so extend their DLL search
# path explicitly (no-op for static builds).
if(WIN32)
    set_tests_properties(test_engine test_engine_stress test_engine_vulkan test_scene test_scene_vulkan test_script test_script_vulkan test_input test_input_script test_input_vulkan test_input_bench test_physics test_physics_script test_physics_vulkan test_physics_bench test_animation test_animation_script test_animation_vulkan test_capsule test_cast test_ccd test_character test_character_platform test_character_script PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:lumac>\;$ENV{PATH}")
endif()
