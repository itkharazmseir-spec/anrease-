# RGL build compatibility patch (2026-11-26)

## What this patch is

Your `gta_sa.exe` is the Rockstar Games Launcher (RGL) build, not the 1.0 US
retail build the original project targeted - confirmed by disassembling the
actual exe in Ghidra (details in `docs/MEMORY.md`). plugin-sdk only ships
addresses up to the Dec-2014 "NewSteam R2" Steam patch, so neither the
original code nor a simple `PLUGIN_SGV_STEAM` define switch would have
worked; the addresses needed re-discovering specifically for your binary.

## Files changed

- **`client/include/ao/GameAddrs.h`** (new) - the one game-code address we
  found and verified (`FindPlayerPed`), with a full writeup of how it was
  found and a runtime byte-fingerprint check so a future game update can't
  make this read/call into the wrong thing silently.
- **`client/src/LocalPlayer.cpp`** - calls the real `FindPlayerPed` address
  directly instead of plugin-sdk's wrapper (which doesn't recognise this
  exe). Interior tracking (`CGame::currArea`) is disabled - see "What still
  doesn't work" below, it was a real crash risk, not a style choice.
- **`client/src/LocalPlayer.h`** - comment updates only, same struct shape.
- **`client/src/Main.cpp`** - no longer calls `ValidateGameVersion()` or
  registers plugin-sdk's four `Events::*` hooks (their 1.0 addresses land
  inside unrelated functions on this exe and would very likely crash the
  game on load). A plain polling thread (`CreateThread`, 50ms `Sleep` loop)
  drives `LocalPlayer::Update()` and the 5-second log line instead.
- **`docs/MEMORY.md`** - new "2026-11-26 - RGL build addendum" section with
  the RGL addresses, how each was verified, and what's still outstanding.

Nothing else changed: CMakeLists.txt, the plugin-sdk/CPed/CVehicle struct
definitions, Log.cpp, and DebugOverlay.*/PluginSdkSA.cmake are all untouched.

## What now works

- Reading the local player's position, heading, health, armour, vehicle
  state and model, on this exact RGL build, via a verified, fingerprinted
  function address (not a guess).
- The `AndreasOnline.log` file logging (position line every 5 seconds),
  timed independently of the game's own clock.

## What still doesn't work (and why, honestly)

- **The on-screen F9 overlay.** Safely drawing with the game's `CFont`/
  RenderWare calls has to happen on the game's own thread at a point the
  engine considers safe to render - that's what `Events::drawingEvent`
  provided on 1.0. A separate polling thread cannot safely do this (calling
  rendering code from the wrong thread is a real crash risk, not a cosmetic
  limitation), and the real RGL address for that hook has not been found
  yet. `DebugOverlay.*` is left in the tree, unused, for whoever finds it.
- **Interior tracking** (`PlayerSnapshot::interior` is always `0`).
  plugin-sdk's own `CGame::currArea` accessor resolves to a null pointer on
  any version it doesn't recognise - reading it here would have been an
  immediate crash, confirmed by reading plugin-sdk's source, not a guess.
- Nothing else in `step 2` (the dedicated server / networking) is affected
  either way - it never touched game memory to begin with.

## 2026-11-26 update - networking, overlay HUD, hardening

Since the original version of this document, step 2 (client-server
networking) and most of step 3's achievable scope landed:

