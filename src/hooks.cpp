#include "hooks.h"
#include "cheat.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "alice_ui.h"

#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

extern void SetStage(const char* stage);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

Vector view_origin;
QAngle view_angles;
float view_fov = 90.0f;
float view_aspect = 1.7777778f;
bool view_valid = false;
QAngle real_view_angles;
QAngle user_camera_angles;
QAngle aa_view_angles;

// ---------------------------------------------------------------------------
// vtable patching
// ---------------------------------------------------------------------------
static bool PatchVtable(void** vt, int index, void* fn, void** original) {
    DWORD old_protect = 0;
    void** slot = &vt[index];
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect))
        return false;
    if (original) *original = *slot;
    *slot = fn;
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &tmp);
    return true;
}

static void* GetInput() {
    if (!client_module) return nullptr;
#ifdef _WIN64
    // 0x5C0090 holds the CInput* (the object itself lives at client.dll+0x691CB0)
    return *reinterpret_cast<void**>((char*)client_module + CLIENT_INPUT_GLOBAL_RVA);
#else
    if (!base_client) return nullptr;
    static void* s_input = nullptr;
    if (!s_input)
        s_input = **reinterpret_cast<void***>((*reinterpret_cast<uintptr_t**>(base_client))[11] + 0x2);
    return s_input;
#endif
}

