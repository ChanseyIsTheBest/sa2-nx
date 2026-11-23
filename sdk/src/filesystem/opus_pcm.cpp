/**
 * rr6-nx (2026-11-21): reading NAME.opcm files (rex/filesystem/opus_pcm.h has the format): the game's file with its PCM
 * samples decoded from Opus as it reads them.
 */
#include <rex/filesystem/opus_pcm.h>

#include "opus_decoder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/string.h>

REXCVAR_DEFINE_BOOL(opus_pcm_log, false, "Filesystem",
                    "One [opcm] line when a handle on a NAME.opcm closes (Opus music): its reads, the PCM made, the "
                    "packets decoded, the jumps (a loop or a seek: decoded from 320 ms before) and the time decoding")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(opus_pcm_dump, "", "Filesystem",
                      "A check: every read of a NAME.opcm is also written into this folder as NAME, at the same offset "
                      "(the bytes the game got; tools/opus_pcm_pack.py --compare measures them against the original)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::filesystem {

namespace {

constexpr char kMagic[8] = {'R', 'X', 'O', 'P', 'C', 'M', '0', '1'};
constexpr size_t kHeaderSize = 64;
constexpr size_t kSegmentSize = 32;
constexpr size_t kStreamSize = 48;
constexpr uint32_t kOpusRate = 48000;
// The resampler: 2 x kHalf taps a phase (a Kaiser-windowed sinc, beta 8, the cutoff at 97 % of the lower Nyquist).
constexpr int kHalf = 32;
constexpr int kTaps = 2 * kHalf;
constexpr double kKaiserBeta = 8.0;
constexpr double kPi = 3.14159265358979323846;
// After a jump, decoding starts this many packets (20 ms each) before the one wanted.
constexpr int64_t kPrerollPackets = 16;
// Opus packet data is read from the file this many packets at a time (about 5 s at 128 kbit/s: ~80 KB).
constexpr uint32_t kPacketBatch = 256;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
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
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
    REXFS_ERROR("[opcm] {}: {}", rex::path_to_utf8(path), why);
}

int64_t NowNs() {
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

struct Header {
  uint32_t streams = 0;
  uint64_t size = 0;
  uint32_t segments = 0;
  uint64_t segments_offset = 0, streams_offset = 0, stored_offset = 0, stored_length = 0;
};

bool ParseHeader(const uint8_t* h, Header* out) {
  if (std::memcmp(h, kMagic, 8) != 0 || Le32(h + 8) != 1) return false;
  out->streams = Le32(h + 12);
  out->size = Le64(h + 16);
  out->segments = Le32(h + 24);
  out->segments_offset = Le64(h + 32);
  out->streams_offset = Le64(h + 40);
  out->stored_offset = Le64(h + 48);
  out->stored_length = Le64(h + 56);
  return out->streams <= 4096 && out->segments >= 1 && out->segments <= (1u << 24);
}

struct Segment {
  uint64_t offset = 0, length = 0;
  uint32_t kind = 0, stream = 0;
  uint64_t source = 0;
};

// The polyphase filter from 48 kHz to `rate`: output sample n sits at input position n * M / L; its phase
// (n * M) mod L picks kTaps coefficients for the input samples floor(n * M / L) - kHalf + 1 onwards.
struct Filter {
  uint32_t L = 1, M = 1;
  std::vector<float> h;  // L x kTaps
};

double BesselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 64; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

std::shared_ptr<const Filter> GetFilter(uint32_t rate) {
  static std::mutex mutex;
  static std::map<uint32_t, std::shared_ptr<const Filter>> filters;
  std::lock_guard<std::mutex> lock(mutex);
  auto it = filters.find(rate);
  if (it != filters.end()) return it->second;
  uint32_t a = rate, b = kOpusRate;
  while (b) {
    const uint32_t t = a % b;
    a = b;
    b = t;
  }
  auto f = std::make_shared<Filter>();
  f->L = rate / a;
  f->M = kOpusRate / a;
  const double fc = 0.5 * double(std::min(rate, kOpusRate)) / double(kOpusRate) * 0.97;  // cycles an input sample
  const double i0b = BesselI0(kKaiserBeta);
  f->h.resize(size_t(f->L) * kTaps);
  for (uint32_t phase = 0; phase < f->L; ++phase) {
    const double frac = double(phase) / double(f->L);
    double sum = 0.0;
    float* h = f->h.data() + size_t(phase) * kTaps;
    for (int k = 0; k < kTaps; ++k) {
      const double t = double(k - kHalf + 1) - frac;  // the tap's input sample, from the output's position
      const double x = 2.0 * fc * t;
      const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      const double r = t / double(kHalf);
      const double w = std::abs(r) >= 1.0 ? 0.0 : BesselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0b;
      const double v = 2.0 * fc * sinc * w;
      h[k] = float(v);
      sum += v;
    }
    for (int k = 0; k < kTaps; ++k) h[k] = float(h[k] / sum);  // unity gain at DC in every phase
  }
  filters[rate] = f;
  return f;
}

struct Stream {
  uint16_t channels = 0, bits = 0;
  uint32_t rate = 0, flags = 0, pre_skip = 0;
  uint64_t frames = 0;
  uint32_t packets = 0, frame_size = 0;
  uint64_t sizes_offset = 0, packets_offset = 0;
  std::vector<uint64_t> packet_offset;  // packets + 1 entries, absolute in the .opcm
  std::shared_ptr<const Filter> filter;  // null at 48 kHz
};

class OpusPcmHandle final : public FileHandle {
 public:
  OpusPcmHandle(std::unique_ptr<FileHandle> packed, const Header& header, std::vector<Segment> segments,
                std::vector<Stream> streams)
      : FileHandle(packed->path()),
        packed_(std::move(packed)),
        header_(header),
        segments_(std::move(segments)),
        streams_(std::move(streams)),
        sessions_(streams_.size()) {
    static const std::string dump = REXCVAR_GET(opus_pcm_dump);
    if (!dump.empty()) {
      std::filesystem::path name = path_.filename();
      name.replace_extension();  // NAME.opcm -> NAME
      dump_path_ = rex::to_path(dump) / name;
    }
  }
  ~OpusPcmHandle() override {
    static const bool log = REXCVAR_GET(opus_pcm_log);
    if (log && reads_)
      REXFS_INFO("[opcm] {}: {} reads, {:.1f} MB of PCM made, {} Opus packets decoded ({} jumps) in {:.0f} ms, "
                 "{:.0f} ms resampling",
                 rex::path_to_utf8(path_.filename()), reads_, double(pcm_bytes_) / 1048576.0, packets_decoded_,
                 jumps_, double(decode_ns_) / 1e6, double(resample_ns_) / 1e6);
    for (auto& s : sessions_) opus::Destroy(s.decoder);
  }

  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out_bytes_read = 0;
    if (file_offset >= header_.size) return true;
    ++reads_;
    const size_t want = size_t(std::min<uint64_t>(buffer_length, header_.size - file_offset));
    auto* out = static_cast<uint8_t*>(buffer);
    // The segment the read starts in: the last one that starts at or before it.
    size_t si = size_t(std::upper_bound(segments_.begin(), segments_.end(), uint64_t(file_offset),
                                        [](uint64_t v, const Segment& s) { return v < s.offset; }) -
                       segments_.begin());
    if (si == 0) return false;
    --si;
    size_t done = 0;
    while (done < want && si < segments_.size()) {
      const Segment& seg = segments_[si];
      const uint64_t at = file_offset + done;
      const uint64_t inside = at - seg.offset;
      const size_t take = size_t(std::min<uint64_t>(want - done, seg.length - inside));
      if (seg.kind == 0) {
        if (!ReadExactly(*packed_, header_.stored_offset + seg.source + inside, out + done, take)) {
          LogBad(path_, "a read of its stored bytes failed");
          return false;
        }
      } else if (!MakePcm(seg.stream, seg.source + inside, out + done, take)) {
        return false;
      }
      done += take;
      ++si;
    }
    *out_bytes_read = done;
    if (!dump_path_.empty()) Dump(file_offset, out, done);
    return true;
  }

  bool Write(size_t, const void*, size_t, size_t* out_bytes_written) override {
    *out_bytes_written = 0;
    return false;
  }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  // The decoder of one stream: the 48 kHz samples decoded so far that later reads may need, as interleaved floats.
  struct Session {
    opus::Decoder* decoder = nullptr;
    bool tried = false;     // Create was called (null: no libopus)
    bool valid = false;     // `next` and `buf` continue one decode
    int64_t next = 0;       // the next packet to decode
    int64_t buf_d0 = 0;     // the decoded sample (pre-skip counted) of buf's first frame
    std::vector<float> buf;
    bool announced = false;
  };

  // `length` bytes of stream `index`'s PCM from byte `byte_offset` into `out`.
  bool MakePcm(uint32_t index, uint64_t byte_offset, uint8_t* out, size_t length) {
    if (index >= streams_.size()) {
      LogBad(path_, "a segment names a stream it does not have");
      return false;
    }
    const Stream& st = streams_[index];
    Session& se = sessions_[index];
    if (!se.announced) {
      se.announced = true;
      REXFS_INFO("[opcm] {}: stream {} ({} Hz, {} ch, {:.0f} s) from Opus", rex::path_to_utf8(path_.filename()), index,
                 st.rate, st.channels, double(st.frames) / double(st.rate));
    }
    const uint32_t frame_bytes = uint32_t(st.channels) * 2;
    const uint64_t f0 = byte_offset / frame_bytes;
    const uint64_t f1 = std::min<uint64_t>((byte_offset + length + frame_bytes - 1) / frame_bytes, st.frames);
    pcm_.assign(size_t(f1 > f0 ? f1 - f0 : 0) * st.channels, 0);
    if (f1 > f0) Synthesize(st, se, f0, f1, pcm_.data());
    pcm_bytes_ += length;
    // The bytes asked for, in the file's byte order; past the stream's last frame: zero.
    const uint8_t* src = reinterpret_cast<const uint8_t*>(pcm_.data());
    const size_t have = pcm_.size() * 2;
    const size_t skip = size_t(byte_offset - f0 * frame_bytes);
    const bool big = (st.flags & 1) != 0;
    for (size_t i = 0; i < length; ++i) {
      const size_t k = skip + i;
      if (k >= have) {
        out[i] = 0;
        continue;
      }
      const uint16_t v = uint16_t(pcm_[k >> 1]);
      // k's byte of sample k / 2: little-endian low byte first, big-endian high byte first.
      out[i] = uint8_t(((k & 1) != big) ? (v >> 8) : (v & 0xFF));
    }
    return true;
  }

  // PCM frames [f0, f1) of the stream, interleaved int16.
  void Synthesize(const Stream& st, Session& se, uint64_t f0, uint64_t f1, int16_t* out) {
    const int ch = st.channels;
    int64_t m_lo, m_hi;  // the 48 kHz samples (after the pre-skip) the frames need
    const Filter* filt = st.filter.get();
    if (!filt) {
      m_lo = int64_t(f0);
      m_hi = int64_t(f1);
    } else {
      m_lo = int64_t((f0 * filt->M) / filt->L) - kHalf + 1;
      m_hi = int64_t(((f1 - 1) * filt->M) / filt->L) + kHalf + 1;
    }
    if (!Ensure(st, se, m_lo, m_hi)) return;  // no decoder: silence (warned once)
    const int64_t pre = st.pre_skip;
    const int64_t have = int64_t(se.buf.size() / size_t(ch));
    // Sample m of channel c, 0 outside what was decoded (before the start, past the end).
    auto at = [&](int64_t m, int c) -> float {
      const int64_t k = m + pre - se.buf_d0;
      return (k >= 0 && k < have) ? se.buf[size_t(k) * ch + c] : 0.0f;
    };
    auto to16 = [](float v) -> int16_t {
      const float s = v * 32768.0f;
      return int16_t(std::lrint(std::clamp(s, -32768.0f, 32767.0f)));
    };
    const int64_t t0 = NowNs();
    if (!filt) {
      for (uint64_t f = f0; f < f1; ++f)
        for (int c = 0; c < ch; ++c) *out++ = to16(at(int64_t(f), c));
    } else {
      float acc[2];
      for (uint64_t f = f0; f < f1; ++f) {
        const uint64_t q = f * filt->M;
        const int64_t i = int64_t(q / filt->L);
        const float* h = filt->h.data() + size_t(q % filt->L) * kTaps;
        const int64_t j0 = i - kHalf + 1;
        const int64_t k0 = j0 + pre - se.buf_d0;
        acc[0] = acc[1] = 0.0f;
        if (k0 >= 0 && k0 + kTaps <= have) {
          const float* x = se.buf.data() + size_t(k0) * ch;
          if (ch == 2) {
            for (int t = 0; t < kTaps; ++t) {
              acc[0] += h[t] * x[2 * t];
              acc[1] += h[t] * x[2 * t + 1];
            }
          } else {
            for (int t = 0; t < kTaps; ++t) acc[0] += h[t] * x[t];
          }
        } else {
          for (int t = 0; t < kTaps; ++t)
            for (int c = 0; c < ch; ++c) acc[c] += h[t] * at(j0 + t, c);
        }
        for (int c = 0; c < ch; ++c) *out++ = to16(acc[c]);
      }
    }
    resample_ns_ += NowNs() - t0;
  }

  // Makes the session's buffer hold the 48 kHz samples [m_lo, m_hi) (as far as the stream has them). False without
  // a decoder.
  bool Ensure(const Stream& st, Session& se, int64_t m_lo, int64_t m_hi) {
    if (!se.tried) {
      se.tried = true;
      se.decoder = opus::Create(st.channels);
    }
    if (!se.decoder) return false;
    const int ch = st.channels;
    const int64_t fs = st.frame_size;
    const int64_t total = int64_t(st.packets) * fs;
    const int64_t d_lo = std::clamp<int64_t>(m_lo + st.pre_skip, 0, total);
    const int64_t d_hi = std::clamp<int64_t>(m_hi + st.pre_skip, 0, total);
    if (d_hi <= d_lo) return true;
    // Go on from the last decode when the read starts inside what it holds or a little after (as cheap as the
    // pre-roll); otherwise start again 320 ms before the packet wanted.
    if (!(se.valid && d_lo >= se.buf_d0 && d_lo <= (se.next + kPrerollPackets) * fs)) {
      opus::Reset(se.decoder);
      se.next = std::max<int64_t>(0, d_lo / fs - kPrerollPackets);
      se.buf.clear();
      se.buf_d0 = se.next * fs;
      se.valid = true;
      ++jumps_;
    }
    const int64_t t0 = NowNs();
    std::vector<float> frame(size_t(fs) * ch);
    while (se.next * fs < d_hi && se.next < int64_t(st.packets)) {
      const uint32_t p = uint32_t(se.next);
      const uint8_t* data = nullptr;
      int size = 0;
      if (!PacketData(st, p, &data, &size)) {
        std::fill(frame.begin(), frame.end(), 0.0f);
      } else {
        const int n = opus::Decode(se.decoder, data, size, frame.data(), int(fs));
        if (n != fs) {
          static std::atomic<uint32_t> logged{0};
          if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
            REXFS_ERROR("[opcm] {}: packet {} decoded to {} samples (not {})", rex::path_to_utf8(path_.filename()), p,
                        n, fs);
          std::fill(frame.begin(), frame.end(), 0.0f);
        }
      }
      se.buf.insert(se.buf.end(), frame.begin(), frame.end());
      ++se.next;
      ++packets_decoded_;
    }
    decode_ns_ += NowNs() - t0;
    // Drop what comes before the read: the reads after it start at or after it (a game reads its music in order).
    if (d_lo > se.buf_d0) {
      const int64_t drop = std::min<int64_t>(d_lo - se.buf_d0, int64_t(se.buf.size() / size_t(ch)));
      se.buf.erase(se.buf.begin(), se.buf.begin() + size_t(drop) * ch);
      se.buf_d0 += drop;
    }
    return true;
  }

  // Packet `p`'s bytes, from the batch held (read from the file when it does not hold them).
  bool PacketData(const Stream& st, uint32_t p, const uint8_t** data, int* size) {
    const uint64_t begin = st.packet_offset[p], end = st.packet_offset[p + 1];
    if (!(cache_stream_ == &st && begin >= cache_begin_ && end <= cache_begin_ + cache_.size())) {
      const uint32_t last = std::min<uint32_t>(st.packets, p + kPacketBatch);
      const uint64_t to = st.packet_offset[last];
      cache_.resize(size_t(to - begin));
      cache_stream_ = nullptr;
      if (!ReadExactly(*packed_, begin, cache_.data(), cache_.size())) {
        LogBad(path_, "a read of its Opus packets failed");
        return false;
      }
      cache_stream_ = &st;
      cache_begin_ = begin;
    }
    *data = cache_.data() + (begin - cache_begin_);
    *size = int(end - begin);
    return true;
  }

  void Dump(size_t offset, const uint8_t* data, size_t length) {
    std::FILE* f = std::fopen(rex::path_to_utf8(dump_path_).c_str(), "r+b");
    if (!f) f = std::fopen(rex::path_to_utf8(dump_path_).c_str(), "w+b");
    if (!f) return;
    std::fseek(f, long(offset), SEEK_SET);
    std::fwrite(data, 1, length, f);
    std::fclose(f);
  }

  std::unique_ptr<FileHandle> packed_;
  const Header header_;
  const std::vector<Segment> segments_;
  const std::vector<Stream> streams_;
  std::vector<Session> sessions_;
  std::mutex mutex_;
  std::vector<int16_t> pcm_;
  std::vector<uint8_t> cache_;
  const Stream* cache_stream_ = nullptr;
  uint64_t cache_begin_ = 0;
  std::filesystem::path dump_path_;
  uint64_t reads_ = 0, pcm_bytes_ = 0, packets_decoded_ = 0, jumps_ = 0;
  int64_t decode_ns_ = 0, resample_ns_ = 0;
};

}  // namespace

