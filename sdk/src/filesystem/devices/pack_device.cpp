/**
 * @file        rex/filesystem/devices/pack_device.cpp
 * @brief       Read-only game data served from one pack file (see pack_device.h)
 *
 * Ported from Fruit Ninja NX's asset_pack.c (format version 2, byte for byte the same files): the index loading and
 * its checks, the 64 KB read window per open file, and the first-boot builder with its read-back verification. What
 * differs is the plumbing: there the pack sat behind fake file descriptors in a libc shim; here it is a ReXGlue VFS
 * device, so the game's NtCreateFile / NtReadFile reach it through the normal entry tree.
 */

#include <rex/filesystem/devices/pack_device.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <system_error>
#include <unordered_map>

#include <rex/filesystem.h>
#include <rex/filesystem/adx_opus.h>    // sa2-nx-ult (2026-11-21): NAME.adxo entries
#include <rex/filesystem/lz4_blocks.h>  // sa2-nx-ult (2026-11-24): the stand-ins a pack cannot serve (below)
#include <rex/filesystem/opus_pcm.h>
#include <rex/filesystem/sfd_hevc.h>    // sa2-nx-ult (2026-11-21): NAME.hevc entries
#include <rex/filesystem/xmo.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

namespace {

// sa2-nx-ult (2026-11-24): version 3 (pack_device.h), what Build and the web installer write; 2 still loads.
constexpr uint32_t kPackVersion = 3;
constexpr uint32_t kPackVersionOld = 2;
constexpr uint32_t kMaxEntries = 100000;
constexpr uint64_t kMaxPathsSize = 64ull << 20;
constexpr size_t kCacheSize = 64 * 1024;       // asset_pack.c PACK_CACHE_SIZE
constexpr size_t kChunk = 1 << 20;             // big reads go in 1 MB pieces, so other readers interleave
constexpr uint64_t kFnvBasis = 1469598103934665603ULL;

// sa2-nx-ult (2026-11-24): the stand-ins a pack does not serve: NAME.opcm, NAME.xmo, NAME.xmop, packed as .lz4b or
// not (the host path device reads them; SA2 has none). A folder holding any is never packed, and a pack holding any
// is not used (the folder is mounted). Before version 3 NAME.lz4b was one of them: the first boot packed the web
// installer's folder file for file, and the game then found none of its files (the console's crash of 2026-11-24 at
// boot: 3,550 .lz4b entries in a version-2 game_root.nxpack); such a pack is still not used (Initialize).
bool NeverInPack(std::string_view path) {
  if (HasLz4BlocksSuffix(path)) path.remove_suffix(kLz4BlocksSuffix.size());
  return HasOpusPcmSuffix(path) || HasXmoSuffix(path) || HasXmopSuffix(path);
}

// What an entry of the pack stands for: NAME.lz4b, NAME.hevc[.lz4b] or NAME.adxo[.lz4b] for NAME (`plain`).
struct StandIn {
  bool lz4 = false, hevc = false, adxo = false;
  std::string plain;  // the guest's name (the path without the suffixes); the path itself for a plain file
  bool any() const { return lz4 || hevc || adxo; }
};
StandIn StandInOf(std::string_view path) {
  StandIn s;
  std::string_view n = path;
  if (HasLz4BlocksSuffix(n)) {
    s.lz4 = true;
    n.remove_suffix(kLz4BlocksSuffix.size());
  }
  if (HasSfdHevcSuffix(n)) {
    s.hevc = true;
    n.remove_suffix(kSfdHevcSuffix.size());
  } else if (HasAdxOpusSuffix(n)) {
    s.adxo = true;
    n.remove_suffix(kAdxOpusSuffix.size());
  }
  s.plain = std::string(n);
  return s;
}

// The size of the file an .lz4b holds, from its 32-byte header ("RXLZ4BLK", version, block size, u64 size).
bool Lz4BlocksSizeFromHeader(const uint8_t* h, uint64_t* out_size) {
  if (std::memcmp(h, "RXLZ4BLK", 8) != 0) return false;
  std::memcpy(out_size, h + 16, sizeof(*out_size));
  return true;
}

// Build: the size the game sees of a host file that is a stand-in (StandInOf), from its header (through
// OpenLz4Blocks for NAME.hevc.lz4b / NAME.adxo.lz4b). False if it is not a valid one.
bool GuestSizeOf(const std::filesystem::path& source, const StandIn& s, uint64_t* out_size) {
  if (!s.hevc && !s.adxo) return ReadLz4BlocksSize(source, out_size);  // a plain NAME.lz4b
  auto handle = FileHandle::OpenExisting(source, FileAccess::kFileReadData);
  if (handle && s.lz4) handle = OpenLz4Blocks(std::move(handle));
  if (!handle) return false;
  uint8_t header[kSfdHevcHeaderBytes];
  static_assert(kSfdHevcHeaderBytes >= kAdxOpusHeaderBytes);
  const size_t want = s.hevc ? kSfdHevcHeaderBytes : kAdxOpusHeaderBytes;
  size_t got = 0;
  if (!handle->Read(0, header, want, &got) || got != want) return false;
  return s.hevc ? SfdHevcSizeFromHeader(header, want, out_size) : AdxOpusSizeFromHeader(header, want, out_size);
}

struct PackHeader {
  char magic[8];
  uint32_t version;
  uint32_t reserved;
  uint64_t pack_id;
  uint64_t file_size;
  uint64_t data_checksum;
};
struct IndexHeader {
  char magic[8];
  uint32_t version;
  uint32_t count;
  uint64_t pack_id;
  uint64_t pack_size;
  uint64_t paths_size;
  uint64_t checksum;
  uint64_t data_checksum;
};
struct DiskEntry {
  uint64_t offset;
  uint64_t size;
  uint32_t path_offset;
  uint32_t path_length;
};
// sa2-nx-ult (2026-11-24): version 3's entry, version 2's and the size the game sees.
struct DiskEntry3 {
  uint64_t offset;
  uint64_t size;
  uint32_t path_offset;
  uint32_t path_length;
  uint64_t guest_size;
};
static_assert(sizeof(PackHeader) == 40, "asset_pack.c PackHeader layout");
static_assert(sizeof(IndexHeader) == 56, "asset_pack.c IndexHeader layout");
static_assert(sizeof(DiskEntry) == 24, "asset_pack.c DiskEntry layout");
static_assert(sizeof(DiskEntry3) == 32, "version 3's DiskEntry layout");

uint64_t Fnv(uint64_t hash, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

// asset_pack.c valid_relative: no empty, "." or ".." component, no backslash or colon, no leading/trailing '/'.
bool ValidRelative(const char* path, size_t length) {
  if (!path || !length || path[0] == '/' || path[length - 1] == '/') return false;
  size_t part = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i == length || path[i] == '/') {
      if (!part || (part == 1 && path[i - 1] == '.') ||
          (part == 2 && path[i - 2] == '.' && path[i - 1] == '.')) return false;
      part = 0;
    } else {
      if (path[i] == '\\' || path[i] == ':') return false;
      ++part;
    }
  }
  return true;
}

