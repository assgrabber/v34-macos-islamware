#include "cheat.h"
#include <psapi.h>

// ---------------------------------------------------------------------------
// Direct connect fix ("Connection failed after 4 retries").
//
// The engine in this build (9540945) prefers SteamNetworkingSockets for game
// connections and asks the Steam client for a "fake UDP port" on top of SNS.
// The emulator (gbe_fork) stubs those calls out — CreateFakeUDPPort returns
// NULL — so the engine's networking comes up half-initialised and every
// connect dies after four retries, even to a server that answers A2S fine.
//
// This hook answers the engine's SteamNetworkingSockets / SteamNetworkingUtils
// / SteamNetworkingMessages interface lookups with NULL.  The engine already
// handles "SNS not available" (that is the pre-update path it ships support
// for), and falls back to the legacy UDP connect that community servers still
// accept — the same path that worked before Valve added SNS support.
//
// Implemented as an IAT patch on every loaded module that imports the
// SteamInternal_* helpers from steam_api64.dll, including modules loaded
// after startup.
// ---------------------------------------------------------------------------

typedef void* (*SteamInternal_FindOrCreateUserInterface_t)(int hSteamPipe, const char* version);
typedef void* (*SteamInternal_CreateInterface_t)(const char* version);
typedef void* (*SteamInternal_FindOrCreateGameServerInterface_t)(int hSteamPipe, const char* version);

static SteamInternal_FindOrCreateUserInterface_t real_find_ui = nullptr;
static SteamInternal_CreateInterface_t real_create = nullptr;
static SteamInternal_FindOrCreateGameServerInterface_t real_find_gs = nullptr;

static volatile LONG fix_enabled = 1;
static volatile LONG patch_count = 0;

static bool IsSteamNetworking(const char* version) {
    if (!version) return false;
    return strstr(version, "SteamNetworkingSockets") != nullptr
        || strstr(version, "SteamNetworkingUtils") != nullptr
        || strstr(version, "SteamNetworkingMessages") != nullptr;
}

static void* HookFindOrCreateUserInterface(int pipe, const char* version) {
    if (fix_enabled && IsSteamNetworking(version)) {
        Log("[connectfix] blocked %s", version);
        return nullptr;
    }
    return real_find_ui ? real_find_ui(pipe, version) : nullptr;
}

static void* HookCreateInterface(const char* version) {
    if (fix_enabled && IsSteamNetworking(version)) {
        Log("[connectfix] blocked %s", version);
        return nullptr;
    }
    return real_create ? real_create(version) : nullptr;
}

static void* HookFindOrCreateGameServerInterface(int pipe, const char* version) {
    if (fix_enabled && IsSteamNetworking(version)) {
        Log("[connectfix] blocked %s", version);
        return nullptr;
    }
    return real_find_gs ? real_find_gs(pipe, version) : nullptr;
}

// ---------------------------------------------------------------------------
// The engine caches the SNS interface through tiny accessor functions that
// call SteamInternal_* once at startup.  IAT hooks alone lose that race, so
// patch the accessors themselves to always store NULL.
//
// Verified against build 9540945, engine.dll:
//   0x1d9de0  client  AccessSteamNetworkingSockets  (FindOrCreateUserInterface)
//   0x149580  server  AccessSteamNetworkingSockets  (FindOrCreateGameServerInterface)
//   0x094c80  utils   AccessSteamNetworkingUtils    (either context)
// Each starts with the same 8-byte prologue: push rbx; sub rsp,0x20; mov rbx,rcx
// ---------------------------------------------------------------------------
static const unsigned char kAccessorPrologue[9] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
static const unsigned char kAccessorStub[9]     = { 0x33, 0xC0, 0x48, 0x89, 0x01, 0xC3, 0x90, 0x90, 0x90 };
// 0x149580 (server context) is intentionally NOT patched: listen-server
// hosting dereferences it during map load and crashes on NULL.
static const uintptr_t kAccessorRvas[] = { 0x1d9de0, 0x94c80 };
static unsigned char g_accessor_orig[2][9];
static bool g_accessor_saved[2] = { false, false };

