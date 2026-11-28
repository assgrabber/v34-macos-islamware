#pragma once
#include "sdk.h"
#include "imgui.h"

enum FeatureBindId { FB_INF_DUCK, FB_CROUCH_AIR, FB_QUICK_STOP, FB_THIRDPERSON, FB_LEFT_HAND, FB_REMOVE_BOB, FB_FULLBRIGHT, FB_NOFOG, FB_NOSKY, FB_CUSTOMFOG, FB_REM_POST, FB_REM_SHADOWS, FB_REM_DECALS, FB_REM_GRASS, FB_REM_ROPES, FB_REM_WATER, FB_REM_DYN, FB_REM_SHELLS, FB_REM_DEBRIS, FB_REM_PARTICLES, FB_ESP, FB_ESP_BOX, FB_ESP_NAME, FB_ESP_HEALTH, FB_ESP_ARMOR, FB_ESP_WEAPON, FB_ESP_AMMO, FB_ESP_FLAGS, FB_ESP_SKELETON, FB_ESP_OFFSCREEN, FB_ESP_AIMBOT, FB_ESP_BOMB, FB_CHAMS_HIT, FB_CHAMS_DISABLE_OCC, FB_FAKE_DUCK, FB_COUNT };

struct Config {
    bool menu_open = false;

    // esp
    bool esp = true;
    bool esp_box = true;
    bool esp_health = true;
    bool esp_name = true;
    bool esp_distance = false;
    bool esp_team_check = false;
    bool esp_skeleton = false;
    float esp_smooth = 0.0f;
    bool esp_dormant_fade = true;
    float esp_dormant_fade_time = 2.0f;
    bool esp_show_dormant = true;
    float esp_dormant_alpha = 1.0f;
    float esp_dormant_grayscale = 1.0f;

