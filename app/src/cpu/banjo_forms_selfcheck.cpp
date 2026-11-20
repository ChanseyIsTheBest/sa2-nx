// banjo-nx: the instruction forms that only exist on one processor, checked at every start. From sa2-nx
// (app/src/cpu/sa2_forms_selfcheck.cpp, GPL-3.0, same project family), which widened banjo-nx's boot-time rounding
// check of 2026-10-12 to every such form; only the names differ.
//
// Several helpers have an AArch64 form the PC build never compiles: the conversions to integer (`frinti` + `fcvtzs`),
// the divisions (`sdiv` / `udiv`), the small VMX helpers (one NEON instruction each), vperm and the two saturating
// float-to-integer conversions. Their unit tests run the NEON expressions through a model on the PC; the processor
// itself first sees them on the console. So each start runs every one of them on a fixed set of edge inputs
// against a reference written in plain C++ (no helper, no intrinsic) and logs one line:
//     [enhancements] instruction forms self-check: N of N results as expected
// or, per failure, a DIFFERENCE line with the helper, the input and both results (and, because the released
// configuration logs nothing, the same lines in banjo_selfcheck_failed.txt next to the program). On the PC the same
// table runs the x86 forms, which proves the expected values; on the console it proves the AArch64 forms. A few
// microseconds, once.
#include "banjo_enhancements.h"

#include "../me/me_verify.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>  // blur-nx (2026-11-12): RoundSingle

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>  // blur-nx (2026-11-12): CRRegister (KnownSingle below)
#include <rex/ppc/diet_ops.h>
#include <rex/ppc/intrinsics.h>
#include <rex/ppc/vec_lane.h>  // blur-nx (2026-11-11): the lanes by a run-time index (VecLanes below)
#include <rex/ppc/vec_batch1.h>  // blur-nx (2026-11-11): CPU batch 1's forms (VecBatch1 below)
#include "hot/blur_block_copy.h"  // blur-nx (2026-11-11): CPU batch 2's natives' forms (VecBatch2 below)
#include "hot/blur_gather.h"
#include <rex/ppc/vec_batch2.h>  // blur-nx (2026-11-11): the dot-product merges (VecBatch2 below)
#include "hot/blur_swap.h"  // blur-nx (2026-11-12): word swaps for lane-wise natives (VecBatch3 below)
#include <rex/ppc/fma_single.h>  // blur-nx (2026-11-12): fmadds rounded once (FmaSingle below)
#include <rex/ppc/float_be.h>  // blur-nx (2026-11-13, M-23): singles stored and loaded in the vector unit

