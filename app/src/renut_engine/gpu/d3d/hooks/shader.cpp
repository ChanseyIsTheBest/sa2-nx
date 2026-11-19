/**
 * @file    gpu/d3d/hooks/shader.cpp
 * @brief   Shader creation. The renderer owns the shader objects: each is a
 *          record keyed to its XenosRecomp cache entry by container hash.
 *
 * The ~150 shaders N&B compiles from HLSL at boot (hcg*ShaderCreateByHlsl)
 * reach D3DDevice_CreateVertexShader/CreatePixelShader too, so a cache miss
 * here is captured to disk for the next shader cache build.
 *
 * Shaders loaded with asset bundles are D3D objects the bundle already holds,
 * set up in place by XGRegisterVertexShader/XGRegisterPixelShader. Those stay
 * guest objects; the host side is kept per object address, keyed by the same
 * container hash.
 */
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>  // blur-nx (2026-11-13, #13's fix)
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/command_buffers.h"  // banjo-nx (2026-10-27): NoteShaderSortKey
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/lean_mutex.h"  // blur-nx (2026-11-17, audit #8)
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/render/shader_library.h"
#include "renut_engine/gpu/backend/video.h"

using namespace renut::gpu;

REXCVAR_DEFINE_INT32(banjo_renut_shader_reuse, 128, "Banjo/Enhancements",
                     "Renut's renderer (2026-10-23, batch 6): the host shaders of this many shader objects the game "
                     "destroyed are kept, and given to the next object of the same shader instead of a new one (on "
                     "deko3d each new one unpacked and parsed its GLSL on the render thread: N&B destroys and makes "
                     "shader objects all the time while it streams the level). 0: none kept")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_shared_host_shaders, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-28, per-draw night): the live shader objects of one shader share one host "
                    "shader (N&B makes a shader object per mesh, many alive at once with the same shader: each unpacked "
                    "and parsed its GLSL on the render thread, kept its own copy of it and compiled its own variants). "
                    "false: one host shader an object, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_bindings_lockfree, true, "Banjo/Enhancements",
                    "blur-nx (2026-11-17, the 24-hour run): every draw's recorded bind (banjo_renut_recorded_binds: "
                    "LastBindingOf, on each recorded op and each queued draw) and the render thread's bind of it read "
                    "from tables written under the binds' lock, not under the lock (the frame jobs waited 12 % of their "
                    "racing time on a lock inside NewOp: this one, ~25,000 times a second). Off: the lock every time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_in_place_memo, true, "Banjo/Enhancements",
                    "blur-nx (2026-11-17, the 24-hour run): the narrowed captures' shader lookups (InPlaceShaderEntry, "
                    "two a Begin*Vertices draw) answered from a small per-thread memo while no shader was registered or "
                    "destroyed since, instead of a shared lock each (Game Master spent 1.2 % of its racing time in that "
                    "lock's unlock). Off: the lock every time, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_recorded_binds, true, "Banjo/Renut",
                    "halo3-nx (2026-11-03, from sasrt-nx's sasrt_renut_recorded_binds): each draw keeps the "
                    "D3DVertexShader_Bind its shader had when the game made the draw. Halo 3 records its draws at load, "
                    "so at a replay the last bind queued may be another mesh's layout. Off: the last bind queued")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

// banjo-nx (2026-10-23, batch 6, banjo_renut_shader_reuse): host shaders of destroyed shader records, by their cache
// entry (one host shader is made from the entry alone: CreateCachedShader), the least recently kept going first. A host
// shader is immutable once made but for the variants it compiles; the pipelines keep their own compiled code and the
// compile jobs their own GLSL, so handing one to a new record is what making it again would give. State().mutex.
struct KeptShader {
  const ShaderCacheEntry *entry = nullptr;
  std::unique_ptr<plume::RenderShader> host;
  uint64_t stamp = 0;
};
// Never destroyed (left at exit, after the device is gone).
std::vector<KeptShader> &g_kept_shaders = *new std::vector<KeptShader>();
uint64_t g_kept_stamp = 0;

u32 KeptShaderCap() {
  static const u32 cap = u32(std::max(0, REXCVAR_GET(banjo_renut_shader_reuse)));
  return cap;
}

void KeepShader(const ShaderCacheEntry *entry, std::unique_ptr<plume::RenderShader> host) {
  const u32 cap = KeptShaderCap();
  if (!entry || !host || !cap)
    return;
  KeptShader *slot = nullptr;
  for (KeptShader &k : g_kept_shaders) {
    if (k.entry == entry) {  // one of each is enough
      slot = &k;
      break;
    }
  }
  if (!slot && g_kept_shaders.size() < cap)
    slot = &g_kept_shaders.emplace_back();
  if (!slot) {
    slot = &g_kept_shaders.front();
    for (KeptShader &k : g_kept_shaders)
      if (k.stamp < slot->stamp)
        slot = &k;
  }
  slot->entry = entry;
  slot->host = std::move(host);
  slot->stamp = ++g_kept_stamp;
}

std::unique_ptr<plume::RenderShader> TakeKeptShader(const ShaderCacheEntry *entry) {
  for (KeptShader &k : g_kept_shaders) {
    if (k.entry == entry && k.host) {
      k.entry = nullptr;
      k.stamp = 0;
      return std::move(k.host);
    }
  }
  return nullptr;
}

// banjo-nx (2026-10-28, per-draw night, banjo_renut_shared_host_shaders): the host shaders the live records use, with
// the count of records using each, keyed by the cache entry (by the record itself with the switch off). A record's
// 'host' points into it; its release (ReleaseHostShader) hands the last record's host to KeepShader. Its own lock (the
// records are made and released on the render thread and on the game's threads, under State().mutex mostly).
struct LiveHost {
  std::unique_ptr<plume::RenderShader> shader;
  u32 records = 0;
};
// Never destroyed (left at exit, after the device is gone), as g_kept_shaders.
std::unordered_map<const void *, LiveHost> &g_live_hosts = *new std::unordered_map<const void *, LiveHost>();
std::mutex g_live_hosts_mutex;
uint64_t g_hosts_made = 0, g_hosts_shared = 0, g_hosts_kept = 0;

