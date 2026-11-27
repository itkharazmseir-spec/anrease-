#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "ao/Protocol.h"

class CPed;
class CVehicle;
struct CVector;

// Spawns and updates a real, visible CPed - or a real, visible CVehicle, when
// the remote player is driving - for every remote player, using
// CPopulation::AddPed and CCarCtrl::CreateCarForScript (see
// client/include/ao/GameAddrs.h for how both RGL addresses were found and why
// they are trustworthy). This is what finally makes other players appear as
// actual in-game characters/vehicles instead of only the overlay roster
// (ao::Overlay) - previously documented as blocked.
//
// Vehicle sync (2026-11-26, step 3 continued): a remote player's
// PlayerState.vehicleModel tells us whether they're on foot (-1) or driving
// (a real model index). This class tracks, per player, which ONE of a ped or
// a vehicle currently represents them - never both at once, to avoid a
// floating duplicate ped standing where the driver should be:
//   - on foot: a CPed is spawned/moved, same as before.
//   - driving: a CVehicle of the right model is spawned/moved instead. The
//     old ped (if the player just got in) is hidden via HideOldPed() -
//     teleported deep underground and THEN killed (2026-11-27 revision:
//     originally this just set health to 0 like a disconnect does, but
//     real testing showed that looked exactly like the ped dying in place
//     every time a remote player got in a car; teleporting away first means
//     nothing visible happens, while the game's own cleanup still reclaims
//     the pool slot the same as before).
//   - switching from driving back to on foot: a fresh CPed is spawned. The
//     vehicle they were driving is NOT despawned or blown up - it is simply
//     forgotten and left parked where it was (it was already created with
//     CreateCarForScript's own "abandoned, locked" flags set, so the game
//     itself already treats it as a normal parked/abandoned vehicle). This
//     avoids needing any "remove vehicle" address, and avoids setting
//     CVehicle::m_fHealth = 0 to force a despawn, which would make it
//     explode (0 health = wrecked, per plugin-sdk's own comment on that
//     field) - a jarring, unexpected fireball at a player's old parking spot
//     is worse than one extra parked car sitting around.
//
// Movement smoothing (2026-11-27): PlayerState arrives over the network at
// only ~10 Hz (kNetSendIntervalMs in Main.cpp). Writing each arriving
// position straight into the ped/vehicle, as the original version did, made
// remote players visibly teleport/jump between those sparse updates instead
// of moving smoothly. OnNetworkUpdate() no longer writes position directly -
// it only records the newest network snapshot as an interpolation target.
// Tick(), called every poll-loop iteration (every 50ms, i.e. twice as often
// as PlayerState arrives), blends the ped/vehicle's rendered position and
// heading from the previous snapshot towards the latest one, timed against
// how much of the interval between those two snapshots has actually elapsed
// - the same "snapshot interpolation" technique most networked games use for
// remote entities. This needs no new game address: it only changes *when*
// and *how smoothly* the existing GetPosition()/m_fHeadingCurrent writes
// happen, not what they write to.
//
// Threading: every method here must be called from the same thread that
// calls NetworkClient::Poll() (the polling thread started in Main.cpp), and
// never concurrently - RemotePedManager itself does no locking, it relies on
// all its callers already being serialised on that one thread, the same way
// LocalPlayer::Update() and NetworkClient::Poll() already are. Tick() must be
// called from that same thread too, on every loop iteration (not just when a
// packet arrives), so interpolation keeps advancing between PlayerState
// updates.
//
// IMPORTANT - disclosed risk (see GameAddrs.h for the full writeup): there is
// no verified-safe "once per frame, on the game's own main thread" hook for
// this build, so AddPed/CreateCarForScript (and now the per-tick position/
// heading writes too) run from this background thread instead, the same as
// everything else in this project. Unlike reading memory (which can only see
// a stale/half-updated value), these calls *allocate into and mutate* the
// game's own global ped/vehicle pools and world-sector lists, and Tick() now
// writes position every ~50ms instead of only every ~100ms - a slightly
// higher rate of the same pre-existing risk, not a new kind of risk. If the
// game's main thread is concurrently iterating those same structures, that
// is a genuine data race and a crash is possible under some timings - this
// is a real, elevated risk, not a formality. Save your game before testing,
// and watch for crashes.
//
// Damage sync (item 5, 2026-11-27): a remote player's stand-in CPed here is
// a real, physically-simulated object in OUR OWN local game world - if our
// local engine damages it (we shoot it, it's caught in an explosion, a car
// hits it, anything), that damage happens only to our local copy; the real
// player elsewhere never feels it, because their own real ped is a separate
// CPed on their own machine. Tick() below watches each avatar's
// CPed::m_fHealth/m_fArmour (the same two fields OnNetworkUpdate already
// reads/writes - no new game address of any kind is used for this) for an
// unexpected drop since the last authoritative PlayerState, and reports any
// such drop via m_damageDetectedCallback. Main.cpp wires that callback to
// NetworkClient::SendDamageReport(), which the server relays to the real
// victim's own client, which applies it to their own real ped via
// LocalPlayer::ApplyDamage() - see Protocol.h's DamageReport struct for the
// full design and why this deliberately does not try to detect a specific
// weapon/shot (that would need a verified CPed::GiveWeapon-equivalent
// address, which this build does not have - see docs/RGL_COMPAT_NOTES.md).
// This only tracks ped health/armour, not vehicle health - vehicle damage
// sync is a separate, not-yet-attempted feature.
//
// Crash fix (2026-11-27): SpawnPed()/SpawnVehicle()'s AddPed_RGL/
// CreateCarForScript_RGL calls are the two true pool-mutating operations
// described in the risk disclosure two paragraphs up, and a real crash
// caused by exactly that race was confirmed via Windows Event Viewer during
// user testing (gta_sa.exe itself, access violation 0xc0000005, fault offset
// 0x001e4e00). A first mitigation attempt (suspending only one thread
// assumed to be the game's main thread) did NOT actually stop the crash in
// real re-testing - see MainThreadGuard.h's revision history for why that
// assumption likely didn't hold for this RGL build. The two calls are now
// wrapped in an AllThreadsSuspendGuard instead (see MainThreadGuard.h for
// the full design and its own, different, honestly-disclosed residual
// risk), which suspends every OTHER thread in the process - no "which
// thread is the main one" guess required at all - for just the duration of
// that one call. SetHeading_RGL and the routine per-tick field writes are
// deliberately NOT wrapped - see MainThreadGuard.h for why.
namespace ao {

class RemotePedManager {
public:
    // Called once per received PlayerState for a remote player (never for
    // the local player - filter that out before calling, as NetworkClient's
    // callback already does). Spawns a CPed or a CVehicle depending on
    // state.vehicleModel (-1 == on foot) the first time a player is seen, or
    // when they switch between the two - see the class comment above. Does
    // NOT move an already-spawned avatar directly; it only records the new
    // position/heading as the next interpolation target for Tick() to blend
    // towards.
    void OnNetworkUpdate(const proto::PlayerState& state);

