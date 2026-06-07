#ifndef RECAP_WEBENGINE_H
#define RECAP_WEBENGINE_H

/* Engine-neutral web view abstraction. EAWebKitView talks to IWebView/IWebEngine so the
   underlying engine (today MiniBlink) can be swapped without touching the consumer. Boundary
   types are the already-neutral typedefs from the mb wrapper headers (no mb/EAWebKit types here). */

#include "ReCapMiniBlink.h"   /* MbPaintSink, MbLoadSink */
#include "ReCapJsBridge.h"    /* JsVal, MbJsSink */

namespace recap {

struct IWebView {
    virtual ~IWebView() {}

    virtual void LoadURL(const char* url) = 0;
    virtual void LoadHTML(const char* html, const char* baseUrl) = 0;
    virtual void Resize(int w, int h) = 0;
    virtual void Wake() = 0;
    virtual int  ConsumeDirty() = 0;
    virtual void SetLoadSink(MbLoadSink sink, void* user) = 0;

    virtual void FireMouseMove(int x, int y, int shift, int ctrl) = 0;
    virtual void FireMouseButton(int button, int down, int x, int y, int shift, int ctrl) = 0;
    virtual void FireWheel(int x, int y, int delta) = 0;
    virtual void FireKey(unsigned id, int isChar, int down) = 0;
    virtual void SetFocus(int focus) = 0;

    virtual void SetUserAgent(const char* ua) = 0;
    virtual void SetTransparent(int transparent) = 0;
    virtual void Reload() = 0;
    virtual void StopLoading() = 0;
    virtual int  GoBack() = 0;
    virtual int  GoForward() = 0;
    virtual int  GetURL(char* buf, unsigned bufSize) = 0;

    virtual void RunJs(const char* script) = 0;
    virtual int  RunJsSync(const char* script, JsVal* ret, char* strBuf, unsigned strBufSize) = 0;
    virtual void CreateJsObject(const char* obj) = 0;
    virtual void BindJsMethod(const char* obj, const char* method, MbJsSink sink, void* user) = 0;

    virtual void Destroy() = 0;   /* tears down the view AND deletes this */
};

struct IWebEngine {
    virtual ~IWebEngine() {}
    virtual bool      Available() = 0;
    virtual IWebView* CreateView(int w, int h, MbPaintSink sink, void* user) = 0;  /* null on failure */
};

/* Process-wide engine singleton (MiniBlink today). */
IWebEngine* GetWebEngine();

} // namespace recap

#endif /* RECAP_WEBENGINE_H */
