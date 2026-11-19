// banjo-nx - native XAudio reverb: princeton_digital::stereo_room_t<float>::process (0x826508C0).
//
// THE GAME
//   XAUDIO::CReverbEffect::Process -> XAUDIO::CStereoRoomFloat::Process (0x8264FE40) ->
//   stereo_room_t<float>::process(r3 = room, r4 = &{in L, in R}, r5 = &{6 channel outputs}, r6 = 256).
//   One call renders one 256-sample frame; the recompiled version costs about a quarter of a core on
//   the Switch (every float is a ctx register round trip, every access a volatile byte-swapped load).
//   The room (r3, about 300 KB) holds the whole network, laid out as (offsets from r3):
//     +92 wet, +96 early gain, +100 input mode (1: L only, 2: R only, else stereo)
//     per channel (L / R): vardelay_t<16384> +104 / +65672, occlusion_t<2> +131240 / +131272,
//       mix gains +131304.. / +149424.., threetap_t<512> +131312 / +149432, allpass_t<128> +133404 /
//       +151524, threetap_t<2048> +133940 / +152060, allpass_t<256> +142176 / +160296,
//       allpass_t<512> +143224 / +161344, twotap_t<1024> +145296 / +163416
//     the late network +167544..+299311 (allpass 512/1024/2048, two-tap 4096 delays, 2-sample IIR
//       sections), run sample by sample, every section fed by the others' outputs of the previous sample
//     output allpass_t<512> x4 +299312.. and vardelay_t<256> x2 +307600 / +308656.
//   Temporaries live in a 17968-byte guest stack frame (dead after return). Outputs: channels 0, 1, 3
//   and 4 of r5 (front L/R from in and the late network, surround L/R from the final vardelays).
//
// THE NATIVE VERSION
//   The same arithmetic per sample, with the state in host registers, the stack temporaries in host
//   arrays and each guest word byte-swapped once. The stages run in the guest's order, except that the
//   two independent input chains run side by side and the late network runs a section at a time over
//   the whole block when its two-tap delays allow it (see LateBlock); block loops go 4 samples per
//   step (vector extensions: NEON on the Switch, SSE on PC) where no sample of a group needs another
//   one of the same group. Bit-identical:
//     - fadds/fsubs/fmuls are computed in double and rounded to float by the recompiler; a float op
//       gives the same bits (double has more than 2p+2 bits).
//     - fmadds is double(float(fma(a, c, b))) in double: with float inputs a*c is exact in double, so
//       any double multiply-add rounded to float is the same value (std::fma on AArch64, where it is
//       one instruction; mul + add elsewhere).
//     - The guest compiler did not contract the same way in every unrolled copy (which product is
//       rounded and which one is fused differs by position); every such case is reproduced by position
//       (occlusion, vardelay crossfade, the first mix loop, the IIR sections).
//     - Scalar FP runs with flush-to-zero off and the output mix with it on, as the generated code
//       does (ctx.fpscr.disableFlushMode / enableFlushMode), and the csr ends as the epilogue leaves it.
//     - The output mix's vmaddfp follows the build's contraction (MixFuses).
//   NaN-ness is reproduced, NaN payloads and signs are not (they depend on each compiler's operand
//   order, and the generated code quiets signalling NaNs on every lfs/stfs). Real audio has no NaNs.
//   The caller (0x8264FE40) reads no volatile register after the call, so none is set; nonvolatile
//   registers are untouched (the original restores them). The one write above the frame, the spill
//   of r3 to the caller's home slot (stw r30,17988(r1) = caller sp + 20), is reproduced.
//
// GUARDS
//   The native path assumes every delay-line write lands inside its line (true for the game's own
//   state: write indices advance by 256 per call) and that the input buffers do not overlap the room.
//   Anything else (corrupt indices, odd buffers) runs the original for that call.
//
// CVARS
//   banjo_native_reverb (true): native path. banjo_native_reverb_verify (false): run both on the same
//   memory snapshot, keep the original's result, compare the room, the four output buffers and the
//   home slot byte for byte, log mismatches and a summary every 500 calls.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/fma_single.h>  // port 96

#include "cpu/banjo_enhancements.h"
#include "me/me_verify.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

REXCVAR_DEFINE_BOOL(banjo_native_reverb, true, "Banjo/Audio",
                    "Run XAudio's reverb (stereo_room_t<float>::process, 0x826508C0) as native code "
                    "instead of the recompiled guest code (bit-identical; the recompiled one costs "
                    "about a quarter of a Switch core)");
REXCVAR_DEFINE_BOOL(banjo_native_reverb_verify, false, "Banjo/Audio",
                    "Debug: run both the native and the recompiled reverb on every call, keep the "
                    "recompiled result and log any byte that differs");

REX_EXTERN(__imp__rex_princeton_digital__stereo_room_t_float___process);

