// Compile-only check for the Switch compiler (it cannot run on the PC): the AArch64-only forms of the CPU round of
// 2026-10-12, one small function each, so tools/arm_forms_check.py can read the instructions GCC makes of them.
//   - rex/thread/atomic.h: the inline __atomic lock words (exclusive pair with acquire/release, no trailing `dmb`
//     outside the branch on g_rex_atomic_legacy_barrier), next to the legacy __sync forms they replace;
//   - rex/ppc/intrinsics.h: vperm and the two float-to-integer conversions as their NEON instruction, with
//     REX_VMX_HELPERS_X86_FORM the x86 emulation through simde;
//   - rex/ppc/diet_ops.h: the any-mode float helpers compile (their PC test is sdk/tests/fp_any_mode_test.cpp).
//
//   python tools/arm_forms_check.py
#include <cstdint>

#include <rex/ppc/diet_ops.h>
#include <rex/ppc/intrinsics.h>
#include <rex/thread/atomic.h>

#define CHECK_FN extern "C" __attribute__((noinline))

CHECK_FN int32_t check_atomic_inc(volatile int32_t* p) { return rex::thread::atomic_inc(p); }
CHECK_FN int32_t check_atomic_dec(volatile int32_t* p) { return rex::thread::atomic_dec(p); }
CHECK_FN bool check_atomic_cas32(volatile int32_t* p) { return rex::thread::atomic_cas(int32_t(-1), int32_t(0), p); }
CHECK_FN bool check_atomic_cas64(volatile int64_t* p, int64_t a, int64_t b) { return rex::thread::atomic_cas(a, b, p); }
CHECK_FN int32_t check_atomic_exchange_add(volatile int32_t* p, int32_t n) {
  return rex::thread::atomic_exchange_add(n, p);
}

// The legacy forms (what atomic_posix.cpp compiled before, and what the generator's stwcx. was).
CHECK_FN int32_t check_legacy_inc(volatile int32_t* p) { return __sync_add_and_fetch(p, 1); }
CHECK_FN bool check_legacy_cas32(volatile int32_t* p) { return __sync_bool_compare_and_swap(p, -1, 0); }

CHECK_FN simde__m128i check_vperm(simde__m128i a, simde__m128i b, simde__m128i c) {
  return rex::ppc::simde_mm_perm_epi8_(a, b, c);
}
CHECK_FN simde__m128i check_vctsxs(simde__m128 a) { return rex::ppc::simde_mm_vctsxs(a); }
CHECK_FN simde__m128i check_vctuxs(simde__m128 a) { return rex::ppc::simde_mm_vctuxs(a); }

CHECK_FN double check_add_single(double a, double b) { return rex::ppc::AddSingleAnyMode(a, b); }
CHECK_FN double check_round_single(double a) { return rex::ppc::RoundToSingleAnyMode(a); }
CHECK_FN bool check_ge_zero(double a) { return rex::ppc::GeZeroAnyMode(a); }
