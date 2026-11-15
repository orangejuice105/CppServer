// 仅供测试：通过 LD_PRELOAD 注入返回值、限制发送大小或映射临时端口。
// 不参与服务器的正常构建；未设置相应环境变量时调用原始系统接口。
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

namespace {
int signalDescriptor = -1;
std::atomic<unsigned long long> acceptedCount{0};
void saveAcceptedCount() {
    const char* path = std::getenv("CPP_EPOLL_TEST_ACCEPT_COUNT");
    if (!path) return;
    char buffer[64];
    const int size = std::snprintf(buffer, sizeof(buffer), "%llu\n", acceptedCount.load());
    const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd >= 0) { (void)write(fd, buffer, static_cast<size_t>(size)); close(fd); }
}
bool enabled(const char* name) {
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

void trace(const char* message) {
    const int saved = errno;
    const char* path = std::getenv("CPP_TCP_TEST_TRACE");
    if (path) {
        const int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
        if (fd >= 0) {
            (void)write(fd, message, std::strlen(message));
            close(fd);
        }
    }
    errno = saved;
}

// Separate monotonic evidence for accept retry intervals; disabled in normal runs.
void traceAccept(const char* kind, int value) {
    const int saved = errno;
    static const char* path = std::getenv("CPP_EPOLL_TEST_ACCEPT_TRACE");
    if (path) {
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            char buffer[192];
            const int size = std::snprintf(buffer, sizeof(buffer), "%lld.%09ld %s %d\n",
                static_cast<long long>(now.tv_sec), now.tv_nsec, kind, value);
            if (size > 0 && size < static_cast<int>(sizeof(buffer))) {
                const int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
                if (fd >= 0) { (void)write(fd, buffer, static_cast<size_t>(size)); close(fd); }
            }
        }
    }
    errno = saved;
}
}

extern "C" int signalfd(int fd, const sigset_t* mask, int flags) {
    using Function = int (*)(int, const sigset_t*, int);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "signalfd"));
    const int result = real(fd, mask, flags);
    if (result >= 0) signalDescriptor = result;
    return result;
}

// Only the signal descriptor is affected; ordinary files and network IO pass through.
extern "C" ssize_t read(int fd, void* buffer, size_t length) {
    using Function = ssize_t (*)(int, void*, size_t);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "read"));
    static bool injected = false;
    if (fd == signalDescriptor && !injected) {
        const char* mode = std::getenv("CPP_EPOLL_TEST_SIGNAL_READ");
        if (mode) {
            injected = true;
            if (std::strcmp(mode, "short") == 0) {
                const auto result = real(fd, buffer, length);
                trace("SIGNAL_READ_SHORT\n");
                return result > 0 ? result - 1 : result;
            }
            const int error = std::strcmp(mode, "eintr") == 0 ? EINTR :
                              std::strcmp(mode, "again") == 0 ? EAGAIN : EIO;
            trace("SIGNAL_READ_FAULT\n");
            errno = error;
            return -1;
        }
    }
    return real(fd, buffer, length);
}

// HTTP 验收专用：仅在测试进程中把回环地址的 8080 映射到临时端口。
// 不改动生产源码，也不抢占用户正在运行的 8080 服务。
extern "C" int bind(int fd, const sockaddr* address, socklen_t length) {
    using Function = int (*)(int, const sockaddr*, socklen_t);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "bind"));
    const char* setting = std::getenv("CPP_HTTP_TEST_PORT");
    if (setting && address && length >= sizeof(sockaddr_in) && address->sa_family == AF_INET) {
        sockaddr_in redirected{};
        std::memcpy(&redirected, address, sizeof(redirected));
        char* end = nullptr;
        const long port = std::strtol(setting, &end, 10);
        if (end && *end == '\0' && port > 0 && port <= 65535 &&
            redirected.sin_port == htons(8080) &&
            redirected.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
            redirected.sin_port = htons(static_cast<unsigned short>(port));
            trace("BIND_REDIRECT\n");
            return real(fd, reinterpret_cast<const sockaddr*>(&redirected), sizeof(redirected));
        }
    }
    return real(fd, address, length);
}

extern "C" int accept(int fd, sockaddr* address, socklen_t* length) {
    using Function = int (*)(int, sockaddr*, socklen_t*);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "accept"));
    static const bool countEnabled = [] {
        if (!std::getenv("CPP_EPOLL_TEST_ACCEPT_COUNT")) return false;
        return std::atexit(saveAcceptedCount) == 0;
    }();
    static std::atomic<bool> injected{false};
    if (enabled("CPP_TCP_TEST_EINTR") && !injected.exchange(true)) {
        trace("ACCEPT_EINTR\n");
        errno = EINTR;
        return -1;
    }
    static const char* errorSetting = std::getenv("CPP_EPOLL_TEST_ACCEPT_ERROR");
    static const int repeatCount = [] {
        const char* value = std::getenv("CPP_EPOLL_TEST_ACCEPT_REPEATS");
        return value ? std::atoi(value) : 1;
    }();
    static bool acceptedOne = false;
    static int faultCount = 0;
    // Inject only after an existing client is managed and another is actually queued.
    if (errorSetting && acceptedOne && faultCount < repeatCount) {
        pollfd pending{fd, POLLIN, 0};
        if (poll(&pending, 1, 0) > 0 && (pending.revents & POLLIN)) {
            ++faultCount;
            const int error = std::atoi(errorSetting);
            traceAccept("accept_error", error);
            errno = error;
            return -1;
        }
    }
    const int client = real(fd, address, length);
    if (client >= 0) acceptedOne = true;
    if (client >= 0 && countEnabled) ++acceptedCount;
    if (client >= 0 && enabled("CPP_TCP_TEST_SMALL_SNDBUF")) {
        const int size = 4096;
        if (setsockopt(client, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0) {
            trace("SMALL_SNDBUF\n");
        } else {
            trace("SHIM_FAILURE\n");
        }
    }
    return client;
}

