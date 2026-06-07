#define _CRT_SECURE_NO_WARNINGS   /* _snprintf/_vsnprintf; project builds /W4 /WX */
#include <windows.h>
#include <stdio.h>
#include <string.h>
/* VC9 (VS2008) lacks nullptr (C++11). Define it before pulling in mb.h. */
#ifndef nullptr
#  define nullptr 0
#endif
/* mb.h is a third-party vendor header; the EAWebkit project builds /W4 /WX. Its inline
   DLL loader casts FARPROC -> typed fn ptrs (C4191) and trips other /W4 nags. Silence them
   for the vendor include only (our own code below stays under full warnings). */
#pragma warning(push)
#pragma warning(disable: 4191 4100 4127 4255 4505 4996 4244 4245 4267 4189)
#include "mb.h"          // copied from the release; self-loads mb132_x32.dll
#pragma warning(pop)
#include "ReCapMiniBlink.h"
#include "ReCapJsBridge.h"   /* MbRunJs — used to drive the CSS-:hover shim (mb ignores synthetic MOVE) */
#include "ReCapLog.h"
#include <shellapi.h>        /* ShellExecuteW — nav-lock opens external links in the OS browser */
#pragma comment(lib, "shell32.lib")

namespace recap {

static int s_state = 0;  // 0=unprobed, 1=ok, -1=unavailable

static bool ensureInit()
{
    if (s_state) return s_state == 1;
    s_state = -1;
    MbLog("ensureInit: setting dll path mb132_x32.dll");
    mbSetMbMainDllPath(L"mb132_x32.dll");   // override mb.h's mb108 default
    mbSettings settings;
    memset(&settings, 0, sizeof(settings));
    settings.version = kMbVersion;           // required by miniblink 132
    MbLog("ensureInit: calling mbInit");
    mbInit(&settings);                       // mb.h inline: LoadLibrary + fill fn ptrs
    MbLogf("ensureInit: mbInit done, mbCreateWebView=%p", (void*)mbCreateWebView);
    if (!mbCreateWebView) return false;      // fn ptr stays null if the DLL didn't load
    s_state = 1;
    return true;
}

struct MbView {
    mbWebView   wv;
    MbPaintSink sink;
    void*       user;
    MbLoadSink  loadSink;
    void*       loadUser;
    int         width;
    int         height;
    HWND        hwnd;        /* hidden 1x1 window mb132 posts render-thread msgs to */
    /* DIB section for HDC-path pixel extraction */
    HDC         dibDC;
    HBITMAP     dibBitmap;
    void*       dibBits;
    int         dirty;       /* set on each paint; consumed (coalesced) by View::Tick */
    int         transparent; /* ViewParameters.mbTransparentBackground: keep real alpha */
    int         lastX, lastY;/* last mouse pos (view coords) — for forwarding captured up */
    int         hoverX, hoverY;/* last pos sent to the JS :hover shim (__rh) — throttle delta */
    /* JS bridge state — set by MbBindJsMethod in ReCapJsBridge */
    void*       jsSink;      /* MbJsSink callback (typed in ReCapJsBridge.h) */
    void*       jsUser;      /* opaque user pointer passed back to jsSink */
};

bool MbAvailable() { return ensureInit(); }

/* mb132 needs a real HWND for its render-thread -> UI-thread message dispatch (it does NOT
   render into it; pixels still come via the paint sink). We create our own hidden window so
   the host (EAWebKit View) never has to source the game's HWND. */
static LRESULT CALLBACK RecapMbWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* NOTE: do NOT install a WM_TIMER pump on this window. The game's launcher runs its own
       modal message loop (PeekMessage, NULL filter) and exits on WM_QUIT — which the Play/Close
       buttons post via PostMessage(hWnd, WM_QUIT). WM_QUIT is the lowest-priority message and is
       only retrieved when the queue is otherwise empty; a perpetual WM_TIMER on a window pumped
       by that loop starves WM_QUIT forever, so Play/Close silently never exit. mb is pumped via
       View::Tick -> MbWake (every frame) and a direct mbWake after each input instead. */
    /* mb132 SetCaptures this hidden window on a fired mouse-down, so the matching mouse-UP
       routes here instead of the launcher window (which is why clicks never completed and the
       button stuck until alt-tab released capture). Forward the UP to mb at the last known
       view position and release capture, so clicks complete. */
    if (m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP)
    {
        MbView* self = (MbView*)GetWindowLongPtrA(h, GWLP_USERDATA);
        if (self && self->wv)
        {
            unsigned int msg = (m == WM_RBUTTONUP) ? MB_MSG_RBUTTONUP
                             : (m == WM_MBUTTONUP) ? MB_MSG_MBUTTONUP : MB_MSG_LBUTTONUP;
            mbFireMouseEvent(self->wv, msg, self->lastX, self->lastY, 0);
            ReleaseCapture();
        }
    }
    /* Once a click gives this hidden window OS keyboard focus (mb's native ::SetFocus on
       mouse-down), the keystrokes arrive HERE — forward them straight to mb. This is the
       canonical mb input model and replaces relying on the game's OnKeyboardEvent (the game
       window no longer has OS focus while a field is being edited). */
    else if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR)
    {
        /* Only non-system keys: WM_SYSKEYDOWN/etc (Alt combos) fall through to DefWindowProc so
           Alt-Tab / Alt-F4 keep working — a form needs no Alt shortcuts. */
        MbView* self = (MbView*)GetWindowLongPtrA(h, GWLP_USERDATA);
        if (self && self->wv)
        {
            if (m == WM_KEYDOWN)     mbFireKeyDownEvent(self->wv, (unsigned)w, 0, FALSE);
            else if (m == WM_KEYUP)  mbFireKeyUpEvent(self->wv, (unsigned)w, 0, FALSE);
            else                     mbFireKeyPressEvent(self->wv, (unsigned)w, 0, FALSE);
            mbWake(self->wv);
            return 0;
        }
    }
    return DefWindowProcA(h, m, w, l);
}

