#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/hook.h>
#include <rex/cvar.h>
#include "globals.h"
#include "profiler/profiler.h"
#include "renut_engine/display_sync.h"  // banjo-nx (2026-10-19, latency batch 3)

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

REXCVAR_DEFINE_INT32(target_refreshRate, -1, "Nuts&Bolts/Performance", "The index for a given VSync setting. Options are: \n\t-1 = Unaffected\n\t0 = Immediate (Uncapped)\n\t1 = 60Hz\n\t2 = 30Hz (No Threshold)\n\t3 = 20Hz\n\t4 - 9 = Variants of 30Hz.").range(-1, 9);

// Typed import of the original generated implementation at sub_823bbe58.
REX_IMPORT(__imp__rex_setParamInterval, setParamInterval, void(int));

static std::once_flag g_refreshRateCallbackSet_once;
bool isInKlungoGame = false;

// TODO: Implement a callback so that we can set the vsync value and have it auto change.
void refreshRate_hook(PPCRegister& r3) // Called at 823bbe58 (Start of the function)
{
	bool overrideTarget = false;
	
	// If the value is -1, don't overwrite the R3 register.
	if(REXCVAR_GET(target_refreshRate) == -1){
		overrideTarget = true;
	}
	
	if(isInKlungoGame && !overrideTarget){
		r3.u32 = 7;
		overrideTarget = true;
	}
	
	if(r3.u32 == 0 && !overrideTarget) {
		overrideTarget = true;
	}
	
	if(!overrideTarget) r3.u32 = REXCVAR_GET(target_refreshRate); // Back to normal
	
	currentSyncVal = r3.u32;
}

void klungoConstruct_hook() // Called at 8244a7a0 (End of the function)
{
	isInKlungoGame = true;
	setParamInterval(2);
}

void klungoDestruct_hook() // Called at 8244b744 (End of the function)
{
	isInKlungoGame = false;
	setParamInterval(REXCVAR_GET(target_refreshRate));
}

// banjo-nx (2026-10-29, title update 3, from reNut upstream ab60251): Klungo's arcade 2 (the DLC) is its own game
// object (sub_823C47E0 builds it, sub_823C68A8 tears it down), held at 30 fps the same way (config/renut_hooks.toml:
// 0x823C519C and 0x823C6B80, the ends of those two).
void klungoDlcConstruct_hook()
{
	klungoConstruct_hook();
}

void klungoDlcDestruct_hook()
{
	klungoDestruct_hook();
}

// =============================================================================
// Frame limiter -- our own low-overhead "vsync".
//
// The old target_refreshRate path only changes the guest's D3D presentation
// interval; it doesn't stop the host from rendering as fast as it can, so the
// GPU stays pinned. This instead paces the main loop itself: once per frame
// (from appMainDraw, the second half of Main_821FB7D8's loop) we sleep until the
// next frame deadline, so the GPU simply renders fewer frames.
//
// The wait is done with a high-resolution waitable timer (near-zero CPU while
// blocked) plus a sub-millisecond spin tail for pacing accuracy, so capping adds
// almost no load of its own. It is deliberately named separately from rexglue's
// GPU `vsync` cvar and does not touch the SDK.
// =============================================================================
#if defined(_WIN32)
#  ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#  endif

#endif

REXCVAR_DEFINE_STRING(frame_cap, "Off", "Nuts&Bolts/Performance",
	"Frame-rate cap (low-overhead limiter that cuts GPU load). "
	"Off = uncapped, Display = match monitor refresh.")
	.allowed({ "Off", "30", "60", "120", "144", "Display" });

#if defined(_WIN32)
// One process-wide timer, created on first use. Falls back to a normal waitable
// timer (and then to sleep_for) if the high-resolution flag isn't supported.
static HANDLE renutFrameTimer()
{
	static HANDLE timer = []() -> HANDLE {
		HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
		return h;
	}();
	return timer;
}
#endif

// Resolve the cvar to a target FPS: 0 = uncapped, "Display" = monitor refresh
// (queried once), otherwise the literal number.
static int renutFrameCapFps()
{
	const std::string& v = REXCVAR_GET(frame_cap);
	if (v.empty() || v == "Off") return 0;
	if (v == "Display") {
#if defined(_WIN32)
		static int hz = []() -> int {
			DEVMODEW dm{}; dm.dmSize = sizeof(dm);
			if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
				dm.dmDisplayFrequency > 1)
				return static_cast<int>(dm.dmDisplayFrequency);
			return 60;
		}();
		return hz;
#else
		return 60;  // the Switch panel and docked output both run at 60 Hz
#endif
	}
	return std::atoi(v.c_str());
}

