// banjo-nx (2026-10-16) - the record format of the direct renderer (banjo_direct_state, banjo_d3d_records), shared by
// the producers (the hooks on the game threads, the native Direct3D draw path) and the decoder. docs/d3d-full-replacement.md
// steps 4-6 is the design; sa2-nx's docs/api-renderer.md section 4 is where the format comes from.
//
// A record replaces a run of packets ("a piece") Direct3D wrote into a command buffer. The command buffer keeps one
// marker NOP where the piece began, covering all of it: [0xC0000000 | (words - 2) << 16 | NOP << 8] ['BJND'] [number].
// The record, in a host ring of 32-bit words:
//
//   [number] [record words] [the marker's physical address] [the three words the marker replaced, guest order] ops...
//
// Operations (tag = kind | count << 8):
//   kRun        [tag] [first register] [count values, host order]      a type-0 packet
//   kRunSingle  [tag] [register] [count values, host order]            a type-0 packet of one register (bit 15)
//   kType1      [tag] [header] [two values, host order]                a type-1 packet
//   kType3      [tag] [header] [packet address] [count words, guest order]   any other packet, run by the decoder's
//                                                                       Type3 with its own header (predicate bit)
//   kType3Ref   [tag] [header] [packet address]                        a tile mask or select: its words are read from
//                                                                       the command buffer when the record is applied
//                                                                       (Direct3D patches them after the call)
//
// What a piece can hold, and where it must end (Classify): a piece ends before an indirect buffer or a WAIT_REG_MEM
// (barriers: they stay in the command buffer; with banjo_d3d_records_barriers they are carried in the record) and
// before another record's marker; its first three
// words (the marker's) must be packets the record copies (kCopy), not a plain NOP (whose payload may be inline data a
// later packet points at) or a tile mask (patched later); those may start at word 2 (MayStartAt). Plain NOPs inside
// a piece have no operations: their words stay in the command buffer, inside the marker's region.
//
// Header-only. Included by me_native_system.cpp (the decoder and the hooks' producer) and banjo_d3d_native.h (the
// native draw path writes the operations of its own packets directly).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace banjo::records {

namespace pm4 = rex::graphics::xenos;

inline constexpr uint32_t kMagic = 0x424A4E44u;  // 'BJND', the first payload word of a marker NOP
inline constexpr uint32_t kRecordHeader = 6;
inline constexpr uint32_t kPad = 0xFFFFFFFFu;  // in the ring: the rest of its last words is unused
enum Op : uint32_t { kRun = 1, kRunSingle = 2, kType1 = 3, kType3 = 4, kType3Ref = 5 };
inline constexpr uint32_t kMaxRegion = 0x4000;  // a marker NOP covers at most 0x3FFF + 2 words
// The renderer's own marker NOPs (banjo_d3d_clear.h): carried as packets (the decoder reads their payload).
inline constexpr uint32_t kClearBegin = 0x424A434Cu, kClearEnd = 0x424A4345u;      // 'BJCL' 'BJCE'
inline constexpr uint32_t kResolveBegin = 0x424A5253u, kResolveEnd = 0x424A5245u;  // 'BJRS' 'BJRE'

// The tile predicate's mask and select: carried by reference (Direct3D fills a predicated draw's mask in after the
// draw call returns).
inline bool ByReference(uint32_t opcode) {
  switch (opcode) {
    case pm4::PM4_SET_BIN_MASK_LO:
    case pm4::PM4_SET_BIN_MASK_HI:
    case pm4::PM4_SET_BIN_SELECT_LO:
    case pm4::PM4_SET_BIN_SELECT_HI:
    case pm4::PM4_SET_BIN_MASK:
    case pm4::PM4_SET_BIN_SELECT:
      return true;
    default:
      return false;
  }
}

// n big-endian words to host order. On the A57 four words a rev32 (sa2-nx's ARM finding: a word loop the compiler
// does not vectorise is several instructions a word); elsewhere the compiler vectorises the word loop.
inline void SwapWords(uint32_t* dst, const uint8_t* src, uint32_t n) {
  uint32_t i = 0;
#if defined(__aarch64__)
  for (; i + 4 <= n; i += 4)
    vst1q_u8(reinterpret_cast<uint8_t*>(dst + i), vrev32q_u8(vld1q_u8(src + size_t(i) * 4)));
#endif
  for (; i < n; ++i) {
    uint32_t w;
    std::memcpy(&w, src + size_t(i) * 4, 4);
    dst[i] = __builtin_bswap32(w);
  }
}

