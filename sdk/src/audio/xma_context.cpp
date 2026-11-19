/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2021 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*
* @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
*/

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/helpers.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/stream.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>

#if defined(__aarch64__) && !REX_ARCH_AMD64
#include <arm_neon.h>
#endif

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

REXCVAR_DEFINE_INT32(audio_dump_xma_s, 0, "Audio",
                     "Diagnostic: seconds of each XMA context that are saved to "
                     "xma_<context>_frames/output_<hz>.wav next to the executable; 0 = none");
REXCVAR_DEFINE_INT32(audio_dump_xma_from_s, 0, "Audio",
                     "Diagnostic: seconds after the first XMA audio before starting to dump");
REXCVAR_DEFINE_INT32(audio_dump_xma_contexts, 12, "Audio",
                     "Diagnostic: maximum number of XMA contexts that are dumped");
REXCVAR_DEFINE_INT32(audio_dump_xma_mb, 32, "Audio",
                     "Diagnostic: maximum MB of accumulated XMA audio not yet written");
REXCVAR_DEFINE_INT32(audio_dump_xma_min_s, 2, "Audio",
                     "Diagnostic: minimum seconds of an XMA sound that are saved when it ends");
// banjo-nx (2026-10-27, #4): the decoded-frame cache (XmaContext::DecodeFrame).
// blur-nx (2026-11-13, M-02 and M-09 of docs/cpu-moonshots-50-1113.md): v2, keyed by (format, previous packet, packet).
REXCVAR_DEFINE_BOOL(banjo_xma_frame_cache, true, "Banjo/Audio",
                    "XMA: a frame met before right after the same previous frame (or as the first after the codec "
                    "opened), in the same format, takes the samples decoded then instead of FFmpeg's decode (exact: "
                    "a frame's samples depend on those alone)");
REXCVAR_DEFINE_BOOL(banjo_xma_frame_cache_verify, false, "Banjo/Debug",
                    "XMA frame cache: decode every frame anyway and compare the cached samples with FFmpeg's; the log "
                    "counts the answers and the differences every 10 s");
// blur-nx (2026-11-13, M-05 of docs/cpu-moonshots-50-1113.md): PrepareDecoder / OpenCodec.
REXCVAR_DEFINE_BOOL(banjo_xma_codec_lazy_open, true, "Banjo/Audio",
                    "XMA, with the frame cache on: a new sound's codec is opened before its first decode, not at its "
                    "first frame (a sound the cache answers whole never opens one; exact: the codec is opened fresh "
                    "before it decodes anything of the sound)");
// blur-nx (2026-11-13, M-01 of docs/cpu-moonshots-50-1113.md): the proof of the one-frame catch-up.
REXCVAR_DEFINE_BOOL(banjo_xma_frame_cache_chain_verify, false, "Banjo/Debug",
                    "Diagnostic (M-01): a second FFmpeg codec per context decodes every frame, the ones the frame cache "
                    "answered too (the full chain, as before 2026-11-13), and every frame the context's own codec decodes "
                    "after its one-frame catch-up, and (blur-nx 2026-11-13, cache v2) every frame the cache answers, is "
                    "compared with it, byte for byte (the 10 s line's 'chain verify')");
// katamari-nx (2026-10-31): the console's constant tone (an XMA context repeating one 128-sample subframe).
REXCVAR_DEFINE_INT32(audio_xma_loop_trace, 0, "Audio",
                     "Diagnostic: log the loop fields of a context each time it jumps back to its loop start "
                     "or decodes the same frame several times in a row (at most this many lines a context; "
                     "0 = off). Also: a sound's input ending, the game re-submitting input or re-validating the "
                     "output, a drained output marked done, a clear");
// katamari-nx (2026-11-01): rc5's whole-buffer loop made the one-shot effects loop forever (the user's console run).
// The trace of runs menu6_end and game6_end: every sound with loop count 255 and no loop end that reached the end of
// its input was released by the game 60-70 ms later (3 on the menus, 16 in the practice room); the game's own loops
// re-submit their buffer. So they are one-shots that play to their end (off is the default since rc6).
REXCVAR_DEFINE_BOOL(audio_xma_whole_buffer_loop, false, "Audio",
                    "XMA: a context with a loop count and no loop end (loop end below bit 32) goes back to its first "
                    "frame past the buffer's last one (rc5: Beautiful Katamari's one-shot effects then loop forever). "
                    "false = it plays to the end of its input, as Xenia's decoder does (no frame starts at such an "
                    "end) and as the game expects");
REXCVAR_DEFINE_BOOL(audio_xma_output_done_when_drained, false, "Audio",
                    "XMA: a pass that finds no input left, nothing pending and the output ring empty (the game took "
                    "every sample) clears output_buffer_valid, as Xenia clears it whenever the ring is empty after a "
                    "pass: the sound is done. false = only a pass that wrote PCM and filled the ring clears it "
                    "(banjo-nx's rule)");
// blur-nx (2026-11-12, CPU batch 3; port 111): 24 filled in Blur's first race minute and answered 26 % of the frames; 256
// answered 65 % (the XMA Decoder 15.2 -> 10.9 ms a frame, 23.84 -> 24.77 fps on the console, 0 differences in the sampled
// verify). It is heap: the heap's room at the race's end was 834 MB with 24 and about 600 MB with 256 (Blur's texture
// reserve is 384 MB). The default stays 24: a game with many short repeated sounds sets more in its toml, carefully
// when it is near its texture limit.
// blur-nx (2026-11-13, #12 of Blur's docs/cpu-near-exact-50-1113.md, the user's choice of near-exact ideas): a node
// matches on its 64-bit key (two CRC32C streams of the packet with the previous packet's mixed in), the format and the
// sizes, without comparing the two packets' bytes. NOT exact: a key collision (about 2^-64 a lookup for compressed
// audio) would answer another frame's samples. The cache's report counts the collisions the byte compare finds.
REXCVAR_DEFINE_BOOL(banjo_xma_frame_cache_hash_only, false, "Banjo/Audio",
                    "XMA frame cache: match nodes on the 64-bit key, format and sizes without comparing the packets' "
                    "bytes (near-exact #12; a key collision would answer another frame's samples)");

REXCVAR_DEFINE_INT32(banjo_xma_frame_cache_mb, 24, "Banjo/Audio",
                     "XMA frame cache: memory for the decoded frames (about 2 KB each); full, the frames least "
                     "answered make room (a clock). A game with many short repeated sounds may want more (Blur's "
                     "first cache: 256, 26 -> 65 % of the frames answered)")
    .range(1, 512);  // blur-nx (2026-11-12; port 120): up to 512, was 256 (Blur filled 256 MB in about 90 s of racing)


namespace {

// Robotic audio diagnostic: 16-bit big-endian PCM of an XMA context, kept in memory until the
// requested seconds are collected, then written as a little-endian WAV in one go.
struct XmaDump {
  uint32_t frequency = 0;
  uint32_t channels = 0;
  std::vector<int16_t> samples;
  bool written = false;
  uint32_t sequence = 0;  // segment number in the file name
};

std::mutex dump_xma_mutex;
std::map<std::pair<uint32_t, int>, XmaDump> dumped_xma;  // (context, 0 frames / 1 output)
uint32_t dumped_files_xma = 0;
uint32_t dumped_sequence_xma = 0;
uint32_t recorded_xma = 0;
size_t accumulated_samples_xma = 0;  // total of the dumps not yet written
// Contexts currently accumulating (at most audio_dump_xma_contexts): with all of them at once
// the memory cap filled up and none got written.
std::set<uint32_t> dumped_contexts_xma;
bool dump_xma_with_clock = false;
std::chrono::steady_clock::time_point dump_xma_start;

// Written on a separate thread: on the Switch SD the file can take a while, and the caller holds
// the context lock on the game's audio thread.
void WriteDumpXmaInThread(uint32_t context, int type, const XmaDump& v) {
  const std::string number = std::to_string(context);
  const auto path = rex::filesystem::GetExecutableFolder() /
                    ("xma_" + std::string(number.size() < 3 ? 3 - number.size() : 0, '0') + number + "_" +
                     std::to_string(v.sequence) + (type ? "_output_" : "_frames_") +
                     std::to_string(v.frequency) + ".wav");
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return;
  }
  const uint32_t bytes = uint32_t(v.samples.size() * 2);
  const auto u32 = [&](uint32_t x) { file.write(reinterpret_cast<const char*>(&x), 4); };
  const auto u16 = [&](uint16_t x) { file.write(reinterpret_cast<const char*>(&x), 2); };
  file.write("RIFF", 4);
  u32(36 + bytes);
  file.write("WAVEfmt ", 8);
  u32(16);
  u16(1);  // PCM
  u16(uint16_t(v.channels));
  u32(v.frequency);
  u32(v.frequency * v.channels * 2);
  u16(uint16_t(v.channels * 2));
  u16(16);
  file.write("data", 4);
  u32(bytes);
  file.write(reinterpret_cast<const char*>(v.samples.data()), bytes);
  REXAPU_INFO("XMA: dump {} of context {}: {} samples at {} Hz, {} channels",
              type ? "of output" : "of frames", context, v.samples.size() / v.channels,
              v.frequency, v.channels);
}