bool SharedHostShadersOn() {
  static const bool on = REXCVAR_GET(banjo_renut_shared_host_shaders);
  return on;
}
const void *LiveHostKey(const ShaderRecord *record) {
  return SharedHostShadersOn() ? static_cast<const void *>(record->entry) : static_cast<const void *>(record);
}

// The last record's host to KeepShader; the record's pointer cleared. Under the record's host_mutex.
void ReleaseHostShader(ShaderRecord *record) {
  if (!record || !record->host)
    return;
  InvalidateHostPacketsLocked();  // banjo-nx (2026-10-28, host packets): a packet may hold this host shader
  std::unique_ptr<plume::RenderShader> last;
  {
    std::lock_guard lock(g_live_hosts_mutex);
    auto it = g_live_hosts.find(LiveHostKey(record));
    if (it != g_live_hosts.end() && it->second.shader.get() == record->host && --it->second.records == 0) {
      last = std::move(it->second.shader);
      g_live_hosts.erase(it);
    }
  }
  record->host = nullptr;
  KeepShader(record->entry, std::move(last));
}

// Where D3DDevice_CreateVertexShader / CreatePixelShader (0x826377C0 /
// 0x826375D8) lay out a shader object: the container's virtual part copied
// after the D3D header, and a pointer to its physical part (literal constants
// and microcode). Objects in bundles have the same layout.
constexpr u32 kVertexShaderContainer = 0x368;
constexpr u32 kVertexShaderPhysical = 0x20;
constexpr u32 kPixelShaderContainer = 0x28;
constexpr u32 kPixelShaderPhysical = 0x18;

// Guest shader objects registered in place, by object address. The render
// thread's (State().mutex).
std::unordered_map<u32, std::unique_ptr<ShaderRecord>> g_guest_shaders;

// Recent FindShaderLocked results (two lookups a draw otherwise, one under a
// shared lock). An entry goes when its shader does (ForgetShader): both kinds
// are destroyed on the render thread, the only user, so a memo'd record is
// alive. (Checking the record registry's generation instead missed whenever
// any record came or went, which busy levels do all the time.) State().mutex.
// A miss is kept too (record null): draws keep binding addresses that are no
// shader of the kind (depth-only passes), and those cost a registry lookup and
// a map search a draw. A shader made at an address forgets it first (both are
// in order with the draws on the render thread). va 0 is an empty entry
// (FindShaderLocked never looks it up).
struct ShaderMemo {
  u32 va = 0;
  ShaderRecord *record = nullptr;
};
// A table per kind, a set of four entries per cache line, the most recent
// first. A single direct-mapped table for both kinds had the shaders of a busy
// level (Showdown Town binds hundreds a frame) evicting each other every frame,
// and each miss was a registry lookup and a map search.
constexpr u32 kShaderMemoWays = 4;
// banjo-nx (2026-10-28, per-draw night): 4,096 sets a kind (1,024): N&B makes a shader object per mesh (69-72 % of the
// city's draws bind another address), more than the 4,096 entries a kind held; a miss is a registry lookup under its
// lock. 512 KB of static memory, not the heap.
constexpr u32 kShaderMemoSetBits = 12;
struct alignas(64) ShaderMemoSet {
  ShaderMemo way[kShaderMemoWays];
};
static_assert(sizeof(ShaderMemoSet) == 64);
ShaderMemoSet g_shader_memo[2][1u << kShaderMemoSetBits];

u32 KindIndex(RecordKind kind) {
  return kind == RecordKind::PixelShader ? 1 : 0;
}

ShaderMemoSet &ShaderMemoSetFor(u32 kind_index, u32 shader_va) {
  return g_shader_memo[kind_index][(shader_va * 0x9E3779B1u) >> (32 - kShaderMemoSetBits)];
}

// The last shader found of each kind: consecutive draws mostly keep their
// shaders, and this is one compare where the memo is a cache miss.
ShaderMemo g_last_shader[2];

void ForgetShader(u32 shader_va) {
  for (u32 kind = 0; kind < 2; ++kind) {
    for (ShaderMemo &memo : ShaderMemoSetFor(kind, shader_va).way) {
      if (memo.va == shader_va)
        memo = {};
    }
    if (g_last_shader[kind].va == shader_va)
      g_last_shader[kind] = {};
  }
}

} // namespace

