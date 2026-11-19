/**
 * @file    gpu/stage_census.h
 * @brief   blur-nx (2026-11-17, the Renut audit's #17-#22 and #30): time spent in the renderer's stages, for the 10 s
 *          report, with blur_renut_diag_stage_census = true (off by default: each stage then costs two clock reads
 *          and two atomics). A stage's time includes the stages it calls (FastDrawLocked holds FillTextures, ...).
 *
 * The Switch's counter (cntpct_el0, 19.2 MHz) is coarse next to a stage of a microsecond, but each stage's time is the
 * sum of many such reads at unrelated phases, so its total is right on average.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace renut::gpu::census {

enum Stage : uint32_t {
  kFastDraw,       // FastDrawLocked (render thread, a replayed draw on the fast path)
  kDrawState,      // DrawStateLocked (render thread, every draw)
  kFillTextures,   // FillTextures
  kPushConstants,  // PushFloatConstants + PushSharedConstants
  kBindTargets,    // BindTargets
  kSetPipeline,    // the pipeline set on the command list
  kBindStreams,    // BindStreams
  kWholeGeometry,  // WholeGeometryTry
  kHostDraw,       // the command list's draw call
  kCardsProve,     // CardsProve
  kUpload,         // textures.cpp Upload
  kRefreshBinding, // RefreshBindingLocked
  kResolveSetup,   // ResolveTargetsLocked
  kRunItem,        // a render-thread item (render_thread.cpp Execute)
  kNewOp,          // command_buffers.cpp NewOp (the job threads)
  kBeginCbFence,   // D3DDevice_BeginCommandBuffer's BlockOnFence (the job threads)
  kStageCount
};

bool On();  // blur_renut_diag_stage_census (banjo_renut_tuning.cpp), read once

struct Acc {
  std::atomic<uint64_t> ticks{0};
  std::atomic<uint64_t> n{0};
};
extern Acc g_acc[kStageCount];

inline uint64_t Now() {
#if defined(__SWITCH__)
  return armGetSystemTick();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

class Scope {
 public:
  explicit Scope(Stage s) : stage_(s), on_(On()) {
    if (on_)
      start_ = Now();
  }
  ~Scope() {
    if (on_) {
      g_acc[stage_].ticks.fetch_add(Now() - start_, std::memory_order_relaxed);
      g_acc[stage_].n.fetch_add(1, std::memory_order_relaxed);
    }
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

 private:
  Stage stage_;
  bool on_;
  uint64_t start_ = 0;
};

// The 10 s line ("stages: ..."), empty with the key off; resets the sums.
std::string Report(double seconds);

}  // namespace renut::gpu::census