// Dumps are written on a single thread that lives until the process ends: on the Switch SD a file
// can take seconds and the game's audio thread must not wait for that. There is no thread per file:
// on the Switch, std::thread::detach() threw std::system_error (the writer thread had already
// finished) and the game closed. If the thread cannot be created, the file is written immediately.
class DumpedWriter {
 public:
  static void Enqueue(std::function<void()> work) {
    static DumpedWriter* const writer = Create();  // never destroyed
    if (!writer) {
      work();
      return;
    }
    {
      std::lock_guard<std::mutex> latch(writer->mutex_);
      writer->queue_.push_back(std::move(work));
    }
    writer->warning_.notify_one();
  }

 private:
  DumpedWriter() : thread_([this]() { Loop(); }) {}

  static DumpedWriter* Create() {
    try {
      return new DumpedWriter();
    } catch (const std::system_error&) {
      return nullptr;
    }
  }

  void Loop() {
    for (;;) {
      std::function<void()> work;
      {
        std::unique_lock<std::mutex> latch(mutex_);
        warning_.wait(latch, [this]() { return !queue_.empty(); });
        work = std::move(queue_.front());
        queue_.pop_front();
      }
      work();
    }
  }

  std::mutex mutex_;
  std::condition_variable warning_;
  std::deque<std::function<void()>> queue_;
  std::thread thread_;  // last: it starts with the other members already constructed
};

void WriteDumpXma(uint32_t context, int type, XmaDump& v) {
  XmaDump copy;
  copy.frequency = v.frequency;
  copy.channels = v.channels;
  copy.sequence = v.sequence;
  copy.samples = std::move(v.samples);
  DumpedWriter::Enqueue([context, type, copy = std::move(copy)]() {
    WriteDumpXmaInThread(context, type, copy);
  });
}

void DumpXma(uint32_t context, int type, uint32_t frequency, uint32_t channels, const uint8_t* be,
               size_t bytes) {
  const int32_t seconds = REXCVAR_GET(audio_dump_xma_s);
  if (seconds <= 0 || !frequency || !channels) {
    return;
  }
  std::lock_guard<std::mutex> latch(dump_xma_mutex);
  if (!dump_xma_with_clock) {
    dump_xma_with_clock = true;
    dump_xma_start = std::chrono::steady_clock::now();
  }
  if (std::chrono::steady_clock::now() - dump_xma_start <
      std::chrono::seconds(std::max(REXCVAR_GET(audio_dump_xma_from_s), 0))) {
    return;
  }
  if (dumped_files_xma >= 2 * uint32_t(std::max(REXCVAR_GET(audio_dump_xma_contexts), 0))) {
    return;
  }
  if (const auto written = dumped_xma.find({context, type});
      written != dumped_xma.end() && written->second.written) {
    return;
  }
  if (!dumped_contexts_xma.count(context)) {
    if (dumped_contexts_xma.size() >=
        size_t(std::max(REXCVAR_GET(audio_dump_xma_contexts), 0))) {
      return;
    }
    dumped_contexts_xma.insert(context);
  }
  XmaDump& v = dumped_xma[{context, type}];
  if (!v.samples.empty() && (v.frequency != frequency || v.channels != channels)) {
    // A different format in the same context: start over.
    accumulated_samples_xma -= v.samples.size();
    v.samples = std::vector<int16_t>();
  }
  if (v.samples.empty() && type == 0 && recorded_xma < 64) {
    ++recorded_xma;
    REXAPU_INFO("XMA: context {} with audio at {} Hz, {} channels", context, frequency, channels);
  }
  if (v.samples.empty()) {
    v.sequence = ++dumped_sequence_xma;
  }
  v.frequency = frequency;
  v.channels = channels;
  const size_t cap = size_t(std::max(REXCVAR_GET(audio_dump_xma_mb), 1)) * (1024 * 1024 / 2);
  if (accumulated_samples_xma + bytes / 2 > cap) {
    // It does not fit: this context starts over so the dump has no gap.
    accumulated_samples_xma -= v.samples.size();
    v.samples = std::vector<int16_t>();
    return;
  }
  accumulated_samples_xma += bytes / 2;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    v.samples.push_back(int16_t(uint16_t((uint16_t(be[i]) << 8) | be[i + 1])));
  }
  if (v.samples.size() >= size_t(seconds) * frequency * channels) {
    ++dumped_files_xma;
    v.written = true;
    accumulated_samples_xma -= v.samples.size();
    WriteDumpXma(context, type, v);  // takes the samples
    v.samples = std::vector<int16_t>();
    const auto other = dumped_xma.find({context, 1 - type});
    if (other == dumped_xma.end() || other->second.written) {
      dumped_contexts_xma.erase(context);  // it has written its part: frees its slot
    }
  }
}

// Another sound in the context: what was accumulated is no longer continuous with what follows.
void ResetDumpXma(uint32_t context) {
  if (REXCVAR_GET(audio_dump_xma_s) <= 0) {
    return;
  }
  std::lock_guard<std::mutex> latch(dump_xma_mutex);
  for (int type = 0; type < 2; ++type) {
    const auto it = dumped_xma.find({context, type});
    if (it != dumped_xma.end() && !it->second.written) {
      XmaDump& v = it->second;
      accumulated_samples_xma -= v.samples.size();
      // The sound has ended: if it is long enough it is saved as a segment.
      const size_t min = size_t(std::max(REXCVAR_GET(audio_dump_xma_min_s), 1)) *
                            std::max<uint32_t>(v.frequency, 1) * std::max<uint32_t>(v.channels, 1);
      if (v.frequency && v.samples.size() >= min &&
          dumped_files_xma < 2 * uint32_t(std::max(REXCVAR_GET(audio_dump_xma_contexts), 0))) {
        ++dumped_files_xma;
        WriteDumpXma(context, type, v);
      }
      dumped_xma.erase(it);
    }
  }
  dumped_contexts_xma.erase(context);  // the sound has ended: frees its slot
}

}  // namespace

namespace rex::audio {

using stream::BitStream;

const uint32_t XmaContext::kBitsPerPacketHeader;
const uint32_t XmaContext::kOutputMaxSizeBytes;

XmaContext::XmaContext()
    : work_completion_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

XmaContext::~XmaContext() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
  if (chain_context_) {
    avcodec_free_context(&chain_context_);  // blur-nx (2026-11-13): banjo_xma_frame_cache_chain_verify's codec
  }
  if (chain_frame_) {
    av_frame_free(&chain_frame_);
  }
}


