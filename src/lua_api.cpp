// ---------------------------------------------------------------------------
// Lua 5.4 scripting API for islamware-css
//
// Features:
//   - Embedded Lua 5.4 runtime with per-script resource tracking (clean unload/reload)
//   - render.* 2D drawing primitives + render.world_to_screen (global world_to_screen / w2s)
//   - ui.window(...) standalone styled ImGui windows rendered OUTSIDE the main menu
//   - imgui.* full immediate-mode ImGui library (windows, glass blur bg, styles,
//     colors, text, widgets, graphs, progress bars, layouts, tooltips, window drawlist)
//   - ui.* extension tabs + full menu controls attachable to custom or built-in tabs
//   - menu.* complete config reflection (bool/float/int/color/string) + menu.on_change hooks
//   - cheat.* direct bindings to internal cheat & menu functions (miss_log, kill_fx,
//     trace_line, can_penetrate, apply_walls, configs, custom_models, resolver, backtrack...)
//   - hooks.add / callbacks.add for paint, render_fg, imgui/window, pre_menu, post_menu,
//     player_esp, create_move, override_view, frame_stage, game_event, config_change,
//     menu, and tab:<TabName>
// ---------------------------------------------------------------------------

#include "cheat.h"
#include "imgui.h"
#include "alice_ui.h"
#include "alice_kit.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace {

CRITICAL_SECTION g_lua_cs;
bool g_cs_init = false;
lua_State* g_L = nullptr;
bool g_lua_ready = false;

struct LuaLock {
    LuaLock() { if (g_cs_init) EnterCriticalSection(&g_lua_cs); }
    ~LuaLock() { if (g_cs_init) LeaveCriticalSection(&g_lua_cs); }
};

// Active render context (valid during Lua_OnPaint / Lua_OnPaintForeground / Lua_OnPlayerESP)
ImDrawList* g_cur_draw = nullptr;
int g_cur_w = 0;
int g_cur_h = 0;

// Script directory & tracking
char g_lua_dir[MAX_PATH] = {};
int g_loading_script = -1;

struct ScriptEntry {
    int id = 0;
    std::string name;
    std::string full_path;
    bool loaded = false;
    std::string last_error;
};
std::vector<ScriptEntry> g_scripts;
int g_next_script_id = 1;

// Console ring buffer
struct ConsoleLine {
    std::string text;
    bool is_error = false;
};
std::vector<ConsoleLine> g_console;
constexpr size_t MAX_CONSOLE_LINES = 140;

void LuaConsolePush(const char* text, bool is_error = false) {
    if (!text) return;
    if (g_console.size() >= MAX_CONSOLE_LINES)
        g_console.erase(g_console.begin());
    g_console.push_back({ text, is_error });
    Log("[lua]%s %s", is_error ? " [ERR]" : "", text);
}

// ---------------------------------------------------------------------------
// Callbacks / hooks registry
// ---------------------------------------------------------------------------
struct LuaCallback {
    int id = 0;
    int script_id = -1;
    std::string event;
    int fn_ref = LUA_NOREF;
};
std::vector<LuaCallback> g_callbacks;
int g_next_cb_id = 1;

// ---------------------------------------------------------------------------
// Standalone ImGui Windows (rendered outside the main menu)
// ---------------------------------------------------------------------------
struct LuaWindow {
    int id = 0;
    int script_id = -1;
    std::string title;
    int render_fn_ref = LUA_NOREF;

    bool open = true;
    bool only_when_menu_open = false;
    bool click_through_when_closed = true;
    bool blur = true;

    bool has_pos = false;
    bool pos_dirty = false;
    float x = 40.0f, y = 120.0f;

    bool has_size = false;
    bool size_dirty = false;
    float w = 260.0f, h = 160.0f;

    float alpha = 1.0f;
    float rounding = 6.0f;
    float border_size = 1.2f;
    bool has_padding = false;
    float pad_x = 10.0f, pad_y = 8.0f;

    bool has_bg = false;
    float bg[4] = { 0.05f, 0.05f, 0.07f, 0.90f };
    bool has_border = false;
    float border[4] = { 0.26f, 0.59f, 0.98f, 0.65f };
    bool has_text = false;
    float text_col[4] = { 0.95f, 0.95f, 0.98f, 1.0f };
    bool has_title_bg = false;
    float title_bg[4] = { 0.08f, 0.08f, 0.12f, 0.95f };

    bool no_titlebar = false;
    bool no_resize = false;
    bool no_move = false;
    bool no_background = false;
    bool no_scrollbar = false;
    bool auto_resize = false;
    bool no_inputs = false;

    // Last actual rendered rect
    float cur_x = 40.0f, cur_y = 120.0f, cur_w = 260.0f, cur_h = 160.0f;
};
std::vector<LuaWindow> g_windows;
int g_next_win_id = 1;
static const char* WIN_MT = "IslamwareWin";
struct WinHandle { int id; };

LuaWindow* FindWindow(int id) {
    for (auto& w : g_windows)
        if (w.id == id) return &w;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Extension tabs & menu controls
// ---------------------------------------------------------------------------
struct LuaExtensionTab {
    int script_id = -1;
    std::string name;
    int render_fn_ref = LUA_NOREF;
};
std::vector<LuaExtensionTab> g_ext_tabs;

enum class CtrlType {
    Checkbox, SliderFloat, SliderInt, Combo, Color, InputText, Keybind, Button, Label, Separator,
};

struct LuaControl {
    int id = 0;
    int script_id = -1;
    std::string tab;
    std::string label;
    CtrlType type = CtrlType::Checkbox;
    bool visible = true;

    bool bval = false;
    float fval = 0.0f;
    float fmin = 0.0f;
    float fmax = 1.0f;
    std::string fmt = "%.2f";
    int ival = 0;
    int imin = 0;
    int imax = 100;
    std::vector<std::string> items;
    float col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    char text[256] = {};
    int vk = 0;
    bool binding = false;

    int fn_ref = LUA_NOREF;
};
std::vector<LuaControl> g_controls;
int g_next_ctrl_id = 1;
static const char* CTRL_MT = "IslamwareCtrl";
struct CtrlHandle { int id; };

LuaControl* FindControl(int id) {
    for (auto& c : g_controls)
        if (c.id == id) return &c;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Menu Config Reflection & Live Change Watchers
// ---------------------------------------------------------------------------
enum class CfgFieldType { Bool, Float, Int, Color4, Str };
struct CfgField {
    const char* name;
    CfgFieldType type;
    void* ptr;
    size_t str_cap;
};

static const CfgField kCfgFields[] = {
    { "menu_open",           CfgFieldType::Bool,   &cfg.menu_open, 0 },
    { "esp",                 CfgFieldType::Bool,   &cfg.esp, 0 },
    { "esp_box",             CfgFieldType::Bool,   &cfg.esp_box, 0 },
    { "esp_box_color",       CfgFieldType::Color4, cfg.esp_box_color, 0 },
    { "esp_health",          CfgFieldType::Bool,   &cfg.esp_health, 0 },
    { "esp_name",            CfgFieldType::Bool,   &cfg.esp_name, 0 },
    { "esp_name_color",      CfgFieldType::Color4, cfg.esp_name_color, 0 },
    { "esp_distance",        CfgFieldType::Bool,   &cfg.esp_distance, 0 },
    { "esp_team_check",      CfgFieldType::Bool,   &cfg.esp_team_check, 0 },
    { "esp_skeleton",        CfgFieldType::Bool,   &cfg.esp_skeleton, 0 },
    { "esp_weapon",          CfgFieldType::Bool,   &cfg.esp_weapon, 0 },
    { "esp_ammo",            CfgFieldType::Bool,   &cfg.esp_ammo, 0 },
    { "esp_armor",           CfgFieldType::Bool,   &cfg.esp_armor, 0 },
    { "esp_flags",           CfgFieldType::Bool,   &cfg.esp_flags, 0 },
    { "esp_bomb",            CfgFieldType::Bool,   &cfg.esp_bomb, 0 },
    { "esp_offscreen",       CfgFieldType::Bool,   &cfg.esp_offscreen, 0 },
    { "esp_offscreen_color", CfgFieldType::Color4, cfg.esp_offscreen_color, 0 },
    { "dropped_weapons",     CfgFieldType::Bool,   &cfg.dropped_weapons, 0 },
    { "aimbot",              CfgFieldType::Bool,   &cfg.aimbot, 0 },
    { "aim_fov",             CfgFieldType::Float,  &cfg.aim_fov, 0 },
    { "aim_smooth",          CfgFieldType::Float,  &cfg.aim_smooth, 0 },
    { "silentaim",           CfgFieldType::Bool,   &cfg.silentaim, 0 },
    { "silent_fov",          CfgFieldType::Float,  &cfg.silent_fov, 0 },
    { "silent_visible",      CfgFieldType::Bool,   &cfg.silent_visible, 0 },
    { "triggerbot",          CfgFieldType::Bool,   &cfg.triggerbot, 0 },
    { "auto_wall",           CfgFieldType::Bool,   &cfg.auto_wall, 0 },
    { "min_damage",          CfgFieldType::Float,  &cfg.min_damage, 0 },
    { "hit_chance",          CfgFieldType::Float,  &cfg.hit_chance, 0 },
    { "auto_stop",           CfgFieldType::Bool,   &cfg.auto_stop, 0 },
    { "auto_scope",          CfgFieldType::Bool,   &cfg.auto_scope, 0 },
    { "backtrack",           CfgFieldType::Bool,   &cfg.backtrack, 0 },
    { "backtrack_time",      CfgFieldType::Float,  &cfg.backtrack_time, 0 },
    { "backtrack_skeleton",  CfgFieldType::Bool,   &cfg.backtrack_skeleton, 0 },
    { "resolver",            CfgFieldType::Bool,   &cfg.resolver, 0 },
    { "nospread",            CfgFieldType::Bool,   &cfg.nospread, 0 },
    { "double_tap",          CfgFieldType::Bool,   &cfg.double_tap, 0 },
    { "baim_force",          CfgFieldType::Bool,   &cfg.baim_force, 0 },
    { "aa_enabled",          CfgFieldType::Bool,   &cfg.aa_enabled, 0 },
    { "fakelag",             CfgFieldType::Bool,   &cfg.fakelag, 0 },
    { "fakelag_ticks",       CfgFieldType::Int,    &cfg.fakelag_ticks, 0 },
    { "bhop",                CfgFieldType::Bool,   &cfg.bhop, 0 },
    { "autostrafe",          CfgFieldType::Bool,   &cfg.autostrafe, 0 },
    { "inf_duck",            CfgFieldType::Bool,   &cfg.inf_duck, 0 },
    { "fake_duck",           CfgFieldType::Bool,   &cfg.fake_duck, 0 },
    { "quick_stop",          CfgFieldType::Bool,   &cfg.quick_stop, 0 },
    { "peek_assist",         CfgFieldType::Bool,   &cfg.peek_assist, 0 },
    { "peek_assist_color",   CfgFieldType::Color4, cfg.peek_assist_color, 0 },
    { "thirdperson",         CfgFieldType::Bool,   &cfg.thirdperson, 0 },
    { "thirdperson_dist",    CfgFieldType::Float,  &cfg.thirdperson_dist, 0 },
    { "fov_override",        CfgFieldType::Float,  &cfg.fov_override, 0 },
    { "viewmodel_fov",       CfgFieldType::Float,  &cfg.viewmodel_fov, 0 },
    { "left_hand",           CfgFieldType::Bool,   &cfg.left_hand, 0 },
    { "remove_bob",          CfgFieldType::Bool,   &cfg.remove_bob, 0 },
    { "super_toss",          CfgFieldType::Bool,   &cfg.super_toss, 0 },
    { "world_fullbright",    CfgFieldType::Bool,   &cfg.world_fullbright, 0 },
    { "world_nofog",         CfgFieldType::Bool,   &cfg.world_nofog, 0 },
    { "world_nosky",         CfgFieldType::Bool,   &cfg.world_nosky, 0 },
    { "world_walls",         CfgFieldType::Bool,   &cfg.world_walls, 0 },
    { "wall_color",          CfgFieldType::Color4, cfg.wall_color, 0 },
    { "world_props",         CfgFieldType::Bool,   &cfg.world_props, 0 },
    { "prop_color",          CfgFieldType::Color4, cfg.prop_color, 0 },
    { "watermark",           CfgFieldType::Bool,   &cfg.watermark, 0 },
    { "hotkey_list",         CfgFieldType::Bool,   &cfg.hotkey_list, 0 },
    { "draw_aim_fov",        CfgFieldType::Bool,   &cfg.draw_aim_fov, 0 },
    { "tracers",             CfgFieldType::Bool,   &cfg.tracers, 0 },
    { "bullet_impacts",      CfgFieldType::Bool,   &cfg.bullet_impacts, 0 },
    { "kill_effects",        CfgFieldType::Bool,   &cfg.kill_effects, 0 },
    { "rem_scope",           CfgFieldType::Bool,   &cfg.rem_scope, 0 },
    { "custom_scope",        CfgFieldType::Bool,   &cfg.custom_scope, 0 },
    { "custom_scope_color",  CfgFieldType::Color4, cfg.custom_scope_color, 0 },
    { "chams_enemy",         CfgFieldType::Bool,   &cfg.chams_enemy.visible.enabled, 0 },
    { "chams_team",          CfgFieldType::Bool,   &cfg.chams_team.visible.enabled, 0 },
    { "chams_local",         CfgFieldType::Bool,   &cfg.chams_local.visible.enabled, 0 },
    { "chams_weapons",       CfgFieldType::Bool,   &cfg.chams_weapons.visible.enabled, 0 },
    { "whitelist",           CfgFieldType::Str,    cfg.whitelist, sizeof(cfg.whitelist) },
    { "model_path",          CfgFieldType::Str,    cfg.model_path, sizeof(cfg.model_path) },
};

const CfgField* FindCfgField(const char* name) {
    if (!name) return nullptr;
    for (const auto& f : kCfgFields)
        if (strcmp(f.name, name) == 0) return &f;
    return nullptr;
}

struct CfgWatch {
    int id = 0;
    int script_id = -1;
    const CfgField* field = nullptr;
    int fn_ref = LUA_NOREF;
    bool last_b = false;
    float last_f = 0.0f;
    int last_i = 0;
    float last_c[4] = {};
    std::string last_s;
};
std::vector<CfgWatch> g_cfg_watches;
int g_next_watch_id = 1;

bool IsBuiltInTab(const std::string& name) {
    static const char* kBuiltIn[] = {
        "Ragebot", "Anti-aim", "Players", "Models", "World",
        "View", "Movement", "Misc", "Configs", "Lua", "Settings"
    };
    for (const char* t : kBuiltIn)
        if (name == t) return true;
    return false;
}

void EnsureExtensionTab(const std::string& name, int script_id, int fn_ref = LUA_NOREF) {
    if (IsBuiltInTab(name)) {
        if (fn_ref != LUA_NOREF) {
            LuaCallback cb;
            cb.id = g_next_cb_id++;
            cb.script_id = script_id;
            cb.event = "tab:" + name;
            cb.fn_ref = fn_ref;
            g_callbacks.push_back(std::move(cb));
        }
        return;
    }
    for (auto& t : g_ext_tabs) {
        if (t.name == name) {
            if (fn_ref != LUA_NOREF) {
                if (t.render_fn_ref != LUA_NOREF && g_L)
                    luaL_unref(g_L, LUA_REGISTRYINDEX, t.render_fn_ref);
                t.render_fn_ref = fn_ref;
            }
            return;
        }
    }
    LuaExtensionTab t;
    t.script_id = script_id;
    t.name = name;
    t.render_fn_ref = fn_ref;
    g_ext_tabs.push_back(std::move(t));
}

void CleanupScriptResources(int script_id) {
    if (!g_L) return;
    for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ) {
        if (it->script_id == script_id) {
            if (it->fn_ref != LUA_NOREF) luaL_unref(g_L, LUA_REGISTRYINDEX, it->fn_ref);
            it = g_callbacks.erase(it);
        } else ++it;
    }
    for (auto it = g_controls.begin(); it != g_controls.end(); ) {
        if (it->script_id == script_id) {
            if (it->fn_ref != LUA_NOREF) luaL_unref(g_L, LUA_REGISTRYINDEX, it->fn_ref);
            it = g_controls.erase(it);
        } else ++it;
    }
    for (auto it = g_ext_tabs.begin(); it != g_ext_tabs.end(); ) {
        if (it->script_id == script_id) {
            if (it->render_fn_ref != LUA_NOREF) luaL_unref(g_L, LUA_REGISTRYINDEX, it->render_fn_ref);
            it = g_ext_tabs.erase(it);
        } else ++it;
    }
    for (auto it = g_windows.begin(); it != g_windows.end(); ) {
        if (it->script_id == script_id) {
            if (it->render_fn_ref != LUA_NOREF) luaL_unref(g_L, LUA_REGISTRYINDEX, it->render_fn_ref);
            it = g_windows.erase(it);
        } else ++it;
    }
    for (auto it = g_cfg_watches.begin(); it != g_cfg_watches.end(); ) {
        if (it->script_id == script_id) {
            if (it->fn_ref != LUA_NOREF) luaL_unref(g_L, LUA_REGISTRYINDEX, it->fn_ref);
            it = g_cfg_watches.erase(it);
        } else ++it;
    }
}

void SafePCall(lua_State* L, int nargs, int nresults, const char* ctx) {
    if (lua_pcall(L, nargs, nresults, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        char buf[512];
        snprintf(buf, sizeof(buf), "%s: %s", ctx ? ctx : "error", err ? err : "(unknown)");
        LuaConsolePush(buf, true);
        lua_pop(L, 1);
    }
}

static int Clamp255(lua_State* L, int idx, int def = 255) {
    if (lua_isnoneornil(L, idx)) return def;
    int v = (int)lua_tonumber(L, idx);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return v;
}

static ImU32 ReadColor(lua_State* L, int r_idx) {
    return IM_COL32(
        Clamp255(L, r_idx, 255),
        Clamp255(L, r_idx + 1, 255),
        Clamp255(L, r_idx + 2, 255),
        Clamp255(L, r_idx + 3, 255));
}

static bool ReadColorTable(lua_State* L, int tbl_idx, float out[4]) {
    if (!lua_istable(L, tbl_idx)) return false;
    for (int i = 0; i < 4; i++) {
        lua_rawgeti(L, tbl_idx, i + 1);
        out[i] = lua_isnumber(L, -1) ? ((float)lua_tonumber(L, -1) / 255.0f) : (i == 3 ? 1.0f : 1.0f);
        if (out[i] < 0.0f) out[i] = 0.0f;
        if (out[i] > 1.0f) out[i] = 1.0f;
        lua_pop(L, 1);
    }
    return true;
}

static void PushCfgValue(lua_State* L, const CfgField* f) {
    switch (f->type) {
    case CfgFieldType::Bool:   lua_pushboolean(L, *(bool*)f->ptr); break;
    case CfgFieldType::Float:  lua_pushnumber(L, *(float*)f->ptr); break;
    case CfgFieldType::Int:    lua_pushinteger(L, *(int*)f->ptr); break;
    case CfgFieldType::Str:    lua_pushstring(L, (const char*)f->ptr); break;
    case CfgFieldType::Color4: {
        const float* c = (const float*)f->ptr;
        lua_createtable(L, 4, 0);
        for (int i = 0; i < 4; i++) {
            lua_pushinteger(L, (int)roundf(c[i] * 255.0f));
            lua_rawseti(L, -2, i + 1);
        }
        break;
    }
    }
}

void PushVec3(lua_State* L, const Vector& v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
}

// ---------------------------------------------------------------------------
// Global print() override
// ---------------------------------------------------------------------------
int l_print(lua_State* L) {
    int n = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) out += "\t";
        if (s) out.append(s, len);
        lua_pop(L, 1);
    }
    LuaConsolePush(out.c_str(), false);
    return 0;
}