void renutFrameLimit()
{
	using clock = std::chrono::steady_clock;
	static clock::time_point next = clock::now();
	// banjo-nx (2026-10-19, latency batch 3): the display's refresh, followed from the first frame on (the Switch).
	banjo::display_sync::Start();

	const int fps = renutFrameCapFps();
	if (fps <= 0) {
		next = clock::now();  // uncapped: keep the baseline current for a clean re-enable
		banjo::profiler::RecordLimiterWait(0.0);
		return;
	}
	const clock::time_point waitStart = clock::now();
	struct WaitRecorder {
		clock::time_point start;
		~WaitRecorder() {
			banjo::profiler::RecordLimiterWait(
				std::chrono::duration<double, std::milli>(clock::now() - start).count());
		}
	} waitRecorder{waitStart};

	const auto frameDur = std::chrono::duration_cast<clock::duration>(
		std::chrono::duration<double>(1.0 / fps));
	clock::time_point now = clock::now();

	// banjo-nx (2026-10-19, latency batch 3, banjo_display_sync): with the display's refresh known, the release is a
	// refresh plus the measured phase (renut_engine/display_sync.h); a late frame goes at once, as below.
	static uint64_t previousRelease = 0;
	{
		const auto toNs = [](clock::time_point t) {
			return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
		};
		bool anchored = false;
		const uint64_t release = banjo::display_sync::NextRelease(
			toNs(now), previousRelease,
			uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(frameDur).count()), &anchored);
		if (release) {
			previousRelease = release;
			next = clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(release)));
			if (next > now) {
				constexpr auto kTail = std::chrono::microseconds(400);
				for (;;) {
					now = clock::now();
					if (now >= next) break;
					if (next - now > kTail) std::this_thread::sleep_for(next - now - kTail);
					else std::this_thread::yield();
				}
			}
			banjo::display_sync::NoteRelease(release, anchored);
			return;
		}
		previousRelease = 0;
	}

	next += frameDur;
	if (next <= now) {
		// A slow frame (or the cap was just raised) put us behind: resync to now
		// rather than bursting to "catch up".
		next = now;
		return;
	}

#if defined(_WIN32)
	HANDLE timer = renutFrameTimer();
#endif
	constexpr auto kSpinTail = std::chrono::microseconds(400);
	for (;;) {
		now = clock::now();
		if (now >= next) break;
		const auto remaining = next - now;
		if (remaining > kSpinTail) {
			const auto waitFor = remaining - kSpinTail;
#if defined(_WIN32)
			if (timer) {
				LARGE_INTEGER due;
				due.QuadPart = -static_cast<LONGLONG>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(waitFor).count() / 100);
				if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
					WaitForSingleObject(timer, INFINITE);
				else
					std::this_thread::sleep_for(waitFor);
			} else {
				std::this_thread::sleep_for(waitFor);
			}
#else
			std::this_thread::sleep_for(waitFor);
#endif
		} else {
			std::this_thread::yield();  // brief sub-ms tail for pacing accuracy
		}
	}
}

// =============================================================================
// Shader compilation mode -- fixes the split-second black flash.
//
// The engine compiles pipelines asynchronously by default (the SDK cvar
// async_shader_compilation). While a newly-seen pipeline is still compiling, its
// draws are SKIPPED for that frame (command_processor: if async is on and the
// pipeline handle isn't ready yet, the draw is dropped). So the first time a new
// effect appears -- a heal-over-time fxScreenColourExport pass, a fresh
// scene-shader permutation it introduces, etc. -- those draws vanish for a frame
// and the screen flashes black, then snaps back once compilation finishes.
//
// Compiling synchronously makes the draw wait for its pipeline instead of being
// dropped, trading the black flash for a small one-time hitch. We keep our own
// toggle and mirror it into the engine cvar (only when it changes) so it also
// appears in the pause menu / persists to renut.toml, without editing the SDK.
// =============================================================================
REXCVAR_DEFINE_BOOL(sync_shader_compile, true, "Nuts&Bolts/Graphics",
	"Compile shaders synchronously to stop the black flash when new effects first "
	"appear (small one-time stutter instead). Off = engine default (async).");

void renutApplyShaderCompileMode()
{
	static int applied = -1;
	const bool sync = REXCVAR_GET(sync_shader_compile);
	if (static_cast<int>(sync) != applied) {
		applied = static_cast<int>(sync);
		// async_shader_compilation is the inverse of our "sync" toggle.
		rex::cvar::SetFlagByName("async_shader_compilation", sync ? "false" : "true");
	}
}