namespace renut::gpu {

// banjo-nx (2026-10-23, render-thread round): the lines FindShaderLocked will read for the next draw's shader, fetched
// a draw ahead (command_buffers.cpp Replay): the memo set, or with the shader the last one, its record and entry.
// Prefetches only: the memo is not reordered.
void PrefetchShaderLocked(u32 shader_va, RecordKind kind) {
  if (!shader_va)
    return;
  const u32 kind_index = KindIndex(kind);
  const ShaderMemo &last = g_last_shader[kind_index];
  if (last.va == shader_va) {
    if (last.record) {
      __builtin_prefetch(last.record);
      __builtin_prefetch(reinterpret_cast<const char *>(last.record) + 64);  // the draw summary
      __builtin_prefetch(reinterpret_cast<const char *>(last.record) + 128);  // (its used words, 2026-10-27)
      __builtin_prefetch(last.record->entry);
    }
    return;
  }
  __builtin_prefetch(&ShaderMemoSetFor(kind_index, shader_va));
}

// banjo-nx (2026-10-27, the replay stream's second stage): when the memo already holds the shader (its set fetched an op
// before by PrefetchShaderLocked), the record's lines the draw reads first (the draw summary with them). A look only:
// the memo is not reordered and a shader not in it is left to FindShaderLocked.
void PrefetchShaderRecordLocked(u32 shader_va, RecordKind kind) {
  if (!shader_va)
    return;
  const u32 kind_index = KindIndex(kind);
  const ShaderRecord *record = nullptr;
  if (g_last_shader[kind_index].va == shader_va) {
    record = g_last_shader[kind_index].record;
  } else {
    for (const ShaderMemo &memo : ShaderMemoSetFor(kind_index, shader_va).way) {
      if (memo.va == shader_va) {
        record = memo.record;
        break;
      }
    }
  }
  if (record) {
    const char *r = reinterpret_cast<const char *>(record);
    __builtin_prefetch(r);
    __builtin_prefetch(r + 64);
    __builtin_prefetch(r + 128);
  }
}

void PrefetchShaderRecordAt(u32 shader_va) {
  if (const char *r = GuestPtr<const char>(shader_va)) {
    __builtin_prefetch(r);        // the cache entry, host shader, declaration pointers
    __builtin_prefetch(r + 64);   // the draw summary
    __builtin_prefetch(r + 128);  // (its used words, flags)
  }
}
void PrefetchShaderBehind(u32 shader_va) {
  const auto *r = GuestPtr<const ShaderRecord>(shader_va);
  if (!r || r->self_va != shader_va)
    return;  // not one of our records (a guest object's bytes)
  if (const char *e = reinterpret_cast<const char *>(r->entry)) {
    __builtin_prefetch(e);
    __builtin_prefetch(e + 64);
  }
  if (const char *d = reinterpret_cast<const char *>(r->bound_declaration.get())) {
    __builtin_prefetch(d);
    __builtin_prefetch(d + 64);
    __builtin_prefetch(d + 128);
  }
}

// blur-nx (2026-11-13, #13's fix, banjo_renut_deferred_narrow): g_guest_shaders as the game threads changed it (every
// shader of Blur's is an in-place one: a narrowed capture looked in the record registry alone, and none was narrowed).
// The render thread's item for the same change is queued first (RegisterGuestShader, D3DResource_Destroy_hook).
namespace {
struct InPlaceShader {
  RecordKind kind;
  const ShaderCacheEntry *entry;
};
LeanSharedMutex g_in_place_mutex;  // blur-nx (2026-11-17, audit #8, blur_renut_lean_rwlock): was std::shared_mutex
std::unordered_map<u32, InPlaceShader> g_in_place;
// blur-nx (2026-11-17, blur_renut_in_place_memo): bumped under the lock at every change of g_in_place; a thread's memo
// answers while the generation it was made at is still the current one: the map then gives the same answer. A change
// made on another thread at the same moment is ordered with the lookup by nothing, with or without the lock (the
// header's comment: the change's render-thread item is queued before it), so the memo answers as the lock would have.
std::atomic<uint64_t> g_in_place_generation{1};
struct InPlaceMemo {
  uint64_t generation = 0;
  u32 shader_va = 0;
  RecordKind kind{};
  bool found = false;
  const ShaderCacheEntry *entry = nullptr;
};
constexpr u32 kInPlaceMemoSlots = 16;
thread_local InPlaceMemo t_in_place_memo[kInPlaceMemoSlots];
}  // namespace

bool InPlaceShaderEntry(u32 shader_va, RecordKind kind, const ShaderCacheEntry **entry) {
  static const bool memo_on = REXCVAR_GET(blur_renut_in_place_memo);
  uint64_t generation = 0;
  InPlaceMemo *memo = nullptr;
  if (memo_on) {
    generation = g_in_place_generation.load(std::memory_order_acquire);
    memo = &t_in_place_memo[(shader_va * 2654435761u) >> 28];
    if (memo->generation == generation && memo->shader_va == shader_va && memo->kind == kind) {
      if (memo->found)
        *entry = memo->entry;
      return memo->found;
    }
  }
  std::shared_lock lock(g_in_place_mutex);
  const auto it = g_in_place.find(shader_va);
  const bool found = it != g_in_place.end() && it->second.kind == kind;
  if (found)
    *entry = it->second.entry;
  if (memo)
    *memo = {generation, shader_va, kind, found, found ? it->second.entry : nullptr};
  return found;
}

void ForgetInPlaceShader(u32 shader_va) {
  std::unique_lock lock(g_in_place_mutex);
  g_in_place.erase(shader_va);
  g_in_place_generation.fetch_add(1, std::memory_order_acq_rel);  // blur-nx (2026-11-17)
}

void NoteInPlaceShader(u32 shader_va, RecordKind kind, const ShaderCacheEntry *entry) {
  std::unique_lock lock(g_in_place_mutex);
  g_in_place[shader_va] = {kind, entry};
  g_in_place_generation.fetch_add(1, std::memory_order_acq_rel);  // blur-nx (2026-11-17)
}

ShaderRecord *FindShaderLocked(u32 shader_va, RecordKind kind) {
  if (!shader_va)
    return nullptr;
  const u32 kind_index = KindIndex(kind);
  ShaderMemo &last = g_last_shader[kind_index];
  if (last.va == shader_va)
    return last.record;
  ShaderMemo *ways = ShaderMemoSetFor(kind_index, shader_va).way;
  u32 hit = 0;
  while (hit < kShaderMemoWays && ways[hit].va != shader_va)
    ++hit;
  ShaderMemo found;
  if (hit < kShaderMemoWays) {
    found = ways[hit];
  } else {
    hit = kShaderMemoWays - 1;  // the least recent goes
    ShaderRecord *record = FindRecord<ShaderRecord>(shader_va, kind);
    if (!record) {
      auto it = g_guest_shaders.find(shader_va);
      record = it != g_guest_shaders.end() && it->second->kind == kind ? it->second.get() : nullptr;
    }
    found = {shader_va, record};
  }
  for (u32 i = hit; i > 0; --i)
    ways[i] = ways[i - 1];
  ways[0] = found;
  last = found;
  return found.record;
}

void ReleaseGuestShaderLocked(u32 shader_va) {
  auto it = g_guest_shaders.find(shader_va);
  if (it == g_guest_shaders.end())
    return;
  if (ShaderRecord *record = it->second.get()) {  // banjo-nx (2026-10-23, batch 6): its host shader kept
    std::lock_guard shader_lock(record->host_mutex);
    ReleaseHostShader(record);  // banjo-nx (2026-10-28)
  }
  g_guest_shaders.erase(it);
  ForgetShader(shader_va);
}

void BuildDeclarationLayout(VertexDeclarationRecord &declaration) {
  declaration.layout =
      BuildHostVertexLayout(declaration.elements.data(), u32(declaration.elements.size()));
  // FNV-1a over what the input layout depends on.
  uint64_t hash = 0xCBF29CE484222325ull;
  auto mix = [&hash](uint64_t v) { hash = (hash ^ v) * 0x100000001B3ull; };
  for (const plume::RenderInputElement &e : declaration.layout.input_elements) {
    mix(e.location);
    mix(uint64_t(e.format));
    mix(e.slotIndex);
    mix(e.alignedByteOffset);
  }
  for (u32 s = 0; s < kMaxVertexStreams; ++s)
    mix(declaration.layout.streams[s].host_stride);
  declaration.layout_hash = hash;
  // banjo-nx (2026-10-23, batch 6): VertexDeclarationRecord::stream_order.
  u32 seen = 0;
  declaration.stream_count = 0;
  for (const GuestVertexElement &e : declaration.elements) {
    const u32 stream = u16(e.Stream);
    if (stream >= kMaxVertexStreams || (seen & (1u << stream)))
      continue;
    seen |= 1u << stream;
    declaration.stream_order[declaration.stream_count++] = u8(stream);
  }
}

const VertexDeclarationRecord *ShaderFetchDeclarationLocked(ShaderRecord &vs) {
  if (vs.fetch_declaration_built)
    return vs.fetch_declaration.get();
  vs.fetch_declaration_built = true;
  if (!vs.entry || !vs.entry->vertexInputCount)
    return nullptr;
  auto declaration = std::make_unique<VertexDeclarationRecord>();
  const ShaderVertexInput *inputs = g_activeShaderVertexInputs + vs.entry->vertexInputOffset;
  for (u32 i = 0; i < vs.entry->vertexInputCount; ++i) {
    const ShaderVertexInput &input = inputs[i];
    // Stream n's fetch constant is Xenos index 95 - n (d3d::StreamFetchIndex).
    const u32 first_stream_fetch = d3d::kFirstVertexFetchIndex;
    if (input.fetchIndex < first_stream_fetch || input.fetchIndex > 95)
      continue;
    const u32 stream = 95 - input.fetchIndex;
    const u32 type = FetchDeclType(input.format, input.flags & 1, input.flags & 2);
    if (type == kDeclTypeUnused)
      continue;
    GuestVertexElement element{};
    element.Stream = u16(stream);
    element.Offset = u16(input.offsetDwords * 4);
    element.Type = type;
    element.Method = 0;
    element.Usage = input.usage;
    element.UsageIndex = input.usageIndex;
    declaration->elements.push_back(element);
    if (stream < d3d::kMaxStreams && !vs.fetch_strides[stream])
      vs.fetch_strides[stream] = input.strideDwords * 4u;
  }
  if (declaration->elements.empty())
    return nullptr;
  BuildDeclarationLayout(*declaration);
  vs.fetch_declaration = std::move(declaration);
  return vs.fetch_declaration.get();
}

plume::RenderShader *GetHostShader(ShaderRecord *shader) {
  if (!shader || !shader->entry)
    return nullptr;
  // Built already: draws call this under State().mutex, which destruction
  // (DestroyShaderRecord) takes too, and only this function sets 'host'.
  if (plume::RenderShader *host = shader->host)
    return host;
  std::lock_guard lock(shader->host_mutex);
  if (!shader->host) {
    // banjo-nx (2026-10-28, per-draw night): a live record's host of the same entry first (banjo_renut_shared_host_shaders),
    // then a destroyed one's (batch 6), then a new one.
    std::lock_guard live_lock(g_live_hosts_mutex);
    LiveHost &live = g_live_hosts[LiveHostKey(shader)];
    uint64_t *count = &g_hosts_shared;
    if (!live.shader) {
      live.shader = TakeKeptShader(shader->entry);  // banjo-nx (2026-10-23, batch 6)
      count = &g_hosts_kept;
      if (!live.shader) {
        live.shader = CreateCachedShader(Video::Device(), *shader->entry);
        count = &g_hosts_made;
      }
    }
    if (!live.shader) {
      g_live_hosts.erase(LiveHostKey(shader));
      RGPU_ERROR("Creating shader {:016X} failed", shader->hash);
      shader->entry = nullptr;  // don't retry every draw
      return nullptr;
    }
    ++live.records;
    shader->host = live.shader.get();
    ++*count;
    const uint64_t n = g_hosts_made + g_hosts_shared + g_hosts_kept;
    if (n >= 256 && (n & (n - 1)) == 0)
      RGPU_INFO("[renut] host shaders: {} made, {} shared with a live object of the same shader "
                "(banjo_renut_shared_host_shaders), {} from a destroyed one (banjo_renut_shader_reuse); {} live",
                g_hosts_made, g_hosts_shared, g_hosts_kept, g_live_hosts.size());
  }
  return shader->host;
}

void DestroyVertexDeclarationRecord(VertexDeclarationRecord *declaration) {
  if (!declaration)
    return;
  std::lock_guard lock(State().mutex);
  InvalidateFastDrawForRecordLocked(declaration);  // banjo-nx (2026-10-28): it may hold this record
  ForgetVertexDeclarationLocked(declaration->self_va);
  DestroyRecord(declaration);
}

void DestroyShaderRecord(ShaderRecord *shader) {
  if (!shader)
    return;
  // Pipelines built from it keep their own compiled copy; only the module goes.
  {
    auto &s = State();
    std::lock_guard lock(s.mutex);
    InvalidateFastDrawForRecordLocked(shader);  // banjo-nx (2026-10-28): it may hold this record
    ForgetShader(shader->self_va);
    std::lock_guard shader_lock(shader->host_mutex);
    ReleaseHostShader(shader);  // banjo-nx (2026-10-23, batch 6; 2026-10-28: the registry's)
  }
  DestroyRecord(shader);
}

// halo3-nx (2026-11-03, banjo_renut_recorded_binds): a shader's binds, each kept once, by id (index + 1).
namespace {
struct RecordedBinding {
  u32 shader_va = 0;
  std::shared_ptr<VertexDeclarationRecord> declaration;
  u32 strides[d3d::kMaxStreams] = {};
};
std::mutex g_bindings_mutex;
std::deque<RecordedBinding> g_bindings;
std::unordered_map<u32, std::vector<u32>> g_shader_bindings;
std::unordered_map<u32, u32> g_last_binding;

// blur-nx (2026-11-17, blur_renut_bindings_lockfree): g_last_binding as an open-addressed table (a shader's address in
// the high half of an entry, its last bind's id in the low half, one 64-bit word: never torn) and g_bindings' elements
// by id, both written under g_bindings_mutex (the map and the deque stay the truth) and read without it. Entries are
// only added or overwritten, never removed (the map is never erased), so a reader's probe ends at the shader's entry or
// at an empty one. Release stores, acquire loads: a bind's id seen in the table has its element in g_binding_ptrs.
// A full table (or an id past the pointer table) sends the readers back to the lock, as before.
constexpr u32 kLastBindingSlots = 1u << 14;
std::atomic<uint64_t> g_last_binding_fast[kLastBindingSlots];
std::atomic<bool> g_last_binding_fast_full{false};
constexpr u32 kBindingPtrs = 1u << 16;
std::atomic<const RecordedBinding *> g_binding_ptrs[kBindingPtrs];

bool BindingsLockFree() {
  static const bool on = REXCVAR_GET(blur_renut_bindings_lockfree);
  return on;
}

// Under g_bindings_mutex.
void NoteLastBindingFast(u32 shader_va, u32 id) {
  if (g_last_binding_fast_full.load(std::memory_order_relaxed))
    return;
  const uint64_t entry = (uint64_t(shader_va) << 32) | id;
  for (u32 i = 0, slot = (shader_va * 2654435761u) >> 18; i < kLastBindingSlots; ++i, slot = (slot + 1) & (kLastBindingSlots - 1)) {
    const uint64_t e = g_last_binding_fast[slot].load(std::memory_order_relaxed);
    if (e == 0 || u32(e >> 32) == shader_va) {
      if (e != entry)
        g_last_binding_fast[slot].store(entry, std::memory_order_release);
      return;
    }
  }
  g_last_binding_fast_full.store(true, std::memory_order_release);
}

u32 InternBinding(u32 shader_va, const VertexDeclarationRecord &declaration, const u32 (&strides)[d3d::kMaxStreams]) {
  std::lock_guard lock(g_bindings_mutex);
  std::vector<u32> &ids = g_shader_bindings[shader_va];
  for (const u32 id : ids) {
    const RecordedBinding &b = g_bindings[id - 1];
    if (std::memcmp(b.strides, strides, sizeof(strides)) == 0 &&
        b.declaration->elements.size() == declaration.elements.size() &&
        std::memcmp(b.declaration->elements.data(), declaration.elements.data(),
                    declaration.elements.size() * sizeof(GuestVertexElement)) == 0) {
      g_last_binding[shader_va] = id;
      NoteLastBindingFast(shader_va, id);  // blur-nx (2026-11-17)
      return id;
    }
  }
  RecordedBinding &b = g_bindings.emplace_back();
  b.shader_va = shader_va;
  b.declaration = std::make_shared<VertexDeclarationRecord>(declaration);
  std::memcpy(b.strides, strides, sizeof(strides));
  const u32 id = u32(g_bindings.size());
  if (id < kBindingPtrs)
    g_binding_ptrs[id].store(&b, std::memory_order_release);  // blur-nx (2026-11-17): before the id is published
  ids.push_back(id);
  g_last_binding[shader_va] = id;
  NoteLastBindingFast(shader_va, id);  // blur-nx (2026-11-17)
  static uint32_t rebinds = 0;
  if (ids.size() > 1 && (++rebinds <= 16 || rebinds % 1000 == 0))
    RGPU_INFO("Vertex shader {:08X} bound with layout {} of its own (declaration {:08X}, stride {}; {} such binds so far)",
              shader_va, ids.size(), declaration.self_va, strides[0], rebinds);
  return id;
}
}  // namespace

u32 LastBindingOf(u32 shader_va) {
  static const bool on = REXCVAR_GET(banjo_renut_recorded_binds);
  if (!on || !shader_va)
    return 0;
  // blur-nx (2026-11-17, blur_renut_bindings_lockfree): the table's answer, the map's as of the read.
  if (BindingsLockFree() && !g_last_binding_fast_full.load(std::memory_order_acquire)) {
    for (u32 i = 0, slot = (shader_va * 2654435761u) >> 18; i < kLastBindingSlots;
         ++i, slot = (slot + 1) & (kLastBindingSlots - 1)) {
      const uint64_t e = g_last_binding_fast[slot].load(std::memory_order_acquire);
      if (e == 0)
        return 0;
      if (u32(e >> 32) == shader_va)
        return u32(e);
    }
  }
  std::lock_guard lock(g_bindings_mutex);
  const auto it = g_last_binding.find(shader_va);
  return it != g_last_binding.end() ? it->second : 0;
}

void ApplyBindingLocked(u32 id) {
  const RecordedBinding *b = nullptr;
  if (BindingsLockFree() && id < kBindingPtrs)  // blur-nx (2026-11-17): stored before the id was handed out
    b = g_binding_ptrs[id].load(std::memory_order_acquire);
  if (!b) {
    std::lock_guard lock(g_bindings_mutex);
    b = &g_bindings[id - 1];  // (a deque: its elements stay where they are)
  }
  auto *shader = FindShaderLocked(b->shader_va, RecordKind::VertexShader);
  if (!shader || shader->applied_binding == id)
    return;
  // banjo-nx (2026-10-27, render thread night): the fast path and its stream memo may hold the declaration this replaces
  // and read these strides: dropped (a bind after the first draws; N&B binds at creation).
  InvalidateFastDrawForRecordLocked(shader);
  shader->bound_declaration = b->declaration;
  for (const GuestVertexElement &element : b->declaration->elements) {
    const u32 stream = element.Stream;
    if (stream < d3d::kMaxStreams)
      shader->bound_strides[stream] = b->strides[stream];
  }
  shader->applied_binding = id;
}

} // namespace renut::gpu

