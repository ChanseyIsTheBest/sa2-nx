// banjo-nx - native XAudio sample-rate converter: XAUDIO::SRC::LinearVector::ProcessBlock<short,1> (0x82666D60).
// (2026-10-23, the user's request: hand-optimise the audio thread. The converter was 11 % of that thread in the
// console's city profile, after the voice filter and the reverb.)
//
// THE GAME
//   ProcessBlock(r3 = source, mono big-endian int16 samples; r5 = destination, floats, 16-aligned; r6 = output
//   samples; r7 = the converter's state). The state (offsets from r7): +0 the source buffer's start, +4 its frames,
//   +13 the channel count (byte), +20 the destination buffer's start, +24 its capacity in floats; +36 / +40 the volume
//   at the start / end of the block, +44 the step and +48 the position, floats in source samples.
//   The step and the position become 32.32 fixed point (fctidz of x * 2^32). Each group of 8 outputs gathers the
//   8 samples at the integer parts (and the 8 after them) one lvlx at a time, then in VMX with flush to zero:
//     x0 = s0 * 2^-15, x1 = s1 * 2^-15, f = float(fraction >> 1) * 2^-31, v = the volume ramp lane
//     out = (x1 - x0) * (f * v) + x0 * v        (vmaddfp as the build computes it: a multiply, then an add)
//   The ramp starts at v0 + delta * {0..7} (delta = (v1 - v0) / count) and adds 8 * delta per group. The epilogue
//   stores the frames consumed (+8, at most +4's), the floats written (+28, at most +24's), the end volume into +36
//   and the remaining fraction as a float into +48 (through the C runtime's u64tod).
//
// THE NATIVE VERSION
//   The same operations in the same order on the same NEON registers SIMDe gives the recompiled code (its
//   vmulq / vaddq / vsubq, UCVTF / SCVTF), with the same flush mode, so the results are the same bits; the gathers
//   are scalar loads instead of the emulated lvlx (a shuffle table per sample) and nothing goes through the context.
//   The scalar prologue and the epilogue are the original's statements. The PC build runs the same arithmetic one
//   lane at a time (SSE's results equal NEON's for these operations; the flush mode is the context's).
//   Registers: the epilogue's (r3 and f1 from u64tod, r7-r10, f0, f13) are the original's; the loop's vector
//   registers and r4-r6 are scratch the callers do not read (the caller, LinearVector::Process<short,1>, returns
//   right after the call). The stack frame's temporaries are dead after the return and not written.
//   Declined (the original runs): a destination that is not 16-aligned, a count of 0 or less (the original then
//   still makes 8 samples), a channel count of 0 (the original traps), positions outside one stretch of memory.
//
// CVARS
//   banjo_native_xaudio_src (true): native path. banjo_native_xaudio_src_verify (false): run both on the same
//   memory, keep the original's result, compare the output, the state and r3 / f1, log mismatches.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>  // banjo-nx (2026-10-27): REX_PLATFORM_SWITCH
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/diet_ops.h>

#include "cpu/banjo_enhancements.h"
#include "me/me_verify.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src, true, "Banjo/Audio",
                    "Run XAudio's mono 16-bit linear sample-rate converter (SRC::LinearVector::ProcessBlock<short,1>, "
                    "0x82666D60) as native code instead of the recompiled guest code: the same NEON operations without "
                    "the emulated vector gathers (bit-identical)");
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src_verify, false, "Banjo/Audio",
                    "Debug: run both the native and the recompiled converter on every call, keep the recompiled one's "
                    "result and log any difference in the output, the state or r3 / f1");

REX_EXTERN(__imp__rex_XAUDIO__SRC__LinearVector__ProcessBlock_short_1_);
REX_EXTERN(rex_u64tod);
REX_EXTERN(__imp__rex_XAUDIO__CEngine__SupervisorProcessingThreadEntryPoint);  // banjo-nx (2026-10-27): the system core
REX_EXTERN(__imp__rex_XAUDIO__CEngine__PeonProcessingThreadEntryPoint);

