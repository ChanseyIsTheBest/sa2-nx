// masseffect - native renderer, shader identification: identify in the native shader library the
// shaders the game's D3D uploads to the ring (PM4_IM_LOAD and
// IM_LOAD_IMMEDIATE).
//
// The ring only carries the microcode. The library stores the original
// compiled container (header, constant table, definitions and microcode)
// with its SPIR-V translation made by XenosRecomp.
//
// Pixel shaders arrive as is and are identified by their microcode. Vertex
// shaders do not: D3D reorders the fetches, changes their swizzles and
// nulls the outputs the pixel shader does not read (measured), so their
// identity comes from the device objects (the D3D object table) and only
// the vertex input is read from the patched microcode.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace masseffect::native {
struct Shader;
struct StatsPreload;
}

namespace masseffect::native {

// Vertex declaration element of the container (XenosRecomp shader.h).
struct ElementVertex {
  uint16_t instruction = 0;  // index of the fetch instruction in the microcode
  uint8_t usage = 0;           // DeclUsage: 0 position, 3 normal, 5 texcoord, 10 color...
  uint8_t usage_index = 0;
};

// Sampler from the constant table: the register is the texture's fetch constant
// in pixel shaders.
struct SamplerShader {
  uint16_t register_value = 0;
  uint16_t type = 0;  // D3DXPARAMETER_TYPE: 10-12 = 2D, 13 = 3D, 14 = cube
};

struct ShaderEntry {
  const masseffect::native::Shader* shader = nullptr;  // original container and SPIR-V
  bool vertices = false;
  // Direct3D's own shader (no constant table): vertex input and interpolators linked by
  // register. Direct3D rewrites these vertex shaders per vertex declaration, so the variant the ring loads
  // is its own library entry (found by microcode), not the one of the object.
  bool binding_per_register = false;
  uint32_t number = 0;  // position in the library, for the reports
  std::vector<uint32_t> microcode;  // in host byte order, with the fetches masked
  uint64_t fingerprint = 0;                // XXH3 of that masked microcode
  std::vector<ElementVertex> elements;
  std::vector<SamplerShader> samplers;
  // banjo-nx (banjo_native_tfetch_filters): the filters its texture fetch instructions ask for instead of the fetch
  // constant's, per fetch constant 0..31. A Xenos tfetch carries its own mag / min / mip filter (3 = use the fetch
  // constant) and anisotropy (7 = use the fetch constant): an HLSL sampler_state compiles into them, and the game
  // can leave the Direct3D sampler state at its default. Stored XORed with "use the fetch constant" so 0 = nothing
  // overridden: bits 0-1 mag ^ 3, 2-3 min ^ 3, 4-5 mip ^ 3, 6-8 aniso ^ 7.
  std::array<uint16_t, 32> tfetch_filters{};
  bool filters_override = false;  // any entry of tfetch_filters is not 0
  bool filters_conflict = false;  // two fetches of one constant ask for different filters (the first one is used)
  // banjo-nx: what its texture fetch instructions say about the level of detail, per fetch constant (first fetch of
  // each): the instruction's own bias (7 bits signed, 1/16 of a level), whether the level comes from a register
  // (setTexLOD) and whether the computed level is used at all.
  std::array<int8_t, 32> tfetch_lod_bias{};
  uint32_t tfetch_register_lod = 0;     // bit per fetch constant
  uint32_t tfetch_no_computed_lod = 0;  // bit per fetch constant
  uint32_t tfetch_register_gradients = 0;
  uint32_t outputs = 0;  // pixel shader: bits COLOR0..3 y DEPTH (PixelShaderOutputs)
  // banjo-nx: register-linked vertex shader: the vertex fetch constants (0..95) read by fetches indexed by a computed
  // register (bone palettes), in the order XenosRecomp numbers them (banjoVfetch K). See ClassifyVertexFetches.
  std::vector<uint8_t> indexed_constants;
  // banjo-nx: vertex shader whose ALU code may read r0, which starts as the vertex index (conservative: any ALU
  // register operand r0). Its draws pass the index base in the shared constants and do not take the zero-base binding.
  bool reads_r0 = true;
  // banjo-nx: vertex shader with memory export (ALU exports to eA / eM0..4, registers 32..37), and the float
  // constants its ALU instructions read: the memexport stream constants are among them (see MemexportStreams).
  bool memexport = false;
  std::vector<uint8_t> memexport_constants;
  // OpKill in the SPIR-V. XenosRecomp puts one in every pixel shader for the alpha test (guarded by
  // SPEC_CONSTANT_ALPHA_TEST), so it only really discards if there is more than one.
  uint32_t kills = 0;
  bool discards = false;
  // Bytes of its float constant buffer that the SPIR-V reads (16 per register).
  uint32_t constants_bytes = 256 * 16;
  // banjo-nx (banjo_gpu_literals_cbuf, package 26): the literal registers whose components the SPIR-V reads from the
  // constant registers (mask bit k = component k: the definition's word has any of its low 12 bits set), with the
  // definition's words. literals_known false = the definition table could not be read (the old form for its draws).
  struct Literal {
    uint16_t reg = 0;
    uint8_t mask = 0;
    std::array<uint32_t, 4> words{};
  };
  std::vector<Literal> literals;
  bool literals_known = false;
};

struct StatsShaders {
  uint64_t loads = 0;           // IM_LOAD packets received
  uint64_t distinct = 0;        // distinct microcodes (with their patches)
  uint64_t identified = 0;    // of the distinct ones
  uint64_t swizzle_variants = 0;  // banjo-nx: identified only through VertexShaderIdentityMatches
  uint64_t no_identify = 0;  // of the distinct ones
  uint64_t ambiguous = 0;         // distinct ones with more than one possible container
};

class ShadersNative {
 public:
  ShadersNative();
  ~ShadersNative();

