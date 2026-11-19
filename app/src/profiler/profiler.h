// banjo-nx profiler: the CPU/GPU profiler of UnleashedRecomp-NX, ported to the ReXGlue runtime.
//
// Three parts, as in Unleashed:
//   1. On-screen profiler (Unleashed's F1 "Profiler" window): frame-time graphs and current/average
//      values for the game's frame, its update and draw halves, the frame limiter, the GPU frame and the
//      host present. F5 on PC; cvar banjo_show_profiler (also the only way on the Switch).
//   2. Switch CPU profiler (profiler/cpu_profiler_switch.cpp, Unleashed's os/switch/cpu_profiler_switch.cpp):
//      per-thread CPU use, and with banjo_cpu_profiler = true a 500 Hz sampler whose reports
//      tools/switch-cpu-profile.py names. Plus the stall watchdog (profiler/stall_watch_switch.cpp).
//   3. GPU pass/draw profiler: timestamps around each render pass and draw of the native renderer.
//      The renderer reports into SetGpuFrameMs / the pass API below.
//
// Every entry point is cheap when its feature is off: a relaxed load or two.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace banjo::profiler {

// Once, after the runtime is set up (RenutApp::OnPostSetup). Starts the Switch CPU sampler and the stall
// watchdog when their cvars ask for them.
void Start();

// The game loop, from the appMainTickPreDraw / appMainDraw hooks (renut_engine/FPS.cpp).
void RecordTick(double ms);         // the update half (Unleashed: "Update Director")
void RecordDraw(double ms);         // the draw-submission half (Unleashed: "Render Director")
void RecordLimiterWait(double ms);  // time slept by the frame cap (Unleashed: "Present Wait")
// End of one game frame, on the game's main thread: the frame time, and the frame markers of the
// Switch CPU profiler (slow-frame reports) and stall watchdog.
void FrameBoundary();
// blur-nx (2026-11-18): the same at the game's Swap (renut::frame_stats::OnGameFrame), unless a Present hook gave a
// boundary in the last second: Blur swaps without Present, so FrameBoundary never ran there (the slow-frame report
// said "0 of 0 frames" and the stall watchdog saw no frame).
void FrameBoundaryAtSwap();

// The renderer: GPU time of the last finished frame, and host presents (Unleashed: "Present").
void SetGpuFrameMs(double ms);
void RecordHostPresent();
// Renut's renderer (package 27): its render thread's busy time in the last frame (recording and the present).
void SetRenderThreadMs(double ms);
void SetRendererInfo(const char* api, const char* device);

// Current values for the overlay, which keeps its own history (as Unleashed's window did).
enum class Series : uint32_t { kApplication, kTick, kDraw, kLimiterWait, kGpuFrame, kPresent, kRenderThread, kCount };
const char* SeriesName(Series s);
double Current(Series s);
bool HasValue(Series s);            // false until the series got its first value
std::string RendererApi();
std::string RendererDevice();

// Per-thread CPU use ("name 12.3%" entries) on the Switch; empty elsewhere.
std::string ThreadCpuUsage();

}  // namespace banjo::profiler
