/**
 * sa2-nx-ult (2026-11-21): rr6-nx's file, unchanged in format; SA2's hooks are app/src/sa2_hevc_movies.cpp (CRI MPV
 * 2.081 / MWSFD 5.54: the tag is found the same way, after the slice header) and its notes docs/opus-h265.md.
 *
 * rr6-nx (2026-11-21, the user: "rencoding ... videos as h.265 copy what culdcept saga does"): NAME.hevc is a Sofdec
 * movie NAME (an .sfd: an MPEG-1 program stream, MPEG-1 video and ADX audio, played by CRI's MWSFD 3.71 in the game's
 * own code) whose pictures are H.265. The game sees NAME as it was (the same size; every pack, packet header, audio
 * packet and video header byte as it was), but each picture's slice data is zero except a 10-byte tag after its first
 * slice header: the movie's id (given when NAME.hevc is opened) and the picture's place in display order. The port's
 * movie hooks (app/src/cpu/rr6_hevc_movies.cpp) find the tag where CRI's MPEG decoder starts the slice, skip that
 * decoder's macroblocks, put the tag in the picture's frame buffer, and when the game takes the frame to show it, write
 * the H.265 picture there instead (NVDEC on the Switch, nxvideo). Timing, audio and the game's own player are untouched:
 * only the pixels come from elsewhere. Culdcept Saga's port does the same for its WMV movies inside their ASF files
 * (culdcept-nx's app/src/cpu/culdcept_wmv_ffmpeg.cpp); this one keeps the SFD byte for byte instead of rewriting it.
 *
 * The file (little-endian; made by tools/hevc_sfd_pack.py and rr6-nx-installer's movie worker):
 *   header, 96 bytes:  "RXSFDH01", u32 version (1), u32 segment count, u64 size of NAME, u64 offset of the segments,
 *                      u64 offset of the stored bytes, u64 length of the stored bytes, u32 picture count (the coded
 *                      pictures that have a tag), u32 0, u64 offset of the pictures, u32 width, u32 height, u32 frame
 *                      count (H.265 frames, display order), u32 frame rate x 1000, u64 offset of the frame table,
 *                      u64 0
 *   segments, 32 bytes each, in NAME's order, back to back from 0 to NAME's size:
 *                      u64 offset in NAME, u64 length, u32 kind (0: stored bytes, 1: zeros, 2: tag bytes), u32 picture
 *                      (kind 2: the coded picture whose tag), u64 source (kind 0: offset in the stored bytes; kind 2:
 *                      the first of the tag's bytes in this segment, 0-9: a tag may be split by a packet header)
 *   pictures:          u32 each, the display index of the coded picture (0 the first shown)
 *   frame table:       frame count x 16 bytes: u64 offset in this file, u32 size, u32 flags (1: a keyframe, which
 *                      carries the parameter sets: decoding may start there)
 *   the stored bytes and the H.265 frames (Annex B, no B frames: decode order is display order)
 *
 * The tag, 10 bytes with no zero among them (the slice data around it is zero, and MPEG's start codes are 00 00 01):
 *   A5 5A C3, the movie id in 3 bytes and the display index in 3 bytes (7 bits each, high bit set, most significant
 *   first), then 0x80 | (the sum of those 6 bytes & 0x7F).
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem.h>

namespace rex::filesystem {

inline constexpr std::string_view kSfdHevcSuffix = ".hevc";
inline constexpr uint32_t kSfdHevcTagBytes = 10;

bool HasSfdHevcSuffix(std::string_view name);
// sa2-nx-ult (2026-11-21, the user: "we want to be able to run ZXC compressed H265 videos"; rr6-nx's of the same day):
// NAME.hevc.lz4b, a NAME.hevc packed as LZ4 / ZXC blocks (rex/filesystem/lz4_blocks.h; the installer packs the movies
// with ZXC: their SFD bytes compress, the H.265 frames do not), stands for the game's NAME as NAME.hevc does.
inline constexpr std::string_view kPackedSfdHevcSuffix = ".hevc.lz4b";
bool HasPackedSfdHevcSuffix(std::string_view name);
// The size of the movie a NAME.hevc stands for, from its header. False if it is not one.
bool ReadSfdHevcSize(const std::filesystem::path& path, uint64_t* out_size);
// A handle that reads the movie `packed` stands for, or nullptr when it is not a valid NAME.hevc (logged). While it is
// open, its H.265 frames can be found by the id its tags carry (FindSfdHevcMovie).
std::unique_ptr<FileHandle> OpenSfdHevc(std::unique_ptr<FileHandle> packed);
// sa2-nx-ult (2026-11-21): the same for a NAME.hevc that is not a host file of its own (an entry of SA2's game pack,
// rex/filesystem/devices/pack_device.h): `packed` and `frames` read the same `size` bytes (two handles, so the game's
// reads and the movie's frame reads do not share one).
std::unique_ptr<FileHandle> OpenSfdHevcFrom(std::unique_ptr<FileHandle> packed, std::unique_ptr<FileHandle> frames,
                                            uint64_t size);
// The size of the movie a NAME.hevc stands for, from its first `length` bytes (96 are enough). False if not one.
bool SfdHevcSizeFromHeader(const uint8_t* header, size_t length, uint64_t* out_size);
inline constexpr size_t kSfdHevcHeaderBytes = 96;

// One frame of a movie: its H.265 bytes.
struct SfdHevcFrame {
  uint64_t offset = 0;
  uint32_t size = 0;
  bool keyframe = false;
};

// A movie the game has open: what the hooks need to decode its pictures.
class SfdHevcMovie {
 public:
  virtual ~SfdHevcMovie() = default;
  uint32_t id = 0;
  std::string name;  // NAME (the guest's file name), for the log
  uint32_t width = 0, height = 0, fps_milli = 0;
  std::vector<SfdHevcFrame> frames;
  // Frame `index`'s bytes into `out`; false when they cannot be read.
  virtual bool ReadFrame(uint32_t index, std::vector<uint8_t>* out) const = 0;
};

// The movie whose tags carry `id`, while a handle on it is open (and a few seconds after: the game's player may
// still show its last pictures); null when none.
std::shared_ptr<const SfdHevcMovie> FindSfdHevcMovie(uint32_t id);

// A tag's fields from its 10 bytes; false when they are not a tag.
bool ParseSfdHevcTag(const uint8_t* p, uint32_t* id, uint32_t* display_index);
// The 10 bytes of the tag for (id, display index).
void MakeSfdHevcTag(uint32_t id, uint32_t display_index, uint8_t* out);

}  // namespace rex::filesystem
