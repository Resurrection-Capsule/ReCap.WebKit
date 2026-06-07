// RecapView.cpp — WebCore-free port of EA::WebKit::View, backed by MiniBlink.
//
// This is the clean-room reimplementation of EAWebKitView.cpp's RECAP_MINIBLINK path.
// The View vtable layout is defined by abi/EAWebKit/EAWebKitView.h (EA's public ABI);
// every virtual declared there is defined here so the vtable is complete and correctly
// ordered. The mb-backed methods carry the proven logic verbatim (paint sink, input,
// JS bridge, load sequence, Tick). Every WebCore-only method is a null/no-op stub.
//
// Build with RECAP_MINIBLINK defined. No WebCore / wtf / WebKit / EASTL headers are
// included — only EA's public abi/ headers, src/recap engine headers, and std/Windows.

#include <EAWebKit/EAWebKit.h>
#include <EAWebKit/EAWebKitView.h>
#include <EAWebKit/EAWebKitViewNotification.h>
#include <EAWebKit/EAWebKitInput.h>
#include <EAWebKit/EAWebKitJavascriptValue.h>
#include <EARaster/EARaster.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ReCapMiniBlink.h"
#include "ReCapLog.h"
#include "ReCapJsBridge.h"
#include "ReCapWebEngine.h"

#ifdef _MSC_VER
    #define snprintf _snprintf
#endif

// ---------------------------------------------------------------------------
// Helper sinks (ported verbatim from EAWebKitView.cpp ~lines 100-220).
// ---------------------------------------------------------------------------
namespace {

void RecapMbPaintSink(void* user, const void* bgra, int srcStride,
                      int x, int y, int w, int h)
{
    EA::WebKit::View* view = (EA::WebKit::View*)user;
    EA::Raster::ISurface* s = view ? view->GetSurface() : 0;
    if (!s || !bgra) return;
    unsigned char* dst = (unsigned char*)s->GetData();
    int dstStride = s->GetStride();
    int surfW = s->GetWidth(), surfH = s->GetHeight();
    if (!dst) return;
    { static int once = 0; if (!once) { once = 1;
        recap::MbLogf("PaintSink first: rect=%d,%d %dx%d srcStride=%d surf=%dx%d dstStride=%d data=%p",
                      x, y, w, h, srcStride, surfW, surfH, dstStride, (void*)dst); } }
    /* clamp the dirty rect to the surface to prevent OOB writes (mb dirty rects can exceed it) */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > surfW) w = surfW - x;
    if (y + h > surfH) h = surfH - y;
    if (w <= 0 || h <= 0) return;
    /* EA kPixelFormatTypeARGB == 0xAARRGGBB == BGRA byte order on LE; mb buffer is BGRA.
       Alpha arrives ALREADY policy-applied by ReCapMiniBlink's paint thunk: opaque views are
       forced 0xFF there (GDI BitBlt leaves alpha=0 -> invisible quad); transparent views
       (ViewParameters.mbTransparentBackground, e.g. the announce slot) keep mb's real alpha
       so unpainted areas stay invisible. Copy all 4 bytes verbatim. */
    for (int row = 0; row < h; ++row)
    {
        const unsigned char* sp = (const unsigned char*)bgra + (size_t)(y + row) * srcStride + (size_t)x * 4;
        unsigned char* dp = dst + (size_t)(y + row) * dstStride + (size_t)x * 4;
        memcpy(dp, sp, (size_t)w * 4);
    }
    /* NOTE: do NOT fire DrawEvent/ViewUpdate here. The paint callback can run during input
       processing; firing the host's draw notification per-paint starves the UI thread (lag,
       stuck hover, dead clicks, frozen drag). Instead we mark dirty (in ReCapMiniBlink) and
       View::Tick fires ONE coalesced ViewUpdate per frame on the UI thread. */
}

/* mb document-ready -> emit the EAWebKit load notifications the host waits on before it
   shows the launcher window (the WebCore path fires these; MiniBlink doesn't natively). */
void RecapMbLoadThunk(void* user)
{
    EA::WebKit::View* v = (EA::WebKit::View*)user;
    if (!v) return;
    EA::WebKit::ViewNotification* pVN = EA::WebKit::GetViewNotification();
    if (!pVN) return;
    EA::WebKit::LoadInfo& li = v->GetLoadInfo();
    li.mpView = v;
    li.mbStarted = true;
    recap::MbLog("RecapMbLoadThunk: firing LoadUpdate started/committed/layout/completed/willshow");
    li.mLET = EA::WebKit::kLETLoadStarted;    li.mbCompleted = false; pVN->LoadUpdate(li);
    li.mLET = EA::WebKit::kLETLoadCommitted;                          pVN->LoadUpdate(li);
    li.mLET = EA::WebKit::kLETLayoutCompleted;                        pVN->LoadUpdate(li);
    li.mLET = EA::WebKit::kLETLoadCompleted;   li.mbCompleted = true; pVN->LoadUpdate(li);
    li.mLET = EA::WebKit::kLETWillShow;                               pVN->LoadUpdate(li);
}

/* JS sink: translates recap::JsVal <-> EA::WebKit::JavascriptValue and calls
   ViewNotification::JavascriptMethodInvoked. The method key arrives as "obj.method";
   the host expects just the method name, so we pass the full key (host may strip obj prefix). */
void RecapMbJsSink(void* user, const char* method, const recap::JsVal* args, int argc, recap::JsVal* ret)
{
    EA::WebKit::View* v = (EA::WebKit::View*)user;
    EA::WebKit::ViewNotification* pVN = EA::WebKit::GetViewNotification();
    if (!v || !pVN) return;
    EA::WebKit::JavascriptMethodInvokedInfo info;
    info.mpView        = v;
    /* the bridge keys methods as "<obj>.<name>" (e.g. Client.playCurrentApp), but the game
       registered + dispatches by the BARE name ("playCurrentApp") — strip the obj prefix. */
    {
        const char* dot = strrchr(method, '.');
        info.mMethodName = dot ? dot + 1 : method;
    }
    info.mArgumentCount = (unsigned)argc;
    int i;
    for (i = 0; i < argc; ++i)
    {
        if (args[i].type == recap::kJsNumber)
        {
            info.mArguments[i].SetNumberValue(args[i].num);
        }
        else if (args[i].type == recap::kJsBoolean)
        {
            info.mArguments[i].SetBooleanValue(args[i].num != 0.0);
        }
        else if (args[i].type == recap::kJsString)
        {
            info.mArguments[i].SetStringType();
            const char* s = args[i].str ? args[i].str : "";
            char16_t w[1024]; int n = 0;
            for (; s[n] && n < 1023; ++n) w[n] = (char16_t)(unsigned char)s[n]; w[n] = 0;
            EA::WebKit::SetCharacters(w, info.mArguments[i].GetStringValue());  /* free fn, NUL-terminated */
        }
        else
        {
            info.mArguments[i].SetUndefined();
        }
    }
    info.mReturn.SetUndefined();
    pVN->JavascriptMethodInvoked(info);
    switch (info.mReturn.GetType())
    {
    case EA::WebKit::JavascriptValueType_Number:
        ret->type = recap::kJsNumber; ret->num = info.mReturn.GetNumberValue(); break;
    case EA::WebKit::JavascriptValueType_Boolean:
        ret->type = recap::kJsBoolean; ret->num = info.mReturn.GetBooleanValue() ? 1.0 : 0.0; break;
    case EA::WebKit::JavascriptValueType_String:
    {
        static char buf[4096];
        const char16_t* w = EA::WebKit::GetCharacters(info.mReturn.GetStringValue());  /* free fn */
        int n = 0; for (; w && w[n] && n < 4095; ++n) buf[n] = (char)w[n]; buf[n] = 0;
        ret->type = recap::kJsString; ret->str = buf; break;
    }
    default: ret->type = recap::kJsUndefined; break;
    }
}

} // namespace

// ---------------------------------------------------------------------------

namespace EA
{
namespace WebKit
{

// LoadInfo ctor — declared in the ABI (EAWebKitViewNotification.h) but defined here, as in
// EA's original EAWebKitView.cpp. Required so View's mLoadInfo member and ResetForNewLoad link.
LoadInfo::LoadInfo()
    : mpView(NULL),
      mLET(kLETNone),
      mbStarted(false),
      mbCompleted(false),
      mContentLength(-1),
      mProgressEstimation(0.0),
      mURI(),
      mResourceURI(),
      mPageTitle(),
      mLastChangedTime(0),
      mStatusCode(0)
{
}

// ===========================================================================
// Setup / teardown
// ===========================================================================

View::View()
  : mpWebView(0),
    mpSurface(0),
    mpRecapMb(0),
    mViewParameters(),
    mLoadInfo(),
    mCursorPos(0, 0),
    mpModalInputClient(0),
    mOverlaySurfaceArrayContainer(0),
    mLinkHookManager(this),
    mTextInputStateInfo(),
    mDebugger(0),
    mNodeListContainer(0),
    mBestNodeFrame(0),
    mBestNodeX(0),
    mBestNodeY(0),
    mBestNodeWidth(0),
    mBestNodeHeight(0),
    mCachedNavigationUpId(),
    mCachedNavigationDownId(),
    mCachedNavigationLeftId(),
    mCachedNavigationRightId(),
    mURI(),
    mJavascriptBindingObject(0),
    mJavascriptBindingObjectName(),
    mOwnsViewSurface(true),
    mEmulatingConsoleOnPC(false)
{
    // ReCap: WebCore-only bookkeeping (ViewArray, NodeListContainer,
    // OverlaySurfaceArrayContainer) is omitted — the mb path needs none of it.
}

View::~View()
{
    View::ShutdownView();
}

bool View::InitView(const ViewParameters& vp)
{
    recap::MbLogf("View::InitView begin: vp=%dx%d extSurf=%p transparent=%d",
                  vp.mWidth, vp.mHeight, (void*)vp.mpViewSurface, vp.mbTransparentBackground ? 1 : 0);
    if (mpSurface || mpWebView)   // Don't allow reinit of View. Return false instead.
        return false;

    mViewParameters = vp;

    if (!mpSurface)
    {
        bool isExternalSurfaceGood = false;

        if (vp.mpViewSurface)
        {
            isExternalSurfaceGood = true;
            if (vp.mpViewSurface->GetPixelFormat().mPixelFormatType != EA::Raster::kPixelFormatTypeARGB)
            {
                isExternalSurfaceGood = false;
            }
            else if ((vp.mpViewSurface->GetWidth() != vp.mWidth) || (vp.mpViewSurface->GetHeight() != vp.mHeight))
            {
                isExternalSurfaceGood = vp.mpViewSurface->Resize(vp.mWidth, vp.mHeight, false);
            }
        }

        if (!isExternalSurfaceGood)
        {
            mpSurface = GetEARasterInstance()->CreateSurface(vp.mWidth, vp.mHeight,
                EA::Raster::kPixelFormatTypeARGB, EA::Raster::kSurfaceCategoryMainView);
            mOwnsViewSurface = true;
        }
        else
        {
            mpSurface = vp.mpViewSurface;
            mOwnsViewSurface = false;
        }
    }

    recap::MbLog("View::InitView enter (RECAP_MINIBLINK)");
    if (mpSurface && recap::GetWebEngine()->Available())
    {
        int w = mpSurface->GetWidth(), h = mpSurface->GetHeight();
        mpRecapMb = recap::GetWebEngine()->CreateView(w, h, RecapMbPaintSink, this);
        recap::MbLog(mpRecapMb ? "View::InitView web engine ready" : "View::InitView engine create failed");
        if (mpRecapMb)
        {
            /* The served pages gate features on the UA containing "EAWebKit"
               (eawebkit.js isUserAgent: Client.closeWindow vs window.close) — mirror the
               WebCore path's "<mpUserAgent> EAWebKit/<version>" so those gates pass on mb. */
            {
                const Parameters& parameters = EA::WebKit::GetParameters();
                char userAgent[256];
                snprintf(userAgent, sizeof(userAgent) - 1, "%s EAWebKit/%s",
                         parameters.mpUserAgent ? parameters.mpUserAgent
                                                : "Mozilla/5.0 (Windows; U; en-US)",
                         EAWEBKIT_VERSION);
                userAgent[sizeof(userAgent) - 1] = 0;
                ((recap::IWebView*)mpRecapMb)->SetUserAgent(userAgent);
            }
            ((recap::IWebView*)mpRecapMb)->SetTransparent(vp.mbTransparentBackground ? 1 : 0);
            ((recap::IWebView*)mpRecapMb)->SetLoadSink(RecapMbLoadThunk, this);
            ((recap::IWebView*)mpRecapMb)->SetFocus(1);   /* start focused so hover/click register */
        }
    }

    return (mpSurface != NULL);
}

void View::ShutdownView()
{
    if (mJavascriptBindingObject)
    {
        // ReCap: mb owns the JS object; the binding-object pointer is never allocated on
        // this path (CreateJavascriptBindings goes straight to mb), so this stays NULL.
        mJavascriptBindingObject = NULL;
    }

    SetModalInput(NULL);

    if (mpRecapMb) { ((recap::IWebView*)mpRecapMb)->Destroy(); mpRecapMb = 0; }

    if (mpSurface)
    {
        if (mOwnsViewSurface)
            GetEARasterInstance()->DestroySurface(mpSurface);
        mpSurface = NULL;
    }
}

// ===========================================================================
// Runtime
// ===========================================================================

bool View::Tick()
{
    if (mpRecapMb)
    {
        ((recap::IWebView*)mpRecapMb)->Wake();   /* pump the web engine each frame */
        /* Coalesced draw notification: if mb repainted the surface since last frame, tell
           the host ONCE (here, on the UI thread) to blit it — never from the paint callback. */
        if (((recap::IWebView*)mpRecapMb)->ConsumeDirty() && mpSurface)
        {
            EA::WebKit::ViewNotification* pVN = EA::WebKit::GetViewNotification();
            if (pVN)
            {
                EA::WebKit::ViewUpdateInfo vui;
                vui.mpView = this; vui.mX = 0; vui.mY = 0;
                vui.mW = mpSurface->GetWidth(); vui.mH = mpSurface->GetHeight();
                vui.mDrawEvent = EA::WebKit::ViewUpdateInfo::kViewDrawStart;
                pVN->DrawEvent(vui);
                vui.mDrawEvent = EA::WebKit::ViewUpdateInfo::kViewDrawEnd;
                pVN->DrawEvent(vui);
                pVN->ViewUpdate(vui);
            }
        }
    }
    return true;
}

// ===========================================================================
// URI navigation
// ===========================================================================

bool View::SetURI(const char* pURI)
{
    if (mpRecapMb && pURI && pURI[0]) { ((recap::IWebView*)mpRecapMb)->LoadURL(pURI); return true; }
    return false;
}

const char16_t* View::GetURI()
{
    if (mpRecapMb)
    {
        char url[512];
        int len = ((recap::IWebView*)mpRecapMb)->GetURL(url, sizeof(url));
        char16_t w[512]; int i = 0;
        if (len > 511) len = 511;
        for (; i < len; ++i) w[i] = (char16_t)(unsigned char)url[i];
        w[i] = 0;
        EA::WebKit::SetCharacters(w, mURI);
        return EA::WebKit::GetCharacters(mURI);
    }
    return NULL;
}

bool View::CanGoBack(uint32_t /*count*/)    { return false; } // ReCap: mb history depth is async-only
bool View::CanGoForward(uint32_t /*count*/) { return false; } // ReCap: mb history depth is async-only

bool View::GoBack()
{
    /* mb history depth is only queryable async — dispatch and report it was issued */
    if (mpRecapMb) return ((recap::IWebView*)mpRecapMb)->GoBack() != 0;
    return false;
}

bool View::GoForward()
{
    if (mpRecapMb) return ((recap::IWebView*)mpRecapMb)->GoForward() != 0;
    return false;
}

void View::Refresh()
{
    if (mpRecapMb) { ((recap::IWebView*)mpRecapMb)->Reload(); return; }
}

void View::CancelLoad()
{
    if (mpRecapMb) { ((recap::IWebView*)mpRecapMb)->StopLoading(); return; }
}

// ===========================================================================
// Input events
// ===========================================================================

void View::OnKeyboardEvent(const KeyboardEvent& keyboardEvent)
{
    recap::MbLogf("key: view=%p mb=%p id=0x%x char=%d down=%d", (void*)this, mpRecapMb,
                  keyboardEvent.mId, keyboardEvent.mbChar ? 1 : 0, keyboardEvent.mbDepressed ? 1 : 0);
    if (mpRecapMb) { ((recap::IWebView*)mpRecapMb)->FireKey(keyboardEvent.mId,
        keyboardEvent.mbChar ? 1 : 0, keyboardEvent.mbDepressed ? 1 : 0); return; }
}

void View::OnMouseMoveEvent(const MouseMoveEvent& mouseMoveEvent)
{
    mCursorPos.set(mouseMoveEvent.mX, mouseMoveEvent.mY);
    if (mpRecapMb) { ((recap::IWebView*)mpRecapMb)->FireMouseMove(mouseMoveEvent.mX, mouseMoveEvent.mY,
        mouseMoveEvent.mbShift ? 1 : 0, mouseMoveEvent.mbControl ? 1 : 0); return; }
}

void View::OnMouseButtonEvent(const MouseButtonEvent& mouseButtonEvent)
{
    if (mpRecapMb)
    {
        if (mouseButtonEvent.mbDepressed) ((recap::IWebView*)mpRecapMb)->SetFocus(1);
        ((recap::IWebView*)mpRecapMb)->FireMouseButton((int)mouseButtonEvent.mId,
            mouseButtonEvent.mbDepressed ? 1 : 0, mouseButtonEvent.mX, mouseButtonEvent.mY,
            mouseButtonEvent.mbShift ? 1 : 0, mouseButtonEvent.mbControl ? 1 : 0);
        return;
    }
}

void View::OnMouseWheelEvent(const MouseWheelEvent& mouseWheelEvent)
{
    if (mpRecapMb) { ((recap::IWebView*)mpRecapMb)->FireWheel(mouseWheelEvent.mX, mouseWheelEvent.mY,
        mouseWheelEvent.mZDelta); return; }
}

void View::OnFocusChangeEvent(bool bHasFocus)
{
    recap::MbLogf("focus: view=%p mb=%p hasFocus=%d", (void*)this, mpRecapMb, bHasFocus ? 1 : 0);
    if (mpRecapMb) { ((recap::IWebView*)mpRecapMb)->SetFocus(bHasFocus ? 1 : 0); return; }
}

// ===========================================================================
// Javascript
// ===========================================================================

bool View::EvaluateJavaScript(const char* pScriptSource, size_t length, EA::WebKit::JavascriptValue* pReturnValue)
{
    // ASCII-only narrow path; widen and delegate to the char16_t overload (matches EA).
    if (mpRecapMb)
    {
        char16_t w[4096]; size_t n = 0;
        for (; n < length && n < 4095; ++n) w[n] = (char16_t)(unsigned char)pScriptSource[n];
        w[n] = 0;
        return EvaluateJavaScript(w, n, pReturnValue);
    }
    return false;
}

bool View::EvaluateJavaScript(const char16_t* pScriptSource, size_t length, EA::WebKit::JavascriptValue* pReturnValue)
{
    if (mpRecapMb)
    {
        char buf[4096]; size_t n = 0;
        for (; n < length && n < 4095; ++n) buf[n] = (char)pScriptSource[n]; buf[n] = 0;
        if (pReturnValue)
        {
            /* caller wants the result -> synchronous eval (mbRunJsSync) */
            recap::JsVal ret;
            static char strBuf[4096];   /* UI-thread only, consumed before return */
            ((recap::IWebView*)mpRecapMb)->RunJsSync(buf, &ret, strBuf, sizeof(strBuf));
            if (ret.type == recap::kJsNumber)
                pReturnValue->SetNumberValue(ret.num);
            else if (ret.type == recap::kJsBoolean)
                pReturnValue->SetBooleanValue(ret.num != 0.0);
            else if (ret.type == recap::kJsString && ret.str)
            {
                pReturnValue->SetStringType();
                char16_t w[4096]; int i = 0;
                for (; ret.str[i] && i < 4095; ++i) w[i] = (char16_t)(unsigned char)ret.str[i];
                w[i] = 0;
                EA::WebKit::SetCharacters(w, pReturnValue->GetStringValue());
            }
            else
                pReturnValue->SetUndefined();
        }
        else
            ((recap::IWebView*)mpRecapMb)->RunJs(buf);
        return true;
    }

    if (pReturnValue)
        pReturnValue->SetUndefined();
    return false;
}

void View::AttachJavascriptDebugger() {} // ReCap: stubbed — WebCore (KJS debugger)

// ===========================================================================
// Javascript binding
// ===========================================================================

void View::CreateJavascriptBindings(const char* bindingObjectName)
{
    EA::WebKit::SetCharacters((const char8_t*)bindingObjectName, mJavascriptBindingObjectName);
    if (mpRecapMb)
    {
        ((recap::IWebView*)mpRecapMb)->CreateJsObject(
            (const char*)EA::WebKit::GetCharacters(mJavascriptBindingObjectName));
        return;
    }
}

void View::RegisterJavascriptMethod(const char* name)
{
    if (mpRecapMb)
    {
        ((recap::IWebView*)mpRecapMb)->BindJsMethod(
            (const char*)EA::WebKit::GetCharacters(mJavascriptBindingObjectName), name, RecapMbJsSink, this);
        return;
    }
}

void View::UnregisterJavascriptMethod(const char* /*name*/)   {} // ReCap: stubbed — WebCore (BalObject)
void View::RegisterJavascriptProperty(const char* /*name*/)   {} // ReCap: stubbed — WebCore (BalObject)
void View::UnregisterJavascriptProperty(const char* /*name*/) {} // ReCap: stubbed — WebCore (BalObject)
void View::RebindJavascript()                                 {} // ReCap: stubbed — WebCore (re-add to JS window)

// ===========================================================================
// Controller navigation — all WebCore-only (no mb equivalent)
// ===========================================================================

void View::SetJumpNavigationParams(const JumpNavigationParams& jumpNavigationParams)
{
    mJumpNavigationParams = jumpNavigationParams; // harmless; navigation itself is stubbed
}

bool View::JumpToNearestElement(EA::WebKit::JumpDirection /*direction*/)                     { return false; } // ReCap: stubbed — WebCore
bool View::JumpToFirstLink(const char* /*jumpToClass*/, bool /*skipJumpIfAlreadyOver*/)      { return false; } // ReCap: stubbed — WebCore
bool View::JumpToId(const char* /*jumpToId*/)                                                { return false; } // ReCap: stubbed — WebCore
void View::AdvanceFocus(EA::WebKit::FocusDirection /*direction*/, const EA::WebKit::KeyboardEvent& /*event*/) {} // ReCap: stubbed — WebCore
bool View::ClickElementById(const char* /*id*/)                                              { return false; } // ReCap: stubbed — WebCore
bool View::ClickElementsByClass(const char* /*className*/)                                   { return false; } // ReCap: stubbed — WebCore
bool View::ClickElementsByIdOrClass(const char* /*idOrClassName*/)                           { return false; } // ReCap: stubbed — WebCore
bool View::Click()                                                                           { return false; } // ReCap: stubbed — WebCore
bool View::IsAlreadyOverNavigableElement()                                                   { return false; } // ReCap: stubbed — WebCore
void View::MoveMouseCursorToFocusElement()                                                   {} // ReCap: stubbed — WebCore
void View::MoveMouseCursorToNode(WebCore::Node* /*node*/, bool /*scrollIfNecessary*/)        {} // ReCap: stubbed — WebCore
void View::UpdateCachedHints(WebCore::Node* /*node*/)                                        {} // ReCap: stubbed — WebCore

void View::GetCursorPosition(int& x, int& y) const { x = mCursorPos.x; y = mCursorPos.y; }
void View::SetCursorPosition(int x, int y)         { mCursorPos.set(x, y); }

#if EAWEBKIT_ENABLE_JUMP_NAVIGATION_DEBUGGING
void View::DrawFoundNodes(DrawNodeCallback /*callback*/)                  {} // ReCap: stubbed — WebCore
void View::DrawBestNode(DrawNodeCallback /*callback*/)                    {} // ReCap: stubbed — WebCore
void View::DrawSearchAxes(DrawAxesCallback /*callback*/)                  {} // ReCap: stubbed — WebCore
void View::DrawRejectedByRadiusNodes(DrawNodeCallback /*callback*/)       {} // ReCap: stubbed — WebCore
void View::DrawRejectedByAngleNodes(DrawNodeCallback /*callback*/)        {} // ReCap: stubbed — WebCore
void View::DrawRejectedWouldBeTrappedNodes(DrawNodeCallback /*callback*/) {} // ReCap: stubbed — WebCore
#endif

// ===========================================================================
// Input fields — WebCore-only
// ===========================================================================

void View::EnterTextIntoSelectedInput(const char16_t* /*contentTextBuffer*/, bool /*textWasAccepted*/) {} // ReCap: stubbed — WebCore
void View::EnterTextIntoSelectedInput(const char* /*textBuffer*/, bool /*textWasAccepted*/)            {} // ReCap: stubbed — WebCore

uint32_t View::GetTextFromSelectedInput(char16_t* contentTextBuffer, const uint32_t maxcontentTextBufferLength,
                                        char16_t* titleTextBuffer, const uint32_t maxTitleTextBufferLength)
{
    // ReCap: stubbed — WebCore. NUL-terminate any provided buffers and report zero length.
    if (contentTextBuffer && maxcontentTextBufferLength) contentTextBuffer[0] = 0;
    if (titleTextBuffer && maxTitleTextBufferLength)     titleTextBuffer[0] = 0;
    return 0;
}

uint32_t View::GetTextFromSelectedInput(char8_t* contentTextBuffer, const uint32_t maxcontentTextBufferLength,
                                        char8_t* titleTextBuffer, const uint32_t maxTitleTextBufferLength)
{
    // ReCap: stubbed — WebCore.
    if (contentTextBuffer && maxcontentTextBufferLength) contentTextBuffer[0] = 0;
    if (titleTextBuffer && maxTitleTextBufferLength)     titleTextBuffer[0] = 0;
    return 0;
}

// ===========================================================================
// Content from application
// ===========================================================================

bool View::LoadResourceRequest(const WebCore::ResourceRequest& /*resourceRequest*/) { return false; } // ReCap: stubbed — WebCore

bool View::SetHTML(const char* pHTML, size_t length, const char* pBaseURL)
{
    return SetContent(pHTML, length, "text/html", "utf-8", pBaseURL);
}

bool View::SetContent(const void* pData, size_t length, const char* /*pMimeType*/,
                      const char* /*pEncoding*/, const char* pBaseURL)
{
    if (mpRecapMb)
    {
        /* mb loader path (mbLoadHtmlWithBaseUrl). Copy: pData need not be NUL-terminated. */
        char* pHtml = new char[length + 1];
        memcpy(pHtml, pData, length);
        pHtml[length] = 0;
        ((recap::IWebView*)mpRecapMb)->LoadHTML(pHtml, pBaseURL);
        delete[] pHtml;
        return true;
    }
    return false;
}

void View::SetElementTextById(const char* /*id*/, const char* /*text*/)              {} // ReCap: stubbed — WebCore
void View::SetElementText(WebCore::HTMLElement* /*htmlElement*/, const char* /*text*/) {} // ReCap: stubbed — WebCore

// ===========================================================================
// Misc
// ===========================================================================

LoadInfo& View::GetLoadInfo() { return mLoadInfo; }

EA::Raster::ISurface* View::GetSurface() const { return mpSurface; }

void View::GetSize(int& w, int& h) const
{
    if (mpSurface) { w = mpSurface->GetWidth(); h = mpSurface->GetHeight(); }
    else           { w = 0; h = 0; }
}

bool View::SetSize(int w, int h)
{
    bool bResult = true;

    if (!mpSurface || (mpSurface->GetWidth() != w) || (mpSurface->GetHeight() != h))
    {
        if (mpSurface)
            bResult = mpSurface->Resize(w, h, false);
        else
        {
            mpSurface = GetEARasterInstance()->CreateSurface(w, h,
                EA::Raster::kPixelFormatTypeARGB, EA::Raster::kSurfaceCategoryMainView);
            mOwnsViewSurface = true;
            bResult = (mpSurface != NULL);
        }
    }

    if (mpRecapMb) { ((recap::IWebView*)mpRecapMb)->Resize(w, h); }
    return bResult;
}

bool View::IsEmulatingConsoleOnPC() const               { return mEmulatingConsoleOnPC; }
void View::SetEmulatingConsoleOnPC(bool emulating)      { mEmulatingConsoleOnPC = emulating; }

// ===========================================================================
// Emulated keyboard — WebCore-only
// ===========================================================================

void View::AttachEventsToInputs(KeyboardCallback /*callback*/)                          {} // ReCap: stubbed — WebCore
void View::AttachEventToElementBtId(const char* /*id*/, KeyboardCallback /*callback*/)  {} // ReCap: stubbed — WebCore
void View::SetInputElementValue(WebCore::HTMLElement* /*htmlElement*/, char16_t* /*text*/) {} // ReCap: stubbed — WebCore

// ===========================================================================
// Scroll — WebCore-only
// ===========================================================================

void View::Scroll(int /*x*/, int /*y*/) {} // ReCap: stubbed — WebCore (main-frame scroll)

void View::Scroll(bool xAxisScroll, bool yAxisScroll, int xScrollLines, int xScrollDelta,
                  int yScrollLines, int yScrollDelta)
{
    // Preserve the wheel-event translation; OnMouseWheelEvent is mb-backed.
    MouseWheelEvent mouseWheelEvent;
    mouseWheelEvent.mX = mCursorPos.x;
    mouseWheelEvent.mY = mCursorPos.y;

    if (yAxisScroll)
    {
        mouseWheelEvent.mbShift = false;
        mouseWheelEvent.mLineDelta = (float)yScrollLines;
        mouseWheelEvent.mZDelta = yScrollDelta;
        OnMouseWheelEvent(mouseWheelEvent);
    }
    if (xAxisScroll)
    {
        mouseWheelEvent.mbShift = true;
        mouseWheelEvent.mLineDelta = (float)(-xScrollLines);
        mouseWheelEvent.mZDelta = -xScrollDelta;
        OnMouseWheelEvent(mouseWheelEvent);
    }
}

void View::GetScrollOffset(int& x, int& y) { x = 0; y = 0; } // ReCap: stubbed — WebCore

// ===========================================================================
// Application-facing extras
// ===========================================================================

double                  View::GetEstimatedProgress() const { return mLoadInfo.mbCompleted ? 1.0 : 0.0; }
TextInputStateInfo&     View::GetTextInputStateInfo()       { return mTextInputStateInfo; }
void                    View::ResetForNewLoad()             { mLoadInfo = LoadInfo(); }

// ===========================================================================
// Overlay surfaces — WebCore-driven compositing; not used by the mb path
// ===========================================================================

void View::SetOverlaySurface(EA::Raster::ISurface* /*pSurface*/, const EA::Raster::Rect& /*viewRect*/) {} // ReCap: stubbed — WebCore
void View::RemoveOverlaySurface(EA::Raster::ISurface* /*pSurface*/)                                     {} // ReCap: stubbed — WebCore
void View::BlitOverlaySurfaces()                                                                        {} // ReCap: stubbed — WebCore

// ===========================================================================
// Modal input
// ===========================================================================

bool View::SetModalInput(ModalInputClient* pModalInputClient)
{
    if (mpModalInputClient != pModalInputClient)
    {
        if (mpModalInputClient)
            mpModalInputClient->ModalEnd();

        mpModalInputClient = pModalInputClient;

        if (mpModalInputClient)
            mpModalInputClient->ModalBegin();
    }
    return true;
}

ModalInputClient* View::GetModalInputClient() const { return mpModalInputClient; }

// ===========================================================================
// Redraw / update — host-driven blit happens via Tick; these are no-ops on mb
// ===========================================================================

void View::ViewUpdated(int /*x*/, int /*y*/, int /*w*/, int /*h*/) {} // ReCap: stubbed — WebCore (overlay reblit)
void View::RedrawArea(int /*x*/, int /*y*/, int /*w*/, int /*h*/)  {} // ReCap: stubbed — WebCore (HTML-level redraw)

// ===========================================================================
// WebKit accessors — all WebCore; the mb path has no WebView/Frame/Page
// ===========================================================================

::WebView*          View::GetWebView() const                       { return 0; }  // ReCap: stubbed — WebCore
::WebFrame*         View::GetWebFrame(bool /*focusFrame*/) const    { return 0; }  // ReCap: stubbed — WebCore
WebCore::Frame*     View::GetFrame(bool /*focusFrame*/) const       { return 0; }  // ReCap: stubbed — WebCore
WebCore::FrameView* View::GetFrameView(bool /*focusFrame*/) const   { return 0; }  // ReCap: stubbed — WebCore
WebCore::Document*  View::GetDocument(bool /*focusFrame*/) const    { return 0; }  // ReCap: stubbed — WebCore
WebCore::Page*      View::GetPage() const                          { return 0; }  // ReCap: stubbed — WebCore

// ===========================================================================
// Private virtuals (in the vtable; must be defined)
// ===========================================================================

void View::ScrollOnJump(bool /*vertical*/, float /*numLinesDelta*/)                         {} // ReCap: stubbed — WebCore
bool View::JumpToNearestElement(EA::WebKit::JumpDirection /*direction*/, bool /*scrollIfNotFound*/) { return false; } // ReCap: stubbed — WebCore
void View::QueueRegionToDrawUpdate(int /*x*/, int /*y*/, int /*width*/, int /*height*/)     {} // ReCap: stubbed — WebCore

} // namespace WebKit
} // namespace EA
