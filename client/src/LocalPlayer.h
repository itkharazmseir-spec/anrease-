#pragma once

#include <cstdint>

enum eWeaponType : unsigned int; // see plugin-sdk's game_sa/enums/eWeaponType.h -
                                   // forward-declared here so this header doesn't
                                   // need to pull in the weapon headers itself.

// Reads the local player's state from game memory once per frame and keeps a
// plain-data copy. Nothing outside this file touches CPed/CVehicle directly
// for the local player - in step 2 this snapshot is what gets serialised and
// sent to the server.
namespace ao {

struct PlayerSnapshot {
    bool  valid       = false;  // false while in main menu / loading / no ped yet
    float x = 0.f, y = 0.f, z = 0.f;
    float headingRad  = 0.f;    // CPed::m_fHeadingCurrent, radians
    bool  inVehicle   = false;
    int   vehicleModel = -1;    // model index when inVehicle, else -1
    int   pedModel    = 0;      // CEntity::m_nModelIndex of the local ped
                                 // itself (0 = MODEL_PLAYER/"CJ" by default)
    // RGL build: CGame::currArea's plugin-sdk address is not verified for
    // this exe (plugin-sdk resolves it to a null pointer for any version it
    // doesn't recognise, which would crash). Left at 0 until a real address
    // is found - see client/include/ao/GameAddrs.h for how to find one.
    int   interior    = 0;
    float health      = 0.f;
    float armour      = 0.f;
};

class LocalPlayer {
public:
    // Call from the game thread (Events::gameProcessEvent in the original
    // 1.0-only design; on the RGL build this is instead called from the
    // polling thread started in Main.cpp - see the comment there for why).
    void Update();

    // Item 5 ("weapon/damage sync", 2026-11-27): applies network-reported
    // damage directly to the local player's own real CPed, clamped to
    // [0, current value] (never heals). Called from the polling thread
    // (same as Update(), never concurrently) when a DamageReport naming us
    // as the victim arrives - see Main.cpp and RemotePeds.h's class comment
    // for the full design. Same direct-field-write risk category as every
    // other game-memory write in this project (see RemotePeds.h) - the only
    // thing new here is that it targets the LOCAL player's own ped instead
    // of only a spawned remote one.
    void ApplyDamage(float healthDelta, float armourDelta);

    // Item 6 ("weapon give", 2026-11-27): gives the local player's own real
    // CPed a weapon by writing directly into its CPed::m_aWeapons[13] array
    // (plugin-sdk-verified offset/layout - pure data, same trust level as
    // m_fHealth/m_fArmour above, not a reverse-engineered code address) -
    // see RemotePeds.h's class comment and docs/MEMORY.md for why this
    // exists instead of calling the game's own GiveWeapon function (that
    // function is itself protected by a code-virtualization layer in this
    // RGL build and cannot be called directly - see docs/MEMORY.md's
    // "weapon give (direct struct write)" writeup for the full
    // investigation). Returns false (does nothing) if weaponType has no
    // known inventory slot (e.g. WEAPONTYPE_ARMOUR, which is a pickup, not
    // a carriable weapon) or if there is currently no local player ped.
    bool GiveWeapon(eWeaponType weaponType, uint32_t ammo);

    const PlayerSnapshot& Snapshot() const { return m_snapshot; }

private:
    PlayerSnapshot m_snapshot;
};

} // namespace ao
