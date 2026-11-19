/**
 * @file        audio/switch/switch_audio_system.cpp
 * @brief       Audio output for Nintendo Switch through libnx audout
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/audio/switch/switch_audio_system.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <switch.h>

// banjo-nx (2026-10-27): the system core (core/threading_switch.h; declared here, that header clashes with switch.h).
extern "C" bool RexSwitchMoveCurrentThreadToSystemCore(int priority);
extern "C" void RexSwitchSampleCoreIdle(void);

// Passive counters: the audio thread neither writes reports nor pauses the game.
extern "C" void RexSwitchPerfAdd(unsigned id, u64 value);
extern "C" void RexSwitchPerfMax(unsigned id, u64 value);

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_BOOL(audio_switch_pump, true, "Audio",
                    "Ask the game for one frame every 5.333 ms (187.5 Hz pump, like "
                    "MarathonRecomp-NX) instead of 4 in a row when each audout buffer is consumed, which "
                    "left the game's audio server voice without a packet (robotic audio); "
                    "read when the audio is opened");
// blur-nx (2026-11-13, M-43 of docs/cpu-moonshots-50-1113.md; the user's go-ahead: "M43 is good implement").
REXCVAR_DEFINE_BOOL(audio_switch_pump_pairs, false, "Audio",
                    "With audio_switch_pump: ask the game for two frames back to back every 10.667 ms instead of one "
                    "every 5.333 ms. The title's callback waits for its audio engine to render each frame, so the "
                    "engine renders the second right after the first, its voices' and effects' state still in the "
                    "caches. One more frame of latency (5.3 ms); the game's audio parameters take effect at 10.7 ms "
                    "steps; read when the audio is opened");
// blur-nx (2026-11-16, the 15 ms batch, #196): M-43 widened: 1-4 frames back to back per pump tick (0: as
// audio_switch_pump_pairs says). The engine renders the later frames of a tick with its voices' and effects' state still
// in the caches; each frame of a tick past the first adds 5.3 ms of latency, and the game's audio parameters take effect
// at the tick's steps. The same frames, rendered sooner or later: exact output.
REXCVAR_DEFINE_INT32(audio_switch_pump_frames, 0, "Audio",
                     "With audio_switch_pump: frames asked back to back per tick, 1-4 (0: audio_switch_pump_pairs "
                     "decides, 2 or 1); read when the audio is opened");
REXCVAR_DEFINE_INT32(audio_switch_frames_in_queue, 10, "Audio",
                     "With audio_switch_pump: game frames kept queued before audout "
                     "(cushion; 10 = 53 ms, 6 up to build 105). A longer queue lets the game wait without cutting the "
                     "output, at the cost of latency: masseffect_audio_wait_server_ms waits up to 30 ms; read "
                     "when the audio is opened");

namespace rex::audio::nx {

namespace {

// Guest frames per audout buffer, and buffers kept queued in the device.
// 4 frames are 1024 samples (21.3 ms), the block size MarathonRecomp-NX found
// to be safe on the shared cores; three buffers cap the latency at ~64 ms.
constexpr uint32_t kFramesPerBuffer = 4;
constexpr uint32_t kBufferCount = 3;
constexpr uint32_t kBufferSamples = kFramesPerBuffer * SwitchAudioDriver::kChannelSamples;
constexpr uint32_t kBufferBytes = kBufferSamples * 2 * sizeof(s16);
// audout wants page-aligned sample memory.
constexpr uint32_t kBufferAllocBytes = (kBufferBytes + 0xFFF) & ~0xFFFu;

// Above guest threads (0x3B) so the device is never starved, like the audio
// pump of MarathonRecomp-NX (0x2B).
constexpr int kOutputThreadPriority = 0x2B;
constexpr size_t kOutputThreadStack = 0x10000;
// Bounded so the thread notices shutdown even if the device stops releasing.
constexpr u64 kWaitTimeoutNs = 100'000'000;
// audio_switch_pump: one frame (256 samples) every 5.333 ms with absolute deadlines. With this many
// frames queued or fewer, another one is requested on each tick, and below the minimum one more is
// requested to catch up (the system clock and the audio clock do not match exactly).
constexpr u64 kPumpIntervalNs = 1'000'000'000ull * SwitchAudioDriver::kChannelSamples / 48000;
constexpr size_t kTargetQueuedFrames = 6;
constexpr size_t kMinQueuedFrames = 2;
// mk9-nx (2026-10-21; port 56): pump ticks with the queue above its target before the oldest frame is dropped to make
// room (~0.34 s). Without it, an audout that stops releasing buffers (seen in Ryujinx) stops the game's audio engine
// altogether, and a game that waits for a voice to drain waits forever (mk9-nx's main-menu freeze).
constexpr uint32_t kStalledTicksBeforeDrop = 64;

}  // namespace

/* --- driver ------------------------------------------------------------------ */

