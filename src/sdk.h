#pragma once
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdarg>

// ---------------------------------------------------------------------------
// minimal Source SDK surface for CS:S build 9540945 (x86-64)
// ---------------------------------------------------------------------------

struct Vector {
    float x, y, z;
    Vector() : x(0), y(0), z(0) {}
    Vector(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    Vector operator+(const Vector& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vector operator-(const Vector& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vector operator*(float s) const { return { x * s, y * s, z * s }; }
    Vector& operator+=(const Vector& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

struct QAngle {
    float x, y, z;
    QAngle() : x(0), y(0), z(0) {}
    QAngle(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct player_info_t {
    char name[32];
    int userID;
    char guid[33];
    unsigned int friendsID;
    char friendsName[32];
    bool fakeplayer;
    bool ishltv;
    unsigned int customFiles[4];
    unsigned char filesDownloaded;
    // Safety padding: the engine may write a larger structure than this
    // build's header definition; undersizing it corrupted the caller's stack.
    char _pad[256];
};

using CreateInterfaceFn = void* (__cdecl*)(const char* name, int* returnCode);

template <typename T>
inline T VFunc(void* inst, int index) {
    return reinterpret_cast<T>((*reinterpret_cast<void***>(inst))[index]);
}

inline void* GetInterface(HMODULE module, const char* name) {
    if (!module) return nullptr;
    auto fn = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
    if (!fn) return nullptr;
    return fn(name, nullptr);
}

struct matrix3x4_t {
    float m[3][4];
};

// IVRenderView (VEngineRenderView014).  Verified in engine.dll (CVRenderView
// vtable 0x1803932e0): [4] = SetBlend, [5] = GetBlend, [6] = SetColorModulation,
// [7] = GetColorModulation.
struct IVRenderView {
    void* self = nullptr;
    void SetColorModulation(const float* c) {
        VFunc<void(__fastcall*)(void*, const float*)>(self, 6)(self, c);
    }
    void SetBlend(float a) {
        VFunc<void(__fastcall*)(void*, float)>(self, 4)(self, a);
    }
};

// IVEngineClient (VEngineClient014)
struct IVEngineClient {
    void* self = nullptr;
    void GetScreenSize(int& w, int& h) { VFunc<void(__fastcall*)(void*, int&, int&)>(self, 5)(self, w, h); }
    bool GetPlayerInfo(int index, player_info_t* info) { return VFunc<bool(__fastcall*)(void*, int, player_info_t*)>(self, 8)(self, index, info); }
    int GetLocalPlayer() { return VFunc<int(__fastcall*)(void*)>(self, 12)(self); }
    void GetViewAngles(QAngle& a) { VFunc<void(__fastcall*)(void*, QAngle&)>(self, 19)(self, a); }
    void GetViewAngles(float* a) { VFunc<void(__fastcall*)(void*, float*)>(self, 19)(self, a); }
    void SetViewAngles(const QAngle& a) { VFunc<void(__fastcall*)(void*, const QAngle&)>(self, 20)(self, a); }
    float Time() { return VFunc<float(__fastcall*)(void*)>(self, 14)(self); }
    void ClientCmd(const char* cmd) { VFunc<void(__fastcall*)(void*, const char*)>(self, 7)(self, cmd); }
    const float* WorldToScreenMatrix() { return VFunc<const float*(__fastcall*)(void*)>(self, 36)(self); }
    void ExecuteClientCmd(const char* cmd) { VFunc<void(__fastcall*)(void*, const char*)>(self, 97)(self, cmd); }
};

// C_BasePlayer::SetLocalViewAngles is C_BasePlayer/C_CSPlayer vtable slot
// 0x950/8 = 298.  Verified in client.dll (sub_18009B580 writes pl.v_angle at
// +0x1564).  CViewRender reads that value, and CPrediction::SetupMove writes
// the outgoing cmd viewangles into it - that is what rotated the camera.
constexpr uintptr_t BASE_PLAYER_VTABLE_RVA = 0x404768;
constexpr uintptr_t CS_PLAYER_VTABLE_RVA   = 0x471098;
constexpr int PLAYER_SET_LOCAL_VIEW_ANGLES = 0x950 / 8;   // 298
constexpr int OFF_PLAYER_VIEW_ANGLES = 0x1564;            // pl.v_angle

// IVDebugOverlay (VDebugOverlay003).  The Vector overload is vtable[10] in
// build 9540945 (engine.dll+0xb9140, verified).  Returns 0 when the point is
// on screen, 1 when behind/off screen.  vtable[9] is a different overload.
// AddBoxOverlay is vtable[1] (same method order as the Source SDK:
// AddEntityTextOverlay, AddBoxOverlay, AddSphereOverlay, ...).
struct IVDebugOverlay {
    void* self = nullptr;
    void AddBoxOverlay(const Vector& origin, const Vector& mins, const Vector& maxs,
                       const QAngle& angles, int r, int g, int b, int a, float duration) {
        VFunc<void(__fastcall*)(void*, const Vector&, const Vector&, const Vector&,
                                const QAngle&, int, int, int, int, float)>(self, 1)(
            self, origin, mins, maxs, angles, r, g, b, a, duration);
    }
    int ScreenPosition(const Vector& point, Vector& screen) {
        return VFunc<int(__fastcall*)(void*, const Vector&, Vector&)>(self, 10)(self, point, screen);
    }
};

// ICvar / ConVar (VEngineCvar004)
struct ConVar {
    char pad[0x54];
    float f_value;   // 0x54
    int i_value;     // 0x58
};

struct ICvar {
    void* self = nullptr;
    ConVar* FindVar(const char* name) {
        return VFunc<ConVar*(__fastcall*)(void*, const char*)>(self, 12)(self, name);
    }
};

struct alignas(16) Ray_t {
    Vector m_Start; float p0;
    Vector m_Delta; float p1;
    Vector m_StartOffset; float p2;
    Vector m_Extents; float p3;
    bool m_IsRay; bool m_IsSwept;
    unsigned char pad[14];
};
struct IEngineTrace {
    void* self = nullptr;
    // vtable[0] = GetPointContents(const Vector&, IHandleEntity**)
    // verified in engine.dll (EngineTraceClient003)
    int GetPointContents(const Vector& pos, void** entity = nullptr) {
        return VFunc<int(__fastcall*)(void*, const Vector&, void**)>(self, 0)(self, pos, entity);
    }
    // vtable[4] = TraceRay(const Ray_t&, unsigned int, ITraceFilter*, trace_t*)
    // verified in engine.dll: sub_18018E7D0 references "CEngineTrace::TraceRay"
    void TraceRay(const Ray_t& ray, unsigned int mask, void* filter, void* tr) {
        VFunc<void(__fastcall*)(void*, const Ray_t&, unsigned int, void*, void*)>(self, 4)(self, ray, mask, filter, tr);
    }
};

// IPhysicsSurfaceProps (VPhysicsSurfaceProps001 from vphysics.dll).
// Verified in vphysics.dll: vtable[5] = GetSurfaceData(int), and in
// client.dll (FireBullet): surfacedata_t->game.material sits at +0x48.
struct IPhysicsSurfaceProps {
    void* self = nullptr;
    const void* GetSurfaceData(int index) {
        return VFunc<const void*(__fastcall*)(void*, int)>(self, 5)(self, index);
    }
    static uint16_t MaterialOf(const void* surface_data) {
        if (!surface_data) return 0;
        return *reinterpret_cast<const uint16_t*>((const char*)surface_data + 0x48);
    }
};

// material / model render
struct IMaterial {
    void* self = nullptr;
    const char* GetName() {
        return VFunc<const char*(__fastcall*)(void*)>(self, 0)(self);
    }
    void AlphaModulate(float a) {
        VFunc<void(__fastcall*)(void*, float)>(self, 27)(self, a);
    }
    void ColorModulate(float r, float g, float b) {
        VFunc<void(__fastcall*)(void*, float, float, float)>(self, 28)(self, r, g, b);
    }
    void SetMaterialVarFlag(int flag, bool on) {
        VFunc<void(__fastcall*)(void*, int, bool)>(self, 29)(self, flag, on);
    }
    bool GetMaterialVarFlag(int flag) {
        return VFunc<bool(__fastcall*)(void*, int)>(self, 30)(self, flag);
    }
    // CMaterial vtable[12] = lock inc dword [this+0x24]; vtable[13] = dec.
    void IncrementReferenceCount() {
        VFunc<void(__fastcall*)(void*)>(self, 12)(self);
    }
};

struct IMaterialSystem {
    void* self = nullptr;
    // Verified against materialsystem.dll: CMaterialSystem primary vtable,
    // entries [28]=ColorModulate(float,float,float), [29]=SetMaterialVarFlag,
    // [121]=FindMaterial(name, group, complain, prefix).
    void* FindMaterial(const char* name, const char* group, bool complain = true) {
        return VFunc<void*(__fastcall*)(void*, const char*, const char*, bool, const char*)>(
            self, 121)(self, name, group, complain, nullptr);
    }
};

struct ModelRenderInfo_t {
    Vector origin;
    QAngle angles;
    void* pRenderable;
    const void* pModel;
    const matrix3x4_t* pModelToWorld;
    const matrix3x4_t* pLightingOffset;
    const Vector* pLightingOrigin;
    int flags;
    int entity_index;
    int skin;
    int body;
    int hitboxset;
    unsigned short instance;
};

struct IModelRender {
    void* self = nullptr;
    void ForcedMaterialOverride(void* mat) {
        VFunc<void(__fastcall*)(void*, void*, int)>(self, 1)(self, mat, 0);
    }
    int DrawModelEx(ModelRenderInfo_t& info) {
        return VFunc<int(__fastcall*)(void*, ModelRenderInfo_t&)>(self, 19)(self, info);
    }
};

// IVModelInfo (VModelInfoClient006)
// IVModelInfo (VModelInfoClient006).  The MSVC vtable starts with the virtual
// destructor slot, so the SDK methods are shifted by one:
//   0 dtor  1 GetModel(int)  2 GetModelIndex(const char*)  3 GetModelName(model_t*)
//   4 GetModelBounds ...
// Verified against engine.dll 0x1803ae7c8 (VModelInfoClient006 vtable):
// slot 1 reads the int index, slot 3 takes the model pointer.
struct IVModelInfo {
    void* self = nullptr;
    const void* GetModel(int index) { return VFunc<const void*(__fastcall*)(void*, int)>(self, 1)(self, index); }
    int GetModelIndex(const char* name) { return VFunc<int(__fastcall*)(void*, const char*)>(self, 2)(self, name); }
    const char* GetModelName(const void* model) { return VFunc<const char*(__fastcall*)(void*, const void*)>(self, 3)(self, model); }
    // slot 28 = GetStudiomodel(const model_t*) (verified in engine.dll 0x1801cafc0;
    // slot 21 was GetStudioHdr(MDLHandle_t) taking a 16-bit handle, NOT model_t*)
    const void* GetStudiomodel(const void* model) { return VFunc<const void*(__fastcall*)(void*, const void*)>(self, 28)(self, model); }
    // slot 52 = RegisterDynamicModel(name, bIsClient) -> returns -(slot*2+3)
    // slot 54 = AddRefDynamicModel(index)
    // Verified in engine.dll (0x1801c9b60 / 0x1801ca670).
    int RegisterDynamicModel(const char* name, bool is_client) {
        return VFunc<int(__fastcall*)(void*, const char*, bool)>(self, 52)(self, name, is_client);
    }
    void AddRefDynamicModel(int index) {
        VFunc<void(__fastcall*)(void*, int)>(self, 54)(self, index);
    }
};

// IMDLCache (MDLCache004 from datacache.dll).
// Verified in datacache.dll (0x180034fe8): [25] = BeginLock, [26] = EndLock.
struct IMDLCache {
    void* self = nullptr;
    void BeginLock() { if (self) VFunc<void(__fastcall*)(void*)>(self, 25)(self); }
    void EndLock()   { if (self) VFunc<void(__fastcall*)(void*)>(self, 26)(self); }
};

// First two fields of IStudioRender::DrawModelInfo_t (studiorender.dll).
struct StudioDrawModelInfo_t {
    const void* pStudioHdr;      // +0x00 (numtextures at +0xE8)
    const void* pHardwareData;   // +0x08 (m_NumLODs at +0x04, m_pLODs at +0x08)
};

// IClientEntityList (VClientEntityList003)
struct IClientEntityList {
    void* self = nullptr;
    // vtable[0] = GetClientNetworkable(int) -> IClientNetworkable*
    void* GetClientNetworkable(int index) { return VFunc<void*(__fastcall*)(void*, int)>(self, 0)(self, index); }
    void* GetClientEntity(int index) { return VFunc<void*(__fastcall*)(void*, int)>(self, 3)(self, index); }
    int GetHighestEntityIndex() { return VFunc<int(__fastcall*)(void*)>(self, 6)(self); }
};

// ---------------------------------------------------------------------------
// netvar offsets.  All relative to the C_BaseEntity returned by the entity
// list.  Taken from dump-x64/classes.md for build 9540945.
// ---------------------------------------------------------------------------
namespace off {
    constexpr int m_lifeState       = 0x0cf;
    constexpr int m_iHealth         = 0x0d0;
    constexpr int m_iTeamNum        = 0x0d8;
    constexpr int m_vecViewOffset   = 0x13c;
    constexpr int m_vecOrigin       = 0x428;
    constexpr int m_fFlags          = 0x440;
    constexpr int m_vecVelocity     = 0x148;
    constexpr int m_nModelIndex     = 0x0cc;
    constexpr int m_nSequence       = 0x0ad0;
    constexpr int m_fEffects        = 0x0b8;
    constexpr int m_nRenderMode     = 0x0bc;
    constexpr int m_flSimulationTime= 0x0a8;
    constexpr int m_hActiveWeapon   = 0x11a0;
    constexpr int m_iAmmo           = 0x1060;
    constexpr int m_iAccount        = 0x1a98;
    constexpr int m_bHasHelmet      = 0x1a9c;
    constexpr int m_ArmorValue      = 0x1aa4;
    constexpr int m_bHasDefuser     = 0x1ab4;
    constexpr int m_bIsDefusing     = 0x1a10;
    constexpr int m_flFlashMaxAlpha = 0x1a54;
    constexpr int m_flFlashDuration = 0x1a58;
    constexpr int m_vecPunchAngle   = 0x127c; // CBasePlayer::m_Local + 0x0074
    constexpr int m_nTickBase       = 0x16b0; // CBasePlayer::m_nTickBase
    constexpr int w_flNextPrimaryAttack = 0x0be8; // CBaseCombatWeapon::m_flNextPrimaryAttack
    constexpr int w_iWorldModelIndex= 0x0bfc;
    constexpr int w_iPrimaryAmmoType= 0x0c28;
    constexpr int w_iClip1          = 0x0c30;
}

constexpr int FL_ONGROUND = 1;
constexpr int EF_NODRAW = 0x20;

// CUserCmd (x86-64)
struct CUserCmd {
    void* vtable;             // 0x00
    int command_number;       // 0x08
    int tick_count;           // 0x0c
    QAngle viewangles;        // 0x10
    float forwardmove;        // 0x1c
    float sidemove;           // 0x20
    float upmove;             // 0x24
    int buttons;              // 0x28
    unsigned char impulse;    // 0x2c
    int weaponselect;         // 0x30
    int weaponsubtype;        // 0x34
    int random_seed;          // 0x38
    short mousedx;            // 0x3c
    short mousedy;            // 0x3e
    bool hasbeenpredicted;    // 0x40
};

// CViewSetup, only the fields we touch (verified against ClientModeCSNormal::OverrideView)
struct CViewSetup {
    char pad0[0x38];
    float fov;          // 0x38 horizontal fov
    float fov_viewmodel;// 0x3c
    Vector origin;      // 0x40
    QAngle angles;      // 0x4c
    char pad1[0x6c - 0x58];
    float aspect;       // 0x6c
};

#define IN_JUMP (1 << 1)
#define IN_DUCK (1 << 2)
#define IN_ATTACK (1 << 0)
#define IN_FORWARD (1 << 3)
#define IN_BACK (1 << 4)
#define IN_MOVELEFT (1 << 9)
#define IN_MOVERIGHT (1 << 10)
#define IN_ATTACK2 (1 << 11)

// VClient017 (IBaseClientDLL) - vtable[18] = CreateMove(int sequence, float, bool)
constexpr int BASE_CLIENT_CREATE_MOVE = 21;  // verified: sub_1800D5690 (calls IInput::CreateMove)
constexpr int BASE_CLIENT_FRAME_STAGE = 35;  // verified: sub_1800D59B0 (switch on 0..6)
constexpr int FRAME_RENDER_START = 5;
constexpr int FRAME_RENDER_END = 6;
constexpr int INPUT_GET_USERCMD = 8;   // IInput vtable

// ClientModeCSNormal vtable RVA inside client.dll (build 9540945)
constexpr uintptr_t CLIENTMODE_VTABLE_RVA = 0x477638;
constexpr int CM_SHOULD_DRAW_LOCAL_PLAYER = 13;
constexpr int CM_OVERRIDE_VIEW = 16;
constexpr int CM_CREATE_MOVE = 21;

// client.dll globals / virtuals for build 9540945
constexpr uintptr_t CLIENT_GLOBALS_RVA      = 0x5AE280;   // CGlobalVarsBase* gpGlobals
constexpr uintptr_t CLIENT_INPUT_GLOBAL_RVA = 0x5C0090;   // IInput*
constexpr uintptr_t CLIENT_CAM_OFFSET_RVA   = 0x607730;   // Vector (pitch, yaw, distance)
constexpr int INPUT_CAM_THIRD_PERSON = 0xA1;              // bool in CInput
constexpr int INPUT_CAM_TO_THIRD = 32;                    // IInput vtable index
constexpr int INPUT_CAM_TO_FIRST = 33;
constexpr int SETUPBONES_INDEX = 226;                     // C_BaseAnimating vtable

constexpr int MATERIAL_VAR_IGNOREZ = (1 << 15);

inline float Deg2Rad(float d) { return d * 0.017453292519943295f; }

inline void AngleVectors(const QAngle& a, Vector& forward, Vector& right, Vector& up) {
    float sp = sinf(Deg2Rad(a.x)), cp = cosf(Deg2Rad(a.x));
    float sy = sinf(Deg2Rad(a.y)), cy = cosf(Deg2Rad(a.y));
    forward.x = cp * cy;
    forward.y = cp * sy;
    forward.z = -sp;
    // Source's AngleVectors (roll = 0): right = (sy, -cy, 0).
    // (-sy, cy) is the negation, which mirrors the screen horizontally.
    right.x = sy;
    right.y = -cy;
    right.z = 0.0f;
    up.x = sp * cy;
    up.y = sp * sy;
    up.z = cp;
}

inline void AngleVectors(const QAngle& a, Vector& forward) {
    Vector r, u;
    AngleVectors(a, forward, r, u);
}

inline float Dot(const Vector& a, const Vector& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
