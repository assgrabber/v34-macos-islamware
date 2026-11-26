#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# islamware-css v34 macOS/Wine Setup & Launcher CLI
# Prepares a Counter-Strike: Source v34 .exe installer (from https://source-cs.ru),
# applies macOS Wine/FPS optimizations, installs islamware_v34.dll + injector,
# and launches the game with automatic DLL injection.
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DATA_DIR="${ISLAMWARE_V34_DIR:-$HOME/Library/Application Support/IslamwareV34}"
LAUNCH_DIR="${ISLAMWARE_LAUNCH_DIR:-$(pwd)}"
DROP_DIR="${ISLAMWARE_DROP_DIR:-$DATA_DIR/drop}"
PREFIX_DIR="${WINEPREFIX:-$DATA_DIR/prefix}"
GAME_REL="drive_c/Program Files (x86)/Counter-Strike Source"
GAME_DIR="$PREFIX_DIR/$GAME_REL"

mkdir -p "$DATA_DIR" "$DROP_DIR"

# Locate Wine binary (prefer CrossOver / SikarugirCX for 2-3x better macOS FPS, then Homebrew Wine)
find_wine() {
    if [[ -n "${WINE_BIN:-}" && -x "$WINE_BIN" ]]; then
        echo "$WINE_BIN"
        return 0
    fi
    for cand in \
        "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" \
        "$HOME/Applications/Counter-Strike Source.app/Contents/SharedSupport/wine/bin/wine" \
        "$REPO_DIR/../Counter-Strike Source-v93.app/Contents/SharedSupport/wine/bin/wine" \
        "/Applications/Whisky.app/Contents/Resources/Libraries/Wine/bin/wine"
    do
        if [[ -n "$cand" && -x "$cand" ]]; then
            echo "$cand"
            return 0
        fi
    done
    # Generically detect any nearby Sikarugir/Wineskin app bundle providing Wine
    local base app
    for base in "$LAUNCH_DIR" "$(dirname "$LAUNCH_DIR")" "$HOME/Applications" "/Applications"; do
        [[ -d "$base" ]] || continue
        while IFS= read -r app; do
            if [[ -x "$app/Contents/SharedSupport/wine/bin/wine" ]]; then
                echo "$app/Contents/SharedSupport/wine/bin/wine"
                return 0
            fi
        done < <(find "$base" -maxdepth 2 -name "*.app" -type d 2>/dev/null || true)
    done
    for cand in \
        "$(command -v wine 2>/dev/null || true)" \
        "$(command -v wine64 2>/dev/null || true)" \
        "/opt/homebrew/bin/wine" \
        "/usr/local/bin/wine" \
        "/Applications/Wine Crossover.app/Contents/Resources/wine/bin/wine" \
        "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine"
    do
        if [[ -n "$cand" && -x "$cand" ]]; then
            echo "$cand"
            return 0
        fi
    done
    return 1
}

find_dll() {
    for cand in \
        "$SCRIPT_DIR/islamware_v34.dll" \
        "$REPO_DIR/build/islamware_v34.dll" \
        "$DATA_DIR/islamware_v34.dll"
    do
        if [[ -f "$cand" ]]; then echo "$cand"; return 0; fi
    done
    return 1
}

find_injector() {
    for cand in \
        "$SCRIPT_DIR/injector_v34.exe" \
        "$REPO_DIR/build/injector_v34.exe" \
        "$DATA_DIR/injector_v34.exe"
    do
        if [[ -f "$cand" ]]; then echo "$cand"; return 0; fi
    done
    return 1
}

find_drop_exe() {
    local dir found=""
    for dir in "$LAUNCH_DIR" "$SCRIPT_DIR" "$DROP_DIR"; do
        [[ -d "$dir" ]] || continue
        found="$(find "$dir" -maxdepth 1 -type f -iname "*.exe" \
            ! -iname "injector*.exe" ! -iname "hl2.exe" ! -iname "launch_*.exe" ! -iname "unins*.exe" \
            2>/dev/null | head -n 1 || true)"
        if [[ -n "$found" ]]; then
            echo "$found"
            return 0
        fi
    done
    return 1
}