static int PatchEngineSnsAccessors(HMODULE eng, bool enable) {
    int patched = 0;
    for (int i = 0; i < 2; i++) {
        unsigned char* fn = reinterpret_cast<unsigned char*>(eng) + kAccessorRvas[i];
        if (enable) {
            if (memcmp(fn, kAccessorPrologue, sizeof(kAccessorPrologue)) != 0) {
                Log("[connectfix] accessor 0x%llx: unexpected prologue, skipped", (unsigned long long)kAccessorRvas[i]);
                continue;
            }
            if (!g_accessor_saved[i]) {
                memcpy(g_accessor_orig[i], fn, sizeof(kAccessorPrologue));
                g_accessor_saved[i] = true;
            }
        } else if (!g_accessor_saved[i]) {
            continue;
        }
        const unsigned char* bytes = enable ? kAccessorStub : g_accessor_orig[i];
        DWORD old_protect = 0;
        if (VirtualProtect(fn, sizeof(kAccessorStub), PAGE_EXECUTE_READWRITE, &old_protect)) {
            memcpy(fn, bytes, sizeof(kAccessorStub));
            VirtualProtect(fn, sizeof(kAccessorStub), old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), fn, sizeof(kAccessorStub));
            patched++;
        }
    }
    return patched;
}

// Rewrite (or restore) the import slots of one module.
static int PatchModule(HMODULE mod, bool enable) {
    if (!mod) return 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<char*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    int patched = 0;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(reinterpret_cast<char*>(mod) + dir.VirtualAddress);
    for (; desc->Name; desc++) {
        const char* dll = reinterpret_cast<const char*>(mod) + desc->Name;
        if (_stricmp(dll, "steam_api64.dll") != 0) continue;

        auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<char*>(mod) + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<char*>(mod) + desc->FirstThunk);

        for (; orig->u1.AddressOfData; orig++, iat++) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                reinterpret_cast<char*>(mod) + orig->u1.AddressOfData);
            const char* fn = reinterpret_cast<const char*>(ibn->Name);

            void* want = nullptr;
            if (!strcmp(fn, "SteamInternal_FindOrCreateUserInterface"))
                want = enable ? reinterpret_cast<void*>(&HookFindOrCreateUserInterface)
                              : reinterpret_cast<void*>(real_find_ui);
            else if (!strcmp(fn, "SteamInternal_CreateInterface"))
                want = enable ? reinterpret_cast<void*>(&HookCreateInterface)
                              : reinterpret_cast<void*>(real_create);
            // Game-server context is left untouched: listen-server init
            // dereferences the returned interface and crashes on NULL.
            else if (!strcmp(fn, "SteamInternal_FindOrCreateGameServerInterface"))
                continue;
            if (!want) continue;
            if (reinterpret_cast<void*>(iat->u1.Function) == want) continue;

            DWORD old_protect = 0;
            if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old_protect)) {
                iat->u1.Function = reinterpret_cast<ULONG_PTR>(want);
                VirtualProtect(&iat->u1.Function, sizeof(void*), old_protect, &old_protect);
                patched++;
            }
        }
    }
    return patched;
}

static int PatchAllModules(bool enable) {
    HMODULE mods[512];
    DWORD needed = 0;
    int patched = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; i++)
            patched += PatchModule(mods[i], enable);
    }
    return patched;
}

void SetConnectFix(bool enable) {
    InterlockedExchange(&fix_enabled, enable ? 1 : 0);
    HMODULE eng = GetModuleHandleA("engine.dll");
    if (eng) PatchEngineSnsAccessors(eng, enable);
    if (!real_find_ui) return;   // loader thread has not resolved the exports yet
    int n = PatchAllModules(enable);
    Log("[connectfix] %s, %d import slots rewritten", enable ? "enabled" : "disabled", n);
}

