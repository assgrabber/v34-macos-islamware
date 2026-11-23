#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>

static DWORD FindProcess(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = { sizeof(pe) };
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int main(int argc, char** argv) {
    const char* process = "cstrike_win64.exe";
    const char* dll = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) process = argv[++i];
        else if (!dll) dll = argv[i];
    }
    if (!dll) {
        printf("usage: injector.exe [-p process.exe] <full path to islamware.dll>\n");
        return 1;
    }

    DWORD pid = FindProcess(process);
    if (!pid) { printf("process '%s' not found\n", process); return 1; }
    printf("target pid %lu\n", pid);

    HANDLE ph = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                            FALSE, pid);
    if (!ph) { printf("OpenProcess failed: %lu\n", GetLastError()); return 1; }

    size_t len = strlen(dll) + 1;
    void* remote = VirtualAllocEx(ph, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("VirtualAllocEx failed: %lu\n", GetLastError()); return 1; }
    WriteProcessMemory(ph, remote, dll, len, nullptr);

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto load_library = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(ph, nullptr, 0, load_library, remote, 0, nullptr);
    if (!thread) { printf("CreateRemoteThread failed: %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(thread, 10000);

    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    printf("LoadLibraryA returned %#lx\n", exit_code);
    return exit_code ? 0 : 1;
}
