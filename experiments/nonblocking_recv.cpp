#include <cstdio>
#include <iostream>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <sys/epoll.h>

// 第一步练习：只修改这个函数。
// 成功返回 true；系统调用失败时打印错误并返回 false。
bool setNonblocking(int fd) {
    // TODO 1：用 F_GETFL 读取原来的文件状态标志，并检查失败。

    int flag = fcntl(fd,F_GETFL);
    if(flag == -1){
        std::perror("fcntl F_GETFL");
        return false;
    }

    // TODO 2：保留原标志，加入 O_NONBLOCK，用 F_SETFL 写回，并检查失败。

    int newFlag = flag | O_NONBLOCK;
    int result = fcntl(fd,F_SETFL,newFlag);
    if(result == -1){
        std::perror("fcntl F_SETFL");
        return false;
    }

    // TODO 3：设置成功后返回 true。
    return true;
}

int main() {
    // 本小步只观察状态标志，暂时不 bind、listen 或 recv。
    const int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == -1) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8081);
    int result = inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
    if(result == 0 || result == -1){
        std::perror("inet_pton");
    }
    
    int bindResult = bind(listenFd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr));

    int listenResult = listen(listenFd,5);

    sockaddr_in clientAddr{};
    socklen_t clientAddrLen = sizeof(clientAddr);
    std::cout << "等待客户端连接..." << std::endl;
    int clientFd = accept(listenFd,reinterpret_cast<sockaddr*>(&clientAddr),&clientAddrLen);

    if(!setNonblocking(clientFd)){
        std::perror("setNonblocking");
    }

    int epollFd = epoll_create1(0);
    epoll_event interest{};
    interest.events = EPOLLIN;
    interest.data.fd = clientFd;
    int epollResult = epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &interest);
    if(epollResult == -1){
        std::perror("epoll_ctl");
    }
    char buff[1024];
    epoll_event readyEvents[1]{};
    
    while (true){
        std::cout << "等待 epoll 事件..." << std::endl;

        int readyCount = epoll_wait(epollFd, readyEvents, 1, -1);

        if (readyCount == -1) {
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            std::perror("epoll_wait");
            break;
        }
        std::cout << "收到事件,fd="
          << readyEvents[0].data.fd << '\n';    
        ssize_t n = recv(clientFd, buff, sizeof(buff), 0); 
        
        if (n == -1) {
            const int error = errno;
            if(error == EAGAIN || error == EWOULDBLOCK){
                std::cout << "暂时没有数据" << '\n';
                continue;
            }
            std::perror("recv");
            std::cout << "errno=" << error << '\n';
            break;
        }

        if(n == 0){
            std::cout << "对方正常结束发送，已有数据已读完" << std::endl;
            break;
        }
        if(n > 0){
            
            std::cout.write(buff, n);
            std::cout << '\n';
        }

    }
    close(epollFd);
    close(listenFd);
    close(clientFd);
    return 0;
}
