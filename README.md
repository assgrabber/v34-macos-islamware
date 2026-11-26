# islamware-css

Internal C++20 mod + injector supporting both **Counter-Strike: Source v93 x64** (`cstrike_win64.exe`, build `9540945`) and **Counter-Strike: Source v34** (`hl2.exe`, 32-bit / Wine).

## Repository Layout

- `src/` — Shared dual-target DLL sources (`hooks.cpp`, `features.cpp`, `precache.cpp`, `esp_layout.cpp`, `alice_ui.cpp`, `imgui_menu.cpp`, `visual_fx.cpp`, `impacts.cpp`, `grenades.cpp`, `game_events.cpp`, `connect_fix.cpp`, `server_list.cpp`, `glb_preview.cpp`, `crash.cpp`, `main.cpp`, `cheat.h`, `sdk.h`)
- `injector/` — `LoadLibrary` injector (`injector.exe` for x64 / `injector_v34.exe` for v34 x86)
- `loader/` — Minimalist macOS `.app` loader (`IslamwareLoader.app`) + CLI setup/launch script (`setup_v34.sh`)
- `third_party/alicekit/` — UI framework, DX9 backend, Dear ImGui, and zipped font atlas (`fonts/packed/packed.zip`)
- `assets/t-leet_face_rig.glb` — Embedded 3D preview model for the ESP preview pane
- `build.sh` — Cross-compile build script (`mingw-w64`, supports `all`, `x64`, `v34`)
- `V34_TUTORIAL.md` — Complete setup & launch guide for CS:S v34 `.exe`

## Building

### Requirements
- `x86_64-w64-mingw32-g++` (for v93 x64 CS:S)
- `i686-w64-mingw32-g++` (for v34 x86 CS:S)
- `python3`, `unzip`
- `swiftc` (macOS only, for building `IslamwareLoader.app`)

### Compile
```sh
chmod +x build.sh
./build.sh        # builds both x64 and v34 (same as ./build.sh all)
./build.sh x64    # builds v93 (x64) only
./build.sh v34    # builds v34 (x86) only
```

Outputs in `build/`:
- `build/islamware.dll` + `build/injector.exe` (v93 x64 CS:S, targets `cstrike_win64.exe`)
- `build/islamware_v34.dll` + `build/injector_v34.exe` (v34 x86 CS:S, targets `hl2.exe`)

## Usage

### Modern CS:S v93 (x64)
1. Launch **Counter-Strike: Source** (`cstrike_win64.exe`).
2. Run `injector.exe <full path to islamware.dll>`.
3. Press `Insert` in-game to toggle the menu.

### CS:S v34 (32-bit / Wine)
Download CS:S v34 from **[https://source-cs.ru](https://source-cs.ru)** and follow **[V34_TUTORIAL.md](V34_TUTORIAL.md)** using either:
- **Minimalist `.app` Loader**: `./loader/build_loader.sh` → open `loader/build/IslamwareLoader.app`
- **CLI Script**: `./loader/setup_v34.sh install <css_v34.exe>` → `./loader/setup_v34.sh launch`
