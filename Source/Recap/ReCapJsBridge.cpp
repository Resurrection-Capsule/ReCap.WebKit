#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* strtod (custom JSON number parse) */
#ifndef nullptr
#  define nullptr 0
#endif
#pragma warning(push)
#pragma warning(disable: 4191 4100 4127 4255 4505 4996 4244 4245 4267 4189)
#include "mb.h"
#pragma warning(pop)
#include "ReCapMiniBlink.h"
#include "ReCapJsBridge.h"
#include "ReCapLog.h"

/* JS bridge for mb132.
   mb132 injects window.mbQuery(customMsg, request, cb) into every page.
   JS calls  window.mbQuery(0, JSON.stringify({m:"method",a:[args]}), cb).
   Native receives request as a real string in recapQueryCb — no mailbox needed.
   Native replies via mbResponseQuery(wv, queryId, customMsg, responseStr),
   which fires cb(id, customMsg, responseStr) asynchronously in JS. */

namespace recap {

struct RecapBoundMethod {
    char              key[128];   /* "obj.method" */
    MbJsSink          sink;
    void*             user;
    RecapBoundMethod* next;
};

/* In-game the client keeps several webviews live at once (hub mainwebview + announce + register
   + child panels). 4 was far too few and slots were never freed (see MbReleaseJsView) — the hub
   exhausted them, its Client.* bindings silently failed, and the client null-derefed on a missing
   getter. Generous cap + proper release on destroy. */
#define RECAP_MAX_VIEWS 32

struct ViewRegistry {
    MbView*           view;
    RecapBoundMethod* methods;
    char              objName[64];   /* JS binding object, e.g. "Client" */
    int               hooked;        /* mbOnJsQuery registered */
    int               ctxHooked;     /* mbOnDidCreateScriptContext registered */
};

static ViewRegistry s_reg[RECAP_MAX_VIEWS];
static int          s_regCount = 0;

static ViewRegistry* getOrCreateReg(MbView* v)
{
    int i;
    for (i = 0; i < s_regCount; ++i)
        if (s_reg[i].view == v) return &s_reg[i];
    if (s_regCount >= RECAP_MAX_VIEWS) { MbLog("ReCapJsBridge: MAX_VIEWS exceeded"); return 0; }
    s_reg[s_regCount].view      = v;
    s_reg[s_regCount].methods   = 0;
    s_reg[s_regCount].objName[0] = 0;
    s_reg[s_regCount].hooked    = 0;
    s_reg[s_regCount].ctxHooked = 0;
    return &s_reg[s_regCount++];
}

static ViewRegistry* findRegByWv(mbWebView wv)
{
    int i;
    for (i = 0; i < s_regCount; ++i)
    {
        mbWebView raw = (mbWebView)MbGetRawView(s_reg[i].view);
        if (raw == wv) return &s_reg[i];
    }
    return 0;
}

void MbReleaseJsView(MbView* v)
{
    int i;
    for (i = 0; i < s_regCount; ++i)
    {
        if (s_reg[i].view == v)
        {
            RecapBoundMethod* m = s_reg[i].methods;
            while (m) { RecapBoundMethod* nx = m->next; delete m; m = nx; }
            s_reg[i] = s_reg[s_regCount - 1];   /* compact: last fills the hole */
            --s_regCount;
            s_reg[s_regCount].view = 0;
            s_reg[s_regCount].methods = 0;
            s_reg[s_regCount].objName[0] = 0;
            s_reg[s_regCount].hooked = 0;
            s_reg[s_regCount].ctxHooked = 0;
            return;
        }
    }
}

/* ---- JSON parser ----
   Parses {"m":<string>,"a":[values...]} where values are string/number/bool/null.
   String values may contain embedded JSON (escaped quotes + backslashes). */

static const char* skipWs(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

/* Parse a JSON string starting AFTER the opening '"'; advances *pp past closing '"'.
   Handles \" \\ \n \r \t escapes; result in caller-supplied buf[bufSize]. */
static void parseJsonString(const char** pp, char* buf, size_t bufSize)
{
    const char* s = *pp;
    size_t n = 0;
    while (*s && *s != '"' && n < bufSize - 1)
    {
        if (*s == '\\')
        {
            ++s;
            if (!*s) break;
            switch (*s)
            {
            case '"':  buf[n++] = '"';  break;
            case '\\': buf[n++] = '\\'; break;
            case '/':  buf[n++] = '/';  break;
            case 'n':  buf[n++] = '\n'; break;
            case 'r':  buf[n++] = '\r'; break;
            case 't':  buf[n++] = '\t'; break;
            default:   buf[n++] = *s;   break;
            }
            ++s;
        }
        else
        {
            buf[n++] = *s++;
        }
    }
    if (*s == '"') ++s;
    buf[n] = 0;
    *pp = s;
}

/* Skip over a JSON value (string/number/bool/null/array/object) without storing it. */
static void skipJsonValue(const char** pp);

static void skipJsonObject(const char** pp)
{
    const char* p = *pp; /* pointing past '{' */
    while (*p && *p != '}')
    {
        p = skipWs(p);
        if (*p == '"') { ++p; char tmp[512]; parseJsonString(&p, tmp, sizeof(tmp)); }
        p = skipWs(p);
        if (*p == ':') { ++p; p = skipWs(p); skipJsonValue(&p); }
        p = skipWs(p);
        if (*p == ',') ++p;
    }
    if (*p == '}') ++p;
    *pp = p;
}

static void skipJsonArray(const char** pp)
{
    const char* p = *pp; /* pointing past '[' */
    while (*p && *p != ']')
    {
        p = skipWs(p);
        if (*p == ']') break;
        skipJsonValue(&p);
        p = skipWs(p);
        if (*p == ',') ++p;
    }
    if (*p == ']') ++p;
    *pp = p;
}

static void skipJsonValue(const char** pp)
{
    const char* p = skipWs(*pp);
    if (*p == '"') { ++p; char tmp[1024]; parseJsonString(&p, tmp, sizeof(tmp)); }
    else if (*p == '{') { ++p; skipJsonObject(&p); }
    else if (*p == '[') { ++p; skipJsonArray(&p); }
    else if (strncmp(p, "true",  4) == 0) { p += 4; }
    else if (strncmp(p, "false", 5) == 0) { p += 5; }
    else if (strncmp(p, "null",  4) == 0) { p += 4; }
    else { /* number or unknown: scan until delimiter */
        while (*p && *p != ',' && *p != ']' && *p != '}' && *p != ' ') ++p;
    }
    *pp = p;
}

/* Static string pool for JsVal.str — valid for the duration of one sink call.
   8 args x 1024 bytes each. */
#define RECAP_MAX_ARGS      8
#define RECAP_ARG_BUFSIZE   1024

static char s_argBufs[RECAP_MAX_ARGS][RECAP_ARG_BUFSIZE];

/* Parse the request JSON {"m":"<method>","a":[...]} into keyOut + args[].
   Returns argc (number of args parsed). */
static int parseJsCallJson(const char* json, char* keyOut, size_t keyMax,
                           JsVal* args, int maxArgs)
{
    int argc = 0;
    char keyBuf[128];
    const char* p = skipWs(json);
    if (*p != '{') return 0;
    ++p;

    while (*p && *p != '}')
    {
        p = skipWs(p);
        if (*p != '"') { ++p; continue; }
        ++p;
        char fieldName[16];
        parseJsonString(&p, fieldName, sizeof(fieldName));
        p = skipWs(p);
        if (*p != ':') continue;
        ++p;
        p = skipWs(p);

        if (strcmp(fieldName, "m") == 0)
        {
            if (*p == '"')
            {
                ++p;
                parseJsonString(&p, keyBuf, sizeof(keyBuf));
                _snprintf(keyOut, keyMax - 1, "%s", keyBuf);
                keyOut[keyMax - 1] = 0;
            }
            else
            {
                skipJsonValue(&p);
            }
        }
        else if (strcmp(fieldName, "a") == 0)
        {
            if (*p != '[') { skipJsonValue(&p); }
            else
            {
                ++p;
                while (*p && *p != ']' && argc < maxArgs)
                {
                    p = skipWs(p);
                    if (*p == ']') break;

                    if (*p == '"')
                    {
                        ++p;
                        parseJsonString(&p, s_argBufs[argc], RECAP_ARG_BUFSIZE);
                        args[argc].type = kJsString;
                        args[argc].str  = s_argBufs[argc];
                        args[argc].num  = 0;
                        ++argc;
                    }
                    else if (strncmp(p, "true", 4) == 0)
                    {
                        args[argc].type = kJsBoolean;
                        args[argc].num  = 1;
                        args[argc].str  = 0;
                        ++argc; p += 4;
                    }
                    else if (strncmp(p, "false", 5) == 0)
                    {
                        args[argc].type = kJsBoolean;
                        args[argc].num  = 0;
                        args[argc].str  = 0;
                        ++argc; p += 5;
                    }
                    else if (strncmp(p, "null", 4) == 0)
                    {
                        args[argc].type = kJsUndefined;
                        args[argc].num  = 0;
                        args[argc].str  = 0;
                        ++argc; p += 4;
                    }
                    else if (*p == '-' || (*p >= '0' && *p <= '9'))
                    {
                        char* end = 0;
                        double d = strtod(p, &end);
                        args[argc].type = kJsNumber;
                        args[argc].num  = d;
                        args[argc].str  = 0;
                        ++argc;
                        p = (end && end != p) ? end : p + 1;
                    }
                    else if (*p == '{' || *p == '[')
                    {
                        /* Nested object/array: cross the boundary as its raw JSON text.
                           EAWebKit's JavascriptValue has object/array kinds, but every page we
                           serve consumes Client.* args as strings — and the game's handler
                           (stricmp dispatch @0x005148a0) reads strings — so raw JSON is the
                           faithful, simple carrier. Revisit only if a page needs structure. */
                        const char* start = p;
                        skipJsonValue(&p);
                        {
                            size_t len = (size_t)(p - start);
                            if (len > RECAP_ARG_BUFSIZE - 1) len = RECAP_ARG_BUFSIZE - 1;
                            memcpy(s_argBufs[argc], start, len);
                            s_argBufs[argc][len] = 0;
                        }
                        args[argc].type = kJsString;
                        args[argc].str  = s_argBufs[argc];
                        args[argc].num  = 0;
                        ++argc;
                    }
                    else
                    {
                        /* unknown value token — skip it */
                        skipJsonValue(&p);
                        /* produce undefined slot */
                        if (argc < maxArgs)
                        {
                            args[argc].type = kJsUndefined;
                            args[argc].num  = 0;
                            args[argc].str  = 0;
                            ++argc;
                        }
                    }

                    p = skipWs(p);
                    if (*p == ',') ++p;
                }
                /* skip past ']' */
                if (*p == ']') ++p;
            }
        }
        else
        {
            skipJsonValue(&p);
        }

        p = skipWs(p);
        if (*p == ',') ++p;
    }

    return argc;
}

/* ---- Serialize a JsVal ret to JSON for mbResponseQuery ---- */

static void serializeRetVal(const JsVal* ret, char* buf, size_t bufSize)
{
    if (!ret || ret->type == kJsUndefined)
    {
        _snprintf(buf, bufSize - 1, "null");
        buf[bufSize - 1] = 0;
        return;
    }
    if (ret->type == kJsBoolean)
    {
        _snprintf(buf, bufSize - 1, "%s", ret->num ? "true" : "false");
        buf[bufSize - 1] = 0;
        return;
    }
    if (ret->type == kJsNumber)
    {
        _snprintf(buf, bufSize - 1, "%g", ret->num);
        buf[bufSize - 1] = 0;
        return;
    }
    if (ret->type == kJsString && ret->str)
    {
        /* Emit a quoted JSON string with " and \ escaped */
        size_t n = 0;
        const char* s = ret->str;
        buf[n++] = '"';
        while (*s && n < bufSize - 3)
        {
            if (*s == '"' || *s == '\\')
            {
                if (n < bufSize - 3) buf[n++] = '\\';
            }
            buf[n++] = *s++;
        }
        buf[n++] = '"';
        buf[n]   = 0;
        return;
    }
    _snprintf(buf, bufSize - 1, "null");
    buf[bufSize - 1] = 0;
}

/* ---- mbOnJsQuery callback ---- */

static void MB_CALL_TYPE recapQueryCb(mbWebView wv, void* param, mbJsExecState /*es*/,
                                      int64_t queryId, int customMsg, const utf8* request)
{
    (void)param;

    if (!request || !*request)
    {
        mbResponseQuery(wv, queryId, customMsg, "null");
        return;
    }

    ViewRegistry* reg = findRegByWv(wv);
    if (!reg)
    {
        MbLog("recapQueryCb: no reg for wv");
        mbResponseQuery(wv, queryId, customMsg, "null");
        return;
    }

    char key[128]; key[0] = 0;
    JsVal args[RECAP_MAX_ARGS];
    int argc = parseJsCallJson(request, key, sizeof(key), args, RECAP_MAX_ARGS);

    RecapBoundMethod* m = reg->methods;
    while (m)
    {
        if (strcmp(m->key, key) == 0) break;
        m = m->next;
    }

    char retBuf[512];
    if (m && m->sink)
    {
        JsVal ret; ret.type = kJsUndefined; ret.num = 0; ret.str = 0;
        m->sink(m->user, m->key, args, argc, &ret);
        serializeRetVal(&ret, retBuf, sizeof(retBuf));
    }
    else
    {
        _snprintf(retBuf, sizeof(retBuf) - 1, "null");
        retBuf[sizeof(retBuf) - 1] = 0;
    }

    mbResponseQuery(wv, queryId, customMsg, retBuf);
}

/* ---- Public API ---- */

void MbRunJs(MbView* v, const char* script)
{
    if (!v || !script) return;
    mbWebView wv = (mbWebView)MbGetRawView(v);
    if (!wv) return;
    mbRunJs(wv, mbWebFrameGetMainFrame(wv), script, FALSE, 0, 0, 0);
}

int MbRunJsSync(MbView* v, const char* script, JsVal* ret, char* strBuf, unsigned strBufSize)
{
    if (ret) { ret->type = kJsUndefined; ret->num = 0; ret->str = 0; }
    if (!v || !script || !ret) return 0;
    {
        mbWebView wv = (mbWebView)MbGetRawView(v);
        if (!wv) return 0;
        /* mb.h's loader leaves an fn ptr null if the DLL lacks the export — degrade to async */
        if (!mbRunJsSync || !mbGetGlobalExecByFrame || !mbGetJsValueType)
        {
            MbLog("MbRunJsSync: export missing in mb build -> fire-and-forget");
            mbRunJs(wv, mbWebFrameGetMainFrame(wv), script, FALSE, 0, 0, 0);
            return 0;
        }
        {
            mbWebFrameHandle frame = mbWebFrameGetMainFrame(wv);
            mbJsValue        val   = mbRunJsSync(wv, frame, script, FALSE);
            mbJsExecState    es    = mbGetGlobalExecByFrame(wv, frame);
            if (!es) return 0;
            switch (mbGetJsValueType(es, val))
            {
            case kMbJsTypeNumber:
                ret->type = kJsNumber;  ret->num = mbJsToDouble(es, val);                 return 1;
            case kMbJsTypeBool:
                ret->type = kJsBoolean; ret->num = mbJsToBoolean(es, val) ? 1.0 : 0.0;    return 1;
            case kMbJsTypeString:
            {
                const utf8* s = mbJsToString(es, val);
                if (s && strBuf && strBufSize)
                {
                    unsigned n = 0;
                    for (; s[n] && n < strBufSize - 1; ++n) strBuf[n] = s[n];
                    strBuf[n] = 0;
                    ret->type = kJsString;
                    ret->str  = strBuf;
                }
                return 1;
            }
            default:
                return 1;   /* undefined/null/object: cross as undefined */
            }
        }
    }
}

/* Build the whole binding (window.<obj>={}; window.<obj>.<m>=shim; ... for every bound
   method) as ONE script and run it in the given frame. Used to (re)install the bindings into
   each freshly-created JS context — navigation wipes window, so injecting once up front is not
   enough; the launcher's own scripts reference `Client` during load. One mbRunJs, not N. */
static void reinjectBindings(ViewRegistry* reg, mbWebView wv, mbWebFrameHandle frame)
{
    if (!reg || !wv || !reg->objName[0]) return;
    char js[4096];
    int n = _snprintf(js, sizeof(js) - 1, "window.%s=window.%s||{};", reg->objName, reg->objName);
    if (n < 0) n = (int)strlen(js);
    int count = 0;
    RecapBoundMethod* m = reg->methods;
    while (m && n < (int)sizeof(js) - 320)
    {
        const char* method = m->key + strlen(reg->objName);
        if (*method == '.') ++method;
        int w = _snprintf(js + n, sizeof(js) - 1 - n,
            "window.%s.%s=function(){var a=Array.prototype.slice.call(arguments);"
            "window.mbQuery(0,JSON.stringify({m:\"%s\",a:a}),function(){});};",
            reg->objName, method, m->key);
        if (w < 0) break;
        n += w; ++count;
        m = m->next;
    }
    js[n] = 0;
    mbRunJs(wv, frame, js, FALSE, 0, 0, 0);
    (void)count;
}

/* CSS :hover shim. mb132 offscreen drops synthetic mousemove, so the engine never updates the
   :hover pseudo-class. __rh(x,y) hit-tests (elementFromPoint, recursing same-origin iframes —
   the launcher UI is a full-window iframe) and toggles a `rhover` class on the element's ancestor
   chain. The page CSS mirrors each `:hover` rule with a `.rhover` selector, so hover renders.
   Native drives __rh from MbFireMouseMove (throttled). Proven offscreen in tests/recapmb_jshover. */
static const char* RECAP_HOVER_SHIM =
    "(function(){if(window.__rhInit)return;window.__rhInit=1;"
    "function apply(x,y){"
    "var el=document.elementFromPoint(x,y);"
    "if(el!==window.__rhEl){"
    "var o=window.__rhC||[];for(var i=0;i<o.length;i++){if(o[i].classList)o[i].classList.remove('rhover');}"
    "var c=[];for(var n=el;n;n=n.parentElement){if(n.classList){n.classList.add('rhover');c.push(n);}}"
    "window.__rhEl=el;window.__rhC=c;}"
    "var f=document.getElementsByTagName('iframe');"
    "for(var k=0;k<f.length;k++){try{var r=f[k].getBoundingClientRect();f[k].contentWindow.postMessage({__rh:1,x:x-r.left,y:y-r.top},'*');}catch(e){}}"
    "}"
    "window.__rh=apply;"
    "window.addEventListener('message',function(ev){var d=ev.data;if(d&&d.__rh)apply(d.x,d.y);});"
    "})();";

/* Synchronous-getter pre-resolution.
   The in-game account hub (mainwebview.html + iframes) calls several Client getters and uses
   the return VALUE inline at load — e.g. `Client.getLocaleId().split('-')`, `Client.install()`.
   Our JS->native channel (mbQuery) is asynchronous, so the async shim returns undefined and the
   page throws ('reading split' of undefined) before it finishes defining its functions (e.g.
   showScreen) -> blank frame. Fix: these getters are no-arg, session-stable, side-effect-free
   native getters that the real client answers SYNCHRONOUSLY via JavascriptMethodInvoked (the
   sink path). Pre-resolve each here (native->native, synchronous) and overwrite the async shim
   with a function that returns the cached value -> the page sees a real synchronous return.
   Getters WITH args (retrieveLocaleString) or live data (getFriendsAsJSON) are handled later. */
static const char* RECAP_SYNC_GETTERS[] = {
    /* session-stable scalars */
    "getLocaleId", "getWebHost", "getSnApiHost", "getPlayerAccountId", "install",
    /* no-arg JSON data getters the hub eval()s inline at load (friendslist etc).
       Resolved native->native at context creation; the cached snapshot is the value the
       client has at that moment. Live refresh of these is the server-data layer, not here. */
    "getFriendsAsJSON", "getLobbyMembersAsJSON", "getBlockedPlayersAsJSON"
};

static void preinjectSyncGetters(ViewRegistry* reg, mbWebView wv, mbWebFrameHandle frame)
{
    if (!reg || !reg->objName[0]) return;
    int g;
    for (g = 0; g < (int)(sizeof(RECAP_SYNC_GETTERS) / sizeof(RECAP_SYNC_GETTERS[0])); ++g)
    {
        const char* getter = RECAP_SYNC_GETTERS[g];
        char key[128];
        _snprintf(key, sizeof(key) - 1, "%s.%s", reg->objName, getter); key[sizeof(key) - 1] = 0;

        RecapBoundMethod* m = reg->methods;
        while (m) { if (strcmp(m->key, key) == 0) break; m = m->next; }
        if (!m || !m->sink) continue;   /* not bound by this view */

        JsVal ret; ret.type = kJsUndefined; ret.num = 0; ret.str = 0;
        m->sink(m->user, m->key, 0, 0, &ret);   /* synchronous native getter via the sink */

        char val[512];
        serializeRetVal(&ret, val, sizeof(val));

        char js[768];
        _snprintf(js, sizeof(js) - 1,
            "window.%s.%s=function(){return %s;};", reg->objName, getter, val);
        js[sizeof(js) - 1] = 0;
        mbRunJs(wv, frame, js, FALSE, 0, 0, 0);
    }
}

/* Fires when a frame's JS context is created — BEFORE the page's scripts run — so the bound
   object exists when the launcher references it. */
static void MB_CALL_TYPE scriptCtxThunk(mbWebView wv, void* /*param*/, mbWebFrameHandle frameId,
                                        void* /*context*/, int /*extGroup*/, int /*worldId*/)
{
    ViewRegistry* reg = findRegByWv(wv);
    if (reg)
    {
        reinjectBindings(reg, wv, frameId);
        preinjectSyncGetters(reg, wv, frameId);   /* overwrite async shims for sync getters */
    }
    mbRunJs(wv, frameId, RECAP_HOVER_SHIM, FALSE, 0, 0, 0);   /* inject __rh into every frame */
}

void MbCreateJsObject(MbView* v, const char* obj)
{
    if (!v || !obj) return;
    ViewRegistry* reg = getOrCreateReg(v);
    if (reg) { _snprintf(reg->objName, sizeof(reg->objName) - 1, "%s", obj); reg->objName[sizeof(reg->objName) - 1] = 0; }

    mbWebView wv = (mbWebView)MbGetRawView(v);

    /* eager create in the current context (harmless if navigation wipes it) */
    char js[128];
    _snprintf(js, sizeof(js) - 1, "window.%s=window.%s||{};", obj, obj); js[sizeof(js) - 1] = 0;
    MbRunJs(v, js);

    if (reg && !reg->hooked)    { if (wv) mbOnJsQuery(wv, recapQueryCb, 0);            reg->hooked = 1; }
    if (reg && !reg->ctxHooked) { if (wv) mbOnDidCreateScriptContext(wv, scriptCtxThunk, 0); reg->ctxHooked = 1; }
}

void MbBindJsMethod(MbView* v, const char* obj, const char* method, MbJsSink sink, void* user)
{
    if (!v || !obj || !method || !sink) return;

    ViewRegistry* reg = getOrCreateReg(v);
    if (!reg) return;

    char key[128];
    _snprintf(key, sizeof(key) - 1, "%s.%s", obj, method);
    key[sizeof(key) - 1] = 0;

    /* Update existing or insert new binding */
    RecapBoundMethod* m = reg->methods;
    while (m)
    {
        if (strcmp(m->key, key) == 0) { m->sink = sink; m->user = user; break; }
        m = m->next;
    }
    if (!m)
    {
        m = new RecapBoundMethod();
        { size_t j; for (j = 0; key[j] && j < sizeof(m->key) - 1; ++j) m->key[j] = key[j]; m->key[j] = 0; }
        m->sink = sink;
        m->user = user;
        m->next = reg->methods;
        reg->methods = m;
    }

    /* Ensure obj name + both hooks are set (robust if MbCreateJsObject was skipped/reordered) */
    if (!reg->objName[0]) { _snprintf(reg->objName, sizeof(reg->objName) - 1, "%s", obj); reg->objName[sizeof(reg->objName) - 1] = 0; }
    {
        mbWebView wv = (mbWebView)MbGetRawView(v);
        if (!reg->hooked)    { if (wv) mbOnJsQuery(wv, recapQueryCb, 0);                 reg->hooked = 1; }
        if (!reg->ctxHooked) { if (wv) mbOnDidCreateScriptContext(wv, scriptCtxThunk, 0); reg->ctxHooked = 1; }
    }

    /* Inject JS shim.
       window.<obj>.<method> = function() {
           var a = Array.prototype.slice.call(arguments);
           window.mbQuery(0, JSON.stringify({m:"<key>", a:a}), function(id,cm,resp){});
       };
       The cb arg is a no-op placeholder; callers that need the return value must
       supply their own callback via direct window.mbQuery usage. */
    char js[768];
    _snprintf(js, sizeof(js) - 1,
        "window.%s.%s=function(){"
            "var a=Array.prototype.slice.call(arguments);"
            "window.mbQuery(0,JSON.stringify({m:\"%s\",a:a}),function(id,cm,resp){});"
        "};",
        obj, method, key);
    js[sizeof(js) - 1] = 0;
    MbRunJs(v, js);
}

} // namespace recap