SwitchAudioDriver::SwitchAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SwitchAudioDriver::~SwitchAudioDriver() {
  std::lock_guard<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SwitchAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  // Runs inside the guest callback while it holds the global critical region:
  // copy and return, never block on the device.
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[kFrameSamples];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }
  std::memcpy(output_frame, input_frame, kFrameSamples * sizeof(float));
  std::lock_guard<std::mutex> guard(frames_mutex_);
  frames_queued_.push(output_frame);
}

bool SwitchAudioDriver::MixFrameInto(float* stereo_out, const StereoFold& fold, float gain,
                                     bool release) {
  float* frame;
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    if (frames_queued_.empty()) {
      return false;
    }
    frame = frames_queued_.front();
    frames_queued_.pop();
  }
  std::array<float, kChannelSamples * 2> folded;
  bool rest_silent = false;  // blur-nx (2026-11-13, #62)
  const float frame_gain = gain * StereoFrameGain(frame, kChannelSamples, &rest_silent);
  if (rest_silent) {
    conversion::front_2_of_6_BE_to_interleaved_2_LE(folded.data(), frame, kChannelSamples, fold.scale * frame_gain);
  } else {
    conversion::sequential_6_BE_to_interleaved_2_LE(folded.data(), frame, kChannelSamples, fold, frame_gain);
  }
  for (size_t i = 0; i < folded.size(); ++i) {
    stereo_out[i] += folded[i];
  }
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    frames_unused_.push(frame);
  }
  // Exactly one release per frame really consumed: that is what lets the guest
  // render the next one. With audio_switch_pump, PumpTick requests them at a fixed rate.
  if (release) {
    auto released = semaphore_->Release(1, nullptr);
    assert_true(released);
  }
  return true;
}

size_t SwitchAudioDriver::QueuedFrames() {
  std::lock_guard<std::mutex> guard(frames_mutex_);
  return frames_queued_.size();
}

bool SwitchAudioDriver::DropOldestFrame() {
  std::lock_guard<std::mutex> guard(frames_mutex_);
  if (frames_queued_.empty()) {
    return false;
  }
  frames_unused_.push(frames_queued_.front());
  frames_queued_.pop();
  return true;
}

void SwitchAudioDriver::RequestFrame() {
  // The semaphore has a maximum: if the game does not serve the requests, the excess ones are lost.
  semaphore_->Release(1, nullptr);
}

/* --- system ------------------------------------------------------------------ */

struct SwitchAudioSystem::Output {
  Thread thread{};
  bool thread_created = false;
  std::atomic<bool> running{false};
  bool pump = false;  // audio_switch_pump, read when audio is opened
  uint32_t frames_per_tick = 1;  // blur-nx (2026-11-13, M-43): audio_switch_pump_pairs: 2
  uint32_t stalled_ticks = 0;   // port 56 (mk9-nx): PumpTick ticks with the queue above frames_in_queue
  uint64_t dropped_frames = 0;  // port 56 (mk9-nx): frames dropped because audout stopped consuming
  size_t frames_in_queue = kTargetQueuedFrames;  // audio_switch_frames_in_queue, read when audio is opened
  bool audout_initialized = false;
  bool audout_started = false;
  std::array<AudioOutBuffer, kBufferCount> buffers{};
  std::array<s16*, kBufferCount> samples{};
  std::array<float, kBufferSamples * 2> mix{};
};

