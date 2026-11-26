# Counter-Strike: Source v34 — Setup & Launch Tutorial

This guide explains how to download CS:S v34, prepare the `.exe` installer on macOS (or Windows/Wine), and launch it with **islamware-css** automatically injected.

## 1. Prerequisites (macOS)

Before installing CS:S v34, make sure you have **Homebrew**, **Rosetta 2** (Apple Silicon), and **Wine** (or CrossOver / SikarugirCX for best macOS FPS). `IslamwareLoader.app` also checks these automatically and displays copyable commands for any missing requirement:

```sh
# Homebrew
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Rosetta 2 (Apple Silicon)
softwareupdate --install-rosetta --agree-to-license

# Wine (via Gcenx tap, since core homebrew/cask disabled wine-stable)
brew tap gcenx/wine && brew install --cask gcenx/wine/wine-stable && xattr -dr com.apple.quarantine "/Applications/Wine Stable.app"
```

## 2. Download CS:S v34

Download the Counter-Strike: Source v34 `.exe` installer from:
- **[https://source-cs.ru](https://source-cs.ru)**

## 3. Build the DLL & Injector (if not already built)

From the repository root:
```sh
chmod +x build.sh
./build.sh all
```
This builds both versions from one codebase:
- `build/islamware.dll` + `build/injector.exe` (CS:S v93 / x64)
- `build/islamware_v34.dll` + `build/injector_v34.exe` (CS:S v34 / x86)

---

## Method A: Minimalist `.app` Loader (Recommended on macOS)

1. Build the native macOS loader app:
   ```sh
   chmod +x loader/build_loader.sh
   ./loader/build_loader.sh
   ```
2. Open **`loader/build/IslamwareLoader.app`**.
3. **Start tab**: The window shows **"Place an .exe in the folder."** — click the box (or drag & drop your downloaded `.exe` from [https://source-cs.ru](https://source-cs.ru) directly onto the window).
4. **Installation tab**: Once the `.exe` is detected, click **Install**. The loader automatically:
   - Initializes the Wine prefix
   - Runs the CS:S v34 installer
   - Applies macOS WineD3D OpenGL performance tweaks (`StrictDrawOrdering=disabled`, `OffscreenRenderingMode=fbo`, `WINEDEBUG=-all`)
   - Writes an FPS-optimized `cstrike/cfg/autoexec.cfg`
   - Installs `islamware_v34.dll` and `injector_v34.exe`
5. **Main tab**: Click **Launch** to start CS:S v34 and automatically inject `islamware_v34.dll` (or click **Uninstall** to cleanly remove the v34 installation).
6. Press **`Insert`** in-game to open/close the menu.

---

## Method B: Command-Line Script (`loader/setup_v34.sh`)

If you prefer the terminal:

```sh
chmod +x loader/setup_v34.sh

# 1. Install CS:S v34 from your downloaded .exe and apply all FPS/Wine optimizations
./loader/setup_v34.sh install ~/Downloads/css_v34_setup.exe

# 2. Launch CS:S v34 with automatic islamware_v34.dll injection
./loader/setup_v34.sh launch

# Optional: Uninstall
./loader/setup_v34.sh uninstall
```

### Customizing Wine / Prefix
If you use a specific Wine binary or existing prefix (e.g., Sikarugir, CrossOver, Whisky, or Homebrew Wine), pass environment variables:
```sh
WINE_BIN="/opt/homebrew/bin/wine" WINEPREFIX="$HOME/my_v34_prefix" ./loader/setup_v34.sh install ~/Downloads/css_v34_setup.exe
```

## Notes
- In-game menu key defaults to **`Insert`** (rebindable under **Settings → Menu key**).
- Custom player models placed in `cstrike/models/player/` or `cstrike/custom/<pack>/models/` are automatically scanned and precached client-side on both v93 and v34.