find_installed_hl2() {
    if [[ -f "$GAME_DIR/hl2.exe" ]]; then
        echo "$GAME_DIR"
        return 0
    fi
    local alt=""
    if [[ -d "$PREFIX_DIR/drive_c" ]]; then
        alt="$(find "$PREFIX_DIR/drive_c" -maxdepth 4 -type f -iname "hl2.exe" 2>/dev/null | head -n 1 || true)"
        if [[ -n "$alt" ]]; then
            dirname "$alt"
            return 0
        fi
    fi
    if [[ -d "$LAUNCH_DIR" ]]; then
        alt="$(find "$LAUNCH_DIR" -maxdepth 3 -type f -iname "hl2.exe" 2>/dev/null | head -n 1 || true)"
        if [[ -n "$alt" ]]; then
            dirname "$alt"
            return 0
        fi
    fi
    return 1
}

apply_wine_optimizations() {
    local user_reg="$PREFIX_DIR/user.reg"
    if [[ -f "$user_reg" ]]; then
        python3 - "$user_reg" <<'PY'
import sys, re
p = sys.argv[1]
with open(p, "r", encoding="utf-8", errors="ignore") as f:
    txt = f.read()

block = (
    "[Software\\\\Wine\\\\Direct3D]\n"
    "\"renderer\"=\"gl\"\n"
    "\"StrictDrawOrdering\"=\"disabled\"\n"
    "\"OffscreenRenderingMode\"=\"fbo\"\n"
    "\"VideoMemorySize\"=\"2048\"\n"
)

if "[Software\\\\Wine\\\\Direct3D]" in txt:
    for key, val in [
        ("StrictDrawOrdering", "disabled"),
        ("OffscreenRenderingMode", "fbo"),
        ("VideoMemorySize", "2048"),
    ]:
        if f'"{key}"=' not in txt:
            txt = txt.replace("[Software\\\\Wine\\\\Direct3D]",
                              f"[Software\\\\Wine\\\\Direct3D]\n\"{key}\"=\"{val}\"", 1)
    # Remove csmt if present (causes macOS WineD3D Reset deadlock)
    txt = re.sub(r'^"csmt"=.*\n', '', txt, flags=re.MULTILINE)
else:
    txt += "\n" + block

with open(p, "w", encoding="utf-8") as f:
    f.write(txt)
PY
    fi
}

setup_wine_env() {
    local wine_bin="$1"
    local wine_root app_contents extra_libs=""
    wine_root="$(cd "$(dirname "$wine_bin")/.." && pwd)"
    [[ -d "$wine_root/lib" ]] && extra_libs="$wine_root/lib"
    [[ -d "$wine_root/lib64" ]] && extra_libs="${extra_libs:+$extra_libs:}$wine_root/lib64"
    app_contents="$(cd "$wine_root/../.." 2>/dev/null && pwd || true)"
    if [[ -n "$app_contents" && -d "$app_contents/Frameworks" ]]; then
        extra_libs="${extra_libs:+$extra_libs:}$app_contents/Frameworks"
    fi
    if [[ -n "$extra_libs" ]]; then
        export DYLD_FALLBACK_LIBRARY_PATH="${extra_libs}:/opt/homebrew/lib:/usr/local/lib:/usr/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
    fi
}

ensure_wine_fonts() {
    local fdir="$PREFIX_DIR/drive_c/windows/Fonts"
    mkdir -p "$fdir"
    local pairs=(
        "Tahoma.ttf:tahoma.ttf"
        "Tahoma Bold.ttf:tahomabd.ttf"
        "Arial.ttf:arial.ttf"
        "Arial Bold.ttf:arialbd.ttf"
        "Verdana.ttf:verdana.ttf"
        "Verdana Bold.ttf:verdanab.ttf"
        "Courier New.ttf:cour.ttf"
    )
    local p src dst
    for p in "${pairs[@]}"; do
        src="/System/Library/Fonts/Supplemental/${p%%:*}"
        dst="$fdir/${p##*:}"
        if [[ -f "$src" && ! -f "$dst" ]]; then
            cp -f "$src" "$dst" 2>/dev/null || true
        fi
    done
}

