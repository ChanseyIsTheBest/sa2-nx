/**
 * sa2-nx-ult (2026-11-21, the user: "rencode ... all the audio to opus in a web installer like culdcept"): NAME.adxo is
 * a game file NAME that holds CRI ADX streams (an .adx, an .afs archive of them) whose sound is Opus. The game sees NAME
 * as it was (the same size; every header, table, end marker and padding byte as it was), but each ADX block (18 bytes:
 * a 16-bit scale and 32 four-bit samples) is a tag naming the stream, the channel and the block's index. CRI's ADX
 * decoder in the game's code (ADXT 11.35: sub_828C2948, the one decode step) is hooked: a run of tagged blocks gets its
 * 32 samples a block from the stream's Opus, decoded and resampled to the stream's own rate, instead of ADX nibbles
 * (app/src/sa2_adx_opus.cpp). Loops, seeks, the end marker, the streaming and the mixer stay the game's: only the
 * samples come from elsewhere. culdcept-nx's NAME.xmo (its rex/filesystem/xmo.h) does the same for XMA; rr6-nx's
 * NAME.hevc (rex/filesystem/sfd_hevc.h) gave the segment layout. docs/opus-h265.md.
 *
 * The file (little-endian; made by tools/adx_opus_pack.mjs and the installer's audio worker):
 *   header, 96 bytes:  "RXADXOP1", u32 version (1), u32 segment count, u64 size of NAME, u64 offset of the segments,
 *                      u64 offset of the stored bytes, u64 length of the stored bytes, u32 stream count, u32 0,
 *                      u64 offset of the streams, then zeros
 *   segments, 32 bytes each, in NAME's order, back to back from 0 to NAME's size:
 *                      u64 offset in NAME, u64 length, u32 kind (0: stored bytes, 1: zeros, 3: ADX blocks), u32 stream
 *                      (kind 3), u64 source (kind 0: offset in the stored bytes; kind 3: the byte of the stream's block
 *                      data this segment starts at: the blocks of its channels interleaved, 18 bytes each, as the
 *                      ADX file lays them out)
 *   streams, 64 bytes each:
 *                      u32 channels (1 or 2), u32 sample rate (the ADX header's), u32 blocks a channel (every data
 *                      block before the end marker), u32 Opus packet count, u16 Opus pre-skip (48 kHz samples), u16 0,
 *                      u32 0, u64 offset of the packet table (u32 each: a packet's size), u64 offset of the Opus
 *                      packets (back to back, 20 ms each, 48 kHz, the stream's channels), u64 their length, u32 the
 *                      bit rate they were made at, then zeros
 *   the stored bytes, the packet tables and the Opus packets
 *
 * A stream's sound: the samples CRI's decoder gives for its blocks in order (32 a block a channel, every block up to
 * the end marker), resampled to 48 kHz (a zero-phase windowed sinc, AdxOpusResampler), encoded as Opus; the decoder's
 * first `pre-skip` samples are dropped, then sample n of the 48 kHz sound is at time n / 48000 of the ADX's.
 *
 * The tag, 18 bytes: 7A D5 C3 (the scale field's high bit clear: not an end marker), the stream id in 3 bytes and the
 * block's index in its channel in 3 bytes (7 bits each, high bit set, most significant first), 0x80 | the channel,
 * 0x80 | (the sum of those 7 bytes & 0x7F), then 7 zero bytes.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <rex/filesystem.h>

namespace rex::filesystem {

inline constexpr std::string_view kAdxOpusSuffix = ".adxo";
inline constexpr uint32_t kAdxBlockBytes = 18;
inline constexpr uint32_t kAdxBlockSamples = 32;
inline constexpr size_t kAdxOpusHeaderBytes = 96;

bool HasAdxOpusSuffix(std::string_view name);
// The size of the file a NAME.adxo stands for, from its first `length` bytes (96 are enough). False if not one.
bool AdxOpusSizeFromHeader(const uint8_t* header, size_t length, uint64_t* out_size);
bool ReadAdxOpusSize(const std::filesystem::path& path, uint64_t* out_size);
// A handle that reads the file `packed` stands for (`size` bytes of it; `opus` reads the same bytes, for the streams'
// Opus), or nullptr when it is not a valid NAME.adxo (logged). While it is open (and a few seconds after), its
// streams can be found by the ids their tags carry (FindAdxOpusStream).
std::unique_ptr<FileHandle> OpenAdxOpus(std::unique_ptr<FileHandle> packed);
std::unique_ptr<FileHandle> OpenAdxOpusFrom(std::unique_ptr<FileHandle> packed, std::unique_ptr<FileHandle> opus,
                                            uint64_t size);

// The decoder hook's one call: the 32 samples (16-bit, host order) of block `block` of channel `channel` of stream `id`
// at the stream's own rate, from its Opus (decoded in order, kept a second back, resampled from 48 kHz). False when no
// open NAME.adxo has that stream (the hook then leaves the blocks to the game's decoder). Silence when libopus is
// missing (a PC without it; warned once). Thread safe: each stream decodes under its own lock.
bool AdxOpusBlockSamples(uint32_t id, uint32_t block, uint32_t channel, int16_t* out);
// For the log: streams decoded so far, packets decoded, seeks (a jump the decoder had to start again for).
struct AdxOpusCounts {
  uint64_t streams = 0, packets = 0, seeks = 0, blocks = 0, unknown = 0;
};
AdxOpusCounts GetAdxOpusCounts();

// A tag's fields from its 18 bytes; false when they are not a tag.
bool ParseAdxOpusTag(const uint8_t* p, uint32_t* id, uint32_t* block, uint32_t* channel);
void MakeAdxOpusTag(uint32_t id, uint32_t block, uint32_t channel, uint8_t* out);

}  // namespace rex::filesystem
