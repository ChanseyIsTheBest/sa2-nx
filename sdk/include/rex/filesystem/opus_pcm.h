/**
 * rr6-nx (2026-11-21, the user: "rencoding audio as opus and videos as h.265 copy what culdcept saga does"): NAME.opcm
 * is a game file NAME in which some byte ranges are 16-bit PCM samples, kept as Opus. The game sees NAME as it was (the
 * same size and every byte outside the samples as it was); the samples are decoded from the Opus as the game reads
 * them. Ridge Racer 6's music is PCM: pack_bgm.bin (20 RIFF WAVE tracks, 44.1 kHz stereo, 765 MB) and each DLC music
 * pack's track inside its Xbox 360 package (41 of them, 2.0 GB; a package's data blocks are interleaved with its hash
 * blocks, so one track is many ranges of the package). At 128 kbit/s the 4 h 23 min of music is about 250 MB.
 *
 * Culdcept Saga's port (culdcept-nx, rex/filesystem/xmo.h) does the same for XMA frames, inside its XMA emulation; this
 * one is at the file level, because the game reads its music as PCM: nothing in the audio path changes.
 *
 * A file NAME.opcm in a host folder is the game's NAME (HostPathEntry, as for NAME.lz4b; a plain NAME next to it wins).
 * A DLC package P read in place is read through P.opcm when one lies next to it (StfsContainerDevice): P itself is then
 * only the package's header (what the DLC scan and the content list read), P.opcm the whole package.
 *
 * The file (little-endian; made by tools/opus_pcm_pack.py and rr6-nx-installer's lib/opcm.js):
 *   header, 64 bytes:  "RXOPCM01", u32 version (1), u32 stream count, u64 size of NAME, u32 segment count, u32 0,
 *                      u64 offset of the segments, u64 offset of the streams, u64 offset of the stored bytes,
 *                      u64 length of the stored bytes
 *   segments, 32 bytes each, in NAME's order, back to back from 0 to NAME's size:
 *                      u64 offset in NAME, u64 length, u32 kind (0: stored bytes, 1: PCM), u32 stream (kind 1),
 *                      u64 source (kind 0: offset in the stored bytes; kind 1: byte offset in the stream's PCM)
 *   streams, 48 bytes each:
 *                      u16 channels (1 or 2), u16 bits (16), u32 PCM sample rate, u32 flags (1: big-endian samples),
 *                      u32 Opus pre-skip, u64 PCM frames (samples a channel), u32 Opus packet count, u32 Opus frame
 *                      size (960: 20 ms at 48 kHz), u64 offset of the packet sizes (u16 each), u64 offset of the
 *                      packets (back to back)
 *   the stored bytes, the packet sizes and the packets, where the offsets say
 *
 * A stream's Opus is 48 kHz: the PCM resampled to 48 kHz (when its rate is not 48 kHz) and encoded. The reader decodes
 * it and resamples it back to the PCM's rate (a 64-tap windowed-sinc polyphase filter) for the frames a read covers;
 * after a jump (a loop, a seek) it starts decoding 320 ms early, as culdcept-nx's xmo reader does (80 ms left 20-25 dB
 * from the in-order decode there, 320 ms 92 dB or better).
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include <rex/filesystem.h>

namespace rex::filesystem {

inline constexpr std::string_view kOpusPcmSuffix = ".opcm";

// Whether a host file name ends in .opcm (any case).
bool HasOpusPcmSuffix(std::string_view name);

// The size of the file an .opcm file stands for, from its header. False if it is not one.
bool ReadOpusPcmSize(const std::filesystem::path& path, uint64_t* out_size);

// A handle that reads the file `packed` stands for (the PCM decoded from its Opus), or nullptr when `packed` is not a
// valid .opcm file (logged).
std::unique_ptr<FileHandle> OpenOpusPcm(std::unique_ptr<FileHandle> packed);

}  // namespace rex::filesystem