write_autoexec() {
    local gdir="$1"
    mkdir -p "$gdir/cstrike/cfg"
    cat > "$gdir/cstrike/cfg/autoexec.cfg" <<'CFG'
// Auto-generated FPS optimization config v2 for CS:S v34 under Wine/macOS
fps_max 999
mat_vsync 0
mat_hdr_level 0
mat_antialias 0
mat_forceaniso 0
mat_trilinear 0
mat_specular 0
mat_bumpmap 0
mat_fastnobump 1
mat_reducefillrate 1
mat_filterlightmaps 0
mat_disable_bloom 1
mat_picmip 2
mat_wateroverlaysize 8
r_shadows 0
r_shadowmaxrendered 0
r_shadowrendertotexture 0
r_dynamic 0
r_maxdlights 0
r_worldlights 0
r_3dsky 0
r_fastzreject -1
r_eyes 0
r_teeth 0
r_flex 0
r_drawmodeldecals 0
r_drawdetailprops 0
r_propsmaxdist 400
r_renderoverlayfragment 0
r_ropetranslucent 0
rope_smooth 0
rope_wind_dist 0
rope_subdiv 0
r_waterdrawreflection 0
r_waterdrawrefraction 0
r_waterforceexpensive 0
r_waterforcereflectentities 0
r_decals 16
mp_decals 16
r_lod 2
r_rootlod 2
cl_detaildist 0
cl_detailfade 0
cl_ejectbrass 0
cl_ragdoll_physics_enable 0
cl_show_splashes 0
cl_smooth 0
muzzleflash_light 0
cl_forcepreload 0
CFG
}

cmd_check_reqs() {
    local has_brew=0 has_rosetta=0 has_wine=0
    if command -v brew >/dev/null 2>&1 || [[ -x /opt/homebrew/bin/brew || -x /usr/local/bin/brew ]]; then
        has_brew=1
    fi
    if [[ "$(uname -m)" != "arm64" ]] || [[ -f /Library/Apple/usr/share/rosetta/rosetta ]] || pgrep -q oahd 2>/dev/null; then
        has_rosetta=1
    fi
    if find_wine >/dev/null 2>&1; then
        has_wine=1
    fi
    echo "brew=$has_brew"
    echo "rosetta=$has_rosetta"
    echo "wine=$has_wine"
}

cmd_status() {
    if find_installed_hl2 >/dev/null 2>&1; then
        echo "installed"
    elif find_drop_exe >/dev/null 2>&1; then
        echo "ready_to_install"
    else
        echo "need_exe"
    fi
}

