#pragma once
#include <Windows.h>
#include <psapi.h>
#include <string>

char* findPattern(const char* moduleName, const char* patternName, const char* pattern) {
	char* moduleBase;
	int moduleSize;
	HMODULE handle = GetModuleHandle(moduleName);
	if (handle) {
		MODULEINFO info;
		GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info));
		moduleBase = (char*)info.lpBaseOfDll;
		moduleSize = info.SizeOfImage;
		int patternLen = strlen(pattern);

		char* curByte = moduleBase;
		char* start = moduleBase;
		char* end = moduleBase + moduleSize - patternLen;

		int matching = 0;
		while (curByte < end) {
			if (*curByte == pattern[matching] || pattern[matching] == '\?') {
				matching++;
			}
			else {
				matching = 0;
			}

			if (matching == strlen(pattern)) {
				return curByte - matching + 1;
			}

			curByte++;
		}

		std::string msg = std::string("Failed to find pattern: ") + patternName;
		MessageBox(0, msg.c_str(), "Error", MB_OK | MB_ICONWARNING);
	}
}