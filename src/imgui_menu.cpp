#include "cheat.h"
#include "alice_ui.h"
#include "alice_kit.hpp"
#include "fonts/fonts.hpp"
#include "esp_layout.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Custom ImGui menu (ported): one bordered window,
// a tab bar, two-column pages, SeparatorText groups, checkbox/color rows.
// Default ImGui dark colors (no accent override).  cfg.menu_imgui selects
// this menu; false falls back to the alicekit backup menu.
// ---------------------------------------------------------------------------

namespace imgui_menu {

static bool  g_style_done = false;
static bool  g_cfg_ready = false;
static int   g_cfg_sel = -1;
static char  g_cfg_name[64] = "default";
static int*  g_kb_capture = nullptr;   // bind being captured

static void ApplyStyle() {
    ImGui::StyleColorsDark(nullptr);
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.f;
    st.ChildRounding = 4.f;
    st.FrameRounding = 3.f;
    st.GrabRounding = 3.f;
    st.TabRounding = 3.f;
    st.WindowBorderSize = 1.f;
    st.FramePadding = ImVec2(6.f, 4.f);
    st.ItemSpacing = ImVec2(8.f, 6.f);

    // neutral surface colors from the the reference source source; the accent-colored
    // entries (checkmark, slider grab, button/header active, tabs) keep the
    // default ImGui dark palette
    auto* colors = st.Colors;
    colors[ImGuiCol_WindowBg]     = ImVec4(0.07f, 0.07f, 0.09f, 0.97f);
    colors[ImGuiCol_ChildBg]      = ImVec4(0.10f, 0.10f, 0.12f, 1.f);
    colors[ImGuiCol_Border]       = ImVec4(0.18f, 0.18f, 0.22f, 1.f);
    colors[ImGuiCol_FrameBg]      = ImVec4(0.14f, 0.14f, 0.17f, 1.f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.22f, 1.f);
    colors[ImGuiCol_FrameBgActive]  = ImVec4(0.22f, 0.22f, 0.26f, 1.f);
    colors[ImGuiCol_TitleBg]      = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgCollapsed] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_Button]       = colors[ImGuiCol_FrameBg];
    colors[ImGuiCol_ButtonHovered] = colors[ImGuiCol_FrameBgHovered];
    colors[ImGuiCol_Header]       = colors[ImGuiCol_FrameBg];
    colors[ImGuiCol_HeaderHovered] = colors[ImGuiCol_FrameBgHovered];
    colors[ImGuiCol_Tab]          = colors[ImGuiCol_FrameBg];
    colors[ImGuiCol_ScrollbarBg]  = ImVec4(0.07f, 0.07f, 0.09f, 1.f);
    colors[ImGuiCol_Separator]    = colors[ImGuiCol_Border];
    colors[ImGuiCol_PopupBg]      = ImVec4(0.08f, 0.08f, 0.10f, 0.98f);
}

