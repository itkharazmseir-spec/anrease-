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

// ---------------------------------------------------------------------------
// CPopulation::AddPed, found 2026-11-26. This was previously suspended as
// "likely infeasible" (see docs/MEMORY.md, 2026-11-26 entry) under the theory
// that profile-guided optimisation (PGO) had split this function across many
// small, disconnected pieces that Ghidra's auto-analysis would never
// recombine into one function, the way 0x612b40-0x6137b0 looked at first
// glance. That theory was WRONG: the real cause was simply that an earlier
// pass probed the wrong sub-address inside that gap region, which does in
// fact contain several small, previously-unanalysed functions -- AddPed
// itself is one single, contiguous, ordinary function, just like
// FindPlayerPed above.
//
// How it was found and cross-verified (all independently checked, nothing
// guessed):
//   1. gta-reversed (github.com/gta-reversed/gta-reversed,
//      source/game_sa/Population.cpp) gives the exact 1.0 US address,
//      0x612710, for CPopulation::AddPed -- right before the gap region
//      above, which is the first clue this build's copy lives nearby.
//   2. Ghidra's create_function on 0x612f36 (the first real instruction
//      after a small preceding stub + NOP padding at 0x612f24-0x612f35)
//      produced a clean, 1380-byte function with a single entry and a
//      single set of exits -- not the scattered mess PGO-splitting would
//      produce.
//   3. The decompiled body matches the 1.0 US source
//      (CPopulation::AddPed in Population.cpp) point for point:
//        - thunk_FUN_014f0e20(0x79c)  -> operator new(sizeof(CPed)=0x79C),
//          the same CPed size already independently verified in
//          docs/MEMORY.md.
//        - the literal constant 0x61a9 (=25001 decimal) appears exactly
//          where the 1.0 source calls
//          GiveAndSetPedWeapon(ped, wtype, /*ammo=*/25001).
//        - a call to FUN_0056e6b0(0xffffffff) is a call to the
//          FindPlayerPed address above with playerId=-1, matching the 1.0
//          source's CHEAT_SLUT_MAGNET branch, which calls FindPlayerPed().
//        - FUN_00439b00(0,0,0) matches SetOrientation(0,0,0).
//        - FUN_005636c0(iVar5) matches CWorld::Add(ped).
//        - a final `if ((char)param_4 != '\0') { ... }` matches the 1.0
//          source's `if (!makeWander) return ped;` (same check, inverted).
//   4. plugin-sdk's own pattern for calling AddPed on 1.0 US
//      (plugin_sa/game_sa/CPopulation.cpp:238-241) confirms the exact
//      calling convention this project relies on:
//        __cdecl, 4 args: (ePedType pedType, unsigned int modelIndex,
//        CVector const& posn, bool makeWander), returns CPed*.
//      Only the address differs between 1.0 US (0x612710) and this RGL
//      build (0x612f36) -- the ABI is identical, which is why it is safe to
//      call through a raw function pointer the same way FindPlayerPed is
//      called above, without needing plugin-sdk's own (wrong-for-this-exe)
//      wrapper.
//
// IMPORTANT - why this is riskier than FindPlayerPed above:
//   FindPlayerPed only *reads* an existing pointer. AddPed *allocates and
//   inserts* a brand-new CPed into the game's own global ped pool
//   (CPools::ms_pPedPool) and world-sector lists (CWorld::Add). There is no
//   verified-safe "call once per frame, on the game's own main thread" hook
//   for this build (see the Events note below), so this is called from our
//   background polling thread instead. That is a materially bigger risk
//   than reading memory from a background thread (already done elsewhere in
//   this project): if the game's own main thread is concurrently iterating
//   the same pool/world-sector lists while we insert into them, that is a
//   genuine data race, not just a "stale read" risk, and a crash under some
//   timings is possible. This is disclosed to the user, not hidden, and
//   RemotePeds.cpp keeps the window as short as possible (SEH-guarded,
//   single call per spawn, no retries on failure).
constexpr std::uintptr_t kAddr_AddPed_RGL = 0x00612F36;

