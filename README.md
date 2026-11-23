# islamware-css

Internal x86-64 C++20 mod + injector for **Counter-Strike: Source** (x64 `client.dll` / `engine.dll`, build `9540945`).

## Repository Layout

- `src/` — Core DLL sources (`hooks.cpp`, `features.cpp`, `precache.cpp`, `esp_layout.cpp`, `alice_ui.cpp`, `imgui_menu.cpp`, `visual_fx.cpp`, `impacts.cpp`, `grenades.cpp`, `game_events.cpp`, `connect_fix.cpp`, `server_list.cpp`, `glb_preview.cpp`, `crash.cpp`, `main.cpp`, `cheat.h`, `sdk.h`)
- `injector/` — Simple `LoadLibrary` injector (`injector.exe`)
- `third_party/alicekit/` — UI framework, DX9 backend, Dear ImGui, and zipped font atlas (`fonts/packed/packed.zip`)
- `assets/t-leet_face_rig.glb` — Embedded 3D preview model for the ESP preview pane
- `build.sh` — Cross-compile build script (`mingw-w64`)

## Building

### Requirements
- `x86_64-w64-mingw32-g++` (`mingw-w64` on macOS/Linux/WSL or MSYS2 on Windows)
- `python3`
- `unzip`

### Compile
```sh
chmod +x build.sh
./build.sh
```

On the first run, `build.sh` extracts `third_party/alicekit/fonts/packed/packed.zip`, embeds `assets/t-leet_face_rig.glb`, and outputs:
- `build/islamware.dll`
- `build/injector.exe`

You can override the compiler with `CXX=/path/to/x86_64-w64-mingw32-g++ ./build.sh`.

## Usage

1. Launch **Counter-Strike: Source** (x64).
2. Place `build/islamware.dll` and `build/injector.exe` in the same folder (or inject `islamware.dll` into `cstrike_win64.exe` / `hl2.exe`).
3. Run `injector.exe` while the game is running.
4. Press `Insert` in-game to toggle the menu.
