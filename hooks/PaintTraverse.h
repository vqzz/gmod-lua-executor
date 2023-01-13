#pragma once
#include <iostream> // remove this
#include "../sdk/Interface.h"
#include "../sdk/vgui2/VPanelWrapper.h"
#include "../sdk/lua_shared/CLuaShared.h"
#include "MakeHook.h"

void ParseError(std::string fullError) {
	int firstColon = fullError.find(":");
	int secondColon = fullError.find(":", firstColon + 1);
	std::string lineSlice = fullError.substr(firstColon + 1, secondColon - firstColon - 1);
	int errorLine = std::stoi(lineSlice);
	std::string errorString = fullError.substr(secondColon + 2, strlen(fullError.c_str()) - (secondColon + 2));

	Globals::luaError.active = true;
	Globals::luaError.line = errorLine;
	Globals::luaError.error = errorString;
}

typedef int(*_luaL_loadbuffer)(lua_State* state, const char* buff, size_t sz, const char* name);
typedef void(__thiscall* _PaintTraverse)(VPanelWrapper* _this, void* panel, bool force_repaint, bool allow_force);
_PaintTraverse oPaintTraverse;
void hkPaintTraverse(VPanelWrapper* _this, void* panel, bool force_repaint, bool allow_force) {
	// This function runs several times a frame. We are going to do stuff only on a certain panel, which runs once a frame.
	static void* overlay;
	if (!overlay) {
		if (!strcmp(_this->GetName(panel), "OverlayPopupPanel")) {
			overlay = panel;
		}
	}

	if (panel == overlay) {
		if (Globals::runLua) {
			static HMODULE luaModule = GetModuleHandle("lua_shared.dll");
			static _luaL_loadbuffer luaL_loadbuffer = (_luaL_loadbuffer)GetProcAddress(luaModule, "luaL_loadbuffer");

			CLuaInterface* LUA = GetLuaInterface(Globals::menuRealm ? 2 : 0);
			lua_State* state = LUA->GetState();

			if (LUA) {
				luaL_loadbuffer(state, Globals::lua.c_str(), Globals::lua.size(), "@lua/includes/util.lua"); // Random file, to-do: dynamically spoof source
				if (LUA->IsType(-1, LuaTypes::String)) {
					const char* error = LUA->GetString(-1);
					ParseError(error);
					LUA->Pop();
				}
				else {
					if (LUA->PCall(0, 0, 0)) {
						const char* error = LUA->GetString(-1);
						ParseError(error);
						LUA->Pop();
					}
				}
			}

			Globals::runLua = false;
		}
	}

	return oPaintTraverse(_this, panel, force_repaint, allow_force);
}

void hookPaintTraverse() {
	void* PanelWrapper = (void*)GetInterface("vgui2.dll", "VGUI_Panel009");
	PVOID* VMT = *((PVOID**)PanelWrapper);
	_PaintTraverse pPaintTraverse = (_PaintTraverse)VMT[41];

	MakeHook(pPaintTraverse, &hkPaintTraverse, &oPaintTraverse);
}