// Portable storage: one FILE* and a lock, each 1 MB piece seeked and read under it.
class PackStorageFile final : public PackStorage {
 public:
  explicit PackStorageFile(FILE* file) : file_(file) {}
  ~PackStorageFile() override {
    if (file_) std::fclose(file_);
  }
  bool ReadAt(uint64_t offset, void* buffer, size_t size) override {
    auto* out = static_cast<uint8_t*>(buffer);
    while (size) {
      const size_t piece = std::min(size, kChunk);
      size_t read = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!rex::filesystem::Seek(file_, int64_t(offset), SEEK_SET)) return false;
        read = std::fread(out, 1, piece, file_);
      }
      if (read == 0) return false;  // a short read continues from where it stopped (see pack_storage_switch.cpp)
      out += read;
      offset += read;
      size -= read;
    }
    return true;
  }

 private:
  FILE* file_;
  std::mutex mutex_;
};

struct LoadedIndex {
  IndexHeader header{};
  std::vector<DiskEntry> entries;
  std::vector<char> paths;
  std::vector<uint64_t> guest_sizes;  // sa2-nx-ult: version 3's, one per entry (empty for version 2)
};

// asset_pack.c load_pair: the pack header and the index must come from one run of the packer, the index checksum
// must match, and every entry must be in bounds, valid and in strictly increasing path order.
bool LoadIndex(const std::filesystem::path& pack_path, const std::filesystem::path& index_path, LoadedIndex* out,
               std::string* error) {
  std::error_code ec;
  const uint64_t pack_size = std::filesystem::file_size(pack_path, ec);
  if (ec) return *error = "no asset pack", false;
  const uint64_t index_size = std::filesystem::file_size(index_path, ec);
  if (ec) return *error = "no asset index", false;
  PackHeader pack{};
  FILE* f = rex::filesystem::OpenFile(pack_path, "rb");
  if (!f) return *error = "the asset pack cannot be opened", false;
  const bool pack_read = std::fread(&pack, sizeof(pack), 1, f) == 1;
  std::fclose(f);
  f = rex::filesystem::OpenFile(index_path, "rb");
  if (!f) return *error = "the asset index cannot be opened", false;
  IndexHeader& h = out->header;
  bool ok = pack_read && std::fread(&h, sizeof(h), 1, f) == 1 && !std::memcmp(pack.magic, "SSNXPAK1", 8) &&
            !std::memcmp(h.magic, "SSNXIDX1", 8) &&
            (pack.version == kPackVersion || pack.version == kPackVersionOld) && h.version == pack.version &&
            pack.pack_id == h.pack_id && pack.file_size == pack_size && pack.data_checksum == h.data_checksum &&
            h.pack_size == pack_size && h.count && h.count <= kMaxEntries && h.paths_size &&
            h.paths_size <= kMaxPathsSize;
  if (!ok) {
    std::fclose(f);
    return *error = "the asset pack header is invalid", false;
  }
  const bool v3 = h.version == kPackVersion;  // sa2-nx-ult (2026-11-24): 32-byte entries with the guest's size
  const size_t entry_bytes = v3 ? sizeof(DiskEntry3) : sizeof(DiskEntry);
  const uint64_t entries_size = uint64_t(h.count) * entry_bytes;
  if (sizeof(IndexHeader) + entries_size + h.paths_size != index_size) {
    std::fclose(f);
    return *error = "the asset index size is invalid", false;
  }
  std::vector<uint8_t> raw(static_cast<size_t>(entries_size));
  out->paths.resize(size_t(h.paths_size));
  ok = std::fread(raw.data(), 1, raw.size(), f) == raw.size() &&
       std::fread(out->paths.data(), 1, out->paths.size(), f) == out->paths.size();
  std::fclose(f);
  if (!ok) return *error = "the asset index cannot be read", false;
  uint64_t checksum = Fnv(kFnvBasis, raw.data(), raw.size());
  checksum = Fnv(checksum, out->paths.data(), out->paths.size());
  if (checksum != h.checksum) return *error = "the asset index checksum is invalid", false;
  out->entries.resize(h.count);
  if (v3) {
    out->guest_sizes.resize(h.count);
    for (uint32_t i = 0; i < h.count; ++i) {
      DiskEntry3 e3;
      std::memcpy(&e3, raw.data() + size_t(i) * sizeof(DiskEntry3), sizeof(e3));
      out->entries[i] = {e3.offset, e3.size, e3.path_offset, e3.path_length};
      out->guest_sizes[i] = e3.guest_size;
    }
  } else {
    std::memcpy(out->entries.data(), raw.data(), raw.size());
  }
  for (uint32_t i = 0; i < h.count; ++i) {
    const DiskEntry& e = out->entries[i];
    const uint64_t path_end = uint64_t(e.path_offset) + e.path_length + 1;
    if (!e.path_length || path_end > h.paths_size || out->paths[e.path_offset + e.path_length] != 0 ||
        !ValidRelative(out->paths.data() + e.path_offset, e.path_length) || e.offset < sizeof(PackHeader) ||
        e.offset > h.pack_size || e.size > h.pack_size - e.offset ||
        (i && std::strcmp(out->paths.data() + out->entries[i - 1].path_offset,
                          out->paths.data() + e.path_offset) >= 0)) {
      return *error = "the asset index contains an invalid entry", false;
    }
  }
  return true;
}

}  // namespace

