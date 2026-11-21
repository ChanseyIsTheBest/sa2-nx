// rr6-nx (2026-11-11): packs a game file as a version-2 .lz4b (ZXC blocks; sdk/include/rex/filesystem/lz4_blocks.h has
// the container), as the installer page does for every game file but the RRM archives: header "RXLZ4BLK", u32 2, u32
// block size, u64 size, u32 block count, u32 0; the blocks (zxc_compress_block, a block stored as it is when that is
// not smaller); u32 per block, its stored size; footer u64 index offset, "RXLZ4END". On every core, written in order.
// The RRM archives (version 1, LZ4): tools/lz4b_pack.py. Check either with tools/test_lz4_blocks.cpp.
//   PATH=/c/devkitPro/msys2/clang64/bin:$PATH clang++ -O2 -DZXC_STATIC_DEFINE -DZXC_ONLY_DEFAULT \
//     -DZXC_FUNCTION_SUFFIX=_default -Isdk/thirdparty/zxc/include -Isdk/thirdparty/zxc/src/lib \
//     -Isdk/thirdparty/zxc/src/lib/vendors -x c sdk/thirdparty/zxc/src/lib/zxc_{common,pivco_tables,dict,dispatch,pstream,seekable,compress,decompress,huffman}.c \
//     -x c++ tools/zxcb_pack.cpp -o out/zxcb_pack.exe
//   out/zxcb_pack.exe IN OUT.lz4b [level 7] [block KB 512]
// Game data: never commit or share what it writes.
#include <zxc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static void Put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void Put64(std::vector<uint8_t> &v, uint64_t x) { Put32(v, uint32_t(x)); Put32(v, uint32_t(x >> 32)); }

int main(int argc, char **argv) {
  if (argc < 3) { std::fprintf(stderr, "zxcb_pack IN OUT [level] [block KB]\n"); return 2; }
  const int level = argc > 3 ? std::atoi(argv[3]) : 7;
  const size_t block = size_t(argc > 4 ? std::atoi(argv[4]) : 512) << 10;
  FILE *in = std::fopen(argv[1], "rb");
  if (!in) return 1;
  _fseeki64(in, 0, SEEK_END);
  const uint64_t size = uint64_t(_ftelli64(in));
  _fseeki64(in, 0, SEEK_SET);
  const uint32_t count = uint32_t((size + block - 1) / block);
  FILE *out = std::fopen(argv[2], "wb");
  if (!out) return 1;
  std::vector<uint8_t> head = {'R', 'X', 'L', 'Z', '4', 'B', 'L', 'K'};
  Put32(head, 2); Put32(head, uint32_t(block)); Put64(head, size); Put32(head, count); Put32(head, 0);
  std::fwrite(head.data(), 1, head.size(), out);
  uint64_t at = head.size();
  std::vector<uint32_t> stored(count);
  const auto t0 = std::chrono::steady_clock::now();
  const unsigned threads = std::thread::hardware_concurrency();
  const uint32_t batch = 256;  // blocks read, compressed on every core and written per round
  std::vector<uint8_t> raw(size_t(batch) * block);
  std::vector<std::vector<uint8_t>> packed(batch);
  for (uint32_t first = 0; first < count; first += batch) {
    const uint32_t n = std::min(batch, count - first);
    const uint64_t from = uint64_t(first) * block, bytes = std::min<uint64_t>(uint64_t(n) * block, size - from);
    if (std::fread(raw.data(), 1, size_t(bytes), in) != bytes) return 1;
    std::atomic<uint32_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
      pool.emplace_back([&] {
        zxc_cctx *c = zxc_create_cctx();
        zxc_cctx_set_param(c, ZXC_C_LEVEL, level);
        zxc_cctx_set_param(c, ZXC_C_BLOCK_SIZE, int64_t(block));
        for (uint32_t k; (k = next.fetch_add(1)) < n;) {
          const uint8_t *src = raw.data() + size_t(k) * block;
          const size_t len = size_t(std::min<uint64_t>(block, size - (from + uint64_t(k) * block)));
          auto &dst = packed[k];
          dst.resize(zxc_compress_block_bound(len));
          const int64_t got = zxc_compress_block(c, src, len, dst.data(), dst.size());
          if (got <= 0 || size_t(got) >= len) dst.assign(src, src + len);
          else dst.resize(size_t(got));
        }
        zxc_free_cctx(c);
      });
    for (auto &t : pool) t.join();
    for (uint32_t k = 0; k < n; ++k) {
      std::fwrite(packed[k].data(), 1, packed[k].size(), out);
      stored[first + k] = uint32_t(packed[k].size());
      at += packed[k].size();
    }
  }
  std::vector<uint8_t> tail;
  for (uint32_t s : stored) Put32(tail, s);
  Put64(tail, at);
  for (char ch : {'R', 'X', 'L', 'Z', '4', 'E', 'N', 'D'}) tail.push_back(uint8_t(ch));
  std::fwrite(tail.data(), 1, tail.size(), out);
  std::fclose(out);
  std::fclose(in);
  const uint64_t total = at + tail.size();
  std::printf("%s -> %s: %llu -> %llu bytes (%.2f %%), %u blocks of %zu KB, ZXC %d, %.1f s\n", argv[1], argv[2],
              (unsigned long long)size, (unsigned long long)total, 100.0 * double(total) / double(size), count, block >> 10,
              level, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return 0;
}
