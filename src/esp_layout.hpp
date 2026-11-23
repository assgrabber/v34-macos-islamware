#pragma once
#include "imgui.h"
#include "imgui_internal.h"

#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The reference ESP layout engine (port of the reference
// game/features/esp/players/layout).  Elements are slots on the four sides of
// the player box; in the menu preview they can be dragged between sides and
// reordered, the arrangement is serialized into cfg.esp_layout and applied to
// the in-game ESP through RenderStatic.
// ---------------------------------------------------------------------------

namespace esp_layout {

enum Side { SideTop = 0, SideBottom, SideLeft, SideRight };
enum Group { GroupBar, GroupText, GroupBox };
enum Align { AlignStart, AlignCenter, AlignEnd };

using MeasureFn = ImVec2 (*)(const ImRect& bounds, Side side, float alpha, void* user, float scale);
using RenderFn = void (*)(ImDrawList* dl, const ImRect& bounds, Side side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale);

struct Slot {
    int id = 0;
    std::string name;
    Side side = SideTop;
    Group group = GroupBar;
    int order = 0;
    Align align = AlignCenter;

    MeasureFn measure = nullptr;
    RenderFn render = nullptr;

    bool enabled = true;
    bool draggable = true;
    bool* enabled_ref = nullptr;

    float scale = 1.0f;
    ImVec2 size{ 0.f, 0.f };
    ImVec2 target_pos{ 0.f, 0.f };

    bool anim_initialized = false;
    ImVec2 anim_pos{ 0.f, 0.f };
    float vis_alpha = 1.f;
    float hover_alpha = 0.f;

    bool isOn() const { return enabled_ref ? *enabled_ref : enabled; }
};

// per-player data the slots render (filled by the in-game ESP loop and by the
// menu preview with fake values)
struct PlayerView {
    bool alive = true;
    const char* name = nullptr;
    float health = 0.f;   // 0..1
    int armor = 0;        // 0..100
    bool has_weapon = false;
    const char* weapon = nullptr;
    bool has_ammo = false;
    char ammo_text[32] = {};
    char distance_text[32] = {};
    bool kit_icon = false;
    bool c4_icon = false;
    bool taser_icon = false;
    const char* flag_money = nullptr;
    const char* flag_armor = nullptr;
    const char* flag_kit = nullptr;
    const char* flag_defuse = nullptr;
    const char* flag_flashed = nullptr;
    const char* flag_c4 = nullptr;
    // dormant grayscale, same meaning as DrawESP's gray/gray_amt
    bool gray = false;
    float gray_amt = 0.f;
};

class Layout {
    std::vector<Slot> slots;
    std::vector<std::vector<int>> bars_by_side;
    std::vector<std::vector<int>> texts_by_side;

    int dragging_id = -1;
    ImVec2 drag_grab_offset{ 0.f, 0.f };
    ImVec2 drag_prev_size{ 0.f, 0.f };
    int resizing_id = -1;
    ImVec2 resize_start_mouse{ 0.f, 0.f };
    float resize_start_scale = 1.f;
    int hovered_id = -1;

    ImVec2 last_bounds_origin{ 0.f, 0.f };
    bool has_last_bounds = false;

    mutable std::vector<ImVec2> scratch_sizes;
    mutable std::vector<ImVec2> scratch_positions;

    ImVec2 floorPos(ImVec2 v) const;
    Slot* findSlot(int id);
    int addSlot(Slot slot);
    void rebuildOrder();
    void moveSlot(int slot_id, Side target_side, int target_index_in_group);
    ImVec2 computeCursor(Side side, const ImRect& bounds, float accumulated, ImVec2 slot_size, Align align) const;
    void hitTestDrop(const ImRect& bounds, ImVec2 mouse, Group drag_group, Side& out_side, int& out_index);
    void computeLayout(const ImRect& bounds, float alpha, void* user, bool animated) const;

public:
    Layout() : bars_by_side(4), texts_by_side(4) {}

    int addBar(std::string name, Side side, int order, MeasureFn measure, RenderFn render);
    int addText(std::string name, Side side, int order, MeasureFn measure, RenderFn render, Align align = AlignCenter);
    int addBox(std::string name, RenderFn render);
    void bindEnabled(int slot_id, bool* ext);
    void reset();

    void render(ImDrawList* dl, const ImRect& bounds, float alpha, void* user);
    void renderStatic(ImDrawList* dl, const ImRect& bounds, float alpha, void* user) const;

    bool empty() const { return slots.empty(); }
    std::string serialize() const;
    void deserialize(const char* data);
};

// builds the slot set (bound to cfg fields) on first use
void Init();
Layout& Get();
// writes the current arrangement into cfg.esp_layout
void SerializeToConfig();
// reloads arrangement/scales from cfg.esp_layout
void ReloadFromConfig();
// in-game render: static placement, no drag/hover logic
void RenderStatic(ImDrawList* dl, const ImVec2& box_min, const ImVec2& box_max, float alpha, PlayerView& view);
// menu preview: animated positions + drag & drop
void RenderPreview(ImDrawList* dl, const ImVec2& box_min, const ImVec2& box_max, float alpha, PlayerView& view);

} // namespace esp_layout
