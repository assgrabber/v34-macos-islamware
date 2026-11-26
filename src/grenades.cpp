#include "cheat.h"
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Grenade prediction + projectile markers (reference grenade feature, ported
// to this build's SDK).
//
// Prediction: while the local player holds HE / flash / smoke, the throw is
// simulated with the game's trace (gravity 0.4 * sv_gravity, 64 Hz, bounce
// elasticity 0.45, ~21 bounces) and drawn as a fading arc with bounce dots
// and a landing point.  HE/flash stop at the 1.5 s fuse; smoke stops when it
// comes to rest.  The landing ring is the effect radius (HE blast, smoke
// cloud).
//
// Markers: live grenade projectiles on the map get a small marker and their
// effect-radius ring, so an incoming HE/smoke is visible through walls.
// ---------------------------------------------------------------------------

namespace {

enum NadeType { NADE_NONE = 0, NADE_HE, NADE_FLASH, NADE_SMOKE };

constexpr float NADE_DT = 1.0f / 64.0f;
constexpr float NADE_ELASTICITY = 0.45f;
constexpr float NADE_MAX_SPEED = 3500.0f;
constexpr int   NADE_POINTS_MAX = 192;
constexpr int   NADE_BOUNCES_MAX = 24;
constexpr DWORD NADE_REBUILD_MS = 33;
constexpr int   LIVE_MAX = 16;

static ConVar* s_gravity = nullptr;

static float NadeGravity() {
    if (!s_gravity && cvar.self) s_gravity = cvar.FindVar("sv_gravity");
    float g = s_gravity ? s_gravity->f_value : 800.0f;
    if (!(g > 1.0f && g < 5000.0f)) g = 800.0f;
    return g * 0.4f;   // reference gravity scale
}

static const float* NadeColor(int type) {
    if (type == NADE_HE) return cfg.nade_color_he;
    if (type == NADE_FLASH) return cfg.nade_color_flash;
    return cfg.nade_color_smoke;
}

struct NadePath {
    bool   active = false;
    int    type = NADE_NONE;
    Vector points[NADE_POINTS_MAX];
    int    count = 0;
    Vector bounces[NADE_BOUNCES_MAX];
    int    bounce_count = 0;
    Vector end;
    bool   resting = false;
    DWORD  built = 0;
};
static NadePath g_path;
static NadePath g_display;   // smoothed path actually drawn

static void BlendDisplay();

struct LiveNade {
    Vector pos;
    int    type;
    bool   valid;
};
static LiveNade g_live[LIVE_MAX];
static DWORD g_live_scan = 0;

const char* NetworkClassName(int index) {
    void* nw = entity_list.GetClientNetworkable(index);
    if (!nw) return nullptr;
    void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
    if (!cc) return nullptr;
    const char* name = ClientClassName(cc);
    return name;
}

const char* EntityModelName(void* ent) {
    int const mi = *reinterpret_cast<int*>((char*)ent + off::m_nModelIndex);
    if (mi <= 0 || mi > 0xFFFF) return nullptr;
    const void* model = model_info.GetModel(mi);
    if (!model) return nullptr;
    return model_info.GetModelName(model);
}

int TypeFromModel(const char* model) {
    if (!model) return NADE_NONE;
    if (strstr(model, "flashbang") || strstr(model, "flash")) return NADE_FLASH;
    if (strstr(model, "smokegrenade") || strstr(model, "smoke")) return NADE_SMOKE;
    if (strstr(model, "hegrenade") || strstr(model, "fraggrenade") || strstr(model, "grenade"))
        return NADE_HE;
    return NADE_NONE;
}

int TypeFromClass(const char* cls) {
    if (!cls) return NADE_NONE;
    if (strstr(cls, "Flashbang")) return NADE_FLASH;
    if (strstr(cls, "SmokeGrenade")) return NADE_SMOKE;
    if (strstr(cls, "HEGrenade") || strstr(cls, "CBaseCSGrenade")) return NADE_HE;
    return NADE_NONE;
}

int HeldType(void* local) {
    int const handle = *reinterpret_cast<int*>((char*)local + off::m_hActiveWeapon);
    int const idx = handle & 0xFFF;
    if (idx <= 0) return NADE_NONE;

    // the held model name is the reliable discriminator
    // (v_eq_fraggrenade / v_eq_flashbang / v_eq_smokegrenade); the network
    // class can be the generic CBaseCSGrenade for all three
    void* weapon = entity_list.GetClientEntity(idx);
    if (weapon) {
        int const from_model = TypeFromModel(EntityModelName(weapon));
        if (from_model != NADE_NONE) return from_model;
    }
    return TypeFromClass(NetworkClassName(idx));
}

float NadeRadius(int type) {
    switch (type) {
        case NADE_HE:    return 350.0f;
        case NADE_FLASH: return 250.0f;
        case NADE_SMOKE: return 166.0f;
        default:         return 120.0f;
    }
}

const char* NadeName(int type) {
    switch (type) {
        case NADE_HE:    return "HE";
        case NADE_FLASH: return "FLASH";
        case NADE_SMOKE: return "SMOKE";
        default:         return "NADE";
    }
}

void DrawGroundCircle(ImDrawList* draw, const Vector& center, float radius,
                      ImU32 color, float thickness, int width, int height) {
    ImVec2 prev;
    bool have = false;
    for (int i = 0; i <= 48; i++) {
        float const a = (float)i / 48.0f * 6.2831853f;
        Vector const p = center + Vector(cosf(a) * radius, sinf(a) * radius, 0.0f);
        ImVec2 s;
        if (!WorldToScreen(p, s, width, height)) { have = false; continue; }
        if (have) draw->AddLine(prev, s, color, thickness);
        prev = s;
        have = true;
    }
}

void Simulate(void* local, int type, const Vector& start, const Vector& velocity) {
    g_path = NadePath{};
    g_path.type = type;
    g_path.active = true;
    g_path.built = GetTickCount();

    Vector pos = start;
    Vector vel = velocity;
    g_path.points[g_path.count++] = pos;

    int const max_steps = 64 * 6;
    float const gravity = NadeGravity();
    for (int step = 0; step < max_steps && g_path.count < NADE_POINTS_MAX; step++) {
        float const new_z = vel.z - gravity * NADE_DT;
        Vector const move(vel.x * NADE_DT, vel.y * NADE_DT,
                          (vel.z + new_z) * 0.5f * NADE_DT);
        vel.z = new_z;

        float const speed = sqrtf(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
        if (speed > NADE_MAX_SPEED) {
            float const k = NADE_MAX_SPEED / speed;
            vel.x *= k; vel.y *= k; vel.z *= k;
        }

        Vector const to = pos + move;
        float fraction = 1.0f;
        Vector end = to;
        Vector normal(0, 0, 1);
        TraceLine(pos, to, fraction, end, local, nullptr, nullptr, nullptr, &normal);

        if (fraction < 1.0f) {
            pos = end;
            if (g_path.bounce_count < NADE_BOUNCES_MAX)
                g_path.bounces[g_path.bounce_count++] = pos;

            float const backoff = (vel.x * normal.x + vel.y * normal.y + vel.z * normal.z) * 2.0f;
            Vector reflected(vel.x - normal.x * backoff,
                             vel.y - normal.y * backoff,
                             vel.z - normal.z * backoff);
            reflected = reflected * NADE_ELASTICITY;
            if (fabsf(reflected.x) < 10.0f) reflected.x = 0.0f;
            if (fabsf(reflected.y) < 10.0f) reflected.y = 0.0f;
            if (fabsf(reflected.z) < 10.0f) reflected.z = 0.0f;

            float const speed2 = reflected.x * reflected.x + reflected.y * reflected.y +
                                 reflected.z * reflected.z;
            if (normal.z > 0.7f && speed2 < 400.0f) {
                g_path.resting = true;
                g_path.points[g_path.count++] = pos;
                break;
            }
            vel = reflected;
            pos = end + normal * 0.5f;
        } else {
            pos = end;
        }

        if (type == NADE_HE || type == NADE_FLASH) {
            // fuse runs from the throw; stop the preview where it detonates
            float const elapsed = (float)(step + 1) * NADE_DT;
            if (elapsed >= 1.5f) break;
        }

        g_path.points[g_path.count++] = pos;
    }

    g_path.end = pos;
}

void RebuildHeld(void* local, int type) {
    Vector const eye = PlayerEye(local);
    Vector fwd;
    AngleVectors(real_view_angles, fwd);

    Vector const start = eye + fwd * 16.0f;
    Vector const carry = *reinterpret_cast<Vector*>((char*)local + off::m_vecVelocity) * 1.25f;
    float const speed = 675.0f;   // 750 * 0.9 at full throw strength
    Vector const velocity = fwd * speed + carry;

    Simulate(local, type, start, velocity);
    BlendDisplay();
}

// Blend the freshly simulated path into the drawn one.  Rebuilding 30x a
// second straight from the view makes the landing point twitch with every
// mouse movement; blending keeps it glued to the aim while removing the
// jitter (and snaps on the first build / grenade switch, so it never trails
// far behind).
static void BlendDisplay() {
    if (!g_display.active || g_display.type != g_path.type) {
        g_display = g_path;
        return;
    }

    float const k = 0.45f;
    int const n = g_path.count;
    int const common = n < g_display.count ? n : g_display.count;
    for (int i = 0; i < common; i++) {
        Vector& d = g_display.points[i];
        Vector const& s = g_path.points[i];
        d.x += (s.x - d.x) * k;
        d.y += (s.y - d.y) * k;
        d.z += (s.z - d.z) * k;
    }
    for (int i = common; i < n; i++) g_display.points[i] = g_path.points[i];
    g_display.count = n;

    g_display.end.x += (g_path.end.x - g_display.end.x) * k;
    g_display.end.y += (g_path.end.y - g_display.end.y) * k;
    g_display.end.z += (g_path.end.z - g_display.end.z) * k;
    if (n > 0) g_display.points[n - 1] = g_display.end;

    int const bn = g_path.bounce_count;
    int const bc = bn < g_display.bounce_count ? bn : g_display.bounce_count;
    for (int i = 0; i < bc; i++) {
        Vector& d = g_display.bounces[i];
        Vector const& s = g_path.bounces[i];
        d.x += (s.x - d.x) * k;
        d.y += (s.y - d.y) * k;
        d.z += (s.z - d.z) * k;
    }
    for (int i = bc; i < bn; i++) g_display.bounces[i] = g_path.bounces[i];
    g_display.bounce_count = bn;
    g_display.resting = g_path.resting;
    g_display.built = g_path.built;
}

void ScanLiveGrenades() {
    DWORD const now = GetTickCount();
    if (now - g_live_scan < 100) return;
    g_live_scan = now;

    memset(g_live, 0, sizeof(g_live));
    int hi = entity_list.GetHighestEntityIndex();
    if (hi > 2048) hi = 2048;

    int found = 0;
    for (int i = 1; i <= hi && found < LIVE_MAX; i++) {
        void* ent = entity_list.GetClientEntity(i);
        if (!ent) continue;
        const char* cls = NetworkClassName(i);
        if (!cls) continue;
        bool const projectile = strstr(cls, "CSGrenadeProjectile") ||
                                strstr(cls, "SmokeGrenadeProjectile") ||
                                strstr(cls, "ParticleSmokeGrenade");
        if (!projectile) continue;

        int type = TypeFromModel(EntityModelName(ent));
        if (type == NADE_NONE)
            type = strstr(cls, "Smoke") ? NADE_SMOKE : NADE_HE;

        Vector const pos = *reinterpret_cast<Vector*>((char*)ent + off::m_vecOrigin);
        if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) continue;

        g_live[found].pos = pos;
        g_live[found].type = type;
        g_live[found].valid = true;
        found++;
    }
}

}

void Grenades_Draw(ImDrawList* draw, int width, int height) {
    if (!draw || !IsGameReady()) return;

    if (cfg.nade_esp) {
        ScanLiveGrenades();
        for (int i = 0; i < LIVE_MAX; i++) {
            LiveNade const& n = g_live[i];
            if (!n.valid) continue;

            ImVec2 s;
            if (!WorldToScreen(n.pos, s, width, height)) continue;

            float const a = NadeColor(n.type)[3];
            float const* const nc = NadeColor(n.type);
            ImU32 const col = ImGui::ColorConvertFloat4ToU32(
                ImVec4(nc[0], nc[1], nc[2], a));
            ImU32 const dark = IM_COL32(0, 0, 0, (int)(180 * a));

            draw->AddRectFilled(ImVec2(s.x - 3, s.y - 3), ImVec2(s.x + 3, s.y + 3), dark, 1.0f);
            draw->AddRect(ImVec2(s.x - 3, s.y - 3), ImVec2(s.x + 3, s.y + 3), col, 1.0f);

            ImVec2 const label(s.x + 7, s.y - 16);
            draw->AddText(ImVec2(label.x + 1, label.y + 1), IM_COL32(0, 0, 0, (int)(200 * a)), NadeName(n.type));
            draw->AddText(label, col, NadeName(n.type));

            // effect radius on the ground under the projectile
            float fraction = 1.0f;
            Vector ground;
            TraceLine(n.pos, n.pos - Vector(0, 0, 96), fraction, ground, nullptr, nullptr);
            float const radius = NadeRadius(n.type);
            DrawGroundCircle(draw, ground + Vector(0, 0, 2), radius,
                             ImGui::ColorConvertFloat4ToU32(
                                 ImVec4(nc[0], nc[1], nc[2], a * 0.35f)),
                             1.0f, width, height);
        }
    }

    if (!cfg.nade_predict) { g_path.active = false; g_display.active = false; return; }
    void* local = GetLocalEntity();
    if (!local) return;
    if (*reinterpret_cast<char*>((char*)local + off::m_lifeState) != 0) return;

    int const type = HeldType(local);
    if (type == NADE_NONE) { g_path.active = false; g_display.active = false; return; }

    DWORD const now = GetTickCount();
    if (!g_path.active || g_path.type != type || now - g_path.built > NADE_REBUILD_MS)
        RebuildHeld(local, type);

    float const* const nc = NadeColor(type);
    float const alpha = nc[3];
    ImU32 const col = ImGui::ColorConvertFloat4ToU32(
        ImVec4(nc[0], nc[1], nc[2], alpha * 0.9f));

    NadePath const& path = g_display;
    ImVec2 prev;
    bool have = false;
    for (int i = 0; i < path.count; i++) {
        ImVec2 s;
        if (!WorldToScreen(path.points[i], s, width, height)) { have = false; continue; }
        if (have) draw->AddLine(prev, s, col, 1.7f);
        prev = s;
        have = true;
    }

    for (int i = 0; i < path.bounce_count; i++) {
        ImVec2 s;
        if (!WorldToScreen(path.bounces[i], s, width, height)) continue;
        draw->AddCircleFilled(s, 2.2f, IM_COL32(255, 255, 255, (int)(210 * alpha)), 8);
    }

    ImVec2 landing;
    if (WorldToScreen(path.end, landing, width, height)) {
        draw->AddCircleFilled(landing, 3.0f, col, 10);
        ImVec2 const label(landing.x + 6, landing.y - 8);
        draw->AddText(ImVec2(label.x + 1, label.y + 1), IM_COL32(0, 0, 0, (int)(200 * alpha)), NadeName(type));
        draw->AddText(label, col, NadeName(type));

        DrawGroundCircle(draw, path.end + Vector(0, 0, 1), NadeRadius(type),
                         ImGui::ColorConvertFloat4ToU32(
                             ImVec4(nc[0], nc[1], nc[2], alpha * 0.4f)),
                         1.0f, width, height);
    }
}