/**
 * @file    gpu/render/vertex_format.cpp
 */
#include "renut_engine/gpu/render/vertex_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/ultimate.h"
#include "shader_common.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace renut::gpu {

namespace {

// D3DDECLUSAGE values.
enum DeclUsage : u8 {
  Position = 0,
  BlendWeight = 1,
  BlendIndices = 2,
  Normal = 3,
  PSize = 4,
  TexCoord = 5,
  Tangent = 6,
  Binormal = 7,
  TessFactor = 8,
  PositionT = 9,
  Color = 10,
  Fog = 11,
  Depth = 12,
  Sample = 13,
};

const char *const kUsageSemantics[] = {
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
    "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE",
};

// Shared with the shaders through shader_common.h.
bool InputLocation(u8 usage, u8 usage_index, u32 &location) {
  struct Row {
    u8 usage;
    u8 index;
    u32 location;
  };
  static const Row kRows[] = {
#define RENUT_ROW(usage, index, location) {DeclUsage::usage, index, location},
      RENUT_VERTEX_INPUT_LOCATIONS(RENUT_ROW)
#undef RENUT_ROW
  };
  for (const Row &row : kRows) {
    if (row.usage == usage && row.index == usage_index) {
      location = row.location;
      return true;
    }
  }
  return false;
}

// Xenos vertex formats (GPUVERTEXFORMAT).
enum VertexFormat : u32 {
  k8_8_8_8 = 6,
  k2_10_10_10 = 7,
  k10_11_11 = 16,
  k11_11_10 = 17,
  k16_16 = 25,
  k16_16_16_16 = 26,
  k16_16_Float = 31,
  k16_16_16_16_Float = 32,
  k32 = 33,
  k32_32 = 34,
  k32_32_32_32 = 35,
  k32_Float = 36,
  k32_32_Float = 37,
  k32_32_32_32_Float = 38,
  k32_32_32_Float = 57,
};

struct FormatInfo {
  u32 dwords;      // guest size
  u32 components;  // components the format supplies
  bool is_float;
};

bool GetFormatInfo(u32 format, FormatInfo &info) {
  switch (format) {
  case k8_8_8_8: info = {1, 4, false}; return true;
  case k2_10_10_10: info = {1, 4, false}; return true;
  case k10_11_11: info = {1, 3, false}; return true;
  case k11_11_10: info = {1, 3, false}; return true;
  case k16_16: info = {1, 2, false}; return true;
  case k16_16_16_16: info = {2, 4, false}; return true;
  case k16_16_Float: info = {1, 2, true}; return true;
  case k16_16_16_16_Float: info = {2, 4, true}; return true;
  case k32: info = {1, 1, false}; return true;
  case k32_32: info = {2, 2, false}; return true;
  case k32_32_32_32: info = {4, 4, false}; return true;
  case k32_Float: info = {1, 1, true}; return true;
  case k32_32_Float: info = {2, 2, true}; return true;
  case k32_32_32_32_Float: info = {4, 4, true}; return true;
  case k32_32_32_Float: info = {3, 3, true}; return true;
  default: return false;
  }
}

// The swizzle equals what the input assembler does anyway: the format's
// components in order, then 0 for missing y/z and 1 for a missing w.
bool IsNativeSwizzle(const DeclType &type, u32 components) {
  for (u32 i = 0; i < 4; ++i) {
    const u8 expected = i < components ? u8(i) : (i == 3 ? 5 : 4);
    if (type.swizzle[i] != expected)
      return false;
  }
  return true;
}

bool Is16BitFormat(u32 format) {
  return format == k16_16 || format == k16_16_16_16 || format == k16_16_Float ||
         format == k16_16_16_16_Float;
}

// A 16-bit element bound with each pair of components exchanged (BoundDeclType)
// reads natively once the halves of its words are swapped: makes 'type' the
// native swizzle and returns true if so.
bool UnswapHalves(DeclType &type, u32 components) {
  if (!Is16BitFormat(type.format))
    return false;
  for (u32 i = 0; i < 4; ++i) {
    const u8 expected = i < components ? u8(i ^ 1) : (i == 3 ? 5 : 4);
    if (type.swizzle[i] != expected)
      return false;
  }
  for (u32 i = 0; i < components; ++i)
    type.swizzle[i] = u8(i);
  return true;
}

// Host format when the swapped guest words can be read as-is.
plume::RenderFormat PassthroughFormat(const DeclType &type, const FormatInfo &info) {
  using F = plume::RenderFormat;
  const bool native = IsNativeSwizzle(type, info.components);
  switch (type.format) {
  case k32_Float: return native ? F::R32_FLOAT : F::UNKNOWN;
  case k32_32_Float: return native ? F::R32G32_FLOAT : F::UNKNOWN;
  case k32_32_32_Float: return native ? F::R32G32B32_FLOAT : F::UNKNOWN;
  case k32_32_32_32_Float: return native ? F::R32G32B32A32_FLOAT : F::UNKNOWN;
  case k16_16_Float: return native ? F::R16G16_FLOAT : F::UNKNOWN;
  case k16_16_16_16_Float: return native ? F::R16G16B16A16_FLOAT : F::UNKNOWN;
  case k16_16:
    if (!native || type.is_integer)
      return F::UNKNOWN;
    return type.is_signed ? F::R16G16_SNORM : F::R16G16_UNORM;
  case k16_16_16_16:
    if (!native || type.is_integer)
      return F::UNKNOWN;
    return type.is_signed ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM;
  case k8_8_8_8:
    if (type.is_integer)
      return F::UNKNOWN;
    if (native)
      return type.is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
    // D3DCOLOR: ZYXW over the swapped word is exactly BGRA.
    if (!type.is_signed && type.swizzle[0] == 2 && type.swizzle[1] == 1 && type.swizzle[2] == 0 &&
        type.swizzle[3] == 3)
      return F::B8G8R8A8_UNORM;
    return F::UNKNOWN;
  default:  // 2_10_10_10 and 10/11-bit formats have no plume equivalent
    return F::UNKNOWN;
  }
}

u32 SwapDword(u32 value, u32 endian) {
  switch (endian) {
  case 1: // 8in16
    return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
  case 2: // 8in32
    return (value << 24) | ((value << 8) & 0x00FF0000u) | ((value >> 8) & 0x0000FF00u) |
           (value >> 24);
  case 3: // 16in32
    return (value << 16) | (value >> 16);
  default:
    return value;
  }
}

// Guest words are stored big-endian in memory; the fetch constant's endian
// mode describes the swap from that storage to the GPU's word value.
template <u32 kEndian>
u32 LoadGuestWord(const u8 *p) {
  u32 raw;
  std::memcpy(&raw, p, 4);  // host (little-endian) view of the stored bytes
  return SwapDword(raw, kEndian);
}

float HalfToFloat(u16 h) {
  const u32 sign = u32(h >> 15) << 31;
  const u32 exponent = (h >> 10) & 0x1F;
  const u32 mantissa = h & 0x3FF;
  u32 bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {  // subnormal
      float f = std::ldexp(float(mantissa), -24);
      return (h >> 15) ? -f : f;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

float DecodeComponent(u32 word, u32 shift, u32 bits, const DeclType &type) {
  const u32 raw = bits == 32 ? word : (word >> shift) & ((1u << bits) - 1u);
  if (type.is_signed) {
    const int32_t value = bits == 32 ? int32_t(raw) : int32_t(raw << (32 - bits)) >> (32 - bits);
    if (type.is_integer)
      return float(value);
    const float max_value = bits == 32 ? 2147483647.0f : float((1u << (bits - 1)) - 1u);
    return std::max(float(value) / max_value, -1.0f);
  }
  if (type.is_integer)
    return float(raw);
  return float(raw) / (bits == 32 ? 4294967295.0f : float((1u << bits) - 1u));
}

// banjo-nx (2026-10-23, batch 6): DecodeComponent's result for every raw value of a 2, 8, 10 or 11-bit component, made
// with DecodeComponent itself (the same floats), for each of signed / integer: the packed formats' decodes read them
// instead of a float division (or a conversion and compare) per component.
struct ComponentTables {
  float b2[4][1u << 2];
  float b8[4][1u << 8];
  float b10[4][1u << 10];
  float b11[4][1u << 11];
};

const ComponentTables &PackedComponentTables() {
  static const ComponentTables *const tables = [] {
    auto *t = new ComponentTables;
    for (u32 which = 0; which < 4; ++which) {
      DeclType type{};
      type.is_signed = (which & 1) != 0;
      type.is_integer = (which & 2) != 0;
      for (u32 raw = 0; raw < (1u << 2); ++raw)
        t->b2[which][raw] = DecodeComponent(raw, 0, 2, type);
      for (u32 raw = 0; raw < (1u << 8); ++raw)
        t->b8[which][raw] = DecodeComponent(raw, 0, 8, type);
      for (u32 raw = 0; raw < (1u << 10); ++raw)
        t->b10[which][raw] = DecodeComponent(raw, 0, 10, type);
      for (u32 raw = 0; raw < (1u << 11); ++raw)
        t->b11[which][raw] = DecodeComponent(raw, 0, 11, type);
    }
    return t;
  }();
  return *tables;
}

inline u32 TableIndex(const DeclType &type) { return (type.is_signed ? 1u : 0u) | (type.is_integer ? 2u : 0u); }

#if defined(__aarch64__)
// banjo-nx (2026-10-28, per-draw night): four 16-bit components (as DecodeComponent takes them from the words: the low
// half first) to floats in one of the four modes (TableIndex: bit 0 signed, bit 1 integer). The same values as
// DecodeComponent: the integers are exact in a float, the divisions are the same IEEE divisions (lane by lane, the
// same rounding mode), and max(x, -1) is std::max's for a quotient that is never NaN.
template <u32 kMode>
inline float32x4_t Decode16x4(u32 w0, u32 w1) {
  const uint16x4_t h = vreinterpret_u16_u32(vset_lane_u32(w1, vdup_n_u32(w0), 1));
  if constexpr ((kMode & 1) != 0) {
    const float32x4_t v = vcvtq_f32_s32(vmovl_s16(vreinterpret_s16_u16(h)));
    if constexpr ((kMode & 2) != 0)
      return v;
    return vmaxq_f32(vdivq_f32(v, vdupq_n_f32(32767.0f)), vdupq_n_f32(-1.0f));
  } else {
    const float32x4_t v = vcvtq_f32_u32(vmovl_u16(h));
    if constexpr ((kMode & 2) != 0)
      return v;
    return vdivq_f32(v, vdupq_n_f32(65535.0f));
  }
}
// Every 16-bit value in every mode against DecodeComponent, once (the console's FPCR is the one this runs with).
template <u32 kMode>
bool Neon16ExactMode() {
  DeclType type{};
  type.is_signed = (kMode & 1) != 0;
  type.is_integer = (kMode & 2) != 0;
  for (u32 v = 0; v < 0x10000u; v += 4) {
    const u32 w0 = v | (v + 1) << 16, w1 = (v + 2) | (v + 3) << 16;
    float got[4];
    vst1q_f32(got, Decode16x4<kMode>(w0, w1));
    for (u32 i = 0; i < 4; ++i) {
      const float want = DecodeComponent(i < 2 ? w0 : w1, (i & 1) * 16, 16, type);
      if (std::memcmp(&got[i], &want, 4) != 0)
        return false;
    }
  }
  return true;
}
bool Neon16Exact() {
  static const bool exact = [] {
    const bool ok = Neon16ExactMode<0>() && Neon16ExactMode<1>() && Neon16ExactMode<2>() && Neon16ExactMode<3>();
    if (!ok)
      RGPU_WARN("16-bit vertex components: the NEON expansion differs from DecodeComponent; the scalar path is used");
    return ok;
  }();
  return exact;
}
#endif

#if defined(__aarch64__)
// banjo-nx (2026-10-23, batch 6): two or four halves (as HalfToFloat takes them from the words: the low half first) to
// floats with the NEON conversion: the same values (exact, subnormals included); a word holding an infinity or NaN half
// goes through HalfToFloat (the conversion would quiet a signalling NaN's payload).
inline bool HalvesFinite(u32 w) { return (w & 0x7C00u) != 0x7C00u && (w & 0x7C000000u) != 0x7C000000u; }
inline void HalvesToFloats(u32 w0, u32 w1, float out[4]) {
  const uint32x2_t words = vset_lane_u32(w1, vdup_n_u32(w0), 1);
  vst1q_f32(out, vcvt_f32_f16(vreinterpret_f16_u32(words)));
}
#endif

// Expands one element of 'count' vertices to four floats: 'decode' fills the
// format's components (before swizzle) of the first slots of c[6], which start
// as 0, 0, 0, 1 followed by the swizzle constants 0 and 1.
template <typename Decode>
void ExpandLoop(const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride, u32 count,
                const u8 swizzle[4], Decode decode) {
  u8 pick[4];
  for (u32 i = 0; i < 4; ++i)
    pick[i] = swizzle[i] < 6 ? swizzle[i] : 4;  // 4: 0, 5: 1; the rest read 0
  // banjo-nx (2026-10-23, batch 5): the identity swizzle (most elements) stores the four decoded slots as they are:
  // the indexed picks of the general loop were its hottest line (vertex_format.cpp:250, 4 % of the render thread at
  // high clocks). The same bytes.
  if (pick[0] == 0 && pick[1] == 1 && pick[2] == 2 && pick[3] == 3) {
    for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride) {
      float c[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f};
      decode(src, c);
      std::memcpy(dst, c, 16);
    }
    return;
  }
  for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride) {
    float c[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    decode(src, c);
    const float out[4] = {c[pick[0]], c[pick[1]], c[pick[2]], c[pick[3]]};
    std::memcpy(dst, out, sizeof(out));
  }
}

// Decodes an element's components with the format and endian mode fixed for
// the whole loop. Packed components count from the low bits of the swapped
// word, 16-bit ones too: with the usual 8in32 fetch, x is the second halfword
// in memory. The bound swizzle (BoundDeclType) or the shader's own (.yx,
// .yxwz) swaps them back.
template <u32 kEndian>
void ExpandElement(const HostVertexElement &e, const u8 *src, u32 src_stride, u8 *dst,
                   u32 dst_stride, u32 count) {
  const DeclType &type = e.type;
  auto word = [](const u8 *p, u32 i) { return LoadGuestWord<kEndian>(p + i * 4); };
  auto expand = [&](auto decode) {
    ExpandLoop(src, src_stride, dst, dst_stride, count, type.swizzle, decode);
  };
  switch (type.format) {
  // banjo-nx (2026-10-23, batch 6): the packed formats from PackedComponentTables (DecodeComponent's own results).
  case k8_8_8_8: {
    const float *t8 = PackedComponentTables().b8[TableIndex(type)];
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      for (u32 i = 0; i < 4; ++i)
        c[i] = t8[(w >> (i * 8)) & 0xFFu];
    });
    break;
  }
  case k2_10_10_10: {
    const ComponentTables &t = PackedComponentTables();
    const float *t10 = t.b10[TableIndex(type)], *t2 = t.b2[TableIndex(type)];
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = t10[w & 0x3FFu];
      c[1] = t10[(w >> 10) & 0x3FFu];
      c[2] = t10[(w >> 20) & 0x3FFu];
      c[3] = t2[w >> 30];
    });
    break;
  }
  case k10_11_11: {
    const ComponentTables &t = PackedComponentTables();
    const float *t11 = t.b11[TableIndex(type)], *t10 = t.b10[TableIndex(type)];
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = t11[w & 0x7FFu];
      c[1] = t11[(w >> 11) & 0x7FFu];
      c[2] = t10[w >> 22];
    });
    break;
  }
  case k11_11_10: {
    const ComponentTables &t = PackedComponentTables();
    const float *t11 = t.b11[TableIndex(type)], *t10 = t.b10[TableIndex(type)];
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = t10[w & 0x3FFu];
      c[1] = t11[(w >> 10) & 0x7FFu];
      c[2] = t11[w >> 21];
    });
    break;
  }
  case k16_16:
