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

#ifdef _WIN64
#define THISCALL_CC __fastcall
#define HOOK_THISCALL(ret, name, self_type, ...) ret __fastcall name(self_type self, ##__VA_ARGS__)
#define TYPEDEF_THISCALL(ret, name, self_type, ...) typedef ret (__fastcall* name)(self_type, ##__VA_ARGS__)
#else
#define THISCALL_CC __thiscall
#define HOOK_THISCALL(ret, name, self_type, ...) ret __fastcall name(self_type self, void* /*edx*/, ##__VA_ARGS__)
#define TYPEDEF_THISCALL(ret, name, self_type, ...) typedef ret (__thiscall* name)(self_type, ##__VA_ARGS__)
#endif

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

inline const char* ClientClassName(void* cc) {
    if (!cc) return nullptr;
    return *reinterpret_cast<const char**>((char*)cc + sizeof(void*) * 2);
}

struct matrix3x4_t {
    float m[3][4];
};

// IVRenderView (VEngineRenderView014 on x64 / VEngineRenderView012 on v34).
struct IVRenderView {
    void* self = nullptr;
    void SetColorModulation(const float* c) {
        VFunc<void(THISCALL_CC*)(void*, const float*)>(self, 6)(self, c);
    }
    void SetBlend(float a) {
        VFunc<void(THISCALL_CC*)(void*, float)>(self, 4)(self, a);
    }
};

// IVEngineClient (VEngineClient014 on x64 / VEngineClient012 on v34)
struct IVEngineClient {
    void* self = nullptr;
    void GetScreenSize(int& w, int& h) { VFunc<void(THISCALL_CC*)(void*, int&, int&)>(self, 5)(self, w, h); }
    bool GetPlayerInfo(int index, player_info_t* info) { return VFunc<bool(THISCALL_CC*)(void*, int, player_info_t*)>(self, 8)(self, index, info); }
    int GetLocalPlayer() { return VFunc<int(THISCALL_CC*)(void*)>(self, 12)(self); }
    void GetViewAngles(QAngle& a) { VFunc<void(THISCALL_CC*)(void*, QAngle&)>(self, 19)(self, a); }
    void GetViewAngles(float* a) { VFunc<void(THISCALL_CC*)(void*, float*)>(self, 19)(self, a); }
    void SetViewAngles(const QAngle& a) { VFunc<void(THISCALL_CC*)(void*, const QAngle&)>(self, 20)(self, a); }
    float Time() { return VFunc<float(THISCALL_CC*)(void*)>(self, 14)(self); }
    void ClientCmd(const char* cmd) { VFunc<void(THISCALL_CC*)(void*, const char*)>(self, 7)(self, cmd); }
#ifdef _WIN64
    const float* WorldToScreenMatrix() { return VFunc<const float*(THISCALL_CC*)(void*)>(self, 36)(self); }
#else
    const float* WorldToScreenMatrix() { return VFunc<const float*(THISCALL_CC*)(void*)>(self, 38)(self); }
#endif
    void ExecuteClientCmd(const char* cmd) { VFunc<void(THISCALL_CC*)(void*, const char*)>(self, 97)(self, cmd); }
};

#ifdef _WIN64
constexpr uintptr_t BASE_PLAYER_VTABLE_RVA = 0x404768;
constexpr uintptr_t CS_PLAYER_VTABLE_RVA   = 0x471098;
constexpr int PLAYER_SET_LOCAL_VIEW_ANGLES = 0x950 / 8;   // 298
constexpr int OFF_PLAYER_VIEW_ANGLES = 0x1564;            // pl.v_angle
#else
constexpr uintptr_t BASE_PLAYER_VTABLE_RVA = 0;
constexpr uintptr_t CS_PLAYER_VTABLE_RVA   = 0;
constexpr int PLAYER_SET_LOCAL_VIEW_ANGLES = 0;
constexpr int OFF_PLAYER_VIEW_ANGLES = 0x0d10;            // pl.v_angle (pl=0x0d08 + 8; +4 is deadflag)
#endif

