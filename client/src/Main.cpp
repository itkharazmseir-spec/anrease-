// Andreas Online - client entry point (ASI plugin).
//
// plugin-sdk convention: a global object whose constructor runs when the
// ASI Loader loads this DLL. We only *register* callbacks here; all game
// access happens later inside those callbacks, on the game's main thread.
//
// --- RGL BUILD NOTICE -------------------------------------------------
// This exe (Rockstar Games Launcher build - confirmed via the embedded
// "RLAUNCHER"/"RLAUNCHER_HS" strings) is not one of the versions
// plugin-sdk recognises (PLUGIN_SGV_10US / PLUGIN_SGV_STEAM only cover up
// to the Dec-2014 "NewSteam R2" patch). Two things had to change from the
// original 1.0-only design as a result:
//
//   1. GetGameVersion()/ValidateGameVersion() are no longer used: they
//      would reject this exe outright (as they already were doing, which
//      is how this was first caught). We instead verify our own, manually
//      found FindPlayerPed address against a byte fingerprint at runtime -
//      see ao/GameAddrs.h for exactly how it was found and why it's
//      trustworthy.
//
//   2. Events::gameProcessEvent / drawingEvent / initGameEvent /
//      shutdownRwEvent are no longer registered. Those hook specific CALL
//      instructions inside specific game functions that we have NOT found
//      for this build yet (unlike FindPlayerPed, they don't have the same
//      "called from everywhere, easy to fingerprint by xref count"
//      property - they're one-off call sites). Registering them with the
//      old 1.0 addresses would patch the wrong bytes in this exe and very
//      likely crash the game on load.
//
//      Instead, a plain Win32 thread polls LocalPlayer::Update() every
//      50ms. This gets position/health/vehicle tracking and file logging
//      working without needing those four addresses at all. The trade-off
//      is the on-screen F9 overlay: safely drawing with CFont/RenderWare
//      needs to happen on the game's own thread at a point the engine
//      considers safe to render (that's what drawingEvent provided) - a
//      separate polling thread cannot do this safely, so the overlay is
//      disabled rather than shipped as a likely crash. DebugOverlay.*/
//      the F9 toggle are left in the tree, unused, for whoever finds the
//      real drawingEvent address later.
//
// --- NETWORKING (step 2) -----------------------------------------------
// The same polling thread now also pumps NetworkClient::Poll() and sends a
// PlayerState snapshot ~10 times/sec once the handshake is accepted.
// Connecting is entirely best-effort: if AndreasOnline.cfg is missing, or
// points at an address nobody is listening on, the plugin logs that and
// keeps working exactly as it did before networking existed - position
// tracking and the local log are never affected by the network state.
// A standalone Win32 overlay window (ao::Overlay) also shows connection
// status, the player roster, and chat - drawn entirely outside the game's
// own renderer, so it keeps working regardless of game-address risk below.
// See Overlay.h.
//
// --- REMOTE PEDS + VEHICLE SYNC (step 3, 2026-11-26) --------------------
// CPopulation::AddPed's real address for this RGL build was found (see
// ao/GameAddrs.h for the full evidence chain) - remote players now get a
// real, visible CPed via ao::RemotePedManager (RemotePeds.h/.cpp), spawned
// and updated from NetworkClient's PlayerState/PlayerLeft callbacks, wired
// up below. CCarCtrl::CreateCarForScript's real address was found the same
// way shortly after (also in GameAddrs.h) - a remote player who is driving
// now shows up as a real, moving CVehicle of the right model instead of a
// ped floating at the car's position; RemotePedManager switches between a
// ped and a vehicle per player as their PlayerState.vehicleModel changes.
// This is the riskiest game-memory interaction in the project so far:
// unlike every previous address (all read-only), AddPed/CreateCarForScript
// allocate into and mutate the game's own global ped/vehicle pools from this
// background polling thread, with no verified-safe main-thread hook to do
// it on instead - see RemotePeds.h for the full risk disclosure. Treat this
// as experimental: save your game before testing, and watch for crashes.
//
// --- CRASH FIX (2026-11-27) ----------------------------------------------
// User testing with FakePlayer.exe (a standalone network-test tool, not part
// of this project) surfaced the risk disclosed two paragraphs up as a real,
// 100%-reproducible crash the moment a remote player's first PlayerState
// arrives: Windows Event Viewer confirmed it as an access violation inside
// gta_sa.exe itself (not this .asi). A first attempt suspended only the
// game's assumed "main" thread by id - real re-testing showed the crash
// still happened, meaning that assumption didn't hold for this RGL build.
// RemotePedManager's AddPed/CreateCarForScript calls now instead briefly
// suspend EVERY OTHER thread in the whole process for just the duration of
// each call, via AllThreadsSuspendGuard - no "which thread is the main one"
// guess required at all. See client/src/MainThreadGuard.h for the full
// design, its revision history, and its own, different, honestly-disclosed
// residual risk (a rare deadlock instead of a crash).
//
// --- DAMAGE SYNC (item 5, 2026-11-27) -----------------------------------
// Real weapon-fire detection (CPed::GiveWeapon or equivalent) was
// investigated and could not be completed for this build - see
// docs/RGL_COMPAT_NOTES.md's "Item 5" section for the full, honest account
// (merged/un-splittable function blobs, one false-positive address, and an
// unresolvable CFG-style indirect thunk). Instead, RemotePedManager watches
// each remote avatar's CPed::m_fHealth/m_fArmour - fields this project
// already reads/writes safely - for a drop caused by OUR OWN local engine
// (we shot it, an explosion, a collision, anything) and reports it over the
// network as a new DamageReport message; the real victim's own client then
// applies it to their own real ped. No new game address is used anywhere in
// this feature.

