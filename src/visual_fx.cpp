#include "cheat.h"
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// x64 world colour + kill effects.
//
// Walls: IMaterialSystem::FindMaterial (vtable index 121) is hooked so every
// world material the engine ever requests is collected; the collected set is
// re-coloured each frame from the menu colour (it restores white when the
// feature is off).  Props are tinted in the DrawModelExecute hook instead.
//
// Kill effects: purely client-side overlay effects anchored at the victim's
// death position (jagged tesla arcs, expanding explosion, blood burst, gauss
// ring).  No engine effect calls are needed, so nothing here can desync.
// ---------------------------------------------------------------------------

// ------------------------------ walls --------------------------------------
TYPEDEF_THISCALL(void*, FindMaterialFn, void*, const char*, const char*, bool, const char*);
static FindMaterialFn oFindMaterial = nullptr;

// FindMaterial can be called from the material streaming thread while the
// render thread walks the list every frame.  A std::vector reallocating under
// that walk corrupts the heap (random AVs, then a hang), so the list is a
// fixed array behind a lock instead.
constexpr int WALL_MATS_MAX = 2048;
static void* g_wall_mats[WALL_MATS_MAX];
static volatile LONG g_wall_count = 0;

// prop materials (models/props/*) get the same treatment as walls: colour
// modulation on the material itself instead of re-drawing the model inside
// DrawModelExecute (the re-draw path corrupted the render pipeline).
constexpr int PROP_MATS_MAX = 2048;
static void* g_prop_mats[PROP_MATS_MAX];
static volatile LONG g_prop_count = 0;
static CRITICAL_SECTION g_wall_cs;
static bool g_wall_cs_ready = false;
static volatile LONG g_wall_applied = 0;   // 1 while the engine materials are tinted
static volatile LONG g_prop_applied = 0;
// last applied inputs, so the (up to 2048 entry) material walks only run when
// something actually changed instead of every frame
static float g_wall_color_applied[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static float g_prop_color_applied[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static float g_night_applied = 0.0f;
static float g_bright_applied = 0.0f;
static LONG g_wall_count_applied = 0;
static LONG g_prop_count_applied = 0;

static void PropListAdd(void* mat) {
    if (!g_wall_cs_ready) return;
    EnterCriticalSection(&g_wall_cs);
    if (g_prop_count < PROP_MATS_MAX) {
        bool found = false;
        for (LONG i = 0; i < g_prop_count; i++)
            if (g_prop_mats[i] == mat) { found = true; break; }
        if (!found) {
            if (g_prop_count == 0) Log("[worldfx] first prop material collected");
            g_prop_mats[g_prop_count++] = mat;
        }
    }
    LeaveCriticalSection(&g_wall_cs);
}

static void WallListAdd(void* mat) {
    if (!g_wall_cs_ready) return;
    EnterCriticalSection(&g_wall_cs);
    if (g_wall_count < WALL_MATS_MAX) {
        bool found = false;
        for (LONG i = 0; i < g_wall_count; i++)
            if (g_wall_mats[i] == mat) { found = true; break; }
        if (!found) {
            if (g_wall_count == 0) Log("[worldfx] first world material collected");
            g_wall_mats[g_wall_count++] = mat;
        }
    }
    LeaveCriticalSection(&g_wall_cs);
}

static float WorldLightScale() {
    float s = 1.0f - cfg.world_night * 0.85f + cfg.world_brightness * 0.35f;
    if (s < 0.05f) s = 0.05f;
    if (s > 1.5f) s = 1.5f;
    return s;
}

static void ApplyWallMaterial(void* mat) {
    if (!mat) return;
    IMaterial m; m.self = mat;
    bool const mod_light = cfg.world_night > 0.001f || cfg.world_brightness > 0.001f;
    if (cfg.world_walls || mod_light) {
        float const ls = WorldLightScale();
        float const r = (cfg.world_walls ? cfg.wall_color[0] : 1.0f) * ls;
        float const g = (cfg.world_walls ? cfg.wall_color[1] : 1.0f) * ls;
        float const b = (cfg.world_walls ? cfg.wall_color[2] : 1.0f) * ls;
        m.ColorModulate(r, g, b);
        float a = cfg.world_walls ? cfg.wall_color[3] : 1.0f;
        if (a < 0.05f) a = 0.05f;
        if (a > 1.0f) a = 1.0f;
        m.AlphaModulate(a);
    } else {
        m.ColorModulate(1.0f, 1.0f, 1.0f);
        m.AlphaModulate(1.0f);
    }
}

static void ApplyPropMaterial(void* mat) {
    if (!mat) return;
    IMaterial m; m.self = mat;
    bool const mod_light = cfg.world_night > 0.001f || cfg.world_brightness > 0.001f;
    if (cfg.world_props || mod_light) {
        float const ls = WorldLightScale();
        float const r = (cfg.world_props ? cfg.prop_color[0] : 1.0f) * ls;
        float const g = (cfg.world_props ? cfg.prop_color[1] : 1.0f) * ls;
        float const b = (cfg.world_props ? cfg.prop_color[2] : 1.0f) * ls;
        m.ColorModulate(r, g, b);
        float a = cfg.world_props ? cfg.prop_color[3] : 1.0f;
        if (a < 0.05f) a = 0.05f;
        if (a > 1.0f) a = 1.0f;
        m.AlphaModulate(a);
    } else {
        m.ColorModulate(1.0f, 1.0f, 1.0f);
        m.AlphaModulate(1.0f);
    }
}

static HOOK_THISCALL(void*, HookedFindMaterial, void*, const char* name, const char* group,
                                           bool complain, const char* prefix) {
    void* mat = oFindMaterial ? oFindMaterial(self, name, group, complain, prefix) : nullptr;
    if (mat && name) {
        // collect only - colours are applied from WorldFx_ApplyWalls on the
        // render thread.  Touching material vars from inside FindMaterial
        // (possibly under the material system's own lock) was the deadlock.
        if (group && strstr(group, "World") && !strstr(name, "models/player"))
            WallListAdd(mat);
        if ((group && strstr(group, "StaticProp")) || strstr(name, "props") || strstr(name, "Props"))
            PropListAdd(mat);
    }
    return mat;
}

void WorldFx_Install() {
    if (!material_system.self) return;
    if (!g_wall_cs_ready) {
        InitializeCriticalSection(&g_wall_cs);
        g_wall_cs_ready = true;
    }
    void** vt = *reinterpret_cast<void***>(material_system.self);
#ifdef _WIN64
    constexpr int FIND_MATERIAL_IDX = 121;
#else
    constexpr int FIND_MATERIAL_IDX = 27;
#endif
    if (!vt || !vt[FIND_MATERIAL_IDX]) { Log("[worldfx] FindMaterial slot missing"); return; }
    oFindMaterial = reinterpret_cast<FindMaterialFn>(vt[FIND_MATERIAL_IDX]);
    DWORD old = 0;
    if (VirtualProtect(&vt[FIND_MATERIAL_IDX], sizeof(void*), PAGE_READWRITE, &old)) {
        vt[FIND_MATERIAL_IDX] = reinterpret_cast<void*>(&HookedFindMaterial);
        VirtualProtect(&vt[FIND_MATERIAL_IDX], sizeof(void*), old, &old);
        Log("[worldfx] FindMaterial hooked, %p", (void*)vt[FIND_MATERIAL_IDX]);
    }
}

void WorldFx_ApplyWalls() {
    if (!g_wall_cs_ready) return;

    bool const mod_light = cfg.world_night > 0.001f || cfg.world_brightness > 0.001f;
    LONG const want_wall = (cfg.world_walls || mod_light) ? 1 : 0;
    LONG const want_prop = (cfg.world_props || mod_light) ? 1 : 0;
    bool const wall_dirty = want_wall != (LONG)g_wall_applied;
    bool const prop_dirty = want_prop != (LONG)g_prop_applied;
    bool const light_dirty = (cfg.world_night != g_night_applied) || (cfg.world_brightness != g_bright_applied);
    if (!wall_dirty && !prop_dirty && !light_dirty && !want_wall && !want_prop) return;

    // snapshot the lists under the lock, then call into the material system
    // with the lock released (no lock-order inversion with FindMaterial)
    static void* walls[WALL_MATS_MAX];
    static void* props[PROP_MATS_MAX];
    LONG wall_count = 0, prop_count = 0;
    EnterCriticalSection(&g_wall_cs);
    wall_count = g_wall_count;
    if (wall_count > WALL_MATS_MAX) wall_count = WALL_MATS_MAX;
    if (wall_count) memcpy(walls, g_wall_mats, sizeof(void*) * (size_t)wall_count);
    prop_count = g_prop_count;
    if (prop_count > PROP_MATS_MAX) prop_count = PROP_MATS_MAX;
    if (prop_count) memcpy(props, g_prop_mats, sizeof(void*) * (size_t)prop_count);
    LeaveCriticalSection(&g_wall_cs);

    // only touch the materials when the toggle, the colour or the collected
    // set changed - re-applying to every material on every frame was a
    // path-length vtable hammer once the list filled up during play
    bool const wall_color = want_wall &&
        memcmp(g_wall_color_applied, cfg.wall_color, sizeof(float) * 4) != 0;
    bool const prop_color = want_prop &&
        memcmp(g_prop_color_applied, cfg.prop_color, sizeof(float) * 4) != 0;
    bool const wall_list = want_wall && wall_count != g_wall_count_applied;
    bool const prop_list = want_prop && prop_count != g_prop_count_applied;

    if (wall_dirty) g_wall_applied = want_wall;
    if (prop_dirty) g_prop_applied = want_prop;
    if (light_dirty) {
        g_night_applied = cfg.world_night;
        g_bright_applied = cfg.world_brightness;
    }

    if (wall_dirty || wall_color || wall_list || light_dirty)
        for (LONG i = 0; i < wall_count; i++) ApplyWallMaterial(walls[i]);
    if (prop_dirty || prop_color || prop_list || light_dirty)
        for (LONG i = 0; i < prop_count; i++) ApplyPropMaterial(props[i]);

    if (wall_dirty || wall_color || wall_list) {
        g_wall_count_applied = wall_count;
        memcpy(g_wall_color_applied, cfg.wall_color, sizeof(float) * 4);
    }
    if (prop_dirty || prop_color || prop_list) {
        g_prop_count_applied = prop_count;
        memcpy(g_prop_color_applied, cfg.prop_color, sizeof(float) * 4);
    }
}

// --------------------------- kill effects ----------------------------------
namespace {
struct KillFx {
    Vector pos;
    Vector dir;
    DWORD  time;
    int    type;
};

constexpr int KILLFX_MAX = 24;
KillFx g_killfx[KILLFX_MAX];
int    g_killfx_next = 0;

float FxRand(DWORD seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return (float)(seed & 0xFFFF) / 65535.0f;
}
}

void KillFx_Clear() {
    memset(g_killfx, 0, sizeof(g_killfx));
    g_killfx_next = 0;
}

void KillFx_Spawn(const Vector& pos, const Vector& dir, int victim_index) {
    if (!cfg.kill_effects) return;
    KillFx& fx = g_killfx[g_killfx_next % KILLFX_MAX];
    g_killfx_next++;
    fx.pos = pos;
    fx.dir = dir;
    fx.time = GetTickCount();
    fx.type = cfg.kill_effect_type;
    (void)victim_index;
}

void KillFx_Draw(ImDrawList* draw, int width, int height) {
    if (!cfg.kill_effects || !draw) return;

    DWORD now = GetTickCount();
    float const life_ms = (cfg.kill_effect_time > 0.2f ? cfg.kill_effect_time : 0.2f) * 1000.0f;
    float const scale = (cfg.kill_effect_scale > 0.1f ? cfg.kill_effect_scale : 0.1f);
    float const cr = cfg.kill_effect_color[0];
    float const cg = cfg.kill_effect_color[1];
    float const cb = cfg.kill_effect_color[2];

    for (int i = 0; i < KILLFX_MAX; i++) {
        KillFx const& fx = g_killfx[i];
        if (!fx.time) continue;
        DWORD const age = now - fx.time;
        if (age > life_ms) continue;

        ImVec2 center;
        if (!WorldToScreen(fx.pos, center, width, height)) continue;

        float const t = (float)age / life_ms;          // 0..1
        float const fade = 1.0f - t;
        ImU32 const col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(cr, cg, cb, cfg.kill_effect_color[3] * fade));

        // pixel radius from a world-space offset so the effect scales with
        // distance like a real world effect
        Vector edge = fx.pos + Vector(48.0f * scale, 0.0f, 0.0f);
        ImVec2 edge_screen;
        float radius = 40.0f * scale;
        if (WorldToScreen(edge, edge_screen, width, height)) {
            float dx = edge_screen.x - center.x;
            float dy = edge_screen.y - center.y;
            radius = sqrtf(dx * dx + dy * dy);
        }
        if (radius < 6.0f) radius = 6.0f;

        switch (fx.type) {
            case 0: { // Tesla: jagged arcs around the victim
                DWORD seed = (DWORD)(fx.time * 2654435761u);
                for (int arc = 0; arc < 8; arc++) {
                    float a0 = FxRand(seed + arc * 7919) * 6.2831853f;
                    float len = radius * (0.8f + 0.9f * FxRand(seed + arc * 104729));
                    ImVec2 from = center;
                    for (int seg = 1; seg <= 4; seg++) {
                        float frac = (float)seg / 4.0f;
                        float jitter = radius * 0.25f * (FxRand(seed + arc * 31 + seg * 17) - 0.5f);
                        ImVec2 to(center.x + cosf(a0) * len * frac + jitter,
                                  center.y + sinf(a0) * len * frac + jitter * 0.5f);
                        // shine: soft glow passes + bright core on every arc
                        draw->AddLine(from, to, ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 0.10f * fade)), 7.0f);
                        draw->AddLine(from, to, ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 0.22f * fade)), 3.6f);
                        draw->AddLine(from, to, ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 0.45f * fade)), 2.0f);
                        draw->AddLine(from, to, col, 1.1f);
                        draw->AddLine(from, to, IM_COL32(255, 255, 255, (int)(170 * fade)), 0.6f);
                        from = to;
                    }
                }
                draw->AddCircleFilled(center, radius * 0.30f * fade,
                                      ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 0.10f * fade)), 16);
                draw->AddCircleFilled(center, radius * 0.18f * fade, col, 12);
                draw->AddCircleFilled(center, radius * 0.07f * fade, IM_COL32(255, 255, 255, (int)(220 * fade)), 10);
                break;
            }
            case 1: { // Explosion: expanding ring + debris dots
                float rr = radius * (0.35f + 1.1f * t);
                draw->AddCircle(center, rr, col, 24, 2.2f);
                draw->AddCircle(center, rr * 0.65f,
                                ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 0.5f * fade)), 20, 1.4f);
                DWORD seed = (DWORD)(fx.time * 2246822519u);
                for (int d = 0; d < 10; d++) {
                    float a = FxRand(seed + d * 97) * 6.2831853f;
                    float dist = rr * (0.9f + 0.5f * FxRand(seed + d * 53));
                    draw->AddCircleFilled(ImVec2(center.x + cosf(a) * dist, center.y + sinf(a) * dist),
                                          radius * 0.07f * fade, col, 6);
                }
                break;
            }
            case 2: { // Blood: red spatter
                DWORD seed = (DWORD)(fx.time * 3266489917u);
                for (int b = 0; b < 14; b++) {
                    float a = FxRand(seed + b * 71) * 6.2831853f;
                    float dist = radius * (0.2f + 0.9f * t) * (0.4f + FxRand(seed + b * 29));
                    float sz = radius * (0.05f + 0.08f * FxRand(seed + b * 13)) * (1.0f - t * 0.5f);
                    ImU32 bc = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(0.75f, 0.02f, 0.02f, (0.85f - 0.6f * t) * cfg.kill_effect_color[3]));
                    draw->AddCircleFilled(ImVec2(center.x + cosf(a) * dist, center.y + sinf(a) * dist),
                                          sz, bc, 8);
                }
                break;
            }
            default: { // Gauss: ring + vertical beam
                float rr = radius * (0.3f + 0.8f * t);
                draw->AddCircle(center, rr, col, 28, 2.0f);
                draw->AddLine(ImVec2(center.x, center.y - radius * 2.5f * fade),
                              ImVec2(center.x, center.y + radius * 0.4f), col, 2.4f);
                draw->AddCircleFilled(center, radius * 0.22f * fade, col, 12);
                break;
            }
        }
    }
}