namespace {

// Keeps the compiler from working a result out at build time.
template <typename T>
T Opaque(T v) {
  volatile T x = v;
  return x;
}

struct Tally {
  int checks = 0;
  int wrong = 0;
  std::string report;  // the first failures, for the file a failed check leaves next to the program
  void Expect(const char* what, const std::string& input, uint64_t got, uint64_t want) {
    ++checks;
    if (got == want) return;
    if (wrong++ < 8) {
      REXLOG_ERROR("[enhancements] DIFFERENCE: instruction forms self-check: {}({}) gave {:016X}, expected {:016X}", what,
                   input, got, want);
      report += fmt::format("{}({}) gave {:016X}, expected {:016X}\n", what, input, got, want);
    }
  }
};

union Vec {
  simde__m128i v;
  simde__m128 f;
  uint8_t u8[16];
  int8_t s8[16];
  uint16_t u16[8];
  int16_t s16[8];
  uint32_t u32[4];
  int32_t s32[4];
  float f32[4];
};

Vec Load(const uint8_t* bytes) {
  Vec r;
  std::memcpy(r.u8, bytes, 16);
  r.v = Opaque(r.v);
  return r;
}

void ExpectVec(Tally& t, const char* what, int index, const Vec& got, const Vec& want) {
  uint64_t g[2], w[2];
  std::memcpy(g, got.u8, 16);
  std::memcpy(w, want.u8, 16);
  t.Expect(what, fmt::format("vector {} low", index), g[0], w[0]);
  t.Expect(what, fmt::format("vector {} high", index), g[1], w[1]);
}

void Conversions(Tally& t) {
  // Round to nearest even: the mode every thread starts in. fctiw's result is the register image: sign extended,
  // except a NaN (0x0000000080000000).
  struct Case {
    double v;
    int64_t fctid, fctiw, fctidz;
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  // banjo-nx (2026-11-19, the rebase; a handoff): with blur-nx's near-exact #2 (REX_NX_FCTI_NO_NAN, N&B's cleared set,
  // docs/banjo-near-exact.md) a NaN converts to 0 by design (AArch64's fcvtzs, no NaN test): that is the expected
  // result then, so the check still proves the form instead of reporting the switch at every start (and writing
  // banjo_selfcheck_failed.txt next to the program).
#if defined(REX_NX_FCTI_NO_NAN)
  constexpr bool kNanToZero = true;
#else
  constexpr bool kNanToZero = false;
#endif
  const Case cases[] = {
      {0.5, 0, 0, 0},
      {1.5, 2, 2, 1},
      {2.5, 2, 2, 2},
      {3.5, 4, 4, 3},
      {-0.5, 0, 0, 0},
      {-1.5, -2, -2, -1},
      {-2.5, -2, -2, -2},
      {-3.5, -4, -4, -3},
      {2.4999, 2, 2, 2},
      {2.5001, 3, 3, 2},
      {1e10, 10000000000ll, INT32_MAX, 10000000000ll},
      {-1e10, -10000000000ll, INT32_MIN, -10000000000ll},
      {9223372036854775808.0, INT64_MAX, INT32_MAX, INT64_MAX},
      {-1e19, INT64_MIN, INT32_MIN, INT64_MIN},
      {nan, kNanToZero ? 0 : INT64_MIN, kNanToZero ? 0 : int64_t(0x80000000u), kNanToZero ? 0 : INT64_MIN},
  };
  for (const Case& c : cases) {
    const double v = Opaque(c.v);
    const std::string in = fmt::format("{}", c.v);
    t.Expect("fctid", in, uint64_t(rex::ppc::fctid(v)), uint64_t(c.fctid));
    t.Expect("fctiw", in, uint64_t(rex::ppc::fctiw(v)), uint64_t(c.fctiw));
    t.Expect("fctidz", in, uint64_t(rex::ppc::fctidz(v)), uint64_t(c.fctidz));
    // fctiwz (the first round's): truncation with the same saturation and NaN image.
    int64_t z;
    if (std::isnan(c.v)) {
      z = kNanToZero ? 0 : int64_t(0x80000000u);
    } else if (c.v >= 2147483647.0) {
      z = INT32_MAX;
    } else if (c.v <= -2147483648.0) {
      z = INT32_MIN;
    } else {
      z = int64_t(int32_t(c.v));
    }
    t.Expect("fctiwz", in, uint64_t(rex::ppc::fctiwz(v)), uint64_t(z));
  }
}

// blur-nx (2026-11-18): the reference follows the build's near-exact switches (BLUR_NEAR_EXACT; docs/cpu-near-exact-
// 50-1113.md), which change two of these forms by design: #17 REX_NX_DIV_NO_CHECK (the AArch64 divisions give sdiv's
// INT_MIN for INT_MIN / -1, where the old form gave 0) and #1 REX_NX_SNAN_PASS (QuietSingleBits keeps a signalling
// NaN's bits, as the Xenon's lfs / stfs pair does). Before, every start of the release reported those four results as
// failures (banjo_selfcheck_failed.txt on the card) though each is the form the build chose.
#if defined(REX_NX_DIV_NO_CHECK) && defined(__aarch64__)
constexpr bool kDivOverflowGivesMin = true;
#else
constexpr bool kDivOverflowGivesMin = false;
#endif
#if defined(REX_NX_SNAN_PASS)
constexpr bool kSnanKept = true;
#else
constexpr bool kSnanKept = false;
#endif

void Divisions(Tally& t) {
  struct Signed {
    int64_t n, d;
  };
  const Signed pairs[] = {{7, 0},   {-7, 0},       {0, 0},         {100, 7},      {-100, 7},      {100, -7},
                          {-7, 2},  {7, -2},       {INT32_MIN, -1}, {INT32_MIN, 1}, {INT32_MAX, -1}, {INT64_MIN, -1},
                          {INT64_MIN, 1}, {INT64_MAX, -1}, {-9, 2}, {1, INT64_MIN}};
  for (const Signed& p : pairs) {
    const std::string in = fmt::format("{}, {}", p.n, p.d);
    {
      const int32_t n = int32_t(p.n), d = int32_t(p.d);
      const uint32_t want = d == 0 ? 0u
                            : (n == INT32_MIN && d == -1) ? (kDivOverflowGivesMin ? uint32_t(INT32_MIN) : 0u)
                                                          : uint32_t(int64_t(n) / int64_t(d));
      t.Expect("divw", in, rex::ppc::divw(Opaque(n), Opaque(d)), want);
      const uint32_t un = uint32_t(n), ud = uint32_t(d);
      t.Expect("divwu", in, rex::ppc::divwu(Opaque(un), Opaque(ud)), ud == 0 ? 0u : un / ud);
    }
    {
      const int64_t n = p.n, d = p.d;
      int64_t want = 0;
      if (d != 0 && !(n == INT64_MIN && d == -1)) want = n / d;
      if (n == INT64_MIN && d == -1 && kDivOverflowGivesMin) want = INT64_MIN;
      t.Expect("divd", in, uint64_t(rex::ppc::divd(Opaque(n), Opaque(d))), uint64_t(want));
      const uint64_t un = uint64_t(n), ud = uint64_t(d);
      t.Expect("divdu", in, rex::ppc::divdu(Opaque(un), Opaque(ud)), ud == 0 ? 0u : un / ud);
    }
  }
}

void FloatCopies(Tally& t) {
  // single bits -> the double a load gives with the flush mode off, and the bits a store of it writes.
  struct Case {
    uint32_t single;
    uint64_t as_double;
    uint32_t stored;
  };
  const Case cases[] = {
      {0x00000000u, 0x0000000000000000ull, 0x00000000u}, {0x80000000u, 0x8000000000000000ull, 0x80000000u},
      {0x3F800000u, 0x3FF0000000000000ull, 0x3F800000u}, {0x00000001u, 0x36A0000000000000ull, 0x00000001u},
      {0x807FFFFFu, 0xB80FFFFFC0000000ull, 0x807FFFFFu}, {0x00800000u, 0x3810000000000000ull, 0x00800000u},
      {0x7F800000u, 0x7FF0000000000000ull, 0x7F800000u}, {0x7FC00001u, 0x7FF8000020000000ull, 0x7FC00001u},
      {0x7F800001u, 0x7FF8000020000000ull, 0x7FC00001u}, {0xFFA00000u, 0xFFFC000000000000ull, 0xFFE00000u},
  };
  for (const Case& c : cases) {
    const std::string in = fmt::format("{:08X}", c.single);
    t.Expect("SingleToDoublePure", in, std::bit_cast<uint64_t>(rex::ppc::SingleToDoublePure(Opaque(c.single))),
             c.as_double);
    t.Expect("QuietSingleBits", in, rex::ppc::QuietSingleBits(Opaque(c.single)), kSnanKept ? c.single : c.stored);
  }
}

// blur-nx (2026-11-13, M-23): the vector-unit single store and load (rex/ppc/float_be.h) against the integer forms:
// the same four bytes, every class of value (zeros, normals, denormals, infinities, quiet and signalling NaNs).
void FloatBigEndian(Tally& t) {
  uint32_t bits[] = {0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x00000001u, 0x807FFFFFu, 0x00800000u,
                     0x7F7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00001u, 0x7F800001u, 0xFFA00000u, 0x12345678u,
                     0xDEADBEEFu, 0x3EAAAAABu};
  for (uint32_t b : bits) {
    const std::string in = fmt::format("{:08X}", b);
    uint8_t out[4];
    rex::ppc::FloatToBigEndian(out, std::bit_cast<float>(Opaque(b)));
    uint32_t got;
    std::memcpy(&got, out, 4);
    t.Expect("FloatToBigEndian", in, got, __builtin_bswap32(b));
    const uint32_t be = __builtin_bswap32(Opaque(b));
    t.Expect("FloatFromBigEndian", in, std::bit_cast<uint32_t>(rex::ppc::FloatFromBigEndian(&be)), b);
  }
  // blur-nx (2026-11-17, #231): the double load (the same eight bytes).
  const uint64_t dbits[] = {0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull,
                            0x0000000000000001ull, 0x7FF0000000000000ull, 0x7FF8000000000001ull,
                            0x7FF0000000000001ull, 0xFFF4000000000000ull, 0x0123456789ABCDEFull};
  for (uint64_t b : dbits) {
    const std::string in = fmt::format("{:016X}", b);
    const uint64_t be = __builtin_bswap64(Opaque(b));
    t.Expect("DoubleFromBigEndian", in, std::bit_cast<uint64_t>(rex::ppc::DoubleFromBigEndian(&be)), b);
  }
}

void VectorHelpers(Tally& t) {
  // Two operands with lanes at the edges of every width, and shift counts below, at and above the lane width.
  static const uint8_t kA[4][16] = {
      {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x55, 0xAA, 0x10, 0x20, 0x40, 0x08, 0x04, 0x02, 0xC3, 0x3C},
      {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x00, 0x00},
      {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x0F, 0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21},
      {0x00, 0x80, 0xFF, 0x7F, 0x01, 0x80, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
  };
  static const uint8_t kB[4][16] = {
      {0x01, 0x01, 0x01, 0x80, 0x01, 0xFF, 0xAA, 0x55, 0x10, 0x1F, 0x40, 0x07, 0x04, 0x03, 0x3C, 0xC3},
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
      {0x00, 0x00, 0x07, 0x00, 0x08, 0x00, 0x0F, 0x00, 0x10, 0x00, 0x11, 0x00, 0xFF, 0x00, 0x00, 0x01},
      {0x00, 0x01, 0x07, 0x08, 0x09, 0x0F, 0x10, 0x1F, 0x20, 0x7F, 0x80, 0xFF, 0x03, 0x05, 0x06, 0x02},
  };
  for (int k = 0; k < 4; ++k) {
    const Vec a = Load(kA[k]), b = Load(kB[k]);
    Vec want, got;
    for (int i = 0; i < 4; ++i) want.u32[i] = uint32_t(std::min<uint64_t>(uint64_t(a.u32[i]) + b.u32[i], 0xFFFFFFFFull));
    got.v = rex::ppc::simde_mm_adds_epu32(a.v, b.v);
    ExpectVec(t, "adds_epu32", k, got, want);
    for (int i = 0; i < 16; ++i) want.s8[i] = int8_t((int(a.s8[i]) + int(b.s8[i]) + 1) >> 1);
    got.v = rex::ppc::simde_mm_avg_epi8(a.v, b.v);
    ExpectVec(t, "avg_epi8", k, got, want);
    for (int i = 0; i < 8; ++i) want.s16[i] = int16_t((int(a.s16[i]) + int(b.s16[i]) + 1) >> 1);
    got.v = rex::ppc::simde_mm_avg_epi16(a.v, b.v);
    ExpectVec(t, "avg_epi16", k, got, want);
    for (int i = 0; i < 4; ++i) want.s32[i] = int32_t((int64_t(a.s32[i]) + int64_t(b.s32[i]) + 1) >> 1);
    got.v = rex::ppc::simde_mm_avg_epi32(a.v, b.v);
    ExpectVec(t, "avg_epi32", k, got, want);
    for (int i = 0; i < 4; ++i) want.f32[i] = float(a.u32[i]);
    got.f = rex::ppc::simde_mm_cvtepu32_ps_(a.v);
    ExpectVec(t, "cvtepu32_ps", k, got, want);
    for (int i = 0; i < 16; ++i) want.u8[i] = a.u8[i] > b.u8[i] ? 0xFF : 0;
    got.v = rex::ppc::simde_mm_cmpgt_epu8(a.v, b.v);
    ExpectVec(t, "cmpgt_epu8", k, got, want);
    for (int i = 0; i < 8; ++i) want.u16[i] = a.u16[i] > b.u16[i] ? 0xFFFF : 0;
    got.v = rex::ppc::simde_mm_cmpgt_epu16(a.v, b.v);
    ExpectVec(t, "cmpgt_epu16", k, got, want);
    for (int i = 0; i < 8; ++i) want.u16[i] = b.u16[i] >= 16 ? 0 : uint16_t(a.u16[i] << b.u16[i]);
    got.v = rex::ppc::simde_mm_sllv_epi16(a.v, b.v);
    ExpectVec(t, "sllv_epi16", k, got, want);
    for (int i = 0; i < 8; ++i) want.u16[i] = b.u16[i] >= 16 ? 0 : uint16_t(a.u16[i] >> b.u16[i]);
    got.v = rex::ppc::simde_mm_srlv_epi16(a.v, b.v);
    ExpectVec(t, "srlv_epi16", k, got, want);
    for (int i = 0; i < 8; ++i) want.s16[i] = int16_t(int32_t(a.s16[i]) >> (b.u16[i] >= 15 ? 15 : b.u16[i]));
    got.v = rex::ppc::simde_mm_srav_epi16(a.v, b.v);
    ExpectVec(t, "srav_epi16", k, got, want);
    for (int i = 0; i < 16; ++i) want.u8[i] = b.u8[i] >= 8 ? 0 : uint8_t(a.u8[i] << b.u8[i]);
    got.v = rex::ppc::simde_mm_sllv_epi8(a.v, b.v);
    ExpectVec(t, "sllv_epi8", k, got, want);
    // vperm: byte i comes from (bit 4 of the control ? b : a)[15 - (control & 15)]; the control here is kB of
    // the next row, which mixes both sources.
    const Vec c = Load(kB[(k + 1) & 3]);
    for (int i = 0; i < 16; ++i) want.u8[i] = ((c.u8[i] & 0x10) ? b : a).u8[15 - (c.u8[i] & 15)];
    got.v = rex::ppc::simde_mm_perm_epi8_(a.v, b.v, c.v);
    ExpectVec(t, "perm_epi8", k, got, want);
  }
  // The two saturating conversions: truncation; out of range saturates; a NaN gives 0.
  static const float kF[2][4] = {
      {0.75f, -0.75f, 2147483648.0f, -2147483904.0f},
      {4294967296.0f, 123456.7f, std::numeric_limits<float>::quiet_NaN(), -1.0f},
  };
  for (int k = 0; k < 2; ++k) {
    Vec a, want, got;
    std::memcpy(a.f32, kF[k], 16);
    a.v = Opaque(a.v);
    for (int i = 0; i < 4; ++i) {
      const float f = a.f32[i];
      want.s32[i] = std::isnan(f) ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : int32_t(f);
    }
    got.v = rex::ppc::simde_mm_vctsxs(a.f);
    ExpectVec(t, "vctsxs", k, got, want);
    for (int i = 0; i < 4; ++i) {
      const float f = a.f32[i];
      want.u32[i] = (std::isnan(f) || f <= 0.0f) ? 0u : f >= 4294967296.0f ? 0xFFFFFFFFu : uint32_t(f);
    }
    got.v = rex::ppc::simde_mm_vctuxs(a.f);
    ExpectVec(t, "vctuxs", k, got, want);
  }
}

// blur-nx (2026-11-11): rex/ppc/vec_lane.h, the lanes of a vector register by a run-time index (tools/vec_lane_index.py
// writes every stvewx / stvehx with them). The guard cannot see a wrong lane here (it compares two forms that both use
// the helper), so every index of a few vectors is checked against the array read, on the device.
void VecLanes(Tally& t) {
  static const uint32_t kV[3][4] = {
      {0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu},
      {0xDEADBEEFu, 0x80000000u, 0x7FFFFFFFu, 0x00000001u},
      {0xFFFFFFFFu, 0x12345678u, 0x9ABCDEF0u, 0x0F0F0F0Fu},
  };
  for (int k = 0; k < 3; ++k) {
    alignas(16) union {
      uint32_t u32[4];
      uint16_t u16[8];
    } v;
    std::memcpy(v.u32, kV[k], 16);
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t idx = Opaque(i);
      t.Expect("VecLaneU32", fmt::format("vector {} lane {}", k, i), rex::ppc::VecLaneU32(v.u32, idx), Opaque(v.u32[i]));
    }
    for (uint32_t i = 0; i < 8; ++i) {
      const uint32_t idx = Opaque(i);
      t.Expect("VecLaneU16", fmt::format("vector {} lane {}", k, i), rex::ppc::VecLaneU16(v.u16, idx), Opaque(v.u16[i]));
    }
  }
}

// blur-nx (2026-11-11, CPU batch 1): the forms of tools/vec_cvt_fixed.py, vec_shift_whole.py, vec_dcbz_zva.py and
// vec_lvlx_splat.py, each against the form the generator writes (rex/ppc/vec_batch1.h's *Ref, or the expression
// itself), on the device. The guard cannot see them: the copy and the original statements both use the new forms.
uint32_t Lcg(uint32_t& s) { return s = s * 1664525u + 1013904223u; }

template <int N>
void CvtFixedOne(Tally& t, const uint32_t (&in)[4]) {
  Vec x;
  std::memcpy(x.u32, in, 16);
  x.v = Opaque(x.v);
  Vec got, want;
  got.f = rex::ppc::VcfsxN<N>(x.v);
  want.f = rex::ppc::VcfsxRef(x.v, N);
  ExpectVec(t, "VcfsxN", N, got, want);
  got.f = rex::ppc::VcfuxN<N>(x.v);
  want.f = rex::ppc::VcfuxRef(x.v, N);
  ExpectVec(t, "VcfuxN", N, got, want);
}

void VecBatch1(Tally& t) {
  // VS-27: every scale the pass writes in Blur (2^-1, 2, 7, 8, 14, 15, 31), on edge values and pseudo-random words.
  uint32_t seed = 0x2F6B1D35u;
  static const uint32_t kEdge[] = {0u, 1u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x01000001u, 0xFEFFFFFFu, 0x00FFFFFFu,
                                   0x55555555u, 0xAAAAAAABu, 0x00000003u, 0x80000001u};
  for (int r = 0; r < 12; ++r) {
    uint32_t in[4];
    for (int k = 0; k < 4; ++k) in[k] = r < 3 ? kEdge[(r * 4 + k) % 12] : Lcg(seed) >> (Lcg(seed) & 31);
    CvtFixedOne<1>(t, in);
    CvtFixedOne<2>(t, in);
    CvtFixedOne<7>(t, in);
    CvtFixedOne<8>(t, in);
    CvtFixedOne<14>(t, in);
    CvtFixedOne<15>(t, in);
    CvtFixedOne<31>(t, in);
  }
  // VX-20 / VX-21: every count 0..31 (with junk in the count's high bits, which the forms ignore) on four words.
  for (uint32_t c = 0; c < 32; ++c) {
    Vec a, b, got, want;
    a.u32[0] = 0x80000000u;
    a.u32[1] = 0xDEADBEEFu;
    a.u32[2] = 0x7FFFFFFFu;
    a.u32[3] = Lcg(seed);
    for (int k = 0; k < 4; ++k) b.u32[k] = ((Lcg(seed) & 0xFFFFFF00u) | ((c + uint32_t(k) * 7u) & 31u)) ^ (Lcg(seed) & 0xE0u);
    a.v = Opaque(a.v);
    b.v = Opaque(b.v);
    got.v = rex::ppc::VecSrw(a.v, b.v);
    want.v = rex::ppc::VecSrwRef(a.v, b.v);
    ExpectVec(t, "VecSrw", int(c), got, want);
    got.v = rex::ppc::VecSraw(a.v, b.v);
    want.v = rex::ppc::VecSrawRef(a.v, b.v);
    ExpectVec(t, "VecSraw", int(c), got, want);
    const simde__m128i masked = simde_mm_and_si128(b.v, simde_mm_set1_epi32(0x1F));
    got.v = rex::ppc::VecSllv31(a.v, masked);
    want.v = simde_mm_sllv_epi32(a.v, masked);
    ExpectVec(t, "VecSllv31", int(c), got, want);
  }
  // VS-05: 128 bytes zeroed at a 128-byte aligned address, the bytes around it untouched.
  {
    alignas(128) static uint8_t buf[384];
    std::memset(buf, 0xA5, sizeof(buf));
    rex::ppc::Dcbz128(Opaque(buf + 128));
    uint64_t zeros = 0, kept = 0;
    for (int i = 0; i < 384; ++i) {
      if (i >= 128 && i < 256) zeros += buf[i] == 0;
      else kept += buf[i] == 0xA5;
    }
    t.Expect("Dcbz128", "zeroed bytes", zeros, 128);
    t.Expect("Dcbz128", "bytes around", kept, 256);
  }
  // VS-25: lvlx + vspltw 0 (the generator's form: the block load, the VectorMaskL row, word 0 = lane 3 splatted)
  // against the one 4-byte load the pass writes, at every offset it is used for (0..12).
  {
    alignas(16) uint8_t mem[32];
    for (int i = 0; i < 32; ++i) mem[i] = uint8_t(0x11 * (i + 1) ^ (i << 5));
    for (uint32_t a = 0; a <= 12; ++a) {
      const simde__m128i block = Opaque(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(mem)));
      const simde__m128i row = simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(&rex::ppc::VectorMaskL[a * 16]));
      Vec want, got;
      want.v = simde_mm_shuffle_epi32(simde_mm_shuffle_epi8(block, row), 0xFF);
      uint32_t word;
      std::memcpy(&word, mem + a, 4);
      got.v = simde_mm_set1_epi32(int(__builtin_bswap32(Opaque(word))));
      ExpectVec(t, "LvlxSplat", int(a), got, want);
    }
  }
}

