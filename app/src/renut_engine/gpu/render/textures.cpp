/**
 * @file    gpu/render/textures.cpp
 */
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/stage_census.h"  // blur-nx (2026-11-17, the Renut audit's census)
#include "renut_engine/gpu/host_batch.h"  // blur-nx (2026-11-18, the Renut host batch)

#include <algorithm>
#include <array>  // port 257
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/ultimate.h"

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"

#include <atomic>
#include <condition_variable>  // port 171
#include <functional>
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

// sasrt-nx (2026-11-21, sasrt_resolve_remake_carry; port 257): render/draw.cpp, a resolve destination's content drawn
// into the texture made again for it.
bool SasrtCarryResolveContentLocked(HostTexture *from, HostTexture *to);

namespace {

// Fetch constant bits that describe the texture data (not sampling).
constexpr u32 kDescMask[6] = {
    0xFFC003FFu,  // type, signs, pitch, tiled (not clamp modes)
    0xFFFFFFFFu,  // format, endian, stacked, base address
    0xFFFFFFFFu,  // size
    0x0007FFFFu,  // number format, swizzle, exponent adjust (not filters)
    0x000003FCu,  // mip min / max level
    0xFFFFFE00u,  // dimension, packed mips, mip address (not border color)
};

constexpr uint32_t kMaxTextureSize = 8192;

std::unordered_map<u32, std::unique_ptr<HostTexture>> g_textures;

// Recent g_textures entries by VA: BindTextureLocked looks up every texture
// of every draw. The map's nodes stay put until erased (ReleaseHostTextureLocked
// drops the entry here first).
struct TextureMemo {
  u32 va = 0;
  std::unique_ptr<HostTexture> *slot = nullptr;
};
constexpr u32 kTextureMemoBits = 10;
TextureMemo g_texture_memo[1u << kTextureMemoBits];

TextureMemo &TextureMemoFor(u32 texture_va) {
  return g_texture_memo[(texture_va * 0x9E3779B1u) >> (32 - kTextureMemoBits)];
}

// g_textures[texture_va], through the memo.
std::unique_ptr<HostTexture> &TextureSlot(u32 texture_va) {
  TextureMemo &memo = TextureMemoFor(texture_va);
  if (memo.slot && memo.va == texture_va)
    return *memo.slot;
  auto &slot = g_textures[texture_va];
  memo = {texture_va, &slot};
  return slot;
}
// Depth resolve destinations with guest words, by base address (4 KB pages).
std::unordered_map<u32, HostTexture *> g_depth_words;
// The last resolve destination into each base address (4 KB pages): D3D
// textures are views of memory, and a game may resolve through one header and
// sample the same memory through another.
std::unordered_map<u32, HostTexture *> g_resolve_targets;
// sasrt-nx (2026-11-20, sasrt_resolve_alias_previous; port 257): the resolve destinations a page's entry held before the
// current one (two at most, newest first). SASRT's menus resolve an RGBA8 and an FP16 image of one size into the same
// memory: with the RGBA8 one the page's entry, the FP16 read found another format and was made from guest memory the GPU
// never wrote (a one-frame flash). Off in the base: banjo-nx saw no effect in N&B, and neither answer is the 360's.
REXCVAR_DEFINE_BOOL(sasrt_resolve_alias_previous, false, "SASRT/Graphics",
                    "A texture read of memory a resolve wrote, in another format than the page's latest resolve, takes "
                    "an earlier resolve destination of that page with its own description (format, size, memory) instead "
                    "of a texture made from guest memory the GPU never wrote (sasrt-nx's menus' one-frame flash, "
                    "2026-11-20). Off by default outside sasrt-nx")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
std::unordered_map<u32, std::array<HostTexture *, 2>> g_resolve_targets_previous;
// sasrt-nx (2026-11-21, sasrt_resolve_remake_carry; port 257): a resolve destination made again only for another scale
// or size (a dynamic resolution step) lost its content (a new image, discarded), where the console's memory keeps it. A
// destination with a part the game resolves only now and then lost that part: SASRT's 3-4 player far shadow cascades
// (the bottom half of one 512x1024 image, resolved only at the race's start) read NaN, 3.4e38 or old depths and drew
// flat dark. Now the old image is drawn (point sampled, scaled) into the new one, then retired. Console-verified in
// sasrt-nx (run 337).
REXCVAR_DEFINE_INT32(sasrt_resolve_remake_carry, 1, "SASRT/Graphics",
                     "A resolve destination made again for another render scale or size keeps its content: the old "
                     "image is drawn (scaled) into the new one before it is retired (sasrt-nx 2026-11-21: far shadow "
                     "cascades resolved only at a race's start were lost at the dynamic resolution's steps). 1 depth "
                     "destinations, 2 colour ones too, 0 off (the new image's content undefined, as before)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
uint64_t g_resolve_carried = 0, g_resolve_carry_failed = 0;
std::unordered_map<uint64_t, uint32_t> g_samplers;  // sampler key -> slot
std::vector<std::unique_ptr<plume::RenderSampler>> g_sampler_objects;
uint64_t g_frame = 1;

// Resources D3D::UnlockResource was called on since the last bind that
// looked (NoteTextureUnlocked; any thread, hence the lock of its own).
std::mutex g_unlocked_mutex;
std::vector<u32> g_unlocked;

// The bindless table filling up (past kEvictPressureSlots of its slots), or
// video memory nearing the budget the OS gives the process (past
// kEvictMemoryShare of it): host textures not bound or resolved into for
// kEvictIdleFrames go, checked every kEvictCheckFrames, and are made again
// from guest memory if bound later. Resolve destinations hold what only the
// GPU has and wait kEvictIdleGpuFrames. The section pools' textures normally
// go with their memory (ReleaseHostTexturesInMemoryLocked); this catches
// whatever else piles up: at 4K, resolve destinations of levels played
// earlier (2.2 GB of them after a few levels, 2026-10-15), while past the
// budget the OS moves allocations out to system memory and the frame rate
// sinks until it frees them.
constexpr uint64_t kEvictCheckFrames = 120;
constexpr uint64_t kEvictIdleFrames = 600;
constexpr uint64_t kEvictIdleGpuFrames = 3600;
constexpr uint32_t kEvictPressureSlots = kTextureDescriptorCount * 3 / 4;
constexpr double kEvictMemoryShare = 0.85;
// sasrt-nx (2026-11-09 night; port 58): EvictIdleTexturesLocked. OFF by default: with it on (1800) every docked race of
// SASRT's hung the GPU 80-150 s in (runs mem14, mem15, home_test: "a fence has not signalled", screen capture and HOME
// dead), 4-5 s after a batch of these releases; off, two races clean. Root cause not found (SASRT's follow-up):
// ReleaseHostTextureLocked retires and bumps the binding epochs, and the fast bind paths check them. Do not turn on
// before that is found. (On the Switch the driver budget is 0, so before this the backstop below never ran there.)
REXCVAR_DEFINE_INT32(sasrt_texture_long_idle_frames, 0, "SASRT/Graphics",
                     "Host textures no draw bound for this many frames are released even with the GPU budget and the "
                     "bindless table fine (made again from guest memory if bound again); 0: only under pressure, as "
                     "before. KNOWN TO HANG THE GPU on the Switch (SASRT, 1800): leave 0")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// renut-ultimate (2026-11-16, port 145; tw2de_nx's of 2026-11-13, ON at the user's word: "port the speed ups as on by
// default"): UploadDirtyRowsLocked.
REXCVAR_DEFINE_BOOL(banjo_renut_dynamic_texture_rows, true, "Banjo/Renut",
                    "tw2de_nx (exact): a CPU-written (linear, dynamic) texture is uploaded only in the rows whose memory "
                    "pages were written since its last upload (the store cards), not whole every frame (tw2de_nx: two "
                    "768x256 RGBA32F textures written in turn, 3 MB a frame read, swapped and uploaded). Same texels. "
                    "false: the whole texture every frame, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Port 171 (from fnxf_xiii, its notes of 2026-10-28 and 11-05): see UntileHelpers.
REXCVAR_DEFINE_BOOL(banjo_renut_untile_helpers, true, "Banjo/Renut",
                    "A large texture level (256 KB of guest blocks or more) untiled by the render thread and two helper "
                    "threads (cores 0 and 2, the render thread's priority) in bands (fnxf_xiii: a 4096x4096 DXT1 level "
                    "38 ms on the render thread alone). The same bytes. false: the render thread alone, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Port 168 (from fnxf_xiii, its note of 2026-11-05): see ParkLocked. Port 181: off by default. Blur's bisect first
// blamed it for ghosting and grain in its races; Blur corrected that the same day: the artifact came from slow
// PGO-instrumented builds (about 15 fps) and showed with the park off too. The park took back no texture in Blur or in
// Katamari (no "[park]" line), so it stays off for no gain there (docs/off-by-default.md section 3b); a game that
// uploads again textures it had (fnxf_xiii: 35 uploads in one frame) turns it on in its toml.
REXCVAR_DEFINE_BOOL(banjo_renut_texture_park, false, "Banjo/Renut",
                    "A host texture replaced at its address by another description is kept (up to 64, 96 MB, all "
                    "dropped when memory or the bindless table runs low) and taken back when that address asks for the "
                    "same layout and content again, instead of uploading it again (fnxf_xiii: 35 uploads of 36.7 MB in "
                    "one frame, all content a host texture had already). Off by default (port 181): no texture taken "
                    "back in Blur or Katamari. false: retired at once, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Port 161 (from fnxf_xiii): see VertexTextureBarrierLocked. A fix, so off by default here (fnxf_xiii: on).
REXCVAR_DEFINE_BOOL(banjo_deko3d_vertex_texture_barrier, false, "Banjo/Renut",
                    "deko3d: a draw whose vertex shader samples textures waits for all earlier GPU work when anything "
                    "was rendered or resolved since (a vertex shader read a resolve before it had landed)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-17, the Renut audit's #4): Create / Retire.
REXCVAR_DEFINE_BOOL(blur_renut_texture_pool, true, "Banjo/Renut",
                    "blur-nx (audit #4, exact): a render-target image Retire gives back (a resolve destination made "
                    "again) is kept, once the in-flight frames are done with it, and Create takes one of the same "
                    "description instead of making a new image (Blur's race: 65 new images a second, each a GPU "
                    "address mapping, 24 ms of the render thread a second). A new render target's content is undefined "
                    "either way (ResolveTarget discards it). false: every image made new and destroyed, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// renut-ultimate (2026-11-13, sasrt-nx's resolve project R2): ResolveTarget.
REXCVAR_DEFINE_BOOL(sasrt_gpu_words_probe_kept, true, "SASRT/GPU",
                    "Renut renderer (sasrt-nx's resolve project R2): a resolve destination made again at the same "
                    "address and host format (a dynamic-resolution step) keeps its depth words state "
                    "(banjo_renut_depth_words_lazy), so a destination whose words nothing reads does not write them for "
                    "another 300 resolves. A late reader is served as before (drawn again from an unchanged source, else "
                    "written from then on). Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Linear textures (see BindTextureLocked): how often a lock-driven texture
// is checked for writes that went around an unlock, and a dynamic one for
// having stopped changing; how long after an unlock the content is watched;
// and how many checks in a row must see a change to make a texture dynamic.
constexpr uint64_t kLinearVerifyFrames = 30;
constexpr uint64_t kUnlockWatchFrames = 8;
constexpr uint32_t kDynamicChanges = 2;

uint32_t NullSlot(TextureDimension dimension) {
  switch (dimension) {
  case TextureDimension::Tex3D:
    return kNullTexture3DSlot;
  case TextureDimension::Cube:
    return kNullTextureCubeSlot;
  default:
    return kNullTextureSlot;
  }
}

// Host dimension the shaders sample it as: stacked 2D arrays go through the
// 3D table, like tfetch3D reads them.
TextureDimension HostDimension(const TextureFetch &fetch) {
  if (fetch.Dimension() == TextureDimension::Tex2D && fetch.Stacked())
    return TextureDimension::Tex3D;
  if (fetch.Dimension() == TextureDimension::Tex1D)
    return TextureDimension::Tex2D;
  return fetch.Dimension();
}

// Host layers of a stacked texture: a 2D array view takes two at least.
uint32_t StackedLayers(const HostTexture &t) {
  return std::max(t.depth, 2u);
}

bool AllSigned(const TextureFetch &fetch) {
  for (u32 c = 0; c < 4; ++c) {
    if (fetch.Sign(c) != TextureSign::Signed)
      return false;
  }
  return true;
}

plume::RenderSwizzle ViewSwizzle(uint32_t component) {
  switch (component) {
  case 0:
    return plume::RenderSwizzle::R;
  case 1:
    return plume::RenderSwizzle::G;
  case 2:
    return plume::RenderSwizzle::B;
  case 3:
    return plume::RenderSwizzle::A;
  case 4:
    return plume::RenderSwizzle::ZERO;
  default:
    return plume::RenderSwizzle::ONE;
  }
}

// Host formats with a single component.
bool SingleComponent(plume::RenderFormat format) {
  using F = plume::RenderFormat;
  switch (format) {
  case F::R8_UNORM:
  case F::R8_SNORM:
  case F::R16_UNORM:
  case F::R16_SNORM:
  case F::R16_FLOAT:
  case F::R32_FLOAT:
  case F::R32_UINT:
  case F::BC4_UNORM:
  case F::BC4_SNORM:
    return true;
  default:
    return false;
  }
}

// The view mapping of a fetch constant swizzle: which stored component each
// shader component reads (0-3 XYZW, 4 zero, 5 one), with X and Z exchanged
// for content stored so (swap_red_blue, see Create). A one-component format
// fetches its value in every component: the game's depth restore
// (ps_beb4ddc38d3b2df5) writes the W of a depth resolve fetched with the
// identity swizzle as its depth, where a host view has 1.
plume::RenderComponentMapping SwizzleMapping(u32 swizzle, bool swap_red_blue,
                                             plume::RenderFormat format) {
  const bool single = SingleComponent(format);
  auto component = [&](uint32_t c) {
    uint32_t source = (swizzle >> (c * 3)) & 7;
    if (single && source < 4)
      source = 0;
    else if (swap_red_blue && (source == 0 || source == 2))
      source ^= 2;
    return ViewSwizzle(source);
  };
  return plume::RenderComponentMapping(component(0), component(1), component(2), component(3));
}

// The bindless slot of 't' seen through 'swizzle': its own view's when that is
// the swizzle it was made with, else a view made for it (kept with 't').
uint32_t SlotForSwizzle(HostTexture &t, u32 swizzle) {
  if (swizzle == t.view_swizzle || !t.texture)
    return t.slot;
  for (const HostTexture::SwizzleView &v : t.swizzle_views) {
    if (v.swizzle == swizzle)
      return v.slot;
  }
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.componentMapping = SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format);
  HostTexture::SwizzleView v;
  v.swizzle = swizzle;
  v.view = t.texture->createTextureView(desc);
  if (!v.view)
    return t.slot;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return t.slot;
  }
  Video::SetTextureSlotLocked(v.slot, t.texture.get(), v.view.get());
  t.swizzle_views.push_back(std::move(v));
  return t.swizzle_views.back().slot;
}

// Where a texture's guest data is depends only on the fetch constant bits in
// t.desc, so it is worked out once per texture.
void KnowGuestExtent(const TextureFetch &fetch, HostTexture &t) {
  if (!t.sample_known) {
    TextureGuestExtent(fetch, t.sample_pages, t.sample_sizes);
    t.sample_known = true;
  }
}

// The ends SampleHash reads of a region, and the spans between them.
constexpr uint32_t kSampleEndBytes = 256;
constexpr uint32_t kSampleProbes = 8;
constexpr uint32_t kSampleProbeBytes = 64;

// Cheap content fingerprint of a tiled texture: the first and last 256 bytes
// of the base level and of the mip chain, and spans spread through each. The
// game streams textures into memory another one used, with the same header
// (format, size, address): masks that are white at the edges (the hill's
// blend mask after the clearing's ring stencil) have the same ends, so the
// ends alone kept the old texture. Every bound texture is checked each frame
// and its guest memory is mostly out of the cache, so the cost is the lines
// read: eight spans, gathered and hashed at once. (32 spans hashed one at a
// time were a tenth of the render thread.)
uint64_t SampleHash(const TextureFetch &fetch, HostTexture &t) {
  KnowGuestExtent(fetch, t);
  alignas(64) uint8_t gathered[2 * (2 * kSampleEndBytes + kSampleProbes * kSampleProbeBytes)];
  uint32_t at = 0;
  for (u32 i = 0; i < 2; ++i) {
    const uint32_t page = t.sample_pages[i], size = t.sample_sizes[i];
    if (!page || !size)
      continue;
    const auto *data = GuestPhysicalPtr<const uint8_t>(page << 12);
    const uint32_t n = std::min<uint32_t>(size, kSampleEndBytes);
    std::memcpy(gathered + at, data, n);
    std::memcpy(gathered + at + n, data + size - n, n);
    at += 2 * n;
    if (size > 2 * n + kSampleProbeBytes) {
      const uint32_t span = size - 2 * n - kSampleProbeBytes;
      for (uint32_t p = 1; p <= kSampleProbes; ++p) {
        const uint32_t offset = n + uint32_t(uint64_t(span) * p / (kSampleProbes + 1));
        std::memcpy(gathered + at, data + offset, kSampleProbeBytes);
        at += kSampleProbeBytes;
      }
    }
  }
  return at ? banjo::renut_gpu::ContentHash64(gathered, at) : 0;  // banjo-nx: banjo_renut_pmull_hash
}

// Fingerprint of every guest byte of a texture, for the CPU-written linear
// ones, whose writes land anywhere: N&B's font cache (FontCache.cpp,
// sub_82BEB798) inflates each new glyph into its cell of a 1920x1920 texture
// between D3DLineTexture_LockRect and D3DArrayTexture_UnlockRect.
uint64_t ContentHash(const TextureFetch &fetch, HostTexture &t) {
  KnowGuestExtent(fetch, t);
  uint64_t hash = 0;
  for (u32 i = 0; i < 2; ++i) {
    const uint32_t page = t.sample_pages[i], size = t.sample_sizes[i];
    if (page && size)
      hash = banjo::renut_gpu::ContentHash64(GuestPhysicalPtr<const uint8_t>(page << 12), size, hash);  // banjo-nx
  }
  return hash;
}

// banjo-nx (2026-10-19, render-thread proposal 1, banjo_renut_store_cards): whether no page of the texture's guest
// extent (both ranges; their cards are read and cleared) was written since interval 'proven'.
bool ExtentClean(HostTexture &t, uint64_t proven) {
  bool clean = proven != UINT64_MAX;
  for (u32 i = 0; i < 2; ++i) {
    if (t.sample_pages[i] && t.sample_sizes[i])
      clean &= GuestRangeCardsClean(GuestPhysicalPtr<const u8>(t.sample_pages[i] << 12), t.sample_sizes[i], proven);
  }
  return clean;
}

// The content check's hash (SampleHash of a tiled texture, ContentHash of a linear one), or the last one taken when
// the cards prove the bytes unchanged since: then the bytes are those hashed, and so would the hash be.
uint64_t CheckedHash(const TextureFetch &fetch, HostTexture &t, bool sampled) {
  KnowGuestExtent(fetch, t);
  const bool cards = banjo::renut_gpu::StoreCardsOn();
  if (cards && ExtentClean(t, t.hash_interval)) {
    t.hash_interval = CardsInterval();
    // The bytes the hash would have read: all of a linear texture; SampleHash's ends and spans of a tiled one.
    uint64_t bytes = 0;
    for (u32 i = 0; i < 2; ++i) {
      const uint32_t size = t.sample_pages[i] ? t.sample_sizes[i] : 0;
      if (!sampled) {
        bytes += size;
      } else if (size) {
        const uint32_t n = std::min<uint32_t>(size, kSampleEndBytes);
        bytes += 2 * n + (size > 2 * n + kSampleProbeBytes ? kSampleProbes * kSampleProbeBytes : 0);
      }
    }
    if (banjo::renut_gpu::StoreCardsVerifyOn()) {
      const uint64_t hash = sampled ? SampleHash(fetch, t) : ContentHash(fetch, t);
      if (hash != t.last_hash) {
        static uint32_t logged = 0;
        if (logged++ < 32)
          RGPU_WARN("[cards] texture {:08X} (physical {:08X}, {} bytes) changed under clean cards: a writer of guest "
                    "memory that marks none (banjo_renut_store_cards_verify)",
                    t.guest_va, t.sample_pages[0] << 12, bytes);
        banjo::renut_gpu::CountTextureCards(bytes, 3);
        t.last_hash = hash;
        return hash;
      }
    }
    banjo::renut_gpu::CountTextureCards(bytes, 0);
    return t.last_hash;
  }
  const uint64_t hash = sampled ? SampleHash(fetch, t) : ContentHash(fetch, t);
  t.last_hash = hash;
  t.hash_interval = cards ? CardsInterval() : UINT64_MAX;
  if (cards)
    banjo::renut_gpu::CountTextureCards(0, 1);
  return hash;
}

struct LevelSource {
  const uint8_t *base = nullptr;  // start of the level's storage (or its packed tail)
  uint32_t pitch_blocks = 0;      // blocks per row of the storage
  uint32_t height_blocks = 0;     // rows of blocks per depth slice (3D)
  uint32_t x_blocks = 0, y_blocks = 0, z_offset = 0;  // offset inside a packed tail
};

// Untiles/copies one subresource (level, slice) into 'dst' rows of host blocks.
// Tiled rows are copied a run of blocks at a time where the tiling keeps
// them together, as the game's UntileSurface does.
void ReadLevel(const TextureFormatInfo &info, const TextureFetch &fetch, const LevelSource &src,
               uint32_t blocks_w, uint32_t blocks_h, uint32_t depth, uint8_t *dst,
               uint32_t dst_row_pitch, uint32_t dst_slice_pitch) {
  const uint32_t gbpb = info.guest_block_bytes;
  const uint32_t hbpb = info.host_block_bytes;
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(gbpb));
  const u32 endian = fetch.Endian();
  const bool is_3d = fetch.Dimension() == TextureDimension::Tex3D;
  const uint32_t run = is_3d ? 1 : TiledRunBlocks(bpb_log2);

  // Port 164 (from fnxf_xiii, its note of 2026-11-06): blocks kept as they are are gathered straight into the
  // staging row and swapped there (no row buffer, no second copy); the others through a per-thread row buffer (no
  // allocation per level). The same bytes in the same places.
  const bool direct = info.conversion == TexelConversion::Copy && hbpb == gbpb;
  thread_local std::vector<uint8_t> t_row;
  const size_t row_size = size_t(blocks_w) * gbpb;
  if (!direct && t_row.size() < row_size)
    t_row.resize(row_size);
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < blocks_h; ++y) {
      const uint32_t sy = y + src.y_blocks;
      const uint32_t sz = z + src.z_offset;
      uint8_t *out = dst + size_t(z) * dst_slice_pitch + size_t(y) * dst_row_pitch;
      uint8_t *row = direct ? out : t_row.data();
      if (fetch.Tiled()) {
        for (uint32_t x = 0; x < blocks_w;) {
          const uint32_t sx = x + src.x_blocks;
          const uint32_t offset =
              is_3d ? TiledOffset3D(sx, sy, sz, src.pitch_blocks, src.height_blocks, bpb_log2)
                    : TiledOffset2D(sx, sy, src.pitch_blocks, bpb_log2);
          const uint32_t count = std::min(run - sx % run, blocks_w - x);
          std::memcpy(row + size_t(x) * gbpb, src.base + offset, size_t(count) * gbpb);
          x += count;
        }
      } else {
        const size_t offset = (size_t(sz) * src.height_blocks + sy) * src.pitch_blocks * gbpb +
                              size_t(src.x_blocks) * gbpb;
        std::memcpy(row, src.base + offset, row_size);
      }
      SwapEndian(endian, row, row, row_size);
      if (direct)
        continue;
      if (info.conversion == TexelConversion::Copy)
        std::memcpy(out, row, size_t(blocks_w) * hbpb);
      else if (info.decoded_blocks)
        DecodeBlockRow(info.conversion, row, out, blocks_w, dst_row_pitch / info.block_size);
      else
        ConvertTexels(info.conversion, row, out, blocks_w);
    }
  }
}


// Port 171 (from fnxf_xiii, its notes of 2026-10-28 and 11-05; banjo_renut_untile_helpers): a large level untiled by
// the render thread and two helpers (cores 0 and 2, the render thread's priority), in twelve bands taken in turn.
// fnxf_xiii's area loads: uploads of 124-311 ms a frame, nearly all of it ReadLevel (a 4096x4096 DXT1 level 38 ms,
// 2048x2048 ones 12-17 ms), while the game waited. A band reads guest memory and writes its own rows of the staging
// buffer: the same bytes in the same places. A helper may join late or not at all: the render thread's part takes
// whatever the helpers did not, and only the helpers that joined are waited for.
class UntileHelpers {
public:
  static constexpr u32 kParts = 3;  // the render thread is part 0

  void Run(const std::function<void(u32)> &fn) {
    Start();
    {
      std::lock_guard lock(mutex_);
      fn_ = &fn;
      busy_ = 0;
      active_ = true;
      ++generation_;
    }
    wake_.notify_all();
    fn(0);
    std::unique_lock lock(mutex_);
    active_ = false;
    done_.wait(lock, [this] { return busy_ == 0; });
    fn_ = nullptr;
  }

  void Loop(u32 index) {
    banjo::renut_gpu::OnCopyHelperStart(index);
    uint64_t seen = 0;
    for (;;) {
      const std::function<void(u32)> *fn;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
        if (!active_)
          continue;  // that run is over
        fn = fn_;
        ++busy_;
      }
      (*fn)(index + 1);
      {
        std::lock_guard lock(mutex_);
        --busy_;
      }
      done_.notify_one();
    }
  }

private:
  void Start();
  bool started_ = false;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  uint64_t generation_ = 0;
  u32 busy_ = 0;         // helpers inside the current run's fn
  bool active_ = false;  // a run is taking helpers
  const std::function<void(u32)> *fn_ = nullptr;
};
UntileHelpers g_untile_helpers;

void UntileHelpers::Start() {
  if (started_)
    return;
  started_ = true;
  banjo::renut_gpu::StartLongLivedThread([] { g_untile_helpers.Loop(0); }, 64 << 10);
  banjo::renut_gpu::StartLongLivedThread([] { g_untile_helpers.Loop(1); }, 64 << 10);
}

// A level split in bands: 256 KB of guest blocks or more, one slice, 48 rows of blocks at least.
bool LevelIsLarge(const TextureFormatInfo &info, uint32_t blocks_w, uint32_t blocks_h, uint32_t depth) {
  return depth == 1 && blocks_h >= 3 * 16 && uint64_t(blocks_w) * blocks_h * info.guest_block_bytes >= (256u << 10);
}

void ReadLevelParallel(const TextureFormatInfo &info, const TextureFetch &fetch, const LevelSource &src,
                       uint32_t blocks_w, uint32_t blocks_h, uint32_t depth, uint8_t *dst, uint32_t dst_row_pitch,
                       uint32_t dst_slice_pitch) {
  static const bool on = REXCVAR_GET(banjo_renut_untile_helpers);
  if (!on || !LevelIsLarge(info, blocks_w, blocks_h, depth)) {
    ReadLevel(info, fetch, src, blocks_w, blocks_h, depth, dst, dst_row_pitch, dst_slice_pitch);
    return;
  }
  constexpr u32 kBands = 4 * UntileHelpers::kParts;
  std::atomic<u32> next{0};
  const std::function<void(u32)> part = [&](u32) {
    for (u32 k; (k = next.fetch_add(1, std::memory_order_relaxed)) < kBands;) {
      const uint32_t y0 = blocks_h * k / kBands, y1 = blocks_h * (k + 1) / kBands;
      if (y1 <= y0)
        continue;
      LevelSource band = src;
      band.y_blocks += y0;
      ReadLevel(info, fetch, band, blocks_w, y1 - y0, 1, dst + size_t(y0) * dst_row_pitch, dst_row_pitch,
                dst_slice_pitch);
    }
  };
  g_untile_helpers.Run(part);
}
// Staging data read from guest memory at submission rather than at the draw,
// the way the GPU reads a texture when it executes (see BindTextureLocked).
struct PendingFill {
  TextureFormatInfo info;
  TextureFetch fetch;
  LevelSource src;
  uint32_t blocks_w, blocks_h, depth;
  uint8_t *dst;
  uint32_t row_pitch, slice_pitch;
};
std::vector<PendingFill> g_pending_fills;

// dishonored-nx (2026-10-31, dishonored_renut_zero_level_recheck; port 104): whether a level's guest bytes (at most the
// first 64 KB) are all zero: a level the game has not streamed in yet (a zero DXT block is black).
static bool GuestLevelZero(const uint8_t *p, size_t bytes) {
  if (!p || !bytes)
    return false;
  bytes = std::min<size_t>(bytes, 64 * 1024);
  size_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    if (w)
      return false;
  }
  for (; i < bytes; ++i)
    if (p[i])
      return false;
  return true;
}
// Its counts, logged every 600 frames while a texture is watched.
uint64_t g_zero_rechecks = 0, g_zero_reuploads = 0, g_zero_marked = 0;

// Records the copies of every level into 't'. The guest data is read now, or
// when the command list is submitted if 'defer_read'.
bool Upload(HostTexture &t, const TextureFetch &fetch, bool defer_read) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kUpload);  // blur-nx (2026-11-17)
  // blur-nx (2026-11-18, the census's #7 and #24): which textures upload again, and how much.
  if (::renut::gpu::host_batch::CensusOn()) {
    const uint64_t bytes = uint64_t(t.width) * t.height * 4;  // (a size's measure: four bytes a texel)
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kUploadCalls);
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kUploadBytes, bytes);
    ::renut::gpu::host_batch::NoteUpload(t.guest_va, t.width, t.height, uint32_t(t.guest_format), bytes);
  }
  FlushResolveMipsLocked(t);  // banjo-nx (2026-10-28, banjo_renut_lazy_resolve_mips): from the level 0 the resolves left
  // banjo-nx (2026-10-19, render-thread proposal 1): the interval the guest bytes are read in: now, or at the next
  // kick's flush (FlushTextureFillsLocked, right after FlushVertexFillsLocked starts a new interval; a later one is
  // only more cautious).
  t.read_interval = CardsInterval() + (defer_read ? 1 : 0);
  // banjo-nx (2026-10-19, render-thread proposal 3): no g_texture_epoch bump: an upload changes content, not a binding.
  t.resolve_signature = 0;  // banjo-nx (banjo_renut_resolve_skip): not what a resolve left any more
  u32 block_width, block_height;
  FormatBlockSize(fetch.Format(), block_width, block_height);

  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  static const uint32_t kProfileLabel = gpu_profile::Label("texture uploads");
  gpu_profile::Mark(s, kProfileLabel);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  t.layout = plume::RenderTextureLayout::COPY_DEST;

  const bool is_3d_data = fetch.Dimension() == TextureDimension::Tex3D;
  // dishonored-nx (2026-10-31; port 104): dishonored_renut_zero_level_recheck: a level all zero in guest memory
  const bool zero_check = fetch.Tiled() && banjo::renut_gpu::ZeroLevelRecheckOn();
  bool any_zero = false;
  // Host array layers (cube faces, stacked layers); a stacked texture's extra
  // layer (StackedLayers) repeats its last one.
  const uint32_t slices = is_3d_data ? 1 : t.stacked ? StackedLayers(t) : t.depth;
  const uint32_t bs = t.info.block_size;
  for (uint32_t level = 0; level < t.mip_levels; ++level) {
    const uint32_t w = std::max(1u, t.width >> level);
    const uint32_t h = std::max(1u, t.height >> level);
    const uint32_t d = is_3d_data ? std::max(1u, t.depth >> level) : 1;
    const uint32_t blocks_w = (w + bs - 1) / bs;
    const uint32_t blocks_h = (h + bs - 1) / bs;

    // Where the level lives: base or mip memory, possibly inside the packed tail.
    const TextureLevelLayout layout = GetTextureLevelLayout(fetch, level);
    const uint32_t page = layout.page;
    if (!page)
      continue;
    LevelSource src;
    src.pitch_blocks = layout.pitch_texels / block_width;
    src.height_blocks = layout.height / block_height;
    src.x_blocks = layout.tail_x_blocks;
    src.y_blocks = layout.tail_y_blocks;
    src.z_offset = layout.tail_z;
    const uint32_t level_offset = layout.offset;

    const uint32_t row_bytes = blocks_w * t.info.host_block_bytes;
    // Footprint rows are 256-byte aligned: rows of blocks, or for decoded
    // blocks each of the block_size texel rows a row of blocks holds.
    const uint32_t row_align = t.info.decoded_blocks ? 256u * bs : 256u;
    const uint32_t row_pitch = (row_bytes + row_align - 1) / row_align * row_align;
    const uint32_t slice_pitch = row_pitch * blocks_h;
    for (uint32_t slice = 0; slice < slices; ++slice) {
      const uint32_t guest_slice = std::min(slice, t.depth - 1);
      src.base = GuestPhysicalPtr<const uint8_t>((page << 12) + level_offset +
                                                 guest_slice * layout.slice_bytes);
      UploadAllocation staging = s.uploads[s.frame].Allocate(uint64_t(slice_pitch) * d, 512);
      if (!staging)
        return false;
      auto *dst = static_cast<uint8_t *>(staging.cpu);
      if (defer_read)
        g_pending_fills.push_back({t.info, fetch, src, blocks_w, blocks_h, d, dst, row_pitch, slice_pitch});
      else
        ReadLevelParallel(t.info, fetch, src, blocks_w, blocks_h, d, dst, row_pitch, slice_pitch);  // port 171
      if (zero_check && !any_zero)  // dishonored-nx (port 104): the level's storage (a packed tail's: the tail)
        any_zero = GuestLevelZero(src.base, size_t(src.pitch_blocks) * src.height_blocks * t.info.guest_block_bytes *
                                                std::max(1u, layout.depth));
      // Footprints are in texels, block-aligned for compressed formats. Decoded
      // blocks are plain texels, so a level smaller than a block (a small mip)
      // takes only its own part of the decoded block.
      const uint32_t copy_w = std::min(blocks_w * bs, std::max(1u, t.alloc_width >> level));
      const uint32_t copy_h = std::min(blocks_h * bs, std::max(1u, t.alloc_height >> level));
      const plume::RenderBox level_box(0, 0, int32_t(copy_w), int32_t(copy_h), 0, int32_t(d));
      // plume's Vulkan copy takes its extent from the footprint and ignores the
      // box, and Vulkan allows no extent past the level (a compressed one ends
      // at the level's edge instead of a block's): the footprint is the level's
      // part there. D3D12 wants block-aligned footprints, cut by the box.
      const bool vulkan = UsesVulkanBindings();  // banjo-nx: deko3d copies as Vulkan does (the level's extent)
      const uint32_t row_texels = row_pitch / t.info.host_block_bytes * bs;
      if (is_3d_data && t.stacked) {
        // sasrt-nx (2026-10-20, sasrt_renut_deko3d_volume_as_array): each depth slice of the volume into its own layer.
        for (uint32_t z = 0; z < d; ++z) {
          cmd->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, z),
              plume::RenderTextureCopyLocation::PlacedFootprint(
                  staging.ref.ref, t.info.host, copy_w, copy_h, 1, row_texels,
                  staging.ref.offset + uint64_t(z) * slice_pitch),
              0, 0, 0, nullptr);
        }
      } else if (vulkan && d > 1 && copy_h != blocks_h * bs) {
        // The footprint's height is also its rows per depth slice there, which
        // the staging has blocks_h * bs of: one depth slice at a time.
        for (uint32_t z = 0; z < d; ++z) {
          cmd->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
              plume::RenderTextureCopyLocation::PlacedFootprint(
                  staging.ref.ref, t.info.host, copy_w, copy_h, 1, row_texels,
                  staging.ref.offset + uint64_t(z) * slice_pitch),
              0, 0, z, nullptr);
        }
      } else {
        cmd->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
            plume::RenderTextureCopyLocation::PlacedFootprint(
                staging.ref.ref, t.info.host, vulkan ? copy_w : blocks_w * bs,
                vulkan ? copy_h : blocks_h * bs, d, row_texels, staging.ref.offset),
            0, 0, 0, !vulkan && t.info.decoded_blocks ? &level_box : nullptr);
      }
    }
  }
  // dishonored-nx (2026-10-31, dishonored_renut_zero_level_recheck; port 104): a tiled upload with an all-zero level: its
  // whole guest data is hashed now and watched (BindTextureLocked) until it changes.
  const bool was_zero = t.zero_levels;
  t.zero_levels = zero_check && any_zero;
  if (t.zero_levels) {
    t.zero_hash = ContentHash(fetch, t);
    t.zero_check_frame = g_frame;
    if (!was_zero)
      ++g_zero_marked;
  }
  return true;
}

