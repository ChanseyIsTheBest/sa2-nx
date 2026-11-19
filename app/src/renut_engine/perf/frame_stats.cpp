// banjo-nx: the frame statistics Renut's renderer reports into (renut_engine/perf/frame_stats.h, copied from Renut by
// tools/renut_gpu_import.py). Renut's own frame_stats.cpp also hooks the game's main loop (appMainTickPreDraw /
// appMainDraw), which banjo-nx's FPS code hooks already, so this keeps the numbers without those hooks: a game frame is
// one D3DDevice_Swap, its length the time between two.
#include "renut_engine/perf/frame_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>  // blur-nx (2026-11-13): WaitReport

// Counter 0 of the console profiler: presented frames, which the Status Monitor overlay's FPS (SaltyNX) and the
// profiler's "game N fps" are made from (sdk/src/ui/switch_perf.cpp; a stub on the PC, me_pc_stubs.cpp).
extern "C" void RexSwitchPerfCount(unsigned id);

#include "profiler/profiler.h"                      // the on-screen profiler's GPU, present and render thread rows
#include "renut_engine/gpu/banjo_renut_tuning.h"  // the [renut] report and its in-run A/B

namespace banjo::renderdoc { void GameFrame(); }  // me/banjo_renderdoc.cpp (banjo_renderdoc_game_s)

namespace renut::frame_stats {

namespace {

using Clock = std::chrono::steady_clock;

// blur-nx (2026-11-18, the user: "help with 1% lows"; written for every port): the frame pacing of the 10 s report
// (PacingReport). A window's frame lengths in a histogram of a quarter of a millisecond a bin (to 250 ms; longer ones
// kept apart with their sum), so its percentiles and 1 % low are exact to a quarter ms, and the logged bins of several
// windows add up to a race's. Three of them: the game's frames (Swap to Swap), the render thread's busy time and the
// GPU's time of each frame.
struct Hist {
  static constexpr int kPerMs = 4, kBins = 250 * kPerMs;
  uint16_t bins[kBins] = {};
  uint32_t n = 0, over = 0;
  double sum = 0.0, over_sum = 0.0, worst = 0.0;