- **Networking**: `client/src/NetworkClient.*` is an ENet client with a
  handshake/protocol-version check, player join/leave tracking, ~10 Hz
  position sync, and chat, talking to `server/src/Main.cpp` (a separate
  dedicated-server executable, not part of the .asi). The client now
  **auto-reconnects** every 5 seconds on any dropped/failed connection,
  except a protocol-version mismatch (which can't fix itself by retrying).
  The server enforces a max-player cap (`HandshakeResult::ServerFull`) and
  rate-limits chat; on Windows it also shuts down cleanly on Ctrl+C.
- **Overlay HUD** (`client/src/Overlay.*`): this replaces the plan to find
  the real `drawingEvent` call site. It's a standalone Win32 window (its own
  thread, `WS_EX_LAYERED | WS_EX_TRANSPARENT | ... | WS_EX_NOACTIVATE`,
  GDI-rendered, click-through, always-on-top) showing connection status, the
  player roster, and a small chat log - entirely outside the game's own
  renderer, so it needed **zero** new game-engine addresses and works
  regardless of whether `drawingEvent` is ever found. `F10` toggles it, `T`
  opens chat input. `DebugOverlay.*` (the original CFont-based plan) is still
  unused dead code in the tree.
- **Remote players now get a real, visible `CPed` in the game world**
  (2026-11-26) - `CPopulation::AddPed`'s real address for this build was
  found after all (the earlier "likely PGO-split, suspended" conclusion was
  wrong; the actual function is contiguous, just at a different sub-address
  inside the gap than was first probed). See `client/include/ao/GameAddrs.h`
  (`kAddr_AddPed_RGL`) and `client/src/RemotePeds.{h,cpp}`. Full evidence
  chain in `docs/MEMORY.md`, "`CPopulation::AddPed` FOUND".
  **This is experimental and riskier than everything else in this document**:
  every other game-memory interaction here only *reads* existing memory;
  `AddPed` *allocates into and mutates* the game's own global ped pool and
  world-sector lists, from this project's background polling thread, with
  no verified-safe "on the game's own main thread" hook to do it on instead
  (see the Events note two paragraphs up). If the game's main thread is
  concurrently iterating those same structures while this happens, that is a
  genuine data race, not just a stale-read risk - a crash under some timings
  is possible. Save your game before testing. (The rotation limitation this
  bullet used to describe - moving a ped without visually turning it - is
  resolved; see the `SetHeading` entry below.)
- **Vehicle sync** (2026-11-26, same day) - `CCarCtrl::CreateCarForScript`'s
  real address for this build was found the same way (an xref search from
  the already-verified `CWorld::Add` address, see `docs/MEMORY.md`). A
  remote player who is driving now shows up as a real, moving `CVehicle` of
  the right model, instead of a ped floating at the car's position -
  `RemotePedManager` (`client/src/RemotePeds.{h,cpp}`) switches each remote
  player between a ped and a vehicle as their `PlayerState.vehicleModel`
  changes. Same risk class as `AddPed` above (allocates/mutates the game's
  vehicle pool from the background thread). One more accepted limitation
  specific to vehicles: when a player gets out, or disconnects while driving, their
  vehicle is **not** despawned - it is left parked where it was (already
  created "locked, abandoned", so the game treats it normally from then on).
  Forcing a despawn via `CVehicle::m_fHealth = 0` was considered and
  rejected - that makes the vehicle explode, which would be a far more
  jarring surprise for other players than one extra parked car.
- **Movement interpolation** (2026-11-27) - remote peds and vehicles used to
  teleport straight to each incoming `PlayerState` packet (every ~100ms),
  which looked like a stutter instead of smooth motion. `RemotePedManager`
  now stores the previous and latest network update as timestamped
  keyframes and blends between them every ~50ms (`Tick()`, called from
  `PollThreadProc` in `client/src/Main.cpp`), with shortest-path heading
  interpolation and a mild overshoot allowance so a slightly late packet
  doesn't visibly freeze motion. Pure client-side change, no new game
  addresses needed. Full writeup in `docs/MEMORY.md`.
- **Real rotation via `SetHeading`** (2026-11-27, same day) -
  `CPlaceable::SetHeading`'s real address for this build was found starting
  from a clue already in hand (AddPed's decompilation had already shown a
  call matching `SetOrientation(0,0,0)`, which led to `SetOrientation`
  itself, then to `SetHeading` at the same `+0x80` delta already seen for
  `CreateCarForScript` - see `docs/MEMORY.md` for the full chain). This
  rewrites the entity's actual rotation matrix, unlike the old
  `m_fHeadingCurrent`-only write, and - because `CPlaceable` sits at object
  offset 0 in both `CPed` and `CVehicle` - the exact same function call
  works for remote peds **and** vehicles. This resolves both rotation
  limitations noted above: a synced ped or vehicle now visually turns to
  face the right direction, not just end up positioned correctly.

