// banjo-nx: on/off switches for every performance enhancement (see banjo_enhancements.h).

#include "banjo_enhancements.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <cstdint>

#ifndef BANJO_BUILD_FLAVOR
#define BANJO_BUILD_FLAVOR "classic codegen"  // app/CMakeLists.txt sets it from app/generated/codegen_flavor.txt
#endif
#include <iterator>
#include <string>

REXCVAR_DEFINE_BOOL(banjo_cpu_enhancements, true, "Banjo/Enhancements",
                    "Master switch of the CPU enhancements (each also has its own key). false = all of them off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_noncpu_enhancements, true, "Banjo/Enhancements",
                    "Master switch of the non-CPU enhancements: GPU, driver and storage side (each also has its own "
                    "key). false = all of them off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The verify configuration (configs/banjo_cpu_verify.toml): the same CPU enhancements, with every self-check of the
// renderer's fast paths comparing on EVERY use instead of the first masseffect_native_verify_n, and the native game
// functions running their original too. Slower: for a test session. The renderer's 10-second report gets a "[verify]"
// line whose difference count must stay 0.
REXCVAR_DEFINE_BOOL(banjo_cpu_verify, false, "Banjo/Enhancements",
                    "Verify configuration: every CPU enhancement checks itself against the original path on every use "
                    "and the log reports '[verify] ... differences: N' (must be 0). Slower; for testing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(int32_t, masseffect_native_verify_n);
REXCVAR_DECLARE(int32_t, masseffect_native_fast_copy_verify);

// The enhancements' own keys, defined next to the code they switch.
REXCVAR_DECLARE(bool, banjo_wait_blocking_ring);
REXCVAR_DECLARE(bool, banjo_native_reverb);         // src/cpu/banjo_reverb_native.cpp
REXCVAR_DECLARE(bool, banjo_native_reverb_verify);
REXCVAR_DECLARE(bool, banjo_native_xaudio_src);         // src/cpu/banjo_xaudio_native.cpp
REXCVAR_DECLARE(bool, banjo_native_xaudio_src_verify);
REXCVAR_DECLARE(bool, banjo_repack_cached);         // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_registers_in_place);  // src/me/me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_direct_state);             // src/me/me_native_system.cpp (+ banjo_me_hooks.cpp)
REXCVAR_DECLARE(bool, banjo_no_draw_pairing);          // src/me/me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_d3d_native_draw_indexed);  // src/me/banjo_d3d_native.h (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_d3d_native_draw_vertices); // src/me/banjo_d3d_native.h
REXCVAR_DECLARE(bool, banjo_ring_edram_memo);          // src/me/masseffect/masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_ring_constants_dedupe);    // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_note_shader_fast);         // src/me/me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_ring_target_memo);         // src/me/masseffect/masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_ring_wait_vblank);         // src/me/me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_ring_vertex_containment);  // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_vertex_persistent);   // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_vertex_epoch);        // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_constants_adaptive);  // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_shader_load_memo);    // src/me/me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_ring_index_epoch);         // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_identity_once);       // src/me/me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_ring_sampler_fast);        // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_vertex_proven_first); // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_memexport_bounds);    // src/me/masseffect/masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_mesa_disk_cache_off);
REXCVAR_DECLARE(bool, banjo_gpu_zcull_greater);  // src/me/me_native_system.cpp
REXCVAR_DECLARE(bool, masseffect_native_pipelines_prewarm);
REXCVAR_DECLARE(bool, masseffect_nvk_cache_per_stage);   // read by RenutApp::OnPreSetup
REXCVAR_DECLARE(int32_t, banjo_prewarm_threads);
// The native renderer's ring-thread fast paths (masseffect-nx; self-checked against the slow path for the first
// masseffect_native_verify_n uses). Their code reads its key directly, so the CPU master switch turns them off by
// resetting the key (ApplyMasterSwitches).
REXCVAR_DECLARE(bool, masseffect_native_flat_identity);
REXCVAR_DECLARE(bool, masseffect_native_fast_pair);
REXCVAR_DECLARE(bool, masseffect_native_raw_microcode);
REXCVAR_DECLARE(bool, masseffect_native_pm4_fast);
REXCVAR_DECLARE(bool, masseffect_native_compare_fast);
REXCVAR_DECLARE(bool, masseffect_native_fast_copy);
REXCVAR_DECLARE(int32_t, masseffect_native_fingerprint_mode);
REXCVAR_DECLARE(int32_t, masseffect_native_dedupe_max_fingerprint);
REXCVAR_DECLARE(bool, masseffect_native_indices_cache);
REXCVAR_DECLARE(int32_t, masseffect_native_upload_memory);
REXCVAR_DECLARE(bool, masseffect_native_deferred_recording);
REXCVAR_DECLARE(bool, masseffect_deferred_native_fast);
REXCVAR_DECLARE(bool, masseffect_deferred_native_update);
REXCVAR_DECLARE(bool, banjo_deferred_packets);  // masseffect_deferred_recording.cpp
REXCVAR_DECLARE(bool, masseffect_notify_no_lock);
// The SDK's file log written by a thread of its own (sdk/src/core/logging.cpp, from futari-nx). Read when logging
// starts, before ApplyMasterSwitches: the off configs set it to false themselves (tools/make_enhancement_configs.py).
REXCVAR_DECLARE(bool, log_file_writer);
REXCVAR_DECLARE(bool, banjo_vertices_base_zero_r0);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_ring_bound_plan_memo);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_ring_estimator_reason);  // me_native_system.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_resolve_draw);  // masseffect_native_targets.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_edram4_clear_color0);  // masseffect_native_targets.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_proof_tess_leftover);  // me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_gpu_proof_triangle);  // me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_gpu_tiles_once);  // me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_gpu_dead_raster);  // me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_gpu_rect_split);  // me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_gpu_proof_depth_only);  // me_native_system.cpp
REXCVAR_DECLARE(bool, banjo_gpu_edram4_color_consumer);  // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_gpu_edram4_alias_by_shape);  // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_gpu_edram4_clear_merge);  // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_gpu_edram4_copy_batch);  // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_gpu_edram4_split_tiles);  // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_d3d_api_clear);  // src/me/banjo_d3d_clear.h
REXCVAR_DECLARE(bool, banjo_d3d_api_resolve);  // src/me/banjo_d3d_clear.h
REXCVAR_DECLARE(bool, banjo_gpu_skip_dead_draws);  // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_ring_vertex_lock_rule);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_decoder);  // me_native_system.cpp (banjo_ring_split.h, step 3)
REXCVAR_DECLARE(bool, banjo_ring_vs_hash_given);  // masseffect_native_draws.cpp (step 7)
REXCVAR_DECLARE(bool, banjo_d3d_records);  // me_native_system.cpp (banjo_records.h, record hooks; step 4)
REXCVAR_DECLARE(bool, banjo_d3d_records_native);  // me_native_system.cpp + banjo_d3d_native.h (step 5)
REXCVAR_DECLARE(bool, banjo_d3d_host_submit);  // me_native_system.cpp + banjo_me_hooks.cpp (step 6)
REXCVAR_DECLARE(bool, banjo_d3d_records_barriers);  // me_native_system.cpp (step 6b)
REXCVAR_DECLARE(bool, banjo_d3d_records_static);  // me_native_system.cpp (step 6b)
REXCVAR_DECLARE(bool, banjo_records_zero_ahead);  // me_native_system.cpp (step 10, from sa2-nx)
REXCVAR_DECLARE(bool, banjo_ring_texture_invalidate_range);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_ring_index_persistent);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_push_once);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_pass_narrow_end);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_blend_skip);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_structured_cf);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_early_alpha);  // masseffect_native_draws.cpp (tools/early_alpha.py)
REXCVAR_DECLARE(bool, banjo_ring_pmull_fingerprint);  // masseffect_native_draws.cpp (banjo_pmull_fingerprint.h)
REXCVAR_DECLARE(bool, banjo_hand_memcpy);       // src/cpu/banjo_hot_guest.cpp (hot/banjo_hand_natives.h)
REXCVAR_DECLARE(bool, banjo_hand_memset);       // src/cpu/banjo_hot_guest.cpp
REXCVAR_DECLARE(bool, banjo_hand_constants);    // src/cpu/banjo_hot_guest.cpp
REXCVAR_DECLARE(bool, banjo_hand_state_flush);  // src/cpu/banjo_hot_guest.cpp
REXCVAR_DECLARE(bool, banjo_gpu_texcoords_unswapped);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_no_texture_signs);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_texture_signs_per_slot);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_vfetch_block);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_branchless_select);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_pred_select);        // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_pred_select_ps);     // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_clamp_cbuf);  // masseffect_native_draws.cpp (package 25)
REXCVAR_DECLARE(bool, banjo_gpu_sign_mask_skip);  // masseffect_native_draws.cpp (package 25)
REXCVAR_DECLARE(bool, banjo_gpu_vs_outputs_unwritten);  // masseffect_native_draws.cpp (package 25)
REXCVAR_DECLARE(bool, banjo_gpu_bone_padding);  // masseffect_native_draws.cpp (package 25)
REXCVAR_DECLARE(bool, banjo_gpu_robustness_off);  // masseffect_native_draws.cpp (package 25)
REXCVAR_DECLARE(bool, banjo_gpu_edram_checks_cpu);  // masseffect_native_targets.cpp (package 25)
REXCVAR_DECLARE(bool, banjo_gpu_literals_cbuf);  // masseffect_native_draws.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_gpu_stencil_clear_partial);  // masseffect_native_targets.cpp (package 26)
#if BANJO_RENUT_RENDERER
REXCVAR_DECLARE(bool, banjo_renut_pipeline_cache);    // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_renut_precise_barriers);  // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_renut_pmull_hash);        // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_renut_upload_cached);     // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_renut_push_point_only);   // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_renut_capture_pool);      // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_renut_neon_swap);         // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_renut_shared_dedupe);     // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_gpu_depth_only);          // renut_engine/gpu/banjo_renut_tuning.cpp (package 28)
REXCVAR_DECLARE(bool, banjo_renut_record_worker);     // renut_engine/gpu/banjo_renut_record.cpp (package 28b)
REXCVAR_DECLARE(bool, banjo_gpu_output_trim);         // renut_engine/gpu/banjo_renut_tuning.cpp (package 28)
REXCVAR_DECLARE(bool, banjo_gpu_depth_words_view);    // renut_engine/gpu/banjo_renut_tuning.cpp (package 28)
REXCVAR_DECLARE(bool, banjo_renut_cache_save_quiet);  // renut_engine/gpu/banjo_renut_tuning.cpp (package 28)
REXCVAR_DECLARE(bool, banjo_renut_queue_notify_idle); // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
REXCVAR_DECLARE(bool, banjo_renut_linked_stages);     // renut_engine/gpu/banjo_renut_tuning.cpp (package 26)
#endif
REXCVAR_DECLARE(bool, banjo_gpu_tess_grid);          // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_bool_spec);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_bool_spec_direct);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_spec_per_stage);    // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_gpu_stencil_fetch_3d);      // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_gpu_depth1x_zcull);         // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_gpu_edram4_lean_barriers);  // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, banjo_gpu_resolve_clear_merge);   // masseffect_native_targets.cpp
REXCVAR_DECLARE(bool, masseffect_native_vs_pruned_outputs);  // masseffect_native_draws.cpp (constant 60)
REXCVAR_DECLARE(bool, banjo_gpu_identity_remap);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_index_base_held);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_gpu_nvk_draw_contract);  // masseffect_native_draws.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_native_partial_resolve_inside);  // masseffect_native_targets.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_native_resolved_retire_idle);  // masseffect_native_targets.cpp (from sa2-nx)
REXCVAR_DECLARE(bool, banjo_native_tfetch_filters);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_native_mip_range);  // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, banjo_native_stacked_baseless);  // masseffect_native_draws.cpp
// sa2-nx's CPU rounds of 2026-10-12 (docs/enhancements.md): the build-time work is in the generated code
// (tools/post_codegen.py, switches in app/CMakeLists.txt); these are its run-time keys.
REXCVAR_DEFINE_BOOL(banjo_cpu_guest_cas_no_barrier, true, "Banjo/Enhancements",
                    "Compare-and-swap (the game's interlocked operations, tools/guest_cas.py, and the SDK's lock words, "
                    "rex/thread/atomic.h) without the legacy builtin's trailing `dmb ish` on the A57; false puts the "
                    "barrier back at run time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-16, the 15 ms batch: docs/cpu-15ms-batch-1116.md): the generated code's guest compare-and-swap as
// relaxed exclusives (#163) and its lwsync / eieio as `dmb ishld` + `dmb ishst` / `dmb ishst` (#164 / #165), each exact
// with respect to the 360's memory model (lwarx / stwcx. order nothing there; lwsync does not order a store before a
// later load; eieio orders stores). false gives the old form back (AArch64 only; the PC's x86 forms do not change).
REXCVAR_DEFINE_BOOL(blur_guest_cas_relaxed, true, "Banjo/Enhancements",
                    "The game's compare-and-swap as relaxed exclusives (ldxr / stxr), as the 360's lwarx / stwcx.; "
                    "false: the sequentially consistent loop (ldaxr / stlxr) as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_guest_fences_split, true, "Banjo/Enhancements",
                    "The game's lwsync as dmb ishld + dmb ishst and eieio as dmb ishst (the PowerPC orderings); "
                    "false: a full dmb ish for both, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The SDK's two keys of those rounds are read and set by name: on the PC they live in the runtime DLL, whose
// storage the app cannot link to. kernel_critsec_pcr_owner (xboxkrnl_rtl.cpp): a critical section's owner from the
// PCR, no thread lookup. thread_signal_skip_idle (threading_posix.cpp, Switch only): no notify when nobody is blocked.
static bool SdkKeyOn(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }
#if defined(__aarch64__)
extern "C" unsigned char g_rex_atomic_legacy_barrier;  // sdk/src/core/atomic_posix.cpp
extern "C" unsigned char g_rex_guest_cas_seq_cst;      // the same file (blur-nx, 2026-11-16)
extern "C" unsigned char g_rex_guest_fence_full;
#endif

// Non-CPU: the handheld GPU profile (from sa2-nx's sa2_gpu_boost; renut_app.h calls it once before setup). Asks the
// console's performance manager for the official handheld configuration 0x92220008 (GPU 460.8 MHz, memory 1331.2 MHz,
// as UnleashedRecomp-NX, MarathonRecomp-NX and masseffect-nx do) through the SDK's switch_apm.cpp, which checks the real
// clocks for 1.5 s and reverts if the memory clock moved. Docked: nothing is requested. Exact: a clock, not a setting
// (the same frames, sooner). Risk: more power draw in handheld mode.
REXCVAR_DEFINE_BOOL(banjo_gpu_boost, true, "Banjo/Enhancements",
                    "Handheld: ask for the official 460.8 MHz GPU profile (memory clock checked unchanged, else "
                    "reverted); docked: nothing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Measurement-only work that costs time during play; the "enhancement" is turning it off (play config: false).
REXCVAR_DECLARE(bool, masseffect_native_diag_reuse);      // CPU: content fingerprints of every new texture (ring)
// Profiling keys (switch_profile_stacks below, masseffect_gpu_marks_categories in the renderer) are plain cvars, kept
// OUT of the enhancement table and its master switches: a config with a master off must still be profiled the same
// way as its A/B partner. The configs set both explicitly (tools/make_enhancement_configs.py).
REXCVAR_DECLARE(bool, masseffect_gpu_marks_categories);   // GPU time per pass category (~60 timestamps per frame)

// The SDK's sampler (sdk/src/ui/switch_perf.cpp) reads this by name, as an alternative to logs/rex/stacks_profile.flag.
REXCVAR_DEFINE_BOOL(switch_profile_stacks, false, "Banjo/Enhancements",
                    "Switch: the SDK's sampling profiler records every thread's pc (logs/rex/rex_profile.log), as an "
                    "empty logs/rex/stacks_profile.flag does. On in the A/B and profile configs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-16, the 15 ms batch): the sampler's interval in ms (read by name too). Each sample pauses one game
// thread from a thread above all of them; 5 ms keeps ~24,000 samples in a two-minute race window for the CPU tables
// at a fifth of the pauses.
REXCVAR_DEFINE_INT32(switch_profile_stacks_ms, 1, "Banjo/Enhancements",
                     "Switch: the SDK sampler's interval in milliseconds (1-100; each sample pauses one game thread)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-17, the Renut audit's #29): read by name by the SDK's profiler thread (switch_perf.cpp).
REXCVAR_DEFINE_BOOL(switch_perf_count_calls, true, "Banjo/Enhancements",
                    "Switch: the wrapped libnx calls (kicks, GPU mappings, cache cleans, yields and sleeps) counted for "
                    "the 10 s report's libnx line, a clock read and two atomics each. false skips that (a shipping "
                    "config that reads no reports)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The native copies of hot game functions (src/cpu/banjo_hot_guest.cpp, from sa2-nx): one key per function
// (banjo_cpu_native_<function>, CPU group), defined by the list tools/localize_native.py writes. Calls `row` for each.
namespace banjo::hot {
void ForEachKey(void (*row)(void* user, const char* key, bool own), void* user);
}  // namespace banjo::hot

namespace banjo {

namespace {

// Enhancements whose code default is off (they came from masseffect-nx, where only its shipped toml turned them on).
// A renut.toml from an older build lacks their keys and would silently leave them off (futari-nx saw 21 of its CPU
// enhancements off that way): when the key was not set by the toml, the environment or the command line, and the
// group's master switch is on, it gets its "on" value here. A key the toml sets is always respected. Deferred
// recording is not in the list (explicit only: futari-nx saw it stall with masseffect_native_output_no_wait on PC).
struct DefaultOn {
  const char* key;
  EnhancementGroup group;
  const char* on;
};
constexpr DefaultOn kDefaultsOn[] = {
    {"masseffect_native_flat_identity", EnhancementGroup::kCpu, "true"},
    {"masseffect_native_fast_pair", EnhancementGroup::kCpu, "true"},
    {"masseffect_native_raw_microcode", EnhancementGroup::kCpu, "true"},
    {"masseffect_native_pm4_fast", EnhancementGroup::kCpu, "true"},
    {"masseffect_native_compare_fast", EnhancementGroup::kCpu, "true"},
    {"masseffect_native_fast_copy", EnhancementGroup::kCpu, "true"},
    {"masseffect_native_fingerprint_mode", EnhancementGroup::kCpu, "1"},
    {"masseffect_native_dedupe_max_fingerprint", EnhancementGroup::kCpu, "16384"},
    {"masseffect_native_indices_cache", EnhancementGroup::kCpu, "true"},
    {"masseffect_notify_no_lock", EnhancementGroup::kCpu, "true"},
    {"masseffect_native_diag_reuse", EnhancementGroup::kCpu, "false"},
    {"masseffect_nvk_cache_per_stage", EnhancementGroup::kNonCpu, "true"},
};
uint32_t g_defaulted_on = 0;  // for the "[enhancements]" line

}  // namespace

bool EnhancementActive(EnhancementGroup group, bool own_key) {
  return own_key && (group == EnhancementGroup::kCpu ? REXCVAR_GET(banjo_cpu_enhancements)
                                                     : REXCVAR_GET(banjo_noncpu_enhancements));
}

void ApplyMasterSwitches() {
  for (const DefaultOn& row : kDefaultsOn) {
    const bool master = row.group == EnhancementGroup::kCpu ? REXCVAR_GET(banjo_cpu_enhancements)
                                                            : REXCVAR_GET(banjo_noncpu_enhancements);
    if (master && rex::cvar::GetFlagSource(row.key) == rex::cvar::Source::kDefault &&
        rex::cvar::SetFlagByName(row.key, row.on)) {
      ++g_defaulted_on;
    }
  }
  if (REXCVAR_GET(banjo_cpu_verify)) {
    REXCVAR_SET(masseffect_native_verify_n, 2147483647);
    REXCVAR_SET(masseffect_native_fast_copy_verify, 2147483647);
    REXCVAR_SET(banjo_native_reverb_verify, true);  // run both reverbs on every call and compare
    REXCVAR_SET(banjo_native_xaudio_src_verify, true);  // and both sample-rate converters (2026-10-23)
  }
#if defined(__aarch64__)
  // banjo_cpu_guest_cas_no_barrier: off puts the legacy `dmb ish` back after every compare-and-swap.
  g_rex_atomic_legacy_barrier = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_cpu_guest_cas_no_barrier)) ? 0 : 1;
  // blur-nx (2026-11-16): the relaxed compare-and-swap and the split fences (the keys above).
  g_rex_guest_cas_seq_cst = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(blur_guest_cas_relaxed)) ? 0 : 1;
  g_rex_guest_fence_full = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(blur_guest_fences_split)) ? 0 : 1;
#endif
  CodegenChecksInit();
  if (REXCVAR_GET(banjo_cpu_enhancements)) {
    return;
  }
  rex::cvar::SetFlagByName("kernel_critsec_pcr_owner", "false");
#if REX_PLATFORM_SWITCH
  rex::cvar::SetFlagByName("thread_signal_skip_idle", "false");
#endif
  REXCVAR_SET(masseffect_native_flat_identity, false);
  REXCVAR_SET(masseffect_native_fast_pair, false);
  REXCVAR_SET(masseffect_native_raw_microcode, false);
  REXCVAR_SET(masseffect_native_pm4_fast, false);
  REXCVAR_SET(masseffect_native_compare_fast, false);
  REXCVAR_SET(masseffect_native_fast_copy, false);
  REXCVAR_SET(masseffect_native_fingerprint_mode, 0);
  REXCVAR_SET(masseffect_native_dedupe_max_fingerprint, 0);
  REXCVAR_SET(masseffect_native_indices_cache, false);
  REXCVAR_SET(masseffect_native_upload_memory, 0);
  REXCVAR_SET(masseffect_native_deferred_recording, false);
  REXCVAR_SET(masseffect_deferred_native_fast, false);
  REXCVAR_SET(masseffect_deferred_native_update, false);
  REXCVAR_SET(banjo_deferred_packets, false);
  REXCVAR_SET(masseffect_notify_no_lock, false);
  REXCVAR_SET(masseffect_native_diag_reuse, true);
}

void LogEnhancements() {
  struct Row {
    const char* key;
    EnhancementGroup group;
    bool own;
  };
  const Row rows[] = {
      {"banjo_wait_blocking_ring", EnhancementGroup::kCpu, REXCVAR_GET(banjo_wait_blocking_ring)},
      {"banjo_native_reverb", EnhancementGroup::kCpu, REXCVAR_GET(banjo_native_reverb)},
      {"banjo_native_xaudio_src", EnhancementGroup::kCpu, REXCVAR_GET(banjo_native_xaudio_src)},
      {"banjo_repack_cached", EnhancementGroup::kCpu, REXCVAR_GET(banjo_repack_cached)},
      {"banjo_ring_registers_in_place", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_registers_in_place)},
      {"banjo_direct_state", EnhancementGroup::kCpu, REXCVAR_GET(banjo_direct_state)},
      {"banjo_no_draw_pairing", EnhancementGroup::kCpu, REXCVAR_GET(banjo_no_draw_pairing)},
      {"banjo_d3d_native_draw_indexed", EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_native_draw_indexed)},
      {"banjo_d3d_native_draw_vertices", EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_native_draw_vertices)},
      {"banjo_ring_edram_memo", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_edram_memo)},
      {"banjo_ring_constants_dedupe", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_constants_dedupe)},
      {"banjo_note_shader_fast", EnhancementGroup::kCpu, REXCVAR_GET(banjo_note_shader_fast)},
      {"banjo_ring_target_memo", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_target_memo)},
      {"banjo_ring_wait_vblank", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_wait_vblank)},
      {"banjo_ring_vertex_containment", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_vertex_containment)},
      {"banjo_ring_vertex_persistent", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_vertex_persistent)},
      {"banjo_ring_vertex_epoch", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_vertex_epoch)},
      {"banjo_ring_constants_adaptive", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_constants_adaptive)},
      {"banjo_ring_shader_load_memo", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_shader_load_memo)},
      {"banjo_ring_index_epoch", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_index_epoch)},
      {"banjo_ring_identity_once", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_identity_once)},
      {"banjo_ring_sampler_fast", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_sampler_fast)},
      {"banjo_ring_vertex_proven_first", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_vertex_proven_first)},
      {"banjo_ring_memexport_bounds", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_memexport_bounds)},
      {"banjo_vertices_base_zero_r0", EnhancementGroup::kCpu, REXCVAR_GET(banjo_vertices_base_zero_r0)},
      {"banjo_ring_bound_plan_memo", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_bound_plan_memo)},
      {"banjo_ring_estimator_reason", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_estimator_reason)},
      {"masseffect_native_flat_identity", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_native_flat_identity)},
      {"masseffect_native_fast_pair", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_native_fast_pair)},
      {"masseffect_native_raw_microcode", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_native_raw_microcode)},
      {"masseffect_native_pm4_fast", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_native_pm4_fast)},
      {"masseffect_native_compare_fast", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_native_compare_fast)},
      {"masseffect_native_fast_copy", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_native_fast_copy)},
      {"masseffect_native_fingerprint_mode", EnhancementGroup::kCpu,
       REXCVAR_GET(masseffect_native_fingerprint_mode) != 0},
      {"masseffect_native_dedupe_max_fingerprint", EnhancementGroup::kCpu,
       REXCVAR_GET(masseffect_native_dedupe_max_fingerprint) != 0},
      {"masseffect_native_indices_cache", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_native_indices_cache)},
      {"masseffect_native_upload_memory=1", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_native_upload_memory) == 1},
      {"masseffect_native_deferred_recording", EnhancementGroup::kCpu,
       REXCVAR_GET(masseffect_native_deferred_recording)},
      {"banjo_deferred_packets", EnhancementGroup::kCpu, REXCVAR_GET(banjo_deferred_packets)},
      {"masseffect_notify_no_lock", EnhancementGroup::kCpu, REXCVAR_GET(masseffect_notify_no_lock)},
      {"masseffect_native_diag_reuse=false", EnhancementGroup::kCpu, !REXCVAR_GET(masseffect_native_diag_reuse)},
      {"log_file_writer", EnhancementGroup::kCpu, REXCVAR_GET(log_file_writer)},
      {"banjo_cpu_guest_cas_no_barrier", EnhancementGroup::kCpu, REXCVAR_GET(banjo_cpu_guest_cas_no_barrier)},
      {"kernel_critsec_pcr_owner", EnhancementGroup::kCpu, SdkKeyOn("kernel_critsec_pcr_owner")},
