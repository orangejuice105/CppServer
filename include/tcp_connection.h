#pragma once
#include <string>
#include <cstddef>
//处理一个客户端连接，结束时负责关闭 clientFd。
void handleClient(int clientFd);

//保存单个连接的收发数据与处理进度，供多次事件处理继续使用
struct TcpConnection{
    int fd = -1; //连接对应的描述符
    std::string request; //已接收的原始请求数据
    std::string response; //准备发送的响应
    std::size_t sentBytes = 0; //响应已经发送了多少字节
    bool requestLineChecked = false; //是否已经完成接收阶段的请求行检查
};

//表示一次接收处理结束后，事件循环应采取的后续操作
enum class ReceiveResult{
    NeedMoreData, //仍需继续接收
    ResponseReady, //响应已经准备好
    CloseConnection //请求调用者关闭连接
};

//处理已完整接收且通过接收阶段长度检查的请求头，返回HTTP响应字符串
//clientFd仅用于日志记录
std::string processHttpRequest(const std::string& requestHead,int clientFd);

//根据已收到的数据构造错误响应；请求行不完整、超过给定预算或解析失败时返回空字符串
std::string buildRequestErrorResponse(const std::string& request,std::size_t maxHeaderBytes,int statusCode,const std::string& reasonPhrase);

//尝试非阻塞接收并更新连接数据，返回后续处理结果；发送及关闭由调用者安排
ReceiveResult receiveRequest(TcpConnection& connection);