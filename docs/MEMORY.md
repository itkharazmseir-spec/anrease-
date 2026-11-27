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

---

## 2026-11-26 - `CPopulation::AddPed` search, suspended (PGO)

Needed for step 3 (spawning a real `CPed` for each remote player). The
`FindPlayerPed`-style technique (xref-count ranking + disassembly-shape
matching near the 1.0 address' relative position) was tried repeatedly here
and failed every time - candidate addresses either disassembled to garbage
or to clearly unrelated functions. One concrete example: forcing Ghidra to
define a function at `0x00612bc0` (inside a conspicuous ~3184-byte gap in
Ghidra's own auto-analysis coverage, between two otherwise-normal functions
at `0x612b40` and `0x6137b0`, sized about right for where `AddPed` should
sit) decompiled to a 16x16 spatial-grid lookup with frame-rate-scaled timers
- some kind of proximity/collision query, not ped allocation.

**Root cause (best understanding so far):** this RGL `gta_sa.exe` is very
likely built with Profile-Guided Optimization. Evidence: the file is ~34MB
vs ~11MB for 1.0 US despite `get_function_count`/`analysis_status` reporting
a normal, fully-analyzed function count (20588); several locations disas-
semble to a `jmp dword ptr [0x0150xxxx]`-style far jump into a completely
separate ~0x0150xxxx code region, which is the classic signature of a PGO
build physically splitting a function's "hot" and "cold" basic blocks apart.
If true, this invalidates the "nearby in 1.0 -> nearby in RGL" address-delta
heuristic for *this specific neighbourhood* of the binary (it still worked
for `FindPlayerPed`, which is presumably "hot enough" to not get split, or
got split in a way that happened not to matter for finding its entry point).

**Checked for an existing answer - none exists:**
- SilentPatch (github.com/CookiePLMonster/SilentPatch, MIT, open source,
  explicitly RGL/Steam-compatible via runtime AOB pattern scanning through
  their ModUtils `hook::pattern`) has **no pattern for `CPopulation::AddPed`**
  anywhere in its source (confirmed by grep).
- SA-MP never supported RGL/Steam builds at all.
- open.mp is a server-only replacement (its own FAQ says so) - no client-side
  ped-rendering code to borrow from even if it did support this binary.

**Decision:** suspended for now rather than continuing to spend effort on
manual single-address probing of an ~34MB restructured binary with no
systematic strategy left to try (the gap-sweep approach does not scale: each
candidate costs one `create_function` + one decompile + manual read, and the
binary offers no shortcut for *where* in a multi-KB gap the real function
starts). Remote players are made visible a different way instead - see
`ao::Overlay` in `client/include/ao/Overlay.h` / `RGL_COMPAT_NOTES.md`.

**If this is picked up again**, the PGO theory should be confirmed or ruled
out first (e.g. look for a `.pgo`/profile section in the PE header, or check
whether MSVC's `/LTCG:PGInstrument`/`/LTCG:PGOptimize` flags are mentioned
anywhere associated with this specific RGL build online) - if true, a
function-boundary-based search is the wrong tool entirely, and the right one
is probably a *data-xref* search instead: find the pool allocator or a known
constant (e.g. `CPed`'s `sizeof` 0x79C, or a known model-id table) and walk
backward from its cross-references, since those tend to stay in one place
even when the calling code around them is split and reordered.

---

## 2026-11-26 (later same day) - `CPopulation::AddPed` FOUND - the above is superseded

The PGO-splitting theory above turned out to be the wrong explanation for
*why the search kept failing* - the gap region is real, but it contains
several small, ordinary, previously-unanalysed functions, not one function
scattered by PGO. The actual mistake was simply probing the wrong
sub-address inside that gap (`0x612bc0`, which really is an unrelated
16x16 spatial-grid/proximity query, exactly as decompiled above) rather than
the right one.

**Found address (this RGL build): `0x612f36`** (note: not `0x612b40`, the
start of the gap itself - there is a small stub + NOP padding from
`0x612f24` to `0x612f35` before the real function entry).

**How it was confirmed, all independently cross-checked:**
1. gta-reversed (`github.com/gta-reversed/gta-reversed`,
   `source/game_sa/Population.cpp`) gives the exact 1.0 US address,
   `0x612710`, right before this gap - the first hint the RGL copy lives
   nearby rather than being unfindable.
2. Ghidra's `create_function` on `0x612f36` produced one clean, contiguous,
   1380-byte function - a single entry, a single set of exits. Not what
   PGO-splitting would produce.
3. The decompiled body matches the 1.0 US source
   (`CPopulation::AddPed`) point for point:
   - `thunk_FUN_014f0e20(0x79c)` -> `operator new(sizeof(CPed)=0x79C)`, the
     same `CPed` size mentioned above.
   - the literal `0x61a9` (=25001 decimal) matches the 1.0 source's
     `GiveAndSetPedWeapon(ped, wtype, /*ammo=*/25001)` default exactly.
   - `FUN_0056e6b0(0xffffffff)` is a call to the already-verified
     `FindPlayerPed` address with `playerId=-1`, matching the 1.0 source's
     `CHEAT_SLUT_MAGNET` branch.
   - `FUN_00439b00(0,0,0)` matches `SetOrientation(0,0,0)`.
   - `FUN_005636c0(iVar5)` matches `CWorld::Add(ped)`.
   - the final `if ((char)param_4 != '\0') { ... }` matches the 1.0 source's
     `if (!makeWander) return ped;` (same check, inverted).
4. plugin-sdk's own 1.0 US calling pattern
   (`plugin_sa/game_sa/CPopulation.cpp:238-241`) confirms the exact ABI:
   `__cdecl`, `(ePedType, unsigned int, CVector const&, bool) -> CPed*`.
   Only the address differs between builds.

See `client/include/ao/GameAddrs.h` (`kAddr_AddPed_RGL`,
`VerifyAddPedFingerprint()`) for the runtime fingerprint and the full
writeup, and `client/src/RemotePeds.{h,cpp}` for how it is used to spawn and
update a real `CPed` for every remote player. This is a materially riskier
call than every previous address in this file: it *allocates into and
mutates* the game's own global ped pool from a background thread (no
verified-safe main-thread hook exists for this build - see
`RGL_COMPAT_NOTES.md`), rather than only reading existing memory. Treat it
as experimental; it is disclosed as such to the user.

**One remaining limitation, left as a future improvement:** moving an
already-spawned remote ped writes `GetPosition()`/`m_fHeadingCurrent`
directly rather than calling `SetPosn()`/`SetOrientation()`/`SetHeading()`
(plugin-sdk shows those as three *more* separate, unverified raw addresses
for this build - `0x420B80`/`0x4241C0`/`0x439A80`/`0x43E0C0` for 1.0 US).
Direct field writes need no new addresses and correctly move the ped's
collision/position, but do not update the RenderWare rotation matrix used
for visual facing - a remote ped will stand in the right place but may not
visually face the right direction until those addresses are found too.

---

## 2026-11-26 (later same day) - `CCarCtrl::CreateCarForScript` FOUND (vehicle sync)

Needed for step 3's "vehicle sync" (a remote player who is driving should
show up as a real moving vehicle, not a ped floating at the car's position).
Found using the methodology the AddPed entry above suggests for next time:
a *data/call-xref* search from an already-verified address, rather than
guessing nearby addresses from scratch.

**Found address (this RGL build): `0x432000`** (1.0 US: `0x431F80`, per
gta-reversed `source/game_sa/CarCtrl.cpp` - a striking `0x80`-byte delta,
the smallest of any address found in this project so far, and the first
clue this build's copy sits almost exactly where 1.0 put it).

**How it was found:** `CWorld::Add`'s RGL address was already confirmed as
part of the AddPed investigation above (kept internally as `FUN_005636c0`).
Listing every caller of that address (`get_functions` with `fields=callers`)
returned 49 functions - every entity type (objects, peds, vehicles) funnels
through `CWorld::Add`, so most of the 49 are unrelated, but `FUN_00432000`
stood out immediately by being close to the 1.0 address of
`CCarCtrl::CreateCarForScript`.

**How it was confirmed, independently, by decompiled-body matching:**
- a call to a `CModelInfo::IsBoatModel`-shaped function splits the function
  into two parallel branches, matching the 1.0 source's
  `if (CModelInfo::IsBoatModel(modelid))`.
- the non-boat branch writes the literal float bit-pattern `0x41500000`
  (=13.0f) and the literal byte `0xD` (=13 decimal) into the vehicle's
  autopilot fields, matching the 1.0 source's
  `vehicle->m_autoPilot.m_speed = 13.0F; ...SetCruiseSpeed(13);` exactly.
- the boat branch instead writes `0x41a00000` (=20.0f) and `0x14` (=20
  decimal), matching `boat->m_autoPilot.m_speed = 20.0F;
  ...SetCruiseSpeed(20);` exactly - two independent float+byte constant
  pairs, in two different branches, both exact matches.
- both branches call `FUN_005636c0` (the already-verified `CWorld::Add`)
  on the freshly built vehicle, matching `CWorld::Add(...)` in both of the
  1.0 source's boat and non-boat paths.
- plugin-sdk's own 1.0 US declaration (`CCarCtrl.h:49`) confirms the exact
  signature: `__cdecl`, `(int modelid, CVector posn, unsigned char
  MissionCleanUpFlag) -> CVehicle*`. Unlike `AddPed`'s `CVector const&`,
  this one takes the `CVector` **by value** (three separate float stack
  slots) - the decompiled RGL function's parameter shape
  (int, float, float, float, char) confirms this was carried over unchanged.

See `client/include/ao/GameAddrs.h` (`kAddr_CreateCarForScript_RGL`,
`VerifyCreateCarForScriptFingerprint()`) and `client/src/RemotePeds.{h,cpp}`
(`RemotePedManager`, extended to switch each remote player between a ped and
a vehicle depending on `PlayerState.vehicleModel`). Same risk profile as
AddPed: allocates into and mutates the game's own global vehicle pool from
the background polling thread.

**Chosen behaviour on disconnect/mode-switch, and why:** `CVehicle::m_fHealth`
exists (plugin-sdk: `0x4C0`, "1000.0f = full health, 0 -> explode"), so
setting it to 0 to force a despawn - mirroring how disconnected peds are
handled - was considered and rejected: it would detonate the vehicle (fire,
explosion sound, wreck) at the exact spot a player disconnected or got out,
which is a far more jarring, unexpected effect than one extra parked car
left behind. Instead, a vehicle is simply forgotten (never despawned) when
its driver disconnects or gets out - it was already created with
`CreateCarForScript`'s own "locked, abandoned" flags set, so the game
itself already treats it as an ordinary parked/abandoned vehicle from that
point on.

## 2026-11-27 - Movement interpolation (smoothing) for remote players/vehicles

Implements item 1 of the "what's left for a fully networked version"
status report given this session: remote peds and vehicles used to
teleport directly to each incoming `PlayerState` packet (every
`kNetSendIntervalMs` = 100ms), which looks like a visible stutter/snap
instead of smooth motion. No new address or Ghidra work was needed for
this - it's a pure client-side technique (snapshot interpolation), the
same one most networked games use to smooth out a low-frequency position
feed.

**Design:** `RemotePedManager` now keeps two timestamped keyframes per
remote avatar - `from*` (the previous network update) and `to*` (the
latest one) - instead of writing position/heading directly on packet
arrival. `OnNetworkUpdate()` (renamed from `UpdatePed()`) only shifts
`to*` into `from*` and records the new packet as the new `to*`, stamped
with the current tick count. A new `Tick()` method, called once per
`PollThreadProc` loop iteration (every `kPollIntervalMs` = 50ms, i.e.
twice as often as new packets arrive) computes how far between the two
keyframes "now" falls and linearly blends position; heading is blended
with shortest-path wraparound handling (so turning from 350 degrees to 10
degrees rotates the short way, 20 degrees, not the long way around). A
mild overshoot allowance (`t` clamped to `[0, 1.5]` rather than `[0, 1]`)
keeps motion from visibly freezing the instant one packet runs a little
late, while a hard clamp still prevents a real stall (lag spike,
disconnect in progress) from flinging the avatar off into the distance.
`ResetInterpolation()` snaps `from == to == current` right after a fresh
spawn or a ped/vehicle mode switch, so the very first frame after a
switch doesn't interpolate from a stale/zeroed keyframe.

Health/armour are still applied immediately in `OnNetworkUpdate()`, not
interpolated - those aren't continuous motion, so smoothing them would
only delay a value that's already correct. Vehicle heading is still never
written at all (position only) - there is no known vehicle-equivalent of
`CPed::m_fHeadingCurrent` to even attempt, same limitation as before this
change; see the CreateCarForScript entry above and the "known limitations"
note in RGL_COMPAT_NOTES.md.

**Risk note:** `Tick()` now writes a ped/vehicle's position roughly twice
as often as `UpdatePed()` used to (every ~50ms instead of only on packet
arrival, ~100ms) - a higher *rate* of the same pre-existing
background-thread write risk already disclosed for AddPed/
CreateCarForScript above, not a new kind of risk. Every write is still
SEH-guarded and forgets the pointer (triggering a respawn) rather than
risking a second fault if the game has freed/recycled it.

See `client/src/RemotePeds.h` (updated `RemoteAvatar` struct, `OnNetworkUpdate`/
`Tick`/`ResetInterpolation` declarations) and `client/src/RemotePeds.cpp`
(the blending math) for the implementation, and `client/src/Main.cpp`
(`PollThreadProc`, the `SetPlayerStateCallback` lambda) for how it's wired
into the poll loop.

Item 2 from the same status report (real orientation via
`SetOrientation`/`SetHeading` instead of the inert `m_fHeadingCurrent`
field write) is still blocked on Ghidra - no instance was running this
session. One strong lead already in hand for next time: the AddPed
decompilation (see the entry above) showed a call `FUN_00439b00(0,0,0)`
matching `SetOrientation(0,0,0)` in the 1.0 source - `0x439b00` may
already be the correct RGL address for `SetOrientation` and is worth
fingerprinting/verifying directly rather than re-deriving from scratch.

## 2026-11-27 (later same day) - CPlaceable::SetHeading FOUND (real rotation)

Implements item 2 of the "what's left for a fully networked version" status
report: real visual rotation for remote peds AND vehicles, replacing the
old `m_fHeadingCurrent`-only write (which only feeds the ped's own AI
heading-blend logic and never touched the rendered rotation matrix - a
known, previously-disclosed limitation in RGL_COMPAT_NOTES.md and the
AddPed/CreateCarForScript writeups above).

**Found address (this RGL build): `0x43e140`** (1.0 US: `0x43E0C0`, per
gta-reversed `source/game_sa/Entity/Placeable.cpp` -
`CPlaceable::SetHeading`). Same `0x80`-byte delta already seen between 1.0
US and this RGL build for `CCarCtrl::CreateCarForScript` above.

**How it was found - starting from a clue already in hand, not a blind
search:** the AddPed writeup (see above) had already noted, in passing,
that AddPed's decompilation contains a call `FUN_00439b00(0,0,0)` matching
`SetOrientation(0,0,0)` in the 1.0 source. That was followed up first:

1. `0x439b00` was decompiled in full and matched point for point against
   gta-reversed's `CPlaceable::SetOrientation`:
   - reads an `int` at object-offset `0x14` and branches on whether it is
     zero - exactly `m_matrix`'s offset (plugin-sdk:
     `VALIDATE_OFFSET(CPlaceable, m_matrix, 0x14)`).
   - the zero branch writes the third argument to offset `0x10` and
     returns, matching `m_placement.m_fHeading = z; return;` (the no-matrix
     early-out).
   - the non-zero branch saves the matrix's position (floats at
     matrix-offset `0x30`/`0x34`/`0x38`), calls a helper, then adds the
     saved floats back - matching `CVector vecPos =
     m_matrix->GetPosition(); m_matrix->SetRotate(x,y,z);
     m_matrix->GetPosition() += vecPos;` exactly.
   This confirmed `0x439b00` = `CPlaceable::SetOrientation` for this build
   (1.0 US: `0x439A80`, delta `0x80` - the same delta already seen for
   `CreateCarForScript`, which is what made `0x80` worth trying as a
   general offset for this neighbourhood of the binary).
