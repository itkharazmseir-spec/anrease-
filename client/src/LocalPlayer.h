#pragma once

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

    const PlayerSnapshot& Snapshot() const { return m_snapshot; }

private:
    PlayerSnapshot m_snapshot;
};

} // namespace ao