/* The EAWebKit.dll image hosting this code (NOT the exe) — the game unloads/reloads the DLL
   between the launcher and the in-game webviews, so anything tied to a module must use the
   live image base, never a cached/fixed identity. */
static HINSTANCE selfModule()
{
    MEMORY_BASIC_INFORMATION mbi;
    HINSTANCE hSelf = GetModuleHandleA(NULL);
    if (VirtualQuery((void*)&RecapMbWndProc, &mbi, sizeof(mbi)) && mbi.AllocationBase)
        hSelf = (HINSTANCE)mbi.AllocationBase;
    return hSelf;
}

static HWND createHiddenWindow(int w, int h)
{
    static bool s_registered = false;
    static char s_className[64] = "RecapMbHiddenWnd";

    /* This window's WndProc (RecapMbWndProc) lives in EAWebKit.dll, which the game UNLOADS after the
       launcher and RELOADS for the in-game webview. A class registered under a FIXED name outlives the
       DLL — its WndProc then dangles into the freed image, and the next CreateWindow crashes in
       WM_NCCREATE (verified: AV executing a freed address with esi=WM_NCCREATE=0x81). Tie the class to
       THIS DLL image: a per-base unique name guarantees a fresh class with a live WndProc every load.
       (Same-base reload is safe too: the name collides but the WndProc address is re-mapped valid.) */
    HINSTANCE hSelf = selfModule();

    if (!s_registered)
    {
        _snprintf(s_className, sizeof(s_className) - 1, "RecapMbHiddenWnd_%p", (void*)hSelf);
        s_className[sizeof(s_className) - 1] = 0;
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = RecapMbWndProc;
        wc.hInstance     = hSelf;
        wc.lpszClassName = s_className;
        ATOM atom = RegisterClassA(&wc);
        MbLogf("createHiddenWindow: RegisterClass '%s' self=%p -> atom=%u", s_className, (void*)hSelf, (unsigned)atom);
        s_registered = true;
    }
    /* WS_POPUP (no border) so the client area == w x h. Not shown (no WS_VISIBLE). */
    return CreateWindowA(s_className, "", WS_POPUP, 0, 0, w, h, NULL, NULL, hSelf, NULL);
}

/* BGRA buffer path — preferred, but mb132 default mode uses the HDC path below. Carries mb's
   REAL alpha (the opaque-view force-0xFF policy lives in the HDC thunk). */
