#ifndef RECAP_MINIBLINK_BACKEND_H
#define RECAP_MINIBLINK_BACKEND_H

#include "ReCapWebEngine.h"

namespace recap {

/* IWebView backed by a MiniBlink MbView*. Every method forwards to the recap::Mb* wrapper. */
class MiniBlinkWebView : public IWebView {
public:
    explicit MiniBlinkWebView(MbView* view) : m_view(view) {}

    void LoadURL(const char* url);
    void LoadHTML(const char* html, const char* baseUrl);
    void Resize(int w, int h);
    void Wake();
    int  ConsumeDirty();
    void SetLoadSink(MbLoadSink sink, void* user);

    void FireMouseMove(int x, int y, int shift, int ctrl);
    void FireMouseButton(int button, int down, int x, int y, int shift, int ctrl);
    void FireWheel(int x, int y, int delta);
    void FireKey(unsigned id, int isChar, int down);
    void SetFocus(int focus);

    void SetUserAgent(const char* ua);
    void SetTransparent(int transparent);
    void Reload();
    void StopLoading();
    int  GoBack();
    int  GoForward();
    int  GetURL(char* buf, unsigned bufSize);

    void RunJs(const char* script);
    int  RunJsSync(const char* script, JsVal* ret, char* strBuf, unsigned strBufSize);
    void CreateJsObject(const char* obj);
    void BindJsMethod(const char* obj, const char* method, MbJsSink sink, void* user);

    void Destroy();

private:
    MbView* m_view;
};

class MiniBlinkEngine : public IWebEngine {
public:
    bool      Available();
    IWebView* CreateView(int w, int h, MbPaintSink sink, void* user);
};

} // namespace recap

#endif /* RECAP_MINIBLINK_BACKEND_H */
