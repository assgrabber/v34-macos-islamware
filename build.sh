#!/bin/sh
set -e
cd "$(dirname "$0")"

TARGET="${1:-all}"

CXX64="${CXX64:-$(command -v x86_64-w64-mingw32-g++ || echo /opt/homebrew/bin/x86_64-w64-mingw32-g++)}"
CXX32="${CXX32:-$(command -v i686-w64-mingw32-g++ || echo /opt/homebrew/bin/i686-w64-mingw32-g++)}"

AK=third_party/alicekit
IMGUI=$AK/imgui

# Extract the packed font atlas headers on first build (kept zipped for repo size)
if [ ! -f "$AK/fonts/packed/font_atlas.hpp" ] || [ ! -f "$AK/fonts/packed/font_data.hpp" ]; then
    echo "[fonts] extracting $AK/fonts/packed/packed.zip"
    unzip -o -q "$AK/fonts/packed/packed.zip" -d "$AK/fonts/packed"
fi

BASEFLAGS="-std=c++20 -O2 -fno-exceptions -DNDEBUG -DWIN32_LEAN_AND_MEAN -DNOMINMAX"
INCLUDES="-Isrc -I$IMGUI -I$IMGUI/backends -I$AK"

AK_SRC="\
    $AK/alice_kit.cpp \
    $AK/ui/ui.cpp \
    $AK/renderer/renderer.cpp \
    $AK/window_animator/window_animator.cpp \
    $AK/util/util.cpp \
    $AK/fonts/font_loader.cpp \
    $AK/fonts/fonts.cpp \
    $AK/themes/themes.cpp \
    $AK/themes/theme_editor.cpp \
    $AK/ui/elem/elem.cpp \
    $AK/standalone_elem/standalone_elem.cpp \
    $AK/elem_animations/elem_animations.cpp \
    $AK/popups/popups.cpp \
    $AK/popups/popup_elem/popup_elem.cpp \
    $AK/custom_scrollbar/custom_scrollbar.cpp \
    $AK/config_manager/config_manager.cpp \
    $AK/notifications/notifications.cpp \
    $AK/tooltips/tooltips.cpp \
    $AK/esp_layout/esp_layout.cpp \
    $AK/windows/windows.cpp \
    $AK/smooth_scroll/smooth_scroll.cpp \
    $AK/assets/assets.cpp \
    $AK/backends/dx9/dx9_backend.cpp \
    $AK/backends/dx9/blur/blur.cpp \
    $AK/backends/dx9/grayscale/grayscale.cpp"

CHEAT_SRC="\
    src/main.cpp src/hooks.cpp src/features.cpp src/crash.cpp \
    src/alice_ui.cpp src/server_list.cpp src/connect_fix.cpp src/http_winhttp.cpp \
    src/impacts.cpp src/visual_fx.cpp src/game_events.cpp src/grenades.cpp \
    src/imgui_menu.cpp src/esp_layout.cpp src/glb_preview.cpp src/precache.cpp \
    build/glb_data.cpp \
    $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
    $IMGUI/backends/imgui_impl_win32.cpp $IMGUI/backends/imgui_impl_dx9.cpp"

LIBS="-ld3d9 -luser32 -lgdi32 -ldwmapi -limm32 -ldbghelp -lwininet -lwinhttp -lws2_32 -lpsapi -static -Wl,--enable-stdcall-fixup"

mkdir -p build

# Embed the GLB preview model as a byte array
GLB=assets/t-leet_face_rig.glb
if [ -f "$GLB" ] && { [ ! -f build/glb_data.cpp ] || [ "$GLB" -nt build/glb_data.cpp ]; }; then
    echo "[glb] embedding $GLB"
    python3 - "$GLB" > build/glb_data.cpp <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
print('extern const unsigned char g_glb_model_data[];')
print('extern const unsigned int g_glb_model_size;')
print('const unsigned char g_glb_model_data[] = {')
line = []
for i, b in enumerate(data):
    line.append('%d,' % b)
    if len(line) == 40:
        print(''.join(line)); line = []
if line: print(''.join(line))
print('};')
print('const unsigned int g_glb_model_size = %d;' % len(data))
PY
fi

build_arch() {
    local arch="$1"
    local cxx="$2"
    local dll_name="$3"
    local inj_name="$4"
    local objdir="build/obj/$arch"

    if [ ! -x "$cxx" ] && ! command -v "$cxx" >/dev/null 2>&1; then
        echo "error: $cxx not found (install mingw-w64)" >&2
        exit 1
    fi

    mkdir -p "$objdir"
    local objs=""
    for src in $AK_SRC; do
        local obj="$objdir/$(echo "$src" | sed 's#[^a-zA-Z0-9]#_#g').o"
        local rebuild=0
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            rebuild=1
        elif [ -n "$(find "$AK" -name '*.hpp' -newer "$obj" -print -quit 2>/dev/null)" ]; then
            rebuild=1
        fi
        if [ "$rebuild" = 1 ]; then
            "$cxx" $BASEFLAGS -c "$src" -o "$obj" $INCLUDES
        fi
        objs="$objs $obj"
    done

    echo "[$arch 1/2] $dll_name"
    "$cxx" $BASEFLAGS -shared -o "build/$dll_name" $CHEAT_SRC $objs $INCLUDES $LIBS

    echo "[$arch 2/2] $inj_name"
    "$cxx" -std=c++17 -O2 -o "build/$inj_name" injector/main.cpp -luser32 -static
}

case "$TARGET" in
    x64)
        build_arch "x64" "$CXX64" "islamware.dll" "injector.exe"
        ;;
    v34)
        build_arch "v34" "$CXX32" "islamware_v34.dll" "injector_v34.exe"
        ;;
    all)
        build_arch "x64" "$CXX64" "islamware.dll" "injector.exe"
        build_arch "v34" "$CXX32" "islamware_v34.dll" "injector_v34.exe"
        ;;
    *)
        echo "usage: $0 [all|x64|v34]" >&2
        exit 1
        ;;
esac

ls -la build
