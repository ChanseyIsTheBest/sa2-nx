// renut-ultimate (2026-11-16, port 145; tw2de_nx's, 2026-10-31): see fetch_filters.h.
#include "fetch_filters.h"

#include <cstring>

namespace renut::gpu {
namespace {

uint32_t Be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Control flow opcodes that run a clause of ALU and fetch instructions (exec, cond exec, pred exec and their "end"
// forms): address (bits 0-11), count (12-14) and sequence (16-27, two bits an instruction, bit 0 = fetch) in the low
// word, the opcode in bits 12-15 of the high one.
bool IsExec(uint32_t opcode) { return (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14; }

}  // namespace

FetchFilterOverrides ScanFetchFilters(const uint8_t *container) {
  FetchFilterOverrides out;
  if (!container)
    return out;
  const uint32_t flags = Be32(container);
  const uint32_t virtual_size = Be32(container + 4), physical_size = Be32(container + 8);
  const uint32_t shader_offset = Be32(container + 0x18);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || virtual_size > 0x10000 || physical_size > 0x100000 ||
      shader_offset + 8 > virtual_size)
    return out;
  const uint32_t physical_offset = Be32(container + shader_offset), code_size = Be32(container + shader_offset + 4);
  if (physical_offset > physical_size || code_size > physical_size - physical_offset)
    return out;
  const uint8_t *code = container + virtual_size + physical_offset;
  const uint32_t code_words = code_size / 4;

  bool seen[32] = {};
  uint32_t value[32] = {};
  uint32_t fetched = 0;
  // The control flow comes first, a pair of 48-bit instructions in 3 words, up to the first clause's instructions.
  uint32_t cf_end = code_words;
  for (uint32_t w = 0; w + 3 <= cf_end; w += 3) {
    const uint32_t d0 = Be32(code + 4 * w), d1 = Be32(code + 4 * (w + 1)), d2 = Be32(code + 4 * (w + 2));
    const uint32_t lo[2] = {d0, (d1 >> 16) | (d2 << 16)};
    const uint32_t hi[2] = {d1 & 0xFFFF, d2 >> 16};
    for (int k = 0; k < 2; ++k) {
      const uint32_t opcode = (hi[k] >> 12) & 15;
      if (!IsExec(opcode))
        continue;
      const uint32_t address = lo[k] & 0xFFF, count = (lo[k] >> 12) & 7;
      uint32_t sequence = (lo[k] >> 16) & 0xFFF;
      if (address != 0 && address * 3 < cf_end)
        cf_end = address * 3;
      for (uint32_t i = 0; i < count; ++i, sequence >>= 2) {
        if (!(sequence & 1))
          continue;  // an ALU instruction
        const uint32_t at = (address + i) * 3;
        if (at + 3 > code_words)
          break;
        const uint32_t f0 = Be32(code + 4 * at), f1 = Be32(code + 4 * (at + 1));
        if ((f0 & 31) != 0)
          fetched |= 1u << ((f0 >> 20) & 31);  // tw2de_nx (2026-11-14): every fetch that is not a vertex fetch
        if ((f0 & 31) != 1)
          continue;  // not a texture fetch (a vertex fetch, a LOD or gradient get or set)
        const uint32_t slot = (f0 >> 20) & 31;
        const uint32_t mag = (f1 >> 12) & 3, min = (f1 >> 14) & 3, mip = (f1 >> 16) & 3, aniso = (f1 >> 18) & 7;
        const uint32_t v = mag | min << 2 | mip << 4 | aniso << 6;
        if (!seen[slot]) {
          seen[slot] = true;
          value[slot] = v;
        } else if (value[slot] != v) {
          out.conflicts |= 1u << slot;
        }
      }
    }
  }
  for (uint32_t r = 0; r < 32; ++r) {
    if (!seen[r] || (out.conflicts >> r) & 1u)
      continue;
    const uint32_t mag = value[r] & 3, min = (value[r] >> 2) & 3, mip = (value[r] >> 4) & 3, aniso = value[r] >> 6;
    uint32_t clear = 0, set = 0;
    if (mag != 3) {
      clear |= 3u << 19;
      set |= mag << 19;
    }
    if (min != 3) {
      clear |= 3u << 21;
      set |= min << 21;
    }
    if (mip != 3) {
      clear |= 3u << 23;
      set |= mip << 23;
    }
    if (aniso != 7) {
      clear |= 7u << 25;
      set |= aniso << 25;
    }
    if (clear) {
      out.slots |= 1u << r;
      out.clear[r] = clear;
      out.set[r] = set;
    }
  }
  out.fetched = fetched;
  return out;
}

void MergeFetchFilters(FetchFilterOverrides &into, const FetchFilterOverrides &from) {
  for (uint32_t m = from.slots; m; m &= m - 1) {
    const uint32_t r = uint32_t(__builtin_ctz(m));
    into.slots |= 1u << r;
    into.clear[r] = from.clear[r];
    into.set[r] = from.set[r];
  }
  into.conflicts |= from.conflicts;
}

}  // namespace renut::gpu