cmd_install() {
    local exe_path="${1:-}"
    if [[ -z "$exe_path" ]]; then
        exe_path="$(find_drop_exe || true)"
    fi
    if [[ -z "$exe_path" || ! -f "$exe_path" ]]; then
        echo "Error: No CS:S v34 .exe installer found."
        echo "Download CS:S v34 from https://source-cs.ru and place the .exe in:"
        echo "  $DROP_DIR"
        exit 1
    fi

    local wine_bin
    wine_bin="$(find_wine)" || {
        echo "Error: Wine not found. Install Wine via 'brew tap gcenx/wine && brew install --cask gcenx/wine/wine-stable' or CrossOver/Sikarugir."
        exit 1
    }

    echo "[1/5] Using Wine: $wine_bin"
    echo "[2/5] Preparing Wine prefix: $PREFIX_DIR"
    setup_wine_env "$wine_bin"
    export WINEPREFIX="$PREFIX_DIR"
    unset WINEARCH
    export WINEDEBUG="-all"
    export WINEMSYNC=1
    export MVK_CONFIG_LOG_LEVEL="${MVK_CONFIG_LOG_LEVEL:-0}"

    "$wine_bin" wineboot --init >/dev/null 2>&1 || true

    echo "[3/5] Running installer: $(basename "$exe_path")"
    local win_target="C:\\Program Files (x86)\\Counter-Strike Source"
    # Detect installer type: Smart Install Maker uses /S /P=<dir>, Inno Setup uses /VERYSILENT /DIR=<dir>
    if head -c 524288 "$exe_path" | grep -aq "Smart Install Maker"; then
        "$wine_bin" "$exe_path" /S /NR "/P=$win_target" >/dev/null 2>&1 || true
    else
        "$wine_bin" "$exe_path" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART "/DIR=$win_target" >/dev/null 2>&1 || true
    fi

    local installed_dir=""
    installed_dir="$(find_installed_hl2 || true)"
    if [[ -z "$installed_dir" ]]; then
        echo "Silent install did not produce hl2.exe; launching interactive installer..."
        if head -c 524288 "$exe_path" | grep -aq "Smart Install Maker"; then
            "$wine_bin" "$exe_path" "/P=$win_target" || true
        else
            "$wine_bin" "$exe_path" "/DIR=$win_target" || true
        fi
        installed_dir="$(find_installed_hl2 || true)"
    fi

    if [[ -z "$installed_dir" ]]; then
        echo "Error: Installation finished but hl2.exe was not found in $PREFIX_DIR/drive_c."
        exit 1
    fi

    echo "[4/5] Applying Wine & CS:S v34 FPS optimizations..."
    apply_wine_optimizations
    write_autoexec "$installed_dir"

    echo "[5/5] Installing islamware_v34.dll and injector_v34.exe..."
    local dll_src inj_src
    dll_src="$(find_dll || true)"
    inj_src="$(find_injector || true)"
    if [[ -n "$dll_src" ]]; then
        cp "$dll_src" "$installed_dir/islamware_v34.dll"
        cp "$dll_src" "$installed_dir/oldcsshook.dll"
    else
        echo "Warning: islamware_v34.dll not found yet. Run './build.sh v34' first."
    fi
    if [[ -n "$inj_src" ]]; then
        cp "$inj_src" "$installed_dir/injector_v34.exe"
    fi

    echo "Done! Installed to: $installed_dir"
    echo "Run '$0 launch' to start CS:S v34 with islamware."
}

