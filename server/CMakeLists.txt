
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)
if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "MessageStore uses Redis by default; please install libhiredis-dev")
endif()

add_executable(server main_service.cpp
    src/ChatServer.cpp
    src/MessageStore.cpp
    src/UserStateService.cpp
    src/ServerEventDispatcher.cpp)

target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HIREDIS_INCLUDE_DIR})
    
#链接必要的库，
target_link_libraries(server
    message_protocol
    pthread
    muduo_net
    muduo_base
    ${HIREDIS_LIBRARY}
)


# 设置 server 可执行文件输出目录
set_target_properties(server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
