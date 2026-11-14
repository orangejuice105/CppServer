// Test-only wrapper: snapshot logger counters after the production event loop returns.
// It changes no request handling, log configuration, clock, or socket operation.
#include "logger.h"
#include <cstdlib>
#include <fstream>
extern "C" int __real__Z12runTcpServeri(int);
extern "C" int __wrap__Z12runTcpServeri(int shutdownFd) {
    const int result = __real__Z12runTcpServeri(shutdownFd);
    const char* path = std::getenv("CPP_EPOLL_TEST_LOGGER_STATS");
    if (path) {
        const auto stats = Logger::getInstance().getStats();
        std::ofstream output(path);
        output << "{\"dropped_count\":" << stats.droppedCount
               << ",\"queue_peak\":" << stats.queuePeak << "}\n";
        output.close();
        if (!output) return 2;
    }
    return result;
}
