/**
 * rr6-nx (2026-11-10): reading *.lz4b files (rex/filesystem/lz4_blocks.h has the format).
 */
#include <rex/filesystem/lz4_blocks.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

constexpr char kMagic[8] = {'R', 'X', 'L', 'Z', '4', 'B', 'L', 'K'};
constexpr char kEndMagic[8] = {'R', 'X', 'L', 'Z', '4', 'E', 'N', 'D'};
constexpr size_t kHeaderSize = 32;
constexpr size_t kFooterSize = 16;
// Whole blocks are read and decoded in runs of up to this much of the file (one host read each).
constexpr uint64_t kRunBytes = 4ull << 20;

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

struct Header {
  uint32_t block_size = 0;
  uint64_t size = 0;
  uint32_t block_count = 0;
};

bool ParseHeader(const uint8_t* h, Header* out) {
  if (std::memcmp(h, kMagic, 8) != 0 || Le32(h + 8) != 1) return false;
  out->block_size = Le32(h + 12);
  out->size = Le64(h + 16);
  out->block_count = Le32(h + 24);
  const uint32_t bs = out->block_size;
  if (bs < 4096 || bs > (16u << 20) || (bs & (bs - 1))) return false;
  return out->block_count == (out->size + bs - 1) / bs;
}

bool ReadExactly(FileHandle& f, uint64_t offset, void* to, size_t length) {
  size_t got = 0;
  return f.Read(size_t(offset), to, length, &got) && got == length;
}

void LogBad(const std::filesystem::path& path, const char* why) {
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
    REXLOG_ERROR("[lz4b] {}: {}", rex::path_to_utf8(path), why);
}

class Lz4BlocksHandle final : public FileHandle {
 public:
  Lz4BlocksHandle(std::unique_ptr<FileHandle> packed, const Header& header, std::vector<uint64_t> offsets)
      : FileHandle(packed->path()),
        packed_(std::move(packed)),
        size_(header.size),
        block_size_(header.block_size),
        block_count_(header.block_count),
        offsets_(std::move(offsets)) {}

  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out_bytes_read = 0;
    if (file_offset >= size_) return true;
    const size_t want = size_t(std::min<uint64_t>(buffer_length, size_ - file_offset));
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < want) {
      const uint64_t at = file_offset + done;
      const uint32_t b = uint32_t(at / block_size_);
      const uint32_t inside = uint32_t(at - uint64_t(b) * block_size_);
      const uint32_t length = BlockLength(b);
      if (inside == 0 && want - done >= length) {
        // A run of whole blocks: one host read, decoded straight into the caller's buffer.
        uint32_t end = b;
        uint64_t raw = 0;
        while (end < block_count_ && raw + BlockLength(end) <= want - done && raw < kRunBytes) raw += BlockLength(end++);
        const size_t packed_length = size_t(offsets_[end] - offsets_[b]);
        packed_buffer_.resize(packed_length);
        if (!ReadExactly(*packed_, offsets_[b], packed_buffer_.data(), packed_length)) {
          LogBad(path_, "a read of its blocks failed");
          return false;
        }
        const uint8_t* p = packed_buffer_.data();
        for (uint32_t i = b; i < end; ++i) {
          if (!Decode(i, p, out + done)) return false;
          p += offsets_[i + 1] - offsets_[i];
          done += BlockLength(i);
        }
      } else {
        // Part of a block: the whole block decoded into block_buffer_, kept for the reads that follow.
        if (cached_block_ != b) {
          const size_t stored = size_t(offsets_[b + 1] - offsets_[b]);
          packed_buffer_.resize(stored);
          block_buffer_.resize(block_size_);
          cached_block_ = UINT32_MAX;
          if (!ReadExactly(*packed_, offsets_[b], packed_buffer_.data(), stored)) {
            LogBad(path_, "a read of a block failed");
            return false;
          }
          if (!Decode(b, packed_buffer_.data(), block_buffer_.data())) return false;
          cached_block_ = b;
        }
        const size_t take = std::min<size_t>(want - done, length - inside);
        std::memcpy(out + done, block_buffer_.data() + inside, take);
        done += take;
      }
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
  uint32_t BlockLength(uint32_t b) const {
    return b + 1 < block_count_ ? block_size_ : uint32_t(size_ - uint64_t(b) * block_size_);
  }

  bool Decode(uint32_t b, const uint8_t* stored, uint8_t* to) {
    const size_t stored_size = size_t(offsets_[b + 1] - offsets_[b]);
    const uint32_t length = BlockLength(b);
    if (stored_size == length) {
      std::memcpy(to, stored, length);
      return true;
    }
    if (Lz4BlockDecode(stored, stored_size, to, length) == int64_t(length)) return true;
    LogBad(path_, "a block does not decompress");
    return false;
  }

  std::unique_ptr<FileHandle> packed_;
  const uint64_t size_;
  const uint32_t block_size_;
  const uint32_t block_count_;
  const std::vector<uint64_t> offsets_;  // block i at offsets_[i] .. offsets_[i + 1] of the packed file
  std::mutex mutex_;
  std::vector<uint8_t> packed_buffer_;
  std::vector<uint8_t> block_buffer_;
  uint32_t cached_block_ = UINT32_MAX;
};

}  // namespace