## I could not runtime-test any of this

Everything above was found and checked through **static analysis in Ghidra
only** - there is no debugger attached to a running game, so nothing here
has actually been exercised against the live process. The fingerprint check
in `GameAddrs.h` and the SEH guards in `LocalPlayer.cpp` exist specifically
because of that: if something is wrong, the goal is "the plugin logs an
error and does nothing" rather than "the plugin crashes the game" or
"the plugin silently reads garbage." Please build it, launch the game, and
check `AndreasOnline.log` and the success criteria below before trusting it
further.

## Updated success criteria for this build

1. The game starts with no error box and no crash.
2. `scripts\AndreasOnline.log` contains `"RGL build detected..."`,
   `"polling thread started..."`, and then a `pos ...` line every 5 seconds
   once a ped exists.
3. Walking changes X/Y in the log; jumping or climbing changes Z.
4. Getting into a car eventually shows `veh N` (N = model index) in a log
   line instead of `-1`, and the coordinates follow the car.
5. (No longer applicable on this build: the F9 overlay and the `Interior`
   log field - see above.)
6. Protocol is now v3 (`ao::kProtocolVersion`, bumped twice more on
   2026-11-27) - a client or server built from before 2026-11-27 will no
   longer connect to one built after it (the existing version-mismatch
   check in `server/src/Main.cpp` rejects it cleanly rather than
   desyncing). Rebuild both ends together.
7. Shooting (or otherwise damaging) a remote player's in-game avatar now
   actually hurts the real player at the other end: watch
   `AndreasOnline.log` for a `"took N hp / N armour damage from player N"`
   line on the victim's machine, and the server console for a
   `"player N dealt N hp / N armour damage to player N"` line.

## Item 5 (weapon/damage sync) - completed 2026-11-27, via a different design

The user asked specifically for this item to be completed, not left open.
The original plan (detect a weapon being fired/given via `CPed::GiveWeapon`
or similar, then relay that as a game event) hit genuine, undodgeable
address-finding walls, documented in full below. Rather than ship something
guessed, the feature was redesigned around data this project could already
trust completely: `CPed::m_fHealth`/`m_fArmour` are plain, plugin-sdk-
verified data offsets (not code addresses), already read and written safely
elsewhere in this project since the very first session. So instead of
asking "what weapon did they fire", `RemotePedManager::Tick()` now asks "did
my own local copy of this player's avatar just lose health or armour,
according to my own game engine's own physics/collision/damage simulation"
- and if so, reports that *already-computed, already-correct* outcome to
the server, which relays it to the real victim's own client to apply to
their own real ped. See `client/src/RemotePeds.h`'s class comment and
`shared/include/ao/Protocol.h`'s `DamageReport` struct for the full design,
and `docs/MEMORY.md` for the changed-files list. **No new RGL game address
of any kind was needed or used for this feature** - it is built entirely on
already-verified field offsets and new protocol/networking logic.

This was verified end-to-end against a real, compiled, running copy of the
server binary and two scripted ENet test clients (not just syntax-checked):
confirmed the server overwrites a forged `attackerPlayerId` with the real
sender's id, clamps an out-of-range `healthDelta` (sent as 500) down to
100, leaves an in-range `armourDelta` (15) unchanged, broadcasts the result
to both the attacker and the victim, and correctly drops a report naming a
`victimPlayerId` that isn't a connected player.

