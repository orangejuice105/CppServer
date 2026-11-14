#include <iostream>
#include "logger.h"
#include "tcp_server.h"
#include <sys/signalfd.h>
#include <unistd.h>
#include <cerrno>
#include <system_error>

//客户端连接测试：nc 127.0.0.1 8080

/*
curl --http1.1 -i --max-time 3 http://127.0.0.1:8080/hello
让 curl 使用 HTTP/1.1，向本机 8080 端口请求 /hello，显示响应头和正文，整个操作最多等待 3 秒
curl	        命令行客户端，用来发送请求、接收服务器响应
--http1.1	    使用 HTTP/1.1，与你正在实现的协议版本一致
-i	            显示响应头；正文也会显示
--max-time 3	整个操作最多允许 3 秒，超过就停止并报超时
http://	        使用普通 HTTP
127.0.0.1	    本机回环地址，这里连接同一环境中运行的服务器
:8080	        连接服务器监听的 8080 端口
/hello	        本次请求的路径
*/

int main(){
    //准备日志配置
    LoggerConfig logConfig;
    logConfig.filePath = "server.log";
    logConfig.minLevel = LogLevel::INFO;
    
    //在日志库初始化之前屏蔽SIGINT和SIGTERM信号
    //完成信号屏蔽以后,日志线程会继承主线程的信号掩码
    sigset_t shutdownSignals;
    if(!prepareShutdownSignals(shutdownSignals)){
        const int error = errno;
        std::error_code errorCode(error,std::generic_category());
        std::cerr << "准备退出信号失败,errno="
          << error
          << ",原因="
          << errorCode.message()
          << '\n';
        return 1;
    }

    //初始化日志库
    const bool loggerReady = Logger::getInstance().init(logConfig);

    if(!loggerReady){
        std::cerr << "日志库初始化失败，服务器退出" << '\n';
        return 1;
    }

    //接收两个待处理信号
    int shutdownFd = signalfd(-1,&shutdownSignals,SFD_NONBLOCK | SFD_CLOEXEC);
    /*
    -1 表示创建新的 signal fd
    SFD_NONBLOCK 表示 epoll 报告可读后再读取时，如果暂时没有数据，不会阻塞整个事件循环
    SFD_CLOEXEC 表示以后执行其他程序时不会把这个 fd 传承下去
    */

    if(shutdownFd == -1){
        const int error = errno;
        LOGGER_ERROR("接收待处理信号失败,errno=" + std::to_string(error));
        return 1;
    }

    //先接收服务器返回值
    const int serverResult = runTcpServer(shutdownFd);

    if(close(shutdownFd) == -1){
        const int error = errno;
        LOGGER_WARN("关闭失败,fd=" + std::to_string(shutdownFd) + ",errno=" + std::to_string(error));
        return 1;
    }
    return serverResult;
}