// blur-nx (2026-11-11, CPU batch 2): the resampler natives' table pairs (hot/blur_gather.h: the NEON form against the
// portable one, at every word offset of a line, 12 included, where the pair reaches into the next line) and the natives'
// block copy (hot/blur_block_copy.h: the NEON form for a multiple of 64, memcpy otherwise).
void VecBatch2(Tally& t) {
  uint32_t seed = 0x6A09E667u;
  alignas(16) static uint8_t mem[256];
  for (int i = 0; i < 256; ++i) mem[i] = uint8_t(Lcg(seed) >> 13);
  for (int r = 0; r < 64; ++r) {
    uint32_t o[4];
    for (int k = 0; k < 4; ++k) o[k] = r < 16 ? uint32_t((r * 4 + k * 52) % 240) & ~3u : (Lcg(seed) % 240u) & ~3u;
    const uint8_t* p[4];
    for (int k = 0; k < 4; ++k) p[k] = Opaque(static_cast<const uint8_t*>(mem + o[k]));
    simde__m128 lo, hi, lo_ref, hi_ref;
    blur_gather::Pairs4(p[0], p[1], p[2], p[3], lo, hi);
    blur_gather::Pairs4Ref(p[0], p[1], p[2], p[3], lo_ref, hi_ref);
    Vec a, b;
    a.v = simde_mm_castps_si128(lo);
    b.v = simde_mm_castps_si128(lo_ref);
    ExpectVec(t, "Pairs4 lo", r, a, b);
    a.v = simde_mm_castps_si128(hi);
    b.v = simde_mm_castps_si128(hi_ref);
    ExpectVec(t, "Pairs4 hi", r, a, b);
  }
  t.Expect("PairOk", "0x82000010", blur_gather::PairOk(0x82000010u), true);
  t.Expect("PairOk", "0x8200001C", blur_gather::PairOk(0x8200001Cu), true);
  t.Expect("PairOk", "0x82000012", blur_gather::PairOk(0x82000012u), false);
  t.Expect("PairOk", "0xFFFFFFF8", blur_gather::PairOk(0xFFFFFFF8u), true);
  t.Expect("PairOk", "0xFFFFFFFC", blur_gather::PairOk(0xFFFFFFFCu), false);
  t.Expect("PairOk", "0xDFFFFFFC", blur_gather::PairOk(0xDFFFFFFCu), false);
  // VX-15 / VX-14: two and four dot products (vmsum4fp128 0xFF, vmsum3fp128 0xEF) and their vmrghw merges against the
  // products and pairwise adds the pass writes, on finite, signed-zero, infinite, NaN and denormal lanes.
  {
    static const uint32_t kOdd[] = {0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00001u, 0xFFA00002u,
                                    0x00000001u, 0x807FFFFFu, 0x3F800000u, 0x7F7FFFFFu, 0x00800000u, 0x4B000001u};
    for (int r = 0; r < 48; ++r) {
      Vec in[8];
      for (int k = 0; k < 8; ++k)
        for (int l = 0; l < 4; ++l) {
          const uint32_t x = Lcg(seed);
          in[k].u32[l] = r >= 8 && (x & 7u) == 0 ? kOdd[(x >> 3) % 12u] : (0x3C000000u + (x & 0x0FFFFFFFu)) ^ (x & 0x80000000u);
        }
      for (int k = 0; k < 8; ++k) in[k].v = Opaque(in[k].v);
      Vec got, want;
      got.f = rex::ppc::DpPairNew<0xFF>(in[0].f, in[1].f, in[2].f, in[3].f);
      want.f = rex::ppc::DpPairRef<0xFF>(in[0].f, in[1].f, in[2].f, in[3].f);
      ExpectVec(t, "DpPair 0xFF", r, got, want);
      got.f = rex::ppc::DpPairNew<0xEF>(in[0].f, in[1].f, in[2].f, in[3].f);
      want.f = rex::ppc::DpPairRef<0xEF>(in[0].f, in[1].f, in[2].f, in[3].f);
      ExpectVec(t, "DpPair 0xEF", r, got, want);
      // the quad: D1 = merge(dp(0,1), dp(2,3)), D2 = merge(dp(4,5), dp(6,7)), D = merge(D1, D2) = [a c b d]
      const simde__m128 d1 = rex::ppc::DpPairRef<0xFF>(in[0].f, in[1].f, in[2].f, in[3].f);
      const simde__m128 d2 = rex::ppc::DpPairRef<0xFF>(in[4].f, in[5].f, in[6].f, in[7].f);
      want.v = simde_mm_unpackhi_epi32(simde_mm_castps_si128(d1), simde_mm_castps_si128(d2));
      got.f = rex::ppc::DpQuad(rex::ppc::DpProd<0xFF>(in[0].f, in[1].f), rex::ppc::DpProd<0xFF>(in[4].f, in[5].f),
                               rex::ppc::DpProd<0xFF>(in[2].f, in[3].f), rex::ppc::DpProd<0xFF>(in[6].f, in[7].f));
      ExpectVec(t, "DpQuad", r, got, want);
    }
  }
  {
    alignas(16) static uint8_t src[1088], dst[1088], ref[1088];
    for (int i = 0; i < 1088; ++i) src[i] = uint8_t(Lcg(seed) >> 7);
    for (size_t len : {size_t(1024), size_t(64), size_t(1000)}) {
      std::memset(dst, 0x5A, sizeof(dst));
      std::memset(ref, 0x5A, sizeof(ref));
      blur_copy::Copy64s(Opaque(dst + 16), Opaque(static_cast<const uint8_t*>(src + 32)), len);
      std::memcpy(ref + 16, src + 32, len);
      t.Expect("Copy64s", std::to_string(len).c_str(), std::memcmp(dst, ref, sizeof(dst)) == 0, true);
    }
  }
}

