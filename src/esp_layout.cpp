#include "esp_layout.hpp"
#include "cheat.h"
#include "alice_kit.hpp"
#include "fonts/fonts.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>

using namespace esp_layout;

// ---------------------------------------------------------------------------
// engine - port of the reference CESPLayout
// ---------------------------------------------------------------------------

ImVec2 Layout::floorPos(ImVec2 v) const { return { std::floor(v.x), std::floor(v.y) }; }

int Layout::addSlot(Slot slot) {
    int const id = static_cast<int>(slots.size());
    slot.id = id;
    slots.push_back(slot);
    rebuildOrder();
    return id;
}

int Layout::addBar(std::string name, Side side, int order, MeasureFn measure, RenderFn render) {
    Slot s;
    s.name = name;
    s.side = side;
    s.group = GroupBar;
    s.order = order;
    s.measure = measure;
    s.render = render;
    return addSlot(s);
}

int Layout::addText(std::string name, Side side, int order, MeasureFn measure, RenderFn render, Align align) {
    Slot s;
    s.name = name;
    s.side = side;
    s.group = GroupText;
    s.order = order;
    s.align = align;
    s.measure = measure;
    s.render = render;
    return addSlot(s);
}

int Layout::addBox(std::string name, RenderFn render) {
    Slot s;
    s.name = name;
    s.group = GroupBox;
    s.render = render;
    s.draggable = false;
    return addSlot(s);
}

void Layout::bindEnabled(int slot_id, bool* ext) {
    for (auto& s : slots)
        if (s.id == slot_id) { s.enabled_ref = ext; return; }
}

void Layout::reset() {
    slots.clear();
    rebuildOrder();
    dragging_id = -1;
    resizing_id = -1;
    hovered_id = -1;
    drag_prev_size = { 0.f, 0.f };
    has_last_bounds = false;
}

std::string Layout::serialize() const {
    std::string out;
    for (auto const& s : slots) {
        out += s.name;
        out += ':';
        out += std::to_string(static_cast<int>(s.side));
        out += ':';
        out += std::to_string(s.order);
        if (std::abs(s.scale - 1.0f) >= 0.01f) {
            char buf[16];
            snprintf(buf, sizeof(buf), ":%.2f", s.scale);
            out += buf;
        }
        out += ';';
    }
    return out;
}

void Layout::deserialize(const char* data) {
    if (slots.empty() || !data || !data[0])
        return;

    std::string const sv(data);
    size_t pos = 0;
    while (pos < sv.size()) {
        size_t const semi = sv.find(';', pos);
        std::string const entry = sv.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? sv.size() : semi + 1;

        // Supports both "Name:side:order" and "Name:side:order:scale"
        size_t const c1 = entry.find(':');
        if (c1 == std::string::npos || c1 == 0) continue;
        size_t const c2 = entry.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        size_t const c3 = entry.find(':', c2 + 1);

        int const side = atoi(entry.c_str() + c1 + 1);
        int const order = atoi(entry.c_str() + c2 + 1);
        float scale = 1.0f;
        if (c3 != std::string::npos) {
            float const parsed = static_cast<float>(atof(entry.c_str() + c3 + 1));
            if (parsed >= 0.4f && parsed <= 3.5f)
                scale = std::clamp(parsed, 0.5f, 3.0f);
        }
        if (side < 0 || side >= 4) continue;

        std::string const name = entry.substr(0, c1);
        for (auto& s : slots) {
            if (s.name != name) continue;
            s.side = static_cast<Side>(side);
            s.order = order;
            s.scale = scale;
            break;
        }
    }
    rebuildOrder();
}

void Layout::moveSlot(int slot_id, Side target_side, int target_index_in_group) {
    Slot* slot = nullptr;
    for (auto& s : slots)
        if (s.id == slot_id) { slot = &s; break; }
    if (!slot) return;

    auto peers_on = [&](Side side) {
        std::vector<Slot*> peers;
        for (auto& s : slots)
            if (&s != slot && s.side == side && s.group == slot->group)
                peers.push_back(&s);
        std::sort(peers.begin(), peers.end(), [](Slot const* a, Slot const* b) { return a->order < b->order; });
        return peers;
    };

    int i = 0;
    for (Slot* peer : peers_on(slot->side))
        peer->order = i++;

    slot->side = target_side;

    std::vector<Slot*> const target_peers = peers_on(target_side);
    int const clamped = std::clamp(target_index_in_group, 0, static_cast<int>(target_peers.size()));
    for (int j = 0; j < static_cast<int>(target_peers.size()); ++j)
        target_peers[j]->order = j < clamped ? j : j + 1;
    slot->order = clamped;

    rebuildOrder();
}

