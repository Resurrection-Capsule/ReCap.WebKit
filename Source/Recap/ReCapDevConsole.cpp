// ReCap.WebKit — Darkspore dev command console (telnet), reactivated in-process.
//
// Rebuilds the compiled-but-unwired start of the retail dev console and bridges it to the game's
// own ArgScript command executor. Flow, all on the game's main thread:
//   1. one-shot: construct a TelnetTransport, then ConsoleServer::SetTransport(&console, t, port)
//      — this opens a real listen socket (socket/bind/listen + accept thread), synchronously.
//   2. each frame: drain the transport's completed command lines and run each through the game's
//      command executor (registry->ExecuteLine), the exact call the retail localCheats.txt path
//      makes. A raw TCP client connects, the transport echoes typed chars locally; on Enter the
//      line executes. Command effects are visible in-game (server->client text echo is a follow-up).
//
// All client addresses are offsets from the image base — Darkspore.exe has no ASLR (base 0x400000),
// and we resolve GetModuleHandle(NULL)+offset the same way ReCapHooks.cpp resolves its cert thunks.
// Proven live 2026-07-13 (x32dbg, retail 5.3.0.127); see the ReCap repo doc
// docs/architecture/research/CONSOLE_REACTIVATION_FEASIBILITY.md.

#include <windows.h>
#include <cstring>
#include <cstdlib>

#include "ReCapDevConsole.h"
#include "ReCapLog.h"
#include "recapredirect.h"   // recap.cfg console_port (RecapRedirectGet)

