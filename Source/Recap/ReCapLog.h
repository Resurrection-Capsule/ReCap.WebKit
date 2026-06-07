#ifndef RECAP_LOG_H
#define RECAP_LOG_H

/* ReCap.WebKit file logger — crash-safe (persistent append handle, flush per line),
   writes to ReCap.WebKit.log inside the data folder next to EAWebKit.dll. */

namespace recap {

/* Per-install data folder: "<EAWebKit.dll dir>\ReCapWebKit\" — created on first call.
   Holds ReCap.WebKit.log + cookies + LocalStorage + Cache, keeping the game bin tidy.
   ASCII path ends with a trailing backslash. Returns "" only if the module dir is unresolved. */
const char* MbDataDir();
/* Wide variant of MbDataDir for the mb132 path APIs (cookie jar / localstorage / disk cache).
   Writes into out (trailing backslash); returns the length, or 0 on failure. */
int MbDataDirW(wchar_t* out, int cap);

/* Append a line to ReCap.WebKit.log (in MbDataDir). Crash-safe (flush per line). */
void MbLog(const char* msg);
void MbLogf(const char* fmt, ...);

/* One-shot diagnostic: write a 32bpp top-down BMP of a BGRA buffer. */
void MbDumpBGRA(const void* bgra, int stride, int w, int h, const char* name);

} // namespace recap

#endif /* RECAP_LOG_H */
