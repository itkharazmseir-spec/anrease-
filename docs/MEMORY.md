# Game memory used by the client

Rule: no raw address or offset is written in AO code. Everything goes through
plugin-sdk, and every entry below must be cross-checked in **gta-reversed**
before we depend on it for networking.

Values from plugin-sdk commit `15f15b6` (2026-09-08), cross-checked against
gta-reversed commit `943c2a3` (2026-11-13). GTA SA 1.0 US.

| What | Through plugin-sdk | Address / offset (plugin-sdk) | gta-reversed (`source/game_sa/`, commit `943c2a3`) |
|---|---|---|---|
| Local player ped | `FindPlayerPed(-1)` | function `0x56E210` | `World.cpp`: `RH_ScopedGlobalInstall(FindPlayerPed, 0x56E210)` ✅ |
| Ped health | `CPed::m_fHealth` | `+0x540` (VALIDATE_OFFSET) | `Entity/Ped/Ped.h` `m_fHealth` - field present, offset not asserted there ⚠️ |
| Ped armour | `CPed::m_fArmour` | `+0x548` (VALIDATE_OFFSET) | `Entity/Ped/Ped.h` - same ⚠️ |
| Ped heading (rad) | `CPed::m_fHeadingCurrent` | `+0x558` (VALIDATE_OFFSET) | `Entity/Ped/Ped.h` `m_fCurrentRotation` (other name) ⚠️ |
| Ped vehicle ptr | `CPed::m_pVehicle` | `+0x58C` (VALIDATE_OFFSET) | `Entity/Ped/Ped.h`; note: may be set even when not in a vehicle -> always check `bInVehicle` ✅ |
| "In vehicle" flag | `CPed::bInVehicle` | flag bit 8 of the ped flags | `Entity/Ped/Ped.h`: "bit 8, `(m_nPedFlags >> 8) & 1`" ✅ |
| Model index | `CEntity::m_nModelIndex` | `+0x22` (VALIDATE_OFFSET) | `Entity/Entity.h` (`uint16`) ✅ |
| Current interior | `CGame::currArea` | global `0xB72914` | `Game.h`: `StaticRef<...>(0xB72914)` ✅ |
| Menu active | `FrontEndMenuManager.m_bMenuActive` | global `0xBA6748`, `+0x5C` | `MenuManager.cpp`: `0xBA6748` ✅ |
| Game time (ms) | `CTimer::m_snTimeInMilliseconds` | global `0xB7CB84` | `Timer.h`: `0xB7CB84` ✅ |

✅ = address identical in both sources. ⚠️ = plugin-sdk asserts the offset at
compile time (VALIDATE_OFFSET) but gta-reversed only asserts the total size of
CPed (`0x79C`); if you want a second opinion, check the field in IDA/Ghidra.

Hook points (plugin-sdk `shared/Events.h`, GTASA block):

| Event | Call site hooked |
|---|---|
| `Events::gameProcessEvent` | `0x53E981` (after) |
| `Events::drawingEvent` | `0x53E293` (after) |
| `Events::initGameEvent` | `0x748CFB` (after) |
| `Events::shutdownRwEvent` | `0x53D910` (before) |

---

## 2026-11-26 - RGL build addendum

**Everything above this line is 1.0 US only and does NOT apply to the
Rockstar Games Launcher (RGL) build of `gta_sa.exe`.** We independently
confirmed via Ghidra (static disassembly of the exact exe) that every address
and hook point above either falls mid-instruction or inside a completely
unrelated function on RGL - this is not a version-numbering nitpick, it's a
different compiled binary with a different code layout. `ValidateGameVersion()`
already refuses to run on it for this reason.

Confirmed by direct disassembly of the user's `gta_sa.exe`
(`/E:/Grand Theft Auto San Andreas/gta_sa.exe`, strings "RLAUNCHER" /
"RLAUNCHER_HS" present, function_count 20588 at base 0x400000):

