#include "profiler/switch_stall_watch.h"
#include "profiler/switch_cpu_profiler.h"

#include <rex/system/wait_registry.h>

#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

// Port 167: sdk/src/system/xthread.cpp, every guest thread's guest call chain for the on-demand dump.
extern "C" size_t RexGuestCallChains(char* out, size_t cap);

// See os/switch_stall_watch.h. Adapted from the watchdog of battd_nx (source/diag.c): threads are only
// paused to read their registers and copy the top of their stack, and resumed before anything is formatted,
// allocated or written, so the watchdog never waits on a lock a paused thread holds; the stack is read from
// the copy made while the thread was paused, never live (a running thread can return out of those frames or
// exit, which unmaps its stack).

namespace
{
    constexpr uint64_t POLL_NS = 250'000'000ull;
    constexpr uint64_t REDUMP_NS = 2'000'000'000ull;
    constexpr uint64_t HITCH_NS = 100'000'000ull;          // a frame slower than this is a hitch
    constexpr uint64_t HITCH_REPORT_NS = 1'000'000'000ull; // hitches are summed up at most once a second
    constexpr int MAX_DUMPS_PER_STALL = 5;
    constexpr size_t MAX_THREADS = 64;
    constexpr size_t STACK_COPY_BYTES = 16384;
    constexpr int MAX_FRAMES = 16;         // frame-pointer chain
    constexpr int MAX_STACK_ADDRESSES = 24; // return addresses found scanning the stack

    std::atomic<uint64_t> g_lastFrameTick{ 0 };
    std::atomic<uint64_t> g_frames{ 0 };
    std::atomic<uint32_t> g_hitches{ 0 };
    std::atomic<uint64_t> g_longestHitchTicks{ 0 };
    uint64_t g_thresholdTicks = 0;
    uint64_t g_hitchTicks = 0;
    void (*g_reporter)(std::string&) = nullptr;
    Thread g_thread;
    bool g_started = false;

    // blur-nx (2026-11-14): the guest waits in each dump, and the quiet and on-demand triggers (switch_stall_watch.h).
    constexpr size_t MAX_WAITS = 256;
    constexpr uint64_t QUIET_REDUMP_NS = 30'000'000'000ull; // the second quiet dump, if reads have not resumed
    constexpr int MAX_QUIET_DUMPS = 2;
    constexpr int TRIGGER_POLLS = 4;                        // the stall_dump file is looked for once a second
    bool g_waits = false;
    uint64_t g_quietTicks = 0;                              // 0: no quiet trigger
    std::string g_triggerPath;                              // <log folder>stall_dump
    rex::system::waitreg::WaitView g_waitViews[MAX_WAITS];

    uint64_t g_base = 0;
    uint64_t g_size = 0;

    bool InModule(uint64_t address)
    {
        return address >= g_base && address - g_base < g_size;
    }

    bool IsReturnAddress(uint64_t address)
    {
        if ((address & 3) != 0 || !InModule(address) || address - g_base < 4)
            return false;

        uint32_t previous;
        memcpy(&previous, reinterpret_cast<const void*>(address - 4), sizeof(previous));
        return (previous & 0xFC000000u) == 0x94000000u || (previous & 0xFFFFFC1Fu) == 0xD63F0000u;
    }

    // One thread's state, captured while it was paused.
    struct Capture
    {
        char name[48];
        uint32_t handle;
        bool paused;
        bool contextRead;
        uint64_t ticks;
        ThreadContext context;
        uint64_t stackBase;   // target address of stack[0]
        size_t stackBytes;
    };

    Capture g_captures[MAX_THREADS];
    uint8_t g_stacks[MAX_THREADS][STACK_COPY_BYTES];
    os::switch_cpu_profiler::ThreadInfo g_threads[MAX_THREADS];

    struct TicksSeen
    {
        uint32_t handle;
        uint64_t ticks;
    };
    TicksSeen g_ticksSeen[MAX_THREADS];

