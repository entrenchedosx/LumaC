add_executable(model_viewer main.c)
target_link_libraries(model_viewer PRIVATE luma_assets luma_renderer lumac)
target_include_directories(model_viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/include)
# Default model: the committed example asset. argv[1] overrides it at
# runtime, so any .glb/.gltf can be inspected without rebuilding.
target_compile_definitions(model_viewer PRIVATE
    LA_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/BoxTextured.glb")
if(UNIX AND NOT APPLE)
    target_link_libraries(model_viewer PRIVATE m)
endif()
