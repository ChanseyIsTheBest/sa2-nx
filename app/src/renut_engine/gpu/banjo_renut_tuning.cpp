// banjo-nx (2026-10-17): see banjo_renut_tuning.h.
#include "banjo_switch_default.h"  // banjo-nx (port 251): BANJO_SWITCH_DEFAULT
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include <rex/frame_latency.h>
#include "renut_engine/display_sync.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"  // banjo-nx (2026-10-26): g_touch_compare
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/textures.h"  // blur-nx (2026-11-17, audit #4/#5): ResolveRemakeReport
#include "renut_engine/gpu/stage_census.h"  // blur-nx (2026-11-17, audit #17-#22, #30)
#include "renut_engine/gpu/host_batch.h"  // blur-nx (2026-11-18, the Renut host batch)
#include "renut_engine/perf/frame_stats.h"  // blur-nx (2026-11-13): #37's census
#include "renut_engine/gpu/render/graphics_options.h"  // banjo-nx (2026-10-28): dynamic resolution's ranges
#include "renut_engine/gpu/guest/command_buffers.h"  // banjo-nx (2026-10-26): TagsReport
#include "renut_engine/gpu/banjo_renut_record.h"
#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/banjo_renut_deko3d.h"
#include <plume_deko3d.h>  // banjo-nx (2026-10-27): A/B 39's flag
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
#include <pthread.h>
#include "profiler/switch_cpu_profiler.h"  // banjo-nx (2026-10-28): the A/B halves' CPU a frame
#endif

#include <plume_render_interface.h>
#if REX_PLATFORM_SWITCH
#include <plume_vulkan.h>  // the VkPipeline of a plume pipeline (BeforePipelineBind)
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

// kameo-nx (2026-11-05): the latency test's clock, at the end of this file.
void BanjoTlbTestTick();

#include "cpu/banjo_enhancements.h"
#include "renut_engine/gpu/sasrt_gpu_round.h"  // sasrt-nx (2026-11-08)

namespace banjo::hot {
void Report();  // app/src/cpu/banjo_hot_guest.cpp: at most once every 10 s
}
#include "me/masseffect/banjo_pmull_fingerprint.h"
#include "me/me_platform_compat.h"  // RexSwitchPinCurrentThreadToCore and the priority helper (no-ops off the Switch)
#include "renut_engine/gpu/backend/backend.h"
#include "shader_cache.h"

// banjo-nx's own renderer defines these (me_native_system.cpp, masseffect_native_draws.cpp); they mean the same job
// here: the renderer's main thread and its background pipeline compiles.
REXCVAR_DECLARE(std::string, log_level);  // sdk/src/core/logging.cpp (banjo-nx 2026-11-20, batch #4)
REXCVAR_DECLARE(int32_t, banjo_ring_core);
REXCVAR_DECLARE(int32_t, banjo_background_compile_core);
// banjo-nx (2026-10-27, the user's request: NetherSX2_nx's 4-core mode). See sdk/src/core/threading_switch.h.
// OFF by default since the first four-core console run (2026-10-27): with them there, core 3 (half the system's) was
// full at times and audio stuttered; they run on banjo_background_compile_core again.
REXCVAR_DEFINE_BOOL(banjo_system_core_compiles, false, "Banjo/Threads",
                    "When the process has the system core (four cores granted, banjo_system_core 1 or 2): the uam "
                    "shader compiles and the pipeline prebuild workers run there instead of on "
                    "banjo_background_compile_core (off by default: core 3 was full with them and audio stuttered). "
                    "No effect with the usual three cores")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-22, after base port 254 showed what a pinned compile does behind port 251's command stage on core 3:
// in RR6 races one took 250-354 s): the shader compile threads (the uam thread and its spare-chunk thread, the pipeline
// prebuild workers, the list writer, the cache saver) placed by this one key, the ring cleaner (not a compile) left to
// banjo_system_core_compiles. The user: first "Let the shader compiles float around instead", then "Or restrict it to
// core 2 actually": 2 by default (the game's core 2, as banjo_background_compile_core's default). -1: every core of the
// process (with four cores, the system core too), no preferred core: the kernel places them (floating). -2: as before
// (banjo_background_compile_core / banjo_system_core_compiles / banjo_system_core_uam).
REXCVAR_DEFINE_INT32(sasrt_compile_threads_core, 2, "Banjo/Threads",
                     "The shader compile threads' core (uam, the pipeline prebuild workers, the list writer, the cache "
                     "saver): 0-3 pinned there (2: the user, 2026-11-22), -1 every core with no preferred one "
                     "(floating), -2 as banjo_background_compile_core / banjo_system_core_compiles / "
                     "banjo_system_core_uam place them")
    .range(-2, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-28, the user's request): the uam thread alone on the system core, with the background compiles held
// to banjo_deko3d_background_compile_share (10 %) by their rests. The pipeline prebuild workers (bursts at area loads,
// banjo_system_core_compiles) stay off it.
// Port 254 (2026-11-22, the user to rr6-nx: "Compile on core 2"): off by default. Port 251's command stage (0x2E) and
// warm helper (0x2F) keep the system core busy in play, above this thread (0x3B): in RR6 races a compile there took
// 250-354 s and the prebuild waiting on it held every specialised pipeline back (GPU +1.2 ms); on core 2, 169 ms.
REXCVAR_DEFINE_BOOL(banjo_system_core_uam, false, "Banjo/Threads",
                    "When the process has the system core (four cores granted, banjo_system_core 1 or 2): the uam "
                    "shader compile thread runs there (at the lowest priority an application gets, so the audio "
                    "threads there go first; its background compiles rest to banjo_deko3d_background_compile_share of "
                    "the core) instead of on banjo_background_compile_core, the game's core 2. The pipeline prebuild "
                    "workers stay where banjo_system_core_compiles puts them. Off by default (port 254: port 251's "
                    "command stage and warm helper keep that core busy). No effect with the usual three cores")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(int32_t, banjo_prewarm_threads);
// banjo-nx's renderer's NVK keys (masseffect_native_draws.cpp), applied to Renut's renderer too (ControlNvkLocked).
REXCVAR_DECLARE(bool, banjo_gpu_nvk_draw_contract);
REXCVAR_DECLARE(bool, masseffect_native_nvk_emission);
REXCVAR_DECLARE(bool, masseffect_native_nvk_cbufs);
REXCVAR_DECLARE(bool, masseffect_native_nvk_dynamic);
REXCVAR_DECLARE(bool, masseffect_native_nvk_preload);
REXCVAR_DECLARE(int32_t, masseffect_native_nvk_measure);
REXCVAR_DECLARE(bool, masseffect_native_set4_differences);
// banjo-nx's renderer's robustness key (masseffect_native_draws.cpp), for Renut's pipelines too (package 27).
REXCVAR_DECLARE(bool, banjo_gpu_robustness_off);
REXCVAR_DECLARE(bool, banjo_gpu_bool_spec);  // the same key for Renut's pipelines (package 27)
REXCVAR_DECLARE(bool, banjo_gpu_blend_skip);  // likewise
REXCVAR_DECLARE(bool, banjo_gpu_tess_grid);   // likewise
// bangaio-nx (2026-10-29): the delta chain's switches (backend/queued.cpp), latched below.
REXCVAR_DECLARE(bool, banjo_renut_snapshot_deltas);
REXCVAR_DECLARE(bool, banjo_renut_snapshot_deltas_narrow);
REXCVAR_DECLARE(bool, banjo_renut_snapshot_deltas_verify);
REXCVAR_DECLARE(bool, banjo_renut_neon_compare);
REXCVAR_DECLARE(bool, banjo_renut_capture_batches);
REXCVAR_DECLARE(bool, banjo_renut_small_captures);
REXCVAR_DECLARE(bool, banjo_renut_delta_masks);
REXCVAR_DECLARE(bool, banjo_renut_delta_prefetch);

// banjo-nx (2026-10-17, package 30): banjo_gpu_blend_skip under Renut's renderer, off. With it on, matched frames of
// the intro cutscene on the PC (Vulkan) blurred what the depth of field keeps sharp (Kazooie, the radio) where the
// emulated renderer does not; off, or the five big material shaders left out of it, or banjo_gpu_bool_spec off, they
// matched. On deko3d (the console, Ryujinx) the river water was missing with it on and drawn with it off. Why its
// discard is not the blend's no-op there is not found yet; the native renderer keeps banjo_gpu_blend_skip.
// banjo-nx (2026-10-26, setter rewrite): d3d_native.h g_touch_compare.
// banjo-nx (2026-11-20, render-thread batch): the 10 s report's per-draw counts (Latch().counts).
REXCVAR_DEFINE_BOOL(banjo_renut_draw_counts, true, "Banjo/Renut",
                    "The 10 s report's per-draw statistics counts (the GPU round's item counts, plume's binds and "
                    "barriers, the shared and pushed constant counts, the depth counters): a load and a store each, "
                    "several a draw. Off: those lines show zeros, the render thread saves their work. Always off with "
                    "log_level \"off\" (the release); this key turns them off with the logs kept (a measured run)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_touch_compare, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26): the native D3D setters store a device field and mark its chunk for "
                    "the recording only when the value changes (compare-on-set, as Unleashed's setters), so a recorded "
                    "op compares the chunks that changed rather than all the game set again. Exact. A/B 26. Off: every "
                    "write stored and marked, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_tags_apart, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26): a recording device's native setters keep their dirty tags out of "
                    "the device (their writes mark their chunks already), so a recorded op compares the render state's "
                    "16 chunks only when the game's own inline code tagged them. The periodic check "
                    "(banjo_renut_tags_check) turns it off at the first change it finds unmarked. A/B 27. Off: the "
                    "setters' tags in the device, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(banjo_renut_blend_skip, false, "Banjo/Enhancements",
                    "Renut's renderer: banjo_gpu_blend_skip (off since package 30: it broke the depth of field with "
                    "banjo_gpu_bool_spec, and the deko3d renderer's river water)");

REXCVAR_DEFINE_BOOL(banjo_renut_half_pixel_offset, true, "Banjo/Enhancements",
                    "Renut's renderer: D3D9 pixel centres (PA_SU_VTX_CNTL.pix_center 0) move the viewport half a pixel, "
                    "as the GPU and Xenia do (before 2026-10-18 everything drew half a pixel up-left: the depth of "
                    "field's blur a whole 320x180 texel, the halo above Kazooie's crest)");

REXCVAR_DEFINE_BOOL(banjo_renut_point_minmax_radius, true, "Banjo/Enhancements",
                    "Renut's renderer: the point size limits (PA_SU_POINT_MINMAX) are radii, as the hardware and Xenia "
                    "read them (before 2026-10-18 they were taken as diameters: the depth of field's largest bokeh sprites "
                    "drew at half size)");

REXCVAR_DEFINE_BOOL(banjo_renut_depth_words_stencil, true, "Banjo/Enhancements",
                    "Renut's renderer: a depth resolve's guest words carry the stencil (they read 0 before 2026-10-18), "
                    "which the depth of field tests to keep the characters sharp");

REXCVAR_DEFINE_BOOL(banjo_renut_alpha_to_mask_test, true, "Banjo/Enhancements",
                    "Renut's renderer: alpha to coverage on a single-sample target is an alpha test at 0.5, as the "
                    "native renderer's banjo_native_alpha_to_mask_test (without it NVIDIA GPUs dither the grass, fur and "
                    "foliage edges in a checker pattern: the 'non-square pixels', 2026-10-18)");

REXCVAR_DEFINE_BOOL(banjo_gpu_depth_only, true, "Banjo/Enhancements",
                    "Renut's renderer (package 28; Unleashed / Marathon's depth-only pipelines): a draw that writes no "
                    "colour into a depth target, without the alpha test or alpha to coverage, whose pixel shader "
                    "neither kills pixels, writes depth nor exports to memory, is drawn without the shader (the "
                    "rasterizer writes the same depth and stencil); its constants and textures are not set up. Shadow "
                    "maps and depth passes: about half the draws. Same image");
REXCVAR_DEFINE_BOOL(banjo_gpu_output_trim, true, "Banjo/Enhancements",
                    "Renut's renderer (package 28, Vulkan; Unleashed / Marathon's trimmed pixel outputs): the colour "
                    "channels a pipeline never writes (write mask, the target's format, no target) are 0 from the "
                    "pixel shader's exits on, after the alpha test, through a specialization constant from the "
                    "pipeline key, so NAK drops what computed only them (the colour of alpha-tested shadow draws, the "
                    "channels of masked passes). The alpha stays when a written channel blends with it or alpha to "
                    "coverage is on. No new pipelines (the key has the mask already). Same image");
REXCVAR_DEFINE_BOOL(banjo_gpu_depth_words_view, true, "Banjo/Enhancements",
                    "Renut's renderer (package 28; fewer resolve copies): a texture over a depth resolve's memory (the "
                    "guest words, an 8_8_8_8 fetch; the game reads nearly every depth resolve so) samples a view of "
                    "the resolve's words texture instead of a copy of the whole texture made at its first bind after "
                    "each resolve (a 4-byte read and write a texel). The same bytes: every draw binds its textures "
                    "again, so the draws after a resolve read its words either way. Off in packages 28b-32 (the depth of field "
                    "was broken by other causes then: the blend skip, the stencil, the half pixel); on again since "
                    "2026-10-18 with the depth of field matching the emulated renderer");
REXCVAR_DEFINE_BOOL(banjo_deko3d_draw_resolves, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): single-sampled colour resolves are drawn (a texel copy into the "
                    "destination as a render target) instead of copied on the copy engine, which needs the whole GPU "
                    "idle before and after it. Same bits");
REXCVAR_DEFINE_BOOL(banjo_deko3d_depth_compression, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): depth/stencil targets get the GPU's hardware compression, as the "
                    "colour targets have (the main pass reads and writes far less depth memory). Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_alpha_spec, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): the alpha test's function is part of the pipeline (specialization "
                    "constant 903 of the deko3d shaders): a draw without the alpha test has no compare and no discard "
                    "in its pixel shader, and the GPU tests depth before shading. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_tess_spec, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): a pipeline that is not a tessellated draw's gives its vertex shader "
                    "the patch kind 0 as specialization constant 904: the tessellation code goes. No new pipelines. "
                    "Same pixels");
REXCVAR_DEFINE_BOOL(banjo_gpu_fetch_spec, true, "Banjo/Enhancements",
                    "deko3d renderer (banjo-nx 2026-10-31; vivapinata-nx 2026-11-03: this game's terrain): a tessellated draw "
                    "gives its vertex shader its patch kind (constant 904) and the vertex types of its first four bound "
                    "fetches (920-923): the other patch kinds' code and the fetch's format switch go. New pipelines, kept in "
                    "the DKSH cache; until one is built the draw uses the generic one. A/B 69. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_vs_outputs, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): a pipeline's vertex shader writes only the outputs its pixel shader "
                    "reads (none for depth-only draws: the shadow maps), specialization constant 905: the others and "
                    "what computes them go. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_uam_prmt, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): uam turns the shaders' byte swaps (the big-endian words the vertex "
                    "fetch reads from memory: bones, skinned vertices) into Maxwell's PRMT, one instruction each. Off: "
                    "the shifts and masks as before, with a DKSH cache of their own (deko3d_shaders_f446.bin, "
                    "compiled by the console as it goes). Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_uam_gvn, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): uam's value numbering over the dominator tree: what a shader computes "
                    "again later (the texture fetch of the early alpha test, repeated by the main body; equivalent "
                    "if / else branches) is reused, where it does not cost the pixel shader a step of 8 registers. "
                    "Off: a DKSH cache of its own (deko3d_shaders_f<flags>.bin, compiled by the console as it goes). "
                    "Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_uam_uniform_branches, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): uam leaves out the reconvergence point (SSY / SYNC) of an if / else "
                    "whose condition comes from constant buffers only (the same for every thread: it cannot diverge). "
                    "Off: a DKSH cache of its own (deko3d_shaders_f<flags>.bin, compiled by the console as it goes). "
                    "Same pixels");
REXCVAR_DEFINE_BOOL(banjo_renut_stall_present, false, "Banjo/Enhancements",
                    "The colour flash (2026-10-19): while the game stalls (loading) the overlay pump re-presents the "
                    "last front buffer every 16 ms (true, as before). Off: nothing is presented then unless a menu or "
                    "notification is up (the display keeps the last frame, as the console does), and a front buffer "
                    "no resolve wrote is never shown");
REXCVAR_DEFINE_BOOL(banjo_renut_device_swizzle, true, "Banjo/Enhancements",
                    "The colour flash (2026-10-20): a texture whose header in guest memory describes the same data as "
                    "the device's fetch constant but differs in the fields SetTexture copies unchanged (signs, pitch, "
                    "tiling, swizzle, number format, exponent adjust, mip address) is sampled through the device's "
                    "fetch constant (what the GPU reads): the header is a temporary the game has rewritten (RenderDoc "
                    "capture of Ryujinx: a bloom level read through the swizzle 1,1,0,0 turned the whole frame "
                    "yellow). The same pixels whenever the header is current. Off: the header's, as before");
REXCVAR_DEFINE_INT32(banjo_renut_portrait_fur_shells, 1, "Banjo/Enhancements",
                     "NOT exact, the user's call (2026-10-20): banjo_renut_fur_shells for the fur drawn into targets of "
                     "128 x 128 or less (the dialogue portrait, 96 x 96, where 21 shells cost ~2.5 ms of the heaviest "
                     "frames for a few thousand pixels). 1 every shell; 3 draws 7 of 21")
    .range(1, 7);
REXCVAR_DEFINE_INT32(banjo_renut_fur_shells, 1, "Banjo/Enhancements",
                     "NOT exact, the user's call (2026-10-20): 1 draws every fur shell (as the game). N > 1: of each fur "
                     "part's draws in a frame (the game draws every part of the body once a shell; a part = the same "
                     "shaders and index range; the fur family of tools/nullps_shaders.txt), every Nth is drawn, the "
                     "first always: 2 draws 11 of Banjo's 21 shells (thinner fur, the same length). The fur is about "
                     "18 ms of the heaviest frame's GPU at the stock clock")
    .range(1, 4);
REXCVAR_DEFINE_INT32(banjo_renut_fur_shells_percent, 100, "Banjo/Enhancements",
                     "NOT exact, the user's call (2026-10-21): the share of each fur part's shells drawn, spread evenly "
                     "(75: three of every four, the 4th, 8th, ... left out: 16 of Banjo's 21, his outermost shell drawn, "
                     "so the fur keeps its length), the first always (it writes the skin-once rows); the dialogue "
                     "portrait's too. 100 draws every shell as "
                     "the game. Below 100 it takes the place of banjo_renut_fur_shells and banjo_renut_portrait_fur_shells")
    .range(25, 100);
REXCVAR_DEFINE_INT32(banjo_renut_portrait_fur_shells_percent, 100, "Banjo/Enhancements",
                     "NOT exact, the user's call (2026-10-21): banjo_renut_fur_shells_percent for the fur drawn into "
                     "targets of 128 x 128 or less (the dialogue portrait, 96 x 96): 50 draws every other shell (11 of "
                     "21, the first always). 100: the same share as the rest of the fur")
    .range(25, 100);
// renut-ultimate (2026-11-12, port 118, the user's choice): MSAA off by default in the base (1), SMAA on
// (banjo_renut_smaa) smooths the edges instead. A game sets 4 (its own) or 2 in its toml.
REXCVAR_DEFINE_INT32(msaa_level, 1, "Katamari/Graphics",
                     "Samples per pixel of the game's MSAA targets: 1 (off, the base's default: SMAA smooths the edges "
                     "instead), 2 or 4 (the game's own). Fewer: rougher edges, less GPU work");
REXCVAR_DEFINE_INT32(banjo_renut_portrait_msaa_samples, 0, "Banjo/Enhancements",
                     "NOT exact, the user's call (2026-10-21): the samples of the colour and depth targets of 128 x 128 "
                     "or less that the game makes with MSAA (the dialogue portrait's 96 x 96 box, 4x). 0 as the game; 2: "
                     "2x (half the samples to test, write and resolve; the edges still smoothed); 1: single-sampled (no "
                     "MSAA: its resolves plain copies, without their GPU stalls, its edges not smoothed: SMAA runs "
                     "before the HUD, the portrait is part of it). Only lowers a count. disable_msaa covers only the "
                     "scene target")
    .range(0, 2);
// Port 163 (from fnxf_xiii, its notes of 2026-10-21 to 11-17; fnxf_xiii's default, flags 77247): see Deko3DUamFlags.
// Off by default: the shaders' code changes (a graphics-breaking key is off in the base; fnxf_xiii's own version once
// overwrote a pixel shader's colour output, fixed here in all four moving passes), and another DKSH cache file
// (deko3d_shaders.bin is the 1471 flags' only).
REXCVAR_DEFINE_BOOL(banjo_deko3d_uam_pressure, false, "Banjo/Enhancements",
                    "deko3d: uam's register pressure passes (values computed at their first use, also across blocks; a "
                    "shader still above 56 registers compiled again with rematerialisation and the hoist). fnxf_xiii: "
                    "about 9 registers fewer on pixel shaders above 56, the 34 above 64 all at 64 or fewer. Another "
                    "shader cache. false: as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_uam_wide_rows, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-20, GPU): uam reads each bone row of tools/deko3d_glsl_opt.py's aw4 view "
                    "(a uvec4 of storage buffer 15) with one 128-bit load instead of four 32-bit ones (banjo flag 1024): "
                    "the fur vertex shader 48 -> 12 loads, 596 -> 560 instructions; the console's profile shows those "
                    "vertex shaders memory-bound. Same words, same pixels. Aligned by construction: the fetch data's "
                    "storage binding starts on 256 bytes (render/draw.cpp) and a row is element n of 16 bytes. Off: the "
                    "32-bit loads, with a DKSH cache of its own (deko3d_shaders_f447.bin)");
REXCVAR_DEFINE_BOOL(banjo_renut_diag_null_ps, false, "Banjo/Diagnostics",
                    "Diagnostic (2026-10-20): the fur family's pixel shaders (tools/nullps_shaders.txt) discard at once, so the GPU profile shows their draws' vertex and rasterisation cost alone. The fur vanishes: a measurement, not a picture (configs/banjo_renut_diag_fur_vertex_only.toml)");
// renut-ultimate (2026-11-12, port 118, the user's choice): on by default in the base. It acts only in a game whose
// final composite pixel shader is in banjo_renut_smaa_after_ps (N&B's by default): a game that has not set its own runs
// SMAA on no frame (the 10 s "SMAA ... frames" line says 0), and while SMAA is on the colour hand-over is off unless
// sasrt_gpu_handover_with_smaa is on.
REXCVAR_DEFINE_BOOL(banjo_renut_smaa, true, "Banjo/Enhancements",
                    "SMAA 1x (2026-10-21, the user's request; Jimenez et al.'s SMAA, MIT: app/thirdparty/smaa): "
                    "anti-aliasing of the game's 3D image right after its final composite (a draw with one of the pixel "
                    "shaders of banjo_renut_smaa_after_ps), before the HUD is drawn over it, so the HUD and its text stay "
                    "as they are. A copy and three full-screen passes at the target's size (luma edges, blending "
                    "weights, neighbourhood blending; render/smaa.cpp); the in-run A/B target 19 measures its GPU time. "
                    "It smooths the 3D image's edges: not the game's own picture. On by default in the base; a game "
                    "lists its own composite in banjo_renut_smaa_after_ps, or SMAA runs on no frame");
REXCVAR_DEFINE_INT32(banjo_renut_smaa_quality, 1, "Banjo/Enhancements",
                     "banjo_renut_smaa's preset: 1 = SMAA's medium (8 search steps, no diagonal or corner detection), "
                     "2 = high (16 search steps, diagonal search of 8, corner rounding 25: smoother diagonals, more GPU "
                     "time)")
    .range(1, 2);
REXCVAR_DEFINE_DOUBLE(banjo_renut_smaa_threshold, 0.1, "Banjo/Enhancements",
                      "banjo_renut_smaa's luma edge threshold (SMAA's presets: 0.15 low, 0.1 medium and high, 0.05 "
                      "ultra): lower finds more edges (and softens more texture detail)");
REXCVAR_DEFINE_STRING(banjo_renut_smaa_after_ps, "981A6A58F2ABFA42", "Banjo/Enhancements",
                      "banjo_renut_smaa: pixel shaders (16-digit hex hashes, comma separated) whose draw is the frame's "
                      "final composite; SMAA runs on its target right after it, once a frame. N&B: 981A6A58F2ABFA42, "
                      "the bloom and depth-of-field composite before the HUD (emulator trace of 2026-10-21)");
// banjo-nx (2026-10-21, internal resolution, the user's request; sa2-nx and Peggle 2 switch resolution by mode too).
REXCVAR_DEFINE_INT32(banjo_renut_render_height_handheld, 720, "Banjo/Enhancements",
                     "NOT exact, the user's request (2026-10-21): on deko3d, the height of the 3D image in handheld mode: "
                     "720 = the game's 1280x720; 648 = 1152x648 (0.81 of the pixels to shade); any height from 360 to "
                     "1080, rounded to a multiple of 9 (whole pixels at every 16:9 size the game uses; package 36z2). "
                     "The HUD, menus and text stay at the display's resolution: right after the game's final "
                     "composite (banjo_renut_smaa_after_ps) and SMAA the image is scaled to it "
                     "(banjo_renut_upscale_filter) and the HUD is drawn over the full-size copy (render/ui_scale.cpp). "
                     "Frames without that composite for half a second (menus, loading) are drawn at the display's "
                     "resolution whole. Shadow maps and the post-processing buffers keep the console's sizes. From the "
                     "next frame")
    .range(360, 1080);  // port 196 (S4ep1-nx's): from 480; a game drawing 1920x1080 is half at 360 (960x540)
