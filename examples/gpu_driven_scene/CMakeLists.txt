add_executable(gpu_driven_scene main.c)
target_link_libraries(gpu_driven_scene PRIVATE luma_renderer lumac)
target_include_directories(gpu_driven_scene PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools)
if(UNIX AND NOT APPLE)
    target_link_libraries(gpu_driven_scene PRIVATE m)
endif()