#if !defined(__SWITCH__)
std::unique_ptr<PackStorage> PackStorage::Open(const std::filesystem::path& path) {
  FILE* file = rex::filesystem::OpenFile(path, "rb");
  if (!file) return nullptr;
  return std::make_unique<PackStorageFile>(file);
}
#endif

// ---------------------------------------------------------------------------------------------------------------
// Entry and file

namespace {

// sa2-nx-ult (2026-11-21): a range of the pack as a FileHandle (positional reads; PackStorage is thread safe), for the
// readers of re-encoded entries (rex/filesystem/sfd_hevc.h).
class PackRangeHandle final : public FileHandle {
 public:
  PackRangeHandle(PackStorage* storage, uint64_t offset, uint64_t size, const std::filesystem::path& name)
      : FileHandle(name), storage_(storage), offset_(offset), size_(size) {}
  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    *out_bytes_read = 0;
    if (file_offset >= size_) return buffer_length == 0;
    const size_t n = size_t(std::min<uint64_t>(buffer_length, size_ - file_offset));
    if (!storage_->ReadAt(offset_ + file_offset, buffer, n)) return false;
    *out_bytes_read = n;
    return n == buffer_length;
  }
  bool Write(size_t, const void*, size_t, size_t* out_bytes_written) override {
    *out_bytes_written = 0;
    return false;
  }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  PackStorage* storage_;
  uint64_t offset_, size_;
};

