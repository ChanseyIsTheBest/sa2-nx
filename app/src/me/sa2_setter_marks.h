// sa2-nx (2026-10-28, sa2_renut_setter_marks): the Direct3D setters' marks (renut_engine/gpu/sa2_setter_marks.h says
// what they are for). SA2_SETTER_MARKS(ADDR), first in a hook's body (the hot hooks' macros, cpu/sa2_hot_guest.h, and
// the hooks of sa2_setter_marks.cpp), marks the chunks the function at ADDR may write without a dirty tag, from its
// arguments; for any other address it is nothing. The setters draw nothing, so their marks are taken at the chain's
// next draw either way: they mark at entry, which keeps nothing live across the call (a hook of its own is then a
// mark and a branch to the original). The whole marks, for functions that may reach a draw, mark after the call.
//
// The functions and what they store without a tag (their PPC, the census build's draw-thread lines):
//   sub_82552408 SetStreamSource(device, stream s, ...): the vertex fetch constant at 0x778 - 8 s (its tag comes from
//     the caller), the stream's buffer at 0x3224 + 4 s and stride at 0x3268 + s
//   sub_82549178 SetTexture(device, sampler n, ...): fetch constant n at 0x480 + 24 n (its tag from the caller), the
//     texture at 0x3278 + 4 n
//   sub_8255DE38 / sub_8255DC30 SetVertexShader / SetPixelShader: the shader at 0x3310 / 0x330C
//   sub_825529F0 (the viewport): floats at 0x32E0-0x32FB, the window offsets at 0x2908-0x291F
//   sub_82552298 (the scissor): 0x32FC-0x330B, the window scissor at 0x28C4
//   sub_82552C48: 0x3000 (then sub_82552298)
//   sub_82552CF8 (the depth surface's state): 0x2880, 0x29BC, 0x32FC-0x330B (then the viewport)
//   sub_82552D88 SetRenderTarget(device, index i, surface): the target at 0x3210 + 4 i, its registers at 0x2884 + 4 i
//     and copies at 0x3078 + 4 i, 0x3004-0x3010 (sub_825533F0 is a branch to it)
//   sub_82553118 SetDepthStencilSurface: the surface at 0x3220, its registers (0x2880-0x2888, 0x2934, 0x2940) and
//     copies (0x3014, 0x3018)
//   sub_825632F8 (the clear): consumes the dirty tags it finds (its packets): their chunks
//   every other Direct3D function that stores to a compared chunk without a tag the static reading can name
//     (tools/d3d/setter_marks_tags.py's reading; none of them ran on the draw thread in the census): all chunks
//
// sa2-nx (2026-10-28, sa2_renut_setters_precise; banjo-nx's setter rewrite, renut_engine/gpu/sa2_setter_marks.h): the
// hottest setters (SetTexture, SetStreamSource, the two fetch word setters sub_82551FE8 / sub_82552038, the two float
// constant uploads) mark instead the chunks whose bytes the call changed (their bytes before the call against after
// it), and the dirty bits the call added (its tag, or its constant groups) go apart from the device: a bit whose state
// did not change needs nothing at the draw, and one whose state did is marked by the bytes. The functions run as they
// did (the localized copy, the guarded native or the original: their writes and registers are theirs); the scope reads
// before and fixes the dirty words after. A function that reads m_Pending (tools: scratchpad pending_readers2.py, the
// Direct3D functions whose loads of words 0-4 are more than an 'or' into them) first gets the bits back
// (Materialise): through the scopes below, and the hooks of sa2_setter_marks.cpp for those without one. Where an
// inline write changes a field a precise call wrote, it sets the field's tag itself (the census found every inline
// register write tagged), which is the device's and is taken at the draw.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <rex/ppc.h>

#include "cpu/sa2_hot_common.h"
#include "renut_engine/gpu/sa2_setter_marks.h"

