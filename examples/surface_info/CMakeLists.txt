add_executable(surface_info main.c)
target_link_libraries(surface_info PRIVATE lumac)
target_include_directories(surface_info PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
