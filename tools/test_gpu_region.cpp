// banjo-nx (2026-10-28, h10): host test of plume's GPU region ranges (app/thirdparty/plume/plume_deko3d_region.h).
// A mock heap: newlib's break moves up and down at random; the region may lower the heap's end only kSlack above it.
// After every step: free ranges sorted, merged and inside [low, high); blocks inside the region and overlapping
// nothing; used + free == high - low; and no allocation refused while a free range fit it or the region could grow
// (battd_nx's arena once returned NULL with a fitting run free).
// Build: clang++ -std=c++20 -O1 -I app/thirdparty/plume tools/test_gpu_region.cpp -o test_gpu_region.exe
#include "plume_deko3d_region.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>

namespace {

constexpr uint64_t kGrain = 64ull << 10, kStep = 2ull << 20, kSlack = 32ull << 20;
constexpr uint64_t kGiveBackAt = 128ull << 20, kKeep = 64ull << 20;
constexpr uintptr_t kBase = 0x80000000ull, kTop = kBase + (3085ull << 20);

int failures = 0;
#define CHECK(c, ...)                                     \
    do {                                                  \
        if (!(c)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
            if (++failures > 10)                          \
                std::exit(1);                             \
        }                                                 \
    } while (0)

struct Heap {
    uintptr_t brk = kBase, end = kTop;  // newlib's break and fake_heap_end
};

void Check(const plume::GpuRegionRanges &r, const std::map<uintptr_t, uint64_t> &blocks, const Heap &heap) {
    CHECK(r.low == heap.end || (r.low == r.high), "low %llx != heap end %llx", (unsigned long long)r.low,
          (unsigned long long)heap.end);
    CHECK(heap.brk <= heap.end, "break above the heap's end");
    uintptr_t prev_end = 0;
    uint64_t free_bytes = 0;
    for (const auto &[at, size] : r.free) {
        CHECK(size > 0, "empty free range");
        CHECK(at >= r.low && at + size <= r.high, "free range outside the region");
        CHECK(at > prev_end || prev_end == 0, "free ranges overlap or touch (not merged) at %llx",
              (unsigned long long)at);
        prev_end = at + size;
        free_bytes += size;
    }
    uint64_t used = 0;
    uintptr_t block_end = 0;
    for (const auto &[at, size] : blocks) {
        CHECK(at >= r.low && at + size <= r.high, "block outside the region");
        CHECK(at >= block_end, "blocks overlap");
        block_end = at + size;
        auto f = r.free.upper_bound(at);
        if (f != r.free.begin()) {
            auto p = std::prev(f);
            CHECK(p->first + p->second <= at, "block inside a free range");
        }
        CHECK(f == r.free.end() || f->first >= at + size, "free range inside a block");
        used += size;
    }
    CHECK(used == r.used, "used %llu != blocks %llu", (unsigned long long)r.used, (unsigned long long)used);
    CHECK(used + free_bytes == r.high - r.low, "used + free != region size");
}

uint64_t RandomSize(std::mt19937_64 &rng) {
    const int kind = int(rng() % 100);
    uint64_t size;
    if (kind < 40)
        size = (64ull << 10) * (1 + rng() % 16);  // 64 KB .. 1 MB (command memory, small buffers)
    else if (kind < 70)
        size = (1ull << 20) * (1 + rng() % 8);   // 1 .. 8 MB
    else if (kind < 85)
        size = 16ull << 20;                      // upload ring chunks
    else if (kind < 93)
        size = 32ull << 20;                      // stream cache chunks
    else
        size = 64ull << 20;                      // image heap chunks
    return (size + kGrain - 1) / kGrain * kGrain;
}

void Run(uint64_t seed, int steps) {
    std::mt19937_64 rng(seed);
    Heap heap;
    plume::GpuRegionRanges r;
    r.low = r.high = heap.end;
    std::map<uintptr_t, uint64_t> blocks;
    uint64_t refused = 0, grants = 0;
    for (int i = 0; i < steps; ++i) {
        const int op = int(rng() % 100);
        if (op < 10) {
            // newlib grows or trims (never past the heap's end, never below the base)
            const int64_t delta = int64_t(rng() % (96ull << 20)) - int64_t(40ull << 20);
            uintptr_t brk = uintptr_t(int64_t(heap.brk) + delta);
            if (brk < kBase)
                brk = kBase;
            if (brk > heap.end)
                brk = heap.end;
            heap.brk = brk;
        } else if (op < 55 || blocks.empty()) {
            const uint64_t size = RandomSize(rng);
            const uint64_t largest_before = r.largestFree();
            const uintptr_t low_before = r.low;
            const uint64_t bottom_before = r.free.count(r.low) ? r.free.at(r.low) : 0;
            const uintptr_t at = r.alloc(size, kStep, [&](uintptr_t new_low) {
                if (new_low < heap.brk + kSlack)
                    return false;
                heap.end = new_low;
                return true;
            });
            if (at) {
                ++grants;
                CHECK(at % kGrain == 0, "block not on the grain");
                blocks.emplace(at, size);
            } else {
                ++refused;
                CHECK(largest_before < size, "refused %llu KB with a free range of %llu KB",
                      (unsigned long long)(size >> 10), (unsigned long long)(largest_before >> 10));
                const uint64_t grow = (size - bottom_before + kStep - 1) / kStep * kStep;
                CHECK(low_before - grow < heap.brk + kSlack, "refused %llu KB though the region could grow",
                      (unsigned long long)(size >> 10));
            }
        } else {
            auto it = blocks.begin();
            std::advance(it, rng() % blocks.size());
            r.release(it->first, it->second, kGiveBackAt, kKeep, kStep, [&](uintptr_t new_low) {
                heap.end = new_low;
                return true;
            });
            blocks.erase(it);
        }
        Check(r, blocks, heap);
    }
    std::printf("seed %llu: %d steps, %llu granted, %llu refused; region %llu MB, %llu MB used (peak %llu), %zu free "
                "ranges, largest %llu MB; grown %u times, given back %u times (%llu MB)\n",
                (unsigned long long)seed, steps, (unsigned long long)grants, (unsigned long long)refused,
                (unsigned long long)((r.high - r.low) >> 20), (unsigned long long)(r.used >> 20),
                (unsigned long long)(r.peak >> 20), r.free.size(), (unsigned long long)(r.largestFree() >> 20),
                r.growths, r.givebacks, (unsigned long long)(r.givenBack >> 20));
}

}  // namespace

int main() {
    // A fixed case first: a range that fits only across a block freed below the top.
    {
        Heap heap;
        plume::GpuRegionRanges r;
        r.low = r.high = heap.end;
        auto lower = [&](uintptr_t n) {
            if (n < heap.brk + kSlack)
                return false;
            heap.end = n;
            return true;
        };
        const uintptr_t a = r.alloc(20ull << 20, kStep, lower);
        const uintptr_t b = r.alloc(20ull << 20, kStep, lower);
        CHECK(a && b && a > b, "two blocks packed from the top");
        r.release(a, 20ull << 20, kGiveBackAt, kKeep, kStep, [&](uintptr_t n) { heap.end = n; return true; });
        const uintptr_t c = r.alloc(20ull << 20, kStep, lower);
        CHECK(c == a, "the freed top range used again");
    }
    for (uint64_t seed = 1; seed <= 40; ++seed)
        Run(seed, 20000);
    std::printf(failures ? "FAILED (%d)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
