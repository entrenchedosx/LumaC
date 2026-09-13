add_executable(pbr_scene main.c)
target_link_libraries(pbr_scene PRIVATE luma_assets luma_renderer lumac)
target_include_directories(pbr_scene PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/include)
# Default imported model: the committed example asset. argv[1]
# overrides it at runtime.
target_compile_definitions(pbr_scene PRIVATE
    LA_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/BoxTextured.glb")
if(UNIX AND NOT APPLE)
    target_link_libraries(pbr_scene PRIVATE m)
endif()
