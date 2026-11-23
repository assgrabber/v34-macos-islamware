#include "cheat.h"
#include "hooks.h"
#include <cstdio>

extern void InstallCrashHandler();
extern void SetStage(const char* stage);

Config cfg;
IVEngineClient engine;
IVRenderView render_view;
IClientEntityList entity_list;
IVDebugOverlay debug_overlay;
ICvar cvar;
IEngineTrace engine_trace;
IPhysicsSurfaceProps physics_props;
void* base_client = nullptr;
IMaterialSystem material_system;
IModelRender model_render;
IVModelInfo model_info;
IMDLCache mdl_cache;
void* studio_render = nullptr;
HMODULE client_module = nullptr;
HMODULE engine_module = nullptr;
HMODULE vstdlib_module = nullptr;
HWND game_window = nullptr;

static HMODULE g_self = nullptr;
char g_log_path[MAX_PATH] = "C:\\islamware_css.log";

extern const char* g_crash_stage;

volatile LONG g_wd_frame_tick = 0;
volatile LONG g_wd_cmd_tick = 0;

// Watches the counters the game thread bumps every frame.  A freeze leaves a
// silent log that only shows where the last frame got to; this prints the
// stall and the stage it happened in, so the next report is diagnosable.
static DWORD WINAPI WatchdogThread(LPVOID) {
    LONG last_frame = 0;
    DWORD last_change = GetTickCount();
    bool stalled = false;
    Log("[watchdog] armed");
    while (true) {
        Sleep(1000);
        DWORD const now = GetTickCount();
        LONG const frame = g_wd_frame_tick;
        if (frame != last_frame) {
            last_frame = frame;
            last_change = now;
        }
        if (!frame) continue;

        DWORD const age = now - last_change;
        if (!stalled && age > 4000) {
            stalled = true;
            Log("[watchdog] render stalled %lums stage=%s cmd_age=%lums "
                "menu=%d esp=%d walls=%d props=%d impacts=%d tracers=%d killfx=%d",
                (unsigned long)age, g_crash_stage,
                (unsigned long)(now - (DWORD)g_wd_cmd_tick),
                (int)cfg.menu_open, (int)cfg.esp, (int)cfg.world_walls,
                (int)cfg.world_props, (int)cfg.bullet_impacts,
                (int)cfg.tracers, (int)cfg.kill_effects);
        } else if (stalled && age < 2000) {
            stalled = false;
            Log("[watchdog] render recovered after %lums", (unsigned long)age);
        }
    }
    return 0;
}

