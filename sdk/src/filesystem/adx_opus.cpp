/**
 * sa2-nx-ult (2026-11-21): reading NAME.adxo files (rex/filesystem/adx_opus.h has the format): a file of CRI ADX
 * streams whose sound is Opus, shown to the game as the file it was (its ADX blocks tags), and the samples the decoder
 * hook (app/src/sa2_adx_opus.cpp) asks for, decoded from the Opus and resampled to each stream's own rate. The segment
 * reader and the registry follow rr6-nx's sfd_hevc.cpp; the Opus decoder is rr6-nx's (culdcept-nx's) opus_decoder.
 */
#include <rex/filesystem/adx_opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <numeric>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/string.h>

#include "opus_decoder.h"

namespace rex::filesystem {

namespace {

constexpr char kMagic[8] = {'R', 'X', 'A', 'D', 'X', 'O', 'P', '1'};
constexpr size_t kSegmentSize = 32;
constexpr size_t kStreamSize = 64;
constexpr uint8_t kTagMagic[3] = {0x7A, 0xD5, 0xC3};
constexpr auto kGrace = std::chrono::seconds(10);  // a stream stays findable this long after its file closes
constexpr int kOpusRate = 48000;
constexpr int kPacketSamples = 960;   // 20 ms at 48 kHz: every packet the tools and the installer make
constexpr int kPreroll = 4;           // packets decoded before the one a seek lands in (Opus converges in 80 ms)
constexpr int64_t kKeepBehind = 48000;  // decoded samples kept behind the decoder's reader (a second)
constexpr double kPi = 3.14159265358979323846;

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

bool ReadExactly(FileHandle& f, uint64_t offset, void* to, size_t length) {
  size_t got = 0;
  return f.Read(size_t(offset), to, length, &got) && got == length;
}

void LogBad(const std::filesystem::path& path, const char* why) {
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) REXFS_ERROR("[adxo] {}: {}", rex::path_to_utf8(path), why);
}

struct Header {
  uint32_t segments = 0, streams = 0;
  uint64_t size = 0, segments_offset = 0, stored_offset = 0, stored_length = 0, streams_offset = 0;
};

bool ParseHeader(const uint8_t* h, Header* out) {
  if (std::memcmp(h, kMagic, 8) != 0 || Le32(h + 8) != 1) return false;
  out->segments = Le32(h + 12);
  out->size = Le64(h + 16);
  out->segments_offset = Le64(h + 24);
  out->stored_offset = Le64(h + 32);
  out->stored_length = Le64(h + 40);
  out->streams = Le32(h + 48);
  out->streams_offset = Le64(h + 56);
  return out->segments >= 1 && out->segments <= (1u << 24) && out->streams >= 1 && out->streams <= 65536;
}

struct Segment {
  uint64_t offset = 0, length = 0;
  uint32_t kind = 0, stream = 0;
  uint64_t source = 0;
};

// ---- the resampler (tools/adx_opus_lib.mjs's resamplerTaps, the same filter) ---------------------------------------

struct Taps {
  int num = 1, den = 1, taps = 0, first = 0;
  std::vector<float> rows;
};

double Bessel0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 30; ++k) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
  }
  return sum;
}

std::shared_ptr<const Taps> MakeTaps(int from, int to) {
  static std::mutex mutex;
  static std::map<std::pair<int, int>, std::shared_ptr<const Taps>> cache;
  std::lock_guard<std::mutex> lock(mutex);
  auto& slot = cache[{from, to}];
  if (slot) return slot;
  constexpr double kHalf = 16, kBeta = 8.0, kPass = 0.97;
  auto t = std::make_shared<Taps>();
  const int g = std::gcd(from, to);
  t->num = from / g;
  t->den = to / g;
  const double scale = std::min(1.0, double(to) / double(from));
  const double fc = kPass * scale;
  const int half = int(std::ceil(kHalf / scale));
  t->taps = 2 * half;
  t->first = half - 1;
  const double i0 = Bessel0(kBeta);
  t->rows.resize(size_t(t->den) * size_t(t->taps));
  for (int p = 0; p < t->den; ++p) {
    const double frac = double(p) / double(t->den);
    double sum = 0;
    for (int j = 0; j < t->taps; ++j) {
      const double d = double(j - t->first) - frac;
      const double x = d / double(half + 1);
      const double w = std::abs(x) >= 1 ? 0 : Bessel0(kBeta * std::sqrt(1 - x * x)) / i0;
      const double s = d == 0 ? 1 : std::sin(kPi * fc * d) / (kPi * fc * d);
      const double v = fc * s * w;
      t->rows[size_t(p) * size_t(t->taps) + size_t(j)] = float(v);
      sum += v;
    }
    for (int j = 0; j < t->taps; ++j) t->rows[size_t(p) * size_t(t->taps) + size_t(j)] /= float(sum);
  }
  slot = t;
  return t;
}