namespace sa2::marks {

using sa2::renut::ChunkBits;
using sa2::renut::MarkBits;

struct StreamTable {
  ChunkBits bits[16];
};
constexpr StreamTable MakeStreamTable() {
  StreamTable t{};
  for (uint32_t s = 0; s < 16; ++s)
    t.bits[s] = MarkBits(0x778 - 8 * s, 8) | MarkBits(0x3224 + 4 * s, 4) | MarkBits(0x3268 + s, 1);
  return t;
}
inline constexpr StreamTable kStreams = MakeStreamTable();
struct TextureTable {
  ChunkBits bits[32];
};
constexpr TextureTable MakeTextureTable() {
  TextureTable t{};
  for (uint32_t n = 0; n < 32; ++n) t.bits[n] = MarkBits(0x480 + 24 * n, 24) | MarkBits(0x3278 + 4 * n, 4);
  return t;
}
inline constexpr TextureTable kTextures = MakeTextureTable();

// sa2-nx (sa2_renut_setters_precise): the precise path is for the chain's device, while the chain has it on.
inline bool Precise(uint32_t device) {
  return sa2::renut::g_setters_precise && device == sa2::renut::g_setter_marks.device && device != 0;
}
inline uint64_t LoadWord(const uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, sa2::hot::Raw(base, a), 8);
  return __builtin_bswap64(v);
}
inline void StoreWord(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(sa2::hot::Raw(base, a), &v, 8);
  sa2::hot::Card(base, a, 8);
}
// The bits held apart, back into the device's m_Pending words (before a function that reads them), on the thread that
// holds them (renut_engine/gpu/sa2_setter_marks.h, g_apart_owner).
inline void Materialise(const PPCContext& ctx, uint8_t* base) {
  auto& g = sa2::renut::g_setter_marks;
  if (!g.apart_any || sa2::renut::g_apart_owner != &ctx) [[likely]] return;
  for (uint32_t w = 0; w < 5; ++w) {
    if (const uint64_t bits = g.apart[w]) {
      StoreWord(base, g.device + 8 * w, LoadWord(base, g.device + 8 * w) | bits);
      g.apart[w] = 0;
    }
  }
  g.apart_any = 0;
}
// After a precise call on the thread of 'ctx': the bits of word 'w' it added (now & ~before) go apart. Bits another
// thread holds (none in SA2, whose draws are one thread's) go back first, so that one thread holds them all.
inline void HoldBits(const PPCContext& ctx, uint8_t* base, uint32_t w, uint64_t bits) {
  auto& g = sa2::renut::g_setter_marks;
  if (g.apart_any && sa2::renut::g_apart_owner != &ctx) [[unlikely]] {
    for (uint32_t k = 0; k < 5; ++k) {
      if (const uint64_t held = g.apart[k]) {
        StoreWord(base, g.device + 8 * k, LoadWord(base, g.device + 8 * k) | held);
        g.apart[k] = 0;
      }
    }
    g.apart_any = 0;
  }
  g.apart[w] |= bits;
  g.apart_any = 1;
  sa2::renut::g_apart_owner = &ctx;
}
inline void HoldApart(const PPCContext& ctx, uint8_t* base, uint32_t device, uint32_t w, uint64_t before) {
  const uint64_t now = LoadWord(base, device + 8 * w);
  const uint64_t added = now & ~before;
  if (!added) return;
  StoreWord(base, device + 8 * w, now & ~added);
  HoldBits(ctx, base, w, added);
}
// A device field's bytes as one word (fixed sizes: the compiler keeps the loads inline), and the mark of its one chunk
// when it changed (the fields below are aligned to their size, so a word never spans two chunks).
template <typename T>
inline T Field(const uint8_t* base, uint32_t a) {
  T v;
  std::memcpy(&v, sa2::hot::Raw(base, a), sizeof(T));
  return v;
}
template <typename T>
inline void MarkIfChanged(const uint8_t* base, uint32_t device, uint32_t offset, T before) {
  if (Field<T>(base, device + offset) != before) sa2::renut::MarkChunkOf(device, offset);
}

