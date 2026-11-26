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

    const PlayerSnapshot& Snapshot() const { return m_snapshot; }

private:
    PlayerSnapshot m_snapshot;
};

} // namespace ao
