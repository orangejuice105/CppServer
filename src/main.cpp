#include <iostream>
#include "thread_pool.h"
#include "logger.h"
#include "tcp_server.h"

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