namespace {

// kameo-nx (2026-11-03, banjo_renut_fetch3d_on_2d): the slots the microcode reads through tfetch3D. The fetch constant says
// what the data is, the instruction which table the recompiled shader reads, and they can differ. Field positions are
// XenosRecomp's shader_code.h; the walk over the control flow is that of Kameo: RePowered's ScanShaderConstantRange
// (src/gpu/shaders/ucode_scan.cpp, MaxDeadBear and BiRabittoh, MIT).
u32 Fetch3DMask(const u8 *container) {
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const u32 virtual_size = header->virtualSize;
  const u32 physical_size = header->physicalSize;
  // A version 0x0E container (XenosRecomp's container_0e.h) has the microcode as its whole physical part; 0x11 names where
  // it starts.
  u32 physical_offset = 0;
  if ((u32(header->flags) & 0xFFFFFF00u) != 0x102A0E00u) {
    const u32 shader_offset = header->shaderOffset;
    if (shader_offset + 4 > virtual_size)
      return 0;
    physical_offset = *reinterpret_cast<const be_u32 *>(container + shader_offset);
  }
  if (physical_offset >= physical_size)
    return 0;
  const auto *code = reinterpret_cast<const be_u32 *>(container + virtual_size + physical_offset);
  const u32 dwords = (physical_size - physical_offset) / 4;
  u32 mask = 0;
  u32 control_flow_end = dwords;  // the instructions start at the lowest address an exec names
  for (u32 at = 0; at + 3 <= control_flow_end; at += 3) {
    const u32 c0 = code[at], c1 = code[at + 1], c2 = code[at + 2];
    const u32 pair[2][2] = {{c0, c1 & 0xFFFF}, {(c1 >> 16) | (c2 << 16), c2 >> 16}};
    for (const auto &cf : pair) {
      const u32 opcode = (cf[1] >> 12) & 0xF;
      if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14))
        continue;
      const u32 address = cf[0] & 0xFFF;
      if (!address)
        continue;
      control_flow_end = std::min(control_flow_end, address * 3);
      const u32 count = (cf[0] >> 12) & 7;
      u32 sequence = (cf[0] >> 16) & 0xFFF;
      for (u32 i = 0; i < count; ++i, sequence >>= 2) {
        const u32 instruction = (address + i) * 3;
        if (instruction + 3 > dwords)
          return mask;
        if (!(sequence & 1))
          continue;
        const u32 w0 = code[instruction], w2 = code[instruction + 2];
        if ((w0 & 0x1F) == 1 && ((w2 >> 14) & 3) == 2)  // TextureFetch, TextureDimension::Texture3D
          mask |= 1u << ((w0 >> 20) & 31);
      }
    }
  }
  return mask;
}

