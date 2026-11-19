#pragma once

// culdcept-nx (2026-11-19): shader containers of version 0x10 (flags 0x102A10xx: XDK 2.0.3424's run-time compiler, "Microsoft
// (R) Xbox 360 Shader Compiler 2.0.3424.0", which Culdcept Saga runs on data\shader.fx, and the containers in its data)
// turned into the version 0x11 layout the translator reads. The cache key stays the XXH3 of the original container (what
// the game hands D3DDevice_Create*Shader), so main.cpp hashes first and converts after, as for container_0e.h.
//
// Version 0x10, from Culdcept Saga's VS00 / PS00 (docs/culdcept-notes.md, "Shaders"):
//   header (0x24 bytes):  0x11's: flags, virtualSize, physicalSize, fieldC, constantTableOffset, definitionTableOffset,
//                         shaderOffset, field1C, field20
//   constant table:       0x11's (a size word, then the D3DX constant table)
//   shader info:          0x11's (physicalOffset, size, field8, fieldC, field10, interpolatorInfo, then the pixel or vertex
//                         shader's fields), the microcode at physicalOffset of the physical part, the literals before it
//   definition table:     field0, field4, field8, fieldC, two more words, size, one more word, then the definitions
//                         (float4 {u16 register, u16 count, u32 physicalOffset}, 0, int4, 0, bool, 0): size counts from
//                         there. 0x11's has the size right after fieldC and the definitions after it.
// So only the definition table is rewritten, at the end of the virtual part; the physical part follows it.

#include <cstdint>
#include <cstring>
#include <vector>

namespace container_10 {

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

constexpr uint32_t kSizeAt = 0x18;         // the definition table's size word
constexpr uint32_t kDefinitionsAt = 0x20;  // its first definition

inline bool IsContainer10(const uint8_t *p, size_t available) {
  if (available < 0x24 || (Rd(p, 0) & 0xFFFFFF00u) != 0x102A1000u)
    return false;
  const uint32_t virt = Rd(p, 4), phys = Rd(p, 8);
  const uint32_t consts = Rd(p, 16), defs = Rd(p, 20), info = Rd(p, 24);
  if (!(virt >= 0x24 && virt < 0x10000 && phys > 0 && phys < 0x100000 && size_t(virt) + phys <= available &&
        consts < virt && defs < virt && info >= 0x24 && info + 0x18 <= virt))
    return false;
  return !defs || defs + kDefinitionsAt + Rd(p, defs + kSizeAt) <= virt;
}

inline std::vector<uint8_t> Convert(const uint8_t *src) {
  const uint32_t virt = Rd(src, 4), phys = Rd(src, 8);
  const uint32_t defs = Rd(src, 20);
  std::vector<uint8_t> out(src, src + virt);
  uint32_t new_defs = 0;
  if (defs) {
    const uint32_t size = Rd(src, defs + kSizeAt);
    new_defs = uint32_t(out.size());
    for (uint32_t k = 0; k < 16; k += 4)  // field0, field4, field8, fieldC
      Wr(out, new_defs + k, Rd(src, defs + k));
    Wr(out, new_defs + 16, size);
    out.resize(new_defs + 20 + size);
    std::memcpy(out.data() + new_defs + 20, src + defs + kDefinitionsAt, size);
  }
  const uint32_t new_virt = uint32_t(out.size());
  Wr(out, 0, (Rd(src, 0) & 0xFFFF00FFu) | 0x1100u);
  Wr(out, 4, new_virt);
  Wr(out, 20, new_defs);
  out.insert(out.end(), src + virt, src + virt + phys);
  return out;
}

}  // namespace container_10