#if defined(__aarch64__)
    // banjo-nx (2026-10-28, per-draw night): Decode16x4 (the mode chosen once for the loop, not per component).
    if (Neon16Exact()) {
      const auto two = [&]<u32 kMode>() {
        expand([&](const u8 *p, float c[6]) {
          const u32 w = word(p, 0);
          vst1_f32(c, vget_low_f32(Decode16x4<kMode>(w, w)));
        });
      };
      switch (TableIndex(type)) {
      case 0: two.template operator()<0>(); break;
      case 1: two.template operator()<1>(); break;
      case 2: two.template operator()<2>(); break;
      default: two.template operator()<3>(); break;
      }
      break;
    }
#endif
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = DecodeComponent(w, 0, 16, type);
      c[1] = DecodeComponent(w, 16, 16, type);
    });
    break;
  case k16_16_16_16:
#if defined(__aarch64__)
    if (Neon16Exact()) {  // banjo-nx (2026-10-28, per-draw night)
      const auto four = [&]<u32 kMode>() {
        expand([&](const u8 *p, float c[6]) { vst1q_f32(c, Decode16x4<kMode>(word(p, 0), word(p, 1))); });
      };
      switch (TableIndex(type)) {
      case 0: four.template operator()<0>(); break;
      case 1: four.template operator()<1>(); break;
      case 2: four.template operator()<2>(); break;
      default: four.template operator()<3>(); break;
      }
      break;
    }
