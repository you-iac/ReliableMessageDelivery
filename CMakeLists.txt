cmake_minimum_required(VERSION 3.0)
project(MiChat)


#设置全局的C++标准
set(CMAKE_CXX_STANDARD 11)#在这里我设置了11，代表使用c++11标准
set(CMAKE_CXX_STANDARD_REQUIRED True)#CMAKE_CXX_STANDARD_REQUIRED表示强制使用指定的标准


add_subdirectory(common)
add_subdirectory(client)
add_subdirectory(server)
