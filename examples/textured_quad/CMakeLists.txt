add_executable(textured_quad main.c)
target_link_libraries(textured_quad PRIVATE lumac)
target_include_directories(textured_quad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
# Absolute staged SPIR-V directory (build-tree binaries, recompiled
# from GLSL when glslc was available at configure time).
target_compile_definitions(textured_quad PRIVATE LC_TRIANGLE_SPV_DIR="${LC_SPV_DIR}")
if(TARGET lumac_triangle_shaders)
    add_dependencies(textured_quad lumac_triangle_shaders)
endif()