#endif
    expand([&](const u8 *p, float c[6]) {
      const u32 w0 = word(p, 0), w1 = word(p, 1);
      c[0] = DecodeComponent(w0, 0, 16, type);
      c[1] = DecodeComponent(w0, 16, 16, type);
      c[2] = DecodeComponent(w1, 0, 16, type);
      c[3] = DecodeComponent(w1, 16, 16, type);
    });
    break;
  case k16_16_Float:
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
#if defined(__aarch64__)
      if (HalvesFinite(w)) {  // banjo-nx (2026-10-23, batch 6)
        float f[4];
        HalvesToFloats(w, w, f);
        c[0] = f[0];
        c[1] = f[1];
        return;
      }
#endif
      c[0] = HalfToFloat(u16(w));
      c[1] = HalfToFloat(u16(w >> 16));
    });
    break;
  case k16_16_16_16_Float:
    expand([&](const u8 *p, float c[6]) {
      const u32 w0 = word(p, 0), w1 = word(p, 1);
#if defined(__aarch64__)
      if (HalvesFinite(w0) && HalvesFinite(w1)) {  // banjo-nx (2026-10-23, batch 6)
        HalvesToFloats(w0, w1, c);
        return;
      }
#endif
      c[0] = HalfToFloat(u16(w0));
      c[1] = HalfToFloat(u16(w0 >> 16));
      c[2] = HalfToFloat(u16(w1));
      c[3] = HalfToFloat(u16(w1 >> 16));
    });
    break;
  case k32:
  case k32_32:
  case k32_32_32_32: {
    FormatInfo info;
    GetFormatInfo(type.format, info);
    const u32 components = info.components;
    expand([&](const u8 *p, float c[6]) {
      for (u32 i = 0; i < components; ++i)
        c[i] = DecodeComponent(word(p, i), 0, 32, type);
    });
    break;
  }
  case k32_Float:
  case k32_32_Float:
  case k32_32_32_Float:
  case k32_32_32_32_Float: {
    FormatInfo info;
    GetFormatInfo(type.format, info);
    const u32 components = info.components;
    expand([&](const u8 *p, float c[6]) {
      for (u32 i = 0; i < components; ++i) {
        const u32 bits = word(p, i);
        std::memcpy(&c[i], &bits, 4);
      }
    });
    break;
  }
  default:
    expand([](const u8 *, float *) {});
    break;
  }
}

} // namespace

