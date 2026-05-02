add_executable(client client.cpp)


target_link_libraries(client
    message_protocol
    pthread
)

# 设置 client 可执行文件输出目录
set_target_properties(client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