ImVec2 Layout::computeCursor(Side side, const ImRect& bounds, float accumulated, ImVec2 slot_size, Align align) const {
    auto along = [&](float mn, float center, float mx, float length) -> float {
        switch (align) {
        case AlignCenter: return center - length / 2.f;
        case AlignEnd: return mx - length;
        case AlignStart:
        default: return mn;
        }
    };
    auto along_h = [&](float length) { return along(bounds.Min.x, (bounds.Min.x + bounds.Max.x) * 0.5f, bounds.Max.x, length); };
    auto along_v = [&](float length) { return along(bounds.Min.y, (bounds.Min.y + bounds.Max.y) * 0.5f, bounds.Max.y, length); };

    ImVec2 r{};
    switch (side) {
    case SideLeft: r = { bounds.Min.x - accumulated - slot_size.x, along_v(slot_size.y) }; break;
    case SideRight: r = { bounds.Max.x + accumulated, along_v(slot_size.y) }; break;
    case SideTop: r = { along_h(slot_size.x), bounds.Min.y - accumulated - slot_size.y }; break;
    case SideBottom: r = { along_h(slot_size.x), bounds.Max.y + accumulated }; break;
    }
    return floorPos(r);
}

void Layout::rebuildOrder() {
    for (int s = 0; s < 4; ++s) {
        bars_by_side[s].clear();
        texts_by_side[s].clear();
    }
    for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        Slot const& s = slots[i];
        if (s.group == GroupBox) continue;
        int const si = static_cast<int>(s.side);
        (s.group == GroupBar ? bars_by_side[si] : texts_by_side[si]).push_back(i);
    }
    auto by_order = [this](int a, int b) { return slots[a].order < slots[b].order; };
    for (int s = 0; s < 4; ++s) {
        std::sort(bars_by_side[s].begin(), bars_by_side[s].end(), by_order);
        std::sort(texts_by_side[s].begin(), texts_by_side[s].end(), by_order);
    }
}

void Layout::computeLayout(const ImRect& bounds, float alpha, void* user, bool animated) const {
    scratch_sizes.assign(slots.size(), ImVec2(0.f, 0.f));
    scratch_positions.assign(slots.size(), ImVec2(0.f, 0.f));

    struct Pad { float box_to_text; float bar_to_text; float between_texts; };
    constexpr Pad paddings[4] = {
        { 1.f, 2.f, 1.f },
        { 1.f, 0.f, 1.f },
        { 3.f, 2.f, 1.f },
        { 4.f, 3.f, 0.f },
    };

    auto reveal = [animated](Slot const& s) { return animated ? s.vis_alpha : (s.isOn() ? 1.f : 0.f); };

    for (int side_i = 0; side_i < 4; ++side_i) {
        Side const side = static_cast<Side>(side_i);
        bool const horizontal_side = side == SideTop || side == SideBottom;
        Pad const& pad = paddings[side_i];

        // bars stack outward from the box edge
        float bar_offset = 2.f;
        bool any_bar = false;
        for (int const i : bars_by_side[side_i]) {
            Slot const& s = slots[i];
            float const va = reveal(s);
            if (!animated && va <= 0.f) continue;

            ImVec2 const sz = s.measure(bounds, side, alpha, user, s.scale);
            scratch_sizes[i] = sz;
            scratch_positions[i] = computeCursor(side, bounds, bar_offset, sz, s.align);
            float const thickness = horizontal_side ? sz.y : sz.x;
            if (thickness <= 0.f) continue;

            any_bar = true;
            bar_offset += thickness * va + 1.f * va;
        }

        float const text_gap = any_bar ? bar_offset - 1.f + pad.bar_to_text : pad.box_to_text;
        float text_offset = horizontal_side ? text_gap : bounds.Min.y - 2.f;
        for (int const i : texts_by_side[side_i]) {
            Slot const& s = slots[i];
            float const va = reveal(s);
            if (!animated && va <= 0.f) continue;

            ImVec2 const sz = s.measure(bounds, side, alpha, user, s.scale);
            scratch_sizes[i] = sz;
            if (horizontal_side)
                scratch_positions[i] = computeCursor(side, bounds, text_offset, { sz.x * va, sz.y * va }, AlignCenter);
            else
                scratch_positions[i] = floorPos({ side == SideLeft ? bounds.Min.x - text_gap - sz.x : bounds.Max.x + text_gap, text_offset });

            if (sz.y > 0.f)
                text_offset += sz.y * va + pad.between_texts * va;
        }
    }
}

