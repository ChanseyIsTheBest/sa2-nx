/**
 * @file        ppc/context.h
 * @brief       PPC thread context, guest function macros, and interrupt handling
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp PPCContext infrastructure
 */

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/platform/fpscr.h>
#include <rex/ppc/diet_ops.h>
#include <rex/ppc/func.h>
#include <rex/sys_counters.h>
#include <rex/types.h>

#include <simde/x86/avx.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

//=============================================================================
// Pack/Unpack Constants (NORMPACKED32 - 2:10:10:10 format)
//=============================================================================

constexpr float kPack2101010_Min10 = std::bit_cast<float>(0x403FFE01u);
constexpr float kPack2101010_Max10 = std::bit_cast<float>(0x404001FFu);
constexpr float kPack2101010_Min2 = std::bit_cast<float>(0x40400000u);
constexpr float kPack2101010_Max2 = std::bit_cast<float>(0x40400003u);

// renut-ultimate (2026-11-16, port 144; tw2de_nx's): the Xenon's spin-wait hints (db16cyc, or r1,r1,r1) with the
// codegen option spin_hint_yield call this (sdk/src/system/xthread.cpp): the thread yields its core, so a spinning
// thread lets the one it waits for run (the 360 had six hardware threads; the Switch gives a game three cores).
extern "C" void rex_spin_hint(void);

namespace rex::ppc {

//=============================================================================
// General Purpose Register
//=============================================================================

union Register {
  int8_t s8;
  uint8_t u8;
  int16_t s16;
  uint16_t u16;
  int32_t s32;
  uint32_t u32;
  int64_t s64;
  uint64_t u64;
  float f32;
  double f64;
};

//=============================================================================
// Fixed-Point Exception Register (XER)
//=============================================================================

struct XERRegister {
  uint8_t so;
  uint8_t ov;
  uint8_t ca;
};

//=============================================================================
// Condition Register (CR) Field
//=============================================================================

struct CRRegister {
  uint8_t lt;
  uint8_t gt;
  uint8_t eq;
  union {
    uint8_t so;
    uint8_t un;
  };

  REX_SMALL_HELPER uint32_t raw() const noexcept { return (lt << 3) | (gt << 2) | (eq << 1) | so; }

  REX_SMALL_HELPER void set_raw(uint32_t value) noexcept {
    lt = (value >> 3) & 1;
    gt = (value >> 2) & 1;
    eq = (value >> 1) & 1;
    so = value & 1;
  }

  template <typename T>
  REX_SMALL_HELPER void compare(T left, T right, const XERRegister& xer) noexcept {  // (diet_ops.h: forced inline)
    lt = left < right;
    gt = left > right;
    eq = left == right;
    REX_NX_CENSUS_IF(xer.so, 11);
#if defined(REX_NX_XER_SO_ZERO)
    so = 0;  // blur-nx (2026-11-13, #5 of docs/cpu-near-exact-50-1113.md): diet_ops.h
#else
    so = xer.so;
#endif
  }

  REX_SMALL_HELPER void compare(double left, double right) noexcept {
#ifndef REX_NO_FAST_FCMP
    // Mass Effect diet: same truth table, but the quiet comparison builtins let one fcmp serve all four
    // flags (the relational operators and == compiled to fcmpe and fcmp pairs plus NaN tests).
    REX_NX_CENSUS_IF(__builtin_isunordered(left, right), 5);
#if defined(REX_NX_FCMP_NO_UN)
    un = 0;  // blur-nx (2026-11-13, #41): diet_ops.h (lt, gt and eq are 0 for a NaN either way)
#else
    un = __builtin_isunordered(left, right);
#endif
    lt = __builtin_isless(left, right);
    gt = __builtin_isgreater(left, right);
    eq = left == right;
#else
    un = __builtin_isnan(left) || __builtin_isnan(right);
    lt = !un && (left < right);
    gt = !un && (left > right);
    eq = !un && (left == right);
#endif
  }

