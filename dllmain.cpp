#include <Windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")

#include <imgui/imgui.h>
#include <imgui/imgui_impl_dx9.h>
#include <imgui/imgui_impl_win32.h>
#include <imgui/TextEditor.h>
#include "hooks/Present.h"
#include "hooks/CursorHooks.h"
#include "hooks/PaintTraverse.h"

#include "globals.h"

HMODULE loaderModule;

DWORD EjectThread() {
    Sleep(200);
    FreeLibraryAndExitThread(loaderModule, 0);
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
WNDPROC oWndProc;
LRESULT WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Prevent sending insert to the game
    if ((uMsg == WM_KEYUP || uMsg == WM_KEYDOWN) && wParam == VK_INSERT) {
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        // Toggle menu
        if (uMsg == WM_KEYDOWN) {
            Globals::showMenu = !Globals::showMenu;
        }
        return TRUE;
    }

    // Uninject on END
    if (uMsg == WM_KEYDOWN && wParam == VK_END) {
        Globals::uninject = true;
    }

    // Prevents a weird game freeze
    if (Globals::showMenu && (uMsg != 15))
    {
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        return TRUE;
    }
    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

DWORD WINAPI LoaderThread(PVOID thread) {
    HWND hWnd = FindWindow("Valve001", 0);
    oWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);

    MH_Initialize();
    hookPresent();
    hookCursor();
    hookPaintTraverse();

    while (!Globals::uninject) {
        Sleep(10);
    }

    // Not using sleeps here causes crashes so this is my fix
    Sleep(500);
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(100);
    MH_Uninitialize();

    SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);

    Sleep(2000);
    CreateThread(0, 0, (LPTHREAD_START_ROUTINE)EjectThread, 0, 0, 0);
    return 0;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        loaderModule = hModule;
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)LoaderThread, NULL, 0, NULL);
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}