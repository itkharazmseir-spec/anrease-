#pragma once

#include <windows.h>

// Crash fix (2026-11-27): a real, 100%-reproducible crash was confirmed via
// Windows Event Viewer during user testing with FakePlayer.exe - the moment
// a remote player's first PlayerState arrives and RemotePedManager::SpawnPed
// calls AddPed_RGL (or SpawnVehicle calls CreateCarForScript_RGL), the game
// itself crashes: Faulting application/module name "gta_sa.exe" (NOT this
// project's own .asi), exception code 0xc0000005 (access violation), fault
// offset 0x001e4e00 - an address region distinct from any of this project's
// own verified addresses. This matches exactly the risk already disclosed in
// RemotePeds.h's class comment: AddPed/CreateCarForScript allocate into and
// mutate the game's own global ped/vehicle pools from our background polling
// thread, with no verified-safe "main thread, once per frame" hook to call
// them from instead (see Main.cpp's RGL BUILD NOTICE - Events::gameProcessEvent
// and friends were explicitly not pursued, since patching the wrong CALL site
// in an unanalysed build is itself a likely crash). If the game's own main
// thread is concurrently touching the same pool/list structures when our
// thread inserts into them, the result is memory corruption that can - and,
// per this confirmed crash, does - surface anywhere else in the game's own
// code, not at the call site itself.
//
// MainThreadSuspendGuard closes this specific race WITHOUT needing any new,
// unverified, RGL-specific game address at all: instead of finding a hook
// inside the game's code, it suspends the game's own main thread (its id
// captured once, at plugin load time - see g_mainThreadId in Main.cpp) using
// only standard, version-independent Win32 thread APIs for the few
// microseconds the wrapped call takes. While the main thread is suspended it
// provably cannot be reading or writing the pool at the same time we are, so
// there is no race during that window - this is a different kind of
// guarantee than every other mitigation in this project (SEH guards, byte
// fingerprints), which only make an existing risk survivable/detectable
// rather than removing it.
//
// Scope this as tightly as possible: wrap ONLY the AddPed_RGL/
// CreateCarForScript_RGL call itself (see RemotePeds.cpp's SpawnPed/
// SpawnVehicle) - these are the true pool-mutating operations, and they only
// happen once per spawn event (a player joining, or switching ped/vehicle),
// not per tick. Do NOT wrap SetHeading_RGL or the routine position/heading/
// health/armour field writes in Tick()/OnNetworkUpdate: those touch an
// already-valid object's own fields rather than pool/list structure, are
// lower severity (per RemotePeds.h's existing risk disclosure), and happen
// far more often (every ~50ms per remote avatar) - wrapping those too would
// mean freezing the game's main thread dozens of times a second per remote
// player, for a much smaller safety benefit than the one-time spawn calls.
//
// Residual risk, disclosed rather than hidden: if the main thread happens to
// be suspended mid-way through its own call into a Windows CRT/heap lock
// (e.g. mid-malloc/mid-free) that our own wrapped call then also needs (both
// AddPed and CreateCarForScript allocate), this deadlocks instead of
// crashing - the heap lock is only held for the brief duration of an actual
// allocation, so this is rare, but it is not zero. A deadlock hangs the game
// (visible immediately, recoverable via Task Manager) rather than silently
// corrupting memory and crashing somewhere unrelated later - judged an
// acceptable trade against a currently 100%-reproducible crash, but this is
// a mitigation based on a real, well-precedented technique (suspend-inject-
// resume, used by various instrumentation/profiling tools for the same
// reason), not a mathematically proven fix. Real-world re-testing (ideally
// again with FakePlayer.exe) is what actually confirms whether it works.
namespace ao {

class MainThreadSuspendGuard {
public:
    // mainThreadId: the game's real main thread id, captured once via
    // GetCurrentThreadId() inside the AndreasOnlineClient constructor in
    // Main.cpp - which, per the ASI Loader's standard, well-documented
    // convention (true for every GTA .asi loader, independent of this RGL
    // build's own unverified addresses), runs synchronously on the game's
    // real main thread while the .asi is being loaded during the game's own
    // startup. 0 means "not yet captured" (should not normally happen, since
    // Main.cpp sets it before starting the polling thread that is the only
    // caller of this class) - treated the same as "unknown", below.
    explicit MainThreadSuspendGuard(DWORD mainThreadId) {
        // Never suspend ourselves (would deadlock immediately, not just
        // rarely), and do nothing at all if the id is unknown - calling the
        // wrapped function unprotected is exactly today's existing,
        // already-disclosed risk, not a new one, so there is nothing worse
        // to fall back to here.
        if (mainThreadId == 0 || mainThreadId == GetCurrentThreadId()) {
            return;
        }
        m_handle = OpenThread(THREAD_SUSPEND_RESUME, FALSE, mainThreadId);
        if (m_handle) {
            SuspendThread(m_handle);
        }
    }

    ~MainThreadSuspendGuard() {
        if (m_handle) {
            ResumeThread(m_handle);
            CloseHandle(m_handle);
        }
    }

    MainThreadSuspendGuard(const MainThreadSuspendGuard&) = delete;
    MainThreadSuspendGuard& operator=(const MainThreadSuspendGuard&) = delete;

private:
    HANDLE m_handle = nullptr;
};

} // namespace ao