REXCVAR_DEFINE_INT32(banjo_renut_render_height_docked, 720, "Banjo/Enhancements",
                     "NOT exact, the user's request (2026-10-21): banjo_renut_render_height_handheld for docked mode "
                     "(864 = 1536x864, 1.2x the game's; 900 = 1600x900; with banjo_renut_display_height_docked = 1080 "
                     "the HUD at 1920x1080). From the next frame on deko3d (blur-nx 2026-11-18), else read at start")
    .range(360, 1080);  // port 196 (S4ep1-nx's): from 480
REXCVAR_DEFINE_INT32(banjo_renut_display_height_docked, 720, "Banjo/Enhancements",
                     "The display's resolution when docked on deko3d (2026-10-21; the swapchain change from sa2-nx "
                     "1.0.3.1): 720 = 1280x720, which the console scales to the TV; 1080 = the swapchain made at "
                     "1920x1080 and the HUD, menus and text drawn at it (1.5x). Docking and undocking switch at the next "
                     "frame")
    .range(720, 1080);
REXCVAR_DEFINE_BOOL(banjo_renut_host_mips, true, "Banjo/Enhancements",
                    "Renut's renderer: a resolve destination drawn above 1x (docked: the 3D image at 1.2x, the HUD at "
                    "1.5x) gets an extra host mip level, rebuilt after every resolve, so minified reads of it filter as "
                    "the guest's 1x texture would (Renut's, made for 1440p and 4K). Off (2026-10-21, NOT exact above "
                    "1x): level 0 only, as at 1x and in handheld mode; the console log of 36z had it at 4 ms a docked "
                    "frame (\"resolve host mip levels\")");
REXCVAR_DEFINE_INT32(banjo_renut_upscale_filter, 0, "Banjo/Enhancements",
                     "The scaling of the 3D image from banjo_renut_render_height_* to the display's resolution "
                     "(2026-10-21): 0 = bilinear (one sample a pixel: the cheapest; the user's choice, every config), "
                     "1 = bicubic Catmull-Rom (five bilinear samples a pixel, sharper)")
    .range(0, 1);
REXCVAR_DEFINE_INT32(banjo_renut_diag_smaa_compare_s, 0, "Banjo/Diagnostics",
                     "banjo_renut_smaa comparison pictures (2026-10-21): every N seconds the same frame's 3D image "
                     "before and after SMAA (the HUD not drawn yet) as capturas/smaa_NNN_off.tga and _on.tga next to "
                     "the NRO, at full size (0 = off; render/smaa.cpp)");
// Port 164 (from fnxf_xiii, its note of 2026-11-05): backend/video.cpp detail::PieceDraws.
REXCVAR_DEFINE_BOOL(banjo_deko3d_progressive_submit, true, "Banjo/Enhancements",
                    "deko3d: a frame's command list ends at a kick once it holds a quarter of the previous frame's draws "
                    "(40 at least, up to 6 pieces a frame), so the GPU works on the frame while the game and the render "
                    "thread still make the rest (fnxf_xiii: a 195-draw frame never reached the fixed 200 and went to the "
                    "GPU only at the present). banjo_deko3d_eager_kicks, when set, keeps its fixed 100. false: 200 draws, "
                    "3 pieces, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_eager_kicks, false, "Banjo/Enhancements",
                    "Input latency A/B (2026-10-20): the frame goes to the GPU in more, earlier parts: a command list ends at a game kick once it holds 100 draws (not 200, at most 6 a frame, not 3) and goes to the GPU at that kick (its upload memory cleaned first), not at the next one. At the frame cap the GPU then works while the render thread is still replaying the frame instead of mostly after the present: the frame is done sooner after its controller read (the [latency] line's \"input -> GPU done\"). The same commands in the same order (configs/banjo_deko3d_eager_kicks_ab.toml; in-run A/B target 18)");
REXCVAR_DEFINE_BOOL(banjo_deko3d_skin_once, false, "Banjo/Enhancements",
                    "GPU A/B (2026-10-20): the fur's skinned meshes (tools/skinonce_shaders.txt) skinned once a frame: a part's first draw of the frame writes each vertex's three skinned matrix rows to a buffer (constant 913, shared word v[84].z = 1), its other shells read them there (= 2) instead of loading 12 bone rows and blending them (the vertex shader about half as long). Only where every input of the skinning is the same draw for draw (shaders, index and vertex buffers, bone data and its constants); the same rows, bit for bit, so the same pixels (configs/banjo_deko3d_skin_once_on.toml; in-run A/B target 17)");
REXCVAR_DEFINE_BOOL(banjo_deko3d_light_branch, false, "Banjo/Enhancements",
                    "GPU A/B (2026-10-20): in the heaviest lit pixel shaders (tools/lightbranch_shaders.txt) each point light's work runs in a branch on its attenuation test, so warps whose pixels the light does not reach skip it (a light is about 35 instructions); where every light reaches, the branches cost a few instructions. The same pixels; whether it is faster is the console's to say (configs/banjo_deko3d_light_branch_on.toml)");
REXCVAR_DEFINE_BOOL(banjo_deko3d_gpr32, false, "Banjo/Enhancements",
                    "GPU A/B (2026-10-19): the pixel shaders of 33-40 GPRs (the fur family, the top of the GPU profile) "
                    "compiled with 32 at most, so the GPU keeps every warp slot (64 instead of about 48) to hide texture "
                    "latency, at the price of a few spills (the fur shader: 15 loads, 5 stores). The same pixels; "
                    "whether it is faster is the console's to say (configs/banjo_deko3d_gpr32_on.toml)");
REXCVAR_DEFINE_BOOL(banjo_deko3d_early_z, true, "Banjo/Enhancements",
                    "GPU (2026-10-19): a pixel shader that discards (alpha test) in a draw that writes neither depth "
                    "nor stencil (the blended fur shells, particles) gets its depth test before it "
                    "(layout(early_fragment_tests), constant 909): the pixels the depth test rejects no longer run it. "
                    "The same pixels: with no depth or stencil write the discard has nothing to undo");
REXCVAR_DEFINE_BOOL(banjo_deko3d_txq_constants, true, "Banjo/Enhancements",
                    "GPU (2026-10-19): a pixel shader's texture size queries of level 0 read the size from a buffer "
                    "plume deko3d fills from the views it binds, instead of a texture instruction per pixel (A5E2, the "
                    "full-screen pass second in the GPU profile, ran 4 fetches and 4 queries a pixel; the fur shells' "
                    "stacked textures 1). Specialization constant 908 (tools/deko3d_glsl_opt.py, pass txq)");
REXCVAR_DEFINE_BOOL(banjo_deko3d_txq_verify, false, "Banjo/Enhancements",
                    "Test of banjo_deko3d_txq_constants: the shaders run the query as before and paint the pixel "
                    "magenta where the buffer's size differs (constant 908 = 2; the variants compile at run time)");
REXCVAR_DEFINE_BOOL(banjo_deko3d_early_hoist, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): in a pixel shader with Renut's early alpha test, the main body's first "
                    "texture region (the fur's texture-array or 2D fetch) goes ahead of the test, so uam drops the "
                    "test's copy of it (specialization constant 907; the fur shader 4 texture fetches fewer, 2 more "
                    "registers). Same pixels");
REXCVAR_DEFINE_BOOL(banjo_renut_depth_words_lazy, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-19): a depth resolve writes its words target (the 8_8_8_8 view of the "
                    "resolved memory, depth and stencil as bytes) only for destinations something reads that way (on "
                    "the emulator route: the depth of field's, 1 of 5 resolves a frame; the shadow maps' never), and "
                    "in the first 300 resolves of each. A read of words not written is served by writing them then "
                    "from the unchanged source; the 10 s depth resolve line counts both. Same pixels");
REXCVAR_DEFINE_BOOL(vivapinata_depth_resolve_skip_unread, false, "Banjo/Enhancements",
                    "vivapinata-nx (2026-11-12, NOT exact in principle, off; A/B 156): a depth resolve of a "
                    "multisampled target into a destination nothing has read yet is not drawn. The first time anything "
                    "reads such a destination (sampled, or through its words) its resolve is drawn then (from the "
                    "target's newer content if it has changed since: that once not the same pixels) and every later "
                    "one. For a game that leaves depth resolves unread (Viva Pinata samples its copies: nothing to skip)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_depth_handover, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-19, GPU): a depth resolve of a whole depth target into a texture of its "
                    "size that needs no words is not drawn: the texture is sampled from the target's image until the "
                    "target is bound again (then the resolve is drawn after all), and at the target's next whole clear "
                    "the texture takes that image and the target another. The shadow maps, resolved and cleared once a "
                    "frame, are never copied. The float a resolve writes is the depth it loads: same pixels");
// kameo-nx (2026-11-03, the GPU round from the third console run; docs/kameo-notes.md).
REXCVAR_DEFINE_BOOL(banjo_deko3d_color_handover, true, "Banjo/Enhancements",
                    "deko3d renderer: a whole colour resolve into a texture of its size and format is not drawn: the "
                    "texture is sampled from the target's image until a draw writes the target (then it is drawn after "
                    "all), and at the target's next whole clear the texture takes that image and the target another. "
                    "Kameo's 512x512 render-to-texture loop, resolve-and-clear 12 times a frame, is never copied. Every "
                    "texel is the one the copy would write: same pixels. About 20 MB of GPU memory more");
REXCVAR_DEFINE_BOOL(banjo_deko3d_scanout_handover, true, "Banjo/Enhancements",
                    "deko3d renderer: the frame's last resolve, left in the target's image (banjo_deko3d_color_handover), "
                    "is scanned out from there, not copied first. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_depth_handover_sampled, true, "Banjo/Enhancements",
                    "deko3d renderer: a draw that writes neither depth nor stencil samples a depth resolve from the image "
                    "of its own depth target, which it only tests, instead of having the resolve drawn first. Kameo draws "
                    "2 full-screen depth resolves a frame for this. Same pixels: the image does not change in that draw");
REXCVAR_DEFINE_BOOL(banjo_deko3d_depth_read_layout, true, "Banjo/Enhancements",
                    "deko3d renderer: a depth target just sampled stays in the read layout while draws that write neither "
                    "depth nor stencil use it: no barrier each way between them. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_resolve_keeps_target, true, "Banjo/Enhancements",
                    "deko3d renderer: a colour resolve that draws nothing leaves the render targets bound. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_renut_present_no_pow, true, "Banjo/Enhancements",
                    "The scan-out skips its pow at an output gamma of 1. Its inputs are 8-bit values and its output is "
                    "8-bit, so the result rounds the same: same pixels");
REXCVAR_DEFINE_BOOL(banjo_renut_present_texel_load, true, "Banjo/Enhancements",
                    "The scan-out loads the texel when the frame covers the output one to one, instead of filtering at "
                    "its centre: same pixels");
REXCVAR_DEFINE_BOOL(banjo_renut_gamma_upload_once, true, "Banjo/Enhancements",
                    "The game's gamma ramp is copied to its texture when it changes, and every frame only for the first "
                    "300 (a frame's list can be dropped at start-up). The copy, every frame, cost a full GPU barrier "
                    "before it and another with an L2 invalidate after it. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_depth_handover_readonly, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-28, GPU block): a handed-over depth resolve (banjo_deko3d_depth_handover) "
                    "is not drawn when its target is bound again for draws that write neither depth nor stencil (the "
                    "image stays as the resolve read it); only before a draw that writes them, a draw that samples it "
                    "with the target bound, or the target's next whole clear (the texture takes the image). The "
                    "night_final city drew all 438 handed resolves a 10 s after all at such binds. Same pixels. "
                    "In-run A/B target 64");
REXCVAR_DEFINE_BOOL(banjo_deko3d_depth24, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-19, GPU): a D24S8 depth target (the shadow maps, small render-to-texture "
                    "targets) as 24-bit unorm depth with stencil, the Xenos's own format, instead of 32-bit float: half "
                    "the bytes written by its draws and read by its resolves and samples. Its depth is then the 24-bit "
                    "value the Xbox 360 keeps (the float target held more bits than the console). The 20e4 float depth "
                    "(D24FS8) stays 32-bit float");
REXCVAR_DEFINE_BOOL(banjo_deko3d_push_constants, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-19, render-thread proposals 2 and 5): a draw's float constants and "
                    "SharedConstants go into three fixed uniform buffers as pipelined updates (dkCmdBufPushConstants) "
                    "of the 64-byte groups that differ from what the GPU holds, instead of a new ring allocation of "
                    "the whole set per draw (4,352 and 1,344 bytes), cleaned at the submit and bound again. Same values "
                    "for every register a shader reads");
REXCVAR_DEFINE_BOOL(banjo_deko3d_push_constants_verify, false, "Banjo/Diagnostics",
                    "banjo_deko3d_push_constants: after every draw's updates, checks that the model of the uniform "
                    "buffers holds the guest's value of every register the shaders read (and their literals), and "
                    "logs a draw where it does not (a test: slower)");
// renut-ultimate (2026-11-16, port 145; tw2de_nx's tw2de_async_present of 2026-11-13, ON at the user's word: "port the
// speed ups as on by default"). Read by name in sdk/src/ui/switch_perf.cpp at the first present.
REXCVAR_DEFINE_BOOL(banjo_async_present, true, "Banjo/Enhancements",
                    "Switch (tw2de_nx): the window's two binder calls of a present (queue the image, dequeue the next "
                    "one: 2.7 ms a frame in tw2de_nx's heaviest stretch) are made by a helper thread, so the renderer's "
                    "feed thread records the next frame instead of waiting for the compositor's answers. The same calls "
                    "in the same order. false: on the presenting thread, as before");
// banjo-nx (2026-11-21): off on the Switch (the user, after console c41: with it the render thread +1.7 % a draw, the GPU
// +1.65 % a draw, -2.3 % fps in Showdown Town; what it buys is input latency).
REXCVAR_DEFINE_BOOL(banjo_deko3d_kick_submit, BANJO_SWITCH_DEFAULT(false, true), "Banjo/Enhancements",
                    "deko3d renderer (2026-10-19, input latency): at the game's kicks (several a frame) the command "
                    "list recorded so far is ended and handed to the GPU at the next kick, instead of the whole frame "
                    "at the present: the GPU starts a frame's work while the render thread still records the rest, so "
                    "the frame (and the controller input in it) reaches the screen sooner. At most 3 more lists a "
                    "frame, each of at least 200 draws. Same commands in the same order");
REXCVAR_DEFINE_BOOL(banjo_deko3d_latency_trim, false, "Banjo/Enhancements",
                    "Input latency (2026-10-20, deko3d, no vsync): where the GPU is the limit a recorded frame waited in "
                    "the GPU's queue behind the one running (the GPU's frame time less the recording time: 30 ms and "
                    "more in the heaviest scenes) and the game had read its controller that much earlier than needed. "
                    "On: after each present the render thread waits until the GPU has finished all but the frame's last "
                    "part (a fence at each kick submission), so the next frame is recorded, and the game thread "
                    "released, just in time; if the GPU then runs dry it stops for 60 frames. The [latency] line's "
                    "\"input -> GPU done\" and the \"latency trim\" line say what it does. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_late_input, true, "Banjo/Enhancements",
                    "Input latency (2026-10-19): when the render thread is the slower one, the game's frame waits at "
                    "its Swap for the frame before; its controller read is delayed by the least such wait of the last "
                    "16 frames, less 2 ms, so the frame starts later with newer input and still reaches the render "
                    "thread in time (backend/render_thread.cpp). No wait (the game thread or the frame cap the limit): "
                    "no delay. The [latency] line every 10 s gives input -> swap -> present");
REXCVAR_DEFINE_BOOL(banjo_deko3d_ring_cleaner, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-19, render-thread proposal 23): the upload ring's cache clean (what the "
                    "CPU wrote, written out for the GPU before each submit) done at each kick by a thread on the "
                    "background core instead of by the render thread at the submit; a range the thread has not "
                    "started when the submit needs it is cleaned by the render thread. Same memory for the GPU");
REXCVAR_DEFINE_BOOL(banjo_renut_store_cards, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-19): cached vertex and index copies proven by the store cards (every "
                    "guest store outside the stack marks its 4 KB page, tools/store_cards.py) instead of hashing them "
                    "at their first use after every kick; a page written since is hashed as before. Only with "
                    "banjo_renut_exact_streams, and only when the generated code marks the cards. The same pixels");
REXCVAR_DEFINE_BOOL(banjo_renut_store_cards_verify, false, "Banjo/Diagnostics",
                    "PC test of banjo_renut_store_cards: every copy the cards prove is hashed as well, and a copy whose "
                    "bytes changed under clean cards is logged (a writer of guest memory that marks none) and hashed");
REXCVAR_DEFINE_BOOL(banjo_renut_cpu_memos, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-19): the render thread's per-draw lookups memoised (render targets, "
                    "texture bindings, vertex streams, memexport checks), each keyed by everything its lookup reads and "
                    "an epoch bumped by every change of what it reads. The same results");
REXCVAR_DEFINE_BOOL(banjo_renut_pipeline_memo, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-19): a 64-entry memo of the pipeline each key gave (built, or the generic "
                    "one a specialisation draws with while pending or after failing), before the key's XXH3 and the "
                    "map. The same pipelines");
REXCVAR_DEFINE_BOOL(banjo_renut_resolve_skip, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-19): a resolve the same as the last one into its texture (same source, "
                    "rectangle and place) whose source nothing has written since (no draw that can write it, no clear) "
                    "is not done again: the texture already holds it. The count is in the 10 s depth resolve line. Same "
                    "pixels");
REXCVAR_DEFINE_INT32(banjo_deko3d_compile_priority, 0x3E, "Banjo/Enhancements",
                     "deko3d renderer (2026-10-18): the uam shader compile thread's priority (Horizon: higher is lower; "
                     "the game's threads are 0x3B, the render thread 0x2C), on banjo_background_compile_core; a thread "
                     "waiting for a compile lends its priority. 0: as created (any core). 2026-10-23: an application's "
                     "lowest is 0x3B, so 0x3E was refused (the thread ran at 0x3B); the lowest allowed is taken and the "
                     "log says which (banjo_deko3d_background_compile_share keeps it off the game's CPU)");
REXCVAR_DEFINE_INT32(banjo_renut_prebuild_release_s, 180, "Banjo/Enhancements",
                     "Renut's renderer (2026-10-28, the heap): pipelines the prebuild made from the list that no draw "
                     "has taken for this many seconds go back to the heap (the list holds every level's pipelines: "
                     "12,953 of 16,358 unused in the d4 city, the heap at 2.9 of 3.08 GB). Their jobs stay: a draw "
                     "that asks later has it built again (a specialised one draws with the generic pipeline "
                     "meanwhile: the same pixels). 0: they stay built")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_deko3d_background_compile_share, 10, "Banjo/Enhancements",
                     "deko3d renderer (2026-10-23): the share of its core (percent, 1-100) the background shader "
                     "compiles may use. The uam thread cannot run below the game's threads (0x3B is an application's "
                     "lowest priority) and took about half of core 2 from them while a backlog compiled; after each "
                     "background compile it now rests in proportion (at most 30 s). A compile a draw waits for ends the "
                     "rest and is never held back. 100: no rests. Same pixels (the generic variant draws meanwhile). "
                     "2026-10-28: 10 (was 25): the c4 console runs compiled run-time variants without end (50 and 135 "
                     "a run, about 2.1 s each), a quarter of core 2 taken from saturated game cores");
REXCVAR_DEFINE_BOOL(banjo_deko3d_async_cache_reads, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-23, the console's stutters on entering an area: hundreds of new pipelines, "
                    "each variant read from deko3d_shaders.bin on the card by the render thread): a variant the cache "
                    "file holds is read on the uam thread, ahead of the compiles, while the draw uses the shader's "
                    "generic variant (as banjo_deko3d_async_variants does for one that compiles). Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_compile_wait_boost, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-23, the user's request): while a draw waits for a shader compile (the "
                    "game stopped: a blocking compile, not a background one), the console in boost mode "
                    "(appletSetCpuBoostMode FastLoad: CPU 1785 MHz, the GPU at 76.8 MHz, which has nothing new to do "
                    "while the frame waits), normal again the moment the compile is done. Background compiles and "
                    "those the pipeline prebuild waits for never boost. The deko3d report line counts the boosts");
REXCVAR_DEFINE_BOOL(banjo_deko3d_fast_first_compile, true, "Banjo/Enhancements",
                    "deko3d renderer (mk9-nx 2026-10-24; port 56): a shader compile a draw waits for (a generic variant "
                    "missing from the DKSH cache; the game stopped) is a single back-end pass, about a quarter of a "
                    "pixel shader's full compile, and is stored in the cache as it is. Background compiles stay full. "
                    "false: every compile full");
REXCVAR_DEFINE_BOOL(banjo_deko3d_barrier_merge, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-19, GPU): barriers recorded one after another with no GPU work between "
                    "them are emitted as one (the strongest wait, every invalidation) right before the next command "
                    "that does work: one pipeline drain instead of several. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_async_variants, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-21, after a console freeze in a run-time shader compile that never ended): "
                    "a shader variant missing from the DKSH cache is compiled in the background while its draws use the "
                    "shader's generic variant (the same pixels: it reads at run time what the specialised one folds), "
                    "and the pipeline takes its own variant once it is done. No draw waits for a compile; the generic "
                    "variants of every game shader are in the cache (tools/deko3d_variant_cache.py --generic). Off: the "
                    "draw waits for the compile, as before. Read at start");
REXCVAR_DEFINE_BOOL(banjo_deko3d_texture_skip, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-27, render thread night, A/B 39): a draw whose pipeline, descriptors and "
                    "texture words (the shared constant lines its bindings read) are those of plume's last bindTextures "
                    "run binds nothing and reads no handle (each colour draw read every binding's words before). Same "
                    "bindings. Read at start");
REXCVAR_DEFINE_BOOL(banjo_deko3d_provisional_count, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-28, render-thread round, A/B 68): plume counts its provisional pipelines "
                    "(those still waiting for a background shader variant); while there are none, a draw does not read "
                    "its pipeline object to ask (0.9 % of the render thread in night_gpu5's profile). The same answer. "
                    "Read at start");
REXCVAR_DEFINE_BOOL(banjo_deko3d_vtx_dedupe, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-26, Unleashed-style step 2): a vertex buffer slot bound again with the "
                    "range it holds in the command list is not sent again (every draw bound each of its streams and the "
                    "zero stream). Same bindings. Read at start");
REXCVAR_DEFINE_BOOL(banjo_deko3d_barrier_lean, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-19, GPU): a transition into a layout that is only written next keeps "
                    "the texture cache (only a read after a write needs it invalidated, and that read's transition "
                    "does it), and a texture going from one read layout to another needs no barrier at all. Same "
                    "pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_zcull, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-18): hierarchical depth culling (Zcull) on the queue: tiles a draw cannot "
                    "pass the depth test in are skipped before shading. Same pixels");
REXCVAR_DEFINE_BOOL(banjo_deko3d_msaa_positions, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-30, from sasrt-nx): a multisampled target's pipelines get the "
                    "standard sample positions for their sample count (dkMultisampleStateSetLocations, as nouveau). "
                    "Off: deko3d's default, every sample at the pixel's centre, so 2x/4x targets have no edge "
                    "antialiasing on the console (Ryujinx keeps the standard positions either way)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_a2c_dither, false, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-30, from sasrt-nx): the hardware's alpha-to-coverage dither (deko3d "
                    "turns it on by default). On: a fixed screen-space grid where alpha to coverage draws on a "
                    "multisampled target; off: the coverage from alpha alone, as the Vulkan and D3D12 back ends")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_strip_restart, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-30, from sasrt-nx): an indexed triangle or line strip starts again at "
                    "the all-ones index (0xFFFF for 16-bit indices), as the Vulkan back end and D3D. Off: deko3d's "
                    "default, no restart, so a cut joins the strips with stray triangles to vertex 65535")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_gpu_region, true, "Banjo/Enhancements",
                    "deko3d renderer (2026-10-28, h10, after the night_gpu2 console freeze): GPU memory blocks of 64 KB "
                    "and more from a region of their own at the heap's top, growing down as needed, not from the heap "
                    "among every small allocation (where freed blocks were cut up: 577 MB free and no 32 MB piece). "
                    "Same pixels");
REXCVAR_DEFINE_BOOL(banjo_renut_cache_save_quiet, true, "Banjo/Enhancements",
                    "Renut's renderer (package 28; as Unleashed saves its pipeline cache at loading screens): the "
                    "pipeline cache is saved once no pipeline was built for 15 s (and at most once a minute; after "
                    "5 minutes of steady compiling regardless), not every minute with new pipelines: the save holds "
                    "the driver's cache lock, which a compile on the render thread waits for. Same pipelines");

