#pragma once

class VPanelWrapper {
public:
	void** VTable;

	inline const char* GetName(void* panel) {
		using GetNameFn = const char* (*)(void*, void*);
		return ((GetNameFn)VTable[36])(this, panel);
	}
};