#if REX_PLATFORM_SWITCH
      {"thread_signal_skip_idle", EnhancementGroup::kCpu, SdkKeyOn("thread_signal_skip_idle")},
#endif
      {"banjo_mesa_disk_cache_off", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_mesa_disk_cache_off)},
      {"banjo_gpu_zcull_greater", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_zcull_greater)},
      {"banjo_gpu_boost", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_boost)},
      {"banjo_gpu_resolve_draw", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_resolve_draw)},
      {"banjo_gpu_edram4_clear_color0", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_edram4_clear_color0)},
      {"banjo_gpu_proof_tess_leftover", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_tess_leftover)},
      {"banjo_gpu_proof_triangle", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_triangle)},
      {"banjo_gpu_tiles_once", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_tiles_once)},
      {"banjo_gpu_dead_raster", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_dead_raster)},
      {"banjo_gpu_rect_split", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_rect_split)},
      {"banjo_gpu_proof_depth_only", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_depth_only)},
      {"banjo_gpu_edram4_color_consumer", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_edram4_color_consumer)},
      {"banjo_gpu_edram4_alias_by_shape", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_edram4_alias_by_shape)},
      {"banjo_gpu_edram4_clear_merge", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_edram4_clear_merge)},
      {"banjo_gpu_edram4_copy_batch", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_edram4_copy_batch)},
      {"banjo_gpu_edram4_split_tiles", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_edram4_split_tiles)},
      {"banjo_d3d_api_clear", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_d3d_api_clear)},
      {"banjo_d3d_api_resolve", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_d3d_api_resolve)},
      {"banjo_gpu_skip_dead_draws", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_skip_dead_draws)},
      {"banjo_ring_vertex_lock_rule", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_vertex_lock_rule)},
      {"banjo_ring_decoder", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_decoder)},
      {"banjo_ring_vs_hash_given", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_vs_hash_given)},
      {"banjo_d3d_records", EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_records)},
      {"banjo_d3d_records_native", EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_records_native)},
      {"banjo_d3d_host_submit", EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_host_submit)},
      {"banjo_d3d_records_barriers", EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_records_barriers)},
      {"banjo_d3d_records_static", EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_records_static)},
      {"banjo_records_zero_ahead", EnhancementGroup::kCpu, REXCVAR_GET(banjo_records_zero_ahead)},
      {"banjo_ring_texture_invalidate_range", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_texture_invalidate_range)},
      {"banjo_ring_index_persistent", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_index_persistent)},
      {"banjo_ring_pmull_fingerprint", EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_pmull_fingerprint)},
      // The hand-written natives (banjo_hot_guest.cpp): in front of the generated copies of their functions in every
      // hook that contains them, so their own rows as well as the hooks' rows below.
      {"banjo_hand_memcpy", EnhancementGroup::kCpu, REXCVAR_GET(banjo_hand_memcpy)},
      {"banjo_hand_memset", EnhancementGroup::kCpu, REXCVAR_GET(banjo_hand_memset)},
      {"banjo_hand_constants", EnhancementGroup::kCpu, REXCVAR_GET(banjo_hand_constants)},
      {"banjo_hand_state_flush", EnhancementGroup::kCpu, REXCVAR_GET(banjo_hand_state_flush)},
      {"banjo_gpu_push_once", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_push_once)},
      {"banjo_gpu_pass_narrow_end", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_pass_narrow_end)},
      {"banjo_gpu_blend_skip", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_blend_skip)},
      {"banjo_gpu_structured_cf", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_structured_cf)},
      {"banjo_gpu_early_alpha", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_early_alpha)},
      {"banjo_gpu_texcoords_unswapped", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_texcoords_unswapped)},
      {"banjo_gpu_no_texture_signs", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_no_texture_signs)},
      {"banjo_gpu_texture_signs_per_slot", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_texture_signs_per_slot)},
      {"banjo_gpu_vfetch_block", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_vfetch_block)},
      {"banjo_gpu_branchless_select", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_branchless_select)},
      {"banjo_gpu_pred_select", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_pred_select)},
      {"banjo_gpu_pred_select_ps", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_pred_select_ps)},
      {"banjo_gpu_clamp_cbuf", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_clamp_cbuf)},
      {"banjo_gpu_sign_mask_skip", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_sign_mask_skip)},
      {"banjo_gpu_vs_outputs_unwritten", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_vs_outputs_unwritten)},
      {"banjo_gpu_bone_padding", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_bone_padding)},
      {"banjo_gpu_robustness_off", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_robustness_off)},
      {"banjo_gpu_edram_checks_cpu", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_edram_checks_cpu)},
      {"banjo_gpu_literals_cbuf", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_literals_cbuf)},  // package 26
      {"banjo_gpu_stencil_clear_partial", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_stencil_clear_partial)},  // package 26