static void MB_CALL_TYPE paintBitThunk(mbWebView, void* param, const void* buffer,
                                       const mbRect* r, int width, int height)
{
    MbView* self = (MbView*)param;
    if (self && self->sink && buffer && r)
    {
        self->sink(self->user, buffer, width * 4, r->x, r->y, r->w, r->h);
        self->dirty = 1;
    }
    (void)height;
}

static void rebuildDib(MbView* self, int w, int h)
{
    if (self->dibBitmap) { DeleteObject(self->dibBitmap); self->dibBitmap = NULL; self->dibBits = NULL; }
    if (self->dibDC)     { DeleteDC(self->dibDC); self->dibDC = NULL; }

    HDC screen = GetDC(NULL);
    self->dibDC = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    self->dibBitmap = CreateDIBSection(self->dibDC, &bmi, DIB_RGB_COLORS, &self->dibBits, NULL, 0);
    if (self->dibBitmap)
        SelectObject(self->dibDC, self->dibBitmap);
}

/* HDC path — fallback when BitUpdated does not fire (mb132 default mode) */
static void MB_CALL_TYPE paintHDCThunk(mbWebView wv, void* param, const HDC srcDC,
                                       int x, int y, int cx, int cy)
{
    MbView* self = (MbView*)param;
    if (!self || !self->sink || !srcDC) return;

    /* DIB is the FULL view size (built in MbCreate / MbResize). Blit the dirty sub-rect into
       its position in the full DIB so the buffer is full-surface-indexed — matching the sink,
       which indexes by absolute (x,y). GDI clips the BitBlt to the DIB bounds (safe). */
    if (!self->dibBits) return;
    /* Blit the WHOLE view from mb's backing HDC into the (persistent) DIB — the DIB is thus
       always a complete, consistent frame (copying only the dirty sub-rect from the HDC can
       catch mid-update/cleared states -> black). GDI BitBlt is cheap. */
    BitBlt(self->dibDC, 0, 0, self->width, self->height, srcDC, 0, 0, SRCCOPY);
    GdiFlush();

    /* Alpha policy. The game draws the surface as a textured quad that honors alpha.
       Opaque views: GDI leaves alpha=0 in the DIB -> force 0xFF or the whole view is invisible
       (=black quad). Transparent views (mbSetTransparent, e.g. the announce slot): keep mb's
       alpha so unpainted areas stay invisible. */
    if (!self->transparent)
    {
        unsigned int* px = (unsigned int*)self->dibBits;
        int total = self->width * self->height, i;
        for (i = 0; i < total; ++i) px[i] |= 0xFF000000;
    }

    /* Push the COMPLETE frame into mpSurface every paint. (Copying only mb's reported dirty
       sub-rect missed hover repaints when mb under-reported the rect -> hover never showed.
       The per-pixel alpha pass above is a cheap loop, so full-frame is fine.) */
    self->sink(self->user, self->dibBits, self->width * 4, 0, 0, self->width, self->height);
    self->dirty = 1;   /* coalesced ViewNotification happens in View::Tick (UI thread) */
    (void)wv; (void)x; (void)y; (void)cx; (void)cy;
}

/* Capture the page's JS console + uncaught errors so we can see why the launcher JS
   does (or doesn't) wire its buttons. */
static void MB_CALL_TYPE consoleThunk(mbWebView, void*, mbConsoleLevel level,
    const utf8* message, const utf8* sourceName, unsigned sourceLine, const utf8*)
{
    /* mb132's own injected JS (window.mbQuery wrapper) console.logs on every call from
       cwebview_runjs.com — pure noise that floods the log. Keep only real page diagnostics. */
    if (sourceName && strstr(sourceName, "cwebview_runjs.com")) return;
    MbLogf("JS console[%d]: %s  (%s:%u)", (int)level,
           message ? message : "", sourceName ? sourceName : "?", sourceLine);
}

/* The client still asks for EAWebKit's packaged-resource scheme (game:///UI/XHTML/...). mb has
   no game:// handler, so map those onto the ReCap HTTP server: the announce page goes to our
   recreated announceen page; anything else to /game/<path> (recreated assets live there). */