void Layout::hitTestDrop(const ImRect& bounds, ImVec2 mouse, Group drag_group, Side& out_side, int& out_index) {
    float const dl = mouse.x - bounds.Min.x;
    float const dr = bounds.Max.x - mouse.x;
    float const dt = mouse.y - bounds.Min.y;
    float const db = bounds.Max.y - mouse.y;
    float const min_d = std::min(std::min(dl, dr), std::min(dt, db));

    if (min_d == dl) out_side = SideLeft;
    else if (min_d == dr) out_side = SideRight;
    else if (min_d == dt) out_side = SideTop;
    else out_side = SideBottom;

    auto perp_distance = [&](ImVec2 p) {
        switch (out_side) {
        case SideTop: return bounds.Min.y - p.y;
        case SideBottom: return p.y - bounds.Max.y;
        case SideLeft: return bounds.Min.x - p.x;
        case SideRight: return p.x - bounds.Max.x;
        }
        return 0.f;
    };

    std::vector<Slot*> peers;
    for (auto& s : slots)
        if (s.id != dragging_id && s.side == out_side && s.group == drag_group && s.isOn())
            peers.push_back(&s);
    std::sort(peers.begin(), peers.end(), [](Slot const* a, Slot const* b) { return a->order < b->order; });

    int idx = 0;
    if (drag_group == GroupBar) {
        float const cursor_d = perp_distance(mouse);
        for (Slot const* p : peers)
            if (cursor_d > perp_distance({ p->target_pos.x + p->size.x * 0.5f, p->target_pos.y + p->size.y * 0.5f }))
                ++idx;
    } else {
        bool const top_side = out_side == SideTop;
        for (Slot const* p : peers) {
            float const cy = p->target_pos.y + p->size.y * 0.5f;
            if (top_side ? cy > mouse.y : cy < mouse.y)
                ++idx;
        }
    }
    out_index = idx;
}

