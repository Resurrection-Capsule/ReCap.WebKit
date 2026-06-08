#ifndef RECAP_REDIRECT_H
#define RECAP_REDIRECT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecapRedirectConfig
{
    unsigned int uHostAddr;   /* assembled (a<<24)|(b<<16)|(c<<8)|d, e.g. 0x7F000001 = 127.0.0.1 */
    unsigned int uHttpPort;   /* HTTP target port (default 8033) */
    int          bSslBypass;  /* nonzero = skip cert/host verification (default 1) */
} RecapRedirectConfig;

/* Pure parser: applies key=value lines from pText onto pCfg (which must already hold
   defaults). No I/O, no Win32 — unit-testable. Unknown keys / malformed lines ignored. */
void RecapRedirectParse(const char *pText, RecapRedirectConfig *pCfg);

/* Pure parser for the "-recapServer=<host[:port]>" launch arg (key matched case-insensitively,
   '-' or '/' prefix). Copies the host (IPv4 or hostname) into hostOut and, when a numeric ":port"
   suffix is present, sets *pPort and *pHasPort. Returns 1 if found, host non-empty. No I/O. */
int RecapRedirectParseArg(const char *pCmdLine, char *hostOut, unsigned int hostCap,
                          unsigned int *pPort, int *pHasPort);

/* Lazy-resolves the active config on first call; cached thereafter. Precedence:
     1. "-recapServer=<host[:port]>" launch arg (the Hub/launcher path — zero file I/O),
     2. %APPDATA%\ReCap\recap.cfg (user-writable),
     3. recap.cfg next to the DLL (DarksporeBin — shipped default),
     4. in-memory defaults (127.0.0.1 / 8033 / bypass=1), also written as a template to (2).
   Never fails: on any error returns the defaults. host may be IPv4 or a hostname (resolved). */
const RecapRedirectConfig *RecapRedirectGet(void);

#ifdef __cplusplus
}
#endif
#endif /* RECAP_REDIRECT_H */
