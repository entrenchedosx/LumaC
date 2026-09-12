add_executable(basic_init main.c)
target_link_libraries(basic_init PRIVATE lumac)
target_include_directories(basic_init PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
