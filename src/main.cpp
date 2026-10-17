#include <iostream>
#include "thread_pool.h"
#include "logger.h"
#include "tcp_server.h"

//客户端连接测试：nc 127.0.0.1 8080


int main(){
    //准备日志配置
    LoggerConfig logConfig;
    logConfig.filePath = "server.log";
    logConfig.minLevel = LogLevel::INFO;

    //初始化日志库
    const bool loggerReady = Logger::getInstance().init(logConfig);

    if(!loggerReady){
        std::cerr << "日志库初始化失败，服务器退出" << '\n';
        return 1;
    }

    //测试配置：1个工作线程，等待队列容量为1
    ThreadPool pool(1,1);

    return runTcpServer(pool);
}
