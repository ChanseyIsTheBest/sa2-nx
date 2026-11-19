// banjo-nx - native renderer: vertex copies kept across frames (banjo_ring_vertex_persistent).
//
// The problem, measured
// ---------------------
// Every draw's vertex window goes to the upload buffer with its byte order swapped. DedupeVertices and
// ContainVertices (masseffect_native_vertices_dedupe.h) reuse a copy while two things hold: the same upload buffer and
// no point in between where the game can learn of GPU progress. Banjo's stream has about 15 such points a frame, and a
// new upload buffer every frame.
//
//   PC, gameplay (Spiral Mountain), masseffect_native_diag_vertices_repeated, 10 s:
//       9280.3 MB of vertex windows drawn; 5913.2 MB (64 %) the same range and bytes as earlier in the frame,
//       3366.7 MB (36 %) the same as in an earlier frame: every byte is a repeat
//       4181.6 MB copied all the same (14 MB a frame)
//   Console (2026-10-13, 2397 MHz): 345 MB a second copied; the uploads are 3.5 us of each draw.
//
// What this piece does
// --------------------
// The other half the header above leaves out: a persistent vertex buffer ("the arena") and a table from (physical
// address, bytes, byte order, conversion) to the block of the arena that holds that window, converted. A window whose
// bytes were the same in three frames gets a block. From then on a draw of it binds the arena and copies nothing.
//
// Why it is exact
// ---------------
// A block is used only when the guest's bytes are the ones it was made from:
//   - the first time a window is drawn after a synchronization point (Forget), all its bytes are fingerprinted
//     (RangeFingerprint: XXH3, or the A57's two CRC32 polynomials) and compared with the fingerprint taken when the
//     block was written. Different: the block is given up and the window is copied to the upload buffer as before;
//   - until the next synchronization point the same rule DedupeVertices states applies (the game cannot rewrite a
//     range a draw already referenced without learning that the GPU is done with it), with its same extra check for
//     the small ranges (the sampled fingerprint);
//   - past a synchronization point, for the draws the game had already submitted when the bytes were read
//     (banjo_ring_vertex_persistent_submitted): a draw's vertices are final when the game moves the ring's write
//     pointer past it (the console's GPU may read them from that moment), and by the rule above they stay untouched
//     until the GPU has run that draw. So for a draw submitted before the bytes were read, they are the same bytes
//     when the ring reaches it, whatever fences lie in between. The ring thread is the slow side on the console, so
//     the game is usually a frame ahead and this covers most of a frame's draws.
// So the bytes are read once instead of read, swapped and written, and the 64-bit fingerprint is the only thing
// between this and a byte comparison (banjo_cpu_verify makes that comparison on every use, and the first 4096 uses
// are compared in every configuration).
//
// What changes often stays out: a window whose bytes changed twice is left alone for a while (longer each time),
// without being fingerprinted at all.
//
// The arena
// ---------
// Blocks are handed out in address order and never reused one by one: the GPU may still be reading a block given up
// a moment ago. When the arena is full every block is given up at once (a new epoch) and it starts again from the
// bottom once every submission that could reference the old blocks has finished (the work slots have all been
// waited for). Windows then earn their block again on their next draw.
//
// Single thread (the ring thread).

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace masseffect::native {

class PersistVertices {
 public:
  // Power of 2 (2 MB); a key lives in kProbes consecutive slots. Gameplay holds about 1,600 windows with a block and
  // a few hundred seen for a frame or two; dropped() tells if it ever falls short.
  static constexpr size_t kSlots = 32768;
  static constexpr uint32_t kProbes = 4;
  static constexpr uint32_t kConfirmations = 2;   // frames a window must come back unchanged in before it gets a block
  static constexpr uint32_t kIdleFrames = 1800;   // a block not drawn for this long may lose its table entry
  static constexpr uint32_t kCalmFrames = 4096;   // without a change for this long: its changes are forgotten

  struct Entry {
    uint64_t key = 0;       // (address << 2) | order, with the top bit set; 0 = never used
    uint64_t hash = 0;      // of the guest bytes, the last time they were read whole
    uint64_t sample = 0;    // the caller's sampled fingerprint of those bytes (0: it takes none for this size)
    uint64_t through = 0;   // the stream position submitted when its bytes were last read whole (0: not known)
    uint32_t offset = 0;    // of its block in the arena, while epoch is the arena's
    uint32_t bytes = 0;
    uint32_t signature = 0; // of the conversion its copy gets besides the byte order (0: none)
    uint32_t epoch = 0;     // the arena epoch its block belongs to; 0 = no block
    uint32_t interval = 0;  // the synchronization interval its block was last proven in
    uint32_t frame = 0;     // the last frame its bytes were read whole
    uint32_t until = 0;     // the frame of its last change, plus how long it is left alone for changing often
    uint8_t confirmations = 0;
    uint8_t changes = 0;
    bool seen = false;      // hash is of bytes really read
  };
  static_assert(sizeof(Entry) == 64, "an entry is one cache line");

  enum class Outcome : uint8_t { kCopy, kHit, kPromote };

  void Configure(uint64_t arena_bytes, uint32_t frames_in_flight) {
    arena_bytes_ = arena_bytes;
    frames_in_flight_ = frames_in_flight;
    table_.assign(arena_bytes ? kSlots : 0, Entry{});
  }
  bool active() const { return arena_bytes_ != 0 && !stopped_; }

  // A new upload buffer: the previous one has been submitted. True when the arena starts a new epoch here (every
  // block given up).
  bool NewFrame(uint64_t frame) {
    frame_ = uint32_t(frame);
    bool new_epoch = false;
    if (reset_wanted_ && !resetting_) {
      resetting_ = true;
      new_epoch = true;
      ++epoch_;
      ++interval_;
      reset_done_at_ = frame_ + frames_in_flight_ + 1;
      // An arena that fills again right after starting over is too small for what is drawn: copying and
      // fingerprinting everything in turns costs more than it saves.
      if (resets_ && frame_ - reset_frame_ < 600 && ++quick_resets_ >= 3) stopped_ = true;
      reset_frame_ = frame_;
    }
    if (resetting_ && int32_t(frame_ - reset_done_at_) >= 0) {
      resetting_ = false;
      reset_wanted_ = false;
      used_ = 0;
      given_up_bytes_ = 0;
      resident_ = 0;
      resident_bytes_ = 0;
      ++resets_;
    }
    return new_epoch;
  }

  // The guest may have synchronized with the GPU and rewritten memory: every block has to be proven again.
  void Forget() { ++interval_; }

  // The entry of that window, made if it is new. nullptr: no room for it in the table (it is copied as before).
  Entry* Open(uint64_t address, uint32_t bytes, uint32_t order, uint32_t signature) {
    const uint64_t key = (address << 2) | (uint64_t(order) & 0x3) | 0x8000000000000000ull;
    const size_t first = Index(key, bytes, signature);
    Entry* victim = nullptr;
    for (uint32_t p = 0; p < kProbes; ++p) {
      Entry& e = table_[(first + p) & (kSlots - 1)];
      if (e.key == key && e.bytes == bytes && e.signature == signature) return &e;
      if (!e.key) {  // (slots never empty again, so the window is in none of the later ones)
        victim = &e;
        break;
      }
      // The one out of use the longest: without a block and not from this frame or the last, or with a block nobody
      // draws any more (the block is given up).
      const uint32_t idle = frame_ - e.frame;
      if (idle < (e.epoch == epoch_ ? kIdleFrames : 2u)) continue;
      if (!victim || idle > frame_ - victim->frame) victim = &e;
    }
    if (!victim) {
      ++dropped_;
      return nullptr;
    }
    if (victim->key && victim->epoch == epoch_) GiveUp(*victim);
    *victim = Entry{};
    victim->key = key;
    victim->bytes = bytes;
    victim->signature = signature;
    victim->frame = frame_;
    return victim;
  }

  // The table slot of an entry (banjo_d3d_locks census: a side array indexed the same way).
  size_t SlotOf(const Entry& e) const { return size_t(&e - table_.data()); }

  // The entry of that window if it has one (nothing is made).
  const Entry* Peek(uint64_t address, uint32_t bytes, uint32_t order, uint32_t signature) const {
    const uint64_t key = (address << 2) | (uint64_t(order) & 0x3) | 0x8000000000000000ull;
    const size_t first = Index(key, bytes, signature);
    for (uint32_t p = 0; p < kProbes; ++p) {
      const Entry& e = table_[(first + p) & (kSlots - 1)];
      if (e.key == key && e.bytes == bytes && e.signature == signature) return &e;
      if (!e.key) return nullptr;
    }
    return nullptr;
  }

  // Its block is usable without reading the bytes again: 1 = proven in this synchronization interval, 2 = the draw
  // at `position` in the stream (0: not known) had been submitted when the bytes were last read. 0 = neither.
  int Proven(const Entry& e, uint64_t sample, uint64_t position) const {
    if (e.epoch != epoch_ || e.sample != sample) return 0;
    if (e.interval == interval_) return 1;
    return position && position <= e.through ? 2 : 0;
  }
  // It changes too often to be worth reading.
  bool LeftAlone(const Entry& e) const { return int32_t(e.until - frame_) > 0; }

  // The window's bytes have just been fingerprinted whole; `submitted` is the stream position the game had submitted
  // before they were read (0: not known).
  Outcome Judge(Entry& e, uint64_t hash, uint64_t sample, uint64_t submitted) {
    ++judged_;
    bytes_judged_ += e.bytes;
    if (e.seen && e.hash == hash) {
      if (e.epoch == epoch_) {
        e.interval = interval_;
        e.through = submitted;
        e.sample = sample;
        e.frame = frame_;
        if (e.changes && frame_ - e.until > kCalmFrames) e.changes = 0;
        return Outcome::kHit;
      }
      if (e.frame != frame_ && e.confirmations < 255) ++e.confirmations;
      e.frame = frame_;
      return e.confirmations >= kConfirmations && !resetting_ ? Outcome::kPromote : Outcome::kCopy;
    }
    if (e.seen) {
      ++changed_;
      if (e.epoch == epoch_) GiveUp(e);
      e.changes = uint8_t(std::min<uint32_t>(e.changes + 1u, 15u));
      e.until = frame_;
      if (e.changes >= 2) {
        e.until += 32u << std::min<uint32_t>(e.changes, 7u);
        ++left_alone_;
      }
    }
    e.seen = true;
    e.hash = hash;
    e.confirmations = 0;
    e.frame = frame_;
    return Outcome::kCopy;
  }

  // Room for a block whose offset is a multiple of `multiple` (the vertex stride, or 0). False: the arena is full;
  // it starts over at the next frame.
  bool Allocate(uint32_t bytes, uint32_t multiple, uint64_t& offset) {
    if (resetting_ || stopped_) return false;
    const uint64_t m = multiple ? multiple : 4;
    const uint64_t start = (used_ + m - 1) / m * m;
    if (start + bytes > arena_bytes_ || start + bytes > UINT32_MAX) {
      reset_wanted_ = true;
      ++full_;
      return false;
    }
    offset = start;
    used_ = start + bytes;
    return true;
  }

  // The block at `offset` now holds the converted copy of the bytes just judged.
  void Commit(Entry& e, uint64_t offset, uint64_t sample, uint64_t submitted) {
    e.offset = uint32_t(offset);
    e.epoch = epoch_;
    e.interval = interval_;
    e.through = submitted;
    e.sample = sample;
    ++promoted_;
    bytes_promoted_ += e.bytes;
    ++resident_;
    resident_bytes_ += e.bytes;
  }

  // A self-check found a block that is not its window's bytes: nothing is served from the arena any more.
  void Stop() {
    stopped_ = true;
    ++epoch_;
    ++interval_;
  }

  // A window bound from the arena: 0 = after reading its bytes, 1 and 2 = Proven's.
  void NoteHit(uint32_t bytes, int proven) {
    ++(proven == 2 ? hits_submitted_ : proven ? hits_proven_ : hits_judged_);
    bytes_hit_ += bytes;
  }

  bool stopped() const { return stopped_; }
  uint32_t epoch() const { return epoch_; }
  uint64_t used() const { return used_; }
  uint64_t arena_bytes() const { return arena_bytes_; }
  uint64_t resident() const { return resident_; }
  uint64_t resident_bytes() const { return resident_bytes_; }
  uint64_t given_up_bytes() const { return given_up_bytes_; }
  uint64_t resets() const { return resets_; }
  // Counters for the periodic report (cumulative; the report takes deltas).
  uint64_t hits_proven() const { return hits_proven_; }
  uint64_t hits_submitted() const { return hits_submitted_; }
  uint64_t hits_judged() const { return hits_judged_; }
  uint64_t bytes_hit() const { return bytes_hit_; }
  uint64_t judged() const { return judged_; }
  uint64_t bytes_judged() const { return bytes_judged_; }
  uint64_t promoted() const { return promoted_; }
  uint64_t bytes_promoted() const { return bytes_promoted_; }
  uint64_t changed() const { return changed_; }
  uint64_t left_alone() const { return left_alone_; }
  uint64_t dropped() const { return dropped_; }
  uint64_t full() const { return full_; }

 private:
  void GiveUp(Entry& e) {
    e.epoch = 0;
    given_up_bytes_ += e.bytes;
    resident_ -= resident_ ? 1 : 0;
    resident_bytes_ -= std::min<uint64_t>(resident_bytes_, e.bytes);
  }

  static size_t Index(uint64_t key, uint32_t bytes, uint32_t signature) {
    uint64_t x = key ^ (uint64_t(bytes) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(signature) << 32);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    return size_t(x) & (kSlots - 1);
  }

  std::vector<Entry> table_;
  uint64_t arena_bytes_ = 0;
  uint64_t used_ = 0;
  uint32_t frames_in_flight_ = 3;
  uint32_t frame_ = 0;
  uint32_t epoch_ = 1;
  uint32_t interval_ = 1;
  bool reset_wanted_ = false, resetting_ = false, stopped_ = false;
  uint32_t reset_done_at_ = 0, reset_frame_ = 0, quick_resets_ = 0;

  uint64_t resident_ = 0, resident_bytes_ = 0, given_up_bytes_ = 0, resets_ = 0;
  uint64_t hits_proven_ = 0, hits_submitted_ = 0, hits_judged_ = 0, bytes_hit_ = 0, judged_ = 0, bytes_judged_ = 0;
  uint64_t promoted_ = 0, bytes_promoted_ = 0, changed_ = 0, left_alone_ = 0, dropped_ = 0, full_ = 0;
};

}  // namespace masseffect::native
