// sa2-nx on the ultimate base (2026-11-19): the keys of base files this port does not build (they are written for
// Beautiful Katamari's or N&B's addresses), which the kept ones still read by name; and the 1.0.6 player keys whose
// base names differ, read as aliases so a player's sa2.toml from 1.0.6 keeps working (docs/sa2-ultimate-notes.md,
// "Settings"). From the base's app/src/katamari_keys.cpp (katamari-nx, 2026-10-30), which this replaces.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "profiler/profiler.h"
#include "renut_engine/display_sync.h"

// N&B's XAudio natives (its reverb and sample-rate converter at N&B's addresses): not built here.
REXCVAR_DEFINE_BOOL(banjo_native_reverb, false, "SA2/Not built", "N&B's native XAudio reverb (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_reverb_verify, false, "SA2/Not built", "Verify mode of the above (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src, false, "SA2/Not built", "N&B's native XAudio SRC (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src_verify, false, "SA2/Not built", "Verify mode of the above (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// N&B's own MSAA key, kept so the tomls that name it still load; Renut reads msaa_level.
REXCVAR_DEFINE_BOOL(disable_msaa, false, "SA2/Graphics",
                    "Not read in this port (N&B's key): msaa_level sets the samples (1, 2 or 4)");

// The frame cap of the base's limiter (renutFrameLimit below, called at the end of the game's Swap). SA2 paces itself
// (renut_engine/gpu/sa2_renut_pacing.cpp: the game's presentation interval on a 60 Hz grid, sa2_vsync_relaxed), so the
// cap stays Off: SA2's game speed follows its frames, and a cap would slow the game.
REXCVAR_DEFINE_STRING(frame_cap, "Off", "SA2/Performance",
                      "The base's frame cap: Off (SA2 paces its own frames at 60 Hz; keep it Off) or a number of frames "
                      "a second")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The game menu's live cap (blur-nx, 2026-11-18; the base's game_menu.cpp calls these).
static std::atomic<int> g_frame_cap_live{-1};
void BlurSetFrameCapLive(int fps) { g_frame_cap_live.store(fps < 0 ? 0 : fps, std::memory_order_relaxed); }
int BlurFrameCapNow() {
  const int live = g_frame_cap_live.load(std::memory_order_relaxed);
  if (live >= 0)
    return live;
  const std::string &v = REXCVAR_GET(frame_cap);
  return v.empty() || v == "Off" ? 0 : std::atoi(v.c_str());
}

// The base's limiter (katamari-nx's renutFrameLimit, as the base has it). With frame_cap Off it returns at its first
// test unless pacman_vblank_display_sync is on (off for SA2: its own pacing does that job).
void renutFrameLimit() {
  using clock = std::chrono::steady_clock;
  static const int configured = [] {
    const std::string &v = REXCVAR_GET(frame_cap);
    return v.empty() || v == "Off" ? 0 : std::atoi(v.c_str());
  }();
  const int live = g_frame_cap_live.load(std::memory_order_relaxed);
  const int fps = live >= 0 ? live : configured;
  if (fps <= 0) {
    static const bool pacing = banjo::display_sync::FramePacingOn();
    if (pacing)
      banjo::display_sync::WaitForFrameStart();
    return;
  }
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
      next = now;
    else
      wait_until(next);
  }
  banjo::profiler::RecordLimiterWait(std::chrono::duration<double, std::milli>(clock::now() - wait_start).count());
}

// banjo-nx's PM4 renderer's D3D-level fast paths (its hooks are N&B's): off.
REXCVAR_DEFINE_BOOL(banjo_d3d_api_clear, false, "SA2/Not built", "banjo-nx's PM4 renderer: Clear markers (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_api_resolve, false, "SA2/Not built", "banjo-nx's PM4 renderer: Resolve markers (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_indexed, false, "SA2/Not built", "banjo-nx's PM4 renderer: native DrawIndexed (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_vertices, false, "SA2/Not built", "banjo-nx's PM4 renderer: native DrawVertices (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Game frames presented: Renut's Present / Swap hook counts them (d3d/hooks); the captures' video waits on it.
std::atomic<uint64_t> g_banjo_game_frames{0};

// renut_engine/cvar_menu.cpp's toml writer (N&B's in-game settings menu, not built): nothing is written here.
void RenutSaveConfig() {}

// ---------------------------------------------------------------------------------------------------------------------
// sa2-nx 1.0.6's player keys under their 1.0.6 names (app/sa2_switch.toml), read and handed to the base's keys. Only a
// value the player changed is handed over, so the base's own key still wins when a test toml sets it directly.
// 1.0.6's sa2_renut_msaa gave the game's single-sample targets 2, 4 or 8 samples (an option off by default). The base
// has no such option: its msaa_level sets the samples of the game's own MSAA targets (1 = off), another thing.
// sa2-nx-ult (2026-11-24, the user: "SA2 used to support MSAA before the rebase"): 1.0.6's MSAA is back, in the
// renderer's BuildTarget (renut_engine/gpu/render/surfaces.cpp), and the game menu's MSAA row sets this key; the
// renderer takes it once a frame (the user: "without closing and restarting the game"), as msaa_level.
REXCVAR_DEFINE_INT32(sa2_renut_msaa, 0, "SA2/Graphics",
                     "Multisample anti-aliasing of the game's single-sample render targets (sa2-nx 1.0.6's): 0 = off "
                     "(the game's own, default), 2 = 2x, 4 = 4x, 8 = 8x. Smoother edges; each step costs more GPU and "
                     "CPU time and memory. Takes effect from the next frame")
    .range(0, 8);
REXCVAR_DEFINE_BOOL(sa2_renut_double_buffer, false, "SA2/Graphics",
                    "Double buffering (sa2-nx 1.0.6's key): two images instead of three, less input lag, more dropped "
                    "frames in busy scenes. Sets pacman_renut_double_buffer when true")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sa2::keys {

// Before the renderer starts (renut_app.h OnPreSetup): it reads msaa_level and the double buffer at its start.
void ApplyAliases() {
  if (REXCVAR_GET(sa2_renut_msaa) != 0)
    REXLOG_INFO("sa2_renut_msaa = {}: MSAA on the game's single-sample render targets", REXCVAR_GET(sa2_renut_msaa));
  if (REXCVAR_GET(sa2_renut_double_buffer) && !rex::cvar::HasNonDefaultValue("pacman_renut_double_buffer")) {
    rex::cvar::SetFlagByName("pacman_renut_double_buffer", "true");
    REXLOG_INFO("sa2_renut_double_buffer = true (1.0.6's key): pacman_renut_double_buffer = true");
  }
}

}  // namespace sa2::keys
