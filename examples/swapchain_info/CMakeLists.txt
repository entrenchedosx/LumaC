add_executable(swapchain_info main.c)
target_link_libraries(swapchain_info PRIVATE lumac)
target_include_directories(swapchain_info PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
