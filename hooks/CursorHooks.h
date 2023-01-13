#pragma once
#include "MakeHook.h"

typedef void(_stdcall* aSetCursorPos)(int X, int Y);
aSetCursorPos pSetCursorPos;
aSetCursorPos pSetCursorPosA;

void _stdcall hkSetCursorPos(int X, int Y) {
    if (Globals::showMenu) {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return;
    }
    else {
        return pSetCursorPosA(X, Y);
    }
}

typedef void(_stdcall* aSetCursor)(HCURSOR hCursor);
aSetCursor pSetCursor;
aSetCursor pSetCursorA;

void _stdcall hkSetCursor(HCURSOR hCursor) {
    if (Globals::showMenu) {
        pSetCursorA(LoadCursor(NULL, IDC_ARROW));
    }
    else {
        pSetCursorA(hCursor);
    }
}

void hookCursor() {
    MakeHookApi(L"user32", "SetCursorPos", pSetCursorPos, &hkSetCursorPos, &pSetCursorPosA);
    MakeHookApi(L"user32", "SetCursor", pSetCursor, &hkSetCursor, &pSetCursorA);
}