// ---------------------------------------------------------------------------
// render.* API + WorldToScreen
// ---------------------------------------------------------------------------
int l_world_to_screen(lua_State* L) {
    if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2) || !lua_isnumber(L, 3))
        return 0;
    Vector world((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3));
    int w = g_cur_w, h = g_cur_h;
    if (w <= 0 || h <= 0) {
        w = (int)ImGui::GetIO().DisplaySize.x;
        h = (int)ImGui::GetIO().DisplaySize.y;
        if ((w <= 0 || h <= 0) && engine.self) engine.GetScreenSize(w, h);
    }
    if (w <= 0 || h <= 0) return 0;
    ImVec2 sp;
    if (!WorldToScreen(world, sp, w, h)) return 0;
    lua_pushnumber(L, sp.x);
    lua_pushnumber(L, sp.y);
    return 2;
}

int l_render_screen_size(lua_State* L) {
    int w = g_cur_w, h = g_cur_h;
    if (w <= 0 || h <= 0) {
        w = (int)ImGui::GetIO().DisplaySize.x;
        h = (int)ImGui::GetIO().DisplaySize.y;
        if ((w <= 0 || h <= 0) && engine.self) engine.GetScreenSize(w, h);
    }
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

int l_render_text(lua_State* L) {
    if (!g_cur_draw || !lua_isnumber(L, 1) || !lua_isnumber(L, 2) || !lua_isstring(L, 3)) return 0;
    g_cur_draw->AddText(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)), ReadColor(L, 4), lua_tostring(L, 3));
    return 0;
}

int l_render_text_shadow(lua_State* L) {
    if (!g_cur_draw || !lua_isnumber(L, 1) || !lua_isnumber(L, 2) || !lua_isstring(L, 3)) return 0;
    float x = (float)lua_tonumber(L, 1), y = (float)lua_tonumber(L, 2);
    const char* txt = lua_tostring(L, 3);
    int a = Clamp255(L, 7, 255);
    g_cur_draw->AddText(ImVec2(x + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, a), txt);
    g_cur_draw->AddText(ImVec2(x, y), ReadColor(L, 4), txt);
    return 0;
}

int l_render_measure_text(lua_State* L) {
    ImVec2 sz = ImGui::CalcTextSize(lua_isstring(L, 1) ? lua_tostring(L, 1) : "");
    lua_pushnumber(L, sz.x);
    lua_pushnumber(L, sz.y);
    return 2;
}

int l_render_line(lua_State* L) {
    if (!g_cur_draw) return 0;
    float thick = lua_isnumber(L, 9) ? (float)lua_tonumber(L, 9) : 1.0f;
    g_cur_draw->AddLine(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)),
                        ImVec2((float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4)),
                        ReadColor(L, 5), thick);
    return 0;
}

int l_render_rect(lua_State* L) {
    if (!g_cur_draw) return 0;
    float x = (float)lua_tonumber(L, 1), y = (float)lua_tonumber(L, 2);
    float w = (float)lua_tonumber(L, 3), h = (float)lua_tonumber(L, 4);
    float rnd = lua_isnumber(L, 9) ? (float)lua_tonumber(L, 9) : 0.0f;
    float thick = lua_isnumber(L, 10) ? (float)lua_tonumber(L, 10) : 1.0f;
    g_cur_draw->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ReadColor(L, 5), rnd, 0, thick);
    return 0;
}

int l_render_rect_filled(lua_State* L) {
    if (!g_cur_draw) return 0;
    float x = (float)lua_tonumber(L, 1), y = (float)lua_tonumber(L, 2);
    float w = (float)lua_tonumber(L, 3), h = (float)lua_tonumber(L, 4);
    float rnd = lua_isnumber(L, 9) ? (float)lua_tonumber(L, 9) : 0.0f;
    g_cur_draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ReadColor(L, 5), rnd);
    return 0;
}

int l_render_rect_gradient(lua_State* L) {
    if (!g_cur_draw) return 0;
    float x = (float)lua_tonumber(L, 1), y = (float)lua_tonumber(L, 2);
    float w = (float)lua_tonumber(L, 3), h = (float)lua_tonumber(L, 4);
    ImU32 c1 = ReadColor(L, 5), c2 = ReadColor(L, 9);
    bool horiz = lua_toboolean(L, 13) != 0;
    if (horiz) g_cur_draw->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + h), c1, c2, c2, c1);
    else       g_cur_draw->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + h), c1, c1, c2, c2);
    return 0;
}

int l_render_circle(lua_State* L) {
    if (!g_cur_draw) return 0;
    int seg = lua_isnumber(L, 8) ? (int)lua_tointeger(L, 8) : 24;
    float thick = lua_isnumber(L, 9) ? (float)lua_tonumber(L, 9) : 1.0f;
    g_cur_draw->AddCircle(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)),
                          (float)lua_tonumber(L, 3), ReadColor(L, 4), seg, thick);
    return 0;
}

int l_render_circle_filled(lua_State* L) {
    if (!g_cur_draw) return 0;
    int seg = lua_isnumber(L, 8) ? (int)lua_tointeger(L, 8) : 24;
    g_cur_draw->AddCircleFilled(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)),
                                (float)lua_tonumber(L, 3), ReadColor(L, 4), seg);
    return 0;
}

int l_render_triangle_filled(lua_State* L) {
    if (!g_cur_draw) return 0;
    g_cur_draw->AddTriangleFilled(
        ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)),
        ImVec2((float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4)),
        ImVec2((float)lua_tonumber(L, 5), (float)lua_tonumber(L, 6)),
        ReadColor(L, 7));
    return 0;
}

// ---------------------------------------------------------------------------
// imgui.* full immediate-mode library (works outside & inside the menu!)
// ---------------------------------------------------------------------------
static ImGuiCol LookupImGuiCol(lua_State* L, int idx) {
    if (lua_isnumber(L, idx)) {
        int id = (int)lua_tointeger(L, idx);
        return (id >= 0 && id < ImGuiCol_COUNT) ? (ImGuiCol)id : ImGuiCol_Text;
    }
    const char* s = lua_isstring(L, idx) ? lua_tostring(L, idx) : "";
    struct Entry { const char* name; ImGuiCol col; };
    static const Entry kCols[] = {
        { "Text",              ImGuiCol_Text },
        { "TextDisabled",      ImGuiCol_TextDisabled },
        { "WindowBg",          ImGuiCol_WindowBg },
        { "ChildBg",           ImGuiCol_ChildBg },
        { "PopupBg",           ImGuiCol_PopupBg },
        { "Border",            ImGuiCol_Border },
        { "FrameBg",           ImGuiCol_FrameBg },
        { "FrameBgHovered",    ImGuiCol_FrameBgHovered },
        { "FrameBgActive",     ImGuiCol_FrameBgActive },
        { "TitleBg",           ImGuiCol_TitleBg },
        { "TitleBgActive",     ImGuiCol_TitleBgActive },
        { "TitleBgCollapsed",  ImGuiCol_TitleBgCollapsed },
        { "CheckMark",         ImGuiCol_CheckMark },
        { "SliderGrab",        ImGuiCol_SliderGrab },
        { "SliderGrabActive",  ImGuiCol_SliderGrabActive },
        { "Button",            ImGuiCol_Button },
        { "ButtonHovered",     ImGuiCol_ButtonHovered },
        { "ButtonActive",      ImGuiCol_ButtonActive },
        { "Header",            ImGuiCol_Header },
        { "HeaderHovered",     ImGuiCol_HeaderHovered },
        { "HeaderActive",      ImGuiCol_HeaderActive },
        { "Separator",         ImGuiCol_Separator },
        { "Tab",               ImGuiCol_Tab },
        { "TabHovered",        ImGuiCol_TabHovered },
        { "TabActive",         ImGuiCol_TabActive },
        { "PlotLines",         ImGuiCol_PlotLines },
        { "PlotHistogram",     ImGuiCol_PlotHistogram },
    };
    for (const auto& e : kCols)
        if (_stricmp(e.name, s) == 0) return e.col;
    return ImGuiCol_Text;
}

static ImGuiStyleVar LookupImGuiVar(lua_State* L, int idx, bool& is_vec2) {
    is_vec2 = false;
    if (lua_isnumber(L, idx)) {
        int id = (int)lua_tointeger(L, idx);
        is_vec2 = (id == ImGuiStyleVar_WindowPadding || id == ImGuiStyleVar_WindowMinSize ||
                   id == ImGuiStyleVar_FramePadding || id == ImGuiStyleVar_ItemSpacing ||
                   id == ImGuiStyleVar_ItemInnerSpacing || id == ImGuiStyleVar_CellPadding ||
                   id == ImGuiStyleVar_ButtonTextAlign || id == ImGuiStyleVar_SelectableTextAlign);
        return (id >= 0 && id < ImGuiStyleVar_COUNT) ? (ImGuiStyleVar)id : ImGuiStyleVar_WindowRounding;
    }
    const char* s = lua_isstring(L, idx) ? lua_tostring(L, idx) : "";
    struct Entry { const char* name; ImGuiStyleVar var; bool vec2; };
    static const Entry kVars[] = {
        { "Alpha",            ImGuiStyleVar_Alpha,            false },
        { "WindowRounding",   ImGuiStyleVar_WindowRounding,   false },
        { "WindowBorderSize", ImGuiStyleVar_WindowBorderSize, false },
        { "WindowPadding",    ImGuiStyleVar_WindowPadding,    true  },
        { "ChildRounding",    ImGuiStyleVar_ChildRounding,    false },
        { "ChildBorderSize",  ImGuiStyleVar_ChildBorderSize,  false },
        { "PopupRounding",    ImGuiStyleVar_PopupRounding,    false },
        { "FrameRounding",    ImGuiStyleVar_FrameRounding,    false },
        { "FrameBorderSize",  ImGuiStyleVar_FrameBorderSize,  false },
        { "FramePadding",     ImGuiStyleVar_FramePadding,     true  },
        { "ItemSpacing",      ImGuiStyleVar_ItemSpacing,      true  },
        { "ItemInnerSpacing", ImGuiStyleVar_ItemInnerSpacing, true  },
        { "IndentSpacing",    ImGuiStyleVar_IndentSpacing,    false },
        { "GrabRounding",     ImGuiStyleVar_GrabRounding,     false },
        { "TabRounding",      ImGuiStyleVar_TabRounding,      false },
    };
    for (const auto& e : kVars) {
        if (_stricmp(e.name, s) == 0) {
            is_vec2 = e.vec2;
            return e.var;
        }
    }
    return ImGuiStyleVar_WindowRounding;
}

