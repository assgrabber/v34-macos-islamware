#include "cheat.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Client-side custom player model changer.
//
// Custom models not precached by the server are registered through
// IVModelInfo::RegisterDynamicModel (VModelInfoClient006 vtable slot 52),
// which loads the model into the engine's client dynamic model table and
// returns a negative index -(slot * 2 + 3).  C_BaseEntity::SetModelIndex
// (client.dll + 0x8D670, wrapped in MDLCache004 BeginLock/EndLock) updates
// m_nModelIndex (int16_t), entity->model, and triggers OnNewModel().
// ---------------------------------------------------------------------------

namespace {

constexpr uintptr_t CLIENT_SET_MODEL_INDEX_RVA = 0x8D670;
constexpr uintptr_t CLIENT_GET_MODEL_PTR_RVA   = 0x751A0;
constexpr uintptr_t CLIENT_LOOKUP_SEQUENCE_RVA = 0x77B70;
constexpr uintptr_t CLIENT_STUDIO_NUM_SEQ_RVA  = 0x1A6BC0;
constexpr uintptr_t CLIENT_STUDIO_SEQ_DESC_RVA = 0x1A8C90;
constexpr int FRAME_NET_UPDATE_POSTDATAUPDATE_END = 4;
constexpr int OFF_ANIM_OVERLAY_VEC   = 0x0be0; // CUtlVector<C_AnimationLayer>::m_Memory.m_pMemory
constexpr int OFF_ANIM_OVERLAY_COUNT = 0x0bf0; // GetNumAnimOverlays()
constexpr int ANIM_LAYER_STRIDE      = 0x2c;
constexpr int STUDIO_DELTA_FLAG      = 0x04;

static const char* const kBuiltinPlayerModels[] = {
    "models/player/ct_urban.mdl",
    "models/player/ct_gsg9.mdl",
    "models/player/ct_sas.mdl",
    "models/player/ct_gign.mdl",
    "models/player/t_phoenix.mdl",
    "models/player/t_leet.mdl",
    "models/player/t_arctic.mdl",
    "models/player/t_guerilla.mdl",
    "models/characters/hostage_01.mdl",
    "models/characters/hostage_02.mdl",
    "models/characters/hostage_03.mdl",
    "models/characters/hostage_04.mdl",
};

static std::vector<std::string> g_model_list;
static bool g_models_scanned = false;
static char g_model_status[128] = "Default";

static int g_map_key = -1;
static char g_cached_path[sizeof(cfg.model_path)] = {};
static int g_cached_index = 0;
static bool g_cached_failed = false;
static int g_last_addref_index = 0;

static void* g_orig_local_ent = nullptr;
static int g_orig_local_slot = 0;
static int16_t g_orig_local_index = 0;
static int g_last_logged_applied = 0;

static int16_t g_remap_orig_idx = 0;
static int g_remap_custom_idx = 0;
static std::vector<int> g_seq_remap;
static int g_remap_fallback_seq = -1;

static bool g_has_saved_seqs = false;
static void* g_saved_seq_ent = nullptr;
static int g_saved_main_seq = 0;
static int g_saved_overlay_count = 0;
static int g_saved_overlay_seqs[16] = {};

static bool FileExistsA(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool ContainsICase(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    size_t const nlen = strlen(needle);
    if (!nlen) return true;
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               std::tolower((unsigned char)p[i]) == std::tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen) return true;
    }
    return false;
}

static bool EqualICase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool IsKnownBuiltinModel(const char* path) {
    if (!path || !path[0]) return false;
    for (const char* b : kBuiltinPlayerModels) {
        if (EqualICase(path, b)) return true;
    }
    return false;
}

static bool GetGameRootDir(char* out, size_t out_sz) {
    if (!GetModuleFileNameA(nullptr, out, (DWORD)out_sz)) {
        out[0] = 0;
        return false;
    }
    if (char* slash = strrchr(out, '\\'))
        *slash = 0;
    return out[0] != 0;
}