// ---- one stream ----------------------------------------------------------------------------------------------------

struct StreamData {
  uint32_t id = 0;
  uint32_t channels = 0, rate = 0, blocks = 0, packets = 0;
  uint16_t pre_skip = 0;
  uint64_t table_offset = 0, data_offset = 0, data_length = 0;
  std::string name;
  std::shared_ptr<FileHandle> file;  // the .adxo, for the packets (read on first use)
  std::shared_ptr<std::mutex> file_mutex;

  std::mutex mutex;  // everything below
  bool loaded = false, broken = false;
  std::vector<uint8_t> opus;
  std::vector<uint32_t> offset;  // each packet's start in `opus`, and the end
  opus::Decoder* decoder = nullptr;
  bool silent = false;           // no libopus: silence
  int next_packet = 0;
  int64_t win_start = 0;         // the 48 kHz timeline after the pre-skip: the window's first sample
  std::vector<float> win[2];
  std::vector<float> scratch;
  std::shared_ptr<const Taps> taps;

  ~StreamData() { opus::Destroy(decoder); }
};

struct Registry {
  std::mutex mutex;
  uint32_t next_id = 1;
  struct Entry {
    std::shared_ptr<StreamData> stream;
    int open = 0;
    std::chrono::steady_clock::time_point closed;
  };
  std::map<uint32_t, Entry> streams;
  AdxOpusCounts counts;

  void Prune() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = streams.begin(); it != streams.end();) {
      if (it->second.open == 0 && now - it->second.closed > kGrace)
        it = streams.erase(it);
      else
        ++it;
    }
  }
  uint32_t NewId() {
    for (;;) {
      const uint32_t id = next_id;
      next_id = (next_id + 1) & 0x1FFFFF;  // 21 bits, the tag's
      if (!next_id) next_id = 1;
      if (!streams.count(id)) return id;
    }
  }
};

Registry& GetRegistry() {
  static Registry r;
  return r;
}
std::atomic<uint64_t> g_packets{0}, g_seeks{0}, g_blocks{0}, g_unknown{0}, g_used{0};

// The stream's packets into memory (once).
bool Load(StreamData& s) {
  if (s.loaded) return !s.broken;
  s.loaded = true;
  std::vector<uint8_t> table(size_t(s.packets) * 4);
  {
    std::lock_guard<std::mutex> lock(*s.file_mutex);
    s.opus.resize(size_t(s.data_length));
    if (!ReadExactly(*s.file, s.table_offset, table.data(), table.size()) ||
        !ReadExactly(*s.file, s.data_offset, s.opus.data(), s.opus.size())) {
      REXFS_ERROR("[adxo] {}: its Opus could not be read", s.name);
      s.broken = true;
      return false;
    }
  }
  s.offset.resize(size_t(s.packets) + 1);
  uint64_t at = 0;
  for (uint32_t k = 0; k < s.packets; ++k) {
    s.offset[k] = uint32_t(at);
    at += Le32(table.data() + size_t(k) * 4);
  }
  s.offset[s.packets] = uint32_t(at);
  if (at != s.data_length) {
    REXFS_ERROR("[adxo] {}: its packet table does not add up to its Opus", s.name);
    s.broken = true;
    return false;
  }
  s.decoder = opus::Create(int(s.channels));
  s.silent = !s.decoder;
  s.taps = MakeTaps(kOpusRate, int(s.rate));
  s.win_start = INT64_MIN / 2;
  if (g_used.fetch_add(1, std::memory_order_relaxed) < 4)
    REXFS_INFO("[adxo] {}: {} ch {} Hz, {} blocks a channel, {} Opus packets ({:.1f} KB){}", s.name, s.channels, s.rate,
               s.blocks, s.packets, double(s.data_length) / 1024.0, s.silent ? ": no libopus, silence" : "");
  return true;
}

