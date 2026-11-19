/**
 * @file    gpu/guest/resources.h
 * @brief   Host records behind the guest D3D objects the renderer creates.
 *          Each starts with the X360 struct the game reads (see d3d.h).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <plume_render_interface.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/render/fetch_filters.h"  // port 145
#include "renut_engine/gpu/render/vertex_format.h"
#include "shader_cache.h"

namespace renut::gpu {

// Common bit the XDK sets on every resource it creates itself.
constexpr u32 kCommonD3DCreated = 0x100000;

struct VertexDeclarationRecord {
  d3d::Shader x360;  // D3DVertexDeclaration header

  RecordKind kind = RecordKind::VertexDeclaration;
  u32 self_va = 0;

  // banjo-nx (2026-10-28, ARM audit): what a draw reads first, on the record's first line (the hash and the stream
  // order were past the layout's 512 bytes of streams, on its ninth and tenth lines: 1.6 % of the render thread in the
  // c4 console run at declaration changes).
  uint64_t layout_hash = 0;  // identifies the host input layout for pipeline keys
  // banjo-nx (2026-10-23, batch 6): the streams its elements read (below kMaxVertexStreams), each once, in the order of
  // their first element (render/draw.cpp DrawNeedsExportFlush walked the elements at every draw). BuildDeclarationLayout.
  u8 stream_order[kMaxVertexStreams] = {};
  u8 stream_count = 0;
  std::vector<GuestVertexElement> elements;  // without D3DDECL_END
  HostVertexLayout layout;
};

struct ShaderRecord {
  d3d::Shader x360;  // D3DVertexShader / D3DPixelShader header

  RecordKind kind = RecordKind::VertexShader;
  u32 self_va = 0;

  uint64_t hash = 0;
  const ShaderCacheEntry *entry = nullptr;  // nullptr: not in the cache (captured)
  u32 fetch_3d_mask = 0;  // kameo-nx (2026-11-03): the slots its microcode reads through tfetch3D (Fetch3DMask, d3d/hooks/shader.cpp)
  // banjo-nx (2026-10-23, render-thread round): what every draw reads, together in the record's first lines (the host
  // shader, the summary and the specialised bools were in its fourth to sixth lines, past the stride arrays). The
  // host shader is built on first use by a pipeline, under host_mutex (below).
  // banjo-nx (2026-10-28, per-draw night, banjo_renut_shared_host_shaders): owned by the host shader registry
  // (d3d/hooks/shader.cpp: one host an entry, shared by its live records), given back by ReleaseHostShader.
  plume::RenderShader *host = nullptr;
  // Vertex shaders: what D3DVertexShader_Bind bound the shader to. The bind
  // bakes the declaration into the microcode, so a draw with no declaration
  // set uses it; a copy, as N&B releases the declaration right after binding.
  // Vertex shaders: the declaration the shader's own vfetches describe, for
  // draws with neither a declaration set nor a bind (bundle meshes). Built on
  // first use; nullptr when the shader fetches no vertex elements.
  // banjo-nx (2026-10-28, ARM audit): both pointers on the record's first two lines (they were on its fourth, past the
  // summary and the bool masks, which no prefetch took: a miss at most shader changes, 1 % of the render thread in the
  // c4 console run; N&B makes a shader object per mesh). Their strides stay below.
  std::shared_ptr<VertexDeclarationRecord> bound_declaration;  // halo3-nx: shared with its binding (ApplyBindingLocked)
  std::unique_ptr<VertexDeclarationRecord> fetch_declaration;
  bool fetch_declaration_built = false;  // (2026-10-28, cache-miss audit 2: beside its pointer, from the sixth line)
  // vivapinata-nx (2026-11-03, the white bird): the vertex shader's export mode is multipass (its header's program control word,
  // 7 in bits 24-26): its outputs are ignored and every primitive killed, so its draw only exports to memory.
  bool export_only = false;
  // banjo-nx (2026-10-23, render-thread round): what every draw used to look up in memos keyed by the cache entry
  // (render/draw.cpp DrawSummaryOf): the float constant groups the shader reads (GroupSummary), whether it exports
  // (GatherMemExportStreams), whether an input is fetched by another index (DescribeBoundFetches). Made at the shader's
  // first draw, on the render thread; the city's few hundred shaders missed those memos' slots.
  struct DrawSummary {
    const u32 *used = nullptr;
    uint64_t groups = 0;
    // banjo-nx (2026-10-27, constants): a copy of *used (the registers the shader reads) and whether its cache entry has
    // literals, in the summary's first line: the push read both from the cache entry, a miss at most shader changes
    // (the console's b35 profile: the push's first lines 0.9 % of the render thread).
    u32 used_words[8] = {};
    // banjo-nx (2026-10-23, batch 6): its literal registers as pushed (render/draw.cpp LiteralRuns), or null.
    const void *literal_runs = nullptr;
    uint32_t bytes = 0;
    bool exports = false;
    bool bound_fetches = false;
    bool composite = false;  // banjo-nx (2026-10-23, batch 6): a pixel shader of the frame's final composite (CompositePs)
    bool skin_once = false;  // banjo-nx (2026-10-27): a vertex shader of banjo_deko3d_skin_once's (IsSkinOnceVs)
    bool known = false;
    bool has_literals = false;  // banjo-nx (2026-10-27): the cache entry's literalCount != 0 (known summaries)
    // banjo-nx (2026-10-27): whether its host shader may read gl_VertexID (plume Deko3DShaderReadsVertexId: a call and
    // a miss on the host shader at most vertex shader changes), -1 until a draw asks; the host is set once per record.
    int8_t reads_vertex_id = -1;
    u32 valid[8] = {};  // the groups' registers (a whole upload's 'valid'; the first upload only)
  };
  mutable DrawSummary draw_summary;
  // banjo-nx (banjo_gpu_bool_spec): the bools the shader's SPIR-V specialises (ShaderBoolSpecMask), read on the
  // render thread at the first draw.
  mutable u32 bool_spec_mask[8] = {};
  mutable bool bool_spec_read = false;

  // The bound declaration's per-stream strides in bytes, used where SetStreamSource was given a zero stride (keep the
  // bound one); the fetch declaration's.
  u32 bound_strides[d3d::kMaxStreams] = {};
  u32 applied_binding = 0;  // halo3-nx (2026-11-03): the binding these are (ApplyBindingLocked)
  u32 fetch_strides[d3d::kMaxStreams] = {};

  std::mutex host_mutex;  // building host

  // renut-ultimate (2026-11-16, port 145; tw2de_nx's): its texture fetches' own filters (render/fetch_filters.h),
  // read from the microcode when made; used with banjo_renut_fetch_filters (off by default). At the end: the fields
  // every draw reads stay in the record's first lines.
  FetchFilterOverrides fetch_filters;

  bool pixel() const { return kind == RecordKind::PixelShader; }
};

// The shader bound as 'shader_va': a record the renderer created, or the host
// side of a guest object set up in place (XGRegisterVertexShader/PixelShader).
// nullptr if it is neither. Requires State().mutex.
ShaderRecord *FindShaderLocked(u32 shader_va, RecordKind kind);
// banjo-nx (2026-10-23): prefetches what FindShaderLocked will read for this shader (no other effect).
void PrefetchShaderLocked(u32 shader_va, RecordKind kind);
// banjo-nx (2026-10-27): the record's first lines when the memo holds the shader already (prefetches only).
void PrefetchShaderRecordLocked(u32 shader_va, RecordKind kind);
// banjo-nx (2026-10-28, per-draw night): a shader record sits at the shader's own guest address (guest_records.h), so
// Replay's stream fetches its first three lines from an op's recorded word alone, an op ahead (no memo look: A/B 49's
// look in the memo cost more than it saved); then, at the op itself, the lines behind it a shader change reads (its
// cache entry, its bound declaration). The c4/d4 profiles: the record's first touch (its cache entry and host shader)
// was the fast path's hottest load, N&B making a shader object per mesh. Prefetches only: an address that is not one of
// our records (a guest shader set up in place) fetches lines of guest memory for nothing.
void PrefetchShaderRecordAt(u32 shader_va);
void PrefetchShaderBehind(u32 shader_va);

// Drops the host side of an in-place guest shader object, if it has one.
// Requires State().mutex.
void ReleaseGuestShaderLocked(u32 shader_va);

// blur-nx (2026-11-13, #13's fix, banjo_renut_deferred_narrow): the cache entry of the in-place shader of 'kind' last
// registered at 'shader_va' (XGRegisterVertexShader/PixelShader), for a capture made on a game thread (the render
// thread's own map, FindShaderLocked's, is not read there). False when no such shader is registered there; '*entry' is
// null when the shader cache lacks it. ForgetInPlaceShader: the object's D3DResource_Destroy. Either is called after
// the render-thread item that makes the same change to the render thread's map is queued, so a capture that sees the
// change is queued after that item.
bool InPlaceShaderEntry(u32 shader_va, RecordKind kind, const ShaderCacheEntry **entry);
void ForgetInPlaceShader(u32 shader_va);
void NoteInPlaceShader(u32 shader_va, RecordKind kind, const ShaderCacheEntry *entry);  // RegisterGuestShader's

// Fills a declaration's host input layout and layout hash from its elements.
void BuildDeclarationLayout(VertexDeclarationRecord &declaration);

// The vertex shader's fetch declaration (see ShaderRecord::fetch_declaration).
// Requires State().mutex.
const VertexDeclarationRecord *ShaderFetchDeclarationLocked(ShaderRecord &vs);

// The shader's host object, building it on first call. nullptr if the shader
// is missing from the cache or creation failed. Requires State().mutex.
plume::RenderShader *GetHostShader(ShaderRecord *shader);
// banjo-nx (2026-10-27, render thread night): the built host shader read inline (GetHostShader's first answer, under the
// same rule: only GetHostShader sets 'host', and the draws that read it hold State().mutex); the call otherwise.
[[gnu::always_inline]] inline plume::RenderShader *HostShaderOf(ShaderRecord *shader) {
  if (shader && shader->entry)
    if (plume::RenderShader *host = shader->host)
      return host;
  return GetHostShader(shader);
}

// Free a record's GPU objects (deferred past in-flight frames) and the record.
void DestroyShaderRecord(ShaderRecord *shader);
void DestroyVertexDeclarationRecord(VertexDeclarationRecord *declaration);

} // namespace renut::gpu