2. `SetHeading`'s 1.0 US address (`0x43E0C0`) at the same `+0x80` delta -
   `0x43e140` - was decompiled next and matched gta-reversed's
   `CPlaceable::SetHeading` exactly: same offset-`0x14` zero check, same
   offset-`0x10` write in the zero branch, and the non-zero branch is a
   single tail `JMP` (not `CALL`) into another small function.
3. That callee (`0x59b7f0`) was decompiled too and is textbook
   `CMatrix::SetRotateZOnly`: computes one `cos`/`sin` pair and writes a
   pure Z-axis rotation (right = `(cos,sin,0)`, up = `(-sin,cos,0)`),
   zeroing the cross terms - a different, cheaper shape than
   `SetOrientation`'s general 3-axis `SetRotate`, exactly matching what the
   name "ZOnly" promises.

**Why `SetHeading`, not `SetOrientation`, is called from
`RemotePeds.cpp`:** this project only ever tracks a single `headingRad`
float per remote player (`PlayerState` in
`shared/include/ao/Protocol.h` has no pitch/roll to send), so the simpler,
cheaper single-angle function is the right one to call - not a
mismatch-covering workaround.

**Why one address fixes both peds and vehicles:** plugin-sdk's class
hierarchy is single, non-virtual inheritance the whole way down for both
(`CPed : CPhysical : CEntity : CPlaceable`, `CVehicle : CPhysical : CEntity
: CPlaceable`), so the `CPlaceable` sub-object sits at object offset 0 in
both - a `CPed*` or a `CVehicle*` is already a valid, unadjusted
`CPlaceable*` "this" pointer for this `__thiscall` function. This is why
`client/src/RemotePeds.cpp`'s `SetHeading_RGL()` helper takes a bare
`void*` rather than a typed pointer: this project's trimmed-down
`CPed.h`/`CVehicle.h` don't expose the full `CEntity`/`CPhysical`/
`CPlaceable` chain, and calling through `void*` sidesteps needing those
types just for this one call.