#if BANJO_RENUT_RENDERER
      // Renut's renderer (banjo_renderer = "renut"): banjo-nx's tuning of it (renut_engine/gpu/banjo_renut_tuning.h).
      {"banjo_renut_pipeline_cache", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_pipeline_cache)},
      {"banjo_renut_precise_barriers", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_precise_barriers)},
      {"banjo_renut_pmull_hash", EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_pmull_hash)},
      {"banjo_renut_upload_cached", EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_upload_cached)},
      {"banjo_renut_push_point_only", EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_push_point_only)},
      {"banjo_renut_capture_pool", EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_capture_pool)},
      {"banjo_renut_shared_dedupe", EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_shared_dedupe)},
      {"banjo_gpu_depth_only", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_depth_only)},
      {"banjo_renut_record_worker", EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_record_worker)},
      {"banjo_gpu_output_trim", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_output_trim)},
      {"banjo_gpu_depth_words_view", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_depth_words_view)},
      {"banjo_renut_cache_save_quiet", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_cache_save_quiet)},
      {"banjo_renut_queue_notify_idle", EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_queue_notify_idle)},
      {"banjo_renut_linked_stages", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_linked_stages)},
#if defined(__aarch64__)
      {"banjo_renut_neon_swap", EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_neon_swap)},
