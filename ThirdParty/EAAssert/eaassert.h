// ReCap.WebKit clean-room stub for <EAAssert/eaassert.h>.
// Provides the EA_ASSERT family as no-ops so EASTL / the wrapper TUs compile without the real
// EAAssert package. (We don't ship assertions in the redistributable DLL.)
#pragma once

#ifndef EA_ASSERT
    #define EA_ASSERT(expr)                   ((void)0)
    #define EA_ASSERT_MSG(expr, msg)          ((void)0)
    #define EA_ASSERT_FORMATTED(expr, fmt)    ((void)0)
    #define EA_FAIL()                         ((void)0)
    #define EA_FAIL_MSG(msg)                  ((void)0)
    #define EA_FAIL_FORMATTED(fmt)            ((void)0)
    #define EA_CRASH()                        ((void)0)
#endif

#ifndef EA_COMPILETIME_ASSERT
    // Keep this one REAL — the STL wrappers use it to verify the opaque buffer sizes match the
    // EASTL types they hide (an ABI-size sanity check worth catching at compile time).
    #define EA_COMPILETIME_ASSERT(expr) static_assert((expr), "EA_COMPILETIME_ASSERT: " #expr)
#endif
