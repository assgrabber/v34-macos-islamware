#include "cheat.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal GLB (glTF 2.0 binary) preview renderer for the ESP preview pane.
// The model is embedded at build time (build/glb_data.cpp -> sources/
// t-leet_face_rig.glb).  Parses POSITION + indices + baseColorFactor, bakes
// flat triangles, then renders them flat-shaded with a painter's sort into an
// ImGui draw list.  No skinning - the bind pose is used as-is.
// ---------------------------------------------------------------------------

extern const unsigned char g_glb_model_data[];
extern const unsigned int g_glb_model_size;

namespace glb_preview {

namespace {

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    JVal const* get(const char* k) const {
        if (type != Obj) return nullptr;
        for (auto const& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    int i(int def = 0) const { return type == Num ? (int)num : def; }
    double d(double def = 0) const { return type == Num ? num : def; }
};

struct Parser {
    char const* p;
    char const* end;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }

    bool pstr(std::string& out) {
        out.clear();
        p++; // opening quote
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int k = 0; k < 4 && p < end; k++) {
                    char h = *p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                }
                out += (cp < 128) ? (char)cp : '?';
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }

    bool parse(JVal& v) {
        ws();
        if (p >= end) return false;
        char c = *p;
        if (c == '{') {
            v.type = JVal::Obj;
            p++;
            ws();
            if (p < end && *p == '}') { p++; return true; }
            while (p < end) {
                ws();
                if (p >= end || *p != '"') return false;
                std::string k;
                if (!pstr(k)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                p++;
                JVal child;
                if (!parse(child)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return false;
            }
            return false;
        }
        if (c == '[') {
            v.type = JVal::Arr;
            p++;
            ws();
            if (p < end && *p == ']') { p++; return true; }
            while (p < end) {
                JVal child;
                if (!parse(child)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return false;
            }
            return false;
        }
        if (c == '"') { v.type = JVal::Str; return pstr(v.str); }
        if (c == 't') { if (end - p < 4) return false; v.type = JVal::Bool; v.b = true; p += 4; return true; }
        if (c == 'f') { if (end - p < 5) return false; v.type = JVal::Bool; v.b = false; p += 5; return true; }
        if (c == 'n') { if (end - p < 4) return false; v.type = JVal::Null; p += 4; return true; }
        {
            char* e = nullptr;
            v.type = JVal::Num;
            v.num = strtod(p, &e);
            if (e == p) return false;
            p = e;
            return true;
        }
    }
};

struct Tri {
    float a[3];
    float b[3];
    float c[3];
    unsigned char r, g, b_col;
};

std::vector<Tri> g_tris;
bool g_loaded = false;
bool g_failed = false;

bool readAccessor(JVal const& root, unsigned char const* bin, size_t bin_size, int acc_idx,
                  std::vector<float>& out, int& comps) {
    JVal const* accessors = root.get("accessors");
    JVal const* views = root.get("bufferViews");
    if (!accessors || !views || acc_idx < 0 || acc_idx >= (int)accessors->arr.size())
        return false;
    JVal const& a = accessors->arr[acc_idx];
    JVal const* bv = a.get("bufferView");
    if (!bv || bv->i() < 0 || bv->i() >= (int)views->arr.size()) return false;
    JVal const& v = views->arr[bv->i()];
    size_t const off = (size_t)(v.get("byteOffset") ? v.get("byteOffset")->i() : 0) +
                       (size_t)(a.get("byteOffset") ? a.get("byteOffset")->i() : 0);
    int const count = a.get("count") ? a.get("count")->i() : 0;
    std::string const t = a.get("type") ? a.get("type")->str : "";
    comps = t == "VEC3" ? 3 : t == "VEC2" ? 2 : t == "VEC4" ? 4 : t == "SCALAR" ? 1 : 0;
    if (!comps || count <= 0) return false;
    size_t const stride = v.get("byteStride") ? (size_t)v.get("byteStride")->i() : (size_t)(comps * 4);
    out.resize((size_t)count * (size_t)comps);
    for (int i = 0; i < count; i++) {
        size_t const base = off + (size_t)i * stride;
        if (base + (size_t)comps * 4 > bin_size) return false;
        memcpy(&out[(size_t)i * (size_t)comps], bin + base, (size_t)comps * 4);
    }
    return true;
}

bool readIndices(JVal const& root, unsigned char const* bin, size_t bin_size, int acc_idx,
                 std::vector<uint32_t>& out) {
    JVal const* accessors = root.get("accessors");
    JVal const* views = root.get("bufferViews");
    if (!accessors || !views || acc_idx < 0 || acc_idx >= (int)accessors->arr.size())
        return false;
    JVal const& a = accessors->arr[acc_idx];
    JVal const* bv = a.get("bufferView");
    if (!bv || bv->i() < 0 || bv->i() >= (int)views->arr.size()) return false;
    JVal const& v = views->arr[bv->i()];
    size_t const off = (size_t)(v.get("byteOffset") ? v.get("byteOffset")->i() : 0) +
                       (size_t)(a.get("byteOffset") ? a.get("byteOffset")->i() : 0);
    int const count = a.get("count") ? a.get("count")->i() : 0;
    int const comp = a.get("componentType") ? a.get("componentType")->i() : 0;
    int const elem = comp == 5125 ? 4 : comp == 5123 ? 2 : comp == 5121 ? 1 : 0;
    if (!elem || count <= 0) return false;
    if (off + (size_t)count * (size_t)elem > bin_size) return false;
    out.resize((size_t)count);
    for (int i = 0; i < count; i++) {
        unsigned char const* src = bin + off + (size_t)i * (size_t)elem;
        uint32_t val = 0;
        memcpy(&val, src, (size_t)elem);
        out[(size_t)i] = val;
    }
    return true;
}

void Load() {
    g_loaded = true;
    if (g_glb_model_size < 20 || memcmp(g_glb_model_data, "glTF", 4) != 0) {
        g_failed = true;
        return;
    }
    unsigned char const* d = g_glb_model_data;
    uint32_t json_len = 0;
    memcpy(&json_len, d + 12, 4);
    if (20u + json_len > g_glb_model_size) { g_failed = true; return; }
    char const* json = (char const*)(d + 20);

    uint32_t bin_len = 0;
    unsigned char const* bin = nullptr;
    size_t const bin_off = 20 + (size_t)json_len + 8;
    if (bin_off + 8 <= g_glb_model_size) {
        memcpy(&bin_len, d + 20 + json_len, 4);
        bin = d + bin_off;
        if (bin_off + bin_len > g_glb_model_size) bin_len = (uint32_t)(g_glb_model_size - bin_off);
    }
    if (!bin) { g_failed = true; return; }

    Parser ps{ json, json + json_len };
    JVal root;
    if (!ps.parse(root)) { g_failed = true; return; }

    JVal const* meshes = root.get("meshes");
    JVal const* materials = root.get("materials");
    if (!meshes || meshes->type != JVal::Arr) { g_failed = true; return; }

    for (auto const& mesh : meshes->arr) {
        JVal const* prims = mesh.get("primitives");
        if (!prims) continue;
        for (auto const& prim : prims->arr) {
            JVal const* attrs = prim.get("attributes");
            if (!attrs) continue;
            JVal const* pos = attrs->get("POSITION");
            JVal const* idx = prim.get("indices");
            if (!pos || !idx) continue;

            std::vector<float> verts;
            std::vector<uint32_t> inds;
            int comps = 0;
            if (!readAccessor(root, bin, bin_len, pos->i(), verts, comps) || comps != 3) continue;
            if (!readIndices(root, bin, bin_len, idx->i(), inds)) continue;

            unsigned char r = 150, g = 158, b = 172;
            JVal const* mat_idx = prim.get("material");
            if (materials && mat_idx && mat_idx->i() >= 0 && mat_idx->i() < (int)materials->arr.size()) {
                JVal const& m = materials->arr[mat_idx->i()];
                JVal const* pbr = m.get("pbrMetallicRoughness");
                if (pbr) {
                    JVal const* bcf = pbr->get("baseColorFactor");
                    if (bcf && bcf->type == JVal::Arr && bcf->arr.size() >= 3) {
                        r = (unsigned char)std::clamp((int)(bcf->arr[0].d() * 255.0), 0, 255);
                        g = (unsigned char)std::clamp((int)(bcf->arr[1].d() * 255.0), 0, 255);
                        b = (unsigned char)std::clamp((int)(bcf->arr[2].d() * 255.0), 0, 255);
                    }
                }
            }

            for (size_t i = 0; i + 2 < inds.size(); i += 3) {
                Tri t{};
                memcpy(t.a, &verts[(size_t)inds[i] * 3], 12);
                memcpy(t.b, &verts[(size_t)inds[i + 1] * 3], 12);
                memcpy(t.c, &verts[(size_t)inds[i + 2] * 3], 12);
                t.r = r; t.g = g; t.b_col = b;
                g_tris.push_back(t);
            }
        }
    }

    if (g_tris.empty()) { g_failed = true; return; }

    // normalise: centre at origin, span ~2 units
    float mn[3] = { 1e9f, 1e9f, 1e9f };
    float mx[3] = { -1e9f, -1e9f, -1e9f };
    for (auto const& t : g_tris) {
        float const* verts[3] = { t.a, t.b, t.c };
        for (float const* v : verts)
            for (int k = 0; k < 3; k++) {
                mn[k] = std::min(mn[k], v[k]);
                mx[k] = std::max(mx[k], v[k]);
            }
    }
    float const cx = (mn[0] + mx[0]) * 0.5f;
    float const cy = (mn[1] + mx[1]) * 0.5f;
    float const cz = (mn[2] + mx[2]) * 0.5f;
    float ext = std::max(std::max(mx[0] - mn[0], mx[1] - mn[1]), mx[2] - mn[2]);
    if (ext < 1e-6f) ext = 1.f;
    float const inv = 2.0f / ext;
    for (auto& t : g_tris) {
        float* verts[3] = { t.a, t.b, t.c };
        for (float* v : verts) {
            v[0] = (v[0] - cx) * inv;
            v[1] = (v[1] - cy) * inv;
            v[2] = (v[2] - cz) * inv;
        }
    }
}

struct ProjTri {
    ImVec2 p[3];
    float depth;
    ImU32 col;
};

} // namespace

// draws the embedded model centred on `center`, roughly `size` pixels tall
void Draw(ImDrawList* dl, ImVec2 center, float size) {
    if (!g_loaded)
        Load();
    if (g_failed || g_tris.empty())
        return;

    static float yaw = 0.4f;
    yaw += ImGui::GetIO().DeltaTime * 0.5f;

    float const cy = cosf(yaw);
    float const sy = sinf(yaw);
    float const pitch = -0.18f;
    float const cp = cosf(pitch);
    float const sp = sinf(pitch);

    float const scale = size * 0.5f;
    float const f = 3.0f;
    float const lx = 0.36f, ly = 0.75f, lz = 0.55f;

    std::vector<ProjTri> out;
    out.reserve(g_tris.size());

    for (auto const& t : g_tris) {
        float const* verts[3] = { t.a, t.b, t.c };
        float rv[3][3];
        ProjTri pt{};
        float depth = 0.f;
        bool behind = false;
        for (int k = 0; k < 3; k++) {
            float const* v = verts[k];
            // yaw about Y
            float x = v[0] * cy + v[2] * sy;
            float z = -v[0] * sy + v[2] * cy;
            float y = v[1];
            // pitch about X
            float const y2 = y * cp - z * sp;
            float const z2 = y * sp + z * cp;
            rv[k][0] = x;
            rv[k][1] = y2;
            rv[k][2] = z2;
            float const zc = z2 + 3.4f;
            if (zc < 0.4f) { behind = true; break; }
            pt.p[k] = ImVec2(center.x + x * f * scale / zc, center.y - y2 * f * scale / zc);
            depth += zc;
        }
        if (behind) continue;

        // screen-space degenerate cull
        float const ax = pt.p[1].x - pt.p[0].x, ay = pt.p[1].y - pt.p[0].y;
        float const bx = pt.p[2].x - pt.p[0].x, by = pt.p[2].y - pt.p[0].y;
        float const area = ax * by - ay * bx;
        if (area > -0.5f && area < 0.5f) continue;

        // lambert: face normal of the rotated triangle vs a fixed light
        float e1[3] = { rv[1][0] - rv[0][0], rv[1][1] - rv[0][1], rv[1][2] - rv[0][2] };
        float e2[3] = { rv[2][0] - rv[0][0], rv[2][1] - rv[0][1], rv[2][2] - rv[0][2] };
        float nx = e1[1] * e2[2] - e1[2] * e2[1];
        float ny = e1[2] * e2[0] - e1[0] * e2[2];
        float nz = e1[0] * e2[1] - e1[1] * e2[0];
        float const nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl > 1e-6f) { nx /= nl; ny /= nl; nz /= nl; }
        float const lambert = nx * lx + ny * ly + nz * lz;
        float const shade = 0.22f + 0.78f * (lambert < 0.f ? -lambert : lambert);

        float r = (float)t.r * shade;
        float g = (float)t.g * shade;
        float bcol = (float)t.b_col * shade;

        pt.depth = depth / 3.f;
        pt.col = IM_COL32((int)std::clamp(r, 0.f, 255.f), (int)std::clamp(g, 0.f, 255.f),
                          (int)std::clamp(bcol, 0.f, 255.f), 255);
        out.push_back(pt);
    }

    if (out.empty())
        return;

    std::sort(out.begin(), out.end(), [](ProjTri const& a, ProjTri const& b) {
        return a.depth > b.depth;
    });

    for (auto const& pt : out)
        dl->AddTriangleFilled(pt.p[0], pt.p[1], pt.p[2], pt.col);
}

} // namespace glb_preview