cmd_launch() {
    local installed_dir
    installed_dir="$(find_installed_hl2)" || {
        echo "Error: CS:S v34 is not installed yet. Run '$0 install' first."
        exit 1
    }

    local wine_bin
    wine_bin="$(find_wine)" || {
        echo "Error: Wine not found."
        exit 1
    }

    # Keep DLL & injector fresh if rebuilt
    local dll_src inj_src
    dll_src="$(find_dll || true)"
    inj_src="$(find_injector || true)"
    if [[ -n "$dll_src" ]]; then
        cp "$dll_src" "$installed_dir/islamware_v34.dll"
        cp "$dll_src" "$installed_dir/oldcsshook.dll"
    fi
    if [[ -n "$inj_src" ]]; then
        cp "$inj_src" "$installed_dir/injector_v34.exe"
    fi

    # Upgrade auto-generated autoexec.cfg to v2 FPS optimizations (leaves custom user autoexec untouched)
    if [[ ! -f "$installed_dir/cstrike/cfg/autoexec.cfg" ]] || grep -q "Auto-generated FPS optimization config for CS:S v34" "$installed_dir/cstrike/cfg/autoexec.cfg" 2>/dev/null; then
        write_autoexec "$installed_dir"
    fi

    setup_wine_env "$wine_bin"
    export WINEPREFIX="$PREFIX_DIR"
    unset WINEARCH
    export LANG=en_US.UTF-8
    export LC_ALL=en_US.UTF-8
    export WINEDEBUG="-all"
    export WINEMSYNC=1
    export WINEESYNC=0
    export MOLTENVKCX=1
    export MVK_CONFIG_LOG_LEVEL="${MVK_CONFIG_LOG_LEVEL:-0}"

    # If Wine version changed (e.g. Wine 11 -> SikarugirCX Wine 9), refresh the prefix
    # so DLLs and VGUI fonts match the active Wine binary (never deleting an in-prefix game).
    local cur_wine_ver=""
    cur_wine_ver="$("$wine_bin" --version 2>/dev/null || true)"
    local prev_wine_ver=""
    [[ -f "$PREFIX_DIR/.wine_version" ]] && prev_wine_ver="$(cat "$PREFIX_DIR/.wine_version" 2>/dev/null || true)"
    if [[ -n "$cur_wine_ver" && "$cur_wine_ver" != "$prev_wine_ver" ]]; then
        if [[ "$installed_dir" != "$PREFIX_DIR/"* ]]; then
            rm -rf "$PREFIX_DIR"
            "$wine_bin" wineboot --init >/dev/null 2>&1 || true
        else
            "$wine_bin" wineboot -u >/dev/null 2>&1 || true
        fi
        mkdir -p "$PREFIX_DIR"
        echo "$cur_wine_ver" > "$PREFIX_DIR/.wine_version"
    fi

    # Install macOS Supplemental Windows core fonts (Tahoma/Arial/Verdana/Courier)
    # so CS:S VGUI menus, buttons, and HUD text always render cleanly.
    ensure_wine_fonts

    # If a large native DXVK d3d9.dll (>500KB) was previously placed in the prefix,
    # remove it and run wineboot -u so Wine restores its builtin d3d9.dll.
    local need_restore=0
    for f in "$PREFIX_DIR/drive_c/windows/syswow64/d3d9.dll" "$PREFIX_DIR/drive_c/windows/system32/d3d9.dll"; do
        if [[ -f "$f" ]] && [[ $(wc -c < "$f" 2>/dev/null || echo 0) -gt 500000 ]]; then
            rm -f "$f"
            need_restore=1
        elif [[ ! -f "$f" ]]; then
            need_restore=1
        fi
    done
    rm -f "$installed_dir/dxvk.conf" 2>/dev/null || true
    if [[ "$need_restore" -eq 1 ]]; then
        "$wine_bin" wineboot -u >/dev/null 2>&1 || true
    fi

    # Match the exact working Counter-Strike Source-v93.app WineD3D GL registry settings
    # (no csmt, no VideoMemorySize override, no RetinaMode override).
    if [[ ! -f "$PREFIX_DIR/user.reg" ]] || ! grep -q '"renderer"="gl"' "$PREFIX_DIR/user.reg" 2>/dev/null || grep -q '"csmt"=' "$PREFIX_DIR/user.reg" 2>/dev/null || grep -q '"RetinaMode"=' "$PREFIX_DIR/user.reg" 2>/dev/null; then
        "$wine_bin" reg add 'HKCU\Software\Wine\Direct3D' /v renderer /t REG_SZ /d gl /f >/dev/null 2>&1 || true
        "$wine_bin" reg add 'HKCU\Software\Wine\Direct3D' /v OffscreenRenderingMode /t REG_SZ /d fbo /f >/dev/null 2>&1 || true
        "$wine_bin" reg add 'HKCU\Software\Wine\Direct3D' /v StrictDrawOrdering /t REG_SZ /d disabled /f >/dev/null 2>&1 || true
        "$wine_bin" reg delete 'HKCU\Software\Wine\Direct3D' /v csmt /f >/dev/null 2>&1 || true
        "$wine_bin" reg delete 'HKCU\Software\Wine\Direct3D' /v VideoMemorySize /f >/dev/null 2>&1 || true
        "$wine_bin" reg delete 'HKCU\Software\Wine\Mac Driver' /v RetinaMode /f >/dev/null 2>&1 || true
    fi

    # Set DXLevel_V1 = 81 and disable HDR/MSAA once in registry (without passing
    # -dxlevel on CLI so saved video settings like windowed mode & resolution persist).
    # HDR (mat_hdr_level=2) + 4x MSAA (ScreenMSAA=4) hangs Wine OpenGL FBO on map load.
    if [[ ! -f "$PREFIX_DIR/user.reg" ]] || ! grep -q '"DXLevel_V1"=dword:00000051' "$PREFIX_DIR/user.reg" 2>/dev/null || grep -q '"mat_hdr_level"=dword:00000002' "$PREFIX_DIR/user.reg" 2>/dev/null || grep -q '"ScreenMSAA"=dword:00000004' "$PREFIX_DIR/user.reg" 2>/dev/null; then
        "$wine_bin" reg add 'HKCU\Software\Valve\Source\cstrike\Settings' /v DXLevel_V1 /t REG_DWORD /d 81 /f >/dev/null 2>&1 || true
        "$wine_bin" reg add 'HKCU\Software\Valve\Source\cstrike\Settings' /v mat_hdr_level /t REG_DWORD /d 0 /f >/dev/null 2>&1 || true
        "$wine_bin" reg add 'HKCU\Software\Valve\Source\cstrike\Settings' /v mat_antialias /t REG_DWORD /d 0 /f >/dev/null 2>&1 || true
        "$wine_bin" reg add 'HKCU\Software\Valve\Source\cstrike\Settings' /v ScreenMSAA /t REG_DWORD /d 0 /f >/dev/null 2>&1 || true
        "$wine_bin" reg add 'HKCU\Software\Valve\Source\cstrike\Settings' /v r_shadowrendertotexture /t REG_DWORD /d 0 /f >/dev/null 2>&1 || true
    fi

    local launch_log="$DATA_DIR/hl2_launch.log"
    : > "$launch_log"

    echo "Launching CS:S v34..."
    (
        trap '' HUP
        cd "$installed_dir"
        # Do NOT use /usr/bin/nohup (SIP strips DYLD_FALLBACK_LIBRARY_PATH, breaking FreeType).
        # Do NOT pass -dxlevel here: Source Engine resets all saved video settings on every launch.
        "$wine_bin" hl2.exe -game cstrike -insecure -console -condebug -novid -nojoy </dev/null >"$launch_log" 2>&1 &
    )

    # Wait briefly for hl2.exe to initialize, then inject islamware_v34.dll
    if [[ -f "$installed_dir/injector_v34.exe" && -f "$installed_dir/islamware_v34.dll" ]]; then
        echo "Waiting for hl2.exe to inject islamware_v34.dll..."
        sleep 4
        local win_dll
        if [[ "$installed_dir" == "$GAME_DIR" ]]; then
            win_dll="C:\\Program Files (x86)\\Counter-Strike Source\\islamware_v34.dll"
        elif [[ "$installed_dir" == "$PREFIX_DIR/drive_c/"* ]]; then
            local rel="${installed_dir#"$PREFIX_DIR/drive_c/"}"
            win_dll="C:\\${rel//\//\\}\\islamware_v34.dll"
        else
            win_dll="Z:${installed_dir//\//\\}\\islamware_v34.dll"
        fi
        "$wine_bin" "$installed_dir/injector_v34.exe" "$win_dll" </dev/null >>"$launch_log" 2>&1 || true
        echo "Injected! Press Insert in-game to open the menu."
    fi
}

cmd_uninstall() {
    local installed_dir=""
    installed_dir="$(find_installed_hl2 || true)"
    echo "Removing CS:S v34 installation at $PREFIX_DIR..."
    rm -rf "$PREFIX_DIR"
    if [[ -n "$installed_dir" && "$installed_dir" == "$LAUNCH_DIR/"* ]]; then
        echo "Removing CS:S v34 folder at $installed_dir..."
        rm -rf "$installed_dir"
    fi
    echo "Uninstalled."
}

case "${1:-help}" in
    status)     cmd_status ;;
    check-reqs) cmd_check_reqs ;;
    install)    shift; cmd_install "$@" ;;
    launch)    cmd_launch ;;
    uninstall) cmd_uninstall ;;
    drop-dir)  echo "$DROP_DIR" ;;
    *)
        echo "Usage: $0 {status|install [installer.exe]|launch|uninstall|drop-dir}"
        echo "Download CS:S v34 installer from: https://source-cs.ru"
        ;;
esac
