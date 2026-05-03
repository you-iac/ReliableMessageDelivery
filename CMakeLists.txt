cmake_minimum_required(VERSION 3.0)
project(MiChat)


#设置全局的C++标准
set(CMAKE_CXX_STANDARD 11)#在这里我设置了11，代表使用c++11标准
set(CMAKE_CXX_STANDARD_REQUIRED True)#CMAKE_CXX_STANDARD_REQUIRED表示强制使用指定的标准

find_package(Protobuf REQUIRED)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS protocol/Message.proto)

# 公共消息协议库：
#   1. 编译 Message.proto 生成的 protobuf 代码。
#   2. 对外暴露 protocol/Codec.h。
# client 和 server 都链接这个库，从而共用同一套协议定义和编解码逻辑。
add_library(message_protocol STATIC
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    protocol/Codec.h
    protocol/EnvelopeFactory.h
    protocol/EnvelopeInspector.h
)

target_include_directories(message_protocol PUBLIC
    ${PROJECT_SOURCE_DIR}/protocol
    ${CMAKE_CURRENT_BINARY_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(message_protocol PUBLIC
    ${Protobuf_LIBRARIES}
)

add_subdirectory(client)
add_subdirectory(server)