// The first 29 bytes of the verified AddPed entry point (the SEH-frame
// prologue + the start of the pedType switch), used the same way as
// VerifyFindPlayerPedFingerprint above: refuse to call into this address if
// a future game update has changed what is there.
inline bool VerifyAddPedFingerprint() {
    static const unsigned char kExpected[29] = {
        0x6A, 0xFF,                               // PUSH -1 (SEH prev-record sentinel)
        0x68, 0xF8, 0xF6, 0x83, 0x00,              // PUSH 0x0083F6F8 (SEH __except filter)
        0x50,                                     // PUSH EAX
        0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00,  // MOV FS:[0],ESP (install SEH frame)
        0x83, 0xEC, 0x30,                          // SUB ESP,0x30
        0x53,                                     // PUSH EBX
        0x8B, 0x5C, 0x24, 0x44,                    // MOV EBX,[ESP+0x44]  (pedType arg)
        0x8D, 0x43, 0xFC,                          // LEA EAX,[EBX-4]
        0x83, 0xF8, 0x1B                           // CMP EAX,0x1B (switch range check)
    };

    __try {
        return std::memcmp(
            reinterpret_cast<const void*>(kAddr_AddPed_RGL),
            kExpected, sizeof(kExpected)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// CCarCtrl::CreateCarForScript, found 2026-11-26 (same session as AddPed
// above) for step 3's "vehicle sync" - needed so a remote player who is
// driving shows up as an actual moving vehicle, not a ped floating at the
// car's position. Found with the same xref-based technique the AddPed
// writeup above recommends for next time: FindFunctions/get_functions on the
// already-verified RGL `CWorld::Add` address (kept internally as
// FUN_005636c0, decompiled address 0x5636C0) returned every caller that adds
// an entity to the world; CreateCarForScript was one of 49 (every entity
// type - objects, peds, vehicles - funnels through CWorld::Add, so most of
// the 49 are unrelated). It stood out immediately both by being close to the
// 1.0 US address (1.0: 0x431F80, RGL: 0x432000 - the small 0x80 delta is the
// biggest clue this build's copy sits almost exactly in the same place) and
// by its decompiled body.
//
// How it was cross-verified, all independently checked, nothing guessed:
//   1. gta-reversed (source/game_sa/CarCtrl.cpp,
//      `CVehicle* CCarCtrl::CreateCarForScript(int32 modelId, CVector posn,
//      bool doMissionCleanup)`) gives the full 1.0 US source to compare
//      against.
//   2. The decompiled body matches it point for point:
//        - a call to a `CModelInfo::IsBoatModel`-shaped function decides
//          between two parallel branches (boat vs normal vehicle), exactly
//          like the 1.0 source's `if (CModelInfo::IsBoatModel(modelid))`.
//        - the literal float bit-pattern 0x41500000 (=13.0f) is written into
//          the autopilot-speed field in the non-boat branch, and the literal
//          byte 0xD (=13 decimal) right after it - matching the 1.0 source's
//          `vehicle->m_autoPilot.m_speed = 13.0F;
//          vehicle->m_autoPilot.SetCruiseSpeed(13);` exactly.
//        - the boat branch instead writes 0x41a00000 (=20.0f) and the byte
//          0x14 (=20 decimal), matching `boat->m_autoPilot.m_speed = 20.0F;
//          ...SetCruiseSpeed(20);` exactly - two independent float+byte
//          constant pairs, in two different branches, both exact matches.
//        - both branches call FUN_005636c0 (the already-verified CWorld::Add
//          address) on the freshly built vehicle, matching `CWorld::Add(...)`
//          in both the 1.0 source's boat and non-boat paths.
//   3. plugin-sdk's own 1.0 US declaration
//      (plugin_sa/game_sa/CCarCtrl.h:49) confirms the exact signature this
//      project relies on: __cdecl, 5 stack slots -
//      (int modelid, CVector posn /* BY VALUE, i.e. 3 separate float stack
//      args - not a reference like AddPed's CVector const& */,
//      unsigned char MissionCleanUpFlag), returns CVehicle*. The decompiled
//      RGL function takes exactly 5 parameters in exactly this shape
//      (int, float, float, float, char), confirming the by-value CVector
//      passing convention carried over unchanged from 1.0 US.
//
// Same risk profile as AddPed above: this allocates into and mutates the
// game's own global vehicle pool / world-sector lists from our background
// polling thread, with no verified-safe main-thread hook to do it on
// instead. See client/src/RemotePeds.h for how this project handles that.
constexpr std::uintptr_t kAddr_CreateCarForScript_RGL = 0x00432000;

// The first 40 bytes of the verified CreateCarForScript entry point (the
// SEH-frame prologue + the start of the IsBoatModel check), used the same
// way as the fingerprints above: refuse to call into this address if a
// future game update has changed what is there.
inline bool VerifyCreateCarForScriptFingerprint() {
    static const unsigned char kExpected[40] = {
        0x6A, 0xFF,                               // PUSH -1 (SEH prev-record sentinel)
        0x68, 0xAB, 0x7D, 0x83, 0x00,              // PUSH 0x00837DAB (SEH __except filter)
        0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,        // MOV EAX,FS:[0]
        0x50,                                     // PUSH EAX
        0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00,  // MOV FS:[0],ESP (install SEH frame)
        0x51,                                     // PUSH ECX
        0x53,                                     // PUSH EBX
        0x56,                                     // PUSH ESI
        0x8B, 0x74, 0x24, 0x1C,                    // MOV ESI,[ESP+0x1C] (modelid arg)
        0x57,                                     // PUSH EDI
        0x56,                                     // PUSH ESI
        0xE8, 0xCD, 0x3A, 0x09, 0x00,              // CALL <IsBoatModel-shaped function>
        0x83, 0xC4, 0x04,                          // ADD ESP,0x4
        0x84, 0xC0                                 // TEST AL,AL
    };

    __try {
        return std::memcmp(
            reinterpret_cast<const void*>(kAddr_CreateCarForScript_RGL),
            kExpected, sizeof(kExpected)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// CPlaceable::SetHeading, found 2026-11-27 for item 2 ("real orientation"):
// writing CPed::m_fHeadingCurrent directly (what RemotePeds.cpp did before
// this) only feeds the ped's own AI heading-blend logic - it does NOT touch
// the actual rotation matrix the game renders, so a remote ped/vehicle was
// positioned correctly but never visibly turned to face the right way. This
// is the real fix: CPlaceable::SetHeading rewrites the entity's rotation
// matrix directly (via CMatrix::SetRotateZOnly), which IS what gets drawn.
//
// How it was found - starting from a clue already in hand, not a blind
// search: the AddPed writeup above (see its "point for point" match list)
// already noted a call `FUN_00439b00(0,0,0)` matching `SetOrientation(0,0,0)`
// in the 1.0 US source. That alone made 0x439b00 a strong candidate for
// CPlaceable::SetOrientation, confirmed here by decompiling it in full and
// comparing point for point against gta-reversed
// (source/game_sa/Entity/Placeable.cpp):
//   - reads an int at object-offset 0x14 and branches on whether it is zero
//     -- exactly m_matrix's offset, independently confirmed by plugin-sdk's
//     own `VALIDATE_OFFSET(CPlaceable, m_matrix, 0x14)`.
//   - the zero branch writes the third argument (z) to offset 0x10 and
//     returns, matching `m_placement.m_fHeading = z; return;` exactly (the
//     1.0 source's early-out for an entity with no allocated matrix).
//   - the non-zero branch saves the three floats at matrix-offset
//     0x30/0x34/0x38 (the matrix's position row), calls a helper, then adds
//     those saved floats back - matching `CVector vecPos =
//     m_matrix->GetPosition(); m_matrix->SetRotate(x,y,z);
//     m_matrix->GetPosition() += vecPos;` exactly (save position, rotate
//     resets it, restore position).
//
// gta-reversed's 1.0 US address for SetOrientation is 0x439A80 - an 0x80
// delta from this RGL build's 0x439b00, the SAME delta already seen between
// 1.0 US and RGL for CreateCarForScript above. That shared delta was the tip
// that `0x80` might be a reliable offset for this whole neighbourhood of the
// binary, which is exactly how SetHeading below was found next.
//
// CPlaceable::SetHeading (1.0 US: 0x43E0C0, source: `if (m_matrix)
// m_matrix->SetRotateZOnly(heading); else m_placement.m_fHeading =
// heading;`) was checked at the same +0x80 delta - 0x43E140 - and decompiled
// to EXACTLY that shape: reads the same offset-0x14 field, the zero branch
// writes to the same offset-0x10 field, and the non-zero branch is a single
// tail call (`JMP`, not `CALL` - the compiler turned it into a jump because
// nothing runs after it) into another small function. That callee was
// decompiled too and is an exact textbook `CMatrix::SetRotateZOnly`: it
// computes a single cos/sin pair and writes a pure Z-axis rotation (right =
// (cos,sin,0), up = (-sin,cos,0), at untouched/identity-ish), zeroing the
// cross terms - precisely the "rotate around up/Z only, ignore pitch/roll"
// math SetRotateZOnly is named for, and distinct from SetOrientation's
// general 3-axis SetRotate above.
//
// Why SetHeading, not SetOrientation, is used in RemotePeds.cpp: this
// project only ever tracks a single `headingRad` float per remote player
// (see PlayerState in shared/include/ao/Protocol.h) - there is no pitch/roll
// to send over the network in the first place, so SetHeading's simpler,
// cheaper single-angle rotation is the correct function to call, not a
// mismatch-covering workaround.
//
// Why this works for BOTH remote peds and vehicles with the same function
// pointer: plugin-sdk's class hierarchy (CPed : CPhysical : CEntity :
// CPlaceable, and CVehicle : CPhysical : CEntity : CPlaceable) is single,
// non-virtual-base inheritance the whole way down for both - no multiple
// inheritance, so the CPlaceable sub-object sits at object offset 0 in
// every derived class, same as it does for CPed. A CPed* and a CVehicle*
// are therefore both valid "this" pointers for this __thiscall function
// without any pointer adjustment, which is also why this single address
// removes the vehicle-heading limitation noted in RGL_COMPAT_NOTES.md at
// the same time as the ped one, instead of needing a second address.
constexpr std::uintptr_t kAddr_SetHeading_RGL = 0x0043E140;

// The full 24 bytes of this function (it really is this short - see the
// evidence chain above), used the same way as the other fingerprints: if a
// future game update changes what is at this address, refuse to call it.
inline bool VerifySetHeadingFingerprint() {
    static const unsigned char kExpected[24] = {
        0x8B, 0xC1,                                // MOV EAX,ECX
        0x8B, 0x48, 0x14,                          // MOV ECX,[EAX+0x14] (m_matrix)
        0x85, 0xC9,                                // TEST ECX,ECX
        0x74, 0x05,                                // JZ +5
        0xE9, 0xA2, 0xD6, 0x15, 0x00,              // JMP CMatrix::SetRotateZOnly (tail call)
        0x8B, 0x4C, 0x24, 0x04,                    // MOV ECX,[ESP+4] (heading arg)
        0x89, 0x48, 0x10,                          // MOV [EAX+0x10],ECX (m_placement.m_fHeading)
        0xC2, 0x04, 0x00                           // RET 4
    };

    __try {
        return std::memcmp(
            reinterpret_cast<const void*>(kAddr_SetHeading_RGL),
            kExpected, sizeof(kExpected)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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