// ---------------------------------------------------------------------------
// banjo-nx (2026-10-27, the user's request: #4 of the CPU-gap table, a cache of decoded XMA frames).
// The codec's output for a frame depends on that frame and on what the codec decoded before it since it was opened
// (WMA Pro's overlap-add tail, the previous block's length, the scale factors it may reuse), nothing else: the codec
// is opened per format (rate, channels) and reopened for every new sound (ResetDecoderState). The first cache kept the
// frames given to a codec since it was opened as chains (a tree of nodes from the format), so a sound played again from
// its start was answered without FFmpeg. The codec does not see the frames the cache answered; before the first frame
// the cache cannot answer it decodes the last one, output discarded, so its state is the one the frames give.
// blur-nx (2026-11-13, M-01 of docs/cpu-moonshots-50-1113.md and its audit): one frame of catch-up, not the run. The
// codec's state after a frame depends on that frame's bits alone (FFmpeg's wmaprodec.c: decode_frame resets reuse_sf
// for every channel; the carry is out[spf .. 1.5 spf) of that frame's own IMDCT, which its subframes tile, while
// wmapro_window mixes only a frame's first subframe with the carry, inside [0, spf); xmaframes_decode_packet starts a
// fresh bit buffer every frame (save_bits(.., 0)); untouched channels are zeroed; a channel's first subframe reads
// every scale factor band fresh). So the samples of frame k depend on (the format, frame k-1, frame k), and decoding
// the last answered frame puts the codec where decoding the whole run would. banjo_xma_frame_cache_chain_verify checks
// it: a second codec decodes the full chain (0 differences in ~41,000 frames of a race, cycle 1).
// blur-nx (2026-11-13, M-02 and M-09, cache v2): so the key is (the format, the previous packet, this packet), the
// previous packet none after an open (the fresh codec), instead of the chain. A frame met through another history (a
// sound started at another point, a loop, a context the full chain cache had dropped for the rest of its sound) is
// answered too: cycle 1's census found 92-97.5 % of the chain cache's misses known as such a pair, and 99.7 % of them
// of contexts it had dropped; a race met ~50,000 pairs, where the chains filled 256 MB (119,266 nodes). A node holds
// both packets' bytes (compared in full, with the format and the sizes) and the samples. Each context keeps its own
// copy of its previous packet (the key, and the catch-up's packet), so nothing depends on a node staying: full, the
// cache evicts (a clock over the nodes; an answer marks its node) instead of stopping, and nothing restarts.
// Exact: an answer is the samples FFmpeg gave for the same packet after the same previous packet in the same format,
// and the codec it skips is put where its frames leave it (M-01). banjo_xma_frame_cache_verify checks the answers
// against the decode; banjo_xma_frame_cache_chain_verify checks the answers and the decodes against the full chain.
// ---------------------------------------------------------------------------
namespace {

struct FrameNode {
  uint64_t key = 0;        // its slot's key in the table (EraseKey)
  uint64_t format = 0;     // FormatKey
  uint32_t prev_size = 0;  // the previous packet's bytes (0: none, a fresh codec)
  uint32_t packet_size = 0;
  uint32_t pcm_bytes = 0;
  uint32_t cost = 0;       // its share of FrameCache::bytes; 0: a free node
  uint8_t referenced = 0;  // the clock's mark: answered since the hand last passed it
  std::unique_ptr<uint8_t[]> data;  // the previous packet, the packet, the samples
  const uint8_t* prev() const { return data.get(); }
  const uint8_t* packet() const { return data.get() + prev_size; }
  const uint8_t* pcm() const { return data.get() + prev_size + packet_size; }
};

struct FrameCache {
  std::mutex mutex;
  std::deque<FrameNode> nodes;  // (a deque: no 100,000-node moves when it grows)
  std::vector<uint32_t> free_nodes;
  size_t hand = 0;  // the clock's (EvictOne)
  // blur-nx (2026-11-13, M-04 of docs/cpu-moonshots-50-1113.md, the audit's rework): the key table as open addressing
  // (a power-of-two array of (key, node index + 1) slots, linear probing, at most half full), where an unordered_map of
  // vectors took a divide for the bucket and three dependent loads before the node. FindNode compares the format, the
  // sizes and both packets' bytes: the key only finds the candidates.
  struct KeySlot {
    uint64_t key = 0;
    uint32_t index_plus1 = 0;  // 0: empty
  };
  std::vector<KeySlot> table = std::vector<KeySlot>(size_t(1) << 16);
  size_t table_used = 0;
  size_t bytes = 0;
  size_t live = 0;  // nodes
  // The 10 s counts (the XMA decoder's report).
  uint64_t answered = 0, decoded = 0, caught_up = 0, unkeyed = 0, verified = 0, differ = 0, evicted = 0;
  // blur-nx (2026-11-13, M-05 lazy open): new sounds (under the lock), and codecs really opened (OpenCodec, which runs
  // outside it)
  uint64_t fresh = 0;
  std::atomic<uint64_t> opens{0};
  uint64_t chain_verified = 0, chain_differ = 0;  // blur-nx (2026-11-13): banjo_xma_frame_cache_chain_verify
  uint64_t collisions = 0;  // blur-nx (2026-11-13, #12's census): key, format and sizes equal, bytes not
};
FrameCache& Cache() {
  static FrameCache c;
  return c;
}

uint64_t FormatKey(int sample_rate, bool stereo) { return uint64_t(uint32_t(sample_rate)) << 1 | uint64_t(stereo); }

// A packet's hash, from a seed (the format): for the key only (FindNode compares the bytes).
// banjo-nx (2026-10-27, ARM pass): the A57's CRC32C instructions, 8 bytes each in two independent streams (the
// instruction's latency is 3 cycles; two chains keep it busy), where a multiply-xor a word was a 5-cycle chain: a
// 1-2 KB packet hashed at about a third of the cost. Other targets: the multiply-xor form.
uint64_t PacketKey(uint64_t seed, const uint8_t* data, size_t size) {
#if defined(__ARM_FEATURE_CRC32)
  uint32_t a = uint32_t(seed) ^ uint32_t(size), b = uint32_t(seed >> 32);
  size_t i = 0;
  for (; i + 16 <= size; i += 16) {
    uint64_t v0, v1;
    std::memcpy(&v0, data + i, 8);
    std::memcpy(&v1, data + i + 8, 8);
    a = __builtin_aarch64_crc32cx(a, v0);
    b = __builtin_aarch64_crc32cx(b, v1);
  }
  for (; i < size; ++i) a = __builtin_aarch64_crc32cb(a, data[i]);
  return (uint64_t(b) << 32 | a) ^ seed * 0x9E3779B97F4A7C15ull;
#else
  uint64_t h = seed * 0x9E3779B97F4A7C15ull ^ size;
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t v;
    std::memcpy(&v, data + i, 8);
    h = (h ^ v) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
  }
  for (; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
  return h ^ (h >> 32);
#endif
}

// The key of (previous packet, packet): the packet's hash with the previous one's mixed in (0: none, a fresh codec).
uint64_t PairKey(uint64_t prev_hash, uint64_t hash) {
  return hash ^ (prev_hash * 0xC2B2AE3D27D4EB4Full + 0x165667B19E3779F9ull);
}

// The key table's first slot for a key (the keys are CRC / multiply-xor mixes: their low bits are spread).
size_t SlotOf(const FrameCache& c, uint64_t key) { return size_t(key ^ (key >> 32)) & (c.table.size() - 1); }

// One more (key, node) in the key table, doubled when it would pass half full. Under the cache's lock.
void InsertKey(FrameCache& c, uint64_t key, uint32_t index) {
  if ((c.table_used + 1) * 2 > c.table.size()) {
    std::vector<FrameCache::KeySlot> old(c.table.size() * 2);
    old.swap(c.table);
    for (const FrameCache::KeySlot& s : old) {
      if (!s.index_plus1) continue;
      size_t i = SlotOf(c, s.key);
      while (c.table[i].index_plus1) i = (i + 1) & (c.table.size() - 1);
      c.table[i] = s;
    }
  }
  size_t i = SlotOf(c, key);
  while (c.table[i].index_plus1) i = (i + 1) & (c.table.size() - 1);
  c.table[i].key = key;
  c.table[i].index_plus1 = index + 1;
  ++c.table_used;
}

// Node 'index' out of the key table (its key's slot run). Backward-shift deletion: each later slot of the run that
// may move back (its first slot is not cyclically in (hole, slot]) fills the hole, so every key stays reachable from
// its first slot without tombstones. Under the cache's lock.
void EraseKey(FrameCache& c, uint64_t key, uint32_t index) {
  const size_t mask = c.table.size() - 1;
  size_t i = SlotOf(c, key);
  for (;; i = (i + 1) & mask) {
    if (!c.table[i].index_plus1) return;  // (not in the table: cannot happen)
    if (c.table[i].index_plus1 == index + 1) break;
  }
  for (size_t j = i;;) {
    j = (j + 1) & mask;
    if (!c.table[j].index_plus1) break;
    const size_t home = SlotOf(c, c.table[j].key);
    const bool stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);
    if (stays) continue;
    c.table[i] = c.table[j];
    i = j;
  }
  c.table[i] = FrameCache::KeySlot{};
  --c.table_used;
}

// The node of this (format, previous packet, packet), or null. Under the cache's lock.
FrameNode* FindNode(FrameCache& c, uint64_t key, uint64_t format, const uint8_t* prev, uint32_t prev_size,
                    const uint8_t* data, uint32_t size, uint32_t pcm_bytes) {
  for (size_t i = SlotOf(c, key); c.table[i].index_plus1; i = (i + 1) & (c.table.size() - 1)) {
    if (c.table[i].key != key) continue;
    FrameNode& n = c.nodes[c.table[i].index_plus1 - 1];
    if (n.format != format || n.prev_size != prev_size || n.packet_size != size || n.pcm_bytes != pcm_bytes) continue;
    if (REXCVAR_GET(banjo_xma_frame_cache_hash_only)) return &n;  // blur-nx (2026-11-13, #12)
    if (std::memcmp(n.packet(), data, size) == 0 && std::memcmp(n.prev(), prev, prev_size) == 0) return &n;
    ++c.collisions;  // (#12's census)
  }
  return nullptr;
}

// blur-nx (2026-11-13, M-09): room for a node when the cache is full. The clock: from the hand, the first node not
// answered since the hand last passed it leaves (those passed lose their mark), so one turn at most clears every mark.
// A freed node is reused next, just behind the hand: a new node has a whole turn to be answered. False: no node left.
// Under the cache's lock.
bool EvictOne(FrameCache& c) {
  if (!c.live) return false;
  for (;;) {
    if (c.hand >= c.nodes.size()) c.hand = 0;
    const uint32_t index = uint32_t(c.hand++);
    FrameNode& n = c.nodes[index];
    if (!n.cost) continue;
    if (n.referenced) {
      n.referenced = 0;
      continue;
    }
    EraseKey(c, n.key, index);
    c.bytes -= n.cost;
    n.cost = 0;
    n.data.reset();
    c.free_nodes.push_back(index);
    --c.live;
    ++c.evicted;
    return true;
  }
}

// A new node (room made first: EvictOne). Under the cache's lock.
void AddNode(FrameCache& c, uint64_t key, uint64_t format, const uint8_t* prev, uint32_t prev_size, const uint8_t* data,
             uint32_t size, const uint8_t* pcm, uint32_t pcm_bytes) {
  const size_t need = size_t(prev_size) + size + pcm_bytes;
  const size_t cost = sizeof(FrameNode) + need;
  const size_t limit = size_t(REXCVAR_GET(banjo_xma_frame_cache_mb)) << 20;
  if (cost > limit) return;
  while (c.bytes + cost > limit && EvictOne(c)) {
  }
  uint32_t index;
  if (!c.free_nodes.empty()) {
    index = c.free_nodes.back();
    c.free_nodes.pop_back();
  } else {
    index = uint32_t(c.nodes.size());
    c.nodes.emplace_back();
  }
  FrameNode& n = c.nodes[index];
  n.key = key;
  n.format = format;
  n.prev_size = prev_size;
  n.packet_size = size;
  n.pcm_bytes = pcm_bytes;
  n.cost = uint32_t(cost);
  n.referenced = 0;
  n.data.reset(new uint8_t[need]);
  std::memcpy(n.data.get(), prev, prev_size);
  std::memcpy(n.data.get() + prev_size, data, size);
  std::memcpy(n.data.get() + prev_size + size, pcm, pcm_bytes);
  InsertKey(c, key, index);
  c.bytes += cost;
  ++c.live;
}

}  // namespace

