#include <iostream>
#include <sys/socket.h>
#include <cstdio>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "thread_pool.h"
#include <exception>
#include <sys/time.h>
#include <cerrno>
#include "logger.h"
#include <system_error>
#include <string>

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

    //socket: Linux提供的系统调用接口，成功返回非负整数，失败返回-1
    //AF_INET: 选择IPv4地址体系
    //SOCK_STREAM: 选择流式通信，此处与IPv4搭配，第三个参数使用0得到TCP socket
    //0：让系统为前两个参数选择默认协议
    //listenFd: 普通变量名，表示准备把它用于监听
    int listenFd = socket(AF_INET,SOCK_STREAM,0);

    //创建socket失败打印错误信息并返回1
    if(listenFd == -1){
        perror("socket");
        return 1;
    }

    std::cout << "socket创建成功,fd = " << listenFd << '\n';

    //sockaddr_in：保存Ipv4地址信息的结构体，这里主要使用三个字段
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET; //sin_family: 地址体系
    //htons(): 把本机表示转换为网络表示
    serverAddr.sin_port = htons(8080); //sin_port: 端口，要求使用网络字节序

    //转换IP地址，使用inet_pton(): 将人能阅读的文本地址转换成网络地址，成功返回1
    //第一个参数指定IPv4，第二个是文本IP，第三个是输出位置
    int result = inet_pton(AF_INET,"127.0.0.1",&serverAddr.sin_addr); //sin_addr: IPv4地址，要求使用网络表示

    //两种错误返回，为0时表示文本不是合法的IP地址
    if(result == 0){
        close(listenFd);
        std::cerr << "文本不是合法的 IP 地址" << '\n';
        return 1;
    }
    //为-1时表示地址体系不受支持等错误
    if(result == -1){
        perror("inet_pton");
        close(listenFd);
        return 1;
    }

    int reuse = 1; //提供一个整数开关，1表示开启
    //按地址复用规则，允许重新使用本地址
    int reuseResult = setsockopt(
        listenFd,
        SOL_SOCKET,
        SO_REUSEADDR, //设置为地址复用
        &reuse,
        sizeof(reuse)
    );

    if(reuseResult == -1){
        perror("setsockopt SO_REUSEADDR");
        close(listenFd);
        return 1;
    }

    //调用bind()：给socket指定本地IP地址和端口。
    //bind(): 通用网络接口，接收sockaddr*,实际使用来专门保存IPv4地址的sockaddr_in
    int bindResult = bind(
        listenFd, //要绑定的socket编号
        //reinterpret_cast：把地址指针转换成接口要求的类型
        reinterpret_cast<const sockaddr*>(&serverAddr), //地址信息存放位置(位置里已包含IP和端口)
        sizeof(serverAddr) //地址信息占的字节数
    );

    if(bindResult == -1){
        perror("bind");
        close(listenFd);
        return 1;
    }

    std::cout << "绑定成功" << '\n';

    //让 socket 进入监听状态，设置等待程序通过 accept() 取走的连接队列长度
    //即在accept()前有人连接进来就先进入等待队列等待
    int listenResult = listen(listenFd,5); //5为等待队列长度，但不等于只能有五个人来连接

    if(listenResult == -1){
        perror("listen");
        close(listenFd);
        return 1;
    }

    std::cout << "开始监听,等待客户端连接" << std::endl;

    LOGGER_INFO("服务器开始监听 127.0.0.1:8080");

    //主线程持续接收连接，将客户端收发任务交给线程池
    while(true){
        //存放客户端地址的空间
        sockaddr_in clientAddr{};
        //socklen_t: socket API专门用于表示地址长度的整数类型
        socklen_t clientAddrLen = sizeof(clientAddr); //clientAddr: 保存连接进来的客户端的IP和端口

        //从listenFd接收一个客户端连接，把客户端地址写进clientAddr，用于通信的编号返回给clientFd
        int clientFd = accept( //一次accept只能取出一个客户端连接
            listenFd,
            reinterpret_cast<sockaddr*>(&clientAddr), //不用const因为accept要修改这块内存
            &clientAddrLen //地址长度变量的位置：操作系统读取提供的空间大小并写回地址的实际长度
        );

        if(clientFd == -1){
            perror("accept");
            close(listenFd);
            return 1;
        }

        LOGGER_INFO("接收客户端连接,fd=" + std::to_string(clientFd));

        //保存函数handle和参数clientFd，交给线程池安排执行
        try{
            pool.enqueue(handleClient,clientFd);
        }
        catch(const std::exception& e){
            LOGGER_ERROR("提交连接任务失败,关闭连接,fd=" + std::to_string(clientFd)
                         + "，原因=" + e.what());
            close(clientFd);
        }
    }

    //释放socket
    close(listenFd);

    return 0;
}
