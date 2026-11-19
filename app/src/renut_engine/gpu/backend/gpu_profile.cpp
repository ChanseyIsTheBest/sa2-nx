/**
 * @file    gpu/backend/gpu_profile.cpp
 */
#include "renut_engine/gpu/backend/gpu_profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // vivapinata-nx (2026-11-12): renut_gpu_profile_counters
#endif
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"

REXCVAR_DEFINE_DOUBLE(renut_gpu_profile, 0.0, "Nuts&Bolts/Debug",
                      "Every this many seconds, log the GPU's time per frame by kind of work "
                      "(draws per target, resolves, mip levels, present). 0 = off.");
// vivapinata-nx (2026-11-12, diagnostic, deko3d): beside each mark's timestamp the GPU's count of pixel shader
// invocations and its Zcull statistics, so each line also says how many pixels it shaded and what Zcull rejected.
REXCVAR_DEFINE_BOOL(renut_gpu_profile_counters, false, "Nuts&Bolts/Debug",
                    "With renut_gpu_profile on deko3d: each line also gives the pixel shader invocations a frame (M) and "
                    "the change of the GPU's four Zcull statistics words a frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (package 28b): the passes split by pixel shader (render/draw.cpp, ShaderProfileLabel), more lines.
REXCVAR_DECLARE(bool, banjo_renut_diag_gpu_shaders);

namespace renut::gpu::gpu_profile {

bool g_recording_timed = false;

namespace {

// Timestamps one frame may write; past them its work stays with the last
// kind marked.
constexpr uint32_t kMaxMarks = 2048;
constexpr uint32_t kEndOfList = 0;  // a list's last timestamp: no span follows
constexpr uint32_t kOther = 1;      // a list's start, before any kind is marked

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters): the pixel shader invocations (an entry a mark) and the
  // Zcull statistics (4 entries a mark: the report's size is not documented, so room for four).
  std::unique_ptr<plume::RenderQueryPool> frag_pool, zcull_pool;
  bool open = false;    // the slot's frame has begun recording
  bool active = false;  // ... and is profiled
  uint32_t count = 0;
  uint32_t list = 0;
  uint32_t current = kEndOfList;
  std::array<uint32_t, kMaxMarks> labels{};
  std::array<uint32_t, kMaxMarks> lists{};
};
std::array<Slot, kFramesInFlight> g_slots;

std::vector<std::string> g_names = {"(list end)", "other"};
std::unordered_map<std::string, uint32_t> g_ids = {{"(list end)", kEndOfList}, {"other", kOther}};

// Since the last report: GPU nanoseconds per kind, and frames.
std::vector<uint64_t> g_totals;
std::vector<uint64_t> g_draws;  // banjo-nx (package 28b): draws per kind (CountDraw)
std::vector<uint64_t> g_frags;  // vivapinata-nx (2026-11-12): pixel shader invocations per kind
std::vector<std::array<uint64_t, 8>> g_zcull;  // vivapinata-nx (2026-11-12): Zcull statistics words per kind
bool CountersOn() {
#if defined(PLUME_DEKO3D)
  static const bool on = REXCVAR_GET(renut_gpu_profile_counters) && IsDeko3D();
  return on;
#else
  return false;
#endif
}
uint64_t g_frames = 0;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();

void Write(VideoState &s, Slot &slot, uint32_t label) {
  s.command_list->writeTimestamp(slot.pool.get(), slot.count);
#if defined(PLUME_DEKO3D)
  if (slot.frag_pool) {  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters)
    plume::Deko3DReportCounter(s.command_list, slot.frag_pool.get(), slot.count, 10);  // FragmentShaderInvocations
    plume::Deko3DReportCounter(s.command_list, slot.zcull_pool.get(), slot.count * 4, 3);  // ZcullStats
  }
#endif
  slot.labels[slot.count] = label;
  slot.lists[slot.count] = slot.list;
  ++slot.count;
  slot.current = label;
}