static std::string XmaFrameCacheReport() {
  FrameCache& c = Cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  if (!c.answered && !c.decoded) return {};
  std::string out = fmt::format(
      "XMA frame cache (banjo_xma_frame_cache): {} frames answered, {} decoded ({:.1f} % answered), {} decoded again "
      "to catch the codec up (one a run), {} decoded unkeyed (after an error); verify {} checked, {} differ; chain "
      "verify {} checked, {} differ; {} nodes, {:.1f} MB, {} evicted; {} new sounds, {} codecs opened; key collisions "
      "{} (cumulative{})",
      c.answered, c.decoded, 100.0 * double(c.answered) / double(c.answered + c.decoded), c.caught_up, c.unkeyed,
      c.verified, c.differ, c.chain_verified, c.chain_differ, c.live, double(c.bytes) / 1048576.0, c.evicted,
      c.fresh, c.opens.exchange(0, std::memory_order_relaxed), c.collisions,
      REXCVAR_GET(banjo_xma_frame_cache_hash_only) ? "; hash only: not compared" : "");
  c.answered = c.decoded = c.caught_up = c.unkeyed = c.verified = c.differ = c.chain_verified = c.chain_differ = 0;
  c.evicted = c.fresh = 0;
  return out;
}

// blur-nx (2026-11-13, cache v2): this frame as the context's previous one (the next frame's key; the catch-up's
// packet): its bytes and FFmpeg's zero padding past them, as the decoder was given them (ProcessFrame's xma_frame_).
void XmaContext::CacheKeepPrev(const uint8_t* data, uint32_t size, uint64_t hash) {
  static_assert(sizeof(cache_prev_packet_) >= sizeof(xma_frame_) + AV_INPUT_BUFFER_PADDING_SIZE);
  std::memcpy(cache_prev_packet_.data(), data, size);
  std::memset(cache_prev_packet_.data() + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  cache_prev_size_ = size;
  cache_prev_hash_ = hash;
  cache_prev_ = CachePrev::kKnown;
}

// The frame in av_packet_ (PreparePacket) decoded into raw_frame_, from the cache when it can answer. 'prepared' is
// PrepareDecoder's result (1: the codec was opened again, fresh).
bool XmaContext::DecodeFrame(int prepared, int sample_rate, bool stereo) {
  static const bool on = REXCVAR_GET(banjo_xma_frame_cache);
  static const bool verify = REXCVAR_GET(banjo_xma_frame_cache_verify);
  static const bool chain_verify = on && REXCVAR_GET(banjo_xma_frame_cache_chain_verify);
  if (prepared == 1) {
    // A fresh codec: this frame's samples depend on the format and its own packet; a frame it was not given no
    // longer matters.
    cache_prev_ = CachePrev::kFresh;
    cache_behind_ = false;
  }
  auto decode = [&]() {
    if (!OpenCodec()) {
      codec_open_failed_ = true;  // (ProcessFrame: error_status 4, as PrepareDecoder's failure was)
      return false;
    }
    if (!DecodePacket(av_context_, av_packet_, av_frame_)) return false;
    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data), stereo, raw_frame_.data());
    return true;
  };
  if (!on) return decode();
  // banjo_xma_frame_cache_chain_verify: the second codec, opened when the first one is (the same fresh chain), decodes
  // every frame, answered or not.
  bool chain_ok = false;
  if (chain_verify) {
    if (prepared == 1 || !chain_context_) {
      if (chain_context_) avcodec_free_context(&chain_context_);
      if (!chain_frame_) chain_frame_ = av_frame_alloc();
      chain_context_ = avcodec_alloc_context3(av_codec_);
      chain_context_->sample_rate = codec_rate_;
      chain_context_->channels = codec_channels_;
      chain_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
      if (avcodec_open2(chain_context_, av_codec_, NULL) < 0) avcodec_free_context(&chain_context_);
    }
    if (chain_context_ && chain_frame_ && DecodePacket(chain_context_, av_packet_, chain_frame_)) {
      ConvertFrame(reinterpret_cast<const uint8_t**>(&chain_frame_->data), stereo, chain_raw_.data());
      chain_ok = true;
    }
  }
  // blur-nx (2026-11-13, M-37): the report's clock read every 64 frames of a thread, not every frame.
  if (static thread_local uint32_t frames = 0; (++frames & 63) == 0) {
    static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(10)) {
      last = now;
      if (const std::string r = XmaFrameCacheReport(); !r.empty()) REXAPU_INFO("{}", r);
    }
  }
  const uint32_t pcm_bytes = kBytesPerFrameChannel << uint32_t(stereo);
  const uint64_t format = FormatKey(sample_rate, stereo);
  uint8_t* const data = av_packet_->data;
  const int size = av_packet_->size;
  // The key, outside the lock: this packet's hash with the previous one's. Unkeyed after an error (kUnknown).
  const bool keyed = cache_prev_ != CachePrev::kUnknown;
  const uint32_t prev_size = cache_prev_ == CachePrev::kKnown ? cache_prev_size_ : 0;
  const uint64_t hash = keyed ? PacketKey(format, data, size_t(size)) : 0;
  const uint64_t key = PairKey(prev_size ? cache_prev_hash_ : 0, hash);
  FrameCache& c = Cache();
  bool verify_hit = false;
  std::array<uint8_t, kBytesPerFrameChannel * 2> verify_pcm;  // (verify mode: the node's samples)
  if (keyed) {
    std::lock_guard<std::mutex> lock(c.mutex);
    if (FrameNode* hit = FindNode(c, key, format, cache_prev_packet_.data(), prev_size, data, uint32_t(size),
                                  pcm_bytes)) {
      if (verify) {
        std::memcpy(verify_pcm.data(), hit->pcm(), pcm_bytes);
        verify_hit = true;
      } else {
        std::memcpy(raw_frame_.data(), hit->pcm(), pcm_bytes);
        hit->referenced = 1;
        ++c.answered;
        if (prepared == 1) ++c.fresh;
        if (chain_ok) {
          ++c.chain_verified;
          if (std::memcmp(raw_frame_.data(), chain_raw_.data(), pcm_bytes) != 0) {
            ++c.chain_differ;
            static uint32_t logged = 0;
            if (logged++ < 16)
              REXAPU_ERROR("banjo_xma_frame_cache_chain_verify: context {}: a frame the cache answered differs from "
                           "the full chain's", id_);
          }
        }
        // The codec does not decode it: it is behind by this frame (decoded before the next one the cache cannot
        // answer).
        CacheKeepPrev(data, uint32_t(size), hash);
        cache_behind_ = true;
        return true;
      }
    }
  }
  bool caught_up = false;
  if (cache_behind_) {
    // The codec catches up with the last frame the cache answered (its samples are already out; the state after it
    // depends on it alone, above): the context's copy of its packet, output discarded.
    av_packet_->data = cache_prev_packet_.data();
    av_packet_->size = int(cache_prev_size_);
    const bool ok = OpenCodec() && DecodePacket(av_context_, av_packet_, av_frame_);
    av_packet_->data = data;
    av_packet_->size = size;
    cache_behind_ = false;
    caught_up = true;
    if (!ok) cache_prev_ = CachePrev::kUnknown;  // (it was decoded before: then this frame is not added)
  }
  const bool ok = decode();
  if (!ok) cache_prev_ = CachePrev::kUnknown;  // an error: the codec's state from here is not one a key describes
  const bool add = ok && keyed && cache_prev_ != CachePrev::kUnknown && !verify_hit;
  {
    std::lock_guard<std::mutex> lock(c.mutex);
    if (caught_up) ++c.caught_up;
    if (prepared == 1) ++c.fresh;
    if (!ok) return false;
    ++c.decoded;
    if (!keyed) ++c.unkeyed;
    if (chain_ok) {
      ++c.chain_verified;
      if (std::memcmp(raw_frame_.data(), chain_raw_.data(), pcm_bytes) != 0) {
        ++c.chain_differ;
        static uint32_t logged = 0;
        if (logged++ < 16)
          REXAPU_ERROR("banjo_xma_frame_cache_chain_verify: context {}: a frame after the one-frame catch-up differs "
                       "from the full chain's", id_);
      }
    }
    if (verify_hit) {
      // verify mode: the decoded samples against the node's
      ++c.verified;
      if (std::memcmp(raw_frame_.data(), verify_pcm.data(), pcm_bytes) != 0) {
        ++c.differ;
        static uint32_t logged = 0;
        if (logged++ < 16)
          REXAPU_ERROR("banjo_xma_frame_cache_verify: context {}: a cached frame differs from FFmpeg's decode", id_);
      }
    }
    if (add) AddNode(c, key, format, cache_prev_packet_.data(), prev_size, data, uint32_t(size), raw_frame_.data(),
                     pcm_bytes);
  }
  if (cache_prev_ != CachePrev::kUnknown) CacheKeepPrev(data, uint32_t(size), hash);
  return true;
}