u32 BoundDeclType(u32 type) {
  if (((type >> 6) & 3) != 1)  // not 8in16 data
    return type;
  for (u32 i = 0; i < 4; ++i) {
    const u32 shift = 10 + 3 * i;
    const u32 swizzle = (type >> shift) & 7;
    if (swizzle <= 3)  // a component, not a constant 0 or 1
      type ^= 1u << shift;
  }
  return type;
}

DeclType DeclType::Decode(u32 type) {
  type = BoundDeclType(type);
  DeclType out;
  out.format = type & 0x3F;
  out.is_signed = (type >> 8) & 1;
  out.is_integer = (type >> 9) & 1;
  for (u32 i = 0; i < 4; ++i)
    out.swizzle[i] = u8((type >> (10 + 3 * i)) & 7);
  return out;
}

u32 FetchDeclType(u32 format, bool is_signed, bool is_integer) {
  FormatInfo info;
  if (!GetFormatInfo(format, info))
    return kDeclTypeUnused;
  u32 type = (format & 0x3F) | (is_signed ? 1u << 8 : 0u) | (is_integer ? 1u << 9 : 0u);
  for (u32 i = 0; i < 4; ++i) {
    const u32 swizzle = i < info.components ? i : (i == 3 ? 5u : 4u);  // 4: 0, 5: 1
    type |= swizzle << (10 + 3 * i);
  }
  return type;
}