namespace banjo_xaudio {
namespace {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
constexpr bool kPhysOffset = true;
#else
constexpr bool kPhysOffset = false;
#endif

inline u8* Host(u8* base, u32 addr) {
  return base + addr + ((kPhysOffset && addr >= 0xE0000000u) ? 0x1000u : 0u);
}
inline bool RangeOk(u32 addr, u64 len) {
  const u64 end = u64(addr) + len;
  if (end > 0x100000000ull) return false;
  if (kPhysOffset && addr < 0xE0000000u && end > 0xE0000000ull) return false;
  return true;
}
inline u32 Ld32(const u8* p) {
  u32 v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void St32(u8* p, u32 v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline float LdF(const u8* p) { return std::bit_cast<float>(Ld32(p)); }
inline int16_t Ld16(const u8* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return int16_t(__builtin_bswap16(v));
}

// One call, decided before anything is written.
struct Plan {
  u32 st = 0, src = 0, dst = 0;
  int32_t count = 0;
  u32 groups = 0;
  int64_t step = 0, pos = 0;
};

// The source samples the gathers read: [src + 2 * first, src + 2 * last + 4) over the 8 * groups positions.
bool Prepare(PPCContext& ctx, u8* base, Plan& p) {
  p.st = ctx.r7.u32;
  p.src = ctx.r3.u32;
  p.dst = ctx.r5.u32;
  p.count = ctx.r6.s32;
  // An odd source address would make lvlx keep one byte of a sample at a 16-byte boundary: declined.
  if (p.count <= 0 || (p.dst & 15u) || (p.src & 1u) || !RangeOk(p.st, 52)) return false;
  const u8* s = Host(base, p.st);
  if (s[13] == 0) return false;  // the original's trap (twllei on the frame size)
  p.groups = u32(p.count + 7) / 8;
  if (!RangeOk(p.dst, u64(p.groups) * 32)) return false;
  // The positions as the original makes them (fctidz of a float times 2^32: never above 2^56).
  const double f9 = double(LdF(s + 44)) * 4294967296.0, f8 = double(LdF(s + 48)) * 4294967296.0;
  p.step = rex::ppc::fctidz(f9);
  p.pos = rex::ppc::fctidz(f8);
  // The last position read: pos + (8 * groups - 1) * step relative to src (the original re-bases every 8 by
  // whole samples, which is the same sum). Out of [0, 2^31) samples: declined.
  const u64 n = u64(p.groups) * 8;
  if (p.step < 0 || p.pos < 0 || p.step > (int64_t(1) << 40) || p.pos > (int64_t(1) << 40)) return false;
  const u64 last = (u64(p.pos) + (n - 1) * u64(p.step)) >> 32;
  if (last > 0x3FFFFFFFull) return false;
  if (!RangeOk(p.src, 2 * last + 4)) return false;
  // Output must not overlap the source nor the state (the original reads both while it writes).
  const u64 out_end = u64(p.dst) + n * 4;
  if (u64(p.src) < out_end && u64(p.dst) < u64(p.src) + 2 * last + 4) return false;
  if (u64(p.st) < out_end && u64(p.dst) < u64(p.st) + 52) return false;
  return true;
}

void Execute(PPCContext& ctx, u8* base, const Plan& p) {
  u8* const s = Host(base, p.st);
  const u8* const src = Host(base, p.src);
  u8* const out = Host(base, p.dst);
  // Prologue (scalar, flush mode off as the original's fsubs sets it).
  const float v0f = LdF(s + 36), v1f = LdF(s + 40);
  ctx.fpscr.disableFlushMode();
  const double f12 = double(float(double(v1f) - double(v0f)));  // fsubs
  const double f3 = double(float(double(int64_t(p.count))));     // fcfid, frsp
  const float delta = float(f12 / f3);                           // fdivs
  St32(s + 36, std::bit_cast<u32>(v1f));                         // stfs f13,36(r31): the end volume
  // The vector part runs with flush to zero (vmaddfp: enableFlushModeUnconditional).
  ctx.fpscr.enableFlushModeUnconditional();
  const float d8 = rex::ppc::DoubleToSingleAnyMode(rex::ppc::MulSingleAnyMode(double(delta), 8.0));  // fmuls, stfs
  const u32 step_lo = u32(p.step), pos_lo = u32(p.pos);
  u64 r11 = u64(p.pos);
  u32 r3 = p.src;
  const u8* sbase = src;
#if defined(__aarch64__)
  const float32x4_t vdelta = vdupq_n_f32(delta), vstart = vdupq_n_f32(v0f), vd8 = vdupq_n_f32(d8);
  const float32x4_t k0 = {0.0f, 1.0f, 2.0f, 3.0f}, k4 = {4.0f, 5.0f, 6.0f, 7.0f};
  float32x4_t volA = vaddq_f32(vmulq_f32(vdelta, k0), vstart);  // vmaddfp v11,v10,v5,v11
  float32x4_t volB = vaddq_f32(vmulq_f32(vdelta, k4), vstart);  // vmaddfp v12,v10,v6,v11
  const uint32x4_t lane0 = {0, 1, 2, 3};
  uint32x4_t fracA = vaddq_u32(vmulq_n_u32(lane0, step_lo), vdupq_n_u32(pos_lo));
  uint32x4_t fracB = vaddq_u32(vmulq_n_u32(vaddq_u32(lane0, vdupq_n_u32(4)), step_lo), vdupq_n_u32(pos_lo));
  const uint32x4_t inc = vdupq_n_u32(step_lo * 8u);
  const float32x4_t k15 = vdupq_n_f32(std::bit_cast<float>(0x38000000u));  // 2^-15
  const float32x4_t k31 = vdupq_n_f32(std::bit_cast<float>(0x30000000u));  // 2^-31
  u8* o = out;
  for (u32 g = 0; g < p.groups; ++g) {
    int16_t a[8], b[8];
    for (int k = 0; k < 8; ++k) {
      const u32 at = (u32(r11 >> 32) << 1) + (r3 - p.src);
      r11 += u64(p.step);
      a[k] = Ld16(sbase + at);
      b[k] = Ld16(sbase + at + 2);
    }
    const int16x8_t sa = vld1q_s16(a), sb = vld1q_s16(b);
    const float32x4_t x0A = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(sa))), k15);
    const float32x4_t x0B = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(sa))), k15);
    const float32x4_t x1A = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(sb))), k15);
    const float32x4_t x1B = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(sb))), k15);
    const float32x4_t fA = vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(fracA, 1)), k31);
    const float32x4_t fB = vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(fracB, 1)), k31);
    r3 += u32(r11 >> 32) << 1;
    r11 &= 0xFFFFFFFFull;
    const float32x4_t v24 = vmulq_f32(x0A, volA), v23 = vmulq_f32(x0B, volB);
    const float32x4_t v22 = vmulq_f32(fA, volA), v20 = vmulq_f32(fB, volB);
    const float32x4_t v21 = vsubq_f32(x1A, x0A), v19 = vsubq_f32(x1B, x0B);
    const float32x4_t outA = vaddq_f32(vmulq_f32(v21, v22), v24);
    const float32x4_t outB = vaddq_f32(vmulq_f32(v19, v20), v23);
    vst1q_u8(o, vrev32q_u8(vreinterpretq_u8_f32(outA)));
    vst1q_u8(o + 16, vrev32q_u8(vreinterpretq_u8_f32(outB)));
    o += 32;
    fracA = vaddq_u32(fracA, inc);
    fracB = vaddq_u32(fracB, inc);
    volA = vaddq_f32(volA, vd8);
    volB = vaddq_f32(volB, vd8);
  }
