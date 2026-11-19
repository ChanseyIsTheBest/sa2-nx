// Short video recordings of the game image (see sa2_capture.h), sa2-nx's lockstep comparison tool.
//
// From sa2_video_s seconds after start, sa2_video_frames consecutive presented frames are written as they come to
// captures/video_<s>s.rgba next to the executable (raw R8G8B8X8, frames back to back; streamed, so a long recording
// does not need RAM, ~55 MB per second at half resolution on disk) with a .txt beside it (width, height, measured fps,
// frame count). tools/make_video.sh turns it into an MP4 with ffmpeg. sa2_video_every keeps one game frame in N.
// Beside the (half-resolution, every other pixel) video, a .hash file holds the XXH3-64 of every frame at FULL
// resolution (RGB, the two wall-clock regions zeroed: "Press START" and the loading spinner), so two lockstep runs are
// compared on every pixel, not a quarter of them; a .frames file the game frame of each.
//
// sa2-nx-ult (2026-11-19): 1.0.6's recorder with Renut's path only (its native renderer's ring lockstep and the PC's
// thread-cycle report stay behind with that renderer). The game frames are the base's count (g_banjo_game_frames,
// counted at the Swap hook).

#include "sa2_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <fmt/format.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_INT32(sa2_video_s, 0, "SA2",
                     "Record a short video of the game image this many seconds after start, to captures/ next to "
                     "the executable (0 = off; works minimized). tools/make_video.sh makes the MP4")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(sa2_video_frame, 0, "SA2",
                     "After sa2_video_s, also wait for this game frame before recording (0 = off): every run then "
                     "records the same stretch of the game, whatever the PC's speed")
    .range(0, 2000000000);
REXCVAR_DEFINE_INT32(sa2_video_frames, 300, "SA2", "Frames in the video (sa2_video_s); the game runs at 60 fps")
    .range(1, 36000);
REXCVAR_DEFINE_BOOL(sa2_video_half, true, "SA2",
                    "Record the video at half resolution (~55 MB of disk per second instead of ~220 MB)");
REXCVAR_DEFINE_INT32(sa2_video_every, 1, "SA2",
                     "Record one game frame in this many (6 = 10 fps: minutes of footage to compare against a "
                     "reference without filling the disk)")
    .range(1, 600);
REXCVAR_DEFINE_INT32(sa2_video_lockstep_every, 1, "SA2",
                     "Lockstep recording (sa2_video_every = 1) that keeps one game frame in this many: the Swap still "
                     "waits for every frame, so runs stay comparable frame for frame, with a fraction of the disk")
    .range(1, 600);
REXCVAR_DEFINE_BOOL(sa2_video_lockstep, true, "SA2",
                    "While a video is recorded (sa2_video_every = 1), the game's Swap hands every frame to the "
                    "recorder and waits until it has taken it: no frame is skipped or taken late, however loaded the "
                    "PC is, and two runs label the same frames the same");

extern std::atomic<uint64_t> g_banjo_game_frames;  // src/sa2_keys.cpp, counted by Renut's Swap hook