int l_im_begin_window(lua_State* L) {
    const char* title = lua_isstring(L, 1) ? lua_tostring(L, 1) : "Lua Window";
    ImGuiWindowFlags flags = lua_isnumber(L, 2) ? (ImGuiWindowFlags)lua_tointeger(L, 2) : 0;
    if (!cfg.menu_open && lua_isnoneornil(L, 2))
        flags |= ImGuiWindowFlags_NoInputs;
    bool open = ImGui::Begin(title, nullptr, flags);
    lua_pushboolean(L, open);
    return 1;
}

int l_im_end_window(lua_State*) { ImGui::End(); return 0; }

int l_im_set_next_pos(lua_State* L) {
    ImGuiCond cond = lua_isnumber(L, 3) ? (ImGuiCond)lua_tointeger(L, 3) : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)), cond);
    return 0;
}

int l_im_set_next_size(lua_State* L) {
    ImGuiCond cond = lua_isnumber(L, 3) ? (ImGuiCond)lua_tointeger(L, 3) : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowSize(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)), cond);
    return 0;
}

int l_im_set_next_bg_alpha(lua_State* L) {
    ImGui::SetNextWindowBgAlpha((float)lua_tonumber(L, 1));
    return 0;
}

int l_im_apply_glass_bg(lua_State* L) {
    float rounding = lua_isnumber(L, 1) ? (float)lua_tonumber(L, 1) : ImGui::GetStyle().WindowRounding;
    int alpha = Clamp255(L, 2, 232);
    ImVec2 wmin = ImGui::GetWindowPos();
    ImVec2 wsz = ImGui::GetWindowSize();
    ImVec2 wmax(wmin.x + wsz.x, wmin.y + wsz.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (alice::core && alice::core->blur)
        alice::core->blur->apply(dl, wmin, wmax, 14.0f, rounding, 2, 1.0f);
    dl->AddRectFilled(wmin, wmax, IM_COL32(13, 13, 18, alpha), rounding);
    dl->AddRect(wmin, wmax, ImGui::GetColorU32(ImGuiCol_Border), rounding, 0, 1.2f);
    return 0;
}

int l_im_push_style_color(lua_State* L) {
    ImGuiCol col = LookupImGuiCol(L, 1);
    ImVec4 v(Clamp255(L, 2, 255) / 255.0f, Clamp255(L, 3, 255) / 255.0f,
             Clamp255(L, 4, 255) / 255.0f, Clamp255(L, 5, 255) / 255.0f);
    ImGui::PushStyleColor(col, v);
    return 0;
}

int l_im_pop_style_color(lua_State* L) {
    ImGui::PopStyleColor(lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : 1);
    return 0;
}

int l_im_push_style_var(lua_State* L) {
    bool is_vec2 = false;
    ImGuiStyleVar var = LookupImGuiVar(L, 1, is_vec2);
    if (is_vec2)
        ImGui::PushStyleVar(var, ImVec2((float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3)));
    else
        ImGui::PushStyleVar(var, (float)lua_tonumber(L, 2));
    return 0;
}

int l_im_pop_style_var(lua_State* L) {
    ImGui::PopStyleVar(lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : 1);
    return 0;
}

int l_im_set_font_scale(lua_State* L) {
    float s = (float)lua_tonumber(L, 1);
    if (s < 0.3f) s = 0.3f;
    if (s > 4.0f) s = 4.0f;
    ImGui::SetWindowFontScale(s);
    return 0;
}

int l_im_text(lua_State* L) {
    ImGui::TextUnformatted(lua_isstring(L, 1) ? lua_tostring(L, 1) : "");
    return 0;
}

int l_im_text_colored(lua_State* L) {
    ImVec4 col(Clamp255(L, 1, 255) / 255.0f, Clamp255(L, 2, 255) / 255.0f,
               Clamp255(L, 3, 255) / 255.0f, Clamp255(L, 4, 255) / 255.0f);
    ImGui::TextColored(col, "%s", lua_isstring(L, 5) ? lua_tostring(L, 5) : "");
    return 0;
}

int l_im_text_disabled(lua_State* L) {
    ImGui::TextDisabled("%s", lua_isstring(L, 1) ? lua_tostring(L, 1) : "");
    return 0;
}

int l_im_text_wrapped(lua_State* L) {
    ImGui::TextWrapped("%s", lua_isstring(L, 1) ? lua_tostring(L, 1) : "");
    return 0;
}

int l_im_bullet_text(lua_State* L) {
    ImGui::BulletText("%s", lua_isstring(L, 1) ? lua_tostring(L, 1) : "");
    return 0;
}

int l_im_text_centered(lua_State* L) {
    const char* txt = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
    float avail = ImGui::GetContentRegionAvail().x;
    float tw = ImGui::CalcTextSize(txt).x;
    if (tw < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - tw) * 0.5f);
    if (lua_isnumber(L, 2)) {
        ImVec4 col(Clamp255(L, 2, 255) / 255.0f, Clamp255(L, 3, 255) / 255.0f,
                   Clamp255(L, 4, 255) / 255.0f, Clamp255(L, 5, 255) / 255.0f);
        ImGui::TextColored(col, "%s", txt);
    } else {
        ImGui::TextUnformatted(txt);
    }
    return 0;
}

int l_im_small_button(lua_State* L) {
    lua_pushboolean(L, ImGui::SmallButton(lua_isstring(L, 1) ? lua_tostring(L, 1) : "Btn"));
    return 1;
}

int l_im_combo(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_istable(L, 3)) return 0;
    const char* label = lua_tostring(L, 1);
    int cur = (int)lua_tointeger(L, 2) - 1;
    std::vector<std::string> items;
    lua_Integer n = luaL_len(L, 3);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 3, i);
        if (lua_isstring(L, -1)) items.push_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    if (cur < 0) cur = 0;
    if (!items.empty() && cur >= (int)items.size()) cur = (int)items.size() - 1;
    bool changed = false;
    const char* prev = items.empty() ? "" : items[cur].c_str();
    if (ImGui::BeginCombo(label, prev)) {
        for (int i = 0; i < (int)items.size(); i++) {
            bool sel = (i == cur);
            if (ImGui::Selectable(items[i].c_str(), sel)) { cur = i; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    lua_pushinteger(L, cur + 1);
    lua_pushboolean(L, changed);
    return 2;
}

int l_im_color_edit(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    float c[4] = {
        Clamp255(L, 2, 255) / 255.0f, Clamp255(L, 3, 255) / 255.0f,
        Clamp255(L, 4, 255) / 255.0f, Clamp255(L, 5, 255) / 255.0f
    };
    bool changed = ImGui::ColorEdit4(lua_tostring(L, 1), c,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    lua_pushinteger(L, (int)roundf(c[0] * 255.0f));
    lua_pushinteger(L, (int)roundf(c[1] * 255.0f));
    lua_pushinteger(L, (int)roundf(c[2] * 255.0f));
    lua_pushinteger(L, (int)roundf(c[3] * 255.0f));
    lua_pushboolean(L, changed);
    return 5;
}

int l_im_input_text(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    char buf[256] = {};
    if (lua_isstring(L, 2)) strncpy(buf, lua_tostring(L, 2), sizeof(buf) - 1);
    bool changed = ImGui::InputText(lua_tostring(L, 1), buf, sizeof(buf));
    lua_pushstring(L, buf);
    lua_pushboolean(L, changed);
    return 2;
}

int l_im_progress_bar(lua_State* L) {
    float frac = (float)lua_tonumber(L, 1);
    float w = lua_isnumber(L, 2) ? (float)lua_tonumber(L, 2) : -FLT_MIN;
    float h = lua_isnumber(L, 3) ? (float)lua_tonumber(L, 3) : 0.0f;
    const char* ov = lua_isstring(L, 4) ? lua_tostring(L, 4) : nullptr;
    ImGui::ProgressBar(frac, ImVec2(w, h), ov);
    return 0;
}

int l_im_plot_lines(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_istable(L, 2)) return 0;
    std::vector<float> vals;
    lua_Integer n = luaL_len(L, 2);
    vals.reserve((size_t)n);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        vals.push_back((float)lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    if (vals.empty()) return 0;
    const char* ov = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr;
    float mn = lua_isnumber(L, 4) ? (float)lua_tonumber(L, 4) : FLT_MAX;
    float mx = lua_isnumber(L, 5) ? (float)lua_tonumber(L, 5) : FLT_MAX;
    float w = lua_isnumber(L, 6) ? (float)lua_tonumber(L, 6) : 0.0f;
    float h = lua_isnumber(L, 7) ? (float)lua_tonumber(L, 7) : 50.0f;
    ImGui::PlotLines(lua_tostring(L, 1), vals.data(), (int)vals.size(), 0, ov, mn, mx, ImVec2(w, h));
    return 0;
}

int l_im_plot_histogram(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_istable(L, 2)) return 0;
    std::vector<float> vals;
    lua_Integer n = luaL_len(L, 2);
    vals.reserve((size_t)n);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        vals.push_back((float)lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    if (vals.empty()) return 0;
    const char* ov = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr;
    float mn = lua_isnumber(L, 4) ? (float)lua_tonumber(L, 4) : FLT_MAX;
    float mx = lua_isnumber(L, 5) ? (float)lua_tonumber(L, 5) : FLT_MAX;
    float w = lua_isnumber(L, 6) ? (float)lua_tonumber(L, 6) : 0.0f;
    float h = lua_isnumber(L, 7) ? (float)lua_tonumber(L, 7) : 50.0f;
    ImGui::PlotHistogram(lua_tostring(L, 1), vals.data(), (int)vals.size(), 0, ov, mn, mx, ImVec2(w, h));
    return 0;
}

int l_im_selectable(lua_State* L) {
    lua_pushboolean(L, ImGui::Selectable(lua_isstring(L, 1) ? lua_tostring(L, 1) : "", lua_toboolean(L, 2) != 0));
    return 1;
}

int l_im_collapsing_header(lua_State* L) {
    lua_pushboolean(L, ImGui::CollapsingHeader(lua_isstring(L, 1) ? lua_tostring(L, 1) : ""));
    return 1;
}

int l_im_tree_node(lua_State* L) {
    lua_pushboolean(L, ImGui::TreeNode(lua_isstring(L, 1) ? lua_tostring(L, 1) : ""));
    return 1;
}
int l_im_tree_pop(lua_State*) { ImGui::TreePop(); return 0; }

int l_im_begin_tab_bar(lua_State* L) {
    lua_pushboolean(L, ImGui::BeginTabBar(lua_isstring(L, 1) ? lua_tostring(L, 1) : "##tb"));
    return 1;
}
int l_im_end_tab_bar(lua_State*) { ImGui::EndTabBar(); return 0; }
int l_im_begin_tab_item(lua_State* L) {
    lua_pushboolean(L, ImGui::BeginTabItem(lua_isstring(L, 1) ? lua_tostring(L, 1) : "Tab"));
    return 1;
}
int l_im_end_tab_item(lua_State*) { ImGui::EndTabItem(); return 0; }

int l_im_new_line(lua_State*) { ImGui::NewLine(); return 0; }
int l_im_dummy(lua_State* L) { ImGui::Dummy(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2))); return 0; }
int l_im_indent(lua_State* L) { ImGui::Indent(lua_isnumber(L, 1) ? (float)lua_tonumber(L, 1) : 0.0f); return 0; }
int l_im_unindent(lua_State* L) { ImGui::Unindent(lua_isnumber(L, 1) ? (float)lua_tonumber(L, 1) : 0.0f); return 0; }

int l_im_begin_child(lua_State* L) {
    const char* id = lua_isstring(L, 1) ? lua_tostring(L, 1) : "##child";
    float w = lua_isnumber(L, 2) ? (float)lua_tonumber(L, 2) : 0.0f;
    float h = lua_isnumber(L, 3) ? (float)lua_tonumber(L, 3) : 0.0f;
    bool border = lua_toboolean(L, 4) != 0;
    lua_pushboolean(L, ImGui::BeginChild(id, ImVec2(w, h), border));
    return 1;
}
int l_im_end_child(lua_State*) { ImGui::EndChild(); return 0; }
int l_im_begin_group(lua_State*) { ImGui::BeginGroup(); return 0; }
int l_im_end_group(lua_State*) { ImGui::EndGroup(); return 0; }

int l_im_columns(lua_State* L) {
    int cnt = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : 1;
    const char* id = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    bool border = lua_isnoneornil(L, 3) ? true : (lua_toboolean(L, 3) != 0);
    ImGui::Columns(cnt, id, border);
    return 0;
}
int l_im_next_column(lua_State*) { ImGui::NextColumn(); return 0; }
int l_im_set_next_item_width(lua_State* L) { ImGui::SetNextItemWidth((float)lua_tonumber(L, 1)); return 0; }

int l_im_get_cursor_pos(lua_State* L) {
    ImVec2 p = ImGui::GetCursorPos();
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y);
    return 2;
}
int l_im_set_cursor_pos(lua_State* L) {
    ImGui::SetCursorPos(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)));
    return 0;
}
int l_im_get_window_pos(lua_State* L) {
    ImVec2 p = ImGui::GetWindowPos();
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y);
    return 2;
}
int l_im_get_window_size(lua_State* L) {
    ImVec2 s = ImGui::GetWindowSize();
    lua_pushnumber(L, s.x); lua_pushnumber(L, s.y);
    return 2;
}
int l_im_get_avail(lua_State* L) {
    ImVec2 s = ImGui::GetContentRegionAvail();
    lua_pushnumber(L, s.x); lua_pushnumber(L, s.y);
    return 2;
}
int l_im_is_item_hovered(lua_State* L) { lua_pushboolean(L, ImGui::IsItemHovered()); return 1; }
int l_im_is_item_clicked(lua_State* L) {
    lua_pushboolean(L, ImGui::IsItemClicked(lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : 0));
    return 1;
}
int l_im_set_tooltip(lua_State* L) {
    if (lua_isstring(L, 1)) ImGui::SetTooltip("%s", lua_tostring(L, 1));
    return 0;
}
int l_im_get_mouse_pos(lua_State* L) {
    ImVec2 m = ImGui::GetIO().MousePos;
    lua_pushnumber(L, m.x); lua_pushnumber(L, m.y);
    return 2;
}