// blur-nx (2026-11-12, CPU batch 3): hot/blur_swap.h. Rev32 swaps each word's bytes; LaneRev(Rev32(x)) is the full
// 16-byte reversal the generated code's REX_BSWAP128 makes (the mixer natives rest on that), on pseudo-random vectors.
void VecBatch3(Tally& t) {
  uint32_t seed = 0xBB67AE85u;
  for (int r = 0; r < 32; ++r) {
    Vec in, want_words, want_full, got;
    for (int k = 0; k < 4; ++k) in.u32[k] = Lcg(seed) ^ (r << 24);
    in.v = Opaque(in.v);
    for (int k = 0; k < 4; ++k) want_words.u32[k] = __builtin_bswap32(in.u32[k]);
    for (int i = 0; i < 16; ++i) want_full.u8[i] = in.u8[15 - i];
    got.v = blur_swap::Rev32(in.v);
    ExpectVec(t, "Rev32", r, got, want_words);
    got.f = blur_swap::LaneRev(simde_mm_castsi128_ps(blur_swap::Rev32(in.v)));
    ExpectVec(t, "LaneRev(Rev32)", r, got, want_full);
  }
}

// blur-nx (2026-11-12, the user: "Please do the fmadds change"): rex/ppc/fma_single.h. On float inputs FmaddsSingle must be
// a * b + c rounded once to single: against a quad-precision reference where long double is wider than 2 * 24 + 2 bits
// (the AArch64's IEEE quad: the product exact, the sum rounded to 113 bits, then to 24, which cannot differ from one
// rounding). On inputs that are not floats it must be the double form exactly. Random values, cancellations
// (c near -a * b), denormals, signed zeros and infinities; NaNs only checked to stay NaN.
void FmaSingle(Tally& t) {
  uint32_t seed = 0x3C6EF372u;
  const auto rnd = [&](int spread) {
    const uint32_t x = Lcg(seed);
    const uint32_t exp = 127u + (Lcg(seed) % uint32_t(2 * spread + 1)) - uint32_t(spread);
    return std::bit_cast<float>((x & 0x807FFFFFu) | (std::min(exp, 254u) << 23));
  };
  static const uint32_t kSpecial[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x7F800000u,
                                      0xFF800000u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0x34000000u, 0x00400000u};
  int checked_exact = 0;
  for (int r = 0; r < 600; ++r) {
    float a, b, c;
    if (r < 144) {
      a = std::bit_cast<float>(kSpecial[r % 12]);
      b = std::bit_cast<float>(kSpecial[(r / 12) % 12]);
      c = rnd(20);
    } else if (r < 400) {
      a = rnd(r < 300 ? 8 : 60);
      b = rnd(r < 300 ? 8 : 60);
      c = rnd(r < 300 ? 8 : 60);
    } else {
      a = rnd(10);
      b = rnd(10);
      // a cancellation: c is -a*b rounded to single, nudged by a few units in its last place
      const float p = -(a * b);
      c = std::bit_cast<float>(std::bit_cast<uint32_t>(p) + (Lcg(seed) % 7u) - 3u);
    }
    const double da = Opaque(double(a)), db = Opaque(double(b)), dc = Opaque(double(c));
    const double got = rex::ppc::FmaddsSingle(da, db, dc);
    const std::string in = fmt::format("{:08X} {:08X} {:08X}", std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b),
                                       std::bit_cast<uint32_t>(c));
    // blur-nx (2026-11-13, M-24): the known-single form (three single inputs) against the tested one, bit for bit
    // (NaN payloads too: the same instruction on the same operands).
    t.Expect("FmaddsSingleKnown", in, std::bit_cast<uint64_t>(rex::ppc::FmaddsSingleKnown(da, db, dc)),
             std::bit_cast<uint64_t>(got));
    if constexpr (sizeof(long double) > 8) {
      const long double exact = (long double)a * (long double)b + (long double)c;
      const float want = float(exact);
      if (std::isnan(want)) {
        t.Expect("FmaddsSingle NaN", in, std::isnan(got), true);
      } else {
        t.Expect("FmaddsSingle", in, std::bit_cast<uint64_t>(got), std::bit_cast<uint64_t>(double(want)));
        ++checked_exact;
      }
    }
    // an input that is not a float: the double form, bit for bit
    const double wide = Opaque(double(a) * (1.0 + 0x1p-40));
    if (!rex::ppc::IsExactSingle(wide)) {
      t.Expect("FmaddsSingle wide", in, std::bit_cast<uint64_t>(rex::ppc::FmaddsSingle(wide, db, dc)),
               std::bit_cast<uint64_t>(rex::ppc::FmaddsDouble(wide, db, dc)));
    }
  }
  (void)checked_exact;
}