static const char* mapGameUrl(const char* url, char* buf, size_t bufSize)
{
    if (!url || strncmp(url, "game://", 7) != 0) return url;
    if (strstr(url, "announcement.html"))
        return "http://localhost/web/sporelabsgame/announceen";
    {
        const char* path = url + 7;
        while (*path == '/') ++path;
        _snprintf(buf, bufSize - 1, "http://localhost/game/%s", path);
        buf[bufSize - 1] = 0;
    }
    return buf;
}

/* Net-layer fallback: subresources (fonts/images) a page references as game:// reach here;
   top-level loads were already mapped in MbLoadURL. FALSE = continue loading (rewritten). */
static BOOL MB_CALL_TYPE loadUrlBeginThunk(mbWebView, void*, const char* url, void* job)
{
    if (url && strncmp(url, "game://", 7) == 0)
    {
        char buf[512];
        const char* mapped = mapGameUrl(url, buf, sizeof(buf));
        MbLogf("loadUrlBegin: rewrite %s -> %s", url, mapped);
        if (mbNetChangeRequestUrl) mbNetChangeRequestUrl(job, mapped);
    }
    return FALSE;
}

/* Nav-lock allowlist. The engine only ever legitimately loads our own server (which the
   socket-level redirect resolves to local or the configured remote) or the packaged game://
   scheme. Everything else is third-party and must not render in the engine. */
static bool hostIs(const char* host, const char* name)
{
    size_t n = strlen(name);
    return strncmp(host, name, n) == 0 && (host[n] == 0 || host[n] == '/' || host[n] == ':');
}
static bool isFirstParty(const char* url)
{
    if (!url) return false;
    if (strncmp(url, "game://", 7) == 0) return true;
    if (strncmp(url, "about:blank", 11) == 0) return true;
    const char* host = 0;
    if      (strncmp(url, "http://",  7) == 0) host = url + 7;
    else if (strncmp(url, "https://", 8) == 0) host = url + 8;
    if (!host) return false;
    return hostIs(host, "localhost") || hostIs(host, "127.0.0.1");
}
static void openInSystemBrowser(const char* url)
{
    if (!url) return;
    WCHAR w[1024]; int n = 0;
    for (; url[n] && n < 1023; ++n) w[n] = (WCHAR)(unsigned char)url[n];
    w[n] = 0;
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    MbLogf("nav-lock: external -> system browser: %s", url);
}

/* window.open / target=_blank — never spawn a child OS window; external targets go to the OS
   browser, first-party popups are just suppressed. */
static mbWebView MB_CALL_TYPE createViewThunk(mbWebView, void*, mbNavigationType,
                                              const utf8* url, const mbWindowFeatures*)
{
    if (!isFirstParty(url)) openInSystemBrowser(url);   /* external popup -> OS browser */
    return NULL_WEBVIEW;                                /* never spawn a child OS window */
}

/* JS dialogs: never show mb's native dialogs over the game window — log instead. */
static void MB_CALL_TYPE alertThunk(mbWebView, void*, const utf8* msg)
{
    MbLogf("JS alert: %s", msg ? msg : "");
}
static BOOL MB_CALL_TYPE confirmThunk(mbWebView, void*, const utf8* msg)
{
    MbLogf("JS confirm (auto-true): %s", msg ? msg : "");
    return TRUE;
}
static mbStringPtr MB_CALL_TYPE promptThunk(mbWebView, void*, const utf8* msg,
                                            const utf8*, BOOL* result)
{
    MbLogf("JS prompt (auto-cancel): %s", msg ? msg : "");
    if (result) *result = FALSE;
    return 0;
}

/* Nav-lock: confine top-level navigation to first-party origins; external links go to the OS
   browser. (Threat: in-page content pointing the engine at a third-party host. The connected
   server being malicious is the renderer's problem, mitigated by the modern engine, not here.) */
static BOOL MB_CALL_TYPE navigationThunk(mbWebView, void*, mbNavigationType, const utf8* url)
{
    if (isFirstParty(url)) return TRUE;        /* allow first-party page loads */
    openInSystemBrowser(url);                  /* external link -> OS browser */
    return FALSE;                              /* cancel in-engine navigation */
}

