// sa2-nx-ult (2026-11-22, the user: "Yeah lets do 2", i.e. play the interviews ourselves, with the port's own decoders
// in place of XMedia2): the two interviews of HELP & OPTIONS -> BONUS VIDEO (gd/interview.wmv, gd/interview_jp.wmv;
// the item shows after 5 hours of play, the save's u32 at 0x2870) played natively.
//
// Why: SA2 plays them through Microsoft's XMedia2, which takes its WMV and WMA decoders from the 360's "optional media
// update" (CodecPack02): the port has no such pack. Before this, choosing the item stopped the port at once: XMedia2's
// setup (sub_8256F830) took the SDK's bare stub XamGetDashBackstackData's r3 (its own argument, a guest address, so a
// negative HRESULT) as a failure, kept it in a global (0x82989138), the player's creation (sub_82571408) then wrote a
// null player and returned that failure, and SA2 (sub_82120D68, which never checks) called the null player's method 11
// (the [census] line of 2026-11-22: "indirect call to 00000000 from sub_82120D68 (site 00000002)"). 1.0.6 has the same
// stub (not run there).
//
// How SA2 drives the player (the interview code, 0x82120D68-0x82121A00; the singleton at 0x83A5DCCC, sub_82121348):
//   sub_82121768 (the menu item): picks the file by the voice language, then sub_82120D68(obj, "interview.wmv"):
//     sub_82570020 (XMedia2's setup), sub_82571408(&obj->player, callback, 0) (the player), player->m11(&obj->a),
//     player->m15(&obj->b), player->m4("gd/interview.wmv", -1) (open), player->m5() (play).
//   sub_82121858 (each frame, state 2): a->m3(); a->m17() != 0: the end; the skip button: player->m7() (stop); else
//     sub_82121560 -> sub_82121150: player->m12() (5, 6 or 7: a picture), b->m3(0, &obj->texture, &info) (the frame
//     as a D3D texture; the last frame's reference released first), the texture's size, and SA2's own 2D sprite draw
//     (sub_8213C920; the subtitles after it, sub_823D3DD8).
//   sub_82121050 (the end): b, a, player and the callback released (m2), sub_8256E938 (XMedia2's shutdown).
//   sub_82153108 (each frame while a player exists): sub_8256EA70 / sub_8256E9E8 (XMedia2's frame hooks).
//
// What this file does: those five XMedia2 functions hooked (setup, creation, shutdown and the frame hooks: nothing,
// S_OK, and a player of our own); the player and its objects a and b in guest memory, their methods guest functions
// of XMedia2's that nothing calls any more (the slots below; each hook takes the call only when r3 is one of our
// objects, so with sa2_interviews_native off XMedia2's code is as it was). The file is read through the VFS (so the
// installer's NAME.wmv.lz4b is read as NAME.wmv), its ASF demuxed here, the video decoded by nxvideo (H.265, the
// installer's re-encode; the Switch's NVDEC, or FFmpeg's software decoder) or by the SDK's FFmpeg (the original WMV3),
// converted to ARGB (BT.709) at 1280x720 and copied into one of three linear textures made by the renderer's native
// D3DDevice_CreateTexture (kept for the session: SA2 keeps a stale reference to the last one); the WMA Pro sound decoded
// by the SDK's FFmpeg and played through an audio render client of its own, whose consumption is the video's clock.
#include <fmt/format.h>
#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}

#if SA2_NXVIDEO
#include <switch.h>

#include "nxvideo.h"
#endif