int XmaContext::Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REXAPU_ERROR("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

/*
 * Fork addition: silence watchdog, measurement only.
 *
 * In one recording, after braking in a corner, the engine acceleration sound takes ~2.5 s to come
 * back, coinciding with a zone load. If the voice that goes quiet is XMA, it shows here: every time
 * the game enables the context with an output buffer pending and the pass produces no PCM, a silence
 * starts (or continues); when it produces again, if the silence lasted 50 ms or more, a log line
 * gives the context, how long it lasted, how many empty passes there were and why (1 only flush
 * without subframes, 2 no input, 3 no progress, 4 error). Contexts that never sound again (end of a
 * sound) write nothing. It does not change anything that is decoded. At most 200 lines per run.
 */
void XmaContext::NoteProduction(bool produced, uint8_t reason, const XMA_CONTEXT_DATA& data) {
  static std::atomic<uint32_t> lines{0};
  // blur-nx (2026-11-13, M-37): the clock only where a silence starts or ends (it was read every pass).
  if (!produced) {
    if (!in_silence_) {
      in_silence_ = true;
      silence_from_ = std::chrono::steady_clock::now();
      passed_silence_ = 0;
    }
    ++passed_silence_;
    silence_reason_ = reason;
    return;
  }
  if (!in_silence_) {
    return;
  }
  in_silence_ = false;
  const auto now = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(now - silence_from_).count();
  if (ms >= 50.0 && lines.fetch_add(1, std::memory_order_relaxed) < 200) {
    REXLOG_INFO("[xma] silence: context {} spent {:.0f} ms without producing and plays again ({} empty passes, "
                "last reason {}: 1 flush only, 2 no input, 3 no progress, 4 error; inputs {}{}, current buffer {}, "
                "loops {})",
                id_, ms, passed_silence_, silence_reason_, uint32_t(data.input_buffer_0_valid),
                uint32_t(data.input_buffer_1_valid), uint32_t(data.current_buffer), uint32_t(data.loop_count));
  }
}

// blur-nx (2026-11-11, CPU batch 2): the bitmap of the enabled contexts (context.h). The flag is stored first; the bit
// after (both seq_cst since batch 4, below): a sweep that sees the bit sees the flag. A sweep that does not see a bit
// set during it misses that context this time, as the walk of the flags did when the flag was set behind it; the
// kick that enabled it signals the worker, which looks again. Work() still checks both flags itself.
namespace {
constexpr uint32_t kEnabledWords = 10;  // 320 contexts
std::atomic<uint32_t> g_enabled_bits[kEnabledWords];
}  // namespace

uint32_t XmaEnabledWord(uint32_t word) {
  return word < kEnabledWords ? g_enabled_bits[word].load(std::memory_order_acquire) : 0u;
}

// blur-nx (2026-11-12, CPU batch 4): Enable() no longer takes the context's lock (it waited for a whole decode), so a
// kick can now meet a clear: the flag and its bit are written with seq_cst, and a clear looks at the flag again after
// clearing the bit, setting the bit back when a kick set the flag in between (in seq_cst's single order: the kick's
// bit either comes after the clear's, or before it, and then the clear's second look sees the kick's flag). The flag's
// final value is the last write's, as before; a bit set for a flag that is false costs one early return in Work.
void XmaContext::set_is_enabled(bool is_enabled) {
  is_enabled_.store(is_enabled, std::memory_order_seq_cst);
  if (is_enabled)
    SetEnabledBit();
  else
    ClearEnabledBit();
}

void XmaContext::SetEnabledBit() {
  if (id_ < kEnabledWords * 32) g_enabled_bits[id_ >> 5].fetch_or(1u << (id_ & 31), std::memory_order_seq_cst);
}

void XmaContext::ClearEnabledBit() {
  if (id_ >= kEnabledWords * 32) return;
  g_enabled_bits[id_ >> 5].fetch_and(~(1u << (id_ & 31)), std::memory_order_seq_cst);
  if (is_enabled_.load(std::memory_order_seq_cst)) SetEnabledBit();  // a kick in between (see above)
}

bool XmaContext::Work() {
  if (!is_allocated() || !is_enabled()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(lock_);
  // The immediate work and the XMA thread may have seen the same notification.
  // Only one must consume it; Disable/Release also use this lock.
  // blur-nx (2026-11-12, CPU batch 4): the kick is taken with one exchange, not a load and a store. Enable() sets the
  // flag without this lock now: a kick between a load and a store would be cleared unseen, and this decode, which reads
  // the context below, would not be ordered after the game's writes before that kick. The exchange reads the latest
  // flag: a kick before it is synchronised with (its writes are seen here), a kick after it stays set for the next
  // pass, as a kick that waited for the lock did.
  if (!is_allocated()) {
    return false;
  }
  if (!is_enabled_.exchange(false, std::memory_order_seq_cst)) {
    return false;
  }
  ClearEnabledBit();

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  // katamari-nx (2026-11-01): audio_xma_loop_trace, what the game changed since the last pass (trace only).
  if (trace_stored_known_ && REXCVAR_GET(audio_xma_loop_trace) > 0) {
    const uint8_t inputs = uint8_t(data.input_buffer_0_valid | (data.input_buffer_1_valid << 1));
    if ((inputs & ~trace_stored_inputs_) && TraceOn()) {
      REXAPU_INFO("XMA trace: context {} input re-submitted: valid {}{} (left {}{}), buffer {}, read bit {}, packets "
                  "{}/{}, loop start {} end {} count {}, output valid {}",
                  id(), uint32_t(data.input_buffer_0_valid), uint32_t(data.input_buffer_1_valid),
                  trace_stored_inputs_ & 1, trace_stored_inputs_ >> 1, uint32_t(data.current_buffer),
                  uint32_t(data.input_buffer_read_offset), uint32_t(data.input_buffer_0_packet_count),
                  uint32_t(data.input_buffer_1_packet_count), uint32_t(data.loop_start), uint32_t(data.loop_end),
                  uint32_t(data.loop_count), uint32_t(data.output_buffer_valid));
    }
    if (data.output_buffer_valid && !trace_stored_output_ && TraceOn()) {
      REXAPU_INFO("XMA trace: context {} output re-validated (read {} write {} of {} blocks), inputs {}{}", id(),
                  uint32_t(data.output_buffer_read_offset), uint32_t(data.output_buffer_write_offset),
                  uint32_t(data.output_buffer_block_count), uint32_t(data.input_buffer_0_valid),
                  uint32_t(data.input_buffer_1_valid));
    }
  }

  if (!data.output_buffer_valid) {
    NoteStored(context_ptr);
    return true;
  }

  // Avoid modulo by zero and accesses outside the guest buffer.
  if (!data.output_buffer_block_count ||
      data.output_buffer_read_offset >= data.output_buffer_block_count ||
      data.output_buffer_write_offset >= data.output_buffer_block_count) {
    data.error_status = 4;
    data.output_buffer_valid = 0;
    StoreContextMerged(data, initial_data, context_ptr);
    NoteStored(context_ptr);
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);
  uint32_t output_bytes_written = 0;

  // Consume-only context: no input, just drain remaining subframes.
  if (data.IsConsumeOnlyContext()) {
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    // Flush all the PCM that fits, even if the packet size was set to zero.
    // Consume returns zero when it has to wait for free space.
    while (current_frame_remaining_subframes_) {
      const uint32_t written = Consume(&output_rb, &data);
      if (!written) {
        break;
      }
      output_bytes_written += written;
    }
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
    if (output_bytes_written && output_rb.empty()) {
      data.output_buffer_valid = 0;
    }
    StoreContextMerged(data, initial_data, context_ptr);
    NoteStored(context_ptr);
    NoteProduction(output_bytes_written != 0, 1, data);  // Measurement only
    return true;
  }

  // Minimum free blocks needed before attempting a decode.
  // Use subframe_decode_count (clamped to 1) instead of full frame size.
  const uint32_t effective_sdc = std::max(static_cast<uint32_t>(1), data.subframe_decode_count);
  const int32_t minimum_subframe_decode_count =
      static_cast<int32_t>(effective_sdc) + data.output_buffer_padding;

  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    StoreContextMerged(data, initial_data, context_ptr);
    NoteStored(context_ptr);
    NoteProduction(true, 0, data);  // output full, not silence
    return true;
  }

  uint8_t silence_reason = 0;  // Measurement only
  while (remaining_subframe_blocks_in_output_buffer_ >= minimum_subframe_decode_count) {
    const uint32_t previous_offset = data.input_buffer_read_offset;
    const uint32_t previous_buffer = data.current_buffer;
    const auto previous_valid = data.input_buffer_0_valid | (data.input_buffer_1_valid << 1);
    const uint32_t previous_loop_count = data.loop_count;
    const uint8_t previous_subframes = current_frame_remaining_subframes_;
    Decode(&data);
    const uint32_t written = Consume(&output_rb, &data);
    output_bytes_written += written;

    // The last packet can leave several decoded subframes. If this returns with
    // PCM pending, the consumer may wait for a full block without advancing the
    // read position or re-enabling this context.
    if ((!data.IsAnyInputBufferValid() && !current_frame_remaining_subframes_) ||
        data.error_status == 4) {
      silence_reason = data.error_status == 4 ? 4 : 2;
      break;
    }
    // A frame's continuation may be missing while the producer refills the other
    // buffer. Keep the state and release the lock until the next Enable, without
    // spinning here or turning an input wait into an error.
    if (!written && previous_offset == data.input_buffer_read_offset &&
        previous_buffer == data.current_buffer &&
        previous_valid == (data.input_buffer_0_valid | (data.input_buffer_1_valid << 1)) &&
        previous_loop_count == data.loop_count &&
        previous_subframes == current_frame_remaining_subframes_) {
      silence_reason = 3;
      break;
    }
  }

  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  // Equal pointers mean full only if PCM was written this time.
  if (output_bytes_written && output_rb.empty()) {
    data.output_buffer_valid = 0;
  } else if (REXCVAR_GET(audio_xma_output_done_when_drained) && !output_bytes_written && output_rb.empty() &&
             !data.IsAnyInputBufferValid() && !current_frame_remaining_subframes_) {
    // katamari-nx (2026-11-01): no input, nothing pending, every sample taken: the sound is done.
    data.output_buffer_valid = 0;
    if (TraceOn()) {
      REXAPU_INFO("XMA trace: context {} output drained with no input left: output marked done (loop count {})",
                  id(), uint32_t(data.loop_count));
    }
  }

  StoreContextMerged(data, initial_data, context_ptr);
  NoteStored(context_ptr);
  NoteProduction(output_bytes_written != 0, silence_reason, data);  // Measurement only
  return true;
}

bool XmaContext::TraceOn() {
  const int32_t trace = REXCVAR_GET(audio_xma_loop_trace);
  if (trace <= 0 || trace_lines_ >= uint32_t(trace)) {
    return false;
  }
  ++trace_lines_;
  return true;
}

void XmaContext::NoteStored(const uint8_t* context_ptr) {
  if (REXCVAR_GET(audio_xma_loop_trace) <= 0) {
    return;
  }
  const XMA_CONTEXT_DATA stored(context_ptr);
  trace_stored_inputs_ = uint8_t(stored.input_buffer_0_valid | (stored.input_buffer_1_valid << 1));
  trace_stored_output_ = stored.output_buffer_valid;
  trace_stored_known_ = true;
}

// blur-nx (2026-11-12, CPU batch 4): without the context's lock. Work holds that lock for a whole decode, so a kick from
// the game's audio server (XmaDecoder::WriteRegister: JQ Workers 3 and 4 in Blur's console race) for a context being
// decoded waited for the decode to end, and the decoder's unlock then had a waiter (a kernel call each time: 0.6 ms a
// frame of mutexUnlock on the XMA thread). The flag is an atomic; Work takes it with an exchange (see there), so a kick
// during a decode is seen by the next pass, which decodes again as it would after waiting: the same frames in the same
// order, sooner for the game. Disable, Clear and Release keep the lock (Disable's caller then waits in Block).
// blur-nx (2026-11-12 evening; the user: "Try turning off the lock free XMA kick ... compare perf"): a switch,
// audio_xma_kick_without_lock, OFF by default (the kick takes the lock, as before batch 4). The first batch-4 races
// showed ~7 % more audio-engine CPU a second than batch 3's; whether this is why is measured with one NRO, both ways.
// Work's exchange is right either way (under the lock, as the load and store it replaced).
REXCVAR_DEFINE_BOOL(audio_xma_kick_without_lock, false, "Audio",
                    "XMA: a kick (XMAEnableContext) sets the context's flag without its lock, so the game's audio thread "
                    "does not wait for a decode in progress (blur-nx batch 4). Read at the first kick");
void XmaContext::Enable() {
  static const bool without_lock = REXCVAR_GET(audio_xma_kick_without_lock);
  if (without_lock) {
    set_is_enabled(true);
    return;
  }
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
}

bool XmaContext::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContext::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_NOISY_DEBUG("XmaContext: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  if (TraceOn()) {
    REXAPU_INFO("XMA trace: context {} cleared by the game (inputs {}{}, output valid {}, loop count {})", id(),
                uint32_t(data.input_buffer_0_valid), uint32_t(data.input_buffer_1_valid),
                uint32_t(data.output_buffer_valid), uint32_t(data.loop_count));
  }
  ClearLocked(&data);
  data.Store(context_ptr);
  NoteStored(context_ptr);
}