#if REX_PLATFORM_SWITCH
// The two contracts with the linked NVK (mesa-switch-main-nfsmw-source: nvk_cmd_buffer.h, struct nvk_switch_dibujo and
// struct nvk_switch_set4), field for field as banjo-nx's renderer declares them (masseffect_native_draws.cpp; the same
// definitions, so one type each across the program). Weak: an NVK without them leaves the addresses null.
extern "C" {
struct NvkSwitchSet4Counts {
  uint64_t bindings_difference;
  uint64_t complete_bindings;
  uint64_t writes_root;
  uint64_t dwords_root;
  uint64_t cbufs_dirty;
  uint64_t cbufs_saved;
  uint64_t draws;
  uint64_t checks;
  uint64_t differences;
};
struct NvkSwitchSet4 {
  int32_t version;
  int32_t request;
  int32_t environment;
  int32_t off;
  NvkSwitchSet4Counts total;
};
extern NvkSwitchSet4 nvk_switch_set4 __attribute__((weak));
struct NvkSwPart {
  uint64_t times;
  uint64_t ticks;  // CNTPCT_EL0, a ticks_per_second
};
struct NvkSwImprovement {
  int32_t request;  // written by the app: 1 yes, 0 no
  int32_t off;  // 1: its guard saw a MISMATCH
  uint64_t uses;
  uint64_t validated;
  uint64_t differences;
  uint64_t no_check;
};
struct NvkSwitchDraw {
  int32_t version;
  int32_t measure;
  int32_t measure_failures;
  int32_t environment;
  uint64_t ticks_per_second;
  NvkSwPart parts[16];   // enum nvk_sw_part
  uint64_t counts[13];    // enum nvk_sw_count
  NvkSwImprovement improvements[5];  // emission, cbufs, dynamic, set4_fast (not applied), prefetch
};
extern NvkSwitchDraw nvk_switch_draw __attribute__((weak));
extern NvkSwitchDraw nvk_switch_dibujo __attribute__((weak));
void vk_switch_preload_pipeline(VkPipeline pipeline) __attribute__((weak));
}
static_assert(sizeof(NvkSwitchSet4) == 16 + 9 * 8, "NvkSwitchSet4 must have the same size as in NVK");
static_assert(sizeof(NvkSwitchDraw) == 24 + 16 * 16 + 13 * 8 + 5 * 40, "NvkSwitchDraw: same size as in NVK");
#endif

REXCVAR_DEFINE_BOOL(banjo_renut_pipeline_cache, true, "Banjo/Enhancements",
                    "Renut's renderer (Vulkan): the driver's compiled pipelines persist in cache/renut_pipelines.bin "
                    "(plume's pipeline cache), so the next start builds the listed pipelines from the file. Saved "
                    "when new pipelines were compiled, at most once a minute, and at exit. Same pipelines");
REXCVAR_DEFINE_BOOL(banjo_renut_pmull_hash, true, "Banjo/Enhancements",
                    "Renut's renderer, ARM only: its per-frame content hashes of guest data (vertex streams, index "
                    "ranges, texture samples) use banjo-nx's PMULL-folded 64-bit CRC instead of XXH3, half the "
                    "instructions per byte on the A57. Checked against the bit-by-bit reference at start");
REXCVAR_DEFINE_BOOL(banjo_renut_upload_cached, true, "Banjo/Enhancements",
                    "Renut's renderer, the Switch only: its upload ring (every draw's constants, converted vertices "
                    "and indices, texture uploads) in CPU-cached memory, flushed before each submission, instead of "
                    "write-combined memory: the A57 wrote 5,251 MB/s there against 2,506 (sa2-nx, banjo-nx's "
                    "masseffect_native_upload_memory = 1). On the PC's discrete GPU cached memory is slower to read, "
                    "so the PC keeps write-combined. Same bytes");
REXCVAR_DEFINE_INT32(banjo_renut_ab_target, 0, "Banjo",
                     "Renut's renderer, in-run A/B: 0 none; 1 banjo_renut_precise_barriers; 2 the render thread's pin "
                     "to banjo_ring_core (the Switch); 3 banjo_renut_shared_dedupe; 4 banjo_renut_push_point_only; "
                     "5 banjo_renut_capture_pool; 6 banjo_renut_exact_streams (what the exact rule costs); 7 "
                     "banjo_gpu_bool_spec (the generic pipelines in the off half); 8 banjo_gpu_blend_skip; 9 "
                     "banjo_gpu_tess_grid; 10 none (was banjo_renut_check_worker, removed in package 28b); 11 banjo_gpu_depth_only; 12 banjo_gpu_output_trim; 13 "
                     "banjo_gpu_depth_words_view; 14 banjo_renut_record_worker; 15 banjo_deko3d_light_branch, 16 "
                     "banjo_deko3d_gpr32 and 17 banjo_deko3d_skin_once (2026-10-20: the key on; the off half's "
                     "pipelines are built without its constant); 18 banjo_deko3d_eager_kicks; 19 "
                     "banjo_renut_smaa (2026-10-21; was banjo_late_input_gpu until 36s); 256-511 a port's own targets (the base's are 1-255). The part is "
                     "switched off and on every banjo_renut_ab_s seconds; the \"[renut A/B]\" line every 10 s gives "
                     "both halves' frame rate, render thread and GPU time")
    .range(0, 511);  // banjo-nx (2026-10-27): was 19, which refused every target from 20 on; 2026-10-28: 127 (64, 65);
                     // sasrt-nx (2026-11-08): 255 (the third GPU round's targets, 120 and up)
                     // renut-ultimate (2026-11-19, port 188; sasrt-nx's sync: its own 240 met the base's 240, Blur's
                     // #36): 511. The base's targets are 1-255; 256-511 are a port's own, and the base never takes a
                     // number there (an item it takes from a port gets a free number below 256)
REXCVAR_DEFINE_INT32(banjo_renut_ab_s, 4, "Banjo", "Renut's renderer: the half-period of banjo_renut_ab_target, in seconds")
    .range(1, 60);
REXCVAR_DEFINE_STRING(banjo_renut_ab_targets, "", "Banjo",
                      "Renut's renderer, in-run A/B over several targets (2026-10-20): a list of banjo_renut_ab_target "
                      "numbers (\"15,16\"); each in turn is switched off and on for one banjo_renut_ab_s period each, "
                      "the next one after, and the 10 s report has a \"[renut A/B]\" line for each with its halves "
                      "so far. One console run measures them all. Empty: banjo_renut_ab_target alone");
// blur-nx (2026-11-10/11, port 76; its keys 211-221, 210 and 214 left out): the renderer fixes blur-nx sent to the base,
// each with blur-nx's name, A/B number and default.
REXCVAR_DEFINE_BOOL(blur_renut_expand_alias, true, "Banjo/Renut",
                    "From blur-nx (2026-11-10, port 76; A/B 211): a resolve destination written as 16_EXPAND / "
                    "16_16_EXPAND / 16_16_16_16_EXPAND and sampled through another header as 16_FLOAT / 16_16_FLOAT / "
                    "16_16_16_16_FLOAT (or the other way) is the same data: the reference's texture cache reads both as "
                    "half floats. Blur's 640x360 HDR downsample is resolved as one and read as the other (its bloom). Off: "
                    "such a texture is made from guest memory, which the resolve never wrote")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_resolve_keep_dest, true, "Banjo/Renut",
                    "From blur-nx (2026-11-10, port 76; A/B 212): a single-sampled resolve that covers part of an existing "
                    "destination made from another format keeps that destination and writes its part with a draw that "
                    "converts, as the console writes the resolve into the destination's own format. Blur resolves a 1x1 "
                    "2_10_10_10 target into the corner of its 640x360 half-float image each frame: without this the image "
                    "was made again (empty) and the post pass that reads last frame's image read nothing. Off: the "
                    "destination is made again in the source's format (Renut's rule)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_occlusion_queries, true, "Banjo/Renut",
                    "From blur-nx (2026-11-10/11, port 76; A/B 213): occlusion queries answered as the reference's GPU "
                    "answers them. D3DQuery_Issue(END) writes 0xFFFFFEED into the query's sample counts and a ZPASS event "
                    "the GPU replaces it with the counts at; Renut runs no GPU packets, so the counts never came and "
                    "D3DQuery_GetData said \"not ready\" for ever (Blur's light glows were never drawn). On: GetData of an "
                    "issued occlusion query finds its blocks where GetData reads them and writes what the emulated GPU "
                    "writes (the SDK's command_processor.cpp, EVENT_WRITE_ZPD: zeroed, 1000 samples passed). Only for a "
                    "game that registers its GetData (d3d/hooks/state.cpp, RENUT_D3DQUERY_GETDATA_HOOK); Beautiful "
                    "Katamari links none. Off: Renut as it was")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_stack_surface_headers, true, "Banjo/Renut",
                    "From blur-nx (2026-11-10, port 76; A/B 215): a render target or depth surface whose header is on the "
                    "stack (a temporary D3DSurface the game builds for one clear and resolve) keeps the header words as "
                    "they were when the game bound it, per queued or recorded operation: the render thread reads surface "
                    "headers later, when the stack has been reused (Blur's 640x360 motion-blur source read as an empty "
                    "1x1 surface: no motion blur, the frame darkened by the composite). Off: the header read at replay")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_sample_grid_resolve, true, "Banjo/Renut",
                    "From blur-nx (2026-11-10, port 76; A/B 216): a 4x multisampled surface over the EDRAM of a "
                    "single-sampled one of the same format and twice its pitch (the same sample grid: each 2x2 block of "
                    "the big surface's pixels is one pixel's four samples; 2x: 1x2) resolves as the console does, the box "
                    "of the big surface when that was drawn last. Blur makes its 640x360 half-size scene (the motion "
                    "blur's and the light trails' source) this way from its 1280x720 post target. Off: the view's own "
                    "target (black)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_surface_size_guard, true, "Banjo/Renut",
                    "From blur-nx (2026-11-10, port 76; A/B 217): a surface header that reads wider or taller than 8192 "
                    "(no Xbox 360 target is) makes no host target: the header is not one (Blur's temporary surfaces on "
                    "the stack, read after the stack was reused, came out as 12120x26733 and the like; in Ryujinx each "
                    "failed allocation set off a memory emergency). Off: Renut tries")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// renut-ultimate (2026-11-11, port 77; the user's rule: a key that breaks graphics in a specific game is off by
// default): vivapinata-nx's fade frames (a clear through another layout at the same EDRAM base, which covers every tile
// of the mask's target) came out 5-8 % white with this on, 0 % off as on the SDK's emulated GPU. blur-nx sets it true.
REXCVAR_DEFINE_BOOL(blur_renut_wipe_overwritten, false, "Banjo/Renut",
                    "From blur-nx (2026-11-10, port 76; A/B 218): vivapinata-nx's EDRAM wipe (a clear through another "
                    "layout, applied at the wiped target's next resolve when nothing wrote it since) is left out when "
                    "another target over the same EDRAM tiles was drawn or cleared after the wipe: on the console that "
                    "write is in the EDRAM the resolve reads; and a clear wipes a target only when it fills every one of "
                    "its tiles (exact tile spans). Blur clears its 640x360 view, draws the scene through the 1280x720 one "
                    "and resolves the 640x360 view: the wipe blackened the frame. Off: as vivapinata-nx wrote it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_resolve_exp_bias, true, "Banjo/Renut",
                    "From blur-nx (2026-11-10, port 76; A/B 219): a colour resolve's exponent bias, as the console "
                    "applies it: the Resolve flags' top six bits (D3DRESOLVE_EXPONENTBIAS, signed; D3D puts them in "
                    "RB_COPY_DEST_INFO's copy_dest_exp_bias, which the SDK's resolve applies) scale the colour by 2^bias. "
                    "Renut copied without it while sasrt_renut_texture_exp_adjust applies the fetch's exponent adjust "
                    "that undoes it: Blur resolves its light-glow chain with +11 and reads it with -11, so the glows were "
                    "2048 times too dim. Off: no bias")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_ms_slice_resolve, true, "Banjo/Renut",
                    "From blur-nx (2026-11-11, port 76; A/B 221): a multisampled resolve into a slice (a cube face, an "
                    "array layer) or a volume's depth other than 0 goes through a single-sampled staging image and a "
                    "copy into that slice. plume's region resolve has no slice: every face landed in face 0. Blur renders "
                    "its 256x256 environment cube 4x and resolves the six faces: five were black. Off: as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// renut-ultimate (2026-11-11, port 86; the user's rule: a key that breaks graphics in a specific game is off by default):
// on the console (deko3d) it drew a band of garbage tiles over the top 960x48 pixels of vivapinata-nx's title screen
// (12x3 EDRAM tiles of 80x16, dark scrambled scene content), at first entry and after every attract fly-through; off
// (alone or with ports 75-76's other keys), no strip; the PC (D3D12) shows none either way, so it is the deko3d side
// (port 76 added the multisampled source's bind there, never run before). blur-nx sets it true.
REXCVAR_DEFINE_BOOL(blur_renut_edram_inherit, false, "Banjo/Renut",
                    "From blur-nx (2026-11-11, port 76; A/B 220): before a draw that keeps part of its target (a write "
                    "mask without every channel, or blending), the target takes what other views wrote over its EDRAM "
                    "tiles since its own last write, through the console's tile and sample layout "
                    "(edram_inherit_ps.hlsl). On the console every view of the same EDRAM sees one memory; here each view "
                    "is its own host target. Blur draws its environment panorama's RGB through a view of a target cleared "
                    "to alpha 1: without it the view's alpha was whatever its own host target last held (the ground and "
                    "car about half as bright, the rear window black). A copy of the target each time (Blur: about 1.4 "
                    "a frame). Off: each view keeps only what it drew")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_push_point_only, true, "Banjo/Enhancements",
                    "Renut's renderer (Vulkan): with the constants in uniform buffers, the push constant (the three "
                    "constant addresses) is read only by the point sprites' geometry shader, so only point-list draws "
                    "push it, not every draw (NVK: a root-table upload a draw less). Same output");
REXCVAR_DEFINE_BOOL(banjo_renut_capture_pool, true, "Banjo/Enhancements",
                    "Renut's renderer: a queued draw's capture (its 12.7 KB device snapshot and the closure the render "
                    "thread runs) comes from a pool and goes back to it, instead of three heap allocations per draw on "
                    "the game thread freed on the render thread (the Switch's malloc has one lock). Same data");
REXCVAR_DEFINE_BOOL(banjo_renut_neon_swap, true, "Banjo/Enhancements",
                    "Renut's renderer, ARM only: the vertex conversion's byte swaps (guest big-endian words to the "
                    "host's) 16 bytes per NEON byte reverse instead of a word at a time. Same bytes");
REXCVAR_DEFINE_BOOL(banjo_renut_shared_dedupe, true, "Banjo/Enhancements",
                    "Renut's renderer: a draw whose SharedConstants (texture and sampler slots, booleans, render "
                    "state; 1,344 bytes) equal the previous draw's on the same command list reuses that upload: no "
                    "copy into upload memory and no descriptor set bind (NVK: a root-table update). Exact compare");
REXCVAR_DEFINE_BOOL(banjo_renut_queue_notify_idle, true, "Banjo/Enhancements",
                    "Renut's renderer: work queued for the render thread (every draw) wakes it only while it waits, "
                    "instead of a condition-variable notify per item (a kernel call on the Switch, about 1,500 a frame "
                    "from the game threads). The render thread checks the queue under the same lock before it sleeps");
REXCVAR_DEFINE_BOOL(banjo_renut_linked_stages, true, "Banjo/Enhancements",
                    "Renut's renderer, the Switch: NVK compiles each pipeline's stages together (its default), not "
                    "with the per-stage keys of masseffect_nvk_cache_per_stage (disable_lto), so the vertex outputs the "
                    "pixel shader does not read are dropped: Renut's vertex shaders write every output, zeros for the "
                    "unused ones. More compiling, kept by banjo_renut_pipeline_cache. Same image");
REXCVAR_DEFINE_BOOL(banjo_renut_exact_streams, true, "Banjo",
                    "Renut's renderer: a cached copy of a vertex stream or index range is hashed whole on its first use "
                    "after each kick (where the game may learn the GPU is done with a buffer and rewrite it) and "
                    "converted again if its guest bytes changed, instead of Renut's check on a background thread once "
                    "every 4 frames, after which a rewritten range could draw from its old copy for about 5 frames. "
                    "The exact rule (banjo-nx's renderer proves its persistent vertex blocks the same way); not under "
                    "a master switch");
#if REX_PLATFORM_SWITCH
constexpr bool kMemoryPolicyDefault = true;
#else
constexpr bool kMemoryPolicyDefault = false;
#endif
REXCVAR_DEFINE_BOOL(banjo_renut_memory_policy, kMemoryPolicyDefault, "Banjo",
                    "Renut's renderer: its memory limits sized for the console (on by default on the Switch, where NVK "
                    "gives about 1,280 MB of GPU memory budget; Renut sized them for a PC): idle host textures released "
                    "below banjo_renut_textures_reserve_mb of budget left, the stream cache capped at "
                    "banjo_renut_stream_cache_mb and emptied of copies idle banjo_renut_stream_cache_idle_s, upload ring "
                    "chunks of a heavy frame released after banjo_renut_upload_trim_frames. The same picture: a released "
                    "texture or copy is made again from guest memory. Not under a master switch");
REXCVAR_DEFINE_INT32(banjo_renut_textures_reserve_mb, 192, "Banjo",
                     "banjo_renut_memory_policy: with less than this much of the GPU memory budget left, or the "
                     "bindless table three quarters full (checked every 32 frames), the host textures not bound for "
                     "120 frames are released (made again from guest memory when bound). Never a resolve destination "
                     "(banjo_renut_keep_gpu_written). 0: Renut's rule only (85 % of the budget, 600 frames idle, "
                     "checked every 120)")
    .range(0, 16384);
REXCVAR_DEFINE_BOOL(banjo_renut_keep_gpu_written, true, "Banjo",
                    "Renut's renderer: a host texture the GPU wrote (a resolve destination; Renut writes nothing back "
                    "to guest memory) is never released by an eviction rule, as it could not be made again from guest "
                    "memory (Renut's backstop released them after 3,600 idle frames). Replaced as before when the game "
                    "frees or reuses their memory. The exact rule: not under a master switch");
REXCVAR_DEFINE_BOOL(banjo_renut_diag_gpu_shaders, false, "Banjo/Diagnostics",
                    "Renut's renderer, a measurement (package 28b): renut_gpu_profile's passes split by pixel shader "
                    "and blending (a timestamp where the shader changes), with draws a frame, 60 lines: the main "
                    "1280x720 pass broken down in ms. The timestamps cost some GPU and render thread time");
REXCVAR_DEFINE_BOOL(banjo_renut_diag_trim_poison, false, "Banjo/Diagnostics",
                    "Renut's renderer, a test of banjo_gpu_output_trim (package 28): the channels it trims are 60000 "
                    "instead of 0, so a channel trimmed wrongly shows; the image must be the same as without the test");
REXCVAR_DEFINE_BOOL(banjo_renut_diag_resolve_trace, false, "Banjo/Diagnostics",
                    "Renut's renderer, a measurement (package 28): one frame every 30 s, every resolve (source and "
                    "destination), clear, change of render targets (with its draws) and first sample of a resolved "
                    "texture, as \"[trace]\" lines");
REXCVAR_DEFINE_INT32(banjo_renut_diag_build_delay_ms, 0, "Banjo/Diagnostics",
                     "Renut's renderer, a test (package 27c): every pipeline build waits this long first, on whichever "
                     "thread builds it, so the PC compiles as slowly as the console (the one-frame flashes seen there "
                     "around compiles)")
    .range(0, 2000);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_poison_slots, false, "Banjo/Diagnostics",
                    "Renut's renderer, a test: descriptor slots given back point at a magenta texture instead of the "
                    "null one, so a draw still sampling a released texture shows magenta (sa2-nx's poison test). With "
                    "a large banjo_renut_textures_reserve_mb it checks the eviction rules");
REXCVAR_DEFINE_INT32(banjo_renut_stream_cache_mb, 128, "Banjo",
                     "banjo_renut_memory_policy: the converted-stream cache (vertex streams and index ranges kept "
                     "across frames) takes no more copies past this size (Renut: 768 MB)")
    .range(16, 768);
REXCVAR_DEFINE_BOOL(banjo_renut_stream_cache_pressure, true, "Banjo",
                    "banjo_renut_memory_policy (2026-10-23): when copies were refused at banjo_renut_stream_cache_mb, "
                    "the next 60-frame pass also drops the copies no draw read for 60 frames, so the scene being drawn "
                    "gets its converted streams kept instead of converted again every frame until the old scene's go "
                    "idle (a console run converted 15-40 MB of vertices a frame for over a minute after leaving the "
                    "city). Same pixels: a dropped copy is converted again from guest memory when drawn");
REXCVAR_DEFINE_INT32(banjo_renut_stream_cache_idle_s, 20, "Banjo",
                     "banjo_renut_memory_policy: a cached copy no draw read for this many seconds (at 30 frames a "
                     "second) goes (Renut: 120 s)")
    .range(2, 120);
REXCVAR_DEFINE_INT32(banjo_renut_upload_trim_frames, 900, "Banjo",
                     "banjo_renut_memory_policy: upload ring chunks beyond what the frames since the last trim used "
                     "(a loading frame's texture uploads grow the ring by 32 MB chunks) are released after this many "
                     "frames (Renut: 600). banjo-nx (2026-10-19, render-thread proposal 18): 900 (was 120): a chunk "
                     "released after 60 frames without a heavy frame was made again by the next one on the render "
                     "thread, mid-draw (a 32 MB heap allocation and its GPU mapping)")
    .range(8, 3600);
REXCVAR_DEFINE_BOOL(banjo_renut_precise_barriers, true, "Banjo/Enhancements",
                    "Renut's renderer (Vulkan): plume's barriers take their access masks from the layouts and the "
                    "declared accesses instead of MEMORY_READ | MEMORY_WRITE on both sides, which NVK turns into "
                    "cache flushes and invalidations at every barrier. Same ordering for every access that exists");

namespace banjo::renut_gpu {

// banjo_renut_shared_dedupe's counts (render thread), read by the [renut] report (game thread).
std::atomic<uint64_t> g_shared_reused{0}, g_shared_uploaded{0};
uint64_t g_ab_draws = 0;  // sasrt-nx (2026-11-09; port 54): every draw (CountSharedConstants), for the A/B halves
// banjo_renut_exact_streams' counts and banjo_renut_memory_policy's evictions, for the same report.
std::atomic<uint64_t> g_exact_checks{0}, g_exact_bytes{0}, g_exact_stale{0}, g_late_stale{0}, g_reserve_evicted{0};
std::atomic<uint64_t> g_cache_refused{0}, g_cache_pressure_bytes{0};
// Package 27c: how the checks spread over a frame (the design of cheaper ones): checks of a copy already checked in the
// same frame, the kicks that start the intervals, and the game's fence queries.
std::atomic<uint64_t> g_exact_repeat{0}, g_exact_repeat_bytes{0}, g_kicks{0}, g_fence_queries{0};
std::atomic<uint64_t> g_exact_memo{0}, g_exact_memo_bytes{0};
// banjo-nx (2026-10-19): banjo_renut_store_cards' checks.
std::atomic<uint64_t> g_cards_clean{0}, g_cards_clean_bytes{0}, g_cards_dirty{0}, g_cards_missed{0};
std::atomic<uint64_t> g_kick_reuse[3]{}, g_kick_reuse_bytes[2]{};  // banjo-nx (2026-10-23, batch 6)
std::atomic<uint64_t> g_tex_cards[4], g_tex_cards_bytes{0};  // proven, hashed, re-uploads saved, changed (verify)
std::atomic<uint64_t> g_kind_depth_only{0}, g_kind_depth_plain{0}, g_kind_nothing{0}, g_kind_partial{0};
// blur-nx (2026-11-13, #47's census): CountConstantUse.
std::atomic<uint64_t> g_const_vs_indexed{0}, g_const_ps_indexed{0}, g_const_draws{0}, g_const_registers{0};
std::atomic<uint64_t> g_depth_only_draws{0};  // banjo_gpu_depth_only (package 28)
std::atomic<uint64_t> g_resolves_drawn{0};  // banjo_deko3d_draw_resolves (2026-10-18)
std::atomic<uint64_t> g_smaa_runs{0};       // banjo_renut_smaa (2026-10-21)
std::atomic<uint64_t> g_ui_upscales[2]{};    // internal resolution (2026-10-21): composites scaled up, drawn at full size
std::atomic<uint64_t> g_ui_depth[2]{};       // internal resolution: UI depth targets filled, draws kept a smaller one
std::atomic<uint64_t> g_resolves_skipped[2];  // banjo_renut_resolve_skip (2026-10-19): colour, depth
std::atomic<uint64_t> g_words_lazy[3];  // banjo_renut_depth_words_lazy: not written, written late, read stale
std::atomic<uint64_t> g_handover[10];   // banjo_deko3d_depth_handover (CountHandOver); [6] h4's binds that kept it,
std::atomic<uint64_t> g_color_handover[10];  // kameo-nx (2026-11-03): CountColorHandOver
std::atomic<uint64_t> g_color_not_left[5];   // kameo-nx (2026-11-03): CountColorNotLeft
                                        // [7-9] drawn after all by: a depth-writing draw's bind, a sample while bound, other
std::atomic<uint64_t> g_fetch_misaligned[2];  // 2026-10-20: draws checked, draws off 16 bytes (CountFetchAlignment)
std::atomic<uint64_t> g_device_swizzle[2];  // 2026-10-20: binds through the device's swizzle, frames with one
std::atomic<uint32_t> g_device_swizzle_frame{0};
std::atomic<uint64_t> g_fur_shells[2];  // 2026-10-20 banjo_renut_fur_shells: drawn, skipped
// banjo-nx (2026-10-23, render-thread round): banjo_renut_chain_reuse's answers per draw: [what][0] draws, [what][1]
// reused (what: targets, textures, viewport, pipeline key, pipeline).
std::atomic<uint64_t> g_chain_reuse[5][2];
std::atomic<uint64_t> g_skin_once[8];  // 2026-10-20 banjo_deko3d_skin_once: written, read, plain, no room; + 4 not fur
std::atomic<uint64_t> g_exp_adjust[3];  // 2026-10-20: binds with an exponent adjust: all, negative, positive
std::atomic<uint64_t> g_skin_miss[6];  // near misses by field: streams, fetch window, index, first vertex, shared, any
std::atomic<uint64_t> g_header_differ[7];  // 2026-10-20: binds whose header (same data) differs from the device, per dword
std::atomic<uint64_t> g_ring_published{0};  // render-thread proposal 23
std::atomic<uint64_t> g_kick_submit[4];      // lists submitted at a kick, ranges cleaned by the thread / by the render thread
std::atomic<uint64_t> g_pushed[4];      // banjo_deko3d_push_constants: float bytes, shared bytes, draws, verify misses
std::atomic<uint64_t> g_depth_float_sampled{0};  // 2026-10-18 census
std::atomic<uint64_t> g_words_viewed{0}, g_words_copied{0};  // banjo_gpu_depth_words_view (package 28)
std::atomic<uint64_t> g_depth_resolve_skip[3]{};  // vivapinata-nx (2026-11-12, vivapinata_depth_resolve_skip_unread)
// banjo_gpu_bool_spec's: draws with specialised bools, of them drawn with the generic pipeline, specialised built.
std::atomic<uint64_t> g_bool_draws{0}, g_bool_generic{0}, g_bool_built{0};
// banjo_gpu_tess_grid's: the corners the per-corner form would shade, and the grid's vertices.
std::atomic<uint64_t> g_tess_corners{0}, g_tess_vertices{0};
// banjo_renut_cache_save_quiet (package 28): pipelines built (any thread) and when the last one was, steady-clock ms.
std::atomic<uint64_t> g_pipelines_built{0};
std::atomic<int64_t> g_last_build_ms{0};

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void NotePipelineBuilt() {
  g_pipelines_built.fetch_add(1, std::memory_order_relaxed);
  g_last_build_ms.store(SteadyMs(), std::memory_order_relaxed);
}

// The A/B word (banjo_renut_ab); the header reads it inline.
std::atomic<int> g_ab_off{0};
int g_ab_off_plain = 0;  // banjo-nx (2026-11-20, batch #3): see the header

namespace {

constexpr int kRenderThreadPriority = 0x2C;   // above the game's threads (0x3B), as banjo-nx's ring decoder
// The lowest an application thread takes is 0x3B (package 27c: 0x3F was refused on the console, "NOT accepted",
// and the workers ran at the default; UnleashedRecomp-NX's pipeline compiler takes 0x3B).
constexpr int kBackgroundPriority = 0x3B;

bool PipelineCacheOn() {
  return ::renut::gpu::IsVulkan() && EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_pipeline_cache));
}