// The host levels a scaled resolve destination gets (HostTexture::extra_levels):
// down to a texel of at least the guest's.
uint32_t ExtraLevels(RenderScale scale) {
  const float factor = scale.Factor();
  return factor > 4.0f ? 3 : factor > 2.0f ? 2 : factor > 1.0f ? 1 : 0;
}

// swap_red_blue: the content was stored with X and Z exchanged (a guest
// resolve does that when the destination's X swizzle is not X). 'scale': see
// HostTexture::scale (content the GPU writes only). host_levels: a color
// resolve destination, which gets HostTexture::extra_levels when it is a
// scaled 2D one.
bool Create(HostTexture &t, const TextureFetch &fetch, bool render_target,
            plume::RenderFormat rt_format, bool swap_red_blue = false, RenderScale scale = {},
            bool host_levels = false) {
  ++g_texture_epoch;  // banjo-nx (CPU proposal 2)
  t.base_page = fetch.BasePage();  // banjo-nx (2026-10-27)
  BumpTextureBindings(t);
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  t.dimension = HostDimension(fetch);
  t.guest_format = fetch.Format();
  t.info = GetTextureFormatInfo(fetch.Format(), AllSigned(fetch));
  // sasrt-nx (2026-11-11, port 75; its formats batch, the diagnostic: renut_diag_formats): the first texture of each
  // guest format / host format.
  if (banjo::renut_gpu::DiagFormatsOn()) {
    static std::vector<uint32_t> seen;
    const plume::RenderFormat host = render_target ? rt_format : t.info.host;  // a resolve destination's: the target's
    const uint32_t key = u32(fetch.Format()) << 16 | u32(host) << 1 | (render_target ? 1u : 0u);
    if (seen.size() < 96 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
      seen.push_back(key);
      RGPU_INFO("[formats] first {} of guest format {} ({}): host format {}, {}x{}",
                render_target ? "resolve destination" : "texture", u32(fetch.Format()),
                GpuTextureFormatName(u32(fetch.Format())), int(host), width, height);
    }
  }
  t.width = width;
  t.height = height;
  t.depth = depth;
  t.scale = scale;
  // No more levels than the size has (GetMaxMipLevels, 0x8263DC20).
  const u32 largest = std::max({width, height, fetch.Dimension() == TextureDimension::Tex3D ? depth : 1u});
  const u32 size_levels = 32 - u32(std::countl_zero(largest));
  t.mip_levels = render_target ? 1 : std::min(fetch.MaxMipLevel() + 1, size_levels);
  t.render_target = render_target;
  plume::RenderFormat format = render_target ? rt_format : t.info.host;
  if (format == plume::RenderFormat::UNKNOWN)
    return false;

  plume::RenderDevice *device = Video::Device();
  plume::RenderTextureDesc desc;
  plume::RenderTextureViewDesc view_desc;
  const uint32_t bs = render_target ? 1 : t.info.block_size;
  const uint32_t w = scale.Size((t.width + bs - 1) / bs * bs);
  const uint32_t h = scale.Size((t.height + bs - 1) / bs * bs);
  t.extra_levels = 0;
  // banjo-nx (2026-10-21, banjo_renut_host_mips): off, no host levels at all (as at 1x and below).
  if (render_target && host_levels && t.dimension == TextureDimension::Tex2D &&
      State().texture_mip_ps && banjo::renut_gpu::HostMipsOn()) {
    // No more than the host size has.
    const uint32_t host_levels_max = 32 - uint32_t(std::countl_zero(std::max(w, h)));
    t.extra_levels = std::min(ExtraLevels(scale), host_levels_max - 1);
    t.mip_levels += t.extra_levels;
  }
  t.stacked = fetch.Dimension() == TextureDimension::Tex2D && fetch.Stacked();
  // sasrt-nx (2026-10-20, sasrt_renut_deko3d_volume_as_array): one level only, as a volume's depth halves with each
  // level and an array's layers do not; two slices at least, as a 2D array view takes two layers (StackedLayers).
  if (IsDeko3D() && !render_target && fetch.Dimension() == TextureDimension::Tex3D && t.mip_levels == 1 &&
      t.depth >= 2 && banjo::renut_gpu::Deko3DVolumeAsArrayOn())
    t.stacked = true;
  switch (t.dimension) {
  case TextureDimension::Tex3D:
    if (t.stacked) {
      // Viewed as a 2D array, which takes two layers at least (StackedLayers).
      desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, w, h, 1,
                                               t.mip_levels, StackedLayers(t), format);
      view_desc = plume::RenderTextureViewDesc::Texture2D(format);
    } else {
      desc = plume::RenderTextureDesc::Texture3D(w, h, t.depth, t.mip_levels, format);
      view_desc = plume::RenderTextureViewDesc::Texture3D(format);
    }
    break;
  case TextureDimension::Cube:
    desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, w, h, 1,
                                             t.mip_levels, 6, format,
                                             plume::RenderTextureFlag::CUBE);
    view_desc = plume::RenderTextureViewDesc::TextureCube(format);
    t.depth = 6;
    break;
  default:
    desc = plume::RenderTextureDesc::Texture2D(w, h, t.mip_levels, format);
    view_desc = plume::RenderTextureViewDesc::Texture2D(format);
    t.depth = 1;
    break;
  }
  if (render_target)
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  // banjo-nx (2026-10-20): whether the game uses stacked (2D array) or volume textures at all, once each (SASRT's
  // deko3d findings: plume's 2D view of a 2D array, and 3D images; docs/deko3d-port-faq.md).
  if (t.dimension == TextureDimension::Tex3D) {
    static std::atomic<bool> logged_stacked{false}, logged_volume{false};
    std::atomic<bool> &logged = t.stacked ? logged_stacked : logged_volume;
    if (!logged.exchange(true, std::memory_order_relaxed))
      RGPU_INFO("[textures] first {} texture: {}x{}x{}, {} levels, host format {}",
                t.stacked ? "stacked (2D array)" : "volume (3D)", w, h, t.depth, t.mip_levels, int(format));
  }
  t.host_format = format;
  t.alloc_width = w;
  t.alloc_height = h;
  // Beyond what the backends create (plume returns an object without a
  // resource then); Xenos textures are at most 8192 wide/high, and the
  // backends' limit is 16384.
  const uint32_t max_size = scale.Factor() > 1.0f ? kMaxTextureSize * 2 : kMaxTextureSize;
  if (w > max_size || h > max_size || t.depth > kMaxTextureSize)
    return false;
  // blur-nx (2026-11-17, audit #4, blur_renut_texture_pool): a render target's image from the pool when one matches.
  static const bool pool = REXCVAR_GET(blur_renut_texture_pool);
  t.pool_texture = nullptr;
  if (pool && render_target)
    t.texture = Video::TakePooledLocked(desc);
  const bool pooled = t.texture != nullptr;
  if (!pooled)
    t.texture = device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    ReportDeviceLost(device);
    return false;
  }
  if (pool && render_target) {
    t.pool_desc = desc;
    t.pool_texture = t.texture.get();
  }
  if (!pooled)
    t.texture->setName("renut texture");

  view_desc.mipLevels = t.mip_levels;
  view_desc.componentMapping = SwizzleMapping(fetch.Swizzle(), swap_red_blue, format);
  t.view_desc = view_desc;
  t.view_swizzle = fetch.Swizzle();
  t.view_swap_red_blue = swap_red_blue;
  t.view = t.texture->createTextureView(view_desc);
  if (!t.view)
    return false;
  t.layout = plume::RenderTextureLayout::UNKNOWN;
  t.slot = Video::AllocateTextureSlotLocked();
  if (t.slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  return true;
}

