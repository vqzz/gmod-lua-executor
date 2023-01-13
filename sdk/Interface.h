#pragma once
#include <Windows.h>

typedef void*(*CreateInterfaceFn)(const char* moduleName, int* code);
void* GetInterface(const char* moduleName, const char* interfaceName)
{
    CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(GetModuleHandleA(moduleName), "CreateInterface");
    return createInterface(interfaceName, NULL);
}