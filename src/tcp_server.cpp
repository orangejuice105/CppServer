#include "tcp_server.h"
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <exception>
#include <cerrno>
#include "logger.h"
#include <system_error>
#include <string>
#include "tcp_connection.h"

int runTcpServer(ThreadPool& pool){
    //socket: Linux提供的系统调用接口，成功返回非负整数，失败返回-1
    //AF_INET: 选择IPv4地址体系
    //SOCK_STREAM: 选择流式通信，此处与IPv4搭配，第三个参数使用0得到TCP socket
    //0：让系统为前两个参数选择默认协议
    //listenFd: 普通变量名，表示准备把它用于监听
    int listenFd = socket(AF_INET,SOCK_STREAM,0);

    //创建socket失败打印错误信息并返回1
    if(listenFd == -1){
        const int error = errno;
        const std::error_code ec(error, std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("创建socket失败,errno=" + std::to_string(error) + ",原因=" + reason);
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
        LOGGER_ERROR("服务器IP地址格式无效: 127.0.0.1");
        close(listenFd);
        return 1;
    }
    //为-1时表示地址体系不受支持等错误
    if(result == -1){
        const int error = errno;
        const std::error_code ec(error, std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("转换IP地址失败,fd=" + std::to_string(listenFd)
                     + ",errno=" + std::to_string(error) + ",原因=" + reason);
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
        const int error = errno;
        const std::error_code ec(error, std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("设置地址复用失败,fd=" + std::to_string(listenFd)
                     + ",errno=" + std::to_string(error) + ",原因=" + reason);
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
        const int error = errno;
        const std::error_code ec(error, std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("bind失败,地址=127.0.0.1:8080,fd=" + std::to_string(listenFd)
                     + ",errno=" + std::to_string(error) + ",原因=" + reason);
        close(listenFd);
        return 1;
    }

    std::cout << "绑定成功" << '\n';

    //让 socket 进入监听状态，设置等待程序通过 accept() 取走的连接队列长度
    //即在accept()前有人连接进来就先进入等待队列等待
    int listenResult = listen(listenFd,5); //5为等待队列长度，但不等于只能有五个人来连接

    if(listenResult == -1){
        const int error = errno;
        const std::error_code ec(error, std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("开启监听失败,fd=" + std::to_string(listenFd)
                     + ",errno=" + std::to_string(error) + ",原因=" + reason);
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
            const int error = errno;

            if(error == EINTR){
                continue;
            }

            const std::error_code ec(error, std::generic_category());
            const std::string reason = ec.message();
            LOGGER_ERROR("接收客户端连接失败,监听fd=" + std::to_string(listenFd)
                         + ",errno=" + std::to_string(error) + ",原因=" + reason);
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