// The saver: plume's save asks the driver for its whole cache under the driver's lock, which pipeline creation on the
// render thread also takes, so it runs on its own low-priority thread, only when something new was compiled.
struct Saver {
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
  std::thread thread;
  plume::RenderDevice *device = nullptr;
};
Saver g_saver;

void SaveNow(const char *why) {
  if (g_saver.device && g_saver.device->savePipelineCache())
    REXLOG_INFO("[renut] pipeline cache saved ({})", why);
}

// The PMULL fingerprint against its bit-by-bit reference over lengths 0-300 (every tail and fold path) of
// pseudo-random bytes, with and without a seed. Any difference keeps XXH3 for the session.
bool PmullSelfTest() {
#if BANJO_PMULL_FINGERPRINT
  uint8_t data[300];
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (uint8_t &b : data) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    b = uint8_t(x >> 24);
  }
  for (size_t n = 0; n <= sizeof(data); ++n) {
    for (const uint64_t seed : {uint64_t(0), uint64_t(0xC0FFEE1234567890ull)}) {
      if (pmull_fingerprint::Fingerprint(data, n, seed) != pmull_fingerprint::Reference(data, n, seed))
        return false;
    }
  }
  return true;
#else
  return false;
#endif
}

// The 10 s report and the A/B halves. Game frames come from the game thread, the rest from the render thread.
struct Stats {
  std::mutex mutex;
  std::chrono::steady_clock::time_point start{}, last_report{}, last_frame{};
  struct Half {
    uint64_t game_frames = 0, render_frames = 0, gpu_frames = 0;
    double render_ms = 0.0, gpu_ms = 0.0, seconds = 0.0;
    double cpu_ms = 0.0;  // banjo-nx (2026-10-28): every registered thread's CPU time in the half (Switch)
    uint64_t draws = 0;   // sasrt-nx (2026-11-09; port 54): the half's draws (g_ab_draws)
  };
  Half total, halves[2];   // halves[1]: the part on, [0]: off
  int half = 1;            // the half now running (the render thread applies it)
  int applied = -1;        // the half the render thread applied last (2026-10-20: target * 2 + half)
  // banjo-nx (2026-10-20, banjo_renut_ab_targets): the list's halves by target, and the place in it.
  // banjo-nx (2026-10-27): 64 (targets 32-40 exist; AbTargets dropped them at 32). 2026-10-28: 128 (target 64, A/B of the
  // night_gpu packages' renut_ab.toml, was dropped silently at 64: the same trap again; 65 the host packets).
  Half by_target[512][2];  // sasrt-nx (2026-11-08): 256 (targets up to 255); port 188: 512 (256-511 a port's own)
  uint32_t cycle = 0;
  std::chrono::steady_clock::time_point half_start{};
  uint64_t cpu_mark = 0;  // banjo-nx (2026-10-28): TotalThreadTicks at the half's start
};
Stats g_stats;

// banjo-nx (2026-10-28, total CPU): every registered thread's CPU time so far (ms), for the A/B halves: with the three
// game cores saturated, a half's CPU a game frame is what an item saves, wherever it runs (0 off the Switch).
double ProcessCpuMs() {
#if REX_PLATFORM_SWITCH
  return double(os::switch_cpu_profiler::TotalThreadTicks()) / 19200.0;
#else
  return 0.0;
#endif
}

const char *AbName(int target) {
  switch (target) {
  case 1: return "banjo_renut_precise_barriers";
  case 2: return "render thread pin (banjo_ring_core)";
  case 3: return "banjo_renut_shared_dedupe";
  case 4: return "banjo_renut_push_point_only";
  case 5: return "banjo_renut_capture_pool";
  case 6: return "banjo_renut_exact_streams";
  case 7: return "banjo_gpu_bool_spec";
  case 8: return "banjo_gpu_blend_skip";
  case 9: return "banjo_gpu_tess_grid";
  case 11: return "banjo_gpu_depth_only";
  case 12: return "banjo_gpu_output_trim";
  case 13: return "banjo_gpu_depth_words_view";
  case 14: return "banjo_renut_record_worker";
  case 15: return "banjo_deko3d_light_branch";
  case 16: return "banjo_deko3d_gpr32";
  case 17: return "banjo_deko3d_skin_once";
  case 18: return "banjo_deko3d_eager_kicks";
  case 19: return "banjo_renut_smaa";
  case 20: return "banjo_renut_chain_reuse (incremental draw state)";  // banjo-nx (2026-10-26)
  case 21: return "banjo_renut_tags (recording from the setters' marks)";  // banjo-nx (2026-10-26)
  case 22: return "banjo_renut_whole_buffers (whole-buffer copies, API-level stage 2)";  // banjo-nx (2026-10-26)
  case 23: return "banjo_renut_convert_reuse (memexport conversions used again)";  // banjo-nx (2026-10-26, GPU)
  case 24: return "banjo_renut_batched_readbacks (memexport readbacks at the list end)";  // banjo-nx (2026-10-26, GPU)
  case 25: return "banjo_renut_fast_draw (the lean executor, record-time draw path stage 1)";  // banjo-nx (2026-10-26)
  case 26: return "banjo_renut_touch_compare (setters mark only changed values)";  // banjo-nx (2026-10-26, setter rewrite)
  case 27: return "banjo_renut_tags_apart (setters' tags out of the recording devices)";  // banjo-nx (2026-10-26)
  case 28: return "render-state numbers by content (the fast path's pipeline cache)";  // banjo-nx (2026-10-27)
  case 29: return "the replay's look-ahead prefetches (banjo_renut_replay_lookahead; SLOWER ON THE CONSOLE, off by default: set it on to measure)";  // banjo-nx (2026-10-27)
  case 30: return "base-vertex draws for vertex shaders without gl_VertexID";  // banjo-nx (2026-10-27)
  case 31: return "draws sampling nothing / without a pixel shader skip the texture refill and pixel constants";
  case 32: return "texture memo entries of an earlier frame refreshed by the frame's check only";  // banjo-nx (2026-10-27)
  case 33: return "banjo_renut_merge_draws (a draw takes the unchanged draws of the next indices)";  // banjo-nx (2026-10-27)
  case 34: return "literal registers still holding a shader's values not pushed again (banjo_renut_literals_resident; neutral on the console, on)";  // banjo-nx (2026-10-27)
  case 35: return "texture memo validity by page and address epochs, not the global one";  // banjo-nx (2026-10-27)
  case 36: return "fetch data of the draw before kept (same shader, fetch constants, chunk and kick)";  // banjo-nx (2026-10-27)
  case 37: return "stream sources of the draw before kept (no guest buffer header read)";  // banjo-nx (2026-10-27)
  case 38: return "whole index copies bound by their pool chunk (the copy's place as the first index)";  // banjo-nx (2026-10-27)
  case 39: return "bound textures not read again for a draw whose pipeline, descriptors and texture words are the last run's";  // banjo-nx (2026-10-27)
  case 40: return "depth-only runs replayed sorted by shader (banjo_renut_shadow_sort; SLOWER ON THE CONSOLE, off by default: set it on to measure)";  // banjo-nx (2026-10-27)
  case 41: return "the fast path's pipeline kept across shader objects of the same contents";  // banjo-nx (2026-10-27)
  case 42: return "mesh objects (banjo_renut_mesh_objects)";  // banjo-nx (2026-10-27, #8)
  case 43: return "texture export checks from record-time extents (banjo_renut_record_extents)";  // banjo-nx (2026-10-27)
  case 44: return "shadow-caster culling (banjo_renut_shadow_cull_indices; set it to measure)";  // banjo-nx (2026-10-27)
  case 45: return "Phase A resource objects (banjo_renut_resource_objects)";  // banjo-nx (2026-10-27)
  case 46: return "the replay stream (banjo_renut_replay_stream: two-line ops, prefetched ahead)";  // banjo-nx (2026-10-27)
  case 47: return "the fetch cache (banjo_renut_fetch_cache: fetch data kept across frames)";  // banjo-nx (2026-10-27)
  case 48: return "geometry objects (banjo_renut_geometry_objects; off half: the mesh objects)";  // banjo-nx (2026-10-27)
  case 49: return "the replay stream's shader stages (banjo_renut_replay_stream_shaders)";  // banjo-nx (2026-10-27)
  case 50: return "the recording's batched object lookups and device memos (banjo_renut_record_lookups)";  // 2026-10-28
  case 51: return "a buffer's first op keeps only the constant groups set so far (banjo_renut_first_op_groups)";  // 2026-10-28
  case 52: return "a replay's constant groups read where the ops recorded them (banjo_renut_replay_group_pointers)";  // 2026-10-28
  case 53: return "texture slots kept when their object's answer holds (banjo_renut_texture_slot_skip; SLOWER ON THE CONSOLE, off by default: set it on to measure)";  // 2026-10-28
  case 54: return "replaced range copies dropped every 16 frames, not every frame (kWholeReplacePeriod)";  // 2026-10-28
  case 55: return "the replay's prefetch of the next op's state lines (off half: none)";  // 2026-10-28
  case 56: return "the replay's prefetch of the next op's resource objects (off half: none)";  // 2026-10-28
  case 57: return "the replay's prefetch of the next op's shader and declaration records (banjo_renut_replay_record_prefetch; no measurable effect, off by default: set it on to measure)";  // 2026-10-28
  case 59: return "a point-sprite draw leaves the fast path a state (banjo_renut_point_fast_state)";  // 2026-10-28
  case 60: return "erased range copies leave the geometry answers (off half: every erase voids them)";  // 2026-10-28
  case 61: return "a predication change kept on the fast path as a viewport input";  // 2026-10-28
  case 62: return "a queued whole-snapshot draw gets a chain number (the fast path may take it)";  // 2026-10-28
  case 63: return "the fast pipeline cache keeps final answers across prebuild epochs";  // 2026-10-28
  case 64: return "a handed-over depth resolve kept across binds for draws that write no depth (banjo_deko3d_depth_handover_readonly)";  // 2026-10-28
  case 65: return "host draw packets (banjo_renut_host_packets: made and taken)";  // 2026-10-28
  case 66: return "resolve host levels made at their first read (banjo_renut_lazy_resolve_mips, from sa2-nx)";  // 2026-10-28
  case 67: return "whole clears of a target holding that clear already not sent (banjo_renut_clear_repeat, from sa2-nx)";
  case 68: return "no pipeline read for the provisional check while none is provisional (banjo_deko3d_provisional_count)";
  // katamari-nx (2026-10-30): Beautiful Katamari's CPU items (cpu/hot/banjo_hand_natives.h, cpu/banjo_hot_guest.h).
  case 69: return "Katamari's hand-written matrix natives (katamari_hand_matrix_*; off half: the generated copies)";
  case 83: return "tessellated draws' patch kind and vertex fetch types as constants (banjo_gpu_fetch_spec; 69 in vivapinata-nx and banjo-nx)";
  case 70: return "the localized copies of the hot functions (localized.txt; off half: the generated originals)";
  // bangaio-nx (2026-10-29): the missile storm round.
  case 71: return "a vertex range's stream cache entry made at its second sighting (banjo_renut_stream_second_sighting)";
  case 72: return "non-indexed quad lists drawn with one index buffer made once (banjo_renut_quad_indices)";
  case 80: return "point sprites pushed and on the fast path on deko3d (banjo_renut_point_fast_draw; 69 in bangaio-nx)";
  case 81: return "a delta's changed chunks found a run at a time with NEON masks (banjo_renut_delta_masks; 73 in bangaio-nx)";
  case 82: return "deltas copied at their size into captures the pool keeps up to 8,192 (banjo_renut_small_captures; 70 in bangaio-nx)";
  // kameo-nx (2026-11-03): the GPU round (docs/kameo-notes.md).
  case 73: return "colour resolves left in the target's image (banjo_deko3d_color_handover)";
  case 74: return "the frame's last resolve scanned out from the target's image (banjo_deko3d_scanout_handover)";
  case 75: return "a depth resolve sampled from the depth target a draw only tests (banjo_deko3d_depth_handover_sampled)";
  case 76: return "a sampled depth target left in the read layout for draws that do not write it (banjo_deko3d_depth_read_layout)";
  case 77: return "a resolve that draws nothing leaves the targets bound (banjo_deko3d_resolve_keeps_target)";
  case 78: return "no pow in the scan-out at an output gamma of 1 (banjo_renut_present_no_pow)";
  case 79: return "the scan-out loads the texel at one to one (banjo_renut_present_texel_load)";
  case 84: return "a queued draw prefetches the delta's device and shadow lines (banjo_renut_delta_prefetch; 74 in bangaio-nx)";
  // vivapinata-nx (2026-11-11, port 87; its number, 85 being its own vivapinata_worker_idle_wait, not in the base).
  case 86: return "a tile's command buffer run and its twin replayed once for both tiles (vivapinata_tiles_once; off half: once per tile)";
  // vivapinata-nx (2026-11-12, port 88; its number).
  case 87: return "a resolve of an unchanged multisampled target copied from the destination it was resolved into (vivapinata_resolve_from_sibling; off half: resolved again)";
  // vivapinata-nx (2026-11-12, port 91; its number).
  case 89: return "the scan-out's gamma ramp read from a 256 x 256 table of its entries, two fetches a pixel (vivapinata_present_lut; off half: three)";
  // vivapinata-nx (2026-11-12, port 94; its number since its commit 7502c604: 90-145 are sasrt::gpu_round's items,
  // kAbBase + item, so 90's off half also switched sasrt_gpu_skip_inert_draws off).
  case 150: return "the shader round's GLSL passes in the pixel shaders, constant 975 (vivapinata_shader_round; off half: 975 = 0, the code as before them)";
  // vivapinata-nx (2026-11-12, port 115; its number).
  case 173: return "command memory CPU-cached, cleaned at each list's end (banjo_deko3d_cached_commands; banjo-nx 2026-11-21)";
  case 174: return "Replay: PrefetchShaderBehind at shader changes (prefetches only; banjo-nx 2026-11-21)";
  case 175: return "Replay: ApplyResources prefetches the geometry objects' copies and cards, not only their answers (banjo-nx 2026-11-21)";
  case 176: return "Replay: the next op's shader memo sets prefetched (PrefetchShaderLocked; banjo-nx 2026-11-21; off half: none)";
  case 177: return "fast path: the shader change's prefetches (device lines, declaration, literal runs, entries; banjo-nx 2026-11-21)";
  case 178: return "fast path: the texture slots' and the pipeline entry's prefetches (banjo-nx 2026-11-21; off half: none)";
  case 179: return "kick submits (banjo_deko3d_kick_submit): lists ended at the game's kicks (banjo-nx 2026-11-21; off half: the frame at the present)";
  case 181: return "Replay: the next draw's store cards prefetched from its record's header words (banjo-nx 2026-11-21; off half: none)";
  case 182: return "the deko3d command stage (banjo_deko3d_command_stage): lists recorded into a stream played on the system core (banjo-nx 2026-11-21; off half: recorded directly)";
  case 183: return "the command stage ends, submits and presents the frame (banjo_deko3d_stage_submit; banjo-nx 2026-11-21; off half: the render thread drains and submits)";
  case 184: return "float constants into the stage's stream as the guest has them, swapped by the stage (banjo_deko3d_stage_swap; banjo-nx 2026-11-21)";
  case 185: return "the cache-warming helper on the system core (banjo_renut_warm_helper; banjo-nx 2026-11-21; off half: given nothing)";
  case 186: return "the command stage looks for work every poll_us, never woken by the render thread (banjo-nx 2026-11-21; off half: woken)";
  case 189: return "the command stage and the warm helper float on cores 1-3 (banjo_deko3d_command_stage_float; banjo-nx 2026-11-22; off half: core 3 alone)";
  case 188: return "the command stage prefetches its stream's lines ahead (banjo_deko3d_command_stage_stream_ahead; banjo-nx 2026-11-21)";
  case 187: return "the command stage prefetches the objects of the commands ahead (banjo_deko3d_command_stage_peek; banjo-nx 2026-11-21)";
  case 172: return "the fast path's spec answers (fetch spec, plain fetches, clip bit, sasrt words) kept while their inputs hold (banjo-nx 2026-11-21)";
  case 156: return "depth resolves nothing has read left out until something does (vivapinata_depth_resolve_skip_unread; NOT exact in principle; off half: drawn)";
  case 152: return "the pixel shaders' alpha test moved up ahead of the colour arithmetic, constant 977 = 1 (vivapinata_shader_earlykill; off half: the test last)";
  case 151: return "the pixel shaders without Renut's early alpha test, constant 976 = 0 (vivapinata_shader_noearly; off half: the test as before)";
  // banjo-nx (2026-11-21, ports 246-247): its 300 and 301, numbered below 256 here (port 188).
  case 170: return "FillTextures' skips: an unasked call's slot compare and copy, a slot answer's epoch buckets while the bind generation holds (port 246; off half: both as before)";
  case 171: return "Replay's PrefetchShaderBehind skipped for a stage whose shader is the last op's (port 247; off half: both stages prefetched)";
  // blur-nx (2026-11-10/11, port 76; its keys, the same numbers; 210 and 214 not ported).
  case 211: return "an EXPAND resolve sampled as its FLOAT twin through the resolve's texture (blur_renut_expand_alias)";
  case 212: return "a partial resolve keeps a destination of another format and converts (blur_renut_resolve_keep_dest)";
  case 213: return "occlusion queries answered as the reference's GPU does (blur_renut_occlusion_queries)";
  case 215: return "stack surface headers kept as bound (blur_renut_stack_surface_headers)";
  case 216: return "a 4x view of a 1x surface resolves as its 2x2 box (blur_renut_sample_grid_resolve)";
  case 217: return "no host target for a surface header over 8192 wide or tall (blur_renut_surface_size_guard)";
  case 218: return "an EDRAM wipe written over through another view is left out (blur_renut_wipe_overwritten)";
  case 219: return "a colour resolve's exponent bias applied (blur_renut_resolve_exp_bias)";
  case 220: return "a target kept in part takes what other views wrote over its EDRAM (blur_renut_edram_inherit)";
  case 221: return "a multisampled resolve into a slice other than 0 goes through staging (blur_renut_ms_slice_resolve)";
  // blur-nx (2026-11-18, the Renut host batch: gpu/host_batch.h, docs/renut-host-ideas-25-1118.md).
  case 230: return "NewOp builds no resource objects or slot extents while replays inherit (blur_renut_record_skip_unused, #1 #2)";
  case 231: return "an op's inherited words put back only in the chunks it applied (blur_renut_inherit_by_chunk, #3)";
  case 232: return "the draw before's whole-buffer answer for the same streams (blur_renut_whole_repeat, #4)";
  case 233: return "the ring cleaner above the game threads (blur_renut_ring_cleaner_priority, #11)";
  case 234: return "BuildInheritRuns a 32-bit word at a time (blur_renut_inherit_runs_words, #13)";
  case 235: return "the fast pipeline cache two-way (blur_renut_fast_pipeline_two_way, #16)";
  case 236: return "(removed: #17, the two-way texture memo)";
  case 237: return "a provisional pipeline answer looked up again with its kept key (blur_renut_provisional_key_kept, #18)";
  case 238: return "runs and resolves wake the render thread as draws do (blur_renut_wake_batch_all, #28)";
  case 239: return "eight viewports kept, not one (blur_renut_viewport_memo, #32)";
  case 240: return "the mesh objects' generation raised only by a whole copy's drop (blur_renut_mesh_gen_whole_only, #36)";
  default: {
    // sasrt-nx (2026-11-08): the GPU round's items (sasrt_gpu_round.h: 90 and up).
    const char *name = sasrt::gpu_round::AbName(target);
    return name ? name : "none";
  }
  }
}

// The A/B target whose part is in its "off" half now (0: none). The per-draw keys (3-5) read it on every use.

bool g_precise_barriers_on = false;  // banjo_renut_precise_barriers as configured (BeforeDeviceCreated)

}  // namespace

namespace {

// banjo-nx (2026-10-20, banjo_renut_ab_targets): the list, read once (numbers 1-31).
// banjo-nx (2026-11-20): filled under std::call_once, so this file can be built with -fno-threadsafe-statics (its
// other function statics are plain values from keys); the list and the flag are constant-initialised.
const std::vector<int> &AbTargets() {
  static std::vector<int> list;
  static std::once_flag once;
  std::call_once(once, [] {
    std::vector<int> v;
    const std::string s = REXCVAR_GET(banjo_renut_ab_targets);
    int n = 0;
    bool digit = false;
    for (const char c : s + ",") {
      if (c >= '0' && c <= '9') {
        n = n * 10 + (c - '0');
        digit = true;
      } else {
        if (digit && n > 0 && n < 512)  // banjo-nx (2026-10-27): was 32, which dropped targets 32-40 silently; sasrt-nx: 256;
                                        // port 188: 512 (256-511 a port's own)
          v.push_back(n);
        else if (digit && n >= 512)
          REXLOG_WARN("[renut] banjo_renut_ab_targets: target {} is past the last one (511): left out", n);
        n = 0;
        digit = false;
      }
    }
    list = std::move(v);
  });
  return list;
}
// The target the A/B switches now (requires g_stats.mutex).
int AbTargetLocked() {
  const std::vector<int> &list = AbTargets();
  if (list.empty())
    return REXCVAR_GET(banjo_renut_ab_target);
  return list[g_stats.cycle % list.size()];
}

// Render thread: the part as the half says.
void AbApply(int target, bool on) {
  g_ab_off.store(on ? 0 : target, std::memory_order_relaxed);
  g_ab_off_plain = on ? 0 : target;  // banjo-nx (2026-11-20, batch #3)
  // banjo-nx (2026-11-20): the GPU round's On() answers follow the half (before ApplyPlume, which asks them)
  if (sasrt::gpu_round::g_latched_ready.load(std::memory_order_acquire))
    sasrt::gpu_round::RefreshEffective();
  sasrt::gpu_round::ApplyPlume();  // sasrt-nx (2026-11-08): the GPU round's plume items follow the half
  if (target == 26)  // banjo-nx (2026-10-26): read by every setter, so a plain flag (d3d_native.h)
    renut::gpu::native::g_touch_compare = on && REXCVAR_GET(banjo_renut_touch_compare);
  if (target == 27)
    renut::gpu::native::g_tags_apart = on && REXCVAR_GET(banjo_renut_tags_apart);
#if defined(PLUME_DEKO3D)
  if (target == 39)  // banjo-nx (2026-10-27): plume's flag
    plume::Deko3DSetTextureSkip(on && REXCVAR_GET(banjo_deko3d_texture_skip));
  if (target == 68)  // banjo-nx (2026-10-28): plume's flag
    plume::Deko3DSetProvisionalCount(on && REXCVAR_GET(banjo_deko3d_provisional_count));
#endif
  if (target == 1) {
    plume::SetVulkanPreciseBarriers(on && g_precise_barriers_on);
  } else if (target == 2) {
#if REX_PLATFORM_SWITCH
    const int32_t core = REXCVAR_GET(banjo_ring_core);
    if (core >= 0)
      (void)(on ? RexSwitchPinCurrentThreadToCore(int(core)) : RexSwitchUnpinCurrentThread(-1));
#endif
  }
}
// 2026-10-20: CountDrawSource's counts (the render thread's).
std::atomic<uint64_t> g_draw_source[2];


}  // namespace

