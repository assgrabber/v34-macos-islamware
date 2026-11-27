#include "cheat.h"
#include <cmath>

// ---------------------------------------------------------------------------
// Game event listener (x64): feeds server-authoritative bullet impacts to the
// marker/tracer overlay.
//
// The engine's IGameEventListener2 is a pure virtual class with a virtual
// destructor.  MinGW's vtable puts two destructor slots where MSVC (which
// compiled client.dll/engine.dll) puts one, so the listener is built as a
// manual vtable: [0] destructor stub, [1] FireGameEvent, [2] GetEventDebugID.
// ---------------------------------------------------------------------------

namespace {

ULONGLONG g_player_shot_ms[65] = {};

HOOK_THISCALL(void, ListenerDtor, void*, unsigned int) { (void)self; }
HOOK_THISCALL(int, ListenerDebugID, void*) { (void)self; return 42; }

int PlayerIndexForUserID(int userid) {
    if (userid <= 0) return -1;

    // cache the mapping: userid is stable for a connection and bullet_impact
    // fires for every shot, so a 64-entry scan per event is wasted work
    static int s_userid[65] = {};
    static int s_index[65] = {};
    static int s_count = 0;
    player_info_t info;
    for (int c = 0; c < s_count; c++) {
        if (s_userid[c] != userid) continue;
        // the mapping is only valid for the current connection - verify it
        // against the live player list and drop it when the slot changed hands
        if (engine.GetPlayerInfo(s_index[c], &info) && info.userID == userid)
            return s_index[c];
        s_count = 0;
        break;
    }

    for (int i = 1; i <= 64; i++) {
        if (engine.GetPlayerInfo(i, &info) && info.userID == userid) {
            if (s_count < 65) {
                s_userid[s_count] = userid;
                s_index[s_count] = i;
                s_count++;
            } else {
                s_userid[0] = userid;
                s_index[0] = i;
            }
            return i;
        }
    }
    return -1;
}

// IGameEvent: [1] GetName, [6] GetInt, [7] GetFloat
HOOK_THISCALL(void, ListenerFire, void*, void* event) {
    (void)self;
    if (!event) return;

    TYPEDEF_THISCALL(const char*, GetNameFn, void*);
    TYPEDEF_THISCALL(int, GetIntFn, void*, const char*, int);
    TYPEDEF_THISCALL(float, GetFloatFn, void*, const char*, float);

    const char* name = VFunc<GetNameFn>(event, 1)(event);
    if (!name) return;

    if (strcmp(name, "weapon_fire") == 0) {
        int const userid = VFunc<GetIntFn>(event, 6)(event, "userid", 0);
        int const shooter = PlayerIndexForUserID(userid);
        if (shooter > 0 && shooter <= 64)
            g_player_shot_ms[shooter] = GetTickCount64();
        Lua_OnGameEvent(name, shooter, 0, nullptr);
        return;
    }

    if (strcmp(name, "bullet_impact") == 0) {
        int const userid = VFunc<GetIntFn>(event, 6)(event, "userid", 0);
        int const shooter = PlayerIndexForUserID(userid);
        if (shooter > 0 && shooter <= 64)
            g_player_shot_ms[shooter] = GetTickCount64();
        Vector pos(
            VFunc<GetFloatFn>(event, 7)(event, "x", 0.0f),
            VFunc<GetFloatFn>(event, 7)(event, "y", 0.0f),
            VFunc<GetFloatFn>(event, 7)(event, "z", 0.0f));
        if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) return;

        Impacts_OnServerImpact(pos, shooter);
        int const local_idx = engine.GetLocalPlayer();
        if (local_idx > 0 && shooter == local_idx)
            MissLog_OnLocalImpact(pos);
        Lua_OnGameEvent(name, shooter, 0, &pos);
        return;
    }

    if (strcmp(name, "player_death") == 0) {
        int const local = engine.GetLocalPlayer();
        int const killer = PlayerIndexForUserID(VFunc<GetIntFn>(event, 6)(event, "attacker", 0));
        int const victim = PlayerIndexForUserID(VFunc<GetIntFn>(event, 6)(event, "userid", 0));
        Lua_OnGameEvent(name, victim, killer, nullptr);
        if (!cfg.kill_effects) return;
        if (local <= 0 || killer != local || victim <= 0 || victim == local) return;

        void* ent = entity_list.GetClientEntity(victim);
        if (!ent) return;

        Vector pos = *reinterpret_cast<Vector*>((char*)ent + off::m_vecOrigin);
        pos.z += 48.0f;

        Vector dir(0.0f, 0.0f, 1.0f);
        void* local_ent = GetLocalEntity();
        if (local_ent) {
            dir = pos - PlayerEye(local_ent);
            float const len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            if (len > 1.0f) { dir.x /= len; dir.y /= len; dir.z /= len; }
        }
        KillFx_Spawn(pos, dir, victim);
        return;
    }
}

void* g_listener_vt[3] = {
    reinterpret_cast<void*>(&ListenerDtor),
    reinterpret_cast<void*>(&ListenerFire),
    reinterpret_cast<void*>(&ListenerDebugID),
};

struct ListenerObj { void** vt; };
ListenerObj g_listener = { g_listener_vt };

}

void InstallGameEvents() {
    if (!engine_module) return;

    void* mgr = GetInterface(engine_module, "GAMEEVENTSMANAGER002");
    if (!mgr) { Log("[events] GAMEEVENTSMANAGER002 missing"); return; }

    // IGameEventManager2 layout (MSVC: one dtor slot):
    //   1 LoadEventsFromFile  2 Reset  3 AddListener  4 FindListener
    // Slot 2 is Reset() - calling it wipes every event definition and the
    // engine then crashes creating events like player_connect on connect.
    TYPEDEF_THISCALL(bool, AddListenerFn, void*, void*, const char*, bool);
    TYPEDEF_THISCALL(bool, FindListenerFn, void*, void*, const char*);

    bool const ok = VFunc<AddListenerFn>(mgr, 3)(mgr, &g_listener, "bullet_impact", false);
    bool const registered = VFunc<FindListenerFn>(mgr, 4)(mgr, &g_listener, "bullet_impact");
    bool const ok_death = VFunc<AddListenerFn>(mgr, 3)(mgr, &g_listener, "player_death", false);
    bool const registered_death = VFunc<FindListenerFn>(mgr, 4)(mgr, &g_listener, "player_death");
    bool const ok_fire = VFunc<AddListenerFn>(mgr, 3)(mgr, &g_listener, "weapon_fire", false);
    Log("[events] bullet_impact listener add=%d verified=%d, player_death add=%d verified=%d, weapon_fire=%d",
        (int)ok, (int)registered, (int)ok_death, (int)registered_death, (int)ok_fire);
}

bool TargetRecentlyShot(int index, ULONGLONG window_ms) {
    if (index <= 0 || index > 64) return false;
    ULONGLONG const t = g_player_shot_ms[index];
    if (t == 0) return false;
    return (GetTickCount64() - t) <= window_ms;
}