std::unique_ptr<AudioSystem> SwitchAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<SwitchAudioSystem>(function_dispatcher);
}

SwitchAudioSystem::SwitchAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

SwitchAudioSystem::~SwitchAudioSystem() {
  StopOutput();
}

X_STATUS SwitchAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                         rex::thread::Semaphore* semaphore,
                                         AudioDriver** out_driver) {
  assert_not_null(out_driver);
  // The device opens with the first client and then stays open, playing
  // silence while nobody renders.
  if (!output_ && !StartOutput()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  auto driver = new SwitchAudioDriver(memory_, semaphore);
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    drivers_.push_back(driver);
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void SwitchAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto switch_driver = static_cast<SwitchAudioDriver*>(driver);
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    drivers_.erase(std::remove(drivers_.begin(), drivers_.end(), switch_driver), drivers_.end());
  }
  delete switch_driver;
}

bool SwitchAudioSystem::StartOutput() {
  auto output = std::make_unique<Output>();
  output->pump = REXCVAR_GET(audio_switch_pump);
  output->frames_per_tick = REXCVAR_GET(audio_switch_pump_pairs) ? 2 : 1;  // blur-nx (2026-11-13, M-43)
  if (const int32_t n = REXCVAR_GET(audio_switch_pump_frames); n >= 1) {  // blur-nx (2026-11-16, #196)
    output->frames_per_tick = uint32_t(n > 4 ? 4 : n);
  }
  output->frames_in_queue = static_cast<size_t>(std::clamp(REXCVAR_GET(audio_switch_frames_in_queue), 2, 32));
  REXLOG_INFO("SwitchAudioSystem: 187.5 Hz pump {}{}, {} frames queued", output->pump ? "active" : "off",
              output->frames_per_tick == 2   ? " (two frames every 10.7 ms: audio_switch_pump_pairs)"
              : output->frames_per_tick > 2 ? " (frames back to back per tick: audio_switch_pump_frames)"
                                            : "",
              output->frames_in_queue);

  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutInitialize failed: 0x{:08X}", rc);
    return false;
  }
  output->audout_initialized = true;
  REXLOG_INFO("SwitchAudioSystem: audout {} Hz, {} channels, PCM format {}",
              audoutGetSampleRate(), audoutGetChannelCount(), int(audoutGetPcmFormat()));
  if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 ||
      audoutGetPcmFormat() != PcmFormat_Int16) {
    REXLOG_ERROR("SwitchAudioSystem: unexpected audout format");
    audoutExit();
    return false;
  }

  for (size_t i = 0; i < kBufferCount; ++i) {
    output->samples[i] = static_cast<s16*>(std::aligned_alloc(0x1000, kBufferAllocBytes));
    if (!output->samples[i]) {
      REXLOG_ERROR("SwitchAudioSystem: out of memory for audio buffers");
      output_ = std::move(output);
      StopOutput();
      return false;
    }
    std::memset(output->samples[i], 0, kBufferAllocBytes);
    AudioOutBuffer& buffer = output->buffers[i];
    buffer.next = nullptr;
    buffer.buffer = output->samples[i];
    buffer.buffer_size = kBufferAllocBytes;
    buffer.data_size = kBufferBytes;
    buffer.data_offset = 0;
  }

  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutStartAudioOut failed: 0x{:08X}", rc);
    output_ = std::move(output);
    StopOutput();
    return false;
  }
  output->audout_started = true;

  output_ = std::move(output);
  output_->running = true;
  rc = threadCreate(
      &output_->thread,
      [](void* arg) { static_cast<SwitchAudioSystem*>(arg)->OutputThreadMain(); }, this, nullptr,
      kOutputThreadStack, kOutputThreadPriority, -2);
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: threadCreate failed: 0x{:08X}", rc);
    output_->running = false;
    StopOutput();
    return false;
  }
  output_->thread_created = true;
  rc = threadStart(&output_->thread);
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: threadStart failed: 0x{:08X}", rc);
    output_->running = false;
    StopOutput();
    return false;
  }
  return true;
}