#endif
#endif
      {"banjo_gpu_tess_grid", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_tess_grid)},
      {"banjo_gpu_bool_spec", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_bool_spec)},
      {"banjo_gpu_bool_spec_direct", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_bool_spec_direct)},
      {"banjo_gpu_spec_per_stage", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_spec_per_stage)},
      {"banjo_gpu_stencil_fetch_3d", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_stencil_fetch_3d)},
      {"banjo_gpu_depth1x_zcull", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_depth1x_zcull)},
      {"banjo_gpu_edram4_lean_barriers", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_edram4_lean_barriers)},
      {"banjo_gpu_resolve_clear_merge", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_resolve_clear_merge)},
      {"masseffect_native_vs_pruned_outputs", EnhancementGroup::kNonCpu,
       REXCVAR_GET(masseffect_native_vs_pruned_outputs)},
      {"banjo_gpu_identity_remap", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_identity_remap)},
      {"banjo_gpu_index_base_held", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_index_base_held)},
      {"banjo_gpu_nvk_draw_contract", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_nvk_draw_contract)},
      {"banjo_native_partial_resolve_inside", EnhancementGroup::kNonCpu,
       REXCVAR_GET(banjo_native_partial_resolve_inside)},
      {"banjo_native_resolved_retire_idle", EnhancementGroup::kNonCpu,
       REXCVAR_GET(banjo_native_resolved_retire_idle)},
      {"banjo_native_tfetch_filters", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_native_tfetch_filters)},
      {"banjo_native_mip_range", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_native_mip_range)},
      {"banjo_native_stacked_baseless", EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_native_stacked_baseless)},
      {"masseffect_native_pipelines_prewarm", EnhancementGroup::kNonCpu,
       REXCVAR_GET(masseffect_native_pipelines_prewarm)},
      {"masseffect_nvk_cache_per_stage", EnhancementGroup::kNonCpu, REXCVAR_GET(masseffect_nvk_cache_per_stage)},
  };
  std::string line;
  for (const Row& row : rows) {
    if (!line.empty()) line += ", ";
    line += row.key;
    line += EnhancementActive(row.group, row.own) ? " ON" : (row.own ? " off (master)" : " off");
  }
  uint32_t active = 0;
  for (const Row& row : rows) active += EnhancementActive(row.group, row.own) ? 1u : 0u;
  // The native copies of hot game functions, each with its own key (CPU group).
  struct HotRows {
    std::string* line;
    uint32_t active = 0;
    uint32_t total = 0;
  } hot{&line};
  hot::ForEachKey(
      [](void* user, const char* key, bool own) {
        auto& h = *static_cast<HotRows*>(user);
        const bool on = EnhancementActive(EnhancementGroup::kCpu, own);
        *h.line += ", ";
        *h.line += key;
        *h.line += on ? " ON" : (own ? " off (master)" : " off");
        h.active += on ? 1u : 0u;
        ++h.total;
      },
      &hot);
  active += hot.active;
  REXLOG_INFO("[enhancements] " BANJO_BUILD_FLAVOR "; cpu master {}, non-cpu master {}{} | {} of {} active, {} switched "
              "on because the toml "
              "does not set them | {} | prewarm threads {} | profiling: CPU sampler {}, GPU per-category marks {} | "
              "shader library: compile-time constants (build-time, docs/enhancements.md)",
              REXCVAR_GET(banjo_cpu_enhancements) ? "on" : "OFF", REXCVAR_GET(banjo_noncpu_enhancements) ? "on" : "OFF",
              REXCVAR_GET(banjo_cpu_verify) ? ", VERIFY (every self-check on every use)" : "", active,
              std::size(rows) + hot.total, g_defaulted_on, line, REXCVAR_GET(banjo_prewarm_threads),
              REXCVAR_GET(switch_profile_stacks) ? "on" : "off",
              REXCVAR_GET(masseffect_gpu_marks_categories) ? "on" : "off");
  // Every helper that has a form for one processor only, against plain C++ references (banjo_forms_selfcheck.cpp).
  FormsSelfCheck();
  BlurMicrobench();
}

}  // namespace banjo
