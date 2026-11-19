/**
 * @file    gpu/render/upload_ring.h
 * @brief   Per-frame linear allocator over host-visible UPLOAD memory.
 *
 * One ring per frame slot. Allocations live until that slot's fence is waited
 * at reuse, when Reset() rewinds it. Overflow grows into extra chunks that are
 * kept (and reused) rather than failing the frame, until no frame has needed
 * them for a while: a load that uploads hundreds of megabytes of textures in
 * one frame would otherwise keep that much host memory for good.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

namespace renut::gpu {

// banjo-nx (2026-10-17): on Vulkan the guest shaders read their constants from dynamic uniform buffers (sets 5-7 of
// the draw layout, render/pipeline.cpp), which NVK keeps in Maxwell's constant banks, instead of through 64-bit
// pointers (global memory loads on NAK). Each chunk has two descriptor sets over its buffer, bound at the
// allocation's offset: the float constants' (all 256 registers) and SharedConstants'.
// banjo-nx (2026-10-18): 256 registers and a zero register after them (tools/deko3d_glsl_opt.py, pass bonerow: a
// relative read clamped to 256 reads zeros instead of selecting them).
constexpr uint32_t kFloatConstantsUboBytes = 256 * 16 + 16;
constexpr uint32_t kSharedConstantsUboBytes = 1552;  // sizeof(SharedConstants), render/draw.cpp (2026-10-19: + pixel origin; halo3-nx: + loop constants)

struct UploadAllocation {
  plume::RenderBufferReference ref;  // buffer + offset, for copies / views
  void *cpu = nullptr;               // mapped write pointer
  uint64_t gpu_address = 0;          // device address (Vulkan raw loads)
  uint32_t size = 0;
  uint64_t chunk_size = 0;  // banjo-nx: the size of the chunk the allocation is in (the bound of a clamped fetch)
  // Vulkan (UploadRing::Init with constant_sets): the chunk's uniform buffer descriptor sets.
  plume::RenderDescriptorSet *float_constants_set = nullptr;
  plume::RenderDescriptorSet *shared_constants_set = nullptr;
  explicit operator bool() const { return cpu != nullptr; }
};

class UploadRing {
public:
  // banjo-nx (2026-10-28, the heap): 16 MB (32 before). The first chunk of each ring stays for the whole session (64 MB
  // for the two rings, the heap at 2.9 of 3.08 GB in the d4 city); a city frame fills a few MB with the constants
  // pushed. A frame that needs more adds a chunk, which the trim below gives back.
  static constexpr uint64_t kChunkSize = 16ull << 20;
  // kameo-nx (2026-11-04, banjo_renut_split_frame_mb): the chunk this frame is writing, so how far it has got.
  size_t CurrentChunk() const { return current_; }
  // Resets (frames of this slot) after which chunks beyond what those frames
  // used are released.
  static constexpr uint32_t kTrimResets = 300;

  // constant_sets: each chunk gets the uniform buffer descriptor sets (Vulkan draws).
  // host_cached (banjo-nx, banjo_renut_upload_cached): the chunks in CPU-cached memory, which Publish() hands to the
  // GPU before each submission.
  bool Init(plume::RenderDevice *device, const char *name, bool constant_sets = false, bool host_cached = false);
  // banjo-nx: publishes what was written since the last call (host_cached; nothing otherwise). Before every submission
  // of the slot's command list.
  void Publish();
  // banjo-nx (2026-10-19, render-thread proposal 23): what Publish() would clean now, handed over instead (another
  // core cleans it, RingCleaner in backend/video.cpp): each chunk's [published, written), then marked published.
  struct Range {
    plume::RenderBuffer *buffer;
    uint64_t begin, end;
  };
  void HandOff(std::vector<Range> &out);
  // The slot's fence has been waited: nothing uses the ring's memory.
  void Reset();
  // banjo-nx (2026-10-23, batch 6): the Reset() calls of every ring so far: an allocation made while this is unchanged
  // still holds what was written into it (render/draw.cpp keeps fetch copies and converted streams across kicks).
  static uint64_t Resets() { return s_resets_; }
  UploadAllocation Allocate(uint64_t size, uint64_t alignment = 256);
  // banjo-nx (2026-10-19, render-thread proposal 10): 'size' bytes, with 'room' bytes (>= size) left in the chunk from
  // the allocation's start (a uniform buffer bound at a fixed size over a smaller allocation stays inside the chunk);
  // only 'size' is taken, so only that is published.
  UploadAllocation AllocateWithRoom(uint64_t size, uint64_t room, uint64_t alignment = 256);
  // The chunk the next allocation goes to, if it fits there (FetchCopy reuse).
  const plume::RenderBuffer *CurrentBuffer() const {
    return current_ < chunks_.size() ? chunks_[current_].buffer.get() : nullptr;
  }
  UploadAllocation Upload(const void *data, uint64_t size, uint64_t alignment = 256);
  uint64_t Bytes() const;

private:
  struct Chunk {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint8_t *mapped = nullptr;
    uint64_t size = 0;
    uint64_t address = 0;
    std::unique_ptr<plume::RenderDescriptorSet> float_constants_set, shared_constants_set;
    uint64_t written = 0, published = 0;  // host_cached: the allocated extent, and how much of it Publish() flushed
  };

  bool AddChunk(uint64_t min_size);

  plume::RenderDevice *device_ = nullptr;
  const char *name_ = "upload";
  bool constant_sets_ = false;
  bool host_cached_ = false;
  std::vector<Chunk> chunks_;
  size_t current_ = 0;
  uint64_t offset_ = 0;
  // The most chunks a frame used since the last trim, and the resets since.
  size_t window_chunks_ = 1;
  uint32_t window_resets_ = 0;
  static inline uint64_t s_resets_ = 1;
};

} // namespace renut::gpu
