#pragma once

#include <Minhook.h>
#pragma comment(lib, "libMinHook.x64.lib")

void MakeHook(void* oFunc, void* hkFunc, void* oFuncA) {
    MH_CreateHook(reinterpret_cast<void**>(oFunc), hkFunc, reinterpret_cast<void**>(oFuncA));
    MH_EnableHook(reinterpret_cast<void**>(oFunc));
}

void MakeHookApi(const wchar_t* dll, const char* oFuncName, void* oFunc, void* hkFunc, void* oFuncA) {
    MH_CreateHookApi(dll, oFuncName, hkFunc, reinterpret_cast<void**>(oFuncA));
    MH_EnableHook(reinterpret_cast<void**>(oFunc));
}