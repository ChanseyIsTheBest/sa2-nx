/**
 * @file    gpu/backend/memory_report.cpp
 */
#include "renut_engine/gpu/backend/memory_report.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif
#if defined(__SWITCH__)
#include <malloc.h>
#include <switch.h>
#include <unistd.h>  // banjo-nx (2026-10-28): sbrk (the heap's room, O(1))
extern "C" size_t __nx_heap_size;  // libnx; the SDK's runtime_switch.cpp sets it
extern "C" char *fake_heap_start, *fake_heap_end;  // libnx: the malloc heap's bounds
#endif

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/render/shader_library.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/backend/video.h"
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // banjo-nx (2026-10-28, h10): the GPU region's figures
#endif
#include <rex/filesystem/devices/host_path_file.h>  // sasrt-nx (2026-11-09 night; port 58): the range cache's bytes
#if defined(__SWITCH__)
#include <threading_switch.h>  // sasrt-nx (2026-11-09 night; port 58): RexSwitchStackFigures
#include <guest_memory_switch.h>  // sasrt-nx (2026-11-09 night; port 58): RexGmDecommitFigures
#endif
REXCVAR_DECLARE(int32_t, switch_guest_stack_mb);  // sasrt-nx (2026-11-09 night; port 58): sdk system/xthread.cpp

REXCVAR_DEFINE_DOUBLE(renut_memory_report, 30.0, "Nuts&Bolts/Debug",
                      "Seconds between memory reports in the log (process, GPU and the "
                      "renderer's pools); 0 turns them off.");
// banjo-nx (2026-10-28, per-draw night: the heap): see the Switch heap line below.
REXCVAR_DEFINE_INT32(banjo_renut_heap_walk_every, 10, "Nuts&Bolts/Debug",
                     "Switch: the heap's in-use figures (mallinfo, which walks every free piece of the heap with the "
                     "malloc lock held: 25-46 ms on the render thread in the d4 city) every Nth memory report (10: "
                     "every 5 minutes at the 30 s default); the reports between print the heap's room only (O(1)). "
                     "1: every report (as before); 0: never.")
    .range(0, 1000);

namespace renut::gpu {

namespace {

// Private: memory only this process has (what Task Manager's "Memory" column
// shows on Windows); resident: what of it, and of shared mappings, is in RAM.
struct ProcessMemory {
  bool valid = false;
  uint64_t private_bytes = 0;
  uint64_t resident_bytes = 0;
};

ProcessMemory QueryProcessMemory() {
  ProcessMemory memory;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                           sizeof(counters))) {
    memory.valid = true;
    memory.private_bytes = counters.PrivateUsage;
    memory.resident_bytes = counters.WorkingSetSize;
  }
#elif defined(__linux__)
  // RssAnon: resident anonymous memory, the closest to Windows' private bytes.
  if (FILE *status = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
      unsigned long long kb = 0;
      if (std::sscanf(line, "RssAnon: %llu kB", &kb) == 1)
        memory.private_bytes = kb << 10;
      else if (std::sscanf(line, "VmRSS: %llu kB", &kb) == 1)
        memory.resident_bytes = kb << 10;
    }
    std::fclose(status);
    memory.valid = memory.resident_bytes != 0;
  }
#endif
  return memory;
}

double Mb(uint64_t bytes) {
  return double(bytes) / double(1u << 20);
}

} // namespace

void MaybeReportMemory() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last = Clock::now();
  const double interval = REXCVAR_GET(renut_memory_report);
  const Clock::time_point now = Clock::now();
  if (interval <= 0.0 || std::chrono::duration<double>(now - last).count() < interval)
    return;
  last = now;

  // Outside State().mutex: the recording lock is never taken inside it.
  const CommandBufferMemory commands = CommandBufferMemoryUsage();

  auto &s = State();
  std::lock_guard lock(s.mutex);
  if (!s.host_ready)
    return;
  const ProcessMemory process = QueryProcessMemory();
  const GpuMemoryUsage gpu = QueryGpuMemory(s.device.get());
  size_t targets = 0;
  uint64_t target_bytes = 0;
  SurfaceMemoryLocked(targets, target_bytes);
  const TextureMemory textures = TextureMemoryLocked();
  const StreamCacheMemory streams = StreamCacheMemoryLocked();
  const PipelineCounts pipelines = PipelineCountsLocked();
  uint64_t uploads = 0, memexport = 0, readback = 0;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    uploads += s.uploads[i].Bytes();
    memexport += s.memexport_capacity[i] + s.memexport_scratch_capacity[i] +
                 s.exported_streams_capacity[i] + s.gpu_fetch_data_capacity[i];
    readback += s.memexport_readback_capacity[i];
  }

  RGPU_INFO("Memory: process {:.0f} MB private, {:.0f} MB resident; GPU {} (local {:.0f}/{:.0f} "
            "MB, shared {:.0f}/{:.0f} MB)",
            Mb(process.private_bytes), Mb(process.resident_bytes), gpu.valid ? "used/budget" : "n/a",
            Mb(gpu.local_used), Mb(gpu.local_budget), Mb(gpu.shared_used), Mb(gpu.shared_budget));