const char *UsageSemantic(u8 usage) {
  return usage < std::size(kUsageSemantics) ? kUsageSemantics[usage] : "TEXCOORD";
}

HostVertexLayout BuildHostVertexLayout(const GuestVertexElement *elements, u32 count) {
  HostVertexLayout layout;
  for (u32 i = 0; i < count; ++i) {
    const GuestVertexElement &element = elements[i];
    const u32 stream = u16(element.Stream);
    if (u32(element.Type) == kDeclTypeUnused || stream >= kMaxVertexStreams)
      continue;

    HostVertexElement host;
    host.usage = element.Usage;
    host.usage_index = element.UsageIndex;
    if (!InputLocation(host.usage, host.usage_index, host.location))
      continue;  // not an input any N&B shader declares
    // A repeated semantic would make the input layout invalid; the first wins.
    const bool repeated = std::any_of(
        std::begin(layout.streams), std::end(layout.streams), [&](const HostVertexStream &s) {
          return std::any_of(s.elements.begin(), s.elements.end(),
                             [&](const HostVertexElement &e) { return e.location == host.location; });
        });
    if (repeated)
      continue;
    host.type = DeclType::Decode(u32(element.Type));
    FormatInfo info;
    if (!GetFormatInfo(host.type.format, info)) {
      RGPU_WARN("Vertex element {}{} uses unhandled vertex format {} (type {:08X})",
                UsageSemantic(host.usage), host.usage_index, host.type.format, u32(element.Type));
      continue;
    }
    host.guest_offset = u16(element.Offset);
    DeclType copy_type = host.type;
    host.swap_halves = UnswapHalves(copy_type, info.components);
    host.host_format = PassthroughFormat(copy_type, info);
    if (host.host_format == plume::RenderFormat::UNKNOWN)
      host.swap_halves = false;
    if (host.host_format != plume::RenderFormat::UNKNOWN) {
      // vivapinata-nx (2026-11-03, distant creatures dark): see FiniteWords.
      host.conversion = banjo::renut_gpu::VertexFiniteOn() && copy_type.format == k32_Float ? ElementConversion::Copy32Finite
                                                                                          : ElementConversion::Copy32;
      host.host_size = info.dwords * 4;
      host.components = info.components;
    } else {
      host.conversion = ElementConversion::ExpandFloat;
      host.host_format = plume::RenderFormat::R32G32B32A32_FLOAT;
      host.host_size = 16;
      host.components = 4;
    }
    layout.streams[stream].used = true;
    layout.used_mask |= 1u << stream;  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_stream_mask)
    layout.streams[stream].elements.push_back(host);
  }

  // Pack each stream's host elements in declaration order.
  for (u32 stream = 0; stream < kMaxVertexStreams; ++stream) {
    HostVertexStream &s = layout.streams[stream];
    u32 offset = 0;
    for (HostVertexElement &element : s.elements) {
      element.host_offset = offset;
      offset += element.host_size;
      layout.input_elements.emplace_back(UsageSemantic(element.usage), element.usage_index,
                                         element.location, element.host_format, stream,
                                         element.host_offset);
    }
    s.host_stride = offset;
  }

  // D3D12 rejects a pipeline whose vertex shader reads an input the layout
  // lacks, and which inputs a shader reads is not known here, so every other
  // location reads zeros from a stride-0 stream.
  struct Row {
    u8 usage;
    u8 index;
    u32 location;
  };
  static const Row kRows[] = {
#define RENUT_ROW(usage, index, location) {DeclUsage::usage, index, location},
      RENUT_VERTEX_INPUT_LOCATIONS(RENUT_ROW)
#undef RENUT_ROW
  };
  for (const Row &row : kRows) {
    const bool present =
        std::any_of(layout.input_elements.begin(), layout.input_elements.end(),
                    [&](const plume::RenderInputElement &e) { return e.location == row.location; });
    if (!present) {
      layout.input_elements.emplace_back(UsageSemantic(row.usage), row.index, row.location,
                                         plume::RenderFormat::R32G32B32A32_FLOAT, kZeroStreamSlot,
                                         0);
    }
  }
  return layout;
}

