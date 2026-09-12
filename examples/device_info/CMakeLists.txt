add_executable(device_info main.c)
target_link_libraries(device_info PRIVATE lumac)
target_include_directories(device_info PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