// blur-nx (2026-11-12, CPU batch 3, CG-16): frsp's any-mode form with one range test against the two-test form it
// replaced, over the boundaries (FLT_MIN and its neighbours, the single denormal range, denormal doubles, the largest
// single and the doubles that round to it or to infinity) and a spread of exponents, with the flush mode on and off.
void RoundSingle(Tally& t) {
  using P = rex::platform::FPSCRPlatform;
  std::vector<double> in = {0.0, -0.0, 0x1p-126, -0x1p-126, std::nextafter(0x1p-126, 0.0), std::nextafter(0x1p-126, 1.0),
                            0x1p-149, 0x1.8p-150, 0x1p-150, 0x1p-151, 0x1.fffffcp-127, 0x1.fffffep-127, 0x1.ffffffp-127,
                            std::numeric_limits<double>::denorm_min(), 0x1p-1022, std::nextafter(0x1p-1022, 0.0),
                            0x1.fffffep127, 0x1.fffffefp127, 0x1.ffffffp127, 0x1p128, 1e300, -1e300,
                            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN(), std::bit_cast<double>(0x7FF0000000000001ull),
                            std::bit_cast<double>(0xFFF8000000001234ull), 1.0, -1.0, 1.0 / 3.0};
  uint32_t seed = 0x2545F491u;
  for (int i = 0; i < 400; ++i) {
    const uint64_t hi = Lcg(seed), lo = Lcg(seed);
    // exponents from 2^-1074 to 2^130 (the biased 0-1153), the first half in the single denormal range's 2^-160..2^-121
    const uint64_t exp = i < 200 ? 1023 - 160 + (hi % 40) : (hi % 1154);
    in.push_back(std::bit_cast<double>(((lo & 1) << 63) | (exp << 52) | ((hi << 32 | lo) & 0xFFFFFFFFFFFFFull)));
  }
  const auto saved = P::getcsr();
  for (int mode = 0; mode < 2; ++mode) {
    P::setcsr(decltype(saved)(mode ? (saved | P::FlushMask) : (saved & ~decltype(saved)(P::FlushMask))));
    for (double v : in) {
      const double d = Opaque(v);
      // Opaque (a volatile object) keeps both computations between the two mode writes.
      const uint64_t got = Opaque(std::bit_cast<uint64_t>(rex::ppc::RoundToSingleAnyMode(d)));
      const uint64_t want = Opaque(std::bit_cast<uint64_t>(rex::ppc::RoundToSingleAnyModeRef(d)));
      P::setcsr(saved);  // the report formats with the thread's own mode
      t.Expect(mode ? "RoundToSingleAnyMode (flush on)" : "RoundToSingleAnyMode (flush off)",
               fmt::format("{:016X}", std::bit_cast<uint64_t>(v)), got, want);
      P::setcsr(decltype(saved)(mode ? (saved | P::FlushMask) : (saved & ~decltype(saved)(P::FlushMask))));
    }
  }
  P::setcsr(saved);
}