// IVDebugOverlay (VDebugOverlay003).
struct IVDebugOverlay {
    void* self = nullptr;
    void AddBoxOverlay(const Vector& origin, const Vector& mins, const Vector& maxs,
                       const QAngle& angles, int r, int g, int b, int a, float duration) {
        VFunc<void(THISCALL_CC*)(void*, const Vector&, const Vector&, const Vector&,
                                 const QAngle&, int, int, int, int, float)>(self, 1)(
            self, origin, mins, maxs, angles, r, g, b, a, duration);
    }
    int ScreenPosition(const Vector& point, Vector& screen) {
#ifdef _WIN64
        return VFunc<int(THISCALL_CC*)(void*, const Vector&, Vector&)>(self, 10)(self, point, screen);
#else
        return VFunc<int(THISCALL_CC*)(void*, const Vector&, Vector&)>(self, 9)(self, point, screen);
#endif
    }
};

// ICvar / ConVar (VEngineCvar004 on x64 / VEngineCvar003 on v34)
struct ConVar {
#ifdef _WIN64
    char pad[0x54];
    float f_value;   // 0x54
    int i_value;     // 0x58
#else
    char pad[0x28];
    float f_value;   // 0x28
    int i_value;     // 0x2C
#endif
};

struct ICvar {
    void* self = nullptr;
    ConVar* FindVar(const char* name) {
#ifdef _WIN64
        return VFunc<ConVar*(THISCALL_CC*)(void*, const char*)>(self, 12)(self, name);
#else
        return VFunc<ConVar*(THISCALL_CC*)(void*, const char*)>(self, 7)(self, name);
#endif
    }
};

#ifdef _WIN64
struct alignas(16) Ray_t {
    Vector m_Start; float p0;
    Vector m_Delta; float p1;
    Vector m_StartOffset; float p2;
    Vector m_Extents; float p3;
    bool m_IsRay; bool m_IsSwept;
    unsigned char pad[14];
};
#else
struct Ray_t {
    Vector m_Start;
    Vector m_Delta;
    Vector m_StartOffset;
    Vector m_Extents;
    bool m_IsRay;
    bool m_IsSwept;
};
#endif

struct IEngineTrace {
    void* self = nullptr;
    int GetPointContents(const Vector& pos, void** entity = nullptr) {
        return VFunc<int(THISCALL_CC*)(void*, const Vector&, void**)>(self, 0)(self, pos, entity);
    }
    void TraceRay(const Ray_t& ray, unsigned int mask, void* filter, void* tr) {
        VFunc<void(THISCALL_CC*)(void*, const Ray_t&, unsigned int, void*, void*)>(self, 4)(self, ray, mask, filter, tr);
    }
};

// IPhysicsSurfaceProps (VPhysicsSurfaceProps001 from vphysics.dll).
struct IPhysicsSurfaceProps {
    void* self = nullptr;
    const void* GetSurfaceData(int index) {
        return VFunc<const void*(THISCALL_CC*)(void*, int)>(self, 5)(self, index);
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
        return VFunc<const char*(THISCALL_CC*)(void*)>(self, 0)(self);
    }
#ifdef _WIN64
    void AlphaModulate(float a) {
        VFunc<void(THISCALL_CC*)(void*, float)>(self, 27)(self, a);
    }
    void ColorModulate(float r, float g, float b) {
        VFunc<void(THISCALL_CC*)(void*, float, float, float)>(self, 28)(self, r, g, b);
    }
    void SetMaterialVarFlag(int flag, bool on) {
        VFunc<void(THISCALL_CC*)(void*, int, bool)>(self, 29)(self, flag, on);
    }
    bool GetMaterialVarFlag(int flag) {
        return VFunc<bool(THISCALL_CC*)(void*, int)>(self, 30)(self, flag);
    }
#else
    void AlphaModulate(float a) {
        VFunc<void(THISCALL_CC*)(void*, float)>(self, 29)(self, a);
    }
    void ColorModulate(float r, float g, float b) {
        VFunc<void(THISCALL_CC*)(void*, float, float, float)>(self, 30)(self, r, g, b);
    }
    void SetMaterialVarFlag(int flag, bool on) {
        VFunc<void(THISCALL_CC*)(void*, int, bool)>(self, 31)(self, flag, on);
    }
    bool GetMaterialVarFlag(int flag) {
        return VFunc<bool(THISCALL_CC*)(void*, int)>(self, 32)(self, flag);
    }
#endif
    void IncrementReferenceCount() {
        VFunc<void(THISCALL_CC*)(void*)>(self, 12)(self);
    }
};

