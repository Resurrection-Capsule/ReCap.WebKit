// ReCap.WebKit clean-room minimal <EAWebKit/internal/EAWebKitNewDelete.h>.
// EA's original routes these macros through EAWebKit's custom debug operator-new (named/flagged
// placement new resolved by the in-DLL allocator). We drop that allocator and map them to plain
// global new/delete — the only users left (the EASTL wrapper HeaderMap alloc) just need a heap.
#pragma once

#ifndef EAWEBKIT_NEW
    #define EAWEBKIT_NEW(name)                              new
    #define EAWEBKIT_NEW_ALIGNED(alignment, offset, name)  new
    #define EAWEBKIT_DELETE                                 delete
#endif