  inline void setFromMask(simde__m128 mask, int imm) noexcept {
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_SETFROMMASK_MOVEMASK)
    // sasrt-nx (2026-11-11, port 71; its idea 43 B4): cr6 of a vcmp*fp. with the all-lanes immediate (the only one the
    // generator emits): lt when every lane's sign bit is set, eq when none is, for any mask. Each lane to all-ones /
    // zero by its sign (an arithmetic shift), narrowed to 16 bits, read as one 64-bit word: all-ones exactly when all
    // four are set, zero exactly when none is. movemask's shifts, `addv` and constant go. REX_SETFROMMASK_MOVEMASK:
    // the movemask form.
    if (imm == 0xF) {  // (exact for any imm; folds when inlined)
      const uint64_t s = vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(vreinterpretq_u32_s32(
                                           vshrq_n_s32(vreinterpretq_s32_f32(simde__m128_to_neon_f32(mask)), 31)))),
                                       0);
      lt = s == ~uint64_t(0);
      gt = 0;
      eq = s == 0;
      so = 0;
      return;
    }
#endif
    int m = simde_mm_movemask_ps(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }

  inline void setFromMask(simde__m128i mask, int imm) noexcept {
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_SETFROMMASK_MOVEMASK)
    // (Port 71, the byte form: vcmp*. on integers, immediate 0xFFFF.) Each byte to all-ones / zero by its sign, two
    // bytes' halves into one byte (shift right by 4 and narrow), read as one 64-bit word.
    if (imm == 0xFFFF) {
      const uint64_t s = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_s8(
                                           vshrq_n_s8(simde__m128i_to_neon_i8(mask), 7)), 4)),
                                       0);
      lt = s == ~uint64_t(0);
      gt = 0;
      eq = s == 0;
      so = 0;
      return;
    }
#endif
    int m = simde_mm_movemask_epi8(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }
};

//=============================================================================
// Vector Register (128-bit)
//=============================================================================

union alignas(0x10) VRegister {
  int8_t s8[16];
  uint8_t u8[16];
  int16_t s16[8];
  uint16_t u16[8];
  int32_t s32[4];
  uint32_t u32[4];
  int64_t s64[2];
  uint64_t u64[2];
  float f32[4];
  double f64[2];
};

//=============================================================================
// Floating-Point Status and Control Register (FPSCR)
//=============================================================================

constexpr uint32_t kRoundNearest = 0x00;
constexpr uint32_t kRoundTowardZero = 0x01;
constexpr uint32_t kRoundUp = 0x02;
constexpr uint32_t kRoundDown = 0x03;
constexpr uint32_t kRoundMask = 0x03;

// blur-nx (2026-11-12, codegen-ideas-3 CG-13's census; PC game code only: REX_DIET_CENSUS_ON, x86): an FPU-mode region
// (the guest code between a switch of the flush mode off and the next switch on) ends at that switch; the host's
// sticky status flags then say whether an operation in it met a denormal operand or gave an underflow
// (rex::syscount::CensusFpuRegionEnd). So that the flags are the region's own, the cached word every switch writes
// back keeps them clear (InitHost, loadFromHost): each switch then clears what the previous region raised.
#if defined(REX_DIET_CENSUS_ON) && !defined(__SWITCH__) && (defined(__x86_64__) || defined(_M_X64))
#define REX_FPU_CENSUS_REGION_END() ::rex::syscount::CensusFpuRegionEnd(uint32_t(getcsr()))
#define REX_FPU_CENSUS_CLEAN(word) ((word) &= ~decltype(word)(0x3F))
#else
#define REX_FPU_CENSUS_REGION_END() ((void)0)
#define REX_FPU_CENSUS_CLEAN(word) ((void)0)
#endif