// vivapinata-nx (2026-11-03, the white bird): the export mode in a vertex shader's header (XenosRecomp's Shader::field8, the word the
// console's Direct3D puts in SQ_PROGRAM_CNTL: the emulated GPU saw 0x17010009 for a header's 0x07010009). 7 is multipass.
// renut-ultimate: a 0x11 container only (a 0x0E one's header is laid out otherwise: no export mode read there).
bool ContainerIsExportOnly(const uint8_t *container) {
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  if ((u32(header->flags) & 0xFFFFFF00u) != 0x102A1100u)
    return false;
  const u32 word_offset = u32(header->shaderOffset) + 8;
  if (word_offset + 4 > u32(header->virtualSize))
    return false;
  return ((u32(*reinterpret_cast<const be_u32 *>(container + word_offset)) >> 24) & 7) == 7;
}

u32 CreateShaderRecord(const be_u32 *function, bool pixel) {
  if (!function)
    return 0;
  const auto *container = reinterpret_cast<const uint8_t *>(function);
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  // kameo-nx (2026-11-02, from feedingfrenzy-nx): version 0x0E too (XDK 2135 and 2571); the translator reads it through
  // XenosRecomp/container_0e.h, and only the sizes are read here.
  if ((u32(header->flags) & 0xFFFFFF00u) != 0x102A1100u && (u32(header->flags) & 0xFFFFFF00u) != 0x102A0E00u) {
    RGPU_ERROR("Create{}Shader: not a shader container (flags {:08X})", pixel ? "Pixel" : "Vertex",
               u32(header->flags));
    return 0;
  }

  auto *shader = CreateRecord<ShaderRecord>(pixel ? RecordKind::PixelShader
                                                  : RecordKind::VertexShader);
  if (!shader)
    return 0;
  const auto type = pixel ? d3d::ResourceType::PixelShader : d3d::ResourceType::VertexShader;
  shader->x360.resource.Common = kCommonD3DCreated | u32(type);
  shader->x360.resource.ReferenceCount = 1u;

  shader->hash = HashShaderContainer(container);
  shader->entry = FindShaderEntry(shader->hash);
  shader->fetch_3d_mask = Fetch3DMask(container);  // kameo-nx (2026-11-03)
  shader->export_only = !pixel && ContainerIsExportOnly(container);  // vivapinata-nx (2026-11-03)
  shader->fetch_filters = ScanFetchFilters(container);  // port 145 (tw2de_nx's, 2026-10-31)
  if (!shader->entry)
    CaptureMissingShader(container, pixel, shader->hash);
  NoteShaderSortKey(shader->self_va, shader->hash, ShaderSortableByHash(shader->hash, shader->entry, pixel));  // 2026-10-27
  PipelineShaderLoaded(shader->entry);
  // Draws before it may have found no shader at the address (FindShaderLocked).
  render_thread::Queue([va = shader->self_va] { ForgetShader(va); });
  return shader->self_va;
}

