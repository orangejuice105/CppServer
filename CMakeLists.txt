cmake_minimum_required(VERSION 3.13)
project(CppServer LANGUAGES CXX)

# 使用 C++17，要求编译器支持它，并关闭编译器特有的语言扩展。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 查找当前平台的线程编译和链接配置。
find_package(Threads REQUIRED)

# 保留线程池供后续学习；按需使用 --target thread_pool 单独构建。
# 当前网络收发由事件循环处理，线程池不加入默认构建。
add_library(thread_pool EXCLUDE_FROM_ALL src/thread_pool.cpp)
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

# 将入口、服务器监听、连接处理、HTTP 请求解析、路由和响应构造代码一起编译、链接为 main。
add_executable(main
    src/main.cpp
    src/tcp_server.cpp
    src/tcp_connection.cpp
    src/http_request.cpp
    src/http_router.cpp
    src/http_response.cpp
)
target_link_libraries(main PRIVATE logger)

# 当前面向 Linux / WSL，开启常用编译警告帮助发现问题。
target_compile_options(main PRIVATE -Wall -Wextra -Wpedantic)

# 独立学习实验：按目标名手动构建，不加入默认构建。
add_executable(nonblocking_recv EXCLUDE_FROM_ALL experiments/nonblocking_recv.cpp)
target_compile_options(nonblocking_recv PRIVATE -Wall -Wextra -Wpedantic)

# 当前 LT epoll 验收工具；默认关闭，不参与正常服务器运行。
option(CPPSERVER_BUILD_EPOLL_TEST_TOOLS "Build Linux epoll verification tools" OFF)
if(CPPSERVER_BUILD_EPOLL_TEST_TOOLS)
    add_library(epoll_test_core OBJECT
        src/tcp_server.cpp
        src/tcp_connection.cpp
        src/http_request.cpp
        src/http_router.cpp
        src/http_response.cpp
    )
    target_link_libraries(epoll_test_core PUBLIC logger)
    target_compile_options(epoll_test_core PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(epoll_connection_checks tests/epoll_connection_checks.cpp)
    add_executable(epoll_send_checks tests/epoll_send_checks.cpp)
    add_executable(epoll_server_fault_checks tests/epoll_server_fault_checks.cpp)
    # 仅测试时把指定响应扩大；沿用真实入口、收发函数及事件循环。
    add_executable(epoll_response_fixture
        src/main.cpp tests/epoll_response_fixture.cpp)
    # 仅在生产事件循环返回后导出日志统计，供性能测试检查日志丢弃情况。
    add_executable(epoll_observed_server src/main.cpp tests/logger_stats_probe.cpp)
    foreach(test_target IN ITEMS epoll_connection_checks epoll_send_checks
            epoll_server_fault_checks epoll_response_fixture epoll_observed_server)
        target_link_libraries(${test_target} PRIVATE epoll_test_core)
        target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
    target_link_options(epoll_connection_checks PRIVATE
        -Wl,--wrap=send -Wl,--wrap=recv)
    target_link_options(epoll_server_fault_checks PRIVATE
        -Wl,--wrap=bind -Wl,--wrap=accept -Wl,--wrap=epoll_ctl
        -Wl,--wrap=epoll_wait -Wl,--wrap=close)
    target_link_options(epoll_response_fixture PRIVATE
        -Wl,--wrap=_Z14receiveRequestR13TcpConnection
        -Wl,--wrap=accept -Wl,--wrap=send)
    target_link_options(epoll_observed_server PRIVATE -Wl,--wrap=_Z12runTcpServeri)
endif()
