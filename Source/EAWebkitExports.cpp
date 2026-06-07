// ReCap.WebKit — DLL entry (DllMain).
//
// The one exported symbol Darkspore resolves, CreateEAWebkitInstance(), lives in
// src/app/EAWebkitConcrete.cpp (it returns the real IEAWebkit). This TU only owns DllMain so the
// host-process hooks can be installed/removed with the DLL lifetime.

#include <windows.h>

#include "ReCapHooks.h"   // RecapSetDpiAware / RecapHooksInstall / RecapHooksUninstall (extern "C")

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ::OutputDebugStringA("[ReCap.WebKit] DllMain attach\n");
        RecapSetDpiAware();    // before any window is created -> kills DPI-virtualization blur
        RecapHooksInstall();   // ws2_32 redirect + cert bypass + DPI source + WM_QUIT re-post
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        RecapHooksUninstall();
    }
    return TRUE;
}
