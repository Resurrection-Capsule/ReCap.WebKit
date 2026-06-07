/* ReCap client redirect — process-wide Detours hooks installed by EAWebkit.dll.

   Replicates Xackery's drop-in redirect entirely in RAM at runtime (Darkspore.exe is NEVER
   modified). Once EAWebkit.dll is loaded into the process it shares the address space, so Detours
   can trampoline the ws2_32 APIs (the whole process: exe Blaze/QoS/bootstrap sockets AND mb's web
   sockets) plus the exe's statically-linked DirtySDK ProtoSSL cert check.

   Hooks (target client = retail 5.3.0.127, image base 0x00400000):
     - ws2_32 gethostbyname -> resolve everything to recap.cfg host.
     - ws2_32 connect       -> force that host; remap HTTP port 80 -> recap.cfg http_port.
     - exe _VerifyCertificate    @0x00E4D4D0 -> return 0 (accept ReCap self-signed cert).
     - exe _WildcardMatchNoCase  @0x00E4B9E0 -> return 0 (host matches cert CN).
     - WM_QUIT re-post: mb132's nested pump eats the launcher's quit msg (Play/Exit/Close).
     - DPI source neutralization for mb (caller-filtered): feed mb 96 DPI so CSS px == surface px.

   Clean-room note: the EA-tree original (eawebkit/source/ReCapHooks.cpp) also wired a UTFwin
   native-SPUI PoC (ReCapNativeUI: F9 test + follow-cursor drag) into Hook_PeekMessageW. That PoC
   is exe-offset-locked and orthogonal to redirect/cert — dropped here to keep the shim focused. */

#include <winsock2.h>
#include <windows.h>
#include <detours.h>
#include <intrin.h>
#include "recapredirect.h"
#include "ReCapHooks.h"
#include "ReCapLog.h"
#include "LocaleUnlock.h"
#pragma intrinsic(_ReturnAddress)

/* exe ProtoSSL offsets from image base 0x00400000 (retail 5.3.0.127). */
#define RECAP_VERIFYCERT_OFFSET     0x00A4D4D0u   /* 0x00E4D4D0 - 0x00400000 */
#define RECAP_WILDCARDMATCH_OFFSET  0x00A4B9E0u   /* 0x00E4B9E0 - 0x00400000 */

static struct hostent* (WSAAPI *Real_gethostbyname)(const char*) = gethostbyname;
static int             (WSAAPI *Real_connect)(SOCKET, const struct sockaddr*, int) = connect;

typedef int (__cdecl *VerifyCertFn)(void);
typedef int (__cdecl *WildcardFn)(const char*, const char*);
static VerifyCertFn Real_VerifyCert = 0;   /* never called (we always return 0) */
static WildcardFn   Real_Wildcard   = 0;

/* gethostbyname -> a static hostent for the configured host (addr in network order). */
static struct hostent s_he;
static unsigned long  s_hostAddr;          /* NB: 's_addr' is a winsock macro -> don't use as a name */
static char*          s_addr_list[2];

static struct hostent* WSAAPI Hook_gethostbyname(const char* name)
{
    const RecapRedirectConfig* c = RecapRedirectGet();
    s_hostAddr = htonl(c->uHostAddr);
    s_addr_list[0] = (char*)&s_hostAddr;
    s_addr_list[1] = 0;
    s_he.h_name      = (char*)name;
    s_he.h_aliases   = 0;
    s_he.h_addrtype  = AF_INET;            /* 2 */
    s_he.h_length    = 4;
    s_he.h_addr_list = s_addr_list;
    return &s_he;
}

static int WSAAPI Hook_connect(SOCKET s, const struct sockaddr* name, int namelen)
{
    if (name && name->sa_family == AF_INET)
    {
        const RecapRedirectConfig* c = RecapRedirectGet();
        struct sockaddr_in sin = *(const struct sockaddr_in*)name;
        sin.sin_addr.s_addr = htonl(c->uHostAddr);
        if (ntohs(sin.sin_port) == 80)
            sin.sin_port = htons((u_short)c->uHttpPort);
        return Real_connect(s, (const struct sockaddr*)&sin, sizeof(sin));
    }
    return Real_connect(s, name, namelen);
}