    // esp colours
    float esp_box_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float esp_box_outline[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float esp_name_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool esp_health_gradient = true;
    float esp_health_min[4] = { 1.0f, 0.2f, 0.2f, 1.0f };
    float esp_health_max[4] = { 0.2f, 1.0f, 0.2f, 1.0f };

    // weapon / ammo / flags
    bool esp_weapon = false;
    float esp_weapon_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool esp_ammo = false;
    bool esp_weapon_icon = false;
    float esp_weapon_icon_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool esp_kit_icon = false;
    float esp_kit_icon_color[4] = { 0.45f, 0.8f, 1.0f, 1.0f };
    bool esp_c4_icon = false;
    float esp_c4_icon_color[4] = { 1.0f, 0.45f, 0.35f, 1.0f };
    bool esp_taser_icon = false;
    float esp_taser_icon_color[4] = { 0.7f, 0.8f, 1.0f, 1.0f };
    bool esp_bomb = false;
    bool esp_armor = false;
    float esp_armor_color[4] = { 0.45f, 0.7f, 1.0f, 1.0f };
    bool esp_armor_gradient = true;
    float esp_armor_min[4] = { 0.2f, 0.55f, 1.0f, 1.0f };
    float esp_armor_max[4] = { 0.05f, 0.15f, 0.45f, 1.0f };
    bool esp_ammo_gradient = true;
    float esp_ammo_min[4] = { 1.0f, 0.4f, 0.2f, 1.0f };
    float esp_ammo_max[4] = { 0.4f, 0.8f, 1.0f, 1.0f };
    bool esp_flags = false;
    float esp_flags_color[4] = { 0.9f, 0.9f, 0.95f, 1.0f };
    bool flag_money = true;
    float flag_money_color[4] = { 0.45f, 1.0f, 0.45f, 1.0f };
    bool flag_armor = true;
    float flag_armor_color[4] = { 0.55f, 0.75f, 1.0f, 1.0f };
    bool flag_kit = true;
    float flag_kit_color[4] = { 0.6f, 0.85f, 1.0f, 1.0f };
    bool flag_defuse = true;
    float flag_defuse_color[4] = { 0.4f, 0.7f, 1.0f, 1.0f };
    bool flag_flashed = true;
    float flag_flashed_color[4] = { 1.0f, 1.0f, 0.4f, 1.0f };
    bool flag_c4 = true;
    float flag_c4_color[4] = { 1.0f, 0.35f, 0.3f, 1.0f };

    // offscreen arrows
    bool esp_offscreen = false;
    float esp_offscreen_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float esp_offscreen_radius = 80.0f;
    float esp_offscreen_size = 1.5f;

    // aimbot visuals
    bool esp_aimbot_visual = false;
    float esp_aimbot_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool esp_aimbot_safe = false;
    float esp_aimbot_safe_color[4] = { 1.0f, 0.6f, 0.2f, 1.0f };

    // chams
    struct ChamsLayer {
        bool enabled = false;
        int material = 0;
        float color[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
    };
    struct ChamsTarget {
        ChamsLayer occluded;
        ChamsLayer visible;
        ChamsLayer overlay;
    };
    ChamsTarget chams_enemy;
    ChamsTarget chams_team;
    ChamsTarget chams_local;
    ChamsTarget chams_viewmodel;
    ChamsTarget chams_weapons;   // dropped / world weapons

    bool chams_hit = false;
    int chams_hit_material = 6;
    float chams_hit_color[4] = { 1.0f, 0.2f, 0.2f, 1.0f };
    float chams_hit_duration = 1.0f;
    bool chams_disable_occlusion = true;

    // aimbot
    bool aimbot = false;
    int aim_key = 2;          // 0 always, 1 mouse4, 2 mouse5, 3 alt
    float aim_fov = 12.0f;
    float aim_smooth = 2.0f;

    // silent aim
    bool silentaim = false;
    float silent_fov = 30.0f;  // 0 = no angular limit
    bool silent_visible = true;
    int silent_hitbox = 1;     // legacy single choice
    bool silent_hb_head = true;
    bool silent_hb_neck = true;
    bool silent_hb_chest = true;
    bool silent_hb_stomach = false;
    bool silent_hb_pelvis = false;
    int silent_select = 0;     // 0 = crosshair, 1 = distance, 2 = health
    bool auto_wall = false;
    float auto_wall_thickness = 35.0f;
    bool silent_multipoint = false;
    float min_damage = 0.0f;
    float hit_chance = 100.0f;
    int delay_shot = 0;
    bool auto_stop = false;
    bool auto_scope = false;
    bool log_misses = false;

    struct WeaponOverride {
        bool enabled = false;
        float silent_fov = 30.0f;
        bool hb[5] = { true, true, true, false, false };
        float trigger_fov = 1.2f;
        float min_damage = 0.0f;
        float hit_chance = 100.0f;
        int delay_shot = 0;
        bool auto_stop = false;
        bool auto_scope = false;
    };
    WeaponOverride weapon_override[7]; // rifle, sniper, pistol, smg, shotgun, mg, knife

    // triggerbot
    bool triggerbot = false;
    float trigger_fov = 1.2f;
    // knifebot: auto-swing when an enemy is within reach (and it also stops
    // the normal trigger from shooting a knife at long range)
    bool knifebot = true;
    float knife_range = 75.0f;

    // keybinds (VK code, 0 = unbound/always)
    int kb_menu = 0x27; // VK_RIGHT
    int kb_silentaim = 0;
    int kb_aimbot = 0;
    int kb_triggerbot = 0;
    int kb_bhop = 0;
    int kb_autostrafe = 0;

    // 0 = hold while key is down, 1 = toggle on key press
    int kb_mode_silentaim = 0;
    int kb_mode_aimbot = 0;
    int kb_mode_triggerbot = 0;
    int kb_mode_bhop = 0;
    int kb_mode_autostrafe = 0;
    int kb_hitchams = 0;
    int kb_mode_hitchams = 0;
    int kb_feature[FB_COUNT] = {};
    int kb_feature_mode[FB_COUNT] = {};

    // anti-aim builder
    bool aa_enabled = false;
    int aa_pitch = 0;         // 0 none, 1 down, 2 up
    int aa_base = 0;          // 0 local view, 1 at target
    int aa_yaw = 1;           // 0 forward, 1 backward
    float aa_yaw_offset = 0.0f;
    bool aa_sides = false;
    float aa_left = 30.0f;
    float aa_right = 30.0f;
    int aa_side_delay = 1;
    int aa_modifier = 0;          // 0 none, 1 center, 2 offset, 3 random, 4 spin, 5 three-way, 6 five-way
    float aa_modifier_offset = 30.0f;
    float aa_roll = 0.0f;
    int aa_manual_left = 0;   // VK held -> fake body faces left
    int aa_manual_right = 0;  // VK held -> fake body faces right

    // movement / camera
    bool thirdperson = false;
    float thirdperson_dist = 150.0f;
    bool bhop = false;
    bool autostrafe = true;
    bool strafe_follow_keys = false; // false = steer toward the camera

    // world
    bool world_fullbright = false;
    bool world_nofog = false;
    bool world_nosky = false;
    bool world_custom_fog = false;
    float fog_start = 0.0f;
    float fog_end = 3000.0f;
    float fog_density = 1.0f;
    float fog_color[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    float world_gamma = 2.2f;
    float world_ambient = 0.0f;
    float world_brightness = 0.0f;
    float world_night = 0.0f;
    float world_model_ambient = 0.0f;
    float world_bloom = 1.0f;

    // removals
    bool rem_postprocess = false;
    bool rem_shadows = false;
    bool rem_decals = false;
    bool rem_grass = false;
    bool rem_ropes = false;
    bool rem_water = false;
    bool rem_dynlights = false;
    bool rem_shells = false;
    bool rem_debris = false;
    bool rem_blood = false;
    bool rem_particles = false;

    // view
    float fov_override = 0.0f;      // 0 = game default
    float viewmodel_fov = 0.0f;     // 0 = game default
    float viewmodel_x = 0.0f;
    float viewmodel_y = 0.0f;
    float viewmodel_z = 0.0f;
    bool left_hand = false;
    bool remove_bob = false;

    // movement extras
    bool inf_duck = false;
    bool crouch_air = false;
    bool fake_duck = false;
    bool quick_stop = false;
    float fastwalk = 1.0f;

    // custom player model
    char model_path[160] = {};
    int model_index = 0;

    // overlays
    bool watermark = true;
    bool draw_aim_fov = false;
    float log_misses_color[4] = { 0.26f, 0.59f, 0.98f, 1.0f };

    // world colour + kill effects (x64)
    bool world_props = false;
    float prop_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool world_walls = false;
    float wall_color[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    bool kill_effects = false;
    int  kill_effect_type = 0;          // 0 Tesla, 1 Explosion, 2 Blood, 3 Gauss
    float kill_effect_color[4] = { 0.35f, 0.65f, 1.0f, 1.0f };
    float kill_effect_scale = 1.0f;
    float kill_effect_time = 1.5f;

    // bullet impact markers: client-side prediction (red) and server-confirmed
    // event positions (blue), plus tracers
    bool bullet_impacts = false;
    float impact_color[4] = { 1.0f, 0.35f, 0.35f, 1.0f };
    bool impacts_server = false;
    float impact_server_color[4] = { 0.35f, 0.6f, 1.0f, 1.0f };
    float impact_size = 2.0f;
    float impact_time = 2.0f;
    bool tracers = false;
    float tracer_local_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float tracer_enemy_color[4] = { 1.0f, 0.4f, 0.4f, 1.0f };
    float tracer_team_color[4] = { 0.4f, 0.7f, 1.0f, 1.0f };
    float tracer_life = 2.5f;

    // grenades (prediction + projectile markers)
    bool nade_predict = true;
    bool nade_esp = true;
    float nade_color_he[4] = { 0.95f, 0.35f, 0.20f, 1.0f };
    float nade_color_flash[4] = { 1.0f, 0.90f, 0.30f, 1.0f };
    float nade_color_smoke[4] = { 0.40f, 0.75f, 1.0f, 1.0f };

    // movement exploits
    bool x_duckpeek = false;   int kb_duckpeek = 0;   int kb_mode_duckpeek = 0;
    bool x_silentrun = false;  int kb_silentrun = 0;  int kb_mode_silentrun = 0;
    bool x_jumpbug = false;    int kb_jumpbug = 0;    int kb_mode_jumpbug = 0;
    bool x_fastladder = false; int kb_fastladder = 0; int kb_mode_fastladder = 0;
    bool x_qcancel = false;    int kb_qcancel = 0;    int kb_mode_qcancel = 0;

    // menu
    bool menu_imgui = true;   // ImGui menu (false = alicekit backup menu)

    // server browser fix (append new fields at the end — config files are raw dumps)
    bool browser_fix = true;
    bool connect_fix = true;
    bool spoof_version = true;
    char reported_build[16] = "11003710";
    bool allow_internet = true;

    // ESP layout (serialized "Name:side:order;..." arrangement)
    char esp_layout[512] = {};
    // hotkey list island (left side of the screen while the menu is open)
    bool hotkey_list = true;
    float hotkey_x = 24.0f;
    float hotkey_y = 140.0f;
    // bomb ESP preview next to the hotkey list while the menu is open
    bool bomb_preview = true;

    // ragebot
    bool double_tap = false;

    // fake model: draw the local player model in first person (what others see)
    bool fake_model = false;

    // hotkey window options
    bool hotkey_show_mode = false;
    bool hotkey_show_bind = true;

    // comma-separated player names ignored by aim and trigger
    char whitelist[512] = {};

    // backtrack (lag compensation): aim at recorded historical positions
    bool backtrack = false;
    float backtrack_time = 0.2f;
    bool backtrack_skeleton = true;
    bool backtrack_chams = false;
    float bt_chams_color[4] = { 0.35f, 0.6f, 1.0f, 1.0f };
    // miss-count resolver: shifts the aim point sideways after misses
    bool resolver = false;

    // reserved: former movement-recorder bindings (kept so the raw config
    // layout stays compatible; the feature and its UI are gone)
    int kb_macro_record = 0;
    int kb_macro_play = 0;

    // fakelag: hold back outgoing movement updates (practical limit 17 =
    // sv_maxusrcmdprocessticks 16 + 1)
    bool fakelag = false;
    int fakelag_ticks = 8;

    // seed-based spread compensation
    bool nospread = false;

    // weapon chams for held weapons of other players
    bool wchams_enemy = false;
    bool wchams_team = false;

    // dropped weapon ESP
    bool dropped_weapons = false;
    bool dropped_name = true;
    bool dropped_icon = true;
    bool dropped_ammo_text = true;
    bool dropped_ammo_bar = true;
    float dropped_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // weapon chams on the local first-person viewmodel (arms are part of the
    // same v_ model, so this also tints the hands - off unless explicitly on)

    // tracer gradient: colour at the muzzle -> colour at the impact point
    float tracer_local_color2[4] = { 0.45f, 0.75f, 1.0f, 1.0f };
    float tracer_enemy_color2[4] = { 1.0f, 0.75f, 0.3f, 1.0f };
    float tracer_team_color2[4] = { 0.35f, 1.0f, 0.75f, 1.0f };

    // body aim (Claret-style): force a body hitbox under a condition instead of
    // the head.  0 = off, 1 = always, 2 = lethal (target hp <= 50),
    // 3 = while moving, 4 = in air, 5 = slow walk
    int body_aim = 0;
    int body_aim_hitbox = 2;   // 2 = chest, 3 = stomach, 4 = pelvis

    // deprecated: first-person weapon chams are merged into chams_weapons
    // (kept in Config to preserve binary config layout)
    bool wchams_firstperson = false;

    // Claret-style body aim options & disablers (appended at end to preserve
    // raw binary config compatibility)
    bool baim_force = false;
    int kb_baim = 0;
    int kb_mode_baim = 0;
    bool baim_on_peek = false;
    bool baim_lethal = false;
    bool baim_in_air = false;
    bool baim_moving = false;
    bool baim_dis_resolved = false;
    bool baim_dis_shooting = false;
    bool baim_dis_low_dmg = false;
    bool baim_dis_head_safe = false;

    // Peek assist (Quick Peek)
    bool peek_assist = false;
    int kb_peek_assist = 0;
    int kb_mode_peek_assist = 0;
    bool peek_retreat_on_shot = true;
    bool peek_retreat_on_release = true;
    int peek_max_distance = 0; // 0 = infinite, 1..200 units
    float peek_assist_color[4] = { 1.0f, 0.25f, 0.25f, 1.0f };

    // Scope override (killed/claret style)
    bool rem_scope = false;
    bool no_scope_zoom = false;
    bool custom_scope = false;
    float custom_scope_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int scope_gap = 10;
    int scope_length = 100;
    bool scope_invert = false;

    // Bomb ESP wide/compact state & saved position for out-of-FOV wide card
    bool bomb_wide_preview = false;
    float bomb_x = 24.0f;
    float bomb_y = 260.0f;

    // Super Toss: compensates grenade throw angle for player velocity so it flies straight
    bool super_toss = false;

    // Penetration crosshair (killed/claret style 3D/2D surface box indicator)
    bool pen_crosshair = false;
    int pen_crosshair_style = 0; // 0 = 3D Box, 1 = 2D Surface Box
    bool pen_crosshair_dot = true;
    float pen_crosshair_yes_color[4] = { 0.25f, 0.95f, 0.35f, 0.90f };
    float pen_crosshair_no_color[4]  = { 0.95f, 0.25f, 0.25f, 0.90f };

    // Fake model chams (translucent local ghost model showing fake angle in thirdperson)
    bool chams_fake = false;
    float chams_fake_color[4] = { 0.16f, 0.45f, 1.0f, 0.55f };

    // CS:S v34 Cheat Revealer & Spoofer (friendsName / SendClientInfo)
    bool cheat_revealer = true;
    bool esp_cheat_tag = true;
    int cheat_spoofer = 1; // 0=Off, 1=Islamware, 2=Epoximotion, 3=Poptape, 4=Claret, 5=Killed, 6=Neverlose, 7=Skeet, 8=Insomnia
};

extern Config cfg;
extern IVEngineClient engine;
extern IVRenderView render_view;
extern IClientEntityList entity_list;
extern IVDebugOverlay debug_overlay;
extern ICvar cvar;
extern void* base_client;
extern IEngineTrace engine_trace;
extern IPhysicsSurfaceProps physics_props;
extern IMaterialSystem material_system;
extern IModelRender model_render;
extern IVModelInfo model_info;
extern IMDLCache mdl_cache;
extern void* studio_render;
extern HMODULE client_module;
extern HMODULE engine_module;
extern HWND game_window;

// render camera, captured from ClientMode::OverrideView
extern Vector view_origin;
extern QAngle view_angles;
extern float view_fov;
extern float view_aspect;
extern bool view_valid;
extern QAngle real_view_angles;

void Log(const char* fmt, ...);
bool StartServerListRefresh(const char* reason);
void ServerListStatus(char* buf, int buf_size);
void KillFx_Spawn(const Vector& pos, const Vector& dir, int victim_index);
void KillFx_Draw(ImDrawList* draw, int width, int height);
void KillFx_Clear();
void WorldFx_ApplyWalls();
void WorldFx_Install();

void Impacts_OnServerImpact(const Vector& pos, int shooter_index);
void Impacts_TrackShot();
bool Impacts_ConsumeFired();
void Impacts_RecordShot(CUserCmd* cmd, void* local);
void Impacts_Probe();
void Tracers_Draw(ImDrawList* draw, int width, int height);
void Grenades_Draw(ImDrawList* draw, int width, int height);
bool Grenades_GetHeldThrowVelocity(void* local, const QAngle& view_ang, Vector& out_vel, float& out_speed);
void InstallGameEvents();
void Impacts_Clear();

// liveness counters written from the game thread, read by the stall watchdog
extern volatile LONG g_wd_frame_tick;
extern volatile LONG g_wd_cmd_tick;

void StartConnectFix();
void SetConnectFix(bool enable);
void ConnectFixStatus(char* buf, int buf_size);
void ApplyVersionSpoof();
void ApplyEmuNetworkFix(bool enable);
void RunNetTest();
void SetVersionSpoof(bool enable);
bool* FeatureBindTarget(int id);
void ApplyFeatureBinds();
bool IsGameReady();
void DrawESP(ImDrawList* draw, int width, int height);
void Bones_OnFrameStage();
void PeekAssist_Draw(ImDrawList* draw, int width, int height);
bool PeekAssist_GetCircle(Vector& out_center);
void ImguiMenuRender();   // ImGui menu (imgui_menu.cpp)

bool WorldToScreen(const Vector& world, ImVec2& out, int width, int height);

// backtrack / lag compensation records (features.cpp)
void Backtrack_Record(int index, void* entity);
Vector Backtrack_Pick(int index, float seconds_ago, float eye_z);
Vector Backtrack_PickRaw(int index, float seconds_ago);
bool Backtrack_PickRecord(int index, float seconds_ago, Vector& out_origin, float& out_simtime);
void Resolver_OnMiss(int index);
void Resolver_OnHit(int index);
int Resolver_Side(int index);
int Resolver_Misses(int index);
bool Resolver_IsResolved(int index);
void Resolver_ApplyFrameStage();
bool TargetRecentlyShot(int index, ULONGLONG window_ms = 250);

// bind state query for the hotkey window (hooks.cpp)
bool BindIsEngaged(int vk, int mode);

// ESP layout engine (esp_layout.cpp)
namespace esp_layout {
    struct PlayerView;
    void RenderStatic(ImDrawList* dl, const ImVec2& box_min, const ImVec2& box_max, float alpha, PlayerView& view);
    void RenderPreview(ImDrawList* dl, const ImVec2& box_min, const ImVec2& box_max, float alpha, PlayerView& view);
    void SerializeToConfig();
    void ReloadFromConfig();
}
// menu islands: hotkey list + bomb ESP preview (imgui_menu.cpp)
void ImguiMenuIslands(ImDrawList* draw, int width, int height);

// embedded GLB player model preview for the ESP preview pane (glb_preview.cpp)
namespace glb_preview {
    void Draw(ImDrawList* dl, ImVec2 center, float size);
}
// custom miss feed
void MissLog_Push(char const* prefix, char const* rest);
void MissLog_OnLocalImpact(Vector const& pos);
void MissLog_OnLocalHurt(int victim_index);
void MissLog_Tick();
void GetLocalEye(Vector& eye, QAngle& angles);
CUserCmd* GetUserCmd(int sequence);
void* GetLocalEntity();
bool IsPlayerIndex(int index);
Vector PlayerEye(void* ent);
bool EntityIsStale(void* ent, int index);
bool TraceLine(const Vector& from, const Vector& to, float& fraction, Vector& end,
               void* skip1 = nullptr, void* skip2 = nullptr,
               void** out_entity = nullptr, const char** out_surface = nullptr,
               Vector* out_normal = nullptr, unsigned int mask = 0x46004003u,
               short* out_surface_props = nullptr, int* out_contents = nullptr,
               bool* out_startsolid = nullptr);
bool CanPenetrate(const Vector& from, const Vector& to, float max_thickness, void* skip = nullptr);
bool CanPenetrateTarget(const Vector& from, const Vector& to, void* target, int hitbox,
                        float min_damage, float* out_damage = nullptr);
bool PenetrationCrosshairCheck(const Vector& from, const QAngle& angles,
                               Vector& out_hit, Vector& out_normal, float& out_damage);
const char* CheatRevealer_Identify(const char* friends_name, const char* player_name,
                                   uint32_t* out_rgba = nullptr, const char** out_short_tag = nullptr);
float CalcHitDamage(float base_damage, float armor_ratio, float range_modifier,
                    float distance, int hitbox, void* target);
bool GetActiveWeaponDamageInfo(int& out_damage, float& out_armor_ratio, float& out_range_modifier);
bool TraceVisible(const Vector& from, const Vector& to, void* skip = nullptr);
bool GetEntityBonePos(int index, int bone, Vector& out);
bool GetEntityHeadPos(int index, int bone, Vector& out);

// client-side custom player model changer (precache.cpp)
int CustomModels_Count(bool rescan = false);
const char* CustomModels_At(int index);
const char* CustomModels_Status();
void ClientModel_OnFrameStage(int stage);
bool ClientModel_ShouldOverride(void* entity, int& out_index);
bool GetEntityAimBones(void* entity, int out[5]);

// CS:S v34 anti-shellcode / anti-RCE protections (shellcode_fix.cpp)
void InstallShellcodeFixes();

// Lua 5.4 scripting API (lua_api.cpp)
void Lua_Init();
void Lua_Shutdown();
void Lua_OnPaint(ImDrawList* draw, int width, int height);
void Lua_OnPaintForeground(ImDrawList* draw, int width, int height);
void Lua_RenderWindows();
void Lua_OnPreMenu();
void Lua_OnPostMenu();
void Lua_OnPlayerESP(ImDrawList* draw, int index, const char* name,
                     int hp, int armor, int team, bool enemy, bool alive, bool dormant,
                     const Vector& origin, const ImVec2& box_min, const ImVec2& box_max, float alpha);
void Lua_OnCreateMove(CUserCmd* cmd);
void Lua_OnOverrideView(CViewSetup* setup);
void Lua_OnFrameStage(int stage);
void Lua_OnGameEvent(const char* name, int userid, int attacker, const Vector* impact_pos);
void Lua_RenderManagerTab();
void Lua_RenderExtensionTabs();
void Lua_RenderTabExtras(const char* tab_name);
