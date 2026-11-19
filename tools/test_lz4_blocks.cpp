// rr6-nx (2026-11-10): host test of the .lz4b reader (sdk/src/filesystem/lz4_blocks.cpp, unchanged) against the file
// it holds: the whole file read in 1 MB pieces, then random reads (parts of blocks, runs of whole blocks, past the
// end), every byte compared. The SDK's logging and file handles are replaced by tools/test_stubs (stdio).
//   PATH=/c/devkitPro/msys2/clang64/bin:$PATH clang++ -std=c++20 -O2 -Itools/test_stubs -Isdk/include \
//     tools/test_lz4_blocks.cpp sdk/src/filesystem/lz4_blocks.cpp -o out/test_lz4_blocks.exe
//   out/test_lz4_blocks.exe <file.lz4b> <original> [random reads]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <rex/filesystem/lz4_blocks.h>

namespace rex::filesystem {
namespace {
class StdioHandle final : public FileHandle {
 public:
  StdioHandle(const std::filesystem::path& path, std::FILE* f) : FileHandle(path), f_(f) {}
  ~StdioHandle() override { std::fclose(f_); }
  bool Read(size_t offset, void* buffer, size_t length, size_t* out) override {
    if (_fseeki64(f_, int64_t(offset), SEEK_SET) != 0) return false;
    *out = std::fread(buffer, 1, length, f_);
    return true;
  }
  bool Write(size_t, const void*, size_t, size_t*) override { return false; }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  std::FILE* f_;
};
}  // namespace

std::unique_ptr<FileHandle> FileHandle::OpenExisting(const std::filesystem::path& path, uint32_t, bool) {
  std::FILE* f = _wfopen(path.wstring().c_str(), L"rb");
  return f ? std::make_unique<StdioHandle>(path, f) : nullptr;
}
}  // namespace rex::filesystem

using namespace rex::filesystem;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: test_lz4_blocks <file.lz4b> <original> [random reads]\n");
    return 2;
  }
  const int reads = argc > 3 ? std::atoi(argv[3]) : 20000;
  uint64_t size = 0;
  if (!ReadLz4BlocksSize(argv[1], &size)) {
    std::fprintf(stderr, "FAIL: no size from the header\n");
    return 1;
  }
  auto packed = OpenLz4Blocks(FileHandle::OpenExisting(argv[1], FileAccess::kFileReadData));
  auto original = FileHandle::OpenExisting(argv[2], FileAccess::kFileReadData);
  if (!packed || !original) {
    std::fprintf(stderr, "FAIL: open\n");
    return 1;
  }
  std::vector<uint8_t> a(4u << 20), b(4u << 20);
  size_t got_a = 0, got_b = 0;

  // 1: the whole file in 1 MB pieces (runs of whole blocks), timed
  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t at = 0; at < size; at += 1u << 20) {
    if (!packed->Read(size_t(at), a.data(), 1u << 20, &got_a) || !original->Read(size_t(at), b.data(), 1u << 20, &got_b) ||
        got_a != got_b || std::memcmp(a.data(), b.data(), got_a) != 0) {
      std::fprintf(stderr, "FAIL: sequential read at %llu\n", (unsigned long long)at);
      return 1;
    }
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  // 2: random reads: inside a block, across blocks, long runs, up to and past the end
  std::mt19937_64 rng(1234);
  for (int i = 0; i < reads; ++i) {
    const uint64_t at = rng() % (size + 1000);
    size_t length;
    switch (rng() % 4) {
      case 0: length = 1 + rng() % 64; break;             // tiny
      case 1: length = 1 + rng() % (300u << 10); break;   // around a block
      case 2: length = 1 + rng() % (4u << 20); break;     // runs of blocks
      default: length = (1 + rng() % 16) * (256u << 10); break;  // whole blocks
    }
    const bool ok_a = packed->Read(size_t(at), a.data(), length, &got_a);
    size_t want = at >= size ? 0 : size_t(std::min<uint64_t>(length, size - at));
    if (want) original->Read(size_t(at), b.data(), want, &got_b);
    if (!ok_a || got_a != want || (want && std::memcmp(a.data(), b.data(), want) != 0)) {
      std::fprintf(stderr, "FAIL: random read %d at %llu, %zu bytes (got %zu, want %zu)\n", i, (unsigned long long)at,
                   length, got_a, want);
      return 1;
    }
  }
  std::printf("OK: %s holds %llu bytes; the whole file read equal in %.2f s (%.0f MB/s here), %d random reads equal\n",
              argv[1], (unsigned long long)size, s, double(size) / 1048576.0 / s, reads);
  return 0;
}