void Report(double interval) {
  const auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration<double>(now - g_last_report).count() < interval || !g_frames)
    return;
  g_last_report = now;
  std::vector<std::pair<uint64_t, uint32_t>> kinds;
  uint64_t total = 0;
  for (uint32_t i = 0; i < g_totals.size(); ++i) {
    if (g_totals[i]) {
      kinds.emplace_back(g_totals[i], i);
      total += g_totals[i];
    }
  }
  std::sort(kinds.rbegin(), kinds.rend());
  const double per_frame = 1e6 * double(g_frames);
  std::string lines;
  const size_t shown = REXCVAR_GET(banjo_renut_diag_gpu_shaders) ? 60 : 24;
  for (size_t i = 0; i < kinds.size() && i < shown; ++i) {
    const uint32_t k = kinds[i].second;
    const uint64_t draws = k < g_draws.size() ? g_draws[k] : 0;
    lines += fmt::format("\n  {:7.3f} ms  {}", double(kinds[i].first) / per_frame, g_names[k]);
    if (draws)
      lines += fmt::format("  ({:.1f} draws a frame)", double(draws) / double(g_frames));
    if (CountersOn() && k < g_frags.size()) {  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters)
      const auto &z = g_zcull[k];
      lines += fmt::format("  [{:.3f} M pixels shaded; zcull {} {} {} {} {} {} {} {}]", double(g_frags[k]) / 1e6 / double(g_frames),
                           z[0] / g_frames, z[1] / g_frames, z[2] / g_frames, z[3] / g_frames, z[4] / g_frames,
                           z[5] / g_frames, z[6] / g_frames, z[7] / g_frames);
    }
  }
  RGPU_INFO("GPU profile: {:.3f} ms per frame over {} frames{}", double(total) / per_frame,
            g_frames, lines);
  std::fill(g_totals.begin(), g_totals.end(), 0);
  std::fill(g_draws.begin(), g_draws.end(), 0);
  std::fill(g_frags.begin(), g_frags.end(), 0);
  std::fill(g_zcull.begin(), g_zcull.end(), std::array<uint64_t, 8>{});
  g_frames = 0;
}

} // namespace

uint32_t Label(std::string_view name) {
  auto it = g_ids.find(std::string(name));
  if (it != g_ids.end())
    return it->second;
  const uint32_t id = uint32_t(g_names.size());
  g_names.emplace_back(name);
  g_ids.emplace(std::string(name), id);
  return id;
}

std::string_view Name(uint32_t label) {
  return label < g_names.size() ? std::string_view(g_names[label]) : std::string_view("?");
}

void CountDraw(VideoState &s, uint32_t label) {
  // Counted when recorded; reported with the frames the GPU profile collected (both every frame).
  if (!g_slots[s.frame].active)
    return;
  if (g_draws.size() <= label)
    g_draws.resize(label + 1);
  ++g_draws[label];
}

void Mark(VideoState &s, uint32_t label) {
  Slot &slot = g_slots[s.frame];
  if (!slot.active || !s.command_list || label == slot.current || slot.count + 2 >= kMaxMarks)
    return;
  Write(s, slot, label);
}

