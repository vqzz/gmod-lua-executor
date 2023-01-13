#pragma once

struct LuaError {
	bool active = true;
	int line;
	std::string error;
};

namespace Globals {
	bool showMenu = false;
	bool uninject = false;
	bool runLua = false;
	bool menuRealm = false;
	std::string lua = "";

	LuaError luaError;
}