// Validate a loose Source .mdl file on disk before handing it to mdlcache.
// Returns 1 if valid IDST v44-49 with matching .vvd and .vtx,
//         0 if the file does not exist on disk at all,
//        -1 if a file exists on disk but is invalid/incomplete/legacy.
static int CheckLooseStudioFile(const char* model_path) {
    if (!model_path || !model_path[0]) return -1;

    char rel_win[MAX_PATH] = {};
    strncpy(rel_win, model_path, sizeof(rel_win) - 1);
    for (char* p = rel_win; *p; p++)
        if (*p == '/') *p = '\\';

    std::vector<std::string> candidates;
    candidates.push_back(std::string("cstrike\\") + rel_win);
    candidates.push_back(std::string("cstrike\\download\\") + rel_win);

    char root[MAX_PATH] = {};
    if (GetGameRootDir(root, sizeof(root))) {
        candidates.push_back(std::string(root) + "\\cstrike\\" + rel_win);
        candidates.push_back(std::string(root) + "\\cstrike\\download\\" + rel_win);

        // Scan cstrike/custom/*/
        WIN32_FIND_DATAA cfd{};
        HANDLE ch = FindFirstFileA((std::string(root) + "\\cstrike\\custom\\*").c_str(), &cfd);
        if (ch != INVALID_HANDLE_VALUE) {
            do {
                if ((cfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    strcmp(cfd.cFileName, ".") != 0 && strcmp(cfd.cFileName, "..") != 0) {
                    candidates.push_back(std::string(root) + "\\cstrike\\custom\\" +
                                         cfd.cFileName + "\\" + rel_win);
                }
            } while (FindNextFileA(ch, &cfd));
            FindClose(ch);
        }
    }

    for (const std::string& cand : candidates) {
        FILE* f = fopen(cand.c_str(), "rb");
        if (!f) continue;

        unsigned int id = 0;
        int ver = 0;
        unsigned int checksum = 0;
        size_t const got = fread(&id, 4, 1, f) + fread(&ver, 4, 1, f) +
                           fread(&checksum, 4, 1, f);
        fclose(f);

        if (got != 3 || id != 0x54534449 /* 'IDST' */ || ver < 44 || ver > 49) {
            Log("[model] rejected '%s' (magic=%08x ver=%d, expected IDST v44-49)",
                cand.c_str(), id, ver);
            return -1;
        }

        std::string stem = cand;
        size_t dot = stem.rfind('.');
        if (dot != std::string::npos) stem.resize(dot);

        bool const has_vtx = FileExistsA((stem + ".dx90.vtx").c_str()) ||
                             FileExistsA((stem + ".dx80.vtx").c_str()) ||
                             FileExistsA((stem + ".sw.vtx").c_str());
        if (!has_vtx) {
            Log("[model] rejected '%s' (missing .vtx)", cand.c_str());
            return -1;
        }

        FILE* vf = fopen((stem + ".vvd").c_str(), "rb");
        if (!vf) {
            Log("[model] rejected '%s' (missing .vvd)", cand.c_str());
            return -1;
        }
        unsigned int vid = 0, vver = 0, vsum = 0;
        size_t const vgot = fread(&vid, 4, 1, vf) + fread(&vver, 4, 1, vf) +
                            fread(&vsum, 4, 1, vf);
        fclose(vf);

        if (vgot != 3 || vid != 0x56534449 /* 'IDSV' */) {
            Log("[model] rejected '%s' (bad .vvd magic=%08x)", cand.c_str(), vid);
            return -1;
        }
        if (checksum && vsum && checksum != vsum) {
            Log("[model] rejected '%s' (.mdl/.vvd checksum mismatch %08x vs %08x)",
                cand.c_str(), checksum, vsum);
            return -1;
        }
        return 1;
    }

    return 0;
}

static bool ShouldIncludeScannedModel(const std::string& rel_path) {
    // Skip first-person viewmodels, world weapon models, and map props
    const char* s = rel_path.c_str();
    if (ContainsICase(s, "/v_") || ContainsICase(s, "/w_") || ContainsICase(s, "props"))
        return false;
    // Validate on disk so broken/GoldSrc models never show up to crash
    return CheckLooseStudioFile(s) == 1;
}

static void AddModelUnique(std::vector<std::string>& out, const std::string& path) {
    for (const auto& existing : out) {
        if (EqualICase(existing, path)) return;
    }
    out.push_back(path);
}

static void ScanModelDir(const std::string& dir, const std::string& rel, int depth,
                         std::vector<std::string>& out) {
    if (depth > 5 || out.size() >= 128) return;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir + "\\" + name;
        std::string next_rel = rel.empty() ? name : (rel + "/" + name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanModelDir(full, next_rel, depth + 1, out);
        } else if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".mdl") == 0) {
            std::string model_rel = (next_rel.rfind("models/", 0) == 0)
                                        ? next_rel
                                        : ("models/" + next_rel);
            if (ShouldIncludeScannedModel(model_rel))
                AddModelUnique(out, model_rel);
        }
    } while (FindNextFileA(h, &fd) && out.size() < 128);
    FindClose(h);
}