  void Add(double ms) {
    ms = std::max(ms, 0.0);
    ++n;
    sum += ms;
    worst = std::max(worst, ms);
    const int bin = int(ms * kPerMs);
    if (bin >= kBins) {
      ++over;
      over_sum += ms;
    } else if (bins[bin] != UINT16_MAX) {
      ++bins[bin];
    }
  }
  // The length that a fraction p of the frames are no longer than (a bin's middle).
  double Percentile(double p) const {
    if (!n)
      return 0.0;
    const uint64_t rank = std::max<uint64_t>(1, uint64_t(p * double(n) + 0.999999));
    uint64_t seen = 0;
    for (int i = 0; i < kBins; ++i) {
      seen += bins[i];
      if (seen >= rank)
        return (i + 0.5) / kPerMs;
    }
    return worst;
  }
  // The mean length of the slowest fraction of the frames (one frame at least), from the top: the "1 % low" is its fps.
  double SlowestMean(double fraction) const {
    if (!n)
      return 0.0;
    uint64_t want = std::max<uint64_t>(1, uint64_t(fraction * double(n)));
    double total = 0.0;
    uint64_t taken = 0;
    if (over) {
      const uint64_t k = std::min<uint64_t>(want, over);
      total += over_sum / over * double(k);
      taken += k;
    }
    for (int i = kBins - 1; i >= 0 && taken < want; --i) {
      const uint64_t k = std::min<uint64_t>(want - taken, bins[i]);
      total += (i + 0.5) / kPerMs * double(k);
      taken += k;
    }
    return taken ? total / double(taken) : 0.0;
  }
  // The non-empty bins as "bin:count" (bin = ms x 4), then "over:count:sum" for the longer frames.
  std::string Bins() const {
    std::string out;
    for (int i = 0; i < kBins; ++i)
      if (bins[i])
        out += fmt::format("{}{}:{}", out.empty() ? "" : " ", i, bins[i]);
    if (over)
      out += fmt::format("{}over:{}:{:.1f}", out.empty() ? "" : " ", over, over_sum);
    return out.empty() ? "-" : out;
  }
};

// One of a window's slowest game frames: what the game thread waited on inside it (OnGameThreadWait's sources, the
// game thread's own waits only), and the longest render thread and GPU frames reported while it ran.
struct SlowFrame {
  double ms = 0.0, at_s = 0.0, wait[4] = {}, render_ms = 0.0, gpu_ms = 0.0;
};
constexpr int kSlowKept = 5;

struct State {
  std::mutex mutex;
  Snapshot snap;
  Clock::time_point last_frame{};
  Clock::time_point window_start{};
  int window_frames = 0;
  double window_worst = 0.0;
  double gpu_sum = 0.0, render_sum = 0.0, wait_sum = 0.0;
  int gpu_n = 0, render_n = 0;
  // blur-nx (2026-11-13, #37's census): by source (OnGameThreadWait), since the last WaitReport. 2026-11-18: a fourth,
  // the frame limiter's sleep (profiler.cpp's RecordLimiterWait), which had been counted as a fence.
  double source_ms[4] = {};
  uint64_t source_n[4] = {};
  Clock::time_point report_start = Clock::now();
  // blur-nx (2026-11-18, the frame pacing): the window's histograms and slowest frames (PacingReport starts them
  // again), the running frame's waits and longest render thread and GPU frames, the game thread (the one at Swap).
  Hist game, render, gpu;
  SlowFrame slow[kSlowKept];
  int slow_n = 0;
  double frame_wait[4] = {}, frame_render = 0.0, frame_gpu = 0.0;
  double other_wait_ms = 0.0;  // waits reported by other threads (a fence a worker waited for)
  uint64_t other_wait_n = 0;
  std::thread::id game_thread{};
  Clock::time_point first_frame{}, pacing_start{};
};

State& S() {
  static State s;
  return s;
}

}  // namespace

void OnGameFrame() {
  banjo::renut_gpu::StatsGameFrame();
  banjo::profiler::FrameBoundaryAtSwap();  // blur-nx (2026-11-18): the profiler's frame marks, for a game without Present
  // culdcept-nx (2026-11-19): banjo_renderdoc_game_s's frame tick. Its only caller was N&B's appMainDraw hook
  // (renut_engine/FPS.cpp, not built since katamari-nx), so the key captured nothing; the Swap is every port's frame.
  banjo::renderdoc::GameFrame();
  State& s = S();
  std::lock_guard lock(s.mutex);
  const auto now = Clock::now();
  if (s.last_frame != Clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - s.last_frame).count();
    Snapshot& snap = s.snap;
    if (snap.history_count < Snapshot::kHistory) {
      snap.history[snap.history_count++] = float(ms);
    } else {
      std::copy(snap.history + 1, snap.history + Snapshot::kHistory, snap.history);
      snap.history[Snapshot::kHistory - 1] = float(ms);
    }
    s.window_worst = std::max(s.window_worst, ms);
    ++s.window_frames;
    // blur-nx (2026-11-18, the frame pacing): the frame to the histogram, and to the slowest kept (longest first).
    s.game.Add(ms);
    if (s.slow_n < kSlowKept || ms > s.slow[s.slow_n - 1].ms) {
      SlowFrame f;
      f.ms = ms;
      f.at_s = std::chrono::duration<double>(now - s.first_frame).count();
      std::copy(s.frame_wait, s.frame_wait + 4, f.wait);
      f.render_ms = s.frame_render;
      f.gpu_ms = s.frame_gpu;
      int i = std::min(s.slow_n, kSlowKept - 1);
      for (; i > 0 && s.slow[i - 1].ms < ms; --i)
        s.slow[i] = s.slow[i - 1];
      s.slow[i] = f;
      s.slow_n = std::min(s.slow_n + 1, kSlowKept);
    }
  } else {
    s.window_start = now;
    s.first_frame = s.pacing_start = now;
  }
  s.last_frame = now;
  s.game_thread = std::this_thread::get_id();
  std::fill(s.frame_wait, s.frame_wait + 4, 0.0);
  s.frame_render = s.frame_gpu = 0.0;
  const double window = std::chrono::duration<double>(now - s.window_start).count();
  if (window >= 1.0 && s.window_frames) {
    Snapshot& snap = s.snap;
    snap.fps = s.window_frames / window;
    snap.frame_ms = window * 1000.0 / s.window_frames;
    snap.worst_ms = s.window_worst;
    snap.cpu_ms = std::max(0.0, snap.frame_ms - s.wait_sum / s.window_frames);
    if (s.gpu_n) {
      snap.gpu_ms = s.gpu_sum / s.gpu_n;
      snap.gpu_known = true;
    }
    if (s.render_n) {
      snap.render_thread_ms = s.render_sum / s.render_n;
      snap.render_thread_known = true;
    }
    s.window_start = now;
    s.window_frames = 0;
    s.window_worst = 0.0;
    s.gpu_sum = s.render_sum = s.wait_sum = 0.0;
    s.gpu_n = s.render_n = 0;
  }
}

void OnGpuFrame(double ms) {
  banjo::renut_gpu::StatsGpuFrame(ms);
  banjo::profiler::SetGpuFrameMs(ms);  // the profiler window's "GPU Frame" (package 27: nobody fed it before)
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.gpu_sum += ms;
  ++s.gpu_n;
  s.gpu.Add(ms);  // blur-nx (2026-11-18, the frame pacing)
  s.frame_gpu = std::max(s.frame_gpu, ms);
}

void OnRenderThreadFrame(double busy_ms) {
  banjo::renut_gpu::StatsRenderThreadFrame(busy_ms);
  // The profiler window (package 27): the render thread's frame ends with its present, so this is also the time
  // between two host presents ("Present"); one thread, as RecordHostPresent needs.
  banjo::profiler::SetRenderThreadMs(busy_ms);
  banjo::profiler::RecordHostPresent();
  // Package 27c: the presented frame for the console's FPS. Only banjo-nx's own renderer (and the SDK's) counted
  // them, so with Renut's renderer the Status Monitor kept an old value (19.7) and the profile said "game 0.0 fps".
  RexSwitchPerfCount(0);
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.render_sum += busy_ms;
  ++s.render_n;
  s.render.Add(busy_ms);  // blur-nx (2026-11-18, the frame pacing)
  s.frame_render = std::max(s.frame_render, busy_ms);
}