void Layout::render(ImDrawList* dl, const ImRect& bounds, float alpha, void* user) {
    ImGuiIO const& io = ImGui::GetIO();
    float const t = 1.f - std::exp(-14.f * io.DeltaTime);
    ImVec2 const mouse = io.MousePos;

    if (has_last_bounds) {
        ImVec2 const delta{ bounds.Min.x - last_bounds_origin.x, bounds.Min.y - last_bounds_origin.y };
        if (delta.x != 0.f || delta.y != 0.f) {
            for (auto& s : slots)
                if (s.anim_initialized && s.id != dragging_id) {
                    s.anim_pos.x += delta.x;
                    s.anim_pos.y += delta.y;
                }
        }
    }
    last_bounds_origin = bounds.Min;
    has_last_bounds = true;

    for (auto& s : slots)
        s.vis_alpha = ImLerp(s.vis_alpha, s.isOn() ? 1.f : 0.f, t);

    bool const shift_held = io.KeyShift || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    if (resizing_id >= 0) {
        Slot* resized = nullptr;
        for (auto& s : slots)
            if (s.id == resizing_id) { resized = &s; break; }
        if (resized) {
            float const dx = mouse.x - resize_start_mouse.x;
            float const dy = mouse.y - resize_start_mouse.y;
            float delta = 0.f;
            switch (resized->side) {
            case SideTop:    delta =  dx - dy; break;
            case SideBottom: delta =  dx + dy; break;
            case SideLeft:   delta = -dx + dy; break;
            case SideRight:  delta =  dx + dy; break;
            }
            resized->scale = std::clamp(resize_start_scale + delta * 0.008f, 0.5f, 3.0f);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            resizing_id = -1;
    } else if (dragging_id >= 0) {
        Slot* dragged = nullptr;
        for (auto& s : slots)
            if (s.id == dragging_id) { dragged = &s; break; }
        if (dragged) {
            Side new_side = dragged->side;
            int new_index = 0;
            hitTestDrop(bounds, mouse, dragged->group, new_side, new_index);
            moveSlot(dragging_id, new_side, new_index);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            dragging_id = -1;
            drag_prev_size = { 0.f, 0.f };
        }
    } else {
        hovered_id = -1;
        for (auto const& s : slots) {
            if (!s.isOn() || s.vis_alpha < 0.5f || !s.draggable) continue;
            ImRect rect{ s.anim_pos, { s.anim_pos.x + s.size.x, s.anim_pos.y + s.size.y } };
            rect.Expand(ImVec2(2, 2));
            if (rect.Contains(mouse)) { hovered_id = s.id; break; }
        }
        if (hovered_id >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            for (auto const& s : slots) {
                if (s.id != hovered_id) continue;
                if (shift_held) {
                    resizing_id = s.id;
                    resize_start_mouse = mouse;
                    resize_start_scale = s.scale;
                } else {
                    dragging_id = s.id;
                    drag_grab_offset = { mouse.x - s.anim_pos.x, mouse.y - s.anim_pos.y };
                    drag_prev_size = s.size;
                }
                break;
            }
        } else if (hovered_id >= 0 && shift_held && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            for (auto& s : slots)
                if (s.id == hovered_id) { s.scale = 1.0f; break; }
        }
    }

    computeLayout(bounds, alpha, user, true);

    for (size_t i = 0; i < slots.size(); ++i) {
        slots[i].size = scratch_sizes[i];
        slots[i].target_pos = scratch_positions[i];
    }

    if (dragging_id >= 0) {
        for (auto& s : slots) {
            if (s.id != dragging_id) continue;
            ImVec2 const sz = s.size;
            bool const aspect_changed = sz.x > 0.f && sz.y > 0.f && drag_prev_size.x > 0.f && drag_prev_size.y > 0.f &&
                                        (sz.x > sz.y) != (drag_prev_size.x > drag_prev_size.y);
            bool const out_of_bounds = drag_grab_offset.x < 0.f || drag_grab_offset.x > sz.x ||
                                       drag_grab_offset.y < 0.f || drag_grab_offset.y > sz.y;
            if (aspect_changed || out_of_bounds)
                drag_grab_offset = { sz.x * 0.5f, sz.y * 0.5f };
            drag_prev_size = sz;
            break;
        }
    }

    for (auto& s : slots) {
        s.hover_alpha = ImLerp(s.hover_alpha, (s.id == hovered_id || s.id == dragging_id || s.id == resizing_id) ? 1.f : 0.f, t);

        if (!s.anim_initialized) {
            s.anim_pos = s.target_pos;
            s.anim_initialized = true;
            continue;
        }

        if (s.id == dragging_id) {
            s.anim_pos = { mouse.x - drag_grab_offset.x, mouse.y - drag_grab_offset.y };
            continue;
        }

        s.anim_pos = ImLerp(s.anim_pos, s.target_pos, t);
        if (std::abs(s.anim_pos.x - s.target_pos.x) < 0.5f && std::abs(s.anim_pos.y - s.target_pos.y) < 0.5f)
            s.anim_pos = s.target_pos;
    }

    for (auto const& s : slots) {
        if (s.hover_alpha < 0.01f) continue;
        ImRect rect{ floorPos(s.anim_pos), { floorPos(s.anim_pos).x + s.size.x, floorPos(s.anim_pos).y + s.size.y } };
        rect.Expand(ImVec2(2, 2));
        float const a = s.hover_alpha * 0.6f;
        bool const resize_col = (s.id == resizing_id) || (s.id == hovered_id && shift_held);
        ImU32 const fill_c = resize_col ? IM_COL32(250, 180, 60, (int)(55 * a)) : IM_COL32(90, 160, 255, (int)(50 * a));
        ImU32 const edge_c = resize_col ? IM_COL32(250, 180, 60, (int)(200 * a)) : IM_COL32(90, 160, 255, (int)(180 * a));
        dl->AddRectFilled(rect.Min, rect.Max, fill_c, 2.f);
        dl->AddRect(rect.Min, rect.Max, edge_c, 2.f);
    }

    Slot const* dragged_render_last = nullptr;
    for (auto const& s : slots) {
        if (s.vis_alpha < 0.01f) continue;
        if (s.id == dragging_id) { dragged_render_last = &s; continue; }
        s.render(dl, bounds, s.side, floorPos(s.anim_pos), alpha, s.vis_alpha, user, s.scale);
    }

    if (dragged_render_last) {
        ImVec2 const tp = dragged_render_last->target_pos;
        ImVec2 const tsz = dragged_render_last->size;
        dl->AddRect({ tp.x - 1, tp.y - 1 }, { tp.x + tsz.x + 1, tp.y + tsz.y + 1 }, IM_COL32(255, 255, 255, 110), 1.f);
        dragged_render_last->render(dl, bounds, dragged_render_last->side, floorPos(dragged_render_last->anim_pos), alpha, dragged_render_last->vis_alpha, user, dragged_render_last->scale);
    }

    if (resizing_id >= 0 || (hovered_id >= 0 && shift_held)) {
        int const show_id = resizing_id >= 0 ? resizing_id : hovered_id;
        for (auto const& s : slots) {
            if (s.id != show_id) continue;
            char badge[32];
            snprintf(badge, sizeof(badge), "%.2fx", s.scale);
            ImVec2 const bp(s.anim_pos.x + s.size.x + 6.f, s.anim_pos.y - 2.f);
            ImVec2 const bsz = ImGui::CalcTextSize(badge);
            dl->AddRectFilled({ bp.x - 4.f, bp.y - 2.f }, { bp.x + bsz.x + 4.f, bp.y + bsz.y + 2.f }, IM_COL32(14, 14, 20, 220), 3.f);
            dl->AddRect({ bp.x - 4.f, bp.y - 2.f }, { bp.x + bsz.x + 4.f, bp.y + bsz.y + 2.f }, IM_COL32(250, 180, 60, 200), 3.f);
            dl->AddText(bp, IM_COL32(255, 230, 170, 255), badge);
            break;
        }
    }
}

void Layout::renderStatic(ImDrawList* dl, const ImRect& bounds, float alpha, void* user) const {
    computeLayout(bounds, alpha, user, false);
    for (size_t i = 0; i < slots.size(); ++i) {
        Slot const& s = slots[i];
        if (s.isOn())
            s.render(dl, bounds, s.side, floorPos(scratch_positions[i]), alpha, 1.f, user, s.scale);
    }
}

// ---------------------------------------------------------------------------
// our slot set - visuals are 1:1 with the pre-layout DrawESP element drawing
// ---------------------------------------------------------------------------

namespace {

ImU32 Col(float const c[4], float alpha, bool gray, float gray_amt) {
    float r = c[0], g = c[1], b = c[2];
    if (gray) {
        float const y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        r += (y - r) * gray_amt;
        g += (y - g) * gray_amt;
        b += (y - b) * gray_amt;
    }
    float a = c[3] * alpha;
    if (a < 0.f) a = 0.f;
    if (a > 1.f) a = 1.f;
    return ImColor(r, g, b, a);
}

ImU32 White(float alpha, bool, float) {
    return ImColor(255, 255, 255, (int)(255 * alpha));
}

void ShadowText(ImDrawList* dl, ImVec2 pos, ImU32 col, char const* text, float alpha, float font_size) {
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, font_size, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, (int)(120 * alpha)), text);
    dl->AddText(font, font_size, pos, col, text);
}