// blur-nx (2026-11-12, codegen-ideas-3 CG-13, the user: "build CG 13 ... it probably should be off by default"): CMake
// BLUR_FPCR_ONE_MODE (REX_FPCR_ONE_MODE) keeps the host's flush mode on for all guest code: a guest thread sets it at
// start (InitHost), any other thread at its first switch on (enableFlushMode still writes it once when the cached word
// says off), and every switch off is nothing, so there is no `msr fpcr` in the game code after the first. The VMX unit
// flushes denormals as the Xenon's does; the scalar FPU now flushes them too, which the Xenon's does not: NOT
// bit-exact for scalar values below FLT_MIN / DBL_MIN (the census, REX_DIET_CENSUS_ON, counts them on the PC).
#if defined(REX_FPCR_ONE_MODE)
#ifndef REX_FLUSH_KNOWN_ON
#define REX_FLUSH_KNOWN_ON() ((void)0)
#define REX_FLUSH_KNOWN_OFF() ((void)0)
#endif
#ifndef REX_FLUSH_HOIST_KNOWN_ON
#define REX_FLUSH_HOIST_KNOWN_ON() ((void)0)
#define REX_FLUSH_HOIST_KNOWN_OFF() ((void)0)
#endif
#endif

struct FPSCRRegister {
  uint32_t csr;

  static constexpr size_t HostToGuest[] = {kRoundNearest, kRoundDown, kRoundUp, kRoundTowardZero};

  using Platform = platform::FPSCRPlatform;
  static constexpr size_t RoundShift = Platform::RoundShift;
  static constexpr size_t RoundMaskVal = Platform::RoundMaskVal;
  static constexpr size_t FlushMask = Platform::FlushMask;

  // Bits the guest owns; the rest is host policy seeded by InitHost.
  static constexpr uint32_t GuestMask = uint32_t(RoundMaskVal) | uint32_t(FlushMask);

  inline uint32_t getcsr() noexcept { return Platform::getcsr(); }
  inline void setcsr(uint32_t csr) noexcept { Platform::setcsr(csr); }

  // Restoring the whole word would unmask every FP exception when csr is 0.
  inline void restoreGuestBits(uint32_t saved) noexcept {
    csr = (getcsr() & ~GuestMask) | (saved & GuestMask);
#if defined(REX_FPCR_ONE_MODE)
    csr |= FlushMask;  // CG-13
#endif
    setcsr(csr);
  }

  inline uint32_t loadFromHost() noexcept {
    REX_NX_CENSUS(10);  // blur-nx (2026-11-13, #4)
    csr = getcsr();
    REX_FPU_CENSUS_CLEAN(csr);
    return HostToGuest[(csr & RoundMaskVal) >> RoundShift];
  }

  inline void storeFromGuest(uint32_t value) noexcept {
    REX_SYS_CENSUS((value & kRoundMask) == kRoundNearest  ? rex::syscount::kCensusRoundNearest
                   : (value & kRoundMask) == kRoundTowardZero ? rex::syscount::kCensusRoundZero
                                                              : rex::syscount::kCensusRoundUpDown,
                   1);
    // blur-nx (2026-11-13, docs/cpu-near-exact-50-1113.md #3): the census of the writes; the write itself (an `msr fpcr`,
    // dozens of cycles on the A57) skipped when the host's word already is what it would write (exact: read back with
    // `mrs`, not taken from the cached copy, which a thread-context restore sets without writing the host's;
    // REX_FPSCR_WRITE_ALWAYS: every write, as before). REX_NX_RN_ONLY (near-exact, NOT for Blur: its census shows
    // round-toward-zero selected, 11,776 times against 23,629 to nearest): the rounding mode stays round-to-nearest.
    const uint32_t next = uint32_t((csr & ~RoundMaskVal) | Platform::GuestToHost[value & kRoundMask]);
    REX_NX_CENSUS(7);
    REX_NX_CENSUS_IF(next != csr, 8);
    REX_NX_CENSUS_IF((value & kRoundMask) != kRoundNearest, 9);
#if defined(REX_NX_RN_ONLY)
    return;
#endif
    csr = next;
#if !defined(REX_FPSCR_WRITE_ALWAYS)
    if (next == getcsr()) {
      return;
    }
#endif
    setcsr(csr);
  }

