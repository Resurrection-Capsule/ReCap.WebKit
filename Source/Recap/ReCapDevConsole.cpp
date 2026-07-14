// ReCap.WebKit — Darkspore dev command console (telnet), reactivated in-process.
//
// Rebuilds the compiled-but-unwired start of the retail dev console and bridges it to the game's
// own ArgScript command executor. Flow, all on the game's main thread:
//   1. one-shot: construct a TelnetTransport, then ConsoleServer::SetTransport(&console, t, port)
//      — this opens a real listen socket (socket/bind/listen + accept thread), synchronously.
//   2. each frame: poll the transport for new/dropped connections (registers each client's
//      line-editor state — a prerequisite for the transport to accumulate its keystrokes at all),
//      then drain the completed command lines and run each through the game's command executor
//      (registry->ExecuteLine), the exact call the retail localCheats.txt path makes. A raw TCP
//      client connects, the transport echoes typed chars locally; on Enter the line executes.
//      Command effects are visible in-game (server->client text echo is a follow-up).
//
// All client addresses are offsets from the image base — Darkspore.exe has no ASLR (base 0x400000),
// and we resolve GetModuleHandle(NULL)+offset the same way ReCapHooks.cpp resolves its cert thunks.
// Proven live 2026-07-13 (x32dbg, retail 5.3.0.127); see the ReCap repo doc
// docs/architecture/research/CONSOLE_REACTIVATION_FEASIBILITY.md.

#include <windows.h>
#include <detours.h>
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
constexpr unsigned kOff_ConsolePrint        = 0x00466230u; // AppConsole broadcast: FUN_00866230(registry this, const char* line)

// TelnetTransport (vtable 0x0103c8dc) virtual slots the retail ProcessCommand uses to pull lines.
constexpr unsigned kVt_GetNextCommand = 0x14u; // () -> cmd object, or null when the queue is empty
constexpr unsigned kVt_PopCommand     = 0x18u; // (cmd) release a handled command
// Connection lifecycle slots. GetNextCommand ONLY accumulates typed bytes for a client whose
// per-connection line-editor state (a 0x100c block) has been registered — and ONLY PollNewConnection
// allocates+registers it (FUN_00ab6960) and sends the IAC WILL-ECHO negotiation. Without polling it
// each frame, a TCP client connects but GetNextCommand discards its bytes and never yields a command.
// PollDisconnect (FUN_00ab6ac0) reaps the state when the client drops. Both: (this, SystemAddress* out).
constexpr unsigned kVt_PollNewConnection = 0x1cu;
constexpr unsigned kVt_PollDisconnect    = 0x20u;
// Send text back to a connected client — the retail ProcessCommand routes parser output through this
// (transport, connLo, connHi, printf-fmt, ...). Variadic, so MSVC lowers it to __cdecl with `this`
// pushed as the first stack arg, NOT ECX.
constexpr unsigned kVt_Send = 0x0cu;
// Command object fields, as read by the retail ProcessCommand (local_8[6]/local_8[8]).
constexpr unsigned kCmd_Len    = 0x18u; // int   length of the typed line
constexpr unsigned kCmd_Buffer = 0x20u; // char* the typed line bytes (not necessarily NUL-terminated)

typedef void*    (__thiscall* Ctor_t)(void* self);
typedef void     (__thiscall* SetTransport_t)(void* self, void* transport, unsigned int port);
typedef void*    (__cdecl*    GetRegistry_t)(void);
typedef unsigned (__thiscall* ExecuteLine_t)(void* self, const char* line);
typedef void*    (__thiscall* TGetCmd_t)(void* transport);
typedef void     (__thiscall* TPop_t)(void* transport, void* cmd);
typedef void*    (__thiscall* TPoll_t)(void* transport, void* outSystemAddress);
typedef void     (__cdecl*    TSend_t)(void* transport, unsigned connLo, unsigned connHi, const char* fmt, ...);
// FUN_00866230 is __thiscall(registry, line); model it as __fastcall with a dummy EDX so the trampoline
// receives `registry` in ECX and `line` from the stack.
typedef void     (__fastcall* ConsolePrint_t)(void* registry, void* edx, const char* line);