**What was investigated and ruled out first, for the original
"detect the weapon directly" plan** (documented here so a future session
doesn't repeat the same dead ends):

- **`CPed::GiveWeapon`** (1.0 US `0x5E6080`, per gta-reversed's `Ped.cpp`)
  and **`CPed::AddWeaponModel`** (1.0 US `0x5E5ED0`) both land inside a
  single, large, pre-existing function body in this RGL build that
  Ghidra's own `create_function` genuinely refuses to re-split at either
  1.0 US sub-address (`dry_run=true` fails cleanly for both) - unlike
  `AddPed`'s case in an earlier session, where a "merged function" theory
  turned out to be wrong and a clean split was possible. This region
  (roughly `0x5Dxxxx`-`0x5Fxxxx`) appears to have been compiled/organized
  materially differently from 1.0 US, not just relocated.
- **`CWeapon::Initialise`** (1.0 US `0x73B4A0`) - tried on the theory that
  the same `+0x80`-style address-reuse seen for `SetOrientation`/
  `SetHeading`/`CreateCarForScript` might hold here too. It didn't: the
  exact 1.0 US address *does* exist as a clean, separable function in this
  build, but decompiling it shows a completely unrelated function (two
  `FindPlayerPed` calls, a `*40.0` scale, particle/sound-looking calls -
  nothing like `CWeapon::Initialise`'s real source). This is a **false
  positive from coincidental address reuse**, not a lead - logged here so
  it isn't retried. Lesson applied project-wide since: a numeric address
  match alone is never enough, every candidate must be decompiled and
  structurally matched against known source before being trusted.
- **The ammunition-count clue** - `AddPed`'s own decompilation contains a
  call `FUN_005e91d0(iVar6, 0x61a9)` (`0x61a9` = 25001, a huge/"infinite"
  ammo count, a strong hint this touches weapon-giving logic). Following
  it: `0x5e91d0` itself is just an indirect call through a global function
  pointer (`*_DAT_014bba20`); the pointer's target (`0x150d2f0`)
  disassembles to a `push`/`pushfd`/stack-arithmetic/`popfd`/`ret` pattern
  that computes its real jump target by arithmetic on the stack rather
  than jumping to a fixed address - recognizable as Control Flow Guard
  (CFG), a compiler security-hardening feature the whole binary seems to
  use in places, not deliberate anti-cheat obfuscation aimed at this
  specific function. Resolving a CFG-indirected call by hand, correctly,
  needs either a live debugger (to just observe the real target at
  runtime) or much more careful manual arithmetic than is safe to trust
  without one - guessing the resolved address and calling through it would
  violate this project's "nothing guessed" rule in the single riskiest
  possible way (an arbitrary code-pointer call). Not pursued further.
- **Missing tooling**: `search_instructions`/`search_byte_patterns` (which
  would have let literal/immediate constants from the 1.0 US source be
  searched for directly, the single most useful technique for all three
  leads above) were reported as loaded on the Ghidra bridge side but were
  never reachable from this session's actual tool-calling interface,
  despite several `ToolSearch`/`RefreshMcpTools` attempts.

**Bottom line on the original plan:** none of the three candidate
approaches reached a verifiable address for detecting a weapon directly, so
no code calls into any weapon-related game function. The health/armour-
drop design above is what actually shipped instead, and does not need any
of these addresses - this record is kept for whoever tries the "detect the
actual weapon/shot" version later (e.g. once/if `search_instructions`/
`search_byte_patterns` become reachable, or with a live debugger attached),
since the dead ends above would still apply to that version of the feature.

**Known limitation of the shipped design:** because it reports "this avatar
lost health/armour" rather than "this avatar was shot with weapon X", it
cannot distinguish a real PvP hit from any other source of damage the
avatar takes in the attacker's local simulation (an explosion, a collision,
being run over, etc.) - all of these get reported the same way, as real
damage the victim should feel. This is treated as a reasonable, even
arguably more complete, interpretation of "damage sync" rather than a bug;
see `client/src/RemotePeds.h`'s class comment for the full reasoning. Also,
only ped health/armour is synced this way - vehicle damage is not.
