// katamari-nx (2026-10-30): the keys of banjo-nx files this port does not build, because their code is written for
// N&B's addresses (cpu/banjo_reverb_native.cpp, cpu/banjo_xaudio_native.cpp, cpu/banjo_hot_guest.cpp,
// renut_engine/hooks.cpp, renut_engine/frameHooks.cpp). banjo-nx's enhancement table (cpu/banjo_enhancements.cpp) and
// Renut's renderer still read them by name, so they are defined here, off: nothing in this build uses their code.
// Each comes back with its own file once it has been moved to Beautiful Katamari's functions (docs/katamari-notes.md).

#include <rex/cvar.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "profiler/profiler.h"
#include "renut_engine/display_sync.h"

// N&B's XAudio natives (its reverb and sample-rate converter at N&B's addresses).
REXCVAR_DEFINE_BOOL(banjo_native_reverb, false, "Katamari/Not built", "N&B's native XAudio reverb (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_reverb_verify, false, "Katamari/Not built", "Verify mode of the above (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src, false, "Katamari/Not built", "N&B's native XAudio SRC (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src_verify, false, "Katamari/Not built", "Verify mode of the above (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The hand-written natives' keys and banjo_hot_count are cpu/banjo_hot_guest.cpp's (Katamari's natives: memset and the
// matrix routines, cpu/hot/banjo_hand_natives.h).

// renut_engine/hooks.cpp's MSAA switch: N&B's own key, read only by its mid-asm hooks at N&B's render-target setup
// (hooks.cpp, which this port does not build). Renut's renderer reads msaa_level (1/2/4) instead (vivapinata-nx's note,
// 2026-11-12; docs/katamari-notes.md, run emu3_nomsaa). Kept so the tomls that name it still load.
REXCVAR_DEFINE_BOOL(disable_msaa, false, "Katamari/Graphics",
                    "Not read in this port (N&B's key): msaa_level sets the samples (4, 2 or 1)");

// The frame cap (renut_engine/frameHooks.cpp's limiter, not built here because of its N&B hooks).
REXCVAR_DEFINE_STRING(frame_cap, "Off", "Katamari/Performance",
                      "Frame rate cap: Off (the game's own, uncapped as on the Xbox 360) or a number of frames a second")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// katamari-nx (2026-11-02, the user's request: a 30 fps cap in katamari.toml, off by default): renutFrameLimit of
// renut_engine/frameHooks.cpp, called at the end of the game's Swap (d3d/hooks/device.cpp). With the display's refresh
// known (the Switch) a frame is released at a refresh plus the measured phase (renut_engine/display_sync.h), else on its
// own timer; a late frame goes at once. Off, it returns at the first test and starts no thread.
// blur-nx (2026-11-18, the game menu): the cap chosen in the menu, from the next frame (-1: frame_cap's, read at start).
// The key itself stays as read (a string another thread would be writing); the menu writes the player's file.
static std::atomic<int> g_frame_cap_live{-1};
void BlurSetFrameCapLive(int fps) { g_frame_cap_live.store(fps < 0 ? 0 : fps, std::memory_order_relaxed); }
int BlurFrameCapNow() {
  const int live = g_frame_cap_live.load(std::memory_order_relaxed);
  if (live >= 0)
    return live;
  const std::string &v = REXCVAR_GET(frame_cap);
  return v.empty() || v == "Off" ? 0 : std::atoi(v.c_str());
}

void renutFrameLimit() {
  using clock = std::chrono::steady_clock;
  static const int configured = [] {
    const std::string &v = REXCVAR_GET(frame_cap);
    return v.empty() || v == "Off" ? 0 : std::atoi(v.c_str());
  }();
  const int live = g_frame_cap_live.load(std::memory_order_relaxed);
  const int fps = live >= 0 ? live : configured;
  // pacman-nx (2026-11-03, pacman_vblank_display_sync, futari-nx's): with no frame cap the display's refresh starts the
  // game's next frame (display_sync.h). The game's speed follows its frames.
  if (fps <= 0) {
    static const bool pacing = banjo::display_sync::FramePacingOn();
    if (pacing)
      banjo::display_sync::WaitForFrameStart();
    return;
  }
  // sa2-nx (2026-10-28, sa2_renut_queue_drain): this release a refresh later, once, when the display thread asks.
  static const bool drain = banjo::display_sync::QueueDrainOn();
  const bool hold = drain && banjo::display_sync::TakeHold();
  constexpr uint64_t kRefreshNs = 16'666'667ull;
  banjo::display_sync::Start();
  static clock::time_point next = clock::now();
  static uint64_t previous_release = 0;
  const clock::time_point wait_start = clock::now();
  clock::time_point now = wait_start;
  const auto frame = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
  constexpr auto kTail = std::chrono::microseconds(400);
  const auto wait_until = [&](clock::time_point target) {
    for (;;) {
      now = clock::now();
      if (now >= target)
        break;
      if (target - now > kTail)
        std::this_thread::sleep_for(target - now - kTail);
      else
        std::this_thread::yield();
    }
  };
  bool anchored = false;
  uint64_t release = banjo::display_sync::NextRelease(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()), previous_release,
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(frame).count()), &anchored);
  if (release && hold)
    release += kRefreshNs;
  if (release) {
    previous_release = release;
    next = clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(release)));
    wait_until(next);
    banjo::display_sync::NoteRelease(release, anchored);
  } else {
    previous_release = 0;
    next += frame;
    if (next <= now)
      next = now;  // a slow frame: from the present, no burst to catch up
    else
      wait_until(next);
  }
  banjo::profiler::RecordLimiterWait(std::chrono::duration<double, std::milli>(clock::now() - wait_start).count());
}

// banjo-nx's PM4 renderer's D3D-level fast paths (me/banjo_d3d_clear.h, me/banjo_d3d_native.h, defined there because
// banjo_me_hooks.cpp includes them): its hooks are N&B's, so off here (masseffect_renderer_native stays false anyway).
REXCVAR_DEFINE_BOOL(banjo_d3d_api_clear, false, "Katamari/Not built", "banjo-nx's PM4 renderer: Clear markers (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_api_resolve, false, "Katamari/Not built", "banjo-nx's PM4 renderer: Resolve markers (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_indexed, false, "Katamari/Not built", "banjo-nx's PM4 renderer: native DrawIndexed (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_vertices, false, "Katamari/Not built", "banjo-nx's PM4 renderer: native DrawVertices (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Game frames presented (banjo-nx counted them in banjo_me_hooks.cpp's Swap hook): Renut's Present hook counts them
// here (d3d/hooks/draw.cpp); the captures' video waits on it.
std::atomic<uint64_t> g_banjo_game_frames{0};


// g_banjo_store_cards (whether the generated code marks the store cards) is cpu/banjo_hot_guest.cpp's again.

// renut_engine/cvar_menu.cpp's toml writer (N&B's in-game settings menu, not built): the keyboard controls dialog
// calls it after a rebind. Not written here: the toml a test or the user wrote stays as it is.
void RenutSaveConfig() {}
