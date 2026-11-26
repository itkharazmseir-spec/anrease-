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
// Remote players' PlayerState updates are only logged for now (see
// NetworkClient::HandlePacket) - actually spawning/moving a CPed for them
// is step 3, and is blocked on finding CPopulation::AddPed's real address
// for this build (the RGL exe turned out to be ~3x the size of 1.0 US with
// this whole code region restructured - see ao/GameAddrs.h notes once that
// search resumes). Until then, remote players are made visible a different
// way that needs ZERO new game addresses: a standalone Win32 overlay window
// (ao::Overlay) showing connection status, the player roster, and chat -
// drawn entirely outside the game's own renderer, so it works regardless of
// whether a drawingEvent/AddPed address is ever found. See Overlay.h.

#include "plugin.h"

#include "ao/Version.h"
#include "ao/GameAddrs.h"
#include "ao/NetworkClient.h"
#include "ao/Overlay.h"
#include "LocalPlayer.h"
#include "Log.h"
#include "NetworkConfig.h"

#include <cstdio>
#include <string>
#include <windows.h>

using namespace plugin;

namespace {

ao::LocalPlayer   g_localPlayer;
ao::NetworkClient g_network;
ao::Overlay       g_overlay;

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
