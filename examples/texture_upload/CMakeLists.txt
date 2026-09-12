add_executable(texture_upload main.c)
target_link_libraries(texture_upload PRIVATE lumac)
target_include_directories(texture_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
