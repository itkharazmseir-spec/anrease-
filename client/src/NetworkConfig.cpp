#include "NetworkConfig.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace ao {
namespace {

std::string Trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

} // namespace

NetworkConfig LoadNetworkConfig(const std::string& filePath) {
    NetworkConfig cfg; // defaults, used for anything the file doesn't set
    std::ifstream in(filePath);
    if (!in.is_open()) return cfg;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        if (value.empty()) continue;

        if (key == "server_ip") {
            cfg.serverIp = value;
        } else if (key == "server_port") {
            try {
                const int p = std::stoi(value);
                if (p > 0 && p <= 65535) cfg.serverPort = static_cast<uint16_t>(p);
            } catch (...) {
                // malformed port -> keep default
            }
        } else if (key == "player_name") {
            cfg.playerName = value;
        } else if (key == "account_user") {
            cfg.accountUser = value;
        } else if (key == "account_password") {
            cfg.accountPassword = value;
        }
    }
    return cfg;
}

} // namespace ao