static bool KeyActive(int vk) {
    return vk == 0 || (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Keybinds support hold and toggle modes.  Each feature gets a slot so the
// toggle state is independent.
enum BindSlot {
    BIND_SILENTAIM = 0, BIND_AIMBOT, BIND_TRIGGERBOT, BIND_BHOP, BIND_AUTOSTRAFE, BIND_HITCHAMS,
    BIND_DUCKPEEK, BIND_SILENTRUN, BIND_JUMPBUG, BIND_LADDER, BIND_QCANCEL, BIND_BAIM, BIND_PEEK_ASSIST, BIND_SLOTS
};
static bool g_bind_toggle[BIND_SLOTS] = {};
static bool g_bind_prev[BIND_SLOTS] = {};

// comma-separated name list; matching is case-insensitive on plain substrings
bool IsWhitelisted(int index) {
    if (!cfg.whitelist[0] || index <= 0) return false;
    player_info_t info = {};
    if (!engine.GetPlayerInfo(index, &info) || !info.name[0]) return false;
    char lower[33];
    size_t n = strlen(info.name);
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++)
        lower[i] = (info.name[i] >= 'A' && info.name[i] <= 'Z') ? (char)(info.name[i] + 32) : info.name[i];
    lower[n] = 0;
    const char* p = cfg.whitelist;
    static char token[33];
    while (*p) {
        size_t t = 0;
        while (*p && *p != ',') {
            if (t < 32) {
                char c = *p;
                token[t++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            p++;
        }
        token[t] = 0;
        while (*p == ',') p++;
        if (t && strstr(lower, token))
            return true;
    }
    return false;
}

bool* FeatureBindTarget(int id) {
    switch (id) {
    case FB_INF_DUCK: return &cfg.inf_duck;
    case FB_CROUCH_AIR: return &cfg.crouch_air;
    case FB_QUICK_STOP: return &cfg.quick_stop;
    case FB_THIRDPERSON: return &cfg.thirdperson;
    case FB_LEFT_HAND: return &cfg.left_hand;
    case FB_REMOVE_BOB: return &cfg.remove_bob;
    case FB_FULLBRIGHT: return &cfg.world_fullbright;
    case FB_NOFOG: return &cfg.world_nofog;
    case FB_NOSKY: return &cfg.world_nosky;
    case FB_CUSTOMFOG: return &cfg.world_custom_fog;
    case FB_REM_POST: return &cfg.rem_postprocess;
    case FB_REM_SHADOWS: return &cfg.rem_shadows;
    case FB_REM_DECALS: return &cfg.rem_decals;
    case FB_REM_GRASS: return &cfg.rem_grass;
    case FB_REM_ROPES: return &cfg.rem_ropes;
    case FB_REM_WATER: return &cfg.rem_water;
    case FB_REM_DYN: return &cfg.rem_dynlights;
    case FB_REM_SHELLS: return &cfg.rem_shells;
    case FB_REM_DEBRIS: return &cfg.rem_debris;
    case FB_REM_PARTICLES: return &cfg.rem_particles;
    case FB_ESP: return &cfg.esp;
    case FB_ESP_BOX: return &cfg.esp_box;
    case FB_ESP_NAME: return &cfg.esp_name;
    case FB_ESP_HEALTH: return &cfg.esp_health;
    case FB_ESP_ARMOR: return &cfg.esp_armor;
    case FB_ESP_WEAPON: return &cfg.esp_weapon;
    case FB_ESP_AMMO: return &cfg.esp_ammo;
    case FB_ESP_FLAGS: return &cfg.esp_flags;
    case FB_ESP_SKELETON: return &cfg.esp_skeleton;
    case FB_ESP_OFFSCREEN: return &cfg.esp_offscreen;
    case FB_ESP_AIMBOT: return &cfg.esp_aimbot_visual;
    case FB_ESP_BOMB: return &cfg.esp_bomb;
    case FB_CHAMS_HIT: return &cfg.chams_hit;
    case FB_CHAMS_DISABLE_OCC: return &cfg.chams_disable_occlusion;
    case FB_FAKE_DUCK: return &cfg.fake_duck;
    default: return nullptr;
    }
}

void ApplyFeatureBinds() {
    static bool prev[FB_COUNT] = {};
    static bool tog[FB_COUNT] = {};
    for (int i = 0; i < FB_COUNT; i++) {
        int const vk = cfg.kb_feature[i];
        bool* const target = FeatureBindTarget(i);
        if (!target) continue;
        if (vk == 0) { prev[i] = false; continue; }
        bool const down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (cfg.kb_feature_mode[i] == 1) {
            if (down && !prev[i]) tog[i] = !tog[i];
            prev[i] = down;
            *target = tog[i];
        } else {
            prev[i] = down;
            *target = down;
        }
    }
}

// active-state query for the hotkey window (holds: key down; toggles: the
// shared toggle table when the vk belongs to a known slot)
// ---------------------------------------------------------------------------
// fakelag: hold back outgoing movement datagrams so the server sees choked
// ticks (the player freezes server-side while moving locally).  Implemented
// at the sendto level: the client emits one movement datagram per tick, so
// dropping N out of every N+2 datagrams reproduces the classic choke pattern.
typedef int (WINAPI* SendToFn)(unsigned long long, char const*, int, int, void const*, int);
static SendToFn oSendTo = nullptr;
static int g_fakelag_tick = 0;

static int WINAPI HookedSendTo(unsigned long long s, char const* buf, int len, int flags, void const* to, int tolen) {
    // only choke small datagrams: reliable messages ride the same UDP
    // payload in bigger packets and losing them stalls the connection
    if (cfg.fakelag && cfg.fakelag_ticks > 0 && IsGameReady() && len > 0 && len < 800) {
        int const period = cfg.fakelag_ticks + 2;
        int const phase = g_fakelag_tick % period;
        g_fakelag_tick++;
        if (phase < cfg.fakelag_ticks)
            return len; // choke: pretend the datagram went out
    }
    return oSendTo(s, buf, len, flags, to, tolen);
}

bool BindIsEngaged(int vk, int mode) {
    if (vk == 0) return false;
    bool const down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (mode != 1) return down;
    int const known[] = { cfg.kb_silentaim, cfg.kb_aimbot, cfg.kb_triggerbot,
                          cfg.kb_bhop, cfg.kb_autostrafe, cfg.kb_hitchams,
                          cfg.kb_duckpeek, cfg.kb_silentrun, cfg.kb_jumpbug,
                          cfg.kb_fastladder, cfg.kb_qcancel, cfg.kb_baim, cfg.kb_peek_assist };
    for (int slot = 0; slot < BIND_SLOTS && slot < 13; slot++)
        if (known[slot] == vk)
            return g_bind_toggle[slot];
    // feature binds: check the feature-bind toggle state by vk
    for (int i = 0; i < FB_COUNT; i++)
        if (cfg.kb_feature[i] == vk && cfg.kb_feature_mode[i] == 1)
            return *FeatureBindTarget(i);
    return down;
}

static bool BindActive(int vk, int mode, int slot) {
    if (vk == 0) return true;
    if (slot < 0 || slot >= BIND_SLOTS) return KeyActive(vk);

    bool const down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (mode == 1) {
        if (down && !g_bind_prev[slot])
            g_bind_toggle[slot] = !g_bind_toggle[slot];
        g_bind_prev[slot] = down;
        return g_bind_toggle[slot];
    }
    g_bind_prev[slot] = down;
    return down;
}

static float NormalizeYaw(float y) {
    if (!(y > -1.0e9f && y < 1.0e9f)) return 0.0f;
    y = fmodf(y + 180.0f, 360.0f);
    if (y < 0.0f) y += 360.0f;
    return y - 180.0f;
}

static char g_miss_reason[80] = { 0 };
static ULONGLONG g_miss_time = 0;

// custom miss feed: coloured entries stacked at the bottom left,
// fading out (kEventLog look)
struct MissFeedEntry { char prefix[32]; char rest[160]; ULONGLONG t; };
static MissFeedEntry g_miss_feed[5] = {};
static int g_miss_feed_count = 0;

void MissLog_Push(char const* prefix, char const* rest) {
    for (int i = 4; i > 0; i--)
        g_miss_feed[i] = g_miss_feed[i - 1];
    strncpy(g_miss_feed[0].prefix, prefix, sizeof(g_miss_feed[0].prefix) - 1);
    strncpy(g_miss_feed[0].rest, rest, sizeof(g_miss_feed[0].rest) - 1);
    g_miss_feed[0].t = GetTickCount64();
    if (g_miss_feed_count < 5)
        g_miss_feed_count++;
}

// shot tracking for miss detection (silent aim target at fire time)
static char g_miss_target[32] = { 0 };
static int g_miss_target_idx = 0;
static char g_miss_hitbox[16] = { 0 };
static int g_miss_hc = 100;
static Vector g_miss_aimpoint;
static bool g_miss_pending = false;
static ULONGLONG g_miss_pending_time = 0;

void MissLog_ArmShot(char const* name, char const* hitbox, int hc, Vector const& aimpoint) {
    strncpy(g_miss_target, name ? name : "?", sizeof(g_miss_target) - 1);
    strncpy(g_miss_hitbox, hitbox ? hitbox : "?", sizeof(g_miss_hitbox) - 1);
    g_miss_hc = hc;
    g_miss_aimpoint = aimpoint;
    g_miss_pending = true;
    g_miss_pending_time = GetTickCount64();
}

void MissLog_OnLocalImpact(Vector const& pos) {
    if (!g_miss_pending) return;
    g_miss_pending = false;
    if (GetTickCount64() - g_miss_pending_time > 1000)
        return; // impact from an older shot
    float const dx = pos.x - g_miss_aimpoint.x;
    float const dy = pos.y - g_miss_aimpoint.y;
    float const dz = pos.z - g_miss_aimpoint.z;
    float const off = sqrtf(dx * dx + dy * dy + dz * dz);
    if (off <= 20.0f)
        Resolver_OnHit(g_miss_target_idx);
    else
        Resolver_OnMiss(g_miss_target_idx);
    // the server impact is far off the aimed hitbox - the shot missed
    if (off > 20.0f) {
        char rest[160];
        snprintf(rest, sizeof(rest), "spread at %s (%s, hc %d%%)",
                 g_miss_target, g_miss_hitbox, g_miss_hc);
        MissLog_Push("Missed shot due to ", rest);
        if (cfg.log_misses)
            Log("[miss] %s%s (%.1f units off)", "Missed shot due to ", rest, off);
    }
}

// knifebot swing: the angle is also applied to the outgoing command in
// BaseCreateMove, otherwise the ClientMode edit can be overwritten
static QAngle g_knife_angle;
static DWORD g_knife_time = 0;
static bool g_knife_swing = false;

static int CurrentWeaponGroup( ) {
    void* local = GetLocalEntity( );
    if ( !local ) return 0;
    int const handle = *reinterpret_cast<int*>( (char*)local + off::m_hActiveWeapon );
    int const idx = handle & 0xFFF;
    if ( idx <= 0 ) return 0;
    static int cached_handle = -1;
    static int cached_group = 0;
    if ( handle == cached_handle ) return cached_group;
    cached_handle = handle;
    void* w = entity_list.GetClientEntity( idx );
    void* nw = w ? entity_list.GetClientNetworkable( idx ) : nullptr;
    void* cc = nw ? VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw) : nullptr;
    char const* cls = ClientClassName(cc);
    int g = 0;
    if ( cls ) {
        if ( strstr( cls, "Knife" ) ) g = 6;
        else if ( strstr( cls, "AWP" ) || strstr( cls, "Scout" ) || strstr( cls, "SG550" ) || strstr( cls, "G3SG1" ) ) g = 1;
        else if ( strstr( cls, "Glock" ) || strstr( cls, "USP" ) || strstr( cls, "Deagle" ) || strstr( cls, "P228" ) ||
                  strstr( cls, "FiveSeven" ) || strstr( cls, "Elite" ) ) g = 2;
        else if ( strstr( cls, "MAC10" ) || strstr( cls, "MP5" ) || strstr( cls, "TMP" ) || strstr( cls, "UMP45" ) || strstr( cls, "P90" ) ) g = 3;
        else if ( strstr( cls, "M3" ) || strstr( cls, "XM1014" ) ) g = 4;
        else if ( strstr( cls, "M249" ) || strstr( cls, "Negev" ) ) g = 5;
        else g = 0;
    }
    cached_group = g;
    return g;
}

struct ActiveAimSettings {
    float silent_fov;
    bool hb[5];
    float trigger_fov;
    float min_damage;
    float hit_chance;
    int delay_shot;
    bool auto_stop;
    bool auto_scope;
};

static ActiveAimSettings GetAimSettings( ) {
    int const g = CurrentWeaponGroup( );
    Config::WeaponOverride const* ov = ( g >= 0 && g < 7 && cfg.weapon_override[g].enabled )
        ? &cfg.weapon_override[g] : nullptr;
    ActiveAimSettings s { };
    if ( ov ) {
        s.silent_fov = ov->silent_fov;
        for ( int i = 0; i < 5; i++ ) s.hb[i] = ov->hb[i];
        s.trigger_fov = ov->trigger_fov;
        s.min_damage = ov->min_damage;
        s.hit_chance = ov->hit_chance;
        s.delay_shot = ov->delay_shot;
        s.auto_stop = ov->auto_stop;
        s.auto_scope = ov->auto_scope;
    } else {
        s.silent_fov = cfg.silent_fov;
        s.hb[0] = cfg.silent_hb_head; s.hb[1] = cfg.silent_hb_neck; s.hb[2] = cfg.silent_hb_chest;
        s.hb[3] = cfg.silent_hb_stomach; s.hb[4] = cfg.silent_hb_pelvis;
        s.trigger_fov = cfg.trigger_fov;
        s.min_damage = cfg.min_damage;
        s.hit_chance = cfg.hit_chance;
        s.delay_shot = cfg.delay_shot;
        s.auto_stop = cfg.auto_stop;
        s.auto_scope = cfg.auto_scope;
    }
    return s;
}

static QAngle CalcAngle(const Vector& src, const Vector& dst) {
    Vector d = dst - src;
    QAngle a;
    a.x = -atan2f(d.z, sqrtf(d.x * d.x + d.y * d.y)) * 57.29577951308232f;
    a.y = atan2f(d.y, d.x) * 57.29577951308232f;
    a.z = 0.0f;
    return a;
}

static float AngleDistance(const QAngle& a, const QAngle& b) {
    float dx = a.x - b.x;
    float dy = NormalizeYaw(a.y - b.y);
    return sqrtf(dx * dx + dy * dy);
}

static QAngle LerpAngle(const QAngle& from, const QAngle& to, float t) {
    QAngle out;
    out.x = from.x + (to.x - from.x) * t;
    out.y = from.y + NormalizeYaw(to.y - from.y) * t;
    out.z = 0.0f;
    return out;
}

// ---------------------------------------------------------------------------
// camera / third person
// ---------------------------------------------------------------------------
// The native third-person path is gated by sv_cheats in client.dll.  On a
// normal server that value is 0, so the engine resets the camera flag and hides
// the local player.  The client-side value only drives local checks; forcing it
// lets the native third-person camera (and body render path) stay stable.
static bool g_world_needs_cheats = false;

static void ForceSvCheats() {
#ifndef _WIN64
    // On v34, CmdSet clears FCVAR_CHEAT and calls ConVar::SetValue directly,
    // and forcing sv_cheats = 1 causes native CAM_ToThirdPerson/CAM_Think to
    // fight with our OverrideView third-person camera.
    return;
#else
    static ConVar* sc = nullptr;
    static int original = -1;
    if (!sc && cvar.self)
        sc = cvar.FindVar("sv_cheats");
    if (!sc) return;

    bool const want = cfg.thirdperson || g_world_needs_cheats;
    if (want) {
        if (original < 0) original = sc->i_value;
        sc->f_value = 1.0f;
        sc->i_value = 1;
        *reinterpret_cast<int*>((char*)sc + 0x58) = 1;
    } else if (original >= 0) {
        sc->f_value = original ? 1.0f : 0.0f;
        sc->i_value = original;
        *reinterpret_cast<int*>((char*)sc + 0x58) = original;
        original = -1;
    }
#endif
}

static bool LocalCanThirdPerson() {
    if (!cfg.thirdperson || !IsGameReady()) return false;
    void* local = GetLocalEntity();
    if (!local) return false;
    if (*reinterpret_cast<const char*>((const char*)local + off::m_lifeState) != 0) return false;
    if (*reinterpret_cast<const int*>((const char*)local + off::m_iObserverMode) != 0) return false; // m_iObserverMode
    return true;
}

static void* GetSpectatedPlayer() {
    if (!IsGameReady() || !entity_list.self) return nullptr;
    void* local = GetLocalEntity();
    if (!local) return nullptr;
    if (*reinterpret_cast<const char*>((const char*)local + off::m_lifeState) == 0) return nullptr;
    int const obs_mode = *reinterpret_cast<const int*>((const char*)local + off::m_iObserverMode);
    if (obs_mode != 4 /* OBS_MODE_IN_EYE */ && obs_mode != 5 /* OBS_MODE_CHASE */) return nullptr;
    int const h = *reinterpret_cast<const int*>((const char*)local + off::m_hObserverTarget); // m_hObserverTarget
    if (h == -1) return nullptr;
    int const idx = h & 0xFFF;
    if (idx <= 0 || idx > 64 || !IsPlayerIndex(idx)) return nullptr;
    void* target = entity_list.GetClientEntity(idx);
    if (!target || target == local) return nullptr;
    if (EntityIsStale(target, idx)) return nullptr;
    if (*reinterpret_cast<const char*>((const char*)target + off::m_lifeState) != 0) return nullptr;
    if (*reinterpret_cast<const int*>((const char*)target + off::m_iHealth) <= 0) return nullptr;
    return target;
}

static void UpdateThirdPerson() {
    void* input = GetInput();
    if (!input || !client_module) return;

    ForceSvCheats();

    bool active = *reinterpret_cast<uint8_t*>((char*)input + INPUT_CAM_THIRD_PERSON) != 0;
    if (LocalCanThirdPerson()) {
        // Native third person keeps the local body in the render path.  The
        // actual camera position/distance is overridden in OverrideView so the
        // slider is authoritative even when sv_cheats is 0.
        if (!active) {
            auto fn = VFunc<void(THISCALL_CC*)(void*)>(input, INPUT_CAM_TO_THIRD);
            fn(input);
        }
        *reinterpret_cast<uint8_t*>((char*)input + INPUT_CAM_THIRD_PERSON) = 1;
    } else if (active) {
        auto fn = VFunc<void(THISCALL_CC*)(void*)>(input, INPUT_CAM_TO_FIRST);
        fn(input);
        *reinterpret_cast<uint8_t*>((char*)input + INPUT_CAM_THIRD_PERSON) = 0;
    }
}

// ---------------------------------------------------------------------------
// movement / combat
// ---------------------------------------------------------------------------
TYPEDEF_THISCALL(bool, CreateMoveFn, void*, float, CUserCmd*);
TYPEDEF_THISCALL(void, OverrideViewFn, void*, CViewSetup*);
TYPEDEF_THISCALL(bool, ShouldDrawLocalPlayerFn, void*, void*);

static CreateMoveFn oCreateMove = nullptr;
static OverrideViewFn oOverrideView = nullptr;
static ShouldDrawLocalPlayerFn oShouldDrawLocalPlayer = nullptr;
TYPEDEF_THISCALL(bool, ShouldDrawViewModelFn, void*);
static ShouldDrawViewModelFn oShouldDrawViewModel = nullptr;

static int AimKeyDown() {
    switch (cfg.aim_key) {
    case 0: return 1;
    case 1: return (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
    case 2: return (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
    case 3: return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    default: return 0;
    }
}

static void* GetActiveWeapon(void* local) {
    if (!local) return nullptr;
    int const handle = *reinterpret_cast<int*>((char*)local + off::m_hActiveWeapon);
    int const idx = handle & 0xFFF;
    return idx > 0 ? entity_list.GetClientEntity(idx) : nullptr;
}

static float GetIntervalPerTick() {
#ifdef _WIN64
    if (client_module) {
        void* globals = *reinterpret_cast<void**>((char*)client_module + CLIENT_GLOBALS_RVA);
        if (globals) {
            float const ipt = *reinterpret_cast<const float*>((const char*)globals + 0x1c);
            if (ipt > 0.001f && ipt < 0.2f) return ipt;
        }
    }
#else
    if (base_client) {
        static void* s_globals = nullptr;
        if (!s_globals)
            s_globals = **reinterpret_cast<void***>((*reinterpret_cast<uintptr_t**>(base_client))[0] + 0x2F);
        if (s_globals) {
            float const ipt = *reinterpret_cast<const float*>((const char*)s_globals + 0x1c);
            if (ipt > 0.001f && ipt < 0.2f) return ipt;
        }
    }
#endif
    return 1.0f / 66.666666f;
}

static float GetLerpTime() {
    static ConVar* cv_interp = nullptr;
    static ConVar* cv_ratio = nullptr;
    static ConVar* cv_updaterate = nullptr;
    if (!cv_interp && cvar.self) {
        cv_interp = cvar.FindVar("cl_interp");
        cv_ratio = cvar.FindVar("cl_interp_ratio");
        cv_updaterate = cvar.FindVar("cl_updaterate");
    }
    float const interp = cv_interp ? cv_interp->f_value : 0.03125f;
    float const ratio = cv_ratio ? cv_ratio->f_value : 2.0f;
    float updaterate = cv_updaterate ? cv_updaterate->f_value : 66.0f;
    if (updaterate < 10.0f) updaterate = 10.0f;
    float const min_interp = ratio / updaterate;
    return interp > min_interp ? interp : min_interp;
}

// CWeaponCSBase in x64 client.dll (build 9540945):
//   [382] +0xbf0 = GetInaccuracy(), [383] +0xbf8 = GetSpread()
static float WeaponGetInaccuracy(void* weapon) {
#ifdef _WIN64
    if (!weapon) return 0.0f;
    float const v = VFunc<float(THISCALL_CC*)(void*)>(weapon, 382)(weapon);
    return (v == v && v > 0.0f && v < 1.0f) ? v : 0.0f;
#else
    (void)weapon;
    return 0.0f;
#endif
}

static float WeaponGetSpread(void* weapon) {
#ifdef _WIN64
    if (!weapon) return 0.0f;
    float const v = VFunc<float(THISCALL_CC*)(void*)>(weapon, 383)(weapon);
    return (v == v && v > 0.0f && v < 1.0f) ? v : 0.0f;
#else
    if (!weapon) return 0.0f;
    void* local = GetLocalEntity();
    if (!local) return 0.0f;

    // CWeaponCSBase::GetWeaponID() is vtable slot 297 (0x4a4 / 4) in v34 client.dll
    int const wid = VFunc<int(THISCALL_CC*)(void*)>(weapon, 297)(weapon);
    float acc = *reinterpret_cast<float*>((char*)weapon + off::w_flAccuracy);
    if (!(acc == acc) || acc < 0.0f || acc > 2.5f) acc = 0.2f;

    int const flags = *reinterpret_cast<int*>((char*)local + off::m_fFlags);
    bool const on_ground = (flags & FL_ONGROUND) != 0;
    bool const ducking = (flags & FL_DUCKING) != 0;
    Vector const vel = *reinterpret_cast<Vector*>((char*)local + off::m_vecVelocity);
    float const speed = sqrtf(vel.x * vel.x + vel.y * vel.y);
    int const fov = *reinterpret_cast<int*>((char*)local + off::m_iFOV);
    bool const zoomed = (fov > 0 && fov < 90);

    // Exact CS:S v34 (Source 2006) per-weapon PrimaryAttack spread formulas
    switch (wid) {
    case 27: // AK47
        if (!on_ground) return 0.04f + 0.4f * acc;
        if (speed > 140.0f) return 0.04f + 0.07f * acc;
        return 0.0275f * acc;
    case 21: // M4A1 (unsilenced)
        if (!on_ground) return 0.035f + 0.4f * acc;
        if (speed > 140.0f) return 0.035f + 0.07f * acc;
        return 0.02f * acc;
    case 25: // DEagle
        if (!on_ground) return 1.5f * (1.0f - acc);
        if (speed > 5.0f) return 0.25f * (1.0f - acc);
        if (ducking) return 0.115f * (1.0f - acc);
        return 0.13f * (1.0f - acc);
    case 17: // AWP
        if (!on_ground) return 0.85f;
        if (speed > 140.0f) return 0.25f;
        if (speed > 10.0f) return 0.10f;
        if (ducking) return zoomed ? 0.0f : 0.08f;
        return zoomed ? 0.001f : 0.08f;
    case 3: // Scout
        if (!on_ground) return 0.2f;
        if (speed > 170.0f) return 0.075f;
        if (ducking) return zoomed ? 0.0f : 0.025f;
        return zoomed ? 0.007f : 0.032f;
    case 13: // SG550
    case 23: // G3SG1
        if (!on_ground) return 0.45f * (1.0f - acc);
        if (speed > 5.0f) return 0.15f;
        if (ducking) return (zoomed ? 0.0f : 0.025f) + 0.04f * (1.0f - acc);
        return (zoomed ? 0.0f : 0.025f) + 0.05f * (1.0f - acc);
    case 16: // USP
        if (!on_ground) return 1.2f * (1.0f - acc);
        if (speed > 5.0f) return 0.225f * (1.0f - acc);
        if (ducking) return 0.08f * (1.0f - acc);
        return 0.1f * (1.0f - acc);
    case 2: // Glock
        if (!on_ground) return 1.0f * (1.0f - acc);
        if (speed > 5.0f) return 0.165f * (1.0f - acc);
        if (ducking) return 0.075f * (1.0f - acc);
        return 0.1f * (1.0f - acc);
    case 1: // P228
        if (!on_ground) return 1.5f * (1.0f - acc);
        if (speed > 5.0f) return 0.255f * (1.0f - acc);
        if (ducking) return 0.075f * (1.0f - acc);
        return 0.15f * (1.0f - acc);
    case 11: // FiveSeven
    case 10: // Elite
        if (!on_ground) return 1.5f * (1.0f - acc);
        if (speed > 5.0f) return 0.255f * (1.0f - acc);
        if (ducking) return 0.075f * (1.0f - acc);
        return 0.15f * (1.0f - acc);
    case 14: // Galil
        if (!on_ground) return 0.04f + 0.3f * acc;
        if (speed > 140.0f) return 0.04f + 0.07f * acc;
        return 0.0375f * acc;
    case 15: // Famas
        if (!on_ground) return 0.03f + 0.3f * acc;
        if (speed > 140.0f) return 0.03f + 0.07f * acc;
        return 0.02f * acc;
    case 8:  // AUG
    case 26: // SG552
        if (!on_ground) return 0.035f + 0.4f * acc;
        if (speed > 140.0f) return 0.035f + 0.07f * acc;
        return 0.02f * acc;
    case 18: // MP5
        if (!on_ground) return 0.2f * acc;
        return 0.04f * acc;
    case 29: // P90
        if (!on_ground) return 0.3f * acc;
        if (speed > 170.0f) return 0.115f * acc;
        return 0.045f * acc;
    case 12: // UMP45
        if (!on_ground) return 0.24f * acc;
        return 0.04f * acc;
    case 7:  // MAC10
        if (!on_ground) return 0.375f * acc;
        return 0.03f * acc;
    case 22: // TMP
        if (!on_ground) return 0.25f * acc;
        return 0.03f * acc;
    case 19: // M249
        if (!on_ground) return 0.045f + 0.5f * acc;
        if (speed > 140.0f) return 0.045f + 0.095f * acc;
        return 0.03f * acc;
    case 20: // M3
    case 5:  // XM1014
        return 0.0675f;
    default:
        return 0.0f;
    }
#endif
}

static bool WeaponCanFire(void* local) {
    void* weapon = GetActiveWeapon(local);
    if (!weapon) return false;
    int const clip = *reinterpret_cast<int*>((char*)weapon + off::w_iClip1);
    if (clip == 0) return false;
    int const tick_base = *reinterpret_cast<int*>((char*)local + off::m_nTickBase);
    float const server_time = (float)tick_base * GetIntervalPerTick();
    float const next_attack = *reinterpret_cast<float*>((char*)weapon + off::w_flNextPrimaryAttack);
    return next_attack <= server_time + 1e-4f;
}

static float ComputeHitChance(void* weapon, float dist, int hitbox) {
    if (!weapon || cfg.nospread) return 100.0f;
    float const inacc = WeaponGetInaccuracy(weapon);
    float const spread = WeaponGetSpread(weapon);
    if (inacc + spread <= 1e-6f) return 100.0f;

    float const radius = (hitbox <= 1) ? 3.2f : 6.0f;
    float const max_tan = radius / (dist > 1.0f ? dist : 1.0f);
    float const max_tan2 = max_tan * max_tan;
    float const two_pi = 6.283185307179586f;

    int hits = 0;
    uint32_t rng = 0x517cc1b7u;
    auto next_u = [&rng]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return (float)(rng >> 8) * (1.0f / 16777216.0f);
    };
    for (int s = 0; s < 32; s++) {
#ifdef _WIN64
        float const a = two_pi * next_u();
        float const b = inacc * next_u();
        float const c = two_pi * next_u();
        float const d = spread * next_u();
        float const sx = cosf(a) * b + cosf(c) * d;
        float const sy = sinf(a) * b + sinf(c) * d;
#else
        // v34 FX_FireBullets triangular spread: (RandomFloat(-0.5, 0.5) + RandomFloat(-0.5, 0.5)) * flSpread
        float const sx = ((next_u() - 0.5f) + (next_u() - 0.5f)) * spread;
        float const sy = ((next_u() - 0.5f) + (next_u() - 0.5f)) * spread;
#endif
        if (sx * sx + sy * sy <= max_tan2)
            hits++;
    }
    return ((float)hits / 32.0f) * 100.0f;
}

static bool FindSilentTarget(const Vector& eye, const QAngle& camera, QAngle& out, int& out_tick_count, bool allow_wall = true) {
    out_tick_count = 0;
    void* local = GetLocalEntity();
    if (!local) return false;

    int local_team = *reinterpret_cast<int*>((char*)local + off::m_iTeamNum);
    ActiveAimSettings const aim = GetAimSettings( );

    // Rank enemies once (nearest cover / lowest health / closest to the
    // crosshair), then walk the hitboxes head-first.  The old code treated
    // every hitbox point of every enemy as an independent "distance" target,
    // so the nearest point could be a pelvis/leg point - that is why shots
    // went into the legs instead of the body.
    struct Cand { float metric; int index; void* ent; };
    Cand cands[64];
    int count = 0;

    for (int i = 1; i <= 64; i++) {
        void* ent = entity_list.GetClientEntity(i);
        if (!ent || ent == local) continue;
        if (!IsPlayerIndex(i)) continue;
        if (EntityIsStale(ent, i)) continue;
        if (*reinterpret_cast<char*>((char*)ent + off::m_lifeState) != 0) continue;
        int const health = *reinterpret_cast<int*>((char*)ent + off::m_iHealth);
        if (health <= 0) continue;
        if (*reinterpret_cast<int*>((char*)ent + off::m_iTeamNum) == local_team) continue;
        if (IsWhitelisted(i)) continue;

        Vector const pe = PlayerEye(ent);
        QAngle const a = CalcAngle(eye, pe);
        float metric;
        if (cfg.silent_select == 1) {
            Vector const d = pe - eye;
            metric = d.x * d.x + d.y * d.y + d.z * d.z;
        } else if (cfg.silent_select == 2) {
            metric = (float)health;
        } else {
            metric = AngleDistance(a, camera);
        }
        cands[count].metric = metric;
        cands[count].index = i;
        cands[count].ent = ent;
        count++;
    }

    // insertion sort, at most 63 entries
    for (int i = 1; i < count; i++) {
        Cand key = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].metric > key.metric) {
            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = key;
    }

    bool any_hb = false;
    for (bool b : aim.hb) any_hb = any_hb || b;
    static const float heights[5] = { 1.0f, 0.88f, 0.72f, 0.58f, 0.44f };
    bool const allow_wall_trace = allow_wall && cfg.auto_wall;

    // Two-pass choice: an enemy that can actually be seen always wins over an
    // enemy behind cover.  The old code returned the enemy nearest to the
    // crosshair even when it was behind a wall, so with several enemies it
    // fired into the wall next to the clear one.  Only when nobody is visible
    // do we fall back to the nearest target the bullet can punch through to.
    int vis_rank = -1, wall_rank = -1;
    Vector vis_target, wall_target;
    int vis_hb = -1, wall_hb = -1;
    int vis_hc_rank = -1, wall_hc_rank = -1;
    Vector vis_hc_target, wall_hc_target;
    int vis_hc_hb = -1, wall_hc_hb = -1;

    void* const local_wpn = GetActiveWeapon(local);
    int wpn_base_dmg = 0;
    float wpn_armor_ratio = 1.0f, wpn_range_mod = 0.98f;
    bool const have_wpn_dmg = GetActiveWeaponDamageInfo(wpn_base_dmg, wpn_armor_ratio, wpn_range_mod);
    bool const baim_bind_on = cfg.baim_force && BindActive(cfg.kb_baim, cfg.kb_mode_baim, BIND_BAIM);

    for (int c = 0; c < count && vis_hc_rank < 0; c++) {
        void* ent = cands[c].ent;
        int const ei = cands[c].index;
        Vector const origin = *reinterpret_cast<Vector*>((char*)ent + off::m_vecOrigin);
        Vector vo = *reinterpret_cast<Vector*>((char*)ent + off::m_vecViewOffset);
        if (!(vo.z > 1.0f && vo.z < 100.0f)) vo.z = 64.0f;

        int hb_bones[5] = { -1, -1, -1, -1, -1 };
        GetEntityAimBones(ent, hb_bones);

        // Per-target Claret-style body aim evaluation
        int const flags = *reinterpret_cast<int*>((char*)ent + off::m_fFlags);
        Vector const vel = *reinterpret_cast<Vector*>((char*)ent + off::m_vecVelocity);
        float const speed2d = sqrtf(vel.x * vel.x + vel.y * vel.y);
        int const hp = *reinterpret_cast<int*>((char*)ent + off::m_iHealth);

        bool const is_air = (flags & FL_ONGROUND) == 0;
        bool const is_moving = speed2d > 10.0f;
        bool const is_peeking = speed2d > 45.0f;
        bool is_lethal_body = (hp <= 50);
        if (have_wpn_dmg) {
            Vector const dv = origin - eye;
            float const dist = sqrtf(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
            int const body_hb = (cfg.body_aim_hitbox >= 2 && cfg.body_aim_hitbox <= 4) ? cfg.body_aim_hitbox : 2;
            float const bdmg = CalcHitDamage((float)wpn_base_dmg, wpn_armor_ratio, wpn_range_mod, dist, body_hb, ent);
            is_lethal_body = bdmg >= (float)hp;
        }

        bool const target_shooting = TargetRecentlyShot(ei, 250);
        bool const target_resolved = Resolver_IsResolved(ei);
        bool const body_disabled = (cfg.baim_dis_shooting && target_shooting) ||
                                   (cfg.baim_dis_resolved && target_resolved);

        bool const force_cond = baim_bind_on || cfg.body_aim == 2 ||
                                (cfg.baim_lethal && is_lethal_body) ||
                                (cfg.baim_in_air && is_air) ||
                                (cfg.baim_moving && is_moving) ||
                                (cfg.baim_on_peek && is_peeking);
        bool const force_baim = force_cond && !body_disabled;
        bool const prefer_baim = (cfg.body_aim == 1) && !body_disabled;

        // Build hitbox scan order (0=head, 1=neck, 2=chest, 3=stomach, 4=pelvis)
        int scan_order[5];
        int scan_n = 0;
        if (force_baim || prefer_baim) {
            int const pref = (cfg.body_aim_hitbox >= 2 && cfg.body_aim_hitbox <= 4) ? cfg.body_aim_hitbox : 2;
            scan_order[scan_n++] = pref;
            for (int b = 2; b <= 4; b++)
                if (b != pref) scan_order[scan_n++] = b;
            // Under forced baim, only allow head/neck as fallback if disablers permit
            if (!force_baim || cfg.baim_dis_low_dmg || cfg.baim_dis_head_safe) {
                scan_order[scan_n++] = 0;
                scan_order[scan_n++] = 1;
            }
        } else {
            for (int b = 0; b < 5; b++) scan_order[scan_n++] = b;
        }

        bool any_body_hit = false;
        for (int si = 0; si < scan_n && vis_hc_rank < 0; si++) {
            int const hb = scan_order[si];
            if (any_hb && !aim.hb[hb]) continue;

            // Under force_baim, only allow head/neck when a body disabler fires
            if (force_baim && hb < 2) {
                bool const allow_head_fallback =
                    (cfg.baim_dis_low_dmg && !any_body_hit) ||
                    (cfg.baim_dis_head_safe && !any_body_hit);
                if (!allow_head_fallback) continue;
            }

            Vector center;
            if (hb_bones[hb] >= 0 && GetEntityBonePos(ei, hb_bones[hb], center)) {
                // Head1 sits at the jaw/neck attachment; +3.5z centers on the head hitbox
                if (hb == 0) center.z += 3.5f;
            } else {
                center = origin + Vector(0.0f, 0.0f, vo.z * heights[hb]);
            }

            // Build multipoint list (center always first)
            Vector pts[3];
            int pt_n = 1;
            pts[0] = center;
            float const mp_side = (hb == 0) ? 2.2f : ((hb == 2 || hb == 3) ? 4.0f : 0.0f);
            if (mp_side > 0.0f) {
                Vector fwd, right, up;
                AngleVectors(CalcAngle(eye, center), fwd, right, up);
                pts[1] = center + right * mp_side;
                pts[2] = center - right * mp_side;
                pt_n = 3;
            }

            for (int pi = 0; pi < pt_n; pi++) {
                Vector const& target = pts[pi];
                QAngle const a = CalcAngle(eye, target);
                float const ang = AngleDistance(a, camera);
                if (cfg.silent_select == 0 && aim.silent_fov > 0.0f && ang > aim.silent_fov)
                    continue;

                Vector const dv = target - eye;
                float const dist = sqrtf(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
                bool const passes_hc = (cfg.nospread || aim.hit_chance <= 0.0f) ||
                                       (ComputeHitChance(local_wpn, dist, hb) >= aim.hit_chance);

                if (TraceVisible(eye, target, ent)) {
                    if (aim.min_damage > 0.0f && have_wpn_dmg) {
                        float const dmg = CalcHitDamage((float)wpn_base_dmg, wpn_armor_ratio, wpn_range_mod, dist, hb, ent);
                        float const req = aim.min_damage > (float)hp ? (float)hp : aim.min_damage;
                        if (dmg < req) continue;
                    }
                    if (hb >= 2) any_body_hit = true;
                    if (vis_rank < 0) {
                        vis_rank = c;
                        vis_target = target;
                        vis_hb = hb;
                    }
                    if (passes_hc) {
                        vis_hc_rank = c;
                        vis_hc_target = target;
                        vis_hc_hb = hb;
                        break;
                    }
                } else if (allow_wall_trace && (wall_hc_rank < 0 || wall_rank < 0)) {
#ifndef _WIN64
                    if (pi > 0) continue;
#endif
                    float const req = (aim.min_damage > 0.0f)
                        ? (aim.min_damage > (float)hp ? (float)hp : aim.min_damage)
                        : 1.0f;
                    if (CanPenetrateTarget(eye, target, ent, hb, req)) {
                        if (hb >= 2) any_body_hit = true;
                        if (wall_rank < 0) {
                            wall_rank = c;
                            wall_target = target;
                            wall_hb = hb;
                        }
                        if (passes_hc && wall_hc_rank < 0) {
                            wall_hc_rank = c;
                            wall_hc_target = target;
                            wall_hc_hb = hb;
                        }
                    }
                }
            }
        }
    }

    int rank = -1;
    Vector target;
    int hb = -1;
    if (vis_hc_rank >= 0) {
        rank = vis_hc_rank; target = vis_hc_target; hb = vis_hc_hb;
    } else if (vis_rank >= 0) {
        rank = vis_rank; target = vis_target; hb = vis_hb;
    } else if (wall_hc_rank >= 0) {
        rank = wall_hc_rank; target = wall_hc_target; hb = wall_hc_hb;
    } else if (wall_rank >= 0) {
        rank = wall_rank; target = wall_target; hb = wall_hb;
    }
    if (rank < 0) return false;
    int const idx = cands[rank].index;
    Vector const cur_origin = *reinterpret_cast<Vector*>((char*)cands[rank].ent + off::m_vecOrigin);
    Vector vo = *reinterpret_cast<Vector*>((char*)cands[rank].ent + off::m_vecViewOffset);
    if (!(vo.z > 1.0f && vo.z < 100.0f)) vo.z = 64.0f;

    // backtrack: translate the chosen hitbox point by the historical origin delta,
    // verify the historical point can still be hit, and rewind cmd->tick_count
    if (cfg.backtrack && cfg.backtrack_time > 0.01f) {
        Vector bt;
        float bt_sim = 0.0f;
        if (Backtrack_PickRecord(idx, cfg.backtrack_time, bt, bt_sim)) {
            Vector const delta = bt - cur_origin;
            float const off2 = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            if (off2 < 64.0f * 64.0f) {
                Vector const bt_target = target + delta;
                if (TraceVisible(eye, bt_target, cands[rank].ent) ||
                    (allow_wall_trace && CanPenetrateTarget(eye, bt_target, cands[rank].ent, hb, 1.0f))) {
                    target = bt_target;
                    if (bt_sim > 0.0f)
                        out_tick_count = (int)(0.5f + (bt_sim + GetLerpTime()) / GetIntervalPerTick());
                }
            }
        }
    }
    // resolver: shift the aim point sideways after consecutive misses
    if (cfg.resolver) {
        int const shift = Resolver_Side(idx);
        if (shift != 0) {
            QAngle const ba = CalcAngle(eye, target);
            Vector fwd, right, up;
            AngleVectors(ba, fwd, right, up);
            target = target + right * (float)shift;
        }
    }

    out = CalcAngle(eye, target);

    // remember what we fired at so bullet impacts can be graded against the
    // aim point (miss log)
    {
        player_info_t info = {};
        if (engine.GetPlayerInfo(idx, &info) && info.name[0])
            strncpy(g_miss_target, info.name, sizeof(g_miss_target) - 1);
        else
            strncpy(g_miss_target, "?", sizeof(g_miss_target) - 1);
        static const char* const hb_names[5] = { "head", "neck", "chest", "stomach", "pelvis" };
        strncpy(g_miss_hitbox, hb_names[hb], sizeof(g_miss_hitbox) - 1);
        g_miss_hc = (int)(aim.hit_chance + 0.5f);
        g_miss_target_idx = idx;
        g_miss_aimpoint = target;
        g_miss_pending = true;
        g_miss_pending_time = GetTickCount64();
    }
    return true;
}

static void RunCombat(CUserCmd* cmd) {
    if (!cmd || !IsGameReady()) return;

    void* local = GetLocalEntity();
    if (!local) return;

    int local_team = *reinterpret_cast<int*>((char*)local + off::m_iTeamNum);
    Vector eye = PlayerEye(local); // shot origin, not the render camera
    QAngle view = view_angles;

    if (cfg.aimbot && BindActive(cfg.kb_aimbot, cfg.kb_mode_aimbot, BIND_AIMBOT) && AimKeyDown()) {
        float best = cfg.aim_fov;
        bool found = false;
        QAngle best_angle = cmd->viewangles;

        for (int i = 1; i <= 64; i++) {
            void* ent = entity_list.GetClientEntity(i);
            if (!ent || ent == local) continue;
            if (!IsPlayerIndex(i)) continue;
            if (EntityIsStale(ent, i)) continue;
            if (*reinterpret_cast<char*>((char*)ent + off::m_lifeState) != 0) continue;
            if (*reinterpret_cast<int*>((char*)ent + off::m_iHealth) <= 0) continue;
            if (*reinterpret_cast<int*>((char*)ent + off::m_iTeamNum) == local_team) continue;

            Vector target = PlayerEye(ent);
            QAngle a = CalcAngle(eye, target);
            float dist = AngleDistance(a, cmd->viewangles);
            if (dist < best) {
                best = dist;
                best_angle = a;
                found = true;
            }
        }

        if (found) {
            float smooth = cfg.aim_smooth;
            if (smooth < 1.0f) smooth = 1.0f;
            QAngle out = (smooth > 1.01f) ? LerpAngle(cmd->viewangles, best_angle, 1.0f / smooth) : best_angle;
            cmd->viewangles = out;
            engine.SetViewAngles(out);
        }
    }

    {
        ActiveAimSettings const aim = GetAimSettings( );
        if ( aim.auto_stop && ( cmd->buttons & IN_ATTACK ) ) {
            cmd->forwardmove = 0.0f;
            cmd->sidemove = 0.0f;
        }
        if ( aim.auto_scope && CurrentWeaponGroup( ) == 1 && ( cmd->buttons & IN_ATTACK ) ) {
            #ifdef _WIN64
            int const zoom = *reinterpret_cast<int*>( (char*)local + off::m_bIsScoped );
            if ( zoom == 0 )
#else
            int const fov = *reinterpret_cast<int*>( (char*)local + off::m_iFOV );
            if ( fov == 0 || fov == 90 )
#endif
                cmd->buttons |= IN_ATTACK2;
        }
    }

    // ---- knifebot / knife gate ------------------------------------------------
    // The trigger has no range limit (silent FOV 0 = 360), so with a knife it
    // kept trying to shoot enemies across the map.  A knife can never fire
    // from range: the normal trigger is skipped while a knife is held and the
    // knifebot only swings when an enemy is actually within reach.
    bool knife = false;
    {
        int const handle = *reinterpret_cast<int*>((char*)local + off::m_hActiveWeapon);
        int const widx = handle & 0xFFF;
        if (widx > 0) {
            void* nw = entity_list.GetClientNetworkable(widx);
            if (nw) {
                void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
                const char* cls = ClientClassName(cc);
                if (cls && (strstr(cls, "Knife") || strstr(cls, "knife")))
                    knife = true;
            }
        }
    }

    bool const alive_now =
        *reinterpret_cast<char*>((char*)local + off::m_lifeState) == 0 &&
        *reinterpret_cast<int*>((char*)local + off::m_iHealth) > 0;

    if (knife && cfg.knifebot && !cfg.menu_open && alive_now) {
        void* best_ent = nullptr;
        float best_dist = 1.0e9f;
        QAngle best_angle;
        Vector const eye_k = PlayerEye(local);

        for (int i = 1; i <= 64; i++) {
            void* ent = entity_list.GetClientEntity(i);
            if (!ent || ent == local) continue;
            if (!IsPlayerIndex(i)) continue;
            if (EntityIsStale(ent, i)) continue;
            if (*reinterpret_cast<char*>((char*)ent + off::m_lifeState) != 0) continue;
            if (*reinterpret_cast<int*>((char*)ent + off::m_iHealth) <= 0) continue;
            if (*reinterpret_cast<int*>((char*)ent + off::m_iTeamNum) == local_team) continue;

            Vector const target = PlayerEye(ent);
            Vector const d = target - eye_k;
            float const dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            if (dist > cfg.knife_range) continue;
            if (IsWhitelisted(i)) continue;
            if (!TraceVisible(eye_k, target, ent)) continue;
            if (dist < best_dist) {
                best_dist = dist;
                best_ent = ent;
                best_angle = CalcAngle(eye_k, target);
            }
        }

        static ULONGLONG last_swing = 0;
        ULONGLONG const now_k = GetTickCount64();
        if (best_ent && now_k - last_swing >= 380) {
            last_swing = now_k;
            cmd->buttons |= IN_ATTACK;
            cmd->viewangles = best_angle;
            g_knife_angle = best_angle;
            g_knife_time = (DWORD)now_k;
            g_knife_swing = true;
        }
    }

    bool const trig_on = cfg.triggerbot ||
        BindActive(cfg.kb_triggerbot, cfg.kb_mode_triggerbot, BIND_TRIGGERBOT);
    if (!knife && trig_on && !cfg.menu_open && WeaponCanFire(local) &&
        *reinterpret_cast<char*>((char*)local + off::m_lifeState) == 0 &&
        *reinterpret_cast<int*>((char*)local + off::m_iHealth) > 0) {
        bool fire = false;

        {
            ActiveAimSettings const aim = GetAimSettings( );
            bool any_hb = false;
            for (bool b : aim.hb) any_hb = any_hb || b;

            // The trigger has no FOV of its own: it uses the silent-aim FOV,
            // and 0 means 360 degrees (fire at anything visible around you).
            // The silent aim then turns the shot onto the target.
            float fov = aim.silent_fov;
            if (fov <= 0.0f) fov = 360.0f;

            for (int i = 1; i <= 64 && !fire; i++) {
                void* ent = entity_list.GetClientEntity(i);
                if (!ent || ent == local) continue;
                if (!IsPlayerIndex(i)) continue;
                if (EntityIsStale(ent, i)) continue;
                if (*reinterpret_cast<char*>((char*)ent + off::m_lifeState) != 0) continue;
                if (*reinterpret_cast<int*>((char*)ent + off::m_iHealth) <= 0) continue;
                if (*reinterpret_cast<int*>((char*)ent + off::m_iTeamNum) == local_team) continue;
                if (IsWhitelisted(i)) continue;

                Vector origin = *reinterpret_cast<Vector*>((char*)ent + off::m_vecOrigin);
                Vector vo = *reinterpret_cast<Vector*>((char*)ent + off::m_vecViewOffset);
                if (!(vo.z > 1.0f && vo.z < 100.0f)) vo.z = 64.0f;

                // 5 bone points plus the eye; also sample between them so a
                // crosshair on the arms/legs still registers.
                Vector points[9];
                int point_hb[9];
                int count = 0;
                int hb_bones[5] = { -1, -1, -1, -1, -1 };
                GetEntityAimBones(ent, hb_bones);
                const float heights[5] = { vo.z, vo.z * 0.88f, vo.z * 0.72f, vo.z * 0.58f, vo.z * 0.44f };
                for (int hb = 0; hb < 5; hb++) {
                    if (any_hb && !aim.hb[hb]) continue;
                    Vector p;
                    if (hb_bones[hb] < 0 || !GetEntityBonePos(i, hb_bones[hb], p))
                        p = origin + Vector(0.0f, 0.0f, heights[hb]);
                    else if (hb == 0)
                        p.z += 3.5f;
                    point_hb[count] = hb;
                    points[count++] = p;
                }
                point_hb[count] = 0; points[count++] = PlayerEye(ent);
                point_hb[count] = 3; points[count++] = origin + Vector(0.0f, 0.0f, vo.z * 0.30f);
                point_hb[count] = 2; points[count++] = origin + Vector(0.0f, 0.0f, vo.z * 0.80f);

                // Screen-space check: the crosshair is literally over the
                // enemy box.  Used as a fallback when the trace entity pointer
                // doesn't compare equal to the client entity.
                bool screen_over = false;
                {
                    int const sw = (int)ImGui::GetIO().DisplaySize.x;
                    int const sh = (int)ImGui::GetIO().DisplaySize.y;
                    ImVec2 s_head, s_feet;
                    if (sw > 0 && sh > 0 && WorldToScreen(origin + Vector(0, 0, vo.z), s_head, sw, sh) &&
                        WorldToScreen(origin, s_feet, sw, sh)) {
                        float const cx = sw * 0.5f;
                        float const cy = sh * 0.5f;
                        float const h = s_feet.y - s_head.y;
                        float const w = h * 0.55f;
                        screen_over = cx >= s_head.x - w * 0.5f && cx <= s_head.x + w * 0.5f &&
                                      cy >= s_head.y - 4.0f && cy <= s_feet.y + 4.0f;
                    }
                }

                for (int k = 0; k < count && !fire; k++) {
                    QAngle a = CalcAngle(eye, points[k]);
                    if (AngleDistance(a, cmd->viewangles) > fov) continue;
                    bool const visible = TraceVisible(eye, points[k], ent);
                    bool const can_wall = !visible && cfg.auto_wall && (k == 0 || k == 2) &&
                        CanPenetrateTarget(eye, points[k], ent, point_hb[k], aim.min_damage);
                    if (!visible && !can_wall) continue;
                    // 0 means "no hit chance requirement" (the slider's leftmost position)
                    if (!cfg.nospread && aim.hit_chance > 0.0f) {
                        Vector const dv = points[k] - eye;
                        float const dist = sqrtf(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
                        if (ComputeHitChance(GetActiveWeapon(local), dist, point_hb[k]) < aim.hit_chance)
                            continue;
                    }
                    fire = true;
                }
            }
        }

        // Pulse instead of holding +attack: holding it emptied the magazine
        // and the weapon then needed time before the next manual shot.
        static ULONGLONG last_trigger = 0;
        static ULONGLONG acquire_time = 0;
        static bool had_target = false;
        ULONGLONG now = GetTickCount64();
        ActiveAimSettings const aim_delay = GetAimSettings( );
        if ( fire ) {
            if ( !had_target ) { had_target = true; acquire_time = now; }
            if ( now - acquire_time < (ULONGLONG)aim_delay.delay_shot )
                fire = false;
        } else {
            had_target = false;
        }
        static ULONGLONG attack_until = 0;
        if (fire && now >= last_trigger) {
            attack_until = now + 55;
            last_trigger = now + 105;
        }
        if (now < attack_until)
            cmd->buttons |= IN_ATTACK;
    }
}

static void FixMovement(CUserCmd* cmd, const QAngle& real) {
    if (!cmd) return;

    // CGameMovement projects the view vectors onto the floor (z = 0) before
    // building wishdir, so only yaw matters for movement.  Using the full
    // pitched basis here made a pitched anti-aim turn W into a slow backward
    // walk (the horizontal wish got dotted against a near-vertical forward).
    float dyaw = NormalizeYaw(cmd->viewangles.y - real.y);
    if (fabsf(dyaw) < 0.01f)
        return;

    QAngle real_yaw(0.0f, real.y, 0.0f);
    QAngle fake_yaw(0.0f, cmd->viewangles.y, 0.0f);

    Vector f_real, r_real, u_tmp, f_fake, r_fake;
    AngleVectors(real_yaw, f_real, r_real, u_tmp);
    AngleVectors(fake_yaw, f_fake, r_fake, u_tmp);

    Vector wish = f_real * cmd->forwardmove + r_real * cmd->sidemove;
    float fm = Dot(wish, f_fake);
    float sm = Dot(wish, r_fake);
    if (fm > 450.0f) fm = 450.0f;
    if (fm < -450.0f) fm = -450.0f;
    if (sm > 450.0f) sm = 450.0f;
    if (sm < -450.0f) sm = -450.0f;
    cmd->forwardmove = fm;
    cmd->sidemove = sm;
}

static void RunAntiAim(CUserCmd* cmd, const QAngle& base) {
    if (!cfg.aa_enabled || !cmd) return;

    static int tick = 0;
    static bool yaw_side = false;
    static int side_hold = 0;
    static float spin = 0.0f;
    static uint32_t rng = 0x12345678u;
    tick++;

    QAngle out = base;

    // Pitch (reference: Disabled / Down / Up / Zero)
    if (cfg.aa_pitch == 1)       out.x = 89.0f;   // down
    else if (cfg.aa_pitch == 2)  out.x = -89.0f;  // up
    else if (cfg.aa_pitch == 3)  out.x = 0.0f;    // zero
    else                         out.x = base.x;  // none = your view pitch

    // Yaw base: local view or nearest target direction (cached 100 ms to avoid
    // scanning 64 entities every tick)
    float yaw = base.y;
    if (cfg.aa_base == 1) {
        static DWORD last_aa_scan = 0;
        static float cached_target_yaw = 0.0f;
        static bool has_cached_target = false;
        DWORD const now_aa = GetTickCount();
        if (now_aa - last_aa_scan >= 100) {
            last_aa_scan = now_aa;
            has_cached_target = false;
            void* local = GetLocalEntity();
            if (local) {
                Vector eye = PlayerEye(local);
                float best = 1.0e18f;
                float best_yaw = yaw;
                int const local_team = *reinterpret_cast<int*>((char*)local + off::m_iTeamNum);
                for (int i = 1; i <= 64; i++) {
                    void* ent = entity_list.GetClientEntity(i);
                    if (!ent || ent == local) continue;
                    if (!IsPlayerIndex(i)) continue;
                    if (EntityIsStale(ent, i)) continue;
                    if (*reinterpret_cast<char*>((char*)ent + off::m_lifeState) != 0) continue;
                    if (*reinterpret_cast<int*>((char*)ent + off::m_iHealth) <= 0) continue;
                    if (*reinterpret_cast<int*>((char*)ent + off::m_iTeamNum) == local_team) continue;

                    Vector t = PlayerEye(ent);
                    Vector d = t - eye;
                    float dist = d.x * d.x + d.y * d.y + d.z * d.z;
                    if (dist < best) {
                        best = dist;
                        best_yaw = atan2f(d.y, d.x) * 57.29577951308232f;
                        has_cached_target = true;
                    }
                }
                if (has_cached_target) cached_target_yaw = best_yaw;
            }
        }
        if (has_cached_target)
            yaw = cached_target_yaw;
    }

    // Yaw sides with delay (reference: left/right hold for iSideDelay ticks)
    float side_offset = 0.0f;
    if (cfg.aa_sides) {
        int delay = cfg.aa_side_delay < 1 ? 1 : cfg.aa_side_delay;
        if (++side_hold >= delay) { side_hold = 0; yaw_side = !yaw_side; }
        side_offset = yaw_side ? cfg.aa_right : -cfg.aa_left;
    }

    // Yaw (+180 for backward)
    float offset = cfg.aa_yaw_offset + side_offset;
    if (cfg.aa_yaw == 1) yaw += 180.0f;
    yaw += offset;

    // Yaw modifier (the reference resolveModifier)
    float modifier = 0.0f;
    float moff = cfg.aa_modifier_offset;
    bool flip = (tick & 1) != 0;
    switch (cfg.aa_modifier) {
    case 1: modifier = flip ? moff * 0.5f : -moff * 0.5f; break;              // center
    case 2: modifier = flip ? moff : 0.0f; break;                              // offset
    case 3:                                                                     // random
        rng = rng * 1664525u + 1013904223u;
        modifier = (((rng >> 8) & 0xFFFF) / 65535.0f * 2.0f - 1.0f) * moff;
        break;
    case 4: {                                                                   // spin
        float range = fabsf(moff);
        if (range < 10.0f) range = 10.0f;
        spin = fmodf(spin + range * 0.61803398875f, range);
        modifier = spin - range * 0.5f;
        break;
    }
    case 5: {                                                                   // three-way
        static const float three[] = { -1.0f, 0.0f, 1.0f };
        modifier = three[tick % 3] * moff;
        break;
    }
    case 6: {                                                                   // five-way
        static const float five[] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
        modifier = five[tick % 5] * moff;
        break;
    }
    default: break;
    }
    yaw += modifier;

    // manual yaw: holding a manual key rotates the fake body to that side
    // (like reference's manuals), overriding base yaw / sides / modifier
    if (cfg.aa_manual_left && (GetAsyncKeyState(cfg.aa_manual_left) & 0x8000))
        yaw = base.y - 90.0f;
    else if (cfg.aa_manual_right && (GetAsyncKeyState(cfg.aa_manual_right) & 0x8000))
        yaw = base.y + 90.0f;

    out.y = NormalizeYaw(yaw);
    out.z = cfg.aa_roll; // extended roll, if the engine accepts it
    cmd->viewangles = out;
    aa_view_angles = out;
}

static bool RunSilentAim(CUserCmd* cmd, const QAngle& camera) {
    if (!cfg.silentaim || !BindActive(cfg.kb_silentaim, cfg.kb_mode_silentaim, BIND_SILENTAIM) || !cmd || !(cmd->buttons & IN_ATTACK) || !IsGameReady())
        return false;

    void* local = GetLocalEntity();
    Vector eye = local ? PlayerEye(local) : Vector(0, 0, 0);

    QAngle target;
    int bt_tick = 0;
    if (FindSilentTarget(eye, camera, target, bt_tick)) {
        cmd->viewangles = target; // outgoing command only; camera is restored
        if (bt_tick > 0)
            cmd->tick_count = bt_tick;
        return true;
    } else if (cfg.log_misses) {
        static bool no_target_prev = false;
        bool const attack = (cmd->buttons & IN_ATTACK) != 0;
        if (attack && !no_target_prev) {
            MissLog_Push("Missed shot due to ", "no target in FOV");
        }
        no_target_prev = attack;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Movement exploits (client-side input automation)
// ---------------------------------------------------------------------------
static void RunExploits(CUserCmd* cmd, void* local) {
    if (!cmd || !local) return;
    if (*reinterpret_cast<char*>((char*)local + off::m_lifeState) != 0) return;

    int const flags = *reinterpret_cast<int*>((char*)local + off::m_fFlags);
    Vector const vel = *reinterpret_cast<Vector*>((char*)local + off::m_vecVelocity);
    float const speed = sqrtf(vel.x * vel.x + vel.y * vel.y);

    // Duck peek / crouch spam: rapid IN_DUCK toggling while held
    if (cfg.x_duckpeek && cfg.kb_duckpeek != 0 && BindActive(cfg.kb_duckpeek, cfg.kb_mode_duckpeek, BIND_DUCKPEEK)) {
        static bool flip = false;
        flip = !flip;
        if (flip) cmd->buttons |= IN_DUCK;
        else cmd->buttons &= ~IN_DUCK;
    }

    // Silent run (double duck): short crouch taps while moving on the ground,
    // resetting the footstep timer without the full crouch animation.
    if (cfg.x_silentrun && cfg.kb_silentrun != 0 && BindActive(cfg.kb_silentrun, cfg.kb_mode_silentrun, BIND_SILENTRUN) &&
        (flags & FL_ONGROUND) && speed > 30.0f) {
        static int tick = 0;
        if ((tick++ % 10) == 0) cmd->buttons |= IN_DUCK;
    }

    // Jump bug assist: duck right before touching the floor while falling.
    if (cfg.x_jumpbug && cfg.kb_jumpbug != 0 && BindActive(cfg.kb_jumpbug, cfg.kb_mode_jumpbug, BIND_JUMPBUG) &&
        !(flags & FL_ONGROUND) && vel.z < -200.0f) {
        Vector const origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
        float const probe = fabsf(vel.z) * 0.03f + 2.0f;
        float fraction = 1.0f;
        Vector end;
        if (TraceLine(origin, origin - Vector(0.0f, 0.0f, probe), fraction, end, local, nullptr) &&
            fraction < 0.99f)
            cmd->buttons |= IN_DUCK;
    }

    // Fast ladder: force forward + a strafe key while held (2x climb)
    if (cfg.x_fastladder && cfg.kb_fastladder != 0 && BindActive(cfg.kb_fastladder, cfg.kb_mode_fastladder, BIND_LADDER)) {
        cmd->buttons |= IN_FORWARD;
        cmd->buttons |= (cmd->buttons & IN_MOVELEFT) ? IN_MOVELEFT : IN_MOVERIGHT;
    }

    // Quick-switch reload cancel: fire lastinv twice on the key press edge
    if (cfg.x_qcancel) {
        static bool last = false;
        bool const now = cfg.kb_qcancel != 0 && BindActive(cfg.kb_qcancel, cfg.kb_mode_qcancel, BIND_QCANCEL);
        if (now && !last && engine.self)
            engine.ClientCmd("lastinv; lastinv");
        last = now;
    }
}

static void RunBhop(CUserCmd* cmd, void* local) {
    if (!cfg.bhop || !BindActive(cfg.kb_bhop, cfg.kb_mode_bhop, BIND_BHOP) || !cmd || !local) return;
    if (*reinterpret_cast<char*>((char*)local + off::m_lifeState) != 0) return;

    int flags = *reinterpret_cast<int*>((char*)local + off::m_fFlags);
    if (!(cmd->buttons & IN_JUMP)) return;

    // Clear jump while airborne and press it on the ground.  The 0->1 edge on
    // the landing tick is what makes the engine jump again; this is the
    // reliable CS:S auto-bhop and needs no state machine.
    if (flags & FL_ONGROUND)
        cmd->buttons |= IN_JUMP;
    else
        cmd->buttons &= ~IN_JUMP;
}

// Directional air control: 1:1 port of islamware's BigPolishAutostrafer
// "MultiDirectional" branch, adapted to this SDK's AngleVectors basis
// (forward = (cy, sy), right = (sy, -cy)).
//
// The wish direction is placed ~90 degrees away from the current velocity, on
// the side that rotates the velocity toward the input direction.  With no
// movement keys held the input direction is the camera forward, so a jump
// bends toward wherever the player is looking.  This is what gives full
// direction changes while bunnyhopping instead of a straight-line jump.
static void RunAutoStrafe(CUserCmd* cmd, void* local, const QAngle& camera) {
    bool auto_on = (cfg.autostrafe && BindActive(cfg.kb_autostrafe, cfg.kb_mode_autostrafe, BIND_AUTOSTRAFE)) ||
                   (cfg.bhop && BindActive(cfg.kb_bhop, cfg.kb_mode_bhop, BIND_BHOP));
    if (!auto_on || !cmd || !local) return;
    if (*reinterpret_cast<char*>((char*)local + off::m_lifeState) != 0) return;

    int flags = *reinterpret_cast<int*>((char*)local + off::m_fFlags);
    Vector vel = *reinterpret_cast<Vector*>((char*)local + off::m_vecVelocity);
    float speed2d = sqrtf(vel.x * vel.x + vel.y * vel.y);
    if (flags & FL_ONGROUND)
        return;

    // Below this vanilla air control can still build the initial speed.
    if (speed2d < 30.0f)
        return;

    static ConVar* cv_airaccel = nullptr;
    static ConVar* cv_maxspeed = nullptr;
    static bool cvars_ready = false;
    if (!cvars_ready) {
        cvars_ready = true;
        if (cvar.self) {
            cv_airaccel = cvar.FindVar("sv_airaccelerate");
            cv_maxspeed = cvar.FindVar("sv_maxspeed");
        }
    }
    float air_accel = cv_airaccel ? cv_airaccel->f_value : 10.0f;
    float max_speed = cv_maxspeed ? cv_maxspeed->f_value : 400.0f;
    if (air_accel <= 0.0f) air_accel = 10.0f;
    if (max_speed <= 0.0f) max_speed = 400.0f;

    // Optimal turn step: hardcoded 30 u/s air wishspeed in Source.
    const float pi = 3.14159265358979323846f;
    float best_delta = 0.0f;
    float term = 30.0f / air_accel / max_speed * 100.0f / speed2d;
    if (term < 1.0f && term > -1.0f)
        best_delta = acosf(term);
    // The optimal turn is ~90 degrees on any sane server.  If a cvar is
    // missing/bogus and the formula collapses, force it instead of silently
    // accelerating along the current velocity (which looks like "no turning").
    if (best_delta < 0.7854f || best_delta > 1.7453f)
        best_delta = 1.5675f; // 89.8 degrees

    // View-relative angles: positive = to the player's right.  The yaw is the
    // user camera captured before aimbot/silent aim/anti-aim touched cmd.
    const float yaw_rad = camera.y * (pi / 180.0f);
    const float vel_dir = yaw_rad - atan2f(vel.y, vel.x);
    // By default the strafer always steers toward the camera, regardless of
    // which movement keys are held.  The informant code used the held keys as
    // the target, which made A/D bunnyhopping circle instead of following the
    // view.  The optional mode restores that behavior.
    const float target_ang = cfg.strafe_follow_keys
        ? atan2f(cmd->sidemove, cmd->forwardmove)
        : 0.0f;

    float delta = vel_dir - target_ang;
    delta = atan2f( sinf( delta ), cosf( delta ) );

    // Steer the wish direction toward the requested direction.
    const float movedir = (delta < 0.0f) ? (vel_dir + best_delta) : (vel_dir - best_delta);

    cmd->forwardmove = cosf(movedir) * 450.0f;
    cmd->sidemove = sinf(movedir) * 450.0f;

    cmd->buttons &= ~(IN_MOVELEFT | IN_MOVERIGHT | IN_FORWARD | IN_BACK);
    if (cmd->sidemove <= 0.0f) cmd->buttons |= IN_MOVELEFT;
    else                       cmd->buttons |= IN_MOVERIGHT;
    if (cmd->forwardmove <= 0.0f) cmd->buttons |= IN_BACK;
    else                          cmd->buttons |= IN_FORWARD;

}

TYPEDEF_THISCALL(bool, ClientModeCreateMoveFn, void*, float, CUserCmd*);
static ClientModeCreateMoveFn oClientModeCreateMove = nullptr;

static QAngle g_menu_lock_angles;
static bool g_menu_was_open_cm = false;

static HOOK_THISCALL(bool, HookedClientModeCreateMove, void*, float frametime, CUserCmd* cmd) {
    UpdateThirdPerson();
    bool ret = oClientModeCreateMove ? oClientModeCreateMove(self, frametime, cmd) : true;
    if (cmd && cmd->command_number != 0) {
        // When the menu is open, freeze camera angles and block all background movement/shooting
        if (cfg.menu_open) {
            if (!g_menu_was_open_cm) {
                g_menu_lock_angles = real_view_angles;
                if (g_menu_lock_angles.x == 0.0f && g_menu_lock_angles.y == 0.0f)
                    g_menu_lock_angles = cmd->viewangles;
                g_menu_was_open_cm = true;
            }
            cmd->viewangles = g_menu_lock_angles;
            cmd->forwardmove = 0.0f;
            cmd->sidemove = 0.0f;
            cmd->upmove = 0.0f;
            cmd->mousedx = 0;
            cmd->mousedy = 0;
            cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2 | IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_DUCK);
            user_camera_angles = g_menu_lock_angles;
            real_view_angles = g_menu_lock_angles;
            if (engine.self) engine.SetViewAngles(g_menu_lock_angles);
            return false;
        }
        g_menu_was_open_cm = false;

        // User view for this tick, before aimbot / silent aim / anti-aim.
        user_camera_angles = cmd->viewangles;

        void* local = GetLocalEntity();
        if (local) RunBhop(cmd, local);

        SetStage("combat");
        RunCombat(cmd);
        real_view_angles = cmd->viewangles; // camera the renderer should keep

        SetStage("clientmode::CreateMove end");
    }
    return ret;
}

CUserCmd* GetUserCmd(int sequence) {
    void* input = GetInput();
    if (!input) return nullptr;
    return VFunc<CUserCmd*(THISCALL_CC*)(void*, int)>(input, INPUT_GET_USERCMD)(input, sequence);
}

// CInput keeps a second copy of the command at +0x110 (stride 0x50) in x64.
CUserCmd* GetUserCmdCopy(int sequence) {
#ifdef _WIN64
    void* input = GetInput();
    if (!input) return nullptr;
    void* base = *reinterpret_cast<void**>((char*)input + 0x110);
    if (!base) return nullptr;
    int idx = sequence % 90;
    CUserCmd* c = reinterpret_cast<CUserCmd*>((char*)base + static_cast<size_t>(idx) * 0x50);
    if (c->command_number != sequence) return nullptr;
    return c;
#else
    (void)sequence;
    return nullptr;
#endif
}

TYPEDEF_THISCALL(void, BaseCreateMoveFn, void*, int, float, bool);
static BaseCreateMoveFn oBaseCreateMove = nullptr;

TYPEDEF_THISCALL(void, FrameStageNotifyFn, void*, int);
static FrameStageNotifyFn oFrameStageNotify = nullptr;

static HOOK_THISCALL(void, HookedFrameStageNotify, void*, int stage) {
    ForceSvCheats();
    // Anti-aim / third-person run at stage 6 exactly like islamware-css-b
    // (its FRAME_RENDER_START constant was 6); custom model work uses 5/6.
    constexpr int AA_STAGE = 6;
    if (stage == AA_STAGE && cfg.aa_enabled) {
        void* local = GetLocalEntity();
        if (local && *reinterpret_cast<const char*>((const char*)local + off::m_lifeState) == 0) {
            *reinterpret_cast<QAngle*>((char*)local + OFF_PLAYER_VIEW_ANGLES) = aa_view_angles;
            *reinterpret_cast<QAngle*>((char*)local + off::m_angEyeAngles) = aa_view_angles;
        }
    }
    if (stage == AA_STAGE && LocalCanThirdPerson()) {
        void* input = GetInput();
        if (input) *reinterpret_cast<uint8_t*>((char*)input + INPUT_CAM_THIRD_PERSON) = 1;
        void* local = GetLocalEntity();
        if (local)
            *reinterpret_cast<int*>((char*)local + off::m_fEffects) &= ~EF_NODRAW;
    }

    static bool g_obs_mode_patched = false;
    if (stage == FRAME_RENDER_START) {
        if (LocalCanThirdPerson()) {
            void* local = GetLocalEntity();
            if (local)
                *reinterpret_cast<int*>((char*)local + off::m_fEffects) &= ~EF_NODRAW;
        }
        ClientModel_OnFrameStage(stage);
        if (cfg.thirdperson) {
            void* obs = GetSpectatedPlayer();
            if (obs) {
                *reinterpret_cast<int*>((char*)obs + off::m_fEffects) &= ~EF_NODRAW;
                void* local = GetLocalEntity();
                if (local && *reinterpret_cast<int*>((char*)local + off::m_iObserverMode) == 4 /* OBS_MODE_IN_EYE */) {
                    *reinterpret_cast<int*>((char*)local + off::m_iObserverMode) = 5; // OBS_MODE_CHASE so obs model draws
                    g_obs_mode_patched = true;
                }
            }
        }
    }
    if (oFrameStageNotify) oFrameStageNotify(self, stage);
    if (stage == FRAME_RENDER_START)
        Bones_OnFrameStage();
    if (stage == 4 /* FRAME_NET_UPDATE_POSTDATAUPDATE_END */ || stage == FRAME_RENDER_END)
        ClientModel_OnFrameStage(stage);
    if (stage == FRAME_RENDER_END && g_obs_mode_patched) {
        g_obs_mode_patched = false;
        void* local = GetLocalEntity();
        if (local && *reinterpret_cast<int*>((char*)local + off::m_iObserverMode) == 5)
            *reinterpret_cast<int*>((char*)local + off::m_iObserverMode) = 4;
    }
}

// Additional movement helpers applied to the final outgoing command.
static void RunExtraMovement(CUserCmd* cmd, void* local, const QAngle& camera) {
    if (!cmd || !local) return;
    if (*reinterpret_cast<char*>((char*)local + off::m_lifeState) != 0) return;

    int const flags = *reinterpret_cast<int*>((char*)local + off::m_fFlags);
    bool const on_ground = (flags & FL_ONGROUND) != 0;

    if (cfg.inf_duck)
        cmd->buttons |= IN_DUCK;
    else if (cfg.crouch_air && !on_ground)
        cmd->buttons |= IN_DUCK;
    if (cfg.fake_duck)
        cmd->buttons |= IN_DUCK;

    bool const moving = (cmd->buttons & (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT)) != 0;

    if (cfg.fastwalk > 1.001f && on_ground && moving) {
        cmd->forwardmove *= cfg.fastwalk;
        cmd->sidemove *= cfg.fastwalk;
    }

    if (cfg.quick_stop && on_ground && !moving) {
        Vector vel = *reinterpret_cast<Vector*>((char*)local + off::m_vecVelocity);
        float const speed = sqrtf(vel.x * vel.x + vel.y * vel.y);
        if (speed > 30.0f) {
            Vector fwd, right, up;
            AngleVectors(QAngle(0.0f, camera.y, 0.0f), fwd, right, up);
            float const fm = -(vel.x * fwd.x + vel.y * fwd.y);
            float const sm = -(vel.x * right.x + vel.y * right.y);
            float const len = sqrtf(fm * fm + sm * sm);
            if (len > 0.001f) {
                float const scale = (len > 450.0f) ? (450.0f / len) : 1.0f;
                cmd->forwardmove = fm * scale;
                cmd->sidemove = sm * scale;
            }
        }
    }
}

// insomniaV4 / islamware hook IBaseClientDLL::CreateMove (VClient017 vtable 18),
// call the original and then edit the command.  This is the only hook point
// whose changes are guaranteed to reach both prediction and the server encode.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// no spread: exact Source mathlib random stream + seed-based compensation
// ---------------------------------------------------------------------------
// Source's CUniformRandomStream (Park-Miller); the server seeds it with
// (cmd->random_seed & 255) + 1 before FireBullets, so the client can predict
// the spread vector exactly and counter-act it.
static int g_rand_idum = 0;

static void SrcRandomSeed(int seed) {
    g_rand_idum = (seed < 0) ? seed : -seed;
}

static int SrcRandomNumber() {
    int const k = g_rand_idum / 127773;
    int result = 16807 * (g_rand_idum - k * 127773) - 2836 * k;
    if (result < 0)
        result += 2147483647;
    g_rand_idum = result;
    return result;
}

static float SrcRandomFloat(float lo, float hi) {
    double const f = (double)SrcRandomNumber() / 2147483647.0;
    return (float)(f * (double)(hi - lo)) + lo;
}

static void RunNoSpread(CUserCmd* cmd, bool do_spread, bool do_recoil) {
    if ((!do_spread && !do_recoil) || !cmd || !(cmd->buttons & IN_ATTACK) || !IsGameReady())
        return;

    void* local = GetLocalEntity();
    if (!local) return;
    void* weapon = GetActiveWeapon(local);
    if (!weapon) return;

    QAngle shot = cmd->viewangles;

    if (do_spread) {
        float const flInaccuracy = WeaponGetInaccuracy(weapon);
        float const flSpread = WeaponGetSpread(weapon);
        if (flInaccuracy + flSpread > 1e-6f) {
            typedef void(__cdecl* RandomSeedFn)(int);
            typedef float(__cdecl* RandomFloatFn)(float, float);
            static RandomSeedFn vRandomSeed = nullptr;
            static RandomFloatFn vRandomFloat = nullptr;
            static bool vstdlib_checked = false;
            if (!vstdlib_checked) {
                vstdlib_checked = true;
                if (HMODULE vs = GetModuleHandleA("vstdlib.dll")) {
                    vRandomSeed = reinterpret_cast<RandomSeedFn>(GetProcAddress(vs, "RandomSeed"));
                    vRandomFloat = reinterpret_cast<RandomFloatFn>(GetProcAddress(vs, "RandomFloat"));
                }
            }

            int const seed = (cmd->random_seed & 255) + 1;
            float sx = 0.0f, sy = 0.0f;
#ifdef _WIN64
            float const two_pi = 6.283185307179586f;
            float a, b, c, d;
            if (vRandomSeed && vRandomFloat) {
                vRandomSeed(seed);
                a = vRandomFloat(0.0f, two_pi);
                b = vRandomFloat(0.0f, flInaccuracy);
                c = vRandomFloat(0.0f, two_pi);
                d = vRandomFloat(0.0f, flSpread);
            } else {
                SrcRandomSeed(seed);
                a = SrcRandomFloat(0.0f, two_pi);
                b = SrcRandomFloat(0.0f, flInaccuracy);
                c = SrcRandomFloat(0.0f, two_pi);
                d = SrcRandomFloat(0.0f, flSpread);
            }

            // x64 FX_FireBullets polar spread sum
            sx = cosf(a) * b + cosf(c) * d;
            sy = sinf(a) * b + sinf(c) * d;
#else
            // v34 FX_FireBullets triangular spread: RandomFloat(-0.5, 0.5) + RandomFloat(-0.5, 0.5)
            float x, y;
            if (vRandomSeed && vRandomFloat) {
                vRandomSeed(seed);
                x = vRandomFloat(-0.5f, 0.5f) + vRandomFloat(-0.5f, 0.5f);
                y = vRandomFloat(-0.5f, 0.5f) + vRandomFloat(-0.5f, 0.5f);
            } else {
                SrcRandomSeed(seed);
                x = SrcRandomFloat(-0.5f, 0.5f) + SrcRandomFloat(-0.5f, 0.5f);
                y = SrcRandomFloat(-0.5f, 0.5f) + SrcRandomFloat(-0.5f, 0.5f);
            }
            sx = x * flSpread;
            sy = y * flSpread;
#endif

            Vector desired_fwd;
            AngleVectors(cmd->viewangles, desired_fwd);
            for (int iter = 0; iter < 3; iter++) {
                Vector f, r, u;
                AngleVectors(shot, f, r, u);
                Vector corr = desired_fwd - r * sx - u * sy;
                float const len = sqrtf(corr.x * corr.x + corr.y * corr.y + corr.z * corr.z);
                if (len < 1e-6f) break;
                corr.x /= len; corr.y /= len; corr.z /= len;
                shot.x = -atan2f(corr.z, sqrtf(corr.x * corr.x + corr.y * corr.y)) * 57.29577951308232f;
                shot.y = atan2f(corr.y, corr.x) * 57.29577951308232f;
                shot.z = 0.0f;
            }
        }
    }

    if (do_recoil) {
        QAngle const punch = *reinterpret_cast<const QAngle*>((const char*)local + off::m_vecPunchAngle);
        shot.x -= punch.x * 2.0f;
        shot.y -= punch.y * 2.0f;
    }

    if (shot.x > 89.0f) shot.x = 89.0f;
    if (shot.x < -89.0f) shot.x = -89.0f;
    shot.y = NormalizeYaw(shot.y);
    shot.z = 0.0f;
    if (shot.x == shot.x && shot.y == shot.y)
        cmd->viewangles = shot;
}

// true for weapons that fire continuously while IN_ATTACK is held; double
// tap must leave those alone (the press-edge toggle would halve their rate)
static bool ActiveWeaponIsAutomatic(const char** out_cls = nullptr) {
    void* local = GetLocalEntity();
    if (!local) return false;
    int const handle = *reinterpret_cast<int*>((char*)local + off::m_hActiveWeapon);
    int const idx = handle & 0xFFF;
    if (idx <= 0) return false;
    void* nw = entity_list.GetClientNetworkable(idx);
    if (!nw) return false;
    void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
    const char* cls = ClientClassName(cc);
    if (out_cls) *out_cls = cls;
    if (!cls) return false;
    static const char* const autos[] = {
        "AK47", "M4A1", "Galil", "Famas", "SG552", "Aug", "MAC10", "TMP",
        "MP5", "UMP45", "P90", "M249", "Negev",
    };
    for (char const* a : autos)
        if (strstr(cls, a)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Peek assist (Quick Peek): places a circle at the start position and walks
// the player back to it on shot / movement-key release.
// ---------------------------------------------------------------------------
namespace {
    struct PeekAssistState {
        bool has_start = false;
        Vector start{};
        bool active_last = false;
        bool shot = false;
        bool release = false;
        bool holding = false;
        float retreat_time = 0.0f;
        float hold = 0.0f;
        Vector retreat_start{};
        Vector prev_origin{};
        CRITICAL_SECTION cs{};
        bool cs_init = false;
        bool draw_active = false;
        Vector draw_center{};
    } g_peek;

    static void PeekLock() {
        if (!g_peek.cs_init) {
            InitializeCriticalSection(&g_peek.cs);
            g_peek.cs_init = true;
        }
        EnterCriticalSection(&g_peek.cs);
    }
    static void PeekUnlock() { LeaveCriticalSection(&g_peek.cs); }

    static void PeekResetRetreat() {
        g_peek.shot = false;
        g_peek.release = false;
        g_peek.holding = false;
        g_peek.retreat_time = 0.0f;
        g_peek.hold = 0.0f;
    }

    static void PeekSetMoveAlongYaw(CUserCmd* cmd, const QAngle& camera, float wish_yaw, float speed) {
        float const dy = Deg2Rad(wish_yaw - camera.y);
        cmd->forwardmove = cosf(dy) * speed;
        cmd->sidemove = -sinf(dy) * speed;
    }

    static void PeekPlaceHome(void* local) {
        Vector const origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
        int const flags = *reinterpret_cast<int*>((char*)local + off::m_fFlags);
        if (flags & FL_ONGROUND) {
            g_peek.start = origin;
            g_peek.has_start = true;
            return;
        }
        float frac = 1.0f;
        Vector end;
        if (TraceLine(origin, Vector(origin.x, origin.y, origin.z - 2000.0f), frac, end, local, nullptr,
                      nullptr, nullptr, nullptr, 0x201400Bu) && frac < 1.0f) {
            g_peek.start = end;
            g_peek.has_start = true;
        }
    }

    static void PeekDrag(void* local, float max_dist) {
        Vector const origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
        Vector const offset = origin - g_peek.start;
        float const len = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
        float const scale = max_dist / (len + 1.1920929e-7f);
        Vector const cand(origin.x - offset.x * scale, origin.y - offset.y * scale,
                          (origin.z - offset.z * scale + 10.0f > origin.z) ? (origin.z - offset.z * scale + 10.0f) : origin.z);
        float frac = 1.0f;
        Vector end;
        bool startsolid = false;
        if (TraceLine(cand, Vector(cand.x, cand.y, cand.z - 16384.0f), frac, end, local, nullptr,
                      nullptr, nullptr, nullptr, 0x201400Bu, nullptr, nullptr, &startsolid) &&
            !startsolid && (!engine_trace.self || !(engine_trace.GetPointContents(end) & 1))) {
            g_peek.start = end;
        }
    }

    static void PeekBrake(CUserCmd* cmd, void* local, const QAngle& camera, float interval,
                          float sv_friction, float sv_stopspeed, float sv_accel) {
        Vector const vel = *reinterpret_cast<Vector*>((char*)local + off::m_vecVelocity);
        float const speed = sqrtf(vel.x * vel.x + vel.y * vel.y);
#ifdef _WIN64
        float const surf_fric = *reinterpret_cast<float*>((char*)local + 0x0304);
#else
        float const surf_fric = 1.0f;
#endif
        float const maxspeed = *reinterpret_cast<float*>((char*)local + off::m_flMaxspeed);
        float const fric = sv_friction * (surf_fric > 0.0f ? surf_fric : 1.0f);
        float const stop_ref = (speed > sv_stopspeed) ? speed : sv_stopspeed;
        if (speed - fric * stop_ref * interval <= 0.1f) {
            cmd->forwardmove = 0.0f;
            cmd->sidemove = 0.0f;
            return;
        }
        float const accel = sv_accel * (maxspeed > 1.0f ? maxspeed : 250.0f) * interval * (surf_fric > 0.0f ? surf_fric : 1.0f);
        float const fwd = (speed - accel > 0.1f) ? 450.0f : (speed / (accel > 1e-4f ? accel : 1.0f));
        float const brake_yaw = atan2f(-vel.y, -vel.x) * 57.29577951308232f;
        PeekSetMoveAlongYaw(cmd, camera, brake_yaw, fwd);
    }

    static int PeekSolve(void* local, const Vector& delta, const QAngle& camera,
                         float raw_fwd, float raw_side, float interval,
                         float sv_friction, float sv_stopspeed, float sv_accel) {
        int const tick_base = *reinterpret_cast<int*>((char*)local + off::m_nTickBase);
        float const now = (float)tick_base * interval;

        if (delta.x * delta.x + delta.y * delta.y <= 16.0f) {
            if (raw_fwd != 0.0f || raw_side != 0.0f) {
                Vector fwd, right, up;
                AngleVectors(camera, fwd, right, up);
                Vector const wish(fwd.x * raw_fwd + right.x * raw_side,
                                  fwd.y * raw_fwd + right.y * raw_side, 0.0f);
                Vector const peek(g_peek.retreat_start.x - g_peek.start.x,
                                  g_peek.retreat_start.y - g_peek.start.y, 0.0f);
                float const wlen = sqrtf(wish.x * wish.x + wish.y * wish.y);
                float const plen = sqrtf(peek.x * peek.x + peek.y * peek.y);
                float const cosine = (wish.x * peek.x + wish.y * peek.y) / (wlen * plen + 1.1920929e-7f);
                if (cosine < -0.34202015f)
                    return 2;
            }

            if (!g_peek.holding) {
                g_peek.holding = true;
                float elapsed = now - g_peek.retreat_time;
                if (elapsed < interval) elapsed = interval;
                if (elapsed > 2.0f) elapsed = 2.0f;
                void* wpn = GetActiveWeapon(local);
                float const next_pri = wpn ? *reinterpret_cast<float*>((char*)wpn + off::w_flNextPrimaryAttack) : 0.0f;
                float const next_atk = *reinterpret_cast<float*>((char*)local + off::m_flNextAttack);
                float ready = ((next_pri > next_atk) ? next_pri : next_atk) - now;
                if (ready < 0.0f) ready = 0.0f;
                if (ready > 2.0f) ready = 2.0f;
                float diff = ready - elapsed;
                if (diff < 0.0f) diff = 0.0f;
                if (diff > 2.0f) diff = 2.0f;
                g_peek.hold = diff + 0.2f;
            }

            if (g_peek.hold <= 0.0f)
                return 1;
            g_peek.hold -= interval;
            return (g_peek.hold <= 0.0f) ? 2 : 1;
        }

        if (g_peek.holding && g_peek.hold > 0.0f) {
            g_peek.hold -= interval;
            if (g_peek.hold <= 0.0f)
                return 2;
        }

        Vector const origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
        Vector const moved = origin - g_peek.prev_origin;
        float const mlen = sqrtf(moved.x * moved.x + moved.y * moved.y + moved.z * moved.z);
        Vector const dir(moved.x / (mlen + 1.1920929e-7f),
                         moved.y / (mlen + 1.1920929e-7f),
                         moved.z / (mlen + 1.1920929e-7f));
        float const home_dist = sqrtf((origin.x - g_peek.start.x) * (origin.x - g_peek.start.x) +
                                      (origin.y - g_peek.start.y) * (origin.y - g_peek.start.y));
#ifdef _WIN64
        float const surf_fric = *reinterpret_cast<float*>((char*)local + 0x0304);
#else
        float const surf_fric = 1.0f;
#endif
        float const maxspeed = *reinterpret_cast<float*>((char*)local + off::m_flMaxspeed);
        float const sf = (surf_fric > 0.0f) ? surf_fric : 1.0f;
        float const fric = sv_friction * sf;
        float const accel = sv_accel * (maxspeed > 1.0f ? maxspeed : 250.0f) * interval * sf;
        Vector const vel = *reinterpret_cast<Vector*>((char*)local + off::m_vecVelocity);
        float speed = sqrtf(vel.x * vel.x + vel.y * vel.y);
        Vector pos = origin;

        int const steps = (int)(0.25f / interval + 0.5f);
        for (int step = 0; step < steps; step++) {
            float const ref = (speed > sv_stopspeed) ? speed : sv_stopspeed;
            speed -= accel + ref * fric * interval;
            if (speed <= 0.0f)
                return 0;
            pos.x += dir.x * speed * interval;
            pos.y += dir.y * speed * interval;
            float const d_home = sqrtf((g_peek.start.x - pos.x) * (g_peek.start.x - pos.x) +
                                       (g_peek.start.y - pos.y) * (g_peek.start.y - pos.y));
            float const d_moved = sqrtf((origin.x - pos.x) * (origin.x - pos.x) +
                                        (origin.y - pos.y) * (origin.y - pos.y));
            if (d_home <= 0.1f || d_moved >= home_dist)
                return 1;
        }
        return 0;
    }
} // namespace

bool PeekAssist_GetCircle(Vector& out_center) {
    PeekLock();
    bool const ok = g_peek.draw_active;
    if (ok) out_center = g_peek.draw_center;
    PeekUnlock();
    return ok;
}

static void RunPeekAssist(CUserCmd* cmd, void* local, const QAngle& camera,
                          int raw_buttons, float raw_fwd, float raw_side, bool fired_shot) {
    bool const enabled = cfg.peek_assist && BindActive(cfg.kb_peek_assist, cfg.kb_mode_peek_assist, BIND_PEEK_ASSIST) && IsGameReady();
    if (!enabled || !local || !cmd || *reinterpret_cast<char*>((char*)local + off::m_lifeState) != 0) {
        g_peek.active_last = false;
        g_peek.has_start = false;
        PeekResetRetreat();
        PeekLock();
        g_peek.draw_active = false;
        PeekUnlock();
        return;
    }

    if (!g_peek.active_last) {
        g_peek.active_last = true;
        PeekResetRetreat();
        PeekPlaceHome(local);
        g_peek.prev_origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
    }

    if (!g_peek.has_start) {
        PeekPlaceHome(local);
        g_peek.prev_origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
        PeekLock();
        g_peek.draw_active = g_peek.has_start;
        g_peek.draw_center = g_peek.start;
        PeekUnlock();
        return;
    }

    static ConVar* cv_fric = nullptr;
    static ConVar* cv_stop = nullptr;
    static ConVar* cv_acc = nullptr;
    static bool cv_done = false;
    if (!cv_done && cvar.self) {
        cv_done = true;
        cv_fric = cvar.FindVar("sv_friction");
        cv_stop = cvar.FindVar("sv_stopspeed");
        cv_acc = cvar.FindVar("sv_accelerate");
    }
    float const sv_friction = cv_fric ? cv_fric->f_value : 4.0f;
    float const sv_stopspeed = cv_stop ? cv_stop->f_value : 75.0f;
    float const sv_accel = cv_acc ? cv_acc->f_value : 5.0f;
    float const interval = GetIntervalPerTick();

    Vector const origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
    int const flags = *reinterpret_cast<int*>((char*)local + off::m_fFlags);
    Vector const before = g_peek.start - origin;
    float const dist2d = sqrtf(before.x * before.x + before.y * before.y);

    if (flags & FL_ONGROUND) {
        if (cfg.peek_max_distance <= 0) {
            if (dist2d >= 800.0f)
                g_peek.start = origin;
        } else if (dist2d > (float)cfg.peek_max_distance) {
            PeekDrag(local, (float)cfg.peek_max_distance);
        }
    }

    int const tick_base = *reinterpret_cast<int*>((char*)local + off::m_nTickBase);
    float const now = (float)tick_base * interval;

    if (cfg.peek_retreat_on_shot && fired_shot) {
        g_peek.retreat_start = origin;
        if (!g_peek.shot)
            g_peek.retreat_time = now;
        g_peek.shot = true;
    }

    if (raw_buttons & (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP)) {
        g_peek.release = false;
    } else if (cfg.peek_retreat_on_release && (before.x * before.x + before.y * before.y > 1.0f)) {
        if (dist2d >= 400.0f)
            g_peek.start = origin;
        if (!g_peek.release) {
            g_peek.retreat_time = now;
            g_peek.retreat_start = origin;
        }
        g_peek.release = true;
    }

    if (g_peek.shot || g_peek.release) {
        Vector const delta = g_peek.start - origin;
        int const res = PeekSolve(local, delta, camera, raw_fwd, raw_side, interval,
                                  sv_friction, sv_stopspeed, sv_accel);
        if (res == 2) {
            PeekResetRetreat();
        } else {
            constexpr int IN_SPEED_BIT = (1 << 17);
            cmd->buttons &= ~(IN_SPEED_BIT | IN_JUMP);
            if (res == 0) {
                float const wish_yaw = atan2f(delta.y, delta.x) * 57.29577951308232f;
                PeekSetMoveAlongYaw(cmd, camera, wish_yaw, 450.0f);
            } else {
                PeekBrake(cmd, local, camera, interval, sv_friction, sv_stopspeed, sv_accel);
            }
        }
    } else {
        PeekResetRetreat();
    }

    g_peek.prev_origin = origin;
    PeekLock();
    g_peek.draw_active = g_peek.has_start;
    g_peek.draw_center = g_peek.start;
    PeekUnlock();
}

static HOOK_THISCALL(void, HookedBaseCreateMove, void*, int sequence_number, float frametime, bool active) {
    g_wd_cmd_tick = (LONG)GetTickCount();
    // previous command was just predicted - if its bullet consumed a round,
    // drop the client-side impact marker for it
    Impacts_TrackShot();
    bool const shot_fired_clip = Impacts_ConsumeFired();
    if (oBaseCreateMove) oBaseCreateMove(self, sequence_number, frametime, active);

    CUserCmd* cmd = GetUserCmd(sequence_number);
    if (!cmd || cmd->command_number == 0) return;

    if (cfg.menu_open) {
        cmd->viewangles = g_menu_lock_angles;
        cmd->forwardmove = 0.0f;
        cmd->sidemove = 0.0f;
        cmd->upmove = 0.0f;
        cmd->mousedx = 0;
        cmd->mousedy = 0;
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2 | IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_DUCK);
        if (engine.self) engine.SetViewAngles(g_menu_lock_angles);
        CUserCmd* copy = GetUserCmdCopy(sequence_number);
        if (copy) {
            copy->viewangles = cmd->viewangles;
            copy->forwardmove = 0.0f;
            copy->sidemove = 0.0f;
            copy->buttons = cmd->buttons;
        }
        return;
    }

    // CInput::CreateMove rewrites forwardmove/sidemove after ClientMode, so the
    // air strafer has to modify the command here.  The camera snapshot and the
    // combat pass were already done in the early (ClientMode) hook.
    int const raw_buttons = cmd->buttons;
    float const raw_forward = cmd->forwardmove;
    float const raw_side = cmd->sidemove;

    void* local = GetLocalEntity();
    if (local) RunBhop(cmd, local);
    if (LocalCanThirdPerson()) {
        void* inp = GetInput();
        if (inp) *reinterpret_cast<uint8_t*>((char*)inp + INPUT_CAM_THIRD_PERSON) = 1;
    }
    if (local) RunAutoStrafe(cmd, local, user_camera_angles);
    RunExploits(cmd, local);

    // Double tap: semi-auto/bolt weapons only fire on a fresh IN_ATTACK press
    // edge, so holding the button never repeats.  While attack is held, clear
    // the bit every other tick: the engine then sees press/release/press and
    // fires again as soon as the weapon cooldown is over, i.e. the pistol or
    // sniper fires at its maximum rate without re-clicking.  Automatic
    // weapons already fire while held and are left untouched.  Runs BEFORE
    // silent aim so the extra press ticks get the silent-aim angle.
    if (cfg.double_tap && IsGameReady()) {
        static int dt_auto_tick = 0;
        static bool dt_auto = false;
        static bool dt_clear = false;
        if (cmd->buttons & IN_ATTACK) {
            if ((dt_auto_tick++ & 7) == 0)
                dt_auto = ActiveWeaponIsAutomatic();
            if (!dt_auto) {
                if (dt_clear)
                    cmd->buttons &= ~IN_ATTACK;
                dt_clear = !dt_clear;
            }
        } else {
            dt_clear = false;
        }
    }

    // Silent aim must write into the command the engine actually sends.  In
    // ClientMode::CreateMove the angle was being lost before the outgoing
    // copy was built, so the server kept firing along the camera/anti-aim
    // angle - every shot missed.  Apply it here, on the outgoing cmd.
    SetStage("silentaim");
    bool const silent_applied = RunSilentAim(cmd, real_view_angles);

    // knifebot swing direction has to be on the command the engine sends
    bool const knife_applied = (cmd->buttons & IN_ATTACK) && g_knife_swing &&
        GetTickCount() - g_knife_time < 250;
    if (knife_applied)
        cmd->viewangles = g_knife_angle;

    // Anti-aim must never overwrite a shot: if the fake yaw reaches the
    // server on an attack tick, the bullet goes where the fake body looks.
    bool const attacking_now = (cmd->buttons & IN_ATTACK) != 0 && WeaponCanFire(local);
    if (cfg.aa_enabled && !attacking_now && !knife_applied)
        RunAntiAim(cmd, real_view_angles);

    RunExtraMovement(cmd, local, user_camera_angles);
    RunPeekAssist(cmd, local, user_camera_angles, raw_buttons, raw_forward, raw_side,
                  attacking_now || silent_applied || shot_fired_clip);
    FixMovement(cmd, user_camera_angles);

    // seed-based spread & recoil compensation (after silent aim sets the shot angle)
    if (attacking_now && !knife_applied)
        RunNoSpread(cmd, cfg.nospread, silent_applied || cfg.nospread);

    // The engine sends the +0x110 command copy, not the +0x108 array returned
    // by GetUserCmd, so mirror the final command into it.
    CUserCmd* copy = GetUserCmdCopy(sequence_number);
    if (copy) {
        copy->tick_count = cmd->tick_count;
        copy->viewangles = cmd->viewangles;
        copy->forwardmove = cmd->forwardmove;
        copy->sidemove = cmd->sidemove;
        copy->buttons = cmd->buttons;
    }
    Impacts_RecordShot(cmd, local);
    SetStage("baseclient::CreateMove end");
}

HOOK_THISCALL(void, HookedOverrideView, void*, CViewSetup* setup) {
    if (oOverrideView) oOverrideView(self, setup);
    if (!setup) return;

    if (cfg.fov_override >= 1.0f)
        setup->fov = (cfg.fov_override < 20.0f) ? 20.0f : (cfg.fov_override > 160.0f ? 160.0f : cfg.fov_override);
    if (cfg.viewmodel_fov >= 1.0f)
        setup->fov_viewmodel = (cfg.viewmodel_fov < 20.0f) ? 20.0f : (cfg.viewmodel_fov > 160.0f ? 160.0f : cfg.viewmodel_fov);

    void* local_ov = GetLocalEntity();
    bool const local_alive_ov = local_ov &&
        *reinterpret_cast<const char*>((const char*)local_ov + off::m_lifeState) == 0 &&
        *reinterpret_cast<const int*>((const char*)local_ov + off::m_iObserverMode) == 0;

    if ((cfg.aa_enabled || cfg.silentaim) && local_alive_ov) {
        // engine.dll keeps the view angles in a global (rva 0x53E4E4) that
        // CInput uses as next tick's mouse base.  The fake cmd angles were just
        // copied into it; restore the real view so the camera cannot drift or
        // rotate from silent aim / anti-aim.
        // Keep the rendered camera real and stop the faked command angles from
        // becoming next tick's mouse base.  Do NOT touch pl.v_angle: the game
        // uses it for the local shot, and the renderer is already overridden.
        if (engine.self) engine.SetViewAngles(real_view_angles);
        setup->angles = real_view_angles;

        if (cfg.aa_enabled) {
            // Restored from islamware-css-b: GetRenderAngles returns pl.v_angle
            // for the local player, so this shows the anti-aim on the body.
            void* local = GetLocalEntity();
            if (local) {
                *reinterpret_cast<QAngle*>((char*)local + OFF_PLAYER_VIEW_ANGLES) = aa_view_angles;
                *reinterpret_cast<QAngle*>((char*)local + off::m_angEyeAngles) = aa_view_angles; // m_angEyeAngles
            }
        }
    }

    static float tp_smooth = -1.0f;
    void* tp_target = nullptr;
    bool tp_local = false;
    if (LocalCanThirdPerson()) {
        // The engine resets the third-person flag when sv_cheats is 0, which
        // makes it cull the local player model.  Set it again here, right
        // before the entity draw list is built.
        void* inp = GetInput();
        if (inp) *reinterpret_cast<uint8_t*>((char*)inp + INPUT_CAM_THIRD_PERSON) = 1;
        tp_target = GetLocalEntity();
        tp_local = true;
    } else if (cfg.thirdperson) {
        tp_target = GetSpectatedPlayer();
    }

    if (tp_target) {
        if (tp_local) {
            int* effects = reinterpret_cast<int*>((char*)tp_target + off::m_fEffects);
            *effects &= ~EF_NODRAW;
        }
        Vector eye = PlayerEye(tp_target);
        Vector fwd, right, up;
        AngleVectors(setup->angles, fwd, right, up);

        float d = cfg.thirdperson_dist;
        if (d < 30.0f) d = 30.0f;
        if (d > 500.0f) d = 500.0f;

        // Pull the camera in if the desired position is inside a wall.
        Vector desired = eye - fwd * d + up * 6.0f;
        float frac;
        Vector hit;
        if (TraceLine(eye, desired, frac, hit, tp_target)) {
            float maxd = d * (frac - 0.03f);
            if (maxd < 5.0f) maxd = 5.0f;
            if (maxd < d) d = maxd;
        }

        // Smooth distance changes; snap inward, ease outward.
        float dt = ImGui::GetIO().DeltaTime;
        if (dt <= 0.0f || dt > 0.05f) dt = 1.0f / 60.0f;
        if (tp_smooth < 0.0f || d < tp_smooth) tp_smooth = d;
        else tp_smooth += (d - tp_smooth) * (1.0f - expf(-8.0f * dt));

        setup->origin = eye - fwd * tp_smooth + up * 6.0f;
    } else {
        tp_smooth = -1.0f;
    }

    if (cfg.fake_duck && local_alive_ov)
        setup->origin.z += 36.0f; // server sees crouched, camera stays standing

    view_origin = setup->origin;
    view_angles = setup->angles;
    if (setup->fov > 1.0f && setup->fov < 179.0f) view_fov = setup->fov;
#ifdef _WIN64
    if (setup->aspect > 0.1f && setup->aspect < 10.0f) view_aspect = setup->aspect;
#else
    if (engine.self) {
        int sw = 0, sh = 0;
        engine.GetScreenSize(sw, sh);
        if (sw > 0 && sh > 0) view_aspect = (float)sw / (float)sh;
    }
#endif
    view_valid = true;
}

HOOK_THISCALL(bool, HookedShouldDrawLocalPlayer, void*, void* player) {
    if (LocalCanThirdPerson()) return true;
    return oShouldDrawLocalPlayer ? oShouldDrawLocalPlayer(self, player) : false;
}

HOOK_THISCALL(bool, HookedShouldDrawViewModel, void*) {
    if (LocalCanThirdPerson() || (cfg.thirdperson && GetSpectatedPlayer() != nullptr))
        return false; // no floating hands in third person (alive or spectating)
    return oShouldDrawViewModel ? oShouldDrawViewModel(self) : true;
}

// ---------------------------------------------------------------------------
// chams
// ---------------------------------------------------------------------------
// IVModelRender vtable index 19 is DrawModelExecute(this, state, info, bones)
// on x64, and DrawModelEx(this, info) on v34.
#ifdef _WIN64
typedef int(__fastcall* DrawModelExecuteFn)(void*, void*, ModelRenderInfo_t*, void*);
static DrawModelExecuteFn oDrawModelExecute = nullptr;
static inline int CallOrigDrawModel(void* self, void* state, ModelRenderInfo_t* info, void* bones) {
    return oDrawModelExecute ? oDrawModelExecute(self, state, info, bones) : 0;
}
#else
typedef int(__thiscall* DrawModelExecuteFn)(void*, ModelRenderInfo_t*);
static DrawModelExecuteFn oDrawModelExecute = nullptr;
static inline int CallOrigDrawModel(void* self, void*, ModelRenderInfo_t* info, void*) {
    return oDrawModelExecute ? oDrawModelExecute(self, info) : 0;
}
#endif

constexpr int CHAMS_TYPE_COUNT = 7;
static void* g_chams_materials[2][CHAMS_TYPE_COUNT] = {};

static const char* kChamsBodies[CHAMS_TYPE_COUNT] = {
    // 0 regular: stock lit material, no styling
    "\"VertexLitGeneric\"\n{\n"
    "    \"$basetexture\" \"lights/white\"\n"
    "    \"$model\" 1\n"
    "    \"$nofog\" 1\n"
    "    \"$nocull\" 1\n}\n",
    // 1 flat
    "\"UnlitGeneric\"\n{\n"
    "    \"$basetexture\" \"lights/white\"\n"
    "    \"$model\" 1\n"
    "    \"$flat\" 1\n"
    "    \"$nofog\" 1\n"
    "    \"$nocull\" 1\n}\n",
    // 2 shaded: reference phong setup
    "\"VertexLitGeneric\"\n{\n"
    "    \"$basetexture\" \"lights/white\"\n"
    "    \"$model\" 1\n"
    "    \"$nofog\" 1\n"
    "    \"$phong\" 1\n"
    "    \"$basemapalphaphongmask\" 1\n"
    "    \"$phongboost\" 1\n"
    "    \"$phongexponent\" 15\n"
    "    \"$phongfresnelranges\" \"[1 0 0]\"\n"
    "    \"$phongtint\" \"[1 1 1]\"\n"
    "    \"$nocull\" 1\n}\n",
    // 3 metallic: envmap reflection
    "\"VertexLitGeneric\"\n{\n"
    "    \"$basetexture\" \"lights/white\"\n"
    "    \"$model\" 1\n"
    "    \"$nofog\" 1\n"
    "    \"$phong\" 1\n"
    "    \"$basemapalphaphongmask\" 1\n"
    "    \"$phongboost\" 0\n"
    "    \"$envmap\" \"env_cubemap\"\n"
    "    \"$envmapcontrast\" 0\n"
    "    \"$envmapfresnel\" 0\n"
    "    \"$envmaptint\" \"[1 1 1]\"\n"
    "    \"$nocull\" 1\n}\n",
    // 4 glow: additive fresnel shell
    "\"VertexLitGeneric\"\n{\n"
    "    \"$basetexture\" \"lights/white\"\n"
    "    \"$model\" 1\n"
    "    \"$nofog\" 1\n"
    "    \"$additive\" 1\n"
    "    \"$envmap\" \"models/effects/cube_white\"\n"
    "    \"$envmapfresnel\" 1\n"
    "    \"$envmapfresnelminmaxexp\" \"[0 1 2]\"\n"
    "    \"$envmaptint\" \"[1 1 1]\"\n"
    "    \"$nocull\" 1\n}\n",
    // 5 glass: additive translucent fresnel
    "\"VertexLitGeneric\"\n{\n"
    "    \"$basetexture\" \"lights/white\"\n"
    "    \"$model\" 1\n"
    "    \"$nofog\" 1\n"
    "    \"$additive\" 1\n"
    "    \"$translucent\" 1\n"
    "    \"$nocull\" 1\n"
    "    \"$envmap\" \"models/effects/cube_white\"\n"
    "    \"$envmapfresnel\" 1\n"
    "    \"$envmapfresnelminmaxexp\" \"[1 0 0.75]\"\n"
    "    \"$envmaptint\" \"[1 1 1]\"\n}\n",
    // 6 wireframe
    "\"UnlitGeneric\"\n{\n"
    "    \"$basetexture\" \"lights/white\"\n"
    "    \"$model\" 1\n"
    "    \"$wireframe\" 1\n"
    "    \"$nofog\" 1\n"
    "    \"$nocull\" 1\n}\n",
};

// The stock debug materials have no $basetexture in this build, so chams
// showed the pink/black missing-texture checkerboard.  Write real VMTs and
// load them through FindMaterial.  The materials must be reference-counted or
// the material system unloads them and binds the error checkerboard instead.
static void WriteChamsVmts() {
    char exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return;
    char* slash = strrchr(exe, '\\');
    if (!slash) return;
    *slash = '\0';

    char mats[MAX_PATH];
    snprintf(mats, sizeof(mats), "%s\\cstrike\\materials", exe);
    CreateDirectoryA(mats, nullptr);

    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\cstrike\\materials\\islamware", exe);
    CreateDirectoryA(dir, nullptr);

    // additive white shell used by the glow halo (tinted via ColorModulate)
    {
        char path[MAX_PATH];
        for (int z = 0; z < 2; z++) {
            snprintf(path, sizeof(path), "%s\\%s.vmt", dir, z ? "glowshellz" : "glowshell");
            FILE* f = fopen(path, "wb");
            if (f) {
                fprintf(f,
                    "\"UnlitGeneric\"\n{\n"
                    "    \"$basetexture\" \"lights/white\"\n"
                    "    \"$model\" 1\n"
                    "    \"$additive\" 1\n"
                    "    \"$nocull\" 1\n"
                    "    \"$nofog\" 1\n"
                    "    \"%s\n}\n",
                    z ? "\"$ignorez\" 1\n" : "");
                fclose(f);
            }
        }
    }

    for (int type = 0; type < CHAMS_TYPE_COUNT; type++) {
        const char* body = kChamsBodies[type];
        std::string body_str(body);
#ifndef _WIN64
        // Source 2006 (v34) uses vgui/white_additive (lights/white & cube_white don't exist)
        // and env_cubemap instead of $phong / $envmapfresnel.
        if (type == 3) {
            body_str =
                "\"VertexLitGeneric\"\n{\n"
                "    \"$basetexture\" \"vgui/white_additive\"\n"
                "    \"$model\" 1\n"
                "    \"$nofog\" 1\n"
                "    \"$envmap\" \"env_cubemap\"\n"
                "    \"$envmaptint\" \"[0.85 0.85 0.85]\"\n"
                "    \"$envmapcontrast\" 1\n"
                "    \"$nocull\" 1\n}\n";
        } else if (type == 4) {
            body_str =
                "\"UnlitGeneric\"\n{\n"
                "    \"$basetexture\" \"vgui/white_additive\"\n"
                "    \"$model\" 1\n"
                "    \"$nofog\" 1\n"
                "    \"$additive\" 1\n"
                "    \"$nocull\" 1\n}\n";
        } else if (type == 5) {
            body_str =
                "\"VertexLitGeneric\"\n{\n"
                "    \"$basetexture\" \"vgui/white_additive\"\n"
                "    \"$model\" 1\n"
                "    \"$nofog\" 1\n"
                "    \"$translucent\" 1\n"
                "    \"$alpha\" 0.45\n"
                "    \"$envmap\" \"env_cubemap\"\n"
                "    \"$envmaptint\" \"[0.7 0.85 1.0]\"\n"
                "    \"$nocull\" 1\n}\n";
        } else {
            for (size_t pos = 0; (pos = body_str.find("lights/white", pos)) != std::string::npos; ) {
                body_str.replace(pos, 12, "vgui/white_additive");
                pos += 19;
            }
        }
#endif
        size_t len = body_str.size();

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\chams%d.vmt", dir, type);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(body_str.data(), 1, len, f); fclose(f); }

        // Same material with $ignorez 1 compiled in, for through-wall chams.
        snprintf(path, sizeof(path), "%s\\chams%dz.vmt", dir, type);
        f = fopen(path, "wb");
        if (f) {
            size_t head = len;
            while (head > 0 && (body_str[head - 1] == '\n' || body_str[head - 1] == ' ')) head--;
            if (head > 0 && body_str[head - 1] == '}') head--;
            fwrite(body_str.data(), 1, head, f);
            const char* tail = "    \"$ignorez\" 1\n}\n";
            fwrite(tail, 1, strlen(tail), f);
            fclose(f);
        }
    }
}

static void* GetChamsMaterial(int type, bool ignorez, const float* glow_color = nullptr) {
    if (!material_system.self) return nullptr;
    if (type < 0 || type >= CHAMS_TYPE_COUNT) type = 0;
    int i = ignorez ? 1 : 0;

    // Glow is colour-baked: the rim comes from $envmaptint, so each colour gets
    // its own tiny VMT (cached).  Without this the additive pass renders a
    // white/black silhouette instead of a coloured glow.
    if (type == 4 && glow_color) {
        static void* glow_mat[2] = { nullptr, nullptr };
        static float glow_cached[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };
        if (glow_mat[i] && glow_cached[i][0] == glow_color[0] &&
            glow_cached[i][1] == glow_color[1] && glow_cached[i][2] == glow_color[2])
            return glow_mat[i];

        char exe[MAX_PATH] = { 0 };
        if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return nullptr;
        char* slash = strrchr(exe, '\\');
        if (!slash) return nullptr;
        *slash = '\0';
        char mats[MAX_PATH], dir[MAX_PATH];
        snprintf(mats, sizeof(mats), "%s\\cstrike\\materials", exe);
        snprintf(dir, sizeof(dir), "%s\\cstrike\\materials\\islamware", exe);
        CreateDirectoryA(mats, nullptr);
        CreateDirectoryA(dir, nullptr);

        int rr = (int)(glow_color[0] * 255.0f), gg = (int)(glow_color[1] * 255.0f), bb = (int)(glow_color[2] * 255.0f);
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\glow_%d_%d_%d%s.vmt", dir, rr, gg, bb, ignorez ? "z" : "");
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f,
                "\"VertexLitGeneric\"\n{\n"
                "    \"$basetexture\" \"lights/white\"\n"
                "    \"$model\" 1\n"
                "    \"$additive\" 1\n"
                "    \"$envmap\" \"models/effects/cube_white\"\n"
                "    \"$envmapfresnel\" 1\n"
                "    \"$envmapfresnelminmaxexp\" \"[0 1 2]\"\n"
                "    \"$envmaptint\" \"[%g %g %g]\"\n"
                "    \"$nocull\" 1\n"
                "    \"$nofog\" 1\n"
                "%s}\n",
                glow_color[0], glow_color[1], glow_color[2],
                ignorez ? "    \"$ignorez\" 1\n" : "");
            fclose(f);
        }

        char name[96];
        snprintf(name, sizeof(name), "islamware/glow_%d_%d_%d%s", rr, gg, bb, ignorez ? "z" : "");
        void* glow_material = material_system.FindMaterial(name, "Model textures", false);
        if (glow_material) {
            IMaterial m; m.self = glow_material;
            m.IncrementReferenceCount();
        }
        glow_mat[i] = glow_material;
        glow_cached[i][0] = glow_color[0];
        glow_cached[i][1] = glow_color[1];
        glow_cached[i][2] = glow_color[2];
        return glow_material;
    }

    static bool s_chams_failed[2][CHAMS_TYPE_COUNT] = {};
    if (g_chams_materials[i][type]) return g_chams_materials[i][type];
    if (s_chams_failed[i][type]) return nullptr;

    static bool s_vmts_written = false;
    if (!s_vmts_written) {
        s_vmts_written = true;
        WriteChamsVmts();
    }

    char name[64];
    snprintf(name, sizeof(name), ignorez ? "islamware/chams%dz" : "islamware/chams%d", type);
    void* material = material_system.FindMaterial(name, "Model textures", false);

    if (!material) {
        static const char* fallbacks[] = { "vgui/white_additive", "effects/flashbang_white" };
        for (const char* candidate : fallbacks) {
            material = material_system.FindMaterial(candidate, "Model textures", false);
            if (material) break;
        }
    }

    if (material) {
        IMaterial m; m.self = material;
        m.IncrementReferenceCount();
        Log("chams: type %d ignorez=%d material %p name=%s", type, ignorez ? 1 : 0,
            material, m.GetName() ? m.GetName() : "?");
    } else {
        s_chams_failed[i][type] = true;
        Log("chams: type %d ignorez=%d material not found", type, ignorez ? 1 : 0);
    }
    g_chams_materials[i][type] = material;
    return material;
}

constexpr int MATERIAL_VAR_WIREFRAME_FLAG = (1 << 28);

static void ApplyChamsMaterial( void* material, const float color[4], bool ignorez, bool glow = false, bool wireframe = false ) {
    IMaterial mat;
    mat.self = material;
    float alpha = color[3];
    if ( alpha < 0.02f ) alpha = 0.02f;
    if ( alpha > 1.0f ) alpha = 1.0f;

    if ( glow ) {
        // kill the base texture contribution; the tinted envmap fresnel rim is
        // the glow itself
        mat.ColorModulate( 0.0f, 0.0f, 0.0f );
        if ( render_view.self ) {
            static const float black[3] = { 0.0f, 0.0f, 0.0f };
            render_view.SetColorModulation( black );
            render_view.SetBlend( alpha );
        }
    } else {
        mat.ColorModulate( color[0], color[1], color[2] );
        if ( render_view.self ) {
            render_view.SetColorModulation( color );
            render_view.SetBlend( alpha );
        }
    }

    mat.AlphaModulate( alpha );
    mat.SetMaterialVarFlag( MATERIAL_VAR_IGNOREZ, ignorez );
    mat.SetMaterialVarFlag( MATERIAL_VAR_WIREFRAME_FLAG, wireframe );
    model_render.ForcedMaterialOverride( material );
}

static void ResetChamsMaterial( ) {
    model_render.ForcedMaterialOverride( nullptr );
    static const float white[3] = { 1.0f, 1.0f, 1.0f };
    if ( render_view.self ) {
        render_view.SetColorModulation( white );
        // the blend was left at the chams alpha before this fix, which made
        // every subsequent engine draw (including other models) render with
        // the chams transparency - looking like misplaced/ghosted models
        render_view.SetBlend( 1.0f );
    }
}

static DWORD g_hit_time[65] = {};
static int g_last_hp[65] = {};

// Glow shell (glow-src style): the bone matrices are inflated around the
// entity origin so the additive material draws a halo around the silhouette.
static void BuildScaledBones(const matrix3x4_t* bones, const Vector& origin,
                             float scale, matrix3x4_t* out) {
    for (int i = 0; i < 128; i++) {
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                out[i].m[r][c] = bones[i].m[r][c] * scale;
        Vector const t(bones[i].m[0][3], bones[i].m[1][3], bones[i].m[2][3]);
        out[i].m[0][3] = origin.x + (t.x - origin.x) * scale;
        out[i].m[1][3] = origin.y + (t.y - origin.y) * scale;
        out[i].m[2][3] = origin.z + (t.z - origin.z) * scale;
    }
}

static void ApplyShellMaterial(void* material, const float color[4], bool ignorez, float alpha_scale = 1.0f) {
    IMaterial mat;
    mat.self = material;
    float alpha = color[3];
    if (alpha < 0.05f) alpha = 0.05f;
    if (alpha > 1.0f) alpha = 1.0f;
    alpha *= alpha_scale;
    mat.ColorModulate(color[0], color[1], color[2]);
    mat.AlphaModulate(alpha);
    mat.SetMaterialVarFlag(MATERIAL_VAR_IGNOREZ, ignorez);
    if (render_view.self)
        render_view.SetBlend(alpha);
    model_render.ForcedMaterialOverride(material);
}

// additive white shell material (tinted per draw via ColorModulate)
static void* GetGlowShellMaterial(bool ignorez) {
    static void* mats[2] = {};
    int const i = ignorez ? 1 : 0;
    if (mats[i]) return mats[i];
    WriteChamsVmts();
    char name[64];
    snprintf(name, sizeof(name), "%s", ignorez ? "islamware/glowshellz" : "islamware/glowshell");
    void* material = material_system.FindMaterial(name, "Model textures", false);
    if (!material)
        material = material_system.FindMaterial(name, "Model textures", true);
    if (material) {
        IMaterial m;
        m.self = material;
        m.IncrementReferenceCount();
    }
    mats[i] = material;
    return mats[i];
}

static bool HitChamsActive( int idx ) {
    if ( !cfg.chams_hit || idx < 1 || idx > 64 || !g_hit_time[idx] ) return false;
    DWORD const dur = (DWORD)( cfg.chams_hit_duration * 1000.0f );
    return GetTickCount( ) - g_hit_time[idx] < dur;
}

static void UpdateHitChams( ) {
    if ( !cfg.chams_hit ) return;

    DWORD const now = GetTickCount( );

    for ( int i = 1; i <= 64; i++ ) {
        void* ent = entity_list.GetClientEntity( i );
        if ( !ent || !IsPlayerIndex( i ) ) continue;
        int hp = *reinterpret_cast<int*>( (char*)ent + off::m_iHealth );

        if ( hp > 0 && g_last_hp[i] > 0 && hp < g_last_hp[i] )
            g_hit_time[i] = now;

        // kill effects are spawned from the "player_death" game event in
        // game_events.cpp (attacker == local player), which is reliable

        g_last_hp[i] = hp > 0 ? hp : 0;
    }
}

static int DrawModelExecuteInner( void* self, void* state, ModelRenderInfo_t* info, void* bones );

#ifdef _WIN64
int __fastcall HookedDrawModelExecute( void* self, void* state, ModelRenderInfo_t* info, void* bones ) {
#else
int __fastcall HookedDrawModelExecute( void* self, void* /*edx*/, ModelRenderInfo_t* info ) {
    void* state = nullptr;
    void* bones = nullptr;
#endif
    if ( !info )
        return CallOrigDrawModel( self, state, info, bones );

    void* bones_use = bones;
    const char* info_model = info->pModel ? model_info.GetModelName(info->pModel) : nullptr;
    auto is_vm_model = [&](const char* m) -> bool {
        return m && (strstr(m, "/v_") || strstr(m, "\\v_"));
    };

    // Viewmodel position offset: the mesh is drawn through the bone matrices,
    // so shifting info->origin alone did nothing - translate the bones too.
    if ( IsGameReady( ) && bones &&
         ( cfg.viewmodel_x != 0.0f || cfg.viewmodel_y != 0.0f || cfg.viewmodel_z != 0.0f ) ) {
        void* local = GetLocalEntity( );
        if ( local ) {
            int const handle = *reinterpret_cast<int*>( (char*)local + off::m_hViewModel );
            int const vm_index = handle & 0xFFF;
            if ( (vm_index > 0 && info->entity_index == vm_index) || is_vm_model(info_model) ) {
                // copy only the entity's real bone count - blindly copying 128
                // entries ran past the engine's bone allocation and crashed.
                // The count comes from the renderable's cached field, NOT from
                // GetStudiomodel: the MDL cache (datacache) is main-thread only
                // and crashed when touched from the render thread.
                int bone_count = 128;
#ifdef _WIN64
                if ( info->pRenderable ) {
                    int const cached = *reinterpret_cast< int const* >( ( char* )info->pRenderable + 0xB50 );
                    if ( cached > 0 && cached <= 128 )
                        bone_count = cached;
                }
#endif

                Vector fwd, right, up;
                AngleVectors( view_angles, fwd, right, up );
                Vector const offset = right * cfg.viewmodel_x + up * cfg.viewmodel_y + fwd * cfg.viewmodel_z;

                static matrix3x4_t shifted[128];
                matrix3x4_t const* src = ( matrix3x4_t const* )bones;
                for ( int i = 0; i < bone_count; i++ ) {
                    shifted[i] = src[i];
                    shifted[i].m[0][3] += offset.x;
                    shifted[i].m[1][3] += offset.y;
                    shifted[i].m[2][3] += offset.z;
                }
                bones_use = shifted;
            }
        }
    }

    return DrawModelExecuteInner( self, state, info, bones_use );
}

static int DrawModelExecuteInner( void* self, void* state, ModelRenderInfo_t* info, void* bones ) {


    // per-entity model name cache (props / world weapons detection)
    static char ent_model_names[256][64];
    static int ent_model_ids[256] = {};
    static DWORD ent_model_tick[256] = {};
    auto entity_model_name = [&]( int entity_index ) -> const char* {
        int const slot = entity_index & 0xFF;
        if (ent_model_ids[slot] != entity_index || GetTickCount() - ent_model_tick[slot] > 1000) {
            ent_model_ids[slot] = entity_index;
            ent_model_tick[slot] = GetTickCount();
            ent_model_names[slot][0] = 0;
            void* ent = entity_list.GetClientEntity(entity_index);
            if (ent) {
                int const mi = *reinterpret_cast<int*>((char*)ent + off::m_nModelIndex);
                if (mi > 0 && mi < 0xFFFF) {
                    const void* model = model_info.GetModel(mi);
                    if (model) {
                        const char* name = model_info.GetModelName(model);
                        if (name) {
                            strncpy(ent_model_names[slot], name, sizeof(ent_model_names[slot]) - 1);
                            ent_model_names[slot][sizeof(ent_model_names[slot]) - 1] = 0;
                        }
                    }
                }
            }
        }
        return ent_model_names[slot];
    };

    // Fast early-out when no DME feature is active (avoids GetLocalEntity / GetModelName per model).
    bool const want_vm_offset = (cfg.viewmodel_x != 0.0f || cfg.viewmodel_y != 0.0f || cfg.viewmodel_z != 0.0f);
    bool const want_vm_chams_any = cfg.chams_viewmodel.visible.enabled || cfg.chams_viewmodel.occluded.enabled || cfg.chams_viewmodel.overlay.enabled;
    bool const want_wep_chams_any =
        cfg.chams_weapons.visible.enabled || cfg.chams_weapons.occluded.enabled || cfg.chams_weapons.overlay.enabled ||
        (cfg.wchams_enemy && (cfg.chams_enemy.visible.enabled || cfg.chams_enemy.occluded.enabled || cfg.chams_enemy.overlay.enabled)) ||
        (cfg.wchams_team && (cfg.chams_team.visible.enabled || cfg.chams_team.occluded.enabled || cfg.chams_team.overlay.enabled));
    bool const want_plr_chams_any =
        (cfg.backtrack && cfg.backtrack_chams) || cfg.chams_hit ||
        cfg.chams_enemy.visible.enabled || cfg.chams_enemy.occluded.enabled || cfg.chams_enemy.overlay.enabled ||
        cfg.chams_team.visible.enabled  || cfg.chams_team.occluded.enabled  || cfg.chams_team.overlay.enabled  ||
        cfg.chams_local.visible.enabled || cfg.chams_local.occluded.enabled || cfg.chams_local.overlay.enabled;
    if (!want_vm_offset && !want_vm_chams_any && !want_wep_chams_any && !want_plr_chams_any && !cfg.world_props)
        return CallOrigDrawModel(self, state, info, bones);

    // Viewmodel position offsets.  The local viewmodel entity handle lives on
    // the player at off::m_hViewModel, so we can identify its draw calls safely.
    void* const dme_local = IsGameReady() ? GetLocalEntity() : nullptr;
    int const dme_vm_index = dme_local
        ? (*reinterpret_cast<int*>((char*)dme_local + off::m_hViewModel) & 0xFFF)
        : 0;
    bool const need_model_name = want_vm_offset || want_vm_chams_any || cfg.world_props ||
                                 (want_wep_chams_any && info->entity_index > 64);
    const char* dme_model = (need_model_name && info->pModel) ? model_info.GetModelName(info->pModel) : nullptr;
    auto is_vm_draw = [&](int vm_index) -> bool {
        if (vm_index > 0 && info->entity_index == vm_index) return true;
        return dme_model && (strstr(dme_model, "/v_") || strstr(dme_model, "\\v_"));
    };

    if ( dme_local && ( cfg.viewmodel_x != 0.0f || cfg.viewmodel_y != 0.0f || cfg.viewmodel_z != 0.0f ) ) {
        if ( is_vm_draw(dme_vm_index) ) {
                Vector fwd, right, up;
                AngleVectors( view_angles, fwd, right, up );
                Vector const new_origin = info->origin + right * cfg.viewmodel_x + up * cfg.viewmodel_y + fwd * cfg.viewmodel_z;
                Vector const shift = new_origin - info->origin;

                // the engine uses pModelToWorld when it is set and ignores
                // info->origin - shift the existing matrix (basis preserved)
                if ( info->pModelToWorld ) {
                    static matrix3x4_t vm_matrix;
                    vm_matrix = *info->pModelToWorld;
                    vm_matrix.m[0][3] += shift.x;
                    vm_matrix.m[1][3] += shift.y;
                    vm_matrix.m[2][3] += shift.z;
                    info->pModelToWorld = &vm_matrix;
                }
                info->origin = new_origin;
        }
    }

    // Viewmodel / hands chams (v_ model) via ForcedMaterialOverride.
    // Check both entity index (m_hViewModel) and model path ("/v_") because
    // the engine can submit the viewmodel draw carrying the held weapon's
    // entity index instead of the viewmodel entity index.
    if ( dme_local &&
         ( cfg.chams_viewmodel.visible.enabled || cfg.chams_viewmodel.occluded.enabled || cfg.chams_viewmodel.overlay.enabled ) ) {
        if ( is_vm_draw( dme_vm_index ) ) {
                Config::ChamsTarget const* target = &cfg.chams_viewmodel;
                Config::ChamsLayer const* base = target->visible.enabled ? &target->visible : &target->occluded;
                bool const overlay = target->overlay.enabled;
                if ( base->enabled || overlay ) {
                    bool const ignorez = (!target->visible.enabled && target->occluded.enabled);
                    int result = 0;
                    bool drew = false;

                    if ( base->enabled ) {
                        if ( base->material >= CHAMS_TYPE_COUNT ) {
                            if ( render_view.self ) {
                                float alpha = base->color[3] < 0.02f ? 0.02f : ( base->color[3] > 1.f ? 1.f : base->color[3] );
                                render_view.SetColorModulation( base->color );
                                render_view.SetBlend( alpha );
                            }
                            result = CallOrigDrawModel(self, state, info, bones);
                            ResetChamsMaterial( );
                            drew = true;
                        } else {
                            void* material = GetChamsMaterial( base->material, ignorez, base->color );
                            if ( material ) {
                                ApplyChamsMaterial( material, base->color, ignorez, false, base->material == 6 );
                                result = CallOrigDrawModel(self, state, info, bones);
                                ResetChamsMaterial( );
                                drew = true;
                            }
                        }
                    }

                    if ( overlay ) {
                        if (!drew)
                            result = CallOrigDrawModel(self, state, info, bones);
                        if ( target->overlay.material >= CHAMS_TYPE_COUNT ) {
                            if ( render_view.self ) {
                                float alpha = target->overlay.color[3] < 0.02f ? 0.02f : ( target->overlay.color[3] > 1.f ? 1.f : target->overlay.color[3] );
                                render_view.SetColorModulation( target->overlay.color );
                                render_view.SetBlend( alpha );
                            }
                            result = CallOrigDrawModel(self, state, info, bones);
                            ResetChamsMaterial( );
                        } else {
                            void* material = GetChamsMaterial( target->overlay.material, false, target->overlay.color );
                            if ( material ) {
                                ApplyChamsMaterial( material, target->overlay.color, false, target->overlay.material == 4, target->overlay.material == 6 );
                                result = CallOrigDrawModel(self, state, info, bones);
                                ResetChamsMaterial( );
                            }
                        }
                    }

                    if ( drew || overlay )
                        return result;
                }
            }
        }
    // World props tint: one draw with colour modulation (no material override,
    // no re-draw — that combination corrupted the render pipeline before).
    // Props are entities outside the player range; the model name check keeps
    // world brushes and players untouched.
    if (cfg.world_props && render_view.self && info->entity_index > 64) {
        const char* model = entity_model_name(info->entity_index);
        if (model && strstr(model, "props")) {
            static bool logged_prop = false;
            if (!logged_prop) {
                logged_prop = true;
                Log("[world] tinting dynamic prop %s", model);
            }
            float a = cfg.prop_color[3];
            if (a < 0.05f) a = 0.05f;
            if (a > 1.0f) a = 1.0f;
            static const float white[3] = { 1.0f, 1.0f, 1.0f };
            render_view.SetColorModulation(cfg.prop_color);
            render_view.SetBlend(a);
            int const r = CallOrigDrawModel(self, state, info, bones);
            render_view.SetColorModulation(white);
            render_view.SetBlend(1.0f);
            return r;
        }
    }

    // Weapon chams: held world weapons only (no dropped weapons).  CS:S
    // viewmodels (v_*.mdl) contain the arms as part of the same model, so
    // first-person viewmodel tinting stays in the separate "Viewmodel" target.
    bool const want_weapon_chams =
        cfg.chams_weapons.visible.enabled || cfg.chams_weapons.occluded.enabled || cfg.chams_weapons.overlay.enabled ||
        (cfg.wchams_enemy && (cfg.chams_enemy.visible.enabled || cfg.chams_enemy.occluded.enabled || cfg.chams_enemy.overlay.enabled)) ||
        (cfg.wchams_team && (cfg.chams_team.visible.enabled || cfg.chams_team.occluded.enabled || cfg.chams_team.overlay.enabled));

    if (want_weapon_chams && IsGameReady() && info->entity_index > 64) {
        if (dme_model && (strstr(dme_model, "/v_") || strstr(dme_model, "\\v_")))
            return CallOrigDrawModel(self, state, info, bones);
        if (dme_vm_index > 0 && info->entity_index == dme_vm_index)
            return CallOrigDrawModel(self, state, info, bones);
        Config::ChamsTarget* weapon_target_config = nullptr;
        int const local_aw_idx = dme_local
            ? (*reinterpret_cast<int const*>((char*)dme_local + off::m_hActiveWeapon) & 0xFFF)
            : 0;
        if (local_aw_idx > 0 && info->entity_index == local_aw_idx) {
            weapon_target_config = &cfg.chams_weapons; // own active weapon in 3rd person
        } else {
            void* went = entity_list.GetClientEntity(info->entity_index);
            if (went) {
                bool is_weapon_entity = false;
                void* nw = entity_list.GetClientNetworkable(info->entity_index);
                if (nw) {
                    void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
                    const char* cls = ClientClassName(cc);
                    if (cls && !strstr(cls, "PlantedC4") &&
                        (strstr(cls, "Weapon") || strstr(cls, "Knife") ||
                         strstr(cls, "Grenade") || strcmp(cls, "CC4") == 0))
                        is_weapon_entity = true;
                }
                if (is_weapon_entity) {
                    int const owner = *reinterpret_cast<int const*>((char*)went + off::w_hOwner);
                    int const owner_idx = owner & 0xFFF;
                    int const local_idx = engine.GetLocalPlayer();
                    if (owner_idx == local_idx && local_idx > 0) {
                        weapon_target_config = &cfg.chams_weapons; // own weapon, 3rd person
                    } else if (owner_idx > 0 && owner_idx <= 64) {
                        void* owner_ent = entity_list.GetClientEntity(owner_idx);
                        if (owner_ent) {
                            int const oteam = *reinterpret_cast<int const*>((char*)owner_ent + off::m_iTeamNum);
                            void* local_ent = entity_list.GetClientEntity(local_idx);
                            int const lteam = local_ent ? *reinterpret_cast<int const*>((char*)local_ent + off::m_iTeamNum) : 0;
                            if (cfg.wchams_enemy && oteam != lteam)
                                weapon_target_config = &cfg.chams_enemy;
                            else if (cfg.wchams_team && oteam == lteam)
                                weapon_target_config = &cfg.chams_team;
                        }
                    }
                }
            }
        }
        if (weapon_target_config) {
            Config::ChamsTarget* wt = weapon_target_config;
            Config::ChamsLayer const* wbase = wt->visible.enabled ? &wt->visible : &wt->occluded;
            bool const woverlay = wt->overlay.enabled;
            int result = 0;
            bool drew = false;
            if (wbase->enabled && wbase->material < CHAMS_TYPE_COUNT) {
                void* material = GetChamsMaterial(wbase->material, cfg.chams_disable_occlusion, wbase->color);
                if (material) {
                    ApplyChamsMaterial(material, wbase->color, cfg.chams_disable_occlusion, false, wbase->material == 6);
                    result = CallOrigDrawModel(self, state, info, bones);
                    ResetChamsMaterial();
                    drew = true;
                }
            }
            if (woverlay && wt->overlay.material < CHAMS_TYPE_COUNT) {
                if (!drew)
                    result = CallOrigDrawModel(self, state, info, bones);
                void* material = GetChamsMaterial(wt->overlay.material, false, wt->overlay.color);
                if (material) {
                    ApplyChamsMaterial(material, wt->overlay.color, false, false, wt->overlay.material == 6);
                    result = CallOrigDrawModel(self, state, info, bones);
                    ResetChamsMaterial();
                }
            }
            if (drew || woverlay)
                return result;
            // a layer was enabled but the material failed to resolve: draw the
            // weapon normally instead of leaving it invisible
            if (wt->visible.enabled || wt->occluded.enabled)
                return CallOrigDrawModel(self, state, info, bones);
        }
    }

    bool const want_player_chams =
        (cfg.backtrack && cfg.backtrack_chams) || cfg.chams_hit ||
        cfg.chams_enemy.visible.enabled || cfg.chams_enemy.occluded.enabled || cfg.chams_enemy.overlay.enabled ||
        cfg.chams_team.visible.enabled  || cfg.chams_team.occluded.enabled  || cfg.chams_team.overlay.enabled  ||
        cfg.chams_local.visible.enabled || cfg.chams_local.occluded.enabled || cfg.chams_local.overlay.enabled;

    if ( want_player_chams && IsGameReady( ) && info->entity_index > 0 && info->entity_index <= 64 && IsPlayerIndex( info->entity_index ) ) {
        void* ent = entity_list.GetClientEntity( info->entity_index );
        int hp = ent ? *reinterpret_cast<int*>( (char*)ent + off::m_iHealth ) : 0;
        int team = ent ? *reinterpret_cast<int*>( (char*)ent + off::m_iTeamNum ) : 0;
        bool alive = ent && hp > 0 &&
            *reinterpret_cast<char*>( (char*)ent + off::m_lifeState ) == 0 &&
            ( team == 2 || team == 3 );

        // backtrack chams: draw the same model again with the bones (x64) or
        // model transform (v34) translated to the recorded historical position
        if ( alive && cfg.backtrack && cfg.backtrack_chams && ent && info->pRenderable ) {
            Vector const cur_org = *reinterpret_cast<Vector*>((char*)ent + off::m_vecOrigin);
            Vector const bt = Backtrack_PickRaw( info->entity_index, cfg.backtrack_time );
            if ( !(bt.x == 0.0f && bt.y == 0.0f && bt.z == 0.0f) ) {
                Vector const delta = bt - cur_org;
                void* material = GetChamsMaterial( 1 /*flat*/, true /*ignorez*/, cfg.bt_chams_color );
                if ( material ) {
#ifdef _WIN64
                    if ( bones ) {
                        int bone_count = 128;
                        int const cached = *reinterpret_cast<int const*>((char*)info->pRenderable + 0xB50);
                        if ( cached > 0 && cached <= 128 ) bone_count = cached;

                        static matrix3x4_t bt_bones[128];
                        matrix3x4_t const* src = ( matrix3x4_t const* )bones;
                        for ( int i = 0; i < bone_count; i++ ) {
                            bt_bones[i] = src[i];
                            bt_bones[i].m[0][3] += delta.x;
                            bt_bones[i].m[1][3] += delta.y;
                            bt_bones[i].m[2][3] += delta.z;
                        }
                        ApplyChamsMaterial( material, cfg.bt_chams_color, true, false, false );
                        CallOrigDrawModel(self, state, info, bt_bones);
                        ResetChamsMaterial( );
                    }
#else
                    matrix3x4_t* cached_bones = *reinterpret_cast<matrix3x4_t**>((char*)info->pRenderable + 0x66c);
                    int const cached_count = *reinterpret_cast<int*>((char*)info->pRenderable + 0x678);
                    if ( cached_bones && cached_count > 0 && cached_count <= 128 ) {
                        static float saved_t[128][3];
                        for ( int i = 0; i < cached_count; i++ ) {
                            saved_t[i][0] = cached_bones[i].m[0][3];
                            saved_t[i][1] = cached_bones[i].m[1][3];
                            saved_t[i][2] = cached_bones[i].m[2][3];
                            cached_bones[i].m[0][3] += delta.x;
                            cached_bones[i].m[1][3] += delta.y;
                            cached_bones[i].m[2][3] += delta.z;
                        }
                        ApplyChamsMaterial( material, cfg.bt_chams_color, true, false, false );
                        CallOrigDrawModel(self, state, info, bones);
                        ResetChamsMaterial( );
                        for ( int i = 0; i < cached_count; i++ ) {
                            cached_bones[i].m[0][3] = saved_t[i][0];
                            cached_bones[i].m[1][3] = saved_t[i][1];
                            cached_bones[i].m[2][3] = saved_t[i][2];
                        }
                    }
#endif
                }
            }
        }

        if ( alive ) {
            int local_index = engine.GetLocalPlayer( );
            bool is_local = info->entity_index == local_index;
            void* local_ent = local_index > 0 ? entity_list.GetClientEntity( local_index ) : nullptr;
            int local_team = local_ent ? *reinterpret_cast<int*>( (char*)local_ent + off::m_iTeamNum ) : 0;
            bool is_teammate = !is_local && team == local_team;

            Config::ChamsTarget* target = is_local ? &cfg.chams_local
                                              : ( is_teammate ? &cfg.chams_team : &cfg.chams_enemy );

            bool visible = true;
            if ( local_ent && target->visible.enabled != target->occluded.enabled ) {
                static bool s_vis_cache[65] = {};
                static DWORD s_vis_tick[65] = {};
                DWORD const now_v = GetTickCount();
                if (now_v - s_vis_tick[info->entity_index] >= 100) {
                    s_vis_tick[info->entity_index] = now_v;
                    Vector eye = PlayerEye( local_ent );
                    s_vis_cache[info->entity_index] = TraceVisible( eye, PlayerEye( ent ) );
                }
                visible = s_vis_cache[info->entity_index];
            } else if ( local_ent && target->visible.enabled && target->occluded.enabled ) {
                static bool s_vis_cache2[65] = {};
                static DWORD s_vis_tick2[65] = {};
                DWORD const now_v = GetTickCount();
                if (now_v - s_vis_tick2[info->entity_index] >= 100) {
                    s_vis_tick2[info->entity_index] = now_v;
                    Vector eye = PlayerEye( local_ent );
                    s_vis_cache2[info->entity_index] = TraceVisible( eye, PlayerEye( ent ) );
                }
                visible = s_vis_cache2[info->entity_index];
            }

            bool const use_occluded = (!visible || is_local);
            Config::ChamsLayer const* base = use_occluded ? &target->occluded : &target->visible;
            if ( !base->enabled ) {
                Config::ChamsLayer const* other = use_occluded ? &target->visible : &target->occluded;
                if ( other->enabled ) base = other;
            }
            bool const hit = BindActive( cfg.kb_hitchams, cfg.kb_mode_hitchams, BIND_HITCHAMS ) &&
                             HitChamsActive( info->entity_index );
            bool const overlay_wanted = target->overlay.enabled || hit;
            bool const base_wanted = base->enabled;
            bool const ignorez = (use_occluded || cfg.chams_disable_occlusion);

            if ( base_wanted || overlay_wanted ) {
                int result = 0;
                bool drew_base = false;

                if ( base_wanted ) {
                    if ( base->material >= CHAMS_TYPE_COUNT ) {
                        // "Regular": keep the original model material, just tint it.
                        if ( render_view.self ) {
                            render_view.SetColorModulation( base->color );
                            float alpha = base->color[3] < 0.02f ? 0.02f : ( base->color[3] > 1.f ? 1.f : base->color[3] );
                            render_view.SetBlend( alpha );
                        }
                        result = CallOrigDrawModel(self, state, info, bones);
                        ResetChamsMaterial( );
                        drew_base = true;
                    } else {
                        void* material = GetChamsMaterial( base->material, ignorez, base->color );
                        if ( material ) {
                            SetStage( "chams draw" );
                            ApplyChamsMaterial( material, base->color, ignorez, false, base->material == 6 );
                            result = CallOrigDrawModel(self, state, info, bones);
                            ResetChamsMaterial( );
                            drew_base = true;
                        }
                    }
                }

                if ( overlay_wanted ) {
                    if (!drew_base)
                        result = CallOrigDrawModel(self, state, info, bones);

                    int const type = hit ? cfg.chams_hit_material : target->overlay.material;
                    float const* color = hit ? cfg.chams_hit_color : target->overlay.color;
                    if ( type >= CHAMS_TYPE_COUNT ) {
                        if ( render_view.self ) {
                            render_view.SetColorModulation( color );
                            float alpha = color[3] < 0.02f ? 0.02f : ( color[3] > 1.f ? 1.f : color[3] );
                            render_view.SetBlend( alpha );
                        }
                        result = CallOrigDrawModel(self, state, info, bones);
                        ResetChamsMaterial( );
                    } else {
                        void* material = GetChamsMaterial( type, false, color );
                        if ( material ) {
                            SetStage( "chams overlay" );
#ifdef _WIN64
                            bool const is_glow = (type == 4);
#else
                            bool const is_glow = false;
#endif
                            ApplyChamsMaterial( material, color, false, is_glow, type == 6 );
                            result = CallOrigDrawModel(self, state, info, bones);
                            ResetChamsMaterial( );
                        }
                    }
                }

                return result;
            }
        }
    }

    if (cfg.world_props && dme_model && (strstr(dme_model, "props") || strstr(dme_model, "Props")) && render_view.self) {
        float a = cfg.prop_color[3];
        if (a < 0.05f) a = 0.05f;
        if (a > 1.0f) a = 1.0f;
        render_view.SetColorModulation(cfg.prop_color);
        render_view.SetBlend(a);
        int const r = CallOrigDrawModel(self, state, info, bones);
        ResetChamsMaterial();
        return r;
    }

    return CallOrigDrawModel(self, state, info, bones);
}

// ---------------------------------------------------------------------------
// world visuals
// ---------------------------------------------------------------------------
static void CvarSetInt(const char* name, int value) {
    if (!cvar.self) return;
    ConVar* v = cvar.FindVar(name);
    if (v) { v->f_value = (float)value; v->i_value = value; }
}

static void CvarSetFloat(const char* name, float value) {
    if (!cvar.self) return;
    ConVar* v = cvar.FindVar(name);
    if (v) { v->f_value = value; v->i_value = (int)value; }
}

static void CvarSetString(const char* name, const char* value) {
    if (!engine.self) return;
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "%s \"%s\"", name, value);
    engine.ClientCmd(cmd);
}

static bool g_occlusion_off = false;

static void UpdateChamsOcclusion() {
    bool const any_player =
        cfg.chams_enemy.occluded.enabled || cfg.chams_enemy.visible.enabled || cfg.chams_enemy.overlay.enabled ||
        cfg.chams_team.occluded.enabled || cfg.chams_team.visible.enabled || cfg.chams_team.overlay.enabled ||
        cfg.chams_local.occluded.enabled || cfg.chams_local.visible.enabled || cfg.chams_local.overlay.enabled ||
        cfg.chams_viewmodel.occluded.enabled || cfg.chams_viewmodel.visible.enabled || cfg.chams_viewmodel.overlay.enabled ||
        cfg.chams_hit;
    bool want = cfg.chams_disable_occlusion && any_player;
    if (want == g_occlusion_off) return;
    g_occlusion_off = want;
    CvarSetInt("r_occlusion", want ? 0 : 1);
    Log("chams: r_occlusion=%d", want ? 0 : 1);
}

struct CmdCacheEntry {
    const char* name;
    float value;
    bool used;
};
static CmdCacheEntry g_cmd_cache[40] = {};

// Cheat-protected cvars must go through the command path (and sv_cheats) so
// their change callbacks run.  Writing the ConVar memory directly corrupted
// the material system and crashed as soon as fullbright was toggled.
static void CmdSet(const char* name, float value) {
    if (!engine.self) return;
    auto apply = [&]() {
#ifndef _WIN64
        if (cvar.self) {
            if (ConVar* cv = cvar.FindVar(name)) {
                int* flags = reinterpret_cast<int*>((char*)cv + 0x14);
                *flags &= ~(1 << 14); // clear FCVAR_CHEAT
                VFunc<void(THISCALL_CC*)(void*, float)>(cv, 10)(cv, value);
            }
        }
        return;
#endif
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "%s %g", name, value);
        engine.ClientCmd(cmd);
    };
    for (int i = 0; i < 40; i++) {
        if (g_cmd_cache[i].used && strcmp(g_cmd_cache[i].name, name) == 0) {
            if (g_cmd_cache[i].value == value) return;
            g_cmd_cache[i].value = value;
            apply();
            return;
        }
    }
    for (int i = 0; i < 40; i++) {
        if (!g_cmd_cache[i].used) {
            g_cmd_cache[i].used = true;
            g_cmd_cache[i].name = name;
            g_cmd_cache[i].value = value;
            apply();
            return;
        }
    }
}

static void ApplyWorldVisuals() {
    // Never queue client commands while the map is loading or the main menu is
    // up; doing so stalls the listen-server load.
    if (!view_valid) return;
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 250) return;
    last = now;

    g_world_needs_cheats = cfg.world_fullbright || cfg.world_nofog || cfg.world_custom_fog ||
                           cfg.world_nosky || cfg.world_night > 0.001f ||
                           cfg.world_brightness > 0.001f || cfg.world_model_ambient > 0.001f ||
                           cfg.rem_postprocess || cfg.rem_shadows ||
                           cfg.rem_decals || cfg.rem_grass || cfg.rem_ropes || cfg.rem_water ||
                           cfg.rem_dynlights || cfg.rem_shells || cfg.rem_debris || cfg.rem_blood ||
                           cfg.rem_particles;

    // Make sure sv_cheats is already on before the first cheat-protected
    // command is queued (the normal update runs earlier in the frame).
    ForceSvCheats();

#ifndef _WIN64
    // On v34, ClientCmd prints FCVAR_CLIENTCMD_CAN_EXECUTE / cheat warnings
    // for every cvar sent at startup.  Only issue commands when a toggle is on
    // (or once when turning everything back off to restore defaults).
    static bool was_world_active = false;
    bool const world_active = g_world_needs_cheats || cfg.world_bloom != 1.0f || cfg.viewmodel_fov != 0.0f;
    if (!world_active && !was_world_active)
        return;
    was_world_active = world_active;
#endif

    // --- lighting / world ---
    // Keep mat_monitorgamma at default (2.2) so brightness is pinned to the
    // world rather than changing the screen/monitor gamma ramp.
    static bool gamma_restored = false;
    if (!gamma_restored) {
        gamma_restored = true;
        CmdSet("mat_monitorgamma", 2.2f);
    }

    CmdSet("mat_fullbright", cfg.world_fullbright ? 1.f : 0.f);
    CmdSet("mat_bloomscale", cfg.world_bloom + cfg.world_brightness);

    float ambient = cfg.world_model_ambient + cfg.world_brightness * 0.5f - cfg.world_night * 0.5f;
    ambient = ambient < 0.f ? 0.f : (ambient > 1.f ? 1.f : ambient);
    CmdSet("r_ambientfraction", ambient);
    CmdSet("r_3dsky", (cfg.world_nosky || cfg.world_night > 0.01f) ? 0.f : 1.f);
    // this build has no mat_ambient_light cvars - the real darkness levers
    // are r_ambientboost (brightens dark areas) and r_ambientmin (clamps the
    // ambient from below); both must follow the night level and model ambient
    CmdSet("r_ambientboost", (cfg.world_night > 0.05f && cfg.world_model_ambient < 0.05f) ? 0.f : 1.f);
    CmdSet("r_ambientmin", cfg.world_model_ambient > 0.05f
                               ? std::min(1.0f, cfg.world_model_ambient * 0.5f)
                               : std::max(0.0f, 0.6f - cfg.world_night * 0.6f));
    CmdSet("r_ambientfactor", 5.0f + cfg.world_model_ambient * 10.0f);

    // --- fog ---
    static float last_col[3] = { -1, -1, -1 };
    if (cfg.world_nofog) {
        CmdSet("fog_override", 1.f);
        CmdSet("fog_enable", 0.f);
        CmdSet("fog_enableskybox", 0.f);
        last_col[0] = -1.f;
    } else if (cfg.world_custom_fog) {
        CmdSet("fog_override", 1.f);
        CmdSet("fog_enable", 1.f);
        CmdSet("fog_enableskybox", 1.f);
        CmdSet("fog_start", cfg.fog_start);
        CmdSet("fog_end", cfg.fog_end);
        CmdSet("fog_density", cfg.fog_density);
        CmdSet("fog_startskybox", cfg.fog_start);
        CmdSet("fog_endskybox", cfg.fog_end);
        CmdSet("fog_densityskybox", cfg.fog_density);

        if (last_col[0] != cfg.fog_color[0] || last_col[1] != cfg.fog_color[1] ||
            last_col[2] != cfg.fog_color[2]) {
            last_col[0] = cfg.fog_color[0]; last_col[1] = cfg.fog_color[1];
            last_col[2] = cfg.fog_color[2];
            char col[64];
            snprintf(col, sizeof(col), "%d %d %d",
                (int)(cfg.fog_color[0] * 255.0f), (int)(cfg.fog_color[1] * 255.0f),
                (int)(cfg.fog_color[2] * 255.0f));
#ifndef _WIN64
            auto set_str = [&](const char* cname, const char* val) {
                if (cvar.self) {
                    if (ConVar* cv = cvar.FindVar(cname)) {
                        int* flags = reinterpret_cast<int*>((char*)cv + 0x14);
                        *flags &= ~(1 << 14);
                        VFunc<void(THISCALL_CC*)(void*, const char*)>(cv, 11)(cv, val);
                    }
                }
            };
            set_str("fog_color", col);
            set_str("fog_colorskybox", col);
#else
            engine.ClientCmd(("fog_color " + std::string(col)).c_str());
            engine.ClientCmd(("fog_colorskybox " + std::string(col)).c_str());
#endif
        }
    } else {
        CmdSet("fog_override", 0.f);
        last_col[0] = -1.f;
    }

    // --- removals ---
    CmdSet("mat_postprocess_enable", cfg.rem_postprocess ? 0.f : 1.f);
    CmdSet("mat_disable_bloom", cfg.rem_postprocess ? 1.f : 0.f);
    CmdSet("r_shadows", cfg.rem_shadows ? 0.f : 1.f);
    CmdSet("r_drawdecals", cfg.rem_decals ? 0.f : 1.f);
    CmdSet("r_drawdetailprops", cfg.rem_grass ? 0.f : 1.f);
    CmdSet("r_drawropes", cfg.rem_ropes ? 0.f : 1.f);
    CmdSet("r_waterdrawreflection", cfg.rem_water ? 0.f : 1.f);
    CmdSet("r_waterforceexpensive", cfg.rem_water ? 0.f : 1.f);
    CmdSet("r_dynamic", cfg.rem_dynlights ? 0.f : 1.f);
    CmdSet("cl_ejectbrass", cfg.rem_shells ? 0.f : 1.f);
    CmdSet("r_drawflecks", cfg.rem_debris ? 0.f : 1.f);
    CmdSet("r_drawparticles", cfg.rem_particles ? 0.f : 1.f);
    CmdSet("cl_ragdoll_physics_enable", cfg.rem_debris ? 0.f : 1.f);
    CmdSet("violence_hblood", cfg.rem_blood ? 0.f : 1.f);
    CmdSet("violence_ablood", cfg.rem_blood ? 0.f : 1.f);
    CmdSet("violence_hgibs", cfg.rem_blood ? 0.f : 1.f);
    CmdSet("violence_agibs", cfg.rem_blood ? 0.f : 1.f);

    // --- viewmodel ---
    CmdSet("viewmodel_fov", cfg.viewmodel_fov >= 1.0f ? cfg.viewmodel_fov : 0.f);
    CmdSet("cl_righthand", cfg.left_hand ? 0.f : 1.f);
    CmdSet("cl_bobcycle", cfg.remove_bob ? 0.0f : 0.98f);
    CmdSet("cl_bob", cfg.remove_bob ? 0.0f : 0.002f);
    CmdSet("cl_bobup", cfg.remove_bob ? 0.0f : 0.5f);
}

// ---------------------------------------------------------------------------
// window / input
// ---------------------------------------------------------------------------
static WNDPROC g_old_wndproc = nullptr;
static bool g_imgui_win32_ready = false;
static bool g_menu_key_held = false;
static ULONGLONG g_menu_key_last = 0;

static void CenterCursor(HWND hwnd) {
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return;
    POINT p{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
    ClientToScreen(hwnd, &p);
    SetCursorPos(p.x, p.y);
}

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_SETCURSOR && cfg.menu_open) {
        SetCursor(nullptr);
        return TRUE;
    }

    // Menu key.  Some message loops deliver a single arrow press more than
    // once (IsDialogMessage / duplicate dispatch), which used to open and
    // instantly close the menu.  Latch on key-down and only release on the
    // matching key-up; a short time debounce catches delayed duplicates too.
    if (msg == WM_KEYDOWN && cfg.kb_menu != 0 && wparam == (WPARAM)cfg.kb_menu && !alice_ui::IsBinding()) {
        ULONGLONG now = GetTickCount64();
        if (now - g_menu_key_last > 350)
            g_menu_key_held = false; // recover if a key-up was never delivered
        if (!(lparam & 0x40000000) && !g_menu_key_held && now - g_menu_key_last > 350) {
            g_menu_key_held = true;
            g_menu_key_last = now;
            alice_ui::Toggle();
            if (cfg.menu_open) CenterCursor(hwnd);
            Log("menu toggled: %d", cfg.menu_open ? 1 : 0);
        }
        return 0;
    }
    if (msg == WM_KEYUP && cfg.kb_menu != 0 && wparam == (WPARAM)cfg.kb_menu) {
        g_menu_key_held = false;
        return 0;
    }

    if (g_imgui_win32_ready) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        if (cfg.menu_open) {
            switch (msg) {
            case WM_INPUT:
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_CHAR:
            case WM_KEYDOWN: case WM_KEYUP:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                return 0;
            default:
                break;
            }
        }
    }

    return CallWindowProcA(g_old_wndproc, hwnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
// user32 IAT hooks so the game cannot recenter the cursor while the menu is up
// ---------------------------------------------------------------------------
typedef BOOL(WINAPI* SetCursorPosFn)(int, int);
static SetCursorPosFn oSetCursorPos = nullptr;
typedef BOOL(WINAPI* GetCursorPosFn)(LPPOINT);
static GetCursorPosFn oGetCursorPos = nullptr;
static HWND g_hwnd = nullptr;

static BOOL WINAPI HookedSetCursorPos(int x, int y) {
    if (cfg.menu_open) return TRUE;
    return oSetCursorPos ? oSetCursorPos(x, y) : FALSE;
}

static BOOL WINAPI HookedGetCursorPos(LPPOINT pt) {
    if (cfg.menu_open && pt && g_hwnd) {
        RECT r{};
        if (GetClientRect(g_hwnd, &r)) {
            POINT c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            ClientToScreen(g_hwnd, &c);
            *pt = c;
            return TRUE;
        }
    }
    return oGetCursorPos ? oGetCursorPos(pt) : FALSE;
}

static void PatchImport(HMODULE module, const char* dll_name, const char* func, void* hook, void** original) {
    if (!module) return;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>((char*)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;

    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>((char*)module + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        const char* name = (const char*)module + imp->Name;
        if (_stricmp(name, dll_name) != 0) continue;

        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>((char*)module + imp->FirstThunk);
        auto orig_thunk = imp->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>((char*)module + imp->OriginalFirstThunk)
            : nullptr;

        for (; thunk->u1.Function; thunk++) {
            if (!orig_thunk || (orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                if (orig_thunk) orig_thunk++;
                continue;
            }
            auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>((char*)module + orig_thunk->u1.AddressOfData);
            orig_thunk++;
            if (strcmp((const char*)by_name->Name, func) != 0) continue;

            DWORD old = 0;
            if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                if (original) *original = (void*)thunk->u1.Function;
                thunk->u1.Function = (ULONG_PTR)hook;
                DWORD tmp = 0;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &tmp);
            }
        }
    }
}

void InstallInputHooks() {
    const char* modules[] = { "client.dll", "engine.dll", "inputsystem.dll", "vguimatsurface.dll", "vgui2.dll", "GameUI.dll", "serverbrowser.dll" };
    for (const char* m : modules) {
        HMODULE mod = GetModuleHandleA(m);
        if (mod) {
            PatchImport(mod, "USER32.dll", "SetCursorPos", (void*)HookedSetCursorPos, (void**)&oSetCursorPos);
            PatchImport(mod, "USER32.dll", "GetCursorPos", (void*)HookedGetCursorPos, (void**)&oGetCursorPos);
        }
    }
    Log("input hooks: SetCursorPos=%p GetCursorPos=%p", oSetCursorPos, oGetCursorPos);
}

// patches every import thunk whose resolved function pointer equals `target`
// (engine.dll imports sendto from WSOCK32 by ordinal, so name matching in
// PatchImport cannot find it)
static int PatchImportByAddress(HMODULE module, const char* dll_name, void* target,
                                void* hook, void** original) {
    if (!module || !target) return 0;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>((char*)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;

    int patched = 0;
    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>((char*)module + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        const char* name = (const char*)module + imp->Name;
        if (_stricmp(name, dll_name) != 0) continue;
        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>((char*)module + imp->FirstThunk);
        for (; thunk->u1.Function; thunk++) {
            if (thunk->u1.Function != (ULONG_PTR)target) continue;
            DWORD old = 0;
            if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                if (original && !*original) *original = (void*)thunk->u1.Function;
                thunk->u1.Function = (ULONG_PTR)hook;
                DWORD tmp = 0;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &tmp);
                patched++;
            }
        }
    }
    return patched;
}

// fakelag needs the engine's sendto; it lives in the WSOCK32 import table
void InstallFakelagHook() {
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!engine) { Log("fakelag: engine.dll not loaded"); return; }

    HMODULE wsock = GetModuleHandleA("WSOCK32.dll");
    if (!wsock) wsock = LoadLibraryA("WSOCK32.dll");
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");

    void* fn_wsock = wsock ? (void*)GetProcAddress(wsock, "sendto") : nullptr;
    void* fn_ws2 = ws2 ? (void*)GetProcAddress(ws2, "sendto") : nullptr;
    if (!fn_wsock && !fn_ws2) { Log("fakelag: sendto export not found"); return; }

    int n = 0;
    if (fn_wsock)
        n += PatchImportByAddress(engine, "WSOCK32.dll", fn_wsock, (void*)HookedSendTo, (void**)&oSendTo);
    if (fn_ws2)
        n += PatchImportByAddress(engine, "WSOCK32.dll", fn_ws2, (void*)HookedSendTo, (void**)&oSendTo);
    Log("fakelag: sendto IAT patched %d thunk(s) (orig %p)", n, oSendTo);
}

void InstallWindowHook(HWND hwnd) {
    if (!hwnd) { Log("window hook: no hwnd"); return; }
    g_hwnd = hwnd;
    g_old_wndproc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    Log("window hook installed (old %p)", g_old_wndproc);
}

// ---------------------------------------------------------------------------
// client mode / model render hooks
// ---------------------------------------------------------------------------
void InstallClientModeHooks() {
    if (!client_module) { Log("client mode hooks: no client.dll"); return; }

    bool ok1 = false;
    void** bcvt = nullptr;
    if (base_client) {
        bcvt = *reinterpret_cast<void***>(base_client);
        ok1 = PatchVtable(bcvt, BASE_CLIENT_CREATE_MOVE, (void*)HookedBaseCreateMove, (void**)&oBaseCreateMove);
        bool ok9 = PatchVtable(bcvt, BASE_CLIENT_FRAME_STAGE, (void*)HookedFrameStageNotify, (void**)&oFrameStageNotify);
        Log("baseclient vt[%d] rva=%#llx  vt[%d] rva=%#llx",
            BASE_CLIENT_CREATE_MOVE, (unsigned long long)((uintptr_t)oBaseCreateMove - (uintptr_t)client_module),
            BASE_CLIENT_FRAME_STAGE, (unsigned long long)((uintptr_t)oFrameStageNotify - (uintptr_t)client_module));
        if (!ok9) Log("frame stage hook failed");
    }

#ifdef _WIN64
    void** vt = reinterpret_cast<void**>((uintptr_t)client_module + CLIENTMODE_VTABLE_RVA);
#else
    void** vt = nullptr;
    if (bcvt) {
        uintptr_t const cbase = reinterpret_cast<uintptr_t>(client_module);
        // VClient013: [7] HudProcessInput / [8] HudUpdate (never [10] HudText/gHUD)
        for (int slot_idx = 7; slot_idx <= 8 && !vt; slot_idx++) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(bcvt[slot_idx]);
            if (!p) continue;
            for (int i = 0; i < 24; i++) {
                void* cand = nullptr;
                int next_off = 0;
                if (p[i] == 0x8B && p[i + 1] == 0x0D) {
                    uintptr_t addr = *reinterpret_cast<const uintptr_t*>(p + i + 2);
                    if (addr > cbase && addr < cbase + 0x2000000)
                        cand = *reinterpret_cast<void**>(addr);
                    next_off = i + 6;
                } else if (p[i] == 0xB9) {
                    uintptr_t addr = *reinterpret_cast<const uintptr_t*>(p + i + 1);
                    if (addr > cbase && addr < cbase + 0x2000000)
                        cand = reinterpret_cast<void*>(addr);
                    next_off = i + 5;
                }
                if (cand && next_off > 0) {
                    // Must be a tiny virtual-dispatch wrapper (mov eax, [ecx]; call/jmp [eax+...])
                    bool has_vcall = false;
                    for (int j = next_off; j < next_off + 12 && j < 32; j++) {
                        if (p[j] == 0x8B && (p[j + 1] == 0x01 || p[j + 1] == 0x11)) {
                            has_vcall = true;
                            break;
                        }
                    }
                    if (!has_vcall) continue;
                    void** cand_vt = *reinterpret_cast<void***>(cand);
                    uintptr_t vt_addr = reinterpret_cast<uintptr_t>(cand_vt);
                    if (vt_addr > cbase && vt_addr < cbase + 0x2000000) {
                        vt = cand_vt;
                        Log("[hooks] v34 ClientMode=%p vt=%p (from bcvt[%d]+%d)", cand, vt, slot_idx, i);
                        break;
                    }
                }
            }
        }
        if (!vt) Log("[hooks] v34 ClientMode not found in bcvt[7..8] (skipping ClientMode hooks)");
    }
#endif
    bool ok8 = vt ? PatchVtable(vt, CM_CREATE_MOVE, (void*)HookedClientModeCreateMove, (void**)&oClientModeCreateMove) : false;
    bool ok2 = vt ? PatchVtable(vt, CM_OVERRIDE_VIEW, (void*)HookedOverrideView, (void**)&oOverrideView) : false;
    bool ok3 = vt ? PatchVtable(vt, CM_SHOULD_DRAW_LOCAL_PLAYER, (void*)HookedShouldDrawLocalPlayer, (void**)&oShouldDrawLocalPlayer) : false;
    bool ok5 = vt ? PatchVtable(vt, 24, (void*)HookedShouldDrawViewModel, (void**)&oShouldDrawViewModel) : false;

    bool ok6 = true;

    // IVModelRender vtable index 19 (DrawModelExecute on x64 / DrawModelEx on v34)
    bool ok4 = false;
    if (model_render.self) {
        WriteChamsVmts();
        void** mvt = *reinterpret_cast<void***>(model_render.self);
        ok4 = PatchVtable(mvt, 19, (void*)HookedDrawModelExecute, (void**)&oDrawModelExecute);
        WorldFx_Install( );
        InstallGameEvents( );
    }

    // IPanel::PaintTraverse hook (x64) / CHudScope::Paint vtable patch (v34) to remove scope overlay
#ifdef _WIN64
    {
        typedef void(THISCALL_CC* PaintTraverseFn)(void*, uintptr_t, bool, bool);
        static PaintTraverseFn oPaintTraverse = nullptr;
        static uintptr_t g_scope_panel = 0;
        static auto HookedPaintTraverse = [](void* self, uintptr_t vgui_panel, bool force_repaint, bool allow_force) -> void {
            if (cfg.rem_scope && vgui_panel) {
                if (!g_scope_panel) {
                    const char* pname = VFunc<const char*(THISCALL_CC*)(void*, uintptr_t)>(self, 36)(self, vgui_panel);
                    if (pname && (strstr(pname, "HudZoom") != nullptr || strstr(pname, "HudScope") != nullptr)) {
                        g_scope_panel = vgui_panel;
                        Log("[scope] matched panel '%s' (0x%p)", pname, (void*)vgui_panel);
                    }
                }
                if (vgui_panel == g_scope_panel)
                    return;
            }
            if (oPaintTraverse)
                oPaintTraverse(self, vgui_panel, force_repaint, allow_force);
        };
        if (HMODULE vgui2 = GetModuleHandleA("vgui2.dll")) {
            if (void* panel = GetInterface(vgui2, "VGUI_Panel009")) {
                void** pvt = *reinterpret_cast<void***>(panel);
                if (pvt) {
                    bool ok_pt = PatchVtable(pvt, 41, (void*)+HookedPaintTraverse, (void**)&oPaintTraverse);
                    Log("[scope] PaintTraverse hook=%d", ok_pt ? 1 : 0);
                }
            }
        }
    }
#else
    if (client_module) {
        typedef void(__thiscall* HudScopePaintFn)(void*);
        static HudScopePaintFn oHudScopePaint = nullptr;
        static auto HookedHudScopePaint = [](void* self, void*) __attribute__((fastcall)) -> void {
            if (cfg.rem_scope) return;
            if (oHudScopePaint) oHudScopePaint(self);
        };
        void** slot = reinterpret_cast<void**>((uintptr_t)client_module + 0x33c120);
        bool ok_sc = PatchVtable(slot, 0, (void*)+HookedHudScopePaint, (void**)&oHudScopePaint);
        Log("[scope] v34 CHudScope::Paint hook=%d orig=%p", ok_sc ? 1 : 0, (void*)oHudScopePaint);
    }
#endif

#ifdef _WIN64
    // C_CSPlayer::ValidateModelIndex is vtable index 45 (prevents cl_minmodels
    // from overriding the local player's custom model index mid-frame).
    typedef void(__fastcall* ValidateModelIndexFn)(void*);
    static ValidateModelIndexFn oValidateModelIndex = nullptr;
    static auto HookedValidateModelIndex = [](void* self) -> void {
        int custom_idx = 0;
        if (ClientModel_ShouldOverride(self, custom_idx)) {
            int16_t const cur = *reinterpret_cast<const int16_t*>((const char*)self + off::m_nModelIndex);
            if (cur != (int16_t)custom_idx && client_module) {
                typedef void(__fastcall* SetModelIndexFn)(void*, int);
                auto set_model_index = reinterpret_cast<SetModelIndexFn>((uintptr_t)client_module + 0x8D670);
                mdl_cache.BeginLock();
                set_model_index(self, custom_idx);
                mdl_cache.EndLock();
            }
            return;
        }
        if (oValidateModelIndex) oValidateModelIndex(self);
    };
    void** cs_vt = reinterpret_cast<void**>((uintptr_t)client_module + CS_PLAYER_VTABLE_RVA);
    bool ok11 = PatchVtable(cs_vt, 45, (void*)(void(*)(void*))HookedValidateModelIndex, (void**)&oValidateModelIndex);
#else
    bool ok11 = false;
#endif

    Log("hooks: base_create=%d mode_create=%d override=%d drawlocal=%d drawmodel=%d valmodel=%d viewmodel=%d viewangles=%d",
        ok1, ok8, ok2, ok3, ok4, ok11, ok5, ok6);
    if (engine.self && engine_module) {
        void** evt = *reinterpret_cast<void***>(engine.self);
        Log("engine vtable rva=%#llx  vt[12]=%#llx  vt[19]=%#llx",
            (unsigned long long)((uintptr_t)evt - (uintptr_t)engine_module),
            (unsigned long long)((uintptr_t)evt[12] - (uintptr_t)engine_module),
            (unsigned long long)((uintptr_t)evt[19] - (uintptr_t)engine_module));
    }
    if (oDrawModelExecute && engine_module)
        Log("modelrender vt[19] rva=%#llx", (unsigned long long)((uintptr_t)oDrawModelExecute - (uintptr_t)engine_module));
}