namespace {

template <u32 kEndian>
void SwapWordsAs(const u8 *src, u8 *dst, size_t words) {
  for (size_t i = 0; i < words; ++i) {
    u32 value;
    std::memcpy(&value, src + i * 4, 4);
    value = SwapDword(value, kEndian);
    std::memcpy(dst + i * 4, &value, 4);
  }
}

#if defined(__aarch64__)
// banjo-nx (banjo_renut_neon_swap): 16 bytes per NEON byte reverse, the same bytes SwapDword gives each word:
// 8in16 = rev16 of bytes, 8in32 = rev32 of bytes, 16in32 = rev32 of halfwords.
template <u32 kEndian>
inline uint8x16_t SwapVector(uint8x16_t v) {
  if constexpr (kEndian == 1)
    return vrev16q_u8(v);
  else if constexpr (kEndian == 2)
    return vrev32q_u8(v);
  else
    return vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
}

// banjo-nx (2026-10-28, per-draw night): SwapVector on half a vector (two words).
template <u32 kEndian>
inline uint8x8_t SwapVector64(uint8x8_t v) {
  if constexpr (kEndian == 1)
    return vrev16_u8(v);
  else if constexpr (kEndian == 2)
    return vrev32_u8(v);
  else
    return vreinterpret_u8_u16(vrev32_u16(vreinterpret_u16_u8(v)));
}

// The words a vector loop did not take are left to the caller.
template <u32 kEndian>
size_t SwapWordsNeon(const u8 *src, u8 *dst, size_t words) {
  size_t i = 0;
  for (; i + 8 <= words; i += 8) {
    const uint8x16_t a = vld1q_u8(src + i * 4), b = vld1q_u8(src + i * 4 + 16);
    vst1q_u8(dst + i * 4, SwapVector<kEndian>(a));
    vst1q_u8(dst + i * 4 + 16, SwapVector<kEndian>(b));
  }
  for (; i + 4 <= words; i += 4)
    vst1q_u8(dst + i * 4, SwapVector<kEndian>(vld1q_u8(src + i * 4)));
  return i;
}
#endif

// LoadGuestWord over 'words' consecutive words, with the endian mode hoisted
// out of the loop.
void SwapWords(const u8 *src, u8 *dst, size_t words, u32 endian) {
#if defined(__aarch64__)
  if (endian && banjo::renut_gpu::NeonSwapOn()) {
    const size_t done = endian == 1   ? SwapWordsNeon<1>(src, dst, words)
                        : endian == 2 ? SwapWordsNeon<2>(src, dst, words)
                                      : SwapWordsNeon<3>(src, dst, words);
    src += done * 4;
    dst += done * 4;
    words -= done;
  }
#endif
  switch (endian) {
  case 1:
    SwapWordsAs<1>(src, dst, words);
    break;
  case 2:
    SwapWordsAs<2>(src, dst, words);
    break;
  case 3:
    SwapWordsAs<3>(src, dst, words);
    break;
  default:
    std::memcpy(dst, src, words * 4);
    break;
  }
}

template <u32 kEndian, u32 kWords>
void CopyWordsLoop(const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride, u32 count) {
#if defined(__aarch64__)
  // banjo-nx (banjo_renut_neon_swap): a four-word element (positions, normals as float4) is one vector.
  if constexpr (kWords == 4 && kEndian != 0) {
    if (banjo::renut_gpu::NeonSwapOn()) {
      for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride)
        vst1q_u8(dst, SwapVector<kEndian>(vld1q_u8(src)));
      return;
    }
  }
  // banjo-nx (2026-10-28, per-draw night): two words as one 64-bit reverse, three as that and one word.
  if constexpr ((kWords == 2 || kWords == 3) && kEndian != 0) {
    if (banjo::renut_gpu::NeonSwapOn()) {
      for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride) {
        vst1_u8(dst, SwapVector64<kEndian>(vld1_u8(src)));
        if constexpr (kWords == 3)
          SwapWordsAs<kEndian>(src + 8, dst + 8, 1);
      }
      return;
    }
  }
