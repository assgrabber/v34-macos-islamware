#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <csetjmp>

extern char g_log_path[MAX_PATH];

const char* g_crash_stage = "startup";
jmp_buf g_safe_call_jmp;
volatile int g_safe_call_active = 0;
CRITICAL_SECTION g_log_cs;
bool g_log_cs_init = false;

void SetStage(const char* stage) {
    g_crash_stage = stage;
}

static void WriteRaw(const char* text) {
    FILE* f = fopen(g_log_path, "a");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void CrashWrite(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    WriteRaw(buf);
    WriteRaw("\n");
}

static const char* ModuleNameForAddress(uintptr_t addr, uintptr_t* out_rva) {
    static char name[MAX_PATH];
    name[0] = 0;
    *out_rva = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return name;
    MODULEENTRY32 me = { sizeof(me) };
    if (Module32First(snap, &me)) {
        do {
            uintptr_t base = (uintptr_t)me.modBaseAddr;
            if (addr >= base && addr < base + me.modBaseSize) {
                strncpy(name, me.szModule, sizeof(name) - 1);
                *out_rva = addr - base;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return name;
}

static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (g_safe_call_active && code == EXCEPTION_ACCESS_VIOLATION) {
        g_safe_call_active = 0;
        CrashWrite("[safe-call] caught AV at %p stage=%s", info->ExceptionRecord->ExceptionAddress, g_crash_stage);
        longjmp(g_safe_call_jmp, 1);
    }
    // only first-chance memory faults matter for diagnosing random crashes
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW) {
        uintptr_t rva = 0;
        const char* mod = ModuleNameForAddress((uintptr_t)info->ExceptionRecord->ExceptionAddress, &rva);
        CrashWrite("[first-chance] code=%08lX addr=%p %s+%#llx stage=%s",
            code, info->ExceptionRecord->ExceptionAddress, mod,
            (unsigned long long)rva, g_crash_stage);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
    uintptr_t addr = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    uintptr_t rva = 0;
    const char* mod = ModuleNameForAddress(addr, &rva);
    CONTEXT* ctx = info->ContextRecord;

    CrashWrite("================ CRASH ================");
    CrashWrite("time: %lld", (long long)time(nullptr));
    CrashWrite("stage: %s", g_crash_stage);
    CrashWrite("code: %08lX  flags: %08lX", info->ExceptionRecord->ExceptionCode,
        info->ExceptionRecord->ExceptionFlags);
    CrashWrite("address: %p  %s+%#llx", (void*)addr, mod, (unsigned long long)rva);
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        CrashWrite("access: %s at %p",
            info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
            (void*)info->ExceptionRecord->ExceptionInformation[1]);
    }
#ifdef _WIN64
    CrashWrite("rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx",
        (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
        (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
    CrashWrite("rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx",
        (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
        (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rsp);
    CrashWrite("r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx",
        (unsigned long long)ctx->R8, (unsigned long long)ctx->R9,
        (unsigned long long)ctx->R10, (unsigned long long)ctx->R11);
    CrashWrite("r12=%016llx r13=%016llx r14=%016llx r15=%016llx rip=%016llx",
        (unsigned long long)ctx->R12, (unsigned long long)ctx->R13,
        (unsigned long long)ctx->R14, (unsigned long long)ctx->R15,
        (unsigned long long)ctx->Rip);

    // return addresses from the stack
    CrashWrite("stack:");
    uintptr_t* sp = (uintptr_t*)ctx->Rsp;
#else
    CrashWrite("eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx",
        (unsigned long)ctx->Eax, (unsigned long)ctx->Ebx,
        (unsigned long)ctx->Ecx, (unsigned long)ctx->Edx);
    CrashWrite("esi=%08lx edi=%08lx ebp=%08lx esp=%08lx eip=%08lx",
        (unsigned long)ctx->Esi, (unsigned long)ctx->Edi,
        (unsigned long)ctx->Ebp, (unsigned long)ctx->Esp,
        (unsigned long)ctx->Eip);

    // return addresses from the stack
    CrashWrite("stack:");
    uintptr_t* sp = (uintptr_t*)ctx->Esp;
#endif
    for (int i = 0; i < 32 && sp; i++) {
        uintptr_t v = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), &sp[i], &v, sizeof(v), &got) || got != sizeof(v))
            break;
        uintptr_t srva = 0;
        const char* smod = ModuleNameForAddress(v, &srva);
        if (smod[0])
            CrashWrite("  [%02d] %p %s+%#llx", i, (void*)v, smod, (unsigned long long)srva);
    }

    char dump[MAX_PATH];
    snprintf(dump, sizeof(dump), "%s.%lld.dmp", g_log_path, (long long)time(nullptr));
    HANDLE file = CreateFileA(dump, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
            MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(file);
        CrashWrite("minidump: %s", dump);
    }
    CrashWrite("=======================================");
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashHandler() {
    InitializeCriticalSection(&g_log_cs);
    g_log_cs_init = true;
    SetUnhandledExceptionFilter(UnhandledFilter);
    AddVectoredExceptionHandler(1, VectoredHandler);
    CrashWrite("crash handler installed");
}