// Window DrawList drawing primitives (draw inside the current ImGui window)
int l_im_draw_line(lua_State* L) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return 0;
    float thick = lua_isnumber(L, 9) ? (float)lua_tonumber(L, 9) : 1.0f;
    dl->AddLine(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)),
                ImVec2((float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4)),
                ReadColor(L, 5), thick);
    return 0;
}
int l_im_draw_rect(lua_State* L) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return 0;
    float x = (float)lua_tonumber(L, 1), y = (float)lua_tonumber(L, 2);
    float w = (float)lua_tonumber(L, 3), h = (float)lua_tonumber(L, 4);
    float rnd = lua_isnumber(L, 9) ? (float)lua_tonumber(L, 9) : 0.0f;
    float thick = lua_isnumber(L, 10) ? (float)lua_tonumber(L, 10) : 1.0f;
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ReadColor(L, 5), rnd, 0, thick);
    return 0;
}
int l_im_draw_rect_filled(lua_State* L) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return 0;
    float x = (float)lua_tonumber(L, 1), y = (float)lua_tonumber(L, 2);
    float w = (float)lua_tonumber(L, 3), h = (float)lua_tonumber(L, 4);
    float rnd = lua_isnumber(L, 9) ? (float)lua_tonumber(L, 9) : 0.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ReadColor(L, 5), rnd);
    return 0;
}
int l_im_draw_rect_gradient(lua_State* L) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return 0;
    float x = (float)lua_tonumber(L, 1), y = (float)lua_tonumber(L, 2);
    float w = (float)lua_tonumber(L, 3), h = (float)lua_tonumber(L, 4);
    ImU32 c1 = ReadColor(L, 5), c2 = ReadColor(L, 9);
    bool horiz = lua_toboolean(L, 13) != 0;
    if (horiz) dl->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + h), c1, c2, c2, c1);
    else       dl->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + h), c1, c1, c2, c2);
    return 0;
}
int l_im_draw_circle(lua_State* L) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return 0;
    int seg = lua_isnumber(L, 8) ? (int)lua_tointeger(L, 8) : 24;
    float thick = lua_isnumber(L, 9) ? (float)lua_tonumber(L, 9) : 1.0f;
    dl->AddCircle(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)),
                  (float)lua_tonumber(L, 3), ReadColor(L, 4), seg, thick);
    return 0;
}
int l_im_draw_circle_filled(lua_State* L) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return 0;
    int seg = lua_isnumber(L, 8) ? (int)lua_tointeger(L, 8) : 24;
    dl->AddCircleFilled(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)),
                        (float)lua_tonumber(L, 3), ReadColor(L, 4), seg);
    return 0;
}
int l_im_draw_text(lua_State* L) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl || !lua_isstring(L, 3)) return 0;
    dl->AddText(ImVec2((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2)), ReadColor(L, 4), lua_tostring(L, 3));
    return 0;
}

// ---------------------------------------------------------------------------
// ui.window(...) handle & methods
// ---------------------------------------------------------------------------
void PushWinHandle(lua_State* L, int id) {
    WinHandle* h = (WinHandle*)lua_newuserdata(L, sizeof(WinHandle));
    h->id = id;
    luaL_getmetatable(L, WIN_MT);
    lua_setmetatable(L, -2);
}

LuaWindow* CheckWinHandle(lua_State* L) {
    WinHandle* h = (WinHandle*)luaL_testudata(L, 1, WIN_MT);
    return h ? FindWindow(h->id) : nullptr;
}

void ApplyWinStyleTable(lua_State* L, int idx, LuaWindow& w) {
    if (!lua_istable(L, idx)) return;
    lua_getfield(L, idx, "x"); if (lua_isnumber(L, -1)) { w.x = (float)lua_tonumber(L, -1); w.has_pos = w.pos_dirty = true; } lua_pop(L, 1);
    lua_getfield(L, idx, "y"); if (lua_isnumber(L, -1)) { w.y = (float)lua_tonumber(L, -1); w.has_pos = w.pos_dirty = true; } lua_pop(L, 1);
    lua_getfield(L, idx, "w"); if (lua_isnumber(L, -1)) { w.w = (float)lua_tonumber(L, -1); w.has_size = w.size_dirty = true; } lua_pop(L, 1);
    lua_getfield(L, idx, "h"); if (lua_isnumber(L, -1)) { w.h = (float)lua_tonumber(L, -1); w.has_size = w.size_dirty = true; } lua_pop(L, 1);
    lua_getfield(L, idx, "open"); if (!lua_isnil(L, -1)) w.open = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "only_when_menu_open"); if (!lua_isnil(L, -1)) w.only_when_menu_open = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "click_through"); if (!lua_isnil(L, -1)) w.click_through_when_closed = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "blur"); if (!lua_isnil(L, -1)) w.blur = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "alpha"); if (lua_isnumber(L, -1)) w.alpha = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "rounding"); if (lua_isnumber(L, -1)) w.rounding = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "border_size"); if (lua_isnumber(L, -1)) w.border_size = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "no_titlebar"); if (!lua_isnil(L, -1)) w.no_titlebar = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "no_resize"); if (!lua_isnil(L, -1)) w.no_resize = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "no_move"); if (!lua_isnil(L, -1)) w.no_move = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "no_background"); if (!lua_isnil(L, -1)) w.no_background = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "no_scrollbar"); if (!lua_isnil(L, -1)) w.no_scrollbar = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "auto_resize"); if (!lua_isnil(L, -1)) w.auto_resize = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "no_inputs"); if (!lua_isnil(L, -1)) w.no_inputs = lua_toboolean(L, -1) != 0; lua_pop(L, 1);

    lua_getfield(L, idx, "bg");
    if (ReadColorTable(L, -1, w.bg)) w.has_bg = true;
    lua_pop(L, 1);
    lua_getfield(L, idx, "border");
    if (ReadColorTable(L, -1, w.border)) w.has_border = true;
    lua_pop(L, 1);
    lua_getfield(L, idx, "text");
    if (ReadColorTable(L, -1, w.text_col)) w.has_text = true;
    lua_pop(L, 1);
    lua_getfield(L, idx, "title_bg");
    if (ReadColorTable(L, -1, w.title_bg)) w.has_title_bg = true;
    lua_pop(L, 1);
    lua_getfield(L, idx, "padding");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1); if (lua_isnumber(L, -1)) w.pad_x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 2); if (lua_isnumber(L, -1)) w.pad_y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        w.has_padding = true;
    }
    lua_pop(L, 1);
}

int l_win_set_open(lua_State* L) { if (auto* w = CheckWinHandle(L)) w->open = lua_toboolean(L, 2) != 0; return 0; }
int l_win_is_open(lua_State* L) { auto* w = CheckWinHandle(L); lua_pushboolean(L, w ? w->open : 0); return 1; }
int l_win_set_pos(lua_State* L) {
    if (auto* w = CheckWinHandle(L)) { w->x = (float)lua_tonumber(L, 2); w->y = (float)lua_tonumber(L, 3); w->has_pos = w->pos_dirty = true; }
    return 0;
}
int l_win_get_pos(lua_State* L) {
    auto* w = CheckWinHandle(L);
    if (!w) return 0;
    lua_pushnumber(L, w->cur_x); lua_pushnumber(L, w->cur_y);
    return 2;
}
int l_win_set_size(lua_State* L) {
    if (auto* w = CheckWinHandle(L)) { w->w = (float)lua_tonumber(L, 2); w->h = (float)lua_tonumber(L, 3); w->has_size = w->size_dirty = true; }
    return 0;
}
int l_win_get_size(lua_State* L) {
    auto* w = CheckWinHandle(L);
    if (!w) return 0;
    lua_pushnumber(L, w->cur_w); lua_pushnumber(L, w->cur_h);
    return 2;
}
int l_win_set_alpha(lua_State* L) { if (auto* w = CheckWinHandle(L)) w->alpha = (float)lua_tonumber(L, 2); return 0; }
int l_win_set_style(lua_State* L) { if (auto* w = CheckWinHandle(L)) ApplyWinStyleTable(L, 2, *w); return 0; }

int l_ui_window(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isfunction(L, 2)) return 0;
    LuaWindow w;
    w.id = g_next_win_id++;
    w.script_id = g_loading_script;
    w.title = lua_tostring(L, 1);
    lua_pushvalue(L, 2);
    w.render_fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, 3)) ApplyWinStyleTable(L, 3, w);
    g_windows.push_back(std::move(w));
    PushWinHandle(L, g_windows.back().id);
    return 1;
}

// ---------------------------------------------------------------------------
// client.* API
// ---------------------------------------------------------------------------
int l_client_log(lua_State* L) { LuaConsolePush(lua_isstring(L, 1) ? lua_tostring(L, 1) : "", false); return 0; }
int l_client_exec(lua_State* L) { if (engine.self && lua_isstring(L, 1)) engine.ClientCmd(lua_tostring(L, 1)); return 0; }
int l_client_is_in_game(lua_State* L) { lua_pushboolean(L, IsGameReady() && GetLocalEntity() != nullptr); return 1; }

int l_client_local_player(lua_State* L) {
    if (!IsGameReady()) return 0;
    void* local = GetLocalEntity();
    if (!local) return 0;
    int idx = engine.GetLocalPlayer();
    int hp = *reinterpret_cast<int*>((char*)local + off::m_iHealth);
    int armor = *reinterpret_cast<int*>((char*)local + off::m_ArmorValue);
    int team = *reinterpret_cast<int*>((char*)local + off::m_iTeamNum);
    bool alive = (*reinterpret_cast<char*>((char*)local + off::m_lifeState) == 0 && hp > 0);
    int flags = *reinterpret_cast<int*>((char*)local + off::m_fFlags);
    float fov = view_fov;
    Vector origin = *reinterpret_cast<Vector*>((char*)local + off::m_vecOrigin);
    Vector eye = PlayerEye(local);
    Vector vel = *reinterpret_cast<Vector*>((char*)local + off::m_vecVelocity);
    float speed = sqrtf(vel.x * vel.x + vel.y * vel.y);

    lua_createtable(L, 0, 11);
    lua_pushinteger(L, idx); lua_setfield(L, -2, "index");
    lua_pushinteger(L, hp); lua_setfield(L, -2, "hp");
    lua_pushinteger(L, armor); lua_setfield(L, -2, "armor");
    lua_pushinteger(L, team); lua_setfield(L, -2, "team");
    lua_pushboolean(L, alive); lua_setfield(L, -2, "alive");
    lua_pushinteger(L, flags); lua_setfield(L, -2, "flags");
    lua_pushnumber(L, fov); lua_setfield(L, -2, "fov");
    lua_pushnumber(L, speed); lua_setfield(L, -2, "speed");
    PushVec3(L, origin); lua_setfield(L, -2, "origin");
    PushVec3(L, eye); lua_setfield(L, -2, "eye");
    PushVec3(L, vel); lua_setfield(L, -2, "velocity");
    return 1;
}

int l_client_players(lua_State* L) {
    lua_newtable(L);
    if (!IsGameReady()) return 1;
    void* local = GetLocalEntity();
    int local_team = local ? *reinterpret_cast<int*>((char*)local + off::m_iTeamNum) : 0;
    int out_idx = 1;
    for (int i = 1; i <= 64; i++) {
        void* ent = entity_list.GetClientEntity(i);
        if (!ent || !IsPlayerIndex(i)) continue;
        bool dormant = EntityIsStale(ent, i);
        int hp = *reinterpret_cast<int*>((char*)ent + off::m_iHealth);
        int armor = *reinterpret_cast<int*>((char*)ent + off::m_ArmorValue);
        int team = *reinterpret_cast<int*>((char*)ent + off::m_iTeamNum);
        bool alive = (*reinterpret_cast<char*>((char*)ent + off::m_lifeState) == 0 && hp > 0);
        Vector origin = *reinterpret_cast<Vector*>((char*)ent + off::m_vecOrigin);
        Vector eye = PlayerEye(ent);
        Vector vel = *reinterpret_cast<Vector*>((char*)ent + off::m_vecVelocity);
        player_info_t pi = {};
        engine.GetPlayerInfo(i, &pi);

        lua_createtable(L, 0, 12);
        lua_pushinteger(L, i); lua_setfield(L, -2, "index");
        lua_pushstring(L, pi.name); lua_setfield(L, -2, "name");
        lua_pushinteger(L, hp); lua_setfield(L, -2, "hp");
        lua_pushinteger(L, armor); lua_setfield(L, -2, "armor");
        lua_pushinteger(L, team); lua_setfield(L, -2, "team");
        lua_pushboolean(L, local && team != local_team); lua_setfield(L, -2, "enemy");
        lua_pushboolean(L, ent == local); lua_setfield(L, -2, "is_local");
        lua_pushboolean(L, alive); lua_setfield(L, -2, "alive");
        lua_pushboolean(L, dormant); lua_setfield(L, -2, "dormant");
        PushVec3(L, origin); lua_setfield(L, -2, "origin");
        PushVec3(L, eye); lua_setfield(L, -2, "eye");
        PushVec3(L, vel); lua_setfield(L, -2, "velocity");
        lua_rawseti(L, -2, out_idx++);
    }
    return 1;
}

int l_client_bone_pos(lua_State* L) {
    if (!IsGameReady() || !lua_isnumber(L, 1) || !lua_isnumber(L, 2)) return 0;
    Vector pos;
    if (!GetEntityBonePos((int)lua_tointeger(L, 1), (int)lua_tointeger(L, 2), pos)) return 0;
    lua_pushnumber(L, pos.x); lua_pushnumber(L, pos.y); lua_pushnumber(L, pos.z);
    return 3;
}

int l_client_trace_visible(lua_State* L) {
    if (!IsGameReady()) { lua_pushboolean(L, 0); return 1; }
    Vector from((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3));
    Vector to((float)lua_tonumber(L, 4), (float)lua_tonumber(L, 5), (float)lua_tonumber(L, 6));
    lua_pushboolean(L, TraceVisible(from, to, GetLocalEntity()));
    return 1;
}

int l_client_view_angles(lua_State* L) {
    QAngle a = view_angles;
    if (engine.self) engine.GetViewAngles(a);
    lua_pushnumber(L, a.x); lua_pushnumber(L, a.y); lua_pushnumber(L, a.z);
    return 3;
}

int l_client_set_view_angles(lua_State* L) {
    if (engine.self) engine.SetViewAngles(QAngle((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3)));
    return 0;
}

int l_client_fps(lua_State* L) { lua_pushnumber(L, ImGui::GetIO().Framerate); return 1; }
int l_client_time(lua_State* L) { lua_pushnumber(L, engine.self ? engine.Time() : (double)GetTickCount64() * 0.001); return 1; }
int l_client_menu_open(lua_State* L) { lua_pushboolean(L, cfg.menu_open); return 1; }
int l_client_key_down(lua_State* L) { int vk = (int)lua_tointeger(L, 1); lua_pushboolean(L, vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0); return 1; }

int l_client_cvar_get(lua_State* L) {
    if (!cvar.self || !lua_isstring(L, 1)) return 0;
    ConVar* cv = cvar.FindVar(lua_tostring(L, 1));
    if (!cv) return 0;
    lua_pushnumber(L, cv->f_value);
    return 1;
}

int l_client_cvar_set(lua_State* L) {
    if (!cvar.self || !lua_isstring(L, 1) || !lua_isnumber(L, 2)) return 0;
    ConVar* cv = cvar.FindVar(lua_tostring(L, 1));
    if (!cv) return 0;
    float v = (float)lua_tonumber(L, 2);
    cv->f_value = v;
    cv->i_value = (int)v;
    return 0;
}

