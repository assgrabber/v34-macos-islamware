#include "cheat.h"
#include "esp_layout.hpp"
#include "imgui.h"
#include "alice_kit.hpp"
#include "fonts/fonts.hpp"

#include <algorithm>
#include <cfloat>
#include <csetjmp>
#include <cstring>
#include <string>
#include <vector>

extern void SetStage(const char* stage);
extern jmp_buf g_safe_call_jmp;
extern volatile int g_safe_call_active;

// ---------------------------------------------------------------------------
// Bone pose cache.  Bones come from a DIRECT call to the game's
// C_BaseAnimating::SetupBones (found in the renderable vtable).  The call is
// made WITHOUT resetting the readable-bone mask: SetupBones then either
// early-outs and hands back the engine's own cached pose (fresh for players
// the engine rendered this frame) or runs its normal rebuild path - exactly
// what the engine itself does when it queries bones.  No mask forcing, no
// IK games, no faults.
// ---------------------------------------------------------------------------
TYPEDEF_THISCALL(bool, SetupBonesFn, void*, matrix3x4_t*, int, int, float);
static SetupBonesFn oSetupBones = nullptr;

struct BoneCacheEntry {
    void* renderable;
    int count;
    int frame;
    bool valid;
    Vector origin;
    matrix3x4_t bones[128];
};
static BoneCacheEntry g_bone_cache[65];
static int g_bone_frame = 0;
static volatile LONG g_bone_fn_state = 0;   // 0 = not tried, 1 = found, -1 = failed
static volatile LONG g_bone_fault_time = 0;

