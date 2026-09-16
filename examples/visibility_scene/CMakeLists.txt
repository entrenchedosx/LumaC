add_executable(visibility_scene main.c)
target_link_libraries(visibility_scene PRIVATE luma_renderer lumac)
target_include_directories(visibility_scene PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools)
if(UNIX AND NOT APPLE)
    target_link_libraries(visibility_scene PRIVATE m)
endif()