void XmaContext::ClearLocked(XMA_CONTEXT_DATA* data) {
  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  ResetDecoderState();
}

void XmaContext::ResetDecoderState() {
  ResetDumpXma(id());  // diagnostic audio_dump_xma_s
  // A freed or re-initialized context is a new logical stream, so the previous
  // wave's MDCT overlap-add tail must not survive into frame 0 of the next one.
  // avcodec_flush_buffers() cannot drop it: ff_xmaframes_decoder declares no
  // flush callback, so the call never reaches the code clearing channel[].out.
  // Invalidating the cached format makes PrepareDecoder reopen the codec on the
  // next decode, which does discard the history.
  // blur-nx (2026-11-13, M-05 lazy open): the format PrepareDecoder compares is codec_rate_ / codec_channels_.
  codec_rate_ = 0;
  codec_channels_ = 0;
  raw_frame_.fill(0);
  in_silence_ = false;  // a new sound does not inherit the previous one's silence
  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
}

void XmaContext::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);
}

void XmaContext::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  ResetDecoderState();
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  if (TraceOn()) {
    const XMA_CONTEXT_DATA data(context_ptr);
    REXAPU_INFO("XMA trace: context {} released by the game (inputs {}{}, output valid {}, loop count {})", id(),
                uint32_t(data.input_buffer_0_valid), uint32_t(data.input_buffer_1_valid),
                uint32_t(data.output_buffer_valid), uint32_t(data.loop_count));
  }
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
  NoteStored(context_ptr);
}

void XmaContext::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContext::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  // katamari-nx (2026-10-31): a loop end below the first frame is no loop (Decode).
  if (data->loop_count == 0 || data->loop_end < kBitsPerPacketHeader) {
    return;
  }

  const uint32_t loop_start = std::max(kBitsPerPacketHeader, data->loop_start);

  if (data->input_buffer_read_offset != data->loop_end) {
    return;
  }

  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;

  if (data->loop_count < 255) {
    data->loop_count--;
  }
}

int XmaContext::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

int16_t XmaContext::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return static_cast<int16_t>(packet_number);
}

uint32_t XmaContext::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContext::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

uint32_t XmaContext::GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

const uint8_t* XmaContext::GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                         uint32_t current_input_packet_count) {
  uint8_t buffer_index = data->current_buffer;
  if (next_packet_index >= current_input_packet_count) {
    buffer_index ^= 1;
    next_packet_index -= current_input_packet_count;
  }

  if (!data->IsInputBufferValid(buffer_index) ||
      next_packet_index >= data->GetInputBufferPacketCount(buffer_index)) {
    return nullptr;
  }

  const uint32_t next_buffer_address = data->GetInputBufferAddress(buffer_index);
  if (!next_buffer_address) {
    REXAPU_ERROR("XmaContext {}: Buffer marked valid but has null pointer!", id());
    return nullptr;
  }

  // The skip between packets may continue inside the second buffer.
  // Always reading its start mixes the continuation with another audio stream.
  return memory()->TranslatePhysical(next_buffer_address) + next_packet_index * kBytesPerPacket;
}

uint32_t XmaContext::GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                             uint32_t current_input_packet_count) {
  while (next_packet_index < current_input_packet_count) {
    uint8_t* next_packet = buffer + (next_packet_index * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    // The helper already includes the 32-bit header. Compare with the packet size,
    // not with its payload: a frame can start in the last bits and continue in the
    // next packet (Decode handles that split header).
    if (packet_frame_offset < kBitsPerPacket) {
      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;
    }
    next_packet_index++;
  }

  // 32 is a valid offset: the first frame of the first packet.
  return UINT32_MAX;
}

void XmaContext::AdvanceInputPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index) {
  // At most the two available buffers are consumed. Keep pending skips when
  // crossing them and search after packets with no new frames.
  for (unsigned buffer = 0; buffer < 2; ++buffer) {
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
    const uint32_t count = data->GetCurrentInputBufferPacketCount();
    if (!data->GetCurrentInputBufferAddress()) {
      data->error_status = 4;
      return;
    }
    if (next_packet_index < count) {
      const uint32_t offset = GetNextPacketReadOffset(
          GetCurrentInputBuffer(data), next_packet_index, count);
      if (offset != UINT32_MAX) {
        data->input_buffer_read_offset = offset;
        return;
      }
      next_packet_index = 0;
    } else {
      next_packet_index -= count;
    }
    SwapInputBuffer(data);
  }
}

memory::RingBuffer XmaContext::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    REXAPU_WARN(
        "XmaContext {}: Output buffer exceeds expected size! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      static_cast<int32_t>(output_rb.write_count()) / kOutputBytesPerBlock;

  return output_rb;
}