static const char* vk_name(int vk) {
    static char buf[16];
    if (vk <= 0) return "None";
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    switch (vk) {
        case VK_LBUTTON: return "Mouse1";
        case VK_RBUTTON: return "Mouse2";
        case VK_MBUTTON: return "Mouse3";
        case VK_XBUTTON1: return "Mouse4";
        case VK_XBUTTON2: return "Mouse5";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_SHIFT: return "Shift";
        case VK_CONTROL: return "Ctrl";
        case VK_MENU: return "Alt";
        case VK_TAB: return "Tab";
        case VK_CAPITAL: return "Caps";
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_BACK: return "Backspace";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
        return buf;
    }
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

static const char* const kBindModes[2]    = { "Hold", "Toggle" };

static void TextLabel(const char* label) {
    // labels may carry a "##id" suffix for unique widget ids - hide it
    const char* cut = strstr(label, "##");
    if (cut)
        ImGui::TextUnformatted(label, cut);
    else
        ImGui::TextUnformatted(label);
}

struct BindRef {
    const char* label;
    const char* group;
    int* vk;
    int* mode;
};
static std::vector<BindRef> g_bind_registry;
static int* g_popup_capture = nullptr;
static bool g_all_hotkeys_open = false;

static void BindPopup(const char* label, const char* group, int* vk, int* mode) {
    bool found = false;
    for (auto const& b : g_bind_registry)
        if (b.vk == vk) { found = true; break; }
    if (!found)
        g_bind_registry.push_back({ label, group, vk, mode });

    // right-click on the previous widget opens the custom bind window
    ImGui::PushID(label);
    if (!ImGui::BeginPopupContextItem("##bind_popup")) {
        ImGui::PopID();
        return;
    }
    TextLabel(label);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", group);
    ImGui::Separator();

    if (g_popup_capture == vk) {
        ImGui::Button("...", {78.f, 0.f});
        for (int k = 1; k < 255; k++) {
            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
            if (GetAsyncKeyState(k) & 0x8000) {
                *vk = (k == VK_ESCAPE) ? 0 : k;
                g_popup_capture = nullptr;
                break;
            }
        }
    } else {
        char keylabel[48];
        snprintf(keylabel, sizeof(keylabel), "%s##key", vk_name(*vk));
        if (ImGui::Button(keylabel, {78.f, 0.f}))
            g_popup_capture = vk;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click and press a key, Escape to clear");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(74.f);
    ImGui::Combo("##mode", mode, kBindModes, 2);
    ImGui::Separator();
    if (ImGui::Button("Remove hotkey"))
        *vk = 0;
    ImGui::EndPopup();
    ImGui::PopID();
}

static void HotkeyRow(BindRef& b, float width) {
    ImGui::PushID(b.label);
    if (g_popup_capture == b.vk) {
        ImGui::Button("...", {width, 0.f});
        for (int k = 1; k < 255; k++) {
            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
            if (GetAsyncKeyState(k) & 0x8000) {
                *b.vk = (k == VK_ESCAPE) ? 0 : k;
                g_popup_capture = nullptr;
                break;
            }
        }
    } else {
        char keylabel[48];
        snprintf(keylabel, sizeof(keylabel), "%s##key", vk_name(*b.vk));
        if (ImGui::Button(keylabel, {width, 0.f}))
            g_popup_capture = b.vk;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    ImGui::Combo("##mode", b.mode, kBindModes, 2);
    ImGui::PopID();
}

// The bind registry lives in .data and its heap block was observed corrupted
// once (crash log islamware.dll+0x409e1).  Snapshot it every frame and drop
// entries whose pointers do not land inside the cfg object.
static int SnapshotBinds(BindRef* out, int max) {
    int n = 0;
    int const* const cfg_begin = reinterpret_cast<int const*>(&cfg);
    int const* const cfg_end = cfg_begin + sizeof(Config) / sizeof(int);
    int dropped = 0;
    for (auto const& b : g_bind_registry) {
        if (n >= max) break;
        bool const vk_ok = b.vk >= cfg_begin && b.vk < cfg_end;
        bool const mode_ok = b.mode >= cfg_begin && b.mode < cfg_end;
        bool const label_ok = b.label != nullptr;
        if (!vk_ok || !mode_ok || !label_ok) {
            dropped++;
            continue;
        }
        out[n++] = b;
    }
    if (dropped > 0) {
        static int logged = 0;
        if (logged < 5) {
            logged++;
            Log("[binds] dropped %d corrupt registry entries (heap corruption)", dropped);
        }
    }
    return n;
}

static void RenderAllHotkeys() {
    if (!g_all_hotkeys_open)
        return;
    ImGui::SetNextWindowSize({560.f, 400.f}, ImGuiCond_FirstUseEver);
    bool const open = ImGui::Begin("All hotkeys", &g_all_hotkeys_open, ImGuiWindowFlags_NoCollapse);
    if (open) {
        if (g_bind_registry.empty())
            ImGui::TextDisabled("No hotkeys. Right click any control in the menu to add one.");
        BindRef snapshot[32];
        int const count = SnapshotBinds(snapshot, 32);
        for (int i = 0; i < count; i++) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", snapshot[i].label);
            ImGui::SameLine(180.f);
            HotkeyRow(snapshot[i], 90.f);
        }
    }
    ImGui::End();
}

static void island(ImDrawList* draw, ImVec2 mn, ImVec2 mx, float alpha) {
    draw->AddRectFilled(mn, mx, IM_COL32(1, 1, 1, (int)(220 * alpha)), 4.f);
    draw->AddRect(mn, mx, IM_COL32(0, 0, 0, (int)(120 * alpha)), 4.f);
}

// hotkey list + bomb ESP preview islands on the left of the screen while the
// menu is open (the reference source hud widgets)
static float g_hk_alpha = 0.f;

void MenuIslandsImpl(ImDrawList* draw, int width, int height) {
    (void)height;
    if (!cfg.menu_imgui)
        return;
    // menu closed: the hotkey window spawns only while a bind is engaged.
    // menu open: it always shows (as a preview) as long as it is enabled.
    // (no early returns here - the animated alpha below handles every state
    // transition, including fading out when the menu closes or a bind releases)

    ImFont* f = ImGui::GetFont();
    float const fs = f->FontSize;
    ImGuiIO& io = ImGui::GetIO();

    float island_h = 0.f;
    if (cfg.hotkey_list || g_hk_alpha > 0.01f) {
        float x = cfg.hotkey_x, y = cfg.hotkey_y;
        float const row_h = fs + 8.f;
        float const pad = 10.f;       // side inset
        float const gap_top = 6.f;    // bar to first row
        float const gap_bottom = 6.f; // last row to window bottom (same as top)
        ImGuiIO& mio = ImGui::GetIO();
        float const ft = 1.f - std::exp(-14.f * (mio.DeltaTime > 0.f ? mio.DeltaTime : 1.f / 60.f));

        // persistent per-function animation state, keyed by the bind pointer:
        // rows fade + slide in when a function engages, fade + slide out when
        // it releases, and their vertical slots animate like the reference
        // keybind widget
        struct HkRow {
            int* vk;
            char label[64];
            char keystr[16];
            char modestr[8];
            float name_w, key_w, mode_w;
            float alpha, slide, y;
            bool present;
        };
        static HkRow g_hk_rows[32];
        for (int i = 0; i < 32; i++)
            g_hk_rows[i].present = false;

        BindRef snapshot[32];
        int const snap_count = SnapshotBinds(snapshot, 32);

        // display labels get their "##id" suffix stripped
        auto visible_label = [](char const* label) {
            static char buf[64];
            char const* cut = strstr(label, "##");
            size_t n = cut ? (size_t)(cut - label) : strlen(label);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, label, n);
            buf[n] = 0;
            return buf;
        };

        bool any_active = false;
        int slot = 0;
        for (int bi = 0; bi < snap_count && slot < 32; bi++) {
            BindRef const& b = snapshot[bi];
            if (*b.vk == 0) continue;
            if (!BindIsEngaged(*b.vk, *b.mode)) continue;
            any_active = true;

            HkRow* r = nullptr;
            for (int i = 0; i < 32; i++)
                if (g_hk_rows[i].vk == b.vk) { r = &g_hk_rows[i]; break; }
            if (!r)
                for (int i = 0; i < 32; i++)
                    if (!g_hk_rows[i].vk) { r = &g_hk_rows[i]; break; }
            if (!r) break;

            strncpy(r->label, visible_label(b.label), sizeof(r->label) - 1);
            r->label[sizeof(r->label) - 1] = 0;
            strncpy(r->keystr, vk_name(*b.vk), sizeof(r->keystr) - 1);
            r->keystr[sizeof(r->keystr) - 1] = 0;
            strncpy(r->modestr, (*b.mode == 1) ? "Toggle" : "Hold", sizeof(r->modestr) - 1);
            r->modestr[sizeof(r->modestr) - 1] = 0;
            r->name_w = ImGui::CalcTextSize(r->label).x;
            r->key_w = cfg.hotkey_show_bind ? ImGui::CalcTextSize(r->keystr).x : 0.f;
            r->mode_w = cfg.hotkey_show_mode ? ImGui::CalcTextSize(r->modestr).x : 0.f;
            r->vk = b.vk;
            r->present = true;
            r->y = (float)slot * (row_h + 4.f);
            slot++;
        }

        bool const visible = cfg.menu_open ? cfg.hotkey_list : (cfg.hotkey_list && any_active);

        // animate every row
        float content_bottom = 0.f;
        float target_w = pad + ImGui::CalcTextSize("Hotkeys").x + pad;
        for (int i = 0; i < 32; i++) {
            HkRow& r = g_hk_rows[i];
            if (!r.vk) continue;
            r.alpha = ImLerp(r.alpha, r.present ? 1.f : 0.f, ft);
            r.slide = ImLerp(r.slide, r.present ? 1.f : 0.f, ft);
            if (r.alpha <= 0.01f && !r.present) { r.vk = nullptr; continue; }
            if (r.present) {
                float const end = pad + r.name_w + pad + r.key_w + ((r.key_w && r.mode_w) ? 8.f : 0.f) + r.mode_w;
                target_w = std::max(target_w, end);
                content_bottom = std::max(content_bottom, r.y + row_h);
            }
        }

        float const g_hk_alpha_target = visible ? 1.f : 0.f;
        g_hk_alpha = ImLerp(g_hk_alpha, g_hk_alpha_target, ft);

        if (g_hk_alpha > 0.01f) {
            float const title_h = fs + 10.f;
            // the frame animates with the rows instead of snapping
            static float anim_w = 0.f;
            static float anim_h = 0.f;
            anim_w = ImLerp(anim_w, target_w + pad, ft);
            float const target_h = title_h + gap_top + content_bottom + gap_bottom;
            anim_h = ImLerp(anim_h, target_h, ft);
            if (anim_w < 40.f) anim_w = 40.f;
            if (anim_h < title_h + 8.f) anim_h = title_h + 8.f;
            float const win_w = anim_w;
            float const win_h = anim_h;

            auto fade = [&](float a, ImU32 col) {
                unsigned av = (unsigned)(((col >> IM_COL32_A_SHIFT) & 0xFFu) * a);
                return (col & ~IM_COL32_A_MASK) | (av << IM_COL32_A_SHIFT);
            };

            // drag: the whole top bar, unclipped (islands draw outside any window)
            static bool dragging = false;
            static ImVec2 grab{};
            if (cfg.menu_open &&
                ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + win_w, y + title_h), false) &&
                ImGui::IsMouseClicked(0)) {
                dragging = true;
                grab = ImVec2(io.MousePos.x - x, io.MousePos.y - y);
            }
            if (dragging && ImGui::IsMouseDown(0)) {
                cfg.hotkey_x = io.MousePos.x - grab.x;
                cfg.hotkey_y = io.MousePos.y - grab.y;
                x = cfg.hotkey_x;
                y = cfg.hotkey_y;
            }
            if (!ImGui::IsMouseDown(0))
                dragging = false;

            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + win_w, y + win_h), fade(g_hk_alpha, IM_COL32(1, 1, 1, 225)), 6.f);
            draw->AddRect(ImVec2(x, y), ImVec2(x + win_w, y + win_h), fade(g_hk_alpha, IM_COL32(0, 0, 0, 140)), 6.f);
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + win_w, y + title_h), fade(g_hk_alpha, IM_COL32(16, 17, 20, 240)), 6.f,
                                ImDrawFlags_RoundCornersTop);
            draw->AddLine(ImVec2(x, y + title_h), ImVec2(x + win_w, y + title_h), fade(g_hk_alpha, IM_COL32(60, 62, 70, 200)), 1.f);
            draw->AddText(ImVec2(x + 8.f, y + (title_h - fs) * 0.5f), fade(g_hk_alpha, IM_COL32(235, 235, 240, 255)), "Hotkeys");

            float const rows_top = y + title_h + gap_top;
            for (int i = 0; i < 32; i++) {
                HkRow const& r = g_hk_rows[i];
                if (!r.vk || r.alpha <= 0.01f) continue;
                float const a = g_hk_alpha * r.alpha;
                float const slide_off = (1.f - r.slide) * 10.f;
                float const ry = rows_top + r.y;
                draw->AddText(ImVec2(x + pad - slide_off, ry), fade(a, IM_COL32(235, 235, 240, 255)), r.label);
                float rx = x + pad - slide_off + r.name_w + pad;
                if (r.key_w) {
                    draw->AddText(ImVec2(rx, ry), fade(a, IM_COL32(66, 150, 250, 230)), r.keystr);
                    rx += r.key_w;
                    if (r.mode_w)
                        rx += 8.f;
                }
                if (r.mode_w)
                    draw->AddText(ImVec2(rx, ry), fade(a, IM_COL32(150, 150, 160, 220)), r.modestr);
            }

            island_h = win_h;
        }
    }

    // bomb ESP preview: menu-only (double-click to switch compact/wide; wide is draggable)
    if (cfg.menu_open && cfg.bomb_preview && cfg.esp_bomb) {
        bool const wide = cfg.bomb_wide_preview;
        float const bw = wide ? 144.f : 72.f;
        float const bh = wide ? 62.f  : 68.f;
        float x = wide ? cfg.bomb_x : 24.f;
        float y = wide ? cfg.bomb_y : (cfg.hotkey_list ? cfg.hotkey_y + island_h + 14.f : 140.f);
        float const disp_w = ImGui::GetIO().DisplaySize.x;
        float const disp_h = ImGui::GetIO().DisplaySize.y;
        if (wide) {
            x = std::clamp(x, 8.f, std::max(8.f, disp_w - bw - 8.f));
            y = std::clamp(y, 8.f, std::max(8.f, disp_h - bh - 8.f));
            cfg.bomb_x = x;
            cfg.bomb_y = y;
        }

        // Handle double-click to toggle wide/compact and drag-to-move when wide
        static bool bomb_dragging = false;
        static ImVec2 bomb_drag_off{};
        ImVec2 const mp = ImGui::GetIO().MousePos;
        bool const hovered = mp.x >= x && mp.x <= x + bw && mp.y >= y && mp.y <= y + bh;
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            cfg.bomb_wide_preview = !cfg.bomb_wide_preview;
            bomb_dragging = false;
            if (cfg.bomb_wide_preview && cfg.bomb_y < 10.f) {
                cfg.bomb_x = 24.f;
                cfg.bomb_y = cfg.hotkey_list ? cfg.hotkey_y + island_h + 14.f : 140.f;
            }
        } else if (wide) {
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                bomb_dragging = true;
                bomb_drag_off = { mp.x - x, mp.y - y };
            }
            if (bomb_dragging) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    cfg.bomb_x = std::clamp(mp.x - bomb_drag_off.x, 8.f, std::max(8.f, disp_w - bw - 8.f));
                    cfg.bomb_y = std::clamp(mp.y - bomb_drag_off.y, 8.f, std::max(8.f, disp_h - bh - 8.f));
                    x = cfg.bomb_x;
                    y = cfg.bomb_y;
                } else {
                    bomb_dragging = false;
                }
            }
        } else {
            bomb_dragging = false;
        }

        for (int gi = 6; gi >= 1; gi--) {
            float const e = gi * 3.0f;
            draw->AddRectFilled({ x - e, y - e }, { x + bw + e, y + bh + e },
                                IM_COL32(1, 1, 1, 12 + (6 - gi) * 7), 12.f);
        }
        draw->AddRectFilled({ x, y }, { x + bw, y + bh }, IM_COL32(1, 1, 1, 235), 8.f);

        ImVec2 const icon_center = wide ? ImVec2(x + 30.f, y + bh * 0.5f) : ImVec2(x + bw * 0.5f, y + 24.f);
        if (alice::core && alice::core->fonts) {
            auto& iconf = alice::core->fonts->get("undefeated");
            if (iconf.ptr) {
                ImFontGlyph const* g = iconf.ptr->FindGlyph('o');
                float const scale = iconf.size / iconf.ptr->FontSize;
                float const gx = g ? (g->X0 + g->X1) * 0.5f * scale : 6.f;
                float const gy = g ? (g->Y0 + g->Y1) * 0.5f * scale : 9.f;
                draw->AddText(iconf.ptr, iconf.size,
                              { icon_center.x - gx, icon_center.y - gy },
                              IM_COL32(255, 255, 255, 255), "o");
            }
        } else {
            draw->AddRectFilled({ icon_center.x - 6.f, icon_center.y - 4.f },
                                { icon_center.x + 6.f, icon_center.y + 5.f },
                                IM_COL32(235, 235, 240, 255), 1.5f);
            draw->AddRectFilled({ icon_center.x - 3.f, icon_center.y - 7.f },
                                { icon_center.x + 1.f, icon_center.y - 4.f },
                                IM_COL32(200, 200, 210, 255), 1.f);
            draw->AddCircleFilled({ icon_center.x + 2.5f, icon_center.y - 1.f }, 1.6f,
                                  IM_COL32(255, 75, 75, 255), 10);
        }

        float const pi = 3.14159265358979323846f;
        float const frac = 0.6f;
        draw->PathArcTo(icon_center, 15.f, -pi * 0.5f, -pi * 0.5f + 2.f * pi * frac, 40);
        draw->PathStroke(IM_COL32(255, 75, 75, 255), 0, 2.5f);

        char const* timer = "34.9s";
        if (wide) {
            float const tx = x + 60.f;
            draw->AddText({ tx, y + bh * 0.5f - 15.f }, IM_COL32(245, 245, 250, 255), "Plant A");
            draw->AddText({ tx, y + bh * 0.5f + 3.f }, IM_COL32(255, 85, 85, 255), timer);
        } else {
            ImVec2 const ts = ImGui::CalcTextSize(timer);
            draw->AddText({ icon_center.x - ts.x * 0.5f, icon_center.y + 18.f }, IM_COL32(255, 85, 85, 255), timer);
        }

        char const* note = "bomb ESP preview";
        ImVec2 const ns = ImGui::CalcTextSize(note);
        draw->AddText({ x + bw * 0.5f - ns.x * 0.5f, y + bh + 4.f }, IM_COL32(255, 255, 255, 120), note);
    }
}

