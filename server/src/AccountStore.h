// Persistent player accounts (2026-11-27) - the foundation needed before any
// website/store purchase can ever be linked to "whichever player is online
// right now": every Handshake can now optionally include a username +
// client-hashed password (see Sha256.h), and this class is what turns that
// into a stable accountId that outlives any one connection.
//
// Storage: a plain, append-mostly text file (default "accounts.dat" next to
// the server executable) - one account per line:
//   accountId|username|sha256HashHex(64 chars)|createdAtUnixSeconds
// Chosen over a real database for this first version because the server has
// no DB dependency at all yet and account volume for a single small
// community server is tiny - this is explicitly a starting point, not a
// scalability decision. See the class comment in Sha256.h for the other
// known, disclosed limitation (no per-account salt yet).
//
// Thread-safety: none - like the rest of this server, every call must come
// from the single thread running the main event loop (see server/src/Main.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "ao/Sha256.h"

namespace ao {

struct AccountRecord {
    uint32_t accountId = 0;
    std::string username;      // normalized lowercase - see Normalize()
    std::array<uint8_t, 32> passwordHash{};
    uint64_t createdAtUnix = 0;
};

class AccountStore {
public:
    // Lowercases and trims surrounding whitespace so "Amir", "amir ", "AMIR"
    // all refer to the same account - usernames are an identifier, not a
    // display string (the existing per-connection playerName, sent
    // separately in Handshake, is still whatever case/spelling the player
    // wants shown in-game).
    static std::string Normalize(std::string username) {
        size_t start = username.find_first_not_of(" \t\r\n");
        size_t end = username.find_last_not_of(" \t\r\n");
        username = (start == std::string::npos) ? std::string()
                                                  : username.substr(start, end - start + 1);
        for (char& c : username) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return username;
    }

    // Loads every existing account from disk, if the file exists. Malformed
    // lines are skipped (logged by the caller, not here - this header has no
    // logging dependency on purpose, see Main.cpp for where load results are
    // reported). Never fails outright - a missing/empty file just means no
    // accounts yet, which is a perfectly normal first run.
    bool Load(const std::string& filePath) {
        m_filePath = filePath;
        m_byUsername.clear();
        m_nextAccountId = 1;

        std::ifstream in(filePath);
        if (!in.is_open()) return true; // no file yet - fine, first run

        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            AccountRecord rec;
            if (!ParseLine(line, rec)) continue;
            m_byUsername[rec.username] = rec;
            if (rec.accountId >= m_nextAccountId) m_nextAccountId = rec.accountId + 1;
        }
        return true;
    }

    // nullptr if no account with this username (after normalization) exists.
    const AccountRecord* Find(const std::string& username) const {
        auto it = m_byUsername.find(Normalize(username));
        return (it != m_byUsername.end()) ? &it->second : nullptr;
    }

    // Creates a brand-new account with the next available id and appends it
    // to the on-disk file immediately (so a crash right after doesn't lose
    // it) - caller must already have checked Find() returned nullptr for
    // this username. Returns the new record (also now owned by this store).
    const AccountRecord& Create(const std::string& username,
                                 const std::array<uint8_t, 32>& passwordHash,
                                 uint64_t nowUnix) {
        AccountRecord rec;
        rec.accountId = m_nextAccountId++;
        rec.username = Normalize(username);
        rec.passwordHash = passwordHash;
        rec.createdAtUnix = nowUnix;

        AppendLine(rec);
        auto [it, _] = m_byUsername.emplace(rec.username, rec);
        return it->second;
    }

    size_t Count() const { return m_byUsername.size(); }

private:
    bool ParseLine(const std::string& line, AccountRecord& out) const {
        std::istringstream iss(line);
        std::string idStr, username, hashHex, createdStr;
        if (!std::getline(iss, idStr, '|')) return false;
        if (!std::getline(iss, username, '|')) return false;
        if (!std::getline(iss, hashHex, '|')) return false;
        if (!std::getline(iss, createdStr, '|')) return false;

        try {
            out.accountId = static_cast<uint32_t>(std::stoul(idStr));
            out.createdAtUnix = static_cast<uint64_t>(std::stoull(createdStr));
        } catch (...) {
            return false;
        }
        if (!FromHex(hashHex, out.passwordHash)) return false;
        out.username = Normalize(username);
        if (out.username.empty() || out.accountId == 0) return false;
        return true;
    }

    void AppendLine(const AccountRecord& rec) {
        std::ofstream out(m_filePath, std::ios::app);
        if (!out.is_open()) return;
        out << rec.accountId << '|' << rec.username << '|'
            << ToHex(rec.passwordHash) << '|' << rec.createdAtUnix << '\n';
    }

    std::string m_filePath;
    uint32_t m_nextAccountId = 1;
    std::unordered_map<std::string, AccountRecord> m_byUsername;
};

} // namespace ao