namespace banjo_reverb {
namespace {

using u8 = uint8_t;
using u32 = uint32_t;

//-----------------------------------------------------------------------------
// Guest memory
//-----------------------------------------------------------------------------

// Same mapping as the generated REX_RAW_ADDR: on Windows (and macOS arm64) the 0xE0000000+ physical
// window sits 0x1000 further in host memory.
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
constexpr bool kPhysOffset = true;
#else
constexpr bool kPhysOffset = false;
#endif

inline u8* Host(u8* base, u32 addr) {
  return base + addr + ((kPhysOffset && addr >= 0xE0000000u) ? 0x1000u : 0u);
}

// [addr, addr + len) is one contiguous host range (no 4 GB wrap, no physical-window seam).
inline bool RangeOk(u32 addr, u32 len) {
  const uint64_t end = uint64_t(addr) + len;
  if (end > 0x100000000ull) return false;
  if (kPhysOffset && addr < 0xE0000000u && end > 0xE0000000ull) return false;
  return true;
}

inline bool Overlap(u32 a, u32 alen, u32 b, u32 blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
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
inline void StF(u8* p, float f) { St32(p, std::bit_cast<u32>(f)); }

// Orders the flush-mode switches against the FP work around them (the csr write is an asm statement
// GCC may otherwise move memory accesses across).
inline void Barrier() { std::atomic_signal_fence(std::memory_order_seq_cst); }

//-----------------------------------------------------------------------------
// PPC arithmetic as the recompiler emits it
//-----------------------------------------------------------------------------

// fmadds frD = frA * frC + frB as the processed generated code makes it: rex::ppc::FmaddsSingle (renut-ultimate,
// 2026-11-12, port 96: tools/fma_single.py; on floats one single-precision fused multiply-add, the Xenon's one rounding;
// REX_FMADDS_DOUBLE_ROUNDING, CMake BLUR_FMADDS_SINGLE OFF: double(float(std::fma(A, C, B))) as before).
inline float Fmadds(float a, float c, float b) {
  return float(::rex::ppc::FmaddsSingle(double(a), double(c), double(b)));
}

// fctidz, exactly the generated expression (only the low word is used).
inline u32 FctidzLow(double v) {
  const int64_t r = std::isnan(v)                ? int64_t(0x8000000000000000ULL)
                    : (v > double(LLONG_MAX))    ? LLONG_MAX
                                                 : simde_mm_cvttsd_si64(simde_mm_load_sd(&v));
  return u32(uint64_t(r));
}

//-----------------------------------------------------------------------------
// Four samples at a time (GCC/Clang vector extensions: NEON on the Switch, SSE on PC). Lane
// operations are the same IEEE single/double operations as the scalar code, so a block loop can
// run 4 samples per step wherever no sample of a group depends on another one of the same group.
//-----------------------------------------------------------------------------

using F4 = float __attribute__((vector_size(16)));
using D4 = double __attribute__((vector_size(32)));
using B16 = uint8_t __attribute__((vector_size(16)));

inline F4 Splat(float f) { return F4{f, f, f, f}; }

// Host floats: one or four (unaligned).
template <class T>
inline T At(const float* p) {
  if constexpr (std::is_same_v<T, float>) {
    return *p;
  } else {
    F4 v;
    std::memcpy(&v, p, 16);
    return v;
  }
}
inline void Put(float* p, F4 v) { std::memcpy(p, &v, 16); }

inline B16 Swap32(B16 b) {
  return __builtin_shufflevector(b, b, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
}
inline F4 LdF4(const u8* p) {
  B16 b;
  std::memcpy(&b, p, 16);
  b = Swap32(b);
  F4 v;
  std::memcpy(&v, &b, 16);
  return v;
}
inline void StF4(u8* p, F4 v) {
  B16 b;
  std::memcpy(&b, &v, 16);
  b = Swap32(b);
  std::memcpy(p, &b, 16);
}

inline F4 Fmadds(F4 a, F4 c, F4 b) {
  const D4 r = __builtin_convertvector(a, D4) * __builtin_convertvector(c, D4) + __builtin_convertvector(b, D4);
  return __builtin_convertvector(r, F4);
}

// Four consecutive entries of a circular delay line from index i (big-endian guest floats). The
// group that straddles the end of the line goes out of line so the compiler keeps the common path
// to one compare, one 16-byte access and one byte reverse.
template <u32 M>
[[gnu::noinline, gnu::cold]] F4 LineLd4Wrap(const u8* line, u32 i) {
  return F4{LdF(line + 4 * i), LdF(line + 4 * ((i + 1) & M)), LdF(line + 4 * ((i + 2) & M)),
            LdF(line + 4 * ((i + 3) & M))};
}
template <u32 M>
[[gnu::noinline, gnu::cold]] void LineSt4Wrap(u8* line, u32 i, F4 v) {
  for (u32 k = 0; k < 4; ++k) StF(line + 4 * ((i + k) & M), v[k]);
}
template <u32 M>
inline F4 LineLd4(const u8* line, u32 i) {
  i &= M;
  if (i <= M - 3) [[likely]]
    return LdF4(line + 4 * i);
  return LineLd4Wrap<M>(line, i);
}
template <u32 M>
inline void LineSt4(u8* line, u32 i, F4 v) {
  i &= M;
  if (i <= M - 3) [[likely]]
    StF4(line + 4 * i, v);
  else
    LineSt4Wrap<M>(line, i, v);
}

// A line written at w + j and read at w + j - d can run 4 samples per step unless a read of the
// group needs a write of the same group (d mod N in 1..3; 0 reads the slot before it is written).
template <u32 M>
inline bool GroupSafe(u32 d) {
  d &= M;
  return d == 0 || d >= 4;
}

// a * b + c with one rounding (vfmaq_f32 / fmla on the Switch).
inline F4 FusedMulAdd(F4 a, F4 b, F4 c) {
#if defined(__aarch64__)
  return F4(vfmaq_f32(float32x4_t(c), float32x4_t(a), float32x4_t(b)));
#else
  return F4{std::fma(a[0], b[0], c[0]), std::fma(a[1], b[1], c[1]), std::fma(a[2], b[2], c[2]),
            std::fma(a[3], b[3], c[3])};
#endif
}

// Keeps the compiler from fusing a product into the add that follows it.
inline F4 Opaque(F4 v) {
#if defined(__aarch64__)
  asm("" : "+w"(v));
#elif defined(__x86_64__) || defined(_M_X64)
  asm("" : "+x"(v));
#endif
  return v;
}

// The generated code emits vmaddfp as simde_mm_add_ps(simde_mm_mul_ps(a, b), c). The app targets
// are not built with -ffp-contract=off (only the SDK is; GCC defaults to "fast"), so on the Switch
// GCC turns that into an fmla: the output loop at 0x82651F9C fuses dry * in and rounds wet * late
// (checked in the disassembly with the app's flags; the scalar fmuls/fadds cannot fuse because
// every result is rounded to float first). This file is built with the same flags as the generated
// code, so the same idiom here shows which way they went. 1.000244140625^2 - 1.00048828125 is
// 2^-24 when fused and 0 when the product is rounded first.
[[gnu::noinline]] bool ProbeMulAddFuses() {
  static volatile float a = 1.000244140625f, c = -1.00048828125f;
  const simde__m128 va = simde_mm_set1_ps(a), vc = simde_mm_set1_ps(c);
  float r[4];
  simde_mm_storeu_ps(r, simde_mm_add_ps(simde_mm_mul_ps(va, va), vc));
  return r[0] != 0.0f;
}
inline bool MixFuses() {
  static const bool fused = ProbeMulAddFuses();
  return fused;
}

//-----------------------------------------------------------------------------
// Layout
//-----------------------------------------------------------------------------

constexpr u32 kRoomSize = 309712;  // end of the last vardelay_t<256> (+308656 + 32 + 256 * 4)
constexpr u32 kInputSpan = 1040;   // vardelay_t<16384>'s vector copy reads 260 floats

// Guest image constants (read each call; they are .rdata).
constexpr u32 kAddrOne = 0x82000C1C;       // C1 (crossfade total; occlusion 1 - a)
constexpr u32 kAddrStep = 0x82109854;      // crossfade step per sample
constexpr u32 kAddrIdle = 0x82000C20;      // crossfade start when no new delay is pending
constexpr u32 kAddrFadeScale = 0x82106E9C; // crossfade position -> samples * 2
constexpr u32 kAddrCopyStep = 0x82109858;  // crossfade decrement of the vector copy path
constexpr u32 kAddrLateOut = 0x820FF0B0;   // late network -> output allpasses
constexpr u32 kAddrTapIn = 0x820FED64;     // IIR section -> 4096 two-tap line
constexpr u32 kAddrDry = 0x82DA3340;       // vector: dry = this - wet

struct Consts {
  float one, step, idle, fade_scale, copy_step, late_out, tap_in;
};

struct alignas(64) Scratch {
  // Guest stack temporaries (offsets in the 17968-byte frame noted for reference).
  alignas(16) float in_l[264];   // vardelay_t<16384> inputs (guest in L/R, 260 floats read)
  alignas(16) float in_r[264];
  alignas(16) float vd_l[264];   // +7744 vardelay out (one buffer per channel here)
  alignas(16) float vd_r[264];
  alignas(16) float oc_l[264];   // +9920 occlusion out
  alignas(16) float oc_r[264];
  alignas(16) float l_tt1[264];  // +480  L threetap512 out1 -> allpass128 in place
  alignas(16) float l_tt2[264];  // +6704 L threetap512 out2
  alignas(16) float l_t1[264];   // (+16639)&~127  L threetap2048 out1 -> allpass256 in place
  alignas(16) float l_t2[264];   // +12096 L threetap2048 out2
  alignas(16) float l_ap[264];   // +4640 L allpass512 out
  alignas(16) float r_tt1[264];  // +2560
  alignas(16) float r_tt2[264];  // +4640 (reused)
  alignas(16) float r_t1[264];   // +1520
  alignas(16) float r_t2[264];   // +13136
  alignas(16) float r_ap[264];   // +3600
  alignas(16) float al5[264];    // (+15471)&~127  L twotap out1, late network L in/out
  alignas(16) float al6[264];    // (+14303)&~127  L twotap out2 at +2 floats, final vardelay out
  alignas(16) float ar5[264];    // (+10047)&~127
  alignas(16) float ar6[264];    // (+7871)&~127
  alignas(16) float lmix[256];   // +5680
  alignas(16) float rmix[256];   // +3600 (reused)
  alignas(16) float late[32][260];  // block form of the late network (LateRow)
};

//-----------------------------------------------------------------------------
// Block stages (256 samples; out[0] carries the previous block's last sample)
//-----------------------------------------------------------------------------

// vardelay_t<float, N>::preprocess (0x826520B8 N=16384, 0x826531C0 N=256).
// +4 crossfade xf, +8 delay A (new), +12 delay B (old), +16 target, +20 last, +24 write index, +32 line.
// in may alias out + 2 (the final stage runs in place on a stack buffer): in[j] is always read
// before out[j + 1] is written, as in the original.
template <u32 N, bool kCopyPath>
void VarDelay(u8* o, const float* in, float* out, const Consts& k) {
  constexpr u32 M = N - 1;
  u8* const line = o + 32;
  out[0] = LdF(o + 20);
  const u32 w = Ld32(o + 24);
  u32 dA = Ld32(o + 8);
  u32 dB = Ld32(o + 12);
  float xf = LdF(o + 4);
  const u32 n = FctidzLow(double(xf * k.fade_scale)) >> 1;

  if (n < 256) {
    // Crossfade from B to A over n samples, then start the next one (A -> target) for the rest.
    float g = k.one - xf;
    const u32 n4 = n & ~3u;  // the guest's 4x unrolled loop; positions 1..3 round the other product
    for (u32 j = 0; j < n; ++j) {
      const float a = LdF(line + 4 * ((w + j - dA) & M));
      const float b = LdF(line + 4 * ((w + j - dB) & M));
      StF(line + 4 * (w + j), in[j]);
      out[j + 1] = (j < n4 && (j & 3) != 0) ? Fmadds(a, g, b * xf) : Fmadds(b, xf, a * g);
      xf = xf - k.step;
      g = g + k.step;
    }
    const u32 target = Ld32(o + 16);
    xf = (target == dA) ? k.idle : k.one;
    g = k.one - xf;
    St32(o + 12, dA);
    St32(o + 8, target);
    dB = dA;
    dA = target;
    for (u32 j = n; j < 256; ++j) {
      const float a = LdF(line + 4 * ((w + j - dA) & M));
      const float b = LdF(line + 4 * ((w + j - dB) & M));
      StF(line + 4 * (w + j), in[j]);
      out[j + 1] = Fmadds(b, xf, a * g);
      xf = xf - k.step;
      g = g + k.step;
    }
    StF(o + 4, xf);
    St32(o + 24, (w + 256) & M);
    StF(o + 20, out[256]);
    return;
  }

  if constexpr (kCopyPath) {
    // 0x82652508: both read windows and the write window inside the line without wrapping, and
    // no fade pending (r6 at 0x826520E0: unsigned |dA - dB| > 4 both ways, i.e. any difference).
    const u32 wa = w - dA, wb = w - dB;
    const bool fading = (dA - dB > 4u) || (dB - dA > 4u);
    if (w + 256 < N && wb + 256 < N && wa + 256 < N && int32_t(wb) >= 0 && int32_t(wa) >= 0 && !fading) {
      // Vector copy (lvlx/lvrx ... stvlx/stvrx): 65 chunks of 16 bytes, read before write,
      // 4 floats past the frame on both sides (the extra output floats are never read).
      u8* dst = line + 4 * w;
      const u8* src = line + 4 * wb;
      for (u32 c = 0; c < 65; ++c) {
        const F4 old = LdF4(src + 16 * c);
        StF4(dst + 16 * c, At<F4>(in + 4 * c));
        if (c < 64) Put(out + 1 + 4 * c, old);
      }
      StF(o + 4, xf - k.copy_step);
      St32(o + 24, w + 256);
      StF(o + 20, out[256]);
      return;
    }
    // The fading variant at 0x8266A0F4 reads the same windows unmasked; with the bounds above the
    // masked general path below is the same computation (and w + 256 < N needs no mask).
  }

  // Steady state: out = B * xf + A * (1 - xf), xf walking one step per sample (rounded each step).
  alignas(16) float xs[256], gs[256];
  float g = k.one - xf;
  for (u32 j = 0; j < 256; ++j) {
    xs[j] = xf;
    gs[j] = g;
    g = g + k.step;
    xf = xf - k.step;
  }
  if (GroupSafe<M>(dA) && GroupSafe<M>(dB)) {
    for (u32 j = 0; j < 256; j += 4) {
      const F4 a = LineLd4<M>(line, w - dA + j);
      const F4 b = LineLd4<M>(line, w - dB + j);
      StF4(line + 4 * (w + j), At<F4>(in + j));
      Put(out + j + 1, Fmadds(b, At<F4>(xs + j), a * At<F4>(gs + j)));
    }
  } else {
    for (u32 j = 0; j < 256; ++j) {
      const float a = LdF(line + 4 * ((w - dA + j) & M));
      const float b = LdF(line + 4 * ((w - dB + j) & M));
      StF(line + 4 * (w + j), in[j]);
      out[j + 1] = Fmadds(b, xs[j], a * gs[j]);
    }
  }
  StF(o + 4, xf);
  St32(o + 24, (w + 256) & M);
  StF(o + 20, out[256]);
}

// occlusion_t<float, 2>::preprocess (0x82652708): two one-pole lowpasses in series.
// +16 s1, +20 s2, +24 gain, +28 a. The guest's 8x unrolled loop fuses gain*(1-a)*x at position 0
// and (x*gain)*(1-a) elsewhere. Both channels run in one loop (independent state) so their two
// recurrences overlap.
struct OcclusionState {
  float a, gain, s1, s2, oma, g;
  void Load(const u8* o, const Consts& k) {
    a = LdF(o + 28);
    gain = LdF(o + 24);
    s2 = LdF(o + 20);
    s1 = LdF(o + 16);
    oma = k.one - a;
    g = gain * oma;
  }
  float Step(u32 j, float x) {
    const float as1 = a * s1, as2 = a * s2;
    const float n1 = (j & 7) == 0 ? Fmadds(g, x, as1) : Fmadds(x * gain, oma, as1);
    const float n2 = Fmadds(g, s1, as2);
    s1 = n1;
    s2 = n2;
    return n2;
  }
  void Store(u8* o) const {
    StF(o + 16, s1);
    StF(o + 20, s2);
  }
};

void Occlusion2(u8* ol, const float* inl, float* outl, u8* orr, const float* inr, float* outr,
                const Consts& k) {
  OcclusionState l, r;
  l.Load(ol, k);
  r.Load(orr, k);
  outl[0] = l.s2;
  outr[0] = r.s2;
  for (u32 j = 0; j < 256; ++j) {
    outl[j + 1] = l.Step(j, inl[j]);
    outr[j + 1] = r.Step(j, inr[j]);
  }
  l.Store(ol);
  r.Store(orr);
}

// threetap_t<float, N>::preprocess3 (0x82652848 N=512, 0x82652BC0 N=2048).
// +0 D0, +4 c0, +8 last1, +12 D1, +16 c1, +24 D2, +28 c2, +32 last2, +36 write index, +44 line.
template <u32 N>
void ThreeTap(u8* o, const float* in, float* out1, float* out2) {
  constexpr u32 M = N - 1;
  u8* const line = o + 44;
  const u32 d0 = Ld32(o + 0), d1 = Ld32(o + 12), d2 = Ld32(o + 24), w = Ld32(o + 36);
  const float c0 = LdF(o + 4), c1 = LdF(o + 16), c2 = LdF(o + 28);
  out1[0] = LdF(o + 8);
  out2[0] = LdF(o + 32);
  if (GroupSafe<M>(d0) && GroupSafe<M>(d1) && GroupSafe<M>(d2)) {
    const F4 c0v = Splat(c0), c1v = Splat(c1), c2v = Splat(c2);
    for (u32 j = 0; j < 256; j += 4) {
      const F4 t0 = LineLd4<M>(line, w - d0 + j);
      const F4 t1 = LineLd4<M>(line, w - d1 + j);
      const F4 t2 = LineLd4<M>(line, w - d2 + j);
      StF4(line + 4 * (w + j), At<F4>(in + j));
      Put(out2 + j + 1, t2 * c2v);
      Put(out1 + j + 1, Fmadds(t1, c1v, t0 * c0v));
    }
  } else {
    for (u32 j = 0; j < 256; ++j) {
      const float t0 = LdF(line + 4 * ((w - d0 + j) & M));
      const float t1 = LdF(line + 4 * ((w - d1 + j) & M));
      const float t2 = LdF(line + 4 * ((w - d2 + j) & M));
      StF(line + 4 * (w + j), in[j]);
      out2[j + 1] = t2 * c2;
      out1[j + 1] = Fmadds(t1, c1, t0 * c0);
    }
  }
  StF(o + 8, out1[256]);
  StF(o + 32, out2[256]);
  St32(o + 36, (w + 256) & M);
}

// allpass_t<float, N> over a block: x[j] in, y[0] = previous last, y[j + 1] = output of sample j.
// +0 D, +4 b, +8 a, +12 last, +16 write index, +24 line. Writes go to (w + j) & M (N=128 writes j,
// then j - 128 in the guest: the same with w == 0, which the guard requires).
template <u32 N>
void AllpassRun(u8* o, const float* x, float* y) {
  constexpr u32 M = N - 1;
  u8* const line = o + 24;
  const u32 d = Ld32(o + 0), w = Ld32(o + 16);
  const float b = LdF(o + 4), a = LdF(o + 8);
  y[0] = LdF(o + 12);
  if (GroupSafe<M>(d)) {
    const F4 av = Splat(a), bv = Splat(b);
    for (u32 j = 0; j < 256; j += 4) {
      const F4 t = LineLd4<M>(line, w - d + j);
      const F4 v = Fmadds(av, t, At<F4>(x + j));
      LineSt4<M>(line, w + j, v);
      Put(y + j + 1, Fmadds(bv, v, t));
    }
  } else {
    for (u32 j = 0; j < 256; ++j) {
      const float t = LdF(line + 4 * ((w - d + j) & M));
      const float v = Fmadds(a, t, x[j]);
      StF(line + 4 * ((w + j) & M), v);
      y[j + 1] = Fmadds(b, v, t);
    }
  }
  StF(o + 12, y[256]);
  St32(o + 16, (w + 256) & M);
}

// allpass_t<float, N>::preprocess (0x82652910 N=128, 0x82652C88 N=256, 0x82652F98 N=512), in place:
// io[j] in, io[j] = previous output, io[256] = last.
template <u32 N>
void AllpassInPlace(u8* o, float* io) {
  alignas(16) float y[260];
  AllpassRun<N>(o, io, y);
  std::memcpy(io, y, 257 * sizeof(float));
}

// allpass_t<float, 512>::preprocess (0x82652E10), out of place: out[0..255], and in[256] = last.
void Allpass512Out(u8* o, float* in, float* out) {
  alignas(16) float y[260];
  AllpassRun<512>(o, in, y);
  std::memcpy(out, y, 256 * sizeof(float));
  in[256] = y[256];
}

// twotap_t<float, 1024>::preprocess (0x82653110).
// +0 D0, +4 c0, +8 last1, +12 D1, +16 c1, +20 last2, +24 write index, +32 line.
void TwoTap1024(u8* o, const float* in, float* out1, float* out2) {
  constexpr u32 M = 1023;
  u8* const line = o + 32;
  const u32 d0 = Ld32(o + 0), d1 = Ld32(o + 12), w = Ld32(o + 24);
  const float c0 = LdF(o + 4), c1 = LdF(o + 16);
  out1[0] = LdF(o + 8);
  out2[0] = LdF(o + 20);
  if (GroupSafe<M>(d0) && GroupSafe<M>(d1)) {
    const F4 c0v = Splat(c0), c1v = Splat(c1);
    for (u32 j = 0; j < 256; j += 4) {
      const F4 t0 = LineLd4<M>(line, w - d0 + j);
      const F4 t1 = LineLd4<M>(line, w - d1 + j);
      StF4(line + 4 * (w + j), At<F4>(in + j));
      Put(out1 + j + 1, t0 * c0v);
      Put(out2 + j + 1, t1 * c1v);
    }
  } else {
    for (u32 j = 0; j < 256; ++j) {
      const float t0 = LdF(line + 4 * ((w - d0 + j) & M));
      const float t1 = LdF(line + 4 * ((w - d1 + j) & M));
      StF(line + 4 * (w + j), in[j]);
      out1[j + 1] = t0 * c0;
      out2[j + 1] = t1 * c1;
    }
  }
  StF(o + 8, out1[256]);
  StF(o + 20, out2[256]);
  St32(o + 24, (w + 256) & M);
}

//-----------------------------------------------------------------------------
// Late network (0x82651398 loop): sections and their state
//-----------------------------------------------------------------------------

// allpass_t<float, N>: +0 D, +4 b, +8 a, +12 y, +16 write index, +24 line.
template <u32 N>
struct LineAllpass {
  static constexpr u32 M = N - 1;
  u8* line;
  u32 d, w;
  float a, b, y;
  void Load(u8* o) {
    d = Ld32(o + 0);
    b = LdF(o + 4);
    a = LdF(o + 8);
    y = LdF(o + 12);
    w = Ld32(o + 16);
    line = o + 24;
  }
  void Store(u8* o) const {
    StF(o + 12, y);
    St32(o + 16, w);
  }
  float Read() const { return LdF(line + 4 * ((w - d) & M)); }
  void Write(float v) {
    StF(line + 4 * w, v);
    w = (w + 1) & M;
  }
};

// The 2048 variant with an extra feed-forward gain: +0 D, +4 b, +8 a, +12 c, +16 y, +20 index, +28 line.
struct LineAllpassC {
  static constexpr u32 M = 2047;
  u8* line;
  u32 d, w;
  float a, b, c, y;
  void Load(u8* o) {
    d = Ld32(o + 0);
    b = LdF(o + 4);
    a = LdF(o + 8);
    c = LdF(o + 12);
    y = LdF(o + 16);
    w = Ld32(o + 20);
    line = o + 28;
  }
  void Store(u8* o) const {
    StF(o + 16, y);
    St32(o + 20, w);
  }
  float Read() const { return LdF(line + 4 * ((w - d) & M)); }
  void Write(float v) {
    StF(line + 4 * w, v);
    w = (w + 1) & M;
  }
};

// twotap_t<float, 4096>: +0 D0, +4 c0, +8 y0, +12 D1, +16 c1, +20 y1, +24 write index, +32 line.
struct LineTwoTap {
  static constexpr u32 M = 4095;
  u8* line;
  u32 d0, d1, w;
  float c0, c1, y0, y1;
  void Load(u8* o) {
    d0 = Ld32(o + 0);
    c0 = LdF(o + 4);
    y0 = LdF(o + 8);
    d1 = Ld32(o + 12);
    c1 = LdF(o + 16);
    y1 = LdF(o + 20);
    w = Ld32(o + 24);
    line = o + 32;
  }
  void Store(u8* o) const {
    StF(o + 8, y0);
    StF(o + 20, y1);
    St32(o + 24, w);
  }
  float Read0() const { return LdF(line + 4 * ((w - d0) & M)); }
  float Read1() const { return LdF(line + 4 * ((w - d1) & M)); }
  void Write(float v) {
    StF(line + 4 * w, v);
    w = (w + 1) & M;
  }
};

// Two-sample history read at a fixed tap (index & 1, constant for the call).
struct History {
  float h0, h1;
  bool second;
  void Load(const u8* p, u32 index) {
    h0 = LdF(p);
    h1 = LdF(p + 4);
    second = (index & 1) != 0;
  }
  void Store(u8* p) const {
    StF(p, h0);
    StF(p + 4, h1);
  }
  float Tap() const { return second ? h1 : h0; }
  void Push(float x) {
    h1 = h0;
    h0 = x;
  }
};

// Short sections on a two-sample history (the tap index is fixed for the call). Three layouts:
// SecA (s3, s7, s10, s14), SecB (s4, s11), SecC (s5, s12, s20, s27); the formulas differ per
// section and are written out where they are used.
struct SecA {  // +0 index, +4 c, +8 y, +16 history
  float c, y;
  History h;
  void Load(u8* o) {
    c = LdF(o + 4);
    y = LdF(o + 8);
    h.Load(o + 16, Ld32(o + 0));
  }
  void Store(u8* o) const {
    StF(o + 8, y);
    h.Store(o + 16);
  }
};
struct SecB {  // +0 index, +4 c1, +8 c2, +12 c3, +16 y, +24 history
  float c1, c2, c3, y;
  History h;
  void Load(u8* o) {
    c1 = LdF(o + 4);
    c2 = LdF(o + 8);
    c3 = LdF(o + 12);
    y = LdF(o + 16);
    h.Load(o + 24, Ld32(o + 0));
  }
  void Store(u8* o) const {
    StF(o + 16, y);
    h.Store(o + 24);
  }
};
struct SecC {  // +0 index, +4 y, +8 c, +12 c0, +20 history
  float y, c, c0;
  History h;
  void Load(u8* o) {
    y = LdF(o + 4);
    c = LdF(o + 8);
    c0 = LdF(o + 12);
    h.Load(o + 20, Ld32(o + 0));
  }
  void Store(u8* o) const {
    StF(o + 4, y);
    h.Store(o + 20);
  }
};

// All sections of the late network (offsets from the room).
struct LateState {
  LineAllpass<512> ap1, ap2, ap8, ap9;
  LineAllpass<2048> ap15, ap17, ap21, ap23;
  LineAllpass<1024> ap18, ap25;
  LineAllpassC apc16, apc22;
  LineTwoTap tt6, tt13, tt19, tt26;
  SecA s3, s7, s10, s14;
  SecB s4, s11;
  SecC s5, s12, s20, s27;

  void Load(u8* r) {
    ap1.Load(r + 167544);
    ap2.Load(r + 169616);
    s3.Load(r + 171688);
    s4.Load(r + 171712);
    s5.Load(r + 171744);
    tt6.Load(r + 171772);
    s7.Load(r + 188188);
    ap8.Load(r + 188212);
    ap9.Load(r + 190284);
    s10.Load(r + 192356);
    s11.Load(r + 192380);
    s12.Load(r + 192412);
    tt13.Load(r + 192440);
    s14.Load(r + 208856);
    ap15.Load(r + 208880);
    apc16.Load(r + 217096);
    ap17.Load(r + 225316);
    ap18.Load(r + 233532);
    tt19.Load(r + 237652);
    s20.Load(r + 254068);
    ap21.Load(r + 254096);
    apc22.Load(r + 262312);
    ap23.Load(r + 270532);
    ap25.Load(r + 278748);
    tt26.Load(r + 282868);
    s27.Load(r + 299284);
  }
  void Store(u8* r) const {
    ap1.Store(r + 167544);
    ap2.Store(r + 169616);
    s3.Store(r + 171688);
    s4.Store(r + 171712);
    s5.Store(r + 171744);
    tt6.Store(r + 171772);
    s7.Store(r + 188188);
    ap8.Store(r + 188212);
    ap9.Store(r + 190284);
    s10.Store(r + 192356);
    s11.Store(r + 192380);
    s12.Store(r + 192412);
    tt13.Store(r + 192440);
    s14.Store(r + 208856);
    ap15.Store(r + 208880);
    apc16.Store(r + 217096);
    ap17.Store(r + 225316);
    ap18.Store(r + 233532);
    tt19.Store(r + 237652);
    s20.Store(r + 254068);
    ap21.Store(r + 254096);
    apc22.Store(r + 262312);
    ap23.Store(r + 270532);
    ap25.Store(r + 278748);
    tt26.Store(r + 282868);
    s27.Store(r + 299284);
  }
};

// The late network as the guest runs it (0x82651398): one sample at a time. Every section reads
// the other sections' outputs of the previous sample (the guest loads all of them at the top of the
// iteration), so each sample computes the new outputs from the old ones and commits them at the
// end. Operand order follows the guest's fmadds/fmuls (which product is fused changes from section
// to section).
void LatePerSample(LateState& state, Scratch& s, float tap_in, float late_out) {
  LateState st = state;  // a local copy the compiler can keep in registers
  auto& [ap1, ap2, ap8, ap9, ap15, ap17, ap21, ap23, ap18, ap25, apc16, apc22, tt6, tt13, tt19, tt26, s3, s7,
         s10, s14, s4, s11, s5, s12, s20, s27] = st;
  float* const al6p = s.al6 + 2;  // twotap out2 sits 8 bytes into its 128-aligned buffer
  float* const ar6p = s.ar6 + 2;
  for (u32 j = 0; j < 256; ++j) {
    const float in_al5 = s.al5[j], in_ar5 = s.ar5[j], in_al6 = al6p[j], in_ar6 = ar6p[j];
    const float l3 = s.l_t2[j], r3 = s.r_t2[j];

    // Left: allpass 512 x2 -> section -> sections -> 4096 two-tap -> section.
    const float t1 = ap1.Read();
    const float v1 = (Fmadds(t1, ap1.a, in_al5) + s27.y) + s20.y;
    ap1.Write(v1);
    const float n1 = Fmadds(v1, ap1.b, t1);

    const float t2 = ap2.Read();
    const float v2 = Fmadds(t2, ap2.a, ap1.y);
    ap2.Write(v2);
    const float n2 = Fmadds(ap2.b, v2, t2);

    const float x3 = l3 + ap2.y;
    const float n3 = Fmadds(s3.h.Tap(), s3.c, x3);

    const float h4 = s4.h.Tap();
    const float v4 = Fmadds(h4, s4.c2, s3.y);
    const float n4 = Fmadds(s4.c1, v4, h4 * s4.c3);

    const float v5 = Fmadds(s5.h.Tap(), s5.c0, s4.y);
    const float n5 = v5 * s5.c;

    const float n6a = tt6.Read0() * tt6.c0;
    const float n6b = tt6.c1 * tt6.Read1();
    tt6.Write(s5.y * tap_in);

    const float n7 = s7.h.Tap() * s7.c;

    // Right: the same chain.
    const float t8 = ap8.Read();
    const float v8 = Fmadds(t8, ap8.a, s27.y - s20.y) + in_ar5;
    ap8.Write(v8);
    const float n8 = Fmadds(ap8.b, v8, t8);

    const float t9 = ap9.Read();
    const float v9 = Fmadds(t9, ap9.a, ap8.y);
    ap9.Write(v9);
    const float n9 = Fmadds(ap9.b, v9, t9);

    const float x10 = r3 + ap9.y;
    const float n10 = Fmadds(s10.h.Tap(), s10.c, x10);

    const float h11 = s11.h.Tap();
    const float v11 = Fmadds(h11, s11.c2, s10.y);
    const float n11 = Fmadds(s11.c3, h11, s11.c1 * v11);

    const float v12 = Fmadds(s12.h.Tap(), s12.c0, s11.y);
    const float n12 = s12.c * v12;

    const float r13b = tt13.Read1();
    const float r13a = tt13.Read0();
    const float n13b = r13b * tt13.c1;
    const float n13a = r13a * tt13.c0;
    tt13.Write(s12.y * tap_in);

    const float n14 = s14.h.Tap() * s14.c;

    // Cross-coupled 2048/1024 allpasses.
    const float t15 = ap15.Read();
    const float v15 = (Fmadds(ap15.a, t15, in_ar6) + s14.y) + s7.y;
    ap15.Write(v15);
    const float n15 = Fmadds(ap15.b, v15, t15);

    const float x16 = in_al6 + ap15.y;
    const float t16 = apc16.Read();
    apc16.Write(Fmadds(apc16.a, t16, x16));
    const float n16 = Fmadds(x16, apc16.b, t16 * apc16.c);

    const float t17 = ap17.Read();
    const float v17 = Fmadds(t17, ap17.a, apc16.y);
    ap17.Write(v17);
    const float n17 = Fmadds(v17, ap17.b, t17);

    const float t18 = ap18.Read();
    const float v18 = Fmadds(ap18.a, t18, in_al6) + ap17.y;
    ap18.Write(v18);
    const float n18 = Fmadds(v18, ap18.b, t18);

    const float n19b = tt19.Read1() * tt19.c1;
    const float n19a = tt19.Read0() * tt19.c0;
    tt19.Write(l3 + ap18.y);

    const float n20 = Fmadds(s20.h.Tap(), s20.c0, tt19.y0 * s20.c);

    const float t21 = ap21.Read();
    const float v21 = Fmadds(t21, ap21.a, s14.y - s7.y) + in_al6;
    ap21.Write(v21);
    const float n21 = Fmadds(v21, ap21.b, t21);

    const float x22 = in_ar6 + ap21.y;
    const float t22 = apc22.Read();
    apc22.Write(Fmadds(t22, apc22.a, x22));
    const float n22 = Fmadds(x22, apc22.b, apc22.c * t22);

    const float t23 = ap23.Read();
    const float v23 = Fmadds(t23, ap23.a, apc22.y);
    ap23.Write(v23);
    const float n23 = Fmadds(ap23.b, v23, t23);

    // Back into the stack buffers for the output stage.
    s.ar5[j] = ((s.rmix[j] + tt13.y1) + tt19.y1) * late_out;
    ar6p[j] = tt13.y1;
    al6p[j] = tt6.y1;
    s.al5[j] = ((s.lmix[j] + tt6.y1) + tt26.y1) * late_out;

    const float t25 = ap25.Read();
    const float v25 = Fmadds(t25, ap25.a, in_ar6) + ap23.y;
    ap25.Write(v25);
    const float n25 = Fmadds(ap25.b, v25, t25);

    const float r26b = tt26.Read1();
    const float r26a = tt26.Read0();
    const float n26a = r26a * tt26.c0;
    const float n26b = tt26.c1 * r26b;
    tt26.Write(r3 + ap25.y);

    const float n27 = Fmadds(s27.h.Tap(), s27.c0, s27.c * tt26.y0);

    // Histories and outputs move to this sample.
    s3.h.Push(x3);
    s4.h.Push(v4);
    s5.h.Push(v5);
    s7.h.Push(tt6.y0);
    s10.h.Push(x10);
    s11.h.Push(v11);
    s12.h.Push(v12);
    s14.h.Push(tt13.y0);
    s20.h.Push(tt19.y0);
    s27.h.Push(tt26.y0);
    ap1.y = n1;
    ap2.y = n2;
    s3.y = n3;
    s4.y = n4;
    s5.y = n5;
    tt6.y0 = n6a;
    tt6.y1 = n6b;
    s7.y = n7;
    ap8.y = n8;
    ap9.y = n9;
    s10.y = n10;
    s11.y = n11;
    s12.y = n12;
    tt13.y0 = n13a;
    tt13.y1 = n13b;
    s14.y = n14;
    ap15.y = n15;
    apc16.y = n16;
    ap17.y = n17;
    ap18.y = n18;
    tt19.y0 = n19a;
    tt19.y1 = n19b;
    s20.y = n20;
    ap21.y = n21;
    apc22.y = n22;
    ap23.y = n23;
    ap25.y = n25;
    tt26.y0 = n26a;
    tt26.y1 = n26b;
    s27.y = n27;
  }
  state = st;
}

// The same network a section at a time. Every feedback path goes through one of the four 4096
// two-tap lines (s5 -> tt6 -> s7, s12 -> tt13 -> s14, ap18 -> tt19 -> s20, ap25 -> tt26 -> s27),
// and a section only sees the others' previous-sample outputs. When no tap of those lines reads a
// sample written in this block (delay mod 4096 is 0 or >= 256), the tap outputs of all 256 samples
// are known up front and every other section can run over the whole block in dependency order,
// with only its own state live: same values, no spills, sequential line accesses.
// Y[k][j] = what section k shows the others at sample j: Y[k][0] is its stored output and
// Y[k][j + 1] its output of sample j.
bool LateBlockOk(const LateState& st) {
  const auto ok = [](u32 d) {
    d &= LineTwoTap::M;
    return d == 0 || d >= 256;
  };
  for (const LineTwoTap* t : {&st.tt6, &st.tt13, &st.tt19, &st.tt26})
    if (!ok(t->d0) || !ok(t->d1)) return false;
  return true;
}

// Taps of all 256 samples, before any write of this block (LateBlockOk).
void TapBlock(LineTwoTap& t, float* y0, float* y1) {
  constexpr u32 M = LineTwoTap::M;
  u8* const line = t.line;
  const u32 r0 = t.w - t.d0, r1 = t.w - t.d1;
  const F4 c0 = Splat(t.c0), c1 = Splat(t.c1);
  y0[0] = t.y0;
  y1[0] = t.y1;
  for (u32 j = 0; j < 256; j += 4) {
    Put(y0 + j + 1, LineLd4<M>(line, r0 + j) * c0);
    Put(y1 + j + 1, LineLd4<M>(line, r1 + j) * c1);
  }
  t.y0 = y0[256];
  t.y1 = y1[256];
}

// Writes of the block into a two-tap line (value(j) is generic: float or F4 for samples j..j+3).
template <class F>
void TapWriteBlock(LineTwoTap& t, F value) {
  constexpr u32 M = LineTwoTap::M;
  u8* const line = t.line;
  const u32 w = t.w;
  for (u32 j = 0; j < 256; j += 4) LineSt4<M>(line, w + j, value.template operator()<F4>(j));
  t.w = (w + 256) & M;
}

// One allpass line over the block: v = in(j, t, a) goes into the line, y = out(v, t, b). The
// lambdas are generic (float, or F4 for four samples) so both paths share one formula.
template <class Line, class In, class Out>
void AllpassBlock(Line& l, float* y, In in, Out out) {
  constexpr u32 M = Line::M;
  u8* const line = l.line;
  const u32 d = l.d;
  const float a = l.a, b = l.b;
  u32 w = l.w;
  y[0] = l.y;
  if (GroupSafe<M>(d)) {
    const F4 a4 = Splat(a), b4 = Splat(b);
    for (u32 j = 0; j < 256; j += 4) {
      const F4 t = LineLd4<M>(line, w + j - d);
      const F4 v = in(j, t, a4);
      LineSt4<M>(line, w + j, v);
      Put(y + j + 1, out(v, t, b4));
    }
    w = (w + 256) & M;
  } else {
    for (u32 j = 0; j < 256; ++j) {
      const float t = LdF(line + 4 * ((w - d) & M));
      const float v = in(j, t, a);
      StF(line + 4 * w, v);
      w = (w + 1) & M;
      y[j + 1] = out(v, t, b);
    }
  }
  l.w = w;
  l.y = y[256];
}

// The feed-forward variant (apc16/apc22): x = in(j), v = mix(t, a, x), y = out(x, t, b, c).
template <class In, class Mix, class Out>
void AllpassCBlock(LineAllpassC& l, float* y, In in, Mix mix, Out out) {
  constexpr u32 M = LineAllpassC::M;
  u8* const line = l.line;
  const u32 d = l.d;
  const float a = l.a, b = l.b, c = l.c;
  u32 w = l.w;
  y[0] = l.y;
  if (GroupSafe<M>(d)) {
    const F4 a4 = Splat(a), b4 = Splat(b), c4 = Splat(c);
    for (u32 j = 0; j < 256; j += 4) {
      const F4 x = in.template operator()<F4>(j);
      const F4 t = LineLd4<M>(line, w + j - d);
      LineSt4<M>(line, w + j, mix(t, a4, x));
      Put(y + j + 1, out(x, t, b4, c4));
    }
    w = (w + 256) & M;
  } else {
    for (u32 j = 0; j < 256; ++j) {
      const float x = in.template operator()<float>(j);
      const float t = LdF(line + 4 * ((w - d) & M));
      StF(line + 4 * w, mix(t, a, x));
      w = (w + 1) & M;
      y[j + 1] = out(x, t, b, c);
    }
  }
  l.w = w;
  l.y = y[256];
}

// History sections fed by a known sequence x (pushes x[j]): y = out(tap, x[j]), where the tap is
// x two or one samples back. hx = {h1, h0, x[0..255]} makes the tap of sample j hx[j] or hx[j + 1].
template <class Sec, class Out>
void HistFirBlock(Sec& sec, const float* x, float* y, Out out) {
  alignas(16) float hx[260];
  hx[0] = sec.h.h1;
  hx[1] = sec.h.h0;
  std::memcpy(hx + 2, x, 256 * sizeof(float));
  const float* const tap = sec.h.second ? hx : hx + 1;
  y[0] = sec.y;
  for (u32 j = 0; j < 256; j += 4) Put(y + j + 1, out(At<F4>(tap + j), At<F4>(x + j)));
  sec.h.h0 = x[255];
  sec.h.h1 = x[254];
  sec.y = y[256];
}

enum LateRow {
  kY1, kY2, kY3, kY4, kY5, kY6a, kY6b, kY7, kY8, kY9, kY10, kY11, kY12, kY13a, kY13b,
  kY14, kY15, kY16, kY17, kY18, kY19a, kY19b, kY20, kY21, kY22, kY23, kY25, kY26a, kY26b, kY27,
  kX3, kX10,  // s3 / s10 inputs
  kLateRows
};
static_assert(kLateRows == std::extent_v<decltype(Scratch::late)>);

void LateBlock(LateState& st, Scratch& s, float tap_in, float late_out) {
  float(&Y)[kLateRows][260] = s.late;
  const float* const al5 = s.al5;
  const float* const ar5 = s.ar5;
  const float* const al6p = s.al6 + 2;
  const float* const ar6p = s.ar6 + 2;
  const float* const l3 = s.l_t2;
  const float* const r3 = s.r_t2;

  // Tap outputs, then the history sections they feed.
  TapBlock(st.tt6, Y[kY6a], Y[kY6b]);
  TapBlock(st.tt13, Y[kY13a], Y[kY13b]);
  TapBlock(st.tt19, Y[kY19a], Y[kY19b]);
  TapBlock(st.tt26, Y[kY26a], Y[kY26b]);
  {
    const F4 c7 = Splat(st.s7.c), c14 = Splat(st.s14.c);
    HistFirBlock(st.s7, Y[kY6a], Y[kY7], [c7](F4 h, F4) { return h * c7; });
    HistFirBlock(st.s14, Y[kY13a], Y[kY14], [c14](F4 h, F4) { return h * c14; });
    const F4 c20 = Splat(st.s20.c), c20b = Splat(st.s20.c0);
    HistFirBlock(st.s20, Y[kY19a], Y[kY20], [=](F4 h, F4 x) { return Fmadds(h, c20b, x * c20); });
    const F4 c27 = Splat(st.s27.c), c27b = Splat(st.s27.c0);
    HistFirBlock(st.s27, Y[kY26a], Y[kY27], [=](F4 h, F4 x) { return Fmadds(h, c27b, c27 * x); });
  }

  // Input allpasses (512) and the 2-sample sections in front of tt6 / tt13.
  {
    const float* y27 = Y[kY27];
    const float* y20 = Y[kY20];
    AllpassBlock(
        st.ap1, Y[kY1],
        [=](u32 j, auto t, auto a) {
          using T = decltype(t);
          return (Fmadds(t, a, At<T>(al5 + j)) + At<T>(y27 + j)) + At<T>(y20 + j);
        },
        [](auto v, auto t, auto b) { return Fmadds(v, b, t); });
    AllpassBlock(
        st.ap8, Y[kY8],
        [=](u32 j, auto t, auto a) {
          using T = decltype(t);
          return Fmadds(t, a, At<T>(y27 + j) - At<T>(y20 + j)) + At<T>(ar5 + j);
        },
        [](auto v, auto t, auto b) { return Fmadds(b, v, t); });
  }
  {
    const float* y1 = Y[kY1];
    const float* y8 = Y[kY8];
    AllpassBlock(
        st.ap2, Y[kY2], [=](u32 j, auto t, auto a) { return Fmadds(t, a, At<decltype(t)>(y1 + j)); },
        [](auto v, auto t, auto b) { return Fmadds(b, v, t); });
    AllpassBlock(
        st.ap9, Y[kY9], [=](u32 j, auto t, auto a) { return Fmadds(t, a, At<decltype(t)>(y8 + j)); },
        [](auto v, auto t, auto b) { return Fmadds(b, v, t); });
  }
  {
    // s3 / s10: x = input + allpass, y = tap * c + x, history of x.
    const float* y2 = Y[kY2];
    const float* y9 = Y[kY9];
    float* x3 = Y[kX3];
    float* x10 = Y[kX10];
    for (u32 j = 0; j < 256; j += 4) {
      Put(x3 + j, At<F4>(l3 + j) + At<F4>(y2 + j));
      Put(x10 + j, At<F4>(r3 + j) + At<F4>(y9 + j));
    }
    const F4 c3 = Splat(st.s3.c), c10 = Splat(st.s10.c);
    HistFirBlock(st.s3, x3, Y[kY3], [c3](F4 h, F4 x) { return Fmadds(h, c3, x); });
    HistFirBlock(st.s10, x10, Y[kY10], [c10](F4 h, F4 x) { return Fmadds(h, c10, x); });
  }
  {
    // s4 / s11 (recursive on v) and s5 / s12 (recursive on v, fed by s4 / s11 one sample later):
    // four independent recurrences per step.
    const float* y3 = Y[kY3];
    const float* y10 = Y[kY10];
    float* y4 = Y[kY4];
    float* y11 = Y[kY11];
    float* y5 = Y[kY5];
    float* y12 = Y[kY12];
    SecB p = st.s4, q = st.s11;
    SecC u = st.s5, v = st.s12;
    y4[0] = p.y;
    y11[0] = q.y;
    y5[0] = u.y;
    y12[0] = v.y;
    for (u32 j = 0; j < 256; ++j) {
      const float hp = p.h.Tap();
      const float vp = Fmadds(hp, p.c2, y3[j]);
      y4[j + 1] = Fmadds(p.c1, vp, hp * p.c3);
      const float hq = q.h.Tap();
      const float vq = Fmadds(hq, q.c2, y10[j]);
      y11[j + 1] = Fmadds(q.c3, hq, q.c1 * vq);
      const float vu = Fmadds(u.h.Tap(), u.c0, y4[j]);
      y5[j + 1] = vu * u.c;
      const float vv = Fmadds(v.h.Tap(), v.c0, y11[j]);
      y12[j + 1] = v.c * vv;
      p.h.Push(vp);
      q.h.Push(vq);
      u.h.Push(vu);
      v.h.Push(vv);
    }
    p.y = y4[256];
    q.y = y11[256];
    u.y = y5[256];
    v.y = y12[256];
    st.s4 = p;
    st.s11 = q;
    st.s5 = u;
    st.s12 = v;
  }
  {
    const float* y5 = Y[kY5];
    const float* y12 = Y[kY12];
    const F4 g = Splat(tap_in);
    TapWriteBlock(st.tt6, [=]<class T>(u32 j) { return At<T>(y5 + j) * g; });
    TapWriteBlock(st.tt13, [=]<class T>(u32 j) { return At<T>(y12 + j) * g; });
  }

  // Cross-coupled 2048 / 1024 allpasses, into tt19 / tt26.
  {
    const float* y14 = Y[kY14];
    const float* y7 = Y[kY7];
    AllpassBlock(
        st.ap15, Y[kY15],
        [=](u32 j, auto t, auto a) {
          using T = decltype(t);
          return (Fmadds(a, t, At<T>(ar6p + j)) + At<T>(y14 + j)) + At<T>(y7 + j);
        },
        [](auto v, auto t, auto b) { return Fmadds(b, v, t); });
    AllpassBlock(
        st.ap21, Y[kY21],
        [=](u32 j, auto t, auto a) {
          using T = decltype(t);
          return Fmadds(t, a, At<T>(y14 + j) - At<T>(y7 + j)) + At<T>(al6p + j);
        },
        [](auto v, auto t, auto b) { return Fmadds(v, b, t); });
  }
  {
    const float* y15 = Y[kY15];
    const float* y21 = Y[kY21];
    AllpassCBlock(
        st.apc16, Y[kY16], [=]<class T>(u32 j) { return At<T>(al6p + j) + At<T>(y15 + j); },
        [](auto t, auto a, auto x) { return Fmadds(a, t, x); },
        [](auto x, auto t, auto b, auto c) { return Fmadds(x, b, t * c); });
    AllpassCBlock(
        st.apc22, Y[kY22], [=]<class T>(u32 j) { return At<T>(ar6p + j) + At<T>(y21 + j); },
        [](auto t, auto a, auto x) { return Fmadds(t, a, x); },
        [](auto x, auto t, auto b, auto c) { return Fmadds(x, b, c * t); });
  }
  {
    const float* y16 = Y[kY16];
    const float* y22 = Y[kY22];
    AllpassBlock(
        st.ap17, Y[kY17], [=](u32 j, auto t, auto a) { return Fmadds(t, a, At<decltype(t)>(y16 + j)); },
        [](auto v, auto t, auto b) { return Fmadds(v, b, t); });
    AllpassBlock(
        st.ap23, Y[kY23], [=](u32 j, auto t, auto a) { return Fmadds(t, a, At<decltype(t)>(y22 + j)); },
        [](auto v, auto t, auto b) { return Fmadds(b, v, t); });
  }
  {
    const float* y17 = Y[kY17];
    const float* y23 = Y[kY23];
    AllpassBlock(
        st.ap18, Y[kY18],
        [=](u32 j, auto t, auto a) {
          using T = decltype(t);
          return Fmadds(a, t, At<T>(al6p + j)) + At<T>(y17 + j);
        },
        [](auto v, auto t, auto b) { return Fmadds(v, b, t); });
    AllpassBlock(
        st.ap25, Y[kY25],
        [=](u32 j, auto t, auto a) {
          using T = decltype(t);
          return Fmadds(t, a, At<T>(ar6p + j)) + At<T>(y23 + j);
        },
        [](auto v, auto t, auto b) { return Fmadds(b, v, t); });
  }
  {
    const float* y18 = Y[kY18];
    const float* y25 = Y[kY25];
    TapWriteBlock(st.tt19, [=]<class T>(u32 j) { return At<T>(l3 + j) + At<T>(y18 + j); });
    TapWriteBlock(st.tt26, [=]<class T>(u32 j) { return At<T>(r3 + j) + At<T>(y25 + j); });
  }

  // Back into the stack buffers (after every read of them above).
  {
    const float* y6b = Y[kY6b];
    const float* y13b = Y[kY13b];
    const float* y19b = Y[kY19b];
    const float* y26b = Y[kY26b];
    const F4 g = Splat(late_out);
    for (u32 j = 0; j < 256; j += 4) {
      Put(s.ar5 + j, ((At<F4>(s.rmix + j) + At<F4>(y13b + j)) + At<F4>(y19b + j)) * g);
      Put(s.ar6 + 2 + j, At<F4>(y13b + j));
      Put(s.al6 + 2 + j, At<F4>(y6b + j));
      Put(s.al5 + j, ((At<F4>(s.lmix + j) + At<F4>(y6b + j)) + At<F4>(y26b + j)) * g);
    }
  }
}

void LateNetwork(u8* r, Scratch& s, float tap_in, float late_out) {
  LateState st;
  st.Load(r);
  if (LateBlockOk(st))
    LateBlock(st, s, tap_in, late_out);
  else
    LatePerSample(st, s, tap_in, late_out);
  st.Store(r);
}

//-----------------------------------------------------------------------------
// Call plan and guards
//-----------------------------------------------------------------------------

struct Plan {
  u32 room, in_l, in_r, sp;
  u32 out[4];  // channel 0, 1, 3, 4 (front L/R, surround L/R); 2 and 5 are not written
};

bool WriteIndexOk(const u8* r, u32 index_off, u32 len, u32 frame) { return Ld32(r + index_off) + frame <= len; }

// Every delay-line write of this call stays inside its line (the original writes linearly from the
// write index and wraps it afterwards).
bool StateOk(const u8* r) {
  for (u32 vd : {104u, 65672u}) {
    // Vector copy path writes 260 floats from w when w + 256 < 16384: w in 16125..16127 would
    // spill into the next field. w == 16128 can only take the 256-float paths.
    const u32 w = Ld32(r + vd + 24);
    if (!(w <= 16124 || w == 16128)) return false;
  }
  for (u32 o : {131312u, 149432u})
    if (!WriteIndexOk(r, o + 36, 512, 256)) return false;
  for (u32 o : {133404u, 151524u})
    if (Ld32(r + o + 16) != 0) return false;
  for (u32 o : {133940u, 152060u})
    if (!WriteIndexOk(r, o + 36, 2048, 256)) return false;
  for (u32 o : {142176u, 160296u})
    if (Ld32(r + o + 16) != 0) return false;
  for (u32 o : {143224u, 161344u, 299312u, 301384u, 303456u, 305528u})
    if (!WriteIndexOk(r, o + 16, 512, 256)) return false;
  for (u32 o : {145296u, 163416u})
    if (!WriteIndexOk(r, o + 24, 1024, 256)) return false;
  for (u32 o : {307600u, 308656u})
    if (Ld32(r + o + 24) != 0) return false;
  // Late network: write index in range (masked after each write).
  for (u32 o : {167544u, 169616u, 188212u, 190284u})
    if (!WriteIndexOk(r, o + 16, 512, 1)) return false;
  for (u32 o : {208880u, 225316u, 254096u, 270532u})
    if (!WriteIndexOk(r, o + 16, 2048, 1)) return false;
  for (u32 o : {217096u, 262312u})
    if (!WriteIndexOk(r, o + 20, 2048, 1)) return false;
  for (u32 o : {233532u, 278748u})
    if (!WriteIndexOk(r, o + 16, 1024, 1)) return false;
  for (u32 o : {171772u, 192440u, 237652u, 282868u})
    if (!WriteIndexOk(r, o + 24, 4096, 1)) return false;
  return true;
}

bool Prepare(PPCContext& ctx, u8* base, Plan& p) {
  p.room = ctx.r3.u32;
  p.sp = ctx.r1.u32;
  const u32 in_vec = ctx.r4.u32, out_vec = ctx.r5.u32;
  if (!RangeOk(p.room, kRoomSize) || !RangeOk(in_vec, 8) || !RangeOk(out_vec, 20) ||
      !RangeOk(p.sp + 20, 4))
    return false;
  const u8* r = Host(base, p.room);
  const int32_t mode = int32_t(Ld32(r + 100));
  const u32 in0 = Ld32(Host(base, in_vec)), in1 = Ld32(Host(base, in_vec + 4));
  p.in_l = (mode == 2) ? in1 : in0;
  p.in_r = (mode == 1) ? in0 : in1;
  const u8* ov = Host(base, out_vec);
  p.out[0] = Ld32(ov + 0);
  p.out[1] = Ld32(ov + 4);
  p.out[2] = Ld32(ov + 12);
  p.out[3] = Ld32(ov + 16);
  for (u32 in : {p.in_l, p.in_r}) {
    const u32 lo = in & ~15u;
    const u32 len = (in - lo) + kInputSpan;
    if (!RangeOk(lo, len) || Overlap(lo, len, p.room, kRoomSize)) return false;
  }
  for (u32 out : p.out)
    if (!RangeOk(out & ~15u, 1024)) return false;
  if (Overlap(p.sp + 20, 4, p.room, kRoomSize)) return false;
  return StateOk(r);
}

// One per thread that renders the reverb (the XAudio thread), never freed: a plain pointer keeps the
// TLS slot trivially destructible.
Scratch& ThreadScratch() {
  thread_local Scratch* s = nullptr;
  if (!s) s = new Scratch;
  return *s;
}

inline void LoadInput(float* dst, const u8* src) {
  for (u32 i = 0; i < kInputSpan / 4; ++i) dst[i] = LdF(src + 4 * i);
}

void Execute(PPCContext& ctx, u8* base, const Plan& p) {
  Scratch& s = ThreadScratch();
  u8* const r = Host(base, p.room);

  // stw r30,17988(r1): the room pointer spilled to the caller's home slot.
  St32(Host(base, p.sp + 20), p.room);

  ctx.fpscr.disableFlushMode();
  Barrier();

  Consts k;
  k.one = LdF(Host(base, kAddrOne));
  k.step = LdF(Host(base, kAddrStep));
  k.idle = LdF(Host(base, kAddrIdle));
  k.fade_scale = LdF(Host(base, kAddrFadeScale));
  k.copy_step = LdF(Host(base, kAddrCopyStep));
  k.late_out = LdF(Host(base, kAddrLateOut));
  k.tap_in = LdF(Host(base, kAddrTapIn));

  // Input chains. The guest runs the left chain, then the right one; their states and lines are
  // disjoint, so running them side by side (both occlusion recurrences in one loop) is the same.
  LoadInput(s.in_l, Host(base, p.in_l));
  LoadInput(s.in_r, Host(base, p.in_r));
  VarDelay<16384, true>(r + 104, s.in_l, s.vd_l, k);
  VarDelay<16384, true>(r + 65672, s.in_r, s.vd_r, k);
  Occlusion2(r + 131240, s.vd_l, s.oc_l, r + 131272, s.vd_r, s.oc_r, k);
  ThreeTap<512>(r + 131312, s.oc_l, s.l_tt1, s.l_tt2);
  ThreeTap<512>(r + 149432, s.oc_r, s.r_tt1, s.r_tt2);
  AllpassInPlace<128>(r + 133404, s.l_tt1);
  AllpassInPlace<128>(r + 151524, s.r_tt1);
  ThreeTap<2048>(r + 133940, s.l_tt1, s.l_t1, s.l_t2);
  ThreeTap<2048>(r + 152060, s.r_tt1, s.r_t1, s.r_t2);
  AllpassInPlace<256>(r + 142176, s.l_t1);
  AllpassInPlace<256>(r + 160296, s.r_t1);
  Allpass512Out(r + 143224, s.l_t1, s.l_ap);
  Allpass512Out(r + 161344, s.r_t1, s.r_ap);
  TwoTap1024(r + 145296, s.l_ap, s.al5, s.al6 + 2);
  TwoTap1024(r + 163416, s.r_ap, s.ar5, s.ar6 + 2);

  // Cross-mix of the two input chains into the late network (0x82650D4C). Position 1 of the guest's
  // 8x unrolled left mix fuses differently from the other seven.
  {
    const float g = LdF(r + 96);
    const float g_l0 = LdF(r + 131304), g_t1 = LdF(r + 131308);
    const float g_r0 = LdF(r + 149424), g_r1 = LdF(r + 149428);
    const F4 gv = Splat(g), l0v = Splat(g_l0), t1v = Splat(g_t1), r0v = Splat(g_r0), r1v = Splat(g_r1);
    for (u32 j = 0; j < 256; j += 4) {
      F4 l = (Fmadds(At<F4>(s.r_tt1 + j), r0v, At<F4>(s.l_t1 + j) * t1v) + At<F4>(s.l_tt2 + j)) * gv;
      if ((j & 7) == 0) l[1] = Fmadds(g_t1, s.l_t1[j + 1], Fmadds(s.r_tt1[j + 1], g_r0, s.l_tt2[j + 1])) * g;
      Put(s.lmix + j, l);
      Put(s.rmix + j, (Fmadds(At<F4>(s.l_tt1 + j), l0v, At<F4>(s.r_t1 + j) * r1v) + At<F4>(s.r_tt2 + j)) * gv);
    }
  }

  LateNetwork(r, s, k.tap_in, k.late_out);

  AllpassInPlace<512>(r + 299312, s.al5);
  AllpassInPlace<512>(r + 301384, s.al5);
  AllpassInPlace<512>(r + 303456, s.ar5);
  AllpassInPlace<512>(r + 305528, s.ar5);
  VarDelay<256, false>(r + 307600, s.al6 + 2, s.al6, k);
  VarDelay<256, false>(r + 308656, s.ar6 + 2, s.ar6, k);

  // Output mix (0x82651F9C), vector ops with flush-to-zero like the original's vmulfp128/vmaddfp:
  // front = (dry - wet) * in + wet * late, surround = 2 * wet * tail. vmaddfp is emitted as
  // add(mul()); whether the compiler fused that multiply is decided by the build flags (MixFuses).
  Barrier();
  ctx.fpscr.enableFlushMode();
  Barrier();
  {
    // lvlx + vspltw 0: the word at room+92, bytes past its 16-byte block read as zero.
    const u32 ea = p.room + 92;
    u32 wet_bits = 0;
    for (u32 i = 0; i < 4; ++i)
      if ((ea & 15) + i < 16) wet_bits |= u32(*Host(base, ea + i)) << (24 - 8 * i);
    const F4 wet = Splat(std::bit_cast<float>(wet_bits));
    const F4 dry = LdF4(Host(base, kAddrDry)) - wet;
    const F4 wet2 = wet + wet;
    const bool fused = MixFuses();
    const auto front = [&](F4 in, F4 late) {
      const F4 wl = wet * late;
      return fused ? FusedMulAdd(dry, in, wl) : Opaque(dry * in) + Opaque(wl);
    };
    const u32 in_l = p.in_l & ~15u, in_r = p.in_r & ~15u;
    const u32 o0 = p.out[0] & ~15u, o1 = p.out[1] & ~15u, o3 = p.out[2] & ~15u, o4 = p.out[3] & ~15u;
    for (u32 i = 0; i < 32; ++i) {
      // Per 8 samples: every load first, then the stores (out3, out4, out0, out1), as the guest
      // loop does, so outputs that overlap the inputs behave the same.
      const u32 b = 32 * i;
      const F4 l5a = At<F4>(s.al5 + 8 * i), l5b = At<F4>(s.al5 + 8 * i + 4);
      const F4 r5a = At<F4>(s.ar5 + 8 * i), r5b = At<F4>(s.ar5 + 8 * i + 4);
      const F4 l6a = At<F4>(s.al6 + 8 * i), l6b = At<F4>(s.al6 + 8 * i + 4);
      const F4 r6a = At<F4>(s.ar6 + 8 * i), r6b = At<F4>(s.ar6 + 8 * i + 4);
      const F4 ila = LdF4(Host(base, in_l + b)), ilb = LdF4(Host(base, in_l + b + 16));
      const F4 ira = LdF4(Host(base, in_r + b)), irb = LdF4(Host(base, in_r + b + 16));
      StF4(Host(base, o3 + b), wet2 * l6a);
      StF4(Host(base, o3 + b + 16), wet2 * l6b);
      StF4(Host(base, o4 + b), wet2 * r6a);
      StF4(Host(base, o4 + b + 16), wet2 * r6b);
      StF4(Host(base, o0 + b), front(ila, l5a));
      StF4(Host(base, o0 + b + 16), front(ilb, l5b));
      StF4(Host(base, o1 + b), front(ira, r5a));
      StF4(Host(base, o1 + b + 16), front(irb, r5b));
    }
  }
#if !BANJO_FP_MOVES_MODELESS
  // The epilogue's __restfpr_14 (lfd) switches flush-to-zero off again.
  Barrier();
  ctx.fpscr.disableFlushMode();
#else
  // Built with the codegen option fp_moves_modeless: the epilogue's lfd switches nothing, the csr stays as the
  // output mix left it (flush-to-zero on), as the generated function leaves it.
#endif
}

//-----------------------------------------------------------------------------
// Verification
//-----------------------------------------------------------------------------

struct VerifyStats {
  std::atomic<uint64_t> calls{0}, mismatches{0}, fallbacks{0}, reports{0};
};
VerifyStats g_stats;

struct Range {
  u32 addr, len;
};

void Verify(PPCContext& ctx, u8* base) {
  Plan p;
  const uint64_t call = g_stats.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!Prepare(ctx, base, p)) {
    g_stats.fallbacks.fetch_add(1, std::memory_order_relaxed);
    __imp__rex_princeton_digital__stereo_room_t_float___process(ctx, base);
  } else {
    const Range ranges[] = {{p.room, kRoomSize},      {p.out[0] & ~15u, 1024}, {p.out[1] & ~15u, 1024},
                            {p.out[2] & ~15u, 1024}, {p.out[3] & ~15u, 1024}, {p.sp + 20, 4}};
    size_t total = 0;
    for (const Range& rg : ranges) total += rg.len;
    thread_local u8* buffers = nullptr;  // before | native, sized for the fixed range list
    thread_local PPCContext* nctx = nullptr;
    if (!buffers) {
      buffers = new u8[2 * total];
      nctx = new PPCContext;
    }
    u8* const before = buffers;
    u8* const native = buffers + total;
    size_t at = 0;
    for (const Range& rg : ranges) {
      std::memcpy(before + at, Host(base, rg.addr), rg.len);
      at += rg.len;
    }

    // Native on a copy of the context; the host csr is put back for the original.
    *nctx = ctx;
    const u32 host_csr = ctx.fpscr.getcsr();
    Execute(*nctx, base, p);
    ctx.fpscr.setcsr(host_csr);
    at = 0;
    for (const Range& rg : ranges) {
      std::memcpy(native + at, Host(base, rg.addr), rg.len);
      at += rg.len;
    }
    // Put the snapshot back for the original (overlapping ranges hold the same bytes in both copies).
    at = total;
    for (size_t i = std::size(ranges); i-- > 0;) {
      at -= ranges[i].len;
      std::memcpy(Host(base, ranges[i].addr), before + at, ranges[i].len);
    }

    __imp__rex_princeton_digital__stereo_room_t_float___process(ctx, base);

    static const char* const kNames[] = {"room", "out0", "out1", "out3", "out4", "home slot"};
    bool bad = nctx->fpscr.csr != ctx.fpscr.csr;
    ::me::native::g_verify_native_checks.fetch_add(1, std::memory_order_relaxed);  // the "[verify]" line
    int shown = 0;
    const bool report = g_stats.reports.load(std::memory_order_relaxed) < 20;
    at = 0;
    for (size_t i = 0; i < std::size(ranges); ++i) {
      const u8* orig = Host(base, ranges[i].addr);
      const u8* mine = native + at;
      if (std::memcmp(orig, mine, ranges[i].len) != 0) {
        bad = true;
        for (u32 off = 0; off < ranges[i].len && report && shown < 6; off += 4) {
          const u32 len = std::min<u32>(4, ranges[i].len - off);
          if (std::memcmp(orig + off, mine + off, len) == 0) continue;
          u32 a = 0, b = 0;
          std::memcpy(&a, orig + off, len);
          std::memcpy(&b, mine + off, len);
          REXLOG_WARN("[banjo_native_reverb] call {} {} +{} ({:08X}): original {:08X} native {:08X}", call,
                      kNames[i], off, ranges[i].addr + off, __builtin_bswap32(a), __builtin_bswap32(b));
          ++shown;
        }
      }
      at += ranges[i].len;
    }
    if (bad) {
      ::me::native::VerifyDifference();
      g_stats.mismatches.fetch_add(1, std::memory_order_relaxed);
      if (report) {
        g_stats.reports.fetch_add(1, std::memory_order_relaxed);
        if (nctx->fpscr.csr != ctx.fpscr.csr)
          REXLOG_WARN("[banjo_native_reverb] call {} fpscr csr: original {:08X} native {:08X}", call,
                      ctx.fpscr.csr, nctx->fpscr.csr);
      }
    }
  }
  if (call % 500 == 0)
    REXLOG_INFO("[banjo_native_reverb] verify: {} calls, {} mismatching, {} ran the original (guard)", call,
                g_stats.mismatches.load(std::memory_order_relaxed),
                g_stats.fallbacks.load(std::memory_order_relaxed));
}

}  // namespace
}  // namespace banjo_reverb

REX_HOOK_RAW(rex_princeton_digital__stereo_room_t_float___process) {
  // A CPU enhancement (src/cpu/banjo_enhancements.h): its own key and the CPU master switch. banjo_cpu_verify turns
  // the verify mode on (ApplyMasterSwitches).
  static const bool native =
      banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_native_reverb));
  if (native) {
    if (REXCVAR_GET(banjo_native_reverb_verify)) {
      banjo_reverb::Verify(ctx, base);
      return;
    }
    banjo_reverb::Plan p;
    if (banjo_reverb::Prepare(ctx, base, p)) {
      banjo_reverb::Execute(ctx, base, p);
      return;
    }
  }
  __imp__rex_princeton_digital__stereo_room_t_float___process(ctx, base);
}