// The 48 kHz samples [lo, hi) of the timeline after the pre-skip in the window (decoded as needed; outside the stream:
// zeros).
void Ensure(StreamData& s, int64_t lo, int64_t hi) {
  const int64_t win_end = s.win_start + int64_t(s.win[0].size());
  const int64_t total = int64_t(s.packets) * kPacketSamples - s.pre_skip;
  if (lo < 0) lo = 0;
  if (hi > total) hi = total;
  if (lo >= hi) return;
  if (lo < s.win_start || lo > win_end + kOpusRate) {  // a jump: start again a few packets before it
    const int64_t k = std::max<int64_t>(0, (lo + s.pre_skip) / kPacketSamples - kPreroll);
    opus::Reset(s.decoder);
    s.next_packet = int(k);
    s.win_start = k * kPacketSamples - s.pre_skip;
    for (uint32_t c = 0; c < s.channels; ++c) s.win[c].clear();
    g_seeks.fetch_add(1, std::memory_order_relaxed);
  }
  s.scratch.resize(size_t(5760) * s.channels);
  while (s.win_start + int64_t(s.win[0].size()) < hi && s.next_packet < int(s.packets)) {
    const uint32_t a = s.offset[size_t(s.next_packet)], b = s.offset[size_t(s.next_packet) + 1];
    int n = opus::Decode(s.decoder, s.opus.data() + a, int(b - a), s.scratch.data(), 5760);
    ++s.next_packet;
    g_packets.fetch_add(1, std::memory_order_relaxed);
    if (n != kPacketSamples) {  // a damaged packet, or one of another length: its 20 ms as silence keeps the timeline
      static std::atomic<int> said{0};
      if (said.fetch_add(1, std::memory_order_relaxed) < 4)
        REXFS_WARN("[adxo] {}: packet {} decoded to {} samples (expected {})", s.name, s.next_packet - 1, n,
                   kPacketSamples);
      n = kPacketSamples;
      std::fill(s.scratch.begin(), s.scratch.end(), 0.0f);
    }
    for (uint32_t c = 0; c < s.channels; ++c)
      for (int i = 0; i < n; ++i) s.win[c].push_back(s.scratch[size_t(i) * s.channels + c]);
  }
  // Keep a second behind what was asked for.
  const int64_t drop = (lo - kKeepBehind) - s.win_start;
  if (drop > kOpusRate) {
    for (uint32_t c = 0; c < s.channels; ++c) s.win[c].erase(s.win[c].begin(), s.win[c].begin() + drop);
    s.win_start += drop;
  }
}

