/**
 * @file    gpu/guest/guest_records.cpp
 */
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/lean_mutex.h"  // blur-nx (2026-11-17, audit #7)

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <o1heap.h>
#include <rex/cvar.h>  // blur-nx (2026-11-17, audit #7)
#include <rex/system/xmemory.h>

#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/ultimate.h"

namespace renut::gpu::records {

namespace {

// Records are small (headers + a few host pointers); 64 MiB is ample.
constexpr u32 kHeapSize = 64u << 20;

std::mutex g_heap_mutex;  // o1heap is not internally synchronised
O1HeapInstance *g_heap = nullptr;
bool g_oom_logged = false;

LeanSharedMutex g_registry_mutex;  // blur-nx (2026-11-17, audit #7, blur_renut_lean_rwlock): was std::shared_mutex
std::unordered_map<u32, RecordKind> g_registry;
std::atomic<uint64_t> g_generation{0};

// rr6-nx (2026-11-08, rr6_records_lockfree; port 47): the same answers as the registry and resource.cpp's owned
// headers, read without a lock. Every record and owned header is an o1heap allocation, so it starts on an
// O1HEAP_ALIGNMENT boundary of the heap block: one byte per such granule says what is there (the low nibble the record
// kind + 1, 0 for none; the top bit an owned header). Pages of 4,096 entries (64 KiB of heap) are made at their first
// mark and never freed, so a reader holds a page without a lock. Marks are made under the registry's (or the owned
// set's) lock, after the containers, which are still kept: the locked path (the switch off) reads them. An address the
// table cannot hold (outside the block or between granules: none in practice) is counted, and while any is, such
// addresses go to the locked path.
constexpr u32 kGranuleShift = 4;
static_assert(O1HEAP_ALIGNMENT == (1u << kGranuleShift), "an allocation starts on a granule");
constexpr u32 kPageShift = 16;
constexpr u32 kPages = kHeapSize >> kPageShift;
constexpr u32 kPageEntries = 1u << (kPageShift - kGranuleShift);
constexpr u8 kOwnedBit = 0x80;
constexpr u8 kKindMask = 0x0F;

std::atomic<u32> g_base_va{0};  // the heap block, set by Init before any allocation
std::atomic<std::atomic<u8> *> g_pages[kPages] = {};
std::mutex g_pages_mutex;  // making a page
std::atomic<u32> g_outside{0};  // marks the table could not hold

// The entry of guest_va, making its page if 'make'. nullptr: not a table address (*outside true), or its page was
// never made (nothing marked there).
std::atomic<u8> *Entry(u32 guest_va, bool make, bool *outside) {
  const u32 base = g_base_va.load(std::memory_order_acquire);
  const u32 off = guest_va - base;
  *outside = !base || off >= kHeapSize || (off & ((1u << kGranuleShift) - 1));
  if (*outside)
    return nullptr;
  std::atomic<std::atomic<u8> *> &slot = g_pages[off >> kPageShift];
  std::atomic<u8> *page = slot.load(std::memory_order_acquire);
  if (!page) {
    if (!make)
      return nullptr;
    std::lock_guard lock(g_pages_mutex);
    page = slot.load(std::memory_order_relaxed);
    if (!page) {
      page = new std::atomic<u8>[kPageEntries]();
      slot.store(page, std::memory_order_release);
    }
  }
  return &page[(off & ((1u << kPageShift) - 1)) >> kGranuleShift];
}

// Sets the bits of 'mask' in guest_va's entry to 'value'. False: the table cannot hold it.
bool Mark(u32 guest_va, u8 mask, u8 value) {
  bool outside = false;
  std::atomic<u8> *e = Entry(guest_va, true, &outside);
  if (!e)
    return false;
  u8 old = e->load(std::memory_order_relaxed);
  while (!e->compare_exchange_weak(old, u8((old & ~mask) | value), std::memory_order_release,
                                   std::memory_order_relaxed)) {
  }
  return true;
}

// The table's entry for guest_va: its byte (0 for an unmade page), or -1 when the locked path must answer (the
// switch off, or an address outside the table while marks are outside).
int Fast(u32 guest_va) {
  if (!banjo::renut_gpu::RecordsLockFreeOn())
    return -1;
  bool outside = false;
  const std::atomic<u8> *e = Entry(guest_va, false, &outside);
  if (e)
    return e->load(std::memory_order_acquire);
  if (outside && g_outside.load(std::memory_order_acquire))
    return -1;
  return 0;
}

} // namespace

u32 AllocPhysicalBlock(u32 size, bool quiet) {
  // The physical window (64 KiB pages) maps host and guest addresses 1:1 in
  // offset, so a host pointer into it round-trips to a guest VA.
  rex::memory::BaseHeap *heap = REX_KERNEL_MEMORY()->LookupHeapByType(true, 64u * 1024u);
  if (!heap) {
    RGPU_ERROR("No guest physical heap");
    return 0;
  }
  u32 guest_va = 0;
  if (!heap->Alloc(size, 64u * 1024u,
                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true,
                   &guest_va)) {
    if (!quiet)
      RGPU_ERROR("Reserving {} bytes of guest physical memory failed", size);
    return 0;
  }
  return guest_va;
}

bool Init() {
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    return true;

  auto *memory = REX_KERNEL_MEMORY();
  // Port 161 (from fnxf_xiii, its note of 2026-11-11): kHeapSize at most; with less guest physical memory left (a
  // game's own pools grown for a larger scene) 48, 32, 24 or 16 MiB, the first that can be reserved, instead of no
  // record heap at all. The lock-free table keeps kHeapSize's span: offsets past the heap are never allocated.
  u32 guest_va = 0, size = 0;
  for (const u32 mb : {kHeapSize >> 20, 48u, 32u, 24u, 16u}) {
    size = mb << 20;
    if (size > kHeapSize)
      continue;
    if ((guest_va = AllocPhysicalBlock(size, true)) != 0)
      break;
  }
  if (!guest_va) {
    RGPU_ERROR("Record heap: reserving even {} bytes failed", size);
    return false;
  }
  if (size != kHeapSize)
    RGPU_WARN("Record heap: {} MiB (the {} MiB could not be reserved)", size >> 20, kHeapSize >> 20);
  g_heap = o1heapInit(memory->TranslateVirtual<void *>(guest_va), size);
  if (!g_heap) {
    RGPU_ERROR("Record heap: o1heapInit failed");
    return false;
  }
  g_base_va.store(guest_va, std::memory_order_release);  // rr6-nx (2026-11-08, rr6_records_lockfree)
  return true;
}

void *AllocRaw(size_t size) {
  std::lock_guard lock(g_heap_mutex);
  if (!g_heap) {
    RGPU_ERROR("Record heap used before Init");
    return nullptr;
  }
  void *p = o1heapAllocate(g_heap, size ? size : 1);
  if (!p && !g_oom_logged) {
    RGPU_ERROR("Record heap exhausted ({} bytes requested)", size);
    g_oom_logged = true;
  }
  return p;
}

void FreeRaw(void *host) {
  if (!host)
    return;
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    o1heapFree(g_heap, host);
}

void Register(u32 guest_va, RecordKind kind) {
  std::unique_lock lock(g_registry_mutex);
  const bool added = g_registry.insert_or_assign(guest_va, kind).second;
  if (!Mark(guest_va, kKindMask, u8(u32(kind) + 1)) && added)  // rr6-nx (2026-11-08, rr6_records_lockfree)
    g_outside.fetch_add(1, std::memory_order_release);
}

void Unregister(u32 guest_va) {
  std::unique_lock lock(g_registry_mutex);
  const bool erased = g_registry.erase(guest_va) != 0;
  if (!Mark(guest_va, kKindMask, 0) && erased)  // rr6-nx (2026-11-08, rr6_records_lockfree)
    g_outside.fetch_sub(1, std::memory_order_release);
  g_generation.fetch_add(1, std::memory_order_release);
}

uint64_t Generation() {
  return g_generation.load(std::memory_order_acquire);
}

bool Lookup(u32 guest_va, RecordKind *out_kind) {
  if (const int fast = Fast(guest_va); fast >= 0) {  // rr6-nx (2026-11-08, rr6_records_lockfree)
    if (!(fast & kKindMask))
      return false;
    *out_kind = RecordKind((fast & kKindMask) - 1);
    return true;
  }
  std::shared_lock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  if (it == g_registry.end())
    return false;
  *out_kind = it->second;
  return true;
}

void MarkOwned(u32 guest_va, bool owned) {
  // resource.cpp's owned set changes under its own lock: the count of marks outside the table is kept here.
  if (!Mark(guest_va, kOwnedBit, owned ? kOwnedBit : 0))
    owned ? g_outside.fetch_add(1, std::memory_order_release) : g_outside.fetch_sub(1, std::memory_order_release);
}

int OwnedFast(u32 guest_va) {
  const int fast = Fast(guest_va);
  return fast < 0 ? -1 : (fast & kOwnedBit) != 0;
}

int OursFast(u32 guest_va) {
  const int fast = Fast(guest_va);
  return fast < 0 ? -1 : fast != 0;
}

} // namespace renut::gpu::records

// blur-nx (2026-11-17, the Renut audit's #7 and #8): gpu/lean_mutex.h.
REXCVAR_DEFINE_BOOL(blur_renut_lean_rwlock, true, "Banjo/Renut",
                    "blur-nx (audit #7/#8, exact): the guest-record registry's lock and the in-place shaders' lock are "
                    "one plain mutex each, not a reader-writer lock (libnx's rwlock: an inner mutex at every lock and "
                    "unlock, condition-variable wakes; 0.5-0.7 % of Blur's Game Master). Their readers are rare (the "
                    "registry is read without a lock, the shaders through a memo). false: std::shared_mutex, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {
bool LeanRwlockOn() {
  static const bool on = REXCVAR_GET(blur_renut_lean_rwlock);
  return on;
}
}  // namespace renut::gpu