#else
  // One lane at a time (PC): with the context's flush mode on, as the vector code.
  float vol[8];
  u32 frac[8];
  for (int k = 0; k < 8; ++k) {
    vol[k] = float(delta * float(k)) + v0f;
    frac[k] = pos_lo + u32(k) * step_lo;
  }
  u8* o = out;
  for (u32 g = 0; g < p.groups; ++g) {
    int16_t a[8], b[8];
    for (int k = 0; k < 8; ++k) {
      const u32 at = (u32(r11 >> 32) << 1) + (r3 - p.src);
      r11 += u64(p.step);
      a[k] = Ld16(sbase + at);
      b[k] = Ld16(sbase + at + 2);
    }
    r3 += u32(r11 >> 32) << 1;
    r11 &= 0xFFFFFFFFull;
    for (int k = 0; k < 8; ++k) {
      const float x0 = float(a[k]) * std::bit_cast<float>(0x38000000u);
      const float x1 = float(b[k]) * std::bit_cast<float>(0x38000000u);
      const float f = float(frac[k] >> 1) * std::bit_cast<float>(0x30000000u);
      const float y = (x1 - x0) * (f * vol[k]) + x0 * vol[k];
      St32(o + 4 * k, std::bit_cast<u32>(y));
      frac[k] += step_lo * 8u;
      vol[k] = vol[k] + d8;
    }
    o += 32;
  }
