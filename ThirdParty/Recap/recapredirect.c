#include "recapredirect.h"
#include <stddef.h>

/* ---- pure parser (no I/O, no Win32) -------------------------------------- */

static int recap_streq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) { if (a[i] != b[i]) return 0; }
    return 1;
}

/* parse "a.b.c.d" -> assembled (a<<24)|(b<<16)|(c<<8)|d. Returns 1 on success. */
static int recap_parse_ipv4(const char *s, unsigned int *pOut)
{
    unsigned int parts[4]; int iPart = 0; unsigned int acc = 0; int seen = 0; const char *p;
    for (p = s; ; ++p)
    {
        if (*p >= '0' && *p <= '9') { acc = acc*10u + (unsigned int)(*p - '0'); seen = 1; if (acc > 255u) return 0; }
        else if (*p == '.' || *p == 0)
        {
            if (!seen || iPart >= 4) return 0;
            parts[iPart++] = acc; acc = 0; seen = 0;
            if (*p == 0) break;
        }
        else return 0;
    }
    if (iPart != 4) return 0;
    *pOut = (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
    return 1;
}

static unsigned int recap_parse_uint(const char *s, int *pOk)
{
    unsigned int v = 0; int seen = 0; const char *p;
    for (p = s; *p; ++p)
    {
        if (*p >= '0' && *p <= '9') { v = v*10u + (unsigned int)(*p - '0'); seen = 1; }
        else break;
    }
    *pOk = seen; return v;
}

/* copy one logical line [pBeg,pEnd) into buf, trimming leading/trailing spaces+tabs */
static void recap_trim_copy(const char *pBeg, const char *pEnd, char *buf, size_t cap)
{
    size_t n;
    while (pBeg < pEnd && (*pBeg == ' ' || *pBeg == '\t')) ++pBeg;
    while (pEnd > pBeg && (pEnd[-1] == ' ' || pEnd[-1] == '\t' || pEnd[-1] == '\r')) --pEnd;
    n = (size_t)(pEnd - pBeg);
    if (n >= cap) n = cap - 1;
    if (n) { size_t i; for (i = 0; i < n; ++i) buf[i] = pBeg[i]; }
    buf[n] = 0;
}

void RecapRedirectParse(const char *pText, RecapRedirectConfig *pCfg)
{
    const char *pLine = pText;
    if (!pText || !pCfg) return;
    while (*pLine)
    {
        const char *pNL = pLine; const char *pEq;
        char key[64]; char val[128];
        while (*pNL && *pNL != '\n') ++pNL;          /* end of this line */
        for (pEq = pLine; pEq < pNL && *pEq != '='; ++pEq) ;
        if (pEq < pNL && pLine[0] != '#')
        {
            recap_trim_copy(pLine, pEq, key, sizeof(key));
            recap_trim_copy(pEq + 1, pNL, val, sizeof(val));
            if (recap_streq_n(key, "host", 5))
            {
                unsigned int a; if (recap_parse_ipv4(val, &a)) pCfg->uHostAddr = a;
            }
            else if (recap_streq_n(key, "http_port", 10))
            {
                int ok; unsigned int v = recap_parse_uint(val, &ok); if (ok) pCfg->uHttpPort = v;
            }
            else if (recap_streq_n(key, "ssl_bypass", 11))
            {
                int ok; unsigned int v = recap_parse_uint(val, &ok); if (ok) pCfg->bSslBypass = (v != 0);
            }
            else if (recap_streq_n(key, "console_port", 13))
            {
                int ok; unsigned int v = recap_parse_uint(val, &ok); if (ok) pCfg->uConsolePort = v;
            }
        }
        if (*pNL == 0) break;
        pLine = pNL + 1;
    }
}

/* Find "-recapServer=VALUE" (or "/recapServer=", key matched case-insensitively) in a command
   line and split VALUE into host + optional ":port". VALUE runs to the first whitespace, or to the
   closing quote when it opens with one. Writes the host into hostOut and, when a numeric ":port"
   suffix is present, sets *pPort and *pHasPort. Returns 1 if the option was found, host non-empty.
   Pure (no I/O) so it stays under the parser test; the Win32 loader resolves the host to an addr. */
int RecapRedirectParseArg(const char *pCmdLine, char *hostOut, unsigned int hostCap,
                          unsigned int *pPort, int *pHasPort)
{
    static const char kKey[] = "recapserver=";
    const char *p;
    if (!pCmdLine || !hostOut || hostCap == 0 || !pPort || !pHasPort) return 0;
    *pPort = 0; *pHasPort = 0; hostOut[0] = 0;
    for (p = pCmdLine; *p; ++p)
    {
        if (*p != '-' && *p != '/') continue;
        {
            const char *k = p + 1; size_t i;
            for (i = 0; kKey[i]; ++i)
            {
                char c = k[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (c != kKey[i]) break;
            }
            if (kKey[i] != 0) continue;                 /* key didn't match in full */
            {
                const char *v = k + i; char tmp[160]; size_t ti = 0; char quote = 0; size_t ci;
                if (*v == '"') { quote = '"'; ++v; }
                while (*v && ti < sizeof(tmp) - 1)
                {
                    char c = *v;
                    if (quote) { if (c == quote) break; }
                    else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
                    tmp[ti++] = c; ++v;
                }
                tmp[ti] = 0;
                for (ci = 0; ci < ti && tmp[ci] != ':'; ++ci) ;
                if (ci < ti)
                {
                    int ok; unsigned int pr;
                    tmp[ci] = 0;
                    pr = recap_parse_uint(tmp + ci + 1, &ok);
                    if (ok) { *pPort = pr; *pHasPort = 1; }
                }
                if (tmp[0] == 0) return 0;
                { size_t hi; for (hi = 0; tmp[hi] && hi < hostCap - 1; ++hi) hostOut[hi] = tmp[hi]; hostOut[hi] = 0; }
                return 1;
            }
        }
    }
    return 0;
}

/* ---- Win32 loader (excluded from the gcc parser test via RECAP_NO_WIN32) -- */
#ifndef RECAP_NO_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RecapRedirectConfig g_cfg;
static int g_loaded = 0;

/* host may be dotted IPv4 or a hostname; resolve to the host-order addr the ws2_32 hooks expect
   (they htonl() it). getaddrinfo gives network order, so we ntohl back into our convention. */
static int recap_resolve_host(const char *host, unsigned int *pOut)
{
    struct addrinfo hints; struct addrinfo *res = 0; WSADATA wsa; int started; int ok = 0;
    if (recap_parse_ipv4(host, pOut)) return 1;
    started = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, 0, &hints, &res) == 0 && res)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        *pOut = ntohl(sin->sin_addr.s_addr);
        ok = 1;
        freeaddrinfo(res);
    }
    if (started) WSACleanup();
    return ok;
}

