/* katamari-nx (2026-11-01, from worldkeflings-nx's tools/lzx_tool.c): decompress one raw LZX stream with the SDK's
 * libmspack (lzxd), to test the game's data formats (the CPDB blocks of d02-d05.dat). A host tool, not shipped.
 *   lzx_tool <in> <out> <output length> <window bits 15..21> [input offset]
 * Build: clang -O2 -Isdk/thirdparty/libmspack/cabextract/mspack tools/lzx_tool.c
 *        sdk/thirdparty/libmspack/cabextract/mspack/lzxd.c sdk/thirdparty/libmspack/cabextract/mspack/system.c
 *        -o out/tools/lzx_tool.exe */
#include <stdio.h>
#include <stdlib.h>

#include "system.h"
#include "lzx.h"

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: lzx_tool <in> <out> <output length> <window bits> [input offset]\n");
    return 2;
  }
  struct mspack_system *sys = mspack_default_system;
  struct mspack_file *in = sys->open(sys, argv[1], MSPACK_SYS_OPEN_READ);
  struct mspack_file *out = sys->open(sys, argv[2], MSPACK_SYS_OPEN_WRITE);
  if (!in || !out) {
    fprintf(stderr, "cannot open files\n");
    return 1;
  }
  if (argc > 5 && sys->seek(in, (off_t)strtol(argv[5], NULL, 0), MSPACK_SYS_SEEK_START)) {
    fprintf(stderr, "cannot seek\n");
    return 1;
  }
  const off_t length = (off_t)strtol(argv[3], NULL, 0);
  const int bits = atoi(argv[4]);
  struct lzxd_stream *lzx = lzxd_init(sys, in, out, bits, 0, 0x8000, length, 0);
  if (!lzx) {
    fprintf(stderr, "lzxd_init failed\n");
    return 1;
  }
  const int err = lzxd_decompress(lzx, length);
  lzxd_free(lzx);
  sys->close(in);
  sys->close(out);
  if (err) {
    fprintf(stderr, "lzxd_decompress: error %d\n", err);
    return 1;
  }
  return 0;
}