void MaskedDesc(const TextureFetch &fetch, u32 out[6]) {
  for (int i = 0; i < 6; ++i)
    out[i] = fetch.dword[i] & kDescMask[i];
}

void Retire(HostTexture &t) {
  t.mips_pending = false;  // banjo-nx (2026-10-28, banjo_renut_lazy_resolve_mips): nothing reads them now
  ++g_texture_epoch;  // banjo-nx (CPU proposal 2)
  BumpTextureBindings(t);  // banjo-nx (2026-10-27)
  DepthHandOverForgetLocked(t);  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover)
  if (t.slot != UINT32_MAX && t.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.slot);
  t.slot = UINT32_MAX;
  for (HostTexture::SwizzleView &v : t.swizzle_views) {
    Video::FreeTextureSlotLocked(v.slot);
    Video::RetireLocked(std::move(v.view));
  }
  t.swizzle_views.clear();
  if (t.level0_slot != UINT32_MAX && t.level0_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.level0_slot);
  t.level0_slot = UINT32_MAX;
  Video::RetireLocked(std::move(t.level0_view));
  Video::RetireLocked(std::move(t.copy_framebuffer));  // banjo-nx (2026-10-18)
  Video::RetireLocked(std::move(t.view));
  // blur-nx (2026-11-17, audit #4, blur_renut_texture_pool): the image Create made for it, back to the pool.
  if (t.texture && t.pool_texture == t.texture.get())
    Video::RetirePooledLocked(std::move(t.texture), t.pool_desc);
  else
    Video::RetireLocked(std::move(t.texture));
  t.pool_texture = nullptr;
  if (t.words) {
    for (HostTexture::WordsView &v : t.words_views) {  // banjo-nx (banjo_gpu_depth_words_view)
      if (v.slot != UINT32_MAX && v.slot != kInvalidDescriptor)
        Video::FreeTextureSlotLocked(v.slot);
      Video::RetireLocked(std::move(v.view));
    }
    t.words_views.clear();
    Video::RetireLocked(std::move(t.depth_framebuffer));
    Retire(*t.words);
    t.words.reset();
    std::erase_if(g_depth_words, [&t](const auto &entry) { return entry.second == &t; });
  }
  if (t.render_target) {
    std::erase_if(g_resolve_targets, [&t](const auto &entry) { return entry.second == &t; });
    // sasrt-nx (2026-11-20, sasrt_resolve_alias_previous; port 257): no earlier destination left pointing at it.
    for (auto it = g_resolve_targets_previous.begin(); it != g_resolve_targets_previous.end();) {
      auto &p = it->second;
      if (p[1] == &t)
        p[1] = nullptr;
      if (p[0] == &t) {
        p[0] = p[1];
        p[1] = nullptr;
      }
      it = p[0] ? std::next(it) : g_resolve_targets_previous.erase(it);
    }
  }
}

// Two descriptions of the same data: the same memory, format, endianness, size
// and dimension (fetch dwords 1, 2 and the dimension in 5). Fetch constants of
// the same memory may still differ in pitch, swizzle or mip fields.
bool SameData(const u32 a[6], const u32 b[6]) {
  constexpr u32 kDimensionMask = 0x600u;  // fetch dword 5 bits 9-10
  return a[1] == b[1] && a[2] == b[2] && (a[5] & kDimensionMask) == (b[5] & kDimensionMask);
}

// sasrt-nx (2026-10-21, sasrt_renut_texture_keep_on_signs): two descriptions that differ only in the component signs (dword 0
// bits 2-9), both or neither all signed (the only part of the signs Create uses: AllSigned).
bool SignsOnly(const u32 a[6], const u32 b[6]) {
  constexpr u32 kSigns = 0x3FCu;
  if (((a[0] ^ b[0]) & ~kSigns) != 0 || std::memcmp(a + 1, b + 1, 5 * sizeof(u32)) != 0)
    return false;
  const auto all_signed = [](u32 w) { return (w & kSigns) == 0x154u; };  // TextureSign::Signed (1) in all four
  return all_signed(a[0]) == all_signed(b[0]) && banjo::renut_gpu::TextureKeepOnSignsOn();
}

// The resolve destination another header wrote this texture's memory through,
// when it holds the same data (SameData). Only the GPU has what the resolve
// wrote.
HostTexture *ResolvedAlias(u32 texture_va, const TextureFetch &fetch, const u32 desc[6]) {
  auto it = g_resolve_targets.find(fetch.BasePage());
  if (it == g_resolve_targets.end())
    return nullptr;
  HostTexture *resolved = it->second;
  if (resolved->guest_va == texture_va || !resolved->texture)
    return nullptr;
  if (SameData(resolved->desc, desc))
    return resolved;
  // blur-nx (2026-11-10, port 76; its key 211: blur_renut_expand_alias): an EXPAND resolve sampled as its FLOAT twin, or
  // the other way (16_EXPAND / 16_FLOAT, 16_16_ / 16_16_FLOAT, 16_16_16_16_ / 16_16_16_16_FLOAT): the same memory, size,
  // endianness and dimension, only the format field differs, and the reference reads both as half floats (the SDK's
  // d3d12 texture_cache.cpp). Blur resolves its 640x360 HDR downsample as 16_16_16_16_EXPAND and reads it as
  // 16_16_16_16_FLOAT: without this the texture was made from guest memory, which the resolve never wrote (no bloom).
  // Only a destination the resolve made in half floats (its EDRAM format's host format).
  if (banjo::renut_gpu::ExpandAliasOn()) {
    constexpr u32 kFormatBits = 0x3Fu;
    constexpr u32 kDimensionMask = 0x600u;
    const u32 a = resolved->desc[1] & kFormatBits, b = desc[1] & kFormatBits;
    const auto twins = [](u32 x, u32 y) { return x >= 27 && x <= 29 && y == x + 3; };
    const bool half = resolved->host_format == plume::RenderFormat::R16_FLOAT ||
                      resolved->host_format == plume::RenderFormat::R16G16_FLOAT ||
                      resolved->host_format == plume::RenderFormat::R16G16B16A16_FLOAT;
    if (half && (twins(a, b) || twins(b, a)) && ((resolved->desc[1] ^ desc[1]) & ~kFormatBits) == 0 &&
        resolved->desc[2] == desc[2] && ((resolved->desc[5] ^ desc[5]) & kDimensionMask) == 0) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_INFO("[renut] texture {:08X} (format {}) reads resolve destination {:08X} ({}x{} format {}) through its "
                  "texture (blur_renut_expand_alias)", texture_va, b, resolved->guest_va, resolved->width,
                  resolved->height, a);
      return resolved;
    }
  }
  // sasrt-nx (2026-11-20, sasrt_resolve_alias_previous; port 257): an earlier destination of this page with the read's
  // own description (the same memory, format, size and dimension), whose image holds that resolve's content.
  if (!g_resolve_targets_previous.empty()) {
    auto p = g_resolve_targets_previous.find(fetch.BasePage());
    if (p != g_resolve_targets_previous.end()) {
      for (HostTexture *earlier : p->second) {
        if (earlier && earlier->texture && earlier->guest_va != texture_va && SameData(earlier->desc, desc)) {
          static uint32_t logged = 0;
          if (logged++ < 16)
            RGPU_INFO("[renut] texture {:08X} (format {}) reads the earlier resolve destination {:08X} ({}x{}) of its "
                      "page, not the latest {:08X} (format {}) (sasrt_resolve_alias_previous)", texture_va,
                      u32(fetch.Format()), earlier->guest_va, earlier->width, earlier->height, resolved->guest_va,
                      u32(resolved->guest_format));
          return earlier;
        }
      }
    }
  }
  return nullptr;
}

// The depth resolve whose words a texture described by 'fetch' reads: a
// 32-bit 8_8_8_8 texture over a depth resolve destination's memory, of its
// size.
HostTexture *DepthWordsSource(const TextureFetch &fetch) {
  if (fetch.Format() != GpuTextureFormat::Fmt8_8_8_8 || g_depth_words.empty())
    return nullptr;
  auto it = g_depth_words.find(fetch.BasePage());
  if (it == g_depth_words.end())
    return nullptr;
  HostTexture *source = it->second;
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  if (!source->words || source->width != width || source->height != height)
    return nullptr;
  return source;
}

// banjo-nx (banjo_gpu_depth_words_view, package 28): the slot of a view of the depth resolve's words that 't' (a
// texture over its memory) samples instead of its own copy, or UINT32_MAX (then the copy). The copy is of the whole
// words texture into a texture of the same format, size and one level, made at the first bind after each resolve, and
// every draw binds its textures again: the draws after a resolve read its words either way. So only a texture of that
// format, size and one level, sampled through its own view's description with the fetch's swizzle.
uint32_t WordsViewSlot(HostTexture &t, HostTexture &source, u32 swizzle) {
  if (!banjo::renut_gpu::DepthWordsViewOn() || !source.words || !source.words->texture ||
      t.host_format != kDepthResolveWordsFormat || t.dimension != TextureDimension::Tex2D || t.stacked ||
      t.mip_levels != 1 || t.alloc_width != source.words->alloc_width || t.alloc_height != source.words->alloc_height)
    return UINT32_MAX;
  for (const HostTexture::WordsView &v : source.words_views) {
    if (v.swizzle == swizzle)
      return v.slot;
  }
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.mipLevels = 1;
  desc.componentMapping = SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format);
  HostTexture::WordsView v;
  v.swizzle = swizzle;
  v.view = source.words->texture->createTextureView(desc);
  if (!v.view)
    return UINT32_MAX;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return UINT32_MAX;
  }
  Video::SetTextureSlotLocked(v.slot, source.words->texture.get(), v.view.get());
  ++g_texture_epoch;  // banjo-nx (CPU proposal 2): a new view
  source.words_views.push_back(std::move(v));
  return source.words_views.back().slot;
}

