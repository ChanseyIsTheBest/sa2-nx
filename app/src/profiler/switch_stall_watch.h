#pragma once

#if defined(__SWITCH__)

#include <string>

// [Switch] Stall watchdog ([Switch] SwitchStallWatchSeconds). A thread of its own notices when no frame has
// been presented for that long and writes to stderr.log, as "[stall]" lines, what every registered thread
// (os::switch_cpu_profiler) is doing: whether it ran since the last look, where it is (program counter,
// return address, frame-pointer chain and the return addresses on its stack, as offsets into the executable
// that tools/switch-cpu-profile.py names), plus the renderer's queues. It looks again every few seconds while
// the stall lasts and writes its length once frames resume. Shorter stutters are counted: a "[hitch]" line
// sums up, at most once a second, the frames that took over 100 ms. Idle otherwise: one check every 250 ms.
//
// blur-nx (2026-11-14, the boot black screen: frames keep coming while the loader threads wait): with waits on
// (banjo_stall_watch_waits), each dump also lists every guest thread blocked in a wait right now, the object
// (type, handle) and for how long, and whether that object was set or released after the wait began or not since
// (rex/system/wait_registry.h). Two more triggers dump the same way while frames still come: "quiet" (no file read
// for quiet_seconds, banjo_stall_watch_quiet_seconds; two dumps at most until reads resume) and on demand (a file
// named stall_dump put in logs/rex/, for instance over FTP: the watchdog deletes it and dumps).
namespace os::switch_stall_watch
{
    void Start(double seconds, double quiet_seconds, bool waits);

    // Called once per presented frame (the game's main thread).
    void OnFrame();

    // Frames presented so far (counted with the watchdog off too).
    uint64_t FrameCount();

    // Extra state printed with each dump. Called on the watchdog thread; must not block or allocate while
    // other threads could be paused (it is only called once they run again).
    void SetStateReporter(void (*reporter)(std::string& out));
}

#endif
