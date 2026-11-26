#pragma once
#include <string>

// Tiny line-based file logger. All calls happen on the game thread for now,
// but it is guarded by a mutex so the network thread (step 2) can use it too.
namespace ao::log {

void Init(const std::string& filePath);
void Shutdown();

// printf-style. Adds timestamp + newline, flushes every line
// (so the log survives a game crash).
void Info(const char* fmt, ...);
void Error(const char* fmt, ...);

} // namespace ao::log
