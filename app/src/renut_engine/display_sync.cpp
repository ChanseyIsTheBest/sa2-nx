// banjo-nx (2026-10-19, latency batch 3): see display_sync.h. After futari-nx's futari_vblank_display_sync and
// futari_vblank_phase_auto (the user's own port, docs/futari-notes.md "Input lag"), for a game paced by the frame
// limiter rather than by its vertical blank, whose frames take more than one refresh from release to image.
#include "renut_engine/display_sync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/frame_latency.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "me/me_platform_compat.h"  // RexSwitchPinCurrentThreadToCore and the priority helper
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"  // StartLongLivedThread
#include "renut_engine/perf/frame_stats.h"  // port 53: WaitForFrameStart's wait is a game-thread wait

#if REX_PLATFORM_SWITCH
#include <switch.h>  // viGetDisplayVsyncEvent
#endif

// Port 258 (sasrt-nx's sync note, 2026-11-22): the FPS overlay's "Shown" counter (port 196) through a function, so a port
// without the base's overlay (renut_engine/Fps.h, hooks.h) builds this file unchanged: renut_engine/FPS.cpp has the real
// one, and this weak empty one stands in where it is missing.
void RenutCountShownFrame();
__attribute__((weak)) void RenutCountShownFrame() {}

REXCVAR_DEFINE_BOOL(banjo_display_sync, false, "Banjo/Enhancements",
                    "OFF by default (2026-10-19): in Ryujinx every run with the thread active stalled or hung (runs 65, "
                    "67, 68), the runs without it did not; to be tried on the console with configs/"
                    "banjo_display_sync_on.toml. Latency batch 3 (2026-10-19, the Switch): a thread follows the display's refresh (its vsync "
                    "event); the frame limiter (frame_cap) releases each frame at a refresh plus a phase instead of on "
                    "its own timer, whose phase to the display slid (an image finished just after a refresh waited "
                    "almost a whole refresh more; now and then one was shown three refreshes and the next one). A frame "
                    "that ends late is released at once, as before. The [display] line every 10 s gives present -> "
                    "display and input -> display (renut_engine/display_sync.cpp)");
REXCVAR_DEFINE_BOOL(banjo_display_phase_auto, true, "Banjo/Enhancements",
                    "banjo_display_sync: the phase found by measurement: as late after the refresh as the frames' "
                    "images still reach the display at the refresh they reached before (0.25 ms later every 90 frames "
                    "in time, 2 ms earlier at one that is not, and no later than that for a minute)");
REXCVAR_DEFINE_INT32(banjo_display_phase_max_us, 12000, "Banjo/Enhancements",
                     "banjo_display_phase_auto: the latest it puts the release after the refresh (microseconds)")
    .range(0, 16000);
REXCVAR_DEFINE_INT32(banjo_display_phase_us, 0, "Banjo/Enhancements",
                     "banjo_display_sync with banjo_display_phase_auto off: the release this long after the refresh "
                     "(microseconds)")
    .range(0, 16000);

