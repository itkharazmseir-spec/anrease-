// Minimal, dependency-free SHA-256 - shared by client and server so both can
// compute the same password hash without linking any crypto library. Public
// domain algorithm (FIPS 180-4), written fresh for this project.
//
// Used for ONE thing only: turning an account password into a 32-byte digest
// client-side, before it ever leaves the player's machine, so the real
// plaintext password is never sent over the network or stored anywhere
// (client config file excepted - see NetworkConfig.h). The server only ever
// sees/stores this digest (hex-encoded in accounts.dat), never the password.
//
// Known, disclosed limitation (2026-11-27, account system v1): this is a
// single unsalted SHA-256 of the password, not a proper password-hashing
// scheme (bcrypt/scrypt/Argon2 with a per-account salt). That means if
// accounts.dat ever leaked, identical passwords across accounts would hash
// identically (visible), and the hashes would be crackable via precomputed
// (rainbow-table-style) lookups for common passwords faster than a salted
// scheme would allow. This is an intentional, documented simplification to
// get a working persistent-account system in place first (needed before any
// website/purchase linking can exist at all) - see docs/MEMORY.md. Before
// this system is ever used to gate real money/purchases, it should be
// upgraded to a per-account salt (and ideally the login itself moved to the
// website over HTTPS rather than raw ENet).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace ao {

namespace detail {

inline uint32_t Sha256RotR(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

} // namespace detail

// Returns the 32-byte SHA-256 digest of `input`.
inline std::array<uint8_t, 32> Sha256(const std::string& input) {
    static constexpr uint32_t kK[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };

    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };

    std::string msg = input;
    const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8ull;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back(static_cast<char>(0x00));
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFF));
    }

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(msg.data() + chunk + i * 4);
            w[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = detail::Sha256RotR(w[i - 15], 7) ^ detail::Sha256RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = detail::Sha256RotR(w[i - 2], 17) ^ detail::Sha256RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = detail::Sha256RotR(e, 6) ^ detail::Sha256RotR(e, 11) ^ detail::Sha256RotR(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + S1 + ch + kK[i] + w[i];
            const uint32_t S0 = detail::Sha256RotR(a, 2) ^ detail::Sha256RotR(a, 13) ^ detail::Sha256RotR(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;

            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
    }
    return out;
}

inline std::string ToHex(const std::array<uint8_t, 32>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

inline bool FromHex(const std::string& hex, std::array<uint8_t, 32>& out) {
    if (hex.size() != 64) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

} // namespace ao