bool verticalSide(Side side) { return side == SideLeft || side == SideRight; }

float ScaledFont(float base_size, float scale) {
    return std::max(7.f, std::round(base_size * scale));
}

ImVec2 TextSizeScaled(float font_size, char const* text) {
    return ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.f, text);
}

// gradient fill for a bar (two-colour ramp like the pre-layout ESP)
void GradientFill(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool gradient, float const* lo_c, float const* hi_c,
                  float alpha, bool gray, float gray_amt, bool horizontal) {
    if (gradient) {
        ImU32 const lo = Col(lo_c, alpha, gray, gray_amt);
        ImU32 const hi = Col(hi_c, alpha, gray, gray_amt);
        // AddRectFilledMultiColor(a=lo, b=hi, c=hi, d=lo) matches the old
        // horizontal bars; for vertical bars a(high)=hi too
        if (horizontal)
            dl->AddRectFilledMultiColor(mn, mx, lo, hi, hi, lo);
        else
            dl->AddRectFilledMultiColor(mn, mx, hi, hi, lo, lo);
    } else {
        dl->AddRectFilled(mn, mx, Col(hi_c, alpha, gray, gray_amt));
    }
}

// ---------------------------------------------------------------- bars

ImVec2 hp_measure(const ImRect& bounds, Side side, float, void*, float scale) {
    float const t = std::max(3.f, std::round(4.f * scale));
    return verticalSide(side) ? ImVec2{ t, bounds.GetHeight() } : ImVec2{ bounds.GetWidth(), t };
}

void hp_render(ImDrawList* dl, const ImRect& bounds, Side side, ImVec2 cursor, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->alive) return;

    float const t = std::max(3.f, std::round(4.f * scale));
    ImRect r;
    if (verticalSide(side))
        r = { { cursor.x + 1.f, cursor.y }, { cursor.x + t - 1.f, cursor.y + bounds.GetHeight() } };
    else
        r = { { cursor.x, cursor.y + 1.f }, { cursor.x + bounds.GetWidth(), cursor.y + t - 1.f } };

    dl->AddRectFilled(r.Min, r.Max, IM_COL32(0, 0, 0, (int)(100 * alpha * vis_alpha)));
    if (v->health > 0.f) {
        float const frac = std::clamp(v->health, 0.f, 1.f);
        if (verticalSide(side)) {
            float const top = r.Max.y - r.GetHeight() * frac;
            GradientFill(dl, { r.Min.x, top }, r.Max, cfg.esp_health_gradient, cfg.esp_health_min, cfg.esp_health_max,
                         alpha * vis_alpha, v->gray, v->gray_amt, false);
        } else {
            ImVec2 const fill_end(r.Min.x + r.GetWidth() * frac, r.Max.y);
            GradientFill(dl, r.Min, fill_end, cfg.esp_health_gradient, cfg.esp_health_min, cfg.esp_health_max,
                         alpha * vis_alpha, v->gray, v->gray_amt, true);
        }
    }
    dl->AddRect({ r.Min.x - 1, r.Min.y - 1 }, { r.Max.x + 1, r.Max.y + 1 }, IM_COL32(0, 0, 0, (int)(120 * alpha * vis_alpha)));
}

ImVec2 armor_measure(const ImRect& bounds, Side side, float, void*, float scale) {
    float const t = std::max(2.f, std::round(3.f * scale));
    return verticalSide(side) ? ImVec2{ t, bounds.GetHeight() } : ImVec2{ bounds.GetWidth(), t };
}