  // false, with a warning in the log, if the library is missing or damaged. With use_index and a
  // matching <file>.idx only the index is loaded and each SPIR-V is read on first use
  // (Shader::Spirv); otherwise the whole package.
  bool Load(const std::filesystem::path& file, bool use_index = true);
  bool loaded() const;
  bool is_indexed() const;  // loaded from the index: SPIR-V read on demand

  // Indexed library: reads now, on the calling thread, the SPIR-V of the entries with these container
  // fingerprints (PerFingerprint), with a file handle of its own. Other threads' Spirv() never wait for it
  // beyond publishing one entry. Nothing to do with a full load.
  masseffect::native::StatsPreload PreloadSpirv(std::span<const uint64_t> fingerprints,
                                                     const std::atomic<bool>* stop) const;

  // Full original container (the one the D3D constructors receive).
  // nullptr if it is not in the library. Can be called from any thread
  // once the library is loaded.
  const ShaderEntry* IdentifyContainer(std::span<const uint8_t> container) const;

  // The entry with that number (its position in the library), or nullptr. Like IdentifyContainer, from any
  // thread once loaded: entries do not change after Load. Pipeline prewarming (masseffect_native_draws.cpp) uses
  // it to recreate the pipelines of the previous session.
  const ShaderEntry* PerNumber(uint32_t number) const;

  // Stable container fingerprint. Unlike library position this survives adding newly discovered shaders,
  // so pipeline prewarming can reuse records produced by an older (smaller) package.
  const ShaderEntry* PerFingerprint(uint64_t fingerprint) const;

  // Microcode already in host byte order. nullptr if it is not in the library.
  // Only the ring thread uses it.
  const ShaderEntry* Identify(bool vertices, std::span<const uint32_t> microcode);

  StatsShaders Stats() const;

 private:
  struct Data;
  std::unique_ptr<Data> data_;
};

// Short name of a DeclUsage for the reports.
const char* UsageName(uint8_t usage);

// Whether a patched microcode from the ring can belong to that vertex shader: every
// declaration element has, at one of the element positions, a fetch that writes the
// same temporary register as the original. D3D reorders the fetches and changes
// formats and swizzles, but not the registers.
bool FetchCoherent(const ShaderEntry& vs, std::span<const uint32_t> patched);

}  // namespace masseffect::native
