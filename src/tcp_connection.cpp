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
#include "http_router.h"
#include <chrono>
#include <algorithm>

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

// 请求接收阶段，若请求行完整、未超限且解析成功，则尝试发送指定错误响应。
// 按请求版本构造响应，并处理 HEAD 无正文；连接由 handleClient() 统一关闭。
void sendRequestErrorResponse(int clientFd,const std::string& request,std::size_t maxHeaderBytes,int statusCode,const std::string& reasonPhrase){
    //寻找第一个换行
    std::size_t lineEnd = request.find("\r\n");

    if(lineEnd == std::string::npos || lineEnd + 2 > maxHeaderBytes){
        LOGGER_WARN("请求行尚未完整接收或长度超限");
        return;
    }

    std::string requestLine = request.substr(0,lineEnd);
    HttpRequest httpRequest;

    if(!parseRequestLine(requestLine,httpRequest)){
        LOGGER_WARN("请求行解析失败");
        return;
    }

    //准备版本和HEAD标记再构造响应
    std::string responseVersion = "HTTP/1.1";
    if (httpRequest.version == "HTTP/1.0") {
        responseVersion = "HTTP/1.0";
    }
    bool isHeadRequest = httpRequest.method == "HEAD";

    std::string response = buildHttpResponse(responseVersion,statusCode,
                            reasonPhrase,
                            reasonPhrase + "\n",
                            isHeadRequest);

    //发送完整响应
    if(sendAll(clientFd,response)){
        LOGGER_INFO("HTTP错误响应发送完成,fd=" + std::to_string(clientFd) + ",状态码=" + std::to_string(statusCode));
    }

    return;
}

