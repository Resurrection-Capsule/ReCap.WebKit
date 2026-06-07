#pragma once

/* Locale unlocker: registers every Data/Locale/<code>/ folder as a selectable client locale
   by calling the client's own registrar after its locale resolver runs. See
   docs/superpowers/specs/2026-06-07-locale-unlocker-design.md (in the ReCap server repo). */

/* Resolve the client function pointers from the exe base. Call BEFORE the Detour transaction. */
void RecapLocaleUnlockPrepare(unsigned char* exeBase);

/* Attach/detach the resolver detour. Call INSIDE the Detour transaction the other hooks use. */
void RecapLocaleUnlockAttach(void);
void RecapLocaleUnlockDetach(void);