void SwitchAudioSystem::StopOutput() {
  if (!output_) {
    return;
  }
  output_->running = false;
  if (output_->thread_created) {
    threadWaitForExit(&output_->thread);
    threadClose(&output_->thread);
  }
  if (output_->audout_started) {
    audoutStopAudioOut();
  }
  if (output_->audout_initialized) {
    audoutExit();
  }
  for (s16* samples : output_->samples) {
    std::free(samples);
  }
  output_.reset();
}

void SwitchAudioSystem::FillAndAppend(size_t buffer_index) {
  Output& output = *output_;
  output.mix.fill(0.0f);
  // Snapshot once, so a change mid-buffer cannot split it across two mixes.
  const StereoFold fold = GetStereoFold();
  const float gain = GetOutputGain();
  uint32_t frames_mixed = 0;
  uint32_t frames_missing = 0;
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    for (uint32_t frame = 0; frame < kFramesPerBuffer; ++frame) {
      float* out = output.mix.data() + frame * SwitchAudioDriver::kChannelSamples * 2;
      for (SwitchAudioDriver* driver : drivers_) {
        // A client with nothing queued contributes silence and gets no
        // release, exactly as the SDL driver does on underrun.
        if (driver->MixFrameInto(out, fold, gain, !output.pump)) {
          ++frames_mixed;
        } else {
          ++frames_missing;
        }
      }
    }
  }
  s16* samples = output.samples[buffer_index];
  bool pcm_nonzero = false;
  // How much the mix saturates before it is clipped (the crackling in handheld mode). Counting only.
  float pico = 0.0f;
  uint32_t saturated = 0;
  if (REXCVAR_GET(audio_mute)) {
    std::memset(samples, 0, kBufferBytes);
  } else {
    for (size_t i = 0; i < output.mix.size(); ++i) {
      const float value = std::isfinite(output.mix[i]) ? output.mix[i] : 0.0f;
      const float magnitude = std::fabs(value);
      pico = std::max(pico, magnitude);
      saturated += magnitude > 1.0f ? 1 : 0;
      samples[i] = static_cast<s16>(std::lrint(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
      pcm_nonzero |= samples[i] != 0;
    }
  }
  RexSwitchPerfAdd(21, saturated);
  RexSwitchPerfAdd(22, pico >= 0.98f ? 1 : 0);
  RexSwitchPerfMax(23, static_cast<u64>(std::min(pico, 100.0f) * 10000.0f));
  output.buffers[buffer_index].data_size = kBufferBytes;
  output.buffers[buffer_index].data_offset = 0;
  Result rc = audoutAppendAudioOutBuffer(&output.buffers[buffer_index]);
  RexSwitchPerfAdd(24, frames_mixed);
  RexSwitchPerfAdd(25, frames_missing);
  if (R_SUCCEEDED(rc)) {
    RexSwitchPerfAdd(26, pcm_nonzero ? 1 : 0);
    RexSwitchPerfAdd(27, 1);
  }
  // blur-nx (2026-11-12, diagnostic): audout's own count of the samples it has played, beside the buffers appended
  // (counter 27). Blur's console races appended ~12 % more 1,024-sample buffers than 48 kHz plays (52.9 a second
  // against 46.9; the menus at 60 fps 47.1): either the device releases some buffers without playing them, or the
  // count is wrong. Counter 57: samples played in the interval (the output thread is the only caller).
  // renut-ultimate (2026-11-17, port 153): asked every 16th buffer (about 12 times a second), not at every one: an IPC
  // request to audout each time, in every game. The count is audout's running total, so the sum is the same.
  static uint32_t played_ask = 0;
  if ((++played_ask & 15) == 0) {
    static u64 last_played = 0;
    u64 played = 0;
    if (R_SUCCEEDED(audoutGetAudioOutPlayedSampleCount(&played))) {
      if (last_played && played >= last_played) RexSwitchPerfAdd(57, played - last_played);
      last_played = played;
    }
  }
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutAppendAudioOutBuffer failed: 0x{:08X}", rc);
  }
}

