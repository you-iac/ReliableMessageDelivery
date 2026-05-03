
add_executable(server main_service.cpp
    src/ChatServer.cpp)

target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
    
#链接必要的库，
target_link_libraries(server
    message_protocol
    pthread
    muduo_net
    muduo_base
)


# 设置 server 可执行文件输出目录
set_target_properties(server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
