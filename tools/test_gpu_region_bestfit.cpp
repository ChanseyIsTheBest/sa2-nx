// sasrt-nx (2026-11-09 night): host test of plume's GPU region allocator, first fit from the top against best fit
// (app/thirdparty/plume/plume_deko3d_region.h, GpuRegionRanges::bestFit). A churn like the docked race's: render-target
// sets of ~10-28 MB made and freed at resolution changes among small buffers and textures (64 KB-4 MB) that come and go.
// Prints, for each policy, the free ranges and the largest one at the end, and the allocations that found no room.
//   _tools/llvm/bin/clang++.exe -std=c++17 -O2 -I app/thirdparty/plume tools/test_gpu_region_bestfit.cpp -o out/t.exe
#include <cstdio>
#include <random>
#include <vector>

#include "plume_deko3d_region.h"

struct Block {
    uintptr_t at;
    uint64_t size;
};

static void run(bool best) {
    plume::GpuRegionRanges r;
    r.bestFit = best;
    r.high = r.low = uintptr_t(1) << 40;
    const uint64_t kMax = uint64_t(1400) << 20, kStep = 2ull << 20;
    auto lower = [&](uintptr_t newLow) { return r.high - newLow <= kMax; };
    std::mt19937_64 rng(1234);
    std::vector<Block> small, targets;
    uint32_t failed = 0, failedTargets = 0;
    for (int round = 0; round < 400; ++round) {
        // a resolution change every 10 rounds: the old set goes, a new one comes (sizes vary with the height)
        if (round % 10 == 0) {
            for (const Block &b : targets)
                r.insert(b.at, b.size), r.used -= b.size;
            targets.clear();
            const double scale = 0.8 + 0.4 * double(rng() % 100) / 100.0;
            for (int i = 0; i < 14; ++i) {
                const uint64_t size = (uint64_t((6 + rng() % 23) * scale * 1048576.0) + 0xFFFF) & ~0xFFFFull;
                const uintptr_t at = r.alloc(size, kStep, lower);
                if (!at) {
                    ++failed, ++failedTargets;
                    continue;
                }
                targets.push_back({at, size});
            }
        }
        // small buffers and textures: some come, some go
        for (int i = 0; i < 20; ++i) {
            const uint64_t size = (uint64_t(64 + rng() % 4096) << 10) & ~0xFFFFull;
            const uintptr_t at = r.alloc(size ? size : 0x10000, kStep, lower);
            if (!at) {
                ++failed;
                continue;
            }
            small.push_back({at, size ? size : 0x10000});
        }
        for (int i = 0; i < 20 && small.size() > 250; ++i) {
            const size_t k = rng() % small.size();
            r.insert(small[k].at, small[k].size);
            r.used -= small[k].size;
            small[k] = small.back();
            small.pop_back();
        }
    }
    uint64_t freeBytes = 0, largest = 0;
    for (const auto &[at, size] : r.free) {
        freeBytes += size;
        if (size > largest)
            largest = size;
    }
    std::printf("%-9s region %4llu MB, used %4llu MB, free %4llu MB in %3zu ranges, largest %4llu MB; failed %u (targets %u)\n",
                best ? "best fit" : "top fit", (unsigned long long)((r.high - r.low) >> 20),
                (unsigned long long)(r.used >> 20), (unsigned long long)(freeBytes >> 20), r.free.size(),
                (unsigned long long)(largest >> 20), failed, failedTargets);
}

int main() {
    run(false);
    run(true);
    return 0;
}