#endif
  for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride)
    SwapWordsAs<kEndian>(src, dst, kWords);
}

template <u32 kEndian>
void CopyElementWordsAs(const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride, u32 count,
                        u32 words) {
  switch (words) {
  case 1:
    CopyWordsLoop<kEndian, 1>(src, src_stride, dst, dst_stride, count);
    break;
  case 2:
    CopyWordsLoop<kEndian, 2>(src, src_stride, dst, dst_stride, count);
    break;
  case 3:
    CopyWordsLoop<kEndian, 3>(src, src_stride, dst, dst_stride, count);
    break;
  case 4:
    CopyWordsLoop<kEndian, 4>(src, src_stride, dst, dst_stride, count);
    break;
  default:
    for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride)
      SwapWordsAs<kEndian>(src, dst, words);
    break;
  }
}

// SwapWords of one element of 'count' vertices.
void CopyElementWords(const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride, u32 count,
                      u32 words, u32 endian) {
  switch (endian & 3) {
  case 1:
    CopyElementWordsAs<1>(src, src_stride, dst, dst_stride, count, words);
    break;
  case 2:
    CopyElementWordsAs<2>(src, src_stride, dst, dst_stride, count, words);
    break;
  case 3:
    CopyElementWordsAs<3>(src, src_stride, dst, dst_stride, count, words);
    break;
  default:
    CopyElementWordsAs<0>(src, src_stride, dst, dst_stride, count, words);
    break;
  }
}

// vivapinata-nx (2026-11-03, distant creatures dark): Viva Pinata's creature meshes keep a specular power per vertex as a 32-bit
// float (an element of its own), with +infinity (0x7F800000) on vertices that have no highlight, whose highlight colour is black. On
// the Xenos 0 times anything is 0, so the pow the pixel shader does, exp2(power * log2(x)) * colour, comes to 0 there. Our shaders
// follow IEEE rules: infinity times 0 is NaN, and a NaN in the highlight made the pixel's whole colour NaN. The largest finite float
// gives the same powers without a NaN, and costs one test a vertex for this rare kind of element, on the CPU.
void FiniteWords(u8 *dst, u32 dst_stride, u32 count) {
  for (u32 i = 0; i < count; ++i, dst += dst_stride) {
    u32 word;
    std::memcpy(&word, dst, 4);
    if ((word & 0x7F800000u) == 0x7F800000u) {
      word = (word & 0x80000000u) | 0x7F7FFFFFu;
      std::memcpy(dst, &word, 4);
    }
  }
}