static int __cdecl Hook_VerifyCert(void) { return 0; }
static int __cdecl Hook_Wildcard(const char* a, const char* b) { (void)a; (void)b; return 0; }

/* WM_QUIT re-post fix for Play/Exit/Close. The launcher's modal loop (FUN_005154b0, PeekMessageA)
   exits on the WM_QUIT that Play/Close PostMessage to its window — but mb132 runs its own nested
   pump (PeekMessageW, NULL filter, PM_REMOVE) during mbWake and EATS that WM_QUIT first, so the
   game loop never sees it and Play/Close hang. Hook_PeekMessageW re-queues it (below). */
#define RECAP_WM_QUIT 0x12u
static BOOL (WINAPI *Real_PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT) = PeekMessageW;

/* Is a code address inside the named loaded module? (caller attribution for the WM_QUIT fix) */
static bool RecapAddrInModule(void* addr, const char* mod)
{
    HMODULE h = GetModuleHandleA(mod);
    if (!h) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((unsigned char*)h + dos->e_lfanew);
    ULONG_PTR base = (ULONG_PTR)h, p = (ULONG_PTR)addr;
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

/* ===== DPI source neutralization for mb132 =====
   The process is DPI-aware (RecapSetDpiAware: sharp window, correct input). But that makes mb132
   auto-read the REAL system DPI and scale its page layout -> in-game pages render at wrong
   size/scale with white margin. We want CSS px == surface px == physical px: feed mb 96 DPI
   (scale 1) at the SOURCE while the rest of the process keeps real awareness. All hooks are
   CALLER-FILTERED to mb132_x32.dll (cached range) so the game's own GDI/DPI queries are
   untouched. 96-DPI machines: hooks attached but return the real (already-96) value -> no-op. */
static ULONG_PTR s_mbBase = 0, s_mbEnd = 0;
static bool RecapCallerIsMb(void* addr)
{
    if (!s_mbBase)
    {
        HMODULE h = GetModuleHandleA("mb132_x32.dll");
        if (!h) return false;   /* mb not loaded yet -> can't be an mb caller */
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)h;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((unsigned char*)h + dos->e_lfanew);
        s_mbBase = (ULONG_PTR)h;
        s_mbEnd  = s_mbBase + nt->OptionalHeader.SizeOfImage;
    }
    ULONG_PTR p = (ULONG_PTR)addr;
    return p >= s_mbBase && p < s_mbEnd;
}

typedef int  (WINAPI *GetDeviceCapsFn)(HDC, int);
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
typedef UINT (WINAPI *GetDpiForSystemFn)(void);
typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int /*MONITOR_DPI_TYPE*/, UINT*, UINT*);
static GetDeviceCapsFn    Real_GetDeviceCaps    = 0;
static GetDpiForWindowFn  Real_GetDpiForWindow  = 0;
static GetDpiForSystemFn  Real_GetDpiForSystem  = 0;
static GetDpiForMonitorFn Real_GetDpiForMonitor = 0;

static int WINAPI Hook_GetDeviceCaps(HDC hdc, int index)
{
    /* LOGPIXELSX=88, LOGPIXELSY=90 — the only indices that carry DPI. Everything else
       (resolution, color depth, ...) must pass through untouched even for mb. */
    if ((index == 88 || index == 90) && RecapCallerIsMb(_ReturnAddress()))
        return 96;
    return Real_GetDeviceCaps(hdc, index);
}
static UINT WINAPI Hook_GetDpiForWindow(HWND hwnd)
{
    if (RecapCallerIsMb(_ReturnAddress())) return 96;
    return Real_GetDpiForWindow(hwnd);
}
static UINT WINAPI Hook_GetDpiForSystem(void)
{
    if (RecapCallerIsMb(_ReturnAddress())) return 96;
    return Real_GetDpiForSystem();
}
static HRESULT WINAPI Hook_GetDpiForMonitor(HMONITOR mon, int type, UINT* dpiX, UINT* dpiY)
{
    if (RecapCallerIsMb(_ReturnAddress()))
    {
        if (dpiX) *dpiX = 96;
        if (dpiY) *dpiY = 96;
        return S_OK;
    }
    return Real_GetDpiForMonitor(mon, type, dpiX, dpiY);
}

