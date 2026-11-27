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
//   account_user=amir123
//   account_password=hunter2
//
// account_user/account_password (2026-11-27, protocol v4): optional - if
// account_user is left blank/unset, the client connects anonymously exactly
// as before (no persistent identity). If set, account_password is hashed
// (SHA-256, see Sha256.h) before it ever leaves this machine - the plaintext
// here in the config file is the only place it exists in plaintext at all,
// which is a real, disclosed limitation of this file-based approach (anyone
// with filesystem access to this PC can read it) - see docs/MEMORY.md.
namespace ao {

struct NetworkConfig {
    std::string serverIp   = "127.0.0.1";
    uint16_t    serverPort = 7777;
    std::string playerName = "Player";
    std::string accountUser;     // empty => connect anonymously
    std::string accountPassword; // plaintext here only; hashed before sending
};

NetworkConfig LoadNetworkConfig(const std::string& filePath);

} // namespace ao
