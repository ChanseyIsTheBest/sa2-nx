/**
 * @file        audio/downmix.cpp
 * @brief       Output-stage mix parameters: 5.1 fold, 5.1 matrix and master gain
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <cstdint>
#include <cstring>
#include <mutex>
#include <rex/cvar.h>

#include <rex/audio/downmix.h>

// blur-nx (2026-11-13): StereoFrameGain (downmix.h).
REXCVAR_DEFINE_DOUBLE(audio_stereo_frame_gain, 1.0, "Audio",
                      "Gain of the stereo fold for a frame whose channels 2-5 are silent (a title mixing stereo into "
                      "the 5.1 frame: Blur with audio_stereo_mix, whose own dynamics leave the folded pair ~2.8 dB "
                      "below the 5.1 mix); 1 = none; read once")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-13, #62 of Blur's docs/cpu-near-exact-50-1113.md; exact): conversion.h's
// front_2_of_6_BE_to_interleaved_2_LE for a frame whose channels 2-5 are silent (the Switch pump).
REXCVAR_DEFINE_BOOL(audio_stereo_direct, true, "Audio",
                    "A frame whose channels 2-5 are all zero (found by audio_stereo_frame_gain's scan) is converted "
                    "from its front pair alone: the same samples. false: the full fold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::audio {
namespace {

// One output device exists, so the mix parameters are process state. The
// reader is the SDL device callback, which runs every 5.33 ms, so a plain
// mutex costs nothing and avoids a torn read across the four weights.
std::mutex g_mutex;
StereoFold g_fold = {};
SurroundMix g_mix = {};
float g_gain = 1.0f;

}  // namespace

void SetStereoFold(const StereoFold& fold) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fold = fold;
}

StereoFold GetStereoFold() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_fold;
}

void SetSurroundMix(const SurroundMix& mix) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mix = mix;
}

SurroundMix GetSurroundMix() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_mix;
}

void SetOutputGain(float linear) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_gain = linear;
}

float GetOutputGain() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_gain;
}

// blur-nx (2026-11-13, #62): rest_silent (optional) set when channels 2-5 are all +-0, found by the scan this already
// makes (only with audio_stereo_frame_gain other than 1: Blur's); the caller may then take the front pair alone.
float StereoFrameGain(const float* frame_be, size_t ch_samples, bool* rest_silent) {
  static const float gain = float(REXCVAR_GET(audio_stereo_frame_gain));
  if (rest_silent) {
    *rest_silent = false;
  }
  if (gain == 1.0f) {
    return 1.0f;
  }
  const auto* words = reinterpret_cast<const uint32_t*>(frame_be + 2 * ch_samples);
  uint32_t any = 0;
  for (size_t i = 0; i < 4 * ch_samples; ++i) {
    any |= words[i];
  }
  // (big-endian floats read as host words: the sign of +-0 is bit 7)
  if (rest_silent) {
    static const bool direct = REXCVAR_GET(audio_stereo_direct);
    *rest_silent = direct && !(any & 0xFFFFFF7Fu);
  }
  return (any & 0xFFFFFF7Fu) ? 1.0f : gain;
}

}  // namespace rex::audio