/* mb's nested pump pulls the launcher's WM_QUIT; re-queue it as a thread message (for the game's
   PeekMessageA) and return "no message" to mb so its pump unwinds back to the game loop. The
   game's own PeekMessageW calls (caller in the exe) pass through. */
static BOOL WINAPI Hook_PeekMessageW(LPMSG msg, HWND h, UINT a, UINT b, UINT r)
{
    BOOL ok = Real_PeekMessageW(msg, h, a, b, r);
    if (ok && msg && msg->message == RECAP_WM_QUIT)
    {
        void* caller = _ReturnAddress();
        if (RecapAddrInModule(caller, "mb132_x32.dll"))
        {
            PostThreadMessageA(GetCurrentThreadId(), RECAP_WM_QUIT, msg->wParam, msg->lParam);
            recap::MbLogf("WMQ: mb ate WM_QUIT (caller=%p) -> re-posted for the game loop", caller);
            msg->message = 0;   /* WM_NULL */
            return FALSE;        /* hide from mb so its pump yields back to the game's loop */
        }
    }
    return ok;
}

/* cert hooks are gated at INSTALL time on recap.cfg ssl_bypass: when 0 we never attach them, so
   the exe's normal cert verification is left untouched. Tracked so uninstall detaches exactly
   what was attached. */
static int s_certHooked = 0;

/* Make the process DPI-aware so Windows stops bitmap-upscaling (blurring) the whole window on
   scaled displays. The exe ships no dpiAware manifest -> DPI-virtualized by default. We opt in
   from DllMain, before the game creates its window. SYSTEM_AWARE (not per-monitor) on purpose:
   the game does not handle WM_DPICHANGED. Resolved via GetProcAddress so the DLL still loads on
   pre-Win10 (falls back to legacy SetProcessDPIAware). */
extern "C" void RecapSetDpiAware(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
#pragma warning(push)
#pragma warning(disable: 4191)   /* FARPROC -> typed fn ptr cast */
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    typedef BOOL (WINAPI *SetAwareFn)(void);
    SetCtxFn   pSetCtx   = (SetCtxFn)  GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    SetAwareFn pSetAware = (SetAwareFn)GetProcAddress(u32, "SetProcessDPIAware");
#pragma warning(pop)
    /* DPI_AWARENESS_CONTEXT_SYSTEM_AWARE == (HANDLE)-2 (Win10 1703+). */
    if (pSetCtx && pSetCtx((HANDLE)(LONG_PTR)-2)) { recap::MbLog("DPI: process set SYSTEM_AWARE (context)"); return; }
    if (pSetAware && pSetAware())                 { recap::MbLog("DPI: process set system-aware (legacy SetProcessDPIAware)"); return; }
    recap::MbLog("DPI: could not set awareness (older OS or already set)");
}