static bool ReadableRange(const void* ptr, size_t bytes) {
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

static void EnsureSetupBonesFn(void* renderable) {
    if (g_bone_fn_state != 0 || !client_module) return;
    void** rvt = *reinterpret_cast<void***>(renderable);
    if (!rvt) return;

#ifdef _WIN64
    uintptr_t const target = (uintptr_t)client_module + 0x7b4a0;   // C_BaseAnimating::SetupBones
    for (int i = 0; i < 64; i++) {
        if ((uintptr_t)rvt[i] != target) continue;
        oSetupBones = reinterpret_cast<SetupBonesFn>(rvt[i]);
        g_bone_fn_state = 1;
        Log("[bones] SetupBones found at renderable vtable[%d]", i);
        return;
    }
    g_bone_fn_state = -1;
    Log("[bones] SetupBones not found in the player renderable vtable");
#else
    oSetupBones = reinterpret_cast<SetupBonesFn>(rvt[SETUPBONES_INDEX]);
    g_bone_fn_state = oSetupBones ? 1 : -1;
    Log("[bones] SetupBones v34 renderable vtable[%d]=%p", SETUPBONES_INDEX, (void*)oSetupBones);
#endif
}

static bool MemReadable(void const* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static const matrix3x4_t* GetEntityBonesPtr(int index, void* entity, int* out_count = nullptr, Vector* out_cached_origin = nullptr) {
    if (!entity || index < 1 || index > 64 || !client_module) return nullptr;
    if (*reinterpret_cast<int*>((char*)entity + off::m_nModelIndex) <= 0) return nullptr;
    if (EntityIsStale(entity, index)) return nullptr;

#ifdef _WIN64
    if (!MemReadable((char*)entity + off::m_vecOrigin, sizeof(Vector))) return nullptr;
    Vector const org = *reinterpret_cast<Vector*>((char*)entity + off::m_vecOrigin);
    if (!(org.x == org.x) || !(org.y == org.y) || !(org.z == org.z)) return nullptr;
    if (fabsf(org.x) > 100000.f || fabsf(org.y) > 100000.f) return nullptr;
    if (!MemReadable((char*)entity + 8, 0xB60)) return nullptr;
    int const pre_count = *reinterpret_cast<int*>((char*)entity + 8 + 0xB50);
    if (pre_count <= 0 || pre_count > 128) return nullptr;
    void* renderable = reinterpret_cast<char*>(entity) + 8;
#else
    void* renderable = reinterpret_cast<char*>(entity) + 4;
#endif
    EnsureSetupBonesFn(renderable);
    if (!oSetupBones) return nullptr;

    BoneCacheEntry& e = g_bone_cache[index];
#ifndef _WIN64
    // On v34, SetupBones touches datacache.dll (main-thread only).  Read the
    // bones cached on the main thread by Bones_OnFrameStage().
    if (e.valid && e.renderable == renderable) {
        if (out_count) *out_count = e.count;
        if (out_cached_origin) *out_cached_origin = e.origin;
        return e.bones;
    }
    return nullptr;
#else
    if (!(e.valid && e.renderable == renderable && e.frame == g_bone_frame)) {
        LONG const fault = g_bone_fault_time;
        if (fault && GetTickCount() - (DWORD)fault < 12000) return nullptr;

        memset(e.bones, 0, sizeof(e.bones));
        g_safe_call_active = 1;
        if (setjmp(g_safe_call_jmp) != 0) {
            g_safe_call_active = 0;
            g_bone_fault_time = (LONG)GetTickCount();
            Log("[bones] SetupBones faulted - bone ESP paused for 12s");
            return nullptr;
        }
        bool const ok = oSetupBones(renderable, e.bones, 128, 0x7FF00, engine.Time());
        g_safe_call_active = 0;
        if (!ok) return nullptr;

        int const count = *reinterpret_cast<int*>((char*)renderable + 0xB50);
        e.count = (count > 0 && count <= 128) ? count : 128;
        e.origin = org;
        e.renderable = renderable;
        e.frame = g_bone_frame;
        e.valid = true;
    }

    if (out_count) *out_count = e.count;
    if (out_cached_origin) *out_cached_origin = e.origin;
    return e.bones;
#endif
}

static bool GetEntityBones(int index, void* entity, matrix3x4_t* out, int* out_count = nullptr) {
    int n = 0;
    const matrix3x4_t* ptr = GetEntityBonesPtr(index, entity, &n);
    if (!ptr) return false;
    memset(out, 0, sizeof(matrix3x4_t) * 128);
    memcpy(out, ptr, sizeof(matrix3x4_t) * (size_t)n);
    if (out_count) *out_count = n;
    return true;
}

// ---------------------------------------------------------------------------
// Bone name map.  Hardcoded indices are wrong for this game: in ct_sas.mdl
// bone 6 is R_Calf and bone 5 is R_Thigh, and custom models (v48) insert
// twist bones that shift everything.  Resolve the standard ValveBiped bones
// by name from the entity's studio header and cache the map per model.
// ---------------------------------------------------------------------------
struct BoneMap {
    int  model_index;
    bool valid;
    int  count;
    int  pelvis, spine, spine1, spine2, spine4, neck1, head1;
    int  l_thigh, l_calf, l_foot;
    int  r_thigh, r_calf, r_foot;
    int  l_clav, l_upper, l_fore, l_hand;
    int  r_clav, r_upper, r_fore, r_hand;
};

constexpr int MODEL_MAP_MAX = 4096;
static BoneMap g_bone_maps[MODEL_MAP_MAX];

static bool NameEndsWith(const char* name, const char* suffix) {
    size_t const a = strlen(name);
    size_t const b = strlen(suffix);
    return a >= b && strcmp(name + a - b, suffix) == 0;
}

// Locate studiohdr_t for the entity.  The member offset is not stable enough
// to hardcode across this build's subobjects, so the entity is scanned for a
// CStudioHdr-like pointer chain that validates as a real "IDST" header.
static void* FindStudioHdr(void* entity, int model_index) {
#ifdef _WIN64
    constexpr uintptr_t STUDIO_SCAN_START = 0xB00;
    constexpr uintptr_t STUDIO_SCAN_END   = 0xE00;
#else
    constexpr uintptr_t STUDIO_SCAN_START = 0x600;
    constexpr uintptr_t STUDIO_SCAN_END   = 0x900;
#endif
    for (uintptr_t off = STUDIO_SCAN_START; off <= STUDIO_SCAN_END; off += sizeof(void*)) {
        if (!ReadableRange((char*)entity + off, sizeof(void*))) break;
        void* const cached = *reinterpret_cast<void**>((char*)entity + off);
        if (!cached) continue;

        void* candidates[2] = { cached, nullptr };
        if (ReadableRange(cached, sizeof(void*)))
            candidates[1] = *reinterpret_cast<void**>(cached);

        for (void* studio : candidates) {
            if (!studio || !ReadableRange(studio, 0xB0)) continue;
            if (*reinterpret_cast<uint32_t*>(studio) != 0x54534449) continue; // 'IDST'
            uint32_t const length = *reinterpret_cast<uint32_t*>((char*)studio + 0x4C);
            int const numbones = *reinterpret_cast<int*>((char*)studio + 0x9C);
            int const boneindex = *reinterpret_cast<int*>((char*)studio + 0xA0);
            if (length < 0x200 || length > 0x4000000) continue;
            if (numbones <= 0 || numbones > 512) continue;
            if (boneindex <= 0 || (uint32_t)boneindex + (uint32_t)numbones * 216u > length) continue;

            static volatile LONG s_logged = 0;
            if (!s_logged) {
                s_logged = 1;
                Log("[bones] studio header at entity+%#llx (model %d, %d bones)",
                    (unsigned long long)off, model_index, numbones);
            }
            return studio;
        }
    }
    return nullptr;
}

static BoneMap* GetBoneMap(void* entity) {
    if (!entity) return nullptr;
    int const model_index = *reinterpret_cast<int*>((char*)entity + off::m_nModelIndex);
    if (model_index <= 0 || model_index >= MODEL_MAP_MAX) return nullptr;

    BoneMap& m = g_bone_maps[model_index];
    if (m.model_index == model_index && m.valid) return &m;

    // On x64, IVModelInfo::GetStudiomodel is safe; on v34, MDLCache/datacache
    // is main-thread only and faults if called from the render thread.
    void* studio = nullptr;
#ifdef _WIN64
    {
        const void* model = model_info.GetModel(model_index);
        if (model) {
            const void* hdr = model_info.GetStudiomodel(model);
            if (hdr) {
                // accept either a raw studiohdr (IDST magic) or a wrapper
                if (*reinterpret_cast<const uint32_t*>(hdr) == 0x54534449)
                    studio = const_cast<void*>(hdr);
                else if (ReadableRange(hdr, sizeof(void*))) {
                    void* const inner = *reinterpret_cast<void* const*>(hdr);
                    if (inner && ReadableRange(inner, 0xB0) &&
                        *reinterpret_cast<uint32_t*>(inner) == 0x54534449)
                        studio = inner;
                }
            }
        }
        if (!studio)
            studio = FindStudioHdr(entity, model_index);
    }
#else
    studio = FindStudioHdr(entity, model_index);
#endif
    if (!studio) {
        static volatile LONG s_fail_logged = 0;
        if (!s_fail_logged) {
            s_fail_logged = 1;
            Log("[bones] no studio header found on entity %p (model %d)", entity, model_index);
        }
        return nullptr;
    }

    uint32_t const length = *reinterpret_cast<uint32_t*>((char*)studio + 0x4C);
    int const numbones = *reinterpret_cast<int*>((char*)studio + 0x9C);
    int const boneindex = *reinterpret_cast<int*>((char*)studio + 0xA0);
    if (length < 0x200 || length > 0x4000000 || numbones <= 0 || numbones > 512 ||
        boneindex <= 0 || (uint32_t)boneindex + (uint32_t)numbones * 216u > length) {
        static volatile LONG s_val_logged = 0;
        if (!s_val_logged) {
            s_val_logged = 1;
            Log("[bones] header validation failed: magic=%08x len=%u bones=%d idx=%d",
                *reinterpret_cast<const uint32_t*>(studio), length, numbones, boneindex);
        }
        return nullptr;
    }

    memset(&m, 0, sizeof(m));
    m.model_index = model_index;
    m.count = numbones;
    m.pelvis = m.spine = m.spine1 = m.spine2 = m.spine4 = m.neck1 = m.head1 = -1;
    m.l_thigh = m.l_calf = m.l_foot = -1;
    m.r_thigh = m.r_calf = m.r_foot = -1;
    m.l_clav = m.l_upper = m.l_fore = m.l_hand = -1;
    m.r_clav = m.r_upper = m.r_fore = m.r_hand = -1;

    for (int i = 0; i < numbones; i++) {
        char* const bone = (char*)studio + boneindex + (size_t)i * 216;
        int const name_off = *reinterpret_cast<int*>(bone);
        if (name_off <= 0) continue;
        uintptr_t const name_ptr = (uintptr_t)bone + (uintptr_t)name_off;
        if (name_ptr < (uintptr_t)studio || name_ptr + 1 > (uintptr_t)studio + length) continue;
        char const* const name = (char const*)name_ptr;

        if (NameEndsWith(name, "Bip01_Pelvis")) m.pelvis = i;
        else if (NameEndsWith(name, "Bip01_Spine")) m.spine = i;
        else if (NameEndsWith(name, "Bip01_Spine1")) m.spine1 = i;
        else if (NameEndsWith(name, "Bip01_Spine2")) m.spine2 = i;
        else if (NameEndsWith(name, "Bip01_Spine4")) m.spine4 = i;
        else if (NameEndsWith(name, "Bip01_Neck1")) m.neck1 = i;
        else if (NameEndsWith(name, "Bip01_Head1")) m.head1 = i;
        else if (NameEndsWith(name, "Bip01_L_Thigh")) m.l_thigh = i;
        else if (NameEndsWith(name, "Bip01_L_Calf")) m.l_calf = i;
        else if (NameEndsWith(name, "Bip01_L_Foot")) m.l_foot = i;
        else if (NameEndsWith(name, "Bip01_R_Thigh")) m.r_thigh = i;
        else if (NameEndsWith(name, "Bip01_R_Calf")) m.r_calf = i;
        else if (NameEndsWith(name, "Bip01_R_Foot")) m.r_foot = i;
        else if (NameEndsWith(name, "Bip01_L_Clavicle")) m.l_clav = i;
        else if (NameEndsWith(name, "Bip01_L_UpperArm")) m.l_upper = i;
        else if (NameEndsWith(name, "Bip01_L_Forearm")) m.l_fore = i;
        else if (NameEndsWith(name, "Bip01_L_Hand")) m.l_hand = i;
        else if (NameEndsWith(name, "Bip01_R_Clavicle")) m.r_clav = i;
        else if (NameEndsWith(name, "Bip01_R_UpperArm")) m.r_upper = i;
        else if (NameEndsWith(name, "Bip01_R_Forearm")) m.r_fore = i;
        else if (NameEndsWith(name, "Bip01_R_Hand")) m.r_hand = i;
    }

    m.valid = true;
    Log("[bones] model %d bones=%d head=%d neck=%d spine2=%d spine1=%d pelvis=%d",
        model_index, numbones, m.head1, m.neck1, m.spine2, m.spine1, m.pelvis);
    return &m;
}

// Aim hitboxes resolved from the model's own bone names.  Returns
// head, neck, chest, stomach, pelvis; false when the model has no ValveBiped
// skeleton (caller then uses the height fallback).
bool GetEntityAimBones(void* entity, int out[5]) {
    BoneMap* map = GetBoneMap(entity);
    if (!map) {
        // default v44 player skeleton (ValveBiped): 14 head, 13 neck,
        // 11 spine2, 10 spine1, 0 pelvis - keeps aim on the body even if the
        // header scan fails
        out[0] = 14;
        out[1] = 13;
        out[2] = 11;
        out[3] = 10;
        out[4] = 0;
        return true;
    }
    int const spine4 = map->spine4 >= 0 ? map->spine4 : map->spine2;
    out[0] = map->head1 >= 0 ? map->head1 : spine4;
    out[1] = map->neck1 >= 0 ? map->neck1 : spine4;
    out[2] = map->spine2 >= 0 ? map->spine2 : map->spine1;
    out[3] = map->spine1 >= 0 ? map->spine1 : map->spine;
    out[4] = map->pelvis;
    return true;
}

// Bounding box of the rendered pose from the bone matrices.  Body bones only
// (skirt / weapon / finger bones would inflate the box), so the box matches
// the skeleton exactly.
static bool GetBoneBounds(int index, void* entity, Vector& feet, Vector& top) {
    int count = 0;
    Vector cached_org;
    const matrix3x4_t* bones = GetEntityBonesPtr(index, entity, &count, &cached_org);
    if (!bones) return false;

    int body[24];
    int n = 0;
    if (BoneMap* map = GetBoneMap(entity)) {
        int const ids[] = {
            map->pelvis, map->spine, map->spine1, map->spine2, map->spine4,
            map->neck1, map->head1,
            map->l_thigh, map->l_calf, map->l_foot,
            map->r_thigh, map->r_calf, map->r_foot,
            map->l_clav, map->l_upper, map->l_fore, map->l_hand,
            map->r_clav, map->r_upper, map->r_fore, map->r_hand,
        };
        for (int id : ids) {
            if (id < 0 || id >= count) continue;
            bool dup = false;
            for (int k = 0; k < n; k++) if (body[k] == id) { dup = true; break; }
            if (!dup) body[n++] = id;
        }
    }
    if (n < 3) {
        int const lim = count > 64 ? 64 : count;
        for (int i = 0; i < lim; i++) body[i] = i;
        n = lim;
    }
    if (n <= 0) return false;

    // reject bones that are not physically near the player: root/weapon/twist
    // bones can sit at the map origin or anywhere else, and one such bone in
    // the min/max set sends the box to a random place
    Vector const org = *reinterpret_cast<Vector*>((char*)entity + off::m_vecOrigin);
#ifndef _WIN64
    Vector const bone_delta = org - cached_org;
#else
    Vector const bone_delta(0.0f, 0.0f, 0.0f);
#endif

    const float k = 1.0e9f;
    Vector mn(k, k, k), mx(-k, -k, -k);
    bool any = false;
    for (int j = 0; j < n; j++) {
        int const i = body[j];
        if (i < 0 || i >= count) continue;
        float x = bones[i].m[0][3] + bone_delta.x;
        float y = bones[i].m[1][3] + bone_delta.y;
        float z = bones[i].m[2][3] + bone_delta.z;
        if (x == 0.0f && y == 0.0f && z == 0.0f) continue;
        if (fabsf(x) > 32768.0f || fabsf(y) > 32768.0f || fabsf(z) > 32768.0f) continue;
        if (fabsf(x - org.x) > 128.0f || fabsf(y - org.y) > 128.0f ||
            fabsf(z - org.z) > 128.0f) continue;
        if (x < mn.x) mn.x = x; if (x > mx.x) mx.x = x;
        if (y < mn.y) mn.y = y; if (y > mx.y) mx.y = y;
        if (z < mn.z) mn.z = z; if (z > mx.z) mx.z = z;
        any = true;
    }
    if (!any) return false;

    // centre on the pelvis (stable) instead of the min/max centre, which the
    // arms/hands shift around while aiming
    float cx = (mn.x + mx.x) * 0.5f;
    float cy = (mn.y + mx.y) * 0.5f;
    {
        BoneMap* map = GetBoneMap(entity);
        if (map && map->pelvis >= 0 && map->pelvis < count) {
            float px = bones[map->pelvis].m[0][3] + bone_delta.x;
            float py = bones[map->pelvis].m[1][3] + bone_delta.y;
            if (!(px == 0.0f && py == 0.0f) &&
                fabsf(px - org.x) < 48.0f && fabsf(py - org.y) < 48.0f) {
                cx = px;
                cy = py;
            }
        }
    }
    feet = Vector(cx, cy, mn.z - 2.0f);
    top = Vector(cx, cy, mx.z + 2.0f);
    return true;
}

static int LocalIndex() {
    if (!engine.self) return 0;
    int idx = engine.GetLocalPlayer();
    return (idx < 0) ? 0 : idx;
}

bool IsPlayerIndex(int index) {
    if (index < 1 || index > 64 || !engine.self) return false;
    // Cache GetPlayerInfo per slot (250 ms TTL) so hot loops (Bones, ESP,
    // Chams, Aimbot) don't lock the engine user-info table hundreds of times
    // per frame.
    static bool s_cached[65] = {};
    static DWORD s_stamp[65] = {};
    DWORD const now = GetTickCount();
    if (now - s_stamp[index] < 250)
        return s_cached[index];
    player_info_t info = {};
    bool const ok = engine.GetPlayerInfo(index, &info);
    s_cached[index] = ok;
    s_stamp[index] = now;
    return ok;
}

Vector PlayerEye(void* ent) {
    Vector origin = *reinterpret_cast<Vector*>((char*)ent + off::m_vecOrigin);
    Vector view = *reinterpret_cast<Vector*>((char*)ent + off::m_vecViewOffset);
    if (!(view.z > 1.0f && view.z < 100.0f)) view.z = 64.0f;
    return origin + view;
}

void* GetLocalEntity() {
    if (!IsGameReady()) return nullptr;

    int idx = LocalIndex();
    void* local = (idx > 0) ? entity_list.GetClientEntity(idx) : nullptr;

    // Alive local player: the engine index is authoritative.  Never use the
    // eye-match below here, otherwise a close enemy could be mistaken for the
    // local player (which hides their ESP box and breaks team logic).
    if (local) {
        char life = *reinterpret_cast<char*>((char*)local + off::m_lifeState);
        int health = *reinterpret_cast<int*>((char*)local + off::m_iHealth);
        if (life == 0 && health > 0)
            return local;
    }

    // Dead / spectating: the render camera can be attached to another player,
    // so follow the camera eye to pick the observed entity.
    if (view_valid) {
        void* best = nullptr;
        float best_dist = 1.0e9f;
        for (int i = 1; i <= 64; i++) {
            void* ent = entity_list.GetClientEntity(i);
            if (!ent) continue;
            Vector origin = *reinterpret_cast<Vector*>((char*)ent + off::m_vecOrigin);
            Vector view = *reinterpret_cast<Vector*>((char*)ent + off::m_vecViewOffset);
            if (view.z == 0.0f) view.z = 64.0f;
            Vector eye = origin + view;
            Vector d = eye - view_origin;
            float dist = d.x * d.x + d.y * d.y + d.z * d.z;
            if (dist < best_dist) { best_dist = dist; best = ent; }
        }
        if (best && best_dist < 24.0f * 24.0f)
            return best;
    }

    return local;
}

bool EntityIsStale(void* ent, int index) {
    if (!ent) return false;

    // Canonical Source dormant flag (IClientNetworkable::IsDormant, vtable[8]).
    // The server stops sending updates for entities outside our PVS, so the
    // client keeps their last origin forever.  IsDormant() is the only
    // reliable way to know the entity is a ghost.
    if (entity_list.self && index > 0) {
        void* networkable = entity_list.GetClientNetworkable(index);
        if (networkable) {
            bool dormant = VFunc<bool(THISCALL_CC*)(void*)>(networkable, 8)(networkable);
            return dormant;
        }
    }

    // Fallback: simulation time stops advancing while dormant.
    if (!engine.self) return false;
    float sim = *reinterpret_cast<float*>((char*)ent + off::m_flSimulationTime);
    float now = engine.Time();
    return now > 0.0f && (now - sim) > 0.75f;
}

// Custom trace filter so props / players participate in the cast.  The
// engine's own CTraceFilterWorldOnly ignored physics props entirely, which is
// why auto-wall happily shot through crates that block bullets in-game.
static void* g_trace_skip1 = nullptr;
static void* g_trace_skip2 = nullptr;

static HOOK_THISCALL(bool, TraceFilter_ShouldHit, void*, void* entity, int) {
    // The engine passes nullptr for world geometry - it must always be hit.
    return entity != g_trace_skip1 && entity != g_trace_skip2;
}
static HOOK_THISCALL(int, TraceFilter_GetType, void*) {
    return 0; // TRACE_EVERYTHING
}
static void* g_trace_filter_vtable[2] = {
    (void*)TraceFilter_ShouldHit,
    (void*)TraceFilter_GetType,
};
struct TraceFilter { void** vt; };

// Engine trace with a caller-supplied skip list.  Optionally reports the hit
// entity, surface material name, physics surface props, contents, and startsolid.
bool TraceLine(const Vector& from, const Vector& to, float& fraction, Vector& end,
               void* skip1, void* skip2, void** out_entity, const char** out_surface,
               Vector* out_normal, unsigned int mask, short* out_surface_props,
               int* out_contents, bool* out_startsolid) {
    fraction = 1.0f;
    end = to;
    if (out_entity) *out_entity = nullptr;
    if (out_surface) *out_surface = nullptr;
    if (out_normal) *out_normal = Vector(0, 0, 1);
    if (out_surface_props) *out_surface_props = 0;
    if (out_contents) *out_contents = 0;
    if (out_startsolid) *out_startsolid = false;
    if (!engine_trace.self) return false;

    Ray_t ray;
    memset(&ray, 0, sizeof(ray));
    ray.m_Start = from;
    ray.m_Delta = to - from;
    ray.m_IsRay = true;
    ray.m_IsSwept = true;

    alignas(16) unsigned char result[512];
    memset(result, 0, sizeof(result));

    g_trace_skip1 = skip1;
    g_trace_skip2 = skip2;

    g_safe_call_active = 1;
    if (setjmp(g_safe_call_jmp) != 0) {
        g_safe_call_active = 0;
        return false;
    }
    TraceFilter filter;
    filter.vt = g_trace_filter_vtable;
    engine_trace.TraceRay(ray, mask, &filter, result);
    g_safe_call_active = 0;

    float f = *reinterpret_cast<float*>(result + 0x2C);
    if (!(f >= 0.0f && f <= 1.0f)) return false;

    bool const startsolid = *reinterpret_cast<bool*>(result + 0x37);
    bool const allsolid = *reinterpret_cast<bool*>(result + 0x36);

    fraction = f;
    end = Vector(*reinterpret_cast<float*>(result + 0x0C),
                 *reinterpret_cast<float*>(result + 0x10),
                 *reinterpret_cast<float*>(result + 0x14));

    if (out_normal)
        *out_normal = Vector(*reinterpret_cast<float*>(result + 0x18),
                             *reinterpret_cast<float*>(result + 0x1C),
                             *reinterpret_cast<float*>(result + 0x20));
    if (out_contents)
        *out_contents = *reinterpret_cast<int*>(result + 0x30);
    if (out_startsolid)
        *out_startsolid = startsolid || allsolid;
#ifdef _WIN64
    if (out_surface)
        *out_surface = *reinterpret_cast<const char**>(result + 0x40);
    if (out_surface_props)
        *out_surface_props = *reinterpret_cast<short*>(result + 0x48);
    if (out_entity)
        *out_entity = *reinterpret_cast<void**>(result + 0x58);
#else
    if (out_surface)
        *out_surface = *reinterpret_cast<const char**>(result + 0x3C);
    if (out_surface_props)
        *out_surface_props = *reinterpret_cast<short*>(result + 0x40);
    if (out_entity)
        *out_entity = *reinterpret_cast<void**>(result + 0x4C);
#endif
    return fraction < 0.97f || startsolid || allsolid;
}

bool TraceVisible(const Vector& from, const Vector& to, void* skip) {
    float f;
    Vector end;
    void* hit = nullptr;
    void* local = GetLocalEntity();
    // Do NOT skip the target: a trace that stops on the target means the
    // point is visible.  Skipping it made visibility depend on whatever was
    // behind the player, so a visible head in front of a wall read as blocked.
    TraceLine(from, to, f, end, local, nullptr, &hit);
    if (f > 0.97f) return true;
    return skip && hit == skip;
}

// ---------------------------------------------------------------------------
// CS:S FireBullet-accurate penetration & damage calculation
// ---------------------------------------------------------------------------
struct WeaponPenStats {
    int penetration;
    int damage;
    float range;
    float range_modifier;
    float armor_ratio;
    float pen_power;
    float pen_range;
};

static bool GetActiveWeaponPenStats(WeaponPenStats& out) {
    // defaults if weapon class isn't recognized
    out = { 1, 30, 4096.0f, 0.85f, 1.0f, 21.0f, 800.0f };
    void* local = GetLocalEntity();
    if (!local) return false;
    int const handle = *reinterpret_cast<int*>((char*)local + off::m_hActiveWeapon);
    int const idx = handle & 0xFFF;
    if (idx <= 0) return false;
    void* nw = entity_list.GetClientNetworkable(idx);
    if (!nw) return false;
    void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
    char const* cls = ClientClassName(cc);
    if (!cls) return false;

    // Exact CS:S weapon script + GetBulletTypeParameters values
    // (pen_power/pen_range verified against client.dll FireBullet).
    struct Entry { const char* token; WeaponPenStats s; };
    static const Entry table[] = {
        { "AWP",      { 3, 115, 8192.0f, 0.990f, 1.95f, 45.0f, 8000.0f } },
        { "G3SG1",    { 3,  80, 8192.0f, 0.980f, 1.65f, 39.0f, 5000.0f } },
        { "Scout",    { 3,  75, 8192.0f, 0.980f, 1.70f, 39.0f, 5000.0f } },
        { "SG550",    { 2,  70, 8192.0f, 0.980f, 1.45f, 35.0f, 4000.0f } },
        { "AK47",     { 2,  36, 8192.0f, 0.980f, 1.55f, 39.0f, 5000.0f } },
        { "M4A1",     { 2,  33, 8192.0f, 0.970f, 1.40f, 35.0f, 4000.0f } },
        { "Aug",      { 2,  32, 8192.0f, 0.960f, 1.40f, 39.0f, 5000.0f } },
        { "SG552",    { 2,  33, 8192.0f, 0.955f, 1.40f, 35.0f, 4000.0f } },
        { "Galil",    { 2,  30, 8192.0f, 0.980f, 1.55f, 35.0f, 4000.0f } },
        { "Famas",    { 2,  30, 8192.0f, 0.960f, 1.40f, 35.0f, 4000.0f } },
        { "M249",     { 2,  32, 8192.0f, 0.970f, 1.50f, 35.0f, 4000.0f } },
        { "Deagle",   { 2,  54, 4096.0f, 0.810f, 1.85f, 30.0f, 1000.0f } },
        { "FiveSeven",{ 1,  25, 4096.0f, 0.885f, 1.50f, 30.0f, 2000.0f } },
        { "P90",      { 1,  26, 4096.0f, 0.840f, 1.50f, 30.0f, 2000.0f } },
        { "P228",     { 1,  40, 4096.0f, 0.800f, 1.25f, 25.0f,  800.0f } },
        { "Glock",    { 1,  25, 4096.0f, 0.750f, 1.05f, 21.0f,  800.0f } },
        { "Elite",    { 1,  45, 4096.0f, 0.750f, 1.05f, 21.0f,  800.0f } },
        { "MP5",      { 1,  26, 4096.0f, 0.840f, 1.00f, 21.0f,  800.0f } },
        { "TMP",      { 1,  26, 4096.0f, 0.840f, 1.00f, 21.0f,  800.0f } },
        { "USP",      { 1,  34, 4096.0f, 0.790f, 1.00f, 15.0f,  500.0f } },
        { "MAC10",    { 1,  29, 4096.0f, 0.820f, 0.95f, 15.0f,  500.0f } },
        { "UMP45",    { 1,  30, 4096.0f, 0.820f, 1.00f, 15.0f,  500.0f } },
        { "M3",       { 0,  22, 3000.0f, 0.700f, 1.00f,  0.0f,    0.0f } },
        { "XM1014",   { 0,  22, 3000.0f, 0.700f, 1.00f,  0.0f,    0.0f } },
        { "Knife",    { 0,  50,   64.0f, 1.000f, 1.00f,  0.0f,    0.0f } },
        { "CC4",      { 0,   0,    0.0f, 1.000f, 1.00f,  0.0f,    0.0f } },
        { "Grenade",  { 0,   0,    0.0f, 1.000f, 1.00f,  0.0f,    0.0f } },
    };
    for (auto const& e : table) {
        if (strstr(cls, e.token)) {
            out = e.s;
            return true;
        }
    }
    return true;
}

static uint16_t ResolveSurfaceMaterial(short surface_props, const char* surface_name) {
    if (physics_props.self) {
        const void* sdata = physics_props.GetSurfaceData(surface_props);
        uint16_t const mat = IPhysicsSurfaceProps::MaterialOf(sdata);
        if (mat != 0) return mat;
    }
    if (!surface_name || !surface_name[0]) return 0;
    if (strstr(surface_name, "wood")) return 'W';
    if (strstr(surface_name, "grate") || strstr(surface_name, "glass")) return 'G';
    if (strstr(surface_name, "concrete") || strstr(surface_name, "stone") || strstr(surface_name, "brick")) return 'C';
    if (strstr(surface_name, "metal")) return 'M';
    if (strstr(surface_name, "dirt") || strstr(surface_name, "sand")) return 'D';
    if (strstr(surface_name, "tile")) return 'T';
    if (strstr(surface_name, "plastic") || strstr(surface_name, "computer")) return 'P';
    if (strstr(surface_name, "vent")) return 'V';
    return 0;
}

// Verified in client.dll FireBullet (0x1801fa149..0x1801fa21a)
static void GetMaterialParameters(uint16_t material, float& pen_mod, float& dmg_mod) {
    switch (material) {
    case 'M': pen_mod = 0.50f; dmg_mod = 0.30f; break;
    case 'D': pen_mod = 0.50f; dmg_mod = 0.30f; break;
    case 'C': pen_mod = 0.40f; dmg_mod = 0.25f; break;
    case 'G': pen_mod = 1.00f; dmg_mod = 0.99f; break;
    case 'V': pen_mod = 0.50f; dmg_mod = 0.45f; break;
    case 'T': pen_mod = 0.65f; dmg_mod = 0.30f; break;
    case 'P': pen_mod = 0.40f; dmg_mod = 0.45f; break;
    case 'W': pen_mod = 1.00f; dmg_mod = 0.60f; break;
    default:  pen_mod = 1.00f; dmg_mod = 0.50f; break;
    }
}

float CalcHitDamage(float base_damage, float armor_ratio, float range_modifier,
                    float distance, int hitbox, void* target) {
    float dmg = base_damage * powf(range_modifier, distance * 0.002f);
    // hitbox: 0=head, 1=neck, 2=chest, 3=stomach, 4=pelvis
    if (hitbox == 0) dmg *= 4.0f;
    else if (hitbox == 3 || hitbox == 4) dmg *= 1.25f;

    if (target) {
        int const armor = *reinterpret_cast<int*>((char*)target + off::m_ArmorValue);
        bool const helmet = *reinterpret_cast<char*>((char*)target + off::m_bHasHelmet) != 0;
        bool const armored = (hitbox == 0) ? helmet : (armor > 0);
        if (armor > 0 && armored) {
            float new_dmg = dmg * (armor_ratio * 0.5f);
            float const armor_dmg = (dmg - new_dmg) * 0.5f;
            if (armor_dmg > (float)armor)
                new_dmg = dmg - (float)armor * 2.0f;
            dmg = new_dmg;
        }
    }
    return dmg;
}

bool CanPenetrateTarget(const Vector& from, const Vector& to, void* target, int hitbox,
                        float min_damage, float* out_damage) {
    if (out_damage) *out_damage = 0.0f;
    void* local = GetLocalEntity();
    if (!local) return false;

    WeaponPenStats stats{};
    GetActiveWeaponPenStats(stats);
    if (stats.damage <= 0 || stats.range <= 0.0f) return false;

    Vector dir = to - from;
    float const total_dist = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (total_dist < 1.0f) {
        float const dmg = CalcHitDamage((float)stats.damage, stats.armor_ratio, stats.range_modifier, 0.0f, hitbox, target);
        if (out_damage) *out_damage = dmg;
        return dmg >= (min_damage > 0.0f ? min_damage : 1.0f);
    }
    if (total_dist > stats.range) return false;
    dir.x /= total_dist; dir.y /= total_dist; dir.z /= total_dist;

    int pen_count = stats.penetration;
    float cur_pen_power = stats.pen_power;
    float cur_damage = (float)stats.damage;
    float cur_max_range = stats.range;
    float traced_dist = 0.0f;
    Vector cursor = from;

    constexpr unsigned int kMaskShot = 0x4600400Bu;
    constexpr int kContentsGrate = 0x8;
    constexpr int kSolidExitMask = 0x400B; // CONTENTS_SOLID | WINDOW | GRATE | MOVEABLE

    float const req_dmg = (min_damage > 0.0f) ? min_damage : 1.0f;

    for (int iter = 0; iter < 5 && cur_damage >= 1.0f; iter++) {
        float const remaining = cur_max_range - traced_dist;
        if (remaining <= 1.0f) return false;

        Vector const ray_end = cursor + dir * remaining;
        float f1 = 1.0f;
        Vector hit1;
        void* hit_entity = nullptr;
        const char* surf = nullptr;
        short surf_props = 0;
        int contents = 0;
        bool startsolid = false;

        TraceLine(cursor, ray_end, f1, hit1, local, nullptr, &hit_entity, &surf,
                  nullptr, kMaskShot, &surf_props, &contents, &startsolid);

        Vector const d_rem = to - cursor;
        float const dist_rem_to_target = sqrtf(d_rem.x * d_rem.x + d_rem.y * d_rem.y + d_rem.z * d_rem.z);
        float const hit_dist = remaining * f1;

        // Reached the target entity directly or passed the target point without hitting a wall
        if ((target && hit_entity == target) || hit_dist >= dist_rem_to_target - 1.0f) {
            float const final_dist = traced_dist + dist_rem_to_target;
            float const final_dmg = CalcHitDamage(cur_damage, stats.armor_ratio,
                                                  stats.range_modifier, final_dist, hitbox, target);
            if (out_damage) *out_damage = final_dmg;
            return final_dmg >= req_dmg;
        }
        if (f1 >= 1.0f) return false; // bullet ran out of remaining range before target

        if (startsolid) return false;
        if (surf && (strstr(surf, "sky") || strstr(surf, "SKY"))) return false;

        float const tmp_dist = traced_dist + hit_dist;
        cur_damage *= powf(stats.range_modifier, tmp_dist * 0.002f);

        bool const is_grate = (contents & kContentsGrate) != 0;
        uint16_t const enter_mat = ResolveSurfaceMaterial(surf_props, surf);
        float pen_mod = 1.0f, dmg_mod = 0.5f;
        if (is_grate) {
            pen_mod = 1.0f;
            dmg_mod = 0.99f;
        } else {
            GetMaterialParameters(enter_mat, pen_mod, dmg_mod);
        }

        if (tmp_dist > stats.pen_range)
            pen_count = (pen_count <= 0) ? pen_count : 0;
        if (pen_count < 0 || (pen_count == 0 && !is_grate))
            return false;
        if (cur_pen_power <= 0.0f)
            return false;

        // TraceToExit: step forward in 4u increments up to 128u until out of solid
        // brush geometry, then trace backwards to find the exact exit surface.
        bool found_exit = false;
        Vector exit_pos;
        short exit_props = 0;
        const char* exit_surf = nullptr;

        for (float step = 4.0f; step <= 128.0f; step += 4.0f) {
            Vector const probe = hit1 + dir * step;
            int const pc = engine_trace.self ? engine_trace.GetPointContents(probe, nullptr) : 0;
            if (pc & kSolidExitMask)
                continue;

            float f2 = 1.0f;
            Vector hit2;
            void* exit_ent = nullptr;
            bool exit_startsolid = false;
            TraceLine(probe, hit1, f2, hit2, local, target, &exit_ent, &exit_surf,
                      nullptr, kMaskShot, &exit_props, nullptr, &exit_startsolid);

            if (exit_ent && exit_ent != hit_entity) {
                TraceLine(probe, hit1, f2, hit2, local, exit_ent, nullptr, &exit_surf,
                          nullptr, kMaskShot, &exit_props, nullptr, &exit_startsolid);
            }

            if (!exit_startsolid && f2 < 1.0f) {
                exit_pos = hit2;
                found_exit = true;
                break;
            }
        }

        if (!found_exit)
            return false;

        Vector const delta = exit_pos - hit1;
        float const thickness = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        uint16_t const exit_mat = ResolveSurfaceMaterial(exit_props, exit_surf);

        if (enter_mat == exit_mat && (enter_mat == 'W' || enter_mat == 'M'))
            pen_mod += pen_mod;

        if (thickness > cur_pen_power * pen_mod)
            return false;

        cur_pen_power -= thickness / pen_mod;
        traced_dist = tmp_dist + thickness;
        cursor = exit_pos;
        cur_max_range = (cur_max_range - traced_dist) * 0.5f;
        cur_damage *= dmg_mod;
        pen_count--;
    }
    return false;
}

bool CanPenetrate(const Vector& from, const Vector& to, float, void* skip) {
    return CanPenetrateTarget(from, to, skip, 2, 1.0f, nullptr);
}

bool GetActiveWeaponDamageInfo(int& out_damage, float& out_armor_ratio, float& out_range_modifier) {
    WeaponPenStats stats{};
    if (!GetActiveWeaponPenStats(stats)) return false;
    out_damage = stats.damage;
    out_armor_ratio = stats.armor_ratio;
    out_range_modifier = stats.range_modifier;
    return true;
}


bool GetEntityBonePos(int index, int bone, Vector& out) {
    if (index < 1 || index > 64) return false;
    void* entity = entity_list.GetClientEntity(index);
    if (!entity) return false;
    int count = 0;
    Vector cached_org;
    const matrix3x4_t* bones = GetEntityBonesPtr(index, entity, &count, &cached_org);
    if (!bones || bone < 0 || bone >= count) return false;
    Vector origin = *reinterpret_cast<Vector*>((char*)entity + off::m_vecOrigin);
#ifndef _WIN64
    Vector p(bones[bone].m[0][3] + (origin.x - cached_org.x),
             bones[bone].m[1][3] + (origin.y - cached_org.y),
             bones[bone].m[2][3] + (origin.z - cached_org.z));
#else
    Vector p(bones[bone].m[0][3], bones[bone].m[1][3], bones[bone].m[2][3]);
#endif
    Vector d = p - origin;
    if (sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) > 130.0f) return false;
    out = p;
    return true;
}

void GetLocalEye(Vector& eye, QAngle& angles) {
    angles = QAngle(0, 0, 0);
    eye = Vector(0, 0, 0);
    if (!IsGameReady()) return;

    // Use the actual render camera captured in OverrideView.  The engine view
    // angles can hold the silent-aim/anti-aim command angles for a frame.
    if (view_valid)
        angles = view_angles;
    else
        engine.GetViewAngles(angles);

    if (view_valid && (view_origin.x != 0.0f || view_origin.y != 0.0f || view_origin.z != 0.0f)) {
        eye = view_origin; // CViewSetup already matches exact rendered camera eye (1st & 3rd person)
        return;
    }

    int idx = LocalIndex();
    void* local = entity_list.GetClientEntity(idx);
    if (local) {
        Vector origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
        Vector view = *reinterpret_cast<Vector*>((char*)local + off::m_vecViewOffset);
        if (view.z == 0.0f) view.z = 64.0f;
        eye = origin + view;
    }
}

bool WorldToScreen(const Vector& world, ImVec2& out, int width, int height) {
    if (width <= 0 || height <= 0) return false;

    // Prefer the engine's exact view*projection matrix from the last rendered
    // frame so zoomed/scoped FOV, aspect ratio, and viewmodel camera match.
    const float* m = engine.self ? engine.WorldToScreenMatrix() : nullptr;
    if (m && (m[0] != 0.0f || m[1] != 0.0f || m[4] != 0.0f || m[5] != 0.0f ||
              m[12] != 0.0f || m[13] != 0.0f)) {
        float const w = m[12] * world.x + m[13] * world.y + m[14] * world.z + m[15];
        if (w < 0.001f) return false;
        float const inv_w = 1.0f / w;
        float const nx = (m[0] * world.x + m[1] * world.y + m[2] * world.z + m[3]) * inv_w;
        float const ny = (m[4] * world.x + m[5] * world.y + m[6] * world.z + m[7]) * inv_w;
        out.x = width * 0.5f + nx * (width * 0.5f);
        out.y = height * 0.5f - ny * (height * 0.5f);
        return true;
    }

    Vector eye;
    QAngle angles;
    GetLocalEye(eye, angles);
    if (eye.x == 0.0f && eye.y == 0.0f && eye.z == 0.0f) return false;

    Vector forward, right, up;
    AngleVectors(angles, forward, right, up);

    Vector delta = world - eye;
    float x = Dot(delta, right);
    float y = Dot(delta, up);
    float z = Dot(delta, forward);
    if (z < 1.0f) return false;

    // Source defines CViewSetup::fov as the horizontal FOV at 4:3.  The actual
    // horizontal half-angle is scaled by aspect/(4/3); the vertical half-angle
    // stays tan(fov/2)*(3/4).  Using tan(fov/2) directly here makes the boxes
    // slide relative to the enemies when turning, because the X scale is off.
    float fov = view_fov;
    if (fov < 1.0f || fov > 179.0f) fov = 90.0f;
    float aspect = view_aspect > 0.1f ? view_aspect : (float)width / (float)height;

    float tan_half = tanf(Deg2Rad(fov * 0.5f));
    float tan_x = tan_half * (aspect / (4.0f / 3.0f));
    float tan_y = tan_half * (3.0f / 4.0f);

    out.x = width * 0.5f + (x / (z * tan_x)) * (width * 0.5f);
    out.y = height * 0.5f - (y / (z * tan_y)) * (height * 0.5f);
    return true;
}

struct SmoothBox {
    ImVec2 min, max;
    float time;
    bool valid;
};
static SmoothBox g_boxes[65];

static void SmoothBoxPositions(int index, ImVec2& mn, ImVec2& mx) {
    if (cfg.esp_smooth <= 0.01f) {
        g_boxes[index].valid = false;
        return;
    }
    float now = (float)ImGui::GetTime();
    SmoothBox& b = g_boxes[index];
    float dt = now - b.time;
    b.time = now;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

    float dx = mn.x - b.min.x, dy = mn.y - b.min.y;
    if (!b.valid || fabsf(dx) + fabsf(dy) > 250.0f) {
        b.min = mn; b.max = mx; b.valid = true;
        return;
    }
    float t = 1.0f - expf(-cfg.esp_smooth * dt);
    if (t > 1.0f) t = 1.0f;
    b.min.x += (mn.x - b.min.x) * t;
    b.min.y += (mn.y - b.min.y) * t;
    b.max.x += (mx.x - b.max.x) * t;
    b.max.y += (mx.y - b.max.y) * t;
    mn = b.min; mx = b.max;
}

struct FadeState {
    void* ent;
    float alpha;
};
static FadeState g_fade[65];

static float EntityFade(int index, void* ent, bool dormant) {
    FadeState& f = g_fade[index];
    if (f.ent != ent) {
        f.ent = ent;
        f.alpha = dormant ? 0.0f : 1.0f;
        return f.alpha;
    }

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.2f) dt = 1.0f / 60.0f;

    if (!cfg.esp_dormant_fade) {
        f.alpha = dormant ? 0.0f : 1.0f;
        return f.alpha;
    }

    if (!dormant) {
        f.alpha += (1.0f - f.alpha) * 8.0f * dt;
        if (f.alpha > 1.0f) f.alpha = 1.0f;
    } else {
        float fade = cfg.esp_dormant_fade_time;
        if (fade < 0.1f) fade = 0.1f;
        f.alpha -= dt / fade;
        if (f.alpha < 0.0f) f.alpha = 0.0f;
    }
    return f.alpha;
}