// sa2-nx (2026-10-28, sa2_renut_queue_drain; ketsui-nx's, passed on by futari-nx): display_sync.h. pacman-nx: on by
// default, the user's call (2026-11-03).
REXCVAR_DEFINE_BOOL(pacman_renut_queue_drain, true, "PacMan/Renderer",
                    "With pacman_renut_double_buffer (two swapchain images): when every new image misses its refresh for "
                    "a second (the display queue stuck one image deep: each image waits for the one the display gives "
                    "back at that same refresh), the game's next frame is released one refresh later, once; the queue "
                    "empties and every image after it is shown a refresh sooner (about 17 ms less input lag). Again at "
                    "the earliest 5 s later, doubled up to 5 minutes while the queue keeps filling again soon after "
                    "with few images in time. "
                    "Also keeps late input from pushing the frames back into it (1 ms more margin a late image)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, pacman_renut_double_buffer);  // gpu/backend/video.cpp
// pacman-nx (2026-11-03, the user: "look how futari does it"): futari-nx's futari_vblank_display_sync and
// futari_vblank_phase_auto (futari_renut_vblank.cpp, docs/futari-notes.md "Input lag"). See display_sync.h.
// renut-ultimate (2026-11-12, port 97; the user's call): off by default. It fixed pacman-nx's input lag at its full
// frame rate, but in blur-nx, a game under the refresh rate (18-21 fps), it cost 15-18 % of the frame rate with no
// latency gain (each frame waits for a refresh it has just missed). A game that wants it sets it true in its own toml
// (with banjo_display_sync, which it needs). docs/off-by-default.md.
REXCVAR_DEFINE_BOOL(pacman_vblank_display_sync, false, "PacMan/Renderer",
                    "OFF BY DEFAULT in the base (slower in a game under the refresh rate: blur-nx at 18-21 fps lost 15-18 % "
                    "of its frame rate with no latency gain; pacman-nx turns it on). "
                    "The display's refresh starts the game's frames, as the 360's vertical blank paces a game: the game "
                    "waits at its Swap until the refresh plus the phase (banjo_display_phase_auto). Off: the game runs "
                    "until the render thread holds it back, with frames queued behind each other (about 3 refreshes of "
                    "input lag in the first console run). Needs banjo_display_sync; a numeric frame_cap uses the limiter")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REX_PLATFORM_SWITCH
// banjo-nx (2026-10-20): libnx's default window (default_window.o: __nx_win_init, __nx_win_exit, nwindowGetDefault)
// done here, the same calls in the same order (from its disassembly), so its display gives the refresh thread below
// its vsync event. A second viOpenDefaultDisplay in the process failed on the console (package 36i's logs: "cannot
// open the display"), the display being libnx's already. Defining all three keeps default_window.o out of the link;
// libnx's init.o calls the two through its weak references as before.
namespace {
ViDisplay g_win_display;
ViLayer g_win_layer;
NWindow g_win;
std::atomic<bool> g_win_ready{false};
}  // namespace

extern "C" {
NWindow *nwindowGetDefault(void) { return &g_win; }

void __nx_win_init(void) {
  Result rc = viInitialize(ViServiceType_Default);
  if (R_SUCCEEDED(rc)) {
    rc = viOpenDisplay("Default", &g_win_display);
    if (R_SUCCEEDED(rc)) {
      rc = viCreateLayer(&g_win_display, &g_win_layer);
      if (R_SUCCEEDED(rc)) {
        rc = viSetLayerScalingMode(&g_win_layer, ViScalingMode_FitToLayer);
        if (R_SUCCEEDED(rc))
          rc = nwindowCreateFromLayer(&g_win, &g_win_layer);
        if (R_SUCCEEDED(rc)) {
          nwindowSetDimensions(&g_win, 1280, 720);
          g_win_ready.store(true, std::memory_order_release);
          return;
        }
        viCloseLayer(&g_win_layer);
      }
      viCloseDisplay(&g_win_display);
    }
    viExit();
  }
  diagAbortWithResult(0x2559);  // MAKERESULT(Module_Libnx, LibnxError_BadGfxInit), as libnx
}

void __nx_win_exit(void) {
  g_win_ready.store(false, std::memory_order_release);
  nwindowClose(&g_win);
  viCloseLayer(&g_win_layer);
  viCloseDisplay(&g_win_display);
  viExit();
}
}  // extern "C"
#endif

namespace banjo::display_sync {
namespace {
// Port 172 (from fnxf_xiii, its notes of 2026-10-27 and 10-29; banjo_vblank_from_display): the display's refreshes, for
// the SDK's guest vblank worker (sdk/src/graphics/graphics_system.cpp, rex::graphics::g_display_refresh_wait).
std::atomic<uint64_t> g_refresh_count{0};
std::mutex g_refresh_mutex;
std::condition_variable g_refresh_cv;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

constexpr int kVsyncThreadPriority = 0x2C;  // the render thread's: it wakes at each refresh and does little
constexpr int kVsyncThreadCore = 2;         // the background core (the game and render threads run on 0 and 1)
constexpr uint64_t kGridStaleNs = 100'000'000ull;  // no refresh for 100 ms: the limiter's own timer
constexpr uint32_t kGoodFrames = 90;               // 3 s at 30 fps in time: the phase 0.25 ms later
constexpr uint64_t kPhaseStepNs = 250'000ull;
constexpr uint64_t kPhaseBackNs = 2'000'000ull;
constexpr uint64_t kHoldNs = 60'000'000'000ull;    // after a miss, no later than the phase it left, for a minute
constexpr uint32_t kFloorFrames = 32;              // the refresh count a frame takes: the least of the last 32

std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_last_vsync{0};  // the latest refresh (steady_clock ns)
std::atomic<uint64_t> g_period{0};      // the refresh period (ns), averaged
std::atomic<uint64_t> g_phase{0};       // the release after the refresh (ns)

// The limiter's releases, by the Swap of the frame each released.
struct Release {
  uint64_t swap = 0;
  uint64_t at = 0;
  uint64_t anchor = 0;  // the refresh the release is a phase after (0: not anchored, a late frame)
};
constexpr uint32_t kReleases = 64;
Release g_releases[kReleases];

// Presented frames not yet seen on the display (the display thread's, but NotePresented adds).
struct Shown {
  uint64_t swap = 0;
  uint64_t present_index = 0;
  uint64_t present_ns = 0;
  uint64_t input_ns = 0;
  uint64_t anchor = 0;
};
constexpr uint32_t kShown = 8;
std::mutex g_mutex;  // g_shown*, g_releases, the sums: a few times a frame
Shown g_shown[kShown];
uint32_t g_shown_head = 0, g_shown_count = 0;

struct Sum {
  uint64_t n = 0;
  double total = 0, max = 0;
  void Add(double ms) {
    ++n;
    total += ms;
    max = std::max(max, ms);
  }
  double Mean() const { return n ? total / double(n) : 0.0; }
};
Sum g_present_display, g_input_display, g_release_display;
uint64_t g_refreshes = 0, g_superseded = 0, g_misses = 0, g_misses_total = 0, g_phase_ups = 0;
uint64_t g_refreshes_with_image = 0;
// Diagnostics (2026-10-19, emulator runs 65 and 67 stalled with the display's refresh followed): the slowest fence test
// and the longest release wait asked for since the last report.
std::atomic<uint64_t> g_fence_test_max_ns{0}, g_release_wait_max_ns{0}, g_release_clamped{0};

// The phase (the display thread's).
uint32_t g_floor[kFloorFrames];
uint32_t g_floor_next = 0, g_floor_count = 0;
uint32_t g_good = 0;
uint64_t g_ceiling = 0, g_hold_until = 0;

double Ms(uint64_t from, uint64_t to) { return to > from ? double(to - from) / 1e6 : 0.0; }

// sa2-nx (2026-10-28, sa2_renut_queue_drain): the hold the next release takes (TakeHold), the late input's margin, and
// the display thread's count of new images in a row not finished at their refresh, the last drain and the least time
// to the next one; the report's late images and the drains so far.
std::atomic<bool> g_hold{false};
std::atomic<uint32_t> g_guard_us{0};
constexpr uint32_t kDrainAfter = 60;  // a second of new images all late
constexpr uint64_t kDrainGapNs = 5'000'000'000ull, kDrainGapMaxNs = 300'000'000'000ull;
// pacman-nx (2026-11-03): this many images in time since the last drain: it worked, and what filled the queue again was
// something else (a load's slow frame). The third console run: 4 drains in the loads had the gap at about 40 s when play began.
constexpr uint32_t kDrainWorkedImages = 30;
uint32_t g_in_time_since_drain = 0;
constexpr uint32_t kGuardStepUs = 1000, kGuardBackUs = 250, kGuardGood = 180, kGuardMaxUs = 8000;
uint64_t g_presented_at_vsync = 0;
uint32_t g_late_run = 0, g_guard_good = 0;
uint64_t g_last_drain = 0, g_drain_gap = kDrainGapNs;
uint64_t g_late_images = 0, g_drains = 0;

// At a refresh, the newest image: 1 finished, 0 not (shown a refresh late), -1 no new image since the last refresh or
// not known. Double buffering only (with three images a late image does not hold the next one back).
void QueueDrainAtRefresh(int ready, uint64_t now) {
  static const bool on = QueueDrainOn();
  if (!on || ready < 0)
    return;
  if (ready == 0) {
    ++g_late_images;
    g_guard_good = 0;
    g_guard_us.store(std::min(g_guard_us.load(std::memory_order_relaxed) + kGuardStepUs, kGuardMaxUs),
                     std::memory_order_relaxed);
    // A second of new images all late is the queue one image deep (one late image fills it, and with two images
    // nothing empties it while the game keeps up): the next release a refresh later. A queue that fills again within
    // twice the gap is real lateness, which a drain does not help: the gap doubles; else it starts again at 5 s. So
    // does a drain that worked (kDrainWorkedImages).
    const bool worked = g_in_time_since_drain >= kDrainWorkedImages;
    if (++g_late_run >= kDrainAfter && (!g_last_drain || now - g_last_drain >= (worked ? kDrainGapNs : g_drain_gap))) {
      g_drain_gap = !worked && g_last_drain && now - g_last_drain < 2 * g_drain_gap
                        ? std::min(g_drain_gap * 2, kDrainGapMaxNs)
                        : kDrainGapNs;
      g_last_drain = now;
      g_in_time_since_drain = 0;
      g_hold.store(true, std::memory_order_release);
      ++g_drains;
      g_late_run = 0;
    }
    return;
  }
  g_late_run = 0;
  ++g_in_time_since_drain;
  if (++g_guard_good >= kGuardGood) {
    g_guard_good = 0;
    const uint32_t guard = g_guard_us.load(std::memory_order_relaxed);
    g_guard_us.store(guard > kGuardBackUs ? guard - kGuardBackUs : 0, std::memory_order_relaxed);
  }
}

// pacman-nx (2026-11-03, pacman_vblank_display_sync, futari-nx's): the ticks that start the game's frames, and those
// not given (a queue drain). g_pacing: the game is waiting for them (the first WaitForFrameStart).
std::atomic<bool> g_pacing{false};
std::mutex g_tick_mutex;
std::condition_variable g_tick_cv;
std::atomic<uint64_t> g_ticks{0}, g_ticks_skipped{0};
uint64_t g_ticks_reported = 0;
constexpr uint32_t kPacingGoodImages = 180;           // 3 s of images in time: the phase 0.25 ms later
constexpr uint64_t kPacingHoldNs = 300'000'000'000ull;  // after a miss, no later than the phase it left, for 5 minutes

// futari_vblank_phase_auto: the frame start as late after the refresh as the newest image is still finished for the next
// one. An image not finished at its refresh is shown one later: back 2 ms and hold there; else later, slowly.
void PacingPhaseAtRefresh(int ready, uint64_t now) {
  if (!REXCVAR_GET(banjo_display_phase_auto))
    return;
  const uint64_t max = uint64_t(REXCVAR_GET(banjo_display_phase_max_us)) * 1000;
  std::lock_guard lock(g_mutex);
  if (g_hold_until && now > g_hold_until)
    g_hold_until = 0;
  if (!g_hold_until)
    g_ceiling = max;
  const uint64_t phase = g_phase.load(std::memory_order_relaxed);
  if (ready == 0) {
    ++g_misses;
    ++g_misses_total;
    const uint64_t back = phase > kPhaseBackNs ? phase - kPhaseBackNs : 0;
    g_phase.store(back, std::memory_order_relaxed);
    g_ceiling = back;
    g_hold_until = now + kPacingHoldNs;
    g_good = 0;
  } else if (ready == 1 && ++g_good >= kPacingGoodImages) {
    g_good = 0;
    const uint64_t up = std::min(phase + kPhaseStepNs, g_ceiling);
    if (up > phase) {
      g_phase.store(up, std::memory_order_relaxed);
      ++g_phase_ups;
    }
  }
}

// A displayed frame's refresh count from its anchor: the phase's measurement. Requires g_mutex.
void PhaseFrame(uint32_t refreshes, uint64_t now) {
  if (!REXCVAR_GET(banjo_display_phase_auto))
    return;
  const uint64_t max = uint64_t(REXCVAR_GET(banjo_display_phase_max_us)) * 1000;
  if (g_hold_until && now > g_hold_until) {
    g_hold_until = 0;
    g_ceiling = max;
  }
  if (!g_hold_until)
    g_ceiling = max;
  uint32_t floor = refreshes;
  for (uint32_t i = 0; i < g_floor_count; ++i)
    floor = std::min(floor, g_floor[i]);
  g_floor[g_floor_next] = refreshes;
  g_floor_next = (g_floor_next + 1) % kFloorFrames;
  g_floor_count = std::min(g_floor_count + 1, kFloorFrames);
  const uint64_t phase = g_phase.load(std::memory_order_relaxed);
  if (refreshes > floor && g_floor_count > 1) {
    // Its image reached the display a refresh later than the frames before: back off, and stay there a while. The
    // frames from here make the floor again (a scene that needs a refresh more settles at it).
    ++g_misses;
    ++g_misses_total;
    const uint64_t back = phase > kPhaseBackNs ? phase - kPhaseBackNs : 0;
    g_phase.store(back, std::memory_order_relaxed);
    g_ceiling = back;
    g_hold_until = now + kHoldNs;
    g_good = 0;
    g_floor_count = 0;
    g_floor_next = 0;
    return;
  }
  if (++g_good >= kGoodFrames) {
    g_good = 0;
    const uint64_t up = std::min(phase + kPhaseStepNs, g_ceiling);
    if (up > phase) {
      g_phase.store(up, std::memory_order_relaxed);
      ++g_phase_ups;
    }
  }
}

// At a refresh: the newest presented frame whose GPU work is done is the one the display shows from it; older ones
// not shown yet never are. The fences are tested without g_mutex (NotePresented takes it on the render thread at every
// present): the frames are copied out first and found again by their present index.
void AtRefresh(uint64_t now, uint64_t period) {
  uint64_t indices[kShown];
  uint32_t count = 0;
  {
    std::lock_guard lock(g_mutex);
    ++g_refreshes;
    for (uint32_t k = 0; k < g_shown_count; ++k)
      indices[count++] = g_shown[(g_shown_head + k) % kShown].present_index;
  }
  uint64_t done_index = 0;
  for (uint32_t k = 0; k < count; ++k) {
    // -1 (unknown): too old to matter, or no fences (not the deko3d back end): taken as done.
    const uint64_t t0 = NowNs();
    const int done = renut::gpu::Video::PresentDone(indices[k]);
    const uint64_t took = NowNs() - t0;
    if (took > g_fence_test_max_ns.load(std::memory_order_relaxed))
      g_fence_test_max_ns.store(took, std::memory_order_relaxed);
    if (done == 0)
      break;  // in order on one queue: the later ones are not done either
    done_index = indices[k];
  }
  if (!done_index)
    return;
  std::lock_guard lock(g_mutex);
  int newest = -1;
  for (uint32_t k = 0; k < g_shown_count; ++k) {
    if (g_shown[(g_shown_head + k) % kShown].present_index == done_index) {
      newest = int(k);
      break;
    }
  }
  if (newest < 0)
    return;  // dropped meanwhile (eight presents without a refresh)
  ++g_refreshes_with_image;
  // renut-ultimate (2026-11-20, port 196): S4ep1-nx's of 2026-11-15 (github.com/FurryLilManChild/S4ep1-nx, GPL-3.0).
  // The FPS counter counts what the player sees: refreshes that showed a new image. The overlay's only other counter
  // ("Tick", renut_engine/FPS.cpp) is N&B's appMainTickPreDraw hook, built for no other game, so it showed nothing.
  RenutCountShownFrame();  // port 258
  g_superseded += uint64_t(newest);
  const Shown s = g_shown[(g_shown_head + uint32_t(newest)) % kShown];
  g_shown_head = (g_shown_head + uint32_t(newest) + 1) % kShown;
  g_shown_count -= uint32_t(newest) + 1;
  g_present_display.Add(Ms(s.present_ns, now));
  if (s.input_ns)
    g_input_display.Add(Ms(s.input_ns, now));
  if (s.anchor && period) {
    g_release_display.Add(Ms(s.anchor, now));
    const double refreshes = double(now - s.anchor) / double(period);
    PhaseFrame(uint32_t(std::lround(refreshes)), now);
  }
}

void Run() {
#if REX_PLATFORM_SWITCH
  const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(kVsyncThreadPriority);
  const bool core_ok = RexSwitchPinCurrentThreadToCore(kVsyncThreadCore);
  // 2026-10-20: the default window's display (opened by __nx_win_init above); a display of its own only without it.
  ViDisplay own{};
  ViDisplay *display = g_win_ready.load(std::memory_order_acquire) ? &g_win_display : nullptr;
  Event vsync{};
  if (!display) {
    const Result rc = viOpenDefaultDisplay(&own);
    if (R_FAILED(rc)) {
      REXLOG_WARN("[display] cannot open the display (0x{:X}): the frame limiter keeps its own timer "
                  "(banjo_display_sync)", rc);
      return;
    }
    display = &own;
  }
  if (const Result rc = viGetDisplayVsyncEvent(display, &vsync); R_FAILED(rc)) {
    REXLOG_WARN("[display] no vsync event from the display (0x{:X}): the frame limiter keeps its own timer "
                "(banjo_display_sync)", rc);
    if (display == &own)
      viCloseDisplay(&own);
    return;
  }
  g_phase.store(REXCVAR_GET(banjo_display_phase_auto) ? 0 : uint64_t(REXCVAR_GET(banjo_display_phase_us)) * 1000,
                std::memory_order_relaxed);
  REXLOG_INFO("[display] the frame limiter follows the display's refresh (banjo_display_sync), the phase {} "
              "(thread at priority 0x{:X}{}, core {}{})",
              REXCVAR_GET(banjo_display_phase_auto)
                  ? fmt::format("found by measurement, at most {} us (banjo_display_phase_auto)",
                                REXCVAR_GET(banjo_display_phase_max_us))
                  : fmt::format("{} us (banjo_display_phase_us)", REXCVAR_GET(banjo_display_phase_us)),
              kVsyncThreadPriority, priority_ok ? "" : " NOT accepted", kVsyncThreadCore, core_ok ? "" : " NOT accepted");
  g_running.store(true, std::memory_order_release);
  uint64_t last = 0;
  double period = 1e9 / 60.0;
  for (;;) {
    // 100 ms: should the display stop refreshing (sleep mode), the grid goes stale and the limiter uses its timer.
    if (R_FAILED(eventWait(&vsync, 100'000'000ull)))
      continue;
    eventClear(&vsync);
    const uint64_t now = NowNs();
    if (last) {
      const uint64_t gap = now - last;
      if (gap < 8'000'000ull)
        continue;  // a guard: an event that did not clear
      // One refresh apart (not two after a missed wake): the period, averaged over about a second.
      const double refreshes = std::max(1.0, std::round(double(gap) / period));
      if (refreshes <= 4.0)
        period += (double(gap) / refreshes - period) / 64.0;
    }
    last = now;
    g_period.store(uint64_t(period), std::memory_order_relaxed);
    g_last_vsync.store(now, std::memory_order_release);
    {
      std::lock_guard lock(g_refresh_mutex);  // port 172: a refresh for the vblank worker
      g_refresh_count.fetch_add(1, std::memory_order_release);
    }
    g_refresh_cv.notify_all();
    // sa2-nx (2026-10-28, sa2_renut_queue_drain): the newest image at this refresh, before AtRefresh's own fence tests.
    const uint64_t presented = renut::gpu::Video::PresentCount();
    const bool new_image = presented != g_presented_at_vsync;
    g_presented_at_vsync = presented;
    const int ready = new_image ? renut::gpu::Video::PresentDone(presented) : -1;
    QueueDrainAtRefresh(ready, now);
    AtRefresh(now, uint64_t(period));
    // pacman-nx (2026-11-03, pacman_vblank_display_sync, futari-nx's FromDisplay): this refresh starts the game's next
    // frame, after the phase. A queue drain gives it no tick: no frame starts in this refresh, the image before makes
    // it, and every image after it is shown a refresh sooner.
    if (g_pacing.load(std::memory_order_relaxed)) {
      PacingPhaseAtRefresh(ready, now);
      if (TakeHold()) {
        g_ticks_skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (const uint64_t phase = g_phase.load(std::memory_order_relaxed))
        svcSleepThread(int64_t(phase));
      {
        std::lock_guard lock(g_tick_mutex);
        g_ticks.fetch_add(1, std::memory_order_relaxed);
      }
      g_tick_cv.notify_one();
    }
  }
#endif
}

}  // namespace

bool FramePacingOn() {
  return REXCVAR_GET(pacman_vblank_display_sync) && REXCVAR_GET(banjo_display_sync);
}

void WaitForFrameStart() {
  g_pacing.store(true, std::memory_order_relaxed);
  Start();
  if (!g_running.load(std::memory_order_acquire))
    return;  // the display thread is not up yet, or could not get the refresh: the game runs on
  static uint64_t seen = 0;  // the game's thread only
  // 50 ms: should the display stop refreshing (sleep mode), the game still gets a frame now and then. A tick that came
  // while the frame ran starts the next one at once (a late frame), and several give one start, no burst.
  std::unique_lock lock(g_tick_mutex);
  // pacman-nx (2026-11-03, the third console run): after a hitch (a load's slow frame, kResyncTicks refreshes or more) the
  // next frame waits for the next tick. Started at once, part-way through a refresh, it missed the next one and filled the
  // display queue the hitch had just emptied, a refresh of input lag until a drain. A frame only a little late still goes
  // at once: no 30 fps in a scene just over the budget.
  constexpr uint64_t kResyncTicks = 3;
  if (g_ticks.load(std::memory_order_relaxed) - seen >= kResyncTicks)
    seen = g_ticks.load(std::memory_order_relaxed);
  const auto waiting = std::chrono::steady_clock::now();
  g_tick_cv.wait_for(lock, std::chrono::milliseconds(50),
                     [] { return g_ticks.load(std::memory_order_relaxed) != seen; });
  seen = g_ticks.load(std::memory_order_relaxed);
  lock.unlock();
  // renut-ultimate port 53 (2026-11-09; sasrt-nx found the same in its own pacing): the wait for the frame's refresh is
  // the game thread waiting, not its work: frame_stats' CPU time (the FPS overlay's, and sasrt_dynamic_cpu_aware's
  // budget, port 52) leaves it out. It had counted it, so a paced game's CPU side was its whole frame.
  renut::frame_stats::OnGameThreadWait(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waiting).count(), 2);
}

bool QueueDrainOn() {
  return REXCVAR_GET(pacman_renut_queue_drain) && REXCVAR_GET(pacman_renut_double_buffer) &&
         REXCVAR_GET(banjo_display_sync);
}

bool TakeHold() {
  if (!g_hold.load(std::memory_order_relaxed))
    return false;
  return g_hold.exchange(false, std::memory_order_acquire);
}

uint32_t LateInputGuardUs() {
  return g_guard_us.load(std::memory_order_relaxed);
}

// Port 172: the refreshes counted so far, once one past 'seen' has come or 'timeout_ns' has passed (0: at once).
uint64_t WaitRefresh(uint64_t seen, uint64_t timeout_ns) {
  std::unique_lock lock(g_refresh_mutex);
  if (timeout_ns)
    g_refresh_cv.wait_for(lock, std::chrono::nanoseconds(timeout_ns),
                          [&] { return g_refresh_count.load(std::memory_order_acquire) != seen; });
  return g_refresh_count.load(std::memory_order_acquire);
}

void Start() {
#if REX_PLATFORM_SWITCH
  static std::once_flag once;
  std::call_once(once, [] {
    if (REXCVAR_GET(banjo_display_sync)) {
      banjo::renut_gpu::StartLongLivedThread(&Run, 128 * 1024);
      rex::graphics::g_display_refresh_wait = &WaitRefresh;  // port 172 (used with banjo_vblank_from_display)
    }
  });
#endif
}

uint64_t NextRelease(uint64_t now, uint64_t previous, uint64_t frame_ns, bool *anchored) {
  *anchored = false;
  if (!g_running.load(std::memory_order_acquire))
    return 0;
  const uint64_t vsync = g_last_vsync.load(std::memory_order_acquire);
  const uint64_t period = g_period.load(std::memory_order_relaxed);
  if (!vsync || !period || now > vsync + kGridStaleNs)
    return 0;
  const uint64_t refreshes = std::max<uint64_t>(1, uint64_t(std::llround(double(frame_ns) / double(period))));
  const uint64_t frame = refreshes * period;
  // A frame that ended after its time: at once, as the timer did (no waiting for a later refresh: no 20 fps).
  if (previous && now >= previous + frame)
    return now;
  // The first refresh plus the phase at or after both now and half a refresh before the frame's time (the previous
  // release was a refresh plus the phase too, or a late one: the grid is found again here).
  const uint64_t phase = g_phase.load(std::memory_order_relaxed);
  const uint64_t earliest = std::max(now, previous ? previous + frame - period / 2 : now);
  const int64_t from = int64_t(earliest) - int64_t(vsync + phase);
  const int64_t n = from <= 0 ? -((-from) / int64_t(period)) : (from + int64_t(period) - 1) / int64_t(period);
  const int64_t at = int64_t(vsync + phase) + n * int64_t(period);
  const uint64_t release = uint64_t(std::max<int64_t>(at, int64_t(now)));
  const uint64_t wait = release - now;
  if (wait > g_release_wait_max_ns.load(std::memory_order_relaxed))
    g_release_wait_max_ns.store(wait, std::memory_order_relaxed);
  if (wait > frame + period) {
    // Never more than a frame and a refresh: a grid that came out wrong must not hold the game (a safety clamp).
    g_release_clamped.fetch_add(1, std::memory_order_relaxed);
    return now;
  }
  *anchored = true;
  return release;
}

void NoteRelease(uint64_t at, bool anchored) {
  if (!g_running.load(std::memory_order_acquire))
    return;
  const uint64_t swap = rex::latency::SwapCount() + 1;
  std::lock_guard lock(g_mutex);
  Release &r = g_releases[swap % kReleases];
  r.swap = swap;
  r.at = at;
  r.anchor = anchored ? at - std::min(at, g_phase.load(std::memory_order_relaxed)) : 0;
}

void NotePresented(uint64_t swap) {
  if (!g_running.load(std::memory_order_acquire)) {
    // Port 196: without the display thread (it starts with the frame limiter; Katamari uncapped, the PC) the FPS
    // counter counts each image presented, since no refresh says which were shown (Ryujinx, 2026-11-20: "0 FPS").
    RenutCountShownFrame();  // port 258
    return;
  }
  const uint64_t now = NowNs();
  const uint64_t index = renut::gpu::Video::PresentCount();
  const uint64_t input = rex::latency::InputTimeOf(swap);
  std::lock_guard lock(g_mutex);
  const Release &r = g_releases[swap % kReleases];
  Shown s;
  s.swap = swap;
  s.present_index = index;
  s.present_ns = now;
  s.input_ns = input;
  s.anchor = r.swap == swap ? r.anchor : 0;
  if (g_shown_count == kShown) {  // the display has not taken one for 8 presents: drop the oldest
    g_shown_head = (g_shown_head + 1) % kShown;
    --g_shown_count;
    ++g_superseded;
  }
  g_shown[(g_shown_head + g_shown_count) % kShown] = s;
  ++g_shown_count;
}

std::string Report() {
  if (!g_running.load(std::memory_order_acquire))
    return {};
  std::lock_guard lock(g_mutex);
  const uint64_t period = g_period.load(std::memory_order_relaxed);
  std::string line = fmt::format(
      "refresh {:.3f} ms, {} refreshes ({} with a new image); present -> display {:.1f} ms (max {:.1f}), input -> "
      "display {:.1f} ms (max {:.1f}), release -> display {:.1f} ms; {} images never shown (a newer one was ready "
      "first); phase {:.2f} ms after the refresh{}, {} misses ({} in all), {} steps later; slowest fence test {:.2f} "
      "ms, longest release wait {:.1f} ms, {} clamped",
      double(period) / 1e6, g_refreshes, g_refreshes_with_image, g_present_display.Mean(), g_present_display.max,
      g_input_display.Mean(), g_input_display.max, g_release_display.Mean(), g_superseded,
      double(g_phase.load(std::memory_order_relaxed)) / 1e6,
      g_hold_until ? fmt::format(" (held at most {:.2f} ms)", double(g_ceiling) / 1e6) : std::string(), g_misses,
      g_misses_total, g_phase_ups, double(g_fence_test_max_ns.exchange(0, std::memory_order_relaxed)) / 1e6,
      double(g_release_wait_max_ns.exchange(0, std::memory_order_relaxed)) / 1e6,
      g_release_clamped.exchange(0, std::memory_order_relaxed));
  g_present_display = g_input_display = g_release_display = Sum{};
  g_refreshes = g_refreshes_with_image = g_superseded = g_misses = g_phase_ups = 0;
  // sa2-nx (2026-10-28): with double buffering, the queue drain's counts (pacman_renut_queue_drain).
  static const bool drain_on = QueueDrainOn();
  if (drain_on) {
    line += fmt::format("; double buffering: {} new images not finished at their refresh, {} queue drains in all, late "
                        "input margin +{:.2f} ms (pacman_renut_queue_drain)",
                        g_late_images, g_drains, double(g_guard_us.load(std::memory_order_relaxed)) / 1000.0);
    g_late_images = 0;  // (the display thread's counts, read here without its lock: a report's worth of drift at most)
  }
  if (g_pacing.load(std::memory_order_relaxed)) {
    const uint64_t ticks = g_ticks.load(std::memory_order_relaxed);
    line += fmt::format("; frames started by the display at the refresh plus the phase: {}, {} refreshes skipped in all "
                        "(pacman_vblank_display_sync)",
                        ticks - g_ticks_reported, g_ticks_skipped.load(std::memory_order_relaxed));
    g_ticks_reported = ticks;
  }
  return line;
}

}  // namespace banjo::display_sync
