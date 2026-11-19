// rr6-nx tools/test_lz4_blocks.cpp: the few parts of rex/filesystem.h that sdk/src/filesystem/lz4_blocks.cpp uses,
// over stdio, so the reader can be tested without the SDK.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace rex {
inline std::string path_to_utf8(const std::filesystem::path& p) { return p.string(); }
}  // namespace rex

namespace rex::filesystem {
struct FileAccess {
  static const uint32_t kGenericRead = 0x80000000;
  static const uint32_t kFileReadData = 0x00000001;
};
class FileHandle {
 public:
  static std::unique_ptr<FileHandle> OpenExisting(const std::filesystem::path& path, uint32_t, bool = false);
  virtual ~FileHandle() = default;
  const std::filesystem::path& path() const { return path_; }
  virtual bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) = 0;
  virtual bool Write(size_t, const void*, size_t, size_t*) = 0;
  virtual bool SetLength(size_t) = 0;
  virtual void Flush() = 0;
 protected:
  explicit FileHandle(const std::filesystem::path& path) : path_(path) {}
  std::filesystem::path path_;
};
struct FileInfo {
  size_t total_size = 0;
};
inline bool GetInfo(const std::filesystem::path& path, FileInfo* out) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return false;
  out->total_size = size_t(size);
  return true;
}
}  // namespace rex::filesystem