class AdxOpusHandle final : public FileHandle {
 public:
  AdxOpusHandle(std::unique_ptr<FileHandle> packed, const Header& header, std::vector<Segment> segments,
                std::vector<std::shared_ptr<StreamData>> streams)
      : FileHandle(packed->path()),
        packed_(std::move(packed)),
        header_(header),
        segments_(std::move(segments)),
        streams_(std::move(streams)) {}
  ~AdxOpusHandle() override {
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto now = std::chrono::steady_clock::now();
    for (const auto& s : streams_) {
      auto it = r.streams.find(s->id);
      if (it != r.streams.end() && --it->second.open == 0) it->second.closed = now;
    }
  }

  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out_bytes_read = 0;
    if (file_offset >= header_.size) return true;
    const size_t want = size_t(std::min<uint64_t>(buffer_length, header_.size - file_offset));
    auto* out = static_cast<uint8_t*>(buffer);
    size_t si = size_t(std::upper_bound(segments_.begin(), segments_.end(), uint64_t(file_offset),
                                        [](uint64_t v, const Segment& s) { return v < s.offset; }) -
                       segments_.begin());
    if (si == 0) return false;
    --si;
    size_t done = 0;
    while (done < want && si < segments_.size()) {
      const Segment& seg = segments_[si];
      const uint64_t inside = file_offset + done - seg.offset;
      const size_t take = size_t(std::min<uint64_t>(want - done, seg.length - inside));
      if (seg.kind == 0) {
        if (!ReadExactly(*packed_, header_.stored_offset + seg.source + inside, out + done, take)) {
          LogBad(path_, "a read of its stored bytes failed");
          return false;
        }
      } else if (seg.kind == 1) {
        std::memset(out + done, 0, take);
      } else {
        const StreamData& s = *streams_[seg.stream];
        uint64_t byte = seg.source + inside;
        size_t w = 0;
        uint8_t tag[kAdxBlockBytes];
        uint64_t tag_block = UINT64_MAX;
        while (w < take) {
          const uint64_t blk = byte / kAdxBlockBytes;
          const uint32_t in_block = uint32_t(byte % kAdxBlockBytes);
          if (blk != tag_block) {
            MakeAdxOpusTag(s.id, uint32_t(blk / s.channels), uint32_t(blk % s.channels), tag);
            tag_block = blk;
          }
          const size_t n = std::min<size_t>(take - w, kAdxBlockBytes - in_block);
          std::memcpy(out + done + w, tag + in_block, n);
          w += n;
          byte += n;
        }
      }
      done += take;
      ++si;
    }
    *out_bytes_read = done;
    return true;
  }

  bool Write(size_t, const void*, size_t, size_t* out_bytes_written) override {
    *out_bytes_written = 0;
    return false;
  }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  std::unique_ptr<FileHandle> packed_;
  const Header header_;
  const std::vector<Segment> segments_;
  const std::vector<std::shared_ptr<StreamData>> streams_;
  std::mutex mutex_;
};

}  // namespace

bool HasAdxOpusSuffix(std::string_view name) {
  if (name.size() <= kAdxOpusSuffix.size()) return false;
  const std::string_view tail = name.substr(name.size() - kAdxOpusSuffix.size());
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != kAdxOpusSuffix[i]) return false;
  }
  return true;
}

bool AdxOpusSizeFromHeader(const uint8_t* h, size_t length, uint64_t* out_size) {
  Header header;
  if (length < kAdxOpusHeaderBytes || !ParseHeader(h, &header)) return false;
  *out_size = header.size;
  return true;
}

bool ReadAdxOpusSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = FileHandle::OpenExisting(path, FileAccess::kFileReadData);
  if (!f) return false;
  uint8_t h[kAdxOpusHeaderBytes];
  return ReadExactly(*f, 0, h, sizeof(h)) && AdxOpusSizeFromHeader(h, sizeof(h), out_size);
}

std::unique_ptr<FileHandle> OpenAdxOpus(std::unique_ptr<FileHandle> packed) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  FileInfo info;
  if (!GetInfo(path, &info)) {
    LogBad(path, "cannot be read");
    return nullptr;
  }
  return OpenAdxOpusFrom(std::move(packed), FileHandle::OpenExisting(path, FileAccess::kFileReadData),
                         info.total_size);
}