#include "plugin.h"

#include "ao/Version.h"
#include "ao/GameAddrs.h"
#include "ao/NetworkClient.h"
#include "ao/Overlay.h"
#include "LocalPlayer.h"
#include "Log.h"
#include "NetworkConfig.h"
#include "RemotePeds.h"

#include <cstdio>
#include <string>
#include <windows.h>

using namespace plugin;

namespace {

ao::LocalPlayer      g_localPlayer;
ao::NetworkClient    g_network;
ao::Overlay          g_overlay;
ao::RemotePedManager g_remotePeds;

constexpr DWORD kPollIntervalMs    = 50;    // local player snapshot refresh rate
constexpr DWORD kLogIntervalMs     = 5000;  // unchanged from the original design
constexpr DWORD kNetSendIntervalMs = 100;   // ~10 Hz PlayerState updates to the server
constexpr DWORD kOverlaySyncMs     = 250;   // player-list/status refresh rate for the HUD

DWORD WINAPI PollThreadProc(LPVOID) {
    DWORD msSinceLastLog = 0;
    DWORD msSinceLastSend = 0;
    DWORD msSinceOverlaySync = 0;

    for (;;) {
        g_localPlayer.Update();
        g_network.Poll();

        // Unconditional, every loop iteration (every kPollIntervalMs = 50ms)
        // rather than gated behind one of the interval counters below: this
        // is what actually blends each remote avatar towards its latest
        // network keyframe, so it needs to run far more often than new
        // keyframes arrive (every kNetSendIntervalMs = 100ms) to produce
        // smooth motion instead of a visible step every other tick.
        g_remotePeds.Tick();

        const auto& s = g_localPlayer.Snapshot();

        if (s.valid && msSinceLastSend >= kNetSendIntervalMs) {
            msSinceLastSend = 0;

            ao::proto::PlayerState state{};
            state.x = s.x;
            state.y = s.y;
            state.z = s.z;
            state.headingRad = s.headingRad;
            state.health = s.health;
            state.armour = s.armour;
            state.vehicleModel = s.inVehicle ? s.vehicleModel : -1;
            state.pedModel = s.pedModel;
            g_network.SendPlayerState(state);
        }

        if (s.valid && msSinceLastLog >= kLogIntervalMs) {
            msSinceLastLog = 0;
            ao::log::Info("pos %.3f %.3f %.3f  heading %.3f rad  veh %d",
                          s.x, s.y, s.z, s.headingRad,
                          s.inVehicle ? s.vehicleModel : -1);
        }

        if (msSinceOverlaySync >= kOverlaySyncMs) {
            msSinceOverlaySync = 0;

            char statusBuf[160];
            if (g_network.IsReady()) {
                std::snprintf(statusBuf, sizeof(statusBuf),
                              "AndreasOnline - connected (id %u)", g_network.LocalPlayerId());
            } else if (g_network.IsConnected()) {
                std::snprintf(statusBuf, sizeof(statusBuf), "AndreasOnline - connecting...");
            } else {
                std::snprintf(statusBuf, sizeof(statusBuf),
                              "AndreasOnline - no server (see AndreasOnline.cfg)");
            }
            g_overlay.SetStatus(statusBuf);
            g_overlay.SetPlayers(g_network.GetPlayers());
        }

        Sleep(kPollIntervalMs);
        msSinceLastLog += kPollIntervalMs;
        msSinceLastSend += kPollIntervalMs;
        msSinceOverlaySync += kPollIntervalMs;
    }
    return 0;
}

class AndreasOnlineClient {
public:
    AndreasOnlineClient() {
        // GetPluginDirRelativePathA returns a pointer to an internal buffer:
        // copy it immediately.
        const std::string logPath = paths::GetPluginDirRelativePathA("AndreasOnline.log");
        ao::log::Init(logPath);
        ao::log::Info("%s %s loading (protocol %u)",
                      ao::kProductName, ao::kVersionString,
                      static_cast<unsigned>(ao::kProtocolVersion));
        ao::log::Info("RGL build detected (plugin-sdk version check bypassed - "
                      "see Main.cpp/GameAddrs.h)");

        if (!ao::VerifyFindPlayerPedFingerprint()) {
            ao::log::Error("FindPlayerPed fingerprint mismatch - game was "
                           "updated since this build's addresses were "
                           "verified. Plugin disabled; re-run the Ghidra "
                           "analysis in GameAddrs.h and update the address "
                           "before re-enabling.");
            return;
        }

        // Wire the overlay's chat input up to the network layer, and route
        // every incoming ChatMessage (including the echo of our own sent
        // lines - the server broadcasts to everyone, sender included) back
        // to the overlay's chat log. Both callbacks must be set before
        // Start()/the polling thread begins.
        g_overlay.SetSendCallback([](const std::string& text) {
            g_network.SendChatMessage(text);
        });
        g_network.SetChatCallback([](uint32_t playerId, const std::string& text) {
            char line[200];
            std::snprintf(line, sizeof(line), "[%u] %s", playerId, text.c_str());
            g_overlay.AddChatLine(line);
        });

        // Wire remote-player PlayerState/PlayerLeft to RemotePedManager, so
        // other players get a real CPed (or CVehicle while driving) - see
        // RemotePeds.h for how and the risk disclosure. If either fingerprint
        // doesn't match (game updated since 2026-11-26), RemotePedManager
        // itself refuses every spawn attempt for that one rather than
        // calling into unknown code - these are only log lines here, they do
        // not disable the rest of the plugin.
        if (!ao::VerifyAddPedFingerprint()) {
            ao::log::Error("AddPed fingerprint mismatch - remote players will "
                           "not get a real in-game character (overlay roster "
                           "still works). Re-run the Ghidra analysis in "
                           "GameAddrs.h and update the address.");
        }
        if (!ao::VerifyCreateCarForScriptFingerprint()) {
            ao::log::Error("CreateCarForScript fingerprint mismatch - remote "
                           "players driving a vehicle will not get a real "
                           "in-game vehicle. Re-run the Ghidra analysis in "
                           "GameAddrs.h and update the address.");
        }
        if (!ao::VerifySetHeadingFingerprint()) {
            ao::log::Error("SetHeading fingerprint mismatch - remote peds/"
                           "vehicles will be positioned correctly but will "
                           "not visually turn to face the right direction. "
                           "Re-run the Ghidra analysis in GameAddrs.h and "
                           "update the address.");
        }
        g_network.SetPlayerStateCallback([](const ao::proto::PlayerState& state) {
            // Only records this as the next interpolation keyframe; it does
            // not move the ped/vehicle itself. PollThreadProc's Tick() call
            // (every 50ms) is what actually blends towards it, smoothing out
            // the ~100ms gaps between PlayerState packets instead of
            // teleporting to each one as it arrives.
            g_remotePeds.OnNetworkUpdate(state);
        });
        g_network.SetPlayerLeftCallback([](uint32_t playerId) {
            g_remotePeds.MarkDisconnected(playerId);
        });

        // Item 5 ("weapon/damage sync", 2026-11-27): RemotePedManager
        // watches each remote avatar ped for a health/armour drop caused by
        // OUR OWN local game engine (we shot it, it was in an explosion,
        // etc. - see RemotePeds.h's class comment for the full design) and
        // reports it here; we forward it to the server, which relays it to
        // the real victim's own client. Conversely, when a DamageReport
        // arrives naming US as the victim, we apply it to our own real ped.
        // No new game address is used anywhere in this - see Protocol.h's
        // DamageReport struct for why.
        g_remotePeds.SetDamageDetectedCallback([](uint32_t victimPlayerId, float healthDelta, float armourDelta) {
            g_network.SendDamageReport(victimPlayerId, healthDelta, armourDelta);
        });
        g_network.SetDamageReportCallback([](const ao::proto::DamageReport& report) {
            if (report.victimPlayerId != g_network.LocalPlayerId()) {
                return; // meant for a different client; we just saw the broadcast echo
            }
            g_localPlayer.ApplyDamage(report.healthDelta, report.armourDelta);
            ao::log::Info("took %.1f hp / %.1f armour damage from player %u",
                          report.healthDelta, report.armourDelta, report.attackerPlayerId);
        });

        g_overlay.Start();

        const std::string cfgPath = paths::GetPluginDirRelativePathA("AndreasOnline.cfg");
        const ao::NetworkConfig netCfg = ao::LoadNetworkConfig(cfgPath);
        ao::log::Info("network config: server=%s:%u name='%s' "
                      "(create/edit AndreasOnline.cfg next to this .asi to change)",
                      netCfg.serverIp.c_str(), netCfg.serverPort, netCfg.playerName.c_str());
        g_network.Connect(netCfg.serverIp, netCfg.serverPort, netCfg.playerName);

        HANDLE hThread = CreateThread(nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
        if (!hThread) {
            ao::log::Error("CreateThread failed (%lu) - plugin disabled", GetLastError());
            return;
        }
        CloseHandle(hThread); // we don't need the handle; the thread runs detached

        ao::log::Info("polling thread started (%lu ms interval)", kPollIntervalMs);
    }
} g_client;

} // namespace