// ---------------------------------------------------------------------------
// d3d9 / imgui
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* EndSceneFn)(IDirect3DDevice9*);
typedef HRESULT(WINAPI* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static EndSceneFn oEndScene = nullptr;
static ResetFn oReset = nullptr;
static IDirect3DDevice9* g_device = nullptr;
static IDirect3DDevice9* g_imgui_device = nullptr;
static bool g_imgui_init = false;
static bool g_alice_init = false;

static LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcA(h, m, w, l);
}

static void InitImGui(IDirect3DDevice9* device) {
    if (g_imgui_init) return;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;

    void* hwnd = game_window ? (void*)game_window : (void*)GetActiveWindow();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(device);
    g_imgui_device = device;
    g_imgui_win32_ready = true;
    g_imgui_init = true;

    int sw = 0, sh = 0;
    engine.GetScreenSize(sw, sh);
    if (sw <= 0 || sh <= 0) { sw = 1920; sh = 1080; }
    bool const was_open = cfg.menu_open;
    alice_ui::Init(device, sw, sh);
    cfg.menu_open = was_open;
    g_alice_init = true;

    Log("imgui initialized (hwnd=%p device=%p)", hwnd, device);
}

static HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    if (g_alice_init)
        alice_ui::OnLostDevice();
    if (g_imgui_init)
        ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = oReset ? oReset(device, pp) : D3D_OK;
    Log("[d3d9] Reset %ux%u windowed=%d -> hr=0x%08x",
        pp ? (unsigned)pp->BackBufferWidth : 0u,
        pp ? (unsigned)pp->BackBufferHeight : 0u,
        pp ? (int)pp->Windowed : -1,
        (unsigned)hr);
    if (g_imgui_init && SUCCEEDED(hr)) {
        g_imgui_device = device;
        ImGui_ImplDX9_CreateDeviceObjects();
        if (g_alice_init) {
            int const w = (pp && pp->BackBufferWidth > 0) ? (int)pp->BackBufferWidth : 1920;
            int const h = (pp && pp->BackBufferHeight > 0) ? (int)pp->BackBufferHeight : 1080;
            alice_ui::OnResetDevice(device, w, h);
        }
    }
    return hr;
}

