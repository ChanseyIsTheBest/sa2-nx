// blur-nx (2026-11-11, CPU batch 2): measurements the Cortex-A57 guide does not settle, for the next batches, on the
// console at boot when blur_microbench is on (off in the released toml; a few milliseconds when on):
// - the audio mixer's kernel (sub_822EA500's native: 1 KB, dst = s * b + a on big-endian floats, three byte swaps a
//   vector) with the 16-byte reversal it uses now (GCC: one tbl with a table register) against rev32 (the swap within
//   each word, which a lane-wise kernel can use as well);
// - an fpcr write (the flush-mode switch of the generated code: `msr fpcr`) against a loop without it;
// - zeroing 1 KB with DC ZVA against 16-byte stores, in the cache and out of it.
// Each figure: the best of 5 runs, nanoseconds per unit, logged as "[microbench] ...".
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

REXCVAR_DEFINE_BOOL(blur_microbench, false, "Blur/Diagnostics",
                    "At boot, time a few processor forms (the mixer's byte swaps, an fpcr write, DC ZVA) and log them "
                    "as [microbench] lines; a few milliseconds")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo {

#if defined(__aarch64__)
namespace {

template <class F>
double BestNs(int reps, F&& f) {
  double best = 1e30;
  for (int r = 0; r < 5; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) f();
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
    best = ns < best ? ns : best;
  }
  return best;
}

alignas(64) uint8_t g_a[1024], g_b[1024], g_d[1024];

[[gnu::noinline]] void MixReverse16(const uint8_t* a, const uint8_t* b, uint8_t* d, float32x4_t s) {
  for (int j = 0; j < 64; ++j) {
    uint8x16_t x = vld1q_u8(a + 16 * j), y = vld1q_u8(b + 16 * j);
    x = vrev64q_u8(x);
    x = vextq_u8(x, x, 8);
    y = vrev64q_u8(y);
    y = vextq_u8(y, y, 8);
    const float32x4_t r = vfmaq_f32(vreinterpretq_f32_u8(x), s, vreinterpretq_f32_u8(y));
    uint8x16_t z = vrev64q_u8(vreinterpretq_u8_f32(r));
    z = vextq_u8(z, z, 8);
    vst1q_u8(d + 16 * j, z);
  }
}

[[gnu::noinline]] void MixRev32(const uint8_t* a, const uint8_t* b, uint8_t* d, float32x4_t s) {
  for (int j = 0; j < 64; ++j) {
    const uint8x16_t x = vrev32q_u8(vld1q_u8(a + 16 * j)), y = vrev32q_u8(vld1q_u8(b + 16 * j));
    const float32x4_t r = vfmaq_f32(vreinterpretq_f32_u8(x), s, vreinterpretq_f32_u8(y));
    vst1q_u8(d + 16 * j, vrev32q_u8(vreinterpretq_u8_f32(r)));
  }
}

// The same 16-byte reversal as two simple permutes (rev64, then ext #8), in asm so GCC cannot fold them into one tbl:
// is that cheaper than the tbl the generated code's every guest vector load and store gets (REX_BSWAP128)?
[[gnu::always_inline]] inline uint8x16_t Reverse16Pair(uint8x16_t v) {
  uint8x16_t r;
  __asm__("rev64 %0.16b, %1.16b\n\text %0.16b, %0.16b, %0.16b, #8" : "=&w"(r) : "w"(v));
  return r;
}

[[gnu::noinline]] void MixRev64Ext(const uint8_t* a, const uint8_t* b, uint8_t* d, float32x4_t s) {
  for (int j = 0; j < 64; ++j) {
    const uint8x16_t x = Reverse16Pair(vld1q_u8(a + 16 * j)), y = Reverse16Pair(vld1q_u8(b + 16 * j));
    const float32x4_t r = vfmaq_f32(vreinterpretq_f32_u8(x), s, vreinterpretq_f32_u8(y));
    vst1q_u8(d + 16 * j, Reverse16Pair(vreinterpretq_u8_f32(r)));
  }
}

[[gnu::noinline]] float FpcrToggles(int n, float x) {
  uint64_t fpcr;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  const uint64_t on = fpcr | (1ull << 24), off = fpcr & ~(1ull << 24);
  for (int i = 0; i < n; ++i) {
    __asm__ volatile("msr fpcr, %0" ::"r"(on));
    x = x * 1.0001f + 0.5f;
    __asm__ volatile("msr fpcr, %0" ::"r"(off));
    x = x * 0.9999f + 0.25f;
  }
  __asm__ volatile("msr fpcr, %0" ::"r"(fpcr));
  return x;
}

[[gnu::noinline]] float NoToggles(int n, float x) {
  for (int i = 0; i < n; ++i) {
    __asm__ volatile("" ::: "memory");
    x = x * 1.0001f + 0.5f;
    __asm__ volatile("" ::: "memory");
    x = x * 0.9999f + 0.25f;
  }
  return x;
}

[[gnu::noinline]] void ZeroZva(uint8_t* p) {
  for (int k = 0; k < 1024; k += 64) __asm__ volatile("dc zva, %0" ::"r"(p + k) : "memory");
}

[[gnu::noinline]] void ZeroStp(uint8_t* p) {
  const uint8x16_t z = vdupq_n_u8(0);
  for (int k = 0; k < 1024; k += 64) {
    vst1q_u8(p + k, z);
    vst1q_u8(p + k + 16, z);
    vst1q_u8(p + k + 32, z);
    vst1q_u8(p + k + 48, z);
  }
  __asm__ volatile("" ::: "memory");
}

}  // namespace
#endif