extern "C" ssize_t recv(int fd, void* buffer, size_t length, int flags) {
    using Function = ssize_t (*)(int, void*, size_t, int);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "recv"));
    static std::atomic<bool> injected{false};
    static std::atomic<bool> again{false};
    if (enabled("CPP_EPOLL_TEST_RECV_EAGAIN") && !again.exchange(true)) {
        trace("RECV_EAGAIN\n");
        errno = EAGAIN;
        return -1;
    }
    if (enabled("CPP_TCP_TEST_EINTR") && !injected.exchange(true)) {
        trace("RECV_EINTR\n");
        errno = EINTR;
        return -1;
    }
    return real(fd, buffer, length, flags);
}

extern "C" ssize_t send(int fd, const void* buffer, size_t length, int flags) {
    using Function = ssize_t (*)(int, const void*, size_t, int);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "send"));
    static std::atomic<unsigned> calls{0};
    static std::atomic<bool> again{false};
    if (enabled("CPP_EPOLL_TEST_SEND_EAGAIN") && !again.exchange(true)) {
        trace("SEND_EAGAIN\n");
        errno = EAGAIN;
        return -1;
    }
    const unsigned call = ++calls;
    if (enabled("CPP_TCP_TEST_EINTR") && (call == 1 || call == 3)) {
        trace("SEND_EINTR\n");
        errno = EINTR;
        return -1;
    }
    const char* setting = std::getenv("CPP_TCP_TEST_CHUNK");
    const size_t cap = setting ? std::strtoul(setting, nullptr, 10) : 0;
    const bool limited = cap > 0 && length > cap;
    const ssize_t sent = real(fd, buffer, limited ? cap : length, flags);
    if (limited && sent > 0) {
        trace("PARTIAL_SEND\n");
    }
    return sent;
}

// 以下故障仅由 epoll 验收显式启用；默认透传真实系统调用。
extern "C" int epoll_wait(int fd, epoll_event* events, int count, int timeout) {
    using Function = int (*)(int, epoll_event*, int, int);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "epoll_wait"));
    static std::atomic<bool> injected{false};
    if (enabled("CPP_EPOLL_TEST_WAIT_EINTR") && !injected.exchange(true)) {
        trace("EPOLL_WAIT_EINTR\n");
        errno = EINTR;
        return -1;
    }
    return real(fd, events, count, timeout);
}

extern "C" int epoll_ctl(int fd, int operation, int client, epoll_event* event) {
    using Function = int (*)(int, int, int, epoll_event*);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "epoll_ctl"));
    static int listener = -1;
    static bool addFailed = false, modFailed = false, delFailed = false;
    static bool listenerPaused = false;
    if (client == signalDescriptor && operation == EPOLL_CTL_ADD &&
        enabled("CPP_EPOLL_TEST_SIGNAL_ADD_FAIL")) {
        trace("SIGNAL_ADD_FAIL\n");
        errno = ENOMEM;
        return -1;
    }
    if (client == listener && operation == EPOLL_CTL_DEL &&
        enabled("CPP_EPOLL_TEST_LISTENER_DEL_FAIL")) {
        trace("LISTENER_DEL_FAIL\n");
        errno = EIO;
        return -1;
    }
    if (client == listener && operation == EPOLL_CTL_ADD && listenerPaused &&
        enabled("CPP_EPOLL_TEST_LISTENER_RESUME_FAIL")) {
        traceAccept("resume_add_failed", EIO);
        errno = EIO;
        return -1;
    }
    if (client != listener && client != signalDescriptor && listener >= 0) {
        if (operation == EPOLL_CTL_ADD && enabled("CPP_EPOLL_TEST_ADD_FAIL") && !addFailed) {
            addFailed = true;
            trace("CLIENT_ADD_FAIL\n");
            errno = ENOMEM;
            return -1;
        }
        if (operation == EPOLL_CTL_MOD && enabled("CPP_EPOLL_TEST_MOD_FAIL") && !modFailed) {
            modFailed = true;
            trace("CLIENT_MOD_FAIL\n");
            errno = EIO;
            return -1;
        }
        if (operation == EPOLL_CTL_DEL && enabled("CPP_EPOLL_TEST_DEL_FAIL") && !delFailed) {
            delFailed = true;
            trace("CLIENT_DEL_FAIL\n");
            errno = EIO;
            return -1;
        }
    }
    const int result = real(fd, operation, client, event);
    if (result == 0 && client == listener) {
        if (operation == EPOLL_CTL_DEL) {
            listenerPaused = true;
            traceAccept("listener_del", client);
        } else if (operation == EPOLL_CTL_ADD && listenerPaused) {
            listenerPaused = false;
            traceAccept("listener_add", client);
        }
    }
    if (result == 0 && listener < 0 && operation == EPOLL_CTL_ADD) listener = client;
    return result;
}

extern "C" int shutdown(int fd, int how) {
    using Function = int (*)(int, int);
    static const auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "shutdown"));
    static bool injected = false;
    if (how == SHUT_WR && enabled("CPP_EPOLL_TEST_SHUTDOWN_FAIL") && !injected) {
        injected = true;
        trace("SHUTDOWN_FAIL\n");
        errno = EIO;
        return -1;
    }
    return real(fd, how);
}
