// Test-only fault injection; descriptors are checked before this process exits.
#include "tcp_server.h"
#include "tcp_connection.h"
#include "logger.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <unordered_map>
#include <string>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cerrno>
#include <cstdlib>

void removeAllConnections(int, std::unordered_map<int, TcpConnection>&);
extern "C" int __real_bind(int, const sockaddr*, socklen_t);
extern "C" int __real_accept(int, sockaddr*, socklen_t*);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event*);
extern "C" int __real_epoll_wait(int, epoll_event*, int, int);
extern "C" int __real_close(int);
namespace {
std::string mode;
std::atomic<unsigned short> port{0};
std::atomic<bool> finished{false};
std::atomic<bool> clientFailure{false};
int listener = -1, poller = -1, shutdownDescriptor = -1;
std::vector<int> registered, deleted, closed;
bool triggered = false, badOrder = false, deleteFault = false, closeFault = false;
bool listenerRemoved = false, resumeFault = false;
int closeAttempts = 0;
bool tracked(int fd) { return std::find(registered.begin(), registered.end(), fd) != registered.end(); }
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool isClosed(int fd) { errno = 0; return fcntl(fd, F_GETFD) == -1 && errno == EBADF; }
void clients() {
    std::vector<int> sockets;
    while (!finished.load() && port.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (int i = 0; i < 3 && !finished.load(); ++i) {
        bool connected = false;
        for (int attempt = 0; attempt < 2000 && !finished.load(); ++attempt) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { clientFailure = true; break; }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port.load());
            if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                sockets.push_back(fd); connected = true; break;
            }
            __real_close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!connected && !finished.load()) clientFailure = true;
    }
    while (!finished.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (int fd : sockets) __real_close(fd);
}
}
extern "C" int __wrap_bind(int fd, const sockaddr* address, socklen_t length) {
    if (address && address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        sockaddr_in local = *reinterpret_cast<const sockaddr_in*>(address);
        local.sin_port = 0;
        int result = __real_bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        if (result == 0) {
            listener = fd;
            socklen_t size = sizeof(local);
            if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &size) == 0) port = ntohs(local.sin_port);
        }
        return result;
    }
    return __real_bind(fd, address, length);
}
extern "C" int __wrap_epoll_ctl(int epfd, int op, int fd, epoll_event* event) {
    poller = epfd;
    if (fd == listener && op == EPOLL_CTL_DEL && mode == "pause-del-failure" && triggered) {
        deleteFault = true; errno = EIO; return -1;
    }
    if (fd == listener && op == EPOLL_CTL_ADD && mode == "resume-add-failure" && listenerRemoved) {
        resumeFault = true; errno = EIO; return -1;
    }
    if (op == EPOLL_CTL_DEL && tracked(fd)) {
        if (isClosed(listener) || isClosed(epfd)) badOrder = true;
        deleted.push_back(fd);
        if (mode == "delete-failure" && !deleteFault) {
            deleteFault = true; errno = EIO; return -1;
        }
    }
    int result = __real_epoll_ctl(epfd, op, fd, event);
    if (result == 0 && op == EPOLL_CTL_DEL && fd == listener) listenerRemoved = true;
    if (result == 0 && op == EPOLL_CTL_ADD && fd != listener && fd != shutdownDescriptor)
        registered.push_back(fd);
    return result;
}
extern "C" int __wrap_accept(int fd, sockaddr* address, socklen_t* length) {
    if ((mode == "accept-fatal" || mode == "pause-del-failure" || mode == "resume-add-failure")
            && registered.size() == 3 && !triggered) {
        // EMFILE now exercises backoff/recovery; retain a genuinely fatal category here.
        triggered = true; errno = mode == "accept-fatal" ? EBADF : EMFILE; return -1;
    }
    return __real_accept(fd, address, length);
}
extern "C" int __wrap_epoll_wait(int fd, epoll_event* events, int maxEvents, int timeout) {
    if (mode != "accept-fatal" && mode != "pause-del-failure" && mode != "resume-add-failure"
            && registered.size() == 3 && !triggered) {
        triggered = true;
        if (mode == "listener-error") {
            events[0] = epoll_event{}; events[0].data.fd = listener; events[0].events = EPOLLERR;
            return 1;
        }
        errno = EIO; return -1;
    }
    return __real_epoll_wait(fd, events, maxEvents, timeout);
}
extern "C" int __wrap_close(int fd) {
    if (tracked(fd)) {
        ++closeAttempts;
        if (isClosed(listener) || isClosed(poller)) badOrder = true;
        int result = __real_close(fd);
        closed.push_back(fd);
        if (mode == "close-eintr" && !closeFault) {
            closeFault = true; errno = EINTR; return -1;
        }
        return result;
    }
    if (fd == listener || fd == poller) {
        for (int client : registered) if (!isClosed(client)) badOrder = true;
    }
    return __real_close(fd);
}
int main(int argc, char** argv) {
    alarm(12);
    if (argc != 3) return 2;
    mode = argv[1];
    // Match production startup: block signals before either background thread exists.
    sigset_t signals;
    if (!prepareShutdownSignals(signals)) return 4;
    LoggerConfig config; config.filePath = argv[2];
    if (!Logger::getInstance().init(config)) return 3;
    shutdownDescriptor = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (shutdownDescriptor < 0) return 5;
    try {
        if (mode == "empty") {
            int epfd = epoll_create1(0);
            require(epfd >= 0, "epoll creation failed");
            std::unordered_map<int, TcpConnection> connections;
            removeAllConnections(epfd, connections);
            removeAllConnections(epfd, connections);
            require(connections.empty(), "empty map changed");
            require(fcntl(epfd, F_GETFD) >= 0, "helper closed epoll");
            __real_close(epfd);
        } else {
            std::thread worker(clients);
            int result = runTcpServer(shutdownDescriptor);
            finished = true;
            worker.join();
            require(!clientFailure.load(), "client connection setup failed");
            require(triggered, "fatal branch was not triggered (possible sandbox socket restriction)");
            require(result == 1, "fatal path did not return failure");
            require(registered.size() == 3, "did not register three real TCP clients");
            require(deleted.size() == 3 && closed.size() == 3 && closeAttempts == 3, "client cleanup skipped or duplicated");
            require(!badOrder, "epoll/listener closed before clients were released");
            for (int fd : registered) require(isClosed(fd), "client fd remained open after runTcpServer returned");
            require(isClosed(listener), "listener leaked");
            require(isClosed(poller), "epoll leaked");
            if (mode == "delete-failure") require(deleteFault, "DEL fault not injected");
            if (mode == "pause-del-failure") require(deleteFault, "listener DEL fault not injected");
            if (mode == "resume-add-failure") require(resumeFault, "listener resume ADD fault not injected");
            if (mode == "close-eintr") require(closeFault, "close EINTR not injected");
        }
        require(!isClosed(shutdownDescriptor), "server closed the caller-owned signal fd");
        require(__real_close(shutdownDescriptor) == 0, "signal fd cleanup failed");
        require(isClosed(shutdownDescriptor), "signal fd remained open");
        std::cout << "PASS " << mode << " clients=" << registered.size() << " returned_without_open_fds=true\n";
        return 0;
    } catch (const std::exception& error) {
        finished = true;
        __real_close(shutdownDescriptor);
        std::cerr << "FAIL " << mode << ": " << error.what() << '\n';
        return 1;
    }
}
