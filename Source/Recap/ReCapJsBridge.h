#ifndef RECAP_JSBRIDGE_H
#define RECAP_JSBRIDGE_H

/* JS bridge: wires window.<obj>.<method> calls from MiniBlink into the EAWebKit
   ViewNotification::JavascriptMethodInvoked callback.

   This header is mb-free and EAWebKit-type-free — the boundary type is JsVal. */

namespace recap {

/* Forward-declared here; defined in ReCapMiniBlink.h / ReCapMiniBlink.cpp. */
struct MbView;

/* Neutral JS value crossing the recapmb <-> View boundary (no mb or EAWebKit types). */
enum JsValType { kJsUndefined = 0, kJsNumber, kJsString, kJsBoolean };
struct JsVal {
    JsValType   type;
    double      num;     /* number; also 0/1 for boolean */
    const char* str;     /* UTF-8; valid only during the sink call */
};

/* The View implements this. Called (UI thread) when a bound JS method fires.
   args[0..argc-1] are inputs; fill *ret for the JS return (default undefined). */
typedef void (*MbJsSink)(void* user, const char* method, const JsVal* args, int argc, JsVal* ret);

/* Create window.<objectName> = {} (idempotent) for the view. */
void MbCreateJsObject(MbView*, const char* objectName);

/* Bind window.<objectName>.<method> -> native; the view's sink(user,...) handles it.
   Uses mb132's injected window.mbQuery(customMsg, request, cb): the shim routes
   window.<objectName>.<method>(args) to window.mbQuery(0, JSON.stringify({m,a}), cb).
   Native dispatches in recapQueryCb and replies via mbResponseQuery (so the JS cb
   gets the return ASYNCHRONOUSLY — synchronous JS return is not available in mb132). */
void MbBindJsMethod(MbView*, const char* objectName, const char* method, MbJsSink sink, void* user);

/* Release the view's bridge slot + its bound-method list. MUST be called when the MbView is
   destroyed — otherwise the fixed registry leaks slots (and a recycled MbView* address would
   alias a stale slot). Called from MbDestroy. */
void MbReleaseJsView(MbView*);

/* Evaluate script in the main frame (fire-and-forget). */
void MbRunJs(MbView*, const char* script);

/* Evaluate script in the main frame and return its result SYNCHRONOUSLY (mbRunJsSync).
   This is native->JS sync (View::EvaluateJavaScript). JS->native (window.<obj>.<m>()) stays
   async — mbQuery has no synchronous return channel in mb132. String results are copied
   into strBuf and ret->str points at it. Returns 0 (ret stays undefined) when sync eval is
   unavailable in the loaded mb build (falls back to fire-and-forget). */
int  MbRunJsSync(MbView*, const char* script, JsVal* ret, char* strBuf, unsigned strBufSize);

} // namespace recap

#endif /* RECAP_JSBRIDGE_H */
