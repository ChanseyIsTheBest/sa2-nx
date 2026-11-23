/**
 * @file    gpu/host_batch.h
 * @brief   blur-nx (2026-11-18, the Renut host batch: docs/renut-host-ideas-25-1118.md #1-#24, #26-#35): the batch's
 *          switches (one key each, and each an in-run A/B target from kAbBase on) and its census
 *          (blur_renut_diag_batch_census, off by default: the counts below, one 10 s line).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "renut_engine/gpu/banjo_renut_tuning.h"

namespace renut::gpu::host_batch {

// The switches. A/B target kAbBase + the item (banjo_renut_ab_target / banjo_renut_ab_targets) turns one off for a half.
enum Item : uint32_t {
  kSkipUnused,       // #1, #2: blur_renut_record_skip_unused (NewOp builds no resource objects or slot extents while
                     //   replays inherit: the replay uses neither then)
  kInheritByChunk,   // #3: blur_renut_inherit_by_chunk (an op's inherited words put back only in the chunks it applied)
  kWholeRepeat,      // #4: blur_renut_whole_repeat (the draw before's whole-buffer answer for the same streams)
  kCleanerPriority,  // #11: blur_renut_ring_cleaner_priority (the ring cleaner above the game threads)
  kInheritWords,     // #13: blur_renut_inherit_runs_words (BuildInheritRuns a 32-bit word at a time)
  kPipelineTwoWay,   // #16: blur_renut_fast_pipeline_two_way
  kRemoved17,        // #17 (the two-way texture memo): removed 2026-11-18 (ghosting on the console, a saving
                     //   of ~0.1 ms a frame at most); its A/B target 236 kept free so the others keep theirs
  kProvisionalKey,   // #18: blur_renut_provisional_key_kept (a provisional answer looked up again with its kept key)
  kWakeBatchAll,     // #28: blur_renut_wake_batch_all (runs and resolves wake the render thread as draws do)
  kViewportMemo,     // #32: blur_renut_viewport_memo (eight viewports kept, not one)
  kMeshGenWhole,     // #36 (found on the way): blur_renut_mesh_gen_whole_only (the mesh objects' generation raised only
                     //   when a whole copy is dropped)
  kItemCount
};
constexpr int kAbBase = 230;

struct Switches {
  bool on[kItemCount] = {};
};
extern Switches g_switches;
extern std::atomic<bool> g_ready;
[[gnu::cold, gnu::noinline]] void LatchSwitches();

[[gnu::always_inline]] inline bool On(Item i) {
  if (!g_ready.load(std::memory_order_relaxed)) [[unlikely]]
    LatchSwitches();
  return g_switches.on[i] && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != kAbBase + int(i);
}
// The key alone (for what is decided once, or read where the A/B half cannot change it).
[[gnu::always_inline]] inline bool KeyOn(Item i) {
  if (!g_ready.load(std::memory_order_relaxed)) [[unlikely]]
    LatchSwitches();
  return g_switches.on[i];
}

// #11: blur_renut_ring_cleaner_priority's value (0: the background priority).
int CleanerPriority();
// The calling thread's priority (the Switch; nothing elsewhere).
void SetThisThreadPriority(int priority);
// The background threads' priority (OnBackgroundThreadStart's).
int BackgroundPriority();

// The census (blur_renut_diag_batch_census).
enum Count : uint32_t {
  kRestoreOps,          // #3: ops whose inherited words were put back
  kRestoreBytesAll,     // #3: the bytes every inherited run is (the old way)
  kRestoreBytesChunk,   // #3: the bytes in the applied chunks' runs (the new way)
  kWholeTries,          // #4: WholeGeometryTry calls past its first tests
  kWholeRepeats,        // #4: answered by the draw before's answer
  kTextureSetCalls,     // #5: FillTextures calls with a mask
  kTextureSetSame,      // #5: the same slots and words as the call before
  kTextureSetSeen,      // #5: as one of the 16 calls before (not the last)
  kResolveCalls,        // #6: ResolveTargetsLocked calls
  kResolveTicksDest,    // #6: ticks finding the destination (Get*ResolveTextureLocked)
  kResolveTicksWork,    // #6: ticks in the resolve's own work (hand-over, the draw or copy)
  kUploadCalls,         // #7: texture uploads
  kUploadBytes,         // #7: their bytes (the destination's)
  kFenceWaits,          // #8: the game thread's fence waits
  kFenceWaitTicks,      // #8: their time
  kFenceWaitsKicked,    // #8: waits that found the fence's work not yet submitted (a kick was made for them)
  kConvertWholeBytes,   // #12: ConvertVertices bytes for whole copies
  kConvertRangeBytes,   // #12: for range copies (PrepareGeometry)
  kConvertUpBytes,      // #12: for Draw*UP and Begin*Vertices data
  kFastDraws,           // #19: fast-path draws
  kFastDrawsNoFields,   // #19: of them, with no field changed (the constants and the call alone)
  kOpGrowths,           // #20: op vectors that grew at an op
  kOps,                 // #20, #21: NewOp's ops
  kOpsStackHeaders,     // #21: ops that captured stack surface headers
  kWakesDraw,           // #28: render-thread wakes by a draw push
  kWakesOther,          // #28: by any other push
  kWakeTicks,           // #28: their time
  kRecordLockWaits,     // #28: QueueRunCommandBuffer's record lock taken after a wait
  kRecordLockTicks,     // #28: the time to take it
  kImmediateDraws,      // #29: QueueDraw calls
  kImmediateTicks,      // #29: their time
  kSetterCalls,         // #35: calls of the native setters counted (the render states, viewport, surfaces)
  kCountCount
};
extern std::atomic<uint64_t> g_count[kCountCount];
#if defined(RENUT_DIAG_CENSUS_OFF)
// banjo-nx (2026-11-20, BANJO_SWITCH_DIAG_CENSUS OFF, the Switch's default): no census in this build (see
// stage_census.h); blur_renut_diag_batch_census does nothing here.
constexpr bool CensusOn() { return false; }
#else
bool CensusOn();
#endif
// blur_renut_host_batch_verify (off): #3, #13 and #32 checked against the old way, differences logged.
bool VerifyOn();
void VerifyDiffer(const char *what);
// One recording thread a count, or a lost increment between two (a census): a load and a store, no read-modify-write
// loop (the A57 has no LSE atomics).
[[gnu::always_inline]] inline void Add(Count c, uint64_t v = 1) {
  g_count[c].store(g_count[c].load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
uint64_t Ticks();  // the system counter (armGetSystemTick on the Switch)
// #7: a texture upload's identity, for the census's list of the most uploaded.
void NoteUpload(uint32_t guest_va, uint32_t width, uint32_t height, uint32_t format, uint64_t bytes);
// The 10 s line, empty with the key off; resets the counts.
std::string Report(double seconds);

}  // namespace renut::gpu::host_batch
