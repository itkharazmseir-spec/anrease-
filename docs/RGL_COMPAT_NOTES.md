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