static DWORD WINAPI ConnectFixThread(LPVOID) {
    // The engine is a static import: it is mapped before the game's entry
    // point runs.  Patch its SNS accessors immediately — this is the call the
    // engine makes when it caches the interface at host init.
    HMODULE eng = nullptr;
    while (!(eng = GetModuleHandleA("engine.dll"))) Sleep(1);
    if (fix_enabled) {
        int n = PatchEngineSnsAccessors(eng, true);
        Log("[connectfix] engine SNS accessors patched: %d/2", n);
    }

    // steam_api64.dll is a static import of the engine: it appears very early.
    HMODULE api = nullptr;
    while (!(api = GetModuleHandleA("steam_api64.dll"))) Sleep(1);

    real_find_ui = reinterpret_cast<SteamInternal_FindOrCreateUserInterface_t>(
        GetProcAddress(api, "SteamInternal_FindOrCreateUserInterface"));
    real_create = reinterpret_cast<SteamInternal_CreateInterface_t>(
        GetProcAddress(api, "SteamInternal_CreateInterface"));
    real_find_gs = reinterpret_cast<SteamInternal_FindOrCreateGameServerInterface_t>(
        GetProcAddress(api, "SteamInternal_FindOrCreateGameServerInterface"));
    if (!real_find_ui) {
        Log("[connectfix] steam_api64 exports not found");
        return 0;
    }

    // Modules load over the first seconds; keep patching until the game is up.
    // Patching is idempotent and cheap, so a short polling window is enough.
    LONG total = 0;
    for (int i = 0; i < 600; i++) {          // ~30 s at 50 ms
        if (fix_enabled)
            total += PatchAllModules(true);
        Sleep(50);
    }
    patch_count = total;
    Log("[connectfix] active (SteamNetworking disabled), %d import slots", (int)total);
    return 0;
}

// ---------------------------------------------------------------------------
// Version gate.
//
// Current community servers (build 11003710, Aug 2026 update) reject clients
// older than their own build: the connect handshake ends with
// S2C_CONNREJECT "#GameUI_ServerRejectOldVersion".  The engine reports the
// build number from cstrike/steam.inf, so rewriting that file to the current
// build passes the gate.  The original is kept next to it and restored when
// the switch is turned off.  The engine reads steam.inf at startup, so a
// change takes effect on the next launch.
// ---------------------------------------------------------------------------
void ApplyVersionSpoof() {
    if (!cfg.spoof_version || !cfg.reported_build[0]) return;

    char exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return;
    char* slash = strrchr(exe, '\\');
    if (!slash) return;
    *slash = '\0';

    char path[MAX_PATH], bak_path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cstrike\\steam.inf", exe);
    snprintf(bak_path, sizeof(bak_path), "%s\\cstrike\\steam.inf.islamware.bak", exe);

    // back up the original once
    if (GetFileAttributesA(bak_path) == INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileA(path, bak_path, TRUE))
            Log("[versionspoof] cannot back up %s", path);
    }

    FILE* f = fopen(path, "rb");
    if (!f) { Log("[versionspoof] cannot open %s", path); return; }
    char data[2048] = { 0 };
    size_t n = fread(data, 1, sizeof(data) - 1, f);
    fclose(f);
    data[n] = '\0';

    char out[2048];
    size_t o = 0;
    const char* line = data;
    while (*line && o < sizeof(out) - 64) {
        const char* eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (!strncmp(line, "PatchVersion=", 13) || !strncmp(line, "ClientVersion=", 14)) {
            const char* eq = strchr(line, '=');
            int written = snprintf(out + o, sizeof(out) - o, "%s=%s%s",
                strncmp(line, "PatchVersion=", 13) ? "ClientVersion" : "PatchVersion",
                cfg.reported_build, eol ? "\n" : "");
            o += (size_t)written;
        } else {
            memcpy(out + o, line, len);
            o += len;
            if (eol) out[o++] = '\n';
        }
        if (!eol) break;
        line = eol + 1;
    }
    out[o] = '\0';

    if (o != n || memcmp(out, data, o) != 0) {
        f = fopen(path, "wb");
        if (!f) { Log("[versionspoof] cannot write %s", path); return; }
        fwrite(out, 1, o, f);
        fclose(f);
        Log("[versionspoof] reported build set to %s", cfg.reported_build);
    }
}