// sa2_renut_setters_precise: the hooked functions that read m_Pending for more than an 'or' into it (scratchpad
// pending_readers2.py; those with no hook of their own: SA2_TAG_READER_HOOK in sa2_setter_marks.cpp, and the clear and
// the whole-mark functions below). Only these get the bits back: a setter that only adds its own bit (the viewport's,
// at every draw) leaves them apart. The console's second run had every hooked Direct3D function give them back, which
// put them into the device before most draws.
constexpr bool TagReader(uint32_t a) {
  switch (a) {
    case 0x82553118u:  // SetDepthStencilSurface
    case 0x82562D58u:
      return true;
    default:
      return false;
  }
}

// A fixed set of chunks for the device in r3 (a function that reads the dirty words gets them back first).
template <uint64_t W0, uint64_t W1, uint32_t A>
struct Fixed {
  Fixed(const PPCContext& ctx, [[maybe_unused]] uint8_t* base) {
    if constexpr (TagReader(A)) Materialise(ctx, base);
    sa2::renut::MarkFixed<W0, W1>(ctx.r3.u32);
  }
};
// Every chunk, after the call.
struct All {
  All(const PPCContext& ctx, uint8_t* base) { Materialise(ctx, base); }
  ~All() { sa2::renut::MarkAll(); }
};

// No marks; a reader of the dirty words gets the bits held apart first.
template <uint32_t A>
struct Scope {
  Scope([[maybe_unused]] const PPCContext& ctx, [[maybe_unused]] uint8_t* base) {
    if constexpr (TagReader(A)) Materialise(ctx, base);
  }
};

#define SA2_MARKS_FIXED(ADDR, BITS)                                                           \
  template <>                                                                                 \
  struct Scope<0x##ADDR##u> : Fixed<(BITS).w[0], (BITS).w[1], 0x##ADDR##u> {                               \
    using Fixed::Fixed;                                                                       \
  };
#define SA2_MARKS_ALL(ADDR)                                                                   \
  template <>                                                                                 \
  struct Scope<0x##ADDR##u> : All {                                                           \
    using All::All;                                                                           \
  };