// A file read through a FileHandle (the movie a NAME.hevc entry stands for).
class PackHandleFile final : public File {
 public:
  PackHandleFile(uint32_t file_access, Entry* entry, std::unique_ptr<FileHandle> handle)
      : File(file_access, entry), handle_(std::move(handle)) {}
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override {
    *out_bytes_read = 0;
    const size_t size = entry()->size();
    if (byte_offset >= size) return X_STATUS_END_OF_FILE;
    const size_t n = std::min(buffer.size(), size - byte_offset);
    size_t got = 0;
    if (!handle_->Read(byte_offset, buffer.data(), n, &got)) return X_STATUS_UNSUCCESSFUL;
    *out_bytes_read = got;
    return X_STATUS_SUCCESS;
  }
  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t*) override { return X_STATUS_ACCESS_DENIED; }
  X_STATUS SetLength(size_t) override { return X_STATUS_ACCESS_DENIED; }

 private:
  std::unique_ptr<FileHandle> handle_;
};

}  // namespace

PackEntry::PackEntry(Device* device, Entry* parent, const std::string_view path) : Entry(device, parent, path) {}
PackEntry::~PackEntry() = default;

X_STATUS PackEntry::Open(uint32_t desired_access, File** out_file) {
  if (desired_access & (FileAccess::kFileWriteData | FileAccess::kFileAppendData)) {
    REXFS_ERROR("Attempting to open {} for write access on the read-only asset pack", path());
    return X_STATUS_ACCESS_DENIED;
  }
  if (sfd_hevc_ || adx_opus_ || lz4_blocks_) {
    // sa2-nx-ult (2026-11-21): the file made from the stored NAME.hevc / NAME.adxo; (2026-11-24) and from a stored
    // NAME.lz4b (LZ4 / ZXC blocks) of NAME, of NAME.hevc or of NAME.adxo, through OpenLz4Blocks.
    PackStorage* storage = pack_->storage();
    std::string stored_name(path());
    if (sfd_hevc_) stored_name += kSfdHevcSuffix;
    if (adx_opus_) stored_name += kAdxOpusSuffix;
    if (lz4_blocks_) stored_name += kLz4BlocksSuffix;
    const std::filesystem::path name = rex::to_path(stored_name);
    const auto range = [&]() -> std::unique_ptr<FileHandle> {
      auto handle = std::make_unique<PackRangeHandle>(storage, data_offset_, stored_size_, name);
      if (!lz4_blocks_) return handle;
      return OpenLz4Blocks(std::move(handle), stored_size_);  // nullptr (logged) when not an .lz4b
    };
    std::unique_ptr<FileHandle> handle;
    if (sfd_hevc_ || adx_opus_) {
      // the stand-in's own size (the NAME.hevc / NAME.adxo bytes): the .lz4b's content size when it is packed
      uint64_t inner_size = stored_size_;
      uint8_t header[32];
      if (lz4_blocks_ && !(stored_size_ >= sizeof(header) && storage->ReadAt(data_offset_, header, sizeof(header)) &&
                           Lz4BlocksSizeFromHeader(header, &inner_size))) {
        REXFS_ERROR("[pack] {}: not a valid .lz4b", stored_name);
        return X_STATUS_NO_SUCH_FILE;
      }
      auto first = range();
      auto second = range();
      if (first && second) {
        handle = sfd_hevc_ ? OpenSfdHevcFrom(std::move(first), std::move(second), inner_size)
                           : OpenAdxOpusFrom(std::move(first), std::move(second), inner_size);  // logs why not
      }
    } else {
      handle = range();
    }
    if (!handle) return X_STATUS_NO_SUCH_FILE;
    *out_file = new PackHandleFile(desired_access, this, std::move(handle));
    return X_STATUS_SUCCESS;
  }
  // A folder opens too (games open directories to enumerate them); reads on it are rejected in ReadSync.
  *out_file = new PackFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

PackFile::PackFile(uint32_t file_access, PackEntry* entry) : File(file_access, entry), pack_entry_(entry) {}
PackFile::~PackFile() = default;

void PackFile::Destroy() {
  delete this;
}

X_STATUS PackFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) {
  *out_bytes_read = 0;
  if (pack_entry_->attributes() & kFileAttributeDirectory) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  const size_t size = pack_entry_->size();
  if (byte_offset >= size) {
    return X_STATUS_END_OF_FILE;
  }
  const size_t n = std::min(buffer.size(), size - byte_offset);
  PackStorage* storage = pack_entry_->pack()->storage();
  const uint64_t base = pack_entry_->data_offset();
  if (n > kCacheSize / 2) {
    if (!storage->ReadAt(base + byte_offset, buffer.data(), n)) return X_STATUS_UNSUCCESSFUL;
    *out_bytes_read = n;
    return X_STATUS_SUCCESS;
  }
  // asset_pack.c asset_pack_read_fd: small reads come out of a 64 KB window of this file, refilled from the read
  // position on a miss.
  std::lock_guard<std::mutex> lock(cache_mutex_);
  if (byte_offset < cache_offset_ || byte_offset + n > cache_offset_ + cache_size_) {
    cache_.resize(kCacheSize);
    cache_offset_ = byte_offset;
    cache_size_ = std::min(kCacheSize, size - byte_offset);
    if (!storage->ReadAt(base + byte_offset, cache_.data(), cache_size_)) {
      cache_size_ = 0;
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  std::memcpy(buffer.data(), cache_.data() + (byte_offset - cache_offset_), n);
  *out_bytes_read = n;
  return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------------
// Device

PackDevice::PackDevice(const std::string_view mount_path, const std::filesystem::path& folder)
    : Device(mount_path), folder_(folder) {}
PackDevice::~PackDevice() = default;

std::filesystem::path PackDevice::PackPath(const std::filesystem::path& folder) {
  auto path = folder;
  path += ".nxpack";
  return path;
}
std::filesystem::path PackDevice::IndexPath(const std::filesystem::path& folder) {
  auto path = folder;
  path += ".nxidx";
  return path;
}
bool PackDevice::Exists(const std::filesystem::path& folder) {
  std::error_code ec;
  return std::filesystem::is_regular_file(PackPath(folder), ec) &&
         std::filesystem::is_regular_file(IndexPath(folder), ec);
}

bool PackDevice::Initialize() {
  const auto start = std::chrono::steady_clock::now();
  LoadedIndex index;
  std::string error;
  if (!LoadIndex(PackPath(folder_), IndexPath(folder_), &index, &error)) {
    REXFS_ERROR("[pack] {}: {}", rex::path_to_utf8(PackPath(folder_)), error);
    return false;
  }
  // sa2-nx-ult (2026-11-24): a pack holding stand-ins it does not serve (NeverInPack), or a version-2 pack holding
  // NAME.lz4b (made by an earlier build's first boot from the web installer's folder: no guest sizes) is not used:
  // the game folder is mounted instead (Runtime::SetupVfs).
  {
    const bool v3 = !index.guest_sizes.empty();
    size_t unserved = 0;
    std::string first;
    for (const DiskEntry& disk : index.entries) {
      const std::string_view path(index.paths.data() + disk.path_offset, disk.path_length);
      if ((NeverInPack(path) || (!v3 && HasLz4BlocksSuffix(path))) && unserved++ == 0) first = std::string(path);
    }
    if (unserved) {
      REXFS_WARN("[pack] {} holds compressed files this pack cannot serve ({} of them, the first {}{}): not used, the "
                 "game folder is read instead. The .nxpack and .nxidx can be deleted.",
                 rex::path_to_utf8(PackPath(folder_)), unserved, first,
                 v3 ? "" : "; a version-2 pack made by an earlier build from the web installer's game folder");
      return false;
    }
  }
  storage_ = PackStorage::Open(PackPath(folder_));
  if (!storage_) {
    REXFS_ERROR("[pack] cannot open {}", rex::path_to_utf8(PackPath(folder_)));
    return false;
  }
  // Every entry gets the pack's own timestamps (the format keeps none per file).
  rex::filesystem::FileInfo info{};
  rex::filesystem::GetInfo(PackPath(folder_), &info);

  root_entry_ = std::make_unique<PackEntry>(this, nullptr, "");
  root_entry_->pack_ = this;
  root_entry_->attributes_ = kFileAttributeDirectory;
  std::unordered_map<std::string, PackEntry*> directories;
  directories.emplace(std::string(), root_entry_.get());
  const auto make = [&](PackEntry* parent, const std::string& name, bool directory) {
    auto* entry = new PackEntry(this, parent, rex::string::utf8_join_guest_paths(parent->path(), name));
    entry->pack_ = this;
    entry->create_timestamp_ = info.create_timestamp;
    entry->access_timestamp_ = info.access_timestamp;
    entry->write_timestamp_ = info.write_timestamp;
    entry->attributes_ = directory ? kFileAttributeDirectory : (kFileAttributeNormal | kFileAttributeReadOnly);
    parent->children_.push_back(std::unique_ptr<Entry>(entry));
    return entry;
  };
  // sa2-nx-ult (2026-11-21): a NAME.hevc (a Sofdec movie as H.265, rex/filesystem/sfd_hevc.h) answers for NAME, with
  // the movie's size from its header; a plain NAME in the same pack wins (and the .hevc stays an ordinary file).
  // The same for NAME.adxo (ADX streams as Opus, rex/filesystem/adx_opus.h). (2026-11-24) And NAME.lz4b, NAME.hevc.lz4b
  // (version 3 only): in version 3 every stand-in's size is the index's guest size, so no header is read here.
  std::unordered_map<std::string, bool> plain_names;
  for (const DiskEntry& disk : index.entries) {
    const std::string path(index.paths.data() + disk.path_offset, disk.path_length);
    if (!StandInOf(path).any()) plain_names.emplace(rex::string::utf8_lower_ascii(path), true);
  }
  size_t movies = 0, sounds = 0, packed = 0;
  for (size_t i = 0; i < index.entries.size(); ++i) {
    const DiskEntry& disk = index.entries[i];
    std::string path(index.paths.data() + disk.path_offset, disk.path_length);
    bool sfd_hevc = false, adx_opus = false, lz4_blocks = false;
    uint64_t movie_size = 0;
    const StandIn s = StandInOf(path);
    if (s.any()) {
      uint8_t header[kSfdHevcHeaderBytes];
      static_assert(kSfdHevcHeaderBytes >= kAdxOpusHeaderBytes);
      if (plain_names.count(rex::string::utf8_lower_ascii(s.plain))) {
        REXFS_WARN("[pack] {} and {} are both in the pack: the plain file is used", s.plain, path);
      } else if (!index.guest_sizes.empty() ||
                 (disk.size >= sizeof(header) && storage_->ReadAt(disk.offset, header, sizeof(header)) &&
                  (s.hevc ? SfdHevcSizeFromHeader(header, sizeof(header), &movie_size)
                          : AdxOpusSizeFromHeader(header, sizeof(header), &movie_size)))) {
        if (!index.guest_sizes.empty()) movie_size = index.guest_sizes[i];
        sfd_hevc = s.hevc;
        adx_opus = s.adxo;
        lz4_blocks = s.lz4;
        path = s.plain;
        if (s.hevc) ++movies;
        if (s.adxo) ++sounds;
        if (s.lz4) ++packed;
      } else {
        REXFS_WARN("[pack] {} is not a valid {} file: left as it is", path, s.hevc ? ".hevc" : ".adxo");
      }
    }
    PackEntry* parent = root_entry_.get();
    size_t start_part = 0;
    for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', start_part)) {
      const std::string directory = path.substr(0, slash);
      auto it = directories.find(directory);
      if (it == directories.end()) {
        it = directories.emplace(directory, make(parent, path.substr(start_part, slash - start_part), true)).first;
      }
      parent = it->second;
      start_part = slash + 1;
    }
    PackEntry* file = make(parent, path.substr(start_part), false);
    file->data_offset_ = disk.offset;
    file->sfd_hevc_ = sfd_hevc;
    file->adx_opus_ = adx_opus;
    file->lz4_blocks_ = lz4_blocks;
    file->stored_size_ = disk.size;
    file->size_ = size_t(sfd_hevc || adx_opus || lz4_blocks ? movie_size : disk.size);
    file->allocation_size_ = rex::round_up(file->size_, size_t(bytes_per_sector()));
  }
  if (movies || sounds || packed)
    REXFS_INFO("[pack] version {}: {} movies stored as H.265 (NAME.hevc), {} sound files as Opus (NAME.adxo), {} "
               "files as LZ4 / ZXC blocks (NAME.lz4b)",
               index.header.version, movies, sounds, packed);
  entry_count_ = index.entries.size();
  REXFS_INFO("[pack] mounted '{}' at {}: {} files, {} directories, {:.1f} MB in {:.1f} ms",
             rex::path_to_utf8(PackPath(folder_)), mount_path(), entry_count_, directories.size() - 1,
             double(index.header.pack_size) / (1024.0 * 1024.0),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return true;
}

void PackDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* PackDevice::ResolvePath(const std::string_view path) {
  REXFS_DEBUG("PackDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

// ---------------------------------------------------------------------------------------------------------------
// Builder (asset_pack.c asset_pack_build)

bool PackDevice::Build(const std::filesystem::path& folder, std::string* error,
                       const std::function<void(const char* stage, uint32_t percent)>& progress) {
  struct Item {
    std::string path;
    std::filesystem::path source;
    uint64_t size = 0, offset = 0;
    uint64_t guest_size = 0;  // sa2-nx-ult (2026-11-24): version 3's (a stand-in's NAME size; else `size`)
  };
  std::vector<Item> items;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    Item item;
    item.source = it->path();
    item.path = rex::path_to_utf8(std::filesystem::relative(it->path(), folder, ec));
    std::replace(item.path.begin(), item.path.end(), '\\', '/');
    item.size = it->file_size(ec);
    if (ec || !ValidRelative(item.path.c_str(), item.path.size())) {
      return *error = "the game folder cannot be enumerated (" + item.path + ")", false;
    }
    if (NeverInPack(item.path)) {  // sa2-nx-ult (2026-11-24): the folder is read as it is (pack_device.h)
      return *error = "the game folder holds stand-ins a pack does not serve (the first: " + item.path +
                      "), which the folder serves with its VFS index",
             false;
    }
    // sa2-nx-ult (2026-11-24): a stand-in (the web installer's NAME.lz4b, NAME.hevc[.lz4b], NAME.adxo) goes in as it
    // is, with NAME's size in the version-3 index
    item.guest_size = item.size;
    const StandIn s = StandInOf(item.path);
    if (s.any() && !GuestSizeOf(item.source, s, &item.guest_size)) {
      return *error = "the game folder holds a stand-in that is not valid: " + item.path, false;
    }
    items.push_back(std::move(item));
  }
  if (ec || items.empty() || items.size() > kMaxEntries) {
    return *error = "the game folder cannot be enumerated", false;
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });

  uint64_t paths_size = 0, total = 0, pack_size = sizeof(PackHeader);
  uint64_t pack_id = kFnvBasis;
  for (Item& item : items) {
    pack_size = rex::round_up(pack_size, uint64_t(16));
    item.offset = pack_size;
    pack_size += item.size;
    paths_size += item.path.size() + 1;
    total += item.size;
    pack_id = Fnv(pack_id, item.path.data(), item.path.size());
    pack_id = Fnv(pack_id, &item.size, sizeof(item.size));
  }
  if (paths_size > kMaxPathsSize) return *error = "the game folder's paths are too long", false;
  // One plain file: FAT32 (the usual Switch SD format) cannot hold 4 GB or more. A bigger game would need a
  // concatenation file (FsCreateOption_BigFile) or a split pack; games whose data is already in a few big archives
  // gain little from a pack anyway.
  if (pack_size >= (4ull << 30)) {
    return *error = "the game data is " + std::to_string(pack_size >> 20) +
                    " MB, over FAT32's 4 GB file limit: no asset pack", false;
  }
  const uint64_t nonce = (uint64_t(std::time(nullptr)) << 32) ^
                         uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  pack_id = Fnv(pack_id, &nonce, sizeof(nonce));
  if (!pack_id) pack_id = 1;
  const auto space = std::filesystem::space(folder.parent_path(), ec);
  if (!ec && space.available < pack_size + (64ull << 20)) {
    return *error = "not enough free space for the asset pack (" + std::to_string(pack_size >> 20) + " MB)", false;
  }

  auto temp_pack = PackPath(folder);
  temp_pack += ".tmp";
  auto temp_index = IndexPath(folder);
  temp_index += ".tmp";
  std::filesystem::remove(temp_pack, ec);
  std::filesystem::remove(temp_index, ec);
  const auto fail = [&](const std::string& message) {
    std::filesystem::remove(temp_pack, ec);
    std::filesystem::remove(temp_index, ec);
    *error = message;
    return false;
  };

  PackHeader header{};
  std::memcpy(header.magic, "SSNXPAK1", 8);
  header.version = kPackVersion;
  header.pack_id = pack_id;
  header.file_size = pack_size;
  FILE* out = rex::filesystem::OpenFile(temp_pack, "wb");
  if (!out || std::fwrite(&header, sizeof(header), 1, out) != 1) {
    if (out) std::fclose(out);
    return fail("the asset pack cannot be created");
  }
  std::vector<uint8_t> buffer(kChunk);
  static const uint8_t kPadding[16] = {};
  uint64_t position = sizeof(PackHeader), written = 0, data_checksum = kFnvBasis;
  uint32_t last_percent = 101;
  for (const Item& item : items) {
    if (item.offset > position && std::fwrite(kPadding, 1, size_t(item.offset - position), out) !=
                                      size_t(item.offset - position)) {
      std::fclose(out);
      return fail("the asset pack cannot be written");
    }
    position = item.offset;
    FILE* in = rex::filesystem::OpenFile(item.source, "rb");
    if (!in) {
      std::fclose(out);
      return fail("a game file disappeared while packing: " + item.path);
    }
    for (uint64_t remaining = item.size; remaining;) {
      const size_t piece = size_t(std::min<uint64_t>(remaining, kChunk));
      if (std::fread(buffer.data(), 1, piece, in) != piece || std::fwrite(buffer.data(), 1, piece, out) != piece) {
        std::fclose(in);
        std::fclose(out);
        return fail("a game file could not be packed: " + item.path);
      }
      data_checksum = Fnv(data_checksum, buffer.data(), piece);
      remaining -= piece;
      position += piece;
      written += piece;
      const uint32_t percent = total ? uint32_t(written * 100 / total) : 100;
      if (progress && percent != last_percent) progress("packing", last_percent = percent);
    }
    std::fclose(in);
  }
  header.data_checksum = data_checksum;
  const bool header_ok = rex::filesystem::Seek(out, 0, SEEK_SET) && std::fwrite(&header, sizeof(header), 1, out) == 1;
  if (std::fclose(out) != 0 || !header_ok) return fail("the asset pack cannot be finalized");

  std::vector<DiskEntry3> entries(items.size());  // sa2-nx-ult (2026-11-24): version 3
  std::vector<char> paths;
  paths.reserve(size_t(paths_size));
  for (size_t i = 0; i < items.size(); ++i) {
    entries[i] = {items[i].offset, items[i].size, uint32_t(paths.size()), uint32_t(items[i].path.size()),
                  items[i].guest_size};
    paths.insert(paths.end(), items[i].path.begin(), items[i].path.end());
    paths.push_back(0);
  }
  IndexHeader index{};
  std::memcpy(index.magic, "SSNXIDX1", 8);
  index.version = kPackVersion;
  index.count = uint32_t(items.size());
  index.pack_id = pack_id;
  index.pack_size = pack_size;
  index.paths_size = paths.size();
  index.checksum = Fnv(Fnv(kFnvBasis, entries.data(), entries.size() * sizeof(DiskEntry3)), paths.data(), paths.size());
  index.data_checksum = data_checksum;
  out = rex::filesystem::OpenFile(temp_index, "wb");
  const bool index_ok = out && std::fwrite(&index, sizeof(index), 1, out) == 1 &&
                        std::fwrite(entries.data(), sizeof(DiskEntry3), entries.size(), out) == entries.size() &&
                        std::fwrite(paths.data(), 1, paths.size(), out) == paths.size();
  if (!out || std::fclose(out) != 0 || !index_ok) return fail("the asset index cannot be written");

  // asset_pack.c verify_pack_data: read every byte back through the new index before installing it.
  LoadedIndex check;
  std::string load_error;
  if (!LoadIndex(temp_pack, temp_index, &check, &load_error)) return fail("the new asset pack: " + load_error);
  auto storage = PackStorage::Open(temp_pack);
  if (!storage) return fail("the new asset pack cannot be reopened");
  uint64_t verify = kFnvBasis, checked = 0;
  last_percent = 101;
  for (const DiskEntry& e : check.entries) {
    for (uint64_t done = 0; done < e.size;) {
      const size_t piece = size_t(std::min<uint64_t>(e.size - done, kChunk));
      if (!storage->ReadAt(e.offset + done, buffer.data(), piece)) return fail("the new asset pack cannot be read");
      verify = Fnv(verify, buffer.data(), piece);
      done += piece;
      checked += piece;
      const uint32_t percent = total ? uint32_t(checked * 100 / total) : 100;
      if (progress && percent != last_percent) progress("verifying", last_percent = percent);
    }
  }
  storage.reset();
  if (verify != data_checksum) return fail("the written asset pack did not pass verification");

  std::filesystem::remove(PackPath(folder), ec);
  std::filesystem::remove(IndexPath(folder), ec);
  std::filesystem::rename(temp_pack, PackPath(folder), ec);
  if (!ec) std::filesystem::rename(temp_index, IndexPath(folder), ec);
  if (ec) return fail("the asset pack cannot be installed: " + ec.message());
  return true;
}

}  // namespace rex::filesystem