extern "C" void RecapHooksInstall(void)
{
    const RecapRedirectConfig* c = RecapRedirectGet();
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    Real_VerifyCert = (VerifyCertFn)(base + RECAP_VERIFYCERT_OFFSET);
    Real_Wildcard   = (WildcardFn)(base + RECAP_WILDCARDMATCH_OFFSET);
    s_certHooked = (c->bSslBypass != 0);
    RecapLocaleUnlockPrepare(base);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)Real_gethostbyname, (PVOID)Hook_gethostbyname);
    DetourAttach(&(PVOID&)Real_connect,       (PVOID)Hook_connect);
    if (s_certHooked)
    {
        DetourAttach(&(PVOID&)Real_VerifyCert, (PVOID)Hook_VerifyCert);
        DetourAttach(&(PVOID&)Real_Wildcard,   (PVOID)Hook_Wildcard);
    }
    /* WM_QUIT re-post fix (Play/Exit/Close): mb132's nested pump eats the launcher's quit msg */
    DetourAttach(&(PVOID&)Real_PeekMessageW, (PVOID)Hook_PeekMessageW);
    /* Locale unlocker: register every Data/Locale/<code> folder (e.g. pt-br) at resolver time */
    RecapLocaleUnlockAttach();

    /* DPI source neutralization for mb (caller-filtered). GetDeviceCaps is always present; the
       per-monitor APIs exist only on Win10 1607 / Win8.1 — resolve + attach what's there. */
    {
        HMODULE gdi32  = GetModuleHandleW(L"gdi32.dll");
        HMODULE u32    = GetModuleHandleW(L"user32.dll");
        HMODULE shcore = GetModuleHandleW(L"shcore.dll");
        if (!shcore) shcore = LoadLibraryW(L"shcore.dll");
#pragma warning(push)
#pragma warning(disable: 4191)
        if (gdi32)  Real_GetDeviceCaps    = (GetDeviceCapsFn)   GetProcAddress(gdi32,  "GetDeviceCaps");
        if (u32)    Real_GetDpiForWindow  = (GetDpiForWindowFn) GetProcAddress(u32,    "GetDpiForWindow");
        if (u32)    Real_GetDpiForSystem  = (GetDpiForSystemFn) GetProcAddress(u32,    "GetDpiForSystem");
        if (shcore) Real_GetDpiForMonitor = (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor");
#pragma warning(pop)
        if (Real_GetDeviceCaps)    DetourAttach(&(PVOID&)Real_GetDeviceCaps,    (PVOID)Hook_GetDeviceCaps);
        if (Real_GetDpiForWindow)  DetourAttach(&(PVOID&)Real_GetDpiForWindow,  (PVOID)Hook_GetDpiForWindow);
        if (Real_GetDpiForSystem)  DetourAttach(&(PVOID&)Real_GetDpiForSystem,  (PVOID)Hook_GetDpiForSystem);
        if (Real_GetDpiForMonitor) DetourAttach(&(PVOID&)Real_GetDpiForMonitor, (PVOID)Hook_GetDpiForMonitor);
    }
    DetourTransactionCommit();
}

extern "C" void RecapHooksUninstall(void)
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)Real_gethostbyname, (PVOID)Hook_gethostbyname);
    DetourDetach(&(PVOID&)Real_connect,       (PVOID)Hook_connect);
    if (s_certHooked)
    {
        DetourDetach(&(PVOID&)Real_VerifyCert, (PVOID)Hook_VerifyCert);
        DetourDetach(&(PVOID&)Real_Wildcard,   (PVOID)Hook_Wildcard);
    }
    DetourDetach(&(PVOID&)Real_PeekMessageW, (PVOID)Hook_PeekMessageW);
    RecapLocaleUnlockDetach();
    if (Real_GetDeviceCaps)    DetourDetach(&(PVOID&)Real_GetDeviceCaps,    (PVOID)Hook_GetDeviceCaps);
    if (Real_GetDpiForWindow)  DetourDetach(&(PVOID&)Real_GetDpiForWindow,  (PVOID)Hook_GetDpiForWindow);
    if (Real_GetDpiForSystem)  DetourDetach(&(PVOID&)Real_GetDpiForSystem,  (PVOID)Hook_GetDpiForSystem);
    if (Real_GetDpiForMonitor) DetourDetach(&(PVOID&)Real_GetDpiForMonitor, (PVOID)Hook_GetDpiForMonitor);
    DetourTransactionCommit();
}
