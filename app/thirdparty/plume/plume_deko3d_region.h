// banjo-nx (2026-10-28, h10): the GPU region's ranges (see plume_deko3d.cpp, "The GPU region"). The heap's end is
// moved through callbacks, so a host test drives this alone (tools/test_gpu_region.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>

namespace plume {

struct GpuRegionRanges {
    // [low, high): the region, at the heap's top; low is newlib's end (fake_heap_end) while the region is in use.
    uintptr_t low = 0, high = 0;
    // Free ranges by address, adjacent ones merged.
    std::map<uintptr_t, uint64_t> free;
    uint64_t used = 0, peak = 0, grown = 0, givenBack = 0;
    uint32_t growths = 0, givebacks = 0;
    // sasrt-nx (2026-11-09 night, Deko3DSetRegionBestFit): a block goes to the smallest free range it fits (the highest
    // of equal ones), the bottom range only when no other fits, at that range's top as before. The first fit from the
    // top split the big ranges a freed target set left for every small buffer: run mem12 held 200-300 MB free in 40-54
    // ranges, the largest 28-55 MB, and a 27 MB target found no room.
    bool bestFit = true;

    void insert(uintptr_t at, uint64_t size) {
        auto next = free.lower_bound(at);
        if (next != free.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == at) {
                at = prev->first;
                size += prev->second;
                free.erase(prev);
            }
        }
        if (next != free.end() && at + size == next->first) {
            size += next->second;
            free.erase(next);
        }
        free.emplace(at, size);
    }

    // 'size' bytes: the top of the highest free range that fits (blocks pack at the region's top and its free space
    // gathers at the bottom, where it can go back to newlib). None fits: the region grows down by what the bottom free
    // range lacks, in whole 'step's, if lower(new low) allows it. 0 when there is no room.
    template <typename Lower>
    uintptr_t alloc(uint64_t size, uint64_t step, Lower &&lower) {
        if (!size)
            return 0;
        for (int pass = 0; pass < 2; ++pass) {
            auto pick = free.end();
            if (bestFit) {
                auto bottom = free.end();
                for (auto it = free.begin(); it != free.end(); ++it) {
                    if (it->second < size)
                        continue;
                    if (it->first == low) {
                        bottom = it;
                        continue;
                    }
                    if (pick == free.end() || it->second <= pick->second)
                        pick = it;  // ascending addresses: '<=' keeps the highest of equal sizes
                }
                if (pick == free.end())
                    pick = bottom;
            } else {
                for (auto it = free.rbegin(); it != free.rend(); ++it) {
                    if (it->second >= size) {
                        pick = std::next(it).base();
                        break;
                    }
                }
            }
            if (pick != free.end()) {
                const uintptr_t start = pick->first;
                const uint64_t left = pick->second - size;
                free.erase(pick);
                if (left)
                    free.emplace(start, left);
                used += size;
                if (used > peak)
                    peak = used;
                return start + left;
            }
            if (pass)
                break;
            uint64_t bottom = 0;
            if (auto it = free.find(low); it != free.end())
                bottom = it->second;
            const uint64_t grow = (size - bottom + step - 1) / step * step;
            if (low < grow)
                return 0;
            const uintptr_t newLow = low - grow;
            if (!lower(newLow))
                return 0;
            insert(newLow, grow);
            low = newLow;
            grown += grow;
            ++growths;
        }
        return 0;
    }

    // A block's range back. When the free range at the bottom reaches 'giveBackAt', all of it but 'keep' (in whole
    // 'step's) goes back to newlib, if raise(new low) allows it.
    template <typename Raise>
    void release(uintptr_t at, uint64_t size, uint64_t giveBackAt, uint64_t keep, uint64_t step, Raise &&raise) {
        used -= size;
        insert(at, size);
        auto it = free.find(low);
        if (it == free.end() || it->second < giveBackAt || it->second <= keep)
            return;
        const uint64_t give = (it->second - keep) / step * step;
        if (!give)
            return;
        const uintptr_t newLow = low + give;
        if (!raise(newLow))
            return;
        const uint64_t rest = it->second - give;
        free.erase(it);
        if (rest)
            free.emplace(newLow, rest);
        low = newLow;
        givenBack += give;
        ++givebacks;
    }

    // sasrt-nx (2026-11-09, Deko3DRegionGiveBack): the free range at the bottom back to newlib now, whatever its size
    // (in whole 'step's), if raise(new low) allows it; the bytes given.
    template <typename Raise>
    uint64_t giveBackBottom(uint64_t step, Raise &&raise) {
        auto it = free.find(low);
        if (it == free.end())
            return 0;
        const uint64_t give = it->second / step * step;
        if (!give)
            return 0;
        const uintptr_t newLow = low + give;
        if (!raise(newLow))
            return 0;
        const uint64_t rest = it->second - give;
        free.erase(it);
        if (rest)
            free.emplace(newLow, rest);
        low = newLow;
        givenBack += give;
        ++givebacks;
        return give;
    }

    uint64_t freeBytes() const {
        uint64_t bytes = 0;
        for (const auto &r : free)
            bytes += r.second;
        return bytes;
    }

    uint64_t largestFree() const {
        uint64_t largest = 0;
        for (const auto &r : free)
            largest = r.second > largest ? r.second : largest;
        return largest;
    }
};

}  // namespace plume