See `client/include/ao/GameAddrs.h` (`kAddr_SetHeading_RGL`,
`VerifySetHeadingFingerprint()`, 24-byte fingerprint covering the whole
function body) and `client/src/RemotePeds.cpp` (`SetHeading_RGL()`, called
from `Tick()` for both the ped and vehicle branches, right alongside the
already-existing interpolated position write). `m_fHeadingCurrent` is also
still written for peds (in addition to the real `SetHeading` call) so the
game's own AI heading-blend logic, which reads that field elsewhere,
doesn't fight the next `SetHeading` call by blending away from a stale
value.

This closes out both items 1 and 2 from this session's "what's left"
status report.

## Item 4 (Events hooks) - dropped by explicit decision

The user agreed to drop this item. It no longer serves any purpose this
project needs (the old reason for it was superseded by the Win32 HUD
overlay long ago) and it would have meant patching live game code, which
is a strictly higher-risk category of change than anything else here
(every other hook in this project calls existing game functions through
their real addresses; patching code means overwriting instructions in a
running process). Not pursued further, and not planned.

## Item 3 (real player models) - scoped/safe implementation, 2026-11-27

**What was wanted:** make a remote player appear using their own real ped
skin (whatever model they're using locally - a custom skin, a mission
outfit, etc.) instead of always spawning them as the same generic
`MODEL_MALE01` civilian.