// ---------------------------------------------------------------------------
// menu.* & cheat.* (hooks to internal menu & cheat functions)
// ---------------------------------------------------------------------------
int l_menu_get(lua_State* L) {
    const CfgField* f = FindCfgField(lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr);
    if (!f) return 0;
    PushCfgValue(L, f);
    return 1;
}

int l_menu_set(lua_State* L) {
    const CfgField* f = FindCfgField(lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr);
    if (!f) { lua_pushboolean(L, 0); return 1; }
    switch (f->type) {
    case CfgFieldType::Bool:  *(bool*)f->ptr = lua_toboolean(L, 2) != 0; break;
    case CfgFieldType::Float: *(float*)f->ptr = (float)lua_tonumber(L, 2); break;
    case CfgFieldType::Int:   *(int*)f->ptr = (int)lua_tointeger(L, 2); break;
    case CfgFieldType::Str:
        if (lua_isstring(L, 2)) {
            strncpy((char*)f->ptr, lua_tostring(L, 2), f->str_cap - 1);
            ((char*)f->ptr)[f->str_cap - 1] = 0;
        }
        break;
    case CfgFieldType::Color4: {
        float* c = (float*)f->ptr;
        if (lua_istable(L, 2)) {
            ReadColorTable(L, 2, c);
        } else {
            c[0] = Clamp255(L, 2, 255) / 255.0f;
            c[1] = Clamp255(L, 3, 255) / 255.0f;
            c[2] = Clamp255(L, 4, 255) / 255.0f;
            c[3] = Clamp255(L, 5, 255) / 255.0f;
        }
        break;
    }
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_menu_list(lua_State* L) {
    lua_newtable(L);
    int idx = 1;
    for (const auto& f : kCfgFields) {
        lua_pushstring(L, f.name);
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

int l_menu_is_open(lua_State* L) { lua_pushboolean(L, cfg.menu_open); return 1; }
int l_menu_set_open(lua_State* L) { cfg.menu_open = lua_toboolean(L, 1) != 0; return 0; }

int l_menu_on_change(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isfunction(L, 2)) return 0;
    const CfgField* f = FindCfgField(lua_tostring(L, 1));
    if (!f) return 0;
    CfgWatch w;
    w.id = g_next_watch_id++;
    w.script_id = g_loading_script;
    w.field = f;
    lua_pushvalue(L, 2);
    w.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (f->type == CfgFieldType::Bool) w.last_b = *(bool*)f->ptr;
    else if (f->type == CfgFieldType::Float) w.last_f = *(float*)f->ptr;
    else if (f->type == CfgFieldType::Int) w.last_i = *(int*)f->ptr;
    else if (f->type == CfgFieldType::Color4) memcpy(w.last_c, f->ptr, sizeof(w.last_c));
    else if (f->type == CfgFieldType::Str) w.last_s = (const char*)f->ptr;
    g_cfg_watches.push_back(std::move(w));
    lua_pushinteger(L, g_cfg_watches.back().id);
    return 1;
}

// cheat.* internal function bindings
int l_cheat_miss_log(lua_State* L) {
    const char* pfx = lua_isstring(L, 1) ? lua_tostring(L, 1) : "[Lua] ";
    const char* txt = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    MissLog_Push(pfx, txt);
    return 0;
}

int l_cheat_kill_fx(lua_State* L) {
    Vector pos((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3));
    KillFx_Spawn(pos, Vector(0, 0, 1), 0);
    return 0;
}

int l_cheat_trace_line(lua_State* L) {
    if (!IsGameReady()) { lua_pushboolean(L, 0); return 1; }
    Vector from((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3));
    Vector to((float)lua_tonumber(L, 4), (float)lua_tonumber(L, 5), (float)lua_tonumber(L, 6));
    float frac = 1.0f;
    Vector end = to;
    bool hit = TraceLine(from, to, frac, end, GetLocalEntity());
    lua_pushboolean(L, hit);
    lua_pushnumber(L, frac);
    lua_pushnumber(L, end.x);
    lua_pushnumber(L, end.y);
    lua_pushnumber(L, end.z);
    return 5;
}

int l_cheat_can_penetrate(lua_State* L) {
    if (!IsGameReady()) { lua_pushboolean(L, 0); return 1; }
    Vector from((float)lua_tonumber(L, 1), (float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3));
    Vector to((float)lua_tonumber(L, 4), (float)lua_tonumber(L, 5), (float)lua_tonumber(L, 6));
    float max_t = lua_isnumber(L, 7) ? (float)lua_tonumber(L, 7) : 64.0f;
    lua_pushboolean(L, CanPenetrate(from, to, max_t, GetLocalEntity()));
    return 1;
}

int l_cheat_apply_walls(lua_State*) { WorldFx_ApplyWalls(); return 0; }

int l_cheat_refresh_servers(lua_State* L) {
    const char* reason = lua_isstring(L, 1) ? lua_tostring(L, 1) : "lua";
    lua_pushboolean(L, StartServerListRefresh(reason));
    return 1;
}

int l_cheat_server_status(lua_State* L) {
    char buf[256] = {};
    ServerListStatus(buf, sizeof(buf));
    lua_pushstring(L, buf);
    return 1;
}

int l_cheat_config_save(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    alice_ui::ConfigMenuInit();
    alice_ui::ConfigMenuSave(lua_tostring(L, 1));
    return 0;
}

int l_cheat_config_load(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    alice_ui::ConfigMenuInit();
    alice_ui::ConfigMenuLoad(lua_tostring(L, 1));
    return 0;
}

int l_cheat_config_delete(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    alice_ui::ConfigMenuInit();
    alice_ui::ConfigMenuDelete(lua_tostring(L, 1));
    return 0;
}

int l_cheat_config_list(lua_State* L) {
    alice_ui::ConfigMenuInit();
    alice_ui::ConfigMenuRefresh();
    int n = alice_ui::ConfigMenuCount();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        const char* nm = alice_ui::ConfigMenuName(i);
        lua_pushstring(L, nm ? nm : "");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_cheat_custom_models(lua_State* L) {
    int n = CustomModels_Count(false);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        const char* m = CustomModels_At(i);
        lua_pushstring(L, m ? m : "");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_cheat_bind_engaged(lua_State* L) {
    lua_pushboolean(L, BindIsEngaged((int)lua_tointeger(L, 1), (int)lua_tointeger(L, 2)));
    return 1;
}

int l_cheat_resolver_side(lua_State* L) { lua_pushinteger(L, Resolver_Side((int)lua_tointeger(L, 1))); return 1; }
int l_cheat_resolver_resolved(lua_State* L) { lua_pushboolean(L, Resolver_IsResolved((int)lua_tointeger(L, 1))); return 1; }
int l_cheat_target_shot(lua_State* L) {
    ULONGLONG ms = lua_isnumber(L, 2) ? (ULONGLONG)lua_tointeger(L, 2) : 250;
    lua_pushboolean(L, TargetRecentlyShot((int)lua_tointeger(L, 1), ms));
    return 1;
}

int l_cheat_peek_circle(lua_State* L) {
    Vector c;
    if (!PeekAssist_GetCircle(c)) return 0;
    lua_pushnumber(L, c.x); lua_pushnumber(L, c.y); lua_pushnumber(L, c.z);
    return 3;
}

int l_cheat_backtrack_pos(lua_State* L) {
    Vector p = Backtrack_PickRaw((int)lua_tointeger(L, 1), (float)lua_tonumber(L, 2));
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}

// ---------------------------------------------------------------------------
// callbacks.* / hooks.* API
// ---------------------------------------------------------------------------
int l_callbacks_add(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isfunction(L, 2)) return 0;
    const char* ev = lua_tostring(L, 1);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    LuaCallback cb;
    cb.id = g_next_cb_id++;
    cb.script_id = g_loading_script;
    cb.event = ev;
    cb.fn_ref = ref;
    g_callbacks.push_back(std::move(cb));
    lua_pushinteger(L, cb.id);
    return 1;
}

int l_callbacks_remove(lua_State* L) {
    int id = (int)lua_tointeger(L, 1);
    for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
        if (it->id == id) {
            if (it->fn_ref != LUA_NOREF) luaL_unref(g_L, LUA_REGISTRYINDEX, it->fn_ref);
            g_callbacks.erase(it);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// ---------------------------------------------------------------------------
// Control Handle Methods
// ---------------------------------------------------------------------------
void PushCtrlHandle(lua_State* L, int id) {
    CtrlHandle* h = (CtrlHandle*)lua_newuserdata(L, sizeof(CtrlHandle));
    h->id = id;
    luaL_getmetatable(L, CTRL_MT);
    lua_setmetatable(L, -2);
}

LuaControl* CheckCtrlHandle(lua_State* L) {
    CtrlHandle* h = (CtrlHandle*)luaL_testudata(L, 1, CTRL_MT);
    return h ? FindControl(h->id) : nullptr;
}

int l_ctrl_get(lua_State* L) {
    LuaControl* c = CheckCtrlHandle(L);
    if (!c) return 0;
    switch (c->type) {
    case CtrlType::Checkbox:    lua_pushboolean(L, c->bval); return 1;
    case CtrlType::SliderFloat: lua_pushnumber(L, c->fval); return 1;
    case CtrlType::SliderInt:   lua_pushinteger(L, c->ival); return 1;
    case CtrlType::Combo:       lua_pushinteger(L, c->ival + 1); return 1;
    case CtrlType::Color:
        lua_pushinteger(L, (int)roundf(c->col[0] * 255.0f));
        lua_pushinteger(L, (int)roundf(c->col[1] * 255.0f));
        lua_pushinteger(L, (int)roundf(c->col[2] * 255.0f));
        lua_pushinteger(L, (int)roundf(c->col[3] * 255.0f));
        return 4;
    case CtrlType::InputText:
    case CtrlType::Label:       lua_pushstring(L, c->text); return 1;
    case CtrlType::Keybind:     lua_pushinteger(L, c->vk); return 1;
    default: return 0;
    }
}

int l_ctrl_get_text(lua_State* L) {
    LuaControl* c = CheckCtrlHandle(L);
    if (!c) return 0;
    if (c->type == CtrlType::Combo && c->ival >= 0 && c->ival < (int)c->items.size()) {
        lua_pushstring(L, c->items[c->ival].c_str());
        return 1;
    }
    lua_pushstring(L, c->text);
    return 1;
}

int l_ctrl_is_down(lua_State* L) {
    LuaControl* c = CheckCtrlHandle(L);
    lua_pushboolean(L, c && c->type == CtrlType::Keybind && c->vk > 0 && (GetAsyncKeyState(c->vk) & 0x8000) != 0);
    return 1;
}

int l_ctrl_set(lua_State* L) {
    LuaControl* c = CheckCtrlHandle(L);
    if (!c) return 0;
    switch (c->type) {
    case CtrlType::Checkbox:    c->bval = lua_toboolean(L, 2) != 0; break;
    case CtrlType::SliderFloat: c->fval = (float)lua_tonumber(L, 2); break;
    case CtrlType::SliderInt:   c->ival = (int)lua_tointeger(L, 2); break;
    case CtrlType::Combo: {
        int v = (int)lua_tointeger(L, 2) - 1;
        if (v < 0) v = 0;
        if (!c->items.empty() && v >= (int)c->items.size()) v = (int)c->items.size() - 1;
        c->ival = v;
        break;
    }
    case CtrlType::Color:
        c->col[0] = Clamp255(L, 2, 255) / 255.0f;
        c->col[1] = Clamp255(L, 3, 255) / 255.0f;
        c->col[2] = Clamp255(L, 4, 255) / 255.0f;
        c->col[3] = Clamp255(L, 5, 255) / 255.0f;
        break;
    case CtrlType::InputText:
    case CtrlType::Label:
        if (lua_isstring(L, 2)) {
            strncpy(c->text, lua_tostring(L, 2), sizeof(c->text) - 1);
            c->text[sizeof(c->text) - 1] = 0;
        }
        break;
    case CtrlType::Keybind: c->vk = (int)lua_tointeger(L, 2); break;
    default: break;
    }
    return 0;
}

int l_ctrl_set_visible(lua_State* L) { if (auto* c = CheckCtrlHandle(L)) c->visible = lua_toboolean(L, 2) != 0; return 0; }
int l_ctrl_on_change(lua_State* L) {
    LuaControl* c = CheckCtrlHandle(L);
    if (!c || !lua_isfunction(L, 2)) return 0;
    if (c->fn_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, c->fn_ref);
    lua_pushvalue(L, 2);
    c->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

// ---------------------------------------------------------------------------
// ui.* API (Extension tabs + Retained controls + Immediate helpers)
// ---------------------------------------------------------------------------
int l_ui_tab(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    std::string name = lua_tostring(L, 1);
    int fn_ref = LUA_NOREF;
    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    EnsureExtensionTab(name, g_loading_script, fn_ref);
    lua_pushstring(L, name.c_str());
    return 1;
}

int l_ui_checkbox(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    c.type = CtrlType::Checkbox; c.bval = lua_toboolean(L, 3) != 0;
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_slider_float(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    c.type = CtrlType::SliderFloat;
    c.fmin = (float)lua_tonumber(L, 3); c.fmax = (float)lua_tonumber(L, 4);
    c.fval = lua_isnumber(L, 5) ? (float)lua_tonumber(L, 5) : c.fmin;
    if (lua_isstring(L, 6)) c.fmt = lua_tostring(L, 6);
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_slider_int(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    c.type = CtrlType::SliderInt;
    c.imin = (int)lua_tointeger(L, 3); c.imax = (int)lua_tointeger(L, 4);
    c.ival = lua_isnumber(L, 5) ? (int)lua_tointeger(L, 5) : c.imin;
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_combo(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2) || !lua_istable(L, 3)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    c.type = CtrlType::Combo;
    lua_Integer n = luaL_len(L, 3);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 3, i);
        if (lua_isstring(L, -1)) c.items.push_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    int def = lua_isnumber(L, 4) ? ((int)lua_tointeger(L, 4) - 1) : 0;
    if (def < 0) def = 0;
    if (!c.items.empty() && def >= (int)c.items.size()) def = (int)c.items.size() - 1;
    c.ival = def;
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_color(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    c.type = CtrlType::Color;
    c.col[0] = Clamp255(L, 3, 255) / 255.0f; c.col[1] = Clamp255(L, 4, 255) / 255.0f;
    c.col[2] = Clamp255(L, 5, 255) / 255.0f; c.col[3] = Clamp255(L, 6, 255) / 255.0f;
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_input_text(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    c.type = CtrlType::InputText;
    if (lua_isstring(L, 3)) strncpy(c.text, lua_tostring(L, 3), sizeof(c.text) - 1);
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_keybind(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    c.type = CtrlType::Keybind;
    c.vk = lua_isnumber(L, 3) ? (int)lua_tointeger(L, 3) : 0;
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_button(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    c.type = CtrlType::Button;
    if (lua_isfunction(L, 3)) { lua_pushvalue(L, 3); c.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX); }
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_label(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_tostring(L, 2);
    strncpy(c.text, c.label.c_str(), sizeof(c.text) - 1);
    c.type = CtrlType::Label;
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_separator(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    LuaControl c;
    c.id = g_next_ctrl_id++; c.script_id = g_loading_script;
    c.tab = lua_tostring(L, 1); c.label = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    c.type = CtrlType::Separator;
    EnsureExtensionTab(c.tab, g_loading_script);
    g_controls.push_back(std::move(c));
    PushCtrlHandle(L, g_controls.back().id);
    return 1;
}

int l_ui_same_line(lua_State* L) {
    ImGui::SameLine(lua_isnumber(L, 1) ? (float)lua_tonumber(L, 1) : 0.0f,
                    lua_isnumber(L, 2) ? (float)lua_tonumber(L, 2) : -1.0f);
    return 0;
}
int l_ui_spacing(lua_State*) { ImGui::Spacing(); return 0; }
int l_ui_im_text(lua_State* L) {
    const char* txt = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
    if (lua_isnumber(L, 2)) {
        ImVec4 col(Clamp255(L, 2, 255) / 255.0f, Clamp255(L, 3, 255) / 255.0f,
                   Clamp255(L, 4, 255) / 255.0f, Clamp255(L, 5, 255) / 255.0f);
        ImGui::TextColored(col, "%s", txt);
    } else ImGui::TextUnformatted(txt);
    return 0;
}
int l_ui_im_separator(lua_State* L) {
    if (lua_isstring(L, 1)) ImGui::SeparatorText(lua_tostring(L, 1));
    else ImGui::Separator();
    return 0;
}
int l_ui_im_checkbox(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    bool v = lua_toboolean(L, 2) != 0;
    bool ch = ImGui::Checkbox(lua_tostring(L, 1), &v);
    lua_pushboolean(L, v); lua_pushboolean(L, ch);
    return 2;
}
int l_ui_im_slider_float(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    float v = (float)lua_tonumber(L, 2);
    bool ch = ImGui::SliderFloat(lua_tostring(L, 1), &v, (float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4),
                                 lua_isstring(L, 5) ? lua_tostring(L, 5) : "%.2f");
    lua_pushnumber(L, v); lua_pushboolean(L, ch);
    return 2;
}
int l_ui_im_slider_int(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    int v = (int)lua_tointeger(L, 2);
    bool ch = ImGui::SliderInt(lua_tostring(L, 1), &v, (int)lua_tointeger(L, 3), (int)lua_tointeger(L, 4));
    lua_pushinteger(L, v); lua_pushboolean(L, ch);
    return 2;
}
int l_ui_im_button(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    lua_pushboolean(L, ImGui::Button(lua_tostring(L, 1), ImVec2(
        lua_isnumber(L, 2) ? (float)lua_tonumber(L, 2) : 0.0f,
        lua_isnumber(L, 3) ? (float)lua_tonumber(L, 3) : 0.0f)));
    return 1;
}

// ---------------------------------------------------------------------------
// Register all Lua libraries into g_L
// ---------------------------------------------------------------------------
void RegisterLuaAPI(lua_State* L) {
    lua_pushcfunction(L, l_print);           lua_setglobal(L, "print");
    lua_pushcfunction(L, l_world_to_screen); lua_setglobal(L, "world_to_screen");
    lua_pushcfunction(L, l_world_to_screen); lua_setglobal(L, "w2s");

    // Control handle metatable
    luaL_newmetatable(L, CTRL_MT);
    lua_newtable(L);
    static const luaL_Reg ctrl_methods[] = {
        { "get", l_ctrl_get }, { "set", l_ctrl_set }, { "get_text", l_ctrl_get_text },
        { "is_down", l_ctrl_is_down }, { "set_visible", l_ctrl_set_visible }, { "on_change", l_ctrl_on_change },
        { nullptr, nullptr }
    };
    luaL_setfuncs(L, ctrl_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    // Window handle metatable
    luaL_newmetatable(L, WIN_MT);
    lua_newtable(L);
    static const luaL_Reg win_methods[] = {
        { "set_open",  l_win_set_open },  { "is_open",  l_win_is_open },
        { "set_pos",   l_win_set_pos },   { "get_pos",  l_win_get_pos },
        { "set_size",  l_win_set_size },  { "get_size", l_win_get_size },
        { "set_alpha", l_win_set_alpha }, { "set_style", l_win_set_style },
        { nullptr, nullptr }
    };
    luaL_setfuncs(L, win_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    // render.*
    static const luaL_Reg render_lib[] = {
        { "world_to_screen", l_world_to_screen }, { "w2s", l_world_to_screen },
        { "screen_size", l_render_screen_size },
        { "text", l_render_text }, { "text_shadow", l_render_text_shadow }, { "measure_text", l_render_measure_text },
        { "line", l_render_line }, { "rect", l_render_rect }, { "rect_filled", l_render_rect_filled },
        { "rect_gradient", l_render_rect_gradient }, { "circle", l_render_circle },
        { "circle_filled", l_render_circle_filled }, { "triangle_filled", l_render_triangle_filled },
        { nullptr, nullptr }
    };
    luaL_newlib(L, render_lib);
    lua_setglobal(L, "render");

    // imgui.*
    static const luaL_Reg imgui_lib[] = {
        { "begin_window",        l_im_begin_window },
        { "end_window",          l_im_end_window },
        { "set_next_window_pos", l_im_set_next_pos },
        { "set_next_window_size",l_im_set_next_size },
        { "set_next_window_bg_alpha", l_im_set_next_bg_alpha },
        { "apply_glass_bg",      l_im_apply_glass_bg },
        { "push_style_color",    l_im_push_style_color },
        { "pop_style_color",     l_im_pop_style_color },
        { "push_style_var",      l_im_push_style_var },
        { "pop_style_var",       l_im_pop_style_var },
        { "set_window_font_scale", l_im_set_font_scale },
        { "text",                l_im_text },
        { "text_colored",        l_im_text_colored },
        { "text_disabled",       l_im_text_disabled },
        { "text_wrapped",        l_im_text_wrapped },
        { "bullet_text",         l_im_bullet_text },
        { "text_centered",       l_im_text_centered },
        { "calc_text_size",      l_render_measure_text },
        { "button",              l_ui_im_button },
        { "small_button",        l_im_small_button },
        { "checkbox",            l_ui_im_checkbox },
        { "slider_float",        l_ui_im_slider_float },
        { "slider_int",          l_ui_im_slider_int },
        { "combo",               l_im_combo },
        { "color_edit",          l_im_color_edit },
        { "input_text",          l_im_input_text },
        { "progress_bar",        l_im_progress_bar },
        { "plot_lines",          l_im_plot_lines },
        { "plot_histogram",      l_im_plot_histogram },
        { "selectable",          l_im_selectable },
        { "collapsing_header",   l_im_collapsing_header },
        { "tree_node",           l_im_tree_node },
        { "tree_pop",            l_im_tree_pop },
        { "begin_tab_bar",       l_im_begin_tab_bar },
        { "end_tab_bar",         l_im_end_tab_bar },
        { "begin_tab_item",      l_im_begin_tab_item },
        { "end_tab_item",        l_im_end_tab_item },
        { "same_line",           l_ui_same_line },
        { "new_line",            l_im_new_line },
        { "spacing",             l_ui_spacing },
        { "dummy",               l_im_dummy },
        { "separator",           l_ui_im_separator },
        { "indent",              l_im_indent },
        { "unindent",            l_im_unindent },
        { "begin_child",         l_im_begin_child },
        { "end_child",           l_im_end_child },
        { "begin_group",         l_im_begin_group },
        { "end_group",           l_im_end_group },
        { "columns",             l_im_columns },
        { "next_column",         l_im_next_column },
        { "set_next_item_width", l_im_set_next_item_width },
        { "get_cursor_pos",      l_im_get_cursor_pos },
        { "set_cursor_pos",      l_im_set_cursor_pos },
        { "get_window_pos",      l_im_get_window_pos },
        { "get_window_size",     l_im_get_window_size },
        { "get_content_region_avail", l_im_get_avail },
        { "is_item_hovered",     l_im_is_item_hovered },
        { "is_item_clicked",     l_im_is_item_clicked },
        { "set_tooltip",         l_im_set_tooltip },
        { "get_mouse_pos",       l_im_get_mouse_pos },
        { "draw_line",           l_im_draw_line },
        { "draw_rect",           l_im_draw_rect },
        { "draw_rect_filled",    l_im_draw_rect_filled },
        { "draw_rect_gradient",  l_im_draw_rect_gradient },
        { "draw_circle",         l_im_draw_circle },
        { "draw_circle_filled",  l_im_draw_circle_filled },
        { "draw_text",           l_im_draw_text },
        { nullptr, nullptr }
    };
    luaL_newlib(L, imgui_lib);
    // Attach useful ImGuiWindowFlags constants directly onto imgui.flags.*
    lua_newtable(L);
    lua_pushinteger(L, ImGuiWindowFlags_NoTitleBar);           lua_setfield(L, -2, "NoTitleBar");
    lua_pushinteger(L, ImGuiWindowFlags_NoResize);             lua_setfield(L, -2, "NoResize");
    lua_pushinteger(L, ImGuiWindowFlags_NoMove);               lua_setfield(L, -2, "NoMove");
    lua_pushinteger(L, ImGuiWindowFlags_NoScrollbar);          lua_setfield(L, -2, "NoScrollbar");
    lua_pushinteger(L, ImGuiWindowFlags_NoCollapse);           lua_setfield(L, -2, "NoCollapse");
    lua_pushinteger(L, ImGuiWindowFlags_AlwaysAutoResize);     lua_setfield(L, -2, "AlwaysAutoResize");
    lua_pushinteger(L, ImGuiWindowFlags_NoBackground);         lua_setfield(L, -2, "NoBackground");
    lua_pushinteger(L, ImGuiWindowFlags_NoSavedSettings);      lua_setfield(L, -2, "NoSavedSettings");
    lua_pushinteger(L, ImGuiWindowFlags_NoInputs);             lua_setfield(L, -2, "NoInputs");
    lua_setfield(L, -2, "flags");
    lua_setglobal(L, "imgui");

    // client.*
    static const luaL_Reg client_lib[] = {
        { "log", l_client_log }, { "exec", l_client_exec }, { "is_in_game", l_client_is_in_game },
        { "local_player", l_client_local_player }, { "players", l_client_players },
        { "bone_pos", l_client_bone_pos }, { "trace_visible", l_client_trace_visible },
        { "view_angles", l_client_view_angles }, { "set_view_angles", l_client_set_view_angles },
        { "fps", l_client_fps }, { "time", l_client_time }, { "menu_open", l_client_menu_open },
        { "key_down", l_client_key_down }, { "cvar_get", l_client_cvar_get }, { "cvar_set", l_client_cvar_set },
        { nullptr, nullptr }
    };
    luaL_newlib(L, client_lib);
    lua_setglobal(L, "client");

    // menu.*
    static const luaL_Reg menu_lib[] = {
        { "get",       l_menu_get },
        { "set",       l_menu_set },
        { "list",      l_menu_list },
        { "is_open",   l_menu_is_open },
        { "set_open",  l_menu_set_open },
        { "on_change", l_menu_on_change },
        { nullptr, nullptr }
    };
    luaL_newlib(L, menu_lib);
    lua_setglobal(L, "menu");

    // cheat.*
    static const luaL_Reg cheat_lib[] = {
        { "miss_log",            l_cheat_miss_log },
        { "kill_fx",             l_cheat_kill_fx },
        { "trace_line",          l_cheat_trace_line },
        { "can_penetrate",       l_cheat_can_penetrate },
        { "world_to_screen",     l_world_to_screen },
        { "apply_walls",         l_cheat_apply_walls },
        { "refresh_servers",     l_cheat_refresh_servers },
        { "server_list_status",  l_cheat_server_status },
        { "config_save",         l_cheat_config_save },
        { "config_load",         l_cheat_config_load },
        { "config_delete",       l_cheat_config_delete },
        { "config_list",         l_cheat_config_list },
        { "custom_models",       l_cheat_custom_models },
        { "bind_engaged",        l_cheat_bind_engaged },
        { "resolver_side",       l_cheat_resolver_side },
        { "resolver_is_resolved",l_cheat_resolver_resolved },
        { "target_recently_shot",l_cheat_target_shot },
        { "peek_circle",         l_cheat_peek_circle },
        { "backtrack_pos",       l_cheat_backtrack_pos },
        { nullptr, nullptr }
    };
    luaL_newlib(L, cheat_lib);
    lua_setglobal(L, "cheat");

    // callbacks.* and hooks.* alias
    static const luaL_Reg cb_lib[] = {
        { "add", l_callbacks_add }, { "remove", l_callbacks_remove },
        { nullptr, nullptr }
    };
    luaL_newlib(L, cb_lib);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "callbacks");
    lua_setglobal(L, "hooks");

    // ui.*
    static const luaL_Reg ui_lib[] = {
        { "window",          l_ui_window },
        { "tab",             l_ui_tab },
        { "checkbox",        l_ui_checkbox },
        { "slider_float",    l_ui_slider_float },
        { "slider_int",      l_ui_slider_int },
        { "combo",           l_ui_combo },
        { "color",           l_ui_color },
        { "input_text",      l_ui_input_text },
        { "keybind",         l_ui_keybind },
        { "button",          l_ui_button },
        { "label",           l_ui_label },
        { "separator",       l_ui_separator },
        { "same_line",       l_ui_same_line },
        { "spacing",         l_ui_spacing },
        { "im_text",         l_ui_im_text },
        { "im_separator",    l_ui_im_separator },
        { "im_checkbox",     l_ui_im_checkbox },
        { "im_slider_float", l_ui_im_slider_float },
        { "im_slider_int",   l_ui_im_slider_int },
        { "im_button",       l_ui_im_button },
        { "get_cfg",         l_menu_get },
        { "set_cfg",         l_menu_set },
        { "list_cfg",        l_menu_list },
        { nullptr, nullptr }
    };
    luaL_newlib(L, ui_lib);
    lua_setglobal(L, "ui");
}

// ---------------------------------------------------------------------------
// Script directory scan & example script creation
// ---------------------------------------------------------------------------
void InitLuaDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    char* slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = 0;
    snprintf(g_lua_dir, sizeof(g_lua_dir), "%sislamware_lua", exe);
    CreateDirectoryA(g_lua_dir, nullptr);
}

void CreateExampleScriptIfEmpty() {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.lua", g_lua_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); return; }

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\example.lua", g_lua_dir);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs(
        "-- Islamware Lua 5.4 Full Showcase\n"
        "-- Demonstrates: standalone HUD ImGui window outside menu, custom styling,\n"
        "-- extension tabs, menu.on_change hooks, cheat.* bindings, and WorldToScreen.\n\n"
        "local TAB = \"Lua Demo\"\n"
        "ui.tab(TAB)\n"
        "ui.separator(TAB, \"HUD & ESP\")\n"
        "local cb_hud    = ui.checkbox(TAB, \"Show standalone HUD window\", true)\n"
        "local cb_snap   = ui.checkbox(TAB, \"Enemy snaplines (WorldToScreen)\", true)\n"
        "local cb_tag    = ui.checkbox(TAB, \"Custom ESP badge hook\", true)\n"
        "local sl_thick  = ui.slider_float(TAB, \"Line thickness\", 0.5, 4.0, 1.5, \"%.1f\")\n"
        "local col_line  = ui.color(TAB, \"Accent color\", 80, 180, 255, 220)\n"
        "ui.button(TAB, \"Push Event Feed Notification\", function()\n"
        "    cheat.miss_log(\"[Lua] \", string.format(\"Hello! FPS = %.0f\", client.fps()))\n"
        "end)\n\n"
        "-- Hook directly into built-in menu toggles!\n"
        "menu.on_change(\"thirdperson\", function(new_val)\n"
        "    cheat.miss_log(\"[Lua] \", \"Thirdperson \" .. (new_val and \"ENABLED\" or \"DISABLED\"))\n"
        "end)\n\n"
        "-- Standalone styled ImGui window rendered OUTSIDE the main menu\n"
        "local speed_hist = {}\n"
        "local hud = ui.window(\"##lua_hud\", function()\n"
        "    local r, g, b, a = col_line:get()\n"
        "    imgui.set_window_font_scale(1.1)\n"
        "    imgui.text_colored(r, g, b, 255, \"ISLAMWARE LUA HUD\")\n"
        "    imgui.set_window_font_scale(1.0)\n"
        "    imgui.separator()\n"
        "    imgui.text(string.format(\"FPS: %.0f\", client.fps()))\n"
        "    local lp = client.local_player()\n"
        "    if lp and lp.alive then\n"
        "        imgui.text(string.format(\"HP: %d  |  Speed: %.0f\", lp.hp, lp.speed))\n"
        "        imgui.progress_bar(lp.hp / 100.0, -1, 10, string.format(\"%d HP\", lp.hp))\n"
        "        speed_hist[#speed_hist + 1] = lp.speed\n"
        "        if #speed_hist > 60 then table.remove(speed_hist, 1) end\n"
        "        imgui.plot_lines(\"##spd\", speed_hist, \"Velocity\", 0, 350, -1, 36)\n"
        "    else\n"
        "        imgui.text_disabled(\"Not in game / dead\")\n"
        "    end\n"
        "end, {\n"
        "    x = 24, y = 320, w = 220, h = 155,\n"
        "    no_titlebar = true, no_resize = true, blur = true,\n"
        "    rounding = 8, border = { 80, 180, 255, 160 }\n"
        "})\n"
        "cb_hud:on_change(function(c) hud:set_open(c:get()) end)\n\n"
        "-- Paint hook with WorldToScreen\n"
        "hooks.add(\"paint\", function(w, h)\n"
        "    if not cb_snap:get() or not client.is_in_game() then return end\n"
        "    local r, g, b, a = col_line:get()\n"
        "    for _, p in ipairs(client.players()) do\n"
        "        if p.enemy and p.alive and not p.dormant then\n"
        "            local sx, sy = render.world_to_screen(p.origin.x, p.origin.y, p.origin.z)\n"
        "            if sx and sy then\n"
        "                render.line(w * 0.5, h, sx, sy, r, g, b, a, sl_thick:get())\n"
        "            end\n"
        "        end\n"
        "    end\n"
        "end)\n\n"
        "-- Hook directly onto every player's ESP box\n"
        "hooks.add(\"player_esp\", function(p, box)\n"
        "    if not cb_tag:get() or not p.enemy then return end\n"
        "    render.text_shadow(box.x, box.y - 14, \"[LUA]\", 110, 210, 255, math.floor(255 * box.alpha))\n"
        "end)\n",
        f);
    fclose(f);
}

void RefreshScriptList() {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.lua", g_lua_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    std::vector<std::string> found;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                found.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end());

    std::vector<ScriptEntry> updated;
    for (const auto& name : found) {
        bool existed = false;
        for (const auto& s : g_scripts) {
            if (s.name == name) { updated.push_back(s); existed = true; break; }
        }
        if (!existed) {
            ScriptEntry s;
            s.id = g_next_script_id++;
            s.name = name;
            char full[MAX_PATH];
            snprintf(full, sizeof(full), "%s\\%s", g_lua_dir, name.c_str());
            s.full_path = full;
            updated.push_back(std::move(s));
        }
    }
    g_scripts = std::move(updated);
}

void LoadScript(ScriptEntry& s) {
    if (!g_L) return;
    if (s.loaded) CleanupScriptResources(s.id);
    s.loaded = false;
    s.last_error.clear();
    g_loading_script = s.id;
    int status = luaL_dofile(g_L, s.full_path.c_str());
    g_loading_script = -1;
    if (status != LUA_OK) {
        const char* err = lua_tostring(g_L, -1);
        s.last_error = err ? err : "unknown error";
        LuaConsolePush(s.last_error.c_str(), true);
        lua_pop(g_L, 1);
        CleanupScriptResources(s.id);
    } else {
        s.loaded = true;
        char msg[256];
        snprintf(msg, sizeof(msg), "Loaded %s", s.name.c_str());
        LuaConsolePush(msg, false);
    }
}

void UnloadScript(ScriptEntry& s) {
    if (!s.loaded) return;
    CleanupScriptResources(s.id);
    s.loaded = false;
    s.last_error.clear();
    char msg[256];
    snprintf(msg, sizeof(msg), "Unloaded %s", s.name.c_str());
    LuaConsolePush(msg, false);
}

static const char* VkName(int vk) {
    static char buf[32];
    if (vk <= 0) return "None";
    switch (vk) {
    case VK_LBUTTON: return "Mouse1"; case VK_RBUTTON: return "Mouse2";
    case VK_MBUTTON: return "Mouse3"; case VK_XBUTTON1: return "Mouse4"; case VK_XBUTTON2: return "Mouse5";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "Shift";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "Alt";
    case VK_SPACE: return "Space"; case VK_TAB: return "Tab"; case VK_INSERT: return "Ins";
    default:
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { buf[0] = (char)vk; buf[1] = 0; return buf; }
        snprintf(buf, sizeof(buf), "VK_%02X", vk);
        return buf;
    }
}

void RenderControlsForTab(const std::string& tab_name) {
    std::vector<int> ids;
    for (const auto& c : g_controls)
        if (c.visible && c.tab == tab_name) ids.push_back(c.id);

    for (int id : ids) {
        LuaControl* c = FindControl(id);
        if (!c || !c->visible) continue;
        ImGui::PushID(c->id);
        bool changed = false;
        switch (c->type) {
        case CtrlType::Checkbox:    changed = ImGui::Checkbox(c->label.c_str(), &c->bval); break;
        case CtrlType::SliderFloat: changed = ImGui::SliderFloat(c->label.c_str(), &c->fval, c->fmin, c->fmax, c->fmt.c_str()); break;
        case CtrlType::SliderInt:   changed = ImGui::SliderInt(c->label.c_str(), &c->ival, c->imin, c->imax); break;
        case CtrlType::Combo: {
            const char* preview = (c->ival >= 0 && c->ival < (int)c->items.size()) ? c->items[c->ival].c_str() : "";
            if (ImGui::BeginCombo(c->label.c_str(), preview)) {
                for (int i = 0; i < (int)c->items.size(); i++) {
                    bool sel = (i == c->ival);
                    if (ImGui::Selectable(c->items[i].c_str(), sel)) { c->ival = i; changed = true; }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case CtrlType::Color:
            changed = ImGui::ColorEdit4(c->label.c_str(), c->col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            break;
        case CtrlType::InputText:
            changed = ImGui::InputText(c->label.c_str(), c->text, sizeof(c->text));
            break;
        case CtrlType::Keybind: {
            ImGui::TextUnformatted(c->label.c_str());
            ImGui::SameLine();
            char btn[48];
            snprintf(btn, sizeof(btn), "%s##kb", c->binding ? "[...]" : VkName(c->vk));
            if (ImGui::SmallButton(btn)) c->binding = true;
            if (c->binding) {
                for (int vk = 1; vk < 255; vk++) {
                    if (GetAsyncKeyState(vk) & 0x8000) {
                        c->vk = (vk == VK_ESCAPE) ? 0 : vk;
                        c->binding = false;
                        changed = true;
                        break;
                    }
                }
            }
            break;
        }
        case CtrlType::Button:
            if (ImGui::Button(c->label.c_str()) && c->fn_ref != LUA_NOREF && g_L) {
                lua_rawgeti(g_L, LUA_REGISTRYINDEX, c->fn_ref);
                SafePCall(g_L, 0, 0, c->label.c_str());
            }
            break;
        case CtrlType::Label: ImGui::TextUnformatted(c->text); break;
        case CtrlType::Separator:
            if (!c->label.empty()) ImGui::SeparatorText(c->label.c_str());
            else ImGui::Separator();
            break;
        }

        if (changed) {
            c = FindControl(id);
            if (c && c->type != CtrlType::Button && c->fn_ref != LUA_NOREF && g_L) {
                lua_rawgeti(g_L, LUA_REGISTRYINDEX, c->fn_ref);
                PushCtrlHandle(g_L, c->id);
                SafePCall(g_L, 1, 0, "on_change");
            }
        }
        ImGui::PopID();
    }
}

void FireSimpleEvent(const char* ev_name) {
    if (!g_L) return;
    std::vector<int> refs;
    for (const auto& cb : g_callbacks)
        if (cb.event == ev_name) refs.push_back(cb.fn_ref);
    for (int ref : refs) {
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        SafePCall(g_L, 0, 0, ev_name);
    }
}

void PollCfgWatchers() {
    if (!g_L || g_cfg_watches.empty()) return;
    std::vector<int> ids;
    for (const auto& w : g_cfg_watches) ids.push_back(w.id);

    for (int id : ids) {
        CfgWatch* w = nullptr;
        for (auto& cw : g_cfg_watches) if (cw.id == id) { w = &cw; break; }
        if (!w || !w->field) continue;
        const CfgField* f = w->field;
        bool changed = false;
        if (f->type == CfgFieldType::Bool) {
            bool cur = *(bool*)f->ptr;
            if (cur != w->last_b) { changed = true; w->last_b = cur; }
        } else if (f->type == CfgFieldType::Float) {
            float cur = *(float*)f->ptr;
            if (fabsf(cur - w->last_f) > 1e-5f) { changed = true; w->last_f = cur; }
        } else if (f->type == CfgFieldType::Int) {
            int cur = *(int*)f->ptr;
            if (cur != w->last_i) { changed = true; w->last_i = cur; }
        } else if (f->type == CfgFieldType::Color4) {
            if (memcmp(f->ptr, w->last_c, sizeof(w->last_c)) != 0) {
                changed = true;
                memcpy(w->last_c, f->ptr, sizeof(w->last_c));
            }
        } else if (f->type == CfgFieldType::Str) {
            if (w->last_s != (const char*)f->ptr) {
                changed = true;
                w->last_s = (const char*)f->ptr;
            }
        }
        if (changed) {
            int ref = w->fn_ref;
            if (ref != LUA_NOREF) {
                lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
                PushCfgValue(g_L, f);
                SafePCall(g_L, 1, 0, "menu.on_change");
            }
            // Also fire global "config_change" hooks
            std::vector<int> grefs;
            for (const auto& cb : g_callbacks)
                if (cb.event == "config_change") grefs.push_back(cb.fn_ref);
            for (int gref : grefs) {
                lua_rawgeti(g_L, LUA_REGISTRYINDEX, gref);
                lua_pushstring(g_L, f->name);
                PushCfgValue(g_L, f);
                SafePCall(g_L, 2, 0, "config_change");
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public C++ entry points
// ---------------------------------------------------------------------------
void Lua_Init() {
    if (!g_cs_init) { InitializeCriticalSection(&g_lua_cs); g_cs_init = true; }
    LuaLock lock;
    if (g_lua_ready) return;

    InitLuaDir();
    CreateExampleScriptIfEmpty();

    g_L = luaL_newstate();
    if (!g_L) { Log("[lua] luaL_newstate failed"); return; }
    luaL_openlibs(g_L);
    RegisterLuaAPI(g_L);
    RefreshScriptList();
    g_lua_ready = true;
    Log("[lua] initialized Lua 5.4 runtime (dir=%s, %d scripts)", g_lua_dir, (int)g_scripts.size());
}

void Lua_Shutdown() {
    LuaLock lock;
    if (!g_lua_ready) return;
    g_callbacks.clear(); g_controls.clear(); g_ext_tabs.clear();
    g_windows.clear(); g_cfg_watches.clear();
    if (g_L) { lua_close(g_L); g_L = nullptr; }
    g_lua_ready = false;
}

void Lua_OnPreMenu() {
    if (!g_lua_ready || !g_L) return;
    LuaLock lock;
    FireSimpleEvent("pre_menu");
}

void Lua_OnPostMenu() {
    if (!g_lua_ready || !g_L) return;
    LuaLock lock;
    FireSimpleEvent("post_menu");
}

void Lua_RenderWindows() {
    if (!g_lua_ready || !g_L) return;
    LuaLock lock;

    PollCfgWatchers();

    // Render all managed ui.window(...) standalone windows
    std::vector<int> ids;
    for (const auto& w : g_windows) ids.push_back(w.id);

    for (int id : ids) {
        LuaWindow* w = FindWindow(id);
        if (!w || !w->open) continue;
        if (w->only_when_menu_open && !cfg.menu_open) continue;

        if (w->has_pos) {
            ImGui::SetNextWindowPos(ImVec2(w->x, w->y), w->pos_dirty ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
            w->pos_dirty = false;
        }
        if (w->has_size) {
            ImGui::SetNextWindowSize(ImVec2(w->w, w->h), w->size_dirty ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
            w->size_dirty = false;
        }

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
        if (w->no_titlebar) flags |= ImGuiWindowFlags_NoTitleBar;
        if (w->no_resize) flags |= ImGuiWindowFlags_NoResize;
        if (w->no_move) flags |= ImGuiWindowFlags_NoMove;
        if (w->no_background || w->blur) flags |= ImGuiWindowFlags_NoBackground;
        if (w->no_scrollbar) flags |= ImGuiWindowFlags_NoScrollbar;
        if (w->auto_resize) flags |= ImGuiWindowFlags_AlwaysAutoResize;
        if (w->no_inputs || (!cfg.menu_open && w->click_through_when_closed))
            flags |= ImGuiWindowFlags_NoInputs;

        int style_vars = 0, style_cols = 0;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, w->alpha); style_vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, w->rounding); style_vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, w->border_size); style_vars++;
        if (w->has_padding) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(w->pad_x, w->pad_y));
            style_vars++;
        }
        if (w->has_bg && !w->blur) {
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(w->bg[0], w->bg[1], w->bg[2], w->bg[3]));
            style_cols++;
        }
        if (w->has_border) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(w->border[0], w->border[1], w->border[2], w->border[3]));
            style_cols++;
        }
        if (w->has_text) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(w->text_col[0], w->text_col[1], w->text_col[2], w->text_col[3]));
            style_cols++;
        }
        if (w->has_title_bg) {
            ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(w->title_bg[0], w->title_bg[1], w->title_bg[2], w->title_bg[3]));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(w->title_bg[0], w->title_bg[1], w->title_bg[2], w->title_bg[3]));
            style_cols += 2;
        }

        if (ImGui::Begin(w->title.c_str(), nullptr, flags)) {
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            w->cur_x = wp.x; w->cur_y = wp.y; w->cur_w = ws.x; w->cur_h = ws.y;

            if (w->blur && !w->no_background) {
                ImVec2 wmax(wp.x + ws.x, wp.y + ws.y);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (alice::core && alice::core->blur)
                    alice::core->blur->apply(dl, wp, wmax, 14.0f, w->rounding, 2, 1.0f);
                ImU32 bg_col = w->has_bg
                    ? IM_COL32((int)(w->bg[0]*255), (int)(w->bg[1]*255), (int)(w->bg[2]*255), (int)(w->bg[3]*255))
                    : IM_COL32(13, 13, 18, 228);
                dl->AddRectFilled(wp, wmax, bg_col, w->rounding);
                ImU32 bdr_col = w->has_border
                    ? IM_COL32((int)(w->border[0]*255), (int)(w->border[1]*255), (int)(w->border[2]*255), (int)(w->border[3]*255))
                    : ImGui::GetColorU32(ImGuiCol_Border);
                if (w->border_size > 0.0f)
                    dl->AddRect(wp, wmax, bdr_col, w->rounding, 0, w->border_size);
            }

            if (w->render_fn_ref != LUA_NOREF) {
                int ref = w->render_fn_ref;
                std::string tname = w->title;
                lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
                SafePCall(g_L, 0, 0, tname.c_str());
            }
        }
        ImGui::End();

        if (style_cols > 0) ImGui::PopStyleColor(style_cols);
        if (style_vars > 0) ImGui::PopStyleVar(style_vars);
    }

    // Also fire immediate-mode "imgui" / "window" hooks (even when menu is closed)
    FireSimpleEvent("imgui");
    FireSimpleEvent("window");
}

void Lua_OnPaint(ImDrawList* draw, int width, int height) {
    if (!g_lua_ready || !g_L || !draw) return;
    LuaLock lock;
    g_cur_draw = draw; g_cur_w = width; g_cur_h = height;

    std::vector<int> refs;
    for (const auto& cb : g_callbacks)
        if (cb.event == "paint" || cb.event == "render") refs.push_back(cb.fn_ref);

    for (int ref : refs) {
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        lua_pushinteger(g_L, width);
        lua_pushinteger(g_L, height);
        SafePCall(g_L, 2, 0, "paint");
    }

    if (cfg.menu_open) FireSimpleEvent("menu");
    g_cur_draw = nullptr;
}

void Lua_OnPaintForeground(ImDrawList* draw, int width, int height) {
    if (!g_lua_ready || !g_L || !draw) return;
    LuaLock lock;
    std::vector<int> refs;
    for (const auto& cb : g_callbacks)
        if (cb.event == "render_fg") refs.push_back(cb.fn_ref);
    if (refs.empty()) return;

    g_cur_draw = draw; g_cur_w = width; g_cur_h = height;
    for (int ref : refs) {
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        lua_pushinteger(g_L, width);
        lua_pushinteger(g_L, height);
        SafePCall(g_L, 2, 0, "render_fg");
    }
    g_cur_draw = nullptr;
}

void Lua_OnPlayerESP(ImDrawList* draw, int index, const char* name,
                     int hp, int armor, int team, bool enemy, bool alive, bool dormant,
                     const Vector& origin, const ImVec2& box_min, const ImVec2& box_max, float alpha) {
    if (!g_lua_ready || !g_L || !draw) return;
    LuaLock lock;

    std::vector<int> refs;
    for (const auto& cb : g_callbacks)
        if (cb.event == "player_esp" || cb.event == "esp") refs.push_back(cb.fn_ref);
    if (refs.empty()) return;

    g_cur_draw = draw;
    g_cur_w = (int)ImGui::GetIO().DisplaySize.x;
    g_cur_h = (int)ImGui::GetIO().DisplaySize.y;

    for (int ref : refs) {
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        lua_createtable(g_L, 0, 9);
        lua_pushinteger(g_L, index); lua_setfield(g_L, -2, "index");
        lua_pushstring(g_L, name ? name : ""); lua_setfield(g_L, -2, "name");
        lua_pushinteger(g_L, hp); lua_setfield(g_L, -2, "hp");
        lua_pushinteger(g_L, armor); lua_setfield(g_L, -2, "armor");
        lua_pushinteger(g_L, team); lua_setfield(g_L, -2, "team");
        lua_pushboolean(g_L, enemy); lua_setfield(g_L, -2, "enemy");
        lua_pushboolean(g_L, alive); lua_setfield(g_L, -2, "alive");
        lua_pushboolean(g_L, dormant); lua_setfield(g_L, -2, "dormant");
        PushVec3(g_L, origin); lua_setfield(g_L, -2, "origin");

        lua_createtable(g_L, 0, 5);
        lua_pushnumber(g_L, box_min.x); lua_setfield(g_L, -2, "x");
        lua_pushnumber(g_L, box_min.y); lua_setfield(g_L, -2, "y");
        lua_pushnumber(g_L, box_max.x - box_min.x); lua_setfield(g_L, -2, "w");
        lua_pushnumber(g_L, box_max.y - box_min.y); lua_setfield(g_L, -2, "h");
        lua_pushnumber(g_L, alpha); lua_setfield(g_L, -2, "alpha");

        SafePCall(g_L, 2, 0, "player_esp");
    }
    g_cur_draw = nullptr;
}

void Lua_OnCreateMove(CUserCmd* cmd) {
    if (!g_lua_ready || !g_L || !cmd) return;
    LuaLock lock;
    std::vector<int> refs;
    for (const auto& cb : g_callbacks)
        if (cb.event == "create_move") refs.push_back(cb.fn_ref);
    if (refs.empty()) return;

    for (int ref : refs) {
        lua_createtable(g_L, 0, 7);
        lua_pushnumber(g_L, cmd->forwardmove); lua_setfield(g_L, -2, "forwardmove");
        lua_pushnumber(g_L, cmd->sidemove);    lua_setfield(g_L, -2, "sidemove");
        lua_pushinteger(g_L, cmd->buttons);    lua_setfield(g_L, -2, "buttons");
        lua_pushinteger(g_L, cmd->tick_count); lua_setfield(g_L, -2, "tick_count");
        lua_pushnumber(g_L, cmd->viewangles.x); lua_setfield(g_L, -2, "pitch");
        lua_pushnumber(g_L, cmd->viewangles.y); lua_setfield(g_L, -2, "yaw");
        lua_pushnumber(g_L, cmd->viewangles.z); lua_setfield(g_L, -2, "roll");

        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        lua_pushvalue(g_L, -2);
        SafePCall(g_L, 1, 0, "create_move");

        lua_getfield(g_L, -1, "forwardmove"); if (lua_isnumber(g_L, -1)) cmd->forwardmove = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "sidemove");    if (lua_isnumber(g_L, -1)) cmd->sidemove = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "buttons");     if (lua_isnumber(g_L, -1)) cmd->buttons = (int)lua_tointeger(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "tick_count");  if (lua_isnumber(g_L, -1)) cmd->tick_count = (int)lua_tointeger(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "pitch");       if (lua_isnumber(g_L, -1)) cmd->viewangles.x = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "yaw");         if (lua_isnumber(g_L, -1)) cmd->viewangles.y = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "roll");        if (lua_isnumber(g_L, -1)) cmd->viewangles.z = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_pop(g_L, 1);
    }
}

void Lua_OnOverrideView(CViewSetup* setup) {
    if (!g_lua_ready || !g_L || !setup) return;
    LuaLock lock;
    std::vector<int> refs;
    for (const auto& cb : g_callbacks)
        if (cb.event == "override_view") refs.push_back(cb.fn_ref);
    if (refs.empty()) return;

    for (int ref : refs) {
        lua_createtable(g_L, 0, 8);
        lua_pushnumber(g_L, setup->fov);           lua_setfield(g_L, -2, "fov");
        lua_pushnumber(g_L, setup->fov_viewmodel); lua_setfield(g_L, -2, "viewmodel_fov");
        lua_pushnumber(g_L, setup->origin.x);      lua_setfield(g_L, -2, "x");
        lua_pushnumber(g_L, setup->origin.y);      lua_setfield(g_L, -2, "y");
        lua_pushnumber(g_L, setup->origin.z);      lua_setfield(g_L, -2, "z");
        lua_pushnumber(g_L, setup->angles.x);      lua_setfield(g_L, -2, "pitch");
        lua_pushnumber(g_L, setup->angles.y);      lua_setfield(g_L, -2, "yaw");
        lua_pushnumber(g_L, setup->angles.z);      lua_setfield(g_L, -2, "roll");

        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        lua_pushvalue(g_L, -2);
        SafePCall(g_L, 1, 0, "override_view");

        lua_getfield(g_L, -1, "fov");           if (lua_isnumber(g_L, -1)) setup->fov = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "viewmodel_fov"); if (lua_isnumber(g_L, -1)) setup->fov_viewmodel = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "x");             if (lua_isnumber(g_L, -1)) setup->origin.x = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "y");             if (lua_isnumber(g_L, -1)) setup->origin.y = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "z");             if (lua_isnumber(g_L, -1)) setup->origin.z = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "pitch");         if (lua_isnumber(g_L, -1)) setup->angles.x = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "yaw");           if (lua_isnumber(g_L, -1)) setup->angles.y = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_getfield(g_L, -1, "roll");          if (lua_isnumber(g_L, -1)) setup->angles.z = (float)lua_tonumber(g_L, -1); lua_pop(g_L, 1);
        lua_pop(g_L, 1);
    }
}

