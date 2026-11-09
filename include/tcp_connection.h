#pragma once
#include <string>
#include <cstddef>
#include <chrono>

//表示连接当前所处的处理阶段
enum class ConnectionPhase{
    Receiving, //正在接收请求
    Sending, //正在发送响应
    Draining //响应已经发送完，正在清理剩余接收数据
};

//保存单个连接的收发数据与处理进度，供多次事件处理继续使用
struct TcpConnection{
    int fd = -1; //连接对应的描述符
    ConnectionPhase phase = ConnectionPhase::Receiving; //默认阶段为Receiving
    std::string request; //已接收的原始请求数据
    std::string response; //准备发送的响应
    std::size_t sentBytes = 0; //响应已经发送了多少字节
    bool requestLineChecked = false; //是否已经完成接收阶段的请求行检查
    std::chrono::steady_clock::time_point receiveIdleDeadline{}; //接收阶段等待下一批数据的截止时间，成功收到数据后延后30秒
    std::chrono::steady_clock::time_point requestDeadline{}; //请求头接收的总截止时间，初始化为当前时间加60秒，之后不延长
    std::chrono::steady_clock::time_point sendIdleDeadline{}; //发送阶段等待发送进展的截止时间
    std::size_t drainedBytes = 0; //清理阶段已经读取并丢弃的字节数，用于限制最多清理 64 KiB
    std::chrono::steady_clock::time_point drainDeadline{}; //整个清理阶段的截止时间，用于限制总时长 1 秒
    std::chrono::steady_clock::time_point drainIdleDeadline{}; //清理阶段等待下一批数据的截止时间，用于保留原来 100 毫秒的接收等待限制
};

//表示一次接收处理结束后，事件循环应采取的后续操作
enum class ReceiveResult{
    NeedMoreData, //仍需继续接收
    ResponseReady, //响应已经准备好
    CloseConnection //请求调用者关闭连接
};

//发送结果类型
enum class SendResult{
    NeedWrite, //响应还没发完,后续继续发送
    SendComplete, //整个响应已经交给内核发送
    CloseConnection //发送失败或等待超时,请调用者关闭连接
};

//表示一次清理处理结束后，调用者应采取的后续操作
enum class DrainResult{
    NeedMoreData, //清理尚未结束，后续继续调度
    CloseConnection //清理结束或者无法继续，安排释放连接
};

//尝试非阻塞发送响应并更新进度，返回后续处理结果；关闭由调用者安排
SendResult sendResponse(TcpConnection& connection);

//结束发送方向并初始化清理阶段；失败时由调用者安排释放连接
bool beginDrain(TcpConnection& connection);

//尝试非阻塞读取并丢弃剩余数据，更新清理进度；关闭由调用者安排
DrainResult drainConnection(TcpConnection& connection);

//处理已完整接收且通过接收阶段长度检查的请求头，返回HTTP响应字符串
//clientFd仅用于日志记录
std::string processHttpRequest(const std::string& requestHead,int clientFd);

//根据已收到的数据构造错误响应；请求行不完整、超过给定预算或解析失败时返回空字符串
std::string buildRequestErrorResponse(const std::string& request,std::size_t maxHeaderBytes,int statusCode,const std::string& reasonPhrase);

//尝试非阻塞接收并更新连接数据，返回后续处理结果；发送及关闭由调用者安排
ReceiveResult receiveRequest(TcpConnection& connection);

//检查Receiving阶段的期限；到期时准备408响应或请求关闭，实际发送和关闭由调用者安排
ReceiveResult checkReceiveTimeout(TcpConnection& connection);