static void KeyBind(const char* label, int* vk) {
    ImGui::PushID(label);
    TextLabel(label);
    ImGui::SameLine(150.f);
    if (g_kb_capture == vk) {
        ImGui::Button("press a key...", {-1.f, 0.f});
        for (int k = 1; k < 255; k++) {
            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
            if (GetAsyncKeyState(k) & 0x8000) {
                *vk = (k == VK_ESCAPE) ? 0 : k;
                g_kb_capture = nullptr;
                break;
            }
        }
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s##bind", vk_name(*vk));
        if (ImGui::Button(buf, {-1.f, 0.f}))
            g_kb_capture = vk;
    }
    ImGui::PopID();
}

static void MultiCombo(const char* label, bool* values[], const char* const items[], int count) {
    std::string preview;
    for (int i = 0; i < count; i++) {
        if (!*values[i]) continue;
        if (!preview.empty()) preview += ", ";
        preview += items[i];
    }
    if (!ImGui::BeginCombo(label, preview.empty() ? "-" : preview.c_str()))
        return;
    for (int i = 0; i < count; i++) {
        bool const v = *values[i];
        if (ImGui::Selectable(items[i], v, ImGuiSelectableFlags_NoAutoClosePopups))
            *values[i] = !v;
    }
    ImGui::EndCombo();
}

