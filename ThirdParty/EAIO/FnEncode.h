// ReCap.WebKit clean-room minimal <EAIO/FnEncode.h>.
// EAWebKitEASTLHelpers.cpp uses only these two strlcpy-style UTF converters. Implemented over the
// Win32 UTF-8 codec so we don't vendor the full EAIO package. Semantics match EAIO: returns the
// length (in dest units, excluding the null terminator) of the COMPLETE conversion; writes up to
// nDestCapacity-1 units + a null when pDest/capacity are non-zero (NULL/0 => just measure).
#pragma once
#include <EABase/eabase.h>
#include <windows.h>

namespace EA { namespace IO {

inline size_t StrlcpyUTF16ToUTF8(char8_t* pDest, size_t nDestCapacity,
                                 const char16_t* pSrc, size_t nSrcLength = (size_t)~0)
{
    int srcLen = (nSrcLength == (size_t)~0) ? -1 : (int)nSrcLength;
    int need = ::WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)pSrc, srcLen, NULL, 0, NULL, NULL);
    if (need < 0) need = 0;
    if (pDest && nDestCapacity > 0)
    {
        int cap = (int)nDestCapacity - 1;            // reserve the null
        int n = (cap < need) ? cap : need;
        int wrote = (n > 0) ? ::WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)pSrc, srcLen,
                                                    (LPSTR)pDest, n, NULL, NULL) : 0;
        if (wrote < 0) wrote = 0;
        pDest[(size_t)wrote < nDestCapacity ? wrote : nDestCapacity - 1] = 0;
    }
    return (size_t)need;
}

inline size_t StrlcpyUTF8ToUTF16(char16_t* pDest, size_t nDestCapacity,
                                 const char8_t* pSrc, size_t nSrcLength = (size_t)~0)
{
    int srcLen = (nSrcLength == (size_t)~0) ? -1 : (int)nSrcLength;
    int need = ::MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)pSrc, srcLen, NULL, 0);
    if (need < 0) need = 0;
    if (pDest && nDestCapacity > 0)
    {
        int cap = (int)nDestCapacity - 1;
        int n = (cap < need) ? cap : need;
        int wrote = (n > 0) ? ::MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)pSrc, srcLen,
                                                    (LPWSTR)pDest, n) : 0;
        if (wrote < 0) wrote = 0;
        pDest[(size_t)wrote < nDestCapacity ? wrote : nDestCapacity - 1] = 0;
    }
    return (size_t)need;
}

}} // namespace EA::IO
