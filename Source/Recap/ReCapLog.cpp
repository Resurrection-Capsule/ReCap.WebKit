#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "ReCapLog.h"

namespace recap {

#define RECAP_WEBKIT_DATA_SUBDIR "ReCapWebKit"

/* Resolve the directory of THIS DLL image (module-relative; survives the game's DLL reload,
   per-install not per-cwd). Writes "<dir>\" into out. Returns length, 0 on failure. */
static int moduleDirA(char* out, int cap)
{
    HMODULE h = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&moduleDirA, &h);
    DWORD n = GetModuleFileNameA(h, out, (DWORD)cap);
    if (!n || n >= (DWORD)cap) return 0;
    while (n > 0 && out[n-1] != '\\' && out[n-1] != '/') --n;
    out[n] = 0;
    return (int)n;
}

const char* MbDataDir()
{
    static char s_dir[MAX_PATH];
    static int  s_done = 0;
    if (!s_done)
    {
        s_done = 1;
        int n = moduleDirA(s_dir, MAX_PATH);
        if (n && n + (int)sizeof(RECAP_WEBKIT_DATA_SUBDIR) + 2 < MAX_PATH)
        {
            _snprintf(s_dir + n, MAX_PATH - 1 - n, "%s\\", RECAP_WEBKIT_DATA_SUBDIR);
            s_dir[MAX_PATH - 1] = 0;
            /* create "<dir>\ReCapWebKit" (without trailing slash); ALREADY_EXISTS is fine. */
            char mk[MAX_PATH]; _snprintf(mk, MAX_PATH - 1, "%s%s", s_dir, ""); mk[MAX_PATH-1]=0;
            size_t L = strlen(mk); if (L) mk[L-1] = 0;
            CreateDirectoryA(mk, 0);
        }
        else
        {
            s_dir[0] = 0;
        }
    }
    return s_dir;
}

int MbDataDirW(wchar_t* out, int cap)
{
    const char* a = MbDataDir();
    if (!a[0]) { if (cap > 0) out[0] = 0; return 0; }
    int len = MultiByteToWideChar(CP_ACP, 0, a, -1, out, cap);
    return len > 0 ? len - 1 : 0;   /* exclude the NUL from the reported length */
}

/* Persistent append handle: open once, flush per line (crash-safe) instead of
   fopen/fclose per line — the latter is a real drain on the input/paint hot path. */
static FILE* s_fp = 0;
static int   s_tried = 0;

static void mbLogLine(const char* s)
{
    if (!s_fp)
    {
        if (s_tried) return;            /* open failed once; don't retry every line */
        s_tried = 1;
        const char* dir = MbDataDir();
        if (!dir[0]) return;
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH - 1, "%sReCap.WebKit.log", dir);
        path[MAX_PATH - 1] = 0;
        s_fp = fopen(path, "a");
        if (!s_fp) return;
    }
    fputs(s, s_fp); fputc('\n', s_fp); fflush(s_fp);
}

void MbLog(const char* s) { mbLogLine(s ? s : "(null)"); }

void MbLogf(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    mbLogLine(buf);
}

void MbDumpBGRA(const void* bgra, int stride, int w, int h, const char* name)
{
    char path[MAX_PATH]; HMODULE hh = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&MbDumpBGRA, &hh);
    DWORD n = GetModuleFileNameA(hh, path, MAX_PATH);
    while (n > 0 && path[n-1] != '\\' && path[n-1] != '/') --n;
    { size_t i; for (i = 0; name[i] && n + i + 1 < MAX_PATH; ++i) path[n+i] = name[i]; path[n+i] = 0; }
    FILE* fp = fopen(path, "wb");
    if (!fp) return;
    int rowbytes = w * 4, imgsize = rowbytes * h;
    unsigned char fhh[14]; memset(fhh, 0, 14); fhh[0] = 'B'; fhh[1] = 'M';
    unsigned int fsize = 14 + 40 + (unsigned)imgsize, off = 14 + 40;
    fhh[2]=fsize&0xff; fhh[3]=(fsize>>8)&0xff; fhh[4]=(fsize>>16)&0xff; fhh[5]=(fsize>>24)&0xff;
    fhh[10]=off&0xff; fhh[11]=(off>>8)&0xff;
    unsigned char ih[40]; memset(ih, 0, 40); ih[0]=40;
    int negh = -h;
    ih[4]=w&0xff; ih[5]=(w>>8)&0xff; ih[6]=(w>>16)&0xff; ih[7]=(w>>24)&0xff;
    ih[8]=negh&0xff; ih[9]=(negh>>8)&0xff; ih[10]=(negh>>16)&0xff; ih[11]=(negh>>24)&0xff;
    ih[12]=1; ih[14]=32;
    fwrite(fhh,1,14,fp); fwrite(ih,1,40,fp);
    for (int row = 0; row < h; ++row) fwrite((const unsigned char*)bgra + (size_t)row * stride, 1, rowbytes, fp);
    fclose(fp);
    MbLogf("MbDumpBGRA: wrote %s (%dx%d)", name, w, h);
}

} // namespace recap
