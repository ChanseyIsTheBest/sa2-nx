// sa2-nx-ult (2026-11-21, the user: "rencode ... all the audio to opus in a web installer like culdcept"): Sonic Adventure
// 2's ADX sound (the music, the cutscene voices, the movies' sound: CRI's ADXT 11.35 in the game's own code) taken from
// Opus. NAME.adxo (rex/filesystem/adx_opus.h) shows the game its file with each ADX block a tag (the stream, the
// block's index, the channel); this hook takes the decode step's blocks and, when they are tags, writes their samples
// from the stream's Opus (decoded and resampled in the SDK, AdxOpusBlockSamples) instead of decoding ADX nibbles.
// culdcept-nx's XMA-from-Opus (its XmaContext::DecodeXmoFrame) is the model; docs/opus-h265.md has the findings.
//
// Found in SA2's code (2026-11-21): sub_828C2948(obj), ADXB's one decode step, reached from the ADX stream decoder
// (sub_828C6D98 <- sub_828C7348): obj +12 the state (1 / 2 pending, 3 done), +16 the blocks decoded, +20 the channels,
// +24 the input (18-byte blocks, a stereo stream's interleaved L R), +28 the blocks (both channels'), +32 / +36 the
// outputs (16-bit big-endian PCM, 32 samples a block), +40 / +44 each channel's history (the last two samples), and
// it calls the mono (sub_828CCD28) or stereo (sub_828CD3E0) block decoder, which stop at an end marker (a scale with
// its high bit set) and return the blocks they decoded.
#include <rex/cvar.h>
#include <rex/filesystem/adx_opus.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <chrono>
#include <cstdint>

REXCVAR_DEFINE_BOOL(sa2_adx_opus, true, "SA2",
                    "The sound the installer made Opus (NAME.adxo) played from the Opus; false: its tags are decoded as "
                    "ADX (noise)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_adx_opus_log, false, "SA2",
                    "An [adxo] line every 10 s while Opus sound plays: the blocks, packets and seeks, and the time in the "
                    "decode step")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_828C2948);  // ADXB: one decode step (obj)

namespace {

uint8_t* Host(uint8_t* base, uint32_t a) { return rex::memory::GuestPtr(base, a); }
uint32_t Ld32(uint8_t* base, uint32_t a) {
  const uint8_t* p = Host(base, a);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void St32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = Host(base, a);
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
  rex::memory::MarkStoreCardsAt(base, a, 4);
}
void St16(uint8_t* p, int16_t v) {
  p[0] = uint8_t(uint16_t(v) >> 8);
  p[1] = uint8_t(v);
}

// The 32 samples of a tagged block, big-endian, at `to`; false when the block is not a tag of a known stream.
bool TaggedBlock(uint8_t* base, uint32_t block_at, uint32_t channel_expected, uint32_t to, int16_t* last2) {
  uint32_t id, block, channel;
  if (!rex::filesystem::ParseAdxOpusTag(Host(base, block_at), &id, &block, &channel) || channel != channel_expected)
    return false;
  int16_t pcm[rex::filesystem::kAdxBlockSamples];
  if (!rex::filesystem::AdxOpusBlockSamples(id, block, channel, pcm)) return false;
  uint8_t* out = Host(base, to);
  for (uint32_t i = 0; i < rex::filesystem::kAdxBlockSamples; ++i) St16(out + 2 * i, pcm[i]);
  rex::memory::MarkStoreCardsAt(base, to, 2 * rex::filesystem::kAdxBlockSamples);
  last2[0] = pcm[rex::filesystem::kAdxBlockSamples - 1];
  last2[1] = pcm[rex::filesystem::kAdxBlockSamples - 2];
  return true;
}

std::atomic<uint64_t> g_steps{0}, g_tagged_blocks{0};
std::atomic<int64_t> g_ns{0};

void LogLine() {
  static std::atomic<int64_t> next{0};
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  int64_t due = next.load(std::memory_order_relaxed);
  if (now < due || !next.compare_exchange_strong(due, now + int64_t(10) * 1000000000)) return;
  const auto c = rex::filesystem::GetAdxOpusCounts();
  const uint64_t steps = g_steps.exchange(0);
  REXLOG_INFO("[adxo] 10 s: {} decode steps from Opus ({:.3f} ms each), {} blocks in all; Opus packets {}, seeks {}, "
              "blocks of a stream no file has open {}",
              steps, steps ? double(g_ns.exchange(0)) / 1e6 / double(steps) : 0.0,
              g_tagged_blocks.load(std::memory_order_relaxed), c.packets, c.seeks, c.unknown);
}

}  // namespace

REX_HOOK_RAW(sub_828C2948) {
  static const bool on = REXCVAR_GET(sa2_adx_opus);
  const uint32_t obj = ctx.r3.u32;
  if (!on || !obj) {
    __imp__sub_828C2948(ctx, base);
    return;
  }
  const uint32_t state = Ld32(base, obj + 12);
  const uint32_t channels = Ld32(base, obj + 20), in = Ld32(base, obj + 24), n = Ld32(base, obj + 28);
  if ((state != 1 && state != 2) || (channels != 1 && channels != 2) || !in || !n) {
    __imp__sub_828C2948(ctx, base);
    return;
  }
  uint32_t id, blk, ch;
  if (!rex::filesystem::ParseAdxOpusTag(Host(base, in), &id, &blk, &ch)) {
    __imp__sub_828C2948(ctx, base);  // ADX as it was
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t out[2] = {Ld32(base, obj + 32), Ld32(base, obj + 36)};
  int16_t last[2][2] = {};
  uint32_t done = 0;  // blocks, both channels'
  const uint32_t frames = n / channels;
  for (uint32_t f = 0; f < frames; ++f) {
    bool ok = true;
    for (uint32_t c = 0; c < channels && ok; ++c)
      ok = TaggedBlock(base, in + (f * channels + c) * rex::filesystem::kAdxBlockBytes, c,
                       out[c] + f * 2 * rex::filesystem::kAdxBlockSamples, last[c]);
    if (!ok) break;  // an end marker, or a block that is not one of ours: the step ends there, as the game's would
    done += channels;
  }
  if (!done) {  // the first block's stream is not open (a stale tag): the game's decoder as it was
    __imp__sub_828C2948(ctx, base);
    return;
  }
  // Each channel's history as the game's decoder leaves it (the last sample, then the one before).
  for (uint32_t c = 0; c < channels; ++c) {
    uint8_t* h = Host(base, obj + 40 + 4 * c);
    St16(h, last[c][0]);
    St16(h + 2, last[c][1]);
    rex::memory::MarkStoreCardsAt(base, obj + 40 + 4 * c, 4);
  }
  St32(base, obj + 16, done);
  St32(base, obj + 12, 3);
  g_tagged_blocks.fetch_add(done, std::memory_order_relaxed);
  g_steps.fetch_add(1, std::memory_order_relaxed);
  g_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                 std::memory_order_relaxed);
  static const bool log = REXCVAR_GET(sa2_adx_opus_log);
  static std::atomic<bool> first{true};
  if (first.exchange(false))
    REXLOG_INFO("[adxo] the first decode step from Opus: stream id {}, block {}, {} channel(s), {} blocks", id, blk,
                channels, done);
  if (log) LogLine();
}
