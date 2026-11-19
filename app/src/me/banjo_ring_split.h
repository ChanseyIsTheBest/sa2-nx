// banjo-nx (2026-10-16): the ring split (banjo_ring_decoder), step 3 of docs/d3d-full-replacement.md.
//
// The ring thread did two kinds of work one after the other: decoding the game's PM4 (packets, register writes, shader
// identification, the tile-pass bookkeeping) and executing it (the native renderer's draws and copies, the fences and
// memory writes the game sees, interrupts). On the console the decoding was about 12 % of the ring's samples (Type3,
// Packet, the register runs, the load memo, shader identity, the draw's preparation). With the split a decoder thread
// does the first and hands the ring (the executor) what it decoded in stream order through this queue; the executor's
// work per draw no longer includes the decoding.
//
// What keeps it exact:
//   - The executor keeps its own register file. Every item of the queue starts with the register pages the decoder
//     wrote since the item before (a page = 16 registers), so at each item the executor's file is the decoder's file
//     at that point of the stream; the renderer reads the executor's.
//   - Everything the game can see is done by the executor, in stream order: fence and other memory writes, the tiling
//     extents, interrupts (guest code, on the ring's own guest thread), the read pointer write-back and the GPU-wait
//     notifications. So the game learns of GPU progress only when the draws before it have been executed, as before.
//   - The decoder waits where it needs the executor's answer (copies, the Swap, the tile check, the reports): a call
//     item it waits for, run by the executor after everything queued before it.
//   - The decoder runs a WAIT_REG_MEM itself (it must not decode past a wait the game has not released).
//
// Single producer (the decoder), single consumer (the executor).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace me::native::split {

// The dirty register pages of the decoder's file (16 registers a page: a draw's constants, fetch constants and state
// writes are scattered; 64-register pages moved 3.5 MB a Swap on the PC, 13,600 pages for about 2,100 draws).
inline constexpr uint32_t kPageShift = 4, kPageRegisters = 1u << kPageShift;
template <uint32_t kRegisters>
struct DirtyPages {
  static constexpr uint32_t kPages = (kRegisters + kPageRegisters - 1) >> kPageShift;
  static constexpr uint32_t kWords = (kPages + 63) / 64;
  uint64_t bits[kWords] = {};
  bool any = false;
  void Mark(uint32_t index) {
    const uint32_t page = index >> kPageShift;
    bits[page >> 6] |= uint64_t(1) << (page & 63);
    any = true;
  }
  void MarkRange(uint32_t index, uint32_t count) {
    if (!count) return;
    for (uint32_t page = index >> kPageShift, last = (index + count - 1) >> kPageShift; page <= last; ++page)
      bits[page >> 6] |= uint64_t(1) << (page & 63);
    any = true;
  }
  void Clear() {
    std::memset(bits, 0, sizeof(bits));
    any = false;
  }
};

// The queue: a byte ring of items, each a header (kind, bytes) and its payload, 8-byte aligned.
struct Header {
  uint32_t kind;
  uint32_t bytes;  // payload bytes (the item takes sizeof(Header) + bytes rounded up to 8)
};

class Queue {
 public:
  // `active`: false once the ring stops (no wait here outlives it).
  Queue(size_t bytes, const std::atomic<bool>* active) : data_(bytes), mask_(bytes - 1), active_(active) {}
  size_t capacity() const { return data_.size(); }

  // Producer: room for `bytes` of payload; waits while the consumer frees space.
  uint8_t* Begin(uint32_t kind, uint32_t bytes) {
    const uint64_t need = Total(bytes);
    // An item never wraps: if it would, the rest of the ring is skipped with a padding item.
    const uint64_t at = write_ & mask_;
    if (at + need > data_.size()) {
      WaitSpace(data_.size() - at);
      Header pad{kPadding, uint32_t(data_.size() - at - sizeof(Header))};
      std::memcpy(&data_[at], &pad, sizeof(pad));
      write_ += data_.size() - at;
    }
    WaitSpace(need);
    Header h{kind, bytes};
    pending_ = write_;
    std::memcpy(&data_[write_ & mask_], &h, sizeof(h));
    return &data_[(write_ & mask_) + sizeof(Header)];
  }
  void End(uint32_t bytes) {
    write_ = pending_ + Total(bytes);
    published_.store(write_, std::memory_order_release);
    if (sleeping_.load(std::memory_order_seq_cst)) {
      { std::lock_guard<std::mutex> lock(mutex_); }
      cv_.notify_one();
    }
  }
  // Producer: everything published has been consumed.
  bool Drained() const { return read_.load(std::memory_order_acquire) == published_.load(std::memory_order_acquire); }
  void WaitDrained() {  // (the A/B's off half: a spin, never a timed sleep, which is a whole tick on Windows)
    while (!Drained() && active_->load(std::memory_order_acquire)) std::this_thread::yield();
  }

  // Consumer: the next item, or nullptr after waiting up to `timeout` with nothing published.
  const Header* Next(std::chrono::microseconds timeout) {
    for (uint32_t laps = 0;; ++laps) {
      const uint64_t published = published_.load(std::memory_order_acquire);
      if (consumer_ != published) {
        const Header* h = reinterpret_cast<const Header*>(&data_[consumer_ & mask_]);
        if (h->kind == kPadding) {
          consumer_ += sizeof(Header) + h->bytes;
          read_.store(consumer_, std::memory_order_release);
          continue;
        }
        return h;
      }
      if (laps < 32) {
        std::this_thread::yield();
        continue;
      }
      std::unique_lock<std::mutex> lock(mutex_);
      sleeping_.store(true, std::memory_order_seq_cst);
      if (published_.load(std::memory_order_seq_cst) == consumer_) cv_.wait_for(lock, timeout);
      sleeping_.store(false, std::memory_order_seq_cst);
      if (published_.load(std::memory_order_acquire) == consumer_) return nullptr;
    }
  }
  void Pop(const Header* h) {
    consumer_ += Total(h->bytes);
    read_.store(consumer_, std::memory_order_release);
  }

  static constexpr uint32_t kPadding = 0xFFFFFFFFu;

 private:
  static uint64_t Total(uint32_t bytes) { return (sizeof(Header) + uint64_t(bytes) + 7) & ~uint64_t(7); }
  void WaitSpace(uint64_t need) {
    for (uint32_t laps = 0; write_ + need - read_.load(std::memory_order_acquire) > data_.size() &&
                            active_->load(std::memory_order_acquire);
         ++laps) {
      ++full_waits_;
      if (laps < 64) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }

  std::vector<uint8_t> data_;
  uint64_t mask_;
  uint64_t write_ = 0, pending_ = 0;            // producer
  uint64_t consumer_ = 0;                        // consumer
  std::atomic<uint64_t> published_{0}, read_{0};
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> sleeping_{false};
  const std::atomic<bool>* active_;

 public:
  uint64_t full_waits_ = 0;  // producer
};

}  // namespace me::native::split