// The host side of a shader object the guest set up in place: its container is
// the virtual part inside the object followed by the physical part.
void RegisterGuestShader(u32 shader_va, u32 physical_va, bool pixel) {
  const u32 container_va = shader_va + (pixel ? kPixelShaderContainer : kVertexShaderContainer);
  const auto *header = GuestPtr<const d3d::ShaderContainer>(container_va);
  const u8 *physical = physical_va < 0x20000000u ? GuestPhysicalPtr<const u8>(physical_va)
                                                 : GuestPtr<const u8>(physical_va);
  if (!header || !physical)
    return;
  const u32 flags = header->flags;
  const u32 virtual_size = header->virtualSize;
  const u32 physical_size = header->physicalSize;
  if (((flags & 0xFFFFFF00u) != 0x102A1100u && (flags & 0xFFFFFF00u) != 0x102A0E00u) || ((flags & 1) != 0) == pixel ||  // kameo-nx
      virtual_size < sizeof(d3d::ShaderContainer) || virtual_size > 0x10000 ||
      physical_size == 0 || physical_size > 0x100000) {
    RGPU_ERROR("XGRegister{}Shader {:08X}: no shader container at {:08X} (flags {:08X})",
               pixel ? "Pixel" : "Vertex", shader_va, container_va, flags);
    return;
  }
  std::vector<uint8_t> container(size_t(virtual_size) + physical_size);
  std::memcpy(container.data(), header, virtual_size);
  std::memcpy(container.data() + virtual_size, physical, physical_size);
  const uint64_t hash = HashShaderContainer(container.data());
  const RecordKind kind = pixel ? RecordKind::PixelShader : RecordKind::VertexShader;
  const bool export_only = !pixel && ContainerIsExportOnly(container.data());  // vivapinata-nx (2026-11-03)
  // Captured here, where its container is at hand, only when the cache lacks it.
  const ShaderCacheEntry *const sort_entry = FindShaderEntry(hash);
  if (!sort_entry)
    CaptureMissingShader(container.data(), pixel, hash);
  NoteShaderSortKey(shader_va, hash, ShaderSortableByHash(hash, sort_entry, pixel));  // banjo-nx (2026-10-27)

  // In order with the draws that use it (render thread).
  const FetchFilterOverrides fetch_filters = ScanFetchFilters(container.data());  // port 145 (tw2de_nx's)
  render_thread::Queue([shader_va, pixel, hash, kind, fetch_3d_mask = Fetch3DMask(container.data()), export_only,
                        fetch_filters] {
    auto &shader = g_guest_shaders[shader_va];
    if (shader && shader->hash == hash && shader->kind == kind)
      return;
    // A different shader in the same memory: pipelines keep their own compiled
    // copy of the old one, so its module can go now. Draws before it may also
    // have found none there (FindShaderLocked).
    ForgetShader(shader_va);
    if (shader) {  // banjo-nx (2026-10-23, batch 6): the old one's host shader kept
      std::lock_guard shader_lock(shader->host_mutex);
      ReleaseHostShader(shader.get());  // banjo-nx (2026-10-28)
    }
    shader = std::make_unique<ShaderRecord>();
    shader->kind = kind;
    shader->self_va = shader_va;
    shader->hash = hash;
    shader->entry = FindShaderEntry(hash);
    shader->fetch_3d_mask = fetch_3d_mask;  // kameo-nx (2026-11-03)
    shader->export_only = export_only;  // vivapinata-nx (2026-11-03)
    shader->fetch_filters = fetch_filters;  // port 145
    PipelineShaderLoaded(shader->entry);
  });
  NoteInPlaceShader(shader_va, kind, sort_entry);  // blur-nx (2026-11-13, #13's fix): after the item above is queued
}

