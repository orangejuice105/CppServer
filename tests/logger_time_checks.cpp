// Test-only fixture linked against the actual logger implementation.
#include "logger.h"
#include <cerrno>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>

std::string getCurrentTime();
extern "C" std::tm* __real_localtime_r(const time_t*, std::tm*);

namespace {
bool failConversion = false;
int conversionCalls = 0;
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
std::string expectedTime(time_t value) {
    std::tm result{};
    require(__real_localtime_r(&value, &result) != nullptr, "Reference conversion failed");
    char text[64]{};
    require(std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &result) != 0,
            "Reference formatting failed");
    return text;
}
void checkNormal() {
    const time_t before = std::time(nullptr);
    const int callsBefore = conversionCalls;
    const std::string actual = getCurrentTime();
    const time_t after = std::time(nullptr);
    require(conversionCalls == callsBefore + 1, "Unexpected conversion call count");
    require(actual == expectedTime(before) || actual == expectedTime(after),
            "Normal timestamp is not the current local time");
}
void checkFailure() {
    const int callsBefore = conversionCalls;
    require(getCurrentTime() == "time-unavailable", "Missing failure placeholder");
    require(conversionCalls == callsBefore + 1, "Conversion failure caused retry or recursion");
}
}

extern "C" std::tm* __wrap_localtime_r(const time_t* value, std::tm* result) {
    ++conversionCalls;
    if (failConversion) {
        errno = EOVERFLOW;
        return nullptr; // Leave the output untouched, like an unsuccessful conversion may do.
    }
    return __real_localtime_r(value, result);
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const std::string mode = argv[1];
        LoggerConfig config;
        config.filePath = argv[2];
        require(Logger::getInstance().init(config), "Logger initialization failed");
        if (mode == "normal") {
            checkNormal();
            LOGGER_INFO("normal-time-marker");
        } else if (mode == "failure") {
            failConversion = true;
            checkFailure();
            LOGGER_ERROR("failed-time-body-preserved");
        } else if (mode == "recovery") {
            failConversion = true;
            checkFailure();
            LOGGER_WARN("before-time-recovery");
            failConversion = false;
            checkNormal();
            LOGGER_INFO("after-time-recovery");
        } else {
            throw std::runtime_error("Unknown test mode");
        }
        std::cout << "PASS " << mode << " conversions=" << conversionCalls << '\n';
        return 0; // Logger's real destructor drains its background queue and closes the log.
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
