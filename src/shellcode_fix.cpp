#include "cheat.h"
#include <cstring>
#include <cstdint>

// ---------------------------------------------------------------------------
// CS:S v34 anti-RCE / anti-shellcode protections
// Ported from no-more-cssource-shellcodes:
//   1. IsSafeFileToDownload (engine.dll+0xFFF90): blocks .asi/.mix/.flt/.dll downloads
//   2. Voice_Init (engine.dll+0x1E4290): allows only vaudio_miles / vaudio_speex
//   3. ProcessVoiceData (engine.dll+0x13BFB5): clamps/zeroes oversized voice packets
//      (>= 32768 bits / 4096 bytes) to block stack buffer overflow shellcode
//   4. ieframe.dll MOTD download popup patches (when ieframe.dll is present)
// ---------------------------------------------------------------------------

#ifdef _WIN64

void InstallShellcodeFixes() {}

#else

namespace {

using IsSafeFileToDownloadFn = bool(__cdecl*)(const char* file_name);
using VoiceInitFn = bool(__cdecl*)(const char* codec_name);

IsSafeFileToDownloadFn oIsSafeFileToDownload = nullptr;
VoiceInitFn oVoiceInit = nullptr;
bool g_installed = false;

bool Splice32(void* splice_addr, void* jump_to, void* out_tramp, size_t patch_size, bool place_call = false) {
    if (!splice_addr || !jump_to || patch_size < 5)
        return false;

    auto* src = static_cast<uint8_t*>(splice_addr);
    auto* dst = static_cast<uint8_t*>(jump_to);

    uint8_t* tramp = nullptr;
    if (out_tramp) {
        tramp = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, patch_size + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!tramp)
            return false;
        std::memcpy(tramp, src, patch_size);
        uint8_t* tramp_jmp = tramp + patch_size;
        tramp_jmp[0] = 0xE9;
        int32_t const rel_back = static_cast<int32_t>((src + patch_size) - (tramp_jmp + 5));
        std::memcpy(tramp_jmp + 1, &rel_back, sizeof(rel_back));
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(src, patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        if (tramp) VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    std::memset(src, 0x90, patch_size);
    src[0] = place_call ? 0xE8 : 0xE9;
    int32_t const rel_to = static_cast<int32_t>(dst - (src + 5));
    std::memcpy(src + 1, &rel_to, sizeof(rel_to));

    FlushInstructionCache(GetCurrentProcess(), src, patch_size);
    DWORD tmp = 0;
    VirtualProtect(src, patch_size, old_protect, &tmp);

    if (out_tramp)
        *reinterpret_cast<void**>(out_tramp) = static_cast<void*>(tramp);
    return true;
}

bool WriteProtected(void* addr, const void* data, size_t size) {
    if (!addr || !data || size == 0) return false;
    DWORD old_protect = 0;
    if (!VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;
    std::memcpy(addr, data, size);
    FlushInstructionCache(GetCurrentProcess(), addr, size);
    DWORD tmp = 0;
    VirtualProtect(addr, size, old_protect, &tmp);
    return true;
}

bool __cdecl hkIsSafeFileToDownload(const char* file_name) {
    if (file_name) {
        const char* ext = strrchr(file_name, '.');
        if (!ext) ext = strchr(file_name, '.');
        if (ext && *ext) {
            if (_stricmp(ext, ".asi") == 0 || _stricmp(ext, ".mix") == 0 ||
                _stricmp(ext, ".flt") == 0 || _stricmp(ext, ".dll") == 0) {
                Log("[shellcode-fix] blocked unsafe download '%s'", file_name);
                return false;
            }
        }
    }
    return oIsSafeFileToDownload ? oIsSafeFileToDownload(file_name) : false;
}

bool __cdecl hkVoice_Init(const char* codec_name) {
    if (!codec_name || !*codec_name)
        return false;

    bool const is_miles = (_stricmp(codec_name, "vaudio_miles") == 0);
    bool const is_speex = !is_miles && (_stricmp(codec_name, "vaudio_speex") == 0);
    if (!is_miles && !is_speex) {
        Log("[shellcode-fix] blocked unsafe voice codec '%s'", codec_name);
        return false;
    }

    bool const ok = oVoiceInit ? oVoiceInit(codec_name) : false;
    Log("[shellcode-fix] voice codec '%s' init=%d", codec_name, ok ? 1 : 0);
    return ok;
}

extern "C" void __fastcall CheckVoiceDataSize(int size) {
    if (size > 4096)
        Log("[shellcode-fix] blocked oversized voice packet (%d/4096 bytes)", size);
}

// Replaces 5 bytes at engine.dll+0x13BFB5:
//   50           push %eax
//   8d 4c 24 14  lea  0x14(%esp), %ecx
__attribute__((naked)) void hkProcessVoiceDataAsm() {
    __asm__ __volatile__ (
        "movl 20(%%ebx), %%ecx\n\t"
        "leal 7(%%ecx), %%ecx\n\t"
        "sarl $3, %%ecx\n\t"
        "call @CheckVoiceDataSize@4\n\t"
        "popl %%edx\n\t"
        "movl 20(%%ebx), %%eax\n\t"
        "cmpl $0x8000, %%eax\n\t"
        "jl 1f\n\t"
        "xorl %%eax, %%eax\n\t"
        "1:\n\t"
        "pushl %%eax\n\t"
        "leal 20(%%esp), %%ecx\n\t"
        "jmpl *%%edx\n\t"
        ::: "memory"
    );
}

unsigned int __stdcall IEDownload_ThreadProc(void**) {
    return 0;
}

void* FindU32InRange(uint8_t* start, uint8_t* min_p, uint8_t* max_p, uint32_t val, bool backward, ptrdiff_t offset) {
    if (!start || !min_p || !max_p || max_p < min_p + 4) return nullptr;
    uint8_t* cur = start;
    uint8_t* limit = max_p - 4;
    while (cur >= min_p && cur <= limit) {
        if (*reinterpret_cast<uint32_t*>(cur) == val)
            return cur + offset;
        cur += backward ? -1 : 1;
    }
    return nullptr;
}

void* FindU16InRange(uint8_t* start, uint8_t* min_p, uint8_t* max_p, uint16_t val, bool backward, ptrdiff_t offset) {
    if (!start || !min_p || !max_p || max_p < min_p + 2) return nullptr;
    uint8_t* cur = start;
    uint8_t* limit = max_p - 2;
    while (cur >= min_p && cur <= limit) {
        if (*reinterpret_cast<uint16_t*>(cur) == val)
            return cur + offset;
        cur += backward ? -1 : 1;
    }
    return nullptr;
}

void PatchIEFrameMotd() {
    HMODULE ie = GetModuleHandleA("ieframe.dll");
    if (!ie) return;

    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(ie);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uintptr_t>(ie) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return;

    uint8_t* base = reinterpret_cast<uint8_t*>(ie);
    uint8_t* end = base + nt->OptionalHeader.SizeOfImage;

    // motd_patch_1: CDownloadUtilities::OpenUI -> xor eax, eax; ret; nop
    if (auto* p = static_cast<uint8_t*>(FindU32InRange(base, base, end, 0x562C6A53u, false, 5))) {
        if (auto* p2 = static_cast<uint8_t*>(FindU16InRange(p, base, end, 0xC084u, true, -4))) {
            if (p2 > base && p2[-1] == 0xE8) {
                int32_t const rel = *reinterpret_cast<int32_t*>(p2);
                void* target = p2 + 4 + rel;
                uint32_t const stub = 0x90C3C033u;
                if (WriteProtected(target, &stub, sizeof(stub)))
                    Log("[shellcode-fix] ieframe motd_patch_1 applied at %p", target);
            }
        }
    }

    // motd_patch_2: IEDownload::ThreadProc pointer
    void* p_dl = FindU32InRange(base, base, end, 0x562C6A53u, false, 5);
    if (!p_dl)
        p_dl = FindU32InRange(base, base, end, 0x684075FFu, false, 4);
    if (p_dl) {
        void* fn = reinterpret_cast<void*>(&IEDownload_ThreadProc);
        if (WriteProtected(p_dl, &fn, sizeof(fn)))
            Log("[shellcode-fix] ieframe motd_patch_2 applied at %p", p_dl);
    }
}

} // namespace

void InstallShellcodeFixes() {
    if (g_installed) return;
    HMODULE eng = engine_module ? engine_module : GetModuleHandleA("engine.dll");
    if (!eng) return;

    uintptr_t const base = reinterpret_cast<uintptr_t>(eng);
    uint8_t* p_safe = reinterpret_cast<uint8_t*>(base + 0xFFF90);
    uint8_t* p_voice_init = reinterpret_cast<uint8_t*>(base + 0x1E4290);
    uint8_t* p_voice_data = reinterpret_cast<uint8_t*>(base + 0x13BFB5);

    bool ok1 = false, ok2 = false, ok3 = false;
    if (p_safe[0] == 0x56 && p_safe[1] == 0x8B)
        ok1 = Splice32(p_safe, reinterpret_cast<void*>(&hkIsSafeFileToDownload), &oIsSafeFileToDownload, 5);
    if (p_voice_init[0] == 0x51 && p_voice_init[1] == 0xA1)
        ok2 = Splice32(p_voice_init, reinterpret_cast<void*>(&hkVoice_Init), &oVoiceInit, 6);
    if (p_voice_data[0] == 0x50 && p_voice_data[1] == 0x8D)
        ok3 = Splice32(p_voice_data, reinterpret_cast<void*>(&hkProcessVoiceDataAsm), nullptr, 5, true);

    PatchIEFrameMotd();
    g_installed = true;
    Log("[shellcode-fix] v34 protections installed: download=%d voice_init=%d voice_data=%d",
        ok1 ? 1 : 0, ok2 ? 1 : 0, ok3 ? 1 : 0);
}

#endif