static ImU32 EspFeedColor(float a) {
    float const* c = cfg.log_misses_color;
    return ImColor(c[0], c[1], c[2], c[3] * a);
}

static void DrawOverlays(ImDrawList* draw, int w, int h) {
    ImGuiIO& io = ImGui::GetIO();

    // Scope override / custom scope lines (claret/killed style)
    if (cfg.rem_scope && IsGameReady()) {
        void* local = GetLocalEntity();
        bool scoped = false;
        if (local && *reinterpret_cast<const char*>((const char*)local + off::m_lifeState) == 0 &&
            *reinterpret_cast<const int*>((const char*)local + off::m_iObserverMode) == 0) {
#ifdef _WIN64
            scoped = (*reinterpret_cast<const int*>((const char*)local + off::m_bIsScoped) != 0);
#else
            int const fov = *reinterpret_cast<const int*>((const char*)local + off::m_iFOV);
            scoped = (fov > 0 && fov < 90);
#endif
        }
        if (scoped) {
            if (!cfg.custom_scope) {
                float const cx = floorf((float)w * 0.5f) + 0.5f;
                float const cy = floorf((float)h * 0.5f) + 0.5f;
                ImU32 const col = IM_COL32(0, 0, 0, 255);
                draw->AddLine(ImVec2(0.f, cy), ImVec2((float)w, cy), col);
                draw->AddLine(ImVec2(cx, 0.f), ImVec2(cx, (float)h), col);
            } else {
                float const cx = floorf((float)w * 0.5f);
                float const cy = floorf((float)h * 0.5f);
                float const inner = (float)(cfg.scope_gap + 12);
                float const outer = inner + (float)cfg.scope_length;
                ImVec4 c(cfg.custom_scope_color[0], cfg.custom_scope_color[1],
                         cfg.custom_scope_color[2], cfg.custom_scope_color[3]);
                ImVec4 tr(c.x, c.y, c.z, 0.f);
                ImU32 const near_col = ImGui::ColorConvertFloat4ToU32(cfg.scope_invert ? tr : c);
                ImU32 const far_col  = ImGui::ColorConvertFloat4ToU32(cfg.scope_invert ? c : tr);
                draw->AddRectFilledMultiColor(ImVec2(cx + inner, cy), ImVec2(cx + outer, cy + 1.f), near_col, far_col, far_col, near_col);
                draw->AddRectFilledMultiColor(ImVec2(cx - outer + 1.f, cy), ImVec2(cx - inner + 1.f, cy + 1.f), far_col, near_col, near_col, far_col);
                draw->AddRectFilledMultiColor(ImVec2(cx, cy + inner), ImVec2(cx + 1.f, cy + outer), near_col, near_col, far_col, far_col);
                draw->AddRectFilledMultiColor(ImVec2(cx, cy - outer + 1.f), ImVec2(cx + 1.f, cy - inner + 1.f), far_col, far_col, near_col, near_col);
            }
        }
    }

    if (cfg.watermark) {
        char text[96];
        snprintf(text, sizeof(text), "islamware  |  %.0f fps", io.Framerate);
        ImVec2 const sz = ImGui::CalcTextSize(text);
        ImVec2 const pos((float)w - sz.x - 16.0f, 14.0f);
        ImVec2 const mn(pos.x - 8, pos.y - 5);
        ImVec2 const mx(pos.x + sz.x + 8, pos.y + sz.y + 5);
        // dark glow: wide soft halo fading outwards around the body
        for ( int i = 6; i >= 1; i-- ) {
            float const e = i * 3.0f;
            draw->AddRectFilled({ mn.x - e, mn.y - e }, { mx.x + e, mx.y + e },
                                IM_COL32(1, 1, 1, 12 + (6 - i) * 7), 11);
        }
        draw->AddRectFilled(mn, mx, IM_COL32(1, 1, 1, 235), 7);
        draw->AddText(pos, IM_COL32(235, 235, 240, 255), text);
    }

    // custom miss feed: "Missed shot due to " in white, the reason and
    // target in the highlight colour, stacked bottom-left, fading out
    if (cfg.log_misses && g_miss_feed_count > 0) {
        ULONGLONG const now = GetTickCount64();
        float const feed_bottom = (float)h - 36.0f;
        for (int i = 0; i < g_miss_feed_count; i++) {
            ULONGLONG const age = now - g_miss_feed[i].t;
            if (age > 6000) continue;
            float const a = age > 5000 ? 1.0f - (float)(age - 5000) / 1000.0f : 1.0f;
            ImVec2 const psz = ImGui::CalcTextSize(g_miss_feed[i].prefix);
            ImVec2 const rsz = ImGui::CalcTextSize(g_miss_feed[i].rest);
            float const total = psz.x + rsz.x;
            float const y = feed_bottom - (float)i * (psz.y + 2.0f);
            draw->AddText(ImVec2(19.0f, y + 1.0f), IM_COL32(0, 0, 0, (int)(150 * a)), g_miss_feed[i].prefix);
            draw->AddText(ImVec2(18.0f, y), IM_COL32(235, 235, 240, (int)(240 * a)), g_miss_feed[i].prefix);
            draw->AddText(ImVec2(19.0f + psz.x, y + 1.0f), IM_COL32(0, 0, 0, (int)(150 * a)), g_miss_feed[i].rest);
            draw->AddText(ImVec2(18.0f + psz.x, y), EspFeedColor(a), g_miss_feed[i].rest);
        }
        // compact the feed
        int n = 0;
        for (int i = 0; i < g_miss_feed_count; i++)
            if (now - g_miss_feed[i].t <= 6000)
                g_miss_feed[n++] = g_miss_feed[i];
        g_miss_feed_count = n;
    }

    if ((cfg.draw_aim_fov || cfg.esp_aimbot_visual) && cfg.aimbot && view_fov > 1.0f) {
        const float pi = 3.14159265358979323846f;
        float const half_h = h * 0.5f;
        float const r = tanf(cfg.aim_fov * pi / 180.0f) /
                        tanf(view_fov * 0.5f * pi / 180.0f) * half_h;
        if (r > 2.0f && r < 4000.0f) {
            float const* c = cfg.esp_aimbot_visual ? cfg.esp_aimbot_color : nullptr;
            ImU32 const col = c ? IM_COL32((int)(c[0] * 255), (int)(c[1] * 255), (int)(c[2] * 255), 200)
                                : IM_COL32(230, 230, 235, 190);
            draw->AddCircle(ImVec2(w * 0.5f, h * 0.5f), r, col, 64, 1.2f);
        }
    }
}

