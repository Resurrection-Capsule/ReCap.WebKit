#ifndef RECAP_HTTP_H
#define RECAP_HTTP_H

/* Minimal blocking HTTP GET helper (WinINet) for talking to the ReCap server from native UI. */

namespace recap {

/* Fetch url; copies up to respCap-1 bytes of the response body into resp (NUL-terminated).
   Returns the HTTP status code, or -1 on transport failure. */
int HttpGet(const char* url, char* resp, unsigned respCap);

/* GET the ReCap REST API: prepends "http://<host>:<port>/recap/api?" (host/port from recap.cfg via
   RecapRedirectGet) to the given query, so callers stay decoupled from server addressing. */
int HttpGetRecapApi(const char* query, char* resp, unsigned respCap);

} // namespace recap

#endif /* RECAP_HTTP_H */
