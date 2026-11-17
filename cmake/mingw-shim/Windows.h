#pragma once
// MinGW ships lower-case Windows headers; Ableton Link includes mixed-case names.
#include <windows.h>
#if defined(__MINGW32__) && defined(__cplusplus)
// Missing from the MinGW-w64 11 headers, exported by kernel32 on Windows 10 1607+.
extern "C" __declspec(dllimport) HRESULT WINAPI SetThreadDescription(HANDLE, PCWSTR);
#endif
