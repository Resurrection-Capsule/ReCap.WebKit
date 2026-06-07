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
        }
        if (*pNL == 0) break;
        pLine = pNL + 1;
    }
}

/* ---- Win32 loader (excluded from the gcc parser test via RECAP_NO_WIN32) -- */
#ifndef RECAP_NO_WIN32
#include <windows.h>
#include <stdio.h>

static RecapRedirectConfig g_cfg;
static int g_loaded = 0;

const RecapRedirectConfig *RecapRedirectGet(void)
{
    char path[MAX_PATH]; HMODULE hMod = NULL; DWORD n; FILE *fp;

    if (g_loaded) return &g_cfg;

    g_cfg.uHostAddr = 0x7F000001u;   /* 127.0.0.1 */
    g_cfg.uHttpPort = 8033u;
    g_cfg.bSslBypass = 1;
    g_loaded = 1;                    /* set early: never retry, never crash-loop */

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&g_cfg, &hMod))
        return &g_cfg;
    n = GetModuleFileNameA(hMod, path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) return &g_cfg;
    while (n > 0 && path[n-1] != '\\' && path[n-1] != '/') --n;
    path[n] = 0;
    if (n + 9 >= sizeof(path)) return &g_cfg; /* "recap.cfg" = 9 chars */
    { const char *f = "recap.cfg"; size_t i; for (i = 0; f[i]; ++i) path[n+i] = f[i]; path[n+i] = 0; }

    fp = fopen(path, "rb");
    if (fp)
    {
        char buf[2048]; size_t got = fread(buf, 1, sizeof(buf) - 1, fp); buf[got] = 0;
        fclose(fp);
        RecapRedirectParse(buf, &g_cfg);
    }
    return &g_cfg;
}
#endif /* RECAP_NO_WIN32 */