static void DrawTextShadow(ImDrawList* draw, ImVec2 pos, ImU32 col, const char* text, float alpha) {
    ImFont* font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    draw->AddText(font, size, ImVec2(pos.x + 1, pos.y + 1), ImColor(0, 0, 0, (int)(120 * alpha)), text);
    draw->AddText(font, size, pos, col, text);
}

static void DrawLineShadow(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 col, float alpha) {
    draw->AddLine(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x + 1, b.y + 1), ImColor(0, 0, 0, (int)(120 * alpha)), 1.0f);
    draw->AddLine(a, b, col, 1.0f);
}

static void DrawStickSkeleton(ImDrawList* draw, void* entity, int width, int height, float alpha) {
    Vector origin = *reinterpret_cast<Vector*>((char*)entity + off::m_vecOrigin);
    Vector view = *reinterpret_cast<Vector*>((char*)entity + off::m_vecViewOffset);
    if (!(view.z > 1.0f && view.z < 100.0f)) view.z = 64.0f;

    float yaw = *reinterpret_cast<float*>((char*)entity + off::m_angRotation + 4); // m_angRotation.y
    Vector fwd, right, up;
    AngleVectors(QAngle(0, yaw, 0), fwd, right, up);

    Vector head    = origin + Vector(0, 0, view.z);
    Vector neck    = origin + Vector(0, 0, view.z * 0.86f);
    Vector chest   = origin + Vector(0, 0, view.z * 0.66f);
    Vector pelvis  = origin + Vector(0, 0, view.z * 0.44f);
    Vector shL     = neck  - right * 8.0f;
    Vector shR     = neck  + right * 8.0f;
    Vector handL   = chest - right * 12.0f - fwd * 2.0f;
    Vector handR   = chest + right * 12.0f - fwd * 2.0f;
    Vector hipL    = pelvis - right * 6.0f;
    Vector hipR    = pelvis + right * 6.0f;
    Vector footL   = origin - right * 7.0f;
    Vector footR   = origin + right * 7.0f;

    auto line = [&](const Vector& a, const Vector& b) {
        ImVec2 sa, sb;
        if (WorldToScreen(a, sa, width, height) && WorldToScreen(b, sb, width, height))
            DrawLineShadow(draw, sa, sb, ImColor(255, 255, 255, (int)(200 * alpha)), alpha);
    };

    line(head, neck);
    line(neck, chest);
    line(chest, pelvis);
    line(neck, shL); line(shL, handL);
    line(neck, shR); line(shR, handR);
    line(pelvis, hipL); line(hipL, footL);
    line(pelvis, hipR); line(hipR, footR);
}