std::unique_ptr<FileHandle> OpenAdxOpusFrom(std::unique_ptr<FileHandle> packed, std::unique_ptr<FileHandle> opus_file,
                                            uint64_t file_size) {
  if (!packed || !opus_file) return nullptr;
  const std::filesystem::path path = packed->path();
  uint8_t h[kAdxOpusHeaderBytes];
  Header header;
  if (file_size < sizeof(h) || !ReadExactly(*packed, 0, h, sizeof(h)) || !ParseHeader(h, &header)) {
    LogBad(path, "not an .adxo file (header)");
    return nullptr;
  }
  auto within = [&](uint64_t offset, uint64_t length) { return offset <= file_size && length <= file_size - offset; };
  if (!within(header.segments_offset, uint64_t(header.segments) * kSegmentSize) ||
      !within(header.stored_offset, header.stored_length) ||
      !within(header.streams_offset, uint64_t(header.streams) * kStreamSize)) {
    LogBad(path, "its tables are not inside it");
    return nullptr;
  }
  std::vector<uint8_t> raw(size_t(header.streams) * kStreamSize);
  if (!ReadExactly(*packed, header.streams_offset, raw.data(), raw.size())) {
    LogBad(path, "its streams could not be read");
    return nullptr;
  }
  std::filesystem::path base = path.filename();
  base.replace_extension();
  const std::string name = rex::path_to_utf8(base);
  auto shared_file = std::shared_ptr<FileHandle>(std::move(opus_file));
  auto file_mutex = std::make_shared<std::mutex>();
  std::vector<std::shared_ptr<StreamData>> streams(header.streams);
  for (uint32_t i = 0; i < header.streams; ++i) {
    const uint8_t* p = raw.data() + size_t(i) * kStreamSize;
    auto s = std::make_shared<StreamData>();
    s->channels = Le32(p);
    s->rate = Le32(p + 4);
    s->blocks = Le32(p + 8);
    s->packets = Le32(p + 12);
    s->pre_skip = uint16_t(p[16] | (p[17] << 8));
    s->table_offset = Le64(p + 24);
    s->data_offset = Le64(p + 32);
    s->data_length = Le64(p + 40);
    s->file = shared_file;
    s->file_mutex = file_mutex;
    s->name = header.streams > 1 ? fmt::format("{} #{}", name, i) : name;
    if (s->channels < 1 || s->channels > 2 || s->rate < 8000 || s->rate > 96000 || !s->packets ||
        !within(s->table_offset, uint64_t(s->packets) * 4) || !within(s->data_offset, s->data_length) ||
        s->data_length >= (1ull << 32)) {
      LogBad(path, "a stream's fields are not valid");
      return nullptr;
    }
    streams[i] = std::move(s);
  }
  raw.assign(size_t(header.segments) * kSegmentSize, 0);
  if (!ReadExactly(*packed, header.segments_offset, raw.data(), raw.size())) {
    LogBad(path, "its segments could not be read");
    return nullptr;
  }
  std::vector<Segment> segments(header.segments);
  uint64_t expect = 0;
  for (uint32_t i = 0; i < header.segments; ++i) {
    const uint8_t* p = raw.data() + size_t(i) * kSegmentSize;
    Segment& s = segments[i];
    s.offset = Le64(p);
    s.length = Le64(p + 8);
    s.kind = Le32(p + 16);
    s.stream = Le32(p + 20);
    s.source = Le64(p + 24);
    bool ok = s.offset == expect && s.length > 0;
    if (s.kind == 0)
      ok = ok && s.source <= header.stored_length && s.length <= header.stored_length - s.source;
    else if (s.kind == 3)
      ok = ok && s.stream < header.streams &&
           s.source + s.length <= uint64_t(streams[s.stream]->blocks) * streams[s.stream]->channels * kAdxBlockBytes;
    else
      ok = ok && s.kind == 1;
    if (!ok) {
      LogBad(path, "its segments do not cover the file in order");
      return nullptr;
    }
    expect += s.length;
  }
  if (expect != header.size) {
    LogBad(path, "its segments do not end at its size");
    return nullptr;
  }
  {
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.Prune();
    for (auto& s : streams) {
      s->id = r.NewId();
      auto& e = r.streams[s->id];
      e.stream = s;
      e.open = 1;
    }
    ++r.counts.streams;
  }
  static std::atomic<int> said{0};
  if (said.fetch_add(1, std::memory_order_relaxed) < 8)
    REXFS_INFO("[adxo] {}: {} ADX streams as Opus (ids {} on)", name, header.streams, streams.front()->id);
  return std::make_unique<AdxOpusHandle>(std::move(packed), header, std::move(segments), std::move(streams));
}

