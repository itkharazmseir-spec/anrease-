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
// is step 3.

#include "plugin.h"

#include "ao/Version.h"
#include "ao/GameAddrs.h"
#include "ao/NetworkClient.h"
#include "LocalPlayer.h"
#include "Log.h"
#include "NetworkConfig.h"

#include <string>
#include <windows.h>

using namespace plugin;

namespace {

ao::LocalPlayer   g_localPlayer;
ao::NetworkClient g_network;

constexpr DWORD kPollIntervalMs    = 50;    // local player snapshot refresh rate
constexpr DWORD kLogIntervalMs     = 5000;  // unchanged from the original design
constexpr DWORD kNetSendIntervalMs = 100;   // ~10 Hz PlayerState updates to the server

DWORD WINAPI PollThreadProc(LPVOID) {
    DWORD msSinceLastLog = 0;
    DWORD msSinceLastSend = 0;

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

        Sleep(kPollIntervalMs);
        msSinceLastLog += kPollIntervalMs;
        msSinceLastSend += kPollIntervalMs;
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
