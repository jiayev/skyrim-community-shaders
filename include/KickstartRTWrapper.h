#pragma once

#include <windows.h>

// This is a stub file to satisfy the build process.
// In the future, a proper implementation can be added here.

class KickstartRTWrapper {
public:
    static bool LoadFunctions(HMODULE dllHandle);
    static void ResetFunctions();
    static bool Initialize();
};