**What would have made that fully general, and why it wasn't found:**
spawning an arbitrary model safely requires checking it is actually loaded
in memory on the *receiving* player's machine first - plugin-sdk's
equivalent is `CStreaming::HasModelLoaded`, which isn't really a function
call at all, just a direct read of a global array,
`CStreaming::ms_aInfoForModel` (1.0 US address `0x8E4CC0`, confirmed via
gta-reversed's `Streaming.h`/`StreamingInfo.h`: an array of
`CStreamingInfo` entries, 20 (`0x14`) bytes each, with the load-state flag
at struct offset `0x10`). Finding *this* build's real address for that
global needed a byte-pattern or literal-constant search over Ghidra's
~15,000 unnamed global data items (to anchor on, e.g., the array's known
element count or stride) - the specific Ghidra tools for that
(`search_instructions`/`search_byte_patterns`) were not reachable this
session despite being reported as loaded on the Ghidra bridge side, and no
reliable anchor was found through the tools that *were* available
(`list_data_items_by_xrefs` across all 15,324 items, several targeted
reads). Spawning an arbitrary, possibly-not-loaded model without that
check risks a hard crash for the viewer, so this was not attempted. This
is a documented gap, not a guess - if a working byte-search tool becomes
available in a future session, this is the next thing to look up.

**What was implemented instead (safe, not a placeholder):** the sender's
real `CEntity::m_nModelIndex` is now synced over the wire (protocol v2,
new `pedModel` field on `PlayerState` - see
`shared/include/ao/Protocol.h`), but a receiving client only ever *acts*
on it for the two models that are, by construction, guaranteed loaded on
every single connected client at all times: `MODEL_PLAYER` (0, "CJ", the
default protagonist model) and `MODEL_CSPLAY` (1, the alternate
player-select skin). Both are safe because every player's own local ped
always exists using one of them from the moment the game boots - so
"is it loaded on the viewer's machine" is always true for these two,
without needing the streaming check above at all. Any other `pedModel`
value received falls back to the previous, always-safe default
(`MODEL_MALE01`, 7) exactly as before. See
`client/src/RemotePeds.cpp`'s `ResolveRemotePedModel()` for the actual
two-value allowlist and its full reasoning in the comment above it.

