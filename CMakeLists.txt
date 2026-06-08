cmake_minimum_required(VERSION 3.28)

# Darkspore.exe is 32-bit and loads EAWebkit.dll in-process -> MUST build x86.
# Configure from an x86 MSVC dev prompt ("x86 Native Tools" / vcvars32) + Ninja:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build
project(ReCapWebKit LANGUAGES CXX C)   # C for the vendored recapredirect.c (DirtySDK redirect parser)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ReCap.WebKit must build x86 (32-bit) — Darkspore.exe is 32-bit. "
                        "Run from an x86 MSVC dev prompt before invoking CMake.")
endif()

# Static CRT (/MT) for every target — the redistributable DLL must not depend on a Visual C++
# runtime DLL on the player's machine. MUST be set BEFORE any add_library so the fetched Detours
# target also inherits /MT (otherwise it links /MD and the final link fails with unresolved
# __except_handler4_common). CMP0091 (NEW since 3.15) routes this property. NOTE: the C++ standard
# is deliberately set AFTER the Detours block so Detours keeps the compiler default — under C++23 it
# hits C2362 (goto crossing initialization) in creatwth.cpp.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Microsoft Detours (x86) — runtime ws2_32/cert/DPI/WM_QUIT trampolines -------------------
# Prefer a prebuilt local tree (fast local dev); otherwise fetch + compile Detours from source
# so the build is self-contained on a clean machine / CI runner (no hardcoded path, no nmake).
set(RECAP_DETOURS_DIR "C:/CodingProjects/Personal/Detours" CACHE PATH "Prebuilt Microsoft Detours root (optional)")
if(EXISTS "${RECAP_DETOURS_DIR}/lib.X86/detours.lib" AND EXISTS "${RECAP_DETOURS_DIR}/include/detours.h")
    set(RECAP_DETOURS_INCLUDE "${RECAP_DETOURS_DIR}/include")
    set(RECAP_DETOURS_LIB     "${RECAP_DETOURS_DIR}/lib.X86/detours.lib")
    message(STATUS "ReCap.WebKit: using prebuilt Detours at ${RECAP_DETOURS_DIR}")
else()
    include(FetchContent)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)   # allow FetchContent_Populate (Detours has no CMakeLists)
    endif()
    FetchContent_Declare(detours
        GIT_REPOSITORY https://github.com/microsoft/Detours.git
        GIT_TAG        v4.0.1)
    FetchContent_GetProperties(detours)
    if(NOT detours_POPULATED)
        FetchContent_Populate(detours)
    endif()
    # Detours ships no CMakeLists — compile its core x86 TUs directly (disasm.cpp #includes the
    # per-arch decoder internally; image/modules/creatwth round out the API we call).
    add_library(detours STATIC
        ${detours_SOURCE_DIR}/src/detours.cpp
        ${detours_SOURCE_DIR}/src/modules.cpp
        ${detours_SOURCE_DIR}/src/disasm.cpp
        ${detours_SOURCE_DIR}/src/image.cpp
        ${detours_SOURCE_DIR}/src/creatwth.cpp)
    target_include_directories(detours PUBLIC ${detours_SOURCE_DIR}/src)
    target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(detours PRIVATE /w)       # silence Detours' own warnings (runtime via CMAKE_MSVC_RUNTIME_LIBRARY)
    set(RECAP_DETOURS_INCLUDE "${detours_SOURCE_DIR}/src")
    set(RECAP_DETOURS_LIB     detours)
    message(STATUS "ReCap.WebKit: building Detours from source (FetchContent v4.0.1)")
endif()

