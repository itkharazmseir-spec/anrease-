#include "LocalPlayer.h"

#include "ao/GameAddrs.h"
#include "CPlayerPed.h"
#include "CVehicle.h"
// CGame.h intentionally not included: CGame::currArea is not safe on this
// build (see GameAddrs.h and LocalPlayer.h) and nothing else here needs it.

namespace ao {
namespace {

// The game's own FindPlayerPed, called directly because plugin-sdk's
// wrapper does not recognise this exe (see GameAddrs.h). __cdecl matches
// the disassembly: the argument is read from [ESP+4] and the function
// returns with a plain RET (not "RET 4"), i.e. the caller cleans the stack.
using FindPlayerPedFn = CPlayerPed*(__cdecl*)(int);

CPlayerPed* FindPlayerPed_RGL(int playerId) {
    if (!VerifyFindPlayerPedFingerprint()) {
        // Game updated since this was verified, or this isn't the exe we
        // analysed - refuse to call into unknown code.
        return nullptr;
    }
    auto fn = reinterpret_cast<FindPlayerPedFn>(kAddr_FindPlayerPed_RGL);
    return fn(playerId);
}

} // namespace

void LocalPlayer::Update() {
    PlayerSnapshot s{};

    // This runs on our own polling thread, not the game's main thread (see
    // Main.cpp for why). That means there is a narrow window where the ped
    // or vehicle pointer below could be freed by the game thread between us
    // reading it and dereferencing it (e.g. mid-respawn). The SEH guard
    // turns that into "skip this tick" instead of crashing the game.
    __try {
        CPlayerPed* ped = FindPlayerPed_RGL(-1);
        if (!ped) {
            m_snapshot = s;
            return;
        }

        s.valid      = true;
        s.headingRad = ped->m_fHeadingCurrent;
        s.health     = ped->m_fHealth;
        s.armour     = ped->m_fArmour;
        s.pedModel   = ped->m_nModelIndex;

        CVehicle* veh = (ped->bInVehicle && ped->m_pVehicle) ? ped->m_pVehicle : nullptr;
        const CVector& pos = veh ? veh->GetPosition() : ped->GetPosition();

        s.x = pos.x;
        s.y = pos.y;
        s.z = pos.z;

        if (veh) {
            s.inVehicle    = true;
            s.vehicleModel = veh->m_nModelIndex;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_snapshot = PlayerSnapshot{}; // valid = false
        return;
    }

    m_snapshot = s;
}

void LocalPlayer::ApplyDamage(float healthDelta, float armourDelta) {
    // Same background-thread/direct-game-memory-write risk already disclosed
    // for every other field write in this project (see RemotePeds.h) - this
    // just extends that existing, already-accepted risk to the local
    // player's own ped instead of only a spawned remote one. The SEH guard
    // turns a crash from a race with the game's own thread into "this
    // damage application silently failed" rather than taking the game down.
    __try {
        CPlayerPed* ped = FindPlayerPed_RGL(-1);
        if (!ped) return;

        ped->m_fArmour -= armourDelta;
        if (ped->m_fArmour < 0.0f) ped->m_fArmour = 0.0f;

        ped->m_fHealth -= healthDelta;
        if (ped->m_fHealth < 0.0f) ped->m_fHealth = 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Pointer went stale mid-write; nothing more to do here - the next
        // successful Update() call picks up whatever the real state is, and
        // the next PlayerState broadcast carries it to everyone else.
    }
}

} // namespace ao
