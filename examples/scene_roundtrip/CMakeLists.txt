add_executable(scene_roundtrip main.c)
target_link_libraries(scene_roundtrip PRIVATE luma_engine luma_assets luma_renderer lumac)
target_include_directories(scene_roundtrip PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools)
if(UNIX AND NOT APPLE)
    target_link_libraries(scene_roundtrip PRIVATE m)
endif()