REXCVAR_DEFINE_BOOL(sa2_interviews_native, true, "SA2",
                    "BONUS VIDEO's two interviews played by the port (its ASF reader, H.265 or WMV3 and WMA Pro "
                    "decoders) in place of XMedia2, which needs the 360's codec pack; false: XMedia2's own code (the "
                    "port stops: a null player)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_interviews_log, true, "SA2",
                    "[interview] log lines: the file, its streams, the decoders, and a line every 10 s of playing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(sa2_interviews_volume, 1.0, "SA2", "The interviews' sound volume (1 = as recorded)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, sa2_hevc_nvdec);  // src/sa2_hevc_movies.cpp: H.265 on NVDEC (false in an emulator)

// The renderer's native texture creation (renut_engine/gpu/d3d/hooks/resource.cpp).
namespace renut::gpu::native {
uint32_t MakeTextureForHost(uint32_t header, uint32_t width, uint32_t height, uint32_t format);
uint32_t AddRefResource(uint32_t resource);
}  // namespace renut::gpu::native

// XMedia2's functions SA2 calls directly.
REX_EXTERN(__imp__sub_82570020);  // setup (params, out)
REX_EXTERN(__imp__sub_82571408);  // the player's creation (&player, callback, 0)
REX_EXTERN(__imp__sub_8256E938);  // shutdown
REX_EXTERN(__imp__sub_8256EA70);  // before SA2's frame end
REX_EXTERN(__imp__sub_8256E9E8);  // after it
REX_EXTERN(__imp__sub_82577AE8);  // under sub_8256EA70
REX_EXTERN(__imp__sub_82577AA8);  // under sub_8256E9E8
// XMedia2's functions that serve as our objects' methods (none runs once XMedia2's setup is not called).
REX_EXTERN(__imp__sub_8256F830);
REX_EXTERN(__imp__sub_82574CA8);
REX_EXTERN(__imp__sub_825750B8);
REX_EXTERN(__imp__sub_82577A68);
REX_EXTERN(__imp__sub_82579B18);
REX_EXTERN(__imp__sub_8257B7D8);
REX_EXTERN(__imp__sub_825702A8);
REX_EXTERN(__imp__sub_825701B8);
REX_EXTERN(__imp__sub_8257B888);
REX_EXTERN(__imp__sub_825D6218);
REX_EXTERN(__imp__sub_825C2D70);
REX_EXTERN(__imp__sub_825C2F00);
REX_EXTERN(__imp__sub_825C2BD8);

namespace {

constexpr uint32_t kSlotRelease = 0x8256F830, kSlotAddRef = 0x82574CA8, kSlotOpen = 0x825750B8,
                   kSlotPlay = 0x82577A68, kSlotStop = 0x82579B18, kSlotGetA = 0x8257B7D8, kSlotStatus = 0x825702A8,
                   kSlotGetB = 0x825701B8, kSlotTick = 0x8257B888, kSlotDone = 0x825D6218,
                   kSlotTexture = 0x825C2D70, kSlotTrap = 0x825C2F00, kSlotAudio = 0x825C2BD8;

constexpr uint32_t kOut = 1280, kOutH = 720;            // the textures' size
constexpr uint32_t kFormat = 0x18280086;                 // D3DFMT_LIN_A8R8G8B8
constexpr int kTextures = 3, kDecodedMax = 3;
constexpr double kAudioLatencyMs = 60.0;
constexpr uint32_t kSOk = 0, kEFail = 0x80004005, kENotImpl = 0x80004001;

uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(rex::memory::GuestPtr(base, a), &v, 4);
}
uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t Rd64(const uint8_t* p) { return uint64_t(Rd32(p)) | (uint64_t(Rd32(p + 4)) << 32); }

// ---------------------------------------------------------------------------------------------------------------------
// ASF (the interviews: 16,415-byte packets, several payloads a packet, WMA Pro stream 1 and WMV3 / H.265 stream 2)

const uint8_t kGuidHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
const uint8_t kGuidData[16] = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
const uint8_t kGuidFileProps[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
const uint8_t kGuidStreamProps[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
const uint8_t kGuidAudio[16] = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
const uint8_t kGuidVideo[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};

struct AsfStream {
  int number = -1;
  uint32_t width = 0, height = 0;                     // video
  uint16_t tag = 0, channels = 0, align = 0, bits = 0;  // audio (WAVEFORMATEX)
  uint32_t rate = 0, avg_bytes = 0;
  std::vector<uint8_t> extra;  // video: BITMAPINFOHEADER's tail (WMV3's sequence header); audio: WAVEFORMATEX's
};

struct AsfInfo {
  uint32_t packet_size = 0;
  uint64_t preroll_ms = 0, packets = 0, data_offset = 0;
  AsfStream video, audio;
};

struct Partial {  // a media object being put together
  bool active = false;
  uint32_t number = 0, size = 0, pts = 0;
  bool key = false;
  std::vector<uint8_t> data;
};

struct Object {
  int stream = 0;
  uint32_t pts = 0;  // ms, preroll included
  bool key = false;
  std::vector<uint8_t> data;
};

// ---------------------------------------------------------------------------------------------------------------------
// The player

struct Frame {
  int64_t pts = 0;  // ms, preroll removed
  std::vector<uint8_t> argb;
};

struct Player {
  // guest objects
  uint32_t block = 0, p = 0, a = 0, b = 0, cookie = 0, vp = 0, va = 0, vb = 0, audio_buf = 0;
  uint32_t textures[kTextures] = {}, texture_data[kTextures] = {};
  int texture_next = 0;
  uint32_t texture_shown = 0;  // the texture holding the shown frame
  int64_t shown_pts = INT64_MIN;
  uint32_t frames_shown = 0;

  // the worker
  std::mutex mutex;
  std::condition_variable cv;
  std::thread worker;
  std::atomic<bool> quit{false}, eof{false}, failed{false}, stopped{false}, playing{false}, running{false};
  std::string path;
  AsfInfo info;
  std::deque<Object> video_in;       // compressed, in decode order
  std::deque<Frame> decoded;         // in display order
  std::vector<std::vector<uint8_t>> pool;

  // the sound: interleaved stereo at the stream's rate
  std::vector<float> ring;
  size_t ring_read = 0, ring_count = 0;  // in sample frames, under `mutex`
  std::atomic<uint64_t> samples_played{0};
  int64_t audio_pts0 = -1;  // under `mutex`
  std::atomic<bool> has_audio{false};
  rex::audio::AudioSystem* audio_system = nullptr;
  size_t audio_index = 0;
  bool audio_registered = false;
  uint32_t audio_cell = 0;  // the guest cell the audio worker passes to our callback
  uint32_t audio_flip = 0;

  // the clock
  std::chrono::steady_clock::time_point last_poll{}, clock_wall{};
  double clock_ms = 0;
  bool clock_started = false;
  std::atomic<int64_t> clock_now{0};        // the clock for the worker (ms)
  std::atomic<int64_t> newest_decoded{-1};  // the newest picture decoded (ms)

  // counts for the log
  uint64_t video_objects = 0, audio_objects = 0, video_decoded = 0, video_failed = 0, audio_frames = 0;
  std::chrono::steady_clock::time_point last_log{};
  std::string video_kind;
};

Player g_player;
std::mutex g_setup_mutex;

uint8_t* g_base = nullptr;  // the guest memory base, as the hooks get it

bool Ours(uint32_t object) {
  const Player& pl = g_player;
  return pl.block && (object == pl.p || object == pl.a || object == pl.b);
}

void SetupGuestObjects(uint8_t* base) {
  Player& pl = g_player;
  if (pl.block)
    return;
  pl.block = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x4000, 0x100);
  if (!pl.block) {
    REXLOG_ERROR("[interview] no guest memory for the player's objects");
    return;
  }
  std::memset(rex::memory::GuestPtr(base, pl.block), 0, 0x4000);
  pl.p = pl.block + 0x00;
  pl.a = pl.block + 0x10;
  pl.b = pl.block + 0x20;
  pl.cookie = pl.block + 0x30;
  pl.vp = pl.block + 0x40;   // 24 entries
  pl.va = pl.block + 0xA0;   // 24 entries
  pl.vb = pl.block + 0x100;  // 8 entries
  pl.audio_buf = pl.block + 0x200;  // two frames of 6 x 256 floats
  for (int i = 0; i < 24; ++i) {
    St32(base, pl.vp + 4 * i, kSlotTrap);
    St32(base, pl.va + 4 * i, kSlotTrap);
  }
  for (int i = 0; i < 8; ++i)
    St32(base, pl.vb + 4 * i, kSlotTrap);
  for (uint32_t vt : {pl.vp, pl.va, pl.vb}) {
    St32(base, vt + 4, kSlotAddRef);
    St32(base, vt + 8, kSlotRelease);
  }
  St32(base, pl.vp + 4 * 4, kSlotOpen);
  St32(base, pl.vp + 4 * 5, kSlotPlay);
  St32(base, pl.vp + 4 * 7, kSlotStop);
  St32(base, pl.vp + 4 * 11, kSlotGetA);
  St32(base, pl.vp + 4 * 12, kSlotStatus);
  St32(base, pl.vp + 4 * 15, kSlotGetB);
  St32(base, pl.va + 4 * 3, kSlotTick);
  St32(base, pl.va + 4 * 17, kSlotDone);
  St32(base, pl.vb + 4 * 3, kSlotTexture);
  St32(base, pl.p, pl.vp);
  St32(base, pl.a, pl.va);
  St32(base, pl.b, pl.vb);
}

bool MakeTextures(uint8_t* base) {
  Player& pl = g_player;
  if (pl.textures[0])
    return true;
  for (int i = 0; i < kTextures; ++i) {
    const uint32_t t = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x40, 0x10);
    if (t)
      std::memset(rex::memory::GuestPtr(base, t), 0, 0x40);
    const uint32_t data_va = t ? renut::gpu::native::MakeTextureForHost(t, kOut, kOutH, kFormat) : 0;
    if (!data_va) {
      REXLOG_ERROR("[interview] the renderer made no {}x{} texture (header {:08X})", kOut, kOutH, t);
      return false;
    }
    pl.textures[i] = t;
    pl.texture_data[i] = data_va;
    uint8_t* data = rex::memory::GuestPtr(base, pl.texture_data[i]);
    for (uint32_t k = 0; k < kOut * kOutH; ++k) {
      data[4 * k] = 0xFF;
      data[4 * k + 1] = data[4 * k + 2] = data[4 * k + 3] = 0;
    }
    rex::memory::MarkStoreCardsAt(base, pl.texture_data[i], kOut * kOutH * 4);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The file

struct Reader {
  rex::filesystem::File* file = nullptr;
  ~Reader() {
    if (file)
      file->Destroy();
  }
  bool Read(uint64_t offset, uint8_t* out, size_t size) {
    size_t got = 0;
    return file && file->ReadSync(std::span<uint8_t>(out, size), size_t(offset), &got) == 0 && got == size;
  }
};

bool OpenFile(Reader& r, const std::string& guest_path, std::string& why) {
  // SA2 passes the whole path ("game:\gd\interview.wmv"); a relative one goes under game:\.
  std::string path = guest_path.find(':') != std::string::npos ? guest_path : "game:\\" + guest_path;
  std::replace(path.begin(), path.end(), '/', '\\');
  rex::filesystem::FileAction action;
  const auto status = REX_KERNEL_FS()->OpenFile(nullptr, path, rex::filesystem::FileDisposition::kOpen,
                                                rex::filesystem::FileAccess::kGenericRead, false, true, &r.file, &action);
  if (status != 0 || !r.file) {
    r.file = nullptr;
    why = fmt::format("{} did not open ({:08X})", path, uint32_t(status));
    return false;
  }
  return true;
}

bool ParseHeader(Reader& r, AsfInfo& info, std::string& why) {
  uint8_t head[30];
  if (!r.Read(0, head, 30) || std::memcmp(head, kGuidHeader, 16) != 0) {
    why = "not an ASF file";
    return false;
  }
  const uint64_t size = Rd64(head + 16);
  const uint32_t count = Rd32(head + 24);
  if (size < 30 || size > (4u << 20)) {
    why = fmt::format("an ASF header of {} bytes", size);
    return false;
  }
  std::vector<uint8_t> h(static_cast<size_t>(size));
  if (!r.Read(0, h.data(), h.size())) {
    why = "the ASF header was not read";
    return false;
  }
  size_t off = 30;
  for (uint32_t i = 0; i < count && off + 24 <= h.size(); ++i) {
    const uint8_t* o = h.data() + off;
    const uint64_t osize = Rd64(o + 16);
    if (osize < 24 || off + osize > h.size())
      break;
    const uint8_t* body = o + 24;
    if (std::memcmp(o, kGuidFileProps, 16) == 0 && osize >= 24 + 80) {
      info.packets = Rd64(body + 32);
      info.preroll_ms = Rd64(body + 56);
      info.packet_size = Rd32(body + 68);
    } else if (std::memcmp(o, kGuidStreamProps, 16) == 0 && osize >= 24 + 54) {
      const uint32_t tlen = uint32_t(std::min<uint64_t>(Rd32(body + 40), osize - 24 - 54));
      const int number = Rd16(body + 48) & 0x7F;
      const uint8_t* ts = body + 54;
      if (std::memcmp(body, kGuidAudio, 16) == 0 && tlen >= 18) {
        AsfStream& s = info.audio;
        s.number = number;
        s.tag = Rd16(ts);
        s.channels = Rd16(ts + 2);
        s.rate = Rd32(ts + 4);
        s.avg_bytes = Rd32(ts + 8);
        s.align = Rd16(ts + 12);
        s.bits = Rd16(ts + 14);
        const uint16_t cb = std::min<uint32_t>(Rd16(ts + 16), tlen - 18);
        s.extra.assign(ts + 18, ts + 18 + cb);
      } else if (std::memcmp(body, kGuidVideo, 16) == 0 && tlen >= 11 + 40) {
        AsfStream& s = info.video;
        s.number = number;
        s.width = Rd32(ts);
        s.height = Rd32(ts + 4);
        const uint8_t* bih = ts + 11;
        const uint32_t bsize = std::min<uint32_t>(Rd32(bih), tlen - 11);
        if (bsize > 40)
          s.extra.assign(bih + 40, bih + bsize);
      }
    }
    off += size_t(osize);
  }
  uint8_t data[50];
  if (!r.Read(size, data, 50) || std::memcmp(data, kGuidData, 16) != 0) {
    why = "no data object after the header";
    return false;
  }
  info.data_offset = size + 50;
  if (!info.packets)
    info.packets = Rd64(data + 40);
  if (!info.packet_size || info.video.number < 0) {
    why = fmt::format("packet size {}, video stream {}", info.packet_size, info.video.number);
    return false;
  }
  return true;
}

// One packet's payloads put into media objects (complete ones appended to `out`).
void ParsePacket(const uint8_t* pkt, uint32_t size, Partial parts[128], std::vector<Object>& out) {
  uint32_t q = 0;
  auto need = [&](uint32_t n) { return q + n <= size; };
  auto field = [&](int kind) -> uint32_t {
    uint32_t v = 0;
    if (kind == 1 && need(1)) v = pkt[q], q += 1;
    else if (kind == 2 && need(2)) v = Rd16(pkt + q), q += 2;
    else if (kind == 3 && need(4)) v = Rd32(pkt + q), q += 4;
    return v;
  };
  if (!need(1))
    return;
  if (pkt[0] & 0x80)
    q = 1 + (pkt[0] & 0x0F);
  if (!need(2))
    return;
  const uint8_t lt = pkt[q], pf = pkt[q + 1];
  q += 2;
  const uint32_t packet_length = field((lt >> 5) & 3);
  field((lt >> 1) & 3);  // sequence
  const uint32_t padding = field((lt >> 3) & 3);
  q += 6;  // send time, duration
  const uint32_t end = packet_length && packet_length <= size ? packet_length : size;
  int n = 1, length_type = 2;
  const bool multiple = lt & 1;
  if (multiple) {
    if (!need(1))
      return;
    n = pkt[q] & 0x3F;
    length_type = (pkt[q] >> 6) & 3;
    q += 1;
  }
  for (int i = 0; i < n && q < end; ++i) {
    if (!need(1))
      return;
    const uint8_t sn = pkt[q];
    q += 1;
    const int stream = sn & 0x7F;
    const bool key = sn & 0x80;
    const uint32_t number = field((pf >> 4) & 3);
    const uint32_t offset = field((pf >> 2) & 3);
    const uint32_t rlen = field(pf & 3);
    if (!need(rlen))
      return;
    const uint8_t* rep = pkt + q;
    q += rlen;
    uint32_t len;
    if (multiple)
      len = field(length_type);
    else
      len = end > q + padding ? end - q - padding : 0;
    if (!need(len))
      return;
    const uint8_t* data = pkt + q;
    q += len;
    if (rlen == 1) {  // compressed payload: whole objects of a byte's length each, at the time `offset`
      for (uint32_t k = 0; k < len;) {
        const uint32_t sub = data[k++];
        if (k + sub > len)
          break;
        Object o;
        o.stream = stream;
        o.pts = offset;
        o.key = key;
        o.data.assign(data + k, data + k + sub);
        out.push_back(std::move(o));
        k += sub;
      }
      continue;
    }
    Partial& p = parts[stream];
    if (offset == 0) {
      p.active = rlen >= 8;
      p.number = number;
      p.size = rlen >= 8 ? Rd32(rep) : 0;
      p.pts = rlen >= 8 ? Rd32(rep + 4) : 0;
      p.key = key;
      p.data.clear();
      p.data.reserve(p.size);
    }
    if (!p.active || p.number != number || offset != p.data.size()) {
      p.active = false;
      continue;
    }
    p.data.insert(p.data.end(), data, data + len);
    if (p.data.size() >= p.size) {
      Object o;
      o.stream = stream;
      o.pts = p.pts;
      o.key = p.key;
      o.data = std::move(p.data);
      o.data.resize(p.size);
      out.push_back(std::move(o));
      p.active = false;
      p.data = {};
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Pictures

// An H.265 access unit in Annex B (culdcept-nx's LooksHevc): a start code, then a VPS, SPS, PPS or access unit delimiter.
bool LooksHevc(const std::vector<uint8_t>& b) {
  size_t i = 0;
  if (b.size() >= 6 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1)
    i = 4;
  else if (b.size() >= 5 && b[0] == 0 && b[1] == 0 && b[2] == 1)
    i = 3;
  else
    return false;
  const uint8_t type = (b[i] >> 1) & 0x3F;
  return (b[i] & 0x80) == 0 && type >= 32 && type <= 35 && b[i + 1] == 1;
}

// Y'CbCr 4:2:0 (planar, or NV12 when v is null) to ARGB bytes at kOut x kOutH (BT.709, limited range; the nearest
// source sample).
void ToArgb(const uint8_t* y, int ys, const uint8_t* u, int us, const uint8_t* v, int vs, int w, int h, uint8_t* out) {
  static std::vector<int> xs, cxs;
  static int last_w = 0;
  if (last_w != w) {
    xs.resize(kOut);
    cxs.resize(kOut);
    for (uint32_t x = 0; x < kOut; ++x) {
      xs[x] = int(uint64_t(x) * uint64_t(w) / kOut);
      cxs[x] = xs[x] / 2;
    }
    last_w = w;
  }
  for (uint32_t oy = 0; oy < kOutH; ++oy) {
    const int sy = int(uint64_t(oy) * uint64_t(h) / kOutH);
    const uint8_t* yr = y + size_t(sy) * ys;
    const uint8_t* ur = u + size_t(sy / 2) * us;
    const uint8_t* vr = v ? v + size_t(sy / 2) * vs : nullptr;
    uint8_t* o = out + size_t(oy) * kOut * 4;
    for (uint32_t ox = 0; ox < kOut; ++ox) {
      const int c = 298 * (int(yr[xs[ox]]) - 16);
      int cb, cr;
      if (vr) {
        cb = int(ur[cxs[ox]]) - 128;
        cr = int(vr[cxs[ox]]) - 128;
      } else {
        cb = int(ur[2 * cxs[ox]]) - 128;
        cr = int(ur[2 * cxs[ox] + 1]) - 128;
      }
      const int r = (c + 459 * cr + 128) >> 8;
      const int g = (c - 55 * cb - 136 * cr + 128) >> 8;
      const int b = (c + 541 * cb + 128) >> 8;
      o[0] = 0xFF;
      o[1] = uint8_t(std::clamp(r, 0, 255));
      o[2] = uint8_t(std::clamp(g, 0, 255));
      o[3] = uint8_t(std::clamp(b, 0, 255));
      o += 4;
    }
  }
}

struct VideoDecoder {
  bool hevc = false, opened = false, broken = false, nxv_used = false;
  int skip = 0;  // FFmpeg's frame skipping while the pictures are behind the sound (0 none, 1 non-reference, 2 non-key)
  AVCodecContext* av = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  std::deque<int64_t> pending;  // pts of frames sent and not out yet (sorted on the way out)
#if SA2_NXVIDEO
  NxvDecoder* nxv = nullptr;
  NxvPicture pic{};
#endif
  ~VideoDecoder() {
#if SA2_NXVIDEO
    if (nxv)
      nxv_close(nxv);
#endif
    if (frame)
      av_frame_free(&frame);
    if (packet)
      av_packet_free(&packet);
    if (av)
      avcodec_free_context(&av);
  }
};

struct AudioDecoder {
  AVCodecContext* av = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  ~AudioDecoder() {
    if (frame)
      av_frame_free(&frame);
    if (packet)
      av_packet_free(&packet);
    if (av)
      avcodec_free_context(&av);
  }
};

std::vector<uint8_t> TakeBuffer(Player& pl) {
  std::lock_guard lock(pl.mutex);
  if (!pl.pool.empty()) {
    std::vector<uint8_t> b = std::move(pl.pool.back());
    pl.pool.pop_back();
    return b;
  }
  return std::vector<uint8_t>(size_t(kOut) * kOutH * 4);
}

void PushFrame(Player& pl, int64_t pts, std::vector<uint8_t>&& argb) {
  std::lock_guard lock(pl.mutex);
  Frame f;
  f.pts = pts;
  f.argb = std::move(argb);
  auto at = std::find_if(pl.decoded.begin(), pl.decoded.end(), [&](const Frame& g) { return g.pts > pts; });
  pl.decoded.insert(at, std::move(f));
  ++pl.video_decoded;
  pl.newest_decoded = std::max<int64_t>(pl.newest_decoded.load(), pts);
}

void OpenVideo(Player& pl, VideoDecoder& vd, const Object& first) {
  vd.opened = true;
  vd.hevc = LooksHevc(first.data);
  const AsfStream& s = pl.info.video;
  if (vd.hevc) {
#if SA2_NXVIDEO
    char why[160] = {};
    vd.nxv = nxv_open(NXV_HEVC, nullptr, 0, int(s.width), int(s.height), REXCVAR_GET(sa2_hevc_nvdec) ? 1 : 0, why,
                      sizeof(why));
    pl.video_kind = fmt::format("H.265 on nxvideo ({})", vd.nxv ? why : "did not open");
    vd.broken = !vd.nxv;
#else
    pl.video_kind = "H.265, and this build has no H.265 decoder (nxvideo is the Switch's): grey pictures";
#endif
    return;
  }
#if SA2_NXVIDEO
  // WMV3 on the Switch: nxvideo too (NVDEC decodes VC-1; culdcept-nx's culdcept_movie_nvdec 2), its software decoder
  // with sa2_hevc_nvdec off (an emulator)
  {
    char why[160] = {};
    vd.nxv = nxv_open(NXV_WMV3, s.extra.empty() ? nullptr : s.extra.data(), int(s.extra.size()), int(s.width),
                      int(s.height), REXCVAR_GET(sa2_hevc_nvdec) ? 1 : 0, why, sizeof(why));
    if (vd.nxv) {
      vd.nxv_used = true;
      pl.video_kind = fmt::format("WMV3 on nxvideo ({})", why);
      return;
    }
  }
#endif
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMV3);
  vd.av = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!vd.av) {
    vd.broken = true;
    pl.video_kind = "WMV3, and no WMV3 decoder";
    return;
  }
  vd.av->width = int(s.width);
  vd.av->height = int(s.height);
  if (!s.extra.empty()) {
    vd.av->extradata = static_cast<uint8_t*>(av_mallocz(s.extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(vd.av->extradata, s.extra.data(), s.extra.size());
    vd.av->extradata_size = int(s.extra.size());
  }
  vd.av->thread_count = 1;
  if (avcodec_open2(vd.av, codec, nullptr) < 0) {
    vd.broken = true;
    pl.video_kind = "WMV3, and FFmpeg's decoder did not open";
    return;
  }
  vd.frame = av_frame_alloc();
  vd.packet = av_packet_alloc();
  pl.video_kind = fmt::format("WMV3 on the SDK's FFmpeg {}x{} (sequence header {} bytes)", s.width, s.height,
                              s.extra.size());
}

int64_t TakePts(VideoDecoder& vd, int64_t fallback) {
  if (vd.pending.empty())
    return fallback;
  auto it = std::min_element(vd.pending.begin(), vd.pending.end());
  const int64_t pts = *it;
  vd.pending.erase(it);
  return pts;
}

void DecodeVideo(Player& pl, VideoDecoder& vd, const Object* o) {  // o null: drain
  if (!vd.opened) {
    if (!o)
      return;
    OpenVideo(pl, vd, *o);
    if (REXCVAR_GET(sa2_interviews_log))
      REXLOG_INFO("[interview] video: {}", pl.video_kind);
  }
  const int64_t pts = o ? int64_t(o->pts) - int64_t(pl.info.preroll_ms) : 0;
  if (vd.hevc || vd.nxv_used) {
    if (!o)
      return;
#if SA2_NXVIDEO
    if (vd.broken || !vd.nxv)
      return;
    vd.pending.push_back(pts);
    if (nxv_decode(vd.nxv, o->data.data(), int(o->data.size()), &vd.pic) == 1) {
      std::vector<uint8_t> argb = TakeBuffer(pl);
      ToArgb(vd.pic.data[0], vd.pic.linesize[0], vd.pic.data[1], vd.pic.linesize[1],
             vd.pic.nv12 ? nullptr : vd.pic.data[2], vd.pic.linesize[2], vd.pic.width, vd.pic.height, argb.data());
      PushFrame(pl, TakePts(vd, pts), std::move(argb));  // the earliest pts waiting: display order
    } else if (vd.pending.size() > 16) {
      ++pl.video_failed;
      vd.pending.pop_front();
    }
#else
    std::vector<uint8_t> argb = TakeBuffer(pl);
    for (size_t k = 0; k < argb.size(); k += 4) {
      argb[k] = 0xFF;
      argb[k + 1] = argb[k + 2] = argb[k + 3] = 0x30;
    }
    PushFrame(pl, pts, std::move(argb));
#endif
    return;
  }
  if (vd.broken || !vd.av)
    return;
  // Behind the sound (a slow software decoder: the PC's FFmpeg has no WMV3 assembly and decodes on one thread):
  // FFmpeg skips non-reference pictures past 250 ms, all but keyframes past a second, until it is close again.
  if (o) {
    const int64_t newest = pl.newest_decoded.load();
    const int64_t lag = newest < 0 ? 0 : pl.clock_now.load() - newest;
    int want = lag > 1000 ? 2 : lag > 250 ? 1 : lag < 80 ? 0 : vd.skip;
    if (vd.skip == 2 && want < 2 && !o->key)
      want = 2;  // out of keyframes-only at a keyframe (the pictures before it were not decoded)
    if (want != vd.skip) {
      vd.skip = want;
      vd.av->skip_frame = want == 2 ? AVDISCARD_NONKEY : want == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    }
    if (o->key && vd.skip == 2)
      pl.newest_decoded = pts;  // a keyframe resets the lag: decode on from it
  }
  if (o) {
    av_packet_unref(vd.packet);
    if (av_new_packet(vd.packet, int(o->data.size())) < 0)
      return;
    std::memcpy(vd.packet->data, o->data.data(), o->data.size());
    vd.packet->pts = pts;
    if (o->key)
      vd.packet->flags |= AV_PKT_FLAG_KEY;
    vd.pending.push_back(pts);
    if (avcodec_send_packet(vd.av, vd.packet) < 0) {
      ++pl.video_failed;
      return;
    }
  } else {
    avcodec_send_packet(vd.av, nullptr);
  }
  while (avcodec_receive_frame(vd.av, vd.frame) >= 0) {
    const int64_t fpts = vd.frame->best_effort_timestamp != AV_NOPTS_VALUE ? vd.frame->best_effort_timestamp
                                                                           : TakePts(vd, pts);
    vd.pending.erase(std::remove_if(vd.pending.begin(), vd.pending.end(), [&](int64_t t) { return t <= fpts; }),
                     vd.pending.end());
    if (vd.frame->format == AV_PIX_FMT_YUV420P || vd.frame->format == AV_PIX_FMT_YUVJ420P) {
      std::vector<uint8_t> argb = TakeBuffer(pl);
      ToArgb(vd.frame->data[0], vd.frame->linesize[0], vd.frame->data[1], vd.frame->linesize[1], vd.frame->data[2],
             vd.frame->linesize[2], vd.frame->width, vd.frame->height, argb.data());
      PushFrame(pl, fpts, std::move(argb));
    } else {
      ++pl.video_failed;
    }
    av_frame_unref(vd.frame);
  }
}

bool OpenAudio(Player& pl, AudioDecoder& ad) {
  const AsfStream& s = pl.info.audio;
  if (s.number < 0 || s.tag != 0x162 || !s.channels || !s.rate)
    return false;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
  ad.av = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!ad.av)
    return false;
  ad.av->sample_rate = int(s.rate);
  ad.av->channels = s.channels;
  ad.av->channel_layout = av_get_default_channel_layout(s.channels);
  ad.av->block_align = s.align;
  ad.av->bit_rate = int64_t(s.avg_bytes) * 8;
  ad.av->bits_per_coded_sample = s.bits;
  if (!s.extra.empty()) {
    ad.av->extradata = static_cast<uint8_t*>(av_mallocz(s.extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(ad.av->extradata, s.extra.data(), s.extra.size());
    ad.av->extradata_size = int(s.extra.size());
  }
  if (avcodec_open2(ad.av, codec, nullptr) < 0)
    return false;
  ad.frame = av_frame_alloc();
  ad.packet = av_packet_alloc();
  return true;
}

size_t RingFrames(const Player& pl) { return pl.ring.size() / 2; }

void DecodeAudio(Player& pl, AudioDecoder& ad, const Object& o) {
  av_packet_unref(ad.packet);
  if (av_new_packet(ad.packet, int(o.data.size())) < 0)
    return;
  std::memcpy(ad.packet->data, o.data.data(), o.data.size());
  if (avcodec_send_packet(ad.av, ad.packet) < 0)
    return;
  const float volume = float(REXCVAR_GET(sa2_interviews_volume));
  while (avcodec_receive_frame(ad.av, ad.frame) >= 0) {
    const int n = ad.frame->nb_samples;
    const int ch = ad.frame->channels;
    const bool planar = ad.frame->format == AV_SAMPLE_FMT_FLTP;
    if (n > 0 && (planar || ad.frame->format == AV_SAMPLE_FMT_FLT)) {
      std::unique_lock lock(pl.mutex);
      if (pl.audio_pts0 < 0)
        pl.audio_pts0 = int64_t(o.pts) - int64_t(pl.info.preroll_ms);
      const size_t cap = RingFrames(pl);
      for (int i = 0; i < n && pl.ring_count < cap; ++i) {
        float l, r;
        if (planar) {
          l = reinterpret_cast<const float*>(ad.frame->extended_data[0])[i];
          r = ch > 1 ? reinterpret_cast<const float*>(ad.frame->extended_data[1])[i] : l;
        } else {
          const float* f = reinterpret_cast<const float*>(ad.frame->data[0]) + size_t(i) * ch;
          l = f[0];
          r = ch > 1 ? f[1] : l;
        }
        const size_t w = (pl.ring_read + pl.ring_count) % cap;
        pl.ring[2 * w] = l * volume;
        pl.ring[2 * w + 1] = r * volume;
        ++pl.ring_count;
      }
      pl.audio_frames += uint64_t(n);
    }
    av_frame_unref(ad.frame);
  }
}

// The worker: reads the file, keeps up to kDecodedMax pictures decoded ahead and about two seconds of sound.
void WorkerMain() {
  Player& pl = g_player;
#if SA2_NXVIDEO
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
#endif
  Reader r;
  std::string why;
  if (!OpenFile(r, pl.path, why) || !ParseHeader(r, pl.info, why)) {
    REXLOG_ERROR("[interview] {}: {}", pl.path, why);
    pl.failed = true;
    pl.cv.notify_all();
    return;
  }
  const AsfInfo& info = pl.info;
  AudioDecoder ad;
  pl.has_audio = OpenAudio(pl, ad);
  if (REXCVAR_GET(sa2_interviews_log)) {
    REXLOG_INFO("[interview] {}: {} packets of {} bytes, preroll {} ms; video stream {} {}x{}; audio stream {} (tag "
                "{:#x}, {} ch, {} Hz, {} bytes a block): {}",
                pl.path, info.packets, info.packet_size, info.preroll_ms, info.video.number, info.video.width,
                info.video.height, info.audio.number, info.audio.tag, info.audio.channels, info.audio.rate,
                info.audio.align, pl.has_audio ? "WMA Pro on the SDK's FFmpeg" : "not played");
  }
  VideoDecoder vd;
  Partial parts[128];
  std::vector<uint8_t> packet(info.packet_size);
  std::vector<Object> objects;
  uint64_t next_packet = 0;
  const size_t audio_target = size_t(info.audio.rate ? info.audio.rate : 48000) * 2;  // two seconds
  while (!pl.quit) {
    bool progress = false;
    // a picture, when there is room for one
    Object* next = nullptr;
    {
      std::lock_guard lock(pl.mutex);
      if (pl.decoded.size() < size_t(kDecodedMax) && !pl.video_in.empty())
        next = &pl.video_in.front();
    }
    if (next) {
      Object o = std::move(*next);
      {
        std::lock_guard lock(pl.mutex);
        pl.video_in.pop_front();
      }
      DecodeVideo(pl, vd, &o);
      progress = true;
    }
    // more of the file, while the sound is short or no picture waits
    size_t audio_have, video_waiting;
    {
      std::lock_guard lock(pl.mutex);
      audio_have = pl.ring_count;
      video_waiting = pl.video_in.size();
    }
    if (!pl.eof && ((pl.has_audio && audio_have < audio_target) || video_waiting < 4)) {
      if (next_packet >= info.packets || !r.Read(info.data_offset + next_packet * info.packet_size, packet.data(),
                                                 packet.size())) {
        DecodeVideo(pl, vd, nullptr);
        pl.eof = true;
      } else {
        ++next_packet;
        objects.clear();
        ParsePacket(packet.data(), info.packet_size, parts, objects);
        for (Object& o : objects) {
          if (o.stream == info.video.number) {
            ++pl.video_objects;
            std::lock_guard lock(pl.mutex);
            pl.video_in.push_back(std::move(o));
          } else if (pl.has_audio && o.stream == info.audio.number) {
            ++pl.audio_objects;
            DecodeAudio(pl, ad, o);
          }
        }
      }
      progress = true;
    }
    if (!progress) {
      std::unique_lock lock(pl.mutex);
      pl.cv.wait_for(lock, std::chrono::milliseconds(10));
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// The clock and the shown frame (the game's thread)

double Clock(Player& pl) {
  const auto now = std::chrono::steady_clock::now();
  if (!pl.clock_started) {
    pl.clock_started = true;
    pl.clock_wall = now;
    pl.clock_ms = 0;
  }
  // the game asked for no frame for a while (a pause, the game menu): no time passed. A second: a busy PC's (or an
  // emulator's) game thread stalls for a few hundred ms, and at 250 ms the sound stopped with it (2026-11-22, a PC run
  // beside a build played 255 s of the interview in 378); the sound is the clock, so the pictures catch up after a stall.
  if (pl.last_poll.time_since_epoch().count() && now - pl.last_poll > std::chrono::milliseconds(1000))
    pl.clock_wall = now;
  pl.last_poll = now;
  bool from_audio = false;
  int64_t audio_pts0 = -1;
  if (pl.has_audio && pl.audio_registered) {
    std::lock_guard lock(pl.mutex);
    audio_pts0 = pl.audio_pts0;
    from_audio = audio_pts0 >= 0 && (pl.ring_count > 0 || !pl.eof);
  }
  if (from_audio) {
    const double rate = pl.info.audio.rate ? double(pl.info.audio.rate) : 48000.0;
    const double t = double(audio_pts0) + double(pl.samples_played.load()) * 1000.0 / rate - kAudioLatencyMs;
    pl.clock_ms = std::max(pl.clock_ms, t);
  } else {
    pl.clock_ms += std::chrono::duration<double, std::milli>(now - pl.clock_wall).count();
  }
  pl.clock_wall = now;
  pl.clock_now = int64_t(pl.clock_ms);
  return pl.clock_ms;
}

// The newest picture due, into the next texture; the texture holding the shown picture.
uint32_t ShowFrame(uint8_t* base) {
  Player& pl = g_player;
  const double now = Clock(pl);
  Frame chosen;
  bool have = false;
  {
    std::lock_guard lock(pl.mutex);
    while (!pl.decoded.empty() && (double(pl.decoded.front().pts) <= now || !pl.frames_shown)) {
      if (have)
        pl.pool.push_back(std::move(chosen.argb));
      chosen = std::move(pl.decoded.front());
      pl.decoded.pop_front();
      have = true;
      if (!pl.frames_shown)
        break;
    }
  }
  if (have) {
    const int slot = pl.texture_next;
    pl.texture_next = (pl.texture_next + 1) % kTextures;
    std::memcpy(rex::memory::GuestPtr(base, pl.texture_data[slot]), chosen.argb.data(), chosen.argb.size());
    rex::memory::MarkStoreCardsAt(base, pl.texture_data[slot], uint32_t(chosen.argb.size()));
    pl.texture_shown = pl.textures[slot];
    pl.shown_pts = chosen.pts;
    ++pl.frames_shown;
    std::lock_guard lock(pl.mutex);
    pl.pool.push_back(std::move(chosen.argb));
  }
  pl.cv.notify_all();
  if (REXCVAR_GET(sa2_interviews_log) &&
      std::chrono::steady_clock::now() - pl.last_log > std::chrono::seconds(10)) {
    pl.last_log = std::chrono::steady_clock::now();
    std::lock_guard lock(pl.mutex);
    REXLOG_INFO("[interview] {:.1f} s: {} pictures shown (the last {} ms), {} decoded of {} read ({} failed), {} "
                "waiting; sound {:.1f} s played, {:.1f} s buffered",
                now / 1000.0, pl.frames_shown, pl.shown_pts, pl.video_decoded, pl.video_objects, pl.video_failed,
                pl.video_in.size(), double(pl.samples_played.load()) / (pl.info.audio.rate ? pl.info.audio.rate : 48000),
                double(pl.ring_count) / (pl.info.audio.rate ? pl.info.audio.rate : 48000));
  }
  return pl.texture_shown;
}

bool Finished(Player& pl) {
  if (pl.stopped || pl.failed)
    return true;
  if (!pl.eof)
    return false;
  std::lock_guard lock(pl.mutex);
  return pl.video_in.empty() && pl.decoded.empty() && (!pl.has_audio || pl.ring_count == 0);
}

void StopPlayer() {
  Player& pl = g_player;
  if (!pl.running && !pl.audio_registered)
    return;
  if (pl.running) {
    pl.quit = true;
    pl.cv.notify_all();
    if (pl.worker.joinable())
      pl.worker.join();
    pl.running = false;
  }
  if (pl.audio_registered && pl.audio_system) {
    pl.audio_system->UnregisterClient(pl.audio_index);
    pl.audio_registered = false;
  }
  if (REXCVAR_GET(sa2_interviews_log))
    REXLOG_INFO("[interview] closed: {} pictures shown, {} decoded, {} failed; {:.1f} s of sound played",
                pl.frames_shown, pl.video_decoded, pl.video_failed,
                double(pl.samples_played.load()) / (pl.info.audio.rate ? pl.info.audio.rate : 48000));
}

void ResetPlayer() {
  Player& pl = g_player;
  StopPlayer();
  std::lock_guard lock(pl.mutex);
  pl.quit = pl.eof = pl.failed = pl.stopped = pl.playing = false;
  pl.path.clear();
  pl.info = {};
  pl.video_in.clear();
  for (Frame& f : pl.decoded)
    pl.pool.push_back(std::move(f.argb));
  pl.decoded.clear();
  pl.ring.assign(size_t(48000) * 4 * 2, 0.0f);
  pl.ring_read = pl.ring_count = 0;
  pl.samples_played = 0;
  pl.audio_pts0 = -1;
  pl.has_audio = false;
  pl.texture_shown = 0;
  pl.shown_pts = INT64_MIN;
  pl.frames_shown = 0;
  pl.last_poll = {};
  pl.clock_started = false;
  pl.clock_ms = 0;
  pl.clock_now = 0;
  pl.newest_decoded = -1;
  pl.video_objects = pl.audio_objects = pl.video_decoded = pl.video_failed = pl.audio_frames = 0;
  pl.video_kind.clear();
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// XMedia2's entry points

REX_HOOK_RAW(sub_82570020) {  // setup
  if (!REXCVAR_GET(sa2_interviews_native)) {
    __imp__sub_82570020(ctx, base);
    return;
  }
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_82571408) {  // the player's creation (&player, callback, 0)
  if (!REXCVAR_GET(sa2_interviews_native)) {
    __imp__sub_82571408(ctx, base);
    return;
  }
  std::lock_guard lock(g_setup_mutex);
  g_base = base;
  SetupGuestObjects(base);
  Player& pl = g_player;
  const uint32_t out = ctx.r3.u32;
  if (!pl.block || !out) {
    if (out)
      St32(base, out, 0);
    ctx.r3.u64 = kEFail;
    return;
  }
  ResetPlayer();
  St32(base, pl.p + 4, 1);
  St32(base, pl.a + 4, 1);
  St32(base, pl.b + 4, 1);
  St32(base, out, pl.p);
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_8256E938) {  // shutdown
  if (!REXCVAR_GET(sa2_interviews_native)) {
    __imp__sub_8256E938(ctx, base);
    return;
  }
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_8256EA70) {
  if (!REXCVAR_GET(sa2_interviews_native)) {
    __imp__sub_8256EA70(ctx, base);
    return;
  }
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_8256E9E8) {
  if (!REXCVAR_GET(sa2_interviews_native)) {
    __imp__sub_8256E9E8(ctx, base);
    return;
  }
  ctx.r3.u64 = kSOk;
}

// What the two frame hooks call: methods of XMedia2's core object (sub_82577890), which only its setup makes. The hand
// native of sub_82153108 (app/generated/hot/n_82153108.h) calls the frame hooks' originals, so these are hooked too.
REX_HOOK_RAW(sub_82577AE8) {
  if (!REXCVAR_GET(sa2_interviews_native)) {
    __imp__sub_82577AE8(ctx, base);
    return;
  }
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_82577AA8) {
  if (!REXCVAR_GET(sa2_interviews_native)) {
    __imp__sub_82577AA8(ctx, base);
    return;
  }
  ctx.r3.u64 = kSOk;
}

// ---------------------------------------------------------------------------------------------------------------------
// The methods

REX_HOOK_RAW(sub_82574CA8) {  // AddRef
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_82574CA8(ctx, base);
    return;
  }
  const uint32_t n = Ld32(base, ctx.r3.u32 + 4) + 1;
  St32(base, ctx.r3.u32 + 4, n);
  ctx.r3.u64 = n;
}

REX_HOOK_RAW(sub_8256F830) {  // Release
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_8256F830(ctx, base);
    return;
  }
  const uint32_t object = ctx.r3.u32;
  uint32_t n = Ld32(base, object + 4);
  n = n ? n - 1 : 0;
  St32(base, object + 4, n);
  if (n == 0 && object == g_player.p) {
    std::lock_guard lock(g_setup_mutex);
    StopPlayer();
  }
  ctx.r3.u64 = n;
}

REX_HOOK_RAW(sub_825750B8) {  // player->m4(path, -1): open
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_825750B8(ctx, base);
    return;
  }
  Player& pl = g_player;
  std::string path;
  for (uint32_t a = ctx.r4.u32; a && path.size() < 260; ++a) {
    const char c = char(*rex::memory::GuestPtr(base, a));
    if (!c)
      break;
    path += c;
  }
  std::lock_guard lock(g_setup_mutex);
  if (pl.running)
    StopPlayer();
  pl.path = path;
  pl.quit = false;
  if (!MakeTextures(base)) {
    pl.failed = true;
    ctx.r3.u64 = kEFail;
    return;
  }
  // black until the first picture (the textures keep the last interview's pictures otherwise)
  for (int i = 0; i < kTextures; ++i) {
    uint32_t* words = reinterpret_cast<uint32_t*>(rex::memory::GuestPtr(base, pl.texture_data[i]));
    std::fill(words, words + size_t(kOut) * kOutH, __builtin_bswap32(0xFF000000u));
    rex::memory::MarkStoreCardsAt(base, pl.texture_data[i], kOut * kOutH * 4);
  }
  pl.texture_shown = pl.textures[0];
  pl.texture_next = 1;
  pl.running = true;
  pl.worker = std::thread(WorkerMain);
  if (REXCVAR_GET(sa2_interviews_log))
    REXLOG_INFO("[interview] {} opened by the port's player (XMedia2 is not used)", path);
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_82577A68) {  // player->m5(): play
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_82577A68(ctx, base);
    return;
  }
  Player& pl = g_player;
  pl.playing = true;
  if (!pl.audio_registered) {
    pl.audio_system = static_cast<rex::audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());
    if (pl.audio_system && pl.audio_system->RegisterClient(kSlotAudio, pl.cookie, &pl.audio_index) == 0) {
      pl.audio_registered = true;
    } else {
      REXLOG_WARN("[interview] no audio render client: the interview plays without sound");
    }
  }
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_82579B18) {  // player->m7(): stop (the skip button)
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_82579B18(ctx, base);
    return;
  }
  g_player.stopped = true;
  if (REXCVAR_GET(sa2_interviews_log))
    REXLOG_INFO("[interview] skipped");
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_8257B7D8) {  // player->m11(&a)
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_8257B7D8(ctx, base);
    return;
  }
  const Player& pl = g_player;
  if (ctx.r4.u32) {
    St32(base, ctx.r4.u32, pl.a);
    St32(base, pl.a + 4, Ld32(base, pl.a + 4) + 1);
  }
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_825701B8) {  // player->m15(&b)
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_825701B8(ctx, base);
    return;
  }
  const Player& pl = g_player;
  if (ctx.r4.u32) {
    St32(base, ctx.r4.u32, pl.b);
    St32(base, pl.b + 4, Ld32(base, pl.b + 4) + 1);
  }
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_825702A8) {  // player->m12(): 5 when there is a picture to draw
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_825702A8(ctx, base);
    return;
  }
  // 5 from the start (a black texture until the first picture): the game then asks for frames, which starts the sound
  // and the clock even when no picture can be decoded (a file whose video this build cannot decode plays its sound).
  Player& pl = g_player;
  ctx.r3.u64 = pl.playing && !pl.failed && pl.textures[0] ? 5 : 1;
}

REX_HOOK_RAW(sub_8257B888) {  // a->m3()
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_8257B888(ctx, base);
    return;
  }
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_825D6218) {  // a->m17(): the end
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_825D6218(ctx, base);
    return;
  }
  const bool done = Finished(g_player);
  if (done && REXCVAR_GET(sa2_interviews_log)) {
    static uint32_t logged_p = 0;
    if (logged_p != g_player.frames_shown) {
      logged_p = g_player.frames_shown;
      REXLOG_INFO("[interview] the end ({})", g_player.stopped ? "skipped" : g_player.failed ? "the file failed" : "played");
    }
  }
  ctx.r3.u64 = done ? 1 : 0;
}