    // Call every poll-loop iteration (see Main.cpp), regardless of whether a
    // new PlayerState just arrived. Advances every tracked avatar's rendered
    // position/heading a little closer to its latest network target, timed
    // against real elapsed time - see the movement-smoothing note above.
    void Tick();

    // Called once when a player disconnects (PlayerLeft). If they were
    // currently represented by a ped, sets its health to 0 and lets the
    // game's own, already-running, internal per-frame ped-death/corpse-
    // cleanup logic reclaim it over time, exactly as it would for any
    // ordinary NPC's death - no "remove ped" address needed. If they were
    // currently represented by a vehicle, it is simply left parked (see the
    // class comment above for why) - no "remove vehicle" address needed
    // either, and no forced explosion.
    void MarkDisconnected(uint32_t playerId);

    // Fired (from whichever thread calls Tick(), i.e. the polling thread)
    // whenever a remote avatar ped's health or armour is observed to have
    // dropped since the last authoritative PlayerState - see the "Damage
    // sync" note in the class comment above. Only ever reports a drop
    // (never a rise, e.g. from a pickup or script-granted health), and only
    // for peds, not vehicles. Must be set before the first Tick() call that
    // should use it - there's no queueing of anything missed before it was
    // set.
    void SetDamageDetectedCallback(std::function<void(uint32_t playerId, float healthDelta, float armourDelta)> cb);

private:
    struct RemoteAvatar {
        CPed*     ped     = nullptr; // non-null while on foot
        CVehicle* vehicle = nullptr; // non-null while driving
        int       vehicleModel = -1; // model the spawned vehicle was created with

        // Interpolation keyframes: "from" is the previous network snapshot,
        // "to" is the latest one. Tick() blends between them using how much
        // of [fromTimeMs, toTimeMs] has elapsed in real (wall-clock) time.
        // Plain floats/uint64 here, not CVector, so this header doesn't need
        // the full CVector definition (forward-declared above) - see the
        // .cpp for the actual blending math.
        float fromX = 0.0f, fromY = 0.0f, fromZ = 0.0f, fromHeading = 0.0f;
        float toX   = 0.0f, toY   = 0.0f, toZ   = 0.0f, toHeading   = 0.0f;
        unsigned long long fromTimeMs = 0;
        unsigned long long toTimeMs   = 0;

        // Damage sync (see class comment above): the last health/armour
        // value we ourselves wrote in from an authoritative PlayerState (or,
        // right after detecting a drop, the last value Tick() observed) -
        // the baseline the next Tick() compares the ped's live
        // m_fHealth/m_fArmour against to notice a new, locally-caused drop.
        float lastKnownHealth = 100.0f;
        float lastKnownArmour = 0.0f;
    };

    CPed*     SpawnPed(const proto::PlayerState& state);
    CVehicle* SpawnVehicle(const proto::PlayerState& state);
    void      RetirePed(CPed* ped);
    void      HideOldPed(CPed* ped); // teleport-then-kill, for ped->vehicle switch only - see .cpp
    void      ResetInterpolation(RemoteAvatar& avatar, float x, float y, float z, float heading, unsigned long long nowMs);

    std::unordered_map<uint32_t, RemoteAvatar> m_avatars; // playerId -> current avatar
    std::function<void(uint32_t, float, float)> m_damageDetectedCallback;
};

} // namespace ao