void armor_render(ImDrawList* dl, const ImRect& bounds, Side side, ImVec2 cursor, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->alive) return;

    float const t = std::max(2.f, std::round(3.f * scale));
    ImRect r;
    if (verticalSide(side))
        r = { { cursor.x, cursor.y }, { cursor.x + t - 1.f, cursor.y + bounds.GetHeight() } };
    else
        r = { { cursor.x, cursor.y }, { cursor.x + bounds.GetWidth(), cursor.y + t - 1.f } };

    dl->AddRectFilled(r.Min, r.Max, IM_COL32(0, 0, 0, (int)(120 * alpha * vis_alpha)));
    if (v->armor > 0) {
        float const frac = std::clamp(v->armor / 100.f, 0.f, 1.f);
        if (verticalSide(side)) {
            float const top = r.Max.y - r.GetHeight() * frac;
            GradientFill(dl, { r.Min.x, top }, r.Max, cfg.esp_armor_gradient, cfg.esp_armor_min, cfg.esp_armor_max,
                         alpha * vis_alpha, v->gray, v->gray_amt, false);
        } else {
            ImVec2 const fill_end(r.Min.x + r.GetWidth() * frac, r.Max.y);
            GradientFill(dl, r.Min, fill_end, cfg.esp_armor_gradient, cfg.esp_armor_min, cfg.esp_armor_max,
                         alpha * vis_alpha, v->gray, v->gray_amt, true);
        }
    }
    dl->AddRect({ r.Min.x - 1, r.Min.y - 1 }, { r.Max.x + 1, r.Max.y + 1 }, IM_COL32(0, 0, 0, (int)(160 * alpha * vis_alpha)));
}

// ---------------------------------------------------------------- texts

ImVec2 name_measure(const ImRect&, Side, float, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->name) return { 0.f, 0.f };
    float const fs = ScaledFont(ImGui::GetFontSize(), scale);
    ImVec2 const ts = TextSizeScaled(fs, v->name);
    return { ts.x, ts.y };
}

void name_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->name) return;
    float const fs = ScaledFont(ImGui::GetFontSize(), scale);
    ShadowText(dl, pos, Col(cfg.esp_name_color, alpha * vis_alpha, v->gray, v->gray_amt), v->name, alpha * vis_alpha, fs);
}

// undefeated glyph for the weapon name (same table as the old DrawESP)
char const* WeaponGlyph(char const* name) {
    if (!name) return nullptr;
    struct G { char const* name; char const* glyph; };
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

ImFont* IconFont() {
    auto& iconf = alice::core->fonts->get("undefeated");
    return iconf.ptr;
}

float IconFontSize() {
    auto& iconf = alice::core->fonts->get("undefeated");
    return iconf.size;
}

ImVec2 weapon_measure(const ImRect&, Side, float, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->has_weapon || !v->weapon) return { 0.f, 0.f };
    float const fs = ScaledFont(ImGui::GetFontSize(), scale);
    ImVec2 const ts = TextSizeScaled(fs, v->weapon);
    return { ts.x, ts.y };
}

void weapon_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->has_weapon || !v->weapon) return;
    float const fs = ScaledFont(ImGui::GetFontSize(), scale);
    ShadowText(dl, pos, Col(cfg.esp_weapon_color, alpha * vis_alpha, v->gray, v->gray_amt), v->weapon, alpha * vis_alpha, fs);
}

// independent weapon-icon slot (draws even with the weapon text off)
ImVec2 wicon_measure(const ImRect&, Side, float, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->has_weapon || !v->weapon) return { 0.f, 0.f };
    char const* g = WeaponGlyph(v->weapon);
    if (!g) return { 0.f, 0.f };
    ImFont* f = IconFont();
    if (!f) return { 0.f, 0.f };
    float const fs = ScaledFont(IconFontSize(), scale);
    ImVec2 const isz = f->CalcTextSizeA(fs, FLT_MAX, 0.f, g);
    return { isz.x, isz.y };
}

void wicon_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->has_weapon || !v->weapon) return;
    char const* g = WeaponGlyph(v->weapon);
    if (!g) return;
    ImFont* f = IconFont();
    if (!f) return;
    float const a = alpha * vis_alpha;
    float const fs = ScaledFont(IconFontSize(), scale);
    ImU32 const col = Col(cfg.esp_weapon_icon_color, a, v->gray, v->gray_amt);
    dl->AddText(f, fs, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, (int)(150 * a)), g);
    dl->AddText(f, fs, pos, col, g);
}

ImVec2 ammo_measure(const ImRect&, Side, float, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->has_ammo || !v->ammo_text[0]) return { 0.f, 0.f };
    float const fs = ScaledFont(11.f, scale);
    ImVec2 const ts = TextSizeScaled(fs, v->ammo_text);
    return { ts.x, fs };
}

void ammo_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->has_ammo || !v->ammo_text[0]) return;
    float const fs = ScaledFont(11.f, scale);
    ImU32 const col = Col(cfg.esp_ammo_max, alpha * vis_alpha, v->gray, v->gray_amt);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, (int)(140 * alpha)), v->ammo_text);
    dl->AddText(ImGui::GetFont(), fs, pos, col, v->ammo_text);
}

ImVec2 distance_measure(const ImRect&, Side, float, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->distance_text[0]) return { 0.f, 0.f };
    float const fs = ScaledFont(ImGui::GetFontSize(), scale);
    ImVec2 const ts = TextSizeScaled(fs, v->distance_text);
    return { ts.x, ts.y };
}

void distance_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->distance_text[0]) return;
    float const fs = ScaledFont(ImGui::GetFontSize(), scale);
    ShadowText(dl, pos, White(alpha * vis_alpha, v->gray, v->gray_amt), v->distance_text, alpha * vis_alpha, fs);
}

