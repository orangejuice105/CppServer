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
#include <sys/epoll.h>
#include <fcntl.h>
#include <unordered_map>
#include <chrono>
#include <cstddef>
#include <sys/signalfd.h>

bool prepareShutdownSignals(sigset_t& signals){
    //准备空集合，sigemptyset: 将集合设置为空
    int setEmptyResult = sigemptyset(&signals);

    if(setEmptyResult == -1){
        //该函数在日志库初始化前调用，所以直接返回
        return false;
    }

    //把交互式中断和服务终止信号加入集合
    int addResult = sigaddset(&signals,SIGINT); //SIGINT: 终端产生的中断请求
    if(addResult == -1){ 
        return false;
    }

    addResult = sigaddset(&signals,SIGTERM); //SIGTERM: 通常表示kill-TERM或服务管理器发出的终止请求
    if(addResult == -1){
        return false;
    }

    //两次sigaddset()成功后: signals = {SIGINT,SIGTERM}

    //将两个信号加入信号掩码中，pthread_sigmask(): 操作当前调用线程的信号掩码
    int maskResult = pthread_sigmask(SIG_BLOCK,&signals,nullptr);
    //失败时返回错误编号而不是-1
    if(maskResult != 0){
        errno = maskResult; //把错误编号放入errno，让调用者统一通过errno获取失败原因
        return false;
    }

    return true;
}

