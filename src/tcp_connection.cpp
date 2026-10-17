#include "tcp_connection.h"
#include "logger.h"
#include <cerrno>
#include <iostream>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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
            ssize_t totalSent = 0; //统计收到的数据中已经回显的字节数

            bool sendFailed = false; //标记发送是否失败

            std::cout << received << '\n';
            std::cout.write(buffer,received) << std::endl; //从buffer开始输出指定数量的字节

            //一次发送不保证把要求的字节全部发完，所以可能要多次发送
            while(totalSent < received){
                //回显：把收到的字节原样发回客户端
                ssize_t sent = send(clientFd, //发给这个链接对应的客户端
                    buffer + totalSent, //发送数据的起始位置
                    received - totalSent, //要发送的字节数
                    MSG_NOSIGNAL //发送失败时，避免产生可能终止进程的SIGPIPE信号
                );

                if(sent == -1){
                    const int error = errno; //保存错误编号

                    if(error == EINTR){
                        continue;
                    }

                    if(error == EAGAIN || error == EWOULDBLOCK){ //EAGAIN,EWOULDBLOCK: 系统定义的错误名称
                        LOGGER_WARN("客户端发送超时,关闭连接,fd=" + std::to_string(clientFd));
                    }
                    else{
                        const std::error_code ec(error,std::generic_category()); //std::generic_category: 指定按通用类别解释
                        const std::string reason = ec.message(); //把编号转换成可阅读的文字说明
                        LOGGER_ERROR("发送失败,fd = " + std::to_string(clientFd)
                                    + ",error=" + std::to_string(error) + ",原因="  + reason);
                    }
                    sendFailed = true;
                    break;
                }

                if(sent == 0){
                    LOGGER_ERROR("发送未取得进展,关闭连接,fd=" + std::to_string(clientFd));
                    sendFailed = true;
                    break;
                }

                totalSent += sent;
            }
            if(sendFailed)break;
        }
    }
    LOGGER_INFO("结束处理客户端,fd=" + std::to_string(clientFd));
    close(clientFd);
}