// Copies the depth resolve's words into 't' (of the source's scale) when it
// has resolved again.
void CopyDepthWords(HostTexture &t, HostTexture &source) {
  if (t.copied_resolve == source.resolves)
    return;
  FlushResolveMipsLocked(t);  // banjo-nx (2026-10-28, banjo_renut_lazy_resolve_mips)
  ++g_texture_epoch;  // banjo-nx (CPU proposal 2)
  plume::RenderCommandList *cmd = State().command_list;
  TransitionTextureLocked(source.words.get(), plume::RenderTextureLayout::COPY_SOURCE);
  TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_DEST);
  // renut-ultimate (port 64, 2026-11-10, sasrt-nx's GPU hang): never past either image (the copy is not checked below).
  const uint32_t copy_w = std::min({t.scale.Size(t.width), t.alloc_width, source.words->alloc_width});
  const uint32_t copy_h = std::min({t.scale.Size(t.height), t.alloc_height, source.words->alloc_height});
  if (copy_w != t.scale.Size(t.width) || copy_h != t.scale.Size(t.height)) {
    static uint32_t logged = 0;
    if (logged++ < 16)
      RGPU_WARN("[renut] depth words copy into {:08X} clamped to {}x{} (its image {}x{}, the words {}x{}, the scale's "
                "size {}x{})", t.guest_va, copy_w, copy_h, t.alloc_width, t.alloc_height, source.words->alloc_width,
                source.words->alloc_height, t.scale.Size(t.width), t.scale.Size(t.height));
  }
  const plume::RenderBox box(0, 0, int32_t(copy_w), int32_t(copy_h));
  t.resolve_signature = 0;  // banjo-nx (banjo_renut_resolve_skip)
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), 0, 0),
                         plume::RenderTextureCopyLocation::Subresource(source.words->texture.get()),
                         0, 0, 0, &box);
  t.copied_resolve = source.resolves;
  t.uploaded = true;
}

// blur-nx (2026-11-10, port 76; its key 212: blur_renut_resolve_keep_dest): colour formats a converting draw writes and
// reads exactly (render/draw.cpp has the same list). renut-ultimate (port 76): with R10G10B10A2_UNORM, 2_10_10_10's host
// format since port 75 (renut_format_10bit_exact; R16G16B16A16_UNORM, in blur-nx's list, before it).
bool ConvertibleResolveFormat(plume::RenderFormat f) {
  switch (f) {
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R10G10B10A2_UNORM:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R16_FLOAT:
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
    return true;
  default:
    return false;
  }
}

// blur-nx (2026-11-17, the Renut audit's #5): why ResolveTarget made a destination (ResolveRemakeReport): 1 none at the
// address, 2 a texture there, not a target, 3 another fetch description, 4 no image, 5 another host format, 6 another
// scale, 7 another allocated size; and render-target host textures released (by memory freed, idle, cold).
std::atomic<uint64_t> g_rt_made[8];
std::atomic<uint64_t> g_rt_released_memory{0}, g_rt_released_idle{0}, g_rt_released_cold{0};
void CountRtRelease(const HostTexture *t, std::atomic<uint64_t> &n) {
  if (t && t->render_target)
    n.fetch_add(1, std::memory_order_relaxed);
}

// GetResolveTextureLocked; 'fetch' receives the header's fetch constant.
HostTexture *ResolveTarget(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                           const d3d::TextureFetchConstant *header_format, bool depth,
                           TextureFetch &fetch, uint32_t cover_pixels = 0) {
  // banjo-nx (2026-10-19, render-thread proposal 3): no unconditional g_texture_epoch bump: Create and Retire bump it,
  // and so does a resolve target entry that changes (below).
  if (!texture_va)
    return nullptr;
  if (!header_format) {
    const auto *header = GuestPtr<const d3d::BaseTexture>(texture_va);
    if (!header)
      return nullptr;
    header_format = &header->Format;
  }
  fetch = TextureFetch::FromHeader(*header_format);
  if (!fetch.IsTexture())
    return nullptr;
  u32 desc[6];
  MaskedDesc(fetch, desc);

  auto &slot = TextureSlot(texture_va);
  // blur-nx (2026-11-10, port 76; its key 212: blur_renut_resolve_keep_dest): a single-sampled resolve of part of an
  // existing destination (the same header, size and scale) made from another format keeps it: the console converts into
  // the destination's format and leaves the rest. Blur's 1x1 2_10_10_10 resolve into the corner of its 640x360 half-float
  // image, every frame before the image's own resolve, made the image again (empty) for the pass that reads last
  // frame's. A resolve of the whole destination still makes it again in its source's format.
  const bool keep_other_format =
      slot && !depth && cover_pixels != 0 && slot->render_target && slot->texture && slot->host_format != format &&
      std::memcmp(slot->desc, desc, sizeof(desc)) == 0 && slot->scale == scale &&
      slot->alloc_width == scale.Size(slot->width) && slot->alloc_height == scale.Size(slot->height) &&
      cover_pixels < slot->width * slot->height && slot->dimension == TextureDimension::Tex2D && !slot->stacked &&
      ConvertibleResolveFormat(format) && ConvertibleResolveFormat(slot->host_format) &&
      banjo::renut_gpu::ResolveKeepDestOn();
  if (keep_other_format) {
    static uint32_t logged = 0;
    if (logged++ < 8)
      RGPU_INFO("[renut] resolve of {} pixels (format {}) into {:08X} ({}x{} format {}) keeps it and converts "
                "(blur_renut_resolve_keep_dest)", cover_pixels, u32(format), texture_va, slot->width, slot->height,
                u32(slot->host_format));
  } else if (slot && !depth && cover_pixels != 0 && slot->render_target && slot->host_format != format &&
             cover_pixels < slot->width * slot->height) {
    static uint32_t logged = 0;  // why a partial resolve of another format still made it again
    if (logged++ < 8)
      RGPU_INFO("[renut] resolve of {} pixels (format {}) into {:08X} ({}x{} format {}) makes it again: desc {} scale "
                "{} formats {}/{}", cover_pixels, u32(format), texture_va, slot->width, slot->height,
                u32(slot->host_format), std::memcmp(slot->desc, desc, sizeof(desc)) == 0 ? "same" : "differs",
                slot->scale == scale ? "same" : "differs", ConvertibleResolveFormat(format),
                ConvertibleResolveFormat(slot->host_format));
  }
  // banjo-nx (2026-10-28, night_gpu5, dynamic resolution steps): a destination of a settable code made at another
  // height of it (render/surfaces.cpp CodeForHeightLocked) is made again: its size is the old height's.
  // renut-ultimate (2026-11-13, sasrt-nx's resolve project R2, sasrt_gpu_words_probe_kept): what a destination made
  // again at the same address and host format knew about its depth words (banjo_renut_depth_words_lazy) goes to the new
  // one: its readers do not change with the scale. Before, each remake started the 300-resolve words probe again.
  static const bool probe_kept = REXCVAR_GET(sasrt_gpu_words_probe_kept);
  bool carry_words = false, carried_wanted = false;
  uint32_t carried_probe = 0;
  std::unique_ptr<HostTexture> carry_from;  // sasrt-nx (2026-11-21, sasrt_resolve_remake_carry; port 257)
  {  // blur-nx (2026-11-17, audit #5): the reason a destination is made (ResolveRemakeReport)
    uint32_t why = 0;
    if (!slot)
      why = 1;
    else if (!keep_other_format) {
      if (!slot->render_target)
        why = 2;
      else if (std::memcmp(slot->desc, desc, sizeof(desc)) != 0)
        why = 3;
      else if (!slot->texture)
        why = 4;
      else if (slot->host_format != format)
        why = 5;
      else if (slot->scale != scale)
        why = 6;
      else if (slot->alloc_width != scale.Size(slot->width) || slot->alloc_height != scale.Size(slot->height))
        why = 7;
    }
    if (why)
      g_rt_made[why].fetch_add(1, std::memory_order_relaxed);
  }
  if (slot && !keep_other_format && (!slot->render_target || std::memcmp(slot->desc, desc, sizeof(desc)) != 0 ||
               !slot->texture || slot->host_format != format || slot->scale != scale ||
               slot->alloc_width != scale.Size(slot->width) || slot->alloc_height != scale.Size(slot->height))) {
    if (probe_kept && slot->host_format == format) {
      carry_words = true;
      carried_wanted = slot->words_wanted;
      carried_probe = slot->words_probe;
    }
    // sasrt-nx (2026-11-21, sasrt_resolve_remake_carry; port 257): made again for its scale or size alone: its image is
    // kept until the new one has its content.
    static const int32_t carry_mode = REXCVAR_GET(sasrt_resolve_remake_carry);
    if (carry_mode > 0 && (depth || carry_mode >= 2) && slot->render_target && slot->texture &&
        slot->host_format == format && std::memcmp(slot->desc, desc, sizeof(desc)) == 0 && slot->resolves > 0 &&
        slot->dimension == TextureDimension::Tex2D && !slot->stacked && slot->mip_levels == 1 &&
        slot->extra_levels == 0)
      carry_from = std::move(slot);
    if (slot) {
      Retire(*slot);
      slot.reset();
    }
  }
  if (!slot) {
    slot = std::make_unique<HostTexture>();
    slot->guest_va = texture_va;
    std::memcpy(slot->desc, desc, sizeof(desc));
    // A depth destination has one component, which the swizzle places.
    if (!Create(*slot, fetch, true, format, !depth && (fetch.Swizzle() & 7) != 0, scale, !depth)) {
      RGPU_ERROR("Resolve destination {:08X}: {}x{} target creation failed", texture_va,
                 slot->width, slot->height);
      Retire(*slot);
      if (carry_from)  // sasrt-nx (2026-11-21; port 257)
        Retire(*carry_from);
      return nullptr;
    }
    if (carry_words) {  // renut-ultimate (2026-11-13, sasrt-nx's R2)
      slot->words_wanted = carried_wanted;
      slot->words_probe = carried_probe;
      static uint64_t kept_n = 0;
      if (const uint64_t n = ++kept_n; n <= 4 || (n & (n - 1)) == 0)
        RGPU_INFO("[renut] resolve destination {:08X} made again kept its depth words state (wanted {}, probe {}; {} "
                  "so far, sasrt_gpu_words_probe_kept)", texture_va, carried_wanted, carried_probe, n);
    }
    // A new render target has no defined content, and D3D12 wants it
    // discarded (or cleared) before a resolve or copy writes part of it.
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      TransitionTextureLocked(slot.get(), plume::RenderTextureLayout::COLOR_WRITE);
      cmd->discardTexture(slot->texture.get());
    }
    // sasrt-nx (2026-11-21, sasrt_resolve_remake_carry; port 257): the old image's content into it, then the old one
    // retired.
    if (carry_from) {
      const bool carried = SasrtCarryResolveContentLocked(carry_from.get(), slot.get());
      ++(carried ? g_resolve_carried : g_resolve_carry_failed);
      const uint64_t n = g_resolve_carried + g_resolve_carry_failed;
      if (n <= 8 || (n & (n - 1)) == 0)
        RGPU_INFO("[renut] resolve destination {:08X} ({}x{}, {}) made again at {}x{}: its content {} ({} carried, {} "
                  "not, so far; sasrt_resolve_remake_carry)", texture_va, slot->width, slot->height,
                  depth ? "depth" : "colour", slot->alloc_width, slot->alloc_height,
                  carried ? "drawn into the new image" : "NOT carried", g_resolve_carried, g_resolve_carry_failed);
      if (carried)
        slot->resolves = carry_from->resolves;  // it holds what they wrote
      Retire(*carry_from);
      carry_from.reset();
    }
  }
  slot->gpu_written = true;
  slot->uploaded = true;
  slot->used_frame = g_frame;
  HostTexture *&entry = g_resolve_targets[fetch.BasePage()];
  if (entry != slot.get()) {
    static const bool keep_previous = REXCVAR_GET(sasrt_resolve_alias_previous);
    if (keep_previous && entry) {  // sasrt-nx (2026-11-20, sasrt_resolve_alias_previous; port 257)
      auto &p = g_resolve_targets_previous[fetch.BasePage()];
      if (p[0] != entry) {
        p[1] = p[0] == slot.get() ? p[1] : p[0];
        p[0] = entry;
      }
      if (p[1] == slot.get() || p[1] == p[0])
        p[1] = nullptr;
    }
    entry = slot.get();
    ++g_texture_epoch;  // banjo-nx (2026-10-19, render-thread proposal 3): ResolvedAlias's answer changes
    BumpTexturePage(fetch.BasePage());  // banjo-nx (2026-10-27)
  }
  return slot.get();
}

} // namespace

namespace {

// Marks the host textures of the resources unlocked since the last call.
// Unlocks of anything else (vertex buffers, textures never bound) are dropped.
void ApplyUnlocksLocked() {
  std::vector<u32> unlocked;
  {
    std::lock_guard lock(g_unlocked_mutex);
    if (g_unlocked.empty())
      return;
    unlocked.swap(g_unlocked);
  }
  for (u32 va : unlocked) {
    auto it = g_textures.find(va);
    if (it != g_textures.end() && it->second) {
      it->second->unlocked = true;
      it->second->unlock_frame = g_frame;
      it->second->lock_driven = true;
    }
  }
}

// Linear textures are the CPU-written ones. The GPU reads them when it
// executes, after the CPU is done with the frame, so an upload reads memory
// as it is at submission; reading at the draw can catch writes still in
// progress. Only bytes that changed are worth uploading: converting the font
// cache every frame cost more than anything else in Present, and hashing its
// 7 MB every frame still cost more than half a millisecond. How a texture
// changes decides how it is checked, once per frame it is bound:
// - Lock-driven: the game writes it between a Lock and D3D::UnlockResource
//   (the font cache, sub_82BEB798). An unlock uploads; the content is watched
//   for kUnlockWatchFrames after one, and checked every kLinearVerifyFrames
//   for a write that went around the unlock (which ends lock-driven).
// - Dynamic: changed at kDynamicChanges checks in a row (the XMV logo movies,
//   whose decoder writes without a Lock and may still be writing at the
//   draw). Uploaded every frame; checked every kLinearVerifyFrames for having
//   stopped changing.
// - Otherwise the content is hashed at every check.
// A write after the check shows the next frame.

// renut-ultimate (2026-11-16, port 145; tw2de_nx's UploadDirtyRowsLocked of 2026-11-13, banjo_renut_dynamic_texture_rows):
// the rows of a dynamic linear texture whose pages were written since its last upload read them, uploaded alone (read
// at the next kick's flush, as a whole deferred upload is). The cards of its extent were just taken (ExtentClean said
// written), so each page's serial says whether it was written since. False: nothing recorded, the caller uploads it
// whole (a texture with levels, a 3D, stacked, tiled or block-compressed one, one under 16 rows, or most of it written).
uint64_t g_row_uploads[3] = {};  // [0] row uploads, [1] rows sent, [2] rows the whole uploads would have sent
bool UploadDirtyRowsLocked(HostTexture &t, const TextureFetch &fetch) {
  static const bool on = REXCVAR_GET(banjo_renut_dynamic_texture_rows);
  if (!on || !banjo::renut_gpu::StoreCardsOn() || banjo::renut_gpu::StoreCardsVerifyOn() || !t.uploaded ||
      t.read_interval == UINT64_MAX || !t.texture)
    return false;
  if (t.mip_levels != 1 || t.depth != 1 || t.stacked || fetch.Tiled() || fetch.Dimension() != TextureDimension::Tex2D ||
      t.info.decoded_blocks || t.info.block_size != 1 || t.info.host == plume::RenderFormat::UNKNOWN ||
      t.height < 16 || t.alloc_width != t.width || t.alloc_height != t.height)
    return false;
  u32 block_width, block_height;
  FormatBlockSize(fetch.Format(), block_width, block_height);
  const TextureLevelLayout layout = GetTextureLevelLayout(fetch, 0);
  if (block_width != 1 || block_height != 1 || !layout.page || layout.in_tail || layout.pitch_texels < t.width)
    return false;
  const uint32_t pitch_bytes = layout.pitch_texels * t.info.guest_block_bytes;
  const uint64_t base = (uint64_t(layout.page) << 12) + layout.offset;
  const uint32_t rows = t.height;
  const uint32_t first_page = u32(base >> 12), last_page = u32((base + uint64_t(rows) * pitch_bytes - 1) >> 12);
  // The runs of rows over written pages; two runs fewer than 4 rows apart are one.
  struct Run {
    uint32_t y0, y1;
  };
  Run runs[16];
  uint32_t run_count = 0, dirty_rows = 0;
  for (uint32_t page = first_page; page <= last_page; ++page) {
    if (GuestRangeCardsClean(GuestPhysicalPtr<const u8>(page << 12), 4096, t.read_interval))
      continue;
    const uint64_t p0 = uint64_t(page) << 12, p1 = p0 + 4096;
    const uint32_t y0 = p0 > base ? u32((p0 - base) / pitch_bytes) : 0;
    const uint32_t y1 = u32(std::min<uint64_t>(rows, (p1 - base + pitch_bytes - 1) / pitch_bytes));
    if (y1 <= y0)
      continue;
    if (run_count && y0 <= runs[run_count - 1].y1 + 4) {
      if (y1 > runs[run_count - 1].y1) {
        dirty_rows += y1 - std::max(y0, runs[run_count - 1].y1);
        runs[run_count - 1].y1 = y1;
      }
      continue;
    }
    if (run_count == 16)
      return false;
    runs[run_count++] = {y0, y1};
    dirty_rows += y1 - y0;
  }
  if (!run_count || uint64_t(dirty_rows) * 4 > uint64_t(rows) * 3)
    return false;  // nothing found on the level's pages (the whole upload decides), or most of it written
  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  const uint32_t row_bytes = t.width * t.info.host_block_bytes;
  const uint32_t row_pitch = (row_bytes + 255u) / 256u * 256u;
  UploadAllocation staging[16];
  for (uint32_t i = 0; i < run_count; ++i) {
    staging[i] = s.uploads[s.frame].Allocate(uint64_t(row_pitch) * (runs[i].y1 - runs[i].y0), 512);
    if (!staging[i])
      return false;  // nothing recorded yet
  }
  // This interval, not the next kick's (which is when the rows are read, and what a whole upload records): the rows
  // left out were only seen clean now. A page written after this and found by another check before the kick (its
  // cards taken in this interval) counts as written at this texture's next check; the price is that the rows
  // uploaded now are uploaded once more then.
  t.read_interval = CardsInterval();
  t.resolve_signature = 0;
  static const uint32_t kProfileLabel = gpu_profile::Label("texture uploads");
  gpu_profile::Mark(s, kProfileLabel);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  t.layout = plume::RenderTextureLayout::COPY_DEST;
  const uint32_t row_texels = row_pitch / t.info.host_block_bytes;
  for (uint32_t i = 0; i < run_count; ++i) {
    const uint32_t n = runs[i].y1 - runs[i].y0;
    LevelSource src;
    src.pitch_blocks = layout.pitch_texels;
    src.height_blocks = layout.height;
    src.x_blocks = layout.tail_x_blocks;
    src.y_blocks = layout.tail_y_blocks + runs[i].y0;
    src.z_offset = layout.tail_z;
    src.base = GuestPhysicalPtr<const uint8_t>(u32(base));
    g_pending_fills.push_back({t.info, fetch, src, t.width, n, 1, static_cast<uint8_t *>(staging[i].cpu), row_pitch,
                               row_pitch * n});
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), 0, 0),
                           plume::RenderTextureCopyLocation::PlacedFootprint(staging[i].ref.ref, t.info.host, t.width, n,
                                                                             1, row_texels, staging[i].ref.offset),
                           0, runs[i].y0, 0, nullptr);
  }
  ++g_row_uploads[0];
  g_row_uploads[1] += dirty_rows;
  g_row_uploads[2] += rows;
  if ((g_row_uploads[0] & 0x3FFF) == 1)
    RGPU_INFO("[renut] banjo_renut_dynamic_texture_rows: {} row uploads so far, {} of {} rows sent", g_row_uploads[0],
              g_row_uploads[1], g_row_uploads[2]);
  return true;
}