std::atomic<uint64_t> g_last_report_us{0};  // blur-nx (2026-11-17, audit #31): StatsGameFrame's report, its last time

}  // namespace banjo::renut_gpu
// blur-nx (2026-11-17, the Renut audit's #17-#22 and #30): gpu/stage_census.h.
REXCVAR_DEFINE_BOOL(blur_renut_diag_stage_census, false, "Banjo/Diagnostics",
                    "blur-nx (the Renut audit's census): the time the renderer spends in its stages (FastDrawLocked, "
                    "DrawStateLocked, FillTextures, the constants, targets, pipeline, streams, uploads, resolves' "
                    "setup, render-thread items, NewOp, BeginCommandBuffer's fence) in the 10 s report. Two clock reads "
                    "and two atomics a stage when on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace renut::gpu::census {
Acc g_acc[kStageCount];
std::atomic<int> g_on{-1};
bool OnSlow() {
  const bool on = REXCVAR_GET(blur_renut_diag_stage_census);
  g_on.store(on ? 1 : 0, std::memory_order_relaxed);
  return on;
}
std::string Report(double seconds) {
  if (!On() || seconds <= 0.0)
    return {};
  static constexpr const char *kNames[kStageCount] = {
      "FastDrawLocked", "DrawStateLocked", "FillTextures", "constants", "BindTargets", "setPipeline", "BindStreams",
      "WholeGeometryTry", "host draw", "CardsProve", "Upload", "RefreshBindingLocked", "ResolveTargetsLocked",
      "render items", "NewOp", "BeginCommandBuffer fence"};
#if defined(__SWITCH__)
  const double tick_ms = 1000.0 / double(armGetSystemTickFreq());
#else
  const double tick_ms = 1000.0 * double(std::chrono::steady_clock::period::num) /
                         double(std::chrono::steady_clock::period::den);
#endif
  std::string line = "stages (blur_renut_diag_stage_census), ms a second and calls a second:";
  for (uint32_t i = 0; i < kStageCount; ++i) {
    const uint64_t ticks = g_acc[i].ticks.exchange(0, std::memory_order_relaxed);
    const uint64_t n = g_acc[i].n.exchange(0, std::memory_order_relaxed);
    if (!n)
      continue;
    line += fmt::format(" {} {:.2f} ms/s ({:.0f}/s, {:.2f} us each);", kNames[i], double(ticks) * tick_ms / seconds,
                        double(n) / seconds, double(ticks) * tick_ms * 1000.0 / double(n));
  }
  return line;
}
}  // namespace renut::gpu::census
namespace banjo::renut_gpu {

void StatsGameFrame() {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_stats.mutex);
  if (g_stats.start == std::chrono::steady_clock::time_point{}) {
    g_stats.start = g_stats.last_report = g_stats.half_start = g_stats.last_frame = now;
    return;
  }
  ++g_stats.total.game_frames;
  ++g_stats.halves[g_stats.half].game_frames;
  // Each frame's time goes to the half it ran in, so a half still running at a report counts what it has so far.
  g_stats.halves[g_stats.half].seconds += std::chrono::duration<double>(now - g_stats.last_frame).count();
  g_stats.last_frame = now;
  int target = AbTargetLocked();
  if (target && std::chrono::duration<double>(now - g_stats.half_start).count() >= double(REXCVAR_GET(banjo_renut_ab_s))) {
    // banjo-nx (2026-10-28): the ending half's CPU time (the first half's mark is taken at its first switch: none).
    const double cpu = ProcessCpuMs();
    if (g_stats.cpu_mark && cpu * 19200.0 > double(g_stats.cpu_mark))
      g_stats.halves[g_stats.half].cpu_ms += cpu - double(g_stats.cpu_mark) / 19200.0;
    g_stats.cpu_mark = uint64_t(cpu * 19200.0);
    g_stats.half ^= 1;
    g_stats.half_start = now;
    // banjo-nx (2026-10-20, banjo_renut_ab_targets): after a target's off half (on, then off), the next target, on.
    if (!AbTargets().empty() && g_stats.half == 1) {
      for (int h = 0; h < 2; ++h) {
        Stats::Half &to = g_stats.by_target[target][h];
        const Stats::Half &from = g_stats.halves[h];
        to.game_frames += from.game_frames;
        to.render_frames += from.render_frames;
        to.gpu_frames += from.gpu_frames;
        to.render_ms += from.render_ms;
        to.gpu_ms += from.gpu_ms;
        to.seconds += from.seconds;
        to.cpu_ms += from.cpu_ms;
        to.draws += from.draws;  // sasrt-nx (2026-11-09; port 54)
        g_stats.halves[h] = {};
      }
      ++g_stats.cycle;
      target = AbTargetLocked();
    }
  }
  const double since = std::chrono::duration<double>(now - g_stats.last_report).count();
  if (since < 10.0)
    return;
  // blur-nx (2026-11-17, the Renut audit's #31, a census): this report is built on the game thread at its Swap (the
  // Game Master in Blur), under g_stats.mutex; how long it takes is logged at its end (the next report says the last).
  struct ReportClock {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~ReportClock() {
      g_last_report_us.store(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start).count()),
                             std::memory_order_relaxed);
    }
  } report_clock;
  if (const uint64_t us = g_last_report_us.load(std::memory_order_relaxed))
    REXLOG_INFO("[renut] the last 10 s report took {:.2f} ms on the game thread (blur-nx audit #31)", double(us) / 1000.0);
  BanjoTlbTestTick();
  // banjo-nx (2026-10-22): the native copies' report (app/src/cpu/banjo_hot_guest.cpp) comes with this one: without
  // a guard (direct dispatch) no hook calls it any more.
  banjo::hot::Report();
  const Stats::Half &t = g_stats.total;
  const uint64_t reused = g_shared_reused.exchange(0, std::memory_order_relaxed);
  const uint64_t uploaded = g_shared_uploaded.exchange(0, std::memory_order_relaxed);
  const uint64_t exact_checks = g_exact_checks.exchange(0, std::memory_order_relaxed);
  const uint64_t exact_bytes = g_exact_bytes.exchange(0, std::memory_order_relaxed);
  const uint64_t exact_stale = g_exact_stale.exchange(0, std::memory_order_relaxed);
  const uint64_t late_stale = g_late_stale.exchange(0, std::memory_order_relaxed);
  const uint64_t exact_repeat = g_exact_repeat.exchange(0, std::memory_order_relaxed);
  const uint64_t exact_repeat_bytes = g_exact_repeat_bytes.exchange(0, std::memory_order_relaxed);
  const uint64_t kicks = g_kicks.exchange(0, std::memory_order_relaxed);
  const uint64_t fence_queries = g_fence_queries.exchange(0, std::memory_order_relaxed);
  const uint64_t exact_memo = g_exact_memo.exchange(0, std::memory_order_relaxed);
  const uint64_t exact_memo_bytes = g_exact_memo_bytes.exchange(0, std::memory_order_relaxed);
  const uint64_t evicted = g_reserve_evicted.exchange(0, std::memory_order_relaxed);
  const uint64_t cache_refused = g_cache_refused.exchange(0, std::memory_order_relaxed);
  const uint64_t cache_pressure = g_cache_pressure_bytes.exchange(0, std::memory_order_relaxed);
  const uint64_t bool_draws = g_bool_draws.exchange(0, std::memory_order_relaxed);
  const uint64_t bool_generic = g_bool_generic.exchange(0, std::memory_order_relaxed);
  const uint64_t bool_built = g_bool_built.exchange(0, std::memory_order_relaxed);
  const uint64_t tess_corners = g_tess_corners.exchange(0, std::memory_order_relaxed);
  const uint64_t tess_vertices = g_tess_vertices.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[renut] {:.1f} s: {} game frames ({:.2f} fps), render thread busy {:.2f} ms a frame ({} frames), "
              "GPU {:.2f} ms a frame ({} frames); {} draws, SharedConstants reused by {} (banjo_renut_shared_dedupe); "
              "cached streams: {} checks of {:.1f} MB ({} of them, {:.1f} MB, of a copy already checked in the frame; {} "
              "answered by the interval memo, {:.1f} MB not read again; {} kicks, {} fence queries), {} stale caught (banjo_renut_exact_streams), {} found stale late, {} copies refused at the cap, {:.1f} MB dropped for room (banjo_renut_stream_cache_pressure); {} textures released under the reserve (banjo_renut_memory_policy); specialised bools: {} "
              "draws, {} of them on the generic pipeline while theirs was built, {} pipelines built "
              "(banjo_gpu_bool_spec); tessellation: {} grid vertices for {} corners (banjo_gpu_tess_grid)",
              since, t.game_frames, double(t.game_frames) / since,
              t.render_frames ? t.render_ms / double(t.render_frames) : 0.0, t.render_frames,
              t.gpu_frames ? t.gpu_ms / double(t.gpu_frames) : 0.0, t.gpu_frames, reused + uploaded, reused,
              exact_checks, double(exact_bytes) / double(1u << 20), exact_repeat,
              double(exact_repeat_bytes) / double(1u << 20), exact_memo, double(exact_memo_bytes) / double(1u << 20),
              kicks, fence_queries, exact_stale, late_stale, cache_refused, double(cache_pressure) / double(1u << 20),
              evicted,
              bool_draws,
              bool_generic, bool_built, tess_vertices, tess_corners);
  {
    const uint64_t immediate = g_draw_source[0].exchange(0, std::memory_order_relaxed);
    const uint64_t replayed = g_draw_source[1].exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[renut] draw sources: {} immediate (a whole device copy each, {:.1f} MB), {} replayed from recorded "
                "command buffers (device deltas)", immediate,
                double(immediate) * double(renut::gpu::d3d::kDeviceSnapshotSize) / double(1u << 20), replayed);
  }
  // 2026-10-20 (from sa2-nx): what the immediate draws keep of the device (backend/queued.cpp, banjo_renut_snapshot_deltas).
  if (const std::string deltas = ::renut::gpu::SnapshotDeltaReport(); !deltas.empty())
    REXLOG_INFO("[renut] {}", deltas);
  if (const std::string census = ::renut::gpu::RepeatCensusReport(); !census.empty())  // banjo-nx (2026-10-23, 6b)
    REXLOG_INFO("[renut] {}", census);
  if (const std::string tags = ::renut::gpu::TagsReport(); !tags.empty())  // banjo-nx (2026-10-26, banjo_renut_tags)
    REXLOG_INFO("[renut] {}", tags);
  if (const std::string ss = ::renut::gpu::ShadowSortReport(); !ss.empty())  // banjo-nx (2026-10-27)
    REXLOG_INFO("[renut] {}", ss);
  if (const std::string fd = ::renut::gpu::FastDrawReport(); !fd.empty())  // banjo-nx (2026-10-26)
    REXLOG_INFO("[renut] {}", fd);
  if (const std::string ta = ::renut::gpu::TextureAnswersReport(); !ta.empty())  // banjo-nx (2026-10-27)
    REXLOG_INFO("[renut] {}", ta);
  if (const std::string dd = ::renut::gpu::DroppedDrawsReport(); !dd.empty())  // halo3-nx (2026-11-03)
    REXLOG_WARN("[renut] {}", dd);
  if (const std::string dk = ::renut::gpu::DrawKindsReport(); !dk.empty())  // banjo-nx (2026-10-27)
    REXLOG_INFO("[renut] {}", dk);
  if (const std::string ss = ::renut::gpu::StateSourceReport(); !ss.empty())  // banjo-nx (2026-10-26)
    REXLOG_INFO("[renut] {}", ss);
  if (const std::string rl = ::renut::gpu::RecordLockReport(); !rl.empty())  // blur-nx (2026-11-17)
    REXLOG_INFO("[renut] {}", rl);
  if (const std::string rr = ::renut::gpu::ResolveRemakeReport(); !rr.empty())  // blur-nx (2026-11-17, audit #4/#5)
    REXLOG_INFO("[renut] {}", rr);
  if (const std::string st = ::renut::gpu::census::Report(since); !st.empty())  // blur-nx (2026-11-17, audit #17)
    REXLOG_INFO("[renut] {}", st);
  if (const std::string hb = ::renut::gpu::host_batch::Report(since); !hb.empty())  // blur-nx (2026-11-18)
    REXLOG_INFO("[renut] {}", hb);
  if (const std::string rc = ::renut::gpu::ReplayRepeatCensusReport(); !rc.empty())  // banjo-nx (2026-10-26)
    REXLOG_INFO("[renut] {}", rc);
  if (const std::string pc = ::renut::gpu::PacketCensusReport(); !pc.empty())  // banjo-nx (2026-10-28)
    REXLOG_INFO("[renut] {}", pc);
  if (const std::string fc = ::renut::gpu::FastCensusReport(); !fc.empty())  // banjo-nx (2026-10-26)
    REXLOG_INFO("[renut] {}", fc);
  if (const std::string uc = ::renut::gpu::UnleashedCensusReport(); !uc.empty())  // banjo-nx (2026-10-26)
    REXLOG_INFO("[renut] {}", uc);
  if (const std::string cr = ::renut::gpu::ConvertReuseReport(); !cr.empty())  // banjo-nx (2026-10-26, GPU)
    REXLOG_INFO("[renut] {}", cr);
  if (const std::string wb = ::renut::gpu::WholeBuffersReport(); !wb.empty())  // banjo-nx (2026-10-26, stage 2)
    REXLOG_INFO("[renut] {}", wb);
  if (const uint64_t copies = g_kick_reuse[0].exchange(0, std::memory_order_relaxed),
      streams = g_kick_reuse[1].exchange(0, std::memory_order_relaxed),
      stale = g_kick_reuse[2].exchange(0, std::memory_order_relaxed);
      copies || streams || stale) {
    REXLOG_INFO("[renut] kept across kicks (banjo_renut_kick_reuse): {} fetch copies ({:.1f} MB) and {} converted streams "
                "({:.1f} MB) of an earlier kick drawn again, not made again{}",
                copies, double(g_kick_reuse_bytes[0].exchange(0, std::memory_order_relaxed)) / double(1u << 20), streams,
                double(g_kick_reuse_bytes[1].exchange(0, std::memory_order_relaxed)) / double(1u << 20),
                stale ? fmt::format("; verify: {} stale under clean cards (made again)", stale) : std::string());
  }
  if (g_latched_ready.load(std::memory_order_acquire) && g_latched.store_cards) {
    const uint64_t clean = g_cards_clean.exchange(0, std::memory_order_relaxed);
    const uint64_t clean_bytes = g_cards_clean_bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t dirty = g_cards_dirty.exchange(0, std::memory_order_relaxed);
    const uint64_t missed = g_cards_missed.exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[renut] store cards: {} copies proven by their cards ({:.1f} MB not hashed), {} with a page written "
                "(hashed){} (banjo_renut_store_cards); textures: {} checks proven, {} hashed, {} dynamic re-uploads "
                "not made ({:.1f} MB not read){}",
                clean, double(clean_bytes) / double(1u << 20), dirty,
                g_latched.store_cards_verify ? fmt::format("; verify: {} changed under clean cards", missed)
                                             : std::string(),
                g_tex_cards[0].exchange(0, std::memory_order_relaxed),
                g_tex_cards[1].exchange(0, std::memory_order_relaxed),
                g_tex_cards[2].exchange(0, std::memory_order_relaxed),
                double(g_tex_cards_bytes.exchange(0, std::memory_order_relaxed)) / double(1u << 20),
                g_latched.store_cards_verify
                    ? fmt::format("; verify: {} textures changed under clean cards",
                                  g_tex_cards[3].exchange(0, std::memory_order_relaxed))
                    : std::string());
  }
  if (target && !AbTargets().empty()) {
    // banjo-nx (2026-10-20, banjo_renut_ab_targets): each listed target's whole periods so far.
    auto line = [](const Stats::Half &h) {
      return fmt::format("{:.2f} fps, render thread {:.2f} ms a frame, GPU {:.2f} ms a frame ({:.0f} s){}{}",
                         h.seconds > 0.0 ? double(h.game_frames) / h.seconds : 0.0,
                         h.render_frames ? h.render_ms / double(h.render_frames) : 0.0,
                         h.gpu_frames ? h.gpu_ms / double(h.gpu_frames) : 0.0, h.seconds,
                         h.cpu_ms > 0.0 && h.game_frames
                             ? fmt::format(", CPU {:.1f} ms a frame (all threads)", h.cpu_ms / double(h.game_frames))
                             : std::string(),
                         // sasrt-nx (2026-11-09; port 54): the half's load, so halves that caught different
                         // scenes can be compared per draw
                         h.render_frames && h.draws
                             ? fmt::format(", {:.0f} draws a frame, render thread {:.2f} us a draw",
                                           double(h.draws) / double(h.render_frames),
                                           1000.0 * h.render_ms / double(h.draws))
                             : std::string());
    };
    for (const int listed : AbTargets()) {
      const Stats::Half &on = g_stats.by_target[listed][1], &off = g_stats.by_target[listed][0];
      if (on.seconds > 0.0 || off.seconds > 0.0)
        REXLOG_INFO("[renut A/B] {} (target {}, banjo_renut_ab_targets): on {}; off {}", AbName(listed), listed,
                    line(on), line(off));
      g_stats.by_target[listed][0] = g_stats.by_target[listed][1] = {};
    }
  } else if (target) {
    const Stats::Half &on = g_stats.halves[1], &off = g_stats.halves[0];
    auto line = [](const Stats::Half &h) {
      return fmt::format("{:.2f} fps, render thread {:.2f} ms a frame, GPU {:.2f} ms a frame ({:.0f} s){}{}",
                         h.seconds > 0.0 ? double(h.game_frames) / h.seconds : 0.0,
                         h.render_frames ? h.render_ms / double(h.render_frames) : 0.0,
                         h.gpu_frames ? h.gpu_ms / double(h.gpu_frames) : 0.0, h.seconds,
                         h.cpu_ms > 0.0 && h.game_frames
                             ? fmt::format(", CPU {:.1f} ms a frame (all threads)", h.cpu_ms / double(h.game_frames))
                             : std::string(),
                         // sasrt-nx (2026-11-09; port 54): the half's load, so halves that caught different
                         // scenes can be compared per draw
                         h.render_frames && h.draws
                             ? fmt::format(", {:.0f} draws a frame, render thread {:.2f} us a draw",
                                           double(h.draws) / double(h.render_frames),
                                           1000.0 * h.render_ms / double(h.draws))
                             : std::string());
    };
    REXLOG_INFO("[renut A/B] {} (target {}): on {}; off {}", AbName(target), target, line(on), line(off));
    g_stats.halves[0] = g_stats.halves[1] = {};
  }
  ReportNvk();
  record::Report(static_cast<int>(since + 0.5));  // banjo_renut_record_worker (package 28b)
#if defined(PLUME_DEKO3D)
  if (::renut::gpu::IsDeko3D())  // banjo-nx (2026-10-17): the deko3d back end
    REXLOG_INFO("[renut] {}", deko3d::Report());