Changed files: `shared/include/ao/Version.h` (protocol bumped 1 → 2 -
old and new builds will refuse to connect to each other, per the existing
version-mismatch check already in `server/src/Main.cpp`),
`shared/include/ao/Protocol.h` (`PlayerState.pedModel`, `int32_t`),
`client/src/LocalPlayer.h`/`.cpp` (reads `ped->m_nModelIndex` into
`PlayerSnapshot::pedModel`), `client/src/Main.cpp` (copies it into the
outgoing `PlayerState`), `client/src/RemotePeds.cpp`
(`ResolveRemotePedModel()` + `SpawnPed()` uses it). No server source
changes were needed - `HandlePlayerState` in `server/src/Main.cpp` just
checks the struct size and rebroadcasts the raw bytes, so a new field is
handled automatically by the existing generic pass-through logic; only a
server rebuild (picking up the new struct size/version) is required.

## Item 5 (weapon/damage sync) - completed, 2026-11-27 (continued session)

The user asked for this to be finished, not left as a reported blocker.
The original plan - detect a real weapon-give/fire call
(`CPed::GiveWeapon` or equivalent) - hit genuine address-finding walls that
could not be safely worked around (full investigation kept in
`docs/RGL_COMPAT_NOTES.md`'s "Item 5" section: a merged/un-splittable
function blob, one coincidental false-positive address, and an
unresolvable CFG-style indirect thunk). Rather than guess an address to
force a "complete" result, the feature was redesigned around data this
project already trusts completely and uses safely elsewhere:
`CPed::m_fHealth`/`m_fArmour` are plain data at plugin-sdk-verified
offsets, not code to call - no RGL-specific address exists for them at all
(same category as the `m_fHeadingCurrent`/`m_nModelIndex` fields already
read/written throughout this project).

**The design:** `RemotePedManager::Tick()` (in `client/src/RemotePeds.cpp`)
now also watches each remote avatar's `CPed::m_fHealth`/`m_fArmour` for a
drop since the last authoritative `PlayerState` - this is a drop the
local game engine itself just computed (a bullet, explosion, collision,
anything the engine's own physics/damage code already processed against
that locally-spawned stand-in CPed). A new baseline pair
(`RemoteAvatar::lastKnownHealth/Armour`) tracks what "no new damage yet"
means, refreshed every time an authoritative `PlayerState` is applied so a
network update is never itself misread as damage. When a real drop is
seen, `RemotePedManager`'s new `SetDamageDetectedCallback` fires with
`(playerId, healthDelta, armourDelta)`; `client/src/Main.cpp` wires that to
a new `NetworkClient::SendDamageReport()`, which sends a new
`MessageType::DamageReport` (protocol v3) to the server over the reliable
channel (unlike `PlayerState`, a damage event must never be silently
dropped). The server (`server/src/Main.cpp`'s new `HandleDamageReport`)
overwrites the sender-claimed `attackerPlayerId` with the real sender id
(same convention as `PlayerState.playerId`), clamps both deltas to
`[0, 100]` (GTA SA's own health/armour range - bounds a buggy or malicious
client's claim to "as if hit by the single biggest legitimate hit" rather
than an unverifiable instant kill), rejects a report naming a
`victimPlayerId` that isn't currently connected, and broadcasts the result
to everyone. Every client's `NetworkClient::SetDamageReportCallback`
ignores a report that doesn't name them as the victim; the one client it
does name applies it directly to their own real ped via a new
`LocalPlayer::ApplyDamage(healthDelta, armourDelta)` (clamped to not go
below 0) - the first write this project makes to the *local* player's own
ped (previously only reads); same accepted background-thread/game-memory-
write risk category as every other field write here, just a new target.

**Changed files:** `shared/include/ao/Protocol.h` (`MessageType::DamageReport`
+ `DamageReport` struct), `shared/include/ao/Version.h` (protocol bumped
2 -> 3 - this is a genuinely new message type, not just a struct field, so
an old build would not understand it at all, not merely misread it),
`client/include/ao/NetworkClient.h`/`.cpp` (`SendDamageReport`,
`SetDamageReportCallback`), `server/src/Main.cpp` (`HandleDamageReport`),
`client/src/RemotePeds.h`/`.cpp` (baseline fields, drop detection,
`SetDamageDetectedCallback`; also fixed a small pre-existing gap where a
freshly-spawned avatar kept the engine's default health/armour until the
*second* `PlayerState` - now applied immediately on spawn, which the new
baseline also needed to be correct from the first `Tick()`),
`client/src/LocalPlayer.h`/`.cpp` (`ApplyDamage`), `client/src/Main.cpp`
(wires `RemotePedManager`'s damage-detected callback to
`SendDamageReport`, and `NetworkClient`'s `DamageReport` callback to
`ApplyDamage`).

**Verification:** compiled clean via the usual mingw sync-check for every
client file; the server side was verified further than usual, because it
has new actual logic (not just a pass-through) - built and linked as a
real, running Linux binary (not just `-fsyntax-only`), then exercised with
two scripted ENet test clients against that live server process. Confirmed
in real, observed server output: the server overwrites a forged
`attackerPlayerId` (sent as `999`) with the real sender's assigned id,
clamps an out-of-range `healthDelta` (sent as `500`) down to `100`, passes
an in-range `armourDelta` (`15`) through unchanged, broadcasts the result
to both the attacker and the victim, and correctly drops a second test
report naming a `victimPlayerId` (`9999`) that was never connected. This is
the most thoroughly runtime-tested piece of networking logic in the
project so far (everything game-memory-related is still Ghidra-static-
analysis-only and untested in a live game, per the existing disclosure).

**Known, disclosed limitation:** because this reports "this avatar lost
health/armour" rather than "this avatar was shot with weapon X", it cannot
distinguish a real PvP hit from any other source of damage the avatar
takes in the attacker's own local simulation (an explosion, a collision,
being run over, etc.) - all such events get reported and applied the same
way. This is treated as a reasonable (arguably more complete)
interpretation of "damage sync" rather than a bug - see
`client/src/RemotePeds.h`'s class comment. Vehicle damage is explicitly
out of scope for this change; only ped health/armour is synced this way.