static int recap_try_file(const char *path, RecapRedirectConfig *pCfg)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    { char buf[2048]; size_t got = fread(buf, 1, sizeof(buf) - 1, fp); buf[got] = 0; fclose(fp);
      RecapRedirectParse(buf, pCfg); }
    return 1;
}

/* "recap.cfg" in the DLL's own folder (DarksporeBin) — the shipped/default location. */
static int recap_dll_dir_cfg(char *path, size_t cap)
{
    HMODULE hMod = NULL; DWORD n;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&recap_dll_dir_cfg, &hMod))
        return 0;
    n = GetModuleFileNameA(hMod, path, (DWORD)cap);
    if (n == 0 || n >= cap) return 0;
    while (n > 0 && path[n-1] != '\\' && path[n-1] != '/') --n;
    path[n] = 0;
    if (n + 9 >= cap) return 0; /* "recap.cfg" = 9 chars */
    { const char *f = "recap.cfg"; size_t i; for (i = 0; f[i]; ++i) path[n+i] = f[i]; path[n+i] = 0; }
    return 1;
}

/* "%APPDATA%\ReCap" — always user-writable, even under a Program Files install. */
static int recap_appdata_dir(char *out, size_t cap)
{
    const char *ad = getenv("APPDATA"); size_t l = 0, i;
    if (!ad) return 0;
    while (ad[l]) ++l;
    if (l + 7 >= cap) return 0;            /* "\ReCap" + nul */
    for (i = 0; i < l; ++i) out[i] = ad[i];
    { const char *s = "\\ReCap"; size_t j; for (j = 0; s[j]; ++j) out[l+j] = s[j]; out[l+j] = 0; }
    return 1;
}