// renut-ultimate (audit, 2026-11-07): no word FiniteWords would change in one element of 'count' guest vertices (the
// element's first word at 'src'), read through the stream's endian mode.
bool ElementAllFinite(const u8 *src, u32 src_stride, u32 count, u32 endian) {
  u32 found = 0;
  for (u32 i = 0; i < count; ++i, src += src_stride) {
    u32 word;
    std::memcpy(&word, src, 4);
    word = SwapDword(word, endian);
    found |= u32((word & 0x7F800000u) == 0x7F800000u);
  }
  return !found;
}

} // namespace

void ConvertVertices(const HostVertexStream &stream, const u8 *guest, u32 guest_stride,
                     u32 vertex_count, u32 endian, u8 *host) {
  ConvertVertices(stream.elements.data(), u32(stream.elements.size()), stream.host_stride, guest, guest_stride,
                  vertex_count, endian, host);
}

void ConvertVertices(const HostVertexElement *elements, u32 element_count, u32 host_stride_in, const u8 *guest,
                     u32 guest_stride, u32 vertex_count, u32 endian, u8 *host) {
  const std::span<const HostVertexElement> stream_elements(elements, element_count);
  bool copy_only = true, same_layout = host_stride_in == guest_stride && guest_stride % 4 == 0;
  bool finite_elements = false;  // renut-ultimate (audit, 2026-11-07)
  for (const HostVertexElement &element : stream_elements) {
    const bool finite = element.conversion == ElementConversion::Copy32Finite;
    finite_elements |= finite;
    copy_only &= (element.conversion == ElementConversion::Copy32 || finite) && !element.swap_halves;
    same_layout &= element.host_offset == element.guest_offset;
  }
  // renut-ultimate (audit, 2026-11-07): port 15's finite single floats leave the stream the one-pass swap below when none of
  // the draw's vertices has a word FiniteWords would change (the chunked path otherwise, which applies it).
  if (copy_only && same_layout && finite_elements) {
    for (const HostVertexElement &element : stream_elements) {
      if (element.conversion == ElementConversion::Copy32Finite &&
          !ElementAllFinite(guest + element.guest_offset, guest_stride, vertex_count, endian)) {
        copy_only = false;
        break;
      }
    }
  } else if (finite_elements) {
    copy_only = false;
  }
  // Swapping the halves after any endian swap is the endian mode ^ 3
  // (8in32 <-> 8in16, none <-> 16in32).
  constexpr u32 kSwapHalves = 3;
  if (copy_only && same_layout) {
    // The host vertices are the guest vertices with their words swapped.
    // (Elements whose halves swap too take the chunked path below: a second
    // pass would read back upload memory, which is uncached.)
    SwapWords(guest, host, size_t(vertex_count) * guest_stride / 4, endian);
    return;
  }
  // One element of every vertex at a time, each in a loop with its format and
  // endian mode fixed: deciding them per vertex cost more than the conversion.
  // A chunk of vertices is built in cache and then copied out in one go: the
  // destination is upload memory, write-combined, where the scattered writes
  // of one element would be slow.
  constexpr u32 kChunkBytes = 16 * 1024;
  alignas(64) u8 chunk[kChunkBytes];
  const u32 host_stride = host_stride_in;
  if (!host_stride)
    return;
  const u32 per_chunk = std::max(1u, kChunkBytes / host_stride);
  for (u32 first = 0; first < vertex_count; first += per_chunk) {
    const u32 count = std::min(per_chunk, vertex_count - first);
    const u8 *chunk_src = guest + size_t(first) * guest_stride;
    for (const HostVertexElement &element : stream_elements) {
      const u8 *src = chunk_src + element.guest_offset;
      u8 *dst = chunk + element.host_offset;
      if (element.conversion == ElementConversion::ExpandFloat) {
        switch (endian & 3) {
        case 1:
          ExpandElement<1>(element, src, guest_stride, dst, host_stride, count);
          break;
        case 2:
          ExpandElement<2>(element, src, guest_stride, dst, host_stride, count);
          break;
        case 3:
          ExpandElement<3>(element, src, guest_stride, dst, host_stride, count);
          break;
        default:
          ExpandElement<0>(element, src, guest_stride, dst, host_stride, count);
          break;
        }
      } else {
        // host little-endian words
        CopyElementWords(src, guest_stride, dst, host_stride, count, element.host_size / 4,
                         element.swap_halves ? endian ^ kSwapHalves : endian);
        if (element.conversion == ElementConversion::Copy32Finite)  // vivapinata-nx (2026-11-03)
          FiniteWords(dst, host_stride, count);
      }
    }
    std::memcpy(host + size_t(first) * host_stride, chunk, size_t(count) * host_stride);
  }
}

} // namespace renut::gpu