REX_HOOK_RAW(sub_825C2D70) {  // b->m3(0, &texture, &info): the frame as a texture
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_825C2D70(ctx, base);
    return;
  }
  const uint32_t out = ctx.r5.u32;
  const uint32_t t = ShowFrame(base);
  if (!t) {
    if (out)
      St32(base, out, 0);
    ctx.r3.u64 = kEFail;
    return;
  }
  renut::gpu::native::AddRefResource(t);
  if (out)
    St32(base, out, t);
  ctx.r3.u64 = kSOk;
}

REX_HOOK_RAW(sub_825C2F00) {  // any other method
  if (!Ours(ctx.r3.u32)) {
    __imp__sub_825C2F00(ctx, base);
    return;
  }
  static std::atomic<int> logged{0};
  if (logged++ < 8)
    REXLOG_WARN("[interview] an XMedia2 method the port does not have, on {:08X} (called from {:08X})", ctx.r3.u32,
                uint32_t(ctx.lr));
  ctx.r3.u64 = kENotImpl;
}

// The audio render client's callback (the audio worker): a frame of 6 x 256 big-endian floats, channel by channel.
REX_HOOK_RAW(sub_825C2BD8) {
  // r3: the audio system's cell holding our argument (AudioSystem::RegisterClient's wrapped_callback_arg). With
  // sa2_interviews_native on, XMedia2's own code never runs, so its callers of this function are not there.
  Player& pl = g_player;
  const uint32_t cell = ctx.r3.u32;
  bool ours = false;
  if (REXCVAR_GET(sa2_interviews_native) && pl.block && cell) {
    if (cell == pl.audio_cell)
      ours = true;
    else if (REX_KERNEL_MEMORY()->LookupHeap(cell) && Ld32(base, cell) == pl.cookie)
      ours = true, pl.audio_cell = cell;
  }
  if (!ours) {
    __imp__sub_825C2BD8(ctx, base);
    return;
  }
  const uint32_t buf = pl.audio_buf + (pl.audio_flip++ & 1) * 6144;
  float* out = reinterpret_cast<float*>(rex::memory::GuestPtr(base, buf));
  float l[256] = {}, r[256] = {};
  const bool paused = !pl.last_poll.time_since_epoch().count() ||
                      std::chrono::steady_clock::now() - pl.last_poll > std::chrono::milliseconds(1000);
  if (pl.playing && !paused && !pl.stopped) {
    std::lock_guard lock(pl.mutex);
    const size_t cap = RingFrames(pl);
    const size_t n = std::min<size_t>(256, pl.ring_count);
    for (size_t i = 0; i < n; ++i) {
      l[i] = pl.ring[2 * pl.ring_read];
      r[i] = pl.ring[2 * pl.ring_read + 1];
      pl.ring_read = (pl.ring_read + 1) % cap;
    }
    pl.ring_count -= n;
    pl.samples_played += n;
  }
  auto put = [&](int channel, const float* s) {
    for (int i = 0; i < 256; ++i) {
      uint32_t v;
      std::memcpy(&v, s ? &s[i] : &l[0], 4);
      if (!s)
        v = 0;
      out[channel * 256 + i] = std::bit_cast<float>(__builtin_bswap32(v));
    }
  };
  put(0, l);
  put(1, r);
  for (int c = 2; c < 6; ++c)
    put(c, nullptr);
  pl.cv.notify_all();
  if (pl.audio_system)
    pl.audio_system->SubmitFrame(pl.audio_index, buf);
  ctx.r3.u64 = 0;
}
