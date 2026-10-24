#include "logger.h"
#include "http_request.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

void closeClientConnection(int clientFd);
bool sendAll(int clientFd, const std::string& data);
extern "C" ssize_t __real_recv(int, void*, size_t, int);
extern "C" int __real_setsockopt(int, int, int, const void*, socklen_t);
extern "C" int __real_shutdown(int, int);
extern "C" int __real_close(int);

enum class Mode { Normal, ReceiveEintr, ShutdownFailure, TimeoutFailure, ReceiveFailure };
struct Audit {
    Mode mode = Mode::Normal;
    std::size_t bytes = 0;
    std::size_t calls = 0;
    std::size_t receiveCap = 1024;
    int closes = 0;
    bool closeSucceeded = false;
    int shutdowns = 0;
    int lastReceiveError = 0;
    long long timeoutUs = -1;
    bool configured = false;
    bool injected = false;
};
static std::atomic<int> targetFd{-1};
static Audit audit;

extern "C" int __wrap_shutdown(int fd, int how) {
    if (fd == targetFd.load() && how == SHUT_WR) {
        ++audit.shutdowns;
        if (audit.mode == Mode::ShutdownFailure) {
            audit.injected = true;
            errno = EIO;
            return -1;
        }
    }
    return __real_shutdown(fd, how);
}
extern "C" int __wrap_setsockopt(int fd, int level, int name, const void* value, socklen_t size) {
    const bool tracked = fd == targetFd.load() && level == SOL_SOCKET &&
                         name == SO_RCVTIMEO && size == sizeof(timeval);
    if (tracked) {
        const auto* timeout = static_cast<const timeval*>(value);
        audit.timeoutUs = static_cast<long long>(timeout->tv_sec) * 1000000 + timeout->tv_usec;
        if (audit.mode == Mode::TimeoutFailure) {
            audit.injected = true;
            errno = ENOPROTOOPT;
            return -1;
        }
    }
    const int result = __real_setsockopt(fd, level, name, value, size);
    if (tracked && result == 0) audit.configured = true;
    return result;
}
extern "C" ssize_t __wrap_recv(int fd, void* data, size_t size, int flags) {
    if (fd != targetFd.load()) return __real_recv(fd, data, size, flags);
    ++audit.calls;
    if (!audit.injected &&
        (audit.mode == Mode::ReceiveEintr || audit.mode == Mode::ReceiveFailure)) {
        audit.injected = true;
        const int error = audit.mode == Mode::ReceiveEintr ? EINTR : ECONNRESET;
        audit.lastReceiveError = error;
        errno = error;
        return -1;
    }
    const ssize_t result = __real_recv(fd, data, std::min(size, audit.receiveCap), flags);
    const int error = errno;
    audit.lastReceiveError = result < 0 ? error : 0;
    if (result > 0) audit.bytes += static_cast<std::size_t>(result);
    errno = error;
    return result;
}
extern "C" int __wrap_close(int fd) {
    const bool tracked = fd == targetFd.load();
    if (tracked) ++audit.closes;
    const int result = __real_close(fd);
    if (tracked) audit.closeSucceeded = result == 0;
    return result;
}