// void XGRegisterVertexShader(D3DVertexShader *pShader, void *pPhysicalPart) [0x826376C8]
// As the original: the header (a vertex shader of one reference), and for
// each of its bind slots (two when the container's flag 0x20 is set) the
// vertex fetch instructions copied from the microcode into the slot.
void XGRegisterVertexShader_hook(u32 shader, u32 physical) {
  auto *bytes = GuestPtr<u8>(shader);
  if (!bytes)
    return;
  auto word = [bytes](u32 offset) -> be_u32 & {
    return *reinterpret_cast<be_u32 *>(bytes + offset);
  };
  std::memset(bytes, 0, kVertexShaderContainer);
  word(kVertexShaderPhysical) = physical;
  word(0x00) = 6u;            // Common: D3DRTYPE_VERTEXSHADER
  word(0x04) = 1u;            // ReferenceCount
  word(0x14) = 0xFFFF0000u;   // BaseFlush
  const u32 slots = (u32(word(kVertexShaderContainer)) & 0x20) ? 2 : 1;
  for (u32 slot = 0; slot < slots; ++slot) {
    const u32 bind = 0x28 + slot * 0x1A0;
    const u32 table = u32(word(0x380 + slot * 8));
    const u32 fetches = u32(word(table + 0x384));
    const u32 microcode = u32(word(kVertexShaderPhysical)) + u32(word(table + 0x368));
    const u32 list = table + 0x368 + (u32(word(table + 0x380)) + 9) * 4;
    for (u32 i = 0; i < fetches; ++i) {
      const u32 source = (u32(word(list + i * 4)) & 0xFFF) * 12 + microcode;
      const u32 destination = shader + bind + 0x1C + i * 12;
      if (destination != source) {
        u8 *to = GuestPtr<u8>(destination);
        const u8 *from = GuestPtr<const u8>(source);
        if (to && from)
          std::memmove(to, from, 12);
      }
    }
    word(bind) = 0xFFFFFFFFu;
  }
  RegisterGuestShader(shader, physical, false);
}

// void XGRegisterPixelShader(D3DPixelShader *pShader, void *pPhysicalPart) [0x826375A0]
void XGRegisterPixelShader_hook(u32 shader, u32 physical) {
  auto *bytes = GuestPtr<u8>(shader);
  if (!bytes)
    return;
  auto *words = reinterpret_cast<be_u32 *>(bytes);
  std::memset(bytes, 0, kPixelShaderContainer);
  words[0] = 7u;            // Common: D3DRTYPE_PIXELSHADER
  words[1] = 1u;            // ReferenceCount
  words[5] = 0xFFFF0000u;   // BaseFlush
  words[6] = physical;      // the physical part (+0x18)
  RegisterGuestShader(shader, physical, true);
}

// D3DVertexShader *D3DDevice_CreateVertexShader(const DWORD *pFunction) [0x826377C0]
u32 D3DDevice_CreateVertexShader_hook(mapped_u32 function) {
  return CreateShaderRecord(static_cast<const be_u32 *>(function), false);
}

// D3DPixelShader *D3DDevice_CreatePixelShader(const DWORD *pFunction) [0x826375D8]
u32 D3DDevice_CreatePixelShader_hook(mapped_u32 function) {
  return CreateShaderRecord(static_cast<const be_u32 *>(function), true);
}

// D3DVertexDeclaration *D3DDevice_CreateVertexDeclaration(
//     const D3DVERTEXELEMENT9 *pVertexElements) [0x826379A0]
u32 D3DDevice_CreateVertexDeclaration_hook(rex::MappedPtr<GuestVertexElement> elements) {
  if (!elements)
    return 0;
  auto *declaration = CreateRecord<VertexDeclarationRecord>(RecordKind::VertexDeclaration);
  if (!declaration)
    return 0;
  declaration->x360.resource.Common =
      kCommonD3DCreated | u32(d3d::ResourceType::VertexDeclaration);
  declaration->x360.resource.ReferenceCount = 1u;

  const GuestVertexElement *element = elements;
  for (u32 i = 0; i < 64 && u32(element[i].Type) != kDeclTypeUnused; ++i)
    declaration->elements.push_back(element[i]);
  BuildDeclarationLayout(*declaration);
  return declaration->self_va;
}

