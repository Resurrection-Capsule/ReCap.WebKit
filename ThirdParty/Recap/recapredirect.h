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

/* Lazy-loads recap.cfg from the DLL's own folder on first call; cached thereafter.
   Never fails: on any error returns defaults (127.0.0.1 / 8033 / bypass=1). */
const RecapRedirectConfig *RecapRedirectGet(void);

#ifdef __cplusplus
}
#endif
#endif /* RECAP_REDIRECT_H */
