# Andreas Online

Independent online multiplayer for GTA San Andreas 1.0 US (x86).
No SA-MP / MTA code or dependencies.

```
AndreasOnline/
├─ CMakeLists.txt          root: options, static MSVC runtime, ao_shared
├─ CMakePresets.json       preset "vs2022-x86" (VS 2022, Win32)
├─ cmake/
│  └─ PluginSdkSA.cmake    imports plugin-sdk as target PluginSDK::SA
├─ shared/include/ao/      code shared by client + server (Version.h, later protocol)
├─ client/                 ASI plugin  -> AndreasOnline.asi
│  └─ src/
│     ├─ Main.cpp          plugin entry, registers game events
│     ├─ LocalPlayer.*     reads local player state into a plain snapshot
│     ├─ DebugOverlay.*    draws the snapshot with CFont, toggle F9
│     └─ Log.*             file logger (scripts/AndreasOnline.log)
├─ server/                 (step 2)
└─ docs/MEMORY.md          every game address/offset we rely on + where to verify
```

## Build (step 1)

### 0. One-time: build plugin-sdk
1. In your plugin-sdk checkout, run the plugin-sdk installer (or premake) to
   generate its solution and set the `PLUGIN_SDK_DIR` environment variable.
2. Open the generated plugin-sdk solution and build the **Plugin_SA** project in
   **Release** and **zDebug** (Win32). You must get:
   - `%PLUGIN_SDK_DIR%\output\lib\Plugin.lib`
   - `%PLUGIN_SDK_DIR%\output\lib\Plugin_d.lib`

### 1. Configure + build the client
```bat
:: optional: makes the build copy the .asi into <game>\scripts automatically
setx GTA_SA_DIR "D:\Games\GTA San Andreas"

cd AndreasOnline
cmake --preset vs2022-x86
cmake --build --preset client-release
```
Without the preset: `cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DPLUGIN_SDK_DIR=C:/dev/plugin-sdk`

Output: `build\vs2022-x86\bin\Release\AndreasOnline.asi`
(Or open `build\vs2022-x86\AndreasOnline.sln` in Visual Studio.)

### 2. Install
Copy `AndreasOnline.asi` to `<game>\scripts\` (or the game root - both are
scanned by the ASI Loader).

## Success criteria for step 1
1. The game starts with no error box.
2. After loading a save, top-left shows `Andreas Online 0.1.0 (proto 1) [F9]`
   and X / Y / Z / Heading / Interior / In vehicle / HP lines.
3. Walking changes X/Y; jumping or climbing changes Z.
4. Getting into a car shows `In vehicle: yes (model N)` and the coords follow the car.
5. Entering an interior changes `Interior` from 0.
6. F9 hides/shows the overlay; it is hidden in the pause menu.
7. `<game>\scripts\AndreasOnline.log` contains the detected game version and
   a `pos ...` line every 5 seconds.