static void DrawSkeleton(ImDrawList* draw, void* entity, int index, int width, int height, float alpha,
                         Vector const* offset = nullptr, ImColor const* color = nullptr) {
    SetStage("skeleton SetupBones");
    int count = 0;
    Vector cached_org;
    const matrix3x4_t* bones = GetEntityBonesPtr(index, entity, &count, &cached_org);
    if (!bones) {
        if (!offset)
            DrawStickSkeleton(draw, entity, width, height, alpha);
        return;
    }
#ifndef _WIN64
    Vector const cur_org = *reinterpret_cast<Vector*>((char*)entity + off::m_vecOrigin);
    Vector const bone_delta = cur_org - cached_org;
#else
    Vector const bone_delta(0.0f, 0.0f, 0.0f);
#endif

    auto line_bones = [&](int a, int b) {
        if (a < 0 || b < 0 || a == b || a >= count || b >= count) return;
        Vector wa(bones[a].m[0][3] + bone_delta.x, bones[a].m[1][3] + bone_delta.y, bones[a].m[2][3] + bone_delta.z);
        Vector wb(bones[b].m[0][3] + bone_delta.x, bones[b].m[1][3] + bone_delta.y, bones[b].m[2][3] + bone_delta.z);
        if (offset) { wa = wa + *offset; wb = wb + *offset; }
        if ((wa.x == 0 && wa.y == 0 && wa.z == 0) || (wb.x == 0 && wb.y == 0 && wb.z == 0))
            return;
        ImVec2 sa, sb;
        if (!WorldToScreen(wa, sa, width, height)) return;
        if (!WorldToScreen(wb, sb, width, height)) return;
        ImColor const col = color ? *color : ImColor(255, 255, 255, (int)(220 * alpha));
        DrawLineShadow(draw, sa, sb, col, alpha);
    };

    BoneMap* map = GetBoneMap(entity);
    if (!map) {
        // default v44 player skeleton; keeps the previous animated skeleton
        // when the model's header cannot be resolved
        static const int legacy[][2] = {
            {0, 9}, {9, 10}, {10, 11}, {11, 12}, {12, 13}, {13, 14},
            {0, 1}, {1, 2}, {2, 3},
            {0, 5}, {5, 6}, {6, 7},
            {12, 15}, {15, 16}, {16, 17}, {17, 18},
            {12, 28}, {28, 29}, {29, 30}, {30, 31},
        };
        for (auto& p : legacy) line_bones(p[0], p[1]);
        return;
    }

    int const spine4 = map->spine4 >= 0 ? map->spine4 : map->spine2;
    int const neck = map->neck1 >= 0 ? map->neck1 : spine4;
    int const head = map->head1 >= 0 ? map->head1 : map->neck1;

    line_bones(map->pelvis, map->spine);
    line_bones(map->spine, map->spine1);
    line_bones(map->spine1, map->spine2);
    line_bones(map->spine2, spine4);
    line_bones(spine4, neck);
    line_bones(neck, head);

    line_bones(map->pelvis, map->l_thigh);
    line_bones(map->l_thigh, map->l_calf);
    line_bones(map->l_calf, map->l_foot);
    line_bones(map->pelvis, map->r_thigh);
    line_bones(map->r_thigh, map->r_calf);
    line_bones(map->r_calf, map->r_foot);

    line_bones(spine4, map->l_clav);
    line_bones(map->l_clav, map->l_upper);
    line_bones(map->l_upper, map->l_fore);
    line_bones(map->l_fore, map->l_hand);
    line_bones(spine4, map->r_clav);
    line_bones(map->r_clav, map->r_upper);
    line_bones(map->r_upper, map->r_fore);
    line_bones(map->r_fore, map->r_hand);
}