//保留原有文件状态标志并开启非阻塞；失败时记录日志
bool setNonblocking(int fd){
    //读取当前文件状态标志
    int flags = fcntl(fd,F_GETFL);

    if(flags == -1){
        const int error = errno;
        const std::error_code ec(error,std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("读取文件状态标志失败,fd=" + std::to_string(fd) + ",errno=" + std::to_string(error) + ",原因=" + reason);
        return false;
    }

    //开启非阻塞
    int newFlags = flags | O_NONBLOCK; //保留原有标志并把非阻塞对应的位设为开启
    int setResult = fcntl(fd,F_SETFL,newFlags); //F_SETFL表示把新的状态标志写回

    if(setResult == -1){
        const int error = errno;
        const std::error_code ec(error,std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("设置非阻塞失败,fd=" + std::to_string(fd) + ",errno=" + std::to_string(error) + ",原因=" + reason);
        return false;
    }

    return true;
}

//最终释放已纳入连接容器管理的客户端；取消epoll登记，关闭描述符并删除连接对象
void removeConnection(int epollFd,std::unordered_map<int,TcpConnection>& connections,int clientFd){
    //先在容器中找到连接
    auto it = connections.find(clientFd);
    //没找到就返回,说明容器目前没有管理这个连接，本函数不继续释放它
    if(it == connections.end()) {
        return;
    }

    //找到连接后，调用epoll_ctl取消该连接的epoll登记
    int removeResult = epoll_ctl(epollFd,EPOLL_CTL_DEL,clientFd,nullptr);

    //这里不因为删除登记失败就提前返回，否则可能留下描述符和连接对象
    if(removeResult == -1){
        const int error = errno;
        LOGGER_WARN("取消epoll登记失败,fd=" + std::to_string(clientFd) + ",errno=" + std::to_string(error));
    }

    //关闭客户端描述符
    int closeResult = close(clientFd);

    if(closeResult == -1){
        const int error = errno;
        LOGGER_WARN("关闭客户端描述符失败,fd=" + std::to_string(clientFd) + ",errno=" + std::to_string(error));
    }

    //删除连接对象
    connections.erase(it);
}

//释放容器管理的全部客户端连接；调用时epoll描述符应仍然有效
void removeAllConnections(int epollFd,std::unordered_map<int,TcpConnection>& connections){
    while(!connections.empty()){
        int clientFd = connections.begin()->first;

        removeConnection(epollFd, connections, clientFd);
    }
}

//根据连接阶段修改epoll关注事件；失败时记录日志；由调用者安排释放连接
bool updateConnectionEvents(int epollFd,const TcpConnection& connection){
    //准备事件结构体
    epoll_event clientEvent{};
    clientEvent.data.fd = connection.fd;

    //根据阶段填写关注事件
    if(connection.phase == ConnectionPhase::Sending){
        clientEvent.events = EPOLLOUT;
    }
    else{
        clientEvent.events = EPOLLIN;
    }

    //调用epoll_ctl()修改登记
    int modifyResult = epoll_ctl(epollFd,EPOLL_CTL_MOD,connection.fd,&clientEvent);

    //处理调用结果
    if(modifyResult == -1){
        const int error = errno;
        LOGGER_ERROR("修改客户端epoll关注事件失败,fd=" + std::to_string(connection.fd) +  ",epollFd="  + std::to_string(epollFd) + ",errno=" + std::to_string(error));
        return false;
    }
    return true;
}

//根据客户端事件和连接阶段推进收发和清理，统一安排连接释放
void handleConnectionEvent(int epollFd,std::unordered_map<int,TcpConnection>& connections,const epoll_event& readyEvent){
    //根据事件中的fd找到连接
    int clientFd = readyEvent.data.fd;
    auto it = connections.find(clientFd);

    if(it == connections.end()){
        return;
    }

    //找到后再取得连接对象的引用
    TcpConnection& connection = it->second;

    //先处理EPOLLERR，EPOLLERR是代表"错误事件"的标志，使用&来检查这个标志是否存在
    if(readyEvent.events & EPOLLERR){
        LOGGER_WARN("客户端出现epoll错误事件,fd=" + std::to_string(clientFd));
        removeConnection(epollFd,connections,clientFd);
        return;
    }

    //建立三个阶段分支
    //接收请求
    if(connection.phase == ConnectionPhase::Receiving){
        //先检查这次事件是否允许尝试接收
        //既没有可读事件也没有挂断事件，就结束本次处理
        if(!(readyEvent.events & (EPOLLIN | EPOLLHUP))){
            return;
        }

        //调用接收函数，更新这个连接的请求数据，必要时准备响应
        ReceiveResult receiveResult = receiveRequest(connection);

        //根据接收结果分别处理
        if(receiveResult == ReceiveResult::NeedMoreData){
            return;
        }
        if(receiveResult == ReceiveResult::CloseConnection){
            removeConnection(epollFd,connections,clientFd);
            return;
        }
        //最后结果剩下ResponseReady，切换阶段
        connection.phase = ConnectionPhase::Sending;
        //进入发送阶段，初始化等待发送进展的截止时间
        connection.sendIdleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        //然后更新关注事件
        if(!updateConnectionEvents(epollFd,connection)){ //这里！表示修改失败
            removeConnection(epollFd,connections,clientFd);
            return;
        }
    }
    //发送响应
    else if(connection.phase == ConnectionPhase::Sending){
        //先事件检查,既没有可写通知也没有挂断通知，就不在这里继续尝试
        if(!(readyEvent.events & (EPOLLOUT | EPOLLHUP))){
            return;
        }

        SendResult sendResult = sendResponse(connection);

        if(sendResult == SendResult::CloseConnection){
            removeConnection(epollFd,connections,clientFd);
            return;
        }
        if(sendResult == SendResult::NeedWrite){
            //判断是否包含挂断事件
            if(readyEvent.events & EPOLLHUP){
                LOGGER_WARN("客户端挂断,停止继续发送,fd=" + std::to_string(connection.fd));
                removeConnection(epollFd,connections,clientFd);
                return;
            }
            return;
        }
        //最后剩下结果SendComplete，表示响应已经全部交给内核发送，可以调用beginDrain(connection)
        if(!beginDrain(connection)){
            removeConnection(epollFd, connections, clientFd);
            return;
        }
        if (!updateConnectionEvents(epollFd, connection)) {
            removeConnection(epollFd, connections, clientFd);
            return;
        }
        return;
    }
    //清理剩余数据
    else if(connection.phase == ConnectionPhase::Draining){
        //先事件检查
        if (!(readyEvent.events & (EPOLLIN | EPOLLHUP))) {
            return;
        }

        DrainResult drainResult = drainConnection(connection);

        if(drainResult == DrainResult::CloseConnection){
            removeConnection(epollFd, connections, clientFd);
            return;
        }
    }
}

//判断 accept() 的错误是否属于后续可以重试的连接或网络错误
/*ECONNABORTED	连接被中止
ENETDOWN	网络不可用
EPROTO	协议错误
ENOPROTOOPT	协议选项不可用
EHOSTDOWN	主机不可用
ENONET	主机没有连接到网络
EHOSTUNREACH	主机不可达
EOPNOTSUPP	操作不受支持
ENETUNREACH	网络不可达*/
bool isRetryableAcceptError(int error){
    return error == ECONNABORTED || error == ENETDOWN || error == EPROTO ||
            error == ENOPROTOOPT || error == EHOSTDOWN || error == ENONET ||
            error == EHOSTUNREACH || error == EOPNOTSUPP || error == ENETUNREACH;
}

//判断accept()是否因文件描述符或系统打开文件资源耗尽而失败
/*EMFILE	当前进程的文件描述符数量已达到上限
ENFILE	整个系统的打开文件资源已达到上限*/
bool isAcceptResourceError(int error){
    return error == EMFILE || error == ENFILE;
}

int runTcpServer(int shutdownFd){
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

    //调用非阻塞设置函数
    if(!setNonblocking(listenFd)){
        close(listenFd);
        return 1;
    }

    //创建epoll、登记监听socket
    int epollFd = epoll_create1(0); //0表示不启用额外选项

    if(epollFd == -1){
        const int error = errno;
        const std::error_code ec(error,std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("创建epoll失败,errno=" + std::to_string(error) + ",原因=" + reason);
        close(listenFd);
        return 1;
    }

    //epoll登记用结构体
    epoll_event listenEvent{};
    listenEvent.events = EPOLLIN; //表示关心这个socket的可读事件
    listenEvent.data.fd = listenFd; //将来报告事件时把填写的listenFd一起带回来，不负责登记socket

    //调用epoll_ctl完成登记
    //把 listenFd 加入 epollFd 管理的关注列表，关注它的可读事件，并保存我们提供的附带信息
    int addResult = epoll_ctl(epollFd,EPOLL_CTL_ADD,listenFd,&listenEvent);

    if(addResult == -1){
        const int error = errno;
        const std::error_code ec(error,std::generic_category());
        const std::string reason = ec.message();
        LOGGER_ERROR("登记监听socket到epoll失败,监听fd=" + std::to_string(listenFd)
                    + ",epollFd=" + std::to_string(epollFd)
                    + ",errno=" + std::to_string(error) + ",原因=" + reason);
        close(epollFd);
        close(listenFd);
        return 1;
    }

    //shutdownFd登记
    epoll_event shutdownEvent{};
    shutdownEvent.events = EPOLLIN;
    shutdownEvent.data.fd = shutdownFd;

    int shutdownAddResult = epoll_ctl(epollFd, EPOLL_CTL_ADD, shutdownFd, &shutdownEvent);

    if (shutdownAddResult == -1) {
        const int error = errno;
        const std::error_code ec(error, std::generic_category());
        LOGGER_ERROR(
            "登记退出信号fd到epoll失败,fd="
            + std::to_string(shutdownFd)
            + ",epollFd="
            + std::to_string(epollFd)
            + ",errno="
            + std::to_string(error)
            + ",原因="
            + ec.message()
        );

        close(epollFd);
        close(listenFd);
        return 1;
    }

    std::cout << "开始监听,等待客户端连接" << std::endl;

    LOGGER_INFO("服务器开始监听 127.0.0.1:8080");

    //创建数组保存epoll_wait()返回的事件
    const int maxEvents = 16; //设置该次调用最多取回16条就绪记录
    const int maxAcceptPerEvent = 32; //处理一次监听事件时，最多接收的连接个数
    //constexpr: 表示这个值在编译时确定，之后不能修改
    constexpr std::size_t maxConnections = 1024; //应用层最多同时保存的连接对象数，覆盖接收、发送和清理阶段
    epoll_event readyEvents[maxEvents]{};
    const int epollWaits = 100;

    //按fd保存连接对象的容器,使收发数据和进度能跨越多次事件处理保留
    std::unordered_map<int,TcpConnection>connections;
    //增加拒绝计数，统计因为应用层连接容量已满而被拒绝的连接
    std::size_t capacityRejectedConnectionCount = 0;

    bool stopping = false; //整个服务器的状态，false为正常运行，true为收到退出请求，只继续处理已有连接
    std::chrono::steady_clock::time_point shutdownDeadline{}; //保存退出的截止时间
    bool acceptPaused = false; //是否因资源不足而暂停接收新连接
    std::chrono::steady_clock::time_point acceptRetryDeadline{}; //暂停接收后的重试时间

    //主线程通过epoll接收新连接，并推进客户端收发和清理
    while(true){
        //等待事件,并记录有效事件个数
        int readyCount = epoll_wait(epollFd,readyEvents,maxEvents,epollWaits); //epollWaits是等待时间参数,单位是毫秒

        if(readyCount == -1){
            const int error = errno;
            const std::error_code ec(error, std::generic_category());
            const std::string reason = ec.message();
            if(error == EINTR){
                readyCount = 0;
            }
            else{
                LOGGER_ERROR("等待epoll事件失败,fd=" + std::to_string(epollFd) + ",errno=" + std::to_string(error) + ",原因=" + reason);
                removeAllConnections(epollFd, connections);
                close(listenFd);
                close(epollFd);
                return 1;
            }
        }
        for(int i = 0;i < readyCount;i++){
            //取出这次事件附带的描述符
            int readyFd = readyEvents[i].data.fd;

            //先判断是否收到了退出信号
            if(readyFd == shutdownFd){
                //读取信号信息
                signalfd_siginfo signalInfo{}; //准备接收信号信息的对象
                ssize_t readResult = read(shutdownFd,&signalInfo,sizeof(signalInfo));

                if(readResult == -1){
                    const int error = errno;
                    if(error == EINTR){
                        continue;
                    }

                    else if(error == EAGAIN || error == EWOULDBLOCK){
                        LOGGER_WARN("通知与读取之间状态发生变化,暂时没有数据,fd=" 
                                    + std::to_string(shutdownFd) + ",errno=" + std::to_string(error));
                        continue;
                    }
                    else{
                        LOGGER_ERROR("读取退出信号失败,fd=" + std::to_string(shutdownFd) + ",errno=" + std::to_string(error));
                        removeAllConnections(epollFd,connections);
                        close(listenFd);
                        close(epollFd);
                        return 1;
                    }
                }

                //判断本次读到的字节数是否刚好足够填写一个完整的signalInfo
                //signalfd按完整的信号记录提供数据，只有读取到一个完整记录，才进入正常退出路径
                if(readResult != static_cast<ssize_t>(sizeof(signalInfo))){
                    LOGGER_ERROR("退出信号读取长度异常,实际="
                                + std::to_string(readResult)
                                + ",预期=" + std::to_string(sizeof(signalInfo)));
                    removeAllConnections(epollFd,connections);
                    close(listenFd);
                    close(epollFd);
                    return 1;
                }

                ////完整读取信号后，记录本次退出原因
                LOGGER_INFO("收到服务器退出请求,signal=" + std::to_string(signalInfo.ssi_signo)); //signalInfo.ssi_signo: 这次读到的信号编号

                //第一次收到退出请求时才进行状态切换
                if(!stopping){
                    if(!acceptPaused){
                        //取消epoll登记的监听fd
                        int removeListenResult = epoll_ctl(epollFd,EPOLL_CTL_DEL,listenFd,nullptr);

                        if(removeListenResult == -1){
                            const int error = errno;
                            LOGGER_ERROR("取消监听socket的epoll登记失败,errno=" + std::to_string(error));
                            removeAllConnections(epollFd,connections);
                            close(listenFd);
                            close(epollFd);
                            return 1;
                        }                        
                    }
                    //监听登记已取消，进入退出状态
                    stopping = true;

                    //设置退出截止时间为5秒后
                    shutdownDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);    
                }
                //结束本次信号事件处理，让事件循环继续处理其他客户
                continue;
            }

            if(readyFd != listenFd){
                //进行客户端事件分发
                handleConnectionEvent(epollFd,connections,readyEvents[i]);
                continue;
            }

            //服务器正在停止或暂停接收时，跳过这条监听事件，不再调用accept
            if(stopping || acceptPaused){
                continue;
            }

            //检查错误或挂断
            if(readyEvents[i].events & (EPOLLERR | EPOLLHUP)){
                LOGGER_ERROR("监听socket出现错误或挂断");
                removeAllConnections(epollFd, connections);
                close(epollFd);
                close(listenFd);
                return 1;
            }

            //检查是否包含可读事件
            if(!(readyEvents[i].events & EPOLLIN)){ //检查事件标志中有没有开启EPOLLIN对应的位
                continue;
            }

            //本次监听事件中，当已经成功接收的连接个数小于上限时，持续接收
            int acceptedCount = 0;
            while(acceptedCount < maxAcceptPerEvent){
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

                    //当前没有连接可接收，结束本轮接收循环
                    if(error == EAGAIN || error == EWOULDBLOCK){
                        break;
                    }

                    //新连接上的可恢复网络错误：结束本轮接收，交还事件循环，后续继续尝试
                    if(isRetryableAcceptError(error)){
                        LOGGER_WARN("接收新连接遇到可恢复网络错误,监听fd="
                                    + std::to_string(listenFd) + ",errno=" + std::to_string(error));
                        break;
                    }

                    //文件描述符或系统打开文件资源不足，暂停接收新连接
                    if(isAcceptResourceError(error)){
                        //取消监听fd的epoll登记
                        const int pauseResult = epoll_ctl(epollFd,EPOLL_CTL_DEL,listenFd,nullptr);

                        if(pauseResult == -1){
                            const int pauseError = errno;
                            LOGGER_ERROR("暂停接收新连接失败,监听fd=" + std::to_string(listenFd)
                                        + ",errno=" + std::to_string(pauseError));
                            
                            removeAllConnections(epollFd, connections);
                            close(epollFd);
                            close(listenFd);
                            return 1;
                        }

                        //取消登记成功后，记录暂停状态并设置重试时间
                        acceptPaused = true;
                        acceptRetryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

                        LOGGER_WARN("接收新连接资源不足，暂停接收,监听fd="
                                    + std::to_string(listenFd)
                                    + ",errno=" + std::to_string(error));

                        break;
                    }

                    const std::error_code ec(error, std::generic_category());
                    const std::string reason = ec.message();
                    LOGGER_ERROR("接收客户端连接失败,监听fd=" + std::to_string(listenFd)
                                + ",errno=" + std::to_string(error) + ",原因=" + reason);
                    removeAllConnections(epollFd, connections);
                    close(epollFd);
                    close(listenFd);
                    return 1;
                }

                //成功接收后，计数加一
                acceptedCount++;

                //检查现有连接是否已达到容量上限，满额时不再接收新连接进入管理流程
                //若连接已达到上限：关闭尚未登记和保存的fd，避免继续占用资源
                if(connections.size() >= maxConnections){
                    capacityRejectedConnectionCount++;
                    
                    //限频记录日志，只在第一次以及累计次数是100的整数倍时记录日志
                    if(capacityRejectedConnectionCount == 1 || capacityRejectedConnectionCount % 100 == 0){
                        LOGGER_WARN("连接数达到应用层上限,current=" + std::to_string(maxConnections) + ",rejected = " + std::to_string(capacityRejectedConnectionCount) + ",fd = " + std::to_string(clientFd));
                    }

                    close(clientFd);

                    continue;
                }

                LOGGER_INFO("接收客户端连接,fd=" + std::to_string(clientFd));

                //客户端初始化
                //先设置客户端非阻塞
                if(!setNonblocking(clientFd)){
                    close(clientFd);
                    continue;
                }
                //再把客户端登记到epoll
                epoll_event clientEvent{};
                clientEvent.events = EPOLLIN;
                clientEvent.data.fd = clientFd;

                int clientAddResult = epoll_ctl(epollFd,EPOLL_CTL_ADD,clientFd,&clientEvent);
                if(clientAddResult == -1){
                    const int error = errno;
                    LOGGER_WARN("登记客户端到epoll失败,fd=" + std::to_string(clientFd) + ",errno=" + std::to_string(error));
                    close(clientFd);
                    continue;
                }
                //把客户端状态存入容器，供后续epoll事件找到并继续处理
                try{
                    TcpConnection connection;
                    connection.fd = clientFd;

                    //时间初始化
                    const auto now = std::chrono::steady_clock::now();
                    connection.receiveIdleDeadline = now + std::chrono::seconds(30);
                    connection.requestDeadline = now + std::chrono::seconds(60);

                    auto insertResult = connections.emplace(clientFd,connection);

                    if(!insertResult.second){
                        LOGGER_WARN("连接容器出现重复fd");
                        removeConnection(epollFd,connections,clientFd);
                        continue;
                    }
                }
                catch(const std::exception& e){
                    LOGGER_ERROR("保存客户端连接失败,fd=" + std::to_string(clientFd) + ",原因=" + e.what());
                    //取消epoll登记
                    int removeResult = epoll_ctl(epollFd,EPOLL_CTL_DEL,clientFd,nullptr);
                    if(removeResult == -1){
                        const int error = errno;
                        LOGGER_WARN("取消客户端epoll登记失败,fd=" + std::to_string(clientFd)
                                    + ",errno=" + std::to_string(error));
                    }

                    close(clientFd);
                    continue;
                }
            }
        }
        //定期检查接收、发送及清理期限，处理没有新事件的连接
        //先取得扫描时的当前时间
        const auto now = std::chrono::steady_clock::now();
        //接着准备从容器第一个元素开始遍历
        auto it = connections.begin();
        while(it != connections.end()){
            //每轮检查一个连接，先取得当前连接的编号和状态
            int clientFd = it->first;
            TcpConnection& connection = it->second;

            //判断是否应该结束清理(处于Draining并且总期限和等待期限至少一个到达时)
            bool expired = connection.phase == ConnectionPhase::Draining && (now >= connection.drainDeadline
                                                    || now >= connection.drainIdleDeadline);
            //removeConnection()：它会取消登记、关闭 fd，并删除容器里的对象，所以先执行++it让迭代器指向下一个元素
            ++it;

            //定期检查仍在接收请求的连接，超时后决定释放连接，或者安排发送 408。这样客户端一直不发数据，也能被处理。
            if (connection.phase == ConnectionPhase::Receiving) {
                ReceiveResult timeoutResult = checkReceiveTimeout(connection);

                if(timeoutResult == ReceiveResult::CloseConnection) {
                    removeConnection(epollFd, connections, clientFd);
                    continue;
                }

                if(timeoutResult == ReceiveResult::ResponseReady){
                    connection.phase = ConnectionPhase::Sending;
                    connection.sendIdleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                    if (!updateConnectionEvents(epollFd, connection)) {
                        removeConnection(epollFd, connections, clientFd);
                        continue;
                    }
                }

                continue;
            }

            //对处于Sending阶段的连接进行定期扫描
            if(connection.phase == ConnectionPhase::Sending && now >= connection.sendIdleDeadline){
                LOGGER_WARN("发送等待超时,fd=" + std::to_string(clientFd));
                removeConnection(epollFd,connections,clientFd);
                continue;
            }

            if(expired){
                LOGGER_INFO("清理期限已到，释放连接,fd=" + std::to_string(clientFd));
                removeConnection(epollFd,connections,clientFd);
            }
        }
        //暂停接收期间，达到重试时间且服务器仍在运行时，恢复监听登记
        if(acceptPaused && !stopping && now >= acceptRetryDeadline){
            //重新登记监听fd
            const int resumeResult = epoll_ctl(epollFd,EPOLL_CTL_ADD,listenFd,&listenEvent);

            if(resumeResult == -1){
                const int resumeError = errno;
                LOGGER_ERROR("恢复监听socket的epoll登记失败,监听fd="
                            + std::to_string(listenFd)
                            + ",errno=" + std::to_string(resumeError));

                removeAllConnections(epollFd, connections);
                close(epollFd);
                close(listenFd);
                return 1;
            }

            //登记成功后，解除暂停
            acceptPaused = false;
            LOGGER_INFO("恢复监听登记，重新尝试接收新连接,监听fd="
                        + std::to_string(listenFd));
        }
        
        //已经给过这些客户端等待时间，但它们仍未结束时主动关闭
        if(stopping && !connections.empty() && std::chrono::steady_clock::now() >= shutdownDeadline){
            LOGGER_WARN("优雅退出等待超时，关闭剩余连接,remaining="
                        + std::to_string(connections.size()));
            removeAllConnections(epollFd, connections);
        }

        //已有连接全部结束时，服务器才返回
        if(stopping && connections.empty()){
            close(listenFd);
            close(epollFd);
            return 0;
        }
    }

}