kPacketInfo XmaContext::GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      // A header that starts here may continue in the next packet. Counting it
      // avoids marking the previous frame as the last one and skipping this one.
      // Decode will join both fragments to get its full size.
      if (stream.BitsRemaining()) {
        if (stream.offset_bits() == frame_offset) {
          packet_info.current_frame_ = packet_info.frame_count_;
        }
        packet_info.frame_count_++;
      }
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      break;
    }

    stream.Advance(frame_size - 1);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    }
  }
  return packet_info;
}

void XmaContext::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                    const XMA_CONTEXT_DATA& initial_data, uint8_t* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;
  fresh.current_buffer = data.current_buffer;
  // ds3-nx (2026-10-24, sdk-patches/0007; port 57): not output_buffer_read_offset. The guest owns it
  // (XMASetOutputBufferReadOffset), and storing this snapshot's value back undid the guest's update made during the
  // decode and replayed output it had already consumed (Hell's Gate, Dante's Inferno's PC port, found it first).

  fresh.Store(context_ptr);
}

uint32_t XmaContext::Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return 0;
  }

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      remaining_subframe_blocks_in_output_buffer_ -= data->output_buffer_padding;
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return 0;
    }
  }

  const uint8_t effective_sdc = std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  int8_t subframes_to_write = std::min(static_cast<int8_t>(current_frame_remaining_subframes_),
                                       static_cast<int8_t>(effective_sdc));

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    const int8_t remaining_until_limit = static_cast<int8_t>(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  // The no-input path also goes through here. Do not drop samples that do not
  // fit: keep them until the consumer frees space.
  if (remaining_subframe_blocks_in_output_buffer_ < subframes_to_write ||
      output_rb->write_count() < uint32_t(subframes_to_write) * kOutputBytesPerBlock) {
    return 0;
  }

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  const uint32_t written = static_cast<uint32_t>(output_rb->Write(
      raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
      subframes_to_write * kOutputBytesPerBlock));
  // Diagnostic (audio_dump_xma_s): the blocks as they reach the game's buffer.
  DumpXma(id(), 1, GetSampleRate(data->sample_rate), data->is_stereo ? 2 : 1,
            raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset), written);

  const int8_t headroom = (current_frame_remaining_subframes_ - subframes_to_write == 0)
                              ? data->output_buffer_padding
                              : 0;

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;
  return written;
}

// blur-nx (2026-11-13, M-05 of docs/cpu-moonshots-50-1113.md, as a lazy open): a new format (or a new sound:
// ResetDecoderState) makes a fresh codec, 1, as before; with the frame cache on, it is opened by OpenCodec before its
// first decode instead of here. The frame cache v2 answers most frames of a sound it has met (the first frame's key is
// the format and its own packet), and a sound whose frames are all answered never needs the codec: per new sound, an
// avcodec_alloc_context3 (a zeroed ~140 KB WMA Pro context) and decode_init's set-up no longer run (the moonshot's
// 0.2-0.4 ms a frame of the audio thread). Exact: the codec is opened fresh before it decodes anything of the sound,
// and a codec behind by answered frames is caught up by M-01's one-frame decode, whose state does not depend on what
// the codec held before (DecodeFrame). With the cache off (or its key off): opened here, as before.
int XmaContext::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  const int channels = is_two_channel ? 2 : 1;
  if (codec_rate_ != sample_rate || codec_channels_ != channels) {
    REXAPU_NOISY_DEBUG("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}", id(), codec_rate_,
                       sample_rate, codec_channels_, channels);
    codec_rate_ = sample_rate;
    codec_channels_ = channels;
    codec_stale_ = true;
    static const bool lazy = REXCVAR_GET(banjo_xma_frame_cache) && REXCVAR_GET(banjo_xma_codec_lazy_open);
    if (!lazy && !OpenCodec()) return -1;
    return 1;
  }
  return 0;
}

// The codec opened fresh for codec_rate_ / codec_channels_ when PrepareDecoder made a new one (codec_stale_).
bool XmaContext::OpenCodec() {
  if (!codec_stale_) return true;
  avcodec_free_context(&av_context_);
  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id());
    codec_rate_ = codec_channels_ = 0;  // (the next frame tries again)
    return false;
  }
  av_context_->sample_rate = codec_rate_;
  av_context_->channels = codec_channels_;
  av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
  if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
    REXAPU_ERROR("XmaContext: Failed to reopen FFmpeg context");
    codec_rate_ = codec_channels_ = 0;
    return false;
  }
  codec_stale_ = false;
  Cache().opens.fetch_add(1, std::memory_order_relaxed);  // (the 10 s report)
  return true;
}

void XmaContext::PreparePacket(uint32_t frame_size, uint32_t frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                      (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XmaContext::DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                              AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error sending packet for decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  return true;
}

void XmaContext::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);

  // blur-nx (2026-11-13, M-03): no whole fill of input_buffer_ here. Its first half is always copied over before it is
  // read (below); its second half is copied over when a frame runs into the next packet, and zeroed below otherwise, as
  // the fill left it.
  bool input_second_half = false;

  // Loop-end frame: decode it here (output limited to loop_subframe_end),
  // jump to loop_start afterwards in the next-offset step.
  // katamari-nx (2026-10-31): a loop end below the first frame (bit 32) is no loop. Beautiful Katamari plays its
  // one-shot 44.1 kHz sounds with loop count 255 and loop start = end = 0; clamping the end up to 32 made the first
  // frame a loop end, so each of them replayed one 128-sample subframe until the voice was reset (a constant
  // 344.5 Hz tone on the menus, and those sounds never played). No frame starts below bit 32, so the hardware never
  // meets such an end and the sound plays to its end.
  bool is_loop_end_frame = false;
  if (data->loop_count > 0 && data->loop_end >= kBitsPerPacketHeader) {
    is_loop_end_frame = (data->input_buffer_read_offset == data->loop_end);
  }

  if (!data->output_buffer_block_count) {
    REXAPU_ERROR("XmaContext {}: Error - Received 0 for output_buffer_block_count!", id());
    return;
  }

  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count = current_input_size / kBytesPerPacket;

  const int16_t packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    REXAPU_ERROR("XmaContext {}: Invalid packet index. Input read offset: {}", id(),
                 static_cast<uint32_t>(data->input_buffer_read_offset));
    // Work must publish the error and release the lock. Without this state, it
    // repeated Decode forever with the same offset outside the input.
    data->error_status = 4;
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip (0xFF) -- no new frames begin in this packet.
  if (skip_count == 0xFF) {
    if (LoopWholeBuffer(data, packet_index + 1, current_input_packet_count)) {
      return;
    }
    AdvanceInputPacket(data, packet_index + 1);
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary.
  if (packet_info.current_frame_size_ == 0) {
    const uint8_t* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      // A split header still belongs to the current buffer. Do not release it
      // before the producer publishes its continuation in the other buffer.
      if (kBitsPerPacket - relative_offset < kBitsPerFrameHeader &&
          next_packet_index >= current_input_packet_count &&
          !data->IsInputBufferValid(data->current_buffer ^ 1)) {
        return;
      }
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                kBytesPerPacketData);
    input_second_half = true;

    BitStream combined(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
  }

  BitStream stream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy = GetAmountOfBitsToRead(static_cast<uint32_t>(stream.BitsRemaining()),
                                                      packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    REXAPU_ERROR("XmaContext {}: There are no bits to copy!", id());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
        if (next_packet_index >= current_input_packet_count &&
            !data->IsInputBufferValid(data->current_buffer ^ 1)) {
          return;
        }
        data->error_status = 4;
        return;
      }
      std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                  kBytesPerPacketData);
      input_second_half = true;
    }
  }

  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
  if (!input_second_half)
    std::memset(input_buffer_.data() + kBytesPerPacketData, 0, kBytesPerPacketData);  // M-03: as the fill left it

  stream = BitStream(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  // blur-nx (2026-11-13, M-03): not the whole 4 KB. BitStream::Copy keeps the bits of its first and last byte outside
  // the frame (the padding, which is part of the packet's bytes: the cache's key and compare) and copies the bytes
  // between whole; FFmpeg may read AV_INPUT_BUFFER_PADDING_SIZE bytes past the packet. So those bytes are zeroed, as the
  // fill left them: the packet's data bytes (the frame's, within 8 of its ends rewritten whole) and 64 past them.
  {
    const uint32_t frame_bits = ((relative_offset - kBitsPerPacketHeader) & 7) + packet_info.current_frame_size_;
    const size_t data_bytes = (frame_bits + 7) / 8;
    const size_t head = std::min<size_t>(data_bytes, 8);
    std::memset(xma_frame_.data() + 1, 0, head);
    const size_t tail_begin = 1 + (data_bytes > 8 ? data_bytes - 8 : 0);
    const size_t tail_end = std::min<size_t>(xma_frame_.size(), 1 + data_bytes + AV_INPUT_BUFFER_PADDING_SIZE);
    if (tail_end > tail_begin) std::memset(xma_frame_.data() + tail_begin, 0, tail_end - tail_begin);
  }

  const uint32_t padding_start =
      static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  // blur-nx (2026-11-13, M-03): no raw_frame_.fill(0): DecodeFrame writes every byte that is read (ConvertFrame the
  // whole 512 samples of each channel, an answer the node's same bytes), and a frame that fails is not output.

  const int prepared = PrepareDecoder(data->sample_rate, bool(data->is_stereo));
  if (prepared < 0) {
    data->error_status = 4;
    return;
  }
  PreparePacket(packet_info.current_frame_size_, padding_start);
  // banjo-nx (2026-10-27, #4): decoded, or answered by the frame cache (DecodeFrame converts it into raw_frame_).
  const bool decoded = DecodeFrame(prepared, GetSampleRate(data->sample_rate), bool(data->is_stereo));
  if (codec_open_failed_) {
    // blur-nx (2026-11-13, M-05 lazy open): the codec failed to open (PrepareDecoder's -1 before).
    codec_open_failed_ = false;
    data->error_status = 4;
    return;
  }
  if (decoded) {
    // Diagnostic (audio_dump_xma_s): the whole frame as it comes out of FFmpeg.
    DumpXma(id(), 0, GetSampleRate(data->sample_rate), data->is_stereo ? 2 : 1, raw_frame_.data(),
              size_t(kBytesPerFrameChannel) << data->is_stereo);

    // The game discards 384 samples at the start of each sound.
    // Deliver the decoder's whole block: shifting it by 192 samples and holding
    // its tail until the next frame also dropped a final block of 512. The
    // consumer ran out of input and waited forever.
    current_frame_remaining_subframes_ = 4 << data->is_stereo;
    loop_frame_output_limit_ =
        is_loop_end_frame ? static_cast<uint8_t>((data->loop_subframe_end + 1) << data->is_stereo)
                          : 0;
    const uint8_t decoded_start_skip =
        loop_start_skip_pending_ ? static_cast<uint8_t>(data->loop_subframe_skip << data->is_stereo)
                                 : 0;
    loop_start_skip_pending_ = false;
    current_frame_remaining_subframes_ -=
        std::min(decoded_start_skip, current_frame_remaining_subframes_);
  }

  // katamari-nx (2026-10-31): audio_xma_loop_trace, diagnostic only (the console's constant tone).
  if (const int32_t trace = REXCVAR_GET(audio_xma_loop_trace); trace > 0 && trace_lines_ < uint32_t(trace)) {
    const uint32_t offset = data->input_buffer_read_offset;
    trace_same_offset_ = offset == trace_last_offset_ ? trace_same_offset_ + 1 : 0;
    trace_last_offset_ = offset;
    if (is_loop_end_frame || trace_same_offset_ == 4) {
      ++trace_lines_;
      REXAPU_INFO(
          "XMA trace: context {} {} at bit {} (packet {} of {}, buffer {}, valid {}{}): loop start {} end {} count {} "
          "subframe end {} skip {}; frame {} bits, last in packet {}; {} Hz {}; output blocks {} read {} write {}",
          id(), is_loop_end_frame ? "loop end" : "same frame 5 times", offset, packet_index,
          current_input_packet_count, uint32_t(data->current_buffer), uint32_t(data->input_buffer_0_valid),
          uint32_t(data->input_buffer_1_valid), uint32_t(data->loop_start), uint32_t(data->loop_end),
          uint32_t(data->loop_count), uint32_t(data->loop_subframe_end), uint32_t(data->loop_subframe_skip),
          packet_info.current_frame_size_, packet_info.isLastFrameInPacket(), GetSampleRate(data->sample_rate),
          data->is_stereo ? "stereo" : "mono", uint32_t(data->output_buffer_block_count),
          uint32_t(data->output_buffer_read_offset), uint32_t(data->output_buffer_write_offset));
    }
  }

  // Compute where to go next.
  if (is_loop_end_frame) {
    UpdateLoopStatus(data);
    return;
  }

  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  if (LoopWholeBuffer(data, next_packet_index, current_input_packet_count)) {
    return;
  }
  AdvanceInputPacket(data, next_packet_index);
}

