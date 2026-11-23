#include "cheat.h"

// ---------------------------------------------------------------------------
// Bullet impacts + tracers — ported 1:1 from the reference source.
//
// Client-side markers (red): the local weapon's clip counter is the ground
// truth for "a bullet actually left the gun".  When it drops between two
// CreateMove calls we trace the outgoing shot angle that produced it (silent
// aim angle when silent aim fired) and place the marker.  Clicks with an
// empty gun, knife swings, grenade throws, reloads and clicks during the fire
// delay do not decrement the clip, so they never spawn a marker.
//
// Server-side markers (blue): the "bullet_impact" game event carries the
// authoritative position the server registered.  Own shots get a blue box.
//
// Tracers: from the shooter's eye to each impact, merged along the same ray
// (the reference onBulletImpact).
// ---------------------------------------------------------------------------

namespace {
struct Tracer {
    Vector start;
    Vector end;
    DWORD  time;
    int    shooter;
    bool   used;
};

constexpr int TRACER_MAX = 64;
Tracer g_tracers[TRACER_MAX];
int    g_tracer_next = 0;

// last outgoing shot command (angles that go to the server, silent aim
// included) - used to trace the client marker when the round is consumed
QAngle g_pending_angles;
Vector g_pending_eye;
DWORD  g_pending_tick = 0;
bool   g_pending = false;

int g_last_weapon = 0;
int g_last_clip = -1;

// only used by the diagnostic probe (reference macOS offset, unverified here)
constexpr uintptr_t CLIENT_IMPACTS_OFFSET = 0x125E8;
constexpr size_t    CLIENT_IMPACT_STRIDE  = 20;

struct RawVector {
    unsigned char* memory;
    int size;
    int capacity;
};

bool Readable(const void* ptr, size_t bytes) {
    if (!ptr || !bytes) return false;
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t const end = p + bytes;
    if (end < p) return false;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void*)p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        uintptr_t const region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return false;
        p = region_end;
    }
    return true;
}

bool PlausiblePos(const Vector& v) {
    return v.x > -16384.0f && v.x < 16384.0f &&
           v.y > -16384.0f && v.y < 16384.0f &&
           v.z > -16384.0f && v.z < 16384.0f;
}

int ColorByte(float v, float alpha) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (int)(v * 255.0f * alpha);
}

void DrawImpactBox(const Vector& pos, const float color[4]) {
    if (!debug_overlay.self) return;
    // cfg.impact_size is the slider value; the box half-extent is scaled down
    // so the default marker is a small dot-like box instead of a brick
    float extent = cfg.impact_size * 0.6f;
    if (extent < 0.5f) extent = 0.5f;
    if (extent > 12.0f) extent = 12.0f;
    Vector const box(extent, extent, extent);
    Vector const neg(-extent, -extent, -extent);
    QAngle const angles(0.0f, 0.0f, 0.0f);
    float duration = cfg.impact_time > 0.1f ? cfg.impact_time : 0.1f;
    int r = ColorByte(color[0], 1.0f);
    int g = ColorByte(color[1], 1.0f);
    int b = ColorByte(color[2], 1.0f);
    int a = ColorByte(color[3], 1.0f);
    if (a <= 0) a = 255;
    debug_overlay.AddBoxOverlay(pos, neg, box, angles, r, g, b, a, duration);
}

ImU32 FadeColor(const float color[4], float alpha) {
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(color[0], color[1], color[2], color[3] * alpha));
}
}

void Impacts_Clear() {
    memset(g_tracers, 0, sizeof(g_tracers));
    g_tracer_next = 0;
}

static bool TracerLengthOk(Vector const& start, Vector const& end);