static int recap_appdata_cfg(char *out, size_t cap)
{
    size_t l = 0;
    if (!recap_appdata_dir(out, cap)) return 0;
    while (out[l]) ++l;
    if (l + 11 >= cap) return 0;           /* "\recap.cfg" + nul */
    { const char *s = "\\recap.cfg"; size_t j; for (j = 0; s[j]; ++j) out[l+j] = s[j]; out[l+j] = 0; }
    return 1;
}

const RecapRedirectConfig *RecapRedirectGet(void)
{
    char path[MAX_PATH]; char host[160]; unsigned int argPort = 0; int hasPort = 0;

    if (g_loaded) return &g_cfg;

    g_cfg.uHostAddr = 0x7F000001u;   /* 127.0.0.1 */
    g_cfg.uHttpPort = 8033u;
    g_cfg.bSslBypass = 1;
    g_cfg.uConsolePort = 0u;         /* dev telnet console off by default */
    g_loaded = 1;                    /* set early: never retry, never crash-loop */

    /* (1) primary path: the server picked in the Hub/launcher arrives as a launch arg, so no file
       is read or written at all — kills the write-permission problem at the root. */
    if (RecapRedirectParseArg(GetCommandLineA(), host, sizeof(host), &argPort, &hasPort)
        && recap_resolve_host(host, &g_cfg.uHostAddr))
    {
        if (hasPort) g_cfg.uHttpPort = argPort;
        return &g_cfg;
    }

    /* (2) user-writable config in %APPDATA%\ReCap (manual / no-Hub runs). */
    if (recap_appdata_cfg(path, sizeof(path)) && recap_try_file(path, &g_cfg))
        return &g_cfg;

    /* (3) shipped config next to the DLL (DarksporeBin). */
    if (recap_dll_dir_cfg(path, sizeof(path)) && recap_try_file(path, &g_cfg))
        return &g_cfg;

    /* (4) first run: drop an editable template in %APPDATA%\ReCap — always writable, unlike a
       Program Files install. Best-effort; we already hold working defaults regardless. */
    if (recap_appdata_dir(path, sizeof(path)))
    {
        CreateDirectoryA(path, NULL);
        if (recap_appdata_cfg(path, sizeof(path)))
        {
            FILE *out = fopen(path, "wb");
            if (out)
            {
                static const char kDefault[] =
                    "# ReCap.WebKit redirect — point Darkspore's web layer at a ReCap server.\r\n"
                    "# Auto-generated with the defaults on first run; edit as needed.\r\n"
                    "# The Hub overrides all of this by passing -recapServer=<host[:port]> at launch.\r\n"
                    "#\r\n"
                    "# host:       IPv4 or hostname of the ReCap server (127.0.0.1 = local).\r\n"
                    "# http_port:  ReCap REST port (default 8033).\r\n"
                    "# ssl_bypass: 1 = accept the server's self-signed certificate. Default 1.\r\n"
                    "# console_port: dev telnet command console listen port (0/absent = off). Note the\r\n"
                    "#   Hub's -recapServer launch skips this file, so it applies to manual launches.\r\n"
                    "host=127.0.0.1\r\n"
                    "http_port=8033\r\n"
                    "ssl_bypass=1\r\n"
                    "# console_port=9200\r\n";
                fwrite(kDefault, 1, sizeof(kDefault) - 1, out);
                fclose(out);
            }
        }
    }
    return &g_cfg;
}
#endif /* RECAP_NO_WIN32 */