  REX_SMALL_HELPER void enableFlushModeUnconditional() noexcept {
#if defined(REX_FPCR_ONE_MODE)
    enableFlushMode();
    return;
#endif
    REX_FPU_CENSUS_REGION_END();
    REX_SYS_CENSUS_SITE(rex::syscount::kCensusFlushWrite);
    if ((csr & FlushMask) == FlushMask) REX_SYS_CENSUS(rex::syscount::kCensusFlushSame, 1);
    csr |= FlushMask;
    setcsr(csr);
  }

  REX_SMALL_HELPER void disableFlushModeUnconditional() noexcept {
#if defined(REX_FPCR_ONE_MODE)
    return;
#endif
    REX_SYS_CENSUS_SITE(rex::syscount::kCensusFlushWrite);
    if ((csr & FlushMask) == 0) REX_SYS_CENSUS(rex::syscount::kCensusFlushSame, 1);
    csr &= ~FlushMask;
    setcsr(csr);
  }

  REX_SMALL_HELPER void enableFlushMode() noexcept {
    if ((csr & FlushMask) != FlushMask) [[unlikely]] {
      REX_FPU_CENSUS_REGION_END();
      REX_SYS_CENSUS_SITE(rex::syscount::kCensusFlushWrite);
      csr |= FlushMask;
      setcsr(csr);
    }
  }

  REX_SMALL_HELPER void disableFlushMode() noexcept {
#if defined(REX_FPCR_ONE_MODE)
    return;
#endif
    if ((csr & FlushMask) != 0) [[unlikely]] {
      REX_SYS_CENSUS_SITE(rex::syscount::kCensusFlushWrite);
      csr &= ~FlushMask;
      setcsr(csr);
    }
  }

  // tools/flush_mode_flow.py: the test of a check the control flow proved redundant (PC builds keep it).
  REX_SMALL_HELPER bool flushModeIsOn() const noexcept { return (csr & FlushMask) == FlushMask; }
#if defined(REX_FPCR_ONE_MODE)
  REX_SMALL_HELPER bool flushModeIsOff() const noexcept { return true; }  // CG-13: no mode to check
#else
  REX_SMALL_HELPER bool flushModeIsOff() const noexcept { return (csr & FlushMask) == 0; }
#endif

  inline void InitHost() noexcept {
    csr = getcsr();
    REX_FPU_CENSUS_CLEAN(csr);
    Platform::InitHostExceptions(csr);
#if defined(REX_FPCR_ONE_MODE)
    csr |= FlushMask;  // CG-13: the flush mode on for the thread's whole life
#endif
    setcsr(csr);
  }
};

}  // namespace rex::ppc

using PPCRegister = rex::ppc::Register;
using PPCXERRegister = rex::ppc::XERRegister;
using PPCCRRegister = rex::ppc::CRRegister;
using PPCVRegister = rex::ppc::VRegister;
using PPCFPSCRRegister = rex::ppc::FPSCRRegister;

//=============================================================================
// PPCContext Structure
//=============================================================================

struct alignas(0x40) PPCContext {
  PPCRegister r3;
  PPCRegister r0;
  PPCRegister r1;
  PPCRegister r2;
  PPCRegister r4;
  PPCRegister r5;
  PPCRegister r6;
  PPCRegister r7;
  PPCRegister r8;
  PPCRegister r9;
  PPCRegister r10;
  PPCRegister r11;
  PPCRegister r12;
  PPCRegister r13;
  PPCRegister r14;
  PPCRegister r15;
  PPCRegister r16;
  PPCRegister r17;
  PPCRegister r18;
  PPCRegister r19;
  PPCRegister r20;
  PPCRegister r21;
  PPCRegister r22;
  PPCRegister r23;
  PPCRegister r24;
  PPCRegister r25;
  PPCRegister r26;
  PPCRegister r27;
  PPCRegister r28;
  PPCRegister r29;
  PPCRegister r30;
  PPCRegister r31;

