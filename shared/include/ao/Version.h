// Shared between client and server. Header-only, no game or OS dependencies.
#pragma once
#include <cstdint>

#ifndef AO_VERSION_STRING
#define AO_VERSION_STRING "0.0.0-dev"
#endif

namespace ao {

inline constexpr const char* kProductName   = "Andreas Online";
inline constexpr const char* kVersionString = AO_VERSION_STRING;

// Network protocol version. Bumped on every incompatible wire-format change.
// Client and server refuse to talk if this differs (enforced from step 2 on).
// v3 (2026-11-27): added MessageType::DamageReport + the DamageReport struct
// (item 5, "weapon/damage sync" - see Protocol.h and docs/MEMORY.md).
inline constexpr std::uint16_t kProtocolVersion = 3;

} // namespace ao
