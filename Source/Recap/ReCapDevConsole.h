#ifndef RECAP_DEV_CONSOLE_H
#define RECAP_DEV_CONSOLE_H

/* ReCap.WebKit — Darkspore dev command console (telnet), reactivated in-process.

   The retail client ships the dev command console (ConsoleServer + TelnetTransport) and the
   ArgScript command executor fully compiled in, but the code that STARTS the console was stripped
   from the ship build. We rebuild the missing start on the game's main thread, then feed typed
   lines to the game's own command executor — the exact path localCheats.txt uses.

   Enable via recap.cfg `console_port=<port>` (0/absent = disabled); env var RECAP_CONSOLE_PORT
   overrides it. Connect a raw TCP client (ncat / PuTTY-Raw) and type Darkspore console commands
   (see, in the ReCap repo, docs/architecture/research/DARKSPORE_CHEAT_COMMANDS.md), e.g.
   `app -pause`, `state -list`.

   Contract proven live 2026-07-13 (x32dbg on retail 5.3.0.127) — see, in the ReCap repo,
   docs/architecture/research/CONSOLE_REACTIVATION_FEASIBILITY.md. */

namespace recap {

/* Read RECAP_CONSOLE_PORT once and resolve the module base. Call from DllMain. No-op / disabled
   when the env var is absent or invalid, so shipping this is inert unless a dev opts in. */
void DevConsoleInit();

/* Call every frame from the game's main-thread message pump (the darkspore.exe caller of
   PeekMessageW). First such call binds the listen socket; subsequent calls drain typed command
   lines into the executor. No-op when disabled or off the latched main thread. */
void DevConsolePump();

} // namespace recap

#endif /* RECAP_DEV_CONSOLE_H */
