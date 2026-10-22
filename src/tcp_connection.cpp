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
#include <vector>
#include "http_response.h"

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

                    //请求行基本格式错误时的处理
                    bool parsed = parseRequestLine(requestLine, httpRequest);

                    //请求行解析成功则解析所有请求头字段
                    if (parsed) {
                        std::string requestHead = request.substr(0, headerEnd + 4);
                        parsed = parseRequestHeaders(requestHead, httpRequest);
                    }

                    //准备响应体
                    int statusCode = 200; //状态码
                    std::string reasonPhrase = "OK"; //状态的文字说明
                    std::string body; //根据路径选择出来的正文
                    std::string responseVersion = "HTTP/1.1";

                    //从原始请求行取第一个词，看是什么方法
                    std::size_t methodEnd = requestLine.find(' ');
                    const std::string method = requestLine.substr(0,methodEnd);

                    //解析失败时填写400响应信息
                    if(!parsed){
                        LOGGER_WARN("HTTP请求格式错误,fd=" + std::to_string(clientFd));
                        statusCode = 400; //400表示请求语法等方面的客户端错误
                        reasonPhrase = "Bad Request";
                        body = "Bad Request\n";
                    }
                    else{
                        //观察
                        for (const HttpHeader& header : httpRequest.headers) {
                            LOGGER_INFO("请求头:" + header.name + "=" + header.value);
                        }

                        LOGGER_INFO("方法=" + httpRequest.method
                                + ",目标=" + httpRequest.target
                                + ",版本=" + httpRequest.version);
                        LOGGER_INFO("目标主机与端口=" + httpRequest.authority
                                    + ",路径与查询=" + httpRequest.pathAndQuery);

                        //请求目标可能包含查询参数/hello?name=Tom ,所以要先找到?再截取
                                              //└─路径─┘└查询参数┘
                        std::size_t queryPos = httpRequest.pathAndQuery.find("?");
                        std::string path = httpRequest.pathAndQuery.substr(0,queryPos);

                        //检查请求方法，明确服务器允许什么操作
                        //认识的方法返回true
                        bool knownMethod =
                                method == "GET" || method == "HEAD" || method == "POST" ||
                                method == "PUT" || method == "DELETE" || method == "CONNECT" ||
                                method == "OPTIONS" || method == "TRACE" || method == "PATCH";

                        //检查有没有Transfer-Encoding字段
                        const bool hasTransferEncoding = hasRequestHeader(httpRequest, "transfer-encoding");

                        //检查有没有Content-Length字段
                        const bool hasContentLength = hasRequestHeader(httpRequest, "content-length");

                        std::vector<std::string> transferCodings; //接收解析得到的编码列表

                        //主版本检查
                        if (httpRequest.version[5] != '1') {
                            statusCode = 505;
                            reasonPhrase = "HTTP Version Not Supported";
                            body = "Only HTTP/1.x is supported\n";
                        }

                        //检查host数量是否合格
                        else if(!validateHostHeader(httpRequest)){
                            statusCode = 400;
                            reasonPhrase = "Bad Request";
                            body = "Bad Request\n";
                            LOGGER_WARN("host字段校验失败,fd=" + std::to_string(clientFd));
                        }

                        //检查HTTP/1.0请求是否使用Transfer-Encoding
                        else if(hasTransferEncoding && httpRequest.version == "HTTP/1.0"){
                            statusCode = 400;
                            reasonPhrase = "Bad Request";
                            body = "Bad Request\n";
                            LOGGER_WARN("HTTP/1.0 请求不能使用 Transfer-Encoding");
                        }

                        //检查请求是否同时包含Transfer-Encoding 和 Content-Length
                        else if (hasTransferEncoding && hasContentLength){
                            statusCode = 400;
                            reasonPhrase = "Bad Request";
                            body = "Bad Request\n";
                            LOGGER_WARN("请求同时包含 Transfer-Encoding 和 Content-Length");
                        }

                        //解析传输编码字段
                        else if(hasTransferEncoding && !parseTransferEncodingHeaders(httpRequest,transferCodings)){
                            statusCode = 400;
                            reasonPhrase = "Bad Request";
                            body = "Bad Request\n";
                        }

                        //检查编码顺序
                        else if(hasTransferEncoding && !validateRequestTransferCodingOrder(transferCodings)){
                            statusCode = 400;
                            reasonPhrase = "Bad Request";
                            body = "Bad Request\n";
                        }

                        //处理当前未实现的正文解码
                        else if(hasTransferEncoding){
                            statusCode = 501;
                            reasonPhrase = "Not Implemented";
                            body = "Not Implemented\n";
                            LOGGER_WARN("当前服务器尚未实现传输编码请求正文的解码");
                        }

                        //检查Content-Length是否合法
                        else if (!validateContentLengthHeader(httpRequest)) {
                            statusCode = 400;
                            reasonPhrase = "Bad Request";
                            body = "Bad Request\n";
                        }

                        //检查合法Content-Length的长度是否大于0
                        else if (hasNonZeroContentLength(httpRequest)){
                            statusCode = 413; //413表示请求正文超过服务器愿意或能够处理的大小
                            reasonPhrase = "Content Too Large";
                            body = "Request body is not supported\n";
                            LOGGER_WARN("当前服务器不接受非零长度的请求正文");
                        }

                        //对不认识的方法处理
                        else if(!knownMethod){
                            statusCode = 501; //501表示服务器不认识遇到的方法
                            reasonPhrase = "Not Implemented";
                            body = "Not Implemented\n";
                        }
                        //再处理认识但不允许的方法
                        else if(method != "GET"){
                            statusCode = 405; //405表示该资源不允许这个方法
                            reasonPhrase = "Method Not Allowed";
                            body = "Method Not Allowed\n";
                        }

                        //处理不同路径分支
                        else if(path == "/hello"){
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

                    }

                    //构造完整响应
                    if (httpRequest.version == "HTTP/1.0") {
                        responseVersion = "HTTP/1.0";
                    }

                    //返回完整响应字符串
                    std::string response = buildHttpResponse(
                        responseVersion, statusCode, reasonPhrase, body, method == "HEAD");

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
