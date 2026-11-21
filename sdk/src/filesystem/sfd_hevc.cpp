/**
 * rr6-nx (2026-11-21): reading NAME.hevc files (rex/filesystem/sfd_hevc.h has the format): a Sofdec movie with its
 * pictures as H.265, shown to the game as the .sfd it was (its slice data zero but for the tags).
 */
#include <rex/filesystem/sfd_hevc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

#include <rex/filesystem.h>
#include <rex/filesystem/lz4_blocks.h>  // NAME.hevc.lz4b (sa2-nx-ult, rr6-nx's, 2026-11-21)
#include <rex/logging.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

constexpr char kMagic[8] = {'R', 'X', 'S', 'F', 'D', 'H', '0', '1'};
constexpr size_t kHeaderSize = 96;
constexpr size_t kSegmentSize = 32;
constexpr uint8_t kTagMagic[3] = {0xA5, 0x5A, 0xC3};
// A movie stays findable this long after its last handle closes: the player may still show what it decoded.
constexpr auto kGrace = std::chrono::seconds(10);

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
    REXFS_ERROR("[hevc] {}: {}", rex::path_to_utf8(path), why);
}

struct Header {
  uint32_t segments = 0;
  uint64_t size = 0, segments_offset = 0, stored_offset = 0, stored_length = 0;
  uint32_t pictures = 0;
  uint64_t pictures_offset = 0;
  uint32_t width = 0, height = 0, frames = 0, fps_milli = 0;
  uint64_t frames_offset = 0;
};

bool ParseHeader(const uint8_t* h, Header* out) {
  if (std::memcmp(h, kMagic, 8) != 0 || Le32(h + 8) != 1) return false;
  out->segments = Le32(h + 12);
  out->size = Le64(h + 16);
  out->segments_offset = Le64(h + 24);
  out->stored_offset = Le64(h + 32);
  out->stored_length = Le64(h + 40);
  out->pictures = Le32(h + 48);
  out->pictures_offset = Le64(h + 56);
  out->width = Le32(h + 64);
  out->height = Le32(h + 68);
  out->frames = Le32(h + 72);
  out->fps_milli = Le32(h + 76);
  out->frames_offset = Le64(h + 80);
  return out->segments >= 1 && out->segments <= (1u << 26) && out->width > 0 && out->width <= 4096 &&
         out->height > 0 && out->height <= 4096 && out->frames >= 1 && out->pictures <= (1u << 24);
}

struct Segment {
  uint64_t offset = 0, length = 0;
  uint32_t kind = 0, picture = 0;
  uint64_t source = 0;
};

class Movie final : public SfdHevcMovie {
 public:
  explicit Movie(std::unique_ptr<FileHandle> file) : file_(std::move(file)) {}
  bool ReadFrame(uint32_t index, std::vector<uint8_t>* out) const override {
    if (index >= frames.size()) return false;
    const SfdHevcFrame& f = frames[index];
    out->resize(f.size);
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadExactly(*file_, f.offset, out->data(), f.size);
  }

 private:
  std::unique_ptr<FileHandle> file_;
  mutable std::mutex mutex_;
};

struct Registry {
  std::mutex mutex;
  uint32_t next_id = 1;
  struct Entry {
    std::shared_ptr<const SfdHevcMovie> movie;
    int open = 0;
    std::chrono::steady_clock::time_point closed;
  };
  std::map<uint32_t, Entry> movies;

  void Prune() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = movies.begin(); it != movies.end();) {
      if (it->second.open == 0 && now - it->second.closed > kGrace)
        it = movies.erase(it);
      else
        ++it;
    }
  }
  uint32_t NewId() {
    for (;;) {
      const uint32_t id = next_id;
      next_id = (next_id + 1) & 0x1FFFFF;  // 21 bits, the tag's
      if (!next_id) next_id = 1;
      if (!movies.count(id)) return id;
    }
  }
};

Registry& GetRegistry() {
  static Registry r;
  return r;
}

class SfdHevcHandle final : public FileHandle {
 public:
  SfdHevcHandle(std::unique_ptr<FileHandle> packed, const Header& header, std::vector<Segment> segments,
                std::vector<uint32_t> pictures, std::shared_ptr<const SfdHevcMovie> movie)
      : FileHandle(packed->path()),
        packed_(std::move(packed)),
        header_(header),
        segments_(std::move(segments)),
        pictures_(std::move(pictures)),
        movie_(std::move(movie)) {}
  ~SfdHevcHandle() override {
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.movies.find(movie_->id);
    if (it != r.movies.end() && --it->second.open == 0) it->second.closed = std::chrono::steady_clock::now();
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
        uint8_t tag[kSfdHevcTagBytes];
        MakeSfdHevcTag(movie_->id, pictures_[seg.picture], tag);
        std::memcpy(out + done, tag + seg.source + inside, take);
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
  const std::vector<uint32_t> pictures_;
  std::shared_ptr<const SfdHevcMovie> movie_;
  std::mutex mutex_;
};

}  // namespace

