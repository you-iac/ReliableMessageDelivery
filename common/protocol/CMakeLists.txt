find_package(Protobuf REQUIRED)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS Message.proto)


# 公共消息协议库：
#   1. 编译 Message.proto 生成的 protobuf 代码。
#   2. 对外暴露 protocol/Codec.h。
# client 和 server 都链接这个库，从而共用同一套协议定义和编解码逻辑。
add_library(message_protocol STATIC
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    Codec.h
    EnvelopeFactory.h
    EnvelopeInspector.h
)

target_include_directories(message_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(message_protocol PUBLIC
    ${Protobuf_LIBRARIES}
)