static bool check(const std::string& name, Mode mode, std::size_t payload,
                  bool halfClose, bool trickle, std::size_t receiveCap = 1024) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) std::abort();
    struct stat original{};
    if (fstat(pair[0], &original) != 0) std::abort();
    timeval originalTimeout{};
    originalTimeout.tv_sec = 30;
    if (__real_setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO,
                          &originalTimeout, sizeof(originalTimeout)) != 0) std::abort();
    audit = Audit{};
    audit.mode = mode;
    audit.receiveCap = receiveCap;
    targetFd.store(pair[0]);
    std::string queued(payload, 'x');
    std::size_t offset = 0;
    while (offset < queued.size()) {
        const ssize_t sent = send(pair[1], queued.data() + offset, queued.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) std::abort();
        offset += static_cast<std::size_t>(sent);
    }
    if (halfClose && shutdown(pair[1], SHUT_WR) != 0) std::abort();
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> sentByWriter{0};
    std::thread writer;
    if (trickle) {
        writer = std::thread([&] {
            while (!stop.load()) {
                const ssize_t sent = send(pair[1], "x", 1, MSG_NOSIGNAL);
                if (sent <= 0) break;
                sentByWriter.fetch_add(static_cast<std::size_t>(sent));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    const auto started = std::chrono::steady_clock::now();
    closeClientConnection(pair[0]);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    stop.store(true);
    if (writer.joinable()) writer.join();
    // 日志线程或 sanitizer 可以立刻复用刚释放的 fd 数字。
    // 校验原 socket 的身份，而不是把“同编号又有效”误报成连接泄漏。
    struct stat current{};
    errno = 0;
    const int status = fstat(pair[0], &current);
    const int statusError = errno;
    const bool descriptorClosed = audit.closeSucceeded &&
        ((status == -1 && statusError == EBADF) ||
         (status == 0 && (current.st_dev != original.st_dev || current.st_ino != original.st_ino)));
    const Audit observed = audit;
    targetFd.store(-1);
    close(pair[1]);
    bool passed = descriptorClosed && observed.closes == 1 && observed.shutdowns == 1;
    if (mode == Mode::ShutdownFailure) {
        passed = passed && observed.injected && !observed.configured && observed.calls == 0 && elapsed < .5;
    } else if (mode == Mode::TimeoutFailure) {
        passed = passed && observed.injected && !observed.configured && observed.calls == 0 && elapsed < .5;
    } else {
        passed = passed && observed.configured && observed.timeoutUs == 100000;
        if (mode == Mode::ReceiveFailure) {
            passed = passed && observed.injected && observed.calls == 1 && observed.bytes == 0 && elapsed < .5;
        } else if (mode == Mode::ReceiveEintr) {
            passed = passed && observed.injected && observed.bytes == payload && elapsed < .5;
        } else if (trickle) {
            passed = passed && elapsed >= .75 && elapsed < 1.6 && observed.bytes >= 20 && observed.bytes < 65536;
        } else if (payload > 65536) {
            passed = passed && observed.bytes == 65536 && elapsed < .8;
        } else if (halfClose) {
            passed = passed && observed.bytes == payload && elapsed < .5;
        } else {
            passed = passed && observed.bytes == 0 && elapsed >= .05 && elapsed < 1.5 &&
                     (observed.lastReceiveError == EAGAIN || observed.lastReceiveError == EWOULDBLOCK);
        }
    }
    std::cout << name << '\t' << passed << '\t' << std::fixed << std::setprecision(6)
              << elapsed << '\t' << observed.bytes << '\t' << observed.calls << '\t'
              << observed.closes << '\t' << observed.shutdowns << '\t' << observed.timeoutUs
              << '\t' << observed.injected << '\t' << observed.lastReceiveError << '\t'
              << sentByWriter.load() << '\t' << descriptorClosed << '\n';
    return passed;
}
// 预先填满真实内核发送缓冲区，再验证生产 sendAll 的 30 秒阻塞超时。
// AF_UNIX stream 不存在 TCP 延迟 ACK 继续释放空间的干扰；不伪造 EAGAIN。
static bool sendTimeout() {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return false;
    const int size = 4096;
    timeval timeout{};
    timeout.tv_sec = 30;
    if (setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0 ||
        setsockopt(pair[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        close(pair[0]); close(pair[1]);
        return false;
    }
    std::string fill(4096, 'x');
    std::size_t filled = 0;
    while (true) {
        const ssize_t sent = send(pair[0], fill.data(), fill.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) { filled += static_cast<std::size_t>(sent); continue; }
        if (sent == -1 && errno == EINTR) continue;
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        close(pair[0]); close(pair[1]);
        return false;
    }
    const auto begin = std::chrono::steady_clock::now();
    const bool result = sendAll(pair[0], "x");
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const bool passed = !result && filled > 0 && elapsed >= 29 && elapsed < 40;
    close(pair[0]); close(pair[1]);
    std::cout << "real-send-timeout\t" << passed << '\t' << std::fixed
              << std::setprecision(6) << elapsed << '\t' << filled << '\n';
    return passed;
}

static bool targetChecks() {
    const std::string at = "/" + std::string(4095, 'a');
    const std::string over = at + "a";
    struct Case { const char* name; std::string input; bool expected; std::string method; };
    const Case cases[] = {
        {"target-at-4096", "GET " + at, false, ""},
        {"target-over-4097", "GET " + over, true, "GET"},
        {"target-head-over", "HEAD " + over, true, "HEAD"},
        {"target-no-space", std::string(8192, 'G'), false, ""},
        {"target-leading-space", " " + over, false, ""},
        {"target-empty", "GET ", false, ""},
        {"target-invalid-method", "GE\tT " + over, false, ""},
        {"target-stops-at-version", "GET / HTTP/1.1" + over, false, ""},
        {"target-stops-at-cr", "GET /\r" + over, false, ""},
        {"target-stops-at-lf", "GET /\n" + over, false, ""},
        {"target-query-over", "GET /?x=" + std::string(4093, 'a'), true, "GET"},
        {"target-absolute-over", "GET http://localhost/" + std::string(4097, 'a'), true, "GET"},
        {"target-unknown-token", "FROB " + over, true, "FROB"},
        {"target-lowercase-token", "get " + over, true, "get"},
        {"target-before-version", "GET " + over + " HTTP/1.", true, "GET"},
        {"target-short-before-long-header", "GET /hello HTTP/1.1\r\nX: " + over, false, ""},
    };
    bool all = true;
    for (const auto& item : cases) {
        std::string method = "unchanged";
        const bool actual = isRequestTargetTooLong(item.input, 4096, method);
        const bool passed = actual == item.expected && method == (actual ? item.method : "unchanged");
        std::cout << item.name << '\t' << passed << '\n';
        all = all && passed;
    }
    return all;
}

int main(int argc, char** argv) {
    LoggerConfig config;
    config.filePath = "helper.log";
    if (!Logger::getInstance().init(config)) return 2;
    if (argc == 2 && std::string(argv[1]) == "--send-timeout") return sendTimeout() ? 0 : 1;
    if (argc != 1) return 2;
    int failures = 0;
    failures += !targetChecks();
    failures += !check("idle-peer-kept-open", Mode::Normal, 0, false, false);
    failures += !check("peer-eof", Mode::Normal, 0, true, false);
    failures += !check("drain-buffered-32769-bytes", Mode::Normal, 32769, true, false);
    failures += !check("64kib-budget-with-17-byte-reads", Mode::Normal, 65536 + 123, false, false, 17);
    failures += !check("one-second-budget-with-20ms-trickle", Mode::Normal, 0, false, true);
    failures += !check("injected-eintr-retry", Mode::ReceiveEintr, 1234, true, false);
    failures += !check("injected-shutdown-failure", Mode::ShutdownFailure, 0, false, false);
    failures += !check("injected-timeout-setting-failure", Mode::TimeoutFailure, 0, false, false);
    failures += !check("injected-receive-reset-error", Mode::ReceiveFailure, 0, false, false);
    return failures == 0 ? 0 : 1;
}
