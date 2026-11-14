#include "tcp_connection.h"
#include "logger.h"
#include <cerrno>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <cstddef>
#include "http_request.h"
#include <vector>
#include "http_response.h"
#include "http_router.h"
#include <chrono>
#include <algorithm>

std::string buildRequestErrorResponse(const std::string& request,std::size_t maxHeaderBytes,int statusCode,const std::string& reasonPhrase){
    //寻找第一个换行
    std::size_t lineEnd = request.find("\r\n");

    if(lineEnd == std::string::npos || lineEnd + 2 > maxHeaderBytes){
        LOGGER_WARN("请求行尚未完整接收或长度超限");
        return "";
    }

    std::string requestLine = request.substr(0,lineEnd);
    HttpRequest httpRequest;

    if(!parseRequestLine(requestLine,httpRequest)){
        LOGGER_WARN("请求行解析失败");
        return "";
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

    return response;
}

std::string processHttpRequest(const std::string& requestHead, int clientFd) {
    //解析请求行
    //先找到\r\n再截取请求行
    std::size_t lineEnd = requestHead.find("\r\n");
    std::string requestLine = requestHead.substr(0,lineEnd);

    //创建结果对象并调用解析函数
    HttpRequest httpRequest;

    //请求行基本格式错误时的处理
    bool parsed = parseRequestLine(requestLine, httpRequest);

    //请求行解析成功则解析所有请求头字段
    if (parsed) {
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
        //取出不含查询参数的路径
        std::size_t queryPos = httpRequest.pathAndQuery.find('?');
        std::string logPath = httpRequest.pathAndQuery.substr(0,queryPos);

        //处理没有适用路径的情况
        if(logPath.empty()){
            logPath = "-"; //日志里的 - 表示“没有适用的路径”，方便阅读
        }
        //记录简要请求信息，不输出请求头字段值和查询参数
        LOGGER_INFO("方法=" + httpRequest.method
                    + ",路径=" + logPath
                    + ",版本=" + httpRequest.version
                    + ",fd=" + std::to_string(clientFd));

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
    return response;
}

ReceiveResult checkReceiveTimeout(TcpConnection& connection){
    const std::size_t maxHeaderBytes = 8 * 1024;
    const auto now = std::chrono::steady_clock::now();

    //判断两个期限
    bool totalExpired = now >= connection.requestDeadline;
    bool idleExpired = now >= connection.receiveIdleDeadline;

    //总期限没到，空闲期限也没到才允许继续等待
    if(!totalExpired && !idleExpired){
        return ReceiveResult::NeedMoreData;
    }

    if(totalExpired){
        LOGGER_WARN("请求头接收总时限已到,fd=" + std::to_string(connection.fd));
    }

    if(idleExpired){
        LOGGER_WARN("接收等待超时,fd=" + std::to_string(connection.fd));
    }

    //到期后尝试准备408
    //空连接没有请求数据，返回关闭结果，由调用者释放连接
    if(connection.request.empty()){
        return ReceiveResult::CloseConnection;
    }

    connection.response = buildRequestErrorResponse(
                        connection.request, maxHeaderBytes, 408, "Request Timeout");

    //判断响应是否构造成功,失败请求释放连接
    if(connection.response.empty()){
        return ReceiveResult::CloseConnection;
    }

    //最后设置发送进度并返回
    connection.sentBytes = 0;
    return ReceiveResult::ResponseReady;
}

ReceiveResult receiveRequest(TcpConnection& connection){
    //准备临时缓冲区和长度预算
    char buffer[1024]{};
    const std::size_t maxHeaderBytes = 8*1024; //请求头的上限
    const std::size_t maxTargetBytes = 4096; //请求行中请求目标的上限，例如/hello?name=Tom这一部分

    //处理中断重试的循环
    while (true){
        //检查是否超时
        ReceiveResult timeoutResult = checkReceiveTimeout(connection);
        if (timeoutResult != ReceiveResult::NeedMoreData) {
            return timeoutResult;
        }

        ssize_t received = recv(connection.fd,buffer,sizeof(buffer),0);
        if(received == -1){
            const int error = errno;
            if(error == EINTR){
                continue;
            }

            else if(error == EAGAIN || error == EWOULDBLOCK){
                return ReceiveResult::NeedMoreData;
            }

            else{
                LOGGER_ERROR("接收客户端请求失败,fd=" + std::to_string(connection.fd) + ",errno=" + std::to_string(error));
                return ReceiveResult::CloseConnection;
            }
        }

        //读取后再检查一次，防止读取后已经越过截止时间，之后才允许更新空闲期限
        timeoutResult = checkReceiveTimeout(connection);
        if (timeoutResult != ReceiveResult::NeedMoreData) {
            return timeoutResult;
        }

        //处理接收方向结束，请求仍未完成
        if(received == 0){
            //没收到请求结束连接
            if(connection.request.empty()){
                return ReceiveResult::CloseConnection;
            }
            //已经收到部分请求尝试准备400
            connection.response = buildRequestErrorResponse(
                            connection.request, maxHeaderBytes, 400, "Bad Request");

            //处理响应为空
            if(connection.response.empty()){
                return ReceiveResult::CloseConnection;
            }
            //响应非空则将错误响应保存到连接对象，发送进度从0开始，由后续发送流程处理
            connection.sentBytes = 0;
            return ReceiveResult::ResponseReady;
        }

        //成功收到数据，重新给予30秒等待时间
        connection.receiveIdleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        //received > 0时把数据追加到连接对象
        connection.request.append(buffer,static_cast<std::size_t>(received));

        //请求行尚未完整时，提前检查请求目标是否超限
        //先判断请求行是否还需要检查
        if(!connection.requestLineChecked){
            std::size_t lineEnd = connection.request.find("\r\n");
            if(lineEnd == std::string::npos){
                std::string earlyMethod;
                if(isRequestTargetTooLong(connection.request,maxTargetBytes,earlyMethod)){
                    LOGGER_WARN("请求行尚未完整,但请求目标已经超限,fd=" + std::to_string(connection.fd));
                    connection.response = buildHttpResponse("HTTP/1.1",414,"URI Too Long","URI Too Long\n",earlyMethod == "HEAD");
                    connection.sentBytes = 0;
                    return ReceiveResult::ResponseReady;
                }
            }

            if(lineEnd != std::string::npos && lineEnd + 2 <= maxHeaderBytes){
                connection.requestLineChecked = true; //标记已经进行过这一轮检查，不表示请求行一定合法

                //截取请求行再调用解析函数
                std::string earlyRequestLine = connection.request.substr(0,lineEnd);
                HttpRequest requestLineInfo;
                bool lineParsed = parseRequestLine(earlyRequestLine,requestLineInfo);

                if(lineParsed && requestLineInfo.version[5] == '1' && requestLineInfo.target.size() > maxTargetBytes){
                    LOGGER_WARN("请求目标超限,fd=" + std::to_string(connection.fd));
                    connection.response = buildRequestErrorResponse(connection.request,maxHeaderBytes,414,"URI Too Long");

                    if(connection.response.empty()){
                        return ReceiveResult::CloseConnection;
                    }
                    connection.sentBytes = 0;
                    return ReceiveResult::ResponseReady;
                }
            }
        }

        //寻找请求头结束位置
        std::size_t headerEnd = connection.request.find("\r\n\r\n");
        if(headerEnd != std::string::npos){
            //处理完整请求头超限的情况
            if(headerEnd + 4 > maxHeaderBytes){
                LOGGER_WARN("完整请求头超限,fd=" + std::to_string(connection.fd));
                connection.response = buildRequestErrorResponse(
                        connection.request, maxHeaderBytes, 431, "Request Header Fields Too Large");

                if(connection.response.empty()){
                    return ReceiveResult::CloseConnection;
                }
                connection.sentBytes = 0;
                return ReceiveResult::ResponseReady;
            }

            //没超限时，处理完整请求
            //先截取请求头
            std::string requestHead = connection.request.substr(0,headerEnd + 4);
            connection.response = processHttpRequest(requestHead,connection.fd);

            connection.sentBytes = 0;
            return ReceiveResult::ResponseReady;
        }
        else{
            if(connection.request.size() >= maxHeaderBytes){
                LOGGER_WARN("请求头超限,fd=" + std::to_string(connection.fd));
                connection.response = buildRequestErrorResponse(
                        connection.request, maxHeaderBytes, 431, "Request Header Fields Too Large");

                if(connection.response.empty()){
                    return ReceiveResult::CloseConnection;
                }
                connection.sentBytes = 0;
                return ReceiveResult::ResponseReady;
            }
        }

        return ReceiveResult::NeedMoreData;
    }

}

SendResult sendResponse(TcpConnection& connection){
    //防止后面计算剩余长度时出错
    if(connection.sentBytes > connection.response.size()){
        LOGGER_ERROR("进度异常,fd=" + std::to_string(connection.fd));
        return SendResult::CloseConnection;
    }
    //没有剩余数据时
    if(connection.sentBytes == connection.response.size()){
        return SendResult::SendComplete;
    }

    //准备中断循环尝试
    while(true){
        //每次发送前先检查是否到达发送截止时间
        if(std::chrono::steady_clock::now() >= connection.sendIdleDeadline){
            LOGGER_WARN("发送等待超时,fd=" + std::to_string(connection.fd));
            return SendResult::CloseConnection;
        }

        ssize_t sent = send(connection.fd,connection.response.data() + connection.sentBytes,
                            connection.response.size() - connection.sentBytes,MSG_NOSIGNAL);

        if(sent == -1){
            const int error = errno;
            if(error == EINTR){
                continue;
            }

            else if(error == EAGAIN || error == EWOULDBLOCK){
                return SendResult::NeedWrite;
            }

            else{
                LOGGER_ERROR("发送失败,fd=" + std::to_string(connection.fd)
                            + ",errno=" + std::to_string(error));
                return SendResult::CloseConnection;
            }
        }
        if(sent == 0){
            LOGGER_ERROR("发送未取得进展,fd=" + std::to_string(connection.fd));
            return SendResult::CloseConnection;
        }

        //成功发送后记录时刻并累计进度
        const auto sentAt = std::chrono::steady_clock::now();
        connection.sentBytes += static_cast<std::size_t>(sent);

        //数据发完了返回SendComplete
        if(connection.sentBytes == connection.response.size()){
            return SendResult::SendComplete;
        }

        //数据没发完检查是否已经过期
        if(sentAt >= connection.sendIdleDeadline){
            LOGGER_WARN("发送等待超时,fd=" + std::to_string(connection.fd));
            return SendResult::CloseConnection;
        }

        //没过期则给予新的30s等待时间
        connection.sendIdleDeadline = sentAt + std::chrono::seconds(30);

        return SendResult::NeedWrite;
    }
}

bool beginDrain(TcpConnection& connection){
    //已经进入清理阶段，就直接返回成功，目的是保留原来的进度和截止时间
    if(connection.phase == ConnectionPhase::Draining){
        return true;
    }

    //判断是否满足开始清理的条件
    //连接处于发送阶段而且整个响应已经交给内核才能正常进入清理
    if(connection.phase != ConnectionPhase::Sending || connection.sentBytes != connection.response.size()){
        LOGGER_WARN("连接阶段或发送进度不满足开始清理的条件,fd=" + std::to_string(connection.fd));
        return false;
    }

    //结束发送方向
    int shutdownResult = shutdown(connection.fd,SHUT_WR);

    if(shutdownResult == -1){
        const int error = errno;
        LOGGER_WARN("结束发送方向失败,fd=" + std::to_string(connection.fd) + ",errno=" + std::to_string(error));
        return false;
    }

    //设置清理进度和截止时间
    const auto now = std::chrono::steady_clock::now(); //先取得当前时间
    connection.drainedBytes = 0; //把已清理字节数归零
    connection.drainDeadline = now + std::chrono::seconds(1); //设置整个清理阶段最多持续1秒
    connection.drainIdleDeadline = now + std::chrono::milliseconds(100); //当前最多等待下一批数据100毫秒

    //切换阶段并返回
    connection.phase = ConnectionPhase::Draining;

    return true;
}

DrainResult drainConnection(TcpConnection& connection){
    //先检查阶段，再准备缓冲区
    if(connection.phase != ConnectionPhase::Draining){
        LOGGER_WARN("连接未进入清理阶段,fd=" + std::to_string(connection.fd));
        return DrainResult::CloseConnection;
    }

    char discardBuffer[1024]; //暂存本次读出的数据
    const std::size_t maxDrainBytes = 64*1024; //读出数据之和的最大字节数

    while(true){
        const auto now = std::chrono::steady_clock::now();

        if(connection.drainedBytes >= maxDrainBytes){
            LOGGER_INFO("达到字节上限,fd=" + std::to_string(connection.fd));
            return DrainResult::CloseConnection;
        }

        if(now >= connection.drainDeadline){
            LOGGER_INFO("达到总时限,fd=" + std::to_string(connection.fd));
            return DrainResult::CloseConnection;
        }

        if(now >= connection.drainIdleDeadline){
            LOGGER_INFO("等待数据超时,fd=" + std::to_string(connection.fd));
            return DrainResult::CloseConnection;
        }

        //通过检查后说明剩余字节预算大于零
        //计算本次读取多少，再调用recv()
        std::size_t toRead = std::min(sizeof(discardBuffer),maxDrainBytes - connection.drainedBytes);

        ssize_t received = recv(connection.fd,discardBuffer,toRead,0);

        if(received > 0){
            //先累计清理量
            connection.drainedBytes += static_cast<std::size_t>(received);

            //重新取得读到这批数据的时间
            const auto receivedAt = std::chrono::steady_clock::now();

            //再更新等待期限
            connection.drainIdleDeadline = receivedAt + std::chrono::milliseconds(100);

            if(connection.drainedBytes >= maxDrainBytes || receivedAt >= connection.drainDeadline){
                //先确定原因
                std::string reason = "达到1秒总时限";
                if (connection.drainedBytes >= maxDrainBytes) {
                    reason = "达到64KiB上限";
                }

                LOGGER_INFO("连接清理结束,fd=" + std::to_string(connection.fd)
                            + ",已清理字节=" + std::to_string(connection.drainedBytes)
                            + ",原因=" + reason);
                return DrainResult::CloseConnection;
            }
            return DrainResult::NeedMoreData;
        }

        //对方结束发送且没有剩余数据可读时
        if(received == 0){
            LOGGER_INFO("对方结束发送,清理结束,fd=" + std::to_string(connection.fd));
            return DrainResult::CloseConnection;
        }

        //失败时
        if(received == -1){
            const int error = errno;
            if(error == EINTR){
                continue;
            }

            if(error == EAGAIN || error == EWOULDBLOCK){
                return DrainResult::NeedMoreData;
            }

            LOGGER_WARN("清理客户端剩余数据失败,fd=" + std::to_string(connection.fd) + ",errno=" + std::to_string(error));
            return DrainResult::CloseConnection;
        }
    }
}