// ------------------------------------------------------------- box + icons

void box_render(ImDrawList* dl, const ImRect& bounds, Side, ImVec2, float alpha, float vis_alpha, void* user, float) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    float const a = alpha * vis_alpha;
    ImU32 const outline = Col(cfg.esp_box_outline, a * 0.75f, v ? v->gray : false, v ? v->gray_amt : 0.f);
    dl->AddRect({ bounds.Min.x + 1, bounds.Min.y + 1 }, { bounds.Max.x - 1, bounds.Max.y - 1 }, outline);
    dl->AddRect({ bounds.Min.x - 1, bounds.Min.y - 1 }, { bounds.Max.x + 1, bounds.Max.y + 1 }, outline);
    dl->AddRect(bounds.Min, bounds.Max, Col(cfg.esp_box_color, a, v ? v->gray : false, v ? v->gray_amt : 0.f));
}

ImVec2 kit_measure(const ImRect&, Side, float, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->kit_icon) return { 0.f, 0.f };
    auto& iconf = alice::core->fonts->get("undefeated");
    if (!iconf.ptr) return { 0.f, 0.f };
    float const fs = ScaledFont(iconf.size, scale);
    ImVec2 const ts = iconf.ptr->CalcTextSizeA(fs, FLT_MAX, 0, "r");
    return { ts.x, ts.y };
}

void kit_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->kit_icon) return;
    auto& iconf = alice::core->fonts->get("undefeated");
    if (!iconf.ptr) return;
    float const fs = ScaledFont(iconf.size, scale);
    ImU32 const col = Col(cfg.esp_kit_icon_color, alpha * vis_alpha, v->gray, v->gray_amt);
    dl->AddText(iconf.ptr, fs, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, (int)(150 * alpha)), "r");
    dl->AddText(iconf.ptr, fs, pos, col, "r");
}

ImVec2 c4icon_measure(const ImRect&, Side, float, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->c4_icon) return { 0.f, 0.f };
    auto& iconf = alice::core->fonts->get("undefeated");
    if (!iconf.ptr) return { 0.f, 0.f };
    float const fs = ScaledFont(iconf.size, scale);
    ImVec2 const ts = iconf.ptr->CalcTextSizeA(fs, FLT_MAX, 0, "o");
    return { ts.x, ts.y };
}

void c4icon_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->c4_icon) return;
    auto& iconf = alice::core->fonts->get("undefeated");
    if (!iconf.ptr) return;
    float const fs = ScaledFont(iconf.size, scale);
    ImU32 const col = Col(cfg.esp_c4_icon_color, alpha * vis_alpha, v->gray, v->gray_amt);
    dl->AddText(iconf.ptr, fs, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, (int)(150 * alpha)), "o");
    dl->AddText(iconf.ptr, fs, pos, col, "o");
}

ImVec2 taser_measure(const ImRect&, Side, float, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->taser_icon) return { 0.f, 0.f };
    auto& iconf = alice::core->fonts->get("undefeated");
    if (!iconf.ptr) return { 0.f, 0.f };
    float const fs = ScaledFont(iconf.size, scale);
    ImVec2 const ts = iconf.ptr->CalcTextSizeA(fs, FLT_MAX, 0, "h");
    return { ts.x, ts.y };
}

void taser_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha, float vis_alpha, void* user, float scale) {
    PlayerView const* v = static_cast<PlayerView const*>(user);
    if (!v || !v->taser_icon) return;
    auto& iconf = alice::core->fonts->get("undefeated");
    if (!iconf.ptr) return;
    float const fs = ScaledFont(iconf.size, scale);
    ImU32 const col = Col(cfg.esp_taser_icon_color, alpha * vis_alpha, v->gray, v->gray_amt);
    dl->AddText(iconf.ptr, fs, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, (int)(150 * alpha)), "h");
    dl->AddText(iconf.ptr, fs, pos, col, "h");
}

// ---------------------------------------------------------------- flags

struct FlagSlotData {
    char const* name;
    const char* PlayerView::* text_ptr;
    float const* color;
};

FlagSlotData g_flag_slots[6] = {
    { "Money",     &PlayerView::flag_money,   cfg.flag_money_color },
    { "ArmorFlag", &PlayerView::flag_armor,   cfg.flag_armor_color },
    { "Kit",       &PlayerView::flag_kit,     cfg.flag_kit_color },
    { "Defuse",    &PlayerView::flag_defuse,  cfg.flag_defuse_color },
    { "Flashed",   &PlayerView::flag_flashed, cfg.flag_flashed_color },
    { "C4",        &PlayerView::flag_c4,      cfg.flag_c4_color },
};