template <>
struct Scope<0x82552408u> {  // SetStreamSource
  // sa2_renut_setters_precise: the fetch constant (8 bytes), the buffer (4) and the stride (1); words 2 and 3.
  const PPCContext* ctx_ = nullptr;
  uint8_t* base_ = nullptr;
  uint32_t device_ = 0, stream_ = 0;
  uint64_t tags_[2] = {};
  uint64_t fetch_ = 0;
  uint32_t buffer_ = 0;
  uint8_t stride_ = 0;
  Scope(const PPCContext& ctx, uint8_t* base) {
    const uint32_t device = ctx.r3.u32, stream = ctx.r4.u32;
    if (stream < 16 && Precise(device)) [[likely]] {
      ctx_ = &ctx;
      base_ = base;
      device_ = device;
      stream_ = stream;
      fetch_ = Field<uint64_t>(base, device + 0x778 - 8 * stream);
      buffer_ = Field<uint32_t>(base, device + 0x3224 + 4 * stream);
      stride_ = Field<uint8_t>(base, device + 0x3268 + stream);
      tags_[0] = LoadWord(base, device + 16);
      tags_[1] = LoadWord(base, device + 24);
      return;
    }
    if (stream < 16) [[likely]]
      sa2::renut::Mark(device, kStreams.bits[stream]);
    else
      sa2::renut::MarkAll();
  }
  ~Scope() {
    if (!device_) return;
    MarkIfChanged(base_, device_, 0x778 - 8 * stream_, fetch_);
    MarkIfChanged(base_, device_, 0x3224 + 4 * stream_, buffer_);
    MarkIfChanged(base_, device_, 0x3268 + stream_, stride_);
    HoldApart(*ctx_, base_, device_, 2, tags_[0]);
    HoldApart(*ctx_, base_, device_, 3, tags_[1]);
  }
};
template <>
struct Scope<0x82549178u> {  // SetTexture
  // sa2_renut_setters_precise: the fetch constant (24 bytes) and the texture (4); word 3.
  const PPCContext* ctx_ = nullptr;
  uint8_t* base_ = nullptr;
  uint32_t device_ = 0, sampler_ = 0;
  uint64_t tags_ = 0;
  uint64_t fetch_[3] = {};
  uint32_t texture_ = 0;
  Scope(const PPCContext& ctx, uint8_t* base) {
    const uint32_t device = ctx.r3.u32, sampler = ctx.r4.u32;
    if (sampler < 32 && Precise(device)) [[likely]] {
      ctx_ = &ctx;
      base_ = base;
      device_ = device;
      sampler_ = sampler;
      const uint32_t at = device + 0x480 + 24 * sampler;  // 8-aligned: three words
      fetch_[0] = Field<uint64_t>(base, at);
      fetch_[1] = Field<uint64_t>(base, at + 8);
      fetch_[2] = Field<uint64_t>(base, at + 16);
      texture_ = Field<uint32_t>(base, device + 0x3278 + 4 * sampler);
      tags_ = LoadWord(base, device + 24);
      return;
    }
    if (sampler < 32) [[likely]]
      sa2::renut::Mark(device, kTextures.bits[sampler]);
    else
      sa2::renut::MarkAll();
  }
  ~Scope() {
    if (!device_) return;
    const uint32_t at = 0x480 + 24 * sampler_;
    MarkIfChanged(base_, device_, at, fetch_[0]);
    MarkIfChanged(base_, device_, at + 8, fetch_[1]);
    MarkIfChanged(base_, device_, at + 16, fetch_[2]);
    MarkIfChanged(base_, device_, 0x3278 + 4 * sampler_, texture_);
    HoldApart(*ctx_, base_, device_, 3, tags_);
  }
};
// sub_82551FE8 / sub_82552038: three bits of fetch constant n's first word (r4 = n, r5 = the value) and its tag (word 3,
// bit 31 - n; none for n of 32 and above, whose shift leaves nothing). Before, their tag was their mark.
template <uint32_t A>
struct FetchWordScope {
  const PPCContext* ctx_ = nullptr;
  uint8_t* base_ = nullptr;
  uint32_t device_ = 0, slot_ = 0;
  uint64_t tags_ = 0;
  uint32_t word_ = 0;
  FetchWordScope(const PPCContext& ctx, uint8_t* base) {
    const uint32_t device = ctx.r3.u32, slot = ctx.r4.u32;
    if (slot < 32 && Precise(device)) [[likely]] {
      ctx_ = &ctx;
      base_ = base;
      device_ = device;
      slot_ = slot;
      word_ = Field<uint32_t>(base, device + 0x480 + 24 * slot);
      tags_ = LoadWord(base, device + 24);
    }
  }
  ~FetchWordScope() {
    if (!device_) return;
    MarkIfChanged(base_, device_, 0x480 + 24 * slot_, word_);
    HoldApart(*ctx_, base_, device_, 3, tags_);
  }
};
template <>
struct Scope<0x82551FE8u> : FetchWordScope<0x82551FE8u> {
  using FetchWordScope::FetchWordScope;
};
template <>
struct Scope<0x82552038u> : FetchWordScope<0x82552038u> {
  using FetchWordScope::FetchWordScope;
};
// sub_8255D8B8 / sub_8255D990, the float constant uploads (cpu/hot/n_d3d_constants.h: r4 = the first constant, r5 = the
// source, r6 = the count of 16-byte vectors, r7 = the dirty bits, ORed into word 0 / 1; group g = constants 4g-4g+3, bit
// 63 - g). sa2_renut_setters_precise: the groups whose new bytes differ from the render thread's (its constants'
// copy on the game thread, renut_engine/gpu/sa2_setter_marks.h g_const_shadow: in this core's cache, where the
// device's lines would be a load miss each before the copy stores them) are found before the call, and the bits it adds
// for any other group go apart: the render thread has those bytes already.
template <uint32_t First, uint32_t Word>
struct ConstantsScope {
  const PPCContext* ctx_ = nullptr;
  uint8_t* base_ = nullptr;
  uint32_t device_ = 0;
  uint64_t before_ = 0, changed_ = 0;
  ConstantsScope(const PPCContext& ctx, uint8_t* base) {
    const uint32_t device = ctx.r3.u32;
    const uint8_t* shadow = sa2::renut::g_const_shadow;
    if (!Precise(device) || !shadow || (ctx.r6.u64 >> 32)) return;
    const uint32_t first = ctx.r4.u32, count = ctx.r6.u32, src = ctx.r5.u32;
    if (first >= 256 || count > 256 - first) return;  // past the 256 constants: as before
    const uint32_t dst = ((first + First) << 4) + device;
    if (dst & 15u) return;  // the device is 16-aligned; anything else is as before
    if (uint64_t(src) + uint64_t(count) * 16 > 0x100000000ull) return;
    // Overlapping ranges: the copy's loads see its own stores (four vectors at a time); as before. A source across the
    // physical-view boundary (the PC's 4 KB offset): as before.
    if (src < dst + count * 16 && dst < src + count * 16) return;
    if (count && sa2::hot::PhysOff(src) != sa2::hot::PhysOff(src + count * 16 - 1)) return;
    uint64_t changed = 0;
    const uint8_t* s = sa2::hot::Raw(base, src);
    const uint8_t* r = shadow + (First == 120 ? 0u : 4096u) + first * 16;  // the render thread's, from 0x780
    for (uint32_t v = 0; v < count; ++v) {
      uint64_t a[2], b[2];
      std::memcpy(a, s + 16 * v, 16);
      std::memcpy(b, r + 16 * v, 16);
      if ((a[0] ^ b[0]) | (a[1] ^ b[1])) changed |= uint64_t(1) << (63 - (first + v) / 4);
    }
    ctx_ = &ctx;
    base_ = base;
    device_ = device;
    before_ = LoadWord(base, device + 8 * Word);
    changed_ = changed;
  }
  ~ConstantsScope() {
    if (!device_) return;
    const uint64_t now = LoadWord(base_, device_ + 8 * Word);
    const uint64_t apart = now & ~before_ & ~changed_;
    if (!apart) return;
    StoreWord(base_, device_ + 8 * Word, now & ~apart);
    HoldBits(*ctx_, base_, Word, apart);
  }
};
template <>
struct Scope<0x8255D8B8u> : ConstantsScope<120, 0> {  // vertex shader constants, word 0
  using ConstantsScope::ConstantsScope;
};
template <>
struct Scope<0x8255D990u> : ConstantsScope<376, 1> {  // pixel shader constants, word 1
  using ConstantsScope::ConstantsScope;
};
template <>
struct Scope<0x825632F8u> {  // the clear: the chunks of the dirty tags pending at its start (it consumes some)
  Scope(const PPCContext& ctx, uint8_t* base) {
    if (const uint32_t device = sa2::renut::MarkedDevice()) {
      uint64_t tags[3];
      std::memcpy(tags, base + device + 16, sizeof(tags));
      sa2::renut::Mark(device, sa2::renut::TagChunks(__builtin_bswap64(tags[0]), __builtin_bswap64(tags[1]),
                                                     __builtin_bswap64(tags[2])));
    }
    // sa2_renut_setters_precise: then the bits held apart (their state is the render thread's already) for it to find.
    Materialise(ctx, base);
  }
};

