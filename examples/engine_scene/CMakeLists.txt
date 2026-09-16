add_executable(engine_scene main.c)
target_link_libraries(engine_scene PRIVATE luma_engine luma_renderer lumac)
target_include_directories(engine_scene PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools)
if(UNIX AND NOT APPLE)
    target_link_libraries(engine_scene PRIVATE m)
endif()
