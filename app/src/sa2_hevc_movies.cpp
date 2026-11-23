// sa2-nx-ult (2026-11-21, the user: "rencode all the videos to h.265 please and all the audio to opus in a web installer
// like culdcept"): Sonic Adventure 2's Sofdec movies (.sfd: MPEG-1 video and ADX audio, played by CRI's MWSFD 5.54 /
// CRI MPV 2.081 in the game's own code) shown from H.265 on the Switch's NVDEC instead of decoded as MPEG-1 by the
// guest: rr6-nx's way (its app/src/cpu/rr6_hevc_movies.cpp, whose code this follows, and rex/filesystem/sfd_hevc.h),
// itself culdcept-nx's (app/src/cpu/culdcept_wmv_ffmpeg.cpp and nxvideo, tools/switch/nxvideo); credit both.
// NAME.hevc shows the game the .sfd with each picture's slice data zero but for a tag (the movie and the picture's
// display index) after its first slice header. docs/opus-h265.md has the plan and the findings.
//
// Found in SA2's code (2026-11-21; the fields confirmed by sa2_hevc_diag on the attract sequence's 320x240 movie):
//   sub_82901C40(mpv, sj, out)   CRI MPV: one slice. The stream joint's GetChunk (vtable +24) into mpv+5288 (address,
//                                +5292 length), the slice start code's row, the macroblock address mpv+5204 (row *
//                                mpv+4836, the macroblocks a row, minus 1), the quantiser (+5128), the extra bytes; what
//                                it read goes back (PutChunk +32, UngetChunk +28) with the bit offset in +5296; then the
//                                picture type's slice decoder mpv+5084 (mpv, sj); the picture is done when mpv+5204
//                                reaches mpv+5216 (its last macroblock).
//   sub_8296BA00 / sub_8296C550 / sub_8296BF68   the slice decoders of I, P and B pictures (the PSFD00 section; the
//                                table 0x820BCCD0 by picture_coding_type, chosen by the picture header's parser
//                                sub_828F7780 into mpv+5084). Reached only through mpv+5084: hooked, they take every
//                                slice. Their first byte is the one with the slice header's last bits (bit offset 6
//                                for the 6 bits of quantiser and extra_bit_slice), so a tag is at that byte + 1.
//   mpv+5008 / +5012 / +5016     the picture being decoded: its Cb, Cr and Y planes (+4992..+5000 the backward
//                                reference, +4976..+4984 the forward one; for an I or P picture +4992..+5000 is the
//                                picture itself, for a B one not: the emulator's e0401_m showed it). The frame buffer
//                                mwPlyGetCurFrm hands out is the Y plane's address. The layout is
//                                rr6-nx's SofdecPlanes: Y pitch the width rounded to 16 then 128, Cb at Y + pitch *
//                                rows (16-rounded), Cr after Cb at half the 16-rounded width rounded to 128.
//   sub_828E06F8(ply, frm)       MWSFD's mwPlyGetCurFrm: frm (168 bytes) filled by sub_828E01B0, +0 the buffer, +12 / +16
//                                the width and height (rr6-nx's layout), +28 / +32 the macroblocks across and down
//
// The hooks: (1) a slice decoder whose slice starts with a tag decodes nothing: the tag goes into the picture's Y plane
// and mpv+5204 is set to the picture's last macroblock (the slice loop ends; the rest of the picture is zero, which the
// next start code search skips); (2) mwPlyGetCurFrm: a frame whose Y plane starts with a tag gets the H.265 picture of
// that display index written over it. Frame buffers, timing, the order of pictures and the audio stay CRI's.
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem/sfd_hevc.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if RENUT_NXVIDEO
#include <switch.h>  // svcGetThreadPriority / svcSetThreadPriority (sa2_hevc_decode_priority), threadCreate

#include "nxvideo.h"
#endif

