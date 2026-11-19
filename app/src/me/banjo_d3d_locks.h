// banjo-nx: Direct3D's vertex and index buffer Locks as a record the renderer can read (2026-10-14; step 4 of the API
// layer, docs/d3d-native.md section 9). The game threads note every Lock of a vertex or index buffer, by 4 KB physical
// page and a global sequence number; the ring can then ask "has a Lock touched these bytes since sequence S?".
//
// Why: the renderer proves that a vertex block it keeps across frames still holds the guest's bytes by fingerprinting
// all of them once per synchronization interval (banjo_ring_vertex_persistent), about a tenth of the ring thread on the
// console. If the game only writes vertex and index data under a Lock, a page that no Lock has touched since the last
// fingerprint holds the same bytes, and the fingerprint can be skipped. That is an assumption about the game, so it is
// measured first (banjo_diag_d3d_locks: the fingerprints go on, and every block that changed without a Lock is
// counted) before any rule relies on it.
//
// Pages, not exact ranges: a Lock marks the whole buffer's pages (a superset: a neighbour's Lock can only cost a
// fingerprint, never skip one). One table for the 512 MB of physical memory (128 K entries, 512 KB).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

namespace banjo::d3d::locks {

inline constexpr uint32_t kPageShift = 12;
inline constexpr uint32_t kPages = (512u << 20) >> kPageShift;

// The last Lock sequence number that touched each page (0 = never). Written by the game threads, read by the ring.
inline std::unique_ptr<std::atomic<uint32_t>[]> g_page_seq{new std::atomic<uint32_t>[kPages]()};
inline std::atomic<uint32_t> g_seq{1};
inline std::atomic<uint64_t> g_locks{0}, g_pages_marked{0};

// A Lock of the buffer at physical address `address` (any guest view: masked to the 512 MB), `bytes` long.
inline void Mark(uint32_t address, uint32_t bytes) {
  if (!bytes) return;
  const uint32_t a = address & 0x1FFFFFFFu;
  const uint64_t end = uint64_t(a) + bytes;
  const uint32_t first = a >> kPageShift;
  const uint32_t last = uint32_t(std::min<uint64_t>(end - 1, (512ull << 20) - 1) >> kPageShift);
  const uint32_t seq = g_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  for (uint32_t p = first; p <= last; ++p) g_page_seq[p].store(seq, std::memory_order_release);
  g_locks.fetch_add(1, std::memory_order_relaxed);
  g_pages_marked.fetch_add(last - first + 1, std::memory_order_relaxed);
}

// The current sequence (taken before reading bytes: a Lock after it shows as a later number).
inline uint32_t Now() { return g_seq.load(std::memory_order_acquire); }

// True when no Lock touched [address, address + bytes) after sequence `since`.
inline bool Untouched(uint32_t address, uint32_t bytes, uint32_t since) {
  if (!bytes) return true;
  const uint32_t a = address & 0x1FFFFFFFu;
  const uint64_t end = uint64_t(a) + bytes;
  const uint32_t first = a >> kPageShift;
  const uint32_t last = uint32_t(std::min<uint64_t>(end - 1, (512ull << 20) - 1) >> kPageShift);
  for (uint32_t p = first; p <= last; ++p)
    if (g_page_seq[p].load(std::memory_order_acquire) > since) return false;
  return true;
}

}  // namespace banjo::d3d::locks