/* Per-view storage rooted in the ReCapWebKit data folder (next to EAWebKit.dll, module-relative:
   survives the game's DLL reload; per-install, not per-cwd) so login/session cookies + localStorage
   persist across views and the game bin stays tidy. */
static void setupCookieJar(mbWebView wv)
{
    WCHAR dir[MAX_PATH];
    int dn = MbDataDirW(dir, MAX_PATH);          /* "<dll dir>\ReCapWebKit\" (created) */
    if (!dn || dn + 24 >= MAX_PATH) return;

    WCHAR path[MAX_PATH];

    _snwprintf(path, MAX_PATH - 1, L"%scookies.dat", dir); path[MAX_PATH - 1] = 0;
    mbSetCookieEnabled(wv, TRUE);
    mbSetCookieJarFullPath(wv, path);

    _snwprintf(path, MAX_PATH - 1, L"%sLocalStorage", dir); path[MAX_PATH - 1] = 0;
    mbSetLocalStorageFullPath(wv, path);

    MbLog("setupCookieJar: cookies.dat + LocalStorage in ReCapWebKit data folder");
}

MbView* MbCreate(int w, int h, MbPaintSink sink, void* user)
{
    MbLogf("MbCreate: w=%d h=%d", w, h);
    if (!ensureInit()) { MbLog("MbCreate: ensureInit FAILED -> null"); return 0; }
    MbView* self = new MbView();
    memset(self, 0, sizeof(*self));
    self->sink   = sink;
    self->user   = user;
    self->width  = w;
    self->height = h;
    self->wv     = mbCreateWebView();
    MbLogf("MbCreate: mbCreateWebView -> %p", (void*)self->wv);
    /* Keyboard model (canonical mb host-window-owns-input): on a fired mouse-down mb's
       onMouseMessage does ::SetFocus(this hidden window) + ::SetCapture (native focus/capture ON).
       The hidden window lives on the game UI thread, so ::SetFocus SUCCEEDS -> the hidden window
       gets OS keyboard focus and blink goes active (caret/editing enabled). The game's main window
       then loses focus, so we can no longer rely on View::OnKeyboardEvent forwarding — instead
       RecapMbWndProc (which now owns OS keyboard focus) forwards WM_KEYDOWN/UP/CHAR straight to mb.
       This is what makes "click the field -> type" work without an alt-tab. */
    /* CSS px == surface px == physical px. The process is DPI-aware (sharp window + correct
       input), but that makes mb auto-read the real system DPI (GetDeviceCaps LOGPIXELSX, etc.)
       and scale its layout. RecapDpiHooks (ReCapHooks) feeds mb 96 DPI at the source so it
       behaves like the known-good DPI-UNAWARE state — no per-view size juggling needed here. */
    self->hwnd = createHiddenWindow(w, h);
    MbLogf("MbCreate: hidden hwnd=%p (%dx%d)", (void*)self->hwnd, w, h);
    if (self->hwnd) SetWindowLongPtrA(self->hwnd, GWLP_USERDATA, (LONG_PTR)self);  /* for RecapMbWndProc */
    if (self->hwnd) mbSetHandle(self->wv, self->hwnd);   /* set handle BEFORE resize */
    mbResize(self->wv, w, h);
    /* Register both callbacks — bit-updated preferred; HDC fallback for mb132 default mode */
    mbOnPaintBitUpdated(self->wv, paintBitThunk, self);
    mbOnPaintUpdated(self->wv, paintHDCThunk, self);
    mbOnConsole(self->wv, consoleThunk, self);   /* surface launcher JS console/errors */
    mbOnLoadUrlBegin(self->wv, loadUrlBeginThunk, self);   /* game:// subresource rewrite */
    mbOnCreateView(self->wv, createViewThunk, self);       /* suppress popup OS windows */
    mbOnAlertBox(self->wv, alertThunk, self);              /* no native dialogs over the game */
    mbOnConfirmBox(self->wv, confirmThunk, self);
    mbOnPromptBox(self->wv, promptThunk, self);
    mbOnNavigation(self->wv, navigationThunk, self);
    mbSetContextMenuEnabled(self->wv, FALSE);              /* no right-click menu in-game */
    setupCookieJar(self->wv);
    rebuildDib(self, w, h);
    MbLog("MbCreate: done");
    return self;
}

