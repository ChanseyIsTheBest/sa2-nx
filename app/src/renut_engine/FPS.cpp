#include <rex/logging.h>
#include "Fps.h"
#include <renut_engine/hooks.h>
#include <renut_engine/Timer.h>
#include <rex/hook.h>
#include "profiler/profiler.h"

namespace banjo::harvest { void Fotograma(); }  // native/banjo_shader_harvest.cpp
namespace banjo::renderdoc { void GameFrame(); }  // me/banjo_renderdoc.cpp (banjo_renderdoc_game_s)

// katamari-nx (2026-10-30): appMainTickPreDraw / appMainDraw are N&B's named functions; Beautiful Katamari has no such
// names (its frame boundary is the Swap, docs/katamari-notes.md). Kept for reference, not built.
#if KATAMARI_BANJO_ADDRESS_HOOKS
//CPU Time
REX_EXTERN(__imp__appMainTickPreDraw);
REX_HOOK_RAW(appMainTickPreDraw){
    Timer timer;
    timer.start();
    __imp__appMainTickPreDraw(ctx, base);
    timer.stop();
    cpuMS = timer.elapsedMilliseconds();
    banjo::profiler::RecordTick(cpuMS);
    auto fpshook = fpsManager.GetCreateCounter("Tick");
    fpshook->Tick();
}

//GPU Time
void renutFrameLimit();             // frameHooks.cpp: our low-overhead frame cap ("vsync")
void renutApplyShaderCompileMode(); // frameHooks.cpp: sync-compile toggle (black-flash fix)

REX_EXTERN(__imp__appMainDraw);
REX_HOOK_RAW(appMainDraw){
    static uint32_t drawTicks = 0;
    if (++drawTicks <= 6 || (drawTicks % 300) == 0) {
        REXLOG_INFO("game: appMainDraw tick {}", drawTicks);
    }
    // Keep the engine's shader-compile mode in sync with our toggle (cheap; only
    // touches the engine cvar when the toggle actually changes).
    renutApplyShaderCompileMode();

    Timer timer;
    timer.start();
    __imp__appMainDraw(ctx, base);
    timer.stop();
    gpuMS = timer.elapsedMilliseconds();
    banjo::profiler::RecordDraw(gpuMS);

    // Pace the frame after the draw is submitted so the GPU renders fewer frames
    // instead of running uncapped. No-op while frame_cap is "Off".
    renutFrameLimit();

    // One game frame ends here (frame time, Switch CPU profiler frame marks, stall watchdog).
    banjo::profiler::FrameBoundary();
    banjo::harvest::Fotograma();  // shader harvesting fallback scan (banjo_volcar_shaders_en_sitio)
    banjo::renderdoc::GameFrame();  // PC diagnostic capture by time (banjo_renderdoc_game_s)
}
#endif  // KATAMARI_BANJO_ADDRESS_HOOKS

void FPSCounter::Tick(){
    auto Time = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> delta = Time - lastTick;
    lastTick = Time;
    float ms = static_cast<float>(delta.count());
    frameTimes.push_back(ms);
    if (frameTimes.size() > AverageCount) {
        frameTimes.erase(frameTimes.begin());
    }
    float total = 0.0f;
    for (float f : frameTimes) {
        total += f;
    }
    averageMs = total / frameTimes.size();
    averageFps = 1000.0f / averageMs;
}