namespace sa2::capture {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;

// Renut's lockstep (RenutLockstepFrame): one frame handed from the Swap hook to the recorder.
std::atomic<bool> g_renut_lockstep{false};
std::mutex g_renut_mutex;
std::condition_variable g_renut_cv;
bool g_renut_full = false;
bool g_renut_ok = false;
uint64_t g_renut_label = 0;
rex::ui::RawImage g_renut_image;

// Recorder thread: the next frame of Renut's lockstep (false after 100 ms without one).
bool TakeRenutFrame(rex::ui::RawImage& image, uint64_t& label, bool& ok) {
  std::unique_lock lock(g_renut_mutex);
  if (!g_renut_cv.wait_for(lock, std::chrono::milliseconds(100), [] { return g_renut_full; })) return false;
  image = std::move(g_renut_image);
  label = g_renut_label;
  ok = g_renut_ok;
  g_renut_full = false;
  g_renut_cv.notify_all();
  return true;
}

bool StopRequested() {
  std::lock_guard lock(g_mutex);
  return g_stop;
}

// The render resolution can change while recording (docked <-> handheld): later frames are resampled to the first
// frame's size (area average) so one recording keeps one frame size. Test tooling only.
void Resample(const rex::ui::RawImage& in, uint32_t width, uint32_t height, rex::ui::RawImage& out) {
  out.width = width;
  out.height = height;
  out.stride = size_t(width) * 4;
  out.data.assign(out.stride * height, 0);
  const auto weights = [](uint32_t from, uint32_t to) {
    std::vector<std::vector<std::pair<uint32_t, float>>> w(to);
    const double f = double(from) / double(to);
    for (uint32_t i = 0; i < to; ++i) {
      const double a = i * f, b = (i + 1) * f;
      for (uint32_t k = uint32_t(a); k < from && double(k) < b; ++k) {
        const double cover = std::min(b, double(k + 1)) - std::max(a, double(k));
        if (cover > 0.0) w[i].push_back({k, float(cover / f)});
      }
      if (w[i].empty()) w[i].push_back({std::min(from - 1, uint32_t(a)), 1.0f});
    }
    return w;
  };
  const auto wx = weights(in.width, width), wy = weights(in.height, height);
  std::vector<float> row(size_t(in.width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    std::fill(row.begin(), row.end(), 0.0f);
    for (const auto& [sy, ky] : wy[y]) {
      const uint8_t* src = in.data.data() + size_t(sy) * in.stride;
      for (uint32_t x = 0; x < in.width; ++x)
        for (uint32_t c = 0; c < 3; ++c) row[size_t(x) * 3 + c] += ky * float(src[size_t(x) * 4 + c]);
    }
    uint8_t* dst = out.data.data() + size_t(y) * out.stride;
    for (uint32_t x = 0; x < width; ++x) {
      float v[3] = {};
      for (const auto& [sx, kx] : wx[x])
        for (uint32_t c = 0; c < 3; ++c) v[c] += kx * row[size_t(sx) * 3 + c];
      for (uint32_t c = 0; c < 3; ++c) dst[size_t(x) * 4 + c] = uint8_t(std::clamp(v[c] + 0.5f, 0.0f, 255.0f));
      dst[size_t(x) * 4 + 3] = 255;
    }
  }
}

void Record(std::function<bool(rex::ui::RawImage&)> grab, int start_s, int frame_count) {
  {
    std::unique_lock lock(g_mutex);
    if (g_cv.wait_for(lock, std::chrono::seconds(start_s), [] { return g_stop; })) {
      return;
    }
  }
  if (const uint64_t first = uint64_t(std::max(0, REXCVAR_GET(sa2_video_frame)))) {
    while (g_banjo_game_frames.load(std::memory_order_relaxed) + 1 < first) {
      if (StopRequested()) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  const uint32_t step = REXCVAR_GET(sa2_video_half) ? 2 : 1;
  const uint64_t every = uint64_t(REXCVAR_GET(sa2_video_every));
  const auto folder = rex::filesystem::GetExecutableFolder() / "captures";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::string name = "video_" + std::to_string(start_s) + "s";
  std::ofstream raw(folder / (name + ".rgba"), std::ios::binary | std::ios::trunc);
  if (!raw) {
    REXLOG_WARN("[capture] cannot write {}", rex::path_to_utf8(folder / (name + ".rgba")));
    return;
  }
  // The game frame of every recorded frame (uint64 each; UINT64_MAX when another Swap came while it was being taken,
  // outside the lockstep): comparisons of two runs match frames by game frame.
  std::ofstream frames_file(folder / (name + ".frames"), std::ios::binary | std::ios::trunc);
  std::ofstream hash_file(folder / (name + ".hash"), std::ios::binary | std::ios::trunc);
  std::vector<uint8_t> frame, full;
  int written = 0;
  uint64_t skipped_flips = 0;
  uint64_t resampled_frames = 0;
  uint32_t width = 0, height = 0;
  const auto started = std::chrono::steady_clock::now();
  uint64_t last = g_banjo_game_frames.load(std::memory_order_relaxed);
  const uint64_t keep_every = uint64_t(std::max(1, REXCVAR_GET(sa2_video_lockstep_every)));
  // Renut's Swap hook hands every frame over (RenutLockstepFrame); labels are game frames.
  const bool renut_lockstep = REXCVAR_GET(sa2_video_lockstep) && every == 1;
  uint64_t renut_first = 0;
  if (renut_lockstep) g_renut_lockstep.store(true, std::memory_order_release);
  REXLOG_INFO("[capture] recording {} frames from {} s, game frame {}{}", frame_count, start_s, last,
              renut_lockstep ? " (lockstep: the Swap waits for every frame; labels are game frames)" : "");
  while (written < frame_count && !StopRequested()) {
    uint64_t now = 0, label = 0;
    rex::ui::RawImage image;
    if (renut_lockstep) {
      bool ok = false;
      if (!TakeRenutFrame(image, now, ok)) continue;
      if (!renut_first) renut_first = now;
      if (!ok || !image.width || !image.height || (keep_every > 1 && (now - renut_first) % keep_every)) continue;
    } else {
      // One capture per kept game frame, a few ms after its Swap so the renderer has presented it.
      now = g_banjo_game_frames.load(std::memory_order_relaxed);
      if (now == last || (written && now - last < every)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (written) skipped_flips += now - last - every;
      last = now;
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      if (!grab || !grab(image) || !image.width || !image.height) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
    }
    if (width && (image.width != width || image.height != height)) {
      if (resampled_frames++ == 0)
        REXLOG_INFO("[capture] the frame size changed ({}x{} -> {}x{}): frames are resampled to {}x{}", width, height,
                    image.width, image.height, width, height);
      rex::ui::RawImage resized;
      Resample(image, width, height, resized);
      image = std::move(resized);
    }
    width = image.width;
    height = image.height;
    const uint32_t w = width / step, h = height / step;
    frame.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* row = image.data.data() + size_t(y) * step * image.stride;
      uint8_t* out = frame.data() + size_t(y) * w * 4;
      if (step == 1) {
        std::memcpy(out, row, size_t(w) * 4);
      } else {
        for (uint32_t x = 0; x < w; ++x) std::memcpy(out + size_t(x) * 4, row + size_t(x) * step * 4, 4);
      }
    }
    raw.write(reinterpret_cast<const char*>(frame.data()), std::streamsize(frame.size()));
    {
      // Full resolution, RGB only, the wall-clock regions zeroed (in 1280x720 terms; scaled to the image).
      full.resize(size_t(width) * height * 3);
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = image.data.data() + size_t(y) * image.stride;
        uint8_t* out = full.data() + size_t(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
          out[3 * x] = row[4 * x];
          out[3 * x + 1] = row[4 * x + 1];
          out[3 * x + 2] = row[4 * x + 2];
        }
      }
      const auto zero = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        x0 = x0 * width / 1280;
        x1 = std::min(width, x1 * width / 1280);
        y0 = y0 * height / 720;
        y1 = std::min(height, y1 * height / 720);
        for (uint32_t y = y0; y < y1; ++y)
          if (x1 > x0) std::memset(full.data() + (size_t(y) * width + x0) * 3, 0, size_t(x1 - x0) * 3);
      };
      zero(430, 390, 850, 470);  // "Press START" (blinks with wall-clock time)
      zero(680, 608, 736, 672);  // the loading screen's spinner
      const uint64_t h = XXH3_64bits(full.data(), full.size());
      hash_file.write(reinterpret_cast<const char*>(&h), sizeof(h));
    }
    label = renut_lockstep ? now : (g_banjo_game_frames.load(std::memory_order_relaxed) == now ? now : UINT64_MAX);
    frames_file.write(reinterpret_cast<const char*>(&label), sizeof(label));
    ++written;
    if (written % 600 == 0) REXLOG_INFO("[capture] {} / {} frames", written, frame_count);
  }
  if (renut_lockstep) {
    g_renut_lockstep.store(false, std::memory_order_release);
    std::lock_guard lock(g_renut_mutex);
    g_renut_full = false;
    g_renut_cv.notify_all();
  }
  raw.close();
  if (!written) {
    REXLOG_WARN("[capture] no frame recorded (no guest output)");
    return;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const double fps = seconds > 0 ? double(written) / seconds : 60.0;
  std::ofstream(folder / (name + ".txt"), std::ios::binary)
      << width / step << ' ' << height / step << ' ' << fps << ' ' << written << '\n';
  REXLOG_INFO("[capture] video {}: {} frames of {}x{} in {:.2f} s ({:.1f} fps, {} frames skipped by the recorder) -> {}",
              name, written, width / step, height / step, seconds, fps, skipped_flips,
              rex::path_to_utf8(folder / (name + ".rgba")));
}

}  // namespace

void StartWithSource(std::function<bool(rex::ui::RawImage&)> grab) {
  if (REXCVAR_GET(sa2_video_s) <= 0 || g_thread.joinable()) {
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_stop = false;
  }
  g_thread = std::thread(Record, std::move(grab), int(REXCVAR_GET(sa2_video_s)), int(REXCVAR_GET(sa2_video_frames)));
}

bool RenutLockstepOn() { return g_renut_lockstep.load(std::memory_order_acquire); }

void RenutLockstepFrame(uint64_t game_frame, const rex::ui::RawImage* image) {
  std::unique_lock lock(g_renut_mutex);
  // The previous frame first (the recorder takes one at a time), then this one, then wait until it is taken.
  const auto taken = [] { return !g_renut_full || !g_renut_lockstep.load(std::memory_order_acquire); };
  g_renut_cv.wait_for(lock, std::chrono::seconds(5), taken);
  if (!g_renut_lockstep.load(std::memory_order_acquire)) return;
  g_renut_ok = image != nullptr;
  if (image) g_renut_image = *image;
  g_renut_label = game_frame;
  g_renut_full = true;
  g_renut_cv.notify_all();
  g_renut_cv.wait_for(lock, std::chrono::seconds(5), taken);
}

void Stop() {
  g_renut_lockstep.store(false, std::memory_order_release);
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  g_renut_cv.notify_all();
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

}  // namespace sa2::capture