// blur-nx (2026-11-12, CPU batch 3, CG-14): the known-single forms (tools/single_floats.py: both operands proven single)
// against the any-mode forms, on pairs of singles (zeros, the smallest and largest denormal singles, FLT_MIN, the
// largest single, infinities, NaNs and a spread of exponents, including pairs whose results land in the single
// denormal range or overflow), with the flush mode on and off; the compare by its CR field.
void KnownSingle(Tally& t) {
  using P = rex::platform::FPSCRPlatform;
  static const uint32_t kSpecial[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x80800000u,
                                      0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x3F800000u,
                                      0xBF800000u, 0x0C000000u, 0x34000001u, 0x00400000u};
  std::vector<float> in;
  for (uint32_t b : kSpecial) in.push_back(std::bit_cast<float>(b));
  uint32_t seed = 0x9E3779B9u;
  for (int i = 0; i < 48; ++i) {
    const uint32_t x = Lcg(seed);
    // biased exponents 0-254: the small ones make products and quotients in the single denormal range
    const uint32_t exp = i < 24 ? (Lcg(seed) % 40u) : (Lcg(seed) % 255u);
    in.push_back(std::bit_cast<float>((x & 0x807FFFFFu) | (exp << 23)));
  }
  const auto saved = P::getcsr();
  const auto with = [&](int mode) {
    P::setcsr(decltype(saved)(mode ? (saved | P::FlushMask) : (saved & ~decltype(saved)(P::FlushMask))));
  };
  // The widening once, with the mode off (with it on, a denormal single would widen to zero): the doubles a register
  // holds after an lfs of these singles.
  std::vector<uint64_t> wide(in.size());
  with(0);
  for (size_t k = 0; k < in.size(); ++k) wide[k] = Opaque(std::bit_cast<uint64_t>(double(Opaque(in[k]))));
  P::setcsr(saved);
  for (int mode = 0; mode < 2; ++mode) {
    for (size_t ia = 0; ia < in.size(); ++ia) {
      for (size_t ib = 0; ib < in.size(); ++ib) {
        const float fa = in[ia], fb = in[ib];
        with(mode);
        const double a = std::bit_cast<double>(Opaque(wide[ia])), b = std::bit_cast<double>(Opaque(wide[ib]));
        uint64_t got[5], want[5];
        got[0] = Opaque(std::bit_cast<uint64_t>(rex::ppc::AddSingleKnown(a, b)));
        want[0] = Opaque(std::bit_cast<uint64_t>(rex::ppc::AddSingleAnyMode(a, b)));
        got[1] = Opaque(std::bit_cast<uint64_t>(rex::ppc::SubSingleKnown(a, b)));
        want[1] = Opaque(std::bit_cast<uint64_t>(rex::ppc::SubSingleAnyMode(a, b)));
        got[2] = Opaque(std::bit_cast<uint64_t>(rex::ppc::MulSingleKnown(a, b)));
        want[2] = Opaque(std::bit_cast<uint64_t>(rex::ppc::MulSingleAnyMode(a, b)));
        got[3] = Opaque(std::bit_cast<uint64_t>(rex::ppc::DivSingleKnown(a, b)));
        want[3] = Opaque(std::bit_cast<uint64_t>(rex::ppc::DivSingleAnyMode(a, b)));
        rex::ppc::CRRegister c1{}, c2{};
        c1.compare(a, b);
        rex::ppc::CompareAnyMode(c2, a, b);
        got[4] = Opaque(uint64_t(c1.raw()));
        want[4] = Opaque(uint64_t(c2.raw()));
        P::setcsr(saved);
        static const char* const kName[2][5] = {
            {"AddSingleKnown (flush off)", "SubSingleKnown (flush off)", "MulSingleKnown (flush off)",
             "DivSingleKnown (flush off)", "compare of singles (flush off)"},
            {"AddSingleKnown (flush on)", "SubSingleKnown (flush on)", "MulSingleKnown (flush on)",
             "DivSingleKnown (flush on)", "compare of singles (flush on)"}};
        for (int k = 0; k < 5; ++k) {
          // Bit for bit, NaNs included (the same operation on the same operands); formatted only on a difference.
          if (got[k] != want[k])
            t.Expect(kName[mode][k], fmt::format("{:08X} {:08X}", std::bit_cast<uint32_t>(fa),
                                                 std::bit_cast<uint32_t>(fb)), got[k], want[k]);
          else
            ++t.checks;
        }
      }
    }
  }
  P::setcsr(saved);
}

}  // namespace