void MbLoadHTML(MbView* v, const char* html, const char* baseUrl)
{
    MbLogf("MbLoadHTML base=%s", baseUrl ? baseUrl : "about:blank");
    if (v) mbLoadHtmlWithBaseUrl(v->wv, html, baseUrl ? baseUrl : "about:blank");
}
void MbLoadURL(MbView* v, const char* url)
{
    /* mb has no 'game://' scheme handler (EAWebKit's packaged-resource protocol) — an
       unmapped game:// load fails -> no paint -> black surface. Map to the ReCap server. */
    char buf[512];
    const char* effective = mapGameUrl(url, buf, sizeof(buf));
    MbLogf("MbLoadURL: %s%s", effective ? effective : "(null)",
           (effective != url) ? "  (mapped from game://)" : "");
    if (v) mbLoadURL(v->wv, effective);
}

void MbSetUserAgent(MbView* v, const char* ua)
{
    if (!v || !ua) return;
    MbLogf("MbSetUserAgent: %s", ua);
    mbSetUserAgent(v->wv, ua);
}

void MbSetTransparent(MbView* v, int transparent)
{
    if (!v) return;
    v->transparent = transparent ? 1 : 0;
    mbSetTransparent(v->wv, v->transparent ? TRUE : FALSE);
    MbLogf("MbSetTransparent: %d", v->transparent);
}

void MbReload(MbView* v)      { MbLog("MbReload");      if (v) mbReload(v->wv); }
void MbStopLoading(MbView* v) { MbLog("MbStopLoading"); if (v) mbStopLoading(v->wv); }
int  MbGoBack(MbView* v)      { MbLog("MbGoBack");      if (!v) return 0; mbGoBack(v->wv);    return 1; }
int  MbGoForward(MbView* v)   { MbLog("MbGoForward");   if (!v) return 0; mbGoForward(v->wv); return 1; }