# C++ standard for OUR targets only (set after Detours so Detours keeps the compiler default).
set(CMAKE_CXX_STANDARD 23)            # bump to 26 when the toolchain supports it (/std:c++latest below)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Sources --------------------------------------------------------------------------------
set(RECAP_SRC
    Source/EAWebkitExports.cpp            # DllMain + host-process hook lifetime
    Source/Recap/ReCapMiniBlink.cpp       # MiniBlink backend: paint / input / focus / keyboard
    Source/Recap/ReCapMiniBlinkBackend.cpp
    Source/Recap/ReCapJsBridge.cpp        # JS bridge (RunJsSync / BindJsMethod, sync-getter preinject)
    Source/Recap/ReCapLog.cpp             # crash-safe logger + ReCapWebKit data folder
    Source/Raster/EARaster.cpp            # concrete EA::Raster::ISurface (ARGB buffer the game reads)
    Source/Raster/EARasterColor.cpp
    Source/View/RecapView.cpp             # EA::WebKit::View (WebCore-free port of EAWebKitView.cpp)
    Source/App/EAWebkitConcrete.cpp       # EA::WebKit::IEAWebkit + CreateEAWebkitInstance
    Source/Stl/EAWebKitSTLWrapper.cpp     # opaque EASTL string/container wrappers (ABI string types)
    Source/Stl/EAWebKitEASTLHelpers.cpp   # GetCharacters/SetCharacters + JS value/hashmap helpers
    # EASTL out-of-line bits the wrappers pull in (gEmptyString, hashtable prime policy,
    # fixed_pool_base::init, red-black tree). Vendored from EASTLEAWebKit/local/source (header-matched).
    Source/Eastl/string.cpp
    Source/Eastl/hashtable.cpp
    Source/Eastl/fixed_pool.cpp
    Source/Eastl/red_black_tree.cpp
    Source/Eastl/allocator.cpp
    Source/Eastl/assert.cpp
    # Host-process hooks: redirect (ws2_32) + ProtoSSL cert bypass + DPI source + WM_QUIT re-post.
    Source/Hooks/ReCapHooks.cpp
    ThirdParty/Recap/recapredirect.c      # recap.cfg parser + lazy loader (DirtySDK origin, C)
)

add_library(EAWebkit SHARED ${RECAP_SRC} Source/EAWebkit.def)
set_target_properties(EAWebkit PROPERTIES OUTPUT_NAME "EAWebkit" PREFIX "")

target_include_directories(EAWebkit PRIVATE
    Abi                          # <EAWebKit/...> <EARaster/...> <EABase/...> — the ABI spec (WebCore-free)
    Source/Recap                 # mb.h + ReCap*.h (quoted includes)
    Source/Hooks                 # ReCapHooks.h
    ThirdParty                   # <EASTL/...> <EAAssert/...> <EAIO/...> <wtf/...>
    ThirdParty/Recap             # recapredirect.h (quoted include)
    "${RECAP_DETOURS_INCLUDE}")

if(MSVC)
    # CXX-only switches (the vendored recapredirect.c is C and must not see /std:c++latest):
    #   /Zc:char8_t- : EABase/eabase.h does `typedef char char8_t`, which collides with C++20/23's
    #   native char8_t under /std:c++latest. Disabling the keyword restores the typedef (ABI-safe).
    target_compile_options(EAWebkit PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/std:c++latest> $<$<COMPILE_LANGUAGE:CXX>:/Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
        /W4)
    target_compile_definitions(EAWebkit PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE RECAP_MINIBLINK
        # We build a DLL but export only CreateEAWebkitInstance via the .def — keep EAWebKit/EARaster
        # APIs as plain (static) symbols (not dllimport/export) and silence the internal-headers
        # "dll build only" guard. EAWEBKIT_STATIC_LINKAGE does both without flipping EA_DLL.
        EAWEBKIT_STATIC_LINKAGE)
    target_link_libraries(EAWebkit PRIVATE
        ws2_32 gdi32 user32 advapi32 winmm wininet shell32
        "${RECAP_DETOURS_LIB}")
endif()

# mb132_x32.dll is loaded at runtime by mb.h (mbSetMbMainDllPath) — no import lib needed.
