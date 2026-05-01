
add_executable(server service.cpp)

#链接必要的库，尤其是example中生成的静态库
target_link_libraries(server ${LIBS})


# 设置 server 可执行文件输出目录
set_target_properties(server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
