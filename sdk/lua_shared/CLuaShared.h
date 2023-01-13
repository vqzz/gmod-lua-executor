#pragma once
#include "CLuaInterface.h"

class CLuaShared
{
public:
	void** VTable;

	inline CLuaInterface* LuaGetInterface(int State)
	{
		using GetLuaInterfaceFn = CLuaInterface * (*)(void*, int);
		return ((GetLuaInterfaceFn)VTable[6])(this, State);
	}
};

CLuaInterface* GetLuaInterface(int realm) {
	static CLuaShared* LuaShared = (CLuaShared*)GetInterface("lua_shared.dll", "LUASHARED003");
	CLuaInterface* LuaInterface = LuaShared->LuaGetInterface(realm);
	return LuaInterface;
}