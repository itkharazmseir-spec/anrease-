#pragma once
#include <cstdint>
#include <string>

// Tiny "key=value" config file, read once at server startup, next to the
// server executable. Lets you keep port/maxPlayers settings without
// retyping them as command-line args every launch. Missing file, or any
// unrecognised/malformed line, just falls back to the defaults below -
// LoadServerConfig() never fails or throws. Command-line args (see main() in
// Main.cpp) still override whatever this file sets, so you can always force
// a one-off value without editing the file.
//
// Example server.cfg (placed next to AndreasOnlineServer.exe):
//   port=7777
//   max_players=32
namespace ao {

struct ServerConfig {
    uint16_t port       = 7777;
    uint32_t maxPlayers = 32;
};

ServerConfig LoadServerConfig(const std::string& filePath);

} // namespace ao