// katamari-nx (2026-10-31): a loop count with no loop end (loop_end below the first frame, as Beautiful Katamari's
// sounds come: start 0, end 0, count 255) loops the whole buffer when audio_xma_whole_buffer_loop is on: past the
// buffer's last frame, with nothing valid in the other buffer, the read position goes back to the loop start (the
// buffer's first frame for a start of 0) instead of releasing the buffer. rc5 shipped it on, read from a Ryujinx run
// whose menu went quiet when these sounds ended; they are one-shots (2026-11-01: the game releases each of them as
// it ends) and it is off by default since rc6. With it off this only traces the end of a sound's input.
bool XmaContext::LoopWholeBuffer(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                 uint32_t current_input_packet_count) {
  if (next_packet_index < current_input_packet_count || data->IsInputBufferValid(data->current_buffer ^ 1)) {
    return false;  // more input follows
  }
  // 2026-11-01: audio_xma_whole_buffer_loop = false plays such a sound to its end, like any other.
  if (data->loop_count == 0 || data->loop_end >= kBitsPerPacketHeader ||
      !REXCVAR_GET(audio_xma_whole_buffer_loop)) {
    if (TraceOn()) {
      REXAPU_INFO("XMA trace: context {} input ends ({} packets, buffer {}; loop start {} end {} count {}): released",
                  id(), current_input_packet_count, uint32_t(data->current_buffer), uint32_t(data->loop_start),
                  uint32_t(data->loop_end), uint32_t(data->loop_count));
    }
    return false;
  }
  uint32_t start = GetNextPacketReadOffset(GetCurrentInputBuffer(data), 0, current_input_packet_count);
  if (start == UINT32_MAX) {
    return false;
  }
  if (data->loop_start >= start && data->loop_start < current_input_packet_count * kBitsPerPacket) {
    start = data->loop_start;
  }
  data->input_buffer_read_offset = start;
  loop_start_skip_pending_ = true;
  if (data->loop_count < 255) {
    data->loop_count--;
  }
  if (const int32_t trace = REXCVAR_GET(audio_xma_loop_trace); trace > 0 && trace_lines_ < uint32_t(trace)) {
    ++trace_lines_;
    REXAPU_INFO("XMA trace: context {} loops its whole buffer ({} packets): back to bit {}, count {}", id(),
                current_input_packet_count, start, uint32_t(data->loop_count));
  }
  return true;
}

void XmaContext::ConvertFrame(const uint8_t** samples, bool is_two_channel,
                              uint8_t* output_buffer) {
  // Loop through every sample, convert and drop it into the output array.
  // If more than one channel, we need to interleave the samples from each
  // channel next to each other. Always saturate because FFmpeg output is
  // not limited to [-1, 1] (for example 1.095 as seen in 5454082B).
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);

  // For testing of vectorized versions, stereo audio is common in 4D5307E6,
  // since the first menu frame; the intro cutscene also has more than 2
  // channels.
#if REX_ARCH_AMD64
  static_assert(kSamplesPerFrame % 8 == 0);
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  const __m128 scale_mm = _mm_set1_ps(scale);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    const __m128i shufmask = _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      // Load 8 samples, 4 for each channel.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Interleave channels and byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 4] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
    }
  } else {
    const __m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 8) {
      // Load 8 samples, as [in_channel_0 + i * 4] and
      // [in_channel_0 + i * 4 + 16] movups.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 2] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
    }
  }
#elif defined(__aarch64__)
  // NEON, bit-identical to the scalar loop below: clamp_float (isgreater/isless selects, so NaN becomes the
  // lower bound), multiply by 32767, truncate toward zero, big-endian int16, channels interleaved.
  static_assert(kSamplesPerFrame % 4 == 0);
  const float32x4_t lo = vdupq_n_f32(-1.0f);
  const float32x4_t hi = vdupq_n_f32(1.0f);
  const float32x4_t scale_v = vdupq_n_f32(scale);
  const auto convert4 = [&](const float* in) {
    float32x4_t x = vld1q_f32(in);
    x = vbslq_f32(vcgtq_f32(x, lo), x, lo);  // std::isgreater(x, lo) ? x : lo
    x = vbslq_f32(vcltq_f32(x, hi), x, hi);  // std::isless(x, hi) ? x : hi
    return vmovn_s32(vcvtq_s32_f32(vmulq_f32(x, scale_v)));
  };
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      const int16x4x2_t z = vzip_s16(convert4(&in_channel_0[i]), convert4(&in_channel_1[i]));
      const int16x8_t both = vcombine_s16(z.val[0], z.val[1]);
      vst1q_u8(reinterpret_cast<uint8_t*>(&out[i * 2]), vrev16q_u8(vreinterpretq_u8_s16(both)));
    }
  } else {
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      vst1_u8(reinterpret_cast<uint8_t*>(&out[i]),
              vrev16_u8(vreinterpret_u8_s16(convert4(&in_channel_0[i]))));
    }
  }
#else
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      // Select the appropriate array based on the current channel.
      auto in = reinterpret_cast<const float*>(samples[j]);

      // Raw samples sometimes aren't within [-1, 1]
      float scaled_sample = rex::clamp_float(in[i], -1.0f, 1.0f) * scale;

      // Convert the sample and output it in big endian.
      auto sample = static_cast<int16_t>(scaled_sample);
      out[o++] = rex::byte_swap(sample);
    }
  }
#endif
}

}  // namespace rex::audio