void OnGameThreadWait(double ms, int source) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.wait_sum += ms;
  if (source >= 0 && source < 4) {  // blur-nx (2026-11-13, #37's census; 2026-11-18: 4, the limiter)
    s.source_ms[source] += ms;
    ++s.source_n[source];
    // blur-nx (2026-11-18, the frame pacing): the running frame's waits are the game thread's own; another thread's
    // (a worker at a fence) is kept apart, so a slow frame's line says what the thread at Swap waited on.
    if (std::this_thread::get_id() == s.game_thread) {
      s.frame_wait[source] += ms;
    } else {
      s.other_wait_ms += ms;
      ++s.other_wait_n;
    }
  }
}

std::string WaitReport() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  const auto now = Clock::now();
  const double seconds = std::max(1e-3, std::chrono::duration<double>(now - s.report_start).count());
  std::string out = fmt::format(
      "game thread's waits for the renderer per second (#37's census): fences {:.0f} ({:.1f} ms), Swap's frames-ahead "
      "limit {:.0f} ({:.1f} ms), the display's refresh {:.0f} ({:.1f} ms), the frame limiter {:.0f} ({:.1f} ms)",
      double(s.source_n[0]) / seconds, s.source_ms[0] / seconds, double(s.source_n[1]) / seconds,
      s.source_ms[1] / seconds, double(s.source_n[2]) / seconds, s.source_ms[2] / seconds,
      double(s.source_n[3]) / seconds, s.source_ms[3] / seconds);
  for (int i = 0; i < 4; ++i) {
    s.source_ms[i] = 0.0;
    s.source_n[i] = 0;
  }
  s.report_start = now;
  return out;
}

std::string PacingReport() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  const auto now = Clock::now();
  const double seconds = std::max(1e-3, std::chrono::duration<double>(now - s.pacing_start).count());
  const Hist &g = s.game;
  auto fps = [](double ms) { return ms > 0.0 ? 1000.0 / ms : 0.0; };
  const double mean = g.n ? g.sum / g.n : 0.0;
  const double low1 = g.SlowestMean(0.01), low01 = g.SlowestMean(0.001);
  std::string out = fmt::format(
      "frame pacing (blur-nx, for 1 % lows), {:.1f} s: {} game frames, mean {:.2f} ms ({:.2f} fps); 1 % low {:.2f} fps "
      "(the slowest 1 %: {:.2f} ms), 0.1 % low {:.2f} fps ({:.2f} ms); the 50th / 90th / 99th / 99.9th percentiles "
      "{:.2f} / {:.2f} / {:.2f} / {:.2f} ms, the longest {:.2f} ms; render thread busy: mean {:.2f}, 99th {:.2f}, longest "
      "{:.2f} ms ({} frames); GPU: mean {:.2f}, 99th {:.2f}, longest {:.2f} ms ({} frames); waits by other threads {} "
      "({:.1f} ms)",
      seconds, g.n, mean, fps(mean), fps(low1), low1, fps(low01), low01, g.Percentile(0.5), g.Percentile(0.9),
      g.Percentile(0.99), g.Percentile(0.999), g.worst, s.render.n ? s.render.sum / s.render.n : 0.0,
      s.render.Percentile(0.99), s.render.worst, s.render.n, s.gpu.n ? s.gpu.sum / s.gpu.n : 0.0, s.gpu.Percentile(0.99),
      s.gpu.worst, s.gpu.n, s.other_wait_n, s.other_wait_ms);
  out += "\n[renut] frame pacing's slowest frames (ms at the race's second: the game thread's waits for fences / Swap's "
         "frames-ahead limit / the display / the limiter; the longest render thread and GPU frames reported in it):";
  for (int i = 0; i < s.slow_n; ++i) {
    const SlowFrame &f = s.slow[i];
    out += fmt::format(" {:.1f} at {:.1f} s (waits {:.1f} / {:.1f} / {:.1f} / {:.1f}; render {:.1f}, GPU {:.1f});", f.ms,
                       f.at_s, f.wait[0], f.wait[1], f.wait[2], f.wait[3], f.render_ms, f.gpu_ms);
  }
  out += fmt::format("\n[renut] frame pacing's bins (a quarter ms each, bin:count): game {}; render {}; gpu {}",
                     g.Bins(), s.render.Bins(), s.gpu.Bins());
  s.game = Hist{};
  s.render = Hist{};
  s.gpu = Hist{};
  s.slow_n = 0;
  s.other_wait_ms = 0.0;
  s.other_wait_n = 0;
  s.pacing_start = now;
  return out;
}

Snapshot Get() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  return s.snap;
}

}  // namespace renut::frame_stats