namespace banjo {

void FormsSelfCheck() {
  Tally t;
  Conversions(t);
  Divisions(t);
  FloatCopies(t);
  FloatBigEndian(t);  // blur-nx (2026-11-13, M-23)
  VectorHelpers(t);
  VecLanes(t);
  VecBatch1(t);
  VecBatch2(t);
  VecBatch3(t);
  FmaSingle(t);
  RoundSingle(t);
  KnownSingle(t);
  if (t.wrong) {
    for (int i = 0; i < t.wrong; ++i) ::me::native::VerifyDifference();
    REXLOG_ERROR("[enhancements] DIFFERENCE: instruction forms self-check: {} of {} results are NOT as expected (see the lines "
                 "above); this build's helpers do not match their reference on this processor",
                 t.wrong, t.checks);
    // The released configuration logs nothing, and a wrong form must not go unseen on the console: the failures
    // also go to a file next to the program. Nothing is written when every result is as expected.
    const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / "banjo_selfcheck_failed.txt";
    if (std::FILE* f = std::fopen(path.string().c_str(), "w")) {
      std::fprintf(f, "banjo-nx instruction forms self-check: %d of %d results are not as expected\n%s", t.wrong,
                   t.checks, t.report.c_str());
      std::fclose(f);
    }
  } else {
    REXLOG_INFO("[enhancements] instruction forms self-check: {} of {} results as expected ({})", t.checks, t.checks,
#if defined(__aarch64__)
                "the AArch64 forms"
#else
                "the x86 forms"
#endif
    );
  }
}

}  // namespace banjo