static void RescanModels() {
    g_models_scanned = true;
    std::vector<std::string> custom_found;

    char root[MAX_PATH] = {};
    if (GetGameRootDir(root, sizeof(root))) {
        std::string r = root;
        ScanModelDir(r + "\\cstrike\\models", "", 0, custom_found);
        ScanModelDir(r + "\\cstrike\\download\\models", "", 0, custom_found);

        WIN32_FIND_DATAA cfd{};
        HANDLE ch = FindFirstFileA((r + "\\cstrike\\custom\\*").c_str(), &cfd);
        if (ch != INVALID_HANDLE_VALUE) {
            do {
                if ((cfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    strcmp(cfd.cFileName, ".") != 0 && strcmp(cfd.cFileName, "..") != 0) {
                    ScanModelDir(r + "\\cstrike\\custom\\" + cfd.cFileName + "\\models",
                                 "", 0, custom_found);
                }
            } while (FindNextFileA(ch, &cfd));
            FindClose(ch);
        }
    }
    std::sort(custom_found.begin(), custom_found.end());

    g_model_list.clear();
    for (const auto& m : custom_found)
        AddModelUnique(g_model_list, m);
    for (const char* b : kBuiltinPlayerModels)
        AddModelUnique(g_model_list, b);
}

static void ResetMapModelState(int new_map_key) {
    g_map_key = new_map_key;
    g_cached_path[0] = 0;
    g_cached_index = 0;
    g_cached_failed = false;
    g_last_addref_index = 0;
    g_orig_local_ent = nullptr;
    g_orig_local_slot = 0;
    g_orig_local_index = 0;
    g_last_logged_applied = 0;
    g_remap_orig_idx = 0;
    g_remap_custom_idx = 0;
    g_seq_remap.clear();
    g_remap_fallback_seq = -1;
    g_has_saved_seqs = false;
    g_saved_seq_ent = nullptr;
    cfg.model_index = 0;
}

static int ResolveClientModelIndex(const char* path) {
    if (!path || !path[0] || !model_info.self) {
        snprintf(g_model_status, sizeof(g_model_status), "Default");
        return 0;
    }

    if (strcmp(g_cached_path, path) != 0) {
        strncpy(g_cached_path, path, sizeof(g_cached_path) - 1);
        g_cached_path[sizeof(g_cached_path) - 1] = 0;
        g_cached_index = 0;
        g_cached_failed = false;
        g_last_logged_applied = 0;
    }

    if (g_cached_index != 0) return g_cached_index;
    if (g_cached_failed) return 0;

    // 1. Already in server precache?
    int idx = model_info.GetModelIndex(path);
    if (idx > 0) {
        g_cached_index = idx;
        snprintf(g_model_status, sizeof(g_model_status), "Server precached (#%d)", idx);
        Log("[model] '%s' found in server precache -> %d", path, idx);
        return idx;
    }

    // 2. Validate loose files on disk before ever asking the engine to load them
    int const disk_state = CheckLooseStudioFile(path);
    if (disk_state < 0) {
        g_cached_failed = true;
        snprintf(g_model_status, sizeof(g_model_status), "Invalid/incomplete .mdl");
        return 0;
    }
    if (disk_state == 0 && !IsKnownBuiltinModel(path)) {
        g_cached_failed = true;
        snprintf(g_model_status, sizeof(g_model_status), "Model file not found");
        Log("[model] '%s' not found on disk or builtin list", path);
        return 0;
    }

    // 3. Register client-side dynamic model through IVModelInfo::RegisterDynamicModel
    int const dyn_idx = model_info.RegisterDynamicModel(path, true);
    if (dyn_idx >= -1) {
        g_cached_failed = true;
        snprintf(g_model_status, sizeof(g_model_status), "Failed to load model");
        Log("[model] RegisterDynamicModel('%s', true) returned %d", path, dyn_idx);
        return 0;
    }

    if (g_last_addref_index != dyn_idx) {
        model_info.AddRefDynamicModel(dyn_idx);
        g_last_addref_index = dyn_idx;
    }

    mdl_cache.BeginLock();
    const void* mdl = model_info.GetModel(dyn_idx);
    const void* hdr = mdl ? model_info.GetStudiomodel(mdl) : nullptr;
    mdl_cache.EndLock();

    if (!mdl || !hdr) {
        g_cached_failed = true;
        snprintf(g_model_status, sizeof(g_model_status), "Model failed studio load");
        Log("[model] dynamic index %d for '%s' produced null mdl=%p hdr=%p",
            dyn_idx, path, mdl, hdr);
        return 0;
    }

    g_cached_index = dyn_idx;
    snprintf(g_model_status, sizeof(g_model_status), "Active (client #%d)", dyn_idx);
    Log("[model] RegisterDynamicModel('%s') -> %d (mdl=%p hdr=%p)", path, dyn_idx, mdl, hdr);
    return dyn_idx;
}

static void ApplyEntityModelIndex(void* entity, int index) {
    if (!entity || !client_module || index == 0 || index == -1) return;
    typedef void(__fastcall* SetModelIndexFn)(void*, int);
    auto set_model_index = reinterpret_cast<SetModelIndexFn>(
        (uintptr_t)client_module + CLIENT_SET_MODEL_INDEX_RVA);

    mdl_cache.BeginLock();
    set_model_index(entity, index);
    mdl_cache.EndLock();
}

static bool BuildSequenceRemap(void* entity, int16_t orig_idx, int custom_idx) {
    if (!entity || !client_module || orig_idx <= 0 || custom_idx == 0 || custom_idx == -1)
        return false;
    if (g_remap_orig_idx == orig_idx && g_remap_custom_idx == custom_idx && !g_seq_remap.empty())
        return true;

    typedef void(__fastcall* SetModelIndexFn)(void*, int);
    typedef void*(__fastcall* GetModelPtrFn)(void*);
    typedef int(__fastcall* LookupSeqFn)(void*, const char*);
    typedef int(__fastcall* GetNumSeqFn)(const void*);
    typedef const void*(__fastcall* SeqDescFn)(void*, int);

    auto set_model_index = reinterpret_cast<SetModelIndexFn>((uintptr_t)client_module + CLIENT_SET_MODEL_INDEX_RVA);
    auto get_model_ptr = reinterpret_cast<GetModelPtrFn>((uintptr_t)client_module + CLIENT_GET_MODEL_PTR_RVA);
    auto lookup_seq = reinterpret_cast<LookupSeqFn>((uintptr_t)client_module + CLIENT_LOOKUP_SEQUENCE_RVA);
    auto get_num_seq = reinterpret_cast<GetNumSeqFn>((uintptr_t)client_module + CLIENT_STUDIO_NUM_SEQ_RVA);
    auto p_seq_desc = reinterpret_cast<SeqDescFn>((uintptr_t)client_module + CLIENT_STUDIO_SEQ_DESC_RVA);

    int const saved_seq = *reinterpret_cast<int*>((char*)entity + off::m_nSequence);
    int16_t const start_idx = *reinterpret_cast<int16_t*>((char*)entity + off::m_nModelIndex);

    std::vector<std::string> orig_labels;
    mdl_cache.BeginLock();

    set_model_index(entity, (int)orig_idx);
    void* orig_hdr = get_model_ptr(entity);
    int const n_orig = orig_hdr ? get_num_seq(orig_hdr) : 0;
    if (n_orig > 0 && n_orig < 4096) {
        orig_labels.resize(n_orig);
        for (int i = 0; i < n_orig; i++) {
            const void* sd = p_seq_desc(orig_hdr, i);
            if (!sd) continue;
            int const name_off = *reinterpret_cast<const int*>((const char*)sd + 4);
            if (name_off > 0)
                orig_labels[i] = (const char*)sd + name_off;
        }
    }

    set_model_index(entity, custom_idx);
    void* custom_hdr = get_model_ptr(entity);
    int const n_custom = custom_hdr ? get_num_seq(custom_hdr) : 0;

    int fallback = lookup_seq(entity, "Idle_lower");
    if (fallback < 0) fallback = lookup_seq(entity, "idle_lower");
    if (fallback < 0) fallback = lookup_seq(entity, "idle");
    if (fallback < 0 && custom_hdr && n_custom > 0) {
        for (int i = 0; i < n_custom; i++) {
            const void* sd = p_seq_desc(custom_hdr, i);
            if (!sd) continue;
            int const flags = *reinterpret_cast<const int*>((const char*)sd + 12);
            int const name_off = *reinterpret_cast<const int*>((const char*)sd + 4);
            const char* lbl = (name_off > 0) ? ((const char*)sd + name_off) : "";
            if ((flags & STUDIO_DELTA_FLAG) == 0 &&
                _stricmp(lbl, "reference") != 0 &&
                _stricmp(lbl, "ragdoll") != 0) {
                fallback = i;
                break;
            }
        }
    }

    g_seq_remap.clear();
    if (!orig_labels.empty() && custom_hdr && n_custom > 0) {
        g_seq_remap.resize(orig_labels.size(), fallback);
        for (size_t i = 0; i < orig_labels.size(); i++) {
            if (orig_labels[i].empty()) continue;
            int mapped = lookup_seq(entity, orig_labels[i].c_str());
            if (mapped >= 0 && mapped < n_custom)
                g_seq_remap[i] = mapped;
        }
    }

    if (start_idx != 0 && start_idx != -1 && start_idx != (int16_t)custom_idx)
        set_model_index(entity, (int)start_idx);

    mdl_cache.EndLock();
    *reinterpret_cast<int*>((char*)entity + off::m_nSequence) = saved_seq;

    if (g_seq_remap.empty()) return false;
    g_remap_orig_idx = orig_idx;
    g_remap_custom_idx = custom_idx;
    g_remap_fallback_seq = fallback;

    Log("[model] seq remap built: orig=%d(%d) -> custom=%d(%d) fallback=%d (0:'%s'->%d, 1:'%s'->%d)",
        (int)orig_idx, (int)orig_labels.size(), custom_idx, n_custom, fallback,
        orig_labels.size() > 0 ? orig_labels[0].c_str() : "?", g_seq_remap.size() > 0 ? g_seq_remap[0] : -1,
        orig_labels.size() > 1 ? orig_labels[1].c_str() : "?", g_seq_remap.size() > 1 ? g_seq_remap[1] : -1);
    return true;
}

static int RemapSequenceIndex(int orig_seq) {
    if (orig_seq >= 0 && orig_seq < (int)g_seq_remap.size())
        return g_seq_remap[orig_seq];
    return g_remap_fallback_seq >= 0 ? g_remap_fallback_seq : orig_seq;
}

static void RestoreSavedSequences(void* entity) {
    if (!g_has_saved_seqs) return;
    if (entity && entity == g_saved_seq_ent) {
        *reinterpret_cast<int*>((char*)entity + off::m_nSequence) = g_saved_main_seq;
        char* layers = *reinterpret_cast<char**>((char*)entity + OFF_ANIM_OVERLAY_VEC);
        int const count = *reinterpret_cast<int*>((char*)entity + OFF_ANIM_OVERLAY_COUNT);
        if (layers && count > 0 && count == g_saved_overlay_count && count <= 16) {
            for (int i = 0; i < count; i++) {
                if (i >= 1 && i <= 3) continue; // owned client-side by CCSPlayerAnimState
                *reinterpret_cast<int*>(layers + (size_t)i * ANIM_LAYER_STRIDE) = g_saved_overlay_seqs[i];
            }
        }
    }
    g_has_saved_seqs = false;
    g_saved_seq_ent = nullptr;
}

} // namespace