enum class Kind : uint8_t { kCopy, kRef, kNop, kBarrier, kPartial, kOurs };

// banjo_d3d_records_barriers (step 6b): indirect buffers and WAIT_REG_MEM are carried too. Set once at start-up, before the first record: records made one way are checked the same way.
inline std::atomic<bool> g_barriers_in_records{false};

// The packet at word i of a region of `words` words: its kind and size in words.
inline Kind Classify(const uint8_t* src, uint32_t i, uint32_t words, uint32_t& size) {
  const uint32_t h = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
  const uint32_t type = h >> 30;
  if (type == 2 || h == 0) {  // a filler, a zero word (the parser skips it)
    size = 1;
    return Kind::kCopy;
  }
  size = type == 1 ? 3u : ((h >> 16) & 0x3FFF) + 2;
  if (i + size > words) return Kind::kPartial;
  if (type != 3) return Kind::kCopy;
  const uint32_t opcode = (h >> 8) & 0x7F;
  if (opcode == pm4::PM4_INDIRECT_BUFFER || opcode == pm4::PM4_INDIRECT_BUFFER_PFD || opcode == pm4::PM4_WAIT_REG_MEM)
    return g_barriers_in_records.load(std::memory_order_relaxed) ? Kind::kCopy : Kind::kBarrier;
  if (ByReference(opcode)) return Kind::kRef;
  if (opcode == pm4::PM4_NOP) {
    const uint32_t first = size >= 2 ? rex::memory::load_and_swap<uint32_t>(src + size_t(i + 1) * 4) : 0;
    if (first == kMagic) return Kind::kOurs;
    if (first == kClearBegin || first == kClearEnd || first == kResolveBegin || first == kResolveEnd) return Kind::kCopy;
    return Kind::kNop;
  }
  return Kind::kCopy;
}

// The operations of the packet at word i (`physical`: the address of word 0). A filler, a zero word and a plain NOP
// have none.
inline uint32_t* Emit(const uint8_t* src, uint32_t physical, uint32_t i, uint32_t size, Kind kind, uint32_t* out) {
  const uint8_t* const hp = src + size_t(i) * 4;
  const uint32_t h = rex::memory::load_and_swap<uint32_t>(hp);
  const uint32_t type = h >> 30;
  if (type == 2 || h == 0 || kind == Kind::kNop) return out;
  if (type == 0) {
    const uint32_t count = size - 1;
    *out++ = ((h >> 15) & 1 ? kRunSingle : kRun) | (count << 8);
    *out++ = h & 0x7FFF;
    SwapWords(out, hp + 4, count);
    return out + count;
  }
  if (type == 1) {
    *out++ = kType1;
    *out++ = h;
    SwapWords(out, hp + 4, 2);
    return out + 2;
  }
  const uint32_t count = size - 1;
  if (kind == Kind::kRef) {
    *out++ = kType3Ref | (count << 8);
    *out++ = h;
    *out++ = physical + i * 4;
    return out;
  }
  *out++ = kType3 | (count << 8);
  *out++ = h;
  *out++ = (physical + i * 4) & 0x1FFFFFFFu;
  std::memcpy(out, hp + 4, size_t(count) * 4);  // guest byte order: Type3 reads them
  return out + count;
}

// Whether a packet of this kind may start at word `at` of a piece: the marker overwrites words 0-2, so a packet whose
// words must stay where they are (a plain NOP's payload, a tile mask Direct3D patches after the call) may start at
// word 2 at the earliest: its header is then the marker's last word (the record carries the header), its payload from
// word 3 on is not touched.
inline bool MayStartAt(uint32_t at, Kind kind) {
  return at >= 3 || kind == Kind::kCopy || (at == 2 && (kind == Kind::kRef || kind == Kind::kNop));
}

// One piece from word `begin` (whose packet is kCopy) of a region of `words` words: its operations at `out` (room for
// 2 * (words - begin) + 4) and the word where it ends: at a barrier, a partial packet, another record's marker, a
// packet that may not start where it is (MayStartAt), or kMaxRegion.
inline uint32_t DecodePiece(const uint8_t* src, uint32_t physical, uint32_t words, uint32_t begin, uint32_t*& out) {
  uint32_t j = begin;
  while (j < words) {
    uint32_t size = 0;
    const Kind kind = Classify(src, j, words, size);
    if (kind == Kind::kBarrier || kind == Kind::kPartial || kind == Kind::kOurs) break;
    if (!MayStartAt(j - begin, kind)) break;
    if (j + size - begin > kMaxRegion) break;
    out = Emit(src, physical, j, size, kind, out);
    j += size;
  }
  return j;
}