  uint64_t lr;
  PPCRegister ctr;
  PPCXERRegister xer;
  PPCRegister reserved;
  uint32_t msr = 0x200A000;
  PPCCRRegister cr0;
  PPCCRRegister cr1;
  PPCCRRegister cr2;
  PPCCRRegister cr3;
  PPCCRRegister cr4;
  PPCCRRegister cr5;
  PPCCRRegister cr6;
  PPCCRRegister cr7;
  PPCFPSCRRegister fpscr;
  uint8_t vscr_sat = 0;  // VSCR saturation flag (for vector ops)
  // renut-ultimate (2026-11-16, port 144; tw2de_nx's): VSCR NJ as mtvscr wrote it (1 at reset), for mfvscr
  // (codegen option extra_instructions). In the padding after vscr_sat: no field moves. The vector unit always flushes.
  uint8_t vscr_nj = 1;

  /**
   * Last indirect call target address. Set by REX_CALL_INDIRECT_FUNC before
   * dispatch. Used by the invalid-function trap to report the faulting address.
   * Unconditional (not guarded by config flags) because ctr may be optimized
   * to a local variable via REX_CONFIG_CTR_AS_LOCAL.
   */
  uint32_t last_indirect_target = 0;

  PPCRegister f0;
  PPCRegister f1;
  PPCRegister f2;
  PPCRegister f3;
  PPCRegister f4;
  PPCRegister f5;
  PPCRegister f6;
  PPCRegister f7;
  PPCRegister f8;
  PPCRegister f9;
  PPCRegister f10;
  PPCRegister f11;
  PPCRegister f12;
  PPCRegister f13;
  PPCRegister f14;
  PPCRegister f15;
  PPCRegister f16;
  PPCRegister f17;
  PPCRegister f18;
  PPCRegister f19;
  PPCRegister f20;
  PPCRegister f21;
  PPCRegister f22;
  PPCRegister f23;
  PPCRegister f24;
  PPCRegister f25;
  PPCRegister f26;
  PPCRegister f27;
  PPCRegister f28;
  PPCRegister f29;
  PPCRegister f30;
  PPCRegister f31;

  PPCVRegister v0;
  PPCVRegister v1;
  PPCVRegister v2;
  PPCVRegister v3;
  PPCVRegister v4;
  PPCVRegister v5;
  PPCVRegister v6;
  PPCVRegister v7;
  PPCVRegister v8;
  PPCVRegister v9;
  PPCVRegister v10;
  PPCVRegister v11;
  PPCVRegister v12;
  PPCVRegister v13;
  PPCVRegister v14;
  PPCVRegister v15;
  PPCVRegister v16;
  PPCVRegister v17;
  PPCVRegister v18;
  PPCVRegister v19;
  PPCVRegister v20;
  PPCVRegister v21;
  PPCVRegister v22;
  PPCVRegister v23;
  PPCVRegister v24;
  PPCVRegister v25;
  PPCVRegister v26;
  PPCVRegister v27;
  PPCVRegister v28;
  PPCVRegister v29;
  PPCVRegister v30;
  PPCVRegister v31;
  PPCVRegister v32;
  PPCVRegister v33;
  PPCVRegister v34;
  PPCVRegister v35;
  PPCVRegister v36;
  PPCVRegister v37;
  PPCVRegister v38;
  PPCVRegister v39;
  PPCVRegister v40;
  PPCVRegister v41;
  PPCVRegister v42;
  PPCVRegister v43;
  PPCVRegister v44;
  PPCVRegister v45;
  PPCVRegister v46;
  PPCVRegister v47;
  PPCVRegister v48;
  PPCVRegister v49;
  PPCVRegister v50;
  PPCVRegister v51;
  PPCVRegister v52;
  PPCVRegister v53;
  PPCVRegister v54;
  PPCVRegister v55;
  PPCVRegister v56;
  PPCVRegister v57;
  PPCVRegister v58;
  PPCVRegister v59;
  PPCVRegister v60;
  PPCVRegister v61;
  PPCVRegister v62;
  PPCVRegister v63;
  PPCVRegister v64;
  PPCVRegister v65;
  PPCVRegister v66;
  PPCVRegister v67;
  PPCVRegister v68;
  PPCVRegister v69;
  PPCVRegister v70;
  PPCVRegister v71;
  PPCVRegister v72;
  PPCVRegister v73;
  PPCVRegister v74;
  PPCVRegister v75;
  PPCVRegister v76;
  PPCVRegister v77;
  PPCVRegister v78;
  PPCVRegister v79;
  PPCVRegister v80;
  PPCVRegister v81;
  PPCVRegister v82;
  PPCVRegister v83;
  PPCVRegister v84;
  PPCVRegister v85;
  PPCVRegister v86;
  PPCVRegister v87;
  PPCVRegister v88;
  PPCVRegister v89;
  PPCVRegister v90;
  PPCVRegister v91;
  PPCVRegister v92;
  PPCVRegister v93;
  PPCVRegister v94;
  PPCVRegister v95;
  PPCVRegister v96;
  PPCVRegister v97;
  PPCVRegister v98;
  PPCVRegister v99;
  PPCVRegister v100;
  PPCVRegister v101;
  PPCVRegister v102;
  PPCVRegister v103;
  PPCVRegister v104;
  PPCVRegister v105;
  PPCVRegister v106;
  PPCVRegister v107;
  PPCVRegister v108;
  PPCVRegister v109;
  PPCVRegister v110;
  PPCVRegister v111;
  PPCVRegister v112;
  PPCVRegister v113;
  PPCVRegister v114;
  PPCVRegister v115;
  PPCVRegister v116;
  PPCVRegister v117;
  PPCVRegister v118;
  PPCVRegister v119;
  PPCVRegister v120;
  PPCVRegister v121;
  PPCVRegister v122;
  PPCVRegister v123;
  PPCVRegister v124;
  PPCVRegister v125;
  PPCVRegister v126;
  PPCVRegister v127;