void Lua_OnFrameStage(int stage) {
    if (!g_lua_ready || !g_L) return;
    LuaLock lock;
    std::vector<int> refs;
    for (const auto& cb : g_callbacks)
        if (cb.event == "frame_stage") refs.push_back(cb.fn_ref);
    for (int ref : refs) {
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        lua_pushinteger(g_L, stage);
        SafePCall(g_L, 1, 0, "frame_stage");
    }
}

void Lua_OnGameEvent(const char* name, int userid, int attacker, const Vector* impact_pos) {
    if (!g_lua_ready || !g_L || !name) return;
    LuaLock lock;
    std::vector<int> refs;
    for (const auto& cb : g_callbacks)
        if (cb.event == "game_event") refs.push_back(cb.fn_ref);
    if (refs.empty()) return;

    for (int ref : refs) {
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
        lua_pushstring(g_L, name);
        lua_createtable(g_L, 0, 5);
        lua_pushstring(g_L, name); lua_setfield(g_L, -2, "name");
        lua_pushinteger(g_L, userid); lua_setfield(g_L, -2, "userid");
        lua_pushinteger(g_L, attacker); lua_setfield(g_L, -2, "attacker");
        if (impact_pos) { PushVec3(g_L, *impact_pos); lua_setfield(g_L, -2, "pos"); }
        SafePCall(g_L, 2, 0, "game_event");
    }
}

