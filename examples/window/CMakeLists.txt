add_executable(window_example main.c)
target_link_libraries(window_example PRIVATE lumac)
target_include_directories(window_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
