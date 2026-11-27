#include "ServerConfig.h"

#include <cstdlib>
#include <fstream>

namespace ao {

namespace {

std::string Trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

ServerConfig LoadServerConfig(const std::string& filePath) {
    ServerConfig cfg{};

    std::ifstream file(filePath);
    if (!file.is_open()) return cfg; // missing file -> defaults, not an error

    std::string line;
    while (std::getline(file, line)) {
        // Allow "# comment" / "; comment" lines and blank lines.
        const std::string trimmedLine = Trim(line);
        if (trimmedLine.empty() || trimmedLine[0] == '#' || trimmedLine[0] == ';') continue;

        const size_t eq = trimmedLine.find('=');
        if (eq == std::string::npos) continue; // malformed line, skip rather than fail

        const std::string key   = Trim(trimmedLine.substr(0, eq));
        const std::string value = Trim(trimmedLine.substr(eq + 1));
        if (key.empty() || value.empty()) continue;

        if (key == "port") {
            const int v = std::atoi(value.c_str());
            if (v > 0 && v <= 65535) cfg.port = static_cast<uint16_t>(v);
        } else if (key == "max_players") {
            const int v = std::atoi(value.c_str());
            if (v > 0) cfg.maxPlayers = static_cast<uint32_t>(v);
        }
        // Unrecognised key: ignore rather than fail - keeps old config files
        // forward-compatible with a newer server binary.
    }
    return cfg;
}

} // namespace ao
