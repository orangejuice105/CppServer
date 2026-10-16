cmake_minimum_required(VERSION 3.13)
project(TcpEchoServer LANGUAGES CXX)

# 使用 C++17，要求编译器支持它，并关闭编译器特有的语言扩展。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 查找当前平台的线程编译和链接配置。
find_package(Threads REQUIRED)

# 编译线程池库，并向使用它的目标提供头文件目录和线程依赖。
add_library(thread_pool src/thread_pool.cpp)
target_include_directories(thread_pool PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(thread_pool PUBLIC Threads::Threads)

# 编译日志库，向调用者提供头文件目录和后台线程依赖。
add_library(logger src/logger.cpp)
target_include_directories(logger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(logger PUBLIC Threads::Threads)

# 将入口文件编译为名叫 main 的可执行程序。
add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE thread_pool logger)

# 当前面向 Linux / WSL，开启常用编译警告帮助发现问题。
target_compile_options(main PRIVATE -Wall -Wextra -Wpedantic)