    uint64_t ThreadTicks(uint32_t handle)
    {
        uint64_t ticks = 0;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, UINT64_MAX)))
            return 0;
        return ticks;
    }

    uint64_t PreviousTicks(uint32_t handle, uint64_t ticks)
    {
        for (TicksSeen& seen : g_ticksSeen)
        {
            if (seen.handle == handle)
            {
                const uint64_t previous = seen.ticks;
                seen.ticks = ticks;
                return previous;
            }
        }
        for (TicksSeen& seen : g_ticksSeen)
        {
            if (seen.handle == 0)
            {
                seen.handle = handle;
                seen.ticks = ticks;
                break;
            }
        }
        return ticks;
    }

    bool StackRead(const Capture& capture, const uint8_t* stack, uint64_t address, uint64_t& value)
    {
        if ((address & 7) != 0 || address < capture.stackBase || address + 8 > capture.stackBase + capture.stackBytes)
            return false;
        memcpy(&value, stack + (address - capture.stackBase), sizeof(value));
        return true;
    }

    void CaptureThreads(size_t count)
    {
        const uint32_t self = threadGetCurHandle();
        for (size_t i = 0; i < count; i++)
        {
            Capture& capture = g_captures[i];
            capture = {};
            memcpy(capture.name, g_threads[i].name, sizeof(capture.name));
            capture.handle = g_threads[i].handle;
            capture.ticks = ThreadTicks(capture.handle);
            if (capture.handle == 0 || capture.handle == self)
                continue;

            // Nothing between pause and resume may allocate, lock or log.
            if (R_FAILED(svcSetThreadActivity(capture.handle, ThreadActivity_Paused)))
                continue;

            capture.paused = true;
            if (R_SUCCEEDED(svcGetThreadContext3(&capture.context, capture.handle)))
            {
                capture.contextRead = true;

                MemoryInfo info{};
                u32 pageInfo = 0;
                const uint64_t sp = capture.context.sp & ~uint64_t(7);
                if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, sp)) && info.type != MemType_Unmapped &&
                    info.type != MemType_Io && (info.perm & Perm_R) != 0)
                {
                    const uint64_t end = std::min<uint64_t>(info.addr + info.size, sp + STACK_COPY_BYTES);
                    if (end > sp)
                    {
                        memcpy(g_stacks[i], reinterpret_cast<const void*>(sp), size_t(end - sp));
                        capture.stackBase = sp;
                        capture.stackBytes = size_t(end - sp);
                    }
                }
            }

            svcSetThreadActivity(capture.handle, ThreadActivity_Runnable);
        }
    }

    void AppendOffset(std::string& out, uint64_t address)
    {
        char text[32];
        if (InModule(address))
            snprintf(text, sizeof(text), " +0x%07llx", (unsigned long long)(address - g_base));
        else
            snprintf(text, sizeof(text), " 0x%llx", (unsigned long long)address);
        out += text;
    }

    const char* TypeName(uint32_t type)
    {
        static const char* const names[] = { "Undefined", "Enumerator", "Event", "File", "IOCompletion", "Module", "Mutant",
            "NotifyListener", "Semaphore", "Session", "Socket", "SymbolicLink", "Thread", "Timer" };
        return type < sizeof(names) / sizeof(names[0]) ? names[type] : "object";
    }

    // blur-nx (2026-11-14): every guest thread blocked in a wait right now (rex/system/wait_registry.h), the longest
    // first, named from the threads just captured.
    void AppendWaits(std::string& out, size_t threadCount, double sinceReadS)
    {
        namespace wr = rex::system::waitreg;
        const uint64_t nowNs = wr::NowNs();
        const size_t n = wr::CopyWaits(g_waitViews, MAX_WAITS);
        std::sort(g_waitViews, g_waitViews + n,
            [](const wr::WaitView& a, const wr::WaitView& b) { return a.since_ns < b.since_ns; });

        char line[384];
        snprintf(line, sizeof(line), "[stall]   guest waits blocked now: %zu; file reads so far %llu (%.1f MB), the last %.1f s ago\n",
            n, (unsigned long long)wr::FileReads(), double(wr::FileReadBytes()) / 1e6, sinceReadS);
        out += line;
        for (size_t i = 0; i < n; i++)
        {
            const wr::WaitView& w = g_waitViews[i];
            char who[64];
            snprintf(who, sizeof(who), "thread 0x%x", w.host_thread);
            for (size_t t = 0; t < threadCount; t++)
            {
                if (g_captures[t].handle == w.host_thread && w.host_thread != 0)
                {
                    snprintf(who, sizeof(who), "%.47s", g_captures[t].name);
                    break;
                }
            }
            char what[96];
            if (w.kind == wr::kWaitAny || w.kind == wr::kWaitAll)
                snprintf(what, sizeof(what), "%s of %u (the first: %s 0x%08x)", w.kind == wr::kWaitAny ? "any" : "all",
                    w.count, TypeName(w.object_type), w.handle);
            else
                snprintf(what, sizeof(what), "%s0x%08x (%s)", w.kind == wr::kSignalAndWait ? "(signal and wait) " : "",
                    w.handle, TypeName(w.object_type));
            char timeout[32];
            if (w.timeout_ms < 0)
                snprintf(timeout, sizeof(timeout), "no timeout");
            else
                snprintf(timeout, sizeof(timeout), "timeout %lld ms", (long long)w.timeout_ms);
            const double forS = nowNs > w.since_ns ? double(nowNs - w.since_ns) / 1e9 : 0.0;
            snprintf(line, sizeof(line), "[stall]     %s waits on %s for %.1f s, %s", who, what, forS, timeout);
            out += line;
            if (w.guest[0])  // port 167 (from fnxf_xiii): who in the game waits
            {
                snprintf(line, sizeof(line), "; guest LR 0x%08x, callers 0x%08x 0x%08x 0x%08x 0x%08x", w.guest[0],
                    w.guest[1], w.guest[2], w.guest[3], w.guest[4]);
                out += line;
            }

            const wr::SignalView s = wr::FindSignal(w.handle);
            if (!s.seen)
                out += "; never set or released (since the watchdog started)";
            else
            {
                if (s.sets != 0)
                {
                    const bool after = s.last_set_ns > w.since_ns;
                    snprintf(line, sizeof(line), "; set or released %u times, the last %.1f s ago, %s", s.sets,
                        nowNs > s.last_set_ns ? double(nowNs - s.last_set_ns) / 1e9 : 0.0,
                        after ? "AFTER the wait began (missed, or another waiter took it)" : "before the wait began");
                    out += line;
                }
                else
                    out += "; never set or released";
                if (s.resets != 0)
                {
                    snprintf(line, sizeof(line), "; reset %u times, the last %.1f s ago", s.resets,
                        nowNs > s.last_reset_ns ? double(nowNs - s.last_reset_ns) / 1e9 : 0.0);
                    out += line;
                }
            }
            out += '\n';
        }
    }

    // header: what triggered the dump ("#3 dump 1: no frame for 2.0 s ...").
    void Dump(const char* header, double sinceReadS)
    {
        const size_t count = os::switch_cpu_profiler::CopyThreads(g_threads, MAX_THREADS);
        CaptureThreads(count);

        // Everything is running again from here on.
        std::string out;
        out.reserve(16384);
        char line[384];
        snprintf(line, sizeof(line), "[stall] %s; module base 0x%llx; run tools/switch-cpu-profile.py on this log to name the "
            "offsets\n", header, (unsigned long long)g_base);
        out += line;

        if (g_reporter != nullptr)
        {
            out += "[stall]   renderer: ";
            g_reporter(out);
            out += '\n';
        }

        for (size_t i = 0; i < count; i++)
        {
            const Capture& capture = g_captures[i];
            const uint64_t previous = PreviousTicks(capture.handle, capture.ticks);
            const double ranMs = double(armTicksToNs(capture.ticks - previous)) / 1e6;

            snprintf(line, sizeof(line), "[stall]   %s: %s, %.1f ms of CPU since the last look", capture.name,
                !capture.paused ? "not paused" : !capture.contextRead ? "no context" : "paused", ranMs);
            out += line;

            if (capture.contextRead)
            {
                // At an SVC instruction (Horizon reports a thread blocked in a system call there; round 9: only
                // right after it was checked) or right after one: in that system call (0x0B sleep, 0x18 wait
                // synchronization, 0x1A arbitrate lock, 0x1C condition variable, 0x21 IPC, 0x34 wait for address).
                const uint64_t pc = capture.context.pc.x;
                if (InModule(pc) && (pc & 3) == 0)
                {
                    uint32_t at;
                    uint32_t previous = 0;
                    memcpy(&at, reinterpret_cast<const void*>(pc), sizeof(at));
                    if (pc - g_base >= 4)
                        memcpy(&previous, reinterpret_cast<const void*>(pc - 4), sizeof(previous));

                    const uint32_t svc = (at & 0xFFE0001Fu) == 0xD4000001u ? at : previous;
                    if ((svc & 0xFFE0001Fu) == 0xD4000001u)
                    {
                        snprintf(line, sizeof(line), "; in svc 0x%02x", (svc >> 5) & 0xFFFF);
                        out += line;
                    }
                }

                out += "; pc";
                AppendOffset(out, capture.context.pc.x);
                out += " lr";
                AppendOffset(out, capture.context.lr);

                // Frame-pointer chain (the port's code keeps frame pointers; recompiled game code does not).
                out += "; frames";
                uint64_t fp = capture.context.fp;
                for (int depth = 0; depth < MAX_FRAMES; depth++)
                {
                    uint64_t nextFp, returnAddress;
                    if (!StackRead(capture, g_stacks[i], fp, nextFp) || !StackRead(capture, g_stacks[i], fp + 8, returnAddress) ||
                        returnAddress == 0)
                    {
                        break;
                    }
                    AppendOffset(out, returnAddress);
                    if (nextFp <= fp)
                        break;
                    fp = nextFp;
                }

                // Every return address on the stack, which also covers the recompiled code.
                out += "; stack";
                int found = 0;
                for (size_t offset = 0; offset + 8 <= capture.stackBytes && found < MAX_STACK_ADDRESSES; offset += 8)
                {
                    uint64_t value;
                    memcpy(&value, g_stacks[i] + offset, sizeof(value));
                    if (IsReturnAddress(value))
                    {
                        AppendOffset(out, value);
                        found++;
                    }
                }
            }
            out += '\n';
        }

        if (g_waits)
            AppendWaits(out, count, sinceReadS);

        os::switch_cpu_profiler::WriteLog(out);
    }

    void WatchThread(void*)
    {
        int episode = 0;
        bool stalled = false;
        int dumps = 0;
        uint64_t lastDump = 0;
        uint64_t stallStart = 0;
        uint64_t lastHitchReport = 0;
        // blur-nx (2026-11-14): the quiet and on-demand triggers (switch_stall_watch.h).
        uint64_t lastReads = rex::system::waitreg::FileReads();
        uint64_t lastReadTick = armGetSystemTick();
        int quietEpisode = 0;
        int quietDumps = 0;
        uint64_t lastQuietDump = 0;
        int polls = 0;
        char header[256];

        while (true)
        {
            svcSleepThread(POLL_NS);

            const uint64_t lastFrame = g_lastFrameTick.load(std::memory_order_acquire);
            const uint64_t now = armGetSystemTick();
            const unsigned long long frames = (unsigned long long)g_frames.load(std::memory_order_relaxed);
            const double sinceFrameS = lastFrame != 0 ? double(armTicksToNs(now - lastFrame)) / 1e9 : -1.0;

            const uint64_t reads = rex::system::waitreg::FileReads();
            if (reads != lastReads)
            {
                if (quietDumps != 0)
                {
                    char line[160];
                    snprintf(line, sizeof(line), "[stall] quiet #%d ended: file reads resumed after %.1f s without one\n",
                        quietEpisode, double(armTicksToNs(now - lastReadTick)) / 1e9);
                    os::switch_cpu_profiler::WriteLog(line, strlen(line));
                }
                lastReads = reads;
                lastReadTick = now;
                quietDumps = 0;
            }
            const double sinceReadS = double(armTicksToNs(now - lastReadTick)) / 1e9;

            // On demand: a file named stall_dump in the log folder (deleted first, so one file is one dump).
            if (!g_triggerPath.empty() && ++polls >= TRIGGER_POLLS)
            {
                polls = 0;
                struct stat st;
                if (stat(g_triggerPath.c_str(), &st) == 0)
                {
                    unlink(g_triggerPath.c_str());
                    if (lastFrame != 0)
                        snprintf(header, sizeof(header), "on-demand dump (stall_dump): the last frame %.1f s ago (%llu frames so far)",
                            sinceFrameS, frames);
                    else
                        snprintf(header, sizeof(header), "on-demand dump (stall_dump): no frame yet");
                    Dump(header, sinceReadS);
                    // Port 167 (from fnxf_xiii): every guest thread's guest call chain too (sdk xthread.cpp; the file
                    // trigger only, no controller combination).
                    static char chains[16384];
                    const size_t len = RexGuestCallChains(chains, sizeof(chains));
                    if (len)
                    {
                        const char* head = "[stall] guest call chains (r1, then each frame's saved lr):\n";
                        os::switch_cpu_profiler::WriteLog(head, strlen(head));
                        os::switch_cpu_profiler::WriteLog(chains, len);
                    }
                }
            }

            // Quiet: no file read for a while, frames or not (a frame stall dumps on its own).
            if (g_quietTicks != 0 && !stalled && now - lastReadTick >= g_quietTicks && quietDumps < MAX_QUIET_DUMPS &&
                (quietDumps == 0 || now - lastQuietDump >= armNsToTicks(QUIET_REDUMP_NS)))
            {
                if (quietDumps == 0)
                    quietEpisode++;
                quietDumps++;
                if (lastFrame != 0)
                    snprintf(header, sizeof(header), "quiet #%d dump %d: no file read for %.1f s, the last frame %.1f s ago (%llu "
                        "frames so far)", quietEpisode, quietDumps, sinceReadS, sinceFrameS, frames);
                else
                    snprintf(header, sizeof(header), "quiet #%d dump %d: no file read for %.1f s, no frame yet", quietEpisode,
                        quietDumps, sinceReadS);
                Dump(header, sinceReadS);
                lastQuietDump = armGetSystemTick();
            }

            if (lastFrame == 0)
                continue; // No frame yet (boot): the frame triggers wait for the first.

            // Frames that took over 100 ms but not long enough for a stall: the stutters. (A stall counts too,
            // once it ends.)
            if (now - lastHitchReport >= armNsToTicks(HITCH_REPORT_NS))
            {
                const uint32_t hitches = g_hitches.exchange(0, std::memory_order_relaxed);
                if (hitches != 0)
                {
                    const uint64_t longest = g_longestHitchTicks.exchange(0, std::memory_order_relaxed);
                    char line[192];
                    snprintf(line, sizeof(line), "[hitch] %u frame%s over %llu ms since the last report, the longest %.0f ms "
                        "(%llu frames so far)\n", hitches, hitches == 1 ? "" : "s", (unsigned long long)(HITCH_NS / 1'000'000ull),
                        double(armTicksToNs(longest)) / 1e6, (unsigned long long)g_frames.load(std::memory_order_relaxed));
                    os::switch_cpu_profiler::WriteLog(line, strlen(line));
                }
                lastHitchReport = now;
            }
            if (now - lastFrame >= g_thresholdTicks)
            {
                if (!stalled)
                {
                    stalled = true;
                    episode++;
                    dumps = 0;
                    stallStart = lastFrame;
                }

                if (dumps < MAX_DUMPS_PER_STALL && (dumps == 0 || now - lastDump >= armNsToTicks(REDUMP_NS)))
                {
                    snprintf(header, sizeof(header), "#%d dump %d: no frame for %.1f s (%llu frames so far)", episode, ++dumps,
                        sinceFrameS, frames);
                    Dump(header, sinceReadS);
                    lastDump = armGetSystemTick();
                }
            }
            else if (stalled)
            {
                stalled = false;
                char line[160];
                snprintf(line, sizeof(line), "[stall] #%d ended: frames resumed after %.1f s without one\n", episode,
                    double(armTicksToNs(lastFrame - stallStart)) / 1e9);
                os::switch_cpu_profiler::WriteLog(line, strlen(line));
            }
        }
    }
}