int MbGetURL(MbView* v, char* buf, unsigned bufSize)
{
    if (!v || !buf || !bufSize) return 0;
    {
        const utf8* u = mbGetUrl(v->wv);
        unsigned n = 0;
        if (u) for (; u[n] && n < bufSize - 1; ++n) buf[n] = u[n];
        buf[n] = 0;
        return (int)n;
    }
}
void MbResize(MbView* v, int w, int h)
{
    if (!v) return;
    MbLogf("MbResize: %dx%d", w, h);
    v->width = w; v->height = h;
    if (v->hwnd) SetWindowPos(v->hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    mbResize(v->wv, w, h);
    rebuildDib(v, w, h);
}
static void MB_CALL_TYPE docReadyThunk(mbWebView, void* param, mbWebFrameHandle)
{
    MbView* self = (MbView*)param;
    if (self && self->loadSink) self->loadSink(self->loadUser);
}

void MbSetLoadSink(MbView* v, MbLoadSink sink, void* user)
{
    if (!v) return;
    v->loadSink = sink; v->loadUser = user;
    mbOnDocumentReady(v->wv, docReadyThunk, v);
}

void MbWake(MbView* v)              { if (v) mbWake(v->wv); }
int  MbConsumeDirty(MbView* v)      { if (!v || !v->dirty) return 0; v->dirty = 0; return 1; }
void MbSetHandle(MbView* v, HWND hwnd) { if (v) mbSetHandle(v->wv, hwnd); }

static unsigned int modifierFlags(int shift, int ctrl)
{
    return (shift ? (unsigned int)MB_SHIFT : 0) | (ctrl ? (unsigned int)MB_CONTROL : 0);
}

void MbFireMouseMove(MbView* v, int x, int y, int shift, int ctrl)
{
    if (!v) return;
    v->lastX = x; v->lastY = y;
    mbFireMouseEvent(v->wv, MB_MSG_MOUSEMOVE, x, y, modifierFlags(shift, ctrl));  /* mb132 ignores synthetic MOVE; shim below */
    /* mb132's offscreen engine drops synthetic mousemove, so CSS :hover never updates. Drive it via
       the injected JS shim __rh(x,y) (ReCapJsBridge) — it hit-tests (elementFromPoint, iframe-aware)
       and toggles a .rhover class the page CSS mirrors onto :hover. Throttle by delta to stay cheap. */
    {
        int dx = x - v->hoverX, dy = y - v->hoverY;
        if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
        if (dx + dy >= 3)
        {
            v->hoverX = x; v->hoverY = y;
            char js[48];
            _snprintf(js, sizeof(js) - 1, "window.__rh&&__rh(%d,%d)", x, y); js[sizeof(js) - 1] = 0;
            MbRunJs(v, js);
        }
    }
    mbWake(v->wv);
}
void MbFireMouseButton(MbView* v, int button, int down, int x, int y, int shift, int ctrl)
{
    if (!v) return;
    MbLogf("MbFireMouseButton: wv=%p btn=%d down=%d at %d,%d", (void*)v->wv, button, down, x, y);
    v->lastX = x; v->lastY = y;
    unsigned int msg, flags = modifierFlags(shift, ctrl);
    if (button == 2)      { msg = down ? MB_MSG_RBUTTONDOWN : MB_MSG_RBUTTONUP; if (down) flags |= MB_RBUTTON; }
    else if (button == 1) { msg = down ? MB_MSG_MBUTTONDOWN : MB_MSG_MBUTTONUP; if (down) flags |= MB_MBUTTON; }
    else                  { msg = down ? MB_MSG_LBUTTONDOWN : MB_MSG_LBUTTONUP; if (down) flags |= MB_LBUTTON; }
    mbFireMouseEvent(v->wv, msg, x, y, flags);
    mbWake(v->wv);
}
void MbFireWheel(MbView* v, int x, int y, int delta)
{
    if (v) mbFireMouseWheelEvent(v->wv, x, y, delta, 0);
}
/* The last webview given focus. miniblink's focus is process-exclusive (one focused
   webview at a time), but the in-game UI hosts several overlapping mb webviews (announce
   banner / mainwebview / child page) and UTFWin routes keyboard to a FIXED host View.
   Clicking a different webview stole mb focus from the keyboard's target -> typing died
   until alt-tab re-focused it. Route keys to whoever was focused last (= last clicked),
   so keyboard always follows focus and clicking back into a field restores typing. */
static MbView* g_activeView = 0;

void MbFireKey(MbView* v, unsigned id, int isChar, int down)
{
    MbView* t = g_activeView ? g_activeView : v;
    if (!t) return;
    MbLogf("MbFireKey: wv=%p (active) id=0x%x char=%d down=%d", (void*)t->wv, id, isChar, down);
    /* NOTE: do NOT mbSetFocus here — re-focusing the webview every keystroke blurs the focused
       <input> element (focus resets to document body) -> typing dies after the first key.
       Focus is set once on click/focus-change in MbSetFocus; keys just route to the active view. */
    if (isChar)    mbFireKeyPressEvent(t->wv, id, 0, FALSE);
    else if (down) mbFireKeyDownEvent(t->wv, id, 0, FALSE);
    else           mbFireKeyUpEvent(t->wv, id, 0, FALSE);
    mbWake(t->wv);
}
void MbSetFocus(MbView* v, int focus)
{
    if (!v) return;
    MbLogf("MbSetFocus: wv=%p focus=%d", (void*)v->wv, focus);
    /* Keyboard follows the last-focused webview (overlapping in-game webviews; UTFWin routes
       keyboard to a fixed host View). The actual page activation on click is handled by mb's
       blink-side setFocus+setIsActive in onMouseMessage — see the disableNativeSetFocus note in
       MbCreate for why that path now works without stealing the game window's OS focus. */
    if (focus) { g_activeView = v; mbSetFocus(v->wv); } else mbKillFocus(v->wv);
}
void MbDestroy(MbView* v)
{
    if (!v) return;
    if (g_activeView == v) g_activeView = 0;   /* don't leave keyboard routing pointing at a freed view */
    MbReleaseJsView(v);                        /* free the JS-bridge slot before the MbView* is recycled */
    mbDestroyWebView(v->wv);
    if (v->dibBitmap) DeleteObject(v->dibBitmap);
    if (v->dibDC)     DeleteDC(v->dibDC);
    if (v->hwnd)      DestroyWindow(v->hwnd);
    delete v;
}
void* MbGetRawView(MbView* v) { return v ? (void*)v->wv : 0; }

} // namespace recap