void Log(const char* fmt, ...) {
    FILE* f = fopen(g_log_path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

bool IsGameReady() {
    return engine.self && entity_list.self && client_module;
}

static bool InitInterfaces() {
    engine_module = GetModuleHandleA("engine.dll");
    client_module = GetModuleHandleA("client.dll");
    vstdlib_module = GetModuleHandleA("vstdlib.dll");
    if (!engine_module || !client_module) return false;

    engine.self = GetInterface(engine_module, "VEngineClient014");
    render_view.self = GetInterface(engine_module, "VEngineRenderView014");
    base_client = GetInterface(client_module, "VClient017");
    entity_list.self = GetInterface(client_module, "VClientEntityList003");
    debug_overlay.self = GetInterface(engine_module, "VDebugOverlay003");
    engine_trace.self = GetInterface(engine_module, "EngineTraceClient003");
    cvar.self = GetInterface(vstdlib_module ? vstdlib_module : engine_module, "VEngineCvar004");
    model_render.self = GetInterface(engine_module, "VEngineModel016");
    model_info.self = GetInterface(engine_module, "VModelInfoClient006");
    material_system.self = GetInterface(GetModuleHandleA("materialsystem.dll"), "VMaterialSystem082");
    physics_props.self = GetInterface(GetModuleHandleA("vphysics.dll"), "VPhysicsSurfaceProps001");
    mdl_cache.self = GetInterface(GetModuleHandleA("datacache.dll"), "MDLCache004");
    studio_render = GetInterface(GetModuleHandleA("studiorender.dll"), "VStudioRender025");

    ConVar* test_cvar = cvar.self ? cvar.FindVar("mat_fullbright") : nullptr;
    Log("bases: client=%p engine=%p", client_module, engine_module);
    Log("interfaces: engine=%p baseclient=%p entitylist=%p overlay=%p trace=%p cvar=%p modelrender=%p mats=%p mdlcache=%p studiorender=%p (cvar test %p)",
        engine.self, base_client, entity_list.self, debug_overlay.self, engine_trace.self, cvar.self, model_render.self, material_system.self, mdl_cache.self, studio_render, test_cvar);
    return engine.self && entity_list.self;
}

static HWND FindGameWindow() {
    HWND h = FindWindowA("Valve001", nullptr);
    if (h) return h;
    // fall back: first visible top-level window owned by this process
    struct Ctx { DWORD pid; HWND found; } ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == c->pid && IsWindowVisible(w)) {
            char cls[64] = { 0 };
            GetClassNameA(w, cls, sizeof(cls));
            if (strstr(cls, "Valve") || strstr(cls, "SDL")) {
                c->found = w;
                return FALSE;
            }
        }
        return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// Type a line into the game console the same way a player would: open with
// `~`, then WM_CHAR the text, then Enter.  Used for the cmd channel's "cc:"
// prefix — "connect" refuses to run from ClientCmd/ExecuteClientCmd
// (FCVAR_CLIENTCMD_CAN_EXECUTE), only from real console input.
static void TypeConsoleText(const char* text) {
    HWND h = game_window;
    if (!h) return;
    auto post_key = [&](WPARAM vk, bool down) {
        LPARAM lp = 1 | ((LONG)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16);
        if (!down) lp |= (LPARAM)0xC0000000;
        PostMessageA(h, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    };
    post_key(VK_OEM_3, true); Sleep(40); post_key(VK_OEM_3, false); Sleep(250);
    for (const char* p = text; *p; ++p) {
        PostMessageA(h, WM_CHAR, (WPARAM)(unsigned char)*p, 1);
        Sleep(8);
    }
    post_key(VK_RETURN, true);
    PostMessageA(h, WM_CHAR, '\r', 1);
    Sleep(10);
    post_key(VK_RETURN, false);
    Sleep(60);
    post_key(VK_OEM_3, true); Sleep(40); post_key(VK_OEM_3, false);   // close console
}

static DWORD WINAPI InitThread(LPVOID) {
    SetStage("init");
    Log("=== islamware-css loaded (build 9540945) ===");
    // emulator + steam.inf must be corrected before the engine/emu read them
    ApplyEmuNetworkFix(cfg.allow_internet);
    ApplyVersionSpoof();
    // Must start before the engine initialises SteamNetworkingSockets: the
    // connect fix rewrites the imports the engine is about to call.
    // A local listen server (-lan-test) needs the untouched SNS path, since
    // the engine calls it while booting the server and the stub crashes.
    if (strstr(GetCommandLineA(), "-lan-test"))
        Log("[connectfix] skipped (-lan-test, local server)");
    else
        StartConnectFix();
    while (!InitInterfaces()) Sleep(200);

    for (int i = 0; i < 100 && !game_window; i++) {
        game_window = FindGameWindow();
        if (!game_window) Sleep(200);
    }
    SetStage("window found");
    Log("window: %p", game_window);

    // The loader injects at process start; let the engine finish bringing up the
    // window/renderer before touching vtables or creating the dummy device.
    Sleep(5000);

    SetStage("installing hooks");
    if (game_window) InstallWindowHook(game_window);
    InstallInputHooks();
    InstallFakelagHook();
    InstallClientModeHooks();
    InstallD3D9Hook();
    SetStage("running");
    CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);

    // server browser fix: refresh the community list in the background
    if (cfg.browser_fix) StartServerListRefresh("startup");

    while (true) {
        FILE* f = fopen("C:\\islamware_cmd.txt", "r");
        if (f) {
            char line[256] = { 0 };
            if (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
                if (!strcmp(line, "net-test")) { Log("net-test"); RunNetTest(); }
                else if (!strcmp(line, "sl-refresh")) { Log("sl-refresh"); StartServerListRefresh("debug"); }
                else if (!strcmp(line, "impacts-probe")) { Log("impacts-probe"); Impacts_Probe(); }
                else if (!strcmp(line, "fx-on")) { Log("fx-on"); cfg.bullet_impacts = true; cfg.impacts_server = true; cfg.tracers = true; }
                else if (!strcmp(line, "fx-off")) { Log("fx-off"); cfg.bullet_impacts = false; cfg.impacts_server = false; cfg.tracers = false; }
                else if (!strcmp(line, "combat-on")) { Log("combat-on"); cfg.silentaim = true; cfg.aa_enabled = true; cfg.hit_chance = 100.0f; }
                else if (!strcmp(line, "combat-off")) { Log("combat-off"); cfg.silentaim = false; cfg.aa_enabled = false; }
                else if (!strcmp(line, "hp-dump")) {
                    Log("hp-dump");
                    for (int i = 1; i <= 32; i++) {
                        void* e = entity_list.GetClientEntity(i);
                        if (!e) continue;
                        int hp = *reinterpret_cast<int*>((char*)e + off::m_iHealth);
                        int team = *reinterpret_cast<int*>((char*)e + off::m_iTeamNum);
                        Vector o = *reinterpret_cast<Vector*>((char*)e + off::m_vecOrigin);
                        Log("[hp] %d life=%d hp=%d team=%d org=%.0f/%.0f/%.0f", i,
                            (int)*reinterpret_cast<char*>((char*)e + off::m_lifeState),
                            hp, team, o.x, o.y, o.z);
                    }
                }
                else if (n > 3 && !strncmp(line, "cc:", 3)) { Log("console: %s", line + 3); TypeConsoleText(line + 3); }
                else if (n && engine.self) { Log("cmd: %s", line); engine.ExecuteClientCmd(line); }
            }
            fclose(f);
            DeleteFileA("C:\\islamware_cmd.txt");
        }
        Sleep(400);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        GetModuleFileNameA(module, g_log_path, MAX_PATH);
        char* slash = strrchr(g_log_path, '\\');
        if (slash) snprintf(slash + 1, MAX_PATH - (int)(slash + 1 - g_log_path), "islamware_css.log");
        InstallCrashHandler();
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
