# Andreas Online

Independent online multiplayer for GTA San Andreas (Rockstar Games Launcher
build - see `docs/RGL_COMPAT_NOTES.md`). No SA-MP / MTA code or dependencies.

```
AndreasOnline/
├─ CMakeLists.txt          root: options, static MSVC runtime, ao_shared
├─ CMakePresets.json        preset "vs2022-x86" (VS 2022, Win32)
├─ cmake/
│  └─ PluginSdkSA.cmake     imports plugin-sdk as target PluginSDK::SA
├─ shared/include/ao/       code shared by client + server: Version.h, Protocol.h (wire format)
├─ client/                  ASI plugin  -> AndreasOnline.asi
│  └─ src/
│     ├─ Main.cpp           plugin entry, polling thread, wires network <-> overlay
│     ├─ LocalPlayer.*      reads local player state into a plain snapshot
│     ├─ NetworkClient.*    ENet client: handshake, auto-reconnect, player roster, chat
│     ├─ NetworkConfig.*    reads AndreasOnline.cfg (server_ip/port/player_name)
│     ├─ Overlay.*          standalone Win32/GDI HUD: status, player list, chat (F10/T)
│     └─ Log.*              file logger (scripts/AndreasOnline.log)
├─ server/                  dedicated server (ENet), Windows + Linux
│  └─ src/
│     ├─ Main.cpp           connection handling, player cap, chat rate limiting/sanitizing
│     └─ ServerConfig.*     reads server.cfg (port/max_players)
└─ docs/
   ├─ MEMORY.md             every game address/offset the client relies on + how it was verified
   ├─ RGL_COMPAT_NOTES.md   why this exe needed different addresses, and what's still missing
   └─ DEPLOYMENT.md         getting the server reachable over LAN/internet, port forwarding
```

## What works today

- **Client**: reads the local player's position/heading/health/armour/vehicle
  on this exact RGL build via a verified, fingerprinted address (see
  `docs/MEMORY.md`). Logs to `scripts/AndreasOnline.log`.
- **Networking**: ENet client <-> dedicated server. Handshake with protocol
  version check, player join/leave broadcast, ~10 Hz position sync, chat.
  The client **auto-reconnects** (every 5s) if the connection drops or the
  server isn't up yet when the game starts - no plugin/game restart needed.
  It stops retrying permanently only on a protocol-version mismatch (that
  can't fix itself); it keeps retrying on "server full" (a slot may open up).
- **Server**: enforces a max-player cap (`HandshakeResult::ServerFull`),
  rate-limits chat to cut down on spam, and shuts down cleanly on Ctrl+C /
  console close (disconnects everyone instead of just dying).
- **Overlay HUD**: a separate always-on-top, click-through window (not drawn
  by the game's own renderer, so it needs zero game hooks) shows connection
  status, the current player roster, and a small chat log.
  - `F10` - show/hide the overlay
  - `T` - open chat input, `Enter` - send, `Esc` - cancel
  - Chat input is ASCII only (A-Z/0-9/space) for now.
- **Remote players are rendered as real in-game characters** (2026-11-26):
  `CPopulation::AddPed`'s address for this RGL build was found (see
  `docs/MEMORY.md`) and is used by `client/src/RemotePeds.*` to spawn and
  move a `CPed` for every connected player. **This is experimental** - it is
  the first write/allocate call in this project against the game's own
  memory, made from a background thread with no verified-safe main-thread
  hook, which is a real (if narrow) crash risk. Save your game before
  testing.
- **Vehicle sync** (2026-11-26, same day): a remote player who is driving
  now shows up as a real moving vehicle of the right model, not a ped
  floating at the car's position (`CCarCtrl::CreateCarForScript`'s address
  was found the same way as `AddPed` - see `docs/MEMORY.md`). Same
  experimental/crash-risk caveat as above. When a player gets out or
  disconnects while driving, their vehicle is left parked rather than
  despawned (see `docs/RGL_COMPAT_NOTES.md` for why).
- **Movement is smoothed (interpolated)** (2026-11-27): remote peds and
  vehicles used to teleport straight to each incoming network update
  (~100ms apart), which looked like a stutter. They now blend smoothly
  between updates instead (see `docs/MEMORY.md`). Pure client-side change,
  no new risk beyond what's disclosed above.
- **Real rotation** (2026-11-27, same day): remote peds and vehicles now
  visually turn to face the right direction, using `CPlaceable::SetHeading`'s
  real address for this build (found starting from a clue already in hand
  from the AddPed investigation - see `docs/MEMORY.md`). One address covers
  both peds and vehicles.