| What | RGL address | How confirmed |
|---|---|---|
| `FindPlayerPed` | function `0x56E6B0` | Byte-exact disassembly matches the textbook implementation (`MOV EAX,[ESP+4]; TEST EAX,EAX; JGE; MOVZX EAX,byte[PlayerInFocus]; IMUL EAX,EAX,0x190; MOV EAX,[EAX+Players]; RET`). `0x190` == `sizeof(CPlayerInfo)`, independently asserted by both plugin-sdk and CookiePLMonster/SilentPatch (open source, MIT, explicitly RGL-compatible). 517 xrefs; two unrelated callers do `PUSH -1; CALL 0x56E6B0`, the universal `FindPlayerPed(-1)` pattern. **High confidence.** |
| `PlayerInFocus` (byte global) | `0xB7F3F4` | Read directly out of the `FindPlayerPed` disassembly above. |
| `Players` array base | `0xB7F418` | Same. `m_pPed` is the first member of `CPlayerInfo` (offset 0), consistent with SilentPatch's `CPlayerInfo` layout. |
| `CPed`/`CVehicle`/`CEntity` field offsets (health, armour, heading, vehicle ptr, `bInVehicle`, model index) | **unchanged from the 1.0 table above** | plugin-sdk's `VALIDATE_OFFSET` macros for these fields are NOT version-gated (no `#if GAME_STEAM` etc. around them), and SilentPatch's independently-maintained, RGL-supporting `CPed`/`CPlayerInfo` classes assert the exact same `sizeof(CPed) == 0x79C` / `sizeof(CPlayerInfo) == 0x190`. Two unrelated open-source projects agreeing is strong evidence the game's *data* layout is identical across versions - only *code* addresses moved. |

**NOT yet found for RGL (do not use the 1.0 values above on this build):**

| What | Why it's dangerous as-is | Suggested next step |
|---|---|---|
| `CGame::currArea` | plugin-sdk's `GLOBAL_ADDRESS_BY_VERSION(0xB72914, 0, 0, 0, 0, 0)` resolves to **address 0** for any non-1.0-US version, i.e. reading it on RGL is a guaranteed null-pointer dereference. Confirmed by reading plugin-sdk's own `CGame.cpp` source, not a guess. | Single, very frequently-read `int` global - same xref/neighbourhood technique used for `FindPlayerPed` should work, or a proper AOB scan (enable `GHIDRA_MCP_ALLOW_SCRIPTS=1` for wildcard memory search) using SilentPatch's own pattern for it as a starting point. |
| `FrontEndMenuManager` / `m_bMenuActive` | Not re-verified against this exe. SilentPatch's own AOB for the manager pointer (`"50 50 68 ? ? ? ? B9 ? ? ? ? E8"`, +8) is NOT version-gated in their source, so it likely works unmodified here too, but nobody has actually run it against this binary yet. | Confirm the pattern resolves to a sane pointer before using it. |
| `CTimer::m_snTimeInMilliseconds` | SilentPatch's own `AddressByVersion<int**>(0x4242D1, 0x53F6A1, 0x406FA1)` call has **no RGL/pattern fallback at all** (falls through to `return 0` for this build in their own code), meaning even the project that already supports RGL doesn't have a ready answer for this one. | Not worth chasing: the client no longer needs it (see client/src/Main.cpp - the 5-second log interval is now timed by the polling thread's own `Sleep` loop instead of reading the game clock). |
| `Events::gameProcessEvent` / `drawingEvent` / `initGameEvent` / `shutdownRwEvent` | Each hooks a specific `CALL` instruction inside a specific, unrelated game function (not a single hot, easy-to-fingerprint utility function like `FindPlayerPed`). The 1.0 addresses land inside *different, unrelated* functions on RGL (confirmed), so hooking them as-is patches the wrong bytes. | `client/src/Main.cpp` currently sidesteps all four with a polling thread. If you want the F9 overlay back, `drawingEvent` specifically needs its real RGL call site found; expect this to take real, dedicated investigation per hook (ideally with `GHIDRA_MCP_ALLOW_SCRIPTS=1` enabled so a proper AOB/wildcard scan can be scripted instead of manual disassembly). |

**Methodology, if you pick this up again:** `find_functions` sorted by
`xref_count` is what found `FindPlayerPed` - look for functions in the same
"neighbourhood" (the game's overall subsystem ordering is roughly preserved
even though exact addresses shift) as the equivalent 1.0 address, then check
candidates' call counts and disassembly shape against the known plugin-sdk/
SilentPatch source for that function. For the four Events hooks specifically,
tracing *callers* of already-confirmed functions (e.g. what calls
`FindPlayerPed` once per frame, near the top of the call stack) is more
promising than searching near the old 1.0 addresses, since those turned out
to be unrelated functions entirely.