unsigned char* g_base = nullptr;
bool     g_enabled = false;
unsigned g_port    = 0;
bool     g_started = false;
DWORD    g_thread  = 0;

// Echo state: the SystemAddress of the client whose command we are currently running, cached from the
// command object each drain so the output hook (which only receives the text) knows where to send it.
unsigned       g_connLo   = 0;
unsigned       g_connHi   = 0;
bool           g_haveConn = false;
ConsolePrint_t Real_ConsolePrint = nullptr;
void __fastcall Hook_ConsolePrint(void* registry, void* edx, const char* line); // defined below

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

// Mirror AppConsole output to the telnet client. MUST run on the game's main thread post-boot (Detours
// suspends threads + rewrites a prologue — illegal under the DllMain loader lock), so it lives here, not
// in DevConsoleInit.
void InstallEchoHook()
{
    Real_ConsolePrint = At<ConsolePrint_t>(kOff_ConsolePrint);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)Real_ConsolePrint, reinterpret_cast<PVOID>(Hook_ConsolePrint));
    LONG err = DetourTransactionCommit();
    recap::MbLogf("[DevConsole] output-echo hook %s (err=%ld)", err == NO_ERROR ? "installed" : "FAILED", err);
}

// Bind the console: construct the transport, then SetTransport opens the listen socket + accept
// thread synchronously. Returns true if SetTransport stored the transport into the console slot.
bool StartOnce()
{
    void* t = At<Ctor_t>(kOff_TelnetTransportCtor)(g_transportObj);
    if (!t) t = g_transportObj;
    At<SetTransport_t>(kOff_SetTransport)(g_console, t, g_port);
    InstallEchoHook();
    return *reinterpret_cast<void**>(g_console) == t;
}

// Forward one console line to the current telnet client via the transport's own send path. Isolated
// with NO C++ objects so __try/__except is legal — a stale/closed connection must not fault the game.
void SendToTelnet(const char* line)
{
    if (!g_haveConn || !line) return;
    __try {
        void* t = *reinterpret_cast<void**>(g_console);
        if (!t) return;
        void** vt = *reinterpret_cast<void***>(t);
        reinterpret_cast<TSend_t>(vt[kVt_Send / 4])(t, g_connLo, g_connHi, "%s", line);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// AppConsole broadcast hook: mirror every line the game prints to the connected telnet client, then let
// the original run so in-game sinks still receive it. Probes whether command output flows through here.
void __fastcall Hook_ConsolePrint(void* registry, void* edx, const char* line)
{
    SendToTelnet(line);
    Real_ConsolePrint(registry, edx, line);
}

void Drain()
{
    void* t = *reinterpret_cast<void**>(g_console); // the bound TelnetTransport
    if (!t) return;
    void** vt = *reinterpret_cast<void***>(t);
    TGetCmd_t getCmd = reinterpret_cast<TGetCmd_t>(vt[kVt_GetNextCommand / 4]);
    TPop_t    popCmd = reinterpret_cast<TPop_t>(vt[kVt_PopCommand / 4]);

    // Register any newly-connected client (allocates its line-editor state + sends IAC WILL-ECHO) and
    // reap any that dropped. GetNextCommand yields nothing for an unregistered client, so this MUST run
    // before the drain loop. Both are no-ops on a frame with no connect/disconnect pending — safe/cheap.
    alignas(8) char addr[16];
    reinterpret_cast<TPoll_t>(vt[kVt_PollNewConnection / 4])(t, addr);
    reinterpret_cast<TPoll_t>(vt[kVt_PollDisconnect / 4])(t, addr);

    for (int guard = 0; guard < 64; ++guard) // bounded: the queue drains to null each frame
    {
        void* cmd = getCmd(t);
        if (!cmd) break;
        g_connLo   = *reinterpret_cast<unsigned*>(static_cast<char*>(cmd));       // command's SystemAddress,
        g_connHi   = *reinterpret_cast<unsigned*>(static_cast<char*>(cmd) + 4);   // valid while we run it
        g_haveConn = true;
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
