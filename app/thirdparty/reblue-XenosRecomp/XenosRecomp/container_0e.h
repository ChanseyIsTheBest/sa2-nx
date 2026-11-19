#pragma once

// feedingfrenzy-nx (2026-10-26): shader containers of version 0x0E (XDK 2.0.2571, Feeding Frenzy's run-time shader
// compiler) turned into the version 0x11 layout the translator reads. The cache key stays the XXH3 of the original
// container (what the game hands D3DDevice_Create*Shader), so main.cpp hashes first and converts after.
//
// Version 0x0E, from the five containers Feeding Frenzy creates (docs/feedingfrenzy-notes.md, "Shaders"):
//   header (0x18 bytes): flags 0x102A0Exx (low bit 1 = vertex shader), virtualSize, physicalSize,
//                        definitionTableOffset, constantTableOffset, shaderOffset
//   constant table:      as 0x11 (a size word, then the D3DX constant table; its offsets are relative to itself)
//   definition table:    as 0x11's header (field0, field4, field8, fieldC, size), then a word, then each float4
//                        literal inline: {u16 register, u16 count of components} followed by the values, then 0
//   shader info:         pixel:  field8, fieldC, 0, 0, interpolatorInfo, 0, field18, outputs, interpolators[]
//                        vertex: field8, fieldC, 0, 0, interpolatorInfo, 0, field18, vertexElementCount, field20, 0,
//                                then field18 entries, the vertex elements, the interpolators and field20 entries
//                        (0x11 has physicalOffset and size in front, field10 where 0x0E has interpolatorInfo, and no
//                        word after field20); the microcode is the whole physical part.
// Version 0x11 keeps literal values in the physical part (Float4Definition.physicalOffset): they are appended there.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace container_0e {

inline uint32_t Rd(const uint8_t *p, size_t o) {
  return (uint32_t(p[o]) << 24) | (uint32_t(p[o + 1]) << 16) | (uint32_t(p[o + 2]) << 8) | uint32_t(p[o + 3]);
}

inline void Wr(std::vector<uint8_t> &v, size_t o, uint32_t x) {
  if (v.size() < o + 4)
    v.resize(o + 4);
  v[o] = uint8_t(x >> 24);
  v[o + 1] = uint8_t(x >> 16);
  v[o + 2] = uint8_t(x >> 8);
  v[o + 3] = uint8_t(x);
}

inline bool IsContainer0E(const uint8_t *p, size_t available) {
  if (available < 0x18 || (Rd(p, 0) & 0xFFFFFF00u) != 0x102A0E00u)
    return false;
  const uint32_t virt = Rd(p, 4), phys = Rd(p, 8);
  const uint32_t defs = Rd(p, 12), consts = Rd(p, 16), info = Rd(p, 20);
  return virt >= 0x18 && virt < 0x10000 && phys > 0 && phys < 0x100000 && size_t(virt) + phys <= available &&
         defs < virt && consts < virt && info >= 0x18 && info < virt;
}

// kameo-nx (2026-11-02): a 0x0E definition is {u16 offset, u16 count}: the byte offset from the device's constants (device
// + 0x480) and a count of WORDS, as D3DDevice_SetPixelShader (Kameo's sub_820CA330) copies them (`rlwinm r30,r11,2`,
// memcpy to r28 + offset). So float registers are (offset - 0x300) / 16, 0-255 vertex and 256-511 pixel (0x12F0 is c255,
// 0x1300 the pixel shaders' c0, Renut's register numbers), and integer constants start at 0x2320.
constexpr uint32_t kIntConstants = 0x2320;
inline bool LiteralIsFloat(uint32_t field) {
  return (field & 0xFu) == 0 && field >= 0x300u && field < 0x2300u;
}
inline bool LiteralIsInt(uint32_t field) {
  return field >= kIntConstants && field < 0x23A0u && (field & 3u) == 0;
}
inline uint32_t LiteralRegister(uint32_t field) {
  return (field - 0x300u) >> 4;
}