struct IMaterialSystem {
    void* self = nullptr;
    void* FindMaterial(const char* name, const char* group, bool complain = true) {
#ifdef _WIN64
        return VFunc<void*(THISCALL_CC*)(void*, const char*, const char*, bool, const char*)>(
            self, 121)(self, name, group, complain, nullptr);
#else
        return VFunc<void*(THISCALL_CC*)(void*, const char*, const char*, bool, const char*)>(
            self, 27)(self, name, group, complain, nullptr);
#endif
    }
};

#ifdef _WIN64
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
#else
struct ModelRenderInfo_t {
    int flags;
    void* pRenderable;
    unsigned short instance;
    int entity_index;
    const void* pModel;
    Vector origin;
    QAngle angles;
    int skin;
    int body;
    int hitboxset;
    const matrix3x4_t* pModelToWorld;
    const matrix3x4_t* pLightingOffset;
    const Vector* pLightingOrigin;
};
#endif

struct IModelRender {
    void* self = nullptr;
    void ForcedMaterialOverride(void* mat) {
#ifdef _WIN64
        VFunc<void(THISCALL_CC*)(void*, void*, int)>(self, 1)(self, mat, 0);
#else
        VFunc<void(THISCALL_CC*)(void*, void*)>(self, 1)(self, mat);
#endif
    }
    int DrawModelEx(ModelRenderInfo_t& info) {
        return VFunc<int(THISCALL_CC*)(void*, ModelRenderInfo_t&)>(self, 19)(self, info);
    }
};

// IVModelInfo (VModelInfoClient006 on x64 / VModelInfoClient003 on v34)
struct IVModelInfo {
    void* self = nullptr;
    const void* GetModel(int index) { return VFunc<const void*(THISCALL_CC*)(void*, int)>(self, 1)(self, index); }
    int GetModelIndex(const char* name) { return VFunc<int(THISCALL_CC*)(void*, const char*)>(self, 2)(self, name); }
    const char* GetModelName(const void* model) { return VFunc<const char*(THISCALL_CC*)(void*, const void*)>(self, 3)(self, model); }
#ifdef _WIN64
    const void* GetStudiomodel(const void* model) { return VFunc<const void*(THISCALL_CC*)(void*, const void*)>(self, 28)(self, model); }
    int RegisterDynamicModel(const char* name, bool is_client) {
        return VFunc<int(THISCALL_CC*)(void*, const char*, bool)>(self, 52)(self, name, is_client);
    }
    void AddRefDynamicModel(int index) {
        VFunc<void(THISCALL_CC*)(void*, int)>(self, 54)(self, index);
    }
#else
    const void* GetStudiomodel(const void* model) { return VFunc<const void*(THISCALL_CC*)(void*, const void*)>(self, 27)(self, model); }
    int RegisterDynamicModel(const char*, bool) { return -1; }
    void AddRefDynamicModel(int) {}
#endif
};

// IMDLCache (MDLCache004 from datacache.dll).
struct IMDLCache {
    void* self = nullptr;
    void BeginLock() { if (self) VFunc<void(THISCALL_CC*)(void*)>(self, 25)(self); }
    void EndLock()   { if (self) VFunc<void(THISCALL_CC*)(void*)>(self, 26)(self); }
};

// First two fields of IStudioRender::DrawModelInfo_t (studiorender.dll).
struct StudioDrawModelInfo_t {
    const void* pStudioHdr;      // +0x00
    const void* pHardwareData;   // +0x08 (x64) / +0x04 (x86)
};

// IClientEntityList (VClientEntityList003)
struct IClientEntityList {
    void* self = nullptr;
    void* GetClientNetworkable(int index) { return VFunc<void*(THISCALL_CC*)(void*, int)>(self, 0)(self, index); }
    void* GetClientEntity(int index) { return VFunc<void*(THISCALL_CC*)(void*, int)>(self, 3)(self, index); }
    int GetHighestEntityIndex() { return VFunc<int(THISCALL_CC*)(void*)>(self, 6)(self); }
};

