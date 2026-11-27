#include "LocalPlayer.h"

#include "ao/GameAddrs.h"
#include "CPlayerPed.h"
#include "CVehicle.h"
#include "CWeapon.h"
#include "eWeaponType.h"
// CGame.h intentionally not included: CGame::currArea is not safe on this
// build (see GameAddrs.h and LocalPlayer.h) and nothing else here needs it.

namespace ao {
namespace {

// Item 6 ("weapon give", 2026-11-27): eWeaponType -> CPed::m_aWeapons[]
// slot index (0-12). This is NOT reverse-engineered from this binary - it
// is GTA San Andreas's own, publicly documented weapon.dat slot assignment,
// identical across every copy/version of the game since it's game-design
// data, not compiled code (same category of "already public, not this
// project's own discovery" as the eWeaponType enum values themselves,
// which also come straight from plugin-sdk). Returns -1 for a type that
// isn't a real carriable weapon (e.g. WEAPONTYPE_ARMOUR is a pickup, not a
// weapon slot) or isn't in this table yet.
int WeaponSlotFor(eWeaponType weaponType) {
    switch (weaponType) {
        case WEAPONTYPE_UNARMED:
            return 0;
        case WEAPONTYPE_BRASSKNUCKLE:
        case WEAPONTYPE_GOLFCLUB:
        case WEAPONTYPE_NIGHTSTICK:
        case WEAPONTYPE_KNIFE:
        case WEAPONTYPE_BASEBALLBAT:
        case WEAPONTYPE_SHOVEL:
        case WEAPONTYPE_POOLCUE:
        case WEAPONTYPE_KATANA:
        case WEAPONTYPE_CHAINSAW:
        case WEAPONTYPE_DILDO1:
        case WEAPONTYPE_DILDO2:
        case WEAPONTYPE_VIBE1:
        case WEAPONTYPE_VIBE2:
        case WEAPONTYPE_FLOWERS:
        case WEAPONTYPE_CANE:
            return 1;
        case WEAPONTYPE_PISTOL:
        case WEAPONTYPE_PISTOL_SILENCED:
        case WEAPONTYPE_DESERT_EAGLE:
            return 2;
        case WEAPONTYPE_SHOTGUN:
        case WEAPONTYPE_SAWNOFF:
        case WEAPONTYPE_SPAS12:
            return 3;
        case WEAPONTYPE_MICRO_UZI:
        case WEAPONTYPE_MP5:
        case WEAPONTYPE_TEC9:
            return 4;
        case WEAPONTYPE_AK47:
        case WEAPONTYPE_M4:
            return 5;
        case WEAPONTYPE_COUNTRYRIFLE:
        case WEAPONTYPE_SNIPERRIFLE:
            return 6;
        case WEAPONTYPE_RLAUNCHER:
        case WEAPONTYPE_RLAUNCHER_HS:
        case WEAPONTYPE_FTHROWER:
        case WEAPONTYPE_MINIGUN:
            return 7;
        case WEAPONTYPE_GRENADE:
        case WEAPONTYPE_TEARGAS:
        case WEAPONTYPE_MOLOTOV:
        case WEAPONTYPE_FREEFALL_BOMB:
            return 8;
        case WEAPONTYPE_SATCHEL_CHARGE:
            return 9;
        case WEAPONTYPE_DETONATOR:
            return 10;
        case WEAPONTYPE_SPRAYCAN:
        case WEAPONTYPE_EXTINGUISHER:
        case WEAPONTYPE_CAMERA:
            return 11;
        case WEAPONTYPE_NIGHTVISION:
        case WEAPONTYPE_INFRARED:
        case WEAPONTYPE_PARACHUTE:
            return 12;
        default:
            return -1;
    }
}

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

bool LocalPlayer::GiveWeapon(eWeaponType weaponType, uint32_t ammo) {
    const int slot = WeaponSlotFor(weaponType);
    if (slot < 0) return false;

    __try {
        CPlayerPed* ped = FindPlayerPed_RGL(-1);
        if (!ped) return false;

        // Direct struct write into CPed::m_aWeapons[slot] (plugin-sdk
        // VALIDATE_OFFSET(CPed, m_aWeapons, 0x5A0), CWeapon size 0x1C) -
        // deliberately does NOT call the game's own GiveWeapon(), which is
        // protected by a code-virtualization layer in this RGL build (see
        // docs/MEMORY.md). m_pFxSystem is intentionally left untouched (not
        // zeroed, not freed) - if this slot previously held a weapon with
        // an active particle system (flamethrower/spraycan/extinguisher,
        // slots 7/11) this is a known, disclosed gap (see docs/MEMORY.md);
        // the realistic store weapon list (pistols/rifles/etc.) never hits
        // those slots, so this is not expected to come up in practice yet.
        CWeapon& w = ped->m_aWeapons[slot];
        w.m_eWeaponType = weaponType;
        w.m_nState = WEAPONSTATE_READY;
        // No per-weapon clip-size table exists yet (would need CWeaponInfo
        // data - see docs/MEMORY.md) - both fields are simply set to the
        // requested ammo count, same simplification many existing SA
        // trainers/mods use. Functional (the weapon fires normally and
        // reloads when the clip empties) even though the displayed clip
        // count can be larger than the weapon's "real" clip size.
        w.m_nAmmoInClip = ammo;
        w.m_nAmmoTotal = ammo;
        w.m_nTimeForNextShot = 0;

        // 2026-11-27, found during the first real in-game test of this
        // feature: writing the weapon into m_aWeapons[slot] alone does NOT
        // make the game hold/show it - CPed::m_nSelectedWepSlot (plugin-sdk
        // VALIDATE_OFFSET(CPed, m_nSelectedWepSlot, 0x718)) is a separate
        // field that says which slot is *currently equipped*, and nothing
        // above touches it. Without this, a given weapon sits correctly in
        // the inventory array but the player keeps visibly holding whatever
        // they already had out, with no indication anything happened unless
        // they manually scroll/TAB to the new weapon. Switching to the new
        // slot here is what a store purchase (or /giveweapon) should
        // reasonably do - immediately equip what was just given.
        ped->m_nSelectedWepSlot = static_cast<unsigned char>(slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace ao