- **Real player models, scoped** (2026-11-27, same day): a remote player
  using the default "CJ" model or the alternate player-select skin
  (`MODEL_CSPLAY`) now appears as that real model instead of always the
  same generic civilian. Any other custom skin still falls back to the
  generic civilian, because safely checking an arbitrary model is loaded
  on the viewer's machine needs a game address that wasn't found this
  session (see `docs/MEMORY.md` for the full reasoning). Protocol bumped
  to v2 - rebuild both client and server together.
- **Damage sync** (2026-11-27, same day): shooting (or otherwise damaging)
  a remote player's in-game avatar now really hurts the real player at the
  other end. Built around data this project already trusted (plain
  `CPed::m_fHealth`/`m_fArmour` fields, not a weapon-detection game call it
  couldn't safely find - see `docs/MEMORY.md`/`docs/RGL_COMPAT_NOTES.md`
  for why and for the full design). Verified end-to-end against a real,
  running server binary and scripted test clients, not just compiled.
  Protocol bumped to v3 - rebuild both client and server together.

## What does NOT work yet (and why)

- Interior tracking (`CGame::currArea`) - would be a null-pointer read on
  this build, so it's left disabled rather than crashing.
- The old CFont-based F9 overlay (`DebugOverlay.*`) is unused dead code,
  superseded by the Win32 HUD above, which needs no `drawingEvent` hook.
- Game-logic event hooks (the old `Events::*` approach) - dropped by
  explicit decision; superseded by the Win32 HUD and not worth the
  code-patching risk it would need on this build (see `docs/MEMORY.md`).
- Arbitrary custom ped skins for remote players beyond CJ/CSPLAY (see
  "Real player models, scoped" above) - investigated in depth and not
  completed; see `docs/RGL_COMPAT_NOTES.md` for the detailed blockers
  found. (Weapon/damage sync, previously listed here too, is now done -
  see "Damage sync" above - via a redesign that doesn't need the same
  blocked game address.)
- Vehicle damage sync - only ped health/armour is synced by the new
  damage-sync feature above; a damaged/destroyed vehicle is not yet
  propagated to other clients.

## Build the client

### 0. One-time: build plugin-sdk
1. In your plugin-sdk checkout, run the plugin-sdk installer (or premake) to
   generate its solution and set the `PLUGIN_SDK_DIR` environment variable.
2. Open the generated plugin-sdk solution and build **Plugin_SA** in
   **Release** and **zDebug** (Win32). You must get:
   - `%PLUGIN_SDK_DIR%\output\lib\Plugin.lib`
   - `%PLUGIN_SDK_DIR%\output\lib\Plugin_d.lib`

### 1. Configure + build
```bat
:: optional: makes the build copy the .asi into <game>\scripts automatically
setx GTA_SA_DIR "D:\Games\GTA San Andreas"

cd AndreasOnline
cmake --preset vs2022-x86
cmake --build --preset client-release
```
Without the preset: `cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DPLUGIN_SDK_DIR=C:/dev/plugin-sdk`

Output: `build\vs2022-x86\bin\Release\AndreasOnline.asi`

### 2. Install
Copy `AndreasOnline.asi` to `<game>\scripts\` (or the game root - both are
scanned by the ASI Loader).

### 3. Configure the server to connect to
Create `AndreasOnline.cfg` next to `AndreasOnline.asi` (optional - defaults
to `127.0.0.1:7777`):
```ini
server_ip=203.0.113.10
server_port=7777
player_name=Amir
```

## Build the server

The server is plain ENet + C++17, no plugin-sdk dependency, builds on
Windows and Linux:
```bat
cmake --preset server
cmake --build --preset server-release
```
Output: `build\server\bin\AndreasOnlineServer(.exe)`

Run it:
```
AndreasOnlineServer [port] [maxPlayers]
```
Both arguments are optional (defaults: port `7777`, max players `32`); they
override `server.cfg` (`port=`/`max_players=`) if one exists next to the
executable. See `docs/DEPLOYMENT.md` for getting it reachable over LAN or
the internet (port forwarding, firewall rule, keeping it running).

## Success criteria

1. The game starts with no error box and no crash.
2. `scripts\AndreasOnline.log` shows the RGL detection line, the polling
   thread starting, and a `pos ...` line every 5 seconds.
3. Walking changes X/Y in the log; jumping or climbing changes Z; getting
   into a car eventually shows a real model id instead of `-1`.
4. With a server running and `AndreasOnline.cfg` pointing at it: the overlay
   (`F10`) shows "connected (id N)", and a second client shows up in the
   player list within about a second of joining.
5. Killing/restarting the server: the overlay falls back to "no server",
   then back to "connected" automatically within ~5 seconds of the server
   coming back - no need to restart the game.
6. Chat (`T`, type, `Enter`) sent from one client appears in every other
   connected client's overlay chat log.