// ---------------------------------------------------------------------------
// netvar offsets
// ---------------------------------------------------------------------------
namespace off {
#ifdef _WIN64
    constexpr int m_lifeState       = 0x0cf;
    constexpr int m_iHealth         = 0x0d0;
    constexpr int m_iTeamNum        = 0x0d8;
    constexpr int m_vecViewOffset   = 0x13c;
    constexpr int m_vecOrigin       = 0x428;
    constexpr int m_angRotation     = 0x434;
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
    constexpr int m_iObserverMode   = 0x15dc;
    constexpr int m_hObserverTarget = 0x15e0;
    constexpr int m_flMaxspeed      = 0x1618;
    constexpr int m_hViewModel      = 0x16f0;
    constexpr int m_flNextAttack    = 0x1058;
    constexpr int m_bIsScoped       = 0x1a08;
    constexpr int m_angEyeAngles    = 0x1aa8;
    constexpr int w_flNextPrimaryAttack = 0x0be8; // CBaseCombatWeapon::m_flNextPrimaryAttack
    constexpr int w_iWorldModelIndex= 0x0bfc;
    constexpr int w_iPrimaryAmmoType= 0x0c28;
    constexpr int w_iClip1          = 0x0c30;
    constexpr int w_hOwner          = 0x0be0;
    constexpr int c4_bBombTicking   = 0x0be0;
    constexpr int c4_flC4Blow       = 0x0bec;
    constexpr int c4_flTimerLength  = 0x0bf0;
    constexpr int c4_flDefuseCountDown = 0x0bf8;
#else
    constexpr int m_lifeState       = 0x087;
    constexpr int m_iHealth         = 0x0da4;
    constexpr int m_iTeamNum        = 0x090;
    constexpr int m_vecViewOffset   = 0x0d4;
    constexpr int m_vecOrigin       = 0x29c;
    constexpr int m_angRotation     = 0x2a8;
    constexpr int m_fFlags          = 0x2b4;
    constexpr int m_vecVelocity     = 0x0e0;
    constexpr int m_nModelIndex     = 0x084;
    constexpr int m_nSequence       = 0x634;
    constexpr int m_fEffects        = 0x070;
    constexpr int m_nRenderMode     = 0x074;
    constexpr int m_flSimulationTime= 0x068;
    constexpr int m_hActiveWeapon   = 0xb38;
    constexpr int m_iAmmo           = 0x9f8;
    constexpr int m_iAccount        = 0x100c;
    constexpr int m_bHasHelmet      = 0x1010;
    constexpr int m_ArmorValue      = 0x1018;
    constexpr int m_bHasDefuser     = 0x1028;
    constexpr int m_bIsDefusing     = 0x0f94;
    constexpr int m_flFlashMaxAlpha = 0x0fd4;
    constexpr int m_flFlashDuration = 0x0fd8;
    constexpr int m_vecPunchAngle   = 0x0bb0; // m_Local (0x0b44) + 0x006c
    constexpr int m_nTickBase       = 0x0e08;
    constexpr int m_iObserverMode   = 0x0d80;
    constexpr int m_hObserverTarget = 0x0d84;
    constexpr int m_flMaxspeed      = 0x0da0;
    constexpr int m_hViewModel      = 0x0e30;
    constexpr int m_flNextAttack    = 0x09f0;
    constexpr int m_iFOV            = 0x0d1c;
    constexpr int m_angEyeAngles    = 0x101c;
    constexpr int w_flNextPrimaryAttack = 0x06b8;
    constexpr int w_iWorldModelIndex= 0x06cc;
    constexpr int w_iPrimaryAmmoType= 0x06f4;
    constexpr int w_iClip1          = 0x06fc;
    constexpr int w_hOwner          = 0x06b0;
    constexpr int w_flAccuracy      = 0x0760;
    constexpr int c4_bBombTicking   = 0x06b0;
    constexpr int c4_flC4Blow       = 0x06bc;
    constexpr int c4_flTimerLength  = 0x06c0;
    constexpr int c4_flDefuseCountDown = 0x06c8;
#endif
}