// The producers' and the decoder's check: the operations re-encoded as packets equal the region's packets (fillers,
// zero words and plain NOPs aside), word for word.
inline bool SamePiece(const uint8_t* src, uint32_t words, const uint32_t* ops, size_t size) {
  size_t o = 0;
  for (uint32_t p = 0; p < words;) {
    uint32_t n = 0;
    const Kind kind = Classify(src, p, words, n);
    if (kind == Kind::kBarrier || kind == Kind::kPartial || kind == Kind::kOurs) return false;
    const uint8_t* const hp = src + size_t(p) * 4;
    const uint32_t h = rex::memory::load_and_swap<uint32_t>(hp);
    p += n;
    if ((h >> 30) == 2 || h == 0 || kind == Kind::kNop) continue;
    if (o >= size) return false;
    const uint32_t tag = ops[o], op = tag & 0xFF, count = tag >> 8;
    if ((h >> 30) == 0) {
      if (op != (((h >> 15) & 1) ? kRunSingle : kRun) || count != n - 1 || ops[o + 1] != (h & 0x7FFF)) return false;
      for (uint32_t i = 0; i < count; ++i)
        if (ops[o + 2 + i] != rex::memory::load_and_swap<uint32_t>(hp + 4 + 4 * i)) return false;
      o += 2 + count;
    } else if ((h >> 30) == 1) {
      if (op != kType1 || ops[o + 1] != h || ops[o + 2] != rex::memory::load_and_swap<uint32_t>(hp + 4) ||
          ops[o + 3] != rex::memory::load_and_swap<uint32_t>(hp + 8))
        return false;
      o += 4;
    } else if (kind == Kind::kRef) {
      if (op != kType3Ref || count != n - 1 || ops[o + 1] != h) return false;
      o += 3;
    } else {
      if (op != kType3 || count != n - 1 || ops[o + 1] != h || std::memcmp(&ops[o + 3], hp + 4, size_t(count) * 4) != 0)
        return false;
      o += 3 + count;
    }
  }
  return o == size;
}

// --- Native producers (banjo_d3d_records_native, step 5) --------------------------------------------------------------
// The native Direct3D draw path writes the operations of the packets it writes itself, as it writes them, so the
// record needs no decoding: the same operations DecodePiece would make of those packets (the producer's check
// compares the two). `cur` null: none (the hook then decodes the region from the packets, which are still written).
struct RecordOps {
  uint32_t* begin = nullptr;
  uint32_t* cur = nullptr;
  uint32_t* end = nullptr;
};
inline constexpr uint32_t kStagingWords = 0x10000;  // a call's operations (a draw writes far less than a segment)
inline constexpr uint32_t kBlockRoom = 0x1200;      // the most one native block (a register or constant run set) needs

// A type-0 run from guest-order words (a Direct3D shadow).
[[gnu::always_inline]] inline uint32_t* EmitRun(uint32_t* out, uint32_t reg, const uint8_t* guest, uint32_t count) {
  out[0] = kRun | (count << 8);
  out[1] = reg;
  SwapWords(out + 2, guest, count);
  return out + 2 + count;
}
// A type-3 packet from host-order words: `physical` is the packet's physical address.
[[gnu::always_inline]] inline uint32_t* EmitType3(uint32_t* out, uint32_t header, uint32_t physical,
                                                  const uint32_t* host, uint32_t count) {
  out[0] = kType3 | (count << 8);
  out[1] = header;
  out[2] = physical & 0x1FFFFFFFu;
  for (uint32_t i = 0; i < count; ++i) out[3 + i] = __builtin_bswap32(host[i]);  // guest order, as in the buffer
  return out + 3 + count;
}
// A tile mask or select, by reference.
[[gnu::always_inline]] inline uint32_t* EmitRef(uint32_t* out, uint32_t header, uint32_t physical) {
  out[0] = kType3Ref | ((((header >> 16) & 0x3FFF) + 1) << 8);
  out[1] = header;
  out[2] = physical;
  return out + 3;
}

}  // namespace banjo::records
