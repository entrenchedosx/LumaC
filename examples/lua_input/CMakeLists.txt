add_executable(lua_input main.c)
target_link_libraries(lua_input PRIVATE luma_engine luma_renderer lumac)
target_include_directories(lua_input PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools)
if(UNIX AND NOT APPLE)
    target_link_libraries(lua_input PRIVATE m)
endif()
