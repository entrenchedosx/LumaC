add_executable(clear_screen main.c)
target_link_libraries(clear_screen PRIVATE lumac)
target_include_directories(clear_screen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