bool HasSfdHevcSuffix(std::string_view name) {
  if (name.size() <= kSfdHevcSuffix.size()) return false;
  const std::string_view tail = name.substr(name.size() - kSfdHevcSuffix.size());
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != kSfdHevcSuffix[i]) return false;
  }
  return true;
}


// sa2-nx-ult (2026-11-21, the user: "we want to be able to run ZXC compressed H265 videos"; rr6-nx's Unpacked of the
// same day): NAME.hevc.lz4b (kPackedSfdHevcSuffix) is a NAME.hevc packed as LZ4 / ZXC blocks (rex/filesystem/
// lz4_blocks.h): the movie it holds is read through OpenLz4Blocks; any other handle as it is.
static bool IsPacked(const std::filesystem::path& path) {
  return HasLz4BlocksSuffix(rex::path_to_utf8(path.filename()));
}
static std::unique_ptr<FileHandle> Unpacked(std::unique_ptr<FileHandle> f) {
  if (!f || !IsPacked(f->path())) return f;
  return OpenLz4Blocks(std::move(f));  // logs why when it is not a valid .lz4b file
}
// The size of the .hevc movie at `path`: the file's own, or the one an .lz4b file holds.
static bool UnpackedSize(const std::filesystem::path& path, uint64_t* out_size) {
  if (IsPacked(path)) return ReadLz4BlocksSize(path, out_size);
  FileInfo info;
  if (!GetInfo(path, &info)) return false;
  *out_size = info.total_size;
  return true;
}

bool HasPackedSfdHevcSuffix(std::string_view name) {
  if (!HasLz4BlocksSuffix(name)) return false;
  return HasSfdHevcSuffix(name.substr(0, name.size() - kLz4BlocksSuffix.size()));
}

bool ReadSfdHevcSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = Unpacked(FileHandle::OpenExisting(path, FileAccess::kFileReadData));  // NAME.hevc.lz4b too
  if (!f) return false;
  uint8_t h[kHeaderSize];
  Header header;
  if (!ReadExactly(*f, 0, h, sizeof(h)) || !ParseHeader(h, &header)) return false;
  *out_size = header.size;
  return true;
}

bool SfdHevcSizeFromHeader(const uint8_t* h, size_t length, uint64_t* out_size) {
  Header header;
  if (length < kHeaderSize || !ParseHeader(h, &header)) return false;
  *out_size = header.size;
  return true;
}

std::unique_ptr<FileHandle> OpenSfdHevc(std::unique_ptr<FileHandle> packed) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  uint64_t size = 0;
  if (!UnpackedSize(path, &size) || size < kHeaderSize) {
    LogBad(path, "too short to be a .hevc movie");
    return nullptr;
  }
  // NAME.hevc.lz4b: both handles (the game's reads and the movie's frame reads) through the block reader
  return OpenSfdHevcFrom(Unpacked(std::move(packed)),
                         Unpacked(FileHandle::OpenExisting(path, FileAccess::kFileReadData)), size);
}