void RefreshLinearLocked(HostTexture &t, const TextureFetch &fetch) {
  ApplyUnlocksLocked();
  auto upload = [&](uint64_t hash) {
    t.content_hash = hash;
    t.uploaded = Upload(t, fetch, true);
  };
  const bool verify_due = g_frame - t.verified_frame >= kLinearVerifyFrames;
  // banjo-nx (2026-10-19, render-thread proposal 1): every hash through CheckedHash (the cards).
  if (!t.uploaded) {
    upload(CheckedHash(fetch, t, false));
    t.verified_frame = g_frame;
  } else if (t.lock_driven) {
    const bool watched = g_frame - t.unlock_frame < kUnlockWatchFrames;
    if (t.unlocked) {
      upload(CheckedHash(fetch, t, false));
    } else if (watched || verify_due) {
      const uint64_t hash = CheckedHash(fetch, t, false);
      if (hash != t.content_hash) {
        upload(hash);
        t.lock_driven = watched;
      }
      if (verify_due)
        t.verified_frame = g_frame;
    }
  } else if (t.dynamic) {
    if (verify_due) {
      const uint64_t hash = CheckedHash(fetch, t, false);
      t.dynamic = hash != t.content_hash;
      t.changes = 0;
      t.content_hash = hash;
      t.verified_frame = g_frame;
    }
    // banjo-nx (2026-10-19, render-thread proposal 1): not uploaded again while the cards prove no page written since
    // the last upload read it (the GPU's copy holds those bytes).
    KnowGuestExtent(fetch, t);
    if (banjo::renut_gpu::StoreCardsOn() && !banjo::renut_gpu::StoreCardsVerifyOn() &&
        ExtentClean(t, t.read_interval)) {
      banjo::renut_gpu::CountTextureCards(uint64_t(t.sample_sizes[0]) + t.sample_sizes[1], 2);
    } else if (!UploadDirtyRowsLocked(t, fetch)) {  // port 145 (banjo_renut_dynamic_texture_rows)
      t.uploaded = Upload(t, fetch, true);
    }
  } else {
    const uint64_t hash = CheckedHash(fetch, t, false);
    if (hash != t.content_hash) {
      upload(hash);
      t.dynamic = ++t.changes >= kDynamicChanges;
      t.verified_frame = g_frame;
    } else {
      t.changes = 0;
    }
  }
  t.unlocked = false;
}

// The backstop (kEvictPressureSlots, kEvictMemoryShare): with the bindless
// table filling up or video memory near its budget, the host textures idle
// for a while go.
// Port 168 (from fnxf_xiii, its note of 2026-11-05; banjo_renut_texture_park): parked host textures. fnxf_xiii's run
// 8b92fd2c: in play the game re-described 30-35 textures at their addresses and then brought the content back, and the
// renderer uploaded it again (35 uploads of 36.7 MB in 69 ms in one frame, 22 of 33 MB in 43 ms in another, all content
// a host texture had already, at the same address). A host texture replaced at its address by another description now
// waits here; when that address asks for an upload whose layout and content hash are a parked one's, that one is taken
// back instead. The same content check as the cache's own. Only textures from guest memory (none a resolve or the GPU
// wrote, none that changes all the time). At most kParkCount / kParkBytes (the oldest go first past them); all go when
// the GPU memory or the bindless table runs low (EvictColdTexturesLocked, EvictIdleTexturesLocked).
uint64_t HostTextureBytes(const HostTexture &t);  // further down
namespace {
uint64_t ParkContentKey(const u32 desc[6], uint64_t hash) {
  const u32 words[5] = {desc[0], desc[1] & 0xFFFu, desc[2], desc[5] & 0xFFFu, desc[3]};
  return XXH3_64bits_withSeed(words, sizeof(words), hash);
}
struct ParkedTexture {
  u32 va = 0;
  uint64_t key = 0, bytes = 0;
  std::unique_ptr<HostTexture> t;
};
constexpr size_t kParkCount = 64;
constexpr uint64_t kParkBytes = 96ull << 20;
std::vector<ParkedTexture> g_parked;
uint64_t g_parked_bytes = 0;
uint64_t g_parked_taken = 0, g_parked_taken_bytes = 0;

bool ParkOn() {
  static const bool on = REXCVAR_GET(banjo_renut_texture_park);
  return on;
}

bool Parkable(const HostTexture &t) {
  return ParkOn() && t.texture && t.uploaded && t.content_hash && !t.gpu_written && !t.render_target && !t.words &&
         !t.dynamic && !t.lock_driven && !t.zero_levels;
}

void UnparkAtLocked(size_t i, bool retire) {
  if (retire && g_parked[i].t)
    Retire(*g_parked[i].t);
  g_parked_bytes -= g_parked[i].bytes;
  g_parked.erase(g_parked.begin() + std::ptrdiff_t(i));
}

// 'slot' leaves its address (empty after). Requires Parkable(*slot).
void ParkLocked(u32 va, std::unique_ptr<HostTexture> &slot) {
  ++g_texture_epoch;  // as Retire does: bound sets made with it are made again
  BumpTextureBindings(*slot);
  DepthHandOverForgetLocked(*slot);
  ParkedTexture p;
  p.va = va;
  p.key = ParkContentKey(slot->desc, slot->content_hash);
  p.bytes = HostTextureBytes(*slot);
  p.t = std::move(slot);
  g_parked_bytes += p.bytes;
  g_parked.push_back(std::move(p));
  while (!g_parked.empty() && (g_parked.size() > kParkCount || g_parked_bytes > kParkBytes))
    UnparkAtLocked(0, true);  // the oldest first
}

// The parked texture of 'va' with this layout and content, put back in 'slot' (whose texture is parked or retired).
bool TakeParkedLocked(u32 va, std::unique_ptr<HostTexture> &slot, const u32 desc[6], uint64_t hash) {
  if (g_parked.empty() || !slot)
    return false;
  const uint64_t key = ParkContentKey(desc, hash);
  for (size_t i = g_parked.size(); i-- > 0;) {
    ParkedTexture &p = g_parked[i];
    if (p.va != va || p.key != key || !p.t || p.t->scale != slot->scale ||
        std::memcmp(p.t->desc, desc, 6 * sizeof(u32)) != 0)
      continue;
    std::unique_ptr<HostTexture> back = std::move(p.t);
    const uint64_t bytes = p.bytes;
    UnparkAtLocked(i, false);
    HostTexture &now = *slot;
    // The content check's state is the texture's now (the cards' interval of the hash just taken).
    back->last_hash = now.last_hash;
    back->hash_interval = now.hash_interval;
    back->content_hash = hash;
    back->uploaded = true;
    back->checked_frame = g_frame;
    back->used_frame = g_frame;
    if (Parkable(now)) {
      ParkLocked(va, slot);
    } else {
      Retire(now);
      slot.reset();
    }
    slot = std::move(back);
    ++g_texture_epoch;
    BumpTextureBindings(*slot);
    ++g_parked_taken;
    g_parked_taken_bytes += bytes;
    if ((g_parked_taken & (g_parked_taken - 1)) == 0)
      RGPU_INFO("[park] {} parked textures taken back ({} MB not uploaded again; banjo_renut_texture_park)",
                g_parked_taken, g_parked_taken_bytes >> 20);
    return true;
  }
  return false;
}
}  // namespace

void DropParkedTexturesLocked() {
  while (!g_parked.empty())
    UnparkAtLocked(g_parked.size() - 1, true);
}
void EvictIdleTexturesLocked() {
  const bool slots = Video::TextureSlotsInUseLocked() >= kEvictPressureSlots;
  GpuMemoryUsage memory;
  // sasrt-nx (2026-11-09 night, sasrt_texture_long_idle_frames; port 58, off): with the budget and the table fine, the
  // textures no draw bound for that long still go
  static const uint64_t long_idle = uint64_t(std::max(0, int(REXCVAR_GET(sasrt_texture_long_idle_frames))));
  bool long_only = false;
  if (!slots) {
    memory = QueryGpuMemory(State().device.get());
    if (!memory.valid || !memory.local_budget ||
        double(memory.local_used) < double(memory.local_budget) * kEvictMemoryShare) {
      if (!long_idle)
        return;
      long_only = true;
    }
  }
  if (!long_only)
    DropParkedTexturesLocked();  // port 168: memory or slots short: the parked textures go first
  std::vector<u32> idle;
  const bool keep_gpu_written = banjo::renut_gpu::KeepGpuWritten();  // banjo-nx
  for (const auto &[va, t] : g_textures) {
    if (t && t->gpu_written && keep_gpu_written)
      continue;  // banjo-nx: only the GPU has its content (banjo_renut_keep_gpu_written)
    uint64_t limit = t && t->gpu_written ? kEvictIdleGpuFrames : kEvictIdleFrames;
    if (long_only)
      limit = std::max(limit, long_idle);  // port 58
    if (!t || g_frame - t->used_frame >= limit)
      idle.push_back(va);
  }
  for (u32 va : idle) {
    if (auto it = g_textures.find(va); it != g_textures.end())
      CountRtRelease(it->second.get(), g_rt_released_idle);  // blur-nx (2026-11-17, audit #5)
    ReleaseHostTextureLocked(va);
  }
  if (long_only) {
    static uint32_t long_logged = 0;
    if (!idle.empty() && long_logged++ < 32)
      RGPU_INFO("[renut] {} host textures no draw bound for {} frames released (sasrt_texture_long_idle_frames)",
                idle.size(), long_idle);
  } else if (slots) {
    RGPU_WARN("Textures: bindless table nearly full; {} idle host textures released ({} of {} "
              "slots in use)",
              idle.size(), Video::TextureSlotsInUseLocked(), kTextureDescriptorCount);
  } else if (!idle.empty()) {
    RGPU_WARN("Textures: video memory near its budget ({} of {} MB); {} idle host textures "
              "released",
              memory.local_used >> 20, memory.local_budget >> 20, idle.size());
  }
}

// banjo-nx (2026-10-17): banjo_renut_memory_policy (banjo_renut_tuning.h). Every kColdCheckFrames frames, with less
// than banjo_renut_textures_reserve_mb left of the GPU memory budget (the driver's, VK_EXT_memory_budget: real
// footprints) or the bindless table three quarters full, the cold host textures go: not bound for kColdFrames frames
// (sa2-nx's kFramesNoUsageForDrop), so no command list still to run reads them (2 frames in flight; a slot given back
// is reused only once its frame has finished, DrainSlot) and a texture in use is never re-uploaded over and over. A
// released texture is made again from guest memory when bound, as it was made the first time. Never one the GPU wrote
// (banjo_renut_keep_gpu_written). If every texture is warm nothing goes: better a larger cache than thrash. Renut's
// backstop above stays (85 % of the budget, 600 frames), sized for a PC's video memory.
constexpr uint64_t kColdCheckFrames = 32;
constexpr uint64_t kColdFrames = 120;
// banjo-nx (2026-10-22): with less than half the reserve left the check comes every second of wall time whatever the
// frame rate (the town's load ran at 0.5-4 fps on the console: 32 frames were 8-60 s, and the heap ran out first), and
// a texture is cold after kColdFramesUrgent frames (still more than the 2 frames in flight).
constexpr uint64_t kColdFramesUrgent = 8;
bool g_memory_pressure = false;  // the last check found less than the reserve left

void EvictColdTexturesLocked(uint64_t reserve) {
  const bool slots = Video::TextureSlotsInUseLocked() >= kEvictPressureSlots;
  uint64_t left = 0, used = 0, budget = 0;
  bool low = false;
  if (!slots) {
    const GpuMemoryUsage memory = QueryGpuMemory(State().device.get());
    if (!memory.valid)
      return;
    // The device-local heaps where there are any (a PC's video memory); otherwise the shared ones.
    const bool local = memory.local_budget != 0;
    budget = local ? memory.local_budget : memory.shared_budget;
    used = local ? memory.local_used : memory.shared_used;
    if (!budget)
      return;
    left = budget > used ? budget - used : 0;
    // sasrt-nx (2026-11-09, sasrt_memory_governor; port 58): while the heap and the GPU region are low, too (the budget
    // is the driver's figure, the governor's is what a block or an allocation can still have)
    low = banjo::renut_gpu::MemoryLow();
    g_memory_pressure = left < reserve || low;
    if (left >= reserve && !low)
      return;
    DropParkedTexturesLocked();  // port 168: the parked textures first
    // renut-ultimate (2026-11-17, port 148): blur_renut_texture_pool's kept images first (Video::DropTexturePoolLocked).
    if (const size_t pooled = Video::DropTexturePoolLocked()) {
      static uint32_t pool_logged = 0;
      if (pool_logged++ < 8)
        RGPU_WARN("Textures: memory short (banjo_renut_memory_policy): {} pooled render-target images released "
                  "(blur_renut_texture_pool)", pooled);
    }
  }
  const uint64_t cold_frames =
      !slots && (left < reserve / 2 || (low && banjo::renut_gpu::MemoryCritical())) ? kColdFramesUrgent : kColdFrames;
  const bool keep_gpu_written = banjo::renut_gpu::KeepGpuWritten();
  std::vector<u32> cold;
  for (const auto &[va, t] : g_textures) {
    if (t && t->gpu_written && keep_gpu_written)
      continue;
    const uint64_t limit = t && t->gpu_written ? kEvictIdleGpuFrames : cold_frames;
    if (!t || g_frame - t->used_frame >= limit)
      cold.push_back(va);
  }
  for (u32 va : cold) {
    if (auto it = g_textures.find(va); it != g_textures.end())
      CountRtRelease(it->second.get(), g_rt_released_cold);  // blur-nx (2026-11-17, audit #5)
    ReleaseHostTextureLocked(va);
  }
  banjo::renut_gpu::CountReserveEviction(cold.size());
  static uint32_t logged = 0;
  if (!cold.empty() && logged++ < 32) {
    if (slots) {
      RGPU_WARN("Textures: bindless table three quarters full (banjo_renut_memory_policy); {} cold host textures "
                "released ({} of {} slots in use)",
                cold.size(), Video::TextureSlotsInUseLocked(), kTextureDescriptorCount);
    } else if (low && left >= reserve) {
      RGPU_WARN("Textures: {} MB free in the heap and GPU region (sasrt_memory_governor); {} cold host textures "
                "released", banjo::renut_gpu::g_heap_free_mb.load(std::memory_order_relaxed), cold.size());
    } else {
      RGPU_WARN("Textures: {} MB of the GPU memory budget left ({} of {} MB used), under the {} MB reserve "
                "(banjo_renut_memory_policy); {} cold host textures released",
                left >> 20, used >> 20, budget >> 20, reserve >> 20, cold.size());
    }
  }
}

} // namespace

uint64_t g_texture_frame = 1;  // banjo-nx (2026-10-19): g_frame, read inline by TextureFrameLocked

uint64_t g_texture_epoch = 1;  // banjo-nx (2026-10-19, CPU proposal 2)
uint64_t g_layout_epoch = 1;   // banjo-nx (2026-10-26): see textures.h
uint64_t g_depth_epoch = 1;    // banjo-nx (2026-10-19, render-thread proposal 3)

void AdvanceTextureFrameLocked() {
  ++g_frame;
  g_texture_frame = g_frame;
  ApplyUnlocksLocked();
  if (g_frame % kEvictCheckFrames == 0)
    EvictIdleTexturesLocked();
  // banjo-nx: banjo_renut_memory_policy. Every kColdCheckFrames frames, and under pressure every second.
  if (const uint64_t reserve = banjo::renut_gpu::TextureReserveBytes(); reserve) {
    static std::chrono::steady_clock::time_point last;
    bool due = g_frame % kColdCheckFrames == 0;
    if (!due && g_memory_pressure) {
      const auto now = std::chrono::steady_clock::now();
      due = now - last >= std::chrono::seconds(1);
    }
    if (due) {
      last = std::chrono::steady_clock::now();
      EvictColdTexturesLocked(reserve);
    }
  }
}

void ReleaseHostTexturesInMemoryLocked(u32 physical, u32 size) {
  if (!size)
    return;
  const u32 first_page = (physical & 0x1FFFFFFFu) >> 12;
  const u32 end_page = first_page + ((size + 0xFFFu) >> 12);
  auto inside = [&](u32 page) { return page >= first_page && page < end_page; };
  std::vector<u32> gone;
  for (const auto &[va, t] : g_textures) {
    if (!t)
      continue;
    const u32 base_page = t->desc[1] >> 12;
    const u32 mip_page = t->desc[5] >> 12;
    if (inside(base_page) || (mip_page && inside(mip_page)))
      gone.push_back(va);
  }
  for (u32 va : gone) {
    if (auto it = g_textures.find(va); it != g_textures.end())
      CountRtRelease(it->second.get(), g_rt_released_memory);  // blur-nx (2026-11-17, audit #5)
    ReleaseHostTextureLocked(va);
  }
}