// SetVertexShader / SetPixelShader: the shader at 0x3310 / 0x330C (their mark), an 'or' of their tags into words 2 and
// 4, an 'and not' of word 0 / 1 (the float constant groups the new shader's own constants cover), and those constants
// copied into the device. sa2_renut_setters_precise: they read nothing else of m_Pending, so only that word's held bits
// go back into the device before the call, for good (the groups the call writes keep their bits: a verify run caught
// the version that held the bits it left apart again, 3,133 draws whose rebuilt constants differed in the story
// stage); the other words' held bits (the fetch and stream tags) stay apart, instead of every held bit going back at
// every shader change.
template <uint32_t Word>
struct GiveBackWord {
  GiveBackWord(const PPCContext& ctx, uint8_t* base) {
    constexpr ChunkBits kShader = MarkBits(0x3300, 32);
    sa2::renut::MarkFixed<kShader.w[0], kShader.w[1]>(ctx.r3.u32);
    auto& g = sa2::renut::g_setter_marks;
    if (!g.apart_any || sa2::renut::g_apart_owner != &ctx || !g.apart[Word]) [[likely]] return;
    const uint64_t held = g.apart[Word];
    g.apart[Word] = 0;
    g.apart_any = (g.apart[0] | g.apart[1] | g.apart[2] | g.apart[3] | g.apart[4]) != 0;
    StoreWord(base, g.device + 8 * Word, LoadWord(base, g.device + 8 * Word) | held);
  }
};
template <>
struct Scope<0x8255DE38u> : GiveBackWord<0> {  // SetVertexShader
  using GiveBackWord::GiveBackWord;
};
template <>
struct Scope<0x8255DC30u> : GiveBackWord<1> {  // SetPixelShader
  using GiveBackWord::GiveBackWord;
};
SA2_MARKS_FIXED(825529F0, MarkBits(0x28C0, 0x80) | MarkBits(0x32C0, 0x60))  // the viewport (and its scissor)
SA2_MARKS_FIXED(82552298, MarkBits(0x28C0, 0x40) | MarkBits(0x32C0, 0x60))  // the scissor
SA2_MARKS_FIXED(82552C48, MarkBits(0x3000, 0x40))
SA2_MARKS_FIXED(82552CF8, MarkBits(0x2880, 0x40) | MarkBits(0x2980, 0x40) | MarkBits(0x32C0, 0x60))
SA2_MARKS_FIXED(82552D88, MarkBits(0x2880, 0x80) | MarkBits(0x3000, 0xC0) | MarkBits(0x3200, 0x40))  // SetRenderTarget
SA2_MARKS_FIXED(82553118, MarkBits(0x2880, 0x40) | MarkBits(0x2900, 0x80) | MarkBits(0x3000, 0x40) |
                              MarkBits(0x3200, 0x40))  // SetDepthStencilSurface