static float WrapAngle(float a) {
    a = fmodf(a + 180.0f, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a - 180.0f;
}

static ImU32 EspColor(const float c[4], float alpha, bool gray, float gray_amt) {
    float r = c[0], g = c[1], b = c[2];
    if (gray) {
        float const y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        r += (y - r) * gray_amt;
        g += (y - g) * gray_amt;
        b += (y - b) * gray_amt;
    }
    float a = c[3] * alpha;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return ImColor(r, g, b, a);
}

static void DrawOffscreenArrow(ImDrawList* draw, int width, int height,
                               const Vector& target, const Vector& eye,
                               const QAngle& camera, float alpha) {
    const float rad2deg = 57.29577951308232f;
    float const yaw = atan2f(target.y - eye.y, target.x - eye.x) * rad2deg;
    float const diff = WrapAngle(yaw - camera.y) / rad2deg;
    ImVec2 const dir(sinf(diff), -cosf(diff));
    ImVec2 const perp(-dir.y, dir.x);
    ImVec2 const center(width * 0.5f, height * 0.5f);
    float const radius = cfg.esp_offscreen_radius;
    float const size = 11.0f * cfg.esp_offscreen_size;
    ImVec2 const pos(center.x + dir.x * radius, center.y + dir.y * radius);
    ImVec2 const tip(pos.x + dir.x * size, pos.y + dir.y * size);
    ImVec2 const a(pos.x + perp.x * size * 0.62f, pos.y + perp.y * size * 0.62f);
    ImVec2 const b(pos.x - perp.x * size * 0.62f, pos.y - perp.y * size * 0.62f);
    draw->AddTriangleFilled(tip, a, b, EspColor(cfg.esp_offscreen_color, alpha, false, 0.0f));
    draw->AddTriangle(tip, a, b, IM_COL32(0, 0, 0, 180), 1.0f);
}

struct EspWeaponInfo {
    char name[48] = { 0 };
    int clip = 0;
    int reserve = 0;
    bool valid = false;
};

static const char* WeaponNameFromModel(const char* model) {
    if (!model) return "Weapon";
    struct Pair { const char* token; const char* name; };
    // Network class names (CWeaponAK47, CKnife, ...)
    static const Pair classes[] = {
        { "AK47", "AK-47" }, { "M4A1", "M4A1" }, { "AWP", "AWP" }, { "Deagle", "Deagle" },
        { "Glock", "Glock" }, { "USP", "USP" }, { "P228", "P228" }, { "FiveSeven", "Five-SeveN" },
        { "Elite", "Dual Berettas" }, { "MAC10", "MAC-10" }, { "TMP", "TMP" }, { "MP5", "MP5" },
        { "UMP45", "UMP-45" }, { "P90", "P90" }, { "Galil", "Galil" }, { "Famas", "FAMAS" },
        { "SG552", "SG552" }, { "Aug", "AUG" }, { "Scout", "Scout" }, { "SG550", "SG550" },
        { "G3SG1", "G3SG1" }, { "M3", "M3" }, { "XM1014", "XM1014" }, { "M249", "M249" },
        { "Negev", "Negev" }, { "Knife", "Knife" }, { "HEGrenade", "HE" }, { "Flashbang", "Flash" },
        { "SmokeGrenade", "Smoke" }, { "Decoy", "Decoy" }, { "C4", "C4" }, { "Taser", "Taser" },
        { "Shield", "Shield" } };
    for (auto const& p : classes)
        if (strstr(model, p.token)) return p.name;

    static const Pair pairs[] = {
        { "ak47", "AK-47" }, { "m4a1", "M4A1" }, { "awp", "AWP" }, { "deagle", "Deagle" },
        { "glock", "Glock" }, { "usp", "USP" }, { "p228", "P228" }, { "fiveseven", "Five-SeveN" },
        { "elite", "Dual Berettas" }, { "mac10", "MAC-10" }, { "tmp", "TMP" }, { "mp5", "MP5" },
        { "ump45", "UMP-45" }, { "p90", "P90" }, { "galil", "Galil" }, { "famas", "FAMAS" },
        { "sg552", "SG552" }, { "aug", "AUG" }, { "scout", "Scout" }, { "sg550", "SG550" },
        { "g3sg1", "G3SG1" }, { "m3", "M3" }, { "xm1014", "XM1014" }, { "m249", "M249" },
        { "negev", "Negev" }, { "knife", "Knife" }, { "hegrenade", "HE" }, { "flashbang", "Flash" },
        { "smokegrenade", "Smoke" }, { "decoy", "Decoy" }, { "c4", "C4" }, { "taser", "Taser" },
        { "shield", "Shield" },
    };
    for (auto const& p : pairs)
        if (strstr(model, p.token)) return p.name;
    return "Weapon";
}

static void GetEspWeaponInfo(void* player, EspWeaponInfo& out) {
    out = {};
    if (!player) return;
    int const handle = *reinterpret_cast<int*>((char*)player + off::m_hActiveWeapon);
    int const idx = handle & 0xFFF;
    if (idx <= 0) return;
    void* weapon = entity_list.GetClientEntity(idx);
    if (!weapon) return;

    const char* model = nullptr;
    void* nw = entity_list.GetClientNetworkable(idx);
    if (nw) {
        void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
        if (cc)
            model = ClientClassName(cc);
    }
    strncpy(out.name, WeaponNameFromModel(model), sizeof(out.name) - 1);
    out.clip = *reinterpret_cast<int*>((char*)weapon + off::w_iClip1);
    int const ammo_type = *reinterpret_cast<int*>((char*)weapon + off::w_iPrimaryAmmoType);
    if (ammo_type >= 0 && ammo_type < 32)
        out.reserve = *reinterpret_cast<int*>((char*)player + off::m_iAmmo + ammo_type * 4);
    out.valid = true;
}

static const char* WeaponGlyph(const char* name) {
    if (!name) return nullptr;
    struct G { const char* name; const char* glyph; };
    static const G glyphs[] = {
        { "AK-47", "W" }, { "M4A1", "T" }, { "AWP", "Z" }, { "Deagle", "A" },
        { "Glock", "D" }, { "USP", "G" }, { "P228", "E" }, { "Five-SeveN", "C" },
        { "Dual Berettas", "B" }, { "MAC-10", "K" }, { "TMP", "N" }, { "MP5", "N" },
        { "UMP-45", "L" }, { "P90", "P" }, { "Galil", "Q" }, { "FAMAS", "R" },
        { "SG552", "V" }, { "AUG", "U" }, { "Scout", "a" }, { "SG550", "Y" },
        { "G3SG1", "X" }, { "M3", "c" }, { "XM1014", "b" }, { "M249", "g" },
        { "Negev", "f" }, { "Knife", "]" }, { "HE", "j" }, { "Flash", "i" },
        { "Smoke", "k" }, { "Decoy", "m" }, { "C4", "o" }, { "Taser", "h" },
    };
    for (auto const& g : glyphs)
        if (strcmp(name, g.name) == 0) return g.glyph;
    return nullptr;
}

// magazine size per weapon (for the dropped-weapon ammo bar); 0 = no ammo
static int WeaponMaxClip(const char* name) {
    if (!name) return 0;
    struct C { const char* name; int clip; };
    static const C clips[] = {
        { "AK-47", 30 }, { "M4A1", 30 }, { "AWP", 10 }, { "Deagle", 7 },
        { "Glock", 20 }, { "USP", 12 }, { "P228", 13 }, { "Five-SeveN", 20 },
        { "Dual Berettas", 30 }, { "MAC-10", 30 }, { "TMP", 30 }, { "MP5", 30 },
        { "UMP-45", 25 }, { "P90", 50 }, { "Galil", 35 }, { "FAMAS", 25 },
        { "SG552", 30 }, { "AUG", 30 }, { "Scout", 10 }, { "SG550", 30 },
        { "G3SG1", 20 }, { "M3", 8 }, { "XM1014", 7 }, { "M249", 100 },
        { "Negev", 150 }, { "Taser", 1 },
    };
    for (auto const& c : clips)
        if (strcmp(name, c.name) == 0) return c.clip;
    return 0;
}

static void DrawIconText(ImDrawList* draw, ImVec2 const& pos, const char* glyph,
                         const float col[4], float alpha) {
    if (!glyph || !alice::core || !alice::core->fonts) return;
    auto& f = alice::core->fonts->get("undefeated");
    if (!f.ptr) return;
    draw->AddText(f.ptr, f.size, pos,
        IM_COL32((int)(col[0] * 255), (int)(col[1] * 255), (int)(col[2] * 255),
                 (int)(col[3] * 255 * alpha)), glyph);
}

// Screen-space bounding box of the same bone positions DrawSkeleton uses.
// This makes the ESP box hug the skeleton exactly instead of being derived
// from two projected world points (which trailed the animated pose/position).
static bool GetBoneScreenBounds(int index, void* entity, int width, int height,
                                ImVec2& mn, ImVec2& mx) {
    matrix3x4_t bones[128];
    int count = 0;
    if (!GetEntityBones(index, entity, bones, &count)) return false;

    int ids[32];
    int n = 0;
    if (BoneMap* map = GetBoneMap(entity)) {
        int const body[] = {
            map->pelvis, map->spine, map->spine1, map->spine2, map->spine4,
            map->neck1, map->head1,
            map->l_thigh, map->l_calf, map->l_foot,
            map->r_thigh, map->r_calf, map->r_foot,
            map->l_upper, map->l_fore, map->l_hand,
            map->r_upper, map->r_fore, map->r_hand,
        };
        for (int id : body) {
            if (id < 0 || id >= count) continue;
            bool dup = false;
            for (int k = 0; k < n; k++) if (ids[k] == id) { dup = true; break; }
            if (!dup) ids[n++] = id;
        }
    }
    if (n < 3) {
        n = 0;
        for (int i = 0; i < count && n < 32; i++) ids[n++] = i;
    }

    ImVec2 lo(1.0e9f, 1.0e9f), hi(-1.0e9f, -1.0e9f);
    int projected = 0;
    for (int k = 0; k < n; k++) {
        int const i = ids[k];
        Vector const p(bones[i].m[0][3], bones[i].m[1][3], bones[i].m[2][3]);
        if (p.x == 0.0f && p.y == 0.0f && p.z == 0.0f) continue;
        if (fabsf(p.x) > 32768.0f || fabsf(p.y) > 32768.0f || fabsf(p.z) > 32768.0f) continue;
        ImVec2 s;
        if (!WorldToScreen(p, s, width, height)) continue;
        if (s.x < lo.x) lo.x = s.x;
        if (s.y < lo.y) lo.y = s.y;
        if (s.x > hi.x) hi.x = s.x;
        if (s.y > hi.y) hi.y = s.y;
        projected++;
    }
    if (projected < 3 || hi.x <= lo.x || hi.y <= lo.y) return false;

    mn = lo;
    mx = hi;
    return true;
}

// ---------------------------------------------------------------------------
// backtrack (lag compensation) + miss-count resolver
// ---------------------------------------------------------------------------
// CS:S servers rewind the world by cmd tick_count (sv_maxunlag, 0.5s by
// default), so aiming at a recorded historical position hits where the enemy
// was - the classic source backtrack.  Records are kept per player for the
// last half second at simulation granularity (one entry per frame is plenty).
struct BtRecord {
    float t;        // engine time of the record
    float simtime;  // entity m_flSimulationTime for tick_count lag comp
    Vector origin;
    float eye_z;
    bool used;
};
static BtRecord g_bt_records[65][40];
static int g_bt_head[65];

void Backtrack_Record(int index, void* entity) {
    if (index <= 0 || index > 64 || !entity) return;
    if (!cfg.backtrack) return;
    float const now = engine.self ? engine.Time() : 0.0f;
    if (now <= 0.0f) return;

    BtRecord* ring = g_bt_records[index];
    int& head = g_bt_head[index];
    Vector const origin = *reinterpret_cast<Vector*>((char*)entity + off::m_vecOrigin);
    float const simtime = *reinterpret_cast<float*>((char*)entity + off::m_flSimulationTime);
    Vector vo = *reinterpret_cast<Vector*>((char*)entity + off::m_vecViewOffset);
    if (!(vo.z > 1.0f && vo.z < 100.0f)) vo.z = 64.0f;

    head = (head + 1) % 40;
    ring[head].t = now;
    ring[head].simtime = simtime;
    ring[head].origin = origin;
    ring[head].eye_z = vo.z;
    ring[head].used = true;

    // drop stale entries
    float const cutoff = now - 0.55f;
    for (int i = 0; i < 40; i++)
        if (ring[i].used && ring[i].t < cutoff)
            ring[i].used = false;
}

bool Backtrack_PickRecord(int index, float seconds_ago, Vector& out_origin, float& out_simtime) {
    if (index <= 0 || index > 64) return false;
    float const now = engine.self ? engine.Time() : 0.0f;
    float const want = now - seconds_ago;
    BtRecord const* ring = g_bt_records[index];
    float best_d = 1e9f;
    bool any = false;
    for (int i = 0; i < 40; i++) {
        if (!ring[i].used) continue;
        float const d = fabsf(ring[i].t - want);
        if (d < best_d) {
            best_d = d;
            out_origin = ring[i].origin;
            out_simtime = ring[i].simtime;
            any = true;
        }
    }
    return any;
}

Vector Backtrack_PickRaw(int index, float seconds_ago) {
    Vector best;
    float simtime = 0.0f;
    if (!Backtrack_PickRecord(index, seconds_ago, best, simtime))
        return Vector(0, 0, 0);
    return best;
}

Vector Backtrack_Pick(int index, float seconds_ago, float eye_z) {
    float const now = engine.self ? engine.Time() : 0.0f;
    float const want = now - seconds_ago;
    BtRecord const* ring = g_bt_records[index];
    int const head = g_bt_head[index];
    Vector best;
    float best_d = 1e9f;
    bool any = false;
    for (int i = 0; i < 40; i++) {
        if (!ring[i].used) continue;
        float const d = fabsf(ring[i].t - want);
        if (d < best_d) {
            best_d = d;
            best = ring[i].origin;
            any = true;
        }
    }
    if (!any) return Vector(0, 0, 0);
    return best + Vector(0.0f, 0.0f, eye_z * 0.72f);
}

// miss-count resolver: after consecutive misses on a target, shift the aim
// point sideways, flipping the side every 2 misses (classic miss resolver)
struct ResolverState {
    int misses;
    int hits;
    int side;        // -1 left, +1 right
    float last_flip;
};
static ResolverState g_resolver[65] = {};

void Resolver_OnMiss(int index) {
    if (index <= 0 || index > 64) return;
    ResolverState& r = g_resolver[index];
    r.misses++;
    r.hits = 0;
    float const now = engine.self ? engine.Time() : 0.0f;
    if (r.misses % 2 == 0) {
        r.side = -r.side;
        r.last_flip = now;
    }
}

void Resolver_OnHit(int index) {
    if (index <= 0 || index > 64) return;
    g_resolver[index].misses = 0;
    g_resolver[index].hits++;
}

int Resolver_Side(int index) {
    if (index <= 0 || index > 64) return 0;
    ResolverState const& r = g_resolver[index];
    if (r.misses < 2) return 0; // a single miss is normal spread - stay put
    int const strength = r.misses / 2;
    int const units = strength > 3 ? 3 : strength; // 1..3 == 8/16/24 units
    return r.side * units * 8;
}

bool Resolver_IsResolved(int index) {
    if (index <= 0 || index > 64) return false;
    ResolverState const& r = g_resolver[index];
    return r.hits > 0 && r.misses == 0;
}

static char g_flag_buf_money[16];
static char g_flag_buf_armor[8];

void DrawESP(ImDrawList* draw, int width, int height) {
    if (!IsGameReady()) return;
    SetStage("ESP loop");

    // bullet impact marks (custom) are drawn by the engine's debug
    // overlay from FrameStageNotify; tracers and kill effects are drawn in
    // EndScene so they work with ESP off too.

    static int frame = 0;
    static int last_targets = 0;
    frame++;
    g_bone_frame++;
    int targets = 0;

    void* local = GetLocalEntity();
    Vector camera_eye;
    QAngle camera_angles;
    GetLocalEye(camera_eye, camera_angles);

    int local_team = 0;
    if (local)
        local_team = *reinterpret_cast<int*>((char*)local + off::m_iTeamNum);

    for (int i = 1; i <= 64; i++) {
        void* entity = entity_list.GetClientEntity(i);
        if (!entity || entity == local) continue;
        player_info_t esp_info = {};
        if (!engine.GetPlayerInfo(i, &esp_info)) continue;

        int team = *reinterpret_cast<int*>((char*)entity + off::m_iTeamNum);
        if (cfg.esp_team_check && local && team == local_team) continue;

        char life = *reinterpret_cast<char*>((char*)entity + off::m_lifeState);
        int health = *reinterpret_cast<int*>((char*)entity + off::m_iHealth);
        bool alive = (life == 0 && health > 0 && health <= 100);

        bool dormant = EntityIsStale(entity, i);
        if (dormant && !cfg.esp_show_dormant) continue;

        Vector origin = *reinterpret_cast<Vector*>((char*)entity + off::m_vecOrigin);
        Vector view = *reinterpret_cast<Vector*>((char*)entity + off::m_vecViewOffset);
        if (!(view.z > 1.0f && view.z < 100.0f)) view.z = 64.0f;

        // entity == local already excludes our own body; no distance filter
        // here, otherwise enemies standing right next to us are never drawn.
        // Bones only for live, non-dormant players; everyone else gets the
        // origin/view-offset fallback box.
        static struct { void* ent; Vector feet, top; bool valid; } g_last_bounds[65] = {};
        Vector feet, top;
        bool const fresh = alive && !dormant && GetBoneBounds(i, entity, feet, top);
        if (fresh) {
            g_last_bounds[i].ent = entity;
            g_last_bounds[i].feet = feet;
            g_last_bounds[i].top = top;
            g_last_bounds[i].valid = true;
        } else if (!alive && g_last_bounds[i].valid && g_last_bounds[i].ent == entity) {
            // death fade: hold the last real box so it does not jump
            feet = g_last_bounds[i].feet;
            top = g_last_bounds[i].top;
        } else {
            g_last_bounds[i].valid = false;
            feet = origin;
            top = origin + Vector(0, 0, view.z + 8.0f);
        }

        ImVec2 s_feet, s_top;
        if (!WorldToScreen(feet, s_feet, width, height) ||
            !WorldToScreen(top, s_top, width, height)) {
            if (alive && cfg.esp_offscreen)
                DrawOffscreenArrow(draw, width, height, origin, camera_eye, camera_angles, 1.0f);
            continue;
        }

        float box_h = s_feet.y - s_top.y;
        if (box_h < 6.0f) continue;
        float box_w = box_h * 0.45f;
        if (box_w < 4.0f) box_w = 4.0f;

        float const mid_x = (s_top.x + s_feet.x) * 0.5f;
        ImVec2 min(mid_x - box_w * 0.5f, s_top.y);
        ImVec2 max(mid_x + box_w * 0.5f, s_feet.y);

        // corpses and dormant ghosts fade out instead of sticking around
        float alpha = EntityFade(i, entity, dormant || life != 0);
        if (dormant) alpha *= cfg.esp_dormant_alpha;
        if (alpha <= 0.01f) continue;
        bool const gray = dormant && cfg.esp_dormant_grayscale > 0.001f;
        float const gray_amt = cfg.esp_dormant_grayscale;

        if (alive && !dormant)
            Backtrack_Record(i, entity);

        // backtrack ghost: a second skeleton at the historical position
        if (alive && !dormant && cfg.backtrack && cfg.backtrack_skeleton && cfg.esp && !cfg.esp_team_check) {
            Vector const bt = Backtrack_PickRaw(i, cfg.backtrack_time);
            if (!(bt.x == 0.0f && bt.y == 0.0f && bt.z == 0.0f)) {
                Vector const delta = bt - origin;
                ImColor const ghost(90, 180, 255, (int)(170 * alpha));
                DrawSkeleton(draw, entity, i, width, height, alpha * 0.85f, &delta, &ghost);
            }
        }

        // Layout-driven elements (ESP layout engine).  Every
        // element keeps its old visuals; sides/orders come from cfg.esp_layout
        // (draggable in the menu preview, Players tab).
        // lives for the whole iteration: view.name points into it, so later
        // stack re-use (weapon info etc.) cannot clobber the nickname
        esp_layout::PlayerView espv;
        espv.alive = alive;
        espv.gray = gray;
        espv.gray_amt = gray_amt;

        if (alive) {
            espv.health = std::clamp(health / 100.0f, 0.0f, 1.0f);
            espv.armor = std::clamp(*reinterpret_cast<int*>((char*)entity + off::m_ArmorValue), 0, 100);

            if ((cfg.esp_name || (!alive && g_last_bounds[i].valid)) && esp_info.name[0])
                espv.name = esp_info.name;

            if (cfg.esp_distance) {
                Vector eye;
                QAngle angles;
                GetLocalEye(eye, angles);
                if (!(eye.x == 0.0f && eye.y == 0.0f && eye.z == 0.0f)) {
                    Vector d = origin - eye;
                    float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
                    snprintf(espv.distance_text, sizeof(espv.distance_text), "%dm", (int)(dist * 0.0254f));
                }
            }

            EspWeaponInfo wi;
            if (cfg.esp_weapon || cfg.esp_weapon_icon || cfg.esp_ammo || cfg.esp_c4_icon || cfg.esp_taser_icon || cfg.flag_c4)
                GetEspWeaponInfo(entity, wi);
            if ((cfg.esp_weapon || cfg.esp_weapon_icon) && wi.valid) {
                espv.has_weapon = true;
                espv.weapon = wi.name;
            }
            if (cfg.esp_ammo && wi.valid) {
                espv.has_ammo = true;
                snprintf(espv.ammo_text, sizeof(espv.ammo_text), "%d / %d", wi.clip, wi.reserve);
            }

            if (cfg.esp_kit_icon && *reinterpret_cast<char*>((char*)entity + off::m_bHasDefuser))
                espv.kit_icon = true;
            if (cfg.esp_c4_icon && wi.valid && strcmp(wi.name, "C4") == 0)
                espv.c4_icon = true;
            if (cfg.esp_taser_icon && wi.valid && strcmp(wi.name, "Taser") == 0)
                espv.taser_icon = true;

            if (cfg.esp_flags) {
                if (cfg.flag_money) {
                    int const money = *reinterpret_cast<int*>((char*)entity + off::m_iAccount);
                    if (money > 0)
                        snprintf(g_flag_buf_money, sizeof(g_flag_buf_money), "$%d", money),
                        espv.flag_money = g_flag_buf_money;
                }
                if (cfg.flag_armor) {
                    int const armor = *reinterpret_cast<int*>((char*)entity + off::m_ArmorValue);
                    int const helmet = *reinterpret_cast<int*>((char*)entity + off::m_bHasHelmet);
                    if (armor > 0) {
                        strcpy(g_flag_buf_armor, helmet ? "HK" : "K");
                        espv.flag_armor = g_flag_buf_armor;
                    }
                }
                if (cfg.flag_kit && *reinterpret_cast<char*>((char*)entity + off::m_bHasDefuser))
                    espv.flag_kit = "KIT";
                if (cfg.flag_defuse && *reinterpret_cast<char*>((char*)entity + off::m_bIsDefusing))
                    espv.flag_defuse = "DEFUSE";
                if (cfg.flag_flashed) {
                    float const flash = *reinterpret_cast<float*>((char*)entity + off::m_flFlashDuration);
                    if (flash > 0.2f)
                        espv.flag_flashed = "FLASH";
                }
                if (cfg.flag_c4 && wi.valid && strcmp(wi.name, "C4") == 0)
                    espv.flag_c4 = "C4";
            }
        }

        esp_layout::RenderStatic(draw, min, max, alpha, espv);

        if (alive && cfg.esp_skeleton && !dormant)
            DrawSkeleton(draw, entity, i, width, height, alpha);

        targets++;
    }

    // -------------------------------------------------------------------
    // Dropped weapon ESP: weapons lying in the world (invalid owner handle),
    // drawn at their origin with optional name / icon / ammo text / bar.
    // -------------------------------------------------------------------
    if (cfg.esp && cfg.dropped_weapons &&
        (cfg.dropped_name || cfg.dropped_icon || cfg.dropped_ammo_text || cfg.dropped_ammo_bar)) {
        static int dw_idx[512];
        static int dw_count = 0;
        static int dw_last_count = -1;
        static DWORD dw_scan = 0;
        DWORD const dw_now = GetTickCount();
        if (dw_now - dw_scan > 300) {
            dw_scan = dw_now;
            dw_count = 0;
            int const max_ent = entity_list.GetHighestEntityIndex();
            for (int i = 65; i <= max_ent && i < 2048 && dw_count < 512; i++) {
                void* e = entity_list.GetClientEntity(i);
                if (!e) continue;
                void* nw = entity_list.GetClientNetworkable(i);
                if (!nw) continue;
                void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
                const char* cls = ClientClassName(cc);
                if (!cls) continue;
                if (strstr(cls, "PlantedC4")) continue;
                if (!(strstr(cls, "Weapon") || strstr(cls, "Knife") || strstr(cls, "Grenade") ||
                      strcmp(cls, "CC4") == 0))
                    continue;
                int const owner = *reinterpret_cast<int*>((char*)e + off::w_hOwner);
                int const owner_idx = owner & 0xFFF;
                if (owner_idx > 0 && owner_idx <= 64 && entity_list.GetClientEntity(owner_idx))
                    continue; // held by a player - not dropped
                dw_idx[dw_count++] = i;
            }
            if (dw_count != dw_last_count) {
                dw_last_count = dw_count;
                Log("dropped weapons: %d in world", dw_count);
            }
        }

        for (int k = 0; k < dw_count; k++) {
            int const i = dw_idx[k];
            void* e = entity_list.GetClientEntity(i);
            if (!e) continue;
            void* nw = entity_list.GetClientNetworkable(i);
            if (!nw) continue;
            void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
            const char* cls = ClientClassName(cc);
            if (!cls) continue;

            Vector const org = *reinterpret_cast<Vector*>((char*)e + off::m_vecOrigin);
            ImVec2 sp;
            if (!WorldToScreen(org, sp, width, height)) continue;

            char const* name = WeaponNameFromModel(cls);
            int const clip = *reinterpret_cast<int*>((char*)e + off::w_iClip1);
            int const maxclip = WeaponMaxClip(name);
            bool const ammo_ok = clip >= 0 && maxclip > 0;

            bool const show_name = cfg.dropped_name && name && name[0];
            float const fsz = ImGui::GetFontSize();
            const char* glyph = cfg.dropped_icon ? WeaponGlyph(name) : nullptr;
            float icon_adv = 0.0f;
            ImFont* icon_font = nullptr;
            float icon_size = 0.0f;
            ImVec2 glyph_sz(0.0f, 0.0f);
            if (glyph && alice::core && alice::core->fonts) {
                auto& f = alice::core->fonts->get("undefeated");
                if (f.ptr) {
                    icon_font = f.ptr;
                    icon_size = std::min(f.size * 0.78f, fsz * 0.90f);
                    glyph_sz = icon_font->CalcTextSizeA(icon_size, FLT_MAX, 0.0f, glyph);
                    icon_adv = glyph_sz.x + 4.0f;
                } else {
                    glyph = nullptr;
                }
            }
            bool const show_icon = glyph != nullptr;
            bool const show_text = cfg.dropped_ammo_text && ammo_ok;
            bool const show_bar = cfg.dropped_ammo_bar && ammo_ok;

            bool const show_line = show_name || show_icon;
            ImVec2 const name_sz = show_name ? ImGui::CalcTextSize(name) : ImVec2(0.0f, 0.0f);
            char ammo[16] = {};
            if (show_text) snprintf(ammo, sizeof(ammo), "%d", clip);
            ImVec2 const ammo_sz = show_text ? ImGui::CalcTextSize(ammo) : ImVec2(0.0f, 0.0f);
            float bar_w = show_line ? (icon_adv + name_sz.x) : ammo_sz.x;
            if (bar_w < 46.0f)
                bar_w = 46.0f;
            if (show_text && ammo_sz.x > bar_w)
                bar_w = ammo_sz.x;

            float const gap = 1.0f;
            float const bar_h = 2.0f;
            float total = 0.0f;
            if (show_line) total += fsz;
            if (show_text) total += (total > 0.0f ? gap : 0.0f) + fsz;
            if (show_bar) total += (total > 0.0f ? gap : 0.0f) + bar_h + 1.0f;
            if (total <= 0.0f) continue;

            float y = sp.y - total - 3.0f;
            float const cx = sp.x;
            ImU32 const col = EspColor(cfg.dropped_color, 1.0f, false, 0.0f);
            ImU32 const sh = IM_COL32(0, 0, 0, 190);

            if (show_line) {
                float line_w = (show_icon ? icon_adv : 0.0f) + (show_name ? name_sz.x : 0.0f);
                float x = cx - line_w * 0.5f;
                if (show_icon) {
                    float const iy = y + (fsz - glyph_sz.y) * 0.5f;
                    draw->AddText(icon_font, icon_size, ImVec2(x + 1.0f, iy + 1.0f), sh, glyph);
                    draw->AddText(icon_font, icon_size, ImVec2(x, iy), col, glyph);
                    x += icon_adv;
                }
                if (show_name) {
                    draw->AddText(ImVec2(x + 1.0f, y + 1.0f), sh, name);
                    draw->AddText(ImVec2(x, y), col, name);
                }
                y += fsz + gap;
            }
            if (show_text) {
                float const x = cx - ammo_sz.x * 0.5f;
                draw->AddText(ImVec2(x + 1.0f, y + 1.0f), sh, ammo);
                draw->AddText(ImVec2(x, y), col, ammo);
                y += fsz + gap;
            }
            if (show_bar) {
                float const x = cx - bar_w * 0.5f;
                draw->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_w, y + bar_h),
                                    IM_COL32(0, 0, 0, 120));
                float const frac = std::clamp(clip / (float)maxclip, 0.0f, 1.0f);
                if (frac > 0.0f)
                    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_w * frac, y + bar_h), col);
                draw->AddRect(ImVec2(x - 1.0f, y - 1.0f), ImVec2(x + bar_w + 1.0f, y + bar_h + 1.0f),
                              IM_COL32(0, 0, 0, 160));
            }
        }
    }

    if (cfg.esp_bomb && IsGameReady()) {
        static void* bombs[8] = {};
        static DWORD last_scan = 0;
        DWORD const now_tick = GetTickCount();
        if (now_tick - last_scan > 500) {
            last_scan = now_tick;
            memset(bombs, 0, sizeof(bombs));
            int n = 0;
            int const max_ent = entity_list.GetHighestEntityIndex();
            for (int i = 1; i <= max_ent && i < 2048 && n < 8; i++) {
                void* e = entity_list.GetClientEntity(i);
                if (!e) continue;
                void* nw = entity_list.GetClientNetworkable(i);
                if (!nw) continue;
                void* cc = VFunc<void*(THISCALL_CC*)(void*)>(nw, 2)(nw);
                const char* cls = ClientClassName(cc);
                if (cls && strstr(cls, "PlantedC4"))
                    bombs[n++] = e;
            }
        }

        struct BombTiming { void* ent; float blow; float rem0; ULONGLONG t0; };
        static BombTiming timing[8] = {};

        const float pi = 3.14159265358979323846f;
        for (void* e : bombs) {
            if (!e) continue;
            // Stop drawing once the bomb is defused or has exploded.
            char const ticking = *reinterpret_cast<char*>((char*)e + off::c4_bBombTicking);
            if (!ticking) continue;

            float const blow = *reinterpret_cast<float*>((char*)e + off::c4_flC4Blow);
            float const len = *reinterpret_cast<float*>((char*)e + off::c4_flTimerLength);
            if (!(len > 1.0f && len < 120.0f)) continue;
            float initial = blow - engine.Time();
            if (initial <= 0.0f || initial > len + 1.0f) initial = len;

            // Cache the first observed blow time and count down with the wall
            // clock.  engine.Time() on a spectator client can jump or be on a
            // different tick base than the server entity, which made a fresh
            // 40s bomb show as a few seconds.
            BombTiming* slot = nullptr;
            for (auto& t : timing)
                if (t.ent == e) { slot = &t; break; }
            if (!slot)
                for (auto& t : timing)
                    if (!t.ent) { slot = &t; break; }
            if (slot) {
                if (slot->ent != e || slot->blow != blow) {
                    slot->ent = e;
                    slot->blow = blow;
                    slot->rem0 = initial;
                    slot->t0 = GetTickCount64();
                }
            }

            float const elapsed = slot ? (float)(GetTickCount64() - slot->t0) / 1000.0f : 0.0f;
            float const remaining = (slot ? slot->rem0 : initial) - elapsed;
            if (remaining <= 0.0f || remaining > len + 1.0f) continue;

            // A defuse countdown that has already finished also means done.
            float const defuse_down = *reinterpret_cast<float*>((char*)e + off::c4_flDefuseCountDown);
            if (defuse_down > 0.0f && engine.Time() > defuse_down) continue;

            Vector const org = *reinterpret_cast<Vector*>((char*)e + off::m_vecOrigin);
            ImVec2 sp;
            bool const in_fov = WorldToScreen(org, sp, width, height);

            auto draw_bomb_icon = [&](ImVec2 c) {
                float const white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                if (alice::core && alice::core->fonts) {
                    auto& iconf = alice::core->fonts->get("undefeated");
                    if (iconf.ptr) {
                        ImFontGlyph const* g = iconf.ptr->FindGlyph('o');
                        float const scale = iconf.size / iconf.ptr->FontSize;
                        float const gx = g ? (g->X0 + g->X1) * 0.5f * scale : 6.0f;
                        float const gy = g ? (g->Y0 + g->Y1) * 0.5f * scale : 9.0f;
                        DrawIconText(draw, ImVec2(c.x - gx, c.y - gy), "o", white, 1.0f);
                    }
                } else {
                    draw->AddRectFilled(ImVec2(c.x - 6.0f, c.y - 4.0f),
                                        ImVec2(c.x + 6.0f, c.y + 5.0f),
                                        IM_COL32(235, 235, 240, 255), 1.5f);
                    draw->AddRectFilled(ImVec2(c.x - 3.0f, c.y - 7.0f),
                                        ImVec2(c.x + 1.0f, c.y - 4.0f),
                                        IM_COL32(200, 200, 210, 255), 1.0f);
                    draw->AddCircleFilled(ImVec2(c.x + 2.5f, c.y - 1.0f), 1.6f,
                                          IM_COL32(255, 75, 75, 255), 10);
                }
            };

            float const frac = std::clamp(remaining / len, 0.0f, 1.0f);
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1fs", remaining);
            ImVec2 const ts = ImGui::CalcTextSize(buf);

            if (in_fov) {
                float const half_w = 34.0f, half_h = 33.0f;
                ImVec2 const mn(sp.x - half_w, sp.y - half_h);
                ImVec2 const mx(sp.x + half_w, sp.y + half_h);
                for (int gi = 6; gi >= 1; gi--) {
                    float const ex = gi * 3.0f;
                    draw->AddRectFilled(ImVec2(mn.x - ex, mn.y - ex), ImVec2(mx.x + ex, mx.y + ex),
                                        IM_COL32(1, 1, 1, 12 + (6 - gi) * 7), 12.f);
                }
                draw->AddRectFilled(mn, mx, IM_COL32(1, 1, 1, 235), 8.f);

                ImVec2 const icon_center(sp.x, mn.y + 23.0f);
                draw_bomb_icon(icon_center);
                draw->PathArcTo(icon_center, 15.0f, -pi * 0.5f, -pi * 0.5f + 2.0f * pi * frac, 40);
                draw->PathStroke(IM_COL32(255, 75, 75, 255), 0, 2.5f);

                draw->AddText(ImVec2(sp.x - ts.x * 0.5f, icon_center.y + 18.0f),
                              IM_COL32(255, 85, 85, 255), buf);
            } else {
                // Out-of-FOV horizontal card positioned at the user-dragged bomb_x/bomb_y
                float const cw = 144.0f, ch = 62.0f;
                float const x = std::clamp(cfg.bomb_x, 8.0f, std::max(8.0f, (float)width - cw - 8.0f));
                float const y = std::clamp(cfg.bomb_y, 8.0f, std::max(8.0f, (float)height - ch - 8.0f));

                ImVec2 const mn(x, y);
                ImVec2 const mx(x + cw, y + ch);
                for (int gi = 6; gi >= 1; gi--) {
                    float const ex = gi * 3.0f;
                    draw->AddRectFilled(ImVec2(mn.x - ex, mn.y - ex), ImVec2(mx.x + ex, mx.y + ex),
                                        IM_COL32(1, 1, 1, 12 + (6 - gi) * 7), 12.f);
                }
                draw->AddRectFilled(mn, mx, IM_COL32(1, 1, 1, 235), 8.f);

                ImVec2 const icon_center(x + 30.0f, y + ch * 0.5f);
                draw_bomb_icon(icon_center);
                draw->PathArcTo(icon_center, 15.0f, -pi * 0.5f, -pi * 0.5f + 2.0f * pi * frac, 40);
                draw->PathStroke(IM_COL32(255, 75, 75, 255), 0, 2.5f);

                float const tx = x + 60.0f;
                draw->AddText(ImVec2(tx, y + ch * 0.5f - 15.0f), IM_COL32(245, 245, 250, 255), "Planted");
                draw->AddText(ImVec2(tx, y + ch * 0.5f + 3.0f), IM_COL32(255, 85, 85, 255), buf);
            }
        }
    }

    last_targets = targets;

    if (frame == 240) {
        QAngle ea;
        engine.GetViewAngles(ea);
        Log("esp: targets=%d local=%d time=%.1f eng=%.1f/%.1f setup=%.1f/%.1f fov=%.1f asp=%.3f",
            targets, LocalIndex(), engine.self ? engine.Time() : -1.0f,
            ea.x, ea.y, view_angles.x, view_angles.y,
            view_fov, view_aspect);
    }
    SetStage("ESP done");
}