bool HasLz4BlocksSuffix(std::string_view name) {
  if (name.size() <= kLz4BlocksSuffix.size()) return false;
  const std::string_view tail = name.substr(name.size() - kLz4BlocksSuffix.size());
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != kLz4BlocksSuffix[i]) return false;
  }
  return true;
}

bool ReadLz4BlocksSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = FileHandle::OpenExisting(path, FileAccess::kFileReadData);
  if (!f) return false;
  uint8_t h[kHeaderSize];
  Header header;
  if (!ReadExactly(*f, 0, h, sizeof(h)) || !ParseHeader(h, &header)) return false;
  *out_size = header.size;
  return true;
}

std::unique_ptr<FileHandle> OpenLz4Blocks(std::unique_ptr<FileHandle> packed) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  FileInfo info;
  if (!GetInfo(path, &info) || info.total_size < kHeaderSize + kFooterSize) {
    LogBad(path, "too short to be an .lz4b file");
    return nullptr;
  }
  uint8_t h[kHeaderSize], f[kFooterSize];
  Header header;
  if (!ReadExactly(*packed, 0, h, sizeof(h)) || !ParseHeader(h, &header)) {
    LogBad(path, "not an .lz4b file (header)");
    return nullptr;
  }
  if (!ReadExactly(*packed, info.total_size - kFooterSize, f, sizeof(f)) || std::memcmp(f + 8, kEndMagic, 8) != 0) {
    LogBad(path, "not an .lz4b file (footer): cut short?");
    return nullptr;
  }
  const uint64_t index_offset = Le64(f);
  if (index_offset + uint64_t(header.block_count) * 4 + kFooterSize != info.total_size || index_offset < kHeaderSize) {
    LogBad(path, "its index is not where the footer says");
    return nullptr;
  }
  std::vector<uint8_t> index(size_t(header.block_count) * 4);
  if (!index.empty() && !ReadExactly(*packed, index_offset, index.data(), index.size())) {
    LogBad(path, "its index could not be read");
    return nullptr;
  }
  std::vector<uint64_t> offsets(size_t(header.block_count) + 1);
  offsets[0] = kHeaderSize;
  for (uint32_t i = 0; i < header.block_count; ++i) {
    const uint32_t stored = Le32(index.data() + size_t(i) * 4);
    const uint64_t length =
        i + 1 < header.block_count ? header.block_size : header.size - uint64_t(i) * header.block_size;
    if (stored == 0 || stored > length) {
      LogBad(path, "a block's stored size is not what the format allows");
      return nullptr;
    }
    offsets[i + 1] = offsets[i] + stored;
  }
  if (offsets.back() != index_offset) {
    LogBad(path, "its blocks do not end where the index starts");
    return nullptr;
  }
  static std::atomic<uint32_t> opened{0};
  if (opened.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_INFO("[lz4b] {}: {} bytes in {} blocks of {} KB, {} packed ({:.1f} %)", rex::path_to_utf8(path), header.size,
                header.block_count, header.block_size >> 10, info.total_size,
                header.size ? double(info.total_size) * 100.0 / double(header.size) : 0.0);
  return std::make_unique<Lz4BlocksHandle>(std::move(packed), header, std::move(offsets));
}

int64_t Lz4BlockDecode(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
  const uint8_t* ip = in;
  const uint8_t* const iend = in + in_size;
  uint8_t* op = out;
  uint8_t* const oend = out + out_size;
  while (ip < iend) {
    const uint32_t token = *ip++;
    size_t literals = token >> 4;
    if (literals == 15) {
      uint8_t b;
      do {
        if (ip >= iend) return -1;
        b = *ip++;
        literals += b;
      } while (b == 255);
    }
    if (literals > size_t(iend - ip) || literals > size_t(oend - op)) return -1;
    std::memcpy(op, ip, literals);
    ip += literals;
    op += literals;
    if (ip >= iend) break;  // the last sequence is literals only
    if (iend - ip < 2) return -1;
    const size_t offset = size_t(ip[0]) | (size_t(ip[1]) << 8);
    ip += 2;
    if (offset == 0 || offset > size_t(op - out)) return -1;
    size_t match = token & 15;
    if (match == 15) {
      uint8_t b;
      do {
        if (ip >= iend) return -1;
        b = *ip++;
        match += b;
      } while (b == 255);
    }
    match += 4;
    if (match > size_t(oend - op)) return -1;
    // A match may overlap its own output (a repeated pattern): copied in runs no longer than what is already there.
    const uint8_t* const from = op - offset;
    while (match) {
      const size_t run = std::min(match, size_t(op - from));
      std::memcpy(op, from, run);
      op += run;
      match -= run;
    }
  }
  return op - out;
}

}  // namespace rex::filesystem
