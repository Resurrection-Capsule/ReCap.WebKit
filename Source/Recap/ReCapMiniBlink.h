#ifndef RECAP_MINIBLINK_H
#define RECAP_MINIBLINK_H

/* Modern-engine (MiniBlink "mb" API) backend for the EAWebKit View.
   Hides all mb/DLL details. A paint sink receives 32-bit BGRA frames; the View copies
   them into its ISurface. */

/* Forward-declare HWND without pulling in windows.h */
struct HWND__;
typedef struct HWND__* HWND;

namespace recap {

typedef void (*MbPaintSink)(void* user, const void* bgra, int srcStride,
                            int x, int y, int w, int h);

/* Fired (on the UI thread) when the page's document is ready — lets the View emit the
   EAWebKit load notifications the host waits on before showing its window. */
typedef void (*MbLoadSink)(void* user);

struct MbView; // opaque

/* True once the MiniBlink DLL is loaded + mbInit succeeded. */
bool MbAvailable();

/* Offscreen view sized w x h; frames delivered to sink(user, ...). */
MbView* MbCreate(int w, int h, MbPaintSink sink, void* user);
void    MbLoadHTML(MbView*, const char* utf8Html, const char* utf8BaseUrl);
void    MbLoadURL(MbView*, const char* utf8Url);
void    MbResize(MbView*, int w, int h);
void    MbSetLoadSink(MbView*, MbLoadSink sink, void* user);
void    MbWake(MbView*);

/* UA must contain "EAWebKit" — the served pages gate on it (eawebkit.js isUserAgent). */
void    MbSetUserAgent(MbView*, const char* utf8UserAgent);
/* Honor ViewParameters.mbTransparentBackground: transparent views keep mb's real alpha
   (announce slot renders invisible when empty); opaque views get alpha forced 0xFF. */
void    MbSetTransparent(MbView*, int transparent);

void    MbReload(MbView*);
void    MbStopLoading(MbView*);
/* mb history depth is only queryable async (mbCanGoBack callback) — these fire the
   navigation and report whether it was dispatched, not whether history existed. */
int     MbGoBack(MbView*);
int     MbGoForward(MbView*);
/* Copies the current URL (utf8) into buf; returns its length (0 = none). */
int     MbGetURL(MbView*, char* buf, unsigned bufSize);
/* Returns 1 (and clears) if the surface was repainted since the last call — lets the
   View coalesce ViewNotification draw events to one per frame (from Tick, on the UI
   thread) instead of firing them re-entrantly inside the paint callback. */
int     MbConsumeDirty(MbView*);
/* Associate a Win32 HWND with the view so mb can post paint notifications. */
void    MbSetHandle(MbView*, HWND hwnd);

/* Input forwarding (button: 0=left, 1=middle, 2=right — matches EAWebKit kMouse*). */
void    MbFireMouseMove(MbView*, int x, int y, int shift, int ctrl);
void    MbFireMouseButton(MbView*, int button, int down, int x, int y, int shift, int ctrl);
void    MbFireWheel(MbView*, int x, int y, int delta);
void    MbFireKey(MbView*, unsigned id, int isChar, int down);
void    MbSetFocus(MbView*, int focus);

void    MbDestroy(MbView*);

/* Returns the raw mbWebView handle (for advanced/diagnostic use). */
void*   MbGetRawView(MbView*);

} // namespace recap

#endif /* RECAP_MINIBLINK_H */
