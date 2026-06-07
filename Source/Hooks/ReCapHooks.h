#ifndef RECAP_HOOKS_H
#define RECAP_HOOKS_H

/* ReCap client redirect — process-wide Detours hooks installed by EAWebKit.dll.
   Install from DllMain (DLL_PROCESS_ATTACH) / uninstall on DLL_PROCESS_DETACH. */

#ifdef __cplusplus
extern "C" {
#endif

void RecapHooksInstall(void);
void RecapHooksUninstall(void);

/* Opt the process into DPI awareness (call as early as possible, before any window is
   created). Darkspore.exe ships no dpiAware manifest, so on a scaled display Windows
   bitmap-upscales the whole window -> blurry launcher + in-game webviews. */
void RecapSetDpiAware(void);

#ifdef __cplusplus
}
#endif

#endif /* RECAP_HOOKS_H */
