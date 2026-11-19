// banjo-nx - what a Xenos shader uses, read from its microcode.
//
// Most of Nuts & Bolts' shader containers ship without a constant table (CTAB): 495 of the first 518 harvested.
// XenosRecomp and the native renderer took the samplers (texture fetch slots and their dimension) and the bool
// constants from the CTAB, so for those shaders they are read from the microcode instead: walk the control flow,
// visit every instruction of every exec block, and note the texture fetches and the bool-conditioned branches.
//
// Header-only and dependency-free: used by the shader tools (shaders/nfsmw_hlsl.cpp) and by the renderer
// (nfsmw_nativo_shaders.cpp). Layouts follow XenosRecomp's shader_code.h (ControlFlowInstruction,
// TextureFetchInstruction). `code` is the microcode in host order (already byte-swapped), `words` its length.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>

namespace banjo::ucode {

struct Uso {
  std::map<uint32_t, uint32_t> texturas;  // fetch constant slot -> TextureDimension (0 1D, 1 2D, 2 3D, 3 cube)
  std::set<uint32_t> booleanos;           // bool constant addresses the control flow tests
  std::set<uint32_t> exportes;            // ALU instructions (index) that export (exportData, dword 0 bit 15)
  bool valido = false;
};

inline Uso Escanear(const uint32_t* code, uint32_t words) {
  Uso uso;
  if (!code || words < 3) return uso;
  // The control flow comes first, two 48-bit instructions per 3 words. It ends where the first exec's
  // instructions begin (address * 3 words), as in XenosRecomp's first pass.
  uint32_t fin_cf = words;
  struct Exec {
    uint32_t direccion, cuenta, secuencia;
  };
  Exec execs[512];
  uint32_t n_execs = 0;
  for (uint32_t w = 0; w + 2 < words && w < fin_cf; w += 3) {
    const uint64_t a = uint64_t(code[w]) | (uint64_t(code[w + 1] & 0xFFFF) << 32);
    const uint64_t b = uint64_t((code[w + 1] >> 16) | (code[w + 2] << 16)) | (uint64_t(code[w + 2] >> 16) << 32);
    for (uint64_t v : {a, b}) {
      const uint32_t opcode = uint32_t(v >> 44) & 0xF;
      switch (opcode) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14: {  // exec family
          const uint32_t direccion = uint32_t(v) & 0xFFF;
          const uint32_t cuenta = uint32_t(v >> 12) & 0x7;
          const uint32_t secuencia = uint32_t(v >> 16) & 0xFFF;
          if (direccion) fin_cf = std::min(fin_cf, direccion * 3);
          if (cuenta && n_execs < 512) execs[n_execs++] = {direccion, cuenta, secuencia};
          // CondExec(End) and CondExecPredClean(End) test a bool constant (XenosRecomp reads condExec for all four).
          if (opcode == 3 || opcode == 4 || opcode == 13 || opcode == 14) uso.booleanos.insert(uint32_t(v >> 34) & 0xFF);
          break;
        }
        case 9:    // CondCall
        case 11: {  // CondJmp
          const bool incondicional = (v >> 13) & 1, predicado = (v >> 14) & 1;
          if (!incondicional && !predicado) uso.booleanos.insert(uint32_t(v >> 34) & 0xFF);
          break;
        }
        default:
          break;
      }
    }
  }
  for (uint32_t e = 0; e < n_execs; ++e) {
    for (uint32_t i = 0; i < execs[e].cuenta; ++i) {
      const uint32_t w = (execs[e].direccion + i) * 3;
      if (w + 2 >= words) continue;
      if (!((execs[e].secuencia >> (i * 2)) & 1)) {  // ALU
        if ((code[w] >> 15) & 1) uso.exportes.insert(execs[e].direccion + i);
        continue;
      }
      const uint32_t opcode = code[w] & 0x1F;
      // TextureFetch, GetTextureBorderColorFrac, GetTextureComputedLod, GetTextureWeights: they name a texture.
      if (opcode == 1 || opcode == 16 || opcode == 17 || opcode == 19) {
        const uint32_t slot = (code[w] >> 20) & 0x1F;
        const uint32_t dimension = (code[w + 2] >> 14) & 0x3;
        uso.texturas.emplace(slot, dimension);
      }
    }
  }
  uso.valido = true;
  return uso;
}

// D3DXPARAMETER_TYPE of a sampler of that dimension (what a CTAB would say): Sampler1D 11, 2D 12, 3D 13, Cube 14.
inline uint16_t TipoSampler(uint32_t dimension) {
  return uint16_t(11 + std::min<uint32_t>(dimension, 3));
}

}  // namespace banjo::ucode