extern "C" const char* RexSwitchLogDir(void);

void os::switch_stall_watch::Start(double seconds, double quiet_seconds, bool waits)
{
    if (g_started || seconds <= 0.0)
        return;

    g_base = os::switch_cpu_profiler::ModuleBase();
    g_size = os::switch_cpu_profiler::ModuleSize();
    g_thresholdTicks = armNsToTicks(uint64_t(seconds * 1e9));
    g_hitchTicks = armNsToTicks(HITCH_NS);

    // blur-nx (2026-11-14): the guest waits and the file reads are noted from here on (rex/system/wait_registry.h).
    g_waits = waits;
    g_quietTicks = quiet_seconds > 0.0 ? armNsToTicks(uint64_t(quiet_seconds * 1e9)) : 0;
    rex::system::waitreg::g_on.store(waits, std::memory_order_relaxed);
    rex::system::waitreg::g_reads_on.store(g_quietTicks != 0 || waits, std::memory_order_relaxed);
    if (const char* dir = RexSwitchLogDir(); dir != nullptr && dir[0] != '\0')
    {
        g_triggerPath = dir;
        g_triggerPath += "stall_dump";
        unlink(g_triggerPath.c_str()); // one left from an earlier run is not a request
    }

    // Above the game's threads (0x1C-0x3B), like the CPU sampler, so a busy thread cannot hide the stall; it
    // sleeps between looks.
    if (R_SUCCEEDED(threadCreate(&g_thread, WatchThread, nullptr, nullptr, 0x10000, 0x1E, -2)))
    {
        os::switch_cpu_profiler::UnregisterThread(g_thread.handle); // banjo-nx: registered by the threadCreate wrapper
        svcSetThreadCoreMask(g_thread.handle, -1, 0x7);
        if (R_SUCCEEDED(threadStart(&g_thread)))
        {
            g_started = true;
            os::switch_cpu_profiler::WriteLogF("Stall watchdog: thread dumps after %.1f s without a frame, frames over %llu ms counted\n",
                seconds, (unsigned long long)(HITCH_NS / 1'000'000ull));
            os::switch_cpu_profiler::WriteLogF("Stall watchdog: guest waits in the dumps %s; quiet dumps after %.0f s without a file "
                "read%s; on-demand dump: a file named %s\n", waits ? "on" : "off", quiet_seconds,
                g_quietTicks != 0 ? "" : " (off)", g_triggerPath.empty() ? "(no log folder: off)" : g_triggerPath.c_str());
        }
    }
}

void os::switch_stall_watch::OnFrame()
{
    if (!g_started)
    {
        g_frames.store(g_frames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
    }

    const uint64_t now = armGetSystemTick();
    const uint64_t last = g_lastFrameTick.load(std::memory_order_relaxed);
    if (last != 0 && now - last >= g_hitchTicks)
    {
        const uint64_t gap = now - last;
        g_hitches.fetch_add(1, std::memory_order_relaxed);
        uint64_t longest = g_longestHitchTicks.load(std::memory_order_relaxed);
        while (gap > longest && !g_longestHitchTicks.compare_exchange_weak(longest, gap, std::memory_order_relaxed))
            ;
    }

    // Only this thread writes these two.
    g_frames.store(g_frames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    g_lastFrameTick.store(now, std::memory_order_release);
}

uint64_t os::switch_stall_watch::FrameCount()
{
    return g_frames.load(std::memory_order_relaxed);
}

void os::switch_stall_watch::SetStateReporter(void (*reporter)(std::string& out))
{
    g_reporter = reporter;
}