REXCVAR_DEFINE_BOOL(sa2_hevc_movies, true, "SA2",
                    "The movies the installer made H.265 (NAME.hevc) shown from H.265 on the Switch's video hardware; "
                    "false: their tags are not looked for (such a movie then shows nothing)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// rr6-nx (2026-11-21): Ryujinx opens nvtegra's NVDEC for H.265 but emulates no H.265 decoding; tests in it use false.
REXCVAR_DEFINE_BOOL(sa2_hevc_nvdec, true, "SA2",
                    "The H.265 movies on the Switch's video hardware (NVDEC); false: FFmpeg's software decoder (slow: for "
                    "an emulator, which has no H.265 NVDEC)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_hevc_log, false, "SA2",
                    "One [hevc] line every 300 pictures shown from H.265: the time reading, decoding and copying, and "
                    "nxvideo's own steps and clocks")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// rr6-nx (2026-11-21) and culdcept-nx (2026-11-20): the decode waits on the hardware on the game's own thread; at the
// game's priority it waits for a core behind the game's other threads, which Horizon does not time-slice.
REXCVAR_DEFINE_INT32(sa2_hevc_decode_priority, 0x2C, "SA2",
                     "The thread's priority while it waits for an H.265 picture on NVDEC (0x1C-0x3B, lower runs first); "
                     "0 = the thread's own")
    .range(0, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_hevc_diag, false, "SA2",
                    "Log CRI MPV's fields at the first slices of each picture type and the first frames mwPlyGetCurFrm "
                    "gives the game (the H.265 movie work's diagnostic)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8296BA00);  // CRI MPV: an I picture's slice (mpv, sj)
REX_EXTERN(__imp__sub_8296C550);  // a P picture's
REX_EXTERN(__imp__sub_8296BF68);  // a B picture's
REX_EXTERN(__imp__sub_828E06F8);  // MWSFD: mwPlyGetCurFrm (ply, frm)

namespace {

constexpr uint32_t kChunk = 5288;      // mpv+5288: the slice decoder's chunk (address, +4 length)
constexpr uint32_t kMbNow = 5204;      // mpv+5204: the last macroblock done
constexpr uint32_t kMbLast = 5216;     // mpv+5216: the picture's last macroblock
// mpv+5016: the picture being decoded, its Y plane (+5008 / +5012 its Cb / Cr). 2026-11-21, the emulator's e0401_m (B
// pictures): +4992..+5000 is the backward reference (the last I or P picture), the same as +5008..+5016 for an I or P
// picture but not for a B one, whose own planes are +5008..+5016 (and +5040..+5048). The first form wrote a B
// picture's tag into its reference: that reference then showed the B picture's index and the B picture no tag.
constexpr uint32_t kPictureY = 5016;
// A tag whose display index could not be read: the frame after the last one shown (rr6-nx's).
constexpr uint32_t kIndexUnknown = 0x1FFFFF;

uint8_t* Host(uint8_t* base, uint32_t a) { return rex::memory::GuestPtr(base, a); }
uint32_t Ld32(uint8_t* base, uint32_t a) {
  const uint8_t* p = Host(base, a);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void St32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = Host(base, a);
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
  rex::memory::MarkStoreCardsAt(base, a, 4);
}

std::atomic<uint64_t> g_tagged_slices{0}, g_shown{0}, g_missing{0}, g_cut_tags{0};
std::mutex g_mpv_mutex;
std::map<uint32_t, uint32_t> g_mpv_movie;  // the H.265 movie each CRI MPV decoder last decoded a tagged picture of

// The slice's first bytes as the decoder will take them (GetChunk(sj, 1, 0x7FFFFFFF, mpv+5288), then
// UngetChunk(sj, 1, mpv+5288) as they were, the slice loop's own look, sub_82901C40): up to `n` into `out`, the count.
// The registers the decoder takes are put back.
uint32_t PeekSlice(PPCContext& ctx, uint8_t* base, uint32_t mpv, uint32_t sj, uint8_t* out, uint32_t n) {
  if (!sj) return 0;
  const uint32_t vtable = Ld32(base, sj);
  PPCFunc* get = rex::runtime::ResolveIndirectFunction(Ld32(base, vtable + 24));
  PPCFunc* unget = rex::runtime::ResolveIndirectFunction(Ld32(base, vtable + 28));
  if (!get || !unget) return 0;
  const uint64_t r1 = ctx.r1.u64, r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
  ctx.r1.u64 = r1 - 128;
  ctx.r3.u64 = sj;
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 0x7FFFFFFF;
  ctx.r6.u64 = mpv + kChunk;
  get(ctx, base);
  const uint32_t p = Ld32(base, mpv + kChunk), len = Ld32(base, mpv + kChunk + 4);
  const uint32_t have = p ? std::min(len, n) : 0;
  if (have) std::memcpy(out, Host(base, p), have);
  ctx.r1.u64 = r1 - 128;
  ctx.r3.u64 = sj;
  ctx.r4.u64 = 1;
  ctx.r5.u64 = mpv + kChunk;
  unget(ctx, base);
  ctx.r1.u64 = r1;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  return have;
}

void SliceDiag(uint8_t* base, const char* type, uint32_t mpv, uint32_t sj, const uint8_t* bytes, uint32_t have,
               std::atomic<int>& count) {
  if (count.fetch_add(1, std::memory_order_relaxed) >= 4) return;
  std::string fields;
  for (uint32_t off : {4828u, 4832u, 4836u, 4976u, 4980u, 4984u, 4992u, 4996u, 5000u, 5008u, 5012u, 5016u, 5040u,
                       5044u, 5048u, 5084u, 5128u, 5204u, 5216u, 5296u})
    fields += fmt::format(" +{}={:08X}", off, Ld32(base, mpv + off));
  std::string hex;
  for (uint32_t i = 0; i < have; ++i) hex += fmt::format(" {:02X}", bytes[i]);
  REXLOG_INFO("[hevc] {} slice: mpv {:08X} sj {:08X}:{}; slice bytes ({}):{}", type, mpv, sj, fields, have, hex);
}

// The slice hook: true when the slice was a tagged one (nothing for the decoder to do).
bool TaggedSlice(PPCContext& ctx, uint8_t* base, const char* type, std::atomic<int>& diag_count) {
  static const bool on = REXCVAR_GET(sa2_hevc_movies);
  static const bool diag = REXCVAR_GET(sa2_hevc_diag);
  if (!on && !diag) return false;
  const uint32_t mpv = ctx.r3.u32, sj = ctx.r4.u32;
  uint8_t head[1 + 4 + rex::filesystem::kSfdHevcTagBytes] = {};
  const uint32_t have = PeekSlice(ctx, base, mpv, sj, head, sizeof(head));
  if (diag) SliceDiag(base, type, mpv, sj, head, have, diag_count);
  if (!on || !have) return false;
  bool tagged = false;
  uint8_t tag[rex::filesystem::kSfdHevcTagBytes];
  for (uint32_t off = 1; off <= 4 && off + rex::filesystem::kSfdHevcTagBytes <= have && !tagged; ++off) {
    uint32_t id, index;
    if (rex::filesystem::ParseSfdHevcTag(head + off, &id, &index)) {
      tagged = true;
      std::memcpy(tag, head + off, sizeof(tag));
      std::lock_guard<std::mutex> lock(g_mpv_mutex);
      g_mpv_movie[mpv] = id;
    }
  }
  // rr6-nx's safety net: a chunk that ends before the tag (CRI's ring keeps a picture's bytes contiguous, so it should
  // not happen). A decoder playing an H.265 movie still decodes nothing; the picture's tag has no display index.
  if (!tagged && have < sizeof(head)) {
    std::lock_guard<std::mutex> lock(g_mpv_mutex);
    auto it = g_mpv_movie.find(mpv);
    if (it != g_mpv_movie.end()) {
      tagged = true;
      rex::filesystem::MakeSfdHevcTag(it->second, kIndexUnknown, tag);
      g_cut_tags.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (!tagged) return false;
  const uint32_t y = Ld32(base, mpv + kPictureY);
  if (y) {
    std::memcpy(Host(base, y), tag, sizeof(tag));
    rex::memory::MarkStoreCardsAt(base, y, sizeof(tag));
  }
  St32(base, mpv + kMbNow, Ld32(base, mpv + kMbLast));  // every macroblock done: the slice loop ends the picture
  g_tagged_slices.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = 0;
  return true;
}

// The Sofdec frame buffer's planes for a w x h picture (rr6-nx's SofdecPlanes; SA2's confirmed on the attract movie:
// 320x240 Y pitch 384, Cb 92,160 bytes after Y, pitch 256, Cr 30,720 after Cb).
struct Planes {
  uint32_t y, cb, cr, y_pitch, c_pitch;
};
Planes SofdecPlanes(uint32_t buf, uint32_t w, uint32_t h) {
  const uint32_t w16 = (w + 15) & ~15u, h16 = (h + 15) & ~15u;
  Planes p;
  p.y_pitch = (w16 + 127) & ~127u;
  p.c_pitch = ((w16 / 2) + 127) & ~127u;
  p.y = buf;
  p.cb = buf + p.y_pitch * h16;
  p.cr = p.cb + p.c_pitch * (h16 / 2);
  return p;
}

#if RENUT_NXVIDEO
// rr6-nx (2026-11-21): the frames a movie will ask for next are read by a thread of their own, kAhead of them ahead of
// the decoder; the game's thread reads a frame itself only when the read-ahead has not got it yet.
constexpr int64_t kAhead = 45;  // 1.5 s at 30 frames a second

struct Prefetch {
  std::shared_ptr<const rex::filesystem::SfdHevcMovie> movie;
  std::mutex m;
  int64_t want = 0;  // the first frame the decoder still needs
  std::map<uint32_t, std::vector<uint8_t>> ready;
  uint64_t hits = 0, misses = 0;
};

std::mutex g_ahead_mutex;
std::condition_variable g_ahead_cv;
std::vector<std::weak_ptr<Prefetch>> g_ahead;

void AheadLoop() {
  std::vector<uint8_t> bytes;
  for (;;) {
    std::shared_ptr<Prefetch> job;
    uint32_t frame = 0;
    {
      std::unique_lock<std::mutex> lock(g_ahead_mutex);
      for (;;) {
        g_ahead.erase(std::remove_if(g_ahead.begin(), g_ahead.end(), [](const auto& w) { return w.expired(); }),
                      g_ahead.end());
        for (const auto& w : g_ahead) {
          auto p = w.lock();
          if (!p) continue;
          std::lock_guard<std::mutex> pl(p->m);
          const int64_t end = std::min<int64_t>(p->want + kAhead, int64_t(p->movie->frames.size()));
          for (int64_t f = std::max<int64_t>(p->want, 0); f < end; ++f) {
            if (!p->ready.count(uint32_t(f))) {
              job = std::move(p);
              frame = uint32_t(f);
              break;
            }
          }
          if (job) break;
        }
        if (job) break;
        g_ahead_cv.wait_for(lock, std::chrono::milliseconds(50));
      }
    }
    if (!job->movie->ReadFrame(frame, &bytes)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    std::lock_guard<std::mutex> pl(job->m);
    if (int64_t(frame) >= job->want && int64_t(frame) < job->want + kAhead) job->ready[frame].swap(bytes);
  }
}

// The read-ahead thread, once: a libnx thread above the game's threads (rr6-nx: a std::thread started from the game's
// thread aborted the process in Ryujinx). Null (each frame read on the game's thread) if it did not start.
std::shared_ptr<Prefetch> StartAhead(std::shared_ptr<const rex::filesystem::SfdHevcMovie> movie) {
  static const bool running = [] {
    static Thread thread;
    const int prio = REXCVAR_GET(sa2_hevc_decode_priority);
    const bool ok = R_SUCCEEDED(threadCreate(&thread, [](void*) { AheadLoop(); }, nullptr, nullptr, 256 * 1024,
                                             prio > 0 ? prio : 0x2C, -2)) &&
                    R_SUCCEEDED(threadStart(&thread));
    if (!ok) REXLOG_WARN("[hevc] the read-ahead thread did not start: the frames are read on the game's thread");
    return ok;
  }();
  if (!running) return nullptr;
  auto p = std::make_shared<Prefetch>();
  p->movie = std::move(movie);
  {
    std::lock_guard<std::mutex> lock(g_ahead_mutex);
    g_ahead.push_back(p);
  }
  g_ahead_cv.notify_one();
  return p;
}

bool TakeFrame(Prefetch& p, uint32_t f, std::vector<uint8_t>* out) {
  bool hit = false;
  {
    std::lock_guard<std::mutex> lock(p.m);
    p.want = int64_t(f) + 1;
    auto it = p.ready.find(f);
    if (it != p.ready.end()) {
      out->swap(it->second);
      hit = true;
    }
    for (auto i = p.ready.begin(); i != p.ready.end();) {
      if (int64_t(i->first) < p.want || int64_t(i->first) >= p.want + kAhead)
        i = p.ready.erase(i);
      else
        ++i;
    }
    ++(hit ? p.hits : p.misses);
  }
  g_ahead_cv.notify_one();
  return hit || p.movie->ReadFrame(f, out);
}

struct Decoder {
  std::shared_ptr<const rex::filesystem::SfdHevcMovie> movie;
  std::shared_ptr<Prefetch> ahead;
  NxvDecoder* d = nullptr;
  int64_t next = 0;  // the next frame nxv_decode would give
  std::vector<uint8_t> bytes;
  std::chrono::steady_clock::time_point used;
  double decode_ms = 0, copy_ms = 0, read_ms = 0, nxv_ms = 0;
  uint64_t frames = 0;
};

int DecodeRaised(Decoder& dc, NxvPicture* pic) {
  static const int boost = REXCVAR_GET(sa2_hevc_decode_priority);
  s32 own = 0;
  const bool raised = boost > 0 && R_SUCCEEDED(svcGetThreadPriority(&own, CUR_THREAD_HANDLE)) && own > boost &&
                      R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, u32(boost)));
  const auto t0 = std::chrono::steady_clock::now();
  const int r = nxv_decode(dc.d, dc.bytes.data(), int(dc.bytes.size()), pic);
  dc.nxv_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (raised) svcSetThreadPriority(CUR_THREAD_HANDLE, u32(own));
  return r;
}

void NxvLine(Decoder& dc) {
  NxvStats st;
  nxv_stats(dc.d, &st);
  if (!st.frames) return;
  const double n = double(st.frames);
  REXLOG_INFO("[hevc] {}: nxvideo a picture ({}): send {:.2f} ms, receive {:.2f} ms, transfer {:.2f} ms; NVDEC {} MHz "
              "(DFS's start {} MHz, put back {} times), VIC {} MHz; copied out by the VIC {}, by the CPU {}",
              dc.movie->name, st.frames, st.send_ms / n, st.receive_ms / n, st.transfer_ms / n, st.nvdec_hz / 1000000,
              st.nvdec_max_hz / 1000000, st.restores, st.vic_hz / 1000000, st.vic_frames, st.cpu_frames);
}

std::mutex g_mutex;
std::map<uint32_t, Decoder> g_decoders;

void CloseIdle() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = g_decoders.begin(); it != g_decoders.end();) {
    if (now - it->second.used > std::chrono::seconds(15)) {
      if (it->second.d) nxv_close(it->second.d);
      it = g_decoders.erase(it);
    } else {
      ++it;
    }
  }
}

// Frame `index` of the movie into `pic`: decoded in order from the last one, or from the keyframe at or before it.
bool DecodeTo(Decoder& dc, uint32_t index, NxvPicture* pic) {
  const auto& frames = dc.movie->frames;
  if (index >= frames.size()) return false;
  if (!dc.d) {
    char why[256] = {};
    static const bool hw = REXCVAR_GET(sa2_hevc_nvdec);
    dc.d = nxv_open(NXV_HEVC, nullptr, 0, int(dc.movie->width), int(dc.movie->height), hw ? 1 : 0, why, sizeof(why));
    REXLOG_INFO("[hevc] {}: {}", dc.movie->name, dc.d ? why : "the H.265 decoder did not open");
    if (!dc.d) return false;
    dc.next = 0;
  }
  if (int64_t(index) < dc.next || int64_t(index) > dc.next + 120) {
    int64_t k = index;
    while (k > 0 && !frames[size_t(k)].keyframe) --k;
    if (!(int64_t(index) >= dc.next && k <= dc.next)) {
      nxv_flush(dc.d);
      dc.next = k;
    }
  }
  bool got = false;
  while (dc.next <= int64_t(index)) {
    const uint32_t f = uint32_t(dc.next++);
    const auto t0 = std::chrono::steady_clock::now();
    if (!(dc.ahead ? TakeFrame(*dc.ahead, f, &dc.bytes) : dc.movie->ReadFrame(f, &dc.bytes))) return false;
    dc.read_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const int r = DecodeRaised(dc, pic);
    got = r == 1 && f == index;
    if (r < 0) {
      REXLOG_WARN("[hevc] {}: frame {} did not decode", dc.movie->name, f);
      return false;
    }
  }
  return got;
}

void WritePicture(uint8_t* base, const NxvPicture& pic, const Planes& pl, uint32_t w, uint32_t h) {
  const uint32_t fw = std::min<uint32_t>(w, uint32_t(pic.width)), fh = std::min<uint32_t>(h, uint32_t(pic.height));
  for (uint32_t y = 0; y < fh; ++y)
    std::memcpy(Host(base, pl.y + y * pl.y_pitch), pic.data[0] + size_t(y) * size_t(pic.linesize[0]), fw);
  const uint32_t cw = fw / 2, ch = fh / 2;
  for (uint32_t y = 0; y < ch; ++y) {
    uint8_t* u = Host(base, pl.cb + y * pl.c_pitch);
    uint8_t* v = Host(base, pl.cr + y * pl.c_pitch);
    if (pic.nv12) {
      const uint8_t* s = pic.data[1] + size_t(y) * size_t(pic.linesize[1]);
      for (uint32_t x = 0; x < cw; ++x) {
        u[x] = s[2 * x];
        v[x] = s[2 * x + 1];
      }
    } else {
      std::memcpy(u, pic.data[1] + size_t(y) * size_t(pic.linesize[1]), cw);
      std::memcpy(v, pic.data[2] + size_t(y) * size_t(pic.linesize[2]), cw);
    }
  }
  rex::memory::MarkStoreCardsAt(base, pl.y, pl.y_pitch * ((h + 15) & ~15u));
  rex::memory::MarkStoreCardsAt(base, pl.cb, pl.c_pitch * ((h + 15) & ~15u));
}
#endif

std::atomic<int> g_diag_i{0}, g_diag_p{0}, g_diag_b{0}, g_diag_frames{0};

}  // namespace

// (1) The slice decoders: a tagged slice decodes no macroblocks and ends the picture.
REX_HOOK_RAW(sub_8296BA00) {
  if (!TaggedSlice(ctx, base, "I", g_diag_i)) __imp__sub_8296BA00(ctx, base);
}
REX_HOOK_RAW(sub_8296C550) {
  if (!TaggedSlice(ctx, base, "P", g_diag_p)) __imp__sub_8296C550(ctx, base);
}
REX_HOOK_RAW(sub_8296BF68) {
  if (!TaggedSlice(ctx, base, "B", g_diag_b)) __imp__sub_8296BF68(ctx, base);
}

// (2) The frame to show: a tagged one gets its H.265 picture.
REX_HOOK_RAW(sub_828E06F8) {
  const uint32_t frm = ctx.r4.u32;
  __imp__sub_828E06F8(ctx, base);
  static const bool on = REXCVAR_GET(sa2_hevc_movies);
  static const bool diag = REXCVAR_GET(sa2_hevc_diag);
  if (!frm) return;
  const uint32_t buf = Ld32(base, frm + 0);
  if (!buf) return;
  if (diag && g_diag_frames.fetch_add(1, std::memory_order_relaxed) < 4) {
    const uint8_t* q = Host(base, buf);
    REXLOG_INFO("[hevc] mwPlyGetCurFrm: buffer {:08X} {}x{}, first bytes {:02X} {:02X} {:02X} {:02X} {:02X}; {} slices "
                "skipped so far",
                buf, Ld32(base, frm + 12), Ld32(base, frm + 16), q[0], q[1], q[2], q[3], q[4],
                g_tagged_slices.load(std::memory_order_relaxed));
  }
  if (!on) return;
  uint32_t id, index;
  if (!rex::filesystem::ParseSfdHevcTag(Host(base, buf), &id, &index)) {
    // A frame without a tag while slices of an H.265 movie are being skipped (the last second): a picture whose tag
    // went elsewhere (the B pictures' planes, 2026-11-21). Counted and named in the log, a few times.
    static std::atomic<uint64_t> last_tagged{0}, untagged{0};
    const uint64_t now = g_tagged_slices.load(std::memory_order_relaxed);
    if (now && now != last_tagged.exchange(now, std::memory_order_relaxed)) {
      const uint64_t n = untagged.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 4 || n % 1000 == 0)
        REXLOG_WARN("[hevc] a frame without a tag while an H.265 movie plays ({} so far): buffer {:08X} {}x{}", n, buf,
                    Ld32(base, frm + 12), Ld32(base, frm + 16));
    }
    return;
  }
  const uint32_t w = Ld32(base, frm + 12), h = Ld32(base, frm + 16);
  if (!w || !h || w > 4096 || h > 4096) return;
  const Planes pl = SofdecPlanes(buf, w, h);
#if RENUT_NXVIDEO
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_decoders.find(id);
  if (it == g_decoders.end()) {
    CloseIdle();
    auto movie = rex::filesystem::FindSfdHevcMovie(id);
    if (!movie) {
      if (g_missing.fetch_add(1, std::memory_order_relaxed) < 4)
        REXLOG_WARN("[hevc] a tagged picture of movie id {} that is not open", id);
      return;
    }
    it = g_decoders.emplace(id, Decoder{}).first;
    it->second.ahead = StartAhead(movie);
    it->second.movie = std::move(movie);
  }
  Decoder& dc = it->second;
  dc.used = std::chrono::steady_clock::now();
  if (index == kIndexUnknown) {
    index = uint32_t(dc.next);
    if (index >= dc.movie->frames.size()) return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  NxvPicture pic{};
  if (!DecodeTo(dc, index, &pic)) return;
  const auto t1 = std::chrono::steady_clock::now();
  WritePicture(base, pic, pl, w, h);
  const auto t2 = std::chrono::steady_clock::now();
  dc.decode_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
  dc.copy_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
  ++dc.frames;
  const uint64_t shown = g_shown.fetch_add(1, std::memory_order_relaxed) + 1;
  static const bool log = REXCVAR_GET(sa2_hevc_log);
  if (shown == 1 || dc.frames == 1 || (log && dc.frames % 300 == 0)) {
    uint64_t hits = 0, misses = 0;
    if (dc.ahead) {
      std::lock_guard<std::mutex> pl2(dc.ahead->m);
      hits = dc.ahead->hits;
      misses = dc.ahead->misses;
    }
    REXLOG_INFO("[hevc] {}: picture {} of {} shown from H.265 ({}x{}); {} pictures, {:.2f} ms decoding ({:.2f} taking "
                "the frames: {} from the read-ahead, {} read here; {:.2f} in nxv_decode) and {:.2f} ms copying a "
                "picture; {} slices skipped in all ({} tags cut short)",
                dc.movie->name, index, dc.movie->frames.size(), w, h, dc.frames, dc.decode_ms / double(dc.frames),
                dc.read_ms / double(dc.frames), hits, misses, dc.nxv_ms / double(dc.frames),
                dc.copy_ms / double(dc.frames), g_tagged_slices.load(std::memory_order_relaxed),
                g_cut_tags.load(std::memory_order_relaxed));
    if (log) NxvLine(dc);
  }
#else
  // No H.265 decoder in this build (nxvideo is the Switch's): the picture is grey, so a PC test sees which frames
  // were tagged (the tag itself is cleared with the rest of the Y plane).
  for (uint32_t y = 0; y < h; ++y) std::memset(Host(base, pl.y + y * pl.y_pitch), 0x80, w);
  for (uint32_t y = 0; y < h / 2; ++y) {
    std::memset(Host(base, pl.cb + y * pl.c_pitch), 0x80, w / 2);
    std::memset(Host(base, pl.cr + y * pl.c_pitch), 0x80, w / 2);
  }
  rex::memory::MarkStoreCardsAt(base, pl.y, pl.y_pitch * ((h + 15) & ~15u));
  rex::memory::MarkStoreCardsAt(base, pl.cb, pl.c_pitch * ((h + 15) & ~15u));
  if (g_missing.fetch_add(1, std::memory_order_relaxed) == 0)
    REXLOG_WARN("[hevc] movie id {} ({}x{}): an H.265 movie, and this build has no H.265 decoder (the Switch's "
                "nxvideo): its pictures are grey",
                id, w, h);
  (void)index;
#endif
}