constexpr int FL_ONGROUND = 1;
constexpr int FL_DUCKING  = 2;
constexpr int EF_NODRAW = 0x20;

// CUserCmd
struct CUserCmd {
    void* vtable;             // 0x00
    int command_number;       // 0x08 / 0x04
    int tick_count;           // 0x0c / 0x08
    QAngle viewangles;        // 0x10 / 0x0c
    float forwardmove;        // 0x1c / 0x18
    float sidemove;           // 0x20 / 0x1c
    float upmove;             // 0x24 / 0x20
    int buttons;              // 0x28 / 0x24
    unsigned char impulse;    // 0x2c / 0x28
    int weaponselect;         // 0x30 / 0x2c
    int weaponsubtype;        // 0x34 / 0x30
    int random_seed;          // 0x38 / 0x34
    short mousedx;            // 0x3c / 0x38
    short mousedy;            // 0x3e / 0x3a
    bool hasbeenpredicted;    // 0x40 / 0x3c
};

// CViewSetup
#ifdef _WIN64
struct CViewSetup {
    char pad0[0x38];
    float fov;          // 0x38 horizontal fov
    float fov_viewmodel;// 0x3c
    Vector origin;      // 0x40
    QAngle angles;      // 0x4c
    char pad1[0x6c - 0x58];
    float aspect;       // 0x6c
};
#else
struct CViewSetup {
    char pad0[0x28];
    float fov;          // 0x28 horizontal fov
    float fov_viewmodel;// 0x2c
    Vector origin;      // 0x30
    QAngle angles;      // 0x3c
};
#endif

#define IN_JUMP (1 << 1)
#define IN_DUCK (1 << 2)
#define IN_ATTACK (1 << 0)
#define IN_FORWARD (1 << 3)
#define IN_BACK (1 << 4)
#define IN_MOVELEFT (1 << 9)
#define IN_MOVERIGHT (1 << 10)
#define IN_ATTACK2 (1 << 11)

#ifdef _WIN64
constexpr int BASE_CLIENT_CREATE_MOVE = 21;
constexpr int BASE_CLIENT_FRAME_STAGE = 35;
constexpr uintptr_t CLIENTMODE_VTABLE_RVA = 0x477638;
constexpr uintptr_t CLIENT_GLOBALS_RVA      = 0x5AE280;
constexpr uintptr_t CLIENT_INPUT_GLOBAL_RVA = 0x5C0090;
constexpr uintptr_t CLIENT_CAM_OFFSET_RVA   = 0x607730;
constexpr int INPUT_CAM_THIRD_PERSON = 0xA1;
constexpr int INPUT_CAM_TO_THIRD = 32;
constexpr int INPUT_CAM_TO_FIRST = 33;
constexpr int SETUPBONES_INDEX = 226;
#else
constexpr int BASE_CLIENT_CREATE_MOVE = 18;
constexpr int BASE_CLIENT_FRAME_STAGE = 32;
constexpr uintptr_t CLIENTMODE_VTABLE_RVA = 0;
constexpr uintptr_t CLIENT_GLOBALS_RVA      = 0;
constexpr uintptr_t CLIENT_INPUT_GLOBAL_RVA = 0;
constexpr uintptr_t CLIENT_CAM_OFFSET_RVA   = 0;
constexpr int INPUT_CAM_THIRD_PERSON = 0x8D;
constexpr int INPUT_CAM_TO_THIRD = 28;
constexpr int INPUT_CAM_TO_FIRST = 29;
constexpr int SETUPBONES_INDEX = 15;
#endif
constexpr int FRAME_RENDER_START = 5;
constexpr int FRAME_RENDER_END = 6;
constexpr int INPUT_GET_USERCMD = 8;   // IInput vtable

constexpr int CM_SHOULD_DRAW_LOCAL_PLAYER = 13;
constexpr int CM_OVERRIDE_VIEW = 16;
constexpr int CM_CREATE_MOVE = 21;

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
