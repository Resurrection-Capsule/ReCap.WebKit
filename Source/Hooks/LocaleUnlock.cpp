#include "LocaleUnlock.h"

#include <windows.h>
#include <detours.h>
#include <string>
#include <cwctype>

/* exe offsets from image base 0x00400000 (retail 5.3.0.127) */
#define RECAP_LOCALERESOLVER_OFFSET 0x003FAC90u  /* FUN_007fac90: locale resolver, __fastcall(ECX) */
#define RECAP_REGISTERLOCALE_OFFSET 0x006E9420u  /* FUN_00ae9420: registrar, __cdecl(langDef, regionDef) */

typedef char (__fastcall *LocaleResolverFn)(void* ecx, void* edx);
typedef void (__cdecl   *RegisterLocaleFn)(const wchar_t* langDef, const wchar_t* regionDef);

static LocaleResolverFn Real_LocaleResolver = nullptr;
static RegisterLocaleFn s_registerLocale    = nullptr;
static bool             s_localesRegistered = false;

/* en/us template: everything AFTER field[0] ("en"/"us"), copied from FUN_007fac90's registration.
   A cloned code reuses these tails with its own 2-char field[0]. */
static const wchar_t* EN_LANG_TAIL =
    L"^English^English"
    L"^Monday,Tuesday,Wednesday,Thursday,Friday,Saturday,Sunday"
    L"^Mon.,Tue.,Wed.,Thur.,Fri.,Sat.,Sun."
    L"^January,February,March,April,May,June,July,August,September,October,November,December"
    L"^Jan.,Feb.,Mar.,Apr.,May,June,July,Aug.,Sept.,Oct.,Nov.,Dec.^0^%f %l^";
static const wchar_t* US_REGION_TAIL =
    L"^United States^United States^R^E^AM,PM^%h:%<02M:%<02S %<A^%#02m/%#02d/%02y"
    L"^%>+m %<#d, %<y^$^,^.^%$%W%?p%?02F^%$-%W%?p%?02F^USA^840";

/* Curated pt-br def (Ghidra-extracted; \uXXXX accents for MSVC source safety). */
static const wchar_t* PT_BR_LANG =
    L"pt^Portuguese^Português"
    L"^Segunda-feira,Terça-feira,Quarta-feira,Quinta-feira,Sexta-feira,Sábado,Domingo"
    L"^Seg,Ter,Qua,Qui,Sex,Sáb,Dom"
    L"^Janeiro,Fevereiro,Março,Abril,Maio,Junho,Julho,Agosto,Setembro,Outubro,Novembro,Dezembro"
    L"^Jan,Fev,Mar,Abr,Mai,Jun,Jul,Ago,Set,Out,Nov,Dez^1^%f %l^";
static const wchar_t* PT_BR_REGION =
    L"br^Brazil^Brasil^R^M^^%H:%<02M:%<02S^%#02d/%#02m/%02y^%#d de %>m de %<y"
    L"^R$^.^,^%$%W%?p%?02F^-%$%W%?p%?02F^BRA^076";

static const wchar_t* const NATIVE_CODES[] = { L"en-us", L"de-de", L"fr-fr", L"pl-pl", L"ru-ru" };

static bool IsNativeCode(const std::wstring& code)
{
    for (auto* n : NATIVE_CODES) if (code == n) return true;
    return false;
}

static bool ParseCode(const std::wstring& code, std::wstring& lang, std::wstring& region)
{
    size_t dash = code.find(L'-');
    if (dash == std::wstring::npos) return false;
    lang = code.substr(0, dash);
    region = code.substr(dash + 1);
    return lang.size() >= 2 && region.size() >= 2;
}

/* Walk up from the exe path until a "Data\Locale" directory exists; "" if none. */
static std::wstring FindLocaleDir()
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return L"";
    std::wstring dir = exe;
    for (;;)
    {
        size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return L"";
        dir.resize(slash);
        std::wstring cand = dir + L"\\Data\\Locale";
        DWORD attr = GetFileAttributesW(cand.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return cand;
    }
}

static void RegisterOne(const std::wstring& code, const std::wstring& lang, const std::wstring& region)
{
    if (code == L"pt-br") { s_registerLocale(PT_BR_LANG, PT_BR_REGION); return; }
    std::wstring langDef = lang + EN_LANG_TAIL;
    std::wstring regionDef = region + US_REGION_TAIL;
    s_registerLocale(langDef.c_str(), regionDef.c_str());
}

/* Has C++ objects with destructors -> NO __try here (kept out of the SEH frame in the hook). */
static void RegisterLocaleFolders()
{
    if (!s_registerLocale) return;
    std::wstring localeDir = FindLocaleDir();
    if (localeDir.empty()) { OutputDebugStringW(L"[ReCap locale] Data\\Locale not found\n"); return; }

    std::wstring pattern = localeDir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring code = fd.cFileName;
        if (code == L"." || code == L"..") continue;
        for (wchar_t& ch : code) ch = (wchar_t)towlower(ch);
        if (IsNativeCode(code)) continue;
        std::wstring lang, region;
        if (!ParseCode(code, lang, region)) continue;
        RegisterOne(code, lang, region);
        std::wstring msg = L"[ReCap locale] registered " + code + L"\n";
        OutputDebugStringW(msg.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* No C++ objects with destructors in this frame -> __try/__except is legal here. */
static char __fastcall Hook_LocaleResolver(void* ecx, void* edx)
{
    char r = Real_LocaleResolver(ecx, edx);
    if (!s_localesRegistered)
    {
        s_localesRegistered = true;
        __try { RegisterLocaleFolders(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { OutputDebugStringW(L"[ReCap locale] register pass faulted\n"); }
    }
    return r;
}

void RecapLocaleUnlockPrepare(unsigned char* exeBase)
{
    Real_LocaleResolver = (LocaleResolverFn)(exeBase + RECAP_LOCALERESOLVER_OFFSET);
    s_registerLocale    = (RegisterLocaleFn)(exeBase + RECAP_REGISTERLOCALE_OFFSET);
}

void RecapLocaleUnlockAttach(void)
{
    if (Real_LocaleResolver) DetourAttach(&(PVOID&)Real_LocaleResolver, (PVOID)Hook_LocaleResolver);
}

void RecapLocaleUnlockDetach(void)
{
    if (Real_LocaleResolver) DetourDetach(&(PVOID&)Real_LocaleResolver, (PVOID)Hook_LocaleResolver);
}