// Called at the start of every base client CreateMove, i.e. right after the
// previous command was predicted.  A clip drop means the previous attack
// command really fired a round.
void Impacts_TrackShot() {
    if (!cfg.bullet_impacts) { g_pending = false; return; }
    void* local = GetLocalEntity();
    if (!local) { g_pending = false; g_last_weapon = 0; g_last_clip = -1; return; }
    if (*reinterpret_cast<char*>((char*)local + off::m_lifeState) != 0) {
        g_pending = false; g_last_clip = -1; return;
    }

    int const handle = *reinterpret_cast<int*>((char*)local + off::m_hActiveWeapon);
    int const index = handle & 0xFFF;
    void* weapon = index > 0 ? entity_list.GetClientEntity(index) : nullptr;
    if (!weapon) { g_pending = false; g_last_weapon = 0; g_last_clip = -1; return; }

    int const clip = *reinterpret_cast<int*>((char*)weapon + off::w_iClip1);
    if (index != g_last_weapon) {
        // weapon switch: rebaseline, the first shot of the new weapon still
        // counts because the pending angle is recorded after this call
        g_last_weapon = index;
        g_last_clip = clip;
        g_pending = false;
        return;
    }

    bool const fired = g_last_clip >= 0 && clip >= 0 && clip < g_last_clip;
    g_last_clip = clip;
    if (!fired) return;

    if (!g_pending || GetTickCount() - g_pending_tick > 300) {
        g_pending = false;
        return;
    }
    g_pending = false;

    Vector fwd;
    AngleVectors(g_pending_angles, fwd);
    Vector const from = g_pending_eye;
    Vector const to = from + fwd * 8192.0f;
    float fraction = 1.0f;
    Vector end = to;
    if (!TraceLine(from, to, fraction, end, local, nullptr)) return;
    // fraction >= 1.0 means the ray hit nothing (sky/void) - don't draw a
    // bogus 8192-unit tracer from extreme distance
    if (fraction >= 1.0f) return;
    if (!TracerLengthOk(from, end)) return;
    DrawImpactBox(end, cfg.impact_color);

    // the local tracer is created here, the moment the round is consumed -
    // waiting for the server's bullet_impact event made tracers appear late
    if (cfg.tracers) {
        int const local_index = engine.GetLocalPlayer();
        Tracer& tr = g_tracers[g_tracer_next % TRACER_MAX];
        g_tracer_next++;
        tr.used = true;
        tr.time = GetTickCount();
        tr.shooter = local_index;
        tr.start = from;
        tr.end = end;
    }
}

// Called at the end of CreateMove with the final outgoing command angles, so
// the marker follows the silent-aim ray instead of the camera.
void Impacts_RecordShot(CUserCmd* cmd, void* local) {
    if (!cfg.bullet_impacts || !cmd || !local) return;
    if (*reinterpret_cast<char*>((char*)local + off::m_lifeState) != 0) return;
    if (!(cmd->buttons & IN_ATTACK)) return;
    g_pending_eye = PlayerEye(local);
    g_pending_angles = cmd->viewangles;
    g_pending_tick = GetTickCount();
    g_pending = true;
}

static bool TracerLengthOk(Vector const& start, Vector const& end) {
    Vector const d = end - start;
    float const len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    // reject degenerate or absurdly long traces (sky/void hits can report far points)
    return len2 > 1.0f && len2 < 9000.0f * 9000.0f;
}

void Impacts_OnServerImpact(const Vector& pos, int shooter_index) {
    if (shooter_index < 1 || shooter_index > 64) return;

    int const local_index = engine.GetLocalPlayer();
    if (cfg.bullet_impacts && shooter_index == local_index)
        DrawImpactBox(pos, cfg.impact_server_color);

    if (!cfg.tracers) return;

    void* shooter = entity_list.GetClientEntity(shooter_index);
    if (!shooter) return;
    Vector const start = PlayerEye(shooter);
    if (!TracerLengthOk(start, pos)) return;

    // reference behaviour: if this impact continues the last ray of the same shooter,
    // extend that tracer instead of adding a new one
    for (int k = 1; k <= TRACER_MAX; k++) {
        int const i = (g_tracer_next - k + TRACER_MAX * 2) % TRACER_MAX;
        Tracer& last = g_tracers[i];
        if (!last.used || last.shooter != shooter_index) continue;

        Vector const previous = last.end - last.start;
        Vector const current = pos - last.start;
        float const plen = sqrtf(previous.x * previous.x + previous.y * previous.y + previous.z * previous.z);
        float const clen = sqrtf(current.x * current.x + current.y * current.y + current.z * current.z);
        if (plen > 0.0f && clen > plen) {
            float const dot = (previous.x * current.x + previous.y * current.y + previous.z * current.z) / (plen * clen);
            if (dot > 0.9995f && TracerLengthOk(last.start, pos)) {
                last.end = pos;
                last.time = GetTickCount();
                return;
            }
        }
        break;
    }

    Tracer& tr = g_tracers[g_tracer_next % TRACER_MAX];
    g_tracer_next++;
    tr.used = true;
    tr.time = GetTickCount();
    tr.shooter = shooter_index;
    tr.start = start;
    tr.end = pos;
}

// linear interpolation between two packed IM_COL32 colours
static ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int const ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF, aa = (a >> 24) & 0xFF;
    int const br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF, ba = (b >> 24) & 0xFF;
    return IM_COL32(ar + (int)((br - ar) * t), ag + (int)((bg - ag) * t),
                    ab + (int)((bb - ab) * t), aa + (int)((ba - aa) * t));
}

// a straight line whose colour fades from ca at the start to cb at the end
static void GradientLine(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 ca, ImU32 cb,
                         float thickness) {
    int const steps = 16;
    ImVec2 prev = a;
    for (int s = 1; s <= steps; s++) {
        float const t = (float)s / (float)steps;
        ImVec2 const p(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
        draw->AddLine(prev, p, LerpColor(ca, cb, t - 0.5f / (float)steps), thickness);
        prev = p;
    }
}

static bool TracerPointVisible(Vector const& eye, Vector const& point, void* skip) {
    // World-only mask (solid / window / grate brushes).  Tracer visibility must
    // depend on walls, not on player hitboxes that happen to sit between the
    // eye and the beam (the default shot mask contains CONTENTS_HITBOX, which
    // made tracers of a visible shooter read as blocked by their own body).
    constexpr unsigned int MASK_WORLD = 0x1u | 0x2u | 0x8u;
    float fraction = 1.0f;
    Vector hit = point;
    // TraceLine returns false both when the ray reached the point (nothing
    // hit) and on a failed trace; fraction stays 1.0 either way.
    TraceLine(eye, point, fraction, hit, skip, nullptr, nullptr, nullptr, nullptr, MASK_WORLD);
    return fraction >= 0.97f;
}

void Tracers_Draw(ImDrawList* draw, int width, int height) {
    if (!cfg.tracers || !draw) return;

    DWORD const now = GetTickCount();
    float const life_ms = (cfg.tracer_life > 0.1f ? cfg.tracer_life : 0.1f) * 1000.0f;

    int const local_index = engine.GetLocalPlayer();
    void* local_ent = local_index > 0 ? entity_list.GetClientEntity(local_index) : nullptr;
    int const local_team = local_ent
        ? *reinterpret_cast<int*>((char*)local_ent + off::m_iTeamNum) : 0;
    Vector const eye = local_ent ? PlayerEye(local_ent) : Vector(0,0,0);

    for (int i = 0; i < TRACER_MAX; i++) {
        Tracer const& tr = g_tracers[i];
        if (!tr.used) continue;
        DWORD const age = now - tr.time;
        if (age > life_ms) continue;
        if (!TracerLengthOk(tr.start, tr.end)) continue;

        // Only draw the tracer segment that is visible from the local eye.
        // Sampling along the beam prevents tracers being drawn through walls.
        // One sample roughly every 64 units catches thin cover; the cap keeps
        // the per-frame trace count bounded on long shots.
        constexpr int MAX_SAMPLES = 48;
        Vector const dir = tr.end - tr.start;
        float const len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
        if (len < 1.0f) continue;
        int samples = (int)(len / 64.0f);
        if (samples < 8) samples = 8;
        if (samples > MAX_SAMPLES - 1) samples = MAX_SAMPLES - 1;
        bool vis[MAX_SAMPLES] = {};
        Vector pts[MAX_SAMPLES];
        for (int s = 0; s <= samples; ++s) {
            float t = (float)s / (float)samples;
            pts[s] = tr.start + dir * t;
            vis[s] = local_ent ? TracerPointVisible(eye, pts[s], local_ent) : true;
        }

        int best_a = -1, best_b = -1, cur_a = -1;
        for (int s = 0; s <= samples; ++s) {
            if (vis[s]) {
                if (cur_a < 0) cur_a = s;
                int cur_b = s;
                if (best_a < 0 || (cur_b - cur_a) > (best_b - best_a)) { best_a = cur_a; best_b = cur_b; }
            } else {
                cur_a = -1;
            }
        }
        if (best_a < 0 || best_b <= best_a) continue;

        Vector const vstart = pts[best_a];
        Vector const vend = pts[best_b];
        Vector const seg = vend - vstart;
        if (seg.x*seg.x + seg.y*seg.y + seg.z*seg.z < 12.0f * 12.0f) continue;

        ImVec2 a, b;
        if (!WorldToScreen(vstart, a, width, height)) continue;
        if (!WorldToScreen(vend, b, width, height)) continue;

        float const alpha = 1.0f - (float)age / life_ms;

        float const* color = cfg.tracer_local_color;
        float const* color2 = cfg.tracer_local_color2;
        if (tr.shooter != local_index) {
            void* shooter = tr.shooter > 0 ? entity_list.GetClientEntity(tr.shooter) : nullptr;
            int const team = shooter
                ? *reinterpret_cast<int*>((char*)shooter + off::m_iTeamNum) : 0;
            if (team == local_team) {
                color = cfg.tracer_team_color;
                color2 = cfg.tracer_team_color2;
            } else {
                color = cfg.tracer_enemy_color;
                color2 = cfg.tracer_enemy_color2;
            }
        }

        ImU32 const end_col = FadeColor(color2, alpha);
        GradientLine(draw, a, b, FadeColor(color, alpha * 0.12f),
                     FadeColor(color2, alpha * 0.12f), 11.0f);
        GradientLine(draw, a, b, FadeColor(color, alpha * 0.28f),
                     FadeColor(color2, alpha * 0.28f), 6.5f);
        GradientLine(draw, a, b, FadeColor(color, alpha * 0.50f),
                     FadeColor(color2, alpha * 0.50f), 3.4f);
        GradientLine(draw, a, b, FadeColor(color, alpha * 0.85f),
                     FadeColor(color2, alpha * 0.85f), 1.8f);
        draw->AddLine(a, b, IM_COL32(255, 255, 255, (int)(200 * alpha)), 0.9f);
        // Only draw impact dot if the impact end itself is visible.
        if (best_b == samples) {
            draw->AddCircleFilled(b, 2.4f, end_col, 10);
            draw->AddCircleFilled(b, 1.1f, IM_COL32(255, 255, 255, (int)(230 * alpha)), 8);
        }
    }
}

// Diagnostic: list CUtlVector-looking fields on the local player whose element
// count changed since the previous probe (fire a few shots between calls).
void Impacts_Probe() {
    void* local = GetLocalEntity();
    if (!local) { Log("[impacts] probe: no local player (join a map first)"); return; }
    Log("[impacts] probe: local=%p", local);

    static struct { uintptr_t off; int size; } s_last[64];
    static int s_last_count = 0;
    struct { uintptr_t off; int size; } found[64];
    int count = 0;

    for (uintptr_t off = 0x0800; off < 0x18000 && count < 64; off += 8) {
        if (!Readable((char*)local + off, sizeof(RawVector))) continue;
        RawVector const* vec = reinterpret_cast<RawVector*>((char*)local + off);
        int const size = vec->size;
        if (size <= 0 || size > 128) continue;
        if (vec->capacity < size || vec->capacity > 4096) continue;
        if (!Readable(vec->memory, (size_t)size * CLIENT_IMPACT_STRIDE)) continue;
        found[count].off = off;
        found[count].size = size;
        count++;
    }

    for (int i = 0; i < count; i++) {
        int prev = -1;
        bool seen = false;
        for (int j = 0; j < s_last_count; j++) {
            if (s_last[j].off == found[i].off) { prev = s_last[j].size; seen = true; break; }
        }
        if (!seen || prev != found[i].size)
            Log("[impacts] probe: off=%#llx size=%d%s",
                (unsigned long long)found[i].off, found[i].size,
                seen ? " (changed)" : "");
    }
    if (count == 0) Log("[impacts] probe: no candidates");

    memcpy(s_last, found, sizeof(found[0]) * (size_t)count);
    s_last_count = count;

    if (Readable((char*)local + CLIENT_IMPACTS_OFFSET, sizeof(RawVector))) {
        RawVector const* at_ref = reinterpret_cast<RawVector*>((char*)local + CLIENT_IMPACTS_OFFSET);
        Log("[impacts] probe: reference offset %#llx -> ptr=%p size=%d cap=%d",
            (unsigned long long)CLIENT_IMPACTS_OFFSET,
            at_ref->memory, at_ref->size, at_ref->capacity);
    } else {
        Log("[impacts] probe: reference offset %#llx outside the entity allocation",
            (unsigned long long)CLIENT_IMPACTS_OFFSET);
    }
}