// blur-nx (2026-11-17, the Renut audit's #4 and #5): the 10 s line (banjo_renut_tuning.cpp); atomics only.
std::string ResolveRemakeReport() {
  uint64_t made[8];
  uint64_t total = 0;
  for (int i = 1; i < 8; ++i)
    total += made[i] = g_rt_made[i].exchange(0, std::memory_order_relaxed);
  const uint64_t by_memory = g_rt_released_memory.exchange(0, std::memory_order_relaxed);
  const uint64_t by_idle = g_rt_released_idle.exchange(0, std::memory_order_relaxed);
  const uint64_t by_cold = g_rt_released_cold.exchange(0, std::memory_order_relaxed);
  std::string pool = Video::TexturePoolReportLocked();
  if (!total && pool.empty())
    return {};
  return fmt::format("resolve destinations made: {} (none at the address {}, a texture there {}, another fetch "
                     "description {}, no image {}, another host format {}, another scale {}, another size {}); target "
                     "textures released: {} with their memory, {} idle, {} cold{}{}",
                     total, made[1], made[2], made[3], made[4], made[5], made[6], made[7], by_memory, by_idle, by_cold,
                     pool.empty() ? "" : "; ", pool);
}

void NoteTextureUnlocked(u32 resource_va) {
  std::lock_guard lock(g_unlocked_mutex);
  g_unlocked.push_back(resource_va);
}

void FlushTextureFillsLocked() {
  for (const PendingFill &f : g_pending_fills) {
    ReadLevelParallel(f.info, f.fetch, f.src, f.blocks_w, f.blocks_h, f.depth, f.dst, f.row_pitch,
                      f.slice_pitch);  // port 171
  }
  g_pending_fills.clear();
}

HostTexture *FindHostTextureLocked(u32 texture_va) {
  auto it = g_textures.find(texture_va);
  return it != g_textures.end() ? it->second.get() : nullptr;
}

// vivapinata-nx (2026-11-11, vivapinata_resolve_from_sibling): a slot that reads 't''s texels as stored: a view with the
// x, y, z, w mapping and no red-blue swap whatever the texture's own view does (2026-11-12: the RGBA8 resolve destinations
// are viewed with red and blue swapped, so the swizzle views could not serve). A copy through it into a texture of the
// same format writes the stored bytes back, as a resolve of the same target would have. A single-component format's
// mapping has its one channel in x. Kept with 't' among its swizzle views under a key no fetch swizzle has (those are
// 12 bits).
uint32_t RawViewSlotLocked(HostTexture &t) {
  constexpr u32 kRawKey = 0x80000688u;
  if (!t.texture)
    return kInvalidDescriptor;
  for (const HostTexture::SwizzleView &v : t.swizzle_views) {
    if (v.swizzle == kRawKey)
      return v.slot;
  }
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.componentMapping = SwizzleMapping(0x688, false, t.host_format);
  HostTexture::SwizzleView v;
  v.swizzle = kRawKey;
  v.view = t.texture->createTextureView(desc);
  if (!v.view)
    return kInvalidDescriptor;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return kInvalidDescriptor;
  }
  Video::SetTextureSlotLocked(v.slot, t.texture.get(), v.view.get());
  t.swizzle_views.push_back(std::move(v));
  return t.swizzle_views.back().slot;
}

void ReleaseHostTextureLocked(u32 texture_va) {
  auto it = g_textures.find(texture_va);
  if (it == g_textures.end())
    return;
  Retire(*it->second);
  if (TextureMemo &memo = TextureMemoFor(texture_va); memo.va == texture_va)
    memo = {};
  g_textures.erase(it);
}

// banjo-nx (2026-10-19, banjo_deko3d_barrier_lean): a layout only read in, both sides: deko3d has no image layouts,
// and two reads need no ordering.
bool ReadOnlyLayout(plume::RenderTextureLayout layout) {
  return layout == plume::RenderTextureLayout::SHADER_READ || layout == plume::RenderTextureLayout::DEPTH_READ ||
         layout == plume::RenderTextureLayout::COPY_SOURCE || layout == plume::RenderTextureLayout::RESOLVE_SOURCE;
}

// Port 161 (from fnxf_xiii, its note of 2026-11-16; banjo_deko3d_vertex_texture_barrier): a texture made readable
// goes through a Fragments barrier (plume_deko3d): later draws' fragments wait, their VERTEX work does not, so a vertex
// shader that samples a texture written earlier in the frame (fnxf_xiii: FF XIII's flare samples the frame's linear
// depth, a resolve, 64 times around the light) could read it before the write had landed (the flare blinked at random).
// A draw whose vertex shader samples textures, after any GPU write since the last such barrier, gets a Primitives
// barrier (all earlier work finished) with the texture cache dropped. Pixel-shader reads keep the lighter barrier.
namespace {
uint64_t g_gpu_writes_since_vertex_barrier = 0;
bool WriteLayout(plume::RenderTextureLayout layout) {
  return layout == plume::RenderTextureLayout::COLOR_WRITE || layout == plume::RenderTextureLayout::DEPTH_WRITE ||
         layout == plume::RenderTextureLayout::COPY_DEST || layout == plume::RenderTextureLayout::RESOLVE_DEST ||
         layout == plume::RenderTextureLayout::GENERAL;
}
}  // namespace

void NoteGpuWriteLayoutLocked(plume::RenderTextureLayout layout) {
  if (WriteLayout(layout))
    ++g_gpu_writes_since_vertex_barrier;
}

void VertexTextureBarrierLocked() {
  static const bool on = REXCVAR_GET(banjo_deko3d_vertex_texture_barrier);
  if (!on || !IsDeko3D() || g_gpu_writes_since_vertex_barrier == 0)
    return;
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  // plume_deko3d: a SHADER_READ barrier without a texture drops the texture cache and waits (a null texture is
  // "written" and "sampled" there); the COMPUTE stage makes it Primitives.
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                plume::RenderTextureBarrier(nullptr, plume::RenderTextureLayout::SHADER_READ));
  g_gpu_writes_since_vertex_barrier = 0;
}

void TransitionTextureLocked(HostTexture *texture, plume::RenderTextureLayout layout) {
  if (!texture || !texture->texture || texture->layout == layout)
    return;
  ++g_layout_epoch;  // banjo-nx (2026-10-26)
  NoteGpuWriteLayoutLocked(layout);  // port 161 (banjo_deko3d_vertex_texture_barrier)
  if (IsDeko3D() && banjo::renut_gpu::Deko3DBarrierLeanOn() && ReadOnlyLayout(texture->layout) &&
      ReadOnlyLayout(layout)) {
    texture->layout = layout;
    return;
  }
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  const bool copy = layout == plume::RenderTextureLayout::COPY_DEST ||
                    layout == plume::RenderTextureLayout::COPY_SOURCE ||
                    layout == plume::RenderTextureLayout::RESOLVE_DEST;
  // sasrt-nx (2026-11-04, sasrt_deko3d_copy_wait): leaving a copy layout on deko3d is a copy-stage move too: plume
  // then emits DkBarrier_Full, the only barrier that waits for the copy engine (a fragment barrier drains the 3D
  // engine only, and the draw could sample a texture the engine was still writing).
  const bool from_copy = IsDeko3D() && banjo::renut_gpu::Deko3DCopyWaitOn() &&
                         (texture->layout == plume::RenderTextureLayout::COPY_DEST ||
                          texture->layout == plume::RenderTextureLayout::COPY_SOURCE ||
                          texture->layout == plume::RenderTextureLayout::RESOLVE_DEST ||
                          texture->layout == plume::RenderTextureLayout::RESOLVE_SOURCE);
  cmd->barriers(copy || from_copy ? plume::RenderBarrierStage::COPY : plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(texture->texture.get(), layout));
  texture->layout = layout;
}

TextureBinding BindTextureLocked(u32 texture_va, const TextureFetch &device_fetch, BindEffects *fx) {
  TextureBinding binding;
  binding.dimension = HostDimension(device_fetch);
  binding.slot = NullSlot(binding.dimension);
  if (!texture_va) {
    // sasrt-nx (2026-10-20, sasrt_renut_texture_fetch_only): a fetch constant set without SetTexture: its own texture,
    // keyed by its base address. renut-ultimate: bit 1 marks the key (a header's address has neither low bit; kameo-nx's
    // stacked views below use bit 0), bit 0 a stacked fetch (FillTextures' SampleAsStacked marks the device's copy).
    if (!device_fetch.IsTexture() || !banjo::renut_gpu::TextureFetchOnlyOn())
      return binding;
    // renut-ultimate (audit, 2026-11-07): bits 2-11 a hash of the data the fetch describes, so one page sampled as two
    // textures keeps both (with one key, each change between them made the texture again).
    u32 key_desc[6];
    MaskedDesc(device_fetch, key_desc);
    u32 key_hash = 0;
    for (const u32 word : key_desc)
      key_hash = (key_hash ^ word) * 0x9E3779B1u;
    texture_va = 2u | (device_fetch.Stacked() ? 1u : 0u) | (key_hash >> 22) << 2 | u32(device_fetch.BasePage()) << 12;
  }
  const auto *header = (texture_va & 2u) ? nullptr : GuestPtr<const d3d::BaseTexture>(texture_va);
  if (!header && !(texture_va & 2u))
    return binding;
  TextureFetch fetch = header ? TextureFetch::FromHeader(header->Format) : device_fetch;
  // kameo-nx (2026-11-03, banjo_renut_fetch3d_on_2d): FillTextures marks a 2D fetch stacked for a shader that reads it through
  // tfetch3D: a host texture of its own (a header can be sampled both ways), reached by the 3D table's stacked path.
  const bool as_stacked = device_fetch.Stacked() && !fetch.Stacked() && fetch.IsTexture();
  if (as_stacked)
    fetch.dword[1] |= 1u << 10;
  // SetTexture copied the header into the device's fetch constant. A header
  // that no longer matches it was a temporary the game has overwritten since
  // (a command buffer runs after its recording thread's stack moved on); the
  // device's copy is what the GPU reads.
  if (!fetch.IsTexture() || fetch.BasePage() != device_fetch.BasePage())
    fetch = device_fetch;
  if (!fetch.IsTexture())
    return binding;

  u32 desc[6];
  MaskedDesc(fetch, desc);
  // banjo-nx (2026-10-18, the one-frame colour flashes): a header read while the game rewrites it (Ryujinx run 34: a
  // bloom level's header with dwords 3-5 still zero, read as a 1D texture of other data) dropped the resolve's texture
  // and made one from guest memory. The device's copy, taken at SetTexture, is what the GPU reads: it wins whenever the
  // two describe different data.
  if (device_fetch.IsTexture() && std::memcmp(fetch.dword, device_fetch.dword, sizeof(fetch.dword)) != 0) {
    u32 device_desc[6];
    MaskedDesc(device_fetch, device_desc);
    if (!SameData(desc, device_desc)) {
      static uint32_t logged = 0;
      if (logged++ < 16)
        RGPU_INFO("[flash] frame {}: texture {:08X} bound by the device's fetch constant: its header describes other "
                  "data (desc {:08X} {:08X} {:08X} vs the device's {:08X} {:08X} {:08X})",
                  g_frame, texture_va, desc[1], desc[2], desc[5], device_desc[1], device_desc[2], device_desc[5]);
      fetch = device_fetch;
      std::memcpy(desc, device_desc, sizeof(desc));
    } else {
      // banjo-nx (2026-10-20, the colour flash, banjo_renut_device_swizzle): the same data, other described fields.
      // SetTexture copies dwords 0, 3 and 5 of the header into the device's fetch constant as they are (signs, pitch,
      // tiling; swizzle, number format, exponent adjust; dimension, packed mips, mip address) and changes only dword
      // 4's mip levels (the sampler's MinMipLevel / MaxMipLevel; d3d_native_state.cpp SetTexture): a header that
      // differs in 0, 3 or 5 is stale. Games build headers as stack temporaries (RenderDoc capture of Ryujinx, frame
      // 5437: a bloom level's header rewritten with the swizzle 1,1,0,0 turned the whole frame yellow); the device's
      // copy is what the GPU reads. The periodic report counts them ("[flash] header vs device", by dword).
      u32 differ = 0;
      for (int i = 0; i < 6; ++i)
        differ |= desc[i] != device_desc[i] ? 1u << i : 0u;
      if (differ) {
        banjo::renut_gpu::CountHeaderDescDiffer(differ);
        if (fetch.Swizzle() != device_fetch.Swizzle())
          banjo::renut_gpu::CountDeviceSwizzle(uint32_t(g_frame));
        if (differ & 0x29u) {
          static uint32_t logged = 0;
          if (logged++ < 16)
            RGPU_INFO("[flash] frame {}: texture {:08X} sampled through the device's fetch constant{}: its header (same "
                      "data) is stale: dword0 {:08X}/{:08X} dword3 {:08X}/{:08X} dword5 {:08X}/{:08X} (header/device)",
                      g_frame, texture_va, banjo::renut_gpu::DeviceSwizzleOn() ? "" : " NOT (key off)", desc[0],
                      device_desc[0], desc[3], device_desc[3], desc[5], device_desc[5]);
          if (banjo::renut_gpu::DeviceSwizzleOn()) {
            fetch = device_fetch;
            std::memcpy(desc, device_desc, sizeof(desc));
          }
        }
      }
    }
  }
  const u32 swizzle = fetch.Swizzle();
  // banjo-nx (2026-10-20, SASRT's finding): Renut does not apply the fetch constant's exponent adjust (the result times
  // 2^adjust). Counted, so a log shows whether the game uses it (the "[renut] exponent adjust" line).
  if (const int adjust = fetch.ExpAdjust())
    banjo::renut_gpu::CountExpAdjust(adjust);
  if (HostTexture *resolved = ResolvedAlias(texture_va, fetch, desc)) {
    resolved->used_frame = g_frame;
    if (resolved->words) {
      banjo::renut_gpu::CountDepthFloatSampled();  // banjo-nx (2026-10-18): a depth resolve's float target sampled
      DepthResolveReadLocked(*resolved, 1);        // vivapinata-nx (2026-11-12, vivapinata_depth_resolve_skip_unread)
    }
    FlushResolveMipsLocked(*resolved);  // banjo-nx (2026-10-28, banjo_renut_lazy_resolve_mips)
    TransitionTextureLocked(resolved, plume::RenderTextureLayout::SHADER_READ);
    if (fx) {
      fx->transition[0] = resolved;
      fx->float_sampled = resolved->words != nullptr;
    }
    binding.dimension = resolved->dimension;
    binding.slot = SlotForSwizzle(*resolved, swizzle);
    // banjo-nx (2026-10-19, banjo_deko3d_depth_handover): its content where it is.
    if (resolved->handed_from || resolved->held_current) {
      const uint32_t handed = DepthHandOverSlotLocked(
          *resolved, SwizzleMapping(swizzle, resolved->view_swap_red_blue, resolved->host_format), fx);
      if (handed != UINT32_MAX)
        binding.slot = handed;
    }
    binding.stacked = resolved->stacked;
    binding.scale = resolved->scale;
    binding.lod_scale = resolved->extra_levels ? resolved->scale : RenderScale{};
    return binding;
  }
  auto &slot = TextureSlot(as_stacked ? texture_va | 1 : texture_va);  // kameo-nx (2026-11-03)
  // A resolve destination sampled through a fetch constant that differs from
  // its header only in fields that leave the data alone (the mip range, say)
  // keeps what the resolve wrote.
  if (slot && std::memcmp(slot->desc, desc, sizeof(desc)) != 0 &&
      !(slot->gpu_written && SameData(slot->desc, desc)) && !SignsOnly(slot->desc, desc)) {  // sasrt-nx: SignsOnly
    // banjo-nx (2026-10-18, the one-frame colour flashes): a texture only the GPU has the content of, thrown away for a
    // fetch constant that reads the memory otherwise; the new one is filled from guest memory, which the resolve never
    // wrote (zero: green through the game's colour encoding on deko3d; stale on Vulkan).
    if (slot->gpu_written) {
      static uint32_t logged = 0;
      if (logged++ < 64)
        RGPU_WARN("[flash] frame {}: resolve destination {:08X} ({}x{} fmt {}) dropped for a fetch of other data "
                  "(desc {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                  g_frame, texture_va, slot->width, slot->height, u32(slot->guest_format), slot->desc[0],
                  slot->desc[1], slot->desc[2], slot->desc[3], slot->desc[4], slot->desc[5], desc[0], desc[1], desc[2],
                  desc[3], desc[4], desc[5]);
    }
    // port 168 (banjo_renut_texture_park): kept a while for its content's return
    if (Parkable(*slot)) {
      ParkLocked(as_stacked ? texture_va | 1 : texture_va, slot);
    } else {
      Retire(*slot);
      slot.reset();
    }
  }
  // What a depth resolve wrote to the memory (only the GPU has it), which is
  // at the resolve's scale; guest data is at the guest size.
  HostTexture *depth_source = !(slot && slot->gpu_written) ? DepthWordsSource(fetch) : nullptr;
  if (depth_source) {
    EnsureDepthWordsLocked(*depth_source);  // banjo-nx (banjo_renut_depth_words_lazy)
    if (fx)
      fx->depth_source = true;  // banjo-nx (2026-10-19, render-thread proposal 3)
  }
  const RenderScale scale = depth_source ? depth_source->scale : RenderScale{};
  if (slot && !slot->gpu_written && slot->scale != scale) {
    Retire(*slot);
    slot.reset();
  }
  // renut-ultimate (port 64, 2026-11-10, sasrt_texture_scale_size_check; sasrt-nx's GPU hang): the same code at another
  // size. Codes 4 and 6 are the dynamic resolution's settable ones: their ratio changes at each step and RenderScale
  // compares the code only, so a texture over a depth resolve made at 666 lines kept that image at 702. WordsViewSlot
  // then refused it (the sizes differ) and CopyDepthWords copied the scale's new, larger size into it: a GPU write past
  // the image, which hung the console once the memory batch packed live data behind it. ResolveTarget already makes a
  // destination again when alloc_width != scale.Size(width); this is the same rule for the textures sampling it.
  if (banjo::renut_gpu::TextureScaleSizeCheckOn() && slot && !slot->gpu_written && slot->texture && depth_source &&
      scale.code != 0 && plume::RenderFormatBlockWidth(slot->host_format) <= 1 &&
      (slot->alloc_width != scale.Size(slot->width) || slot->alloc_height != scale.Size(slot->height))) {
    static uint64_t remade = 0;
    if (const uint64_t n = ++remade; n <= 8 || (n & (n - 1)) == 0)
      RGPU_INFO("[renut] texture {:08X} over a depth resolve made again: {}x{} host, its scale's size now {}x{} (the "
                "code's size changed; sasrt_texture_scale_size_check; {} so far)", texture_va, slot->alloc_width,
                slot->alloc_height, scale.Size(slot->width), scale.Size(slot->height), n);
    Retire(*slot);
    slot.reset();
  }
  if (!slot) {
    // banjo-nx (2026-10-18, the colour flashes): memory a resolve wrote, sampled as other data than the resolve's
    // (ResolvedAlias said no) and not as its depth words: this texture is made from guest memory, which only has what
    // the CPU wrote there.
    if (!depth_source) {
      auto it = g_resolve_targets.find(fetch.BasePage());
      if (it != g_resolve_targets.end() && it->second && it->second->texture) {
        static uint32_t logged = 0;
        const HostTexture &r = *it->second;
        if (logged++ < 64)
          RGPU_WARN("[flash] frame {}: texture {:08X} (format {}) made from guest memory over resolve destination "
                    "{:08X} ({}x{} format {}; desc {:08X} {:08X} {:08X} vs {:08X} {:08X} {:08X})",
                    g_frame, texture_va, u32(fetch.Format()), r.guest_va, r.width, r.height, u32(r.guest_format),
                    r.desc[1], r.desc[2], r.desc[5], desc[1], desc[2], desc[5]);
      }
    }
    slot = std::make_unique<HostTexture>();
    slot->guest_va = texture_va;
    std::memcpy(slot->desc, desc, sizeof(desc));
    if (!Create(*slot, fetch, false, plume::RenderFormat::UNKNOWN, false, scale)) {
      static uint32_t logged = 0;
      if (logged++ < 16) {
        RGPU_WARN("Texture {:08X}: format {} {}x{}x{} not supported", texture_va,
                  u32(fetch.Format()), slot->width, slot->height, slot->depth);
      }
      Retire(*slot);
      return binding;  // keep the entry so it is not retried every draw
    }
  }
  HostTexture &t = *slot;
  if (banjo::renut_gpu::ResolveTraceOn() && t.gpu_written && t.trace_frame != g_frame) {
    t.trace_frame = g_frame;  // banjo-nx (package 28): banjo_renut_diag_resolve_trace
    RGPU_INFO("[trace] sample resolved {:08X} ({}x{} levels {} fmt {})", texture_va, t.width, t.height,
              t.extra_levels ? 1 + t.extra_levels : 1, u32(t.host_format));
  }
  // One that could not be made is not marked used: it goes once idle, and the
  // next bind tries again (the bindless table may have room by then).
  if (!t.texture)
    return binding;
  t.used_frame = g_frame;

  if (depth_source && !t.gpu_written) {
    // banjo-nx (banjo_gpu_depth_words_view, package 28): the resolve's words themselves, not a copy.
    if (const uint32_t words_slot = WordsViewSlot(t, *depth_source, swizzle); words_slot != UINT32_MAX) {
      t.copied_resolve = 0;  // its own texture has not got them: copied in full if the view is not used later
      TransitionTextureLocked(depth_source->words.get(), plume::RenderTextureLayout::SHADER_READ);
      banjo::renut_gpu::CountDepthWords(true);
      if (fx) {
        fx->transition[0] = depth_source->words.get();
        fx->words_viewed = true;
      }
      if (banjo::renut_gpu::ResolveTraceOn() && t.trace_frame != g_frame) {
        t.trace_frame = g_frame;  // banjo-nx (2026-10-19): which resolves' words are read, by the view
        RGPU_INFO("[trace] sample depth words (view) {:08X} ({}x{}) from the resolve of {:08X}", texture_va, t.width,
                  t.height, depth_source->guest_va);
      }
      binding.dimension = t.dimension;
      binding.slot = words_slot;
      binding.stacked = false;
      binding.scale = t.scale;
      binding.lod_scale = RenderScale{};
      return binding;
    }
    banjo::renut_gpu::CountDepthWords(false);
    if (banjo::renut_gpu::ResolveTraceOn() && t.trace_frame != g_frame) {
      t.trace_frame = g_frame;  // banjo-nx (2026-10-18): which header samples a depth resolve's words
      RGPU_INFO("[trace] sample depth words {:08X} ({}x{}) from the resolve of {:08X}", texture_va, t.width, t.height,
                depth_source->guest_va);
    }
    CopyDepthWords(t, *depth_source);
  } else if (!t.gpu_written && t.checked_frame != g_frame) {
    t.checked_frame = g_frame;
    if (!fetch.Tiled()) {
      RefreshLinearLocked(t, fetch);
    } else {
      const uint64_t hash = CheckedHash(fetch, t, true);  // banjo-nx (2026-10-19, render-thread proposal 1)
      // port 168 (banjo_renut_texture_park): a parked texture of this address with this content goes back in place of
      // an upload (the slot then holds it; bound again from the top, where its check of this frame is already done)
      if ((!t.uploaded || hash != t.content_hash) && hash &&
          TakeParkedLocked(as_stacked ? texture_va | 1 : texture_va, slot, desc, hash))
        return BindTextureLocked(texture_va, device_fetch, fx);
      if (!t.uploaded || hash != t.content_hash) {
        t.content_hash = hash;
        t.uploaded = Upload(t, fetch, false);
      } else if (t.zero_levels && g_frame - t.zero_check_frame >= 8) {
        // dishonored-nx (2026-10-31, dishonored_renut_zero_level_recheck; port 104): a level was all zeros at the upload:
        // the whole data hashed, at most 24 textures a frame, uploaded again once it changed.
        static uint64_t budget_frame = 0;
        static uint32_t budget = 0;
        if (budget_frame != g_frame) {
          budget_frame = g_frame;
          budget = 24;
        }
        if (budget) {
          --budget;
          ++g_zero_rechecks;
          t.zero_check_frame = g_frame;
          if (ContentHash(fetch, t) != t.zero_hash) {
            ++g_zero_reuploads;
            t.uploaded = Upload(t, fetch, false);
          }
        }
        static uint64_t report_frame = 0;
        if (g_frame - report_frame >= 600) {
          report_frame = g_frame;
          RGPU_INFO("[levels] textures with an all-zero level: {} marked, {} whole-data checks, {} uploaded again after "
                    "their data came in (dishonored_renut_zero_level_recheck)",
                    g_zero_marked, g_zero_rechecks, g_zero_reuploads);
        }
      }
    }
    // banjo-nx (2026-10-19, the colour flash): a texture neither a resolve nor an upload filled is sampled with what
    // its new image's memory held (deko3d does not clear it).
    if (!t.gpu_written && !t.uploaded) {
      static uint32_t logged = 0;
      if (logged++ < 32)
        RGPU_WARN("[flash] frame {}: texture {:08X} ({}x{} format {}{}) sampled with nothing written to it (the "
                  "upload from guest memory failed)",
                  g_frame, texture_va, t.width, t.height, u32(fetch.Format()), fetch.Tiled() ? ", tiled" : "");
    }
  }
  if (t.words)
    DepthResolveReadLocked(t);  // vivapinata-nx (2026-11-12, vivapinata_depth_resolve_skip_unread): before its transition
  FlushResolveMipsLocked(t);  // banjo-nx (2026-10-28, banjo_renut_lazy_resolve_mips)
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
  if (t.words)
    banjo::renut_gpu::CountDepthFloatSampled();  // banjo-nx (2026-10-18): a depth resolve's float target sampled
  if (fx) {
    fx->transition[0] = &t;
    fx->float_sampled = t.words != nullptr;
    fx->slot_texture = !depth_source;  // banjo-nx (2026-10-27): RefreshBindingLocked's
  }
  binding.dimension = t.dimension;
  binding.slot = SlotForSwizzle(t, swizzle);
  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover): its content where it is.
  if (t.handed_from || t.held_current) {
    const uint32_t handed = DepthHandOverSlotLocked(
        t, SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format), fx);
    if (handed != UINT32_MAX)
      binding.slot = handed;
  }
  binding.stacked = t.stacked;
  binding.scale = t.scale;
  binding.lod_scale = t.extra_levels ? t.scale : RenderScale{};
  return binding;
}

