#pragma once

class CLuaInterface;

struct lua_State
{
	unsigned char _ignore_this_common_lua_header_[92 + 22];
	CLuaInterface* luabase;

};

class CLuaInterface {
public:
	void** VTable;

	inline void Pop(int iAmt = 1) {
		using PopFn = void (*)(void*, int);
		return ((PopFn)VTable[2])(this, iAmt);
	}

	inline int PCall(int iArgs, int iResults, int iErrorFunc) {
		using PCallFn = int (*)(void*, int, int, int);
		return ((PCallFn)VTable[11])(this, iArgs, iResults, iErrorFunc);
	}

	inline const char* GetString(int iStackPos = -1, unsigned int* iOutLen = 0) {
		using GetStringFn = const char* (*)(void*, int, unsigned int*);
		return ((GetStringFn)VTable[23])(this, iStackPos, iOutLen);
	}

	inline bool IsType(int iStackPos, int iType) {
		using IsTypeFn = bool (*)(void*, int, int);
		return ((IsTypeFn)VTable[39])(this, iStackPos, iType);
	}

	lua_State* GetState() {
		return *reinterpret_cast<lua_State**>(this + 1);
	}
};

namespace LuaTypes {
	enum
	{
		// Default Lua
		None = -1,
		Nil,
		Bool,
		LightUserData,
		Number,
		String,
		Table,
		Function,
		UserData,
		Thread,

		// gLua
		Entity,
		Vector,
		Angle
	};
};