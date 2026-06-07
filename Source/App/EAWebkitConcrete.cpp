// ReCap.WebKit — EAWebkitConcrete: the IEAWebkit implementation Darkspore.exe drives.
//
// Ported from EA's EAWebKit.cpp (the EAWebkitConcrete class + CreateEAWebkitInstance), made
// WebCore-free. Darkspore loads EAWebkit.dll, GetProcAddress("CreateEAWebkitInstance"), and from
// the returned IEAWebkit* it walks C++ vtables (IEAWebkit -> View -> ISurface -> ViewNotification)
// whose layout is fixed by abi/EAWebKit/*.
//
// What is real here (matches EA semantics): Init/Shutdown/Destroy, allocator, WebKitStatus,
// FileSystem get/set, ViewNotification get/set (+ the EA::WebKit::GetViewNotification() free
// function the View/binding code calls), CreateView/DestroyView + the View registry
// (GetViewCount/GetView/IsViewValid), Parameters get/set, EARaster instance get/set, GetVersion,
// GetTime.
//
// What is stubbed (return false/null/0/no-op): everything routed through WebCore/WTF/JavaScriptCore
// in EA's build — RAM/disk cache, cookies, network metrics, SSL certs, transport handlers,
// JavaScript value/hashmap helpers, header-map wrappers, string-wrapper helpers, glyph/font server
// wrappers, the WebView/WebFrame/Frame/FrameView -> View reverse lookups. These need WebCore, which
// we do not compile. Each is marked "ReCap: stubbed — needs WebCore".
//
// This file OWNS the real CreateEAWebkitInstance export (the placeholder src/EAWebkitExports.cpp is
// removed by the caller). No other translation unit may define that symbol or the EA::WebKit globals
// declared static/owned below.

#include <EAWebKit/EAWebKit.h>
#include <EAWebKit/EAWebKitView.h>
#include <EAWebKit/EAWebKitViewNotification.h>
#include <EAWebKit/EAWebKitFileSystem.h>
#include <EAWebKit/EAWebKitJavascriptValue.h>            // JavascriptValue (real new/delete)
#include <EAWebKit/internal/EAWebKitEASTLHelpers.h>      // GetVectorJavaScriptValue / GetHashMap / GetHashMapIterator
#include <EARaster/EARaster.h>

#include <windows.h>   // GetTickCount
#include <malloc.h>    // _aligned_malloc / _aligned_free / _aligned_realloc
#include <vector>
#include <cstring>

// ::WebView, ::WebFrame, WebCore::Frame, WebCore::FrameView are forward-declared by
// EAWebKitForwardDeclarations.h (pulled in via EAWebKit.h) — used only as opaque pointers in the
// reverse-lookup overloads, which are stubbed.

namespace EA
{
namespace WebKit
{

///////////////////////////////////////////////////////////////////////
// Default allocator (malloc-based). Ported verbatim from EAWebKit.cpp.
///////////////////////////////////////////////////////////////////////

namespace
{
    struct DefaultAllocator : public Allocator
    {
        void* Malloc(size_t size, int /*flags*/, const char* /*pName*/)
        {
            return _aligned_malloc(size, 1);
        }
        void* MallocAligned(size_t size, size_t alignment, size_t /*offset*/, int /*flags*/, const char* /*pName*/)
        {
            return _aligned_malloc(size, alignment);
        }
        void Free(void* p, size_t /*size*/)
        {
            _aligned_free(p);
        }
        void* Realloc(void* p, size_t size, int /*flags*/)
        {
            return _aligned_realloc(p, size, 1);
        }
    };

    Allocator*       gpAllocator       = nullptr;
    FileSystem*      gpFileSystem      = nullptr;
    ViewNotification* gpViewNotification = nullptr;
    IGlyphCache*     gpGlyphCache      = nullptr;   // ReCap: stored but unused — needs WebCore to wire in.
    IFontServer*     gpFontServer      = nullptr;   // ReCap: stored but unused — needs WebCore to wire in.
    EA::Raster::IEARaster* gpRasterInstance = nullptr;

    WebKitStatus    gWebKitStatus = kWebKitStatusInactive;