uint32_t g_texture_page_epochs[kTextureEpochBuckets] = {};  // banjo-nx (2026-10-27): textures.h
uint64_t g_texture_bind_gen = 1;  // banjo-nx (2026-10-27, Phase A): textures.h
uint32_t g_texture_va_epochs[kTextureEpochBuckets] = {};

bool RefreshBindingLocked(u32 texture_va, const TextureFetch &device_fetch, const BindEffects &fx) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kRefreshBinding);  // blur-nx (2026-11-17)
  HostTexture *t = fx.transition[0];
  if (!t || fx.depth_source || fx.words_viewed || fx.handover_surface || fx.handover_image || fx.transition[1])
    return false;
  t->used_frame = g_frame;
  // A resolve's alias, or a texture the GPU wrote: BindTextureLocked checks nothing for them.
  if (!fx.slot_texture || t->gpu_written || t->checked_frame == g_frame)
    return true;
  // The fetch BindTextureLocked takes: the header's, or the device's where the header no longer describes the same
  // data or (banjo_renut_device_swizzle) differs in dwords 0, 3 or 5 (its logs and counters left out).
  // renut-ultimate (audit, 2026-11-07): a slot bound by its fetch constant alone (sasrt_renut_texture_fetch_only, address
  // 0) checks its content from the device's fetch, as its bind does.
  const auto *header = texture_va ? GuestPtr<const d3d::BaseTexture>(texture_va) : nullptr;
  if (!header && (texture_va || !banjo::renut_gpu::TextureFetchOnlyOn()))
    return false;
  TextureFetch fetch = header ? TextureFetch::FromHeader(header->Format) : device_fetch;
  if (!fetch.IsTexture() || fetch.BasePage() != device_fetch.BasePage())
    fetch = device_fetch;
  if (!fetch.IsTexture())
    return false;
  if (device_fetch.IsTexture() && std::memcmp(fetch.dword, device_fetch.dword, sizeof(fetch.dword)) != 0) {
    u32 desc[6], device_desc[6];
    MaskedDesc(fetch, desc);
    MaskedDesc(device_fetch, device_desc);
    if (!SameData(desc, device_desc)) {
      fetch = device_fetch;
    } else {
      u32 differ = 0;
      for (int i = 0; i < 6; ++i)
        differ |= desc[i] != device_desc[i] ? 1u << i : 0u;
      if ((differ & 0x29u) && banjo::renut_gpu::DeviceSwizzleOn())
        fetch = device_fetch;
    }
  }
  t->checked_frame = g_frame;
  if (!fetch.Tiled()) {
    RefreshLinearLocked(*t, fetch);
  } else {
    const uint64_t hash = CheckedHash(fetch, *t, true);
    if (!t->uploaded || hash != t->content_hash) {
      t->content_hash = hash;
      t->uploaded = Upload(*t, fetch, false);
    }
  }
  return true;
}

namespace {
// Memo over the fetch constant: while memexport results are pending, every
// draw asks for its textures' extents (DrawNeedsExportFlush), and the layout
// math cost more than the rest of the check.
struct ExtentEntry {
  uint32_t key[6];
  uint32_t pages[2];
  uint32_t sizes[2];
  bool valid;
};
// banjo-nx (2026-10-23, render-thread round): 2,048 entries (96 KB): the city binds 400-odd textures a frame, which
// the 256 of before missed often enough to do the layout math again for most of them.
ExtentEntry g_extent_memo[2048];

ExtentEntry &ExtentSlot(const uint32_t key[6]) {
  uint32_t hash = 0;
  for (int i = 0; i < 6; ++i)
    hash = (hash ^ key[i]) * 0x9E3779B1u;
  return g_extent_memo[hash >> 21];
}
}  // namespace

void PrefetchTextureGuestExtent(const TextureFetch &fetch) {
  __builtin_prefetch(&ExtentSlot(fetch.dword));
}

void TextureGuestExtent(const TextureFetch &fetch, uint32_t pages[2], uint32_t sizes[2]) {
  using Entry = ExtentEntry;
  static_assert(sizeof(fetch.dword) == sizeof(Entry::key));
  // banjo-nx (2026-10-19, render-thread proposal 4): a plain static table of 256 (12 KB): both callers run on the render
  // thread (the draw's export check, KnowGuestExtent), and the thread-local one (48 KB, more than the A57's L1) cost a
  // call to read the thread pointer at every access (-mtp=soft).
  uint32_t key[6];
  std::memcpy(key, fetch.dword, sizeof(key));
  Entry &entry = ExtentSlot(key);
  if (entry.valid && std::memcmp(entry.key, key, sizeof(key)) == 0) {
    std::memcpy(pages, entry.pages, sizeof(entry.pages));
    std::memcpy(sizes, entry.sizes, sizeof(entry.sizes));
    return;
  }

  uint32_t base_bytes, mip_bytes;
  GetTextureDataSizes(fetch, base_bytes, mip_bytes);
  pages[0] = fetch.BasePage();
  sizes[0] = base_bytes;
  pages[1] = mip_bytes ? fetch.MipPage() : 0;
  sizes[1] = mip_bytes;
  std::memcpy(entry.key, key, sizeof(key));
  std::memcpy(entry.pages, pages, sizeof(entry.pages));
  std::memcpy(entry.sizes, sizes, sizeof(entry.sizes));
  entry.valid = true;
}

HostTexture *GetResolveTextureLocked(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                                     const d3d::TextureFetchConstant *header_format, uint32_t cover_pixels) {
  TextureFetch fetch;
  return ResolveTarget(texture_va, format, scale, header_format, false, fetch, cover_pixels);  // port 76 (212)
}

HostTexture *GetDepthResolveTextureLocked(u32 texture_va, RenderScale scale,
                                          const d3d::TextureFetchConstant *header_format) {
  TextureFetch fetch;
  HostTexture *t =
      ResolveTarget(texture_va, kDepthResolveFormat, scale, header_format, true, fetch);
  if (!t)
    return nullptr;
  if (!t->words) {
    auto words = std::make_unique<HostTexture>();
    words->width = t->width;
    words->height = t->height;
    words->scale = t->scale;
    words->alloc_width = t->alloc_width;
    words->alloc_height = t->alloc_height;
    words->host_format = kDepthResolveWordsFormat;
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
        t->alloc_width, t->alloc_height, 1, kDepthResolveWordsFormat);
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
    plume::RenderDevice *device = Video::Device();
    words->texture = device->createTexture(desc);
    if (!TextureCreated(words->texture.get())) {
      ReportDeviceLost(device);
      return nullptr;
    }
    words->texture->setName("renut depth resolve words");
    const plume::RenderTexture *targets[] = {t->texture.get(), words->texture.get()};
    t->depth_framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(targets, 2));
    if (!t->depth_framebuffer) {
      Video::RetireLocked(std::move(words->texture));
      return nullptr;
    }
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      TransitionTextureLocked(words.get(), plume::RenderTextureLayout::COLOR_WRITE);
      cmd->discardTexture(words->texture.get());
    }
    t->words = std::move(words);
  }
  HostTexture *&words_entry = g_depth_words[fetch.BasePage()];
  if (words_entry != t) {
    words_entry = t;
    ++g_texture_epoch;  // banjo-nx (2026-10-19, render-thread proposal 3): DepthWordsSource's answer changes
    BumpTexturePage(fetch.BasePage());  // banjo-nx (2026-10-27)
  }
  ++g_depth_epoch;  // banjo-nx (2026-10-19): the words of this destination may change with this resolve
  ++t->resolves;
  return t;
}

namespace {

// Single-level color targets the host levels are drawn into (plume's barriers
// cover a whole texture, so a level cannot be drawn while level 0 of the same
// texture is sampled) and multisampled resolves staged in: a bare target
// (texture, layout, size) and its framebuffer, by format and size. Kept for
// reuse.
struct ScratchTarget {
  HostTexture target;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  // Its bindless slot, for the next level down to read (ScratchSlot).
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = UINT32_MAX;
};

uint32_t ScratchSlot(ScratchTarget &scratch) {
  if (scratch.slot != UINT32_MAX)
    return scratch.slot;
  scratch.view = scratch.target.texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(scratch.target.host_format));
  if (!scratch.view)
    return kInvalidDescriptor;
  scratch.slot = Video::AllocateTextureSlotLocked();
  if (scratch.slot != kInvalidDescriptor)
    Video::SetTextureSlotLocked(scratch.slot, scratch.target.texture.get(), scratch.view.get());
  return scratch.slot;
}
std::unordered_map<uint64_t, std::unique_ptr<ScratchTarget>> g_scratch_targets;
// The level pipelines, by render target format.
std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> g_mip_pipelines;

ScratchTarget *GetScratchTarget(plume::RenderFormat format, uint32_t width, uint32_t height) {
  const uint64_t key = uint64_t(format) << 40 | uint64_t(width) << 20 | height;
  auto &entry = g_scratch_targets[key];
  if (entry)
    return entry.get();
  auto scratch = std::make_unique<ScratchTarget>();
  HostTexture &t = scratch->target;
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, format);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  plume::RenderDevice *device = Video::Device();
  t.texture = device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    ReportDeviceLost(device);
    return nullptr;
  }
  t.texture->setName("renut scratch target");
  t.host_format = format;
  t.width = t.alloc_width = width;
  t.height = t.alloc_height = height;
  const plume::RenderTexture *targets[] = {t.texture.get()};
  scratch->framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
  if (!scratch->framebuffer) {
    Video::RetireLocked(std::move(t.texture));
    return nullptr;
  }
  // Like a resolve destination, defined before anything writes part of it.
  if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
    TransitionTextureLocked(&t, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->discardTexture(t.texture.get());
  }
  entry = std::move(scratch);
  return entry.get();
}

