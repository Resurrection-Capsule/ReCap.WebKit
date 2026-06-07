#include "ReCapMiniBlinkBackend.h"

namespace recap {

void MiniBlinkWebView::LoadURL(const char* url)                 { MbLoadURL(m_view, url); }
void MiniBlinkWebView::LoadHTML(const char* html, const char* baseUrl) { MbLoadHTML(m_view, html, baseUrl); }
void MiniBlinkWebView::Resize(int w, int h)                    { MbResize(m_view, w, h); }
void MiniBlinkWebView::Wake()                                  { MbWake(m_view); }
int  MiniBlinkWebView::ConsumeDirty()                          { return MbConsumeDirty(m_view); }
void MiniBlinkWebView::SetLoadSink(MbLoadSink sink, void* user){ MbSetLoadSink(m_view, sink, user); }

void MiniBlinkWebView::FireMouseMove(int x, int y, int shift, int ctrl) { MbFireMouseMove(m_view, x, y, shift, ctrl); }
void MiniBlinkWebView::FireMouseButton(int button, int down, int x, int y, int shift, int ctrl) { MbFireMouseButton(m_view, button, down, x, y, shift, ctrl); }
void MiniBlinkWebView::FireWheel(int x, int y, int delta)      { MbFireWheel(m_view, x, y, delta); }
void MiniBlinkWebView::FireKey(unsigned id, int isChar, int down) { MbFireKey(m_view, id, isChar, down); }
void MiniBlinkWebView::SetFocus(int focus)                     { MbSetFocus(m_view, focus); }

void MiniBlinkWebView::SetUserAgent(const char* ua)           { MbSetUserAgent(m_view, ua); }
void MiniBlinkWebView::SetTransparent(int transparent)        { MbSetTransparent(m_view, transparent); }
void MiniBlinkWebView::Reload()                              { MbReload(m_view); }
void MiniBlinkWebView::StopLoading()                         { MbStopLoading(m_view); }
int  MiniBlinkWebView::GoBack()                             { return MbGoBack(m_view); }
int  MiniBlinkWebView::GoForward()                         { return MbGoForward(m_view); }
int  MiniBlinkWebView::GetURL(char* buf, unsigned bufSize)  { return MbGetURL(m_view, buf, bufSize); }

void MiniBlinkWebView::RunJs(const char* script)            { MbRunJs(m_view, script); }
int  MiniBlinkWebView::RunJsSync(const char* script, JsVal* ret, char* strBuf, unsigned strBufSize) { return MbRunJsSync(m_view, script, ret, strBuf, strBufSize); }
void MiniBlinkWebView::CreateJsObject(const char* obj)      { MbCreateJsObject(m_view, obj); }
void MiniBlinkWebView::BindJsMethod(const char* obj, const char* method, MbJsSink sink, void* user) { MbBindJsMethod(m_view, obj, method, sink, user); }

void MiniBlinkWebView::Destroy()                            { MbDestroy(m_view); m_view = 0; delete this; }

bool MiniBlinkEngine::Available() { return MbAvailable(); }

IWebView* MiniBlinkEngine::CreateView(int w, int h, MbPaintSink sink, void* user)
{
    MbView* v = MbCreate(w, h, sink, user);
    if (!v) return 0;
    return new MiniBlinkWebView(v);
}

IWebEngine* GetWebEngine()
{
    static MiniBlinkEngine s_engine;
    return &s_engine;
}

} // namespace recap