void SwitchAudioSystem::PumpTick() {
  std::lock_guard<std::mutex> guard(drivers_mutex_);
  for (SwitchAudioDriver* driver : drivers_) {
    // blur-nx (2026-11-13, M-43): a tick asks for frames_per_tick frames (two every 10.7 ms with
    // audio_switch_pump_pairs), each with the one-frame rule below; those asked this tick are not queued yet.
    const size_t queued_at_tick = driver->QueuedFrames();
    for (uint32_t k = 0; k < output_->frames_per_tick; ++k) {
      const size_t queued = queued_at_tick + k;
      if (queued <= output_->frames_in_queue) {
        driver->RequestFrame();
        output_->stalled_ticks = 0;
      } else if (++output_->stalled_ticks >= kStalledTicksBeforeDrop && driver->DropOldestFrame()) {
        // port 56 (mk9-nx): audout is not consuming; keep the game's audio clock running (see kStalledTicksBeforeDrop).
        driver->RequestFrame();
        if (output_->dropped_frames++ == 0) {
          REXLOG_WARN("SwitchAudioSystem: audout stopped consuming frames; dropping the oldest to keep the game's "
                      "audio running");
        }
      }
    }
    if (queued_at_tick < kMinQueuedFrames) {
      driver->RequestFrame();  // the queue has emptied: one more to catch up
    }
  }
}

void SwitchAudioSystem::OutputThreadMain() {
  Output& output = *output_;
  // banjo-nx (2026-10-27): on the system core when the process has it (threading_switch.h, banjo_system_core), above
  // the Audio Worker and the XMA Decoder there (0x2B; Horizon does not preempt between equal priorities). It also
  // samples that core's idle time for the report.
  const bool on_system_core = RexSwitchMoveCurrentThreadToSystemCore(kOutputThreadPriority - 1);
  for (size_t i = 0; i < kBufferCount; ++i) {
    FillAndAppend(i);
  }
  const u64 interval = armNsToTicks(kPumpIntervalNs * output.frames_per_tick);  // blur-nx (2026-11-13, M-43)
  u64 deadline = armGetSystemTick() + interval;
  while (output.running.load(std::memory_order_relaxed)) {
    if (on_system_core) RexSwitchSampleCoreIdle();  // banjo-nx (2026-10-27): at most every 100 ms
    u64 timeout_ns = kWaitTimeoutNs;
    if (output.pump) {
      const u64 now = armGetSystemTick();
      if (now >= deadline) {
        PumpTick();
        deadline += interval;
        if (now >= deadline) {
          deadline = now + interval;  // more than one tick late: no catch-up burst
        }
        continue;
      }
      timeout_ns = armTicksToNs(deadline - now);  // at most until the next deadline
    }
    AudioOutBuffer* released = nullptr;
    u32 released_count = 0;
    Result rc = audoutWaitPlayFinish(&released, &released_count, timeout_ns);
    // libnx hands back one released buffer per call; drain the rest so none is
    // left out of the rotation.
    uint32_t released_in_wake = 0;  // blur-nx (2026-11-12, diagnostic): counter 58, below
    while (R_SUCCEEDED(rc) && released && released_count) {
      ++released_in_wake;
      const ptrdiff_t index = released - output.buffers.data();
      if (index >= 0 && index < ptrdiff_t(kBufferCount)) {
        FillAndAppend(size_t(index));
      }
      released = nullptr;
      released_count = 0;
      rc = audoutGetReleasedAudioOutBuffer(&released, &released_count);
    }
    // blur-nx (2026-11-12, diagnostic): a wake that found every buffer released: the device had nothing left to play.
    if (released_in_wake >= kBufferCount) RexSwitchPerfAdd(58, 1);
  }
}

}  // namespace rex::audio::nx