#endif
  REXLOG_INFO("[renut] {}", renut::frame_stats::WaitReport());  // blur-nx (2026-11-13, #37's census)
  REXLOG_INFO("[renut] {}", renut::frame_stats::PacingReport());  // blur-nx (2026-11-18, for 1 % lows)
  {  // blur-nx (2026-11-13, #47's census; banjo_renut_lean_diagnostics' draws)
    const uint64_t draws = g_const_draws.exchange(0, std::memory_order_relaxed);
    const uint64_t regs = g_const_registers.exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[renut] float constants (#47's census): {} draws, {} with a vertex shader and {} with a pixel shader "
                "that index the register file (every register uploaded); the others read {:.1f} registers a draw",
                draws, g_const_vs_indexed.exchange(0, std::memory_order_relaxed),
                g_const_ps_indexed.exchange(0, std::memory_order_relaxed), draws ? double(regs) / double(draws) : 0.0);
  }
  REXLOG_INFO("[renut] draw kinds: {} depth-only ({} of them without alpha test or alpha to coverage, with a pixel "
              "shader; {} drawn without it, banjo_gpu_depth_only), {} writing nothing, {} writing part of a target's "
              "channels",
              g_kind_depth_only.exchange(0, std::memory_order_relaxed),
              g_kind_depth_plain.exchange(0, std::memory_order_relaxed),
              g_depth_only_draws.exchange(0, std::memory_order_relaxed),
              g_kind_nothing.exchange(0, std::memory_order_relaxed), g_kind_partial.exchange(0, std::memory_order_relaxed));
  REXLOG_INFO("{}", sasrt::gpu_round::Report());  // sasrt-nx (2026-11-08): the GPU round's counts
  // sasrt-nx (2026-11-11, port 82; its GPU list's rows 1, 2 and 4; UNTESTED): whether this game hits them at all.
  REXLOG_INFO("{}", sasrt::gpu_round::UntestedReport());
  REXLOG_INFO("[renut] depth resolve words: {} binds through the resolve's own texture (banjo_gpu_depth_words_view), "
              "{} with the copy (made only when resolved since); {} binds of a depth resolve's float target; {} colour "
              "resolves drawn (banjo_deko3d_draw_resolves); resolves of an unchanged source not done again "
              "(banjo_renut_resolve_skip): {} colour, {} depth; depth resolves without their words "
              "(banjo_renut_depth_words_lazy): {}, words written late {}, read stale {}",
              g_words_viewed.exchange(0, std::memory_order_relaxed), g_words_copied.exchange(0, std::memory_order_relaxed),
              g_depth_float_sampled.exchange(0, std::memory_order_relaxed),
              g_resolves_drawn.exchange(0, std::memory_order_relaxed),
              g_resolves_skipped[0].exchange(0, std::memory_order_relaxed),
              g_resolves_skipped[1].exchange(0, std::memory_order_relaxed),
              g_words_lazy[0].exchange(0, std::memory_order_relaxed),
              g_words_lazy[1].exchange(0, std::memory_order_relaxed),
              g_words_lazy[2].exchange(0, std::memory_order_relaxed));
  // renut-ultimate (2026-11-13, sasrt-nx's resolve project R1): while sasrt_gpu_diag_depth_words is on.
  if (const std::string words = ::renut::gpu::DepthWordsCensusReport(); !words.empty())
    REXLOG_INFO("{}", words);
  // vivapinata-nx (2026-11-12, port 115): while vivapinata_depth_resolve_skip_unread is on.
  if (REXCVAR_GET(vivapinata_depth_resolve_skip_unread))
    REXLOG_INFO("[renut] depth resolves nothing had read (vivapinata_depth_resolve_skip_unread): {} left out; read "
                "later: {} drawn then from an unchanged source, {} from newer content or not at all",
                g_depth_resolve_skip[0].exchange(0, std::memory_order_relaxed),
                g_depth_resolve_skip[1].exchange(0, std::memory_order_relaxed),
                g_depth_resolve_skip[2].exchange(0, std::memory_order_relaxed));
  // banjo-nx (2026-10-21): how many frames had SMAA (banjo_renut_smaa), against the game frames of the line above.
  if (REXCVAR_GET(banjo_renut_smaa))
    REXLOG_INFO("[renut] SMAA (banjo_renut_smaa, quality {}, threshold {:.3f}): {} frames", SmaaQuality(),
                SmaaThreshold(), g_smaa_runs.exchange(0, std::memory_order_relaxed));
  // banjo-nx (2026-10-21): the internal resolution's frames.
  {
    const uint64_t scaled = g_ui_upscales[0].exchange(0, std::memory_order_relaxed);
    const uint64_t full = g_ui_upscales[1].exchange(0, std::memory_order_relaxed);
    const uint64_t depth = g_ui_depth[0].exchange(0, std::memory_order_relaxed);
    const uint64_t kept = g_ui_depth[1].exchange(0, std::memory_order_relaxed);
    // banjo-nx (2026-10-28): with dynamic resolution on, its ranges (the fixed-height keys are not used then).
    // rr6-nx (2026-11-22): each mode's own (banjo_renut_dynamic_resolution_handheld / _docked).
    const auto mode_heights = [](bool docked) {
      return ::renut::gpu::DynamicModeOn(docked)
                 ? fmt::format("{} dynamic {}-{}", docked ? "docked" : "handheld",
                               ::renut::gpu::DynamicHeight(docked, false), ::renut::gpu::DynamicHeight(docked, true))
                 : fmt::format("{} fixed {}", docked ? "docked" : "handheld",
                               docked ? RenderHeightDocked() : RenderHeightHandheld());
    };
    const std::string heights = mode_heights(false) + ", " + mode_heights(true) +
                                fmt::format(", {} steps between", ::renut::gpu::DynamicSteps());
    if (scaled || RenderHeightHandheld() != 720 || RenderHeightDocked() != 720)
      REXLOG_INFO("[renut] internal resolution ({}, display docked {}, "
                  "{} upscale, not exact): {} composites scaled up for the HUD, {} drawn at the display's resolution, "
                  "{} depth targets scaled up for 3D drawn after the composite, {} draws testing a smaller depth "
                  "target", heights, DisplayHeightDocked(),
                  UpscaleFilter() ? "bicubic" : "bilinear", scaled, full, depth, kept);
  }
  if (PushConstantsOn()) {
    REXLOG_INFO("[renut] pushed constants (banjo_deko3d_push_constants): {} draws, {:.1f} KB of float constants and "
                "{:.1f} KB of shared constants pushed{}",
                g_pushed[2].exchange(0, std::memory_order_relaxed),
                double(g_pushed[0].exchange(0, std::memory_order_relaxed)) / 1024.0,
                double(g_pushed[1].exchange(0, std::memory_order_relaxed)) / 1024.0,
                PushConstantsVerifyOn() ? fmt::format("; verify: {} draws with a register the model got wrong",
                                                      g_pushed[3].exchange(0, std::memory_order_relaxed))
                                        : std::string());
  }
  {
    // banjo-nx (2026-10-19, input latency): the probe and the late input delay.
    const std::string probe = rex::latency::Report();
    REXLOG_INFO("[latency] {}; {}", probe.empty() ? std::string("no frame presented") : probe,
                ::renut::gpu::render_thread::LateInputReport());
    if (const std::string trim = ::renut::gpu::Video::LatencyTrimReport(); !trim.empty())
      REXLOG_INFO("[latency] {}", trim);  // banjo-nx (2026-10-20, banjo_deko3d_latency_trim)
    // banjo-nx (2026-10-19, latency batch 3): the display's refresh, the phase and input -> display.
    if (const std::string display = banjo::display_sync::Report(); !display.empty())
      REXLOG_INFO("[display] {}", display);
  }
  REXLOG_INFO("[renut] upload ring: {:.1f} MB cleaned (render-thread proposal 23); ranges cleaned by the cleaner "
              "thread {}, by the render thread {}; command lists submitted at a kick {} (banjo_deko3d_kick_submit)",
              double(g_ring_published.exchange(0, std::memory_order_relaxed)) / double(1u << 20),
              g_kick_submit[1].exchange(0, std::memory_order_relaxed),
              g_kick_submit[2].exchange(0, std::memory_order_relaxed),
              g_kick_submit[0].exchange(0, std::memory_order_relaxed));
  // banjo-nx (2026-10-20): tools/deko3d_glsl_opt.py's aw4 pass reads bone rows as 16-byte loads; the fetch data's
  // offset, slots and size must stay multiples of 16 (render/draw.cpp UploadVertexFetchData). Expected: 0 off.
  if (const uint64_t checked = g_fetch_misaligned[0].exchange(0, std::memory_order_relaxed)) {
    const uint64_t off = g_fetch_misaligned[1].exchange(0, std::memory_order_relaxed);
    if (off)
      REXLOG_WARN("[renut] fetch data: {} of {} draws with an offset, slot or size off 16 bytes (the aw4 loads read "
                  "the wrong words there)", off, checked);
  }
  if (const uint64_t n = g_exp_adjust[0].exchange(0, std::memory_order_relaxed)) {
    const uint64_t neg = g_exp_adjust[1].exchange(0, std::memory_order_relaxed);
    const uint64_t pos = g_exp_adjust[2].exchange(0, std::memory_order_relaxed);
    REXLOG_WARN("[renut] exponent adjust: {} texture binds with a fetch exponent adjust ({} negative, {} positive), "
                "which Renut does not apply (the sampled values are 2^adjust too large or small)", n, neg, pos);
  }
  if (REXCVAR_GET(banjo_deko3d_skin_once)) {
    uint64_t k[8];
    for (int i = 0; i < 8; ++i)
      k[i] = g_skin_once[i].exchange(0, std::memory_order_relaxed);
    uint64_t m[6];
    for (int i = 0; i < 6; ++i)
      m[i] = g_skin_miss[i].exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[renut] skin once (banjo_deko3d_skin_once): {} fur draws skinned and wrote their rows, {} read them, "
                "{} skinned as before (not the part's first draw with the same inputs: none to read), {} without room; "
                "writes that missed a same-size entry {}: vertex data {}, bone data {}, indices {}, first vertex {}, "
                "shared constants {}; other skinned draws (shadow maps, depth only): {} wrote, {} read, {} as before, {} "
                "without room",
                k[0], k[1], k[2], k[3], m[5], m[0], m[1], m[2], m[3], m[4], k[4], k[5], k[6], k[7]);
  }
  if (REXCVAR_GET(banjo_renut_fur_shells) > 1 || REXCVAR_GET(banjo_renut_portrait_fur_shells) > 1 ||
      REXCVAR_GET(banjo_renut_fur_shells_percent) < 100 || REXCVAR_GET(banjo_renut_portrait_fur_shells_percent) < 100) {
    const uint64_t drawn = g_fur_shells[0].exchange(0, std::memory_order_relaxed);
    const uint64_t skipped = g_fur_shells[1].exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[renut] fur shells (banjo_renut_fur_shells {}, portrait {}, percent {}, portrait percent {}, not "
                "exact): {} drawn, {} not drawn", REXCVAR_GET(banjo_renut_fur_shells),
                REXCVAR_GET(banjo_renut_portrait_fur_shells), REXCVAR_GET(banjo_renut_fur_shells_percent),
                REXCVAR_GET(banjo_renut_portrait_fur_shells_percent), drawn, skipped);
  }
  if (const uint64_t draws = g_chain_reuse[0][0].exchange(0, std::memory_order_relaxed)) {
    uint64_t r[5][2];
    for (int i = 0; i < 5; ++i) {
      r[i][0] = i ? g_chain_reuse[i][0].exchange(0, std::memory_order_relaxed) : draws;
      r[i][1] = g_chain_reuse[i][1].exchange(0, std::memory_order_relaxed);
    }
    REXLOG_INFO("[renut] chain reuse (banjo_renut_chain_reuse; reused of draws): targets {} of {}, textures {} of {}, "
                "viewport {} of {}, pipeline key {} of {}, pipeline {} of {}",
                r[0][1], r[0][0], r[1][1], r[1][0], r[2][1], r[2][0], r[3][1], r[3][0], r[4][1], r[4][0]);
  }
  if (const uint64_t binds = g_header_differ[6].exchange(0, std::memory_order_relaxed)) {
    uint64_t d[6];
    for (int i = 0; i < 6; ++i)
      d[i] = g_header_differ[i].exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[flash] header vs device (same data): {} binds with other fields; by dword: 0 (signs, pitch, tiling) "
                "{}, 3 (swizzle, number format, exponent) {}, 4 (mip levels) {}, 5 (mips) {}, 1/2 {}", binds, d[0], d[3],
                d[4], d[5], d[1] + d[2]);
  }
  if (const uint64_t binds = g_device_swizzle[0].exchange(0, std::memory_order_relaxed)) {
    const uint64_t frames = g_device_swizzle[1].exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[flash] device swizzle: {} binds in {} frames whose texture header's swizzle differed from the "
                "device's; sampled through the {}'s (banjo_renut_device_swizzle)", binds, frames,
                DeviceSwizzleOn() ? "device" : "header");
  }
  if (DepthHandOverOn()) {
    REXLOG_INFO("[renut] depth hand-over (banjo_deko3d_depth_handover): {} resolves not drawn, {} binds of a target's "
                "image, {} images taken at a whole clear; {} drawn after all ({} from a taken image); {} lost; {} binds "
                "for draws writing no depth or stencil that kept it (banjo_deko3d_depth_handover_readonly); drawn "
                "after all for: a draw writing depth or stencil {}, a draw sampling it with its target bound {}, "
                "other {}",
                g_handover[0].exchange(0, std::memory_order_relaxed), g_handover[1].exchange(0, std::memory_order_relaxed),
                g_handover[2].exchange(0, std::memory_order_relaxed), g_handover[3].exchange(0, std::memory_order_relaxed),
                g_handover[4].exchange(0, std::memory_order_relaxed), g_handover[5].exchange(0, std::memory_order_relaxed),
                g_handover[6].exchange(0, std::memory_order_relaxed), g_handover[7].exchange(0, std::memory_order_relaxed),
                g_handover[8].exchange(0, std::memory_order_relaxed), g_handover[9].exchange(0, std::memory_order_relaxed));
  }
  // kameo-nx (2026-11-03): the colour hand-over's own line.
  if (ColorHandOverOn()) {
    REXLOG_INFO("[renut] colour hand-over (banjo_deko3d_color_handover): {} resolves not drawn, {} binds of a target's "
                "image, {} images taken at a whole clear; {} drawn after all ({} from a taken image); {} lost; drawn after "
                "all for: a draw writing the target {}, a draw sampling it with the target bound {}, other {}; drawn "
                "at once: part of the target into another size {}, part into its size {}, whole into another size {}, "
                "a texture with levels {}, format or kind {}",
                g_color_handover[0].exchange(0, std::memory_order_relaxed), g_color_handover[1].exchange(0, std::memory_order_relaxed),
                g_color_handover[2].exchange(0, std::memory_order_relaxed), g_color_handover[3].exchange(0, std::memory_order_relaxed),
                g_color_handover[4].exchange(0, std::memory_order_relaxed), g_color_handover[5].exchange(0, std::memory_order_relaxed),
                g_color_handover[7].exchange(0, std::memory_order_relaxed), g_color_handover[8].exchange(0, std::memory_order_relaxed),
                g_color_handover[9].exchange(0, std::memory_order_relaxed),
                g_color_not_left[0].exchange(0, std::memory_order_relaxed), g_color_not_left[1].exchange(0, std::memory_order_relaxed),
                g_color_not_left[2].exchange(0, std::memory_order_relaxed), g_color_not_left[3].exchange(0, std::memory_order_relaxed),
                g_color_not_left[4].exchange(0, std::memory_order_relaxed));
  }
  // The generated code's self-checks (their "[verify]" line and any DIFFERENCE): banjo-nx's renderer reports them
  // in its own 10 s report; with Renut's renderer nobody did (package 27).
  banjo::CodegenChecksReport(static_cast<int>(since + 0.5));
  g_stats.total = {};
  g_stats.last_report = now;
}

void StatsRenderThreadFrame(double busy_ms) {
  ControlNvkLocked();  // banjo_gpu_nvk_draw_contract (the Switch): requests and guards, once a frame
  int half, target;
  bool apply;
  {
    std::lock_guard lock(g_stats.mutex);
    ++g_stats.total.render_frames;
    g_stats.total.render_ms += busy_ms;
    ++g_stats.halves[g_stats.half].render_frames;
    g_stats.halves[g_stats.half].render_ms += busy_ms;
    {  // sasrt-nx (2026-11-09; port 54): the frame's draws to its half (this runs on the render thread, which counts them)
      static uint64_t last_draws = 0;
      g_stats.halves[g_stats.half].draws += g_ab_draws - last_draws;
      last_draws = g_ab_draws;
    }
    target = AbTargetLocked();
    half = g_stats.half;
    // banjo-nx (2026-10-20, banjo_renut_ab_targets): a new target applies too (its half may be the last one's).
    apply = target && g_stats.applied != target * 2 + half;
    g_stats.applied = target * 2 + half;
  }
  if (apply)
    AbApply(target, half == 1);
}

void StatsGpuFrame(double ms) {
  std::lock_guard lock(g_stats.mutex);
  ++g_stats.total.gpu_frames;
  g_stats.total.gpu_ms += ms;
  ++g_stats.halves[g_stats.half].gpu_frames;
  g_stats.halves[g_stats.half].gpu_ms += ms;
}

bool g_pmull_hash = false;