std::unique_ptr<FileHandle> OpenSfdHevcFrom(std::unique_ptr<FileHandle> packed, std::unique_ptr<FileHandle> frames,
                                            uint64_t file_size) {
  if (!packed || !frames) return nullptr;
  const std::filesystem::path path = packed->path();
  if (file_size < kHeaderSize) {
    LogBad(path, "too short to be a .hevc movie");
    return nullptr;
  }
  uint8_t h[kHeaderSize];
  Header header;
  if (!ReadExactly(*packed, 0, h, sizeof(h)) || !ParseHeader(h, &header)) {
    LogBad(path, "not a .hevc movie (header)");
    return nullptr;
  }
  auto within = [&](uint64_t offset, uint64_t length) { return offset <= file_size && length <= file_size - offset; };
  if (!within(header.segments_offset, uint64_t(header.segments) * kSegmentSize) ||
      !within(header.stored_offset, header.stored_length) ||
      !within(header.pictures_offset, uint64_t(header.pictures) * 4) ||
      !within(header.frames_offset, uint64_t(header.frames) * 16)) {
    LogBad(path, "its tables are not inside it");
    return nullptr;
  }
  std::vector<uint8_t> raw(size_t(header.segments) * kSegmentSize);
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
    s.picture = Le32(p + 20);
    s.source = Le64(p + 24);
    bool ok = s.offset == expect && s.length > 0;
    if (s.kind == 0)
      ok = ok && s.source <= header.stored_length && s.length <= header.stored_length - s.source;
    else if (s.kind == 2)
      ok = ok && s.picture < header.pictures && s.source + s.length <= kSfdHevcTagBytes;
    else
      ok = ok && s.kind == 1;
    if (!ok) {
      LogBad(path, "its segments do not cover the movie in order");
      return nullptr;
    }
    expect += s.length;
  }
  if (expect != header.size) {
    LogBad(path, "its segments do not end at its size");
    return nullptr;
  }
  std::vector<uint32_t> pictures(header.pictures);
  {
    std::vector<uint8_t> p(size_t(header.pictures) * 4);
    if (!p.empty() && !ReadExactly(*packed, header.pictures_offset, p.data(), p.size())) {
      LogBad(path, "its pictures could not be read");
      return nullptr;
    }
    for (uint32_t i = 0; i < header.pictures; ++i) pictures[i] = Le32(p.data() + size_t(i) * 4);
  }
  auto movie = std::make_shared<Movie>(std::move(frames));
  {
    std::vector<uint8_t> t(size_t(header.frames) * 16);
    if (!ReadExactly(*packed, header.frames_offset, t.data(), t.size())) {
      LogBad(path, "its frame table could not be read");
      return nullptr;
    }
    movie->frames.resize(header.frames);
    for (uint32_t i = 0; i < header.frames; ++i) {
      const uint8_t* p = t.data() + size_t(i) * 16;
      SfdHevcFrame& f = movie->frames[i];
      f.offset = Le64(p);
      f.size = Le32(p + 8);
      f.keyframe = (Le32(p + 12) & 1) != 0;
      if (!within(f.offset, f.size) || f.size == 0) {
        LogBad(path, "a frame is not inside it");
        return nullptr;
      }
    }
    if (!movie->frames[0].keyframe) {
      LogBad(path, "its first frame is not a keyframe");
      return nullptr;
    }
  }
  std::filesystem::path name = path.filename();
  name.replace_extension();
  movie->name = rex::path_to_utf8(name);
  movie->width = header.width;
  movie->height = header.height;
  movie->fps_milli = header.fps_milli;
  {
    Registry& r = GetRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.Prune();
    movie->id = r.NewId();
    auto& e = r.movies[movie->id];
    e.movie = movie;
    e.open = 1;
  }
  REXFS_INFO("[hevc] {}: {}x{}, {} H.265 frames, movie id {}", movie->name, header.width, header.height,
             header.frames, movie->id);
  return std::make_unique<SfdHevcHandle>(std::move(packed), header, std::move(segments), std::move(pictures),
                                         std::move(movie));
}

std::shared_ptr<const SfdHevcMovie> FindSfdHevcMovie(uint32_t id) {
  Registry& r = GetRegistry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto it = r.movies.find(id);
  return it == r.movies.end() ? nullptr : it->second.movie;
}

bool ParseSfdHevcTag(const uint8_t* p, uint32_t* id, uint32_t* display_index) {
  if (p[0] != kTagMagic[0] || p[1] != kTagMagic[1] || p[2] != kTagMagic[2]) return false;
  uint32_t sum = 0;
  for (int i = 3; i < 9; ++i) {
    if (!(p[i] & 0x80)) return false;
    sum += p[i];
  }
  if (p[9] != (0x80 | (sum & 0x7F))) return false;
  *id = (uint32_t(p[3] & 0x7F) << 14) | (uint32_t(p[4] & 0x7F) << 7) | (p[5] & 0x7F);
  *display_index = (uint32_t(p[6] & 0x7F) << 14) | (uint32_t(p[7] & 0x7F) << 7) | (p[8] & 0x7F);
  return true;
}

void MakeSfdHevcTag(uint32_t id, uint32_t display_index, uint8_t* out) {
  out[0] = kTagMagic[0];
  out[1] = kTagMagic[1];
  out[2] = kTagMagic[2];
  out[3] = uint8_t(0x80 | ((id >> 14) & 0x7F));
  out[4] = uint8_t(0x80 | ((id >> 7) & 0x7F));
  out[5] = uint8_t(0x80 | (id & 0x7F));
  out[6] = uint8_t(0x80 | ((display_index >> 14) & 0x7F));
  out[7] = uint8_t(0x80 | ((display_index >> 7) & 0x7F));
  out[8] = uint8_t(0x80 | (display_index & 0x7F));
  uint32_t sum = 0;
  for (int i = 3; i < 9; ++i) sum += out[i];
  out[9] = uint8_t(0x80 | (sum & 0x7F));
}

}  // namespace rex::filesystem