void BlurMicrobench() {
  if (!REXCVAR_GET(blur_microbench)) return;
#if defined(__aarch64__)
  for (int i = 0; i < 1024; ++i) {
    g_a[i] = uint8_t(i * 7 + 1);
    g_b[i] = uint8_t(i * 13 + 5);
  }
  const float32x4_t s = vdupq_n_f32(0.5f);
  const double rev16 = BestNs(2000, [&] { MixReverse16(g_a, g_b, g_d, s); });
  const double rev32 = BestNs(2000, [&] { MixRev32(g_a, g_b, g_d, s); });
  const double pair = BestNs(2000, [&] { MixRev64Ext(g_a, g_b, g_d, s); });
  REXLOG_INFO("[microbench] mixer 1 KB (64 vectors, 3 swaps each): 16-byte reversal as GCC writes it (tbl) {:.1f} ns, "
              "as rev64 + ext {:.1f} ns, rev32 {:.1f} ns",
              rev16, pair, rev32);
  volatile float sink = 0;
  const double tog = BestNs(1, [&] { sink = FpcrToggles(20000, sink); }) / 20000.0;
  const double plain = BestNs(1, [&] { sink = NoToggles(20000, sink); }) / 20000.0;
  REXLOG_INFO("[microbench] two fpcr writes and two fmadd: {:.2f} ns, the fmadds alone {:.2f} ns", tog, plain);
  uint64_t dczid;
  __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
  if ((dczid & 0x1F) == 4) {
    static uint8_t* big = new uint8_t[(8u << 20) + 64];
    uint8_t* const big64 = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(big) + 63) & ~uintptr_t(63));
    const double zva_l1 = BestNs(4000, [&] { ZeroZva(g_d); });
    const double stp_l1 = BestNs(4000, [&] { ZeroStp(g_d); });
    uint32_t off = 0;
    const double zva_mem = BestNs(2048, [&] { ZeroZva(big64 + off); off = (off + 4096u) & ((8u << 20) - 1); });
    off = 0;
    const double stp_mem = BestNs(2048, [&] { ZeroStp(big64 + off); off = (off + 4096u) & ((8u << 20) - 1); });
    REXLOG_INFO("[microbench] zero 1 KB: in the cache DC ZVA {:.1f} ns, 16-byte stores {:.1f} ns; out of it (8 MB "
                "stride 4 KB) DC ZVA {:.1f} ns, 16-byte stores {:.1f} ns",
                zva_l1, stp_l1, zva_mem, stp_mem);
  }
  (void)sink;
#endif
}

}  // namespace banjo