  //--- Host scratch (rexglue ports; sasrt-nx 2026-10-12) --------
  // Per-thread counters for the host's hooks, reached through the context every hook is handed: no thread_local
  // (on the Switch each access is a call to __aarch64_read_tp) and no counter shared by all threads (a cache line
  // bouncing between cores on every hooked call). Not guest state: never saved, restored or compared; a context
  // copied and copied back only repeats a few counts. Zero in a new thread's context (value-initialised).
  static constexpr size_t kHostCounters = 64;
  uint32_t host_counters[kHostCounters];

  //--- Non-volatile register save/restore --------
  // Layout: r14-r31 (144) | f14-f31 (144) | v14-v31 (288) | v64-v127 (1024)
  //       | cr2-cr4 (12) | fpscr (4).  Total: 1616 bytes.
  static constexpr size_t kNonVolatileSaveSize =
      18 * sizeof(PPCRegister) + 18 * sizeof(PPCRegister) + 18 * sizeof(PPCVRegister) +
      64 * sizeof(PPCVRegister) + 3 * sizeof(PPCCRRegister) + sizeof(PPCFPSCRRegister);

  inline void SaveNonVolatiles(uint8_t* dst) const {
    std::memcpy(dst, &r14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &f14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &v14, 18 * sizeof(PPCVRegister));
    dst += 18 * sizeof(PPCVRegister);
    std::memcpy(dst, &v64, 64 * sizeof(PPCVRegister));
    dst += 64 * sizeof(PPCVRegister);
    std::memcpy(dst, &cr2, 3 * sizeof(PPCCRRegister));
    dst += 3 * sizeof(PPCCRRegister);
    std::memcpy(dst, &fpscr, sizeof(PPCFPSCRRegister));
  }

  inline void RestoreNonVolatiles(const uint8_t* src) {
    std::memcpy(&r14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&f14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&v14, src, 18 * sizeof(PPCVRegister));
    src += 18 * sizeof(PPCVRegister);
    std::memcpy(&v64, src, 64 * sizeof(PPCVRegister));
    src += 64 * sizeof(PPCVRegister);
    std::memcpy(&cr2, src, 3 * sizeof(PPCCRRegister));
    src += 3 * sizeof(PPCCRRegister);
    PPCFPSCRRegister saved_fpscr;
    std::memcpy(&saved_fpscr, src, sizeof(PPCFPSCRRegister));
    fpscr.restoreGuestBits(saved_fpscr.csr);
  }
};