Everything past items 1-5 on the original "what's left" list (multi-
passenger vehicles, security/auth, interior tracking, and actual
multiplayer testing) remains open; item 4 is dropped (see above).

---

## 2026-11-27 - crash fix: main-thread suspend guard

**What actually happened:** real, live user testing (the user's own game,
with the standalone `FakePlayer.exe` network-test tool simulating a second
player - see below) hit a genuine, **100%-reproducible crash** the moment a
remote player's first `PlayerState` arrived and `RemotePedManager::SpawnPed`
called `AddPed_RGL`. The user confirmed via Windows Event Viewer that the
crash is: `Faulting application name: gta_sa.exe`, `Faulting module name:
gta_sa.exe` (i.e. the fault is in the game's **own** code, not
`AndreasOnline.asi`), `Exception code: 0xc0000005` (access violation),
`Fault offset: 0x001e4e00` - an address distinct from any of this project's
own verified addresses (`kAddr_AddPed_RGL`, `kAddr_CreateCarForScript_RGL`,
`kAddr_SetHeading_RGL`, `kAddr_FindPlayerPed_RGL`,
`CPlaceable::SetOrientation`). This is exactly the risk this project has
disclosed since 2026-11-26 in `RemotePeds.h`'s class comment ("a genuine
data race and a crash is possible under some timings - this is a real,
elevated risk, not a formality") turning out to be real, not theoretical:
`AddPed_RGL`/`CreateCarForScript_RGL` allocate into and mutate the game's
own global ped/vehicle pools from this project's background polling
thread, and the game's own main thread was concurrently touching the same
structures.

**The fix:** a true "no other thread is touching this right now" guarantee,
without needing to find any new, RGL-specific, unverified game address at
all - `client/src/MainThreadGuard.h` (new file), `MainThreadSuspendGuard`:
an RAII class that calls the standard Win32 `OpenThread(
THREAD_SUSPEND_RESUME, ...)` + `SuspendThread()` on construction (skipping
itself if the id is 0/unknown, or equals the calling thread, to avoid
suspending ourselves) and `ResumeThread()` + `CloseHandle()` on
destruction. The game's real main thread id is captured exactly once, at
plugin load time, via `GetCurrentThreadId()` inside the
`AndreasOnlineClient` constructor in `Main.cpp` (new global
`g_mainThreadId`) - this relies only on the ASI Loader's standard,
well-documented, version-independent convention that a plugin's global
constructor runs synchronously on the game's own main thread while it is
being loaded during the game's own startup, **not** on any RGL-specific or
otherwise unverified assumption. It is handed to `RemotePedManager` via a
new `SetMainThreadId()` setter (wired in `Main.cpp`, before anything that
could trigger a spawn), and the guard is applied **only** around the
`AddPed_RGL` call inside `SpawnPed()` and the `CreateCarForScript_RGL` call
inside `SpawnVehicle()` - the two true pool-mutating, once-per-spawn-event
operations. `SetHeading_RGL` and the routine per-tick position/heading/
health/armour field writes in `Tick()`/`OnNetworkUpdate()` are deliberately
**not** wrapped: those write to an already-valid object's own fields
(lower severity than pool/list mutation, per the pre-existing risk
disclosure) and happen far more often (~20x/sec per remote avatar) -
wrapping those too would mean freezing the game's main thread dozens of
times a second per remote player, for a much smaller safety benefit than
the one-time spawn calls.

**Changed files:** `client/src/MainThreadGuard.h` (new),
`client/src/RemotePeds.h`/`.cpp` (`SetMainThreadId`, `m_mainThreadId`, the
guard wrapped around the two spawn calls), `client/src/Main.cpp`
(`g_mainThreadId` captured + wired to `RemotePedManager`).

**Verification:** the new code (`MainThreadGuard.h`, the modified
`RemotePeds.h`/`.cpp`) compiles clean via the usual mingw sync-check
against the project's real headers. `Main.cpp`'s own two new lines (the
`GetCurrentThreadId()` capture and the `SetMainThreadId()` call) were
syntax-checked in isolation against real `<windows.h>` with the exact same
pattern, since fully compiling `Main.cpp` itself requires the complete
plugin-sdk + RenderWare header chain (`plugin.h` pulls in the full game
class tree, 32-bit structure-offset validation, and RenderWare types this
sandbox does not have set up) - this is consistent with how `Main.cpp` has
always been treated in this project (every other change to it was reviewed
by hand plus a targeted check of the new lines, never a full build, because
a full build has only ever been possible on the user's own Windows/MSVC
toolchain). **This is a mitigation based on a real, well-precedented
low-level technique (suspend-inject-resume, used by instrumentation/
profiling tools for the same reason), not a mathematically proven fix - it
has not yet been re-tested in the live game.**

**Known, disclosed residual risk (different in kind from the risk it
fixes):** if the main thread happens to be suspended mid-way through its
own call into a Windows CRT/heap lock (e.g. mid-`malloc`/mid-`free`) that
the wrapped call then also needs (both `AddPed` and `CreateCarForScript`
allocate), this **deadlocks** (hangs) instead of crashing. The heap lock is
only held for the brief duration of an actual allocation, so this is rare,
but it is not zero - genuinely different from, not strictly better than,
the crash it replaces. Judged an acceptable trade only because a hang is
at least visible immediately and recoverable via Task Manager, unlike a
crash that corrupts memory and can surface anywhere. **Next step: the user
needs to rebuild, redeploy, and re-test (ideally again with
`FakePlayer.exe`, since that is the only way they can currently test with
a second player) to confirm in the real game whether the crash is actually
gone.**