plume::RenderPipeline *MipPipeline(plume::RenderFormat format) {
  auto &pipeline = g_mip_pipelines[uint32_t(format)];
  if (pipeline)
    return pipeline.get();
  auto &s = State();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = s.texture_mip_ps.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  pipeline = s.device->createGraphicsPipeline(desc);
  if (!pipeline) {
    RGPU_ERROR("Texture mip pipeline creation failed (format {})", uint32_t(format));
    return nullptr;
  }
  pipeline->setName("renut texture mip");
  return pipeline.get();
}

// Level 0 of 't' without the fetch swizzle, in the texture table.
uint32_t Level0Slot(HostTexture &t) {
  if (t.level0_slot != UINT32_MAX)
    return t.level0_slot;
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.mipSlice = 0;
  desc.mipLevels = 1;
  desc.componentMapping = plume::RenderComponentMapping();
  t.level0_view = t.texture->createTextureView(desc);
  if (!t.level0_view)
    return kInvalidDescriptor;
  t.level0_slot = Video::AllocateTextureSlotLocked();
  if (t.level0_slot != kInvalidDescriptor)
    Video::SetTextureSlotLocked(t.level0_slot, t.texture.get(), t.level0_view.get());
  return t.level0_slot;
}

} // namespace

HostTexture *ResolveStagingLocked(const HostTexture *dest) {
  ScratchTarget *scratch = GetScratchTarget(dest->host_format, dest->alloc_width, dest->alloc_height);
  return scratch ? &scratch->target : nullptr;
}

// blur-nx (2026-11-10, port 76; its key 219: blur_renut_resolve_exp_bias): see textures.h.
HostTexture *ResolveSourceStagingLocked(plume::RenderFormat format, uint32_t width, uint32_t height) {
  ScratchTarget *scratch = GetScratchTarget(format, width, height);
  return scratch ? &scratch->target : nullptr;
}

uint32_t ResolveSourceStagingSlotLocked(plume::RenderFormat format, uint32_t width, uint32_t height) {
  ScratchTarget *scratch = GetScratchTarget(format, width, height);
  return scratch ? ScratchSlot(*scratch) : kInvalidDescriptor;
}

void UpdateResolveMipsLocked(HostTexture *texture, const plume::RenderRect &rect) {
  auto &s = State();
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!texture || !texture->texture || !texture->extra_levels || !cmd || !s.texture_mip_ps)
    return;
  HostTexture &t = *texture;
  plume::RenderPipeline *pipeline = MipPipeline(t.host_format);
  const uint32_t source_slot = Level0Slot(t);
  if (!pipeline || source_slot == kInvalidDescriptor)
    return;
  static const uint32_t kProfileLabel = gpu_profile::Label("resolve host mip levels");
  gpu_profile::Mark(s, kProfileLabel);

  // Each level is the 2x2 box of the one above: level 1 of level 0, the rest
  // of the previous level's scratch target. The rect is first widened to
  // whole texels of the smallest level, so each level's region is exactly
  // what the next one reads (the scratch targets are shared, and hold other
  // textures' texels outside what was drawn now).
  struct Level {
    ScratchTarget *scratch;
    plume::RenderRect region;
  };
  Level levels[kMaxExtraLevels] = {};
  const uint32_t count = std::min(t.extra_levels, uint32_t(std::size(levels)));
  const int32_t align = 1 << count;
  const plume::RenderRect aligned(rect.left & ~(align - 1), rect.top & ~(align - 1),
                                  (rect.right + align - 1) & ~(align - 1),
                                  (rect.bottom + align - 1) & ~(align - 1));
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
  uint32_t source = source_slot;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t level = i + 1;
    const int32_t span = 1 << level;
    const uint32_t width = std::max(1u, t.alloc_width >> level);
    const uint32_t height = std::max(1u, t.alloc_height >> level);
    Level &l = levels[i];
    l.region = plume::RenderRect(std::min(aligned.left >> level, int32_t(width)),
                                 std::min(aligned.top >> level, int32_t(height)),
                                 std::min((aligned.right + span - 1) >> level, int32_t(width)),
                                 std::min((aligned.bottom + span - 1) >> level, int32_t(height)));
    if (l.region.right <= l.region.left || l.region.bottom <= l.region.top)
      break;
    l.scratch = GetScratchTarget(t.host_format, width, height);
    if (!l.scratch || source == kInvalidDescriptor) {
      l.scratch = nullptr;
      break;
    }
    if (i > 0) {
      // The level above, drawn just now, is read.
      TransitionTextureLocked(&levels[i - 1].scratch->target, plume::RenderTextureLayout::SHADER_READ);
    }
    TransitionTextureLocked(&l.scratch->target, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->setFramebuffer(l.scratch->framebuffer.get());
    cmd->setGraphicsPipelineLayout(s.utility_layout.get());
    cmd->setPipeline(pipeline);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
    // Pixel coordinates are texels of the level.
    cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
    cmd->setScissors(l.region);
    UtilityPush push{};
    push.scale[0] = push.scale[1] = 1.0f;
    push.texture_index = source;
    push.sampler_index = kPointClampSamplerSlot;
    push.param0 = 2.0f;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    cmd->drawInstanced(3, 1, 0, 0);
    source = ScratchSlot(*l.scratch);
  }

  TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_DEST);
  for (uint32_t i = 0; i < count; ++i) {
    const Level &l = levels[i];
    if (!l.scratch)
      continue;
    TransitionTextureLocked(&l.scratch->target, plume::RenderTextureLayout::COPY_SOURCE);
    const plume::RenderBox box(l.region.left, l.region.top, l.region.right, l.region.bottom);
    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(t.texture.get(), i + 1, 0),
        plume::RenderTextureCopyLocation::Subresource(l.scratch->target.texture.get()),
        uint32_t(l.region.left), uint32_t(l.region.top), 0, &box);
  }
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
}

namespace {

// The host texture's levels in bytes, from its host format.
uint64_t HostTextureBytes(const HostTexture &t) {
  if (!t.texture)
    return 0;
  const uint32_t block = std::max(plume::RenderFormatBlockWidth(t.host_format), 1u);
  const uint64_t block_bytes = plume::RenderFormatSize(t.host_format);
  const uint32_t layers = t.stacked ? StackedLayers(t) : t.depth;
  uint64_t bytes = 0;
  for (uint32_t level = 0; level < t.mip_levels; ++level) {
    const uint64_t w = std::max(1u, t.alloc_width >> level);
    const uint64_t h = std::max(1u, t.alloc_height >> level);
    const uint64_t d = t.dimension == TextureDimension::Tex3D && !t.stacked
                           ? std::max(1u, t.depth >> level)
                           : layers;
    bytes += (w + block - 1) / block * ((h + block - 1) / block) * block_bytes * d;
  }
  if (t.words)
    bytes += HostTextureBytes(*t.words);
  return bytes;
}

} // namespace

TextureMemory TextureMemoryLocked() {
  TextureMemory usage;
  for (const auto &[va, t] : g_textures) {
    if (!t || !t->texture)
      continue;
    const uint64_t bytes = HostTextureBytes(*t);
    ++usage.textures;
    usage.bytes += bytes;
    if (t->render_target) {
      ++usage.resolve_targets;
      usage.resolve_bytes += bytes;
    }
  }
  return usage;
}

namespace {
uint32_t SamplerSlotUncached(const TextureFetch &fetch, RenderScale lod_scale);
} // namespace

uint32_t SamplerSlotLocked(const TextureFetch &fetch, RenderScale lod_scale) {
  // Every draw asks for each of its textures: remember the answer by the
  // fetch constant words the sampler state comes from (clamps in 0, filters,
  // anisotropy and LOD bias in 3, mip range in 4, border color in 5).
  struct Memo {
    bool valid = false;
    u32 words[4] = {};
    RenderScale lod_scale;
    uint32_t min_anisotropy = 1;
    // sasrt-nx (2026-11-11, port 75; its formats batch, row 38: renut_float32_point_filter): the format is not in the
    // words
    bool f32_point = false;
    uint32_t slot = 0;
  };
  static Memo memo[256];
  const uint32_t min_anisotropy = MinAnisotropy();
  const u32 words[4] = {fetch.dword[0], fetch.dword[3], fetch.dword[4], fetch.dword[5]};
  // sasrt-nx (2026-11-11, port 75; its formats batch, row 38)
  const bool f32_point = banjo::renut_gpu::Float32PointFilterOn() && IsFloat32TextureFormat(fetch.Format());
  Memo &entry = memo[(words[0] * 0x9E3779B1u ^ words[1] * 0x85EBCA6Bu ^ words[2] * 0xC2B2AE35u ^
                      words[3] ^ lod_scale.code * 0x27D4EB2Fu ^ (f32_point ? 0x5BD1E995u : 0u)) >> 24];
  if (entry.valid && entry.lod_scale == lod_scale && entry.min_anisotropy == min_anisotropy &&
      entry.f32_point == f32_point && std::memcmp(entry.words, words, sizeof(words)) == 0)
    return entry.slot;
  const uint32_t slot = SamplerSlotUncached(fetch, lod_scale);
  entry.valid = true;
  std::memcpy(entry.words, words, sizeof(words));
  entry.lod_scale = lod_scale;
  entry.min_anisotropy = min_anisotropy;
  entry.f32_point = f32_point;  // sasrt-nx (2026-11-11, port 75; its formats batch, row 38)
  entry.slot = slot;
  return slot;
}

namespace {

// The sampler slot for the fetch constant's sampler state, created the first
// time it is asked for.
uint32_t SamplerSlotUncached(const TextureFetch &fetch, RenderScale lod_scale) {
  auto address = [](TextureAddress mode) {
    switch (mode) {
    case TextureAddress::Wrap:
      return plume::RenderTextureAddressMode::WRAP;
    case TextureAddress::Mirror:
      return plume::RenderTextureAddressMode::MIRROR;
    case TextureAddress::Border:
    case TextureAddress::MirrorOnceBorder:
      return plume::RenderTextureAddressMode::BORDER;
    case TextureAddress::MirrorOnce:
    case TextureAddress::MirrorOnceBorderHalf:
      return plume::RenderTextureAddressMode::MIRROR_ONCE;
    default:
      return plume::RenderTextureAddressMode::CLAMP;
    }
  };
  auto filter = [](TextureFilter f) {
    return f == TextureFilter::Point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  };

  plume::RenderSamplerDesc desc;
  desc.addressU = address(fetch.Address(0));
  desc.addressV = address(fetch.Address(1));
  desc.addressW = address(fetch.Address(2));
  desc.magFilter = filter(fetch.MagFilter());
  desc.minFilter = filter(fetch.MinFilter());
  desc.mipmapMode = fetch.MipFilter() == TextureFilter::Point ? plume::RenderMipmapMode::NEAREST
                                                              : plume::RenderMipmapMode::LINEAR;
  // MaxAnisotropy stores 2..5 for ratios 2:1..16:1 (its table at 0x82003CA8).
  const uint32_t aniso = fetch.Anisotropy();
  desc.anisotropyEnabled = aniso > 1 && aniso <= 5;
  desc.maxAnisotropy = desc.anisotropyEnabled ? (1u << (aniso - 1)) : 1;
  // renut_anisotropic_filtering: the least a mipmapped, linearly filtered
  // texture gets (render targets' host levels keep their own filtering).
  const uint32_t min_anisotropy = MinAnisotropy();
  if (min_anisotropy > desc.maxAnisotropy && lod_scale.code == 0 &&
      desc.minFilter == plume::RenderFilter::LINEAR &&
      desc.magFilter == plume::RenderFilter::LINEAR &&
      fetch.MipFilter() != TextureFilter::BaseMap && fetch.MaxMipLevel() > fetch.MinMipLevel()) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = min_anisotropy;
  }
  // banjo-nx (2026-10-28, GPU block, banjo_renut_anisotropy_cap: not pixel-identical, off by default): at most the cap.
  // The samplers made, by the game's ratio, logged now and then.
  {
    static uint64_t made[6] = {};  // by the fetch constant's field: 0-1 none, 2-5 2:1-16:1
    ++made[std::min<uint32_t>(aniso, 5u)];
    const uint64_t total = made[0] + made[1] + made[2] + made[3] + made[4] + made[5];
    if ((total & (total - 1)) == 0 && total >= 16)
      RGPU_INFO("[renut] samplers made: {} without anisotropy, {} at 2:1, {} at 4:1, {} at 8:1, {} at 16:1 (the game's "
                "own; banjo_renut_anisotropy_cap {})",
                made[0] + made[1], made[2], made[3], made[4], made[5], AnisotropyCap());
    const uint32_t cap = AnisotropyCap();
    if (cap && desc.anisotropyEnabled && desc.maxAnisotropy > cap) {
      desc.maxAnisotropy = cap;
      desc.anisotropyEnabled = cap > 1;
    }
  }
  // sasrt-nx (2026-11-11, port 75; its formats batch, row 38: renut_float32_point_filter): the Xenos cannot filter
  // 32-bit float textures and point-samples them whatever the sampler asks; the host filtered them when asked.
  if (banjo::renut_gpu::Float32PointFilterOn() && IsFloat32TextureFormat(fetch.Format())) {
    const bool asked = desc.minFilter == plume::RenderFilter::LINEAR || desc.magFilter == plume::RenderFilter::LINEAR ||
                       desc.mipmapMode == plume::RenderMipmapMode::LINEAR || desc.anisotropyEnabled;
    if (asked && banjo::renut_gpu::DiagFormatsOn()) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_INFO("[formats] a 32-bit float texture (guest format {}) asked for filtering: point-sampled "
                  "(renut_float32_point_filter)", u32(fetch.Format()));
    }
    desc.minFilter = plume::RenderFilter::NEAREST;
    desc.magFilter = plume::RenderFilter::NEAREST;
    desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
    desc.anisotropyEnabled = false;
    desc.maxAnisotropy = 1;
  }
  desc.minLOD = float(fetch.MinMipLevel());
  desc.maxLOD = fetch.MipFilter() == TextureFilter::BaseMap ? float(fetch.MinMipLevel())
                                                            : float(fetch.MaxMipLevel());
  desc.mipLODBias = float(fetch.LodBias()) / 32.0f;  // MipMapLodBias stores bias * 32
  // A resolve destination with host levels (HostTexture::extra_levels) has
  // the guest's one level: a filtered fetch reaches the host level whose
  // texel is the guest's, blending between them; a point fetch stays at the
  // guest texel it hits.
  if (lod_scale.Factor() > 1.0f) {
    desc.minLOD = 0.0f;
    if (desc.minFilter == plume::RenderFilter::LINEAR) {
      desc.maxLOD = std::log2(lod_scale.Factor());
      desc.mipmapMode = plume::RenderMipmapMode::LINEAR;
    } else {
      desc.maxLOD = 0.0f;
    }
  }
  // BorderColor stores 0 for a zero colour and 1 (white) for any other.
  switch (fetch.BorderColor()) {
  case 1:
    desc.borderColor = plume::RenderBorderColor::OPAQUE_WHITE;
    break;
  case 0:
    desc.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
    break;
  default:
    desc.borderColor = plume::RenderBorderColor::OPAQUE_BLACK;
    break;
  }

  uint64_t key = uint64_t(desc.addressU) | uint64_t(desc.addressV) << 4 |
                 uint64_t(desc.addressW) << 8 | uint64_t(desc.magFilter) << 12 |
                 uint64_t(desc.minFilter) << 14 | uint64_t(desc.mipmapMode) << 16 |
                 uint64_t(desc.maxAnisotropy) << 18 | uint64_t(fetch.MinMipLevel()) << 24 |
                 uint64_t(fetch.MaxMipLevel()) << 28 | uint64_t(fetch.MipFilter()) << 32 |
                 uint64_t(uint32_t(fetch.LodBias()) & 0x3FF) << 34 | uint64_t(desc.borderColor) << 44 |
                 uint64_t(lod_scale.code) << 48;
  auto it = g_samplers.find(key);
  if (it != g_samplers.end())
    return it->second;

  auto &s = State();
  // vivapinata-nx (2026-11-03): a family of kSamplerVariants consecutive slots: the fetch constant's sampler, then the same with
  // the filters a texture fetch can name over it (XenosRecomp adds the number to the sampler word): 1 linear, 2 point, 3 point
  // with point mips, 4 linear with point mips. Nothing on the draw path changes: the word stays the fetch constant's.
  uint32_t slot = UINT32_MAX;
  for (uint32_t i = kFirstDynamicSamplerSlot; i + kSamplerVariants <= kSamplerDescriptorCount; i += kSamplerVariants) {
    if (!s.sampler_slot_used[i]) {
      slot = i;
      break;
    }
  }
  if (slot == UINT32_MAX) {
    static bool logged = false;
    if (!logged) {
      RGPU_WARN("Sampler table full; falling back to linear clamp");
      logged = true;
    }
    return kLinearClampSamplerSlot;
  }
  auto sampler = s.device->createSampler(desc);
  if (!sampler)
    return kLinearClampSamplerSlot;
  s.sampler_slot_used[slot] = true;
  s.sampler_set->setSampler(slot, sampler.get());
  for (uint32_t v = 1; v < kSamplerVariants; ++v) {
    plume::RenderSamplerDesc d = desc;
    const bool linear = v == 1 || v == 4;
    d.magFilter = d.minFilter = linear ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
    if (v >= 3)
      d.mipmapMode = plume::RenderMipmapMode::NEAREST;
    if (!linear) {
      d.anisotropyEnabled = false;
      d.maxAnisotropy = 1;
      if (lod_scale.Factor() > 1.0f)
        d.maxLOD = 0.0f;  // as above: a point fetch stays at the guest texel it hits
    }
    auto variant = s.device->createSampler(d);
    s.sampler_set->setSampler(slot + v, variant ? variant.get() : sampler.get());
    if (variant)
      g_sampler_objects.push_back(std::move(variant));
  }
  g_sampler_objects.push_back(std::move(sampler));
  g_samplers.emplace(key, slot);
  return slot;
}

} // namespace

} // namespace renut::gpu