bool AdxOpusBlockSamples(uint32_t id, uint32_t block, uint32_t channel, int16_t* out) {
  std::shared_ptr<StreamData> sp;
  {
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.streams.find(id);
    if (it != r.streams.end()) sp = it->second.stream;
  }
  if (!sp) {
    g_unknown.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  StreamData& s = *sp;
  std::lock_guard<std::mutex> lock(s.mutex);
  if (channel >= s.channels || !Load(s)) return false;
  g_blocks.fetch_add(1, std::memory_order_relaxed);
  if (s.silent) {
    std::memset(out, 0, sizeof(int16_t) * kAdxBlockSamples);
    return true;
  }
  const Taps& t = *s.taps;
  // Output n (the stream's rate) is the 48 kHz input at n * num / den.
  const int64_t n0 = int64_t(block) * kAdxBlockSamples;
  const int64_t lo = (n0 * t.num) / t.den - t.first;
  const int64_t hi = ((n0 + kAdxBlockSamples - 1) * t.num) / t.den - t.first + t.taps;
  Ensure(s, lo, hi);
  const std::vector<float>& w = s.win[channel];
  const int64_t ws = s.win_start, we = ws + int64_t(w.size());
  for (uint32_t i = 0; i < kAdxBlockSamples; ++i) {
    const int64_t q = (n0 + i) * t.num;
    const int64_t k0 = q / t.den;
    const int p = int(q - k0 * t.den);
    const float* row = t.rows.data() + size_t(p) * size_t(t.taps);
    float acc = 0;
    int64_t k = k0 - t.first;
    for (int j = 0; j < t.taps; ++j, ++k)
      if (k >= ws && k < we) acc += w[size_t(k - ws)] * row[j];
    const float v = std::nearbyint(acc * 32768.0f);  // libopus' float samples are -1..1
    out[i] = int16_t(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
  }
  return true;
}

AdxOpusCounts GetAdxOpusCounts() {
  AdxOpusCounts c;
  {
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    c.streams = r.counts.streams;
  }
  c.packets = g_packets.load(std::memory_order_relaxed);
  c.seeks = g_seeks.load(std::memory_order_relaxed);
  c.blocks = g_blocks.load(std::memory_order_relaxed);
  c.unknown = g_unknown.load(std::memory_order_relaxed);
  return c;
}

bool ParseAdxOpusTag(const uint8_t* p, uint32_t* id, uint32_t* block, uint32_t* channel) {
  if (p[0] != kTagMagic[0] || p[1] != kTagMagic[1] || p[2] != kTagMagic[2]) return false;
  uint32_t sum = 0;
  for (int i = 3; i < 10; ++i) {
    if (!(p[i] & 0x80)) return false;
    sum += p[i];
  }
  if (p[10] != (0x80 | (sum & 0x7F))) return false;
  for (int i = 11; i < int(kAdxBlockBytes); ++i)
    if (p[i]) return false;
  *id = (uint32_t(p[3] & 0x7F) << 14) | (uint32_t(p[4] & 0x7F) << 7) | (p[5] & 0x7F);
  *block = (uint32_t(p[6] & 0x7F) << 14) | (uint32_t(p[7] & 0x7F) << 7) | (p[8] & 0x7F);
  *channel = p[9] & 0x7F;
  return true;
}

void MakeAdxOpusTag(uint32_t id, uint32_t block, uint32_t channel, uint8_t* out) {
  out[0] = kTagMagic[0];
  out[1] = kTagMagic[1];
  out[2] = kTagMagic[2];
  out[3] = uint8_t(0x80 | ((id >> 14) & 0x7F));
  out[4] = uint8_t(0x80 | ((id >> 7) & 0x7F));
  out[5] = uint8_t(0x80 | (id & 0x7F));
  out[6] = uint8_t(0x80 | ((block >> 14) & 0x7F));
  out[7] = uint8_t(0x80 | ((block >> 7) & 0x7F));
  out[8] = uint8_t(0x80 | (block & 0x7F));
  out[9] = uint8_t(0x80 | channel);
  uint32_t sum = 0;
  for (int i = 3; i < 10; ++i) sum += out[i];
  out[10] = uint8_t(0x80 | (sum & 0x7F));
  std::memset(out + 11, 0, kAdxBlockBytes - 11);
}

}  // namespace rex::filesystem
