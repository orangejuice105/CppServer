// 仅供测试：通过 LD_PRELOAD 注入返回值、限制发送大小或映射临时端口。
// 不参与服务器的正常构建；未设置相应环境变量时调用原始系统接口。
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
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
    static std::atomic<bool> injected{false};
    if (enabled("CPP_TCP_TEST_EINTR") && !injected.exchange(true)) {
        trace("ACCEPT_EINTR\n");
        errno = EINTR;
        return -1;
    }
    const int client = real(fd, address, length);
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
