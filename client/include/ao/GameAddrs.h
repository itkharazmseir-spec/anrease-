// Client-only (Windows). This deliberately lives under client/include, not
// shared/include: it uses SEH (__try/__except) and reads live process
// memory, neither of which belongs in the cross-platform client/server
// shared headers.
#pragma once
#include <cstdint>
#include <cstring>
#include <windows.h>

// ---------------------------------------------------------------------------
// Raw game addresses for the Rockstar Games Launcher (RGL) build of
// gta_sa.exe, found 2026-11-26 by static analysis (Ghidra) of the exact
// binary the user has installed, because that build is NOT one of the
// versions plugin-sdk (PLUGIN_SGV_10US / PLUGIN_SGV_STEAM) recognises.
//
// How we know it is the RGL build, not 1.0 US / old Steam:
//   - The exe contains the strings "RLAUNCHER" and "RLAUNCHER_HS", which only
//     exist in builds with Rockstar Games Launcher integration.
//   - Every address plugin-sdk's shared/Events.h and PlayerInfoSA.cpp (from
//     SilentPatch, see below) expect for 1.0 US either falls strictly inside
//     an unrelated instruction, or inside a completely unrelated function,
//     in this binary.
//
// How FindPlayerPed was found (verifiable independently in Ghidra):
//   1. plugin-sdk's own 1.0 US address for FindPlayerPed (0x56E210) and
//      CookiePLMonster/SilentPatch's open-source, multi-version-verified
//      PlayerInfoSA.cpp both point at the same conceptual function/region.
//   2. We listed functions in this exe by cross-reference count
//      (find_functions, sort_by=xref_count) and inspected the ones in the
//      same functional neighbourhood as the 1.0 address. FUN_0056e6b0
//      (517 xrefs) disassembles to exactly the textbook FindPlayerPed body:
//
//        MOV  EAX, [ESP+4]            ; playerID argument
//        TEST EAX, EAX
//        JGE  +7                      ; if (playerID >= 0) skip
//        MOVZX EAX, byte [0xB7F3F4]   ; EAX = PlayerInFocus
//        IMUL EAX, EAX, 0x190         ; EAX *= sizeof(CPlayerInfo)
//        MOV  EAX, [EAX + 0xB7F418]   ; EAX = Players[idx].m_pPed
//        RET
//
//      The 0x190 multiplier is not a guess: it is exactly sizeof(CPlayerInfo)
//      as independently asserted by plugin-sdk's VALIDATE_OFFSET machinery
//      AND by SilentPatch's `static_assert(sizeof(CPlayerInfo) == 0x190, ...)`
//      -- i.e. two independent open-source projects agree this struct size
//      is unchanged across every PC build of GTA SA, which is why this
//      70%-confidence-by-region candidate became a ~100%-confidence match.
//   3. Two separate, unrelated callers in this exe call this function with
//      `PUSH -1` immediately before `CALL 0x0056E6B0`, matching the universal
//      `FindPlayerPed(-1)` ("local player") calling pattern used everywhere
//      in the original game code.
//
// IMPORTANT - what is still NOT verified for this build:
//   - CGame::currArea (interior index). plugin-sdk hardcodes this as a
//     single, version-gated global that resolves to address 0 (a null
//     pointer dereference -> crash) for any version it doesn't recognise,
//     RGL included. We have NOT found the real address yet, so interior
//     tracking is disabled in LocalPlayer.cpp rather than risk a crash.
//     Finding it is a good next step if you want that feature back: it is a
//     single, very frequently read `int` global, so the same
//     "disassemble neighbouring functions / look for a short, hot accessor"
//     technique used above should work, or a byte-pattern (AOB) scan against
//     SilentPatch's source (which already supports RGL) if you enable
//     Ghidra script execution (GHIDRA_MCP_ALLOW_SCRIPTS=1) for a proper
//     wildcard memory search instead of manual disassembly.
//   - The four plugin-sdk Events (gameProcessEvent, drawingEvent,
//     initGameEvent, shutdownRwEvent). These patch a specific CALL
//     instruction inside specific, unrelated game functions, and finding
//     them reliably needs the same kind of binary-specific investigation.
//     Main.cpp below replaces them with a plain polling thread instead of
//     hooking the game's own per-frame tick, which avoids needing these
//     addresses at all, at the cost of the on-screen F9 overlay (that part
//     specifically needs a safe "once per rendered frame, on the game's own
//     thread" hook, which a polling thread on its own thread cannot safely
//     provide -- CFont/RenderWare calls from the wrong thread are a crash
//     risk, not a cosmetic issue).
//
// Re-verify before trusting any of this after a game update: Rockstar
// updates the RGL build occasionally, and these addresses are only valid for
// the exact gta_sa.exe you had loaded in Ghidra on 2026-11-26. AoVerifyGameAddresses()
// below does a byte-for-byte check at runtime and disables the plugin
// instead of reading wrong memory if the binary has changed.
// ---------------------------------------------------------------------------

namespace ao {

constexpr std::uintptr_t kAddr_FindPlayerPed_RGL = 0x0056E6B0;

// The exact 28 bytes of FUN_0056E6B0 in the analysed binary, used as a
// runtime fingerprint so a future game update can't make us read/call into
// whatever ends up at this address after a patch.
inline bool VerifyFindPlayerPedFingerprint() {
    static const unsigned char kExpected[28] = {
        0x8B, 0x44, 0x24, 0x04,                   // MOV EAX,[ESP+4]
        0x85, 0xC0,                               // TEST EAX,EAX
        0x7D, 0x07,                               // JGE +7
        0x0F, 0xB6, 0x05, 0xF4, 0xF3, 0xB7, 0x00,  // MOVZX EAX,byte [0xB7F3F4]
        0x69, 0xC0, 0x90, 0x01, 0x00, 0x00,        // IMUL EAX,EAX,0x190
        0x8B, 0x80, 0x18, 0xF4, 0xB7, 0x00,        // MOV EAX,[EAX+0xB7F418]
        0xC3                                       // RET
    };

    __try {
        return std::memcmp(
            reinterpret_cast<const void*>(kAddr_FindPlayerPed_RGL),
            kExpected, sizeof(kExpected)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Unmapped/changed memory (e.g. a different exe entirely) - treat as
        // "not verified" rather than letting a structured exception escape.
        return false;
    }
}

} // namespace ao
