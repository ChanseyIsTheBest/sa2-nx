/**
 * @file        kernel/crt/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/xmemory.h>

// renut-ultimate (2026-11-16, port 146; tw2de_nx's, from nx-mod/rexglue-sdk-nx; off by default at the user's word).
REXCVAR_DEFINE_BOOL(crt_xmemcpy_as_memmove, false, "Kernel",
                    "tw2de_nx (exact): the native XMemCpy (rexcrt_XMemCpy, which a game maps its own block copy to) "
                    "is a memmove: a game's forward block copy may be called with overlapping ranges, which memcpy "
                    "leaves undefined. false: memcpy, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::kernel::crt {

// ---------------------------------------------------------------------------
// Standard memory operations
// ---------------------------------------------------------------------------

// banjo-nx (2026-10-19): every write of guest memory here marks its store cards (rex/system/xmemory.h), as the guest
// routines' stores would have.
using rex::memory::MarkStoreCardsHost;

static void* native_memcpy(void* dst, const void* src, size_t n) {
  MarkStoreCardsHost(dst, n);
  return std::memcpy(dst, src, n);
}

static void* native_memmove(void* dst, const void* src, size_t n) {
  MarkStoreCardsHost(dst, n);
  return std::memmove(dst, src, n);
}

static void* native_memset(void* dst, int val, size_t n) {
  MarkStoreCardsHost(dst, n);
  return std::memset(dst, val, n);
}

static void* native_memchr(const void* ptr, int val, size_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------

static void* native_XMemCpy(void* dst, const void* src, size_t n) {
  MarkStoreCardsHost(dst, n);
  // Port 146 (crt_xmemcpy_as_memmove): a game's own block copy hooked here may be called with overlapping ranges
  // that its forward copy handled.
  static const bool as_memmove = REXCVAR_GET(crt_xmemcpy_as_memmove);
  if (as_memmove)
    return std::memmove(dst, src, n);
  return std::memcpy(dst, src, n);
}

static void* native_XMemSet(void* dst, int val, size_t n) {
  MarkStoreCardsHost(dst, n);
  return std::memset(dst, val, n);
}

static void* native_XMemSet128(void* dst, int val, size_t n) {
  MarkStoreCardsHost(dst, n);
  return std::memset(dst, val, n);
}

static void* native_memset_vmx(void* dst, int val, size_t n) {
  MarkStoreCardsHost(dst, n);
  return std::memset(dst, val, n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

static int native_memcpy_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  MarkStoreCardsHost(dst, count);
  std::memcpy(dst, src, count);
  return 0;
}

static int native_memmove_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  MarkStoreCardsHost(dst, count);
  std::memmove(dst, src, count);
  return 0;
}

}  // namespace rex::kernel::crt

REX_HOOK(rexcrt_memcpy, rex::kernel::crt::native_memcpy)
REX_HOOK(rexcrt_memmove, rex::kernel::crt::native_memmove)
REX_HOOK(rexcrt_memset, rex::kernel::crt::native_memset)
REX_HOOK(rexcrt_memchr, rex::kernel::crt::native_memchr)
REX_HOOK(rexcrt_XMemCpy, rex::kernel::crt::native_XMemCpy)
REX_HOOK(rexcrt_XMemSet, rex::kernel::crt::native_XMemSet)
REX_HOOK(rexcrt_XMemSet128, rex::kernel::crt::native_XMemSet128)
REX_HOOK(rexcrt_memset_vmx, rex::kernel::crt::native_memset_vmx)
REX_HOOK(rexcrt_memcpy_s, rex::kernel::crt::native_memcpy_s)
REX_HOOK(rexcrt_memmove_s, rex::kernel::crt::native_memmove_s)