#if defined(__SWITCH__)
  // banjo-nx (2026-10-17): the Switch's malloc heap is fixed (__nx_heap_size, or the loader's), the guest's memory in
  // it: what the process line above cannot show there. (deko3d's memory blocks are in it too: since h10 in the GPU
  // region at its top, below.)
  // banjo-nx (2026-10-28, per-draw night): mallinfo walks every free piece (newlib's bins) with the malloc lock held, on
  // the render thread under State().mutex: 25-46 ms in the d4 city (150,000-260,000 pieces), every report. Now every
  // banjo_renut_heap_walk_every-th report (the first one included), timed; the others print the room sbrk has left.
  {
    const size_t heap = envHasHeapOverride() ? size_t(envGetHeapOverrideSize()) : __nx_heap_size;
    static uint32_t reports = 0;
    const int every = REXCVAR_GET(banjo_renut_heap_walk_every);
    if (every > 0 && reports++ % uint32_t(every) == 0) {
      const auto walk_start = std::chrono::steady_clock::now();
      const struct mallinfo mi = mallinfo();
      const double walk_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - walk_start).count();
      RGPU_INFO("Memory: heap {:.0f} MB in use of {:.0f} MB ({:.0f} MB taken from it so far, {:.0f} MB of that free in "
                "{} pieces; the walk took {:.1f} ms, every {} reports, banjo_renut_heap_walk_every)",
                Mb(mi.uordblks), Mb(heap), Mb(mi.arena), Mb(mi.fordblks), size_t(mi.ordblks), walk_ms, every);
    } else {
      const char *top = static_cast<const char *>(sbrk(0));
      const uint64_t room = (top && fake_heap_end > top) ? uint64_t(fake_heap_end - top) : 0;
      const uint64_t taken = (top && top > fake_heap_start) ? uint64_t(top - fake_heap_start) : 0;
      RGPU_INFO("Memory: heap {:.0f} MB taken from it so far of {:.0f} MB, {:.0f} MB never handed out (the in-use "
                "figures every {} reports, banjo_renut_heap_walk_every)",
                Mb(taken), Mb(heap), Mb(room), every);
    }
  }
#if defined(PLUME_DEKO3D)
  // banjo-nx (2026-10-28, h10): the GPU region at the heap's top (banjo_deko3d_gpu_region); the heap's room above is
  // newlib's, below the region.
  {
    plume::Deko3DGpuRegionInfo r;
    plume::Deko3DGpuRegionFigures(r);
    if (r.on)
      RGPU_INFO("Memory: GPU region {:.0f} MB at the heap's top: {} blocks {:.0f} MB (peak {:.0f}), {:.0f} MB free in {} "
                "ranges (largest {:.0f} MB); grown {} times, {} times given back ({:.0f} MB), {} blocks not placed in it",
                Mb(r.size), r.blocks, Mb(r.used), Mb(r.peak), Mb(r.freeBytes), r.freeRanges, Mb(r.largestFree),
                r.growths, r.givebacks, Mb(r.givenBack), r.refused);
    // sasrt-nx (2026-11-11, sasrt_target_block_cache_mb)
    plume::Deko3DTargetCacheInfo c;
    plume::Deko3DTargetCacheFigures(c);
    RGPU_INFO("Memory: kept target blocks {} ({:.0f} MB); so far {} targets took one, {} made a new one, {} kept, {} "
              "dropped over the cap, {} given back (sasrt_target_block_cache_mb)",
              c.blocks, Mb(c.bytes), c.hits, c.misses, c.kept, c.evicted, c.released);
  }
#endif
#endif
  // sasrt-nx (2026-11-09 night; port 58): the CPU side's known holders (the command buffers are in the GPU line).
  {
    size_t live = 0, kept = 0, kept_bytes = 0;
#if defined(__SWITCH__)
    RexSwitchStackFigures(&live, &kept, &kept_bytes);
#endif
    const rex::filesystem::StatsRanges ranges = rex::filesystem::ReadStatsRanges();
    const uint64_t stack_mb = uint64_t(REXCVAR_GET(switch_guest_stack_mb));
    RGPU_INFO("Memory: CPU: guest thread stacks {} in use x {} MB ({} MB) + {} kept ({:.0f} MB); I/O range cache {:.0f} "
              "MB", live, stack_mb, uint64_t(live) * stack_mb, kept, Mb(kept_bytes), Mb(ranges.bytes_live));
#if defined(__SWITCH__)
    size_t committed = 0, chunks = 0, asked = 0, freed = 0;
    RexGmDecommitFigures(&committed, &chunks, &asked, &freed);
    RGPU_INFO("Memory: guest: {:.0f} MB committed in {} chunks; decommits asked for {:.0f} MB and freed {:.0f} MB (a "
              "chunk goes only when a decommit covers all of it)", Mb(committed), chunks, Mb(asked), Mb(freed));
#endif
  }
  RGPU_INFO("Memory: GPU: {} render targets {:.0f} MB (x{:.3g} scale), {} textures {:.0f} MB (of "
            "which {} resolve targets {:.0f} MB), memexport {:.0f} MB",
            targets, Mb(target_bytes), ResolutionScale().Factor(), textures.textures, Mb(textures.bytes),
            textures.resolve_targets, Mb(textures.resolve_bytes), Mb(memexport));
  RGPU_INFO("Memory: host: upload rings {:.0f} MB, stream cache {:.0f} MB in a {:.0f} MB pool "
            "({} streams, {} index ranges), memexport readback {:.0f} MB, command buffers "
            "{:.0f} MB in {} ({} live) + {:.0f} MB in {} spare, shader cache {:.0f} MB, "
            "pipelines {} built + {} prebuilt unused (of {} listed, {} waiting for their shaders; {} given back "
            "unused so far, banjo_renut_prebuild_release_s)",
            Mb(uploads), Mb(streams.cached_bytes), Mb(streams.pool_bytes), streams.streams,
            streams.index_ranges, Mb(readback), Mb(commands.recorded_bytes), commands.recorded,
            commands.live, Mb(commands.spare_bytes), commands.spare, Mb(ShaderCacheHostBytes()),
            pipelines.built, pipelines.prebuilt_unused, pipelines.listed, pipelines.waiting, pipelines.released);
}

} // namespace renut::gpu