static void group(const char* label) {
    if (label && label[0])
        ImGui::SeparatorText(label);
}

// checkbox + optional inline color pickers (reference row style)
static void Row(const char* label, bool* v, float* c1 = nullptr, float* c2 = nullptr) {
    ImGui::PushID(label);
    ImGui::Checkbox("##on", v);
    ImGui::SameLine();
    TextLabel(label);
    if (c1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(56.f);
        ImGui::ColorEdit4("##c1", c1, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    }
    if (c2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(56.f);
        ImGui::ColorEdit4("##c2", c2, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    }
    ImGui::PopID();
}

static void ColorRow(const char* label, float* col) {
    ImGui::PushID(label);
    TextLabel(label);
    ImGui::SameLine(150.f);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::ColorEdit4("##color", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    ImGui::PopID();
}

// label followed by two inline colour pickers on the same row (start/end of
// a gradient)
static void ColorRow2(const char* label, float* c1, float* c2) {
    ImGui::PushID(label);
    TextLabel(label);
    ImGui::SameLine(150.f);
    ImGui::SetNextItemWidth(56.f);
    ImGui::ColorEdit4("##c1", c1, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.f);
    ImGui::ColorEdit4("##c2", c2, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    ImGui::PopID();
}

template <class L, class R>
static void Columns(const char* id, L&& left, R&& right) {
    float const width = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    ImGui::PushID(id);
    if (ImGui::BeginChild("##left", {width, 0.f}, true))
        left();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##right", {0.f, 0.f}, true))
        right();
    ImGui::EndChild();
    ImGui::PopID();
}

// checkbox whose feature can be bound: resolves the dedicated bind field or a
// feature-bind slot and offers the right-click bind window on any checkbox
static void Check(const char* label, bool* v, const char* group) {
    ImGui::Checkbox(label, v);
    int* vk = nullptr;
    int* mode = nullptr;
    if (v == &cfg.silentaim) { vk = &cfg.kb_silentaim; mode = &cfg.kb_mode_silentaim; }
    else if (v == &cfg.aimbot) { vk = &cfg.kb_aimbot; mode = &cfg.kb_mode_aimbot; }
    else if (v == &cfg.triggerbot) { vk = &cfg.kb_triggerbot; mode = &cfg.kb_mode_triggerbot; }
    else if (v == &cfg.chams_hit) { vk = &cfg.kb_hitchams; mode = &cfg.kb_mode_hitchams; }
    else if (v == &cfg.bhop) { vk = &cfg.kb_bhop; mode = &cfg.kb_mode_bhop; }
    else if (v == &cfg.autostrafe) { vk = &cfg.kb_autostrafe; mode = &cfg.kb_mode_autostrafe; }
    else if (v == &cfg.baim_force) { vk = &cfg.kb_baim; mode = &cfg.kb_mode_baim; }
    else if (v == &cfg.peek_assist) { vk = &cfg.kb_peek_assist; mode = &cfg.kb_mode_peek_assist; }
    else {
        for (int i = 0; i < FB_COUNT; i++) {
            if (FeatureBindTarget(i) == v) {
                vk = &cfg.kb_feature[i];
                mode = &cfg.kb_feature_mode[i];
                break;
            }
        }
    }
    if (vk) {
        // small dot next to the name when the function has a bind
        if (*vk != 0) {
            ImVec2 const mn = ImGui::GetItemRectMin();
            ImVec2 const mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(mx.x + 6.f, (mn.y + mx.y) * 0.5f), 2.5f,
                IM_COL32(66, 150, 250, 210), 12);
        }
        BindPopup(label, group, vk, mode);
    }
}

// ------------------------------------------------------------------
// pages
// ------------------------------------------------------------------

static const char* const kHitboxes[5]     = { "Head", "Neck", "Chest", "Stomach", "Pelvis" };
static const char* const kTargets[3]      = { "Crosshair", "Distance", "Health" };
static const char* const kAimKeys[4]      = { "Always", "Mouse4", "Mouse5", "Alt" };
static const char* const kKillEffects[4]  = { "Tesla", "Explosion", "Blood", "Gauss" };
static const char* const kChamsMaterials[7] = { "Regular", "Flat", "Shaded", "Metallic", "Glow", "Glass", "Wireframe" };
static const char* const kChamsTargets[5] = { "Enemies", "Teammates", "Local", "Viewmodel", "Weapons" };
static const char* const kAaPitch[3]      = { "None", "Down", "Up" };
static const char* const kAaBase[2]       = { "At Local", "At Target" };
static const char* const kAaYaw[2]        = { "Forward", "Backward" };
static const char* const kAaMod[7]        = { "None", "Center", "Offset", "Random", "Spin", "Three-way", "Five-way" };
static const char* const kBodyAimModes[3] = { "Default", "Prefer", "Force" };
static const char* const kBodyHitboxes[3] = { "Chest", "Stomach", "Pelvis" };
static const char* const kBaimConds[4]    = { "Lethal", "In air", "Moving", "On peek" };
static const char* const kBaimDis[4]      = { "Target resolved", "Target shooting", "Low damage", "Head safe" };

static void RenderRagebot() {
    Columns("##rage",
    [] {
        group("Silent aim");
        Check("Silent aim", &cfg.silentaim, "Ragebot");
        ImGui::BeginDisabled(!cfg.silentaim);
        ImGui::SliderFloat("Silent FOV", &cfg.silent_fov, 0.f, 180.f, "%.0f");
        {
            bool* values[5] = { &cfg.silent_hb_head, &cfg.silent_hb_neck, &cfg.silent_hb_chest, &cfg.silent_hb_stomach, &cfg.silent_hb_pelvis };
            MultiCombo("Hitboxes", values, kHitboxes, 5);
        }
        ImGui::Combo("Target selection", &cfg.silent_select, kTargets, 3);
        Check("Visible only", &cfg.silent_visible, "Ragebot");
        Check("Multipoint", &cfg.silent_multipoint, "Ragebot");
        ImGui::SliderFloat("Min. damage", &cfg.min_damage, 0.f, 100.f, "%.0f");
        ImGui::SliderFloat("Hit chance", &cfg.hit_chance, 0.f, 100.f, "%.0f%%");
        ImGui::SliderInt("Delay shot (ms)", &cfg.delay_shot, 0, 500);
        Check("Auto stop", &cfg.auto_stop, "Ragebot");
        Check("Auto scope", &cfg.auto_scope, "Ragebot");
        Check("Auto wall", &cfg.auto_wall, "Ragebot");
        if (cfg.auto_wall)
            ImGui::TextDisabled("Uses each weapon's real penetration & surface data.");
        Check("Draw aim FOV", &cfg.draw_aim_fov, "Ragebot");
        Check("Double tap", &cfg.double_tap, "Ragebot");
        Check("Log misses##rage", &cfg.log_misses, "Ragebot");
        ImGui::EndDisabled();

        group("Aimbot");
        ImGui::Combo("Aimbot key", &cfg.aim_key, kAimKeys, 4);
        Check("Visible aimbot", &cfg.aimbot, "Ragebot");
        ImGui::BeginDisabled(!cfg.aimbot);
        ImGui::SliderFloat("Aim FOV", &cfg.aim_fov, 1.f, 60.f, "%.1f");
        ImGui::SliderFloat("Smoothing", &cfg.aim_smooth, 1.f, 10.f, "%.1f");
        ImGui::EndDisabled();
    },
    [] {
        group("Triggerbot");
        Check("Auto fire", &cfg.triggerbot, "Ragebot");
        ImGui::SliderFloat("Trigger FOV", &cfg.trigger_fov, 0.2f, 15.f, "%.1f");

        group("Knifebot");
        Check("Knifebot", &cfg.knifebot, "Ragebot");
        ImGui::SliderFloat("Knife reach", &cfg.knife_range, 30.f, 120.f, "%.0f");

        group("Backtrack");
        Check("Backtrack", &cfg.backtrack, "Ragebot");
        ImGui::BeginDisabled(!cfg.backtrack);
        ImGui::SliderFloat("Backtrack time", &cfg.backtrack_time, 0.05f, 0.45f, "%.2f s");
        ImGui::EndDisabled();

        group("Accuracy");
        Check("No spread", &cfg.nospread, "Ragebot");

        group("Resolver");
        Check("Miss resolver", &cfg.resolver, "Ragebot");
        ImGui::TextDisabled("Shifts the aim sideways after misses.");

        group("Body aim");
        ImGui::Combo("Body aim", &cfg.body_aim, kBodyAimModes, 3);
        {
            int bhb = cfg.body_aim_hitbox - 2;
            if (bhb < 0 || bhb > 2) bhb = 0;
            if (ImGui::Combo("Body hitbox", &bhb, kBodyHitboxes, 3))
                cfg.body_aim_hitbox = bhb + 2;
        }
        Check("Force body aim", &cfg.baim_force, "Ragebot");
        {
            bool* conds[4] = { &cfg.baim_lethal, &cfg.baim_in_air, &cfg.baim_moving, &cfg.baim_on_peek };
            MultiCombo("Conditions##baim", conds, kBaimConds, 4);
            bool* dis[4] = { &cfg.baim_dis_resolved, &cfg.baim_dis_shooting, &cfg.baim_dis_low_dmg, &cfg.baim_dis_head_safe };
            MultiCombo("Disablers##baim", dis, kBaimDis, 4);
        }

        group("Peek assist");
        Check("Peek assist##pa", &cfg.peek_assist, "Ragebot");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(56.f);
        ImGui::ColorEdit4("##pa_col", cfg.peek_assist_color,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
        ImGui::BeginDisabled(!cfg.peek_assist);
        Check("Retreat on shot##pa", &cfg.peek_retreat_on_shot, "Ragebot");
        Check("Retreat on release##pa", &cfg.peek_retreat_on_release, "Ragebot");
        ImGui::SliderInt("Max distance##pa", &cfg.peek_max_distance, 0, 200,
                         cfg.peek_max_distance == 0 ? "Infinite" : "%d");
        ImGui::EndDisabled();
    });
}

static void RenderAntiAim() {
    Columns("##aa",
    [] {
        group("Angles");
        Check("Enabled##aa", &cfg.aa_enabled, "Anti-aim");
        ImGui::BeginDisabled(!cfg.aa_enabled);
        ImGui::Combo("Pitch", &cfg.aa_pitch, kAaPitch, 3);
        ImGui::Combo("Base", &cfg.aa_base, kAaBase, 2);
        ImGui::Combo("Yaw", &cfg.aa_yaw, kAaYaw, 2);
        ImGui::SliderFloat("Yaw offset", &cfg.aa_yaw_offset, -180.f, 180.f, "%.0f");
        ImGui::Combo("Yaw modifier", &cfg.aa_modifier, kAaMod, 7);
        ImGui::BeginDisabled(cfg.aa_modifier == 0);
        ImGui::SliderFloat("Modifier offset", &cfg.aa_modifier_offset, 0.f, 180.f, "%.0f");
        ImGui::EndDisabled();
        Check("Yaw sides", &cfg.aa_sides, "Anti-aim");
        ImGui::BeginDisabled(!cfg.aa_sides);
        ImGui::SliderFloat("Left", &cfg.aa_left, 0.f, 180.f, "%.0f");
        ImGui::SliderFloat("Right", &cfg.aa_right, 0.f, 180.f, "%.0f");
        ImGui::SliderInt("Side delay", &cfg.aa_side_delay, 1, 10);
        ImGui::EndDisabled();
        ImGui::SliderFloat("Roll", &cfg.aa_roll, -45.f, 45.f, "%.0f");
        ImGui::EndDisabled();
    },
    [] {
        group("Manuals");
        KeyBind("Manual left", &cfg.aa_manual_left);
        KeyBind("Manual right", &cfg.aa_manual_right);
        ImGui::TextDisabled("Hold a manual key to rotate the fake body.");
    });
}

static void RenderPlayers() {
    // layout: 340px settings pane on the left, live drag & drop ESP
    // preview on the right
    float const left_w = 340.f;
    ImGui::PushID("##esp");
    if (ImGui::BeginChild("##esp_settings", { left_w, 0.f }, true)) {
        group("Elements");
        Check("Player ESP", &cfg.esp, "Players");
        Check("Bomb ESP", &cfg.esp_bomb, "Players");
        ImGui::BeginDisabled(!cfg.esp);
        Check("Team check", &cfg.esp_team_check, "Players");
        Row("Box", &cfg.esp_box, cfg.esp_box_color, cfg.esp_box_outline);
        Row("Name", &cfg.esp_name, cfg.esp_name_color);
        BindPopup("Name", "Players", nullptr, nullptr);
        Row("Health", &cfg.esp_health, cfg.esp_health_min, cfg.esp_health_max);
        ImGui::BeginDisabled(!cfg.esp_health);
        ImGui::Indent();
        Check("Health gradient", &cfg.esp_health_gradient, "Players");
        ImGui::Unindent();
        ImGui::EndDisabled();
        Row("Armor", &cfg.esp_armor, cfg.esp_armor_min, cfg.esp_armor_max);
        ImGui::BeginDisabled(!cfg.esp_armor);
        ImGui::Indent();
        Check("Armor gradient", &cfg.esp_armor_gradient, "Players");
        ImGui::Unindent();
        ImGui::EndDisabled();
        Row("Ammo", &cfg.esp_ammo, cfg.esp_ammo_min, cfg.esp_ammo_max);
        Row("Weapon", &cfg.esp_weapon, cfg.esp_weapon_color);
        Row("Weapon icon", &cfg.esp_weapon_icon, cfg.esp_weapon_icon_color);
        Row("Kit icon", &cfg.esp_kit_icon, cfg.esp_kit_icon_color);
        Row("C4 icon", &cfg.esp_c4_icon, cfg.esp_c4_icon_color);
        Row("Taser icon", &cfg.esp_taser_icon, cfg.esp_taser_icon_color);
        Row("Skeleton", &cfg.esp_skeleton);
        Check("Distance", &cfg.esp_distance, "Players");
        ImGui::EndDisabled();

        group("Dropped weapons");
        Row("Dropped weapons##dw", &cfg.dropped_weapons, cfg.dropped_color);
        ImGui::BeginDisabled(!cfg.dropped_weapons);
        Check("Weapon name##dw", &cfg.dropped_name, "Players");
        Check("Weapon icon##dw", &cfg.dropped_icon, "Players");
        Check("Ammo text##dw", &cfg.dropped_ammo_text, "Players");
        Check("Ammo bar##dw", &cfg.dropped_ammo_bar, "Players");
        ImGui::EndDisabled();

        group("Flags");
        Row("Flags", &cfg.esp_flags);
        ImGui::BeginDisabled(!cfg.esp_flags);
        Row("Money", &cfg.flag_money, cfg.flag_money_color);
        Row("Armor##flag", &cfg.flag_armor, cfg.flag_armor_color);
        Row("Kit", &cfg.flag_kit, cfg.flag_kit_color);
        Row("Defuse", &cfg.flag_defuse, cfg.flag_defuse_color);
        Row("Flashed", &cfg.flag_flashed, cfg.flag_flashed_color);
        Row("C4", &cfg.flag_c4, cfg.flag_c4_color);
        ImGui::EndDisabled();

        group("Offscreen");
        Row("Offscreen arrows", &cfg.esp_offscreen, cfg.esp_offscreen_color);
        ImGui::BeginDisabled(!cfg.esp_offscreen);
        ImGui::SliderFloat("Radius", &cfg.esp_offscreen_radius, 40.f, 200.f, "%.0f");
        ImGui::SliderFloat("Size##offscreen", &cfg.esp_offscreen_size, 1.f, 3.f, "%.1f");
        ImGui::EndDisabled();

        group("Backtrack");
        Check("Backtrack skeleton", &cfg.backtrack_skeleton, "Players");
        Row("Backtrack chams", &cfg.backtrack_chams, cfg.bt_chams_color);

        group("Dormant");
        Check("Show dormant", &cfg.esp_show_dormant, "Players");
        ImGui::BeginDisabled(!cfg.esp_show_dormant);
        ImGui::SliderFloat("Alpha", &cfg.esp_dormant_alpha, 0.1f, 1.f, "%.2f");
        Check("Fade dormant", &cfg.esp_dormant_fade, "Players");
        ImGui::BeginDisabled(!cfg.esp_dormant_fade);
        ImGui::SliderFloat("Fade time", &cfg.esp_dormant_fade_time, 1.f, 60.f, "%.0f s");
        ImGui::EndDisabled();
        ImGui::SliderFloat("Grayscale", &cfg.esp_dormant_grayscale, 0.f, 1.f, "%.2f");
        ImGui::EndDisabled();

        group("Aimbot visuals");
        Row("Aim point", &cfg.esp_aimbot_visual, cfg.esp_aimbot_color);
        ImGui::BeginDisabled(!cfg.esp_aimbot_visual);
        Row("Safe point", &cfg.esp_aimbot_safe, cfg.esp_aimbot_safe_color);
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("##esp_preview", { 0.f, 0.f }, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 const origin = ImGui::GetWindowPos();
        ImVec2 const avail = ImGui::GetContentRegionAvail();

        // fake player box centred in the preview pane (taller so the model
        // fits inside it)
        ImVec2 const center{ origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f };
        ImVec2 const bmin{ center.x - 70.f, center.y - 150.f };
        ImVec2 const bmax{ center.x + 70.f, center.y + 150.f };

        esp_layout::PlayerView v;
        v.alive = true;
        v.name = "enemy";
        v.health = 0.72f;
        v.armor = 100;
        v.has_weapon = true;
        v.weapon = "AK-47";
        v.has_ammo = true;
        snprintf(v.ammo_text, sizeof(v.ammo_text), "5 / 30");
        snprintf(v.distance_text, sizeof(v.distance_text), "24m");
        v.kit_icon = true;
        v.c4_icon = true;
        v.taser_icon = true;
        if (cfg.esp_flags) {
            v.flag_money = "$16000";
            v.flag_armor = "HK";
            v.flag_kit = "KIT";
            v.flag_defuse = "DEFUSE";
            v.flag_flashed = "FLASH";
            v.flag_c4 = "C4";
        }

        glb_preview::Draw(dl, center, bmax.y - bmin.y - 50.f);
        esp_layout::RenderPreview(dl, bmin, bmax, 1.f, v);

        static char const* const hints[] = {
            "Drag to move",
            "Shift + drag to resize",
            "Shift + RMB to reset",
        };
        float const hfs = ImGui::GetFontSize() * 0.82f;
        float hy = origin.y + avail.y - 8.f;
        for (int hi = 2; hi >= 0; hi--) {
            ImVec2 const hs = ImGui::GetFont()->CalcTextSizeA(hfs, FLT_MAX, 0.f, hints[hi]);
            hy -= hs.y;
            dl->AddText(ImGui::GetFont(), hfs,
                        ImVec2(origin.x + 10.f, hy),
                        IM_COL32(255, 255, 255, 110), hints[hi]);
            hy -= 2.f;
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

static void RenderModels() {
    Columns("##models",
    [] {
        ImGui::SeparatorText("Chams");
        static int target = 0;
        ImGui::Combo("Target", &target, kChamsTargets, 5);
        target = target < 0 ? 0 : (target > 4 ? 4 : target);
        Config::ChamsTarget* targets[5] = { &cfg.chams_enemy, &cfg.chams_team, &cfg.chams_local, &cfg.chams_viewmodel, &cfg.chams_weapons };
        Config::ChamsTarget* t = targets[target];

        auto layer = [](const char* label, Config::ChamsLayer& l) {
            ImGui::PushID(label);
            ImGui::Checkbox(label, &l.enabled);
            ImGui::BeginDisabled(!l.enabled);
            ImGui::Combo("Material", &l.material, kChamsMaterials, 7);
            ImGui::ColorEdit4("Color", &l.color[0], ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::EndDisabled();
            ImGui::PopID();
        };
        layer("Through walls", t->occluded);
        layer("Visible", t->visible);
        layer("Overlay", t->overlay);

        ImGui::SeparatorText("Hit chams");
        Check("Enabled##hit", &cfg.chams_hit, "Models");
        ImGui::BeginDisabled(!cfg.chams_hit);
        ImGui::Combo("Material##hit", &cfg.chams_hit_material, kChamsMaterials, 7);
        ImGui::ColorEdit4("Color##hit", &cfg.chams_hit_color[0], ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SliderFloat("Duration", &cfg.chams_hit_duration, 0.1f, 5.f, "%.1fs");
        ImGui::EndDisabled();

        Check("Disable occlusion", &cfg.chams_disable_occlusion, "Models");

        ImGui::SeparatorText("Held weapons");
        Check("Enemy weapons", &cfg.wchams_enemy, "Models");
        Check("Teammate weapons", &cfg.wchams_team, "Models");
        ImGui::TextDisabled("Enemy/team use their layer; Weapons applies to");
        ImGui::TextDisabled("your third-person weapon.");
    },
    [] {
        ImGui::SeparatorText("First person");
        ImGui::TextDisabled("Viewmodel & hands use the 'Viewmodel' chams target.");
        ImGui::TextDisabled("World weapons use the 'Weapons' chams target.");

        ImGui::SeparatorText("Custom player model");
        int const count = CustomModels_Count(false);
        std::vector<std::string> entries;
        entries.reserve(count + 1);
        entries.push_back("Default");
        int sel = 0;
        for (int i = 0; i < count; i++) {
            const char* p = CustomModels_At(i);
            entries.push_back(p);
            if (cfg.model_path[0] && _stricmp(cfg.model_path, p) == 0)
                sel = i + 1;
        }
        auto getter = [](void* data, int idx, const char** out_text) -> bool {
            auto* vec = static_cast<std::vector<std::string>*>(data);
            if (idx < 0 || idx >= (int)vec->size()) return false;
            const std::string& s = (*vec)[idx];
            // Strip leading "models/player/" when present so the combo stays readable
            if (s.rfind("models/player/", 0) == 0)
                *out_text = s.c_str() + 14;
            else
                *out_text = s.c_str();
            return true;
        };
        int prev = sel;
        if (ImGui::Combo("Model##custom", &sel, getter, &entries, (int)entries.size())) {
            if (sel != prev) {
                if (sel <= 0) {
                    cfg.model_path[0] = 0;
                    cfg.model_index = 0;
                } else {
                    strncpy(cfg.model_path, entries[sel].c_str(), sizeof(cfg.model_path) - 1);
                    cfg.model_path[sizeof(cfg.model_path) - 1] = 0;
                    cfg.model_index = 0;
                }
            }
        }
        if (ImGui::Button("Refresh models"))
            CustomModels_Count(true);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", CustomModels_Status());
        ImGui::TextDisabled("Visible in third-person view. Put .mdl packs in");
        ImGui::TextDisabled("cstrike/models/player/ or cstrike/custom/<pack>/models/.");
    });
}

static void RenderWorld() {
    Columns("##world",
    [] {
        group("Night");
        ImGui::SliderFloat("Night level", &cfg.world_night, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Brightness", &cfg.world_brightness, 0.f, 2.f, "%.2f");
        ImGui::SliderFloat("Model ambient", &cfg.world_model_ambient, 0.f, 2.f, "%.2f");
        Check("Fullbright", &cfg.world_fullbright, "World");

        group("Tint");
        Row("Tint walls", &cfg.world_walls, cfg.wall_color);
        Row("Tint props", &cfg.world_props, cfg.prop_color);

        group("Fog");
        Check("No fog", &cfg.world_nofog, "World");
        Row("Custom fog", &cfg.world_custom_fog, cfg.fog_color);
        ImGui::BeginDisabled(!cfg.world_custom_fog);
        ImGui::SliderFloat("Start", &cfg.fog_start, 0.f, 5000.f, "%.0f");
        ImGui::SliderFloat("End", &cfg.fog_end, 0.f, 10000.f, "%.0f");
        ImGui::SliderFloat("Density", &cfg.fog_density, 0.f, 1.f, "%.2f");
        ImGui::EndDisabled();
    },
    [] {
        group("Removals");
        Check("Post processing", &cfg.rem_postprocess, "World");
        Check("Shadows", &cfg.rem_shadows, "World");
        Check("Decals", &cfg.rem_decals, "World");
        Check("Grass", &cfg.rem_grass, "World");
        Check("Ropes", &cfg.rem_ropes, "World");
        Check("Water reflections", &cfg.rem_water, "World");
        Check("Dynamic lights", &cfg.rem_dynlights, "World");
        Check("Shell casings", &cfg.rem_shells, "World");
        Check("Debris", &cfg.rem_debris, "World");
        Check("Blood", &cfg.rem_blood, "World");
        Check("Particles", &cfg.rem_particles, "World");

        group("Scope");
        Check("Remove scope overlay", &cfg.rem_scope, "World");
        ImGui::BeginDisabled(!cfg.rem_scope);
        Row("Custom scope lines", &cfg.custom_scope, cfg.custom_scope_color);
        ImGui::BeginDisabled(!cfg.custom_scope);
        ImGui::SliderInt("Scope gap", &cfg.scope_gap, -15, 300, "%d");
        ImGui::SliderInt("Scope length", &cfg.scope_length, 0, 300, "%d");
        Check("Invert scope gradient", &cfg.scope_invert, "World");
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        group("Grenades");
        Check("Trajectory prediction", &cfg.nade_predict, "World");
        Check("Grenade markers", &cfg.nade_esp, "World");
        ImGui::BeginDisabled(!(cfg.nade_predict || cfg.nade_esp));
        ColorRow("HE color", cfg.nade_color_he);
        ColorRow("Flash color", cfg.nade_color_flash);
        ColorRow("Smoke color", cfg.nade_color_smoke);
        ImGui::EndDisabled();

        group("Effects");
        Check("Bullet tracers", &cfg.tracers, "World");
        ImGui::BeginDisabled(!cfg.tracers);
        ColorRow2("Local tracer", cfg.tracer_local_color, cfg.tracer_local_color2);
        ColorRow2("Enemy tracer", cfg.tracer_enemy_color, cfg.tracer_enemy_color2);
        ColorRow2("Team tracer", cfg.tracer_team_color, cfg.tracer_team_color2);
        ImGui::SliderFloat("Tracer life", &cfg.tracer_life, 0.5f, 8.f, "%.1f s");
        ImGui::EndDisabled();

        Row("Bullet impacts", &cfg.bullet_impacts, cfg.impact_color);
        ImGui::BeginDisabled(!cfg.bullet_impacts);
        Row("Server impacts", &cfg.impacts_server, cfg.impact_server_color);
        ImGui::SliderFloat("Size##impacts", &cfg.impact_size, 1.f, 12.f, "%.1f");
        ImGui::SliderFloat("Life##impacts", &cfg.impact_time, 0.2f, 8.f, "%.1f s");
        ImGui::EndDisabled();

        group("Kill effect");
        Row("Kill effect", &cfg.kill_effects, cfg.kill_effect_color);
        ImGui::BeginDisabled(!cfg.kill_effects);
        ImGui::Combo("Effect", &cfg.kill_effect_type, kKillEffects, 4);
        ImGui::SliderFloat("Scale", &cfg.kill_effect_scale, 0.5f, 3.f, "%.1f");
        ImGui::SliderFloat("Life##fx", &cfg.kill_effect_time, 0.5f, 5.f, "%.1f s");
        ImGui::EndDisabled();
    });
}

static void RenderView() {
    Columns("##view",
    [] {
        group("Camera");
        Check("Third person", &cfg.thirdperson, "View");
        ImGui::BeginDisabled(!cfg.thirdperson);
        ImGui::SliderFloat("Distance", &cfg.thirdperson_dist, 30.f, 500.f, "%.0f");
        ImGui::EndDisabled();
        Check("Left hand", &cfg.left_hand, "View");
        Check("Remove bob", &cfg.remove_bob, "View");
        ImGui::SliderFloat("FOV override", &cfg.fov_override, 0.f, 160.f, "%.0f (0 = off)");
        ImGui::SliderFloat("Viewmodel FOV", &cfg.viewmodel_fov, 0.f, 30.f, "%.0f (0 = off)");
    },
    [] {
        group("Viewmodel");
        ImGui::SliderFloat("Offset X", &cfg.viewmodel_x, -20.f, 20.f, "%.1f");
        ImGui::SliderFloat("Offset Y", &cfg.viewmodel_y, -20.f, 20.f, "%.1f");
        ImGui::SliderFloat("Offset Z", &cfg.viewmodel_z, -20.f, 20.f, "%.1f");
        ImGui::TextDisabled("Shifts the viewmodel along your view axes.");
    });
}

static void RenderMovement() {
    Columns("##movement",
    [] {
        group("Movement");
        Check("Bunny hop", &cfg.bhop, "Movement");
        Check("Auto strafe", &cfg.autostrafe, "Movement");
        ImGui::BeginDisabled(!cfg.autostrafe);
        Check("Strafe follows keys", &cfg.strafe_follow_keys, "Movement");
        ImGui::EndDisabled();
        Check("Crouch in air", &cfg.crouch_air, "Movement");
        Check("Infinite duck", &cfg.inf_duck, "Movement");
        Check("Quick stop", &cfg.quick_stop, "Movement");
        ImGui::SliderFloat("Fastwalk", &cfg.fastwalk, 1.f, 2.f, "%.2f");
    },
    [] {
        group("Fakelag");
        Check("Fakelag", &cfg.fakelag, "Movement");
        ImGui::BeginDisabled(!cfg.fakelag);
        ImGui::SliderInt("Choke ticks", &cfg.fakelag_ticks, 1, 17, "%d");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Server limit: sv_maxusrcmdprocessticks 16 + 1.");

        group("Exploits");
        Check("Duck peek", &cfg.x_duckpeek, "Movement");
        Check("Silent run", &cfg.x_silentrun, "Movement");
        Check("Jump bug", &cfg.x_jumpbug, "Movement");
        Check("Fast ladder", &cfg.x_fastladder, "Movement");
        Check("Reload cancel (Q)", &cfg.x_qcancel, "Movement");
    });
}

static void RenderMisc() {
    Columns("##misc",
    [] {
        group("Misc");
        Check("Watermark", &cfg.watermark, "Misc");
        Check("Super toss", &cfg.super_toss, "Misc");
        Row("Log misses##misc", &cfg.log_misses, cfg.log_misses_color);
    },
    [] {
        group("Menu");
        KeyBind("Menu key##misc", &cfg.kb_menu);
    });
}

static void RenderConfigs() {
    if (!g_cfg_ready) {
        alice_ui::ConfigMenuInit();
        g_cfg_ready = true;
    }

    Columns("##configs",
    [] {
        ImGui::SeparatorText("Configs");
        int const count = alice_ui::ConfigMenuCount();
        if (g_cfg_sel >= count) g_cfg_sel = count > 0 ? count - 1 : -1;
        if (ImGui::BeginListBox("##list", {-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 5.f})) {
            for (int i = 0; i < count; i++) {
                const char* name = alice_ui::ConfigMenuName(i);
                if (!name) continue;
                if (ImGui::Selectable(name, g_cfg_sel == i)) {
                    g_cfg_sel = i;
                    strncpy(g_cfg_name, name, sizeof(g_cfg_name) - 1);
                }
            }
            ImGui::EndListBox();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##name", "config name", g_cfg_name, sizeof(g_cfg_name));

        float const half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Save", {half, 0.f}) && g_cfg_name[0])
            alice_ui::ConfigMenuSave(g_cfg_name);
        ImGui::SameLine();
        ImGui::BeginDisabled(g_cfg_sel < 0);
        if (ImGui::Button("Load", {half, 0.f}) && g_cfg_sel >= 0)
            alice_ui::ConfigMenuLoad(alice_ui::ConfigMenuName(g_cfg_sel));
        ImGui::EndDisabled();
        if (ImGui::Button("Delete", {half, 0.f}) && g_cfg_sel >= 0)
            alice_ui::ConfigMenuDelete(alice_ui::ConfigMenuName(g_cfg_sel));
        ImGui::SameLine();
        if (ImGui::Button("Refresh", {half, 0.f}))
            alice_ui::ConfigMenuRefresh();
    },
    [] {
        ImGui::SeparatorText("Menu");
        KeyBind("Menu key##configs", &cfg.kb_menu);
    });
}

static void RenderSettings() {
    Columns("##settings",
    [] {
        ImGui::SeparatorText("Menu");
        KeyBind("Menu key##settings", &cfg.kb_menu);
    },
    [] {
        ImGui::SeparatorText("Hotkey window");
        Check("Show hotkey window", &cfg.hotkey_list, "Settings");
        Check("Show hold/toggle text", &cfg.hotkey_show_mode, "Settings");
        Check("Show bind", &cfg.hotkey_show_bind, "Settings");

        ImGui::SeparatorText("Whitelist");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##whitelist", "names separated by commas", cfg.whitelist, sizeof(cfg.whitelist));
        ImGui::TextDisabled("Aim, trigger and knifebot skip these players.");

        ImGui::SeparatorText("Debug");
        if (ImGui::Button("All hotkeys..."))
            g_all_hotkeys_open = true;
        ImGui::Text("build: 9540945");
        ImGui::Text("renderer: d3d9");
        ImGui::Text("fps: %.0f", ImGui::GetIO().Framerate);
    });
}

} // namespace imgui_menu

void ImguiMenuRender() {
    using namespace imgui_menu;
    if (!g_style_done) {
        ApplyStyle();
        g_style_done = true;
    }

    ImGui::SetNextWindowSize({700.f, 500.f}, ImGuiCond_FirstUseEver);
    bool const open = ImGui::Begin("islamware", nullptr, ImGuiWindowFlags_NoCollapse);
    if (open) {
        // reference blurred window background: the game frame is captured into
        // the blur chain and drawn back over the window, then the dark
        // window tint (0.05/0.05/0.07 @ 0.35) goes on top
        ImVec2 const wmin = ImGui::GetWindowPos();
        ImVec2 const wmax = { wmin.x + ImGui::GetWindowSize().x, wmin.y + ImGui::GetWindowSize().y };
        float const rounding = ImGui::GetStyle().WindowRounding;
        if (alice::core && alice::core->blur)
            alice::core->blur->apply(ImGui::GetWindowDrawList(), wmin, wmax, 16.f, rounding, 2, 1.f);
        ImGui::GetWindowDrawList()->AddRectFilled(wmin, wmax, IM_COL32(13, 13, 18, 242), rounding);
        // the tint paints over the window border - redraw it on top so the
        // rounded corners (especially bottom) stay visible
        ImGui::GetWindowDrawList()->AddRect(wmin, wmax, ImGui::GetColorU32(ImGuiCol_Border), rounding, 0, 1.5f);
    }
    if (open && ImGui::BeginTabBar("##tabs")) {
        auto tab = [](const char* label, auto&& body) {
            if (ImGui::BeginTabItem(label)) {
                body();
                Lua_RenderTabExtras(label);
                ImGui::EndTabItem();
            }
        };
        tab("Ragebot", RenderRagebot);
        tab("Anti-aim", RenderAntiAim);
        tab("Players", RenderPlayers);
        tab("Models", RenderModels);
        tab("World", RenderWorld);
        tab("View", RenderView);
        tab("Movement", RenderMovement);
        tab("Misc", RenderMisc);
        tab("Configs", RenderConfigs);
        tab("Lua", Lua_RenderManagerTab);
        tab("Settings", RenderSettings);
        Lua_RenderExtensionTabs();
        ImGui::EndTabBar();
    }
    ImGui::End();

    RenderAllHotkeys();
}

void ImguiMenuIslands(ImDrawList* draw, int width, int height) {
    imgui_menu::MenuIslandsImpl(draw, width, height);
}