bool HasOpusPcmSuffix(std::string_view name) {
  if (name.size() <= kOpusPcmSuffix.size()) return false;
  const std::string_view tail = name.substr(name.size() - kOpusPcmSuffix.size());
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != kOpusPcmSuffix[i]) return false;
  }
  return true;
}

bool ReadOpusPcmSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = FileHandle::OpenExisting(path, FileAccess::kFileReadData);
  if (!f) return false;
  uint8_t h[kHeaderSize];
  Header header;
  if (!ReadExactly(*f, 0, h, sizeof(h)) || !ParseHeader(h, &header)) return false;
  *out_size = header.size;
  return true;
}

std::unique_ptr<FileHandle> OpenOpusPcm(std::unique_ptr<FileHandle> packed) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  FileInfo info;
  if (!GetInfo(path, &info) || info.total_size < kHeaderSize) {
    LogBad(path, "too short to be an .opcm file");
    return nullptr;
  }
  const uint64_t file_size = info.total_size;
  uint8_t h[kHeaderSize];
  Header header;
  if (!ReadExactly(*packed, 0, h, sizeof(h)) || !ParseHeader(h, &header)) {
    LogBad(path, "not an .opcm file (header)");
    return nullptr;
  }
  auto within = [&](uint64_t offset, uint64_t length) {
    return offset <= file_size && length <= file_size - offset;
  };
  if (!within(header.segments_offset, uint64_t(header.segments) * kSegmentSize) ||
      !within(header.streams_offset, uint64_t(header.streams) * kStreamSize) ||
      !within(header.stored_offset, header.stored_length)) {
    LogBad(path, "its tables are not inside it");
    return nullptr;
  }
  std::vector<uint8_t> raw(size_t(header.segments) * kSegmentSize);
  if (!ReadExactly(*packed, header.segments_offset, raw.data(), raw.size())) {
    LogBad(path, "its segments could not be read");
    return nullptr;
  }
  std::vector<Stream> streams(header.streams);
  {
    std::vector<uint8_t> s(size_t(header.streams) * kStreamSize);
    if (!s.empty() && !ReadExactly(*packed, header.streams_offset, s.data(), s.size())) {
      LogBad(path, "its streams could not be read");
      return nullptr;
    }
    for (uint32_t i = 0; i < header.streams; ++i) {
      const uint8_t* p = s.data() + size_t(i) * kStreamSize;
      Stream& st = streams[i];
      st.channels = Le16(p);
      st.bits = Le16(p + 2);
      st.rate = Le32(p + 4);
      st.flags = Le32(p + 8);
      st.pre_skip = Le32(p + 12);
      st.frames = Le64(p + 16);
      st.packets = Le32(p + 24);
      st.frame_size = Le32(p + 28);
      st.sizes_offset = Le64(p + 32);
      st.packets_offset = Le64(p + 40);
      if ((st.channels != 1 && st.channels != 2) || st.bits != 16 || st.rate < 8000 || st.rate > 192000 ||
          st.frame_size == 0 || st.frame_size > 5760 || !within(st.sizes_offset, uint64_t(st.packets) * 2)) {
        LogBad(path, "a stream is not one the format allows");
        return nullptr;
      }
      std::vector<uint8_t> sizes(size_t(st.packets) * 2);
      if (!sizes.empty() && !ReadExactly(*packed, st.sizes_offset, sizes.data(), sizes.size())) {
        LogBad(path, "a stream's packet sizes could not be read");
        return nullptr;
      }
      st.packet_offset.resize(size_t(st.packets) + 1);
      st.packet_offset[0] = st.packets_offset;
      for (uint32_t k = 0; k < st.packets; ++k) st.packet_offset[k + 1] = st.packet_offset[k] + Le16(&sizes[size_t(k) * 2]);
      if (!within(st.packets_offset, st.packet_offset.back() - st.packets_offset)) {
        LogBad(path, "a stream's packets are not inside it");
        return nullptr;
      }
      if (st.rate != kOpusRate) st.filter = GetFilter(st.rate);
    }
  }
  std::vector<Segment> segments(header.segments);
  uint64_t expect = 0;
  for (uint32_t i = 0; i < header.segments; ++i) {
    const uint8_t* p = raw.data() + size_t(i) * kSegmentSize;
    Segment& seg = segments[i];
    seg.offset = Le64(p);
    seg.length = Le64(p + 8);
    seg.kind = Le32(p + 16);
    seg.stream = Le32(p + 20);
    seg.source = Le64(p + 24);
    const bool ok = seg.offset == expect && seg.length > 0 &&
                    (seg.kind == 0 ? seg.source <= header.stored_length &&
                                         seg.length <= header.stored_length - seg.source
                                   : seg.kind == 1 && seg.stream < header.streams);
    if (!ok) {
      LogBad(path, "its segments do not cover the file in order");
      return nullptr;
    }
    expect += seg.length;
  }
  if (expect != header.size) {
    LogBad(path, "its segments do not end at its size");
    return nullptr;
  }
  return std::make_unique<OpusPcmHandle>(std::move(packed), header, std::move(segments), std::move(streams));
}

}  // namespace rex::filesystem