#endif
  rex::memory::MarkStoreCardsAt(base, p.dst, p.groups * 32);
  // The epilogue, as the original's statements (the integer part, then u64tod and the position).
  const u32 dst_end = p.dst + p.groups * 32;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r3;
  ctx.r5.u64 = dst_end;
  ctx.r6.s64 = int64_t(p.count) - 8 * int64_t(p.groups);
  ctx.r10.u64 = s[13];
  ctx.r9.u64 = Ld32(s + 0);
  ctx.r8.u64 = __builtin_rotateleft32(ctx.r10.u32, 1);
  ctx.r10.u64 = Ld32(s + 4);
  ctx.r7.u64 = ctx.r3.u64 - ctx.r9.u64;
  ctx.r9.u64 = rex::ppc::divwu(ctx.r7.u32, ctx.r8.u32);
  if (!(ctx.r9.u32 < ctx.r10.u32)) ctx.r9.u64 = ctx.r10.u64;
  ctx.r10.u64 = Ld32(s + 20);
  St32(s + 8, ctx.r9.u32);
  ctx.r9.u64 = ctx.r5.u64 - ctx.r10.u64;
  ctx.r10.u64 = Ld32(s + 24);
  ctx.r9.u64 = (ctx.r9.u32 >> 2);
  if (ctx.r9.u32 < ctx.r10.u32) ctx.r10.u64 = ctx.r9.u64;
  ctx.r3.u64 = r11;
  St32(s + 28, ctx.r10.u32);
  rex_u64tod(ctx, base);
  ctx.fpscr.disableFlushMode();
  ctx.f0.f64 = ctx.f1.f64 * std::bit_cast<double>(0x3DF0000000000000ull);
  ctx.f13.f64 = double(float(ctx.f0.f64));
  St32(s + 48, std::bit_cast<u32>(float(ctx.f13.f64)));
  rex::memory::MarkStoreCardsAt(base, p.st, 52);
}

struct VerifyStats {
  std::atomic<u64> calls{0}, mismatches{0}, fallbacks{0}, reports{0};
};
VerifyStats g_stats;

void Verify(PPCContext& ctx, u8* base) {
  Plan p;
  const u64 call = g_stats.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!Prepare(ctx, base, p)) {
    g_stats.fallbacks.fetch_add(1, std::memory_order_relaxed);
    __imp__rex_XAUDIO__SRC__LinearVector__ProcessBlock_short_1_(ctx, base);
    return;
  }
  const u32 out_len = p.groups * 32;
  thread_local u8* buffers = nullptr;
  thread_local u32 capacity = 0;
  thread_local PPCContext* nctx = nullptr;
  const u32 need = 2 * (out_len + 52);
  if (capacity < need) {
    delete[] buffers;
    buffers = new u8[need];
    capacity = need;
  }
  if (!nctx) nctx = new PPCContext;
  u8* const before = buffers;
  u8* const mine = buffers + out_len + 52;
  std::memcpy(before, Host(base, p.dst), out_len);
  std::memcpy(before + out_len, Host(base, p.st), 52);
  *nctx = ctx;
  const u32 host_csr = ctx.fpscr.getcsr();
  Execute(*nctx, base, p);
  ctx.fpscr.setcsr(host_csr);
  std::memcpy(mine, Host(base, p.dst), out_len);
  std::memcpy(mine + out_len, Host(base, p.st), 52);
  std::memcpy(Host(base, p.dst), before, out_len);
  std::memcpy(Host(base, p.st), before + out_len, 52);
  __imp__rex_XAUDIO__SRC__LinearVector__ProcessBlock_short_1_(ctx, base);
  bool bad = std::memcmp(Host(base, p.dst), mine, out_len) != 0 ||
             std::memcmp(Host(base, p.st), mine + out_len, 52) != 0 || nctx->r3.u64 != ctx.r3.u64 ||
             nctx->f1.u64 != ctx.f1.u64 || nctx->fpscr.csr != ctx.fpscr.csr;
  ::me::native::g_verify_native_checks.fetch_add(1, std::memory_order_relaxed);
  if (bad) {
    ::me::native::VerifyDifference();
    g_stats.mismatches.fetch_add(1, std::memory_order_relaxed);
    if (g_stats.reports.fetch_add(1, std::memory_order_relaxed) < 20) {
      for (u32 off = 0; off < out_len + 52; off += 4) {
        const u8* orig = off < out_len ? Host(base, p.dst + off) : Host(base, p.st + off - out_len);
        if (std::memcmp(orig, mine + off, 4) != 0) {
          REXLOG_WARN("[banjo_native_xaudio_src] call {} {} +{}: original {:08X} native {:08X}", call,
                      off < out_len ? "out" : "state", off < out_len ? off : off - out_len, Ld32(orig),
                      Ld32(mine + off));
          break;
        }
      }
      REXLOG_WARN("[banjo_native_xaudio_src] call {}: r3 {:X}/{:X} f1 {:X}/{:X} csr {:X}/{:X}", call, ctx.r3.u64,
                  nctx->r3.u64, ctx.f1.u64, nctx->f1.u64, ctx.fpscr.csr, nctx->fpscr.csr);
    }
  }
  if (call % 2000 == 0)
    REXLOG_INFO("[banjo_native_xaudio_src] verify: {} calls, {} mismatching, {} ran the original", call,
                g_stats.mismatches.load(std::memory_order_relaxed), g_stats.fallbacks.load(std::memory_order_relaxed));
}

}  // namespace
}  // namespace banjo_xaudio