namespace {

// --- retail client symbols, as offsets from base 0x00400000 (5.3.0.127) -------------------------
constexpr unsigned kOff_TelnetTransportCtor = 0x006b6250u; // FUN_00ab6250(this) -> vtable 0x0103c8dc
constexpr unsigned kOff_SetTransport        = 0x006ad990u; // ConsoleServer::SetTransport(this, transport, u16 port)
constexpr unsigned kOff_GetCmdRegistry      = 0x003b3760u; // AppCommandRegistry::GetInstance() -> singleton
constexpr unsigned kOff_ExecuteLine         = 0x004675e0u; // registry vtbl+0x20: ExecuteLine(this, const char* line)

// TelnetTransport (vtable 0x0103c8dc) virtual slots the retail ProcessCommand uses to pull lines.
constexpr unsigned kVt_GetNextCommand = 0x14u; // () -> cmd object, or null when the queue is empty
constexpr unsigned kVt_PopCommand     = 0x18u; // (cmd) release a handled command
// Command object fields, as read by the retail ProcessCommand (local_8[6]/local_8[8]).
constexpr unsigned kCmd_Len    = 0x18u; // int   length of the typed line
constexpr unsigned kCmd_Buffer = 0x20u; // char* the typed line bytes (not necessarily NUL-terminated)

typedef void*    (__thiscall* Ctor_t)(void* self);
typedef void     (__thiscall* SetTransport_t)(void* self, void* transport, unsigned int port);
typedef void*    (__cdecl*    GetRegistry_t)(void);
typedef unsigned (__thiscall* ExecuteLine_t)(void* self, const char* line);
typedef void*    (__thiscall* TGetCmd_t)(void* transport);
typedef void     (__thiscall* TPop_t)(void* transport, void* cmd);

unsigned char* g_base = nullptr;
bool     g_enabled = false;
unsigned g_port    = 0;
bool     g_started = false;
DWORD    g_thread  = 0;

// A ConsoleServer only needs {transport@0, parserArray@4, parserCount@8}; a zeroed static block is
// a valid empty ConsoleServer (proven live). The TelnetTransport ctor zero-inits its own fields;
// Open() lazily allocates its internal TCPInterface via the game's allocator.
alignas(16) char g_console[0x40]      = {0};
alignas(16) char g_transportObj[0x100] = {0};

template <class T> T At(unsigned off) { return reinterpret_cast<T>(g_base + off); }

// Run one command line through the game's ArgScript executor. Isolated with NO C++ objects so
// __try/__except is legal under /EHsc. The retail executor THROWS a C++ exception ("Unknown
// command") on a bad line — surfacing as SEH 0xE06D7363; without this it would unwind through us
// and crash the process, so a typo must be swallowed here. Executor is a no-op until the game has
// initialized ArgScript (post-boot), which is exactly the safe behavior we want.
unsigned SafeExecute(const char* line)
{
    __try {
        void* reg = At<GetRegistry_t>(kOff_GetCmdRegistry)();
        if (!reg) return 0;
        return At<ExecuteLine_t>(kOff_ExecuteLine)(reg, line);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Bind the console: construct the transport, then SetTransport opens the listen socket + accept
// thread synchronously. Returns true if SetTransport stored the transport into the console slot.
bool StartOnce()
{
    void* t = At<Ctor_t>(kOff_TelnetTransportCtor)(g_transportObj);
    if (!t) t = g_transportObj;
    At<SetTransport_t>(kOff_SetTransport)(g_console, t, g_port);
    return *reinterpret_cast<void**>(g_console) == t;
}

void Drain()
{
    void* t = *reinterpret_cast<void**>(g_console); // the bound TelnetTransport
    if (!t) return;
    void** vt = *reinterpret_cast<void***>(t);
    TGetCmd_t getCmd = reinterpret_cast<TGetCmd_t>(vt[kVt_GetNextCommand / 4]);
    TPop_t    popCmd = reinterpret_cast<TPop_t>(vt[kVt_PopCommand / 4]);

    for (int guard = 0; guard < 64; ++guard) // bounded: the queue drains to null each frame
    {
        void* cmd = getCmd(t);
        if (!cmd) break;
        int   len = *reinterpret_cast<int*>(static_cast<char*>(cmd) + kCmd_Len);
        char* buf = *reinterpret_cast<char**>(static_cast<char*>(cmd) + kCmd_Buffer);
        if (len > 0 && buf)
        {
            char line[2048];
            int n = len < static_cast<int>(sizeof(line)) - 1 ? len : static_cast<int>(sizeof(line)) - 1;
            memcpy(line, buf, static_cast<size_t>(n));
            line[n] = '\0';
            recap::MbLogf("[DevConsole] exec: %s", line);
            SafeExecute(line);
        }
        popCmd(t, cmd);
    }
}

} // namespace

namespace recap {

void DevConsoleInit()
{
    // Primary source: recap.cfg `console_port` (0/absent = disabled). Env var RECAP_CONSOLE_PORT
    // overrides it — handy when the Hub launches with -recapServer (which bypasses the cfg file).
    unsigned port = 0;
    const RecapRedirectConfig* cfg = RecapRedirectGet();
    if (cfg && cfg->uConsolePort > 0 && cfg->uConsolePort <= 65535)
        port = cfg->uConsolePort;

    char buf[16];
    DWORD n = GetEnvironmentVariableA("RECAP_CONSOLE_PORT", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf))
    {
        int p = atoi(buf);
        if (p > 0 && p <= 65535) port = static_cast<unsigned>(p);
    }

    if (port == 0) return;                        // opt-in only
    g_base = reinterpret_cast<unsigned char*>(GetModuleHandleA(nullptr));
    if (!g_base) return;
    g_port    = port;
    g_enabled = true;
    MbLogf("[DevConsole] enabled on port %u (base=%p)", g_port, static_cast<void*>(g_base));
}

void DevConsolePump()
{
    if (!g_enabled) return;
    if (!g_started)
    {
        // First main-thread (darkspore.exe caller) frame: bind the socket, latch the thread so we
        // only ever construct/pump the console engine from the game's own message-pump thread.
        g_thread  = GetCurrentThreadId();
        g_started = true;
        bool ok = StartOnce();
        MbLogf("[DevConsole] start %s (port=%u thread=%lu)", ok ? "OK" : "FAILED", g_port, g_thread);
        return;
    }
    if (GetCurrentThreadId() != g_thread) return; // main thread only
    Drain();
}

} // namespace recap
