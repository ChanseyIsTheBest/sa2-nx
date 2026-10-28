// Sonic Adventure 2 - what every file of native replacements compiles the localized copies with (sa2_hot_guest.cpp
// and the bulk copies' shards sa2_hot_bulk_N.cpp; sa2-nx 2026-10-28, from bejeweled3-nx's bejeweled3_hot_prelude.h).
// Include it first: it brings the generated code's precompiled header and redefines its guest memory macros.
#pragma once

// The generated code's macros and helpers (the localized copies are made of generated code) ...
#include "../../generated/default/sa2_pch.h"

// ... with plain guest memory: the recompiled code's accesses are volatile, which keeps GCC from pairing and
// scheduling them; a native replacement does not need that (a racing write from another thread is a race either way).
#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_LOAD_U8(x) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_STORE_U8(x, y) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define REX_STORE_U16(x, y) (*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))

#include "sa2_hot_guest.h"
#include "sa2_hot_copies.h"

// The localized copies' inline constant-size memcpy / memset (tools/localize_native.py MEMCALL_LINE): the generated
// pch's helpers, as the generated statement uses them; never taken where the generated code does not inline either.
#if defined(REX_INLINE_MEMCALLS_OFF)
#define SA2_MEMCPY_INLINE(n) false
#define SA2_MEMSET_INLINE(n) false
#else
#define SA2_MEMCPY_INLINE(n) RexMemcpyConst<n>(ctx, base)
#define SA2_MEMSET_INLINE(n) RexMemsetConst<n>(ctx, base)
#endif

// Default of the unguarded localized copies (sa2_hot_guest.cpp's and the bulk ones). On since round 4: with round 3's
// smart sync a call syncs only what its callee can reach, and all of them passed the PC checks (verify mode and the
// lockstep capture, pixel-identical). configs/sa2_test_copies_off.toml turns them all off for the console A/B.
#ifndef SA2_COPIES_DEFAULT
#define SA2_COPIES_DEFAULT true
#endif
