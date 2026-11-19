#include <rex/platform.h>
#include <rex/thread/atomic.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

// rexglue ports (sasrt-nx, 2026-10-12): the atomics themselves are inline in rex/thread/atomic.h on POSIX builds.
// They use the __atomic builtins with sequentially consistent ordering: the legacy __sync builtins promise a full
// barrier, so on AArch64 without LSE (the Switch's Cortex-A57) GCC ends each of them with `dmb ish` after the
// exclusive loop (GCC PR 65697): tens of cycles on every critical-section enter and leave, spin lock and
// interlocked list operation. This byte puts the barrier back at run time (the app's key decides; also read by the
// generated code's REX_GUEST_CAS, tools/guest_cas.py).
extern "C" {
unsigned char g_rex_atomic_legacy_barrier = 0;
// blur-nx (2026-11-16, the 15 ms batch): the generated code's guest compare-and-swap as relaxed exclusives
// (tools/guest_cas.py, #163) and its lwsync / eieio as the PowerPC's own orderings (tools/guest_fences.py, #164 /
// #165). Each byte, set by the app's key, gives the old form back at run time: 1 = the sequentially consistent CAS,
// 1 = a full dmb ish for lwsync and eieio.
unsigned char g_rex_guest_cas_seq_cst = 0;
unsigned char g_rex_guest_fence_full = 0;
}
