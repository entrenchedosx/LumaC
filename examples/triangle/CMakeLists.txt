add_executable(triangle main.c)
target_link_libraries(triangle PRIVATE lumac)
target_include_directories(triangle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
# Absolute staged SPIR-V directory (build-tree binaries, recompiled
# from GLSL when glslc was available at configure time).
target_compile_definitions(triangle PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
if(TARGET lumac_triangle_shaders)
    add_dependencies(triangle lumac_triangle_shaders)
endif()