---

## 2026-11-27 (later same day) - crash fix v1 did NOT work; v2 (suspend
## every thread) deployed instead

**First, two separate real MSVC/CI build failures, both fixed:**

1. `error C2712: Cannot use __try in functions that require object
   unwinding` on both `SpawnPed()`/`SpawnVehicle()`. Cause: the
   `MainThreadSuspendGuard` object (has a destructor) was declared directly
   inside the `__try` block wrapping `AddPed_RGL`/`CreateCarForScript_RGL` -
   MSVC flatly refuses to let `__try`/`__except` share a function with any
   C++ object that needs destructor unwinding. The mingw sync-check used
   throughout this project for quick syntax checks does not enforce this
   MSVC-only rule, which is why it wasn't caught until the user's own real
   CI build. **Fix:** the `__try`/`__except` itself was moved into two tiny
   free functions (`TryAddPed_RGL`/`TryCreateCarForScript_RGL` in
   `client/src/RemotePeds.cpp`) containing only raw pointers/PODs, no C++
   objects at all; `SpawnPed()`/`SpawnVehicle()` now construct the guard in
   their own scope and call one of those functions instead of wrapping the
   raw game call with an inline `__try`/`__except` themselves.

2. **The fix (v1) did not actually stop the crash.** The user rebuilt,
   redeployed, and re-tested with `FakePlayer.exe` again - the exact same
   symptom recurred (the real player's client joined, then disconnected
   again within seconds; same join/leave/reconnect pattern as the original
   crash report). This means the v1 design's core assumption - that
   `GetCurrentThreadId()` captured inside the `AndreasOnlineClient`
   constructor (which runs when the ASI Loader loads this DLL) reliably
   identifies the game's real main thread - most likely does not hold for
   however this specific RGL build's loader actually works. This project
   has already had to work around several other RGL-specific surprises (see
   `client/src/Main.cpp`'s RGL BUILD NOTICE), so a non-standard loader
   thread for this build specifically is plausible; it's also possible some
   other thread besides "the main thread" touches the pool. Either way,
   suspending one single, assumed thread left the real race open.

**v2 fix, deployed now: `AllThreadsSuspendGuard`** (replacing
`MainThreadSuspendGuard` entirely in `client/src/MainThreadGuard.h`) makes
NO assumption at all about which thread is "the" main thread, or how many
threads might touch the pool. At the moment `SpawnPed()`/`SpawnVehicle()`
needs to call `AddPed_RGL`/`CreateCarForScript_RGL`, it enumerates every
thread in the current process via
`CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)` +
`Thread32First`/`Thread32Next`, and suspends every one of them except the
thread making the call (our own background polling thread) - still only
standard Win32 APIs, no RGL-specific game address of any kind. While every
other thread in the process is suspended, it is simply not possible for
anything else to be touching the pool concurrently, regardless of which
thread that would have been - this closes the race without needing to
correctly guess which thread v1 got wrong. The now-unused main-thread-id
plumbing (`RemotePedManager::SetMainThreadId`, `m_mainThreadId`,
`Main.cpp`'s `g_mainThreadId`) was removed rather than left as dead code.

**Changed files (this round):** `client/src/MainThreadGuard.h` (rewritten:
`AllThreadsSuspendGuard` replaces `MainThreadSuspendGuard`, full revision
history kept in the file's own comment), `client/src/RemotePeds.cpp` (the
C2712 fix above, plus `SpawnPed`/`SpawnVehicle` now use
`AllThreadsSuspendGuard`; `SetMainThreadId` removed), `client/src/RemotePeds.h`
(`SetMainThreadId`/`m_mainThreadId` removed; class comment updated),
`client/src/Main.cpp` (`g_mainThreadId` capture and wiring removed).

**Verification:** same method as before - the mingw sync-check compiles
clean for `MainThreadGuard.h`/`RemotePeds.h`/`.cpp` (including
`<tlhelp32.h>`, `THREADENTRY32`, and `std::vector<HANDLE>`, none of which
were used by v1); `Main.cpp`'s change is a pure removal (no new code), so
no new syntax risk there.

**Residual risk, updated:** same category as v1 (a possible rare deadlock
if a suspended thread was mid-allocation holding the CRT heap lock when
suspended), but now spread across every thread in the process instead of
just one - still judged low in absolute terms (most of a game's other
threads, e.g. audio/streaming, are idle/sleeping most of the time) but
honestly higher than v1's number, since more threads are being suspended
each time. **This is a second genuine attempt, not a guaranteed fix - the
user needs to rebuild, redeploy, and re-test (again with `FakePlayer.exe`)
to find out whether this version actually closes the race. If it still
crashes, the likely next step is a live debugger attached to a reproduction
to directly observe which thread is actually racing with
`AddPed`/`CreateCarForScript`, since guessing a third time without that
data would not be well-grounded.**

## 2026-11-27 (later still) - v2 confirmed working for ped spawn, but vehicle spawn deadlocks the whole game - AllThreadsSuspendGuard removed from SpawnVehicle

**Positive result first:** the user rebuilt/redeployed the v2 `.asi`
(`AllThreadsSuspendGuard`, previous section) and ran `FakePlayer.exe`
against a real running copy of the game. The on-foot ped-spawn path
(`AddPed_RGL`, wrapped in `AllThreadsSuspendGuard`) now joined, spawned,
walked, and switched pedModel (CJ/CSPLAY) repeatedly with **no crash** -
this is real evidence v2 actually closed the original ped-spawn race, at
least for this test.

**New finding - vehicle spawn deadlocks:** `FakePlayer.exe` was upgraded
this round into an automatic multi-phase test tool (see its own top-of-file
comment) that, given one start position, cycles through on-foot CJ ->
on-foot CSPLAY -> in vehicle (Landstalker, model 400) -> exiting vehicle ->
repeat, plus periodic automatic damage-sync tests - specifically so the
previously-untested vehicle-spawn path (`CreateCarForScript_RGL`) would
finally get exercised. The very first time phase 3 ("IN VEHICLE") ran
(`SpawnVehicle()` -> `AllThreadsSuspendGuard` + `TryCreateCarForScript_RGL`
with `state.vehicleModel = 400`), the real game **hung completely and had
to be force-closed** - confirmed by the user ("بازی هنگ کرد و کلا خارج شد
از بازی"). The server's own position log (item 10's periodic logging,
previous section) shows the real player's position freezing dead solid for
over a minute right around the moment `veh 400` first appears in
FakePlayer's own state log, then nothing further from that player at all -
consistent with the whole process having frozen, not just the one spawn
call failing cleanly.

**Root cause (reasoned, not yet directly observed via debugger):**
`CreateCarForScript` almost certainly needs to pump `CStreaming` - the
game's background model/collision loader thread - to finish bringing a
vehicle's resources fully in before the function can return, the same way
streaming in any other game asset works. `AllThreadsSuspendGuard` suspends
*every* other thread in the process for the full duration of the call,
including that streaming thread. If `CreateCarForScript_RGL` ever needs
that thread to run (post a completion, service a callback, etc.) before it
can finish, suspending it first guarantees the call can never return - a
deterministic deadlock, not the rare "mid-allocation" hang disclosed for
peds. Pedestrian spawn never showed this because `AddPed_RGL`'s models
(CJ/CSPLAY) are, by this project's own design (see the comment above
`kRemotePedModel`/`ResolveRemotePedModel` in `RemotePeds.cpp`), guaranteed
already resident from the moment the game boots - so `AddPed_RGL` never
needs to touch streaming at all, and suspending the streaming thread around
it costs nothing. The comment that assumed "the base vehicle set is loaded
on every client too" (same file, above `CreateCarForScript_RGL`) is the
part that turned out to matter: even if the Landstalker's raw model data is
resident, `CreateCarForScript` itself may still synchronize with that
thread for other reasons (e.g. collision data, handling data, or some
per-spawn bookkeeping) that a plain "is the model loaded" check wouldn't
capture.

**Also worth being honest about:** the ORIGINAL 100%-reproducible crash
this whole crash-fix effort was built around was only ever confirmed for
ped spawn, via real testing, both for the bug itself and for v1/v2's
fixes. Vehicle spawn was never shown to have the same race - the
`AllThreadsSuspendGuard` wrap was added to `SpawnVehicle` symmetrically, on
the assumption the same risk applied equally to both calls. That
assumption is what caused this regression: it made vehicle spawn strictly
worse (a new, reproducible hang) without ever having had evidence it was
protecting against anything real there in the first place.

**Fix applied:** `RemotePedManager::SpawnVehicle()` (`RemotePeds.cpp`) no
longer constructs `AllThreadsSuspendGuard` around the call. It still calls
`TryCreateCarForScript_RGL` (the `__try`/`__except` wrapper from the C2712
fix, previous section), which still converts a genuine access-violation
crash into a caught `nullptr` - that part costs nothing and stays. Only the
thread-suspension is removed, specifically because it is what caused the
observed deadlock. `SpawnPed()` is UNCHANGED - it keeps
`AllThreadsSuspendGuard`, since that is the call with actual confirmed
positive evidence behind it from this same test round.

**Residual risk, honestly stated:** removing the guard from `SpawnVehicle`
means the original (never actually confirmed for vehicles) race, if it
exists there at all, is not covered by anything beyond the `__try`/
`__except` itself - which catches a crash but does not prevent whatever
corruption led to it. If real testing ever shows vehicle spawn crashing
(as opposed to hanging) the same way ped spawn once did, that would need a
*narrower* fix scoped to just the pool-insert step inside
`CreateCarForScript`, not a blanket suspend-every-thread wrap around the
whole call - since the whole call is exactly what must not be wrapped, per
the deadlock just found. No such narrower fix exists yet; this is a
documented gap, not a guess dressed up as a fix.

**Changed files (this round):** `client/src/RemotePeds.cpp` (removed
`AllThreadsSuspendGuard suspendGuard;` from `SpawnVehicle()`, expanded the
surrounding comment with the full reasoning above). The standalone
`FakePlayer.exe` test tool (not part of the shipped project) was also
rewritten to automatically cycle through all four ped/vehicle states at one
position plus periodic automatic damage tests, specifically so this gap
would get found by routine testing rather than staying hidden until a real
second player happened to get in a car.

**Verification:** mingw sync-check compiles clean for the updated
`RemotePeds.cpp` (`-fsyntax-only`, `__try`/`__except` neutralized the usual
way). **Still needs:** the user must rebuild/redeploy the client `.asi` via
GitHub Actions and re-run the FakePlayer auto-test cycle to confirm phase 3
(in vehicle) no longer hangs the game - this has NOT been re-tested yet as
of this writing, only reasoned through and patched.
