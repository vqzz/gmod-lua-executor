#pragma once
#include <Windows.h>
#include "../mem.h"
#include "MakeHook.h"
#include "../globals.h"

typedef HRESULT(__stdcall* _Present)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
_Present oPresent;
HRESULT hkPresent(IDirect3DDevice9* pDevice, CONST RECT* x1, CONST RECT* x2, HWND x3, CONST RGNDATA* x4) {
	static bool init = false;
	if (!init) {
		init = true;
		ImGui::CreateContext();

		// Remove ini file
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = "";

		ImGui_ImplWin32_Init(FindWindow("Valve001", 0));
		ImGui_ImplDX9_Init(pDevice);

		ImGuiStyle* style = &ImGui::GetStyle();
		style->WindowTitleAlign = ImVec2(0.5, 0.5);
		style->WindowMinSize = ImVec2(300, 150);
	}

	if (!Globals::showMenu) return oPresent(pDevice, x1, x2, x3, x4);

	// Fix the weird shading glitch when console is open
	DWORD colorwrite, srgbwrite;
	pDevice->GetRenderState(D3DRS_COLORWRITEENABLE, &colorwrite);
	pDevice->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbwrite);
	pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0xffffffff);
	pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, false);

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// Menu
	ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_Once);
	ImGui::Begin("glua executor - github.com/codabro", 0, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

	// Code editor
	static TextEditor editor;
	ImVec2 winSize = ImGui::GetWindowSize();
	editor.Render("##Editor", ImVec2(winSize.x - 15, winSize.y - 55), false);

	// Error markers
	if (Globals::luaError.active) {
		TextEditor::ErrorMarkers markers;
		markers.insert(std::make_pair(Globals::luaError.line, Globals::luaError.error));
		editor.SetErrorMarkers(markers);
		Globals::luaError.active = false;
	}

	// Execute button
	ImGui::SetCursorPosX(winSize.x - 67);
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
	if (ImGui::Button("Execute", ImVec2(60, 20))) {
		Globals::runLua = true;
		Globals::lua = editor.GetText();
	}

	// Menu realm checkbox
	ImGui::SetCursorPosX(winSize.x - 130);
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 23);
	ImGui::Checkbox("menu", &Globals::menuRealm);

	ImGui::End();

	// Finish up
	ImGui::EndFrame();
	ImGui::Render();

	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

	pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, colorwrite);
	pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbwrite);

	return oPresent(pDevice, x1, x2, x3, x4);
}

void hookPresent() {
	_Present Present = (_Present)findPattern("gameoverlayrenderer64.dll", "Present", "\x48\x89\x5C\x24\?\x48\x89\x6C\x24\?\x48\x89\x74\x24\?\x48\x89\x7C\x24\?\x41\x54\x41\x56\x41\x57\x48\x81\xEC\?\?\?\?\x4C\x8B\xA4\x24\?\?\?\?");
	MakeHook(Present, &hkPresent, &oPresent);
}