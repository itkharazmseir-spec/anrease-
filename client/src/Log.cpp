#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace ao::log {
namespace {

// Function-local statics: safe regardless of static-initialisation order
// between translation units (the plugin object in Main.cpp logs from its ctor).
struct State {
    std::mutex mtx;
    std::FILE* file = nullptr;
};

State& GetState() {
    static State s;
    return s;
}

void Write(const char* level, const char* fmt, va_list args) {
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.file)
        return;

    std::time_t now = std::time(nullptr);
    std::tm tmLocal{};
    localtime_s(&tmLocal, &now);

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmLocal);

    std::fprintf(s.file, "[%s] [%s] ", stamp, level);
    std::vfprintf(s.file, fmt, args);
    std::fputc('\n', s.file);
    std::fflush(s.file);
}

} // namespace

void Init(const std::string& filePath) {
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.file)
        return;
    // "w": a fresh log every game start.
    if (fopen_s(&s.file, filePath.c_str(), "w") != 0)
        s.file = nullptr;
}

void Shutdown() {
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
}

void Info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Write("INFO", fmt, args);
    va_end(args);
}

void Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Write("ERROR", fmt, args);
    va_end(args);
}

} // namespace ao::log