    // Live View registry. EA keeps the same list in EAWebKitView.cpp; we keep it here so this TU
    // owns GetViewCount/GetView/IsViewValid without pulling in WebCore.
    std::vector<View*>& ViewRegistry()
    {
        static std::vector<View*> s_views;
        return s_views;
    }
}

///////////////////////////////////////////////////////////////////////
// Global accessors (the free functions the View / JS-binding / input
// code call directly — see abi headers). These are the WebCore-free
// subset; the rest stay inside EAWebkitConcrete as no-ops.
///////////////////////////////////////////////////////////////////////

Allocator* GetAllocator()
{
    if (!gpAllocator)
    {
        static DefaultAllocator s_defaultAllocator;
        gpAllocator = &s_defaultAllocator;
    }
    return gpAllocator;
}

void SetAllocator(Allocator* pAllocator)
{
    gpAllocator = pAllocator;
}

void SetFileSystem(FileSystem* pFileSystem)
{
    gpFileSystem = pFileSystem;
}

FileSystem* GetFileSystem()
{
    return gpFileSystem;
}

void SetViewNotification(ViewNotification* pViewNotification)
{
    gpViewNotification = pViewNotification;
}

// Called by EAWebKitView / EAWebKitJavascriptBinding / EAWebKitEventListener (abi/internal).
ViewNotification* GetViewNotification()
{
    return gpViewNotification;
}

WebKitStatus GetWebKitStatus()
{
    return gWebKitStatus;
}

EA::Raster::IEARaster* GetEARasterInstance()
{
    // EA lazily installs a default EARasterConcrete when the app never sets one (EAWebKit.cpp:1035).
    // Darkspore relies on this — it never calls SetEARasterInstance, so View::InitView's
    // GetEARasterInstance()->CreateSurface() would null-deref without the lazy default.
    if (!gpRasterInstance)
    {
        static EA::Raster::EARasterConcrete s_defaultRaster;
        gpRasterInstance = &s_defaultRaster;
    }
    return gpRasterInstance;
}

void SetEARasterInstance(EA::Raster::IEARaster* pRasterInstance)
{
    gpRasterInstance = pRasterInstance;
}

///////////////////////////////////////////////////////////////////////
// Parameters. EA stores these in a static ParametersEx (EASTL-backed
// string copies). We keep a plain static Parameters — WebCore-free —
// since nothing in this build reads the WebPreferences side effects.
///////////////////////////////////////////////////////////////////////

// Parameters ctor — ported verbatim from EAWebKit.cpp:539 (the defaults the client relies on).
Parameters::Parameters()
:	mLogChannelFlags(kLogChannelNotYetImplemented | kLogChannelPopupBlocking | kLogChannelEvents),
	mpLocale(NULL),
	mpAcceptLanguageHttpHeaderValue(NULL),
	mpApplicationName(NULL),
	mpUserAgent(NULL),
	mMaxTransportJobs(32),
	mMaxTransportJobsSynchronous(2),
	mHttpRequestResponseBufferSize(4096),
	mTransportPollTimeSeconds(0.05),
	mPageTimeoutSeconds(kPageTimeoutDefault),
	mbEnableHttpPipelining(false),
	mbVerifyPeers(true),
	mbDrawIntermediatePages(true),
	mCaretBlinkSeconds(1.f),
	mCheckboxSize(kCheckboxSizeDefault),
	mSystemFontDescription(),
	mDefaultFontSize(16),
	mDefaultMonospaceFontSize(13),
	mMinimumFontSize(1),
	mMinimumLogicalFontSize(8),
	mJavaScriptEnabled(true),
	mJavaScriptCanOpenWindows(true),
	mPluginsEnabled(true),
	mPrivateBrowsing(false),
	mTabsToLinks(true),
	mFontSmoothingEnabled(false),
	mHistoryItemLimit(100),
	mHistoryAgeLimit(7),
	mbEnableFileTransport(true),
	mbEnableDirtySDKTransport(true),
	mbEnableCurlTransport(true),
	mbEnableUTFTransport(true),
	mbEnableImageCompression(false),
	mJavaScriptStackSize(128 * 1024),
	mEnableSmoothText(false),
	mSmoothDefaultTextSize(18.0f),
	mbEnableProfiling(false),
	mbEnableGammaCorrection(true),
	#if defined(_DEBUG)
	mbEnableJavaScriptDebugOutput(true),
	#else
	mbEnableJavaScriptDebugOutput(false),
	#endif
	mFireTimerRate(kFireTimerRate60Hz),
	mbEnableDefaultToolTip(false),
	mbEnableCrossDomainScripting(false)
{
	mColors[kColorActiveSelectionBack]         .setRGB(0xff3875d7);
	mColors[kColorActiveSelectionFore]         .setRGB(0xffd4d4d4);
	mColors[kColorInactiveSelectionBack]       .setRGB(0xff3875d7);
	mColors[kColorInactiveSelectionFore]       .setRGB(0xffd4d4d4);
	mColors[kColorActiveListBoxSelectionBack]  .setRGB(0xff3875d7);
	mColors[kColorActiveListBoxSelectionFore]  .setRGB(0xffd4d4d4);
	mColors[kColorInactiveListBoxSelectionBack].setRGB(0xff3875d7);
	mColors[kColorInactiveListBoxSelectionFore].setRGB(0xffd4d4d4);
	mFontFamilyStandard[0] = mFontFamilySerif[0] = mFontFamilySansSerif[0] = 0;
	mFontFamilyMonospace[0] = mFontFamilyCursive[0] = mFontFamilyFantasy[0] = 0;
}

Parameters& GetParameters()
{
    static Parameters s_parameters;
    return s_parameters;
}

void SetParameters(const Parameters& parameters)
{
    GetParameters() = parameters;
    // ReCap: EA also pushed font/JS/history settings into WebCore::WebPreferences here — stubbed,
    // needs WebCore.
}

///////////////////////////////////////////////////////////////////////
// Init / Shutdown. EA's versions spin up XMLHttpRequest listeners, the
// DeepSee trace framework and WebView static init — all WebCore. We just
// flip status and remember the allocator.
///////////////////////////////////////////////////////////////////////

bool Init(Allocator* pAllocator)
{
    gpAllocator = pAllocator;
    gWebKitStatus = kWebKitStatusInitializing;
    // ReCap: WebCore static init (XMLHttpRequest listeners, DeepSee, WebView) stubbed — needs WebCore.
    gWebKitStatus = kWebKitStatusActive;
    return true;
}

void Shutdown()
{
    gWebKitStatus = kWebKitStatusShuttingDown;
    // ReCap: WebCore static finalize (WebView::unInit*, BalClass cleanup) stubbed — needs WebCore.
    gWebKitStatus = kWebKitStatusInactive;
}

const char* GetVersion()
{
    return "ReCap-EAWebKit";
}

double GetTime()
{
    // ReCap: EA returned WebCore::currentTime(). Provide a real clock so timer-driven callers behave.
    return (double)::GetTickCount() / 1000.0;
}

///////////////////////////////////////////////////////////////////////
// View lifetime + enumeration.
//
// EA's free function EA::WebKit::CreateView() does `new View` and the
// caller (here, EAWebkitConcrete::CreateView) hands it back; the consumer
// then calls View::InitView(). DestroyView deletes it. The View ctor/dtor
// + InitView live in the separate RecapView.cpp port (src/view/RecapView.cpp);
// they link in once that TU is added to the build. This TU owns the registry.
///////////////////////////////////////////////////////////////////////

View* CreateView()
{
    View* pView = new View;          // View::View() resolved from RecapView.cpp at link time.
    ViewRegistry().push_back(pView);
    return pView;
}

void DestroyView(View* pView)
{
    if (!pView)
        return;
    std::vector<View*>& views = ViewRegistry();
    for (std::vector<View*>::iterator it = views.begin(); it != views.end(); ++it)
    {
        if (*it == pView)
        {
            views.erase(it);
            break;
        }
    }
    delete pView;                    // View::~View() resolved from RecapView.cpp at link time.
}

int GetViewCount()
{
    return (int)ViewRegistry().size();
}

View* GetView(int index)
{
    std::vector<View*>& views = ViewRegistry();
    if (index >= 0 && index < (int)views.size())
        return views[index];
    return nullptr;
}

bool IsViewValid(View* pView)
{
    std::vector<View*>& views = ViewRegistry();
    for (size_t i = 0; i < views.size(); ++i)
    {
        if (views[i] == pView)
            return true;
    }
    return false;
}

///////////////////////////////////////////////////////////////////////
// EAWebkitConcrete — implements every IEAWebkit virtual, in header order
// so the vtable matches Darkspore's expectation exactly.
///////////////////////////////////////////////////////////////////////

bool EAWebkitConcrete::Init(Allocator* pAllocator)
{
    return EA::WebKit::Init(pAllocator);
}

void EAWebkitConcrete::Shutdown()
{
    EA::WebKit::Shutdown();
}

void EAWebkitConcrete::Destroy()
{
    // EA: no-op (static instance, see CreateEAWebkitInstance). Matched.
}

Allocator* EAWebkitConcrete::GetAllocator()
{
    return EA::WebKit::GetAllocator();
}

void EAWebkitConcrete::SetFileSystem(FileSystem* pFileSystem)
{
    EA::WebKit::SetFileSystem(pFileSystem);
}

FileSystem* EAWebkitConcrete::GetFileSystem()
{
    return EA::WebKit::GetFileSystem();
}

void EAWebkitConcrete::SetViewNotification(ViewNotification* pViewNotification)
{
    EA::WebKit::SetViewNotification(pViewNotification);
}

ViewNotification* EAWebkitConcrete::GetViewNotification()
{
    return EA::WebKit::GetViewNotification();
}

IGlyphCache* EAWebkitConcrete::GetGlyphCache()
{
    return gpGlyphCache;   // ReCap: stubbed — needs WebCore (text/glyph subsystem).
}

void EAWebkitConcrete::SetGlyphCache(IGlyphCache* pGlyphCache)
{
    gpGlyphCache = pGlyphCache;   // ReCap: stubbed — needs WebCore.
}

IFontServer* EAWebkitConcrete::GetFontServer()
{
    return gpFontServer;   // ReCap: stubbed — needs WebCore (font subsystem).
}

void EAWebkitConcrete::SetFontServer(IFontServer* pFontServer)
{
    gpFontServer = pFontServer;   // ReCap: stubbed — needs WebCore.
}

void EAWebkitConcrete::SetHighResolutionTimer(EAWebKitTimerCallback /*timer*/)
{
    // ReCap: stubbed — needs WebCore (SharedTimer hookup).
}

void EAWebkitConcrete::SetStackBaseCallback(EAWebKitStackBaseCallback /*callback*/)
{
    // ReCap: stubbed — needs WebCore (JS GC stack base).
}

View* EAWebkitConcrete::CreateView()
{
    return EA::WebKit::CreateView();
}

void EAWebkitConcrete::DestroyView(View* pView)
{
    EA::WebKit::DestroyView(pView);
}

int EAWebkitConcrete::GetViewCount()
{
    return EA::WebKit::GetViewCount();
}

View* EAWebkitConcrete::GetView(int index)
{
    return EA::WebKit::GetView(index);
}

bool EAWebkitConcrete::IsViewValid(View* pView)
{
    return EA::WebKit::IsViewValid(pView);
}

void EAWebkitConcrete::SetParameters(const Parameters& parameters)
{
    EA::WebKit::SetParameters(parameters);
}

Parameters& EAWebkitConcrete::GetParameters()
{
    return EA::WebKit::GetParameters();
}

void EAWebkitConcrete::SetRAMCacheUsage(const RAMCacheInfo& /*ramCacheInfo*/)
{
    // ReCap: stubbed — needs WebCore (WebCore::cache / pageCache).
}

void EAWebkitConcrete::GetRAMCacheUsage(RAMCacheInfo& ramCacheInfo)
{
    ramCacheInfo = RAMCacheInfo();   // ReCap: stubbed — needs WebCore.
}

bool EAWebkitConcrete::SetDiskCacheUsage(const DiskCacheInfo& /*diskCacheInfo*/)
{
    return false;   // ReCap: stubbed — needs WebCore (ResourceHandleManager).
}

void EAWebkitConcrete::GetDiskCacheUsage(DiskCacheInfo& diskCacheInfo)
{
    diskCacheInfo = DiskCacheInfo();   // ReCap: stubbed — needs WebCore.
}

void EAWebkitConcrete::PurgeCache(bool /*bPurgeRAMCache*/, bool /*bPurgeFontCache*/, bool /*bPurgeDiskCache*/)
{
    // ReCap: stubbed — needs WebCore.
}

void EAWebkitConcrete::SetCookieUsage(const CookieInfo& /*cookieInfo*/)
{
    // ReCap: stubbed — needs WebCore (CookieManager).
}

void EAWebkitConcrete::GetCookieUsage(CookieInfo& cookieInfo)
{
    cookieInfo = CookieInfo();   // ReCap: stubbed — needs WebCore.
}

void EAWebkitConcrete::AddAllowedDomainInfo(const char8_t* /*allowedDomain*/, const char8_t* /*excludedPaths*/)
{
    // ReCap: stubbed — needs WebCore (EAWebKitDomainFilter). Default = no restriction.
}

bool EAWebkitConcrete::CanNavigateToURL(const char8_t* /*url*/)
{
    return true;   // ReCap: stubbed — needs WebCore. Permissive: allow navigation.
}

void EAWebkitConcrete::AddTransportHandler(TransportHandler* /*pTH*/, const char16_t* /*pScheme*/)
{
    // ReCap: stubbed — needs WebCore (transport subsystem).
}

void EAWebkitConcrete::RemoveTransportHandler(TransportHandler* /*pTH*/, const char16_t* /*pScheme*/)
{
    // ReCap: stubbed — needs WebCore.
}

TransportHandler* EAWebkitConcrete::GetTransportHandler(const char16_t* /*pScheme*/)
{
    return nullptr;   // ReCap: stubbed — needs WebCore.
}

// Delegate to the real free accessors (EAWebKitEASTLHelpers.cpp) — the wrappers are EASTL
// fixed_strings we own; returning null here would crash any client that reads a JS string result.
const char16_t* EAWebkitConcrete::GetCharacters(const EASTLFixedString16Wrapper& str)
{
    return EA::WebKit::GetCharacters(str);
}

void EAWebkitConcrete::SetCharacters(const char16_t* chars, EASTLFixedString16Wrapper& str)
{
    EA::WebKit::SetCharacters(chars, str);
}

const char8_t* EAWebkitConcrete::GetCharacters(const EASTLFixedString8Wrapper& str)
{
    return EA::WebKit::GetCharacters(str);
}

void EAWebkitConcrete::SetCharacters(const char8_t* chars, EASTLFixedString8Wrapper& str)
{
    EA::WebKit::SetCharacters(chars, str);
}

// JavascriptValue family — ported real from EAWebKitView.cpp:351 / EAWebKit.cpp:852. The client
// builds these to push lobby/friends data into the page; the stubs (returning null / empty)
// null-deref'd the client at lobby entry. JavascriptValue is a WebCore-free public type and the
// EASTL vector/hash_map helpers are linked, so these implement EA's semantics exactly.
JavascriptValue* EAWebkitConcrete::CreateJavaScriptValue()
{
    return new JavascriptValue();
}

void EAWebkitConcrete::DestroyJavaScriptValue(JavascriptValue* pValue)
{
    delete pValue;
}

JavascriptValue* EAWebkitConcrete::CreateJavaScriptValueArray(int count)
{
    return new JavascriptValue[count];
}

void EAWebkitConcrete::DestroyJavaScriptValueArray(JavascriptValue* pValues)
{
    delete[] pValues;
}

void EAWebkitConcrete::GetJavaScriptValues(const EASTLVectorJavaScriptValueWrapper& wrapper, JavascriptValue** ppValues, int* pSize)
{
    VectorJavaScriptValue* v = GetVectorJavaScriptValue(wrapper);
    if (pSize)    *pSize    = (int)v->size();
    if (ppValues) *ppValues = v->data();
}

void EAWebkitConcrete::SetJavaScriptValues(const JavascriptValue* pValues, int size, EASTLVectorJavaScriptValueWrapper& wrapper)
{
    GetVectorJavaScriptValue(wrapper)->assign(pValues, pValues + size);
}

EASTLJavascriptValueHashMapIteratorWrapper* EAWebkitConcrete::CreateJavascriptValueHashMapIterator()
{
    return new EASTLJavascriptValueHashMapIteratorWrapper();
}

void EAWebkitConcrete::DestroyHashMapIterator(EASTLJavascriptValueHashMapIteratorWrapper* i)
{
    delete i;
}

JavascriptValue* EAWebkitConcrete::GetJavascriptValue(const EASTLJavascriptValueHashMapWrapper& wrapper, const char16_t* key, bool createIfMissing)
{
    HashMapJavaScriptValue* hashMap = GetHashMap(wrapper);
    if (!createIfMissing)
    {
        HashMapJavaScriptValue::iterator it = hashMap->find(key);
        if (it == hashMap->end())
            return nullptr;
        return &it->second;
    }
    return &(*hashMap)[key];
}

void EAWebkitConcrete::SetHashMapIteratorToMapBegin(const EASTLJavascriptValueHashMapWrapper& wrapper, EASTLJavascriptValueHashMapIteratorWrapper* i)
{
    HashMapJavaScriptValue* hashMap = GetHashMap(wrapper);
    HashMapJavaScriptValue::iterator itr = hashMap->begin();
    *i = *reinterpret_cast<EASTLJavascriptValueHashMapIteratorWrapper*>(&itr);
}

JavascriptValue* EAWebkitConcrete::GetNextJavascriptValue(const EASTLJavascriptValueHashMapWrapper& wrapper, EASTLJavascriptValueHashMapIteratorWrapper* iteratorWrapper, JavascriptValue** valueOut, const char16_t** keyOut)
{
    HashMapJavaScriptValue* hashMap = GetHashMap(wrapper);
    HashMapJavaScriptValue::iterator* itr = GetHashMapIterator(*iteratorWrapper);

    JavascriptValue* value = nullptr;
    const char16_t* key = nullptr;
    if (*itr != hashMap->end())
    {
        value = &(*itr)->second;
        key   = (*itr)->first.c_str();
        (*itr)++;
    }
    if (valueOut) *valueOut = value;
    if (keyOut)   *keyOut   = key;
    return value;
}

void EAWebkitConcrete::RemoveJavascriptValue(const EASTLJavascriptValueHashMapWrapper& wrapper, const char16_t* key)
{
    GetHashMap(wrapper)->erase(key);
}

void EAWebkitConcrete::ClearJavascriptValues(const EASTLJavascriptValueHashMapWrapper& wrapper)
{
    GetHashMap(wrapper)->clear();
}

size_t EAWebkitConcrete::GetJavascriptValueCount(const EASTLJavascriptValueHashMapWrapper& wrapper)
{
    return GetHashMap(wrapper)->size();
}

void EAWebkitConcrete::SetDebugFileDumpStatus(const bool /*enabled*/)
{
    // ReCap: stubbed — needs WebCore (ResourceHandleManager debug dump).
}

void EAWebkitConcrete::RemoveCookies()
{
    // ReCap: stubbed — needs WebCore (CookieManager).
}

const char* EAWebkitConcrete::GetVersion()
{
    return EA::WebKit::GetVersion();
}

void EAWebkitConcrete::RegisterJavascriptDebugListener(EAWebKitJavascriptDebugListener* /*listener*/)
{
    // ReCap: stubbed — needs WebCore (JS debugger).
}

void EAWebkitConcrete::UnregisterJavascriptDebugListener(EAWebKitJavascriptDebugListener* /*listener*/)
{
    // ReCap: stubbed — needs WebCore.
}

void EAWebkitConcrete::SetEARasterInstance(EA::Raster::IEARaster* pRasterInstance)
{
    EA::WebKit::SetEARasterInstance(pRasterInstance);
}

EA::Raster::IEARaster* EAWebkitConcrete::GetEARasterInstance()
{
    return EA::WebKit::GetEARasterInstance();
}

WebKitStatus EAWebkitConcrete::GetWebKitStatus()
{
    return EA::WebKit::GetWebKitStatus();
}

void EAWebkitConcrete::AddCookie(const char8_t* /*pHeaderValue*/, const char8_t* /*pURI*/)
{
    // ReCap: stubbed — needs WebCore (CookieManager).
}

uint16_t EAWebkitConcrete::ReadCookies(char8_t** /*rawCookieData*/, uint16_t /*numCookiesToRead*/)
{
    return 0;   // ReCap: stubbed — needs WebCore.
}

void EAWebkitConcrete::GetNetworkMetrics(NetworkMetrics& metrics)
{
    metrics = NetworkMetrics();   // ReCap: stubbed — needs WebCore (ResourceHandleManager).
}

int32_t EAWebkitConcrete::LoadSSLCertificate(const uint8_t* /*pCACert*/, int32_t /*iCertSize*/)
{
    return -1;   // ReCap: stubbed — needs DirtySDK/WebCore (ProtoSSLSetCACert). EA returns -1 w/o DirtySDK.
}

void EAWebkitConcrete::ClearSSLCertificates()
{
    // ReCap: stubbed — needs DirtySDK/WebCore.
}

void EAWebkitConcrete::SetHeaderMapValue(EASTLHeaderMapWrapper& /*headerMapWrapper*/, const char16_t* /*pKey*/, const char16_t* /*pValue*/)
{
    // ReCap: stubbed — needs WebCore (EASTL HeaderMap internals).
}

const char16_t* EAWebkitConcrete::GetHeaderMapValue(const EASTLHeaderMapWrapper& /*headerMapWrapper*/, const char16_t* /*pKey*/)
{
    return nullptr;   // ReCap: stubbed — needs WebCore.
}

void EAWebkitConcrete::EraseHeaderMapValue(EASTLHeaderMapWrapper& /*headerMapWrapper*/, const char16_t* /*pKey*/)
{
    // ReCap: stubbed — needs WebCore.
}

uint32_t EAWebkitConcrete::SetTextFromHeaderMapWrapper(const EASTLHeaderMapWrapper& /*headerMapWrapper*/, char* pHeaderMapText, uint32_t textCapacity)
{
    if (pHeaderMapText && textCapacity)   // ReCap: stubbed — needs WebCore.
        pHeaderMapText[0] = 0;
    return 0;
}

bool EAWebkitConcrete::SetHeaderMapWrapperFromText(const char8_t* /*pHeaderMapText*/, uint32_t /*textSize*/, EASTLHeaderMapWrapper& /*headerMapWrapper*/, bool /*bExpectFirstCommandLine*/, bool /*bClearMap*/)
{
    return false;   // ReCap: stubbed — needs WebCore.
}

void EAWebkitConcrete::SetPlatformSocketAPI(EA::WebKit::PlatformSocketAPI& /*platformSocketAPI*/)
{
    // ReCap: stubbed — needs DirtySDK (PlatformSocketAPICallbacks).
}

double EAWebkitConcrete::GetTime()
{
    return EA::WebKit::GetTime();
}

void EAWebkitConcrete::ReattachCookies(TransportInfo* /*pTInfo*/)
{
    // ReCap: stubbed — needs WebCore (CookieManager + HeaderMap).
}

void EAWebkitConcrete::CookiesReceived(TransportInfo* /*pTInfo*/)
{
    // ReCap: stubbed — needs WebCore.
}

IGlyphCache* EAWebkitConcrete::CreateGlyphCacheWrapperInterface(void* /*pGlyphCache*/)
{
    return nullptr;   // ReCap: stubbed — needs WebCore/EAText (GlyphCacheProxy). Deprecated in EA too.
}

void EAWebkitConcrete::DestroyGlyphCacheWrapperInterface(IGlyphCache* /*pGlyphCacheInterface*/)
{
    // ReCap: stubbed — needs WebCore/EAText.
}

IFontServer* EAWebkitConcrete::CreateFontServerWrapperInterface(void* /*pFontServer*/)
{
    return nullptr;   // ReCap: stubbed — needs WebCore/EAText (FontServerProxy). Deprecated in EA too.
}

void EAWebkitConcrete::DestroyFontServerWrapperInterface(IFontServer* /*pFontServerInterface*/)
{
    // ReCap: stubbed — needs WebCore/EAText.
}

View* EAWebkitConcrete::GetView(::WebView* /*pWebView*/)
{
    return nullptr;   // ReCap: stubbed — needs WebCore (WebView->View reverse map).
}

View* EAWebkitConcrete::GetView(::WebFrame* /*pWebFrame*/)
{
    return nullptr;   // ReCap: stubbed — needs WebCore.
}

View* EAWebkitConcrete::GetView(WebCore::Frame* /*pFrame*/)
{
    return nullptr;   // ReCap: stubbed — needs WebCore.
}

View* EAWebkitConcrete::GetView(WebCore::FrameView* /*pFrameView*/)
{
    return nullptr;   // ReCap: stubbed — needs WebCore.
}

} // namespace WebKit
} // namespace EA


///////////////////////////////////////////////////////////////////////
// The single exported factory Darkspore.exe resolves by name. EA returns
// a function-local static so the instance is never freed through a foreign
// allocator (see EA's note in EAWebKit.cpp).
///////////////////////////////////////////////////////////////////////

extern "C" EA::WebKit::IEAWebkit* CreateEAWebkitInstance(void)
{
    ::OutputDebugStringA("[ReCap.WebKit] CreateEAWebkitInstance\n");
    static EA::WebKit::EAWebkitConcrete s;
    return &s;
}