// void D3DVertexShader_Bind(D3DVertexShader *pShader, D3DVertexDeclaration *pDecl,
//     const DWORD *pStreamStrides) [0x82628580] (TU3's build dropped the unused
// Flags and Index of the first release's)
// Patches a CVertexShader's vertex fetches to a declaration and the strides of
// streams 0..(highest stream in the declaration). Draws take their input layout
// from a declaration record instead, so nothing is patched; the declaration and
// strides are kept for draws that rely on the bind: N&B binds its shaders when
// it creates them and then draws with no declaration set, or with a zero
// SetStreamSource stride.
void D3DVertexShader_Bind_hook(u32 shader_va, u32 declaration_va, mapped_u32 stream_strides) {
  // The declaration and strides as they are now; the shader record is the
  // render thread's (draws read it), so it is updated there, in order.
  const auto *declaration =
      FindRecord<VertexDeclarationRecord>(declaration_va, RecordKind::VertexDeclaration);
  if (!declaration)
    return;
  u32 strides[d3d::kMaxStreams] = {};
  if (stream_strides) {
    const auto *guest_strides = static_cast<const be_u32 *>(stream_strides);
    for (const GuestVertexElement &element : declaration->elements) {
      const u32 stream = element.Stream;
      if (stream < d3d::kMaxStreams)
        strides[stream] = u32(guest_strides[stream]);
    }
  }
  const u32 binding = InternBinding(shader_va, *declaration, strides);
  render_thread::Queue([binding] { ApplyBindingLocked(binding); });
}

// Port 173 (from rehatched-nx, its commit 9d93d896): D3DVertexShader_Bind in the first release's form, (pShader, Flags,
// pDecl, pStreamStrides, Index), at XDK 2.0.9407 and 11164 (rehatched-nx's sub_82280DE8 reads the declaration's highest
// stream at r5 + 28 and the strides from r6). Unhooked, the game's own Bind read a declaration record of
// CreateVertexDeclaration's hook as its own and ran off the stack. Not bound here (Katamari's XDK has the three-argument
// form above); a port at such an XDK binds it at its address in place of D3DVertexShader_Bind_hook:
//   BANJO_RENUT_HOOK(sub_<its address>, D3DVertexShader_Bind_flags_hook);
[[maybe_unused]] void D3DVertexShader_Bind_flags_hook(u32 shader_va, u32 /*flags*/, u32 declaration_va,
                                                      mapped_u32 stream_strides, u32 /*index*/) {
  D3DVertexShader_Bind_hook(shader_va, declaration_va, stream_strides);
}

// Port 173 (from rehatched-nx, its commit 4b3295a9: Angry Birds' crash at the end of level 1): void
// D3DVertexDeclaration_GetDeclaration(D3DVertexDeclaration*, D3DVERTEXELEMENT9 *pElements, UINT *pNumElements). The XDK's
// copies min(count, *pNumElements) elements from the declaration (its count at +24, its elements at +52), adds
// D3DDECL_END when there is room and stores count + 1. Renut's declarations are records whose guest block holds no
// elements: unhooked, a game that asks for a declaration's elements (Angry Birds' mesh code) got an array of zeros with
// no end marker, and its search for a usage ran off the stack. This answers from the record as the XDK does, with
// D3DDECL_END built here (Stream 0xFF, Type D3DDECLTYPE_UNUSED). Not bound here (Katamari does not call it); a port whose
// game does binds it at its address:
//   BANJO_RENUT_HOOK(sub_<its address>, D3DVertexDeclaration_GetDeclaration_hook);
[[maybe_unused]] void D3DVertexDeclaration_GetDeclaration_hook(u32 declaration_va, u32 elements_va, u32 count_va) {
  auto *count_word = GuestPtr<be_u32>(count_va);
  const auto *declaration = FindRecord<VertexDeclarationRecord>(declaration_va, RecordKind::VertexDeclaration);
  if (!count_word || !declaration)
    return;
  const u32 have = u32(declaration->elements.size());
  const u32 room = u32(*count_word);
  *count_word = have + 1;
  auto *out = elements_va ? GuestPtr<GuestVertexElement>(elements_va) : nullptr;
  if (!out)
    return;
  const u32 copied = std::min(have, room);
  for (u32 i = 0; i < copied; ++i)
    out[i] = declaration->elements[i];
  if (room > have) {
    GuestVertexElement end{};
    end.Stream = u16(0xFF);
    end.Offset = u16(0);
    end.Type = kDeclTypeUnused;
    out[have] = end;
  }
}

} // namespace

BANJO_RENUT_HOOK(sub_824D6120, D3DDevice_CreateVertexShader_hook);  // D3DDevice_CreateVertexShader  // katamari-nx: N&B sub_826377C0, signature 0.85 (next 0.55)
BANJO_RENUT_HOOK(sub_824D6010, D3DDevice_CreatePixelShader_hook);  // D3DDevice_CreatePixelShader  // katamari-nx: N&B sub_826375D8, D3DDevice_CreatePixelShader: CreateVertexShader takes sub_824D6120 (0.85); this one's next best, 0.66
BANJO_RENUT_HOOK(sub_824D4C50, D3DDevice_CreateVertexDeclaration_hook);  // D3DDevice_CreateVertexDeclaration  // katamari-nx: N&B sub_826379A0, signature 0.77 (next 0.47)
BANJO_RENUT_UNHOOKED_HOOK(sub_8263FBD0, D3DVertexShader_Bind_hook);  // D3DVertexShader_Bind  // katamari-nx: not hooked (D3DVertexShader_Bind: no clear counterpart (sig 0.35/0.28))
BANJO_RENUT_HOOK(sub_824D4838, XGRegisterVertexShader_hook);  // XGRegisterVertexShader  // katamari-nx: N&B sub_826376C8, signature 0.91 (next 0.32)
BANJO_RENUT_UNHOOKED_HOOK(sub_826375A0, XGRegisterPixelShader_hook);  // XGRegisterPixelShader  // katamari-nx: not hooked (XGRegisterPixelShader: no counterpart (sig 0.26))
