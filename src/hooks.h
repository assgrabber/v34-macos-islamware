#pragma once
#include <windows.h>

void InstallWindowHook(HWND hwnd);
void InstallClientModeHooks();
void InstallD3D9Hook();
void InstallInputHooks();
void InstallFakelagHook();