// banjo-nx (2026-10-27, the user's request: NetherSX2_nx's 4-core mode; sdk/src/core/threading_switch.h). XAudio's
// engine mixes on its own threads: the supervisor (signalled by the render driver's callback each 256-sample frame)
// and its peons, which take voices from a shared list (CEngine::ProcessVoiceList). With four cores granted
// (banjo_system_core 1 or 2) each moves to the system core as it starts, at its guest priority (0x3B: below the
// Audio Worker, the XMA Decoder and the output thread there). Their entry points are the threads' start addresses
// (reached through the function table, so these hooks take them).
// banjo-nx (2026-10-27, after the first four-core console run): at banjo_system_core_mixer_priority (0x2D) there, so
// nothing of ours at the game threads' 0x3B time-slices it (the shader compiles did: audio requests went unanswered).
#if REX_PLATFORM_SWITCH
extern "C" bool RexSwitchMoveCurrentThreadToSystemCore(int priority);
extern "C" int RexSwitchSystemCoreMixerPriority(void);
#else
inline bool RexSwitchMoveCurrentThreadToSystemCore(int) { return false; }
inline int RexSwitchSystemCoreMixerPriority() { return -1; }
#endif

REX_HOOK_RAW(rex_XAUDIO__CEngine__SupervisorProcessingThreadEntryPoint) {
  if (RexSwitchMoveCurrentThreadToSystemCore(RexSwitchSystemCoreMixerPriority()))
    REXLOG_INFO("[banjo] XAudio supervisor thread on the system core at priority 0x{:X} (banjo_system_core)",
                RexSwitchSystemCoreMixerPriority());
  __imp__rex_XAUDIO__CEngine__SupervisorProcessingThreadEntryPoint(ctx, base);
}

REX_HOOK_RAW(rex_XAUDIO__CEngine__PeonProcessingThreadEntryPoint) {
  if (RexSwitchMoveCurrentThreadToSystemCore(RexSwitchSystemCoreMixerPriority()))
    REXLOG_INFO("[banjo] XAudio peon thread on the system core at priority 0x{:X} (banjo_system_core)",
                RexSwitchSystemCoreMixerPriority());
  __imp__rex_XAUDIO__CEngine__PeonProcessingThreadEntryPoint(ctx, base);
}

REX_HOOK_RAW(rex_XAUDIO__SRC__LinearVector__ProcessBlock_short_1_) {
  static const bool native =
      banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_native_xaudio_src));
  if (native) {
    if (REXCVAR_GET(banjo_native_xaudio_src_verify)) {
      banjo_xaudio::Verify(ctx, base);
      return;
    }
    banjo_xaudio::Plan p;
    if (banjo_xaudio::Prepare(ctx, base, p)) {
      banjo_xaudio::Execute(ctx, base, p);
      return;
    }
  }
  __imp__rex_XAUDIO__SRC__LinearVector__ProcessBlock_short_1_(ctx, base);
}
