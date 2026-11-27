#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <vector>

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
// --- Revision history of this fix (both still here so the reasoning is not
// lost - only AllThreadsSuspendGuard, the second one, is actually used) ---
//
// v1 (first attempt, 2026-11-27 morning): MainThreadSuspendGuard suspended
// only ONE thread - the game's main thread, identified by capturing
// GetCurrentThreadId() once inside the AndreasOnlineClient constructor in
// Main.cpp, relying on the assumption that the ASI Loader loads a plugin's
// global constructor synchronously on the game's real main thread. Real
// user re-testing after this was deployed showed the EXACT same crash
// pattern recurring (player joins, game crashes/disconnects within
// seconds) - meaning that assumption was most likely wrong for however
// this specific RGL build's loader actually works (this project has
// already had to work around several other RGL-specific surprises - see
// Main.cpp's RGL BUILD NOTICE - so a non-standard loader thread is
// plausible here too), or some OTHER thread besides "the main thread" is
// also touching the ped/vehicle pool. Either way, "suspend the one thread
// we assumed was the main thread" left the real race open.
//
// v2 (this version, 2026-11-27, same day): AllThreadsSuspendGuard makes NO
// assumption at all about which thread is "the" main thread, or how many
// threads might touch the pool. It suspends EVERY thread in this process
// except the one calling it (our own background polling thread), using
// CreateToolhelp32Snapshot/Thread32First/Thread32Next to enumerate them by
// process id at the moment of the call - a different, standard Win32
// technique, still no RGL-specific game address of any kind. While every
// other thread in the process is suspended, it is simply not possible for
// anything else to be touching the pool concurrently, regardless of which
// thread that would have been - this closes the race without needing to
// correctly guess which thread the v1 approach got wrong.
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
// mean freezing the entire game process dozens of times a second per remote
// player, for a much smaller safety benefit than the one-time spawn calls.
//
// Residual risk, disclosed rather than hidden: if any of the suspended
// threads happens to be paused mid-way through its own call into a Windows
// CRT/heap lock (e.g. mid-malloc/mid-free) that our own wrapped call then
// also needs (both AddPed and CreateCarForScript allocate), this deadlocks
// instead of crashing. Suspending MORE threads than v1 did means slightly
// more chances one of them is mid-allocation at the exact suspend instant,
// but each individual chance is still small (the heap lock is only held for
// the brief duration of an actual allocation) and most of a game's other
// threads (audio, streaming, etc.) are idle/sleeping most of the time. A
// deadlock hangs the game (visible immediately, recoverable via Task
// Manager) rather than silently corrupting memory and crashing somewhere
// unrelated later - judged an acceptable trade against a currently
// 100%-reproducible crash, but this is still a mitigation based on a real,
// well-precedented technique (suspend-inject-resume, used by various
// instrumentation/profiling/garbage-collection tools for exactly this
// "stop the world" reason), not a mathematically proven fix. Real-world
// re-testing (ideally again with FakePlayer.exe) is what actually confirms
// whether THIS version works.
namespace ao {

class AllThreadsSuspendGuard {
public:
    AllThreadsSuspendGuard() {
        const DWORD selfThreadId = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            // Can't enumerate threads at all - fall back to doing nothing,
            // same as "id unknown" in v1. Calling the wrapped function
            // unprotected is exactly the pre-existing, already-disclosed
            // risk, not a new one, so there is nothing worse to fall back
            // to here.
            return;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);

        if (Thread32First(snapshot, &entry)) {
            do {
                // th32OwnerProcessID is present on every real THREADENTRY32
                // this API returns; the dwSize check is defensive, matching
                // Microsoft's own documented usage pattern for this struct.
                if (entry.dwSize >= (FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(entry.th32OwnerProcessID))) {
                    if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != selfThreadId) {
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                        if (h) {
                            SuspendThread(h);
                            m_handles.push_back(h);
                        }
                    }
                }
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snapshot, &entry));
        }

        CloseHandle(snapshot);
    }

    ~AllThreadsSuspendGuard() {
        for (HANDLE h : m_handles) {
            ResumeThread(h);
            CloseHandle(h);
        }
    }

    AllThreadsSuspendGuard(const AllThreadsSuspendGuard&) = delete;
    AllThreadsSuspendGuard& operator=(const AllThreadsSuspendGuard&) = delete;

private:
    std::vector<HANDLE> m_handles;
};

} // namespace ao