static void RestoreVersionSpoof_() {
    char exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return;
    char* slash = strrchr(exe, '\\');
    if (!slash) return;
    *slash = '\0';
    char path[MAX_PATH], bak_path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cstrike\\steam.inf", exe);
    snprintf(bak_path, sizeof(bak_path), "%s\\cstrike\\steam.inf.islamware.bak", exe);
    if (GetFileAttributesA(bak_path) != INVALID_FILE_ATTRIBUTES) {
        CopyFileA(bak_path, path, FALSE);
        DeleteFileA(bak_path);
        Log("[versionspoof] original steam.inf restored");
    }
}

// ---------------------------------------------------------------------------
// Emulator network gate.
//
// gbe_fork ships "LAN only" behaviour by default: unless
// [main::connectivity] disable_lan_only=1 is set in steam_settings/configs.main.ini
// it detours connect/WSAConnect/sendto/WinHttpConnect and silently drops every
// non-LAN destination — the game can reach the router but not the internet.
// This writes the key at startup so the emulator stops doing that.
// ---------------------------------------------------------------------------
static void WriteLanOnlyKey(bool allow_internet) {
    char exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return;
    char* slash = strrchr(exe, '\\');
    if (!slash) return;
    *slash = '\0';
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\steam_settings\\configs.main.ini", exe);

    const char* want = allow_internet ? "disable_lan_only=1" : "disable_lan_only=0";

    FILE* f = fopen(path, "rb");
    char data[8192] = { 0 };
    size_t n = 0;
    if (f) { n = fread(data, 1, sizeof(data) - 1, f); fclose(f); }
    data[n] = '\0';

    char out[8192];
    size_t o = 0;
    bool found = false;
    const char* line = data;
    while (*line && o < sizeof(out) - 64) {
        const char* eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (!strncmp(line, "disable_lan_only", 16)) {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%s%s", want, eol ? "\n" : "");
            found = true;
        } else {
            memcpy(out + o, line, len);
            o += len;
            if (eol) out[o++] = '\n';
        }
        if (!eol) break;
        line = eol + 1;
    }
    if (!found) {
        o += (size_t)snprintf(out + o, sizeof(out) - o,
            "\n[main::connectivity]\n%s\n", want);
    }
    out[o] = '\0';

    if (o == n && memcmp(out, data, o) == 0) return;
    f = fopen(path, "wb");
    if (!f) { Log("[netfix] cannot write %s", path); return; }
    fwrite(out, 1, o, f);
    fclose(f);
    Log("[netfix] emulator LAN-only %s", allow_internet ? "disabled" : "enabled");
}

void ApplyEmuNetworkFix(bool enable) {
    if (!cfg.allow_internet) { WriteLanOnlyKey(false); return; }
    WriteLanOnlyKey(true);
}

void SetVersionSpoof(bool enable) {
    if (enable) ApplyVersionSpoof();
    else RestoreVersionSpoof_();
}

void StartConnectFix() {
    if (!cfg.connect_fix) {
        InterlockedExchange(&fix_enabled, 0);
        Log("[connectfix] disabled in settings");
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, ConnectFixThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

void ConnectFixStatus(char* buf, int buf_size) {
    snprintf(buf, buf_size, fix_enabled ? "SNS disabled — direct UDP connect active"
                                        : "SNS enabled (game update mode)");
}