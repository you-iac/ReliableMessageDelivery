add_executable(client client.cpp)

# 设置 client 可执行文件输出目录
set_target_properties(client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
