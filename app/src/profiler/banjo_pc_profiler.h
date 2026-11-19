#pragma once

// PC sampling profiler (cvar banjo_pc_profile_s); see banjo_pc_profiler.cpp. Does nothing on the Switch, where the
// SDK's sampler (sdk/src/ui/switch_perf.cpp) does the same job.
namespace banjo::pc_profiler {

void Start();
void Stop();

}  // namespace banjo::pc_profiler