void PeekAssist_Draw(ImDrawList* draw, int width, int height) {
    if (!draw || !IsGameReady()) return;
    Vector center;
    if (!PeekAssist_GetCircle(center)) return;

    constexpr int SEGMENTS = 32;
    constexpr float RADIUS = 16.0f;
    constexpr float TWO_PI = 6.283185307179586f;
    ImVec2 pts[SEGMENTS];
    for (int s = 0; s < SEGMENTS; s++) {
        float const ang = (float)s / (float)SEGMENTS * TWO_PI;
        Vector const p(center.x + cosf(ang) * RADIUS, center.y + sinf(ang) * RADIUS, center.z);
        if (!WorldToScreen(p, pts[s], width, height))
            return;
    }

    int const r = std::clamp((int)(cfg.peek_assist_color[0] * 255.0f + 0.5f), 0, 255);
    int const g = std::clamp((int)(cfg.peek_assist_color[1] * 255.0f + 0.5f), 0, 255);
    int const b = std::clamp((int)(cfg.peek_assist_color[2] * 255.0f + 0.5f), 0, 255);
    float const a = std::clamp(cfg.peek_assist_color[3], 0.0f, 1.0f);
    draw->AddConvexPolyFilled(pts, SEGMENTS, IM_COL32(r, g, b, (int)(a * 0.25f * 255.0f)));
    draw->AddPolyline(pts, SEGMENTS, IM_COL32(r, g, b, (int)(a * 255.0f)), ImDrawFlags_Closed, 1.5f);
}