// per-flag measure/render pairs (text_ptr + color baked into lambdas is not
// possible with function pointers, so one pair per flag)
#define FLAG_PAIR(idx)                                                                      \
    ImVec2 flag##idx##_measure(const ImRect&, Side, float, void* user, float scale) {       \
        PlayerView const* v = static_cast<PlayerView const*>(user);                         \
        if (!v || !(v->*g_flag_slots[idx].text_ptr)) return { 0.f, 0.f };                   \
        float const fs = ScaledFont(11.f, scale);                                           \
        ImVec2 const ts = TextSizeScaled(fs, v->*g_flag_slots[idx].text_ptr);              \
        return { ts.x, fs };                                                                \
    }                                                                                       \
    void flag##idx##_render(ImDrawList* dl, const ImRect&, Side, ImVec2 pos, float alpha,   \
                            float vis_alpha, void* user, float scale) {                     \
        PlayerView const* v = static_cast<PlayerView const*>(user);                         \
        if (!v || !(v->*g_flag_slots[idx].text_ptr)) return;                                \
        float const fs = ScaledFont(11.f, scale);                                           \
        ShadowText(dl, pos,                                                                 \
                   Col(g_flag_slots[idx].color, alpha * vis_alpha, v->gray, v->gray_amt),   \
                   v->*g_flag_slots[idx].text_ptr, alpha * vis_alpha, fs);                  \
    }

FLAG_PAIR(0)
FLAG_PAIR(1)
FLAG_PAIR(2)
FLAG_PAIR(3)
FLAG_PAIR(4)
FLAG_PAIR(5)

} // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

namespace esp_layout {

static Layout g_layout;
static bool g_built = false;

void Init() {
    if (g_built) return;
    g_built = true;

    g_layout.bindEnabled(g_layout.addBox("Box", box_render), &cfg.esp_box);
    g_layout.bindEnabled(g_layout.addText("Name", SideTop, 0, name_measure, name_render), &cfg.esp_name);
    g_layout.bindEnabled(g_layout.addBar("HP", SideLeft, 0, hp_measure, hp_render), &cfg.esp_health);
    g_layout.bindEnabled(g_layout.addBar("Armor", SideBottom, 0, armor_measure, armor_render), &cfg.esp_armor);
    g_layout.bindEnabled(g_layout.addText("Weapon", SideBottom, 1, weapon_measure, weapon_render), &cfg.esp_weapon);
    g_layout.bindEnabled(g_layout.addText("Weapon icon", SideBottom, 2, wicon_measure, wicon_render), &cfg.esp_weapon_icon);
    g_layout.bindEnabled(g_layout.addText("Ammo", SideBottom, 3, ammo_measure, ammo_render), &cfg.esp_ammo);
    g_layout.bindEnabled(g_layout.addText("Distance", SideBottom, 4, distance_measure, distance_render), &cfg.esp_distance);
    g_layout.bindEnabled(g_layout.addText("Kit icon", SideLeft, 1, kit_measure, kit_render), &cfg.esp_kit_icon);
    g_layout.bindEnabled(g_layout.addText("C4 icon", SideLeft, 2, c4icon_measure, c4icon_render), &cfg.esp_c4_icon);
    g_layout.bindEnabled(g_layout.addText("Taser icon", SideLeft, 3, taser_measure, taser_render), &cfg.esp_taser_icon);

    static MeasureFn const flag_measures[6] = { flag0_measure, flag1_measure, flag2_measure, flag3_measure, flag4_measure, flag5_measure };
    static RenderFn const flag_renders[6] = { flag0_render, flag1_render, flag2_render, flag3_render, flag4_render, flag5_render };
    static bool* const flag_refs[6] = { &cfg.flag_money, &cfg.flag_armor, &cfg.flag_kit, &cfg.flag_defuse, &cfg.flag_flashed, &cfg.flag_c4 };
    for (int i = 0; i < 6; i++)
        g_layout.bindEnabled(g_layout.addText(g_flag_slots[i].name, SideRight, i, flag_measures[i], flag_renders[i], AlignStart), flag_refs[i]);

    g_layout.deserialize(cfg.esp_layout);
}

Layout& Get() {
    Init();
    return g_layout;
}

void SerializeToConfig() {
    Init();
    std::string const data = g_layout.serialize();
    strncpy(cfg.esp_layout, data.c_str(), sizeof(cfg.esp_layout) - 1);
    cfg.esp_layout[sizeof(cfg.esp_layout) - 1] = 0;
}

void ReloadFromConfig() {
    Init();
    g_layout.deserialize(cfg.esp_layout);
}

void RenderStatic(ImDrawList* dl, const ImVec2& box_min, const ImVec2& box_max, float alpha, PlayerView& view) {
    Init();
    ImRect const bounds{ box_min, box_max };
    g_layout.renderStatic(dl, bounds, alpha, &view);
}

void RenderPreview(ImDrawList* dl, const ImVec2& box_min, const ImVec2& box_max, float alpha, PlayerView& view) {
    Init();
    ImRect const bounds{ box_min, box_max };
    g_layout.render(dl, bounds, alpha, &view);
    SerializeToConfig();
}

} // namespace esp_layout