void Lua_RenderTabExtras(const char* tab_name) {
    if (!g_lua_ready || !tab_name) return;
    LuaLock lock;
    std::string name(tab_name);
    bool has_any = false;
    for (const auto& c : g_controls)
        if (c.visible && c.tab == name) { has_any = true; break; }
    std::string ev = "tab:" + name;
    for (const auto& cb : g_callbacks)
        if (cb.event == ev) { has_any = true; break; }
    if (!has_any) return;

    ImGui::Spacing();
    ImGui::SeparatorText("Lua Extensions");
    RenderControlsForTab(name);
    FireSimpleEvent(ev.c_str());
}

void Lua_RenderExtensionTabs() {
    if (!g_lua_ready) return;
    LuaLock lock;
    std::vector<std::pair<std::string, int>> tabs;
    for (const auto& t : g_ext_tabs) tabs.push_back({ t.name, t.render_fn_ref });

    for (const auto& tp : tabs) {
        if (ImGui::BeginTabItem(tp.first.c_str())) {
            ImGui::BeginChild(("##luatab_" + tp.first).c_str(), ImVec2(0, 0), false);
            RenderControlsForTab(tp.first);
            if (tp.second != LUA_NOREF && g_L) {
                lua_rawgeti(g_L, LUA_REGISTRYINDEX, tp.second);
                SafePCall(g_L, 0, 0, tp.first.c_str());
            }
            FireSimpleEvent(("tab:" + tp.first).c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }
}

void Lua_RenderManagerTab() {
    if (!g_lua_ready) { ImGui::TextDisabled("Lua runtime not initialized."); return; }
    LuaLock lock;

    static int selected = -1;
    static char quick_code[1024] = "cheat.miss_log('[Lua] ', 'Hello from Lua 5.4!')";

    if (ImGui::BeginTable("##lua_cols", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Scripts");
        ImGui::TextDisabled("%s", g_lua_dir);

        if (ImGui::Button("Refresh")) RefreshScriptList();
        ImGui::SameLine();
        if (ImGui::Button("Unload all")) {
            for (auto& s : g_scripts) UnloadScript(s);
            CleanupScriptResources(-1);
        }

        ImGui::BeginChild("##lua_list", ImVec2(0, 210), true);
        for (int i = 0; i < (int)g_scripts.size(); i++) {
            auto& s = g_scripts[i];
            char label[160];
            snprintf(label, sizeof(label), "%s  %s##sc_%d",
                     s.name.c_str(), s.loaded ? "[loaded]" : "", i);
            if (ImGui::Selectable(label, selected == i)) selected = i;
        }
        ImGui::EndChild();

        if (selected >= 0 && selected < (int)g_scripts.size()) {
            auto& s = g_scripts[selected];
            if (!s.loaded) {
                if (ImGui::Button("Load", ImVec2(90, 0))) LoadScript(s);
            } else {
                if (ImGui::Button("Unload", ImVec2(90, 0))) UnloadScript(s);
                ImGui::SameLine();
                if (ImGui::Button("Reload", ImVec2(90, 0))) LoadScript(s);
            }
            if (!s.last_error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", s.last_error.c_str());
        }

        ImGui::TableNextColumn();
        ImGui::SeparatorText("Quick Run");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextMultiline("##lua_quick", quick_code, sizeof(quick_code), ImVec2(-FLT_MIN, 75));
        if (ImGui::Button("Execute")) {
            g_loading_script = -1;
            if (luaL_dostring(g_L, quick_code) != LUA_OK) {
                const char* err = lua_tostring(g_L, -1);
                LuaConsolePush(err ? err : "error", true);
                lua_pop(g_L, 1);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Console")) g_console.clear();

        ImGui::SeparatorText("Console");
        ImGui::BeginChild("##lua_console", ImVec2(0, 170), true);
        for (const auto& line : g_console) {
            if (line.is_error) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", line.text.c_str());
            else               ImGui::TextUnformatted(line.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::EndTable();
    }
}