// The rest that store to a compared chunk untagged, or tags the reading cannot name: whole.
SA2_MARKS_ALL(82559600)  // the present
SA2_MARKS_ALL(82550D98)
SA2_MARKS_ALL(8255EFB8)
SA2_MARKS_ALL(825525B0)  // SetIndices
SA2_MARKS_ALL(8256FE38)
SA2_MARKS_ALL(8256FB60)
SA2_MARKS_ALL(8256DF00)
SA2_MARKS_ALL(8256ABC0)
SA2_MARKS_ALL(82568DB8)
SA2_MARKS_ALL(825645A0)  // the resolve core (sa2_me_hooks.cpp's hook)
SA2_MARKS_ALL(8255DAE8)
SA2_MARKS_ALL(8255DA88)
SA2_MARKS_ALL(82558348)
SA2_MARKS_ALL(82557A40)
SA2_MARKS_ALL(825576D0)
SA2_MARKS_ALL(82557560)
SA2_MARKS_ALL(825573E0)
SA2_MARKS_ALL(82556A60)  // the draw-time shader load (sa2_me_hooks.cpp's hook, and its copy)
SA2_MARKS_ALL(825553A8)
SA2_MARKS_ALL(8254B108)
SA2_MARKS_ALL(82549F98)
SA2_MARKS_ALL(82549EA0)
SA2_MARKS_ALL(8256E1D0)
SA2_MARKS_ALL(8256E2C8)
SA2_MARKS_ALL(825528A8)

#undef SA2_MARKS_FIXED
#undef SA2_MARKS_ALL

}  // namespace sa2::marks

#define SA2_SETTER_MARKS(ADDR) const sa2::marks::Scope<0x##ADDR##u> sa2_setter_marks_scope_{ctx, base}