//先结束发送，再在时间和字节上限内清理剩余接收数据，最后关闭连接
void closeClientConnection(int clientFd){
    //先结束发送方向
    //SHUT_WR: 结束这个连接的发送方向
    int shutdownResult = shutdown(clientFd,SHUT_WR); //shutdown: 结束此连接的发送方向，仍可接收数据；最后仍需close释放描述符

    if(shutdownResult == -1){
        const int error = errno;
        LOGGER_WARN("结束发送方向失败,fd=" + std::to_string(clientFd) + ",errno=" + std::to_string(error));
        close(clientFd);
        return;
    }

    //给清理阶段设置较短的接收超时
    timeval drainTimeout{};
    drainTimeout.tv_usec = 100000;
    int drainTimeoutResult = setsockopt(clientFd,SOL_SOCKET,SO_RCVTIMEO,&drainTimeout,sizeof(drainTimeout));

    if(drainTimeoutResult == -1){
        const int error = errno;
        LOGGER_WARN("设置清理接收超时失败,fd=" + std::to_string(clientFd) + ",errno=" + std::to_string(error));
        close(clientFd);
        return;
    }

    //准备清理缓冲区、字节上限和截止时间
    char discardBuffer[1024]; //暂存清理过程中收到的数据
    const std::size_t maxDrainBytes = 64*1024; //最多额外清理64KiB
    std::size_t drainedBytes = 0; //已经清理的字节数
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1); //总截止时间

    //清理循环
    while(drainedBytes < maxDrainBytes && std::chrono::steady_clock::now() < deadline){
        //计算本次最多能读取多少字节
        std::size_t toRead = std::min(sizeof(discardBuffer),maxDrainBytes - drainedBytes);

        //读取数据
        ssize_t received = recv(clientFd,discardBuffer,toRead,0);
        //收到数据时，累积清理量并继续
        if (received > 0) {
            drainedBytes += static_cast<std::size_t>(received);
            continue;
        }
        //对方结束发送，已发来的数据也读完时停止清理
        if (received == 0) {
            break;
        }
        //接收失败时
        const int error = errno;
        //被信号中断就重新尝试
        if(error == EINTR){
            continue;
        }
        //清理接收等待超时直接结束清理
        if(error == EAGAIN || error == EWOULDBLOCK){
            break;
        }
        //其他错误记录后结束
        LOGGER_WARN("清理客户端剩余数据失败,fd=" + std::to_string(clientFd)
                    + ",errno=" + std::to_string(error));
        break;
    }
    close(clientFd);
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
    const std::size_t maxTargetBytes = 4096; //请求目标允许的最大字节数
    bool requestLineChecked = false; //是否已完成接收阶段的请求行检查

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

                //已收到数据时尝试发送 408；能否发送由辅助函数的请求行检查决定
                if(!request.empty()){
                    sendRequestErrorResponse(clientFd,request,maxHeaderBytes,408,"Request Timeout"); //408 表示服务器没有在愿意等待的时间内收到完整请求
                }
            }
            else{
                //std::error_code: 保存错误编号及其所属类别的对象
                const std::error_code ec(error,std::generic_category()); //std::generic_category: 指定按通用类别解释
                const std::string reason = ec.message(); //把编号转换成可阅读的文字说明
                LOGGER_ERROR("接收失败,fd = " + std::to_string(clientFd)
                            + ",errno=" + std::to_string(error) + ",原因="  + reason);
            }
            break;
        }

        if(received == 0){
            std::cout << "对方已结束发送" << '\n';

            if(!request.empty()){
                LOGGER_WARN("客户端结束发送时请求头仍不完整,fd=" + std::to_string(clientFd));
                sendRequestErrorResponse(clientFd,request,maxHeaderBytes,400,"Bad Request");
            }
            break;
        }
        if(received > 0){
            //累积数据
            //append: 把新内容追加到字符串末尾，保留此前内容
            //从buffer开始追加static_cast<std::size_t>(received)个字节
            request.append(buffer,static_cast<std::size_t>(received)); //static_cast: 类型转换

            //检查接收中的请求行是否已经表明目标超长
            if(!requestLineChecked){
                std::size_t lineEnd = request.find("\r\n");
                if(lineEnd == std::string::npos){
                    std::string earlyMethod; //接收判断函数输出的方法名
                    if(isRequestTargetTooLong(request,maxTargetBytes,earlyMethod)){
                        LOGGER_WARN("请求目标长度超出上限,请求行尚未完整,fd=" + std::to_string(clientFd));
                        std::string response = buildHttpResponse(
                        "HTTP/1.1", 414, "URI Too Long", "URI Too Long\n",
                        earlyMethod == "HEAD");

                        if (sendAll(clientFd, response)) {
                            LOGGER_INFO("HTTP错误响应发送完成,fd=" + std::to_string(clientFd) + ",状态码=414");
                        }
                        break;
                    }
                }

                //检查完整请求行是否表明目标超长
                if(lineEnd != std::string::npos && lineEnd + 2 <= maxHeaderBytes){
                    requestLineChecked = true; //标记这条请求行已经检查过
                    std::string earlyRequestLine = request.substr(0,lineEnd); //截取请求行
                    HttpRequest requestLineInfo;
                    bool lineParsed = parseRequestLine(earlyRequestLine,requestLineInfo); //解析

                    //解析成功后再判断目标是否超长
                    if(lineParsed && requestLineInfo.version[5] == '1' && requestLineInfo.target.size() > maxTargetBytes){
                        LOGGER_WARN("请求目标长度超出上限,fd=" + std::to_string(clientFd));
                        sendRequestErrorResponse(
                            clientFd, request, maxHeaderBytes, 414, "URI Too Long");
                        break;
                    }
                }
            }

            //判断请求头是否收完整(HTTP/1.1 请求头以空行结束，通常表现为连续的 \r\n\r\n)
            std::size_t headerEnd = request.find("\r\n\r\n");

            if(headerEnd != std::string::npos){
                if(headerEnd + 4 > maxHeaderBytes){
                    LOGGER_WARN("请求头长度超出上限");
                    sendRequestErrorResponse(
                        clientFd, request, maxHeaderBytes,431,"Request Header Fields Too Large");
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

                        //前面的请求校验都通过了才进入方法和路径分支
                        else{
                            HttpRouteResult route = routeRequest(httpRequest);
                            statusCode = route.statusCode;
                            reasonPhrase = route.reasonPhrase;
                            body = route.body;
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
                    sendRequestErrorResponse(
                        clientFd, request, maxHeaderBytes,431,"Request Header Fields Too Large");
                    break;
                }
            }
        }
    }
    closeClientConnection(clientFd);
    LOGGER_INFO("结束处理客户端,fd=" + std::to_string(clientFd));
}
