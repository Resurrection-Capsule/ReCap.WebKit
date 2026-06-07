#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include "ReCapHttp.h"
#include "ReCapLog.h"
#include "recapredirect.h"   /* RecapRedirectGet -> host/port from recap.cfg */

#pragma comment(lib, "wininet.lib")

namespace recap {

int HttpGet(const char* url, char* resp, unsigned respCap)
{
    if (resp && respCap) resp[0] = 0;

    HINTERNET hNet = InternetOpenA("ReCapNativeUI", INTERNET_OPEN_TYPE_PRECONFIG, 0, 0, 0);
    if (!hNet) { MbLog("http: InternetOpen failed"); return -1; }

    HINTERNET hUrl = InternetOpenUrlA(hNet, url, 0, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE, 0);
    if (!hUrl) { MbLogf("http: InternetOpenUrl failed (%lu) %s", GetLastError(), url); InternetCloseHandle(hNet); return -1; }

    DWORD status = 0, len = sizeof(status);
    HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, 0);

    unsigned total = 0;
    if (resp && respCap > 1)
    {
        DWORD got = 0;
        while (total < respCap - 1 &&
               InternetReadFile(hUrl, resp + total, respCap - 1 - total, &got) && got > 0)
            total += got;
        resp[total] = 0;
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);
    MbLogf("http: GET %s -> status=%lu bytes=%u", url, status, total);
    return (int)status;
}

int HttpGetRecapApi(const char* query, char* resp, unsigned respCap)
{
    const RecapRedirectConfig* c = RecapRedirectGet();   /* never null; defaults 127.0.0.1/8033 */
    unsigned a = c->uHostAddr;
    char url[1200];
    _snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%u/recap/api?%s",
              (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF,
              c->uHttpPort, query ? query : "");
    url[sizeof(url) - 1] = 0;
    return HttpGet(url, resp, respCap);
}

} // namespace recap