int CustomModels_Count(bool rescan) {
    if (!g_models_scanned || rescan)
        RescanModels();
    return (int)g_model_list.size();
}

const char* CustomModels_At(int index) {
    if (!g_models_scanned)
        RescanModels();
    if (index < 0 || index >= (int)g_model_list.size()) return "";
    return g_model_list[index].c_str();
}

const char* CustomModels_Status() {
    return g_model_status;
}

void ClientModel_OnFrameStage(int stage) {
    if (!IsGameReady() || !model_info.self || !client_module) {
        if (g_map_key != -1) ResetMapModelState(-1);
        return;
    }

    int const urban = model_info.GetModelIndex("models/player/ct_urban.mdl");
    if (urban <= 0) {
        if (g_map_key != -1) ResetMapModelState(-1);
        return;
    }
    if (g_map_key != urban)
        ResetMapModelState(urban);

    void* local = GetLocalEntity();
    if (!local) {
        g_has_saved_seqs = false;
        g_saved_seq_ent = nullptr;
        return;
    }

    // Choose target entity: local player when alive, or spectated player when dead.
    void* model_ent = nullptr;
    int model_slot = 0;
    bool const local_alive = *reinterpret_cast<const char*>((const char*)local + off::m_lifeState) == 0;
    int const local_team = *reinterpret_cast<const int*>((const char*)local + off::m_iTeamNum);
    if (local_alive && (local_team == 2 || local_team == 3)) {
        model_ent = local;
        model_slot = engine.GetLocalPlayer();
    } else {
        int const obs_mode = *reinterpret_cast<const int*>((const char*)local + 0x15dc);
        if (obs_mode == 4 || obs_mode == 5) {
            int const h = *reinterpret_cast<const int*>((const char*)local + 0x15e0);
            int const idx = (h != -1) ? (h & 0xFFF) : 0;
            if (idx > 0 && idx <= 64) {
                void* obs = entity_list.GetClientEntity(idx);
                if (obs && obs != local && !EntityIsStale(obs, idx) &&
                    *reinterpret_cast<const char*>((const char*)obs + off::m_lifeState) == 0 &&
                    *reinterpret_cast<const int*>((const char*)obs + off::m_iHealth) > 0) {
                    int const obs_team = *reinterpret_cast<const int*>((const char*)obs + off::m_iTeamNum);
                    if (obs_team == 2 || obs_team == 3) {
                        model_ent = obs;
                        model_slot = idx;
                    }
                }
            }
        }
    }

    if (stage == FRAME_RENDER_END) {
        if (g_saved_seq_ent && g_saved_seq_ent == model_ent)
            RestoreSavedSequences(model_ent);
        else {
            g_has_saved_seqs = false;
            g_saved_seq_ent = nullptr;
        }
        return;
    }

    if (stage != FRAME_NET_UPDATE_POSTDATAUPDATE_END && stage != FRAME_RENDER_START)
        return;

    // If we switched away from a previously modified entity, safely restore it.
    if (g_orig_local_ent && g_orig_local_ent != model_ent && g_orig_local_index > 0 && g_orig_local_slot > 0) {
        void* prev = entity_list.GetClientEntity(g_orig_local_slot);
        if (prev == g_orig_local_ent) {
            int16_t const pcur = *reinterpret_cast<const int16_t*>((const char*)prev + off::m_nModelIndex);
            if (pcur < 0)
                ApplyEntityModelIndex(prev, (int)g_orig_local_index);
        }
        g_orig_local_ent = nullptr;
        g_orig_local_slot = 0;
        g_orig_local_index = 0;
    }

    if (!model_ent) {
        g_has_saved_seqs = false;
        g_saved_seq_ent = nullptr;
        if (!cfg.model_path[0])
            snprintf(g_model_status, sizeof(g_model_status), "Default");
        cfg.model_index = 0;
        return;
    }

    int16_t const cur = *reinterpret_cast<const int16_t*>((const char*)model_ent + off::m_nModelIndex);
    if (cur > 0) {
        g_orig_local_ent = model_ent;
        g_orig_local_slot = model_slot;
        g_orig_local_index = cur;
    }

    if (cfg.model_path[0]) {
        int const target = ResolveClientModelIndex(cfg.model_path);
        cfg.model_index = target;
        if (target != 0 && target != -1 && cur != (int16_t)target) {
            ApplyEntityModelIndex(model_ent, target);
            if (g_last_logged_applied != target) {
                g_last_logged_applied = target;
                Log("[model] applied '%s' (idx=%d orig=%d stage=%d slot=%d)",
                    cfg.model_path, target, (int)g_orig_local_index, stage, model_slot);
            }
        }
        if (stage == FRAME_RENDER_START && target < 0 && g_orig_local_index > 0) {
            if (BuildSequenceRemap(model_ent, g_orig_local_index, target)) {
                int* seq_ptr = reinterpret_cast<int*>((char*)model_ent + off::m_nSequence);
                g_saved_seq_ent = model_ent;
                g_saved_main_seq = *seq_ptr;
                g_has_saved_seqs = true;
                int const remapped = RemapSequenceIndex(g_saved_main_seq);
                if (remapped >= 0)
                    *seq_ptr = remapped;

                char* layers = *reinterpret_cast<char**>((char*)model_ent + OFF_ANIM_OVERLAY_VEC);
                int const count = *reinterpret_cast<int*>((char*)model_ent + OFF_ANIM_OVERLAY_COUNT);
                g_saved_overlay_count = (layers && count > 0 && count <= 16) ? count : 0;
                for (int i = 0; i < g_saved_overlay_count; i++) {
                    int* lseq = reinterpret_cast<int*>(layers + (size_t)i * ANIM_LAYER_STRIDE);
                    g_saved_overlay_seqs[i] = *lseq;
                    if (i >= 1 && i <= 3) continue; // owned client-side by CCSPlayerAnimState
                    float const lweight = *reinterpret_cast<float*>(layers + (size_t)i * ANIM_LAYER_STRIDE + 8);
                    if (*lseq >= 0 && lweight > 0.0f && *lseq < (int)g_seq_remap.size()) {
                        int const r = RemapSequenceIndex(*lseq);
                        if (r >= 0) *lseq = r;
                    }
                }
            }
        }
    } else {
        RestoreSavedSequences(model_ent);
        if (!cfg.model_path[0])
            snprintf(g_model_status, sizeof(g_model_status), "Default");
        cfg.model_index = 0;
        if (cur < 0 && g_orig_local_ent == model_ent && g_orig_local_index > 0) {
            ApplyEntityModelIndex(model_ent, (int)g_orig_local_index);
            g_last_logged_applied = 0;
        }
    }
}

bool ClientModel_ShouldOverride(void* entity, int& out_index) {
    if (!entity || !cfg.model_path[0] || !IsGameReady()) return false;
    if (entity != GetLocalEntity()) return false;
    bool const alive = *reinterpret_cast<const char*>((const char*)entity + off::m_lifeState) == 0;
    int const team = *reinterpret_cast<const int*>((const char*)entity + off::m_iTeamNum);
    if (!alive || (team != 2 && team != 3)) return false;
    int const target = ResolveClientModelIndex(cfg.model_path);
    if (target == 0 || target == -1) return false;
    out_index = target;
    return true;
}