void Bones_OnFrameStage() {
#ifndef _WIN64
    bool const need_bones = cfg.esp_skeleton || cfg.aimbot || cfg.silentaim || cfg.triggerbot;
    if (!IsGameReady() || (!need_bones && !cfg.backtrack))
        return;
    static ULONGLONG s_last_ms = 0;
    ULONGLONG const now_ms = GetTickCount64();
    if (now_ms - s_last_ms < 16)
        return;
    s_last_ms = now_ms;

    g_bone_frame++;
    void* local = GetLocalEntity();
    int const local_team = local ? *reinterpret_cast<const int*>((const char*)local + off::m_iTeamNum) : 0;
    float const cur_time = engine.Time();
    for (int i = 1; i <= 64; i++) {
        void* entity = entity_list.GetClientEntity(i);
        if (!entity || entity == local) continue;
        if (!IsPlayerIndex(i)) continue;
        if (EntityIsStale(entity, i)) continue;
        char const life = *reinterpret_cast<const char*>((const char*)entity + off::m_lifeState);
        int const hp = *reinterpret_cast<const int*>((const char*)entity + off::m_iHealth);
        if (life != 0 || hp <= 0) continue;
        if (cfg.backtrack && local_team > 0 &&
            *reinterpret_cast<const int*>((const char*)entity + off::m_iTeamNum) != local_team)
            Backtrack_Record(i, entity);
        if (!need_bones) continue;
        if (cfg.esp_team_check && local_team > 0 &&
            *reinterpret_cast<const int*>((const char*)entity + off::m_iTeamNum) == local_team)
            continue;

        void* renderable = reinterpret_cast<char*>(entity) + 4;
        EnsureSetupBonesFn(renderable);
        if (!oSetupBones) continue;

        BoneCacheEntry& e = g_bone_cache[i];
        if (oSetupBones(renderable, e.bones, 128, 0x100, cur_time)) {
            e.origin = *reinterpret_cast<const Vector*>((const char*)entity + off::m_vecOrigin);
            e.count = 128;
            e.renderable = renderable;
            e.frame = g_bone_frame;
            e.valid = true;
            GetBoneMap(entity);
        }
    }
#endif
}
