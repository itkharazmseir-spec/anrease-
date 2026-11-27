#include "RemotePeds.h"

#include "ao/GameAddrs.h"
#include "CPed.h"
#include "CVehicle.h"
#include "Log.h"
#include "MainThreadGuard.h"

#include <utility>
#include <windows.h>

namespace ao {
namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

// Item 5 ("weapon/damage sync", 2026-11-27): minimum health/armour drop (in
// game units, same scale as CPed::m_fHealth/m_fArmour, i.e. 0-100 normally)
// Tick() treats as real damage rather than float-noise. A bare CPed's health
// does not drift on its own between writes, so this is a generous margin,
// not a tight tolerance.
constexpr float kDamageEpsilon = 0.5f;

// Shortest-path angular difference, wrapped into (-pi, pi]. Used so a
// heading interpolation that crosses the 0/2pi boundary (e.g. from 350
// degrees to 10 degrees) turns the short way (20 degrees) instead of the
// long way around (340 degrees).
float NormalizeAngleDelta(float delta) {
    while (delta > kPi)  delta -= kTwoPi;
    while (delta < -kPi) delta += kTwoPi;
    return delta;
}

unsigned long long NowMs() {
    return GetTickCount64();
}

// Raw calls through the verified RGL addresses, the same way FindPlayerPed_RGL
// in LocalPlayer.cpp calls through kAddr_FindPlayerPed_RGL - see GameAddrs.h
// for the full evidence chains and why these are safe to call this way
// without plugin-sdk's own (wrong-address-for-this-exe) wrappers. Both
// __cdecl, matching plugin-sdk's documented 1.0 US signatures exactly; only
// the addresses differ between builds.
using AddPedFn = CPed*(__cdecl*)(ePedType, unsigned int, const CVector&, bool);

// NOTE the CVector here is passed BY VALUE, not by reference - this matches
// CCarCtrl::CreateCarForScript's real 1.0 US signature (plugin-sdk
// CCarCtrl.h:49) and the decompiled RGL function, which takes modelid/x/y/z/
// flag as five separate stack slots rather than a pointer to a CVector.
using CreateCarForScriptFn = CVehicle*(__cdecl*)(int, CVector, unsigned char);

// CPlaceable::SetHeading - __thiscall(CPlaceable*, float). Declared against
// a void* "this" rather than CPed*/CVehicle* because neither of those
// classes' plugin-sdk headers actually expose CPlaceable as a visible base
// (the project doesn't pull in the full CEntity/CPhysical chain, just the
// narrow CPed/CVehicle slices it needs elsewhere) - calling through void*
// sidesteps needing that type at all. This is safe because CPlaceable sits
// at object offset 0 in both CPed and CVehicle (single, non-virtual
// inheritance the whole way down for both - see GameAddrs.h for the full
// chain), so a CPed* or CVehicle* is already a valid, unadjusted CPlaceable*
// "this" pointer.
using SetHeadingFn = void(__thiscall*)(void*, float);

void SetHeading_RGL(void* thisPtr, float heading) {
    if (!VerifySetHeadingFingerprint()) {
        return;
    }
    auto fn = reinterpret_cast<SetHeadingFn>(kAddr_SetHeading_RGL);
    fn(thisPtr, heading);
}

// Standard male civilian skin (plugin-sdk eModelID.h: MODEL_MALE01 = 7).
// Chosen specifically because it is a base-game model guaranteed to already
// be resident in memory from the moment the game starts, which sidesteps a
// second, separate unverified-address problem (CStreaming model-loading
// addresses for this build are not verified, so we deliberately never ask
// the game to stream in a model we aren't sure is loaded). Remote vehicles,
// by contrast, use whatever model the remote player is actually driving
// (state.vehicleModel) - that model is, by definition, already loaded on
// THEIR machine, and in practice the base vehicle set is loaded on every
// client, so no separate "is this model resident" problem exists for
// vehicles the way it would for an arbitrary ped skin.
constexpr unsigned int kRemotePedModel = 7; // MODEL_MALE01

// Item 3 ("real player models", 2026-11-27): rather than always using
// kRemotePedModel above, use the remote player's REAL model - but only for
// the two protagonist models that are, by construction, always resident on
// every connected client: MODEL_PLAYER (0, "CJ") and MODEL_CSPLAY (1, the
// alternate player-select skin used by a couple of early missions). Both
// are loaded from the moment the game boots because the local player's own
// ped always exists using one of them, on every machine, in every ordinary
// (unmodified) installation of this game - so a remote player's PC always
// having it loaded too is a safe assumption, unlike an arbitrary ped skin.
//
// This deliberately does NOT attempt general custom-skin sync. Doing that
// safely needs a model-loaded check equivalent to plugin-sdk's
// CStreaming::HasModelLoaded - which, per plugin-sdk's own source
// (CStreaming.cpp), isn't a real function call at all but a direct read of
// a global array, CStreaming::ms_aInfoForModel (1.0 US address 0x8E4CC0,
// an array of 26316 20-byte CStreamingInfo entries - see gta-reversed
// Streaming.h/StreamingInfo.h). Finding ITS real address for this RGL
// build needs a byte-pattern/immediate-constant search tool (to anchor on
// e.g. the 26316-entry array bound) that wasn't available this session, so
// it's left as a documented gap rather than guessed. Any pedModel value
// outside the two allowed here safely falls back to kRemotePedModel below
// instead of risking a crash from an unloaded model.
constexpr int kModelPlayerCJ    = 0; // MODEL_PLAYER
constexpr int kModelCutsceneCJ  = 1; // MODEL_CSPLAY

unsigned int ResolveRemotePedModel(int32_t requestedPedModel) {
    if (requestedPedModel == kModelPlayerCJ || requestedPedModel == kModelCutsceneCJ) {
        return static_cast<unsigned int>(requestedPedModel);
    }
    return kRemotePedModel;
}

CPed* AddPed_RGL(ePedType pedType, unsigned int modelIndex, const CVector& pos, bool makeWander) {
    if (!VerifyAddPedFingerprint()) {
        // Game updated since this was verified, or this isn't the exe we
        // analysed - refuse to call into unknown code.
        return nullptr;
    }
    auto fn = reinterpret_cast<AddPedFn>(kAddr_AddPed_RGL);
    return fn(pedType, modelIndex, pos, makeWander);
}

CVehicle* CreateCarForScript_RGL(int modelId, const CVector& pos, unsigned char doMissionCleanup) {
    if (!VerifyCreateCarForScriptFingerprint()) {
        return nullptr;
    }
    auto fn = reinterpret_cast<CreateCarForScriptFn>(kAddr_CreateCarForScript_RGL);
    return fn(modelId, pos, doMissionCleanup);
}

} // namespace

void RemotePedManager::ResetInterpolation(RemoteAvatar& avatar, float x, float y, float z, float heading, unsigned long long nowMs) {
    // "from" == "to" and a zero-length window means Tick()'s very next call
    // just renders (x, y, z, heading) exactly - no stale old position to
    // blend away from right after a fresh spawn or a ped/vehicle switch.
    avatar.fromX = avatar.toX = x;
    avatar.fromY = avatar.toY = y;
    avatar.fromZ = avatar.toZ = z;
    avatar.fromHeading = avatar.toHeading = heading;
    avatar.fromTimeMs = avatar.toTimeMs = nowMs;
}

CPed* RemotePedManager::SpawnPed(const proto::PlayerState& state) {
    CPed* ped = nullptr;

    // See RemotePeds.h for the full risk disclosure, and MainThreadGuard.h
    // for the crash-fix design: MainThreadSuspendGuard briefly suspends the
    // game's real main thread for just this one call, so there is provably
    // no other thread touching the pool while we insert into it - this is
    // what actually closes the race a real, confirmed crash exposed (see
    // MainThreadGuard.h), not just the SEH guard below, which only makes a
    // crash survivable/attributable rather than preventing it.
    __try {
        MainThreadSuspendGuard mainThreadGuard(m_mainThreadId);
        ped = AddPed_RGL(PED_TYPE_CIVMALE, ResolveRemotePedModel(state.pedModel),
                         CVector(state.x, state.y, state.z),
                         /*makeWander=*/false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Error("RemotePedManager: AddPed crashed for player %u - skipping this attempt", state.playerId);
        return nullptr;
    }

    if (!ped) {
        log::Error("RemotePedManager: AddPed returned null for player %u "
                   "(fingerprint mismatch, or the game itself refused - "
                   "out of pool slots?)", state.playerId);
        return nullptr;
    }

    log::Info("RemotePedManager: spawned ped for player %u at %.3f %.3f %.3f",
              state.playerId, state.x, state.y, state.z);
    return ped;
}

CVehicle* RemotePedManager::SpawnVehicle(const proto::PlayerState& state) {
    CVehicle* veh = nullptr;

    // Same risk and same fix as SpawnPed above - see RemotePeds.h and
    // MainThreadGuard.h.
    __try {
        MainThreadSuspendGuard mainThreadGuard(m_mainThreadId);
        veh = CreateCarForScript_RGL(state.vehicleModel,
                                     CVector(state.x, state.y, state.z),
                                     /*doMissionCleanup=*/0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Error("RemotePedManager: CreateCarForScript crashed for player %u - skipping this attempt", state.playerId);
        return nullptr;
    }

    if (!veh) {
        log::Error("RemotePedManager: CreateCarForScript returned null for "
                   "player %u (fingerprint mismatch, invalid model %d, or "
                   "the game itself refused)", state.playerId, state.vehicleModel);
        return nullptr;
    }

    log::Info("RemotePedManager: spawned vehicle (model %d) for player %u at %.3f %.3f %.3f",
              state.vehicleModel, state.playerId, state.x, state.y, state.z);
    return veh;
}

void RemotePedManager::RetirePed(CPed* ped) {
    if (!ped) return;
    __try {
        ped->m_fHealth = 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Already gone one way or another - nothing more to do.
    }
}

void RemotePedManager::OnNetworkUpdate(const proto::PlayerState& state) {
    RemoteAvatar& avatar = m_avatars[state.playerId]; // default-constructed on first sight
    const bool wantsVehicle = state.vehicleModel >= 0;
    const unsigned long long now = NowMs();

    if (wantsVehicle) {
        // Switched (or newly joined) into a vehicle: retire any on-foot ped
        // first so we never show both at once.
        if (avatar.ped) {
            RetirePed(avatar.ped);
            avatar.ped = nullptr;
        }

        // Spawn a new vehicle if we don't have one yet, or if the player
        // switched to a different vehicle model (e.g. got into a different
        // car) - we have no "change model in place" address, so a model
        // change means a fresh spawn; the old vehicle is left parked, same
        // as the on-foot transition described in RemotePeds.h.
        if (!avatar.vehicle || avatar.vehicleModel != state.vehicleModel) {
            avatar.vehicle = SpawnVehicle(state);
            avatar.vehicleModel = state.vehicleModel;
            ResetInterpolation(avatar, state.x, state.y, state.z, state.headingRad, now);
            return;
        }

        // Already have the right vehicle - record this as the next
        // interpolation target; Tick() blends towards it over time instead
        // of snapping straight there (see the movement-smoothing note in
        // RemotePeds.h). No heading write is attempted for vehicles, same
        // as before this change - there is no known vehicle-equivalent of
        // CPed::m_fHeadingCurrent to even try.
        avatar.fromX = avatar.toX; avatar.fromY = avatar.toY; avatar.fromZ = avatar.toZ;
        avatar.fromHeading = avatar.toHeading;
        avatar.fromTimeMs = avatar.toTimeMs;
        avatar.toX = state.x; avatar.toY = state.y; avatar.toZ = state.z;
        avatar.toHeading = state.headingRad;
        avatar.toTimeMs = now;
        return;
    }

    // On foot.
    if (avatar.vehicle) {
        // Switched out of the vehicle: forget it (left parked, see
        // RemotePeds.h) and fall through to spawn a fresh ped below.
        avatar.vehicle = nullptr;
        avatar.vehicleModel = -1;
    }

    if (!avatar.ped) {
        avatar.ped = SpawnPed(state);
        ResetInterpolation(avatar, state.x, state.y, state.z, state.headingRad, now);

        if (avatar.ped) {
            // Apply the sender's real health/armour immediately rather than
            // leaving the freshly-spawned ped at whatever default the game
            // itself gives a new CPed (observed ~100/0) until the *next*
            // PlayerState arrives. This also gives item 5's damage-drop
            // baseline (lastKnownHealth/Armour) a correct starting value
            // from the very first Tick(), instead of possibly misreading
            // "spawned at 100, sender says 60" as 40 points of locally-
            // caused damage that never actually happened.
            __try {
                avatar.ped->m_fHealth = state.health;
                avatar.ped->m_fArmour = state.armour;
                avatar.lastKnownHealth = state.health;
                avatar.lastKnownArmour = state.armour;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                log::Error("RemotePedManager: stale ped pointer for player %u right after spawn - will respawn", state.playerId);
                avatar.ped = nullptr;
            }
        }
        return;
    }

    // Already spawned: record this as the next interpolation target (see
    // above) and apply health/armour immediately - those are not jittery in
    // the same way continuous movement is, so there is no need to smooth
    // them, just write them straight through as before.
    avatar.fromX = avatar.toX; avatar.fromY = avatar.toY; avatar.fromZ = avatar.toZ;
    avatar.fromHeading = avatar.toHeading;
    avatar.fromTimeMs = avatar.toTimeMs;
    avatar.toX = state.x; avatar.toY = state.y; avatar.toZ = state.z;
    avatar.toHeading = state.headingRad;
    avatar.toTimeMs = now;

    __try {
        avatar.ped->m_fHealth = state.health;
        avatar.ped->m_fArmour = state.armour;
        // This is the authoritative truth from the real owning client - reset
        // item 5's drop-detection baseline to match exactly, so nothing about
        // this write itself (nor any drift Tick() hasn't reported yet) gets
        // misread as a fresh drop on the next Tick().
        avatar.lastKnownHealth = state.health;
        avatar.lastKnownArmour = state.armour;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The ped pointer we cached may have been freed/recycled by the game
        // itself (e.g. it died some other way, or the pool slot was reused).
        // Forget it so the next PlayerState spawns a fresh one instead of
        // repeatedly faulting on a dangling pointer.
        log::Error("RemotePedManager: stale ped pointer for player %u - will respawn", state.playerId);
        avatar.ped = nullptr;
    }
}

void RemotePedManager::Tick() {
    const unsigned long long now = NowMs();

    for (auto it = m_avatars.begin(); it != m_avatars.end(); ++it) {
        RemoteAvatar& avatar = it->second;
        if (!avatar.ped && !avatar.vehicle) continue;

        // Blend factor across [fromTimeMs, toTimeMs]. duration == 0 (right
        // after a fresh spawn/switch, see ResetInterpolation) means t stays
        // at its default 1.0 - render the target exactly, nothing to blend
        // from yet.
        float t = 1.0f;
        if (avatar.toTimeMs > avatar.fromTimeMs) {
            const unsigned long long duration = avatar.toTimeMs - avatar.fromTimeMs;
            const unsigned long long elapsed = (now > avatar.fromTimeMs) ? (now - avatar.fromTimeMs) : 0;
            t = static_cast<float>(elapsed) / static_cast<float>(duration);
            // Allow a little overshoot (mild extrapolation) when the next
            // network update is running late, so motion doesn't visibly
            // freeze the instant one packet is overdue; clamp hard past
            // that so a real stall (lag spike, disconnect-in-progress)
            // doesn't fling the avatar off into the distance.
            if (t < 0.0f) t = 0.0f;
            if (t > 1.5f) t = 1.5f;
        }

        const float x = avatar.fromX + (avatar.toX - avatar.fromX) * t;
        const float y = avatar.fromY + (avatar.toY - avatar.fromY) * t;
        const float z = avatar.fromZ + (avatar.toZ - avatar.fromZ) * t;
        const float headingDelta = NormalizeAngleDelta(avatar.toHeading - avatar.fromHeading);
        const float heading = avatar.fromHeading + headingDelta * t;

        if (avatar.vehicle) {
            __try {
                avatar.vehicle->GetPosition() = CVector(x, y, z);
                // CPlaceable::SetHeading rewrites the rotation matrix itself
                // (via CMatrix::SetRotateZOnly) - unlike the old
                // ped-only m_fHeadingCurrent write, this is what the game
                // actually renders, and it works for a vehicle for the same
                // reason it works for a ped: CPlaceable sits at object
                // offset 0 in both (see GameAddrs.h). This removes the
                // "vehicle may not visually point the right way" limitation
                // noted in RGL_COMPAT_NOTES.md.
                SetHeading_RGL(avatar.vehicle, heading);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                log::Error("RemotePedManager: stale vehicle pointer for player %u during Tick - will respawn", it->first);
                avatar.vehicle = nullptr;
            }
        } else if (avatar.ped) {
            __try {
                avatar.ped->GetPosition() = CVector(x, y, z);
                // Real rotation (see above), not just the inert
                // m_fHeadingCurrent field - this removes the "ped may not
                // visually face the right direction yet" limitation noted
                // in RGL_COMPAT_NOTES.md. m_fHeadingCurrent is also still
                // kept in sync so the game's own AI heading-blend logic
                // (which reads it elsewhere) doesn't fight the next
                // SetHeading call by trying to blend away from a stale
                // value.
                SetHeading_RGL(avatar.ped, heading);
                avatar.ped->m_fHeadingCurrent = heading;

                // Item 5 ("weapon/damage sync", 2026-11-27) - see the class
                // comment in RemotePeds.h for the full design. This reads the
                // exact same m_fHealth/m_fArmour fields already written just
                // above/in OnNetworkUpdate, so it adds no new game-memory
                // access of any kind, only a comparison against our own last
                // recorded baseline.
                const float currentHealth = avatar.ped->m_fHealth;
                const float currentArmour = avatar.ped->m_fArmour;
                const float healthDrop = avatar.lastKnownHealth - currentHealth;
                const float armourDrop = avatar.lastKnownArmour - currentArmour;
                if ((healthDrop > kDamageEpsilon || armourDrop > kDamageEpsilon) && m_damageDetectedCallback) {
                    m_damageDetectedCallback(it->first,
                                              healthDrop > 0.0f ? healthDrop : 0.0f,
                                              armourDrop > 0.0f ? armourDrop : 0.0f);
                }
                // Always re-baseline to what we just observed - whether or
                // not a callback is wired up, and whether the change was a
                // drop (damage, now reported above) or a rise (e.g. a
                // pickup/script effect on their machine, which this
                // deliberately does not sync - see the class comment).
                avatar.lastKnownHealth = currentHealth;
                avatar.lastKnownArmour = currentArmour;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                log::Error("RemotePedManager: stale ped pointer for player %u during Tick - will respawn", it->first);
                avatar.ped = nullptr;
            }
        }
    }
}

void RemotePedManager::SetDamageDetectedCallback(std::function<void(uint32_t, float, float)> cb) {
    m_damageDetectedCallback = std::move(cb);
}

void RemotePedManager::SetMainThreadId(unsigned long mainThreadId) {
    m_mainThreadId = mainThreadId;
}

void RemotePedManager::MarkDisconnected(uint32_t playerId) {
    auto it = m_avatars.find(playerId);
    if (it == m_avatars.end()) return;

    if (it->second.ped) {
        RetirePed(it->second.ped);
        log::Info("RemotePedManager: player %u disconnected, ped left to the game's own death/cleanup logic", playerId);
    } else if (it->second.vehicle) {
        log::Info("RemotePedManager: player %u disconnected while driving - vehicle left parked", playerId);
    }

    m_avatars.erase(it);
}

} // namespace ao