void BeginList(VideoState &s) {
  Slot &slot = g_slots[s.frame];
  if (!slot.open) {
    slot.open = true;
    slot.count = 0;
    slot.list = 0;
    slot.active = REXCVAR_GET(renut_gpu_profile) > 0.0 && s.device->getCapabilities().queryPools;
    // banjo-nx (2026-10-19, GPU): with the per-shader breakdown (a timestamp at nearly every draw), one frame in
    // kShaderSample is timed; the report's averages are over the timed frames (draws are counted in them only).
    static uint32_t frames_seen = 0;
    // banjo-nx (2026-10-28, per-draw night): 8 (was 4): the timed frames' marks and labels cost the render thread.
    constexpr uint32_t kShaderSample = 8;
    if (slot.active && REXCVAR_GET(banjo_renut_diag_gpu_shaders) && (frames_seen++ % kShaderSample) != 0)
      slot.active = false;
    if (slot.active && !slot.pool) {
      slot.pool = s.device->createQueryPool(kMaxMarks);
      slot.active = slot.pool != nullptr;
      if (slot.active && CountersOn()) {  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters)
        slot.frag_pool = s.device->createQueryPool(kMaxMarks);
        slot.zcull_pool = s.device->createQueryPool(kMaxMarks * 4);
        if (!slot.frag_pool || !slot.zcull_pool)
          slot.frag_pool.reset(), slot.zcull_pool.reset();
      }
    }
    // Vulkan queries are reset before they are written again.
    if (slot.active)
      s.command_list->resetQueryPool(slot.pool.get(), 0, kMaxMarks);
  }
  g_recording_timed = slot.active;  // banjo-nx (2026-10-19, render-thread proposal 7)
  if (slot.active && slot.count + 2 < kMaxMarks)
    Write(s, slot, kOther);
}

void EndList(VideoState &s, bool last_in_frame) {
  Slot &slot = g_slots[s.frame];
  if (!slot.active || !s.command_list)
    return;
  if (slot.count < kMaxMarks)
    Write(s, slot, kEndOfList);
  ++slot.list;
  // Vulkan returns no results while any query of the pool is unwritten.
  // banjo-nx (package 28b): unless the pool reads only the written ones (plume's queryResults(count)).
  if (last_in_frame && IsVulkan() && !slot.pool->partialResults()) {
    for (uint32_t i = slot.count; i < kMaxMarks; ++i)
      s.command_list->writeTimestamp(slot.pool.get(), i);
  }
}

void Collect(VideoState &s, uint32_t index) {
  (void)s;
  Slot &slot = g_slots[index];
  const bool active = slot.active && slot.count > 1;
  slot.open = false;
  slot.active = false;
  if (!active)
    return;
  slot.pool->queryResults(slot.count);  // banjo-nx: the written ones only, where the backend can
  const uint64_t *ns = slot.pool->getResults();
  if (g_totals.size() < g_names.size())
    g_totals.resize(g_names.size());
  for (uint32_t i = 0; i + 1 < slot.count; ++i) {
    if (slot.labels[i] == kEndOfList || slot.lists[i] != slot.lists[i + 1] || ns[i + 1] <= ns[i])
      continue;
    g_totals[slot.labels[i]] += ns[i + 1] - ns[i];
  }
#if defined(PLUME_DEKO3D)
  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters): each span's change of the counters, to its label.
  if (slot.frag_pool) {
    const volatile uint64_t *f = plume::Deko3DQueryPoolWords(slot.frag_pool.get());
    const volatile uint64_t *z = plume::Deko3DQueryPoolWords(slot.zcull_pool.get());
    if (f && z) {
      if (g_frags.size() < g_names.size()) {
        g_frags.resize(g_names.size());
        g_zcull.resize(g_names.size());
      }
      for (uint32_t i = 0; i + 1 < slot.count; ++i) {
        if (slot.labels[i] == kEndOfList || slot.lists[i] != slot.lists[i + 1] || ns[i + 1] <= ns[i])
          continue;
        const uint32_t k = slot.labels[i];
        const uint64_t a = f[i * 2], b = f[(i + 1) * 2];
        if (b >= a)
          g_frags[k] += b - a;
        // the four 16-byte entries of a mark's Zcull report, read as eight 32-bit words
        for (int w = 0; w < 8; ++w) {
          const uint64_t wa = z[i * 8 + w / 2] >> (32 * (w & 1)) & 0xFFFFFFFFull;
          const uint64_t wb = z[(i + 1) * 8 + w / 2] >> (32 * (w & 1)) & 0xFFFFFFFFull;
          if (wb >= wa)
            g_zcull[k][w] += wb - wa;
        }
      }
    }
  }
#endif
  ++g_frames;
  Report(REXCVAR_GET(renut_gpu_profile));
}

} // namespace renut::gpu::gpu_profile
