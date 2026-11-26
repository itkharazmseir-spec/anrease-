#pragma once
#include <cstdint>
#include <string>

// Tiny "key=value" config file, read once at plugin load. Lets someone test
// against a real server without recompiling. Missing file, or any
// unrecognised/malformed line, just falls back to the defaults below -
// LoadNetworkConfig() never fails or throws.
//
// Example AndreasOnline.cfg (placed next to AndreasOnline.asi):
//   server_ip=203.0.113.10
//   server_port=7777
//   player_name=Amir
namespace ao {

struct NetworkConfig {
    std::string serverIp   = "127.0.0.1";
    uint16_t    serverPort = 7777;
    std::string playerName = "Player";
};

NetworkConfig LoadNetworkConfig(const std::string& filePath);

} // namespace ao
