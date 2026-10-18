#include "tcp_connection.h"
#include "logger.h"
#include <cerrno>
#include <iostream>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstddef>
#include "http_request.h"

//负责完整发送响应
bool sendAll(int clientFd,const std::string& data){
    std::size_t totalSent = 0;

    while(totalSent < data.size()){
        ssize_t sent = send(clientFd,
                            data.data() + totalSent, //data.data(): 取得字符串内部字节的起始地址，加上totalSent跳过已发送的
                            data.size() - totalSent, //本次最多发送剩余多少字节
                            MSG_NOSIGNAL); //延续之前的处理，避免发送失败时的SIGPIPE终止进程

        if(sent == -1){
            const int error = errno;
            if(error == EINTR){
                continue;
            }

            else if(error == EAGAIN || error == EWOULDBLOCK){
                LOGGER_WARN("客户端发送超时,fd=" + std::to_string(clientFd));
                return false;
            }

            else{
                const std::error_code ec(error,std::generic_category());
                LOGGER_ERROR("发送失败,fd=" + std::to_string(clientFd)
                            + ",errno=" + std::to_string(error)
                            + ",原因=" + ec.message());
                return false;
            }
        }

        if(sent == 0){
            LOGGER_ERROR("发送未取得进展,fd=" + std::to_string(clientFd));
            return false;
        }

        totalSent += static_cast<std::size_t>(sent);
    }

    return true;
}

void handleClient(int clientFd){
    //记录开始处理
    LOGGER_INFO("开始处理客户端,fd=" + std::to_string(clientFd));

    //timeval：表示时间的结构体
    timeval timeout{};
    timeout.tv_sec = 30; //秒，当前设置为30
    timeout.tv_usec = 0; //微秒，当前设置为0

    //设置接收超时
    int recvTimeoutResult = setsockopt(
        clientFd,
        SOL_SOCKET, //设置socket层面的通用选项
        SO_RCVTIMEO, //选项名称：接收超时
        &timeout, //选项值存的位置
        sizeof(timeout) //选项值占的字节
    );

    if(recvTimeoutResult == -1){
        const int error = errno;
        const std::error_code ec(error, std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("设置接收超时失败,关闭连接,fd=" + std::to_string(clientFd)
             + ",errno=" + std::to_string(error) + "，原因=" + reason);
        close(clientFd);
        return;
    }

    //设置发送超时
    int sendTimeoutResult = setsockopt(
        clientFd,
        SOL_SOCKET, //设置socket层面的通用选项
        SO_SNDTIMEO, //选项名称：发送超时
        &timeout, //选项值存的位置
        sizeof(timeout) //选项值占的字节
    );

    if(sendTimeoutResult == -1){
        const int error = errno;
        const std::error_code ec(error, std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("设置发送超时失败，关闭连接,fd=" + std::to_string(clientFd)
             + ",errno=" + std::to_string(error) + "，原因=" + reason);
        close(clientFd);
        return;
    }

    char buffer[1024]{}; //缓冲区，暂存收到的数据的内存

    //准备累积空间
    std::string request; //保存多次接收累积起来的字节
    const std::size_t maxHeaderBytes = 8*1024; //给请求头设置8192字节上限

    while(true){
        //读取从客户端(clientFd)发来的数据并存在buffer里
        //ssize_t: 系统接口使用的有符号整数类型
        //recv(): 接收返回收到的字节数，等于0时说明对方已正常结束发送，且待接收数据已读完，等于-1时说明接收失败
        ssize_t received = recv(clientFd,buffer,sizeof(buffer),0); //sizeof(buffer): 最多允许写入的字节数
                                                                    //0: 不启用额外的接受选项

        if(received == -1){
            const int error = errno; //保存错误编号

            //当系统调用被信号打断时，重新尝试
            if(error == EINTR){
                continue;
            }

            if(error == EAGAIN || error == EWOULDBLOCK){ //EAGAIN,EWOULDBLOCK: 系统定义的错误名称
                LOGGER_WARN("客户端接收超时,关闭连接,fd=" + std::to_string(clientFd));
            }
            else{
                //std::error_code: 保存错误编号及其所属类别的对象
                const std::error_code ec(error,std::generic_category()); //std::generic_category: 指定按通用类别解释
                const std::string reason = ec.message(); //把编号转换成可阅读的文字说明
                LOGGER_ERROR("接收失败,fd = " + std::to_string(clientFd)
                            + ",error=" + std::to_string(error) + ",原因="  + reason);
            }
            break;
        }

        if(received == 0){
            std::cout << "对方已结束发送" << '\n';
            break;
        }
        if(received > 0){
            //累积数据
            //append: 把新内容追加到字符串末尾，保留此前内容
            //从buffer开始追加static_cast<std::size_t>(received)个字节
            request.append(buffer,static_cast<std::size_t>(received)); //static_cast: 类型转换

            //判断请求头是否收完整(HTTP/1.1 请求头以空行结束，通常表现为连续的 \r\n\r\n)
            std::size_t headerEnd = request.find("\r\n\r\n");

            if(headerEnd != std::string::npos){
                if(headerEnd + 4 > maxHeaderBytes){
                    LOGGER_WARN("请求头长度超出上限");
                    break;
                }
                else{
                    //打印完整请求头
                    std::cout << request.substr(0, headerEnd + 4) << std::endl;

                    //解析请求行
                    //先找到\r\n再截取请求行
                    std::size_t lineEnd = request.find("\r\n");
                    std::string requestLine = request.substr(0,lineEnd);

                    //创建结果对象并调用解析函数
                    HttpRequest httpRequest;

                    if (!parseRequestLine(requestLine, httpRequest)) {
                        LOGGER_WARN("请求行基本格式错误,fd=" + std::to_string(clientFd));
                        break;
                    }

                    LOGGER_INFO("方法=" + httpRequest.method
                                + ",目标=" + httpRequest.target
                                + ",版本=" + httpRequest.version);

                    //准备响应体
                    int statusCode = 200; //状态码
                    std::string reasonPhrase = "OK"; //状态的文字说明
                    std::string body; //根据路径选择出来的正文

                    //请求目标可能包含查询参数/hello?name=Tom ,所以要先找到?再截取
                                          //└─路径─┘└查询参数┘
                    std::size_t queryPos = httpRequest.target.find("?");
                    std::string path = httpRequest.target.substr(0,queryPos);

                    //处理不同路径分支
                    if(path == "/hello"){
                        body = "Hello,HTTP!\n";
                    }

                    else if(path == "/health"){
                        body = "OK\n";
                    }

                    else {
                        statusCode = 404;
                        reasonPhrase = "Not Found";
                        body = "Not Found\n";
                    }

                    //构造完整响应
                    std::string response = "HTTP/1.1 " + std::to_string(statusCode) + " " + reasonPhrase + "\r\n";
                    response += "Content-Type: text/plain; charset=utf-8\r\n";
                    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                    response += "Connection: close\r\n";
                    response += "\r\n";
                    response += body;

                    //发送完整响应
                    if(sendAll(clientFd,response)){
                        LOGGER_INFO("HTTP响应发送完成,fd=" + std::to_string(clientFd));
                    }

                    break;
                }
            }
            else{
                if(request.size() >= maxHeaderBytes){
                    LOGGER_WARN("累积数据达到上限");
                    break;
                }
            }
        }
    }
    LOGGER_INFO("结束处理客户端,fd=" + std::to_string(clientFd));
    close(clientFd);
}