// banjo-nx (2026-10-19, CPU proposal 5): a counter only the render thread adds to: a load and a store, not an exclusive
// read-modify-write loop (the A57 has no LSE atomics). The 10 s report's exchange may rarely lose one count.
inline void Bump(std::atomic<uint64_t> &c, uint64_t v) {
  c.store(c.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

void CountTessGrid(uint64_t corners, uint64_t vertices) {
  Bump(g_tess_corners, corners);
  Bump(g_tess_vertices, vertices);
}



bool RecordWorkerAbOn() {
  return g_ab_off.load(std::memory_order_relaxed) != 14;
}


bool Deko3DDepthCompressionOn() { return REXCVAR_GET(banjo_deko3d_depth_compression); }
bool Deko3DZcullOn() { return REXCVAR_GET(banjo_deko3d_zcull); }
bool Deko3DMsaaPositionsOn() { return REXCVAR_GET(banjo_deko3d_msaa_positions); }  // 2026-10-30, from sasrt-nx
bool Deko3DA2cDitherOn() { return REXCVAR_GET(banjo_deko3d_a2c_dither); }
bool Deko3DStripRestartOn() { return REXCVAR_GET(banjo_deko3d_strip_restart); }
bool Deko3DGpuRegionOn() { return REXCVAR_GET(banjo_deko3d_gpu_region); }
bool Deko3DBarrierMergeOn() { return REXCVAR_GET(banjo_deko3d_barrier_merge); }
bool Deko3DBarrierLeanOn() { return REXCVAR_GET(banjo_deko3d_barrier_lean); }
bool Deko3DVtxDedupeOn() { return REXCVAR_GET(banjo_deko3d_vtx_dedupe); }
bool Deko3DTextureSkipOn() { return REXCVAR_GET(banjo_deko3d_texture_skip); }  // banjo-nx (2026-10-27)
bool Deko3DProvisionalCountOn() { return REXCVAR_GET(banjo_deko3d_provisional_count); }  // banjo-nx (2026-10-28)
bool Deko3DAsyncVariantsOn() { return REXCVAR_GET(banjo_deko3d_async_variants); }
bool Deko3DVsOutputsOn() { return REXCVAR_GET(banjo_deko3d_vs_outputs); }
bool Deko3DUamPrmtOn() { return REXCVAR_GET(banjo_deko3d_uam_prmt); }
bool EarlyHoistOn() { return REXCVAR_GET(banjo_deko3d_early_hoist); }
bool StallPresentOn() { return REXCVAR_GET(banjo_renut_stall_present); }
bool EarlyZOn() { return REXCVAR_GET(banjo_deko3d_early_z); }
bool Gpr32On() { return REXCVAR_GET(banjo_deko3d_gpr32); }
bool LightBranchOn() { return REXCVAR_GET(banjo_deko3d_light_branch); }
bool SkinOnceOn() { return REXCVAR_GET(banjo_deko3d_skin_once); }
bool EagerKicksOn() {
  return REXCVAR_GET(banjo_deko3d_eager_kicks) && g_ab_off.load(std::memory_order_relaxed) != 18;
}
bool ProgressiveSubmitOn() {  // port 164
  static const bool on = REXCVAR_GET(banjo_deko3d_progressive_submit);
  return on;
}
bool SmaaOn() {
  return REXCVAR_GET(banjo_renut_smaa) && g_ab_off.load(std::memory_order_relaxed) != 19;
}
int SmaaQuality() { return REXCVAR_GET(banjo_renut_smaa_quality); }
double SmaaThreshold() { return REXCVAR_GET(banjo_renut_smaa_threshold); }
std::string SmaaAfterPsList() { return REXCVAR_GET(banjo_renut_smaa_after_ps); }
int32_t SmaaCompareSeconds() { return REXCVAR_GET(banjo_renut_diag_smaa_compare_s); }
void CountExpAdjust(int adjust) {
  Bump(g_exp_adjust[0], 1);
  Bump(g_exp_adjust[adjust < 0 ? 1 : 2], 1);
}
void CountSkinOnce(uint32_t kind) { Bump(g_skin_once[kind & 7], 1); }
void CountSkinMiss(uint32_t fields) {
  for (int i = 0; i < 5; ++i)
    if (fields & (1u << i))
      g_skin_miss[i].fetch_add(1, std::memory_order_relaxed);
  g_skin_miss[5].fetch_add(1, std::memory_order_relaxed);
}
bool NullPsOn() { return REXCVAR_GET(banjo_renut_diag_null_ps); }
bool DeviceSwizzleOn() { return REXCVAR_GET(banjo_renut_device_swizzle); }
bool LatencyTrimOn() { return REXCVAR_GET(banjo_deko3d_latency_trim); }
uint32_t FurShellEvery() { return uint32_t(REXCVAR_GET(banjo_renut_fur_shells)); }
uint32_t PortraitFurShellEvery() { return uint32_t(REXCVAR_GET(banjo_renut_portrait_fur_shells)); }
uint32_t FurShellPercent() { return uint32_t(REXCVAR_GET(banjo_renut_fur_shells_percent)); }
uint32_t PortraitFurShellPercent() { return uint32_t(REXCVAR_GET(banjo_renut_portrait_fur_shells_percent)); }
uint32_t PortraitMsaaSamples() { return uint32_t(REXCVAR_GET(banjo_renut_portrait_msaa_samples)); }
uint32_t MsaaLevel() {
  const int32_t level = REXCVAR_GET(msaa_level);
  return level >= 4 ? 4u : level >= 2 ? 2u : 1u;
}
void CountFurShell(bool drawn) { Bump(g_fur_shells[drawn ? 0 : 1], 1); }
void CountChainReuseN(const uint64_t counts[5][2]) {
  for (int i = 0; i < 5; ++i) {
    Bump(g_chain_reuse[i][0], counts[i][0]);
    Bump(g_chain_reuse[i][1], counts[i][1]);
  }
}
void CountChainReuse(int what, bool reused) {
  Bump(g_chain_reuse[what][0], 1);
  if (reused)
    Bump(g_chain_reuse[what][1], 1);
}
uint32_t TxqMode() {
  return REXCVAR_GET(banjo_deko3d_txq_verify) ? 2u : REXCVAR_GET(banjo_deko3d_txq_constants) ? 1u : 0u;
}
unsigned Deko3DUamFlags() {
  // PRMT (1), offsets in the load (2), the shift distribution (4), the input interpolation cache (8), the choice
  // per pixel shader (16), value numbering (32) keeping the interpolations (128), no reconvergence around uniform
  // branches (256), bone rows as 128-bit loads (1024, 2026-10-20): docs/deko3d-shader-optimisations.md
  unsigned flags = 1471u;
  if (!REXCVAR_GET(banjo_deko3d_uam_wide_rows))
    flags &= ~1024u;
  if (!REXCVAR_GET(banjo_deko3d_uam_prmt))
    flags &= ~1u;
  if (!REXCVAR_GET(banjo_deko3d_uam_gvn))
    flags &= ~32u;
  if (!REXCVAR_GET(banjo_deko3d_uam_uniform_branches))
    flags &= ~256u;
  // Port 163 (from fnxf_xiii): the register pressure passes (2048 the sink, 8192 its neutral moves, 65536 the global
  // sink; a shader still above 56 registers compiled again with the costlier ones: DekoCompiler::TryRescue).
  if (REXCVAR_GET(banjo_deko3d_uam_pressure))
    flags |= 2048u | 8192u | 65536u;
  return flags;
}
// banjo-nx (2026-10-27): the background threads' core: the system core when the process has it
// (banjo_system_core_compiles), else banjo_background_compile_core.
static int32_t BackgroundCore() {
  if (REXCVAR_GET(banjo_system_core_compiles)) {
    const int system_core = RexSwitchSystemCore();
    if (system_core >= 0) return system_core;
  }
  return REXCVAR_GET(banjo_background_compile_core);
}
#if REX_PLATFORM_SWITCH
// sasrt-nx (2026-11-22, sasrt_compile_threads_core -1): the calling thread on every core of the process (the system
// core kept in the mask: RexSwitchSetCurrentThreadCoresWithSystem), no preferred core.
static bool FloatCurrentThread() {
  return RexSwitchSetCurrentThreadCoresWithSystem(-1, RexSwitchProcessCoreMask());
}
static void FloatCompileThread() {
  const bool ok = FloatCurrentThread();
  REXLOG_INFO("[renut] uam thread floats: every core of the process (mask 0x{:X}){} (sasrt_compile_threads_core -1)",
              RexSwitchProcessCoreMask(), ok ? "" : " (NOT accepted)");
}
// The compile threads' place by sasrt_compile_threads_core (>= 0 pinned, -1 floating); false for -2 (the old keys).
static bool PlaceCompileThreadByKey() {
  const int32_t core = REXCVAR_GET(sasrt_compile_threads_core);
  if (core == -1)
    return FloatCurrentThread();
  if (core >= 0)
    return RexSwitchPinCurrentThreadToCore(int(core));
  return false;
}
#endif
void (*Deko3DCompileThreadPlacer())() {
#if REX_PLATFORM_SWITCH
  if (REXCVAR_GET(sasrt_compile_threads_core) == -1)  // floating; a pinned core goes through compileCore
    return &FloatCompileThread;
#endif
  return nullptr;
}
int Deko3DCompileCore() {
  // sasrt-nx (2026-11-22): sasrt_compile_threads_core (-1: placed by Deko3DCompileThreadPlacer; -2: the keys below)
  if (REXCVAR_GET(sasrt_compile_threads_core) != -2)
    return REXCVAR_GET(sasrt_compile_threads_core);
  // banjo-nx (2026-10-28): the uam thread on the system core when there is one (banjo_system_core_uam).
  if (REXCVAR_GET(banjo_system_core_uam)) {
    const int system_core = RexSwitchSystemCore();
    if (system_core >= 0)
      return system_core;
  }
  return BackgroundCore();
}
int Deko3DCompilePriority() { return REXCVAR_GET(banjo_deko3d_compile_priority); }
bool Deko3DCompileWaitBoostOn() { return REXCVAR_GET(banjo_deko3d_compile_wait_boost); }
bool Deko3DFastFirstCompileOn() { return REXCVAR_GET(banjo_deko3d_fast_first_compile); }  // port 56
bool Deko3DAsyncCacheReadsOn() { return REXCVAR_GET(banjo_deko3d_async_cache_reads); }
int Deko3DBackgroundCompileShare() {
  return std::clamp<int32_t>(REXCVAR_GET(banjo_deko3d_background_compile_share), 1, 100);
}


// (CountDepthFloatSampled: inline in the header since 2026-11-20)

void CountDrawSource(int source, uint64_t draws) {
  g_draw_source[source].store(g_draw_source[source].load(std::memory_order_relaxed) + draws, std::memory_order_relaxed);
}

void CountDrawnResolve() {
  Bump(g_resolves_drawn, 1);
}
void CountSmaa() {
  Bump(g_smaa_runs, 1);
}
int32_t RenderHeightHandheld() { return REXCVAR_GET(banjo_renut_render_height_handheld); }
int32_t RenderHeightDocked() { return REXCVAR_GET(banjo_renut_render_height_docked); }
int32_t DisplayHeightDocked() { return REXCVAR_GET(banjo_renut_display_height_docked); }
int UpscaleFilter() { return REXCVAR_GET(banjo_renut_upscale_filter); }
bool HostMipsOn() {
  static const bool on = REXCVAR_GET(banjo_renut_host_mips);
  return on;
}
void CountUiUpscale(bool scaled) { Bump(g_ui_upscales[scaled ? 0 : 1], 1); }
void CountUiDepth(bool filled) { Bump(g_ui_depth[filled ? 0 : 1], 1); }



void CountKickSubmit(int what) {
  g_kick_submit[what].store(g_kick_submit[what].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void CountRingPublished(uint64_t bytes) {
  g_ring_published.store(g_ring_published.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
}

// CountPushedConstants is inline in banjo_renut_tuning.h since port 54 (sasrt-nx's RT4).

void CountFetchAlignment(bool aligned) {
  Bump(g_fetch_misaligned[0], 1);
  if (!aligned)
    Bump(g_fetch_misaligned[1], 1);
}

void CountHeaderDescDiffer(uint32_t dwords) {
  Bump(g_header_differ[6], 1);
  for (int i = 0; i < 6; ++i)
    if (dwords & (1u << i))
      Bump(g_header_differ[i], 1);
}

void CountDeviceSwizzle(uint32_t frame) {
  Bump(g_device_swizzle[0], 1);
  if (g_device_swizzle_frame.exchange(frame, std::memory_order_relaxed) != frame)
    Bump(g_device_swizzle[1], 1);
}

namespace {
template <int kTarget>
[[gnu::always_inline]] inline bool KeyOn(bool key) {
  return key && g_ab_off.load(std::memory_order_relaxed) != kTarget;
}
}  // namespace
bool ColorHandOverOn() {
  static const bool on = REXCVAR_GET(banjo_deko3d_color_handover);
  return KeyOn<73>(on);
}
// blur-nx (2026-11-10, port 76; its key 211): ResolvedAlias's EXPAND / FLOAT pairs (render/textures.cpp).
bool ExpandAliasOn() {
  static const bool on = REXCVAR_GET(blur_renut_expand_alias);
  return KeyOn<211>(on);
}
// blur-nx (2026-11-10, port 76; its key 212): ResolveTarget's kept destination and the converting draw (textures.cpp,
// draw.cpp).
bool ResolveKeepDestOn() {
  static const bool on = REXCVAR_GET(blur_renut_resolve_keep_dest);
  return KeyOn<212>(on);
}
// blur-nx (2026-11-10, port 76; its key 213): D3DQuery_GetData's answer (d3d/hooks/state.cpp AnswerOcclusionQuery).
bool OcclusionQueriesOn() {
  static const bool on = REXCVAR_GET(blur_renut_occlusion_queries);
  return KeyOn<213>(on);
}
// blur-nx (2026-11-10, port 76; its key 215): render/surfaces.cpp CaptureStackHeaders, NoteSurfaceHeader.
bool StackSurfaceHeadersOn() {
  static const bool on = REXCVAR_GET(blur_renut_stack_surface_headers);
  return KeyOn<215>(on);
}
// blur-nx (2026-11-10, port 76; its key 216): render/draw.cpp's sample-grid resolve.
bool SampleGridResolveOn() {
  static const bool on = REXCVAR_GET(blur_renut_sample_grid_resolve);
  return KeyOn<216>(on);
}
// blur-nx (2026-11-10, port 76; its key 217): render/surfaces.cpp GetHostSurfaceSlow.
bool SurfaceSizeGuardOn() {
  static const bool on = REXCVAR_GET(blur_renut_surface_size_guard);
  return KeyOn<217>(on);
}
// blur-nx (2026-11-10, port 76; its key 218): render/draw.cpp ResolveStateLocked's EDRAM wipe, surfaces.cpp
// ColorEdramWipedLocked.
bool WipeOverwrittenOn() {
  static const bool on = REXCVAR_GET(blur_renut_wipe_overwritten);
  return KeyOn<218>(on);
}
// blur-nx (2026-11-10, port 76; its key 219): render/draw.cpp ResolveStateLocked's exponent bias.
bool ResolveExpBiasOn() {
  static const bool on = REXCVAR_GET(blur_renut_resolve_exp_bias);
  return KeyOn<219>(on);
}
// blur-nx (2026-11-11, port 76; its key 221): render/draw.cpp ResolveStateLocked's multisampled slice resolves.
bool MsSliceResolveOn() {
  static const bool on = REXCVAR_GET(blur_renut_ms_slice_resolve);
  return KeyOn<221>(on);
}
// blur-nx (2026-11-11, port 76; its key 220): render/draw.cpp InheritEdramBeforeDrawLocked.
bool EdramInheritOn() {
  static const bool on = REXCVAR_GET(blur_renut_edram_inherit);
  return KeyOn<220>(on);
}
bool ScanOutHandOverOn() {
  static const bool on = REXCVAR_GET(banjo_deko3d_scanout_handover);
  return KeyOn<74>(on);
}
bool DepthHandOverSampledOn() {
  static const bool on = REXCVAR_GET(banjo_deko3d_depth_handover_sampled);
  return KeyOn<75>(on);
}
bool DepthReadLayoutOn() {
  static const bool on = REXCVAR_GET(banjo_deko3d_depth_read_layout);
  return KeyOn<76>(on);
}
bool ResolveKeepsTargetOn() {
  static const bool on = REXCVAR_GET(banjo_deko3d_resolve_keeps_target);
  return KeyOn<77>(on);
}
bool PresentNoPowOn() {
  static const bool on = REXCVAR_GET(banjo_renut_present_no_pow);
  return KeyOn<78>(on);
}
bool PresentTexelLoadOn() {
  static const bool on = REXCVAR_GET(banjo_renut_present_texel_load);
  return KeyOn<79>(on);
}
bool GammaUploadOnceOn() {
  static const bool on = REXCVAR_GET(banjo_renut_gamma_upload_once);
  return on;
}
void CountColorNotLeft(int why) {
  g_color_not_left[why].store(g_color_not_left[why].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
void CountColorHandOver(int what) {
  g_color_handover[what].store(g_color_handover[what].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

bool DepthHandOverReadOnlyOn() {
  static const bool on = REXCVAR_GET(banjo_deko3d_depth_handover_readonly);
  return on && g_ab_off.load(std::memory_order_relaxed) != 64;
}

void CountHandOver(int what) {
  g_handover[what].store(g_handover[what].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void CountLazyWords(int what) {
  Bump(g_words_lazy[what], 1);
}

void CountSkippedResolve(bool depth) {
  Bump(g_resolves_skipped[depth ? 1 : 0], 1);
}

// (CountDepthWords: inline in the header since 2026-11-20)



// vivapinata-nx (2026-11-12, port 115): vivapinata_depth_resolve_skip_unread, but in A/B 156's off half.
bool DepthResolveSkipUnreadOn() {
  static const bool on = REXCVAR_GET(vivapinata_depth_resolve_skip_unread);
  return on && g_ab_off.load(std::memory_order_relaxed) != 156;
}

void CountDepthResolveSkip(int what) {
  Bump(g_depth_resolve_skip[what], 1);
}

void CountDepthOnly() {
  Bump(g_depth_only_draws, 1);
}






void CountBoolSpec(BoolSpecEvent event) {
  Bump(event == BoolSpecEvent::kDraw      ? g_bool_draws
       : event == BoolSpecEvent::kGeneric ? g_bool_generic
                                          : g_bool_built,
       1);
}


void CountExactCheck(uint64_t bytes, bool stale, bool repeat_in_frame) {
  Bump(g_exact_checks, 1);
  Bump(g_exact_bytes, bytes);
  if (stale)
    Bump(g_exact_stale, 1);
  if (repeat_in_frame) {
    Bump(g_exact_repeat, 1);
    Bump(g_exact_repeat_bytes, bytes);
  }
}

void CountCards(uint64_t bytes, bool clean) {
  if (clean) {
    Bump(g_cards_clean, 1);
    Bump(g_cards_clean_bytes, bytes);
  } else {
    Bump(g_cards_dirty, 1);
  }
}

void CountCardsMissed(uint64_t) { Bump(g_cards_missed, 1); }

void CountKickReuse(int kind, uint64_t bytes) {
  Bump(g_kick_reuse[kind], 1);
  if (kind < 2)
    Bump(g_kick_reuse_bytes[kind], bytes);
}

void CountTextureCards(uint64_t bytes, int what) {
  Bump(g_tex_cards[what], 1);
  if (what == 0 || what == 2)
    Bump(g_tex_cards_bytes, bytes);
}

void CountExactMemoHit(uint64_t bytes) {
  Bump(g_exact_memo, 1);
  Bump(g_exact_memo_bytes, bytes);
}

void CountConstantUse(bool vs_indexed, bool ps_indexed, uint32_t registers) {  // blur-nx (2026-11-13, #47)
  Bump(g_const_draws, 1);
  if (vs_indexed)
    Bump(g_const_vs_indexed, 1);
  if (ps_indexed)
    Bump(g_const_ps_indexed, 1);
  Bump(g_const_registers, registers);
}

void CountDrawKind(bool depth_only, bool depth_only_plain, bool writes_nothing, bool partial_mask) {
  if (depth_only)
    Bump(g_kind_depth_only, 1);
  if (depth_only_plain)
    Bump(g_kind_depth_plain, 1);
  if (writes_nothing)
    Bump(g_kind_nothing, 1);
  if (partial_mask)
    Bump(g_kind_partial, 1);
}

void CountKick() {
  Bump(g_kicks, 1);
}

void CountFenceQuery() {
  g_fence_queries.fetch_add(1, std::memory_order_relaxed);
}

bool StreamCachePressureOn() { return REXCVAR_GET(banjo_renut_stream_cache_pressure); }
void CountStreamCachePressure(uint64_t refused, uint64_t dropped_bytes) {
  if (refused)
    g_cache_refused.fetch_add(refused, std::memory_order_relaxed);
  if (dropped_bytes)
    g_cache_pressure_bytes.fetch_add(dropped_bytes, std::memory_order_relaxed);
}

void CountLateStale(uint64_t copies) {
  g_late_stale.fetch_add(copies, std::memory_order_relaxed);
}

uint64_t TextureReserveBytes() {
  return REXCVAR_GET(banjo_renut_memory_policy) ? uint64_t(REXCVAR_GET(banjo_renut_textures_reserve_mb)) << 20 : 0;
}

// sasrt-nx (2026-11-09; port 58; SASRT: "improve our memory management ... 4 player mode will probably be memory
// intensive"; its docked race at max clocks: the 3D at 1080 lines, the race's load took the stream cache to 239 MB in its
// 240 MB pool and the upload rings to 160 MB, two buffers found no room with 97 and 72 MB of the heap free, and the game
// went on at one frame in 12 s until the system closed it). A look at the heap's and the GPU region's free memory every
// 10 frames (surfaces.cpp, the Switch), and what it does: below the low mark dynamic resolution takes no step up,
// render targets of other scales go sooner, the stream cache is capped at 96 MB, the upload rings' heavy-frame chunks
// go at the next reset, unused prebuilt pipelines after 10 s, the thread stacks kept for the next threads, the I/O range
// cache and the image heap's empty chunks go back; below the critical mark dynamic resolution steps down and the cache
// is capped at 48 MB. Copies, targets and pipelines are made again when needed: the same pixels.
REXCVAR_DEFINE_BOOL(sasrt_memory_governor, true, "SASRT/Graphics",
                    "When the heap's free memory (with the GPU region's) is under sasrt_memory_low_mb: no dynamic "
                    "resolution step up, the stream cache capped at 96 MB, idle render targets, caches and kept stacks "
                    "given back sooner; under sasrt_memory_critical_mb: a step down and the cache at 48 MB. The same "
                    "pixels (copies made again)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sasrt_memory_low_mb, 256, "SASRT/Graphics", "sasrt_memory_governor: the low mark (MB free)")
    .range(16, 2048)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sasrt_memory_critical_mb, 128, "SASRT/Graphics", "sasrt_memory_governor: the critical mark (MB free)")
    .range(8, 2048)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
std::atomic<uint32_t> g_heap_free_mb{UINT32_MAX};
std::atomic<uint32_t> g_memory_emergency_looks{0};
bool MemoryLow() {
  static const bool on = REXCVAR_GET(sasrt_memory_governor);
  static const uint32_t mark = uint32_t(REXCVAR_GET(sasrt_memory_low_mb));
  return on && (g_heap_free_mb.load(std::memory_order_relaxed) < mark ||
                g_memory_emergency_looks.load(std::memory_order_relaxed) != 0);
}
bool MemoryCritical() {
  static const bool on = REXCVAR_GET(sasrt_memory_governor);
  static const uint32_t mark = uint32_t(REXCVAR_GET(sasrt_memory_critical_mb));
  return on && (g_heap_free_mb.load(std::memory_order_relaxed) < mark ||
                g_memory_emergency_looks.load(std::memory_order_relaxed) != 0);
}
uint32_t MemoryLowMarkMb() {
  static const uint32_t mark = uint32_t(REXCVAR_GET(sasrt_memory_low_mb));
  return mark;
}

uint64_t StreamCacheMaxBytes(uint64_t renut_max) {
  uint64_t cap = REXCVAR_GET(banjo_renut_memory_policy) ? uint64_t(REXCVAR_GET(banjo_renut_stream_cache_mb)) << 20 : renut_max;
  if (MemoryCritical())  // sasrt-nx (2026-11-09, sasrt_memory_governor; port 58)
    cap = std::min<uint64_t>(cap, uint64_t(48) << 20);
  else if (MemoryLow())
    cap = std::min<uint64_t>(cap, uint64_t(96) << 20);
  return cap;
}

uint64_t StreamCacheIdleFrames(uint64_t renut_frames) {
  return REXCVAR_GET(banjo_renut_memory_policy) ? uint64_t(REXCVAR_GET(banjo_renut_stream_cache_idle_s)) * 30
                                                : renut_frames;
}

uint32_t UploadTrimResets(uint32_t renut_resets, uint32_t frames_in_flight) {
  if (!REXCVAR_GET(banjo_renut_memory_policy))
    return renut_resets;
  if (MemoryLow())  // sasrt-nx (2026-11-09, sasrt_memory_governor; port 58): a heavy frame's chunks go at the next reset
    return 1;
  return std::max<uint32_t>(1, uint32_t(REXCVAR_GET(banjo_renut_upload_trim_frames)) / std::max<uint32_t>(frames_in_flight, 1));
}

void CountReserveEviction(uint64_t textures) {
  g_reserve_evicted.fetch_add(textures, std::memory_order_relaxed);
}

#if REX_PLATFORM_SWITCH
namespace {

// The per-draw contract under either name (banjo_gpu_nvk_draw_contract: the linked Mesa exports nfsmw-nx's).
NvkSwitchDraw *NvkDrawContract() {
  if (&nvk_switch_draw != nullptr)
    return &nvk_switch_draw;
  static const bool also_dibujo =
      EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_nvk_draw_contract));
  return also_dibujo ? &nvk_switch_dibujo : nullptr;
}

struct NvkState {
  bool logged = false, preload = false;
  bool off_warned[5] = {}, set4_off_warned = false;
  uint64_t parts[16][2] = {}, counts[13] = {}, improvements[5][4] = {}, set4[4] = {};
  bool reported = false;
};
NvkState g_nvk;

}  // namespace
#endif

void ControlNvkLocked() {
#if REX_PLATFORM_SWITCH
  NvkSwitchDraw *const nvk = NvkDrawContract();
  NvkSwitchSet4 *const set4 = &nvk_switch_set4;
  const bool draw_ok = nvk != nullptr && nvk->version == 1;
  const bool set4_ok = set4 != nullptr && set4->version == 1;
  const bool set4_request = REXCVAR_GET(masseffect_native_set4_differences);
  const int32_t requests[5] = {REXCVAR_GET(masseffect_native_nvk_emission) ? 1 : 0,
                               REXCVAR_GET(masseffect_native_nvk_cbufs) ? 1 : 0,
                               REXCVAR_GET(masseffect_native_nvk_dynamic) ? 1 : 0,
                               0,  // the fast set 4 path is not applied (as banjo-nx's renderer)
                               REXCVAR_GET(masseffect_native_nvk_preload) ? 1 : 0};
  if (draw_ok) {
    int32_t measure = std::max<int32_t>(0, REXCVAR_GET(masseffect_native_nvk_measure));
    if (measure > 0)
      measure = int32_t(std::bit_floor(uint32_t(measure)));  // NVK wants a power of 2
    __atomic_store_n(&nvk->measure, measure, __ATOMIC_RELAXED);
    for (size_t m = 0; m < 5; ++m)
      __atomic_store_n(&nvk->improvements[m].request, requests[m], __ATOMIC_RELAXED);
    static constexpr const char *kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "prefetch"};
    for (size_t m = 0; m < 5; ++m) {
      if (!g_nvk.off_warned[m] && __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0) {
        g_nvk.off_warned[m] = true;
        REXLOG_ERROR("[renut] NVK per draw: MISMATCH seen by the guard of '{}' ({} differing checks; details in "
                     "rex_stderr.log). Switched off by NVK for the rest of the session: done as usual",
                     kNames[m], __atomic_load_n(&nvk->improvements[m].differences, __ATOMIC_RELAXED));
      }
    }
  }
  g_nvk.preload = draw_ok && requests[4] != 0;
  if (set4_ok) {
    __atomic_store_n(&set4->request, set4_request ? 1 : 0, __ATOMIC_RELAXED);
    if (!g_nvk.set4_off_warned && __atomic_load_n(&set4->off, __ATOMIC_RELAXED) != 0) {
      g_nvk.set4_off_warned = true;
      REXLOG_ERROR("[renut] NVK dynamic descriptors by differences: MISMATCH seen by its guard ({} cbufs wrongly "
                   "bound, each fixed in its own draw). Switched off for the rest of the session",
                   __atomic_load_n(&set4->total.differences, __ATOMIC_RELAXED));
    }
  }
  if (!g_nvk.logged) {
    g_nvk.logged = true;
    if (draw_ok) {
      REXLOG_INFO("[renut] NVK per draw (banjo_gpu_nvk_draw_contract): improvements requested (emission {}, cbufs {}, "
                  "dynamic {}, prefetch {}), measuring 1 in {} calls; dynamic descriptors by differences {}",
                  requests[0], requests[1], requests[2], requests[4], REXCVAR_GET(masseffect_native_nvk_measure),
                  set4_ok ? (set4_request ? "requested" : "not requested") : "not in this NVK");
    } else {
      REXLOG_INFO("[renut] NVK per draw: this NVK lacks the contract (or banjo_gpu_nvk_draw_contract is off): no "
                  "measurement and no improvements; dynamic descriptors by differences {}",
                  set4_ok ? (set4_request ? "requested" : "not requested") : "not in this NVK");
    }
  }
#endif
}

void BeforePipelineBind(plume::RenderPipeline *pipeline) {
#if REX_PLATFORM_SWITCH
  // PRFM only: it reads and writes nothing and cannot fail (banjo-nx's renderer does the same before its binds).
  if (g_nvk.preload && pipeline && vk_switch_preload_pipeline && ::renut::gpu::IsVulkan())
    vk_switch_preload_pipeline(static_cast<plume::VulkanGraphicsPipeline *>(pipeline)->vk);
#else
  (void)pipeline;
#endif
}

void ReportNvk() {
#if REX_PLATFORM_SWITCH
  NvkSwitchDraw *const nvk = NvkDrawContract();
  if (nvk == nullptr || nvk->version != 1)
    return;
  uint64_t times[16], ticks[16], counts[13], improvements[5][4];
  for (size_t i = 0; i < 16; ++i) {
    const uint64_t v = __atomic_load_n(&nvk->parts[i].times, __ATOMIC_RELAXED);
    const uint64_t t = __atomic_load_n(&nvk->parts[i].ticks, __ATOMIC_RELAXED);
    times[i] = v - g_nvk.parts[i][0];
    ticks[i] = t - g_nvk.parts[i][1];
    g_nvk.parts[i][0] = v;
    g_nvk.parts[i][1] = t;
  }
  for (size_t i = 0; i < 13; ++i) {
    const uint64_t c = __atomic_load_n(&nvk->counts[i], __ATOMIC_RELAXED);
    counts[i] = c - g_nvk.counts[i];
    g_nvk.counts[i] = c;
  }
  for (size_t m = 0; m < 5; ++m) {
    const uint64_t c[4] = {__atomic_load_n(&nvk->improvements[m].uses, __ATOMIC_RELAXED),
                           __atomic_load_n(&nvk->improvements[m].validated, __ATOMIC_RELAXED),
                           __atomic_load_n(&nvk->improvements[m].differences, __ATOMIC_RELAXED),
                           __atomic_load_n(&nvk->improvements[m].no_check, __ATOMIC_RELAXED)};
    for (size_t k = 0; k < 4; ++k) {
      improvements[m][k] = c[k] - g_nvk.improvements[m][k];
      g_nvk.improvements[m][k] = c[k];
    }
  }
  if (!g_nvk.reported) {  // the first call takes the starting point
    g_nvk.reported = true;
    return;
  }
  const uint64_t tps = __atomic_load_n(&nvk->ticks_per_second, __ATOMIC_RELAXED);
  const double us_tick = tps ? 1e6 / double(tps) : 0.0;
  const uint64_t draws = counts[0];
  auto part = [&](size_t i) {
    return fmt::format("{:.2f} [{:.2f}]", times[i] ? double(ticks[i]) * us_tick / double(times[i]) : 0.0,
                       draws ? double(ticks[i]) * us_tick / double(draws) : 0.0);
  };
  if (draws) {
    REXLOG_INFO("[renut] NVK parts (us per measured call [us per measured draw]; {} measured draws): whole draw {} | "
                "push desc {} | dynamic {} | shaders {} | cbufs {} | emit {} | BindPipeline: shaders {} state copy {} "
                "| sets {} (root {} dirty {}) | root table {} | new chunk {} | BindVertexBuffers {}",
                draws, part(0), part(1), part(2), part(4), part(5), part(6), part(7), part(9), part(10), part(11),
                part(12), part(13), part(14), part(15));
  }
  static constexpr size_t kShown[4] = {0, 1, 2, 4};
  static constexpr const char *kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "prefetch"};
  std::string text;
  for (const size_t m : kShown) {
    const char *state = __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0 ? "SWITCHED OFF by the guard"
                        : __atomic_load_n(&nvk->improvements[m].request, __ATOMIC_RELAXED) == 0 ? "not requested"
                                                                                           : "on";
    text += fmt::format(" | {} ({}): {} uses, {} checked equal, {} differing, {} unchecked", kNames[m], state,
                        improvements[m][0], improvements[m][1], improvements[m][2], improvements[m][3]);
  }
  if (NvkSwitchSet4 *const set4 = &nvk_switch_set4; set4 != nullptr && set4->version == 1) {
    const uint64_t c[4] = {__atomic_load_n(&set4->total.bindings_difference, __ATOMIC_RELAXED),
                           __atomic_load_n(&set4->total.cbufs_saved, __ATOMIC_RELAXED),
                           __atomic_load_n(&set4->total.checks, __ATOMIC_RELAXED),
                           __atomic_load_n(&set4->total.differences, __ATOMIC_RELAXED)};
    text += fmt::format(" | dynamic descriptors by differences ({}): {} binds, {} cbuf rebinds saved, {} checked, {} "
                        "differing",
                        __atomic_load_n(&set4->off, __ATOMIC_RELAXED) ? "SWITCHED OFF by the guard"
                        : __atomic_load_n(&set4->request, __ATOMIC_RELAXED) > 0 ? "on" : "not requested",
                        c[0] - g_nvk.set4[0], c[1] - g_nvk.set4[1], c[2] - g_nvk.set4[2], c[3] - g_nvk.set4[3]);
    for (size_t k = 0; k < 4; ++k)
      g_nvk.set4[k] = c[k];
  }
  REXLOG_INFO("[renut] NVK improvements (last 10 s){}", text);
#endif
}


std::atomic<bool> g_trace_frame{false};  // banjo-nx (2026-10-19): read inline by ResolveTraceOn
namespace {
std::chrono::steady_clock::time_point g_trace_last{};
}  // namespace

void ResolveTraceFrameStart() {
  if (!REXCVAR_GET(banjo_renut_diag_resolve_trace)) {
    g_trace_frame.store(false, std::memory_order_relaxed);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (g_trace_frame.load(std::memory_order_relaxed)) {
    g_trace_frame.store(false, std::memory_order_relaxed);
    REXLOG_INFO("[trace] frame end");
  } else if (g_trace_last == std::chrono::steady_clock::time_point{} ||
             now - g_trace_last >= std::chrono::seconds(30)) {
    g_trace_last = now;
    g_trace_frame.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[trace] frame start");
  }
}


int DiagBuildDelayMs() {
  return REXCVAR_GET(banjo_renut_diag_build_delay_ms);
}


uint64_t PmullHash(const void *data, size_t size, uint64_t seed) {
#if BANJO_PMULL_FINGERPRINT
  return pmull_fingerprint::Fingerprint(static_cast<const uint8_t *>(data), size, seed);
#else
  return XXH3_64bits_withSeed(data, size, seed);
#endif
}

void OnRenderThreadStart() {
  LatchTuning();  // banjo-nx (2026-10-27): before any draw reads the switches (Latch's plain load)
#if REX_PLATFORM_SWITCH
  const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(kRenderThreadPriority);
  const int32_t core = REXCVAR_GET(banjo_ring_core);
  const bool core_ok = core < 0 || RexSwitchPinCurrentThreadToCore(int(core));
  REXLOG_INFO("[renut] render thread at priority 0x{:X}{}, {} (banjo_ring_core), now on core {}",
              kRenderThreadPriority, priority_ok ? "" : " (NOT accepted)",
              core < 0 ? std::string("not pinned")
                       : fmt::format("pinned to core {}{}", core, core_ok ? "" : " (NOT accepted)"),
              RexSwitchCurrentCore());
  // banjo-nx (2026-10-27): the four-core mode's state (sdk/src/core/threading_switch.h).
  {
    char cores[384];
    RexSwitchSystemCoreDescribe(cores, sizeof(cores));
    REXLOG_INFO("[banjo] system core: {}", cores);
  }
#endif
}

int BackgroundThreadPriority() { return kBackgroundPriority; }  // blur-nx (2026-11-18, #11)
void OnBackgroundThreadStart(const char *what) {
#if REX_PLATFORM_SWITCH
  const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(kBackgroundPriority);
  // sasrt-nx (2026-11-22): the compile threads by sasrt_compile_threads_core; the ring cleaner (not a compile) stays
  // where banjo_system_core_compiles places it
  if (REXCVAR_GET(sasrt_compile_threads_core) != -2 && std::string_view(what) != "ring cleaner") {
    const int32_t key_core = REXCVAR_GET(sasrt_compile_threads_core);
    const bool place_ok = PlaceCompileThreadByKey();
    REXLOG_INFO("[renut] {} at priority 0x{:X}{}, {}{} (sasrt_compile_threads_core)", what, kBackgroundPriority,
                priority_ok ? "" : " (NOT accepted)",
                key_core < 0 ? fmt::format("floating on every core of the process (mask 0x{:X})", RexSwitchProcessCoreMask())
                             : fmt::format("pinned to core {}", key_core),
                place_ok ? "" : " (NOT accepted)");
    return;
  }
  const int32_t core = BackgroundCore();  // banjo-nx (2026-10-27): the system core when there is one
  const bool core_ok = core < 0 || RexSwitchPinCurrentThreadToCore(int(core));
  REXLOG_INFO("[renut] {} at priority 0x{:X}{}, {} (banjo_background_compile_core / banjo_system_core_compiles)",
              what, kBackgroundPriority,
              priority_ok ? "" : " (NOT accepted)",
              core < 0 ? std::string("not pinned")
                       : fmt::format("pinned to core {}{}", core, core_ok ? "" : " (NOT accepted)"));
#else
  (void)what;
#endif
}

void OnPrebuildWorkerStart() {
  OnBackgroundThreadStart("pipeline prebuild worker");
}

// Port 171 (from fnxf_xiii, its note of 2026-11-05): render/textures.cpp's untile helpers, at the render thread's
// priority on cores 0 and 2 (fnxf_xiii tried the main thread's 0x3B on core 0: no gain, and a helper that lost core 0
// to main held the run's end).
void OnCopyHelperStart(uint32_t index) {
#if REX_PLATFORM_SWITCH
  const int core = index == 0 ? 0 : 2;
  const int priority = int(kRenderThreadPriority);
  const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(priority);
  const bool core_ok = RexSwitchPinCurrentThreadToCore(core);
  REXLOG_INFO("[renut] untile helper {} at priority 0x{:X}{}, core {}{}", index, priority,
              priority_ok ? "" : " (NOT accepted)", core, core_ok ? "" : " (NOT accepted)");
#else
  (void)index;
#endif
}

uint32_t PrebuildReleaseSeconds() {
  static const uint32_t s = uint32_t(std::max(0, int(REXCVAR_GET(banjo_renut_prebuild_release_s))));
  // sasrt-nx (2026-11-09, sasrt_memory_governor; port 58): prebuilt pipelines no draw took for 10 s go while memory is low
  // (3,080 of 3,617 were held unused in SASRT's docked race's load; a draw that asks for one builds it again)
  return s && MemoryLow() ? std::min<uint32_t>(s, 10) : s;
}

uint32_t PrebuildWorkers(uint32_t renut_count) {
#if REX_PLATFORM_SWITCH
  (void)renut_count;
  return uint32_t(std::clamp<int32_t>(REXCVAR_GET(banjo_prewarm_threads), 1, 3));
#else
  return renut_count;
#endif
}

void StartLongLivedThread(void (*fn)(), size_t stack_bytes) {
#if REX_PLATFORM_SWITCH
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack_bytes);
  pthread_t thread;
  const int error = pthread_create(
      &thread, &attr, [](void *arg) -> void * {
        reinterpret_cast<void (*)()>(arg)();
        return nullptr;
      },
      reinterpret_cast<void *>(fn));
  pthread_attr_destroy(&attr);
  if (error) {
    REXLOG_ERROR("[renut] thread creation failed ({}), {} KB stack: running it as a std::thread", error,
                 stack_bytes >> 10);
    new std::thread(fn);
  }
#else
  (void)stack_bytes;
  new std::thread(fn);  // never joined or destroyed (a joinable std::thread destroyed at exit would terminate)
#endif
}

std::filesystem::path NextToExecutable(const std::string &path) {
  const std::filesystem::path p(path);
  return p.is_relative() ? rex::filesystem::GetExecutableFolder() / p : p;
}


// CountSharedConstants is inline in banjo_renut_tuning.h since port 54 (sasrt-nx's RT4).





bool UploadCached() {
#if REX_PLATFORM_SWITCH
  static const bool on = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_upload_cached));
  return on;
#else
  return false;
#endif
}

void BeforeDeviceCreated() {
  renut::gpu::native::g_touch_compare = REXCVAR_GET(banjo_renut_touch_compare);  // banjo-nx (2026-10-26)
  renut::gpu::native::g_tags_apart = REXCVAR_GET(banjo_renut_tags_apart);
  // The two rules outside the enhancement table (no master switch), stated once.
  REXLOG_INFO("[renut] banjo_renut_exact_streams {}; banjo_renut_keep_gpu_written {}{}; banjo_renut_memory_policy {}{}",
              REXCVAR_GET(banjo_renut_exact_streams), REXCVAR_GET(banjo_renut_keep_gpu_written),
              REXCVAR_GET(banjo_renut_diag_poison_slots) ? " (TEST: banjo_renut_diag_poison_slots)" : "",
              REXCVAR_GET(banjo_renut_memory_policy),
              REXCVAR_GET(banjo_renut_memory_policy)
                  ? fmt::format(" (textures reserve {} MB, stream cache {} MB and {} s idle, upload trim {} frames)",
                                REXCVAR_GET(banjo_renut_textures_reserve_mb), REXCVAR_GET(banjo_renut_stream_cache_mb),
                                REXCVAR_GET(banjo_renut_stream_cache_idle_s), REXCVAR_GET(banjo_renut_upload_trim_frames))
                  : std::string());
  if (pmull_fingerprint::kAvailable && EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_pmull_hash))) {
    g_pmull_hash = PmullSelfTest();
    REXLOG_INFO("[renut] content hashes: {}", g_pmull_hash ? "PMULL CRC-64 (banjo_renut_pmull_hash)"
                                                           : "XXH3 (the PMULL self-test FAILED)");
  }
  if (!::renut::gpu::IsVulkan())
    return;
  g_precise_barriers_on = EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_precise_barriers));
  plume::SetVulkanPreciseBarriers(g_precise_barriers_on);
  // banjo_gpu_robustness_off (package 27, from banjo-nx's renderer): Renut's pipelines without buffer and image
  // robustness, so NVK drops its two descriptor index clamps from every texture handle. Renut's indices are in range by
  // construction: bindless slots below the 8,192 of the table (unused ones point at the null texture), constant
  // registers inside the 4,096 bytes of their uniform buffer (Maxwell's constant banks return 0 past a bank's size in
  // any case), the rest raw buffer-address loads, which robustness does not cover.
  plume::SetVulkanPipelineRobustnessOff(EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_robustness_off)));
  if (!PipelineCacheOn())
    return;
  std::error_code ec;
  const std::filesystem::path folder = rex::filesystem::GetExecutableFolder() / "cache";
  std::filesystem::create_directories(folder, ec);
  const std::filesystem::path path = folder / "renut_pipelines.bin";
  // The shader package in the tag: a new one starts the file over, so entries of shaders no build has any more do not
  // pile up (banjo-nx's own cache once reached 258 MB that way). The driver checks its own version and cache UUID.
  const std::string tag = fmt::format("renut|layout {}|spirv {} {}|entries {}", g_shaderCacheLayout,
                                      g_spirvCacheCompressedSize, g_spirvCacheDecompressedSize,
                                      g_shaderCacheEntryCount);
  plume::ConfigureVulkanPipelineCache(path.string(), tag);
