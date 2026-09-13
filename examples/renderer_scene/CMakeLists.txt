add_executable(renderer_scene main.c)
target_link_libraries(renderer_scene PRIVATE luma_renderer lumac)
target_include_directories(renderer_scene PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/include)
if(UNIX AND NOT APPLE)
    target_link_libraries(renderer_scene PRIVATE m)
endif()