static void RenderOverlay(IDirect3DDevice9* device) {
    if (!g_imgui_init || !device) return;

    // No ImGui window-layout persistence: the ImGui menu must always open at
    // its default size/position instead of restoring imgui.ini leftovers.
    {
        static bool ini_disabled = false;
        if (!ini_disabled) {
            ini_disabled = true;
            ImGui::GetIO().IniFilename = nullptr;
        }
    }

    // Layout against the render target the game actually presents.  With
    // DXVK the swap-chain backbuffer can be larger than the scene target;
    // drawing against the backbuffer put the menu off the visible area.
    alice_ui::PrepareFrame();

    int target_w = 0, target_h = 0;
    {
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt) {
            D3DSURFACE_DESC desc;
            rt->GetDesc(&desc);
            rt->Release();
            target_w = (int)desc.Width;
            target_h = (int)desc.Height;
        }
    }

    if (target_w > 0 && target_h > 0) {
        D3DVIEWPORT9 vp = { 0, 0, (DWORD)target_w, (DWORD)target_h, 0.0f, 1.0f };
        device->SetViewport(&vp);
    }

    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX9_NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    if (target_w > 0 && target_h > 0 && io.DisplaySize.x > 1.0f && io.DisplaySize.y > 1.0f) {
        float sx = (float)target_w / io.DisplaySize.x;
        float sy = (float)target_h / io.DisplaySize.y;
        if (fabsf(sx - 1.0f) > 0.01f || fabsf(sy - 1.0f) > 0.01f) {
            io.MousePos.x *= sx;
            io.MousePos.y *= sy;
        }
        io.DisplaySize = ImVec2((float)target_w, (float)target_h);
    }

    ImGui::NewFrame();

    // ImGui's software cursor needs cursor textures from the default font
    // atlas; this build uses a custom atlas, so it drew garbage.  Keep it off
    // and draw a small accent dot below instead.
    ImGui::GetIO().MouseDrawCursor = false;

    // Show the normal system arrow while the menu is open.  Bump ShowCursor
    // once on open and undo it once on close so we do not fight the game each
    // frame (which was what caused the cursor to shimmer).
    // Hide the real cursor while the menu is up and draw our own arrow.  The
    // game re-hides/re-centers the system cursor every frame, which made the
    // hardware cursor shimmer; a draw-list cursor is unaffected by that.
    static bool cursor_was_open = false;
    static int cursor_hidden = 0;
    if (cfg.menu_open != cursor_was_open) {
        if (cfg.menu_open) {
            // Count every call, including the one that finally makes the
            // counter negative, otherwise the cursor stays hidden on close.
            cursor_hidden = 0;
            for (int i = 0; i < 8; i++) {
                int const count = ShowCursor(FALSE);
                cursor_hidden++;
                if (count < 0)
                    break;
            }
            SetCursor(nullptr);
        } else {
            for (int i = 0; i < cursor_hidden; i++)
                ShowCursor(TRUE);
            // Sweep any extra hides the game added while the menu was open.
            for (int i = 0; i < 8 && ShowCursor(TRUE) < 0; i++) {}
            cursor_hidden = 0;
            SetCursor(LoadCursorA(nullptr, IDC_ARROW));
        }
        cursor_was_open = cfg.menu_open;
    }
    static bool menu_style_logged = false;
    if (!menu_style_logged) {
        menu_style_logged = true;
        Log("[menu] style: %s (menu_imgui=%d)", cfg.menu_imgui ? "imgui" : "alicekit", cfg.menu_imgui ? 1 : 0);
    }
    if (cfg.menu_imgui) {
        if (cfg.menu_open)
            ImguiMenuRender();
        // hud islands: the hotkey window also spawns while the menu is closed
        // (whenever a bind is engaged) - visibility is handled inside
        ImguiMenuIslands(ImGui::GetForegroundDrawList(),
                         (int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y);
    } else {
        if (!g_alice_init && device) {
            int sw = 0, sh = 0;
            engine.GetScreenSize(sw, sh);
            if (sw <= 0 || sh <= 0) { sw = 1920; sh = 1080; }
            alice_ui::Init(device, sw, sh);
            g_alice_init = true;
        }
        alice_ui::Render();
    }

    // night mode is world-pinned now: it darkens through r_ambientfraction /
    // r_3dsky in ApplyWorldVisuals instead of a fullscreen screen tint

    if ((cfg.esp || cfg.esp_skeleton || cfg.esp_bomb) && IsGameReady()) {
        SetStage("ESP");
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        draw->Flags &= ~(ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill);
        DrawESP(draw, (int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y);
    }

    if (IsGameReady()) {
        SetStage("Overlays");
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        int const w = (int)ImGui::GetIO().DisplaySize.x;
        int const h = (int)ImGui::GetIO().DisplaySize.y;
        Tracers_Draw(draw, w, h);
        KillFx_Draw(draw, w, h);
        Grenades_Draw(draw, w, h);
        PeekAssist_Draw(draw, w, h);
        DrawOverlays(draw, w, h);
    }

    if (cfg.menu_open) {
        ImVec2 const m = ImGui::GetIO().MousePos;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddCircleFilled(ImVec2(m.x + 1.5f, m.y + 1.5f), 5.0f, IM_COL32(0, 0, 0, 110), 28);
        fg->AddCircleFilled(m, 4.2f, IM_COL32(66, 150, 250, 255), 28);
        fg->AddCircle(m, 4.2f, IM_COL32(12, 12, 12, 220), 28, 1.2f);
        fg->AddCircleFilled(m, 1.2f, IM_COL32(255, 255, 255, 235), 12);
    }

    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}


static void InstallStaticPropTint();

static HRESULT WINAPI HookedEndScene(IDirect3DDevice9* device) {
    g_wd_frame_tick = (LONG)GetTickCount();
    SetStage("EndScene");
    g_device = device;

    if ( !g_imgui_init ) {
        if ( IsGameReady( ) )
            InitImGui( device );
    } else if ( g_imgui_device != device ) {
        Log("[d3d9] device changed %p -> %p, rebinding ImGui DX9", g_imgui_device, device);
        if (g_alice_init) {
            alice_ui::Shutdown();
            g_alice_init = false;
        }
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplDX9_Init(device);
        g_imgui_device = device;
    }

    ApplyFeatureBinds();
    UpdateThirdPerson();
    InstallStaticPropTint();
    static DWORD last_world_fx_tick = 0;
    DWORD const now_es = GetTickCount();
    if (now_es - last_world_fx_tick >= 50) {
        last_world_fx_tick = now_es;
        SetStage("WorldVisuals");
        ApplyWorldVisuals();
        WorldFx_ApplyWalls();
        SetStage("ChamsOcclusion");
        UpdateChamsOcclusion();
    }
    UpdateHitChams();

    RenderOverlay(device);
    SetStage("idle");

    return oEndScene ? oEndScene(device) : D3D_OK;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// static prop tint: static map props never reach DrawModelExecute (they are
// drawn by CStaticProp::DrawModel in engine.dll), so hook that function.  The
// engine recomputes the modulation colour from the prop's ambient lighting
// immediately before drawing, which overwrites anything set from the hook -
// the three writes are redirected through tiny stubs that multiply the
// computed colour by the configured tint first (1.0 = unchanged).
typedef int (__fastcall* StaticPropDrawModelFn)(void*, int);
static StaticPropDrawModelFn oStaticPropDrawModel = nullptr;

// tint factors read by the engine-side stores; 1.0 / disabled by default
static float g_prop_tint_rgb[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static uintptr_t g_prop_stub_mem = 0;

static int __fastcall HookedStaticPropDrawModel(void* self, int flags) {
    static int calls = 0;
    if (++calls == 1)
        Log("[world] static prop DrawModel invoked (flags=%d)", flags);

    if (cfg.world_props && render_view.self) {
        float a = cfg.prop_color[3];
        if (a < 0.05f) a = 0.05f;
        if (a > 1.0f) a = 1.0f;
        g_prop_tint_rgb[0] = cfg.prop_color[0];
        g_prop_tint_rgb[1] = cfg.prop_color[1];
        g_prop_tint_rgb[2] = cfg.prop_color[2];
        static const float white[3] = { 1.0f, 1.0f, 1.0f };
        render_view.SetColorModulation(cfg.prop_color);
        render_view.SetBlend(a);
        int const r = oStaticPropDrawModel ? oStaticPropDrawModel(self, flags) : 0;
        render_view.SetColorModulation(white);
        render_view.SetBlend(1.0f);
        return r;
    }
    g_prop_tint_rgb[0] = g_prop_tint_rgb[1] = g_prop_tint_rgb[2] = 1.0f;
    return oStaticPropDrawModel ? oStaticPropDrawModel(self, flags) : 0;
}

// rewrites `movss [rip+disp], xmm<src_reg>` at site so the value is multiplied
// by *factor first.  Verifies the original bytes (opcode, register and global
// target) before patching so a different engine build is left untouched.
static bool PatchPropStore(uintptr_t engine_base, uintptr_t site_rva, uintptr_t dest_rva,
                           float* factor, int src_reg, uintptr_t stub_addr) {
    unsigned char* site = (unsigned char*)(engine_base + site_rva);
    uintptr_t const dest_addr = engine_base + dest_rva;
    if (site[0] != 0xf3 || site[1] != 0x0f || site[2] != 0x11)
        return false;
    unsigned char const modrm = site[3];
    if ((modrm & 0xC7) != 0x05 || ((modrm >> 3) & 7) != src_reg)
        return false;
    int32_t disp = 0;
    memcpy(&disp, site + 4, 4);
    if ((uintptr_t)(engine_base + site_rva + 8 + (intptr_t)disp) != dest_addr)
        return false;

    unsigned char* code = (unsigned char*)stub_addr;
    int n = 0;
    auto b = [&](unsigned char v) { code[n++] = v; };
    auto imm64 = [&](uint64_t v) {
        b(0x48); b(0xB8);
        for (int k = 0; k < 8; k++) b((unsigned char)(v >> (k * 8)));
    };
    // sub rsp,0x18 ; mov [rsp],rax ; movups [rsp+8],xmm7
    b(0x48); b(0x83); b(0xEC); b(0x18);
    b(0x48); b(0x89); b(0x04); b(0x24);
    b(0x0F); b(0x11); b(0x7C); b(0x24); b(0x08);
    // mov rax,factor ; movss xmm7,[rax] ; mulss xmm<src>,xmm7
    imm64((uint64_t)factor);
    b(0xF3); b(0x0F); b(0x10); b(0x38);
    b(0xF3); b(0x0F); b(0x59); b(src_reg == 0 ? 0xC7 : 0xDF);
    // mov rax,dest ; movss [rax],xmm<src>
    imm64((uint64_t)dest_addr);
    b(0xF3); b(0x0F); b(0x11); b(src_reg == 0 ? 0x00 : 0x18);
    // movups xmm7,[rsp+8] ; mov rax,[rsp] ; add rsp,0x18
    b(0x0F); b(0x10); b(0x7C); b(0x24); b(0x08);
    b(0x48); b(0x8B); b(0x04); b(0x24);
    b(0x48); b(0x83); b(0xC4); b(0x18);
    // jmp back to the instruction after the original store
    uintptr_t const back = engine_base + site_rva + 8;
    uintptr_t const jmp_at = stub_addr + (uintptr_t)n;
    int64_t const rel = (int64_t)back - (int64_t)(jmp_at + 5);
    b(0xE9);
    for (int k = 0; k < 4; k++) b((unsigned char)((uint64_t)rel >> (k * 8)));

    DWORD old = 0;
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old))
        return false;
    int64_t const from = (int64_t)stub_addr - (int64_t)(engine_base + site_rva + 5);
    site[0] = 0xE9;
    for (int k = 0; k < 4; k++) site[1 + k] = (unsigned char)((uint64_t)from >> (k * 8));
    site[5] = 0x90;
    site[6] = 0x90;
    site[7] = 0x90;
    DWORD tmp = 0;
    VirtualProtect(site, 8, old, &tmp);
    return true;
}

static void InstallStaticPropTint() {
#ifndef _WIN64
    return;
#else
    if (!engine_module || oStaticPropDrawModel) return;
    uintptr_t const base = (uintptr_t)engine_module;
    uintptr_t const target = base + 0x203420; // CStaticProp::DrawModel

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t scan = base;
    uintptr_t const scan_end = base + 0x900000;
    while (scan < scan_end) {
        if (!VirtualQuery((LPCVOID)scan, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            uintptr_t const sect_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            for (uintptr_t p = scan; p + 8 <= sect_end; p += 8) {
                if (*(uintptr_t*)p == target) {
                    DWORD old = 0;
                    if (VirtualProtect((LPVOID)p, 8, PAGE_EXECUTE_READWRITE, &old)) {
                        oStaticPropDrawModel = (StaticPropDrawModelFn)target;
                        *(uintptr_t*)p = (uintptr_t)&HookedStaticPropDrawModel;
                        DWORD tmp = 0;
                        VirtualProtect((LPVOID)p, 8, old, &tmp);
                        Log("[world] static prop DrawModel hooked at %p", (void*)p);
                    }
                }
            }
        }
        scan = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    if (!oStaticPropDrawModel) {
        Log("[world] static prop DrawModel vtable pointer not found");
        return;
    }

    if (!g_prop_stub_mem)
        g_prop_stub_mem = (uintptr_t)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_EXECUTE_READWRITE);
    if (!g_prop_stub_mem) {
        Log("[world] prop tint stub allocation failed");
        return;
    }
    bool const r = PatchPropStore(base, 0x2038f5, 0x4713e8, &g_prop_tint_rgb[0], 0, g_prop_stub_mem + 0x40);
    bool const bl = PatchPropStore(base, 0x203918, 0x4713f0, &g_prop_tint_rgb[2], 3, g_prop_stub_mem + 0x100);
    bool const g = PatchPropStore(base, 0x203920, 0x4713ec, &g_prop_tint_rgb[1], 0, g_prop_stub_mem + 0x1c0);
    Log("[world] static prop tint stores r=%d g=%d b=%d", r ? 1 : 0, g ? 1 : 0, bl ? 1 : 0);
#endif
}

void InstallD3D9Hook() {
    HMODULE d3d9_module = LoadLibraryA("d3d9.dll");
    if (!d3d9_module) { Log("d3d9.dll not present"); return; }

    typedef IDirect3D9* (WINAPI* Direct3DCreate9Fn)(UINT);
    auto create9 = (Direct3DCreate9Fn)GetProcAddress(d3d9_module, "Direct3DCreate9");
    if (!create9) { Log("Direct3DCreate9 missing"); return; }

    IDirect3D9* d3d = create9(D3D_SDK_VERSION);
    if (!d3d) { Log("Direct3DCreate9 failed"); return; }

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "islamware_dummy";
    RegisterClassExA(&wc);

    HWND dummy = CreateWindowExA(0, "islamware_dummy", "", WS_POPUP, 0, 0, 8, 8,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy) { Log("dummy window failed"); d3d->Release(); return; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = dummy;
    pp.BackBufferWidth = 8;
    pp.BackBufferHeight = 8;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev) {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, dummy,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    }
    if (FAILED(hr) || !dev) {
        Log("dummy device failed (0x%08x)", hr);
        DestroyWindow(dummy);
        d3d->Release();
        return;
    }

    void** vt = *reinterpret_cast<void***>(dev);
    bool ok1 = PatchVtable(vt, 42, (void*)HookedEndScene, (void**)&oEndScene);
    bool ok2 = PatchVtable(vt, 16, (void*)HookedReset, (void**)&oReset);
    Log("d3d9 hooked: endscene=%d reset=%d vt=%p", ok1, ok2, vt);

    dev->Release();
    d3d->Release();
}