#if defined(__SWITCH__)
  // A save writes nothing, without asking the driver for its data, when every creation since the last one hit.
  plume::SetSwitchPipelineCacheSaveOnMiss(true);
#endif
  REXLOG_INFO("[renut] pipeline cache {} (banjo_renut_pipeline_cache), key \"{}\"", path.string(), tag);
}

void OnDeviceCreated(plume::RenderDevice *device) {
  if (device && ::renut::gpu::IsVulkan()) {
    const bool wanted = EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_robustness_off));
    REXLOG_INFO("[renut] pipeline robustness (banjo_gpu_robustness_off): {}",
                !wanted ? "as the device has it (key off)"
                : plume::VulkanPipelineRobustnessAvailable(device) ? "off for the game's pipelines (VK_EXT_pipeline_robustness)"
                                                                   : "as the device has it (no VK_EXT_pipeline_robustness)");
  }
  if (!device || !PipelineCacheOn())
    return;
  g_saver.device = device;
  g_saver.stop = false;
  g_saver.thread = std::thread([] {
#if REX_PLATFORM_SWITCH
    (void)RexSwitchSetCurrentThreadPriorityOk(kBackgroundPriority);
    const int32_t core = REXCVAR_GET(banjo_background_compile_core);
    if (REXCVAR_GET(sasrt_compile_threads_core) != -2)  // sasrt-nx (2026-11-22)
      (void)PlaceCompileThreadByKey();
    else if (core >= 0)
      (void)RexSwitchPinCurrentThreadToCore(int(core));
#endif
    std::unique_lock lock(g_saver.mutex);
    // banjo_renut_cache_save_quiet (package 28): a save once the compiling has stopped for 15 s (a level loaded, a
    // scene shown), at most once a minute, or after 5 minutes of compiling without such a pause.
    const bool quiet = EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_cache_save_quiet));
    uint64_t saved_builds = g_pipelines_built.load(std::memory_order_relaxed);
    int64_t last_save = SteadyMs();
    for (;;) {
      if (g_saver.cv.wait_for(lock, std::chrono::seconds(quiet ? 5 : 60), [] { return g_saver.stop; }))
        return;
      lock.unlock();
      if (!quiet) {
        SaveNow("new pipelines in the last minute");
      } else {
        const uint64_t builds = g_pipelines_built.load(std::memory_order_relaxed);
        const int64_t now = SteadyMs();
        const int64_t since_build = now - g_last_build_ms.load(std::memory_order_relaxed);
        if (builds != saved_builds && now - last_save >= 60000 && (since_build >= 15000 || now - last_save >= 300000)) {
          SaveNow(since_build >= 15000 ? "no pipeline built for 15 s" : "5 minutes of compiling");
          saved_builds = builds;
          last_save = now;
        }
      }
      lock.lock();
    }
  });
}

void BeforeDeviceDestroyed() {
  record::Shutdown();  // banjo-nx (package 28b): the record worker has played everything and stopped
  if (!g_saver.thread.joinable())
    return;
  {
    std::lock_guard lock(g_saver.mutex);
    g_saver.stop = true;
  }
  g_saver.cv.notify_all();
  g_saver.thread.join();
  SaveNow("exit");
}

// banjo-nx (2026-10-19, CPU proposal 5): the per-draw switches, latched once (the cvars are final by the first draw,
// as the function-local statics they replace assumed); the A/B word stays a live relaxed load (a plain load on
// ARM).
Latched g_latched;
std::atomic<bool> g_latched_ready{false};

void LatchTuning() {
  g_latched.bool_spec = ::renut::gpu::UsesVulkanBindings() && EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_bool_spec));
  g_latched.tess_grid = EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_tess_grid));
  g_latched.depth_only = EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_depth_only));
  g_latched.fetch_spec = ::renut::gpu::UsesVulkanBindings() && EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_fetch_spec));
  g_latched.output_trim = ::renut::gpu::UsesVulkanBindings() && EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_output_trim));
  g_latched.depth_words_view = EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_depth_words_view));
  g_latched.tess_spec = REXCVAR_GET(banjo_deko3d_tess_spec);
  g_latched.alpha_spec = REXCVAR_GET(banjo_deko3d_alpha_spec);
  g_latched.draw_resolves = REXCVAR_GET(banjo_deko3d_draw_resolves);
  g_latched.resolve_skip = REXCVAR_GET(banjo_renut_resolve_skip);
  g_latched.depth_words_lazy = REXCVAR_GET(banjo_renut_depth_words_lazy);
  g_latched.depth_handover = REXCVAR_GET(banjo_deko3d_depth_handover);
  g_latched.depth24 = REXCVAR_GET(banjo_deko3d_depth24);
  g_latched.push_constants = REXCVAR_GET(banjo_deko3d_push_constants);
  g_latched.kick_submit = REXCVAR_GET(banjo_deko3d_kick_submit);
  g_latched.late_input = REXCVAR_GET(banjo_late_input);
  g_latched.ring_cleaner = REXCVAR_GET(banjo_deko3d_ring_cleaner);
  g_latched.push_constants_verify = REXCVAR_GET(banjo_deko3d_push_constants_verify);
  g_latched.diag_gpu_shaders = REXCVAR_GET(banjo_renut_diag_gpu_shaders);
  g_latched.diag_trim_poison = REXCVAR_GET(banjo_renut_diag_trim_poison);
  g_latched.half_pixel_offset = REXCVAR_GET(banjo_renut_half_pixel_offset);
  g_latched.point_minmax_radius = REXCVAR_GET(banjo_renut_point_minmax_radius);
  g_latched.depth_words_stencil = REXCVAR_GET(banjo_renut_depth_words_stencil);
  g_latched.alpha_to_mask_test = REXCVAR_GET(banjo_renut_alpha_to_mask_test);
  g_latched.blend_skip = ::renut::gpu::UsesVulkanBindings() && REXCVAR_GET(banjo_renut_blend_skip) && EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_blend_skip));
  g_latched.exact_streams = REXCVAR_GET(banjo_renut_exact_streams);
  g_latched.keep_gpu_written = REXCVAR_GET(banjo_renut_keep_gpu_written);
  g_latched.poison_slots = REXCVAR_GET(banjo_renut_diag_poison_slots);
  g_latched.shared_dedupe = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_shared_dedupe));
  g_latched.queue_notify_idle = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_queue_notify_idle));
  g_latched.neon_swap = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_neon_swap));
  g_latched.capture_pool = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_capture_pool));
  g_latched.push_point_only = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_push_point_only));
  g_latched.pipeline_memo = REXCVAR_GET(banjo_renut_pipeline_memo);
  g_latched.cpu_memos = REXCVAR_GET(banjo_renut_cpu_memos);
  g_latched.store_cards = REXCVAR_GET(banjo_renut_store_cards) && g_banjo_store_cards;
  g_latched.store_cards_verify = g_latched.store_cards && REXCVAR_GET(banjo_renut_store_cards_verify);
  // bangaio-nx (2026-10-29): the delta chain's (backend/queued.cpp); verify only with the deltas, as before.
  g_latched.deltas_narrow = REXCVAR_GET(banjo_renut_snapshot_deltas_narrow);
  g_latched.deltas_verify = REXCVAR_GET(banjo_renut_snapshot_deltas) && REXCVAR_GET(banjo_renut_snapshot_deltas_verify);
  g_latched.neon_compare = REXCVAR_GET(banjo_renut_neon_compare);
  g_latched.capture_batches = REXCVAR_GET(banjo_renut_capture_batches);
  g_latched.small_captures = REXCVAR_GET(banjo_renut_small_captures);
  g_latched.delta_masks = REXCVAR_GET(banjo_renut_delta_masks);
  g_latched.delta_prefetch = REXCVAR_GET(banjo_renut_delta_prefetch);
  g_latched.counts = REXCVAR_GET(log_level) != "off" && REXCVAR_GET(banjo_renut_draw_counts);  // banjo-nx (2026-11-20, batch #4)
  g_latched.ready = true;  // banjo-nx (2026-11-20, batch #3): Latch() reads this one
  g_latched_ready.store(true, std::memory_order_release);
}

}  // namespace banjo::renut_gpu

// kameo-nx (2026-11-05): at the end of the file, so that the other functions keep their PGO profile.
REXCVAR_DEFINE_INT32(banjo_tlb_test_s, 0, "Banjo/Diagnostics",
                     "Once, this many seconds after the first report: the TLB's cost in game memory and the NRO's code, "
                     "in the log (a few hundred ms on the render thread). Only the console's numbers mean anything. 0: never");

#if REX_PLATFORM_SWITCH
extern "C" void RexGmLatencyTest(void (*emit)(const char*));
#endif

void BanjoTlbTestTick() {
#if REX_PLATFORM_SWITCH
  const int32_t seconds = REXCVAR_GET(banjo_tlb_test_s);
  if (seconds <= 0) return;
  static const auto first = std::chrono::steady_clock::now();
  static bool done = false;
  if (done || std::chrono::steady_clock::now() - first < std::chrono::seconds(seconds)) return;
  done = true;
  RexGmLatencyTest([](const char* line) { REXLOG_INFO("{}", line); });
#endif
}

// blur-nx (2026-11-18, the Renut host batch: gpu/host_batch.h; docs/renut-host-ideas-25-1118.md). Each switch is exact
// (the same pixels) and an in-run A/B target (host_batch::kAbBase + its item); the census is a diagnostic.
REXCVAR_DEFINE_BOOL(blur_renut_record_skip_unused, true, "Banjo/Enhancements",
                    "blur-nx (#1, #2; A/B 230): while banjo_renut_replay_inherits is on, NewOp builds no resource "
                    "objects (banjo_renut_resource_objects) and no slot extents (banjo_renut_record_extents): a replay "
                    "with inherited words uses neither (Blur: every replay)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_inherit_by_chunk, true, "Banjo/Enhancements",
                    "blur-nx (#3; A/B 231): a replayed op's inherited words are put back from the running device only "
                    "in the chunks the op applied (ApplyState writes no other), not every inherited run at every op")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_whole_repeat, true, "Banjo/Enhancements",
                    "blur-nx (#4; A/B 232): a draw whose streams, index buffer, shader and layout are the draw before's, "
                    "in the same kick and frame and with no export pending, takes that draw's whole-buffer copies "
                    "without WholeGeometryTry's probes and lookups (their proofs and uses were made by that draw)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(blur_renut_ring_cleaner_priority, 0x2D, "Banjo/Enhancements",
                     "blur-nx (#11; A/B 233): the upload ring cleaner's priority on the background core (0x2D: above "
                     "the game threads' 0x3B, below the render thread and audio), so that it cleans the ranges before "
                     "the submit needs them instead of the render thread taking them over. 0: the background priority")
    .range(0, 0x3F)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_inherit_runs_words, true, "Banjo/Enhancements",
                    "blur-nx (#13; A/B 234): BuildInheritRuns works on the set-word bitmap a 32-bit word at a time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_fast_pipeline_two_way, true, "Banjo/Enhancements",
                    "blur-nx (#16; A/B 235): the fast path's pipeline cache looks in two neighbouring entries (the "
                    "older one replaced on a miss), not one: its conflicts each made the key and looked it up again")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_provisional_key_kept, true, "Banjo/Enhancements",
                    "blur-nx (#18; A/B 237): a pipeline cache entry with this draw's inputs from an earlier prebuild "
                    "epoch is looked up again with the key it holds (made from the same inputs), not a new MakeKey")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_wake_batch_all, false, "Banjo/Enhancements",
                    "blur-nx (#28; A/B 238): command buffer runs and resolves wake a sleeping render thread as draws do "
                    "(once banjo_renut_wake_batch items wait; it looks every 2 ms anyway), not at once. The same "
                    "commands; the Game Master makes fewer wake calls, the render thread may start an item up to 2 ms "
                    "later")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_viewport_memo, true, "Banjo/Enhancements",
                    "blur-nx (#32; A/B 239): ComputeViewport keeps its last eight answers by their inputs, not one")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_mesh_gen_whole_only, true, "Banjo/Enhancements",
                    "blur-nx (#36, found on the way; A/B 240): a cached copy's drop raises the mesh objects' generation "
                    "(voiding every one of them) only when the copy is a whole buffer's, which alone mesh objects, "
                    "geometry objects and the whole-repeat answer hold; range copies' drops left them all to miss")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_host_batch_verify, false, "Banjo/Debug",
                    "blur-nx (2026-11-18, the Renut host batch): #3's restored image checked to hold every inherited "
                    "run of the running device after each op, #13's runs made both ways and compared, #32's kept "
                    "viewports computed again and compared; differences counted and logged (the first 16)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_diag_batch_census, false, "Banjo/Diagnostics",
                    "blur-nx (2026-11-18, the Renut host batch's census): one 10 s line of counts for the batch's "
                    "questions (inherited bytes, repeated geometry and texture sets, resolves, uploads, fence waits, "
                    "conversions by kind, draws without field changes, op growth, stack headers, wakes, the record "
                    "lock, immediate draws, setter calls). A load and a store a count when on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu::host_batch {
Switches g_switches;
std::atomic<bool> g_ready{false};
std::atomic<uint64_t> g_count[kCountCount];
void LatchSwitches() {
  Switches s;
  s.on[kSkipUnused] = REXCVAR_GET(blur_renut_record_skip_unused);
  s.on[kInheritByChunk] = REXCVAR_GET(blur_renut_inherit_by_chunk);
  s.on[kWholeRepeat] = REXCVAR_GET(blur_renut_whole_repeat);
  s.on[kCleanerPriority] = REXCVAR_GET(blur_renut_ring_cleaner_priority) > 0;
  s.on[kInheritWords] = REXCVAR_GET(blur_renut_inherit_runs_words);
  s.on[kPipelineTwoWay] = REXCVAR_GET(blur_renut_fast_pipeline_two_way);
  s.on[kProvisionalKey] = REXCVAR_GET(blur_renut_provisional_key_kept);
  s.on[kWakeBatchAll] = REXCVAR_GET(blur_renut_wake_batch_all);
  s.on[kViewportMemo] = REXCVAR_GET(blur_renut_viewport_memo);
  s.on[kMeshGenWhole] = REXCVAR_GET(blur_renut_mesh_gen_whole_only);
  g_switches = s;
  g_ready.store(true, std::memory_order_release);
}
void SetThisThreadPriority(int priority) {
#if REX_PLATFORM_SWITCH
  (void)RexSwitchSetCurrentThreadPriorityOk(priority);
#else
  (void)priority;
#endif
}
int BackgroundPriority() { return banjo::renut_gpu::BackgroundThreadPriority(); }
int CleanerPriority() {
  static const int p = REXCVAR_GET(blur_renut_ring_cleaner_priority);
  return p;
}
#if !defined(RENUT_DIAG_CENSUS_OFF)
bool CensusOn() {
  static const bool on = REXCVAR_GET(blur_renut_diag_batch_census);
  return on;
}
#endif
bool VerifyOn() {
  static const bool on = REXCVAR_GET(blur_renut_host_batch_verify);
  return on;
}
void VerifyDiffer(const char *what) {
  static std::atomic<uint32_t> logged{0};
  static std::atomic<uint64_t> total{0};
  const uint64_t n = total.fetch_add(1, std::memory_order_relaxed) + 1;
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16 || (n & 1023) == 0)
    REXLOG_WARN("[renut] blur_renut_host_batch_verify: {} ({} differences so far)", what, n);
}
uint64_t Ticks() {
#if defined(__SWITCH__)
  return armGetSystemTick();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
namespace {
struct UploadNote {
  uint32_t va = 0, width = 0, height = 0, format = 0;
  uint64_t count = 0, bytes = 0;
};
std::mutex g_upload_mutex;
UploadNote g_uploads[32];
double TickMs() {
#if defined(__SWITCH__)
  return 1000.0 / double(armGetSystemTickFreq());
#else
  return 1000.0 * double(std::chrono::steady_clock::period::num) / double(std::chrono::steady_clock::period::den);
#endif
}
}  // namespace
void NoteUpload(uint32_t guest_va, uint32_t width, uint32_t height, uint32_t format, uint64_t bytes) {
  if (!CensusOn())
    return;
  std::lock_guard lock(g_upload_mutex);
  UploadNote *free_slot = nullptr, *least = &g_uploads[0];
  for (UploadNote &n : g_uploads) {
    if (n.count && n.va == guest_va && n.width == width && n.height == height && n.format == format) {
      ++n.count;
      n.bytes += bytes;
      return;
    }
    if (!n.count && !free_slot)
      free_slot = &n;
    if (n.count < least->count)
      least = &n;
  }
  UploadNote &n = free_slot ? *free_slot : *least;  // (full: the least uploaded one goes)
  n = {guest_va, width, height, format, 1, bytes};
}
std::string Report(double seconds) {
  if (!CensusOn() || seconds <= 0.0)
    return {};
  uint64_t c[kCountCount];
  for (uint32_t i = 0; i < kCountCount; ++i)
    c[i] = g_count[i].exchange(0, std::memory_order_relaxed);
  const double ms = TickMs();
  const auto per_s = [seconds](uint64_t v) { return double(v) / seconds; };
  const auto pct = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; };
  std::string line = fmt::format(
      "host batch census (blur_renut_diag_batch_census), a second: #3 inherited words {:.0f} ops, {:.1f} KB as "
      "before, {:.1f} KB by chunk; #4 whole geometry {:.0f} tries, {:.1f} % the draw before's; #5 texture fills "
      "{:.0f}, {:.1f} % as the call before, {:.1f} % as one of the 16 before; #6 resolves {:.0f}, {:.2f} ms finding "
      "the destination, {:.2f} ms their work; #7 texture uploads {:.0f} ({:.1f} MB); #8 fence waits {:.0f} ({:.2f} "
      "ms), {:.0f} of them kicked their own work; #12 conversions: whole {:.1f} KB, range {:.1f} KB, up {:.1f} KB; "
      "#19 fast draws {:.0f}, {:.1f} % without a field changed; #20 ops {:.0f}, {:.0f} grew their vector; #21 {:.0f} "
      "captured stack headers; #28 wakes {:.0f} by draws, {:.0f} by others ({:.2f} ms), record lock waits {:.0f} "
      "({:.2f} ms); #29 immediate draws {:.0f} ({:.2f} ms queueing); #35 setter calls {:.0f}",
      per_s(c[kRestoreOps]), per_s(c[kRestoreBytesAll]) / 1024.0, per_s(c[kRestoreBytesChunk]) / 1024.0,
      per_s(c[kWholeTries]), pct(c[kWholeRepeats], c[kWholeTries]), per_s(c[kTextureSetCalls]),
      pct(c[kTextureSetSame], c[kTextureSetCalls]), pct(c[kTextureSetSeen], c[kTextureSetCalls]),
      per_s(c[kResolveCalls]), double(c[kResolveTicksDest]) * ms / seconds, double(c[kResolveTicksWork]) * ms / seconds,
      per_s(c[kUploadCalls]), per_s(c[kUploadBytes]) / 1048576.0, per_s(c[kFenceWaits]),
      double(c[kFenceWaitTicks]) * ms / seconds, per_s(c[kFenceWaitsKicked]), per_s(c[kConvertWholeBytes]) / 1024.0,
      per_s(c[kConvertRangeBytes]) / 1024.0, per_s(c[kConvertUpBytes]) / 1024.0, per_s(c[kFastDraws]),
      pct(c[kFastDrawsNoFields], c[kFastDraws]), per_s(c[kOps]), per_s(c[kOpGrowths]), per_s(c[kOpsStackHeaders]),
      per_s(c[kWakesDraw]), per_s(c[kWakesOther]), double(c[kWakeTicks]) * ms / seconds, per_s(c[kRecordLockWaits]),
      double(c[kRecordLockTicks]) * ms / seconds, per_s(c[kImmediateDraws]), double(c[kImmediateTicks]) * ms / seconds,
      per_s(c[kSetterCalls]));
  {
    std::lock_guard lock(g_upload_mutex);
    UploadNote sorted[32];
    std::copy(std::begin(g_uploads), std::end(g_uploads), sorted);
    std::sort(std::begin(sorted), std::end(sorted),
              [](const UploadNote &a, const UploadNote &b) { return a.count > b.count; });
    std::string top;
    for (int i = 0; i < 8 && sorted[i].count; ++i)
      top += fmt::format(" {:08X} {}x{} format {} x{} ({:.1f} KB each);", sorted[i].va, sorted[i].width,
                         sorted[i].height, sorted[i].format, sorted[i].count,
                         double(sorted[i].bytes) / double(sorted[i].count) / 1024.0);
    if (!top.empty())
      line += "; #7 the most uploaded since the start:" + top;
  }
  return line;
}
}  // namespace renut::gpu::host_batch