inline std::vector<uint8_t> Convert(const uint8_t *src) {
  const bool vertex = (Rd(src, 0) & 1u) != 0;
  const uint32_t virt = Rd(src, 4), phys = Rd(src, 8);
  const uint32_t defs = Rd(src, 12), consts = Rd(src, 16), info = Rd(src, 20);
  constexpr uint32_t kShift = 0x24 - 0x18;

  // Physical part: the microcode, then any literal values moved out of the definition table.
  std::vector<uint8_t> physical(src + virt, src + virt + phys);
  struct Literal {
    uint32_t reg, count, offset;
  };
  struct IntLiteral {
    uint32_t field;
    std::vector<uint32_t> values;
  };
  std::vector<Literal> literals;
  std::vector<IntLiteral> int_literals;
  if (defs) {
    const uint32_t size = Rd(src, defs + 16);
    size_t o = defs + 20 + 4;  // past the header and the word before the first literal
    const size_t end = defs + 20 + size;
    while (o + 4 <= end && o + 4 <= virt) {
      const uint32_t head = Rd(src, o);
      if (!head)
        break;
      const uint32_t field = head >> 16;
      const uint32_t words = head & 0xFFFFu;
      if (LiteralIsFloat(field) && (words & 3u) == 0) {
        // renut-ultimate (2026-11-08, found by rr6-nx's merge): the 0x11 table's float4 definition counts WORDS, as 0x0E's
        // does: the translator reads (count + 3) / 4 registers (shader_recompiler.cpp). words / 4 here kept only the first
        // register of a literal of several (Ridge Racer 6's c253-c255 of 22D0/12: c254 and c255 read the game's registers,
        // its race drawn black).
        Literal l{LiteralRegister(field), words, uint32_t(physical.size())};
        for (uint32_t k = 0; k < words; ++k) {
          const uint32_t value = o + 4 + 4 * k + 4 <= virt ? Rd(src, o + 4 + 4 * k) : 0;
          physical.push_back(uint8_t(value >> 24));
          physical.push_back(uint8_t(value >> 16));
          physical.push_back(uint8_t(value >> 8));
          physical.push_back(uint8_t(value));
        }
        literals.push_back(l);
      } else if (LiteralIsInt(field)) {
        IntLiteral l{field, {}};
        for (uint32_t k = 0; k < words; ++k)
          l.values.push_back(o + 4 + 4 * k + 4 <= virt ? Rd(src, o + 4 + 4 * k) : 0);
        int_literals.push_back(std::move(l));
      } else {
        std::fprintf(stderr, "container_0e: literal at %04X (%u words) is not a float or integer constant\n", field, words);
      }
      o += 4 + 4 * size_t(words);
    }
  }

  // Virtual part: the 0x24-byte header, then 0x0E's body from 0x18 on (the constant table stays where it was,
  // kShift later), then a rebuilt definition table and shader info at the end.
  std::vector<uint8_t> out(0x24 + (virt - 0x18));
  std::memcpy(out.data() + 0x24, src + 0x18, virt - 0x18);
  uint32_t new_defs = 0;
  if (!literals.empty() || !int_literals.empty()) {
    new_defs = uint32_t(out.size());
    Wr(out, new_defs + 0, Rd(src, defs + 0));
    Wr(out, new_defs + 4, Rd(src, defs + 4));
    Wr(out, new_defs + 8, Rd(src, defs + 8));
    Wr(out, new_defs + 12, Rd(src, defs + 12));
    size_t o = new_defs + 20;
    for (const auto &l : literals) {
      Wr(out, o, (l.reg << 16) | (l.count & 0xFFFFu));
      Wr(out, o + 4, l.offset);
      o += 8;
    }
    Wr(out, o, 0);  // end of the float4 literals
    o += 4;
    for (const auto &l : int_literals) {  // one (register, count) word, then the values (shader_recompiler.cpp)
      Wr(out, o, (l.field << 16) | uint32_t(l.values.size()));
      o += 4;
      for (uint32_t value : l.values) {
        Wr(out, o, value);
        o += 4;
      }
    }
    Wr(out, o, 0);  // end of the int4 literals
    o += 4;
    Wr(out, o, 0);  // no bool literals
    o += 4;
    Wr(out, new_defs + 16, uint32_t(o - (new_defs + 20)));
  }
  const uint32_t new_info = uint32_t(out.size());
  Wr(out, new_info + 0, 0);     // physicalOffset: the microcode starts the physical part
  Wr(out, new_info + 4, phys);  // size: all of 0x0E's physical part
  Wr(out, new_info + 8, Rd(src, info + 0));    // field8
  Wr(out, new_info + 12, Rd(src, info + 4));   // fieldC
  Wr(out, new_info + 16, 0);                   // field10
  Wr(out, new_info + 20, Rd(src, info + 16));  // interpolatorInfo
  Wr(out, new_info + 24, Rd(src, info + 24));  // field18
  Wr(out, new_info + 28, Rd(src, info + 28));  // vertex: vertexElementCount; pixel: outputs
  size_t rest = info + 32;
  size_t dst = new_info + 32;
  if (vertex) {
    Wr(out, dst, Rd(src, info + 32));  // field20
    dst += 4;
    rest = info + 40;  // past field20 and 0x0E's extra word
  }
  for (; rest + 4 <= virt; rest += 4, dst += 4)
    Wr(out, dst, Rd(src, rest));

  const uint32_t new_virt = uint32_t(out.size());
  Wr(out, 0, (Rd(src, 0) & 0xFFFF00FFu) | 0x1100u);
  Wr(out, 4, new_virt);
  Wr(out, 8, uint32_t(physical.size()));
  Wr(out, 12, 0);                                  // fieldC
  Wr(out, 16, consts ? consts + kShift : 0);       // constantTableOffset
  Wr(out, 20, new_defs);                           // definitionTableOffset
  Wr(out, 24, new_info);                           // shaderOffset
  Wr(out, 28, 0);                                  // field1C
  Wr(out, 32, 0);                                  // field20
  out.insert(out.end(), physical.begin(), physical.end());
  return out;
}

}  // namespace container_0e
