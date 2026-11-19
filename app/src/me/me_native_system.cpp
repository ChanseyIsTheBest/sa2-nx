// The native graphics system: an IGraphicsSystem that the app installs in config.graphics, so the Xenos emulation
// plugin is not loaded and no guest memory is watched. The game keeps its own Direct3D; a ring thread reads the PM4
// command list it writes, keeps a copy of the GPU registers, returns the read pointer, delivers interrupts, writes
// SCRATCH_REG/MEM_WRITE/EVENT_WRITE values the game waits for, and counts draws, copies and Swaps. Draws, resolves
// and Swaps are handed to the modules in src/native/masseffect (TargetsNative, DrawsVulkan, ShadersNative)
// with the native shader package (masseffect_shaders.mesp next to the executable).
//
// The app uses it when masseffect_renderer_native is true (see masseffect_app.h).

#include "me_native_system.h"
#include "me_verify.h"
#include "cpu/banjo_enhancements.h"
#include "me_shader_identity.h"
#include "me_shader_candidate_policy.h"
#include "me_native_draw_extent_estimator.h"
#include "me_pm4_runs.h"
#include "me_object_table.h"
#include "me_record_table.h"
#include "me_native_ps_no_kill.h"
#include "me_platform_compat.h"  // banjo-nx: banjo_ring_core (RexSwitchPinCurrentThreadToCore)
#include "banjo_ring_split.h"  // banjo-nx: banjo_ring_decoder (step 3 of docs/d3d-full-replacement.md)
#include "banjo_records.h"     // banjo-nx: the record format (banjo_direct_state, banjo_d3d_records)

#include <algorithm>
#include <array>
#include <bitset>
#include <atomic>
#include <thread>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/cvar.h>
#if defined(__ARM_NEON)
#include "me_platform_compat.h"  // banjo-nx: NEON on aarch64, SIMDe elsewhere
#endif
#include <rex/sys_counters.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>

#include "masseffect/masseffect_deferred_recording.h"
#include "masseffect/masseffect_native_targets.h"
#include "masseffect/me_edram_layout.h"  // banjo_gpu_dead_draws: tiles of a surface
#include "masseffect/masseffect_native_draws.h"
#include "masseffect/masseffect_native_shaders.h"
#include "masseffect/masseffect_shader_library.h"

namespace masseffect::native {
extern const ShadersNative* g_active_library;  // me_masseffect_glue.cpp
}
namespace banjo::renderdoc {
bool Start(const char* path_template);  // banjo_renderdoc.cpp
bool End();
}
extern "C" uint32_t g_banjo_last_reject_cause;  // masseffect_native_draws.cpp (C linkage)
extern "C" bool g_banjo_trace_edram;  // masseffect_native_targets.cpp: the traced frame also lists EDRAM events

// banjo-nx: defined here (masseffect-nx defined it in me_audio_hooks.cpp, which hooks Mass Effect's audio).
// sa2-nx's sa2_cpu_ring_estimator_reason: the draw-extent estimator keeps its reject reason as a pointer to the
// literal instead of a std::string (a heap allocation per call for reasons over 15 characters).
REXCVAR_DEFINE_BOOL(banjo_gpu_proof_tess_leftover, true, "Banjo",
                    "Native renderer (non-CPU enhancement): the overwrite proof of a Direct3D Clear treats "
                    "VGT_OUTPUT_PATH_CNTL as the reference renderer does, in force only in the explicit major mode. "
                    "The game leaves it on the tessellator after its water, and every clear after that was drawn as "
                    "an ordinary draw (EDRAM contents carried into it and out of it again)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_gpu_tiles_once, true, "Banjo",
                    "Native renderer (non-CPU enhancement): the scene's two predicated tile passes are drawn once. "
                    "The first pass draws every draw for both tiles (into the first tile's rows and the rows below "
                    "them in the same host images), copies both at each resolve, and the second pass's draws and "
                    "copies are dropped after being checked against what the first pass made for them. See "
                    "TilesOnce in me_native_system.cpp for the conditions and the one visible difference (effects "
                    "that sample the scene across the tile boundary)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_gpu_proof_triangle, true, "Banjo",
                    "Native renderer (non-CPU enhancement): one auto-indexed triangle that covers its whole viewport "
                    "or scissor (the game's post-processing and composite passes) is proven to overwrite what it "
                    "writes in full, like a rectangle: the tiles under it are not brought in from their previous "
                    "owner first")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_ring_estimator_reason, true, "Banjo",
                    "Ring thread (CPU enhancement, sa2-nx): the draw-extent estimator keeps its reject reason as a "
                    "pointer to the literal instead of a std::string")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_vblank_sleep_exact, true, "Mass Effect",
                    "Native renderer: the vblank thread sleeps until the exact next vblank instead of 1 ms steps")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Read by RenutApp::OnPreSetup (renut_app.h) on the Switch, before the Vulkan device is created.
REXCVAR_DEFINE_BOOL(banjo_mesa_disk_cache_off, true, "Banjo/Enhancements",
                    "Switch (non-CPU enhancement): turn off Mesa's own on-disk shader cache (sdmc:/.mesa). The "
                    "renderer's pipeline cache file (cache/masseffect_native_pipelines.bin) already keeps the compiled "
                    "pipelines, and Mesa's SD-card writes held a lock the ring thread waited on during compiles")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx: the Switch GPU's hierarchical depth culling (ZCULL) rejects whole blocks of pixels against a coarse copy
// of the depth buffer before they are shaded, in one direction per depth target. The driver sets "less" for every
// target, as the official driver does. Banjo draws its scene with reversed depth (depth test GREATER_EQUAL, cleared
// to 0: 1,227 of a traced frame's 1,653 depth-tested draws; only the shadow maps test LESS_EQUAL, and those targets
// have no ZCULL plane), so with "less" the scene gets no culling from it. The driver has the other direction as an
// option of its own (NVK_ZCULL=greater, nvk_cmd_draw.c): this key asks for it. Off by default until a console run
// shows the picture unchanged and the "scene" GPU time lower: it cannot be tried on the PC.
REXCVAR_DEFINE_BOOL(banjo_gpu_zcull_greater, false, "Banjo/Enhancements",
                    "Switch (non-CPU enhancement, console trial): ask the driver for ZCULL in the GREATER direction "
                    "(NVK_ZCULL=greater), the one Banjo's reversed-depth scene can use. Culling is conservative, so "
                    "the picture should be the same; false = the driver's default direction (less)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(banjo_shader_misses, "", "Nuts&Bolts/Graphics",
                      "Folder where the native renderer writes each shader it cannot identify (raw microcode, once), "
                      "to add them to banjo_shaders.mesp later; empty = off (MASSEFFECT_SHADER_DISCOVERY overrides)");
REXCVAR_DEFINE_INT32(banjo_renderdoc_capture_s, 0, "Nuts&Bolts/Graphics",
                     "Diagnostic: this many seconds after the renderer starts, ask RenderDoc (when its layer is loaded) "
                     "to capture one frame into capturas/rdc next to the exe (0 = off)");
// banjo-nx: Banjo ends the part of the frame before the scene with a few point-list draws (1 to 8 points each, depth
// tested and written, blended color) into a 1280x720 1x surface nothing else has drawn depth to. They are the
// particle simulation: their vertex shader writes memory (memexport), and that is all they are for. The EDRAM tiles
// of that surface belong to the shadow maps and the small targets by then, so the first of them imported all 720
// depth tiles and 112 color tiles into views of that surface, every frame (the most expensive pass of the frame on
// the PC's GPU: 957 ms of 10 s, half of what the whole scene category costs), to test a few points against what is
// shadow map data read as scene depth. The next thing that touches those tiles is the scene's clear, which
// overwrites every one of them whole.
//
// Such a draw now runs with its color mask at 0 and its depth test and write off: the same vertex shader on the same
// inputs at the same place in the stream, so the same memory written; nothing drawn, so no tile of the surface is
// needed. That is the same frame exactly when every tile it could have drawn to is overwritten in every bit before
// anything reads it, and that is checked on every use by the next draw that touches a render target (a proven
// overwrite: depth with stencil replaced, color with a full mask). Whether a draw gets this treatment is learned per
// site (shaders, surface, state): only after the site has been seen overwritten that way four times in a row with
// the draw made normally. A use that is not followed by such an overwrite is reported (DIFFERENCE: that frame lacks
// the points in the target) and turns the key off for the session; on the console those points would be drawn
// against garbage depth, so the game never lets them stay.
REXCVAR_DEFINE_BOOL(banjo_gpu_dead_raster, true, "Banjo",
                    "Native renderer (non-CPU enhancement): a small point-list draw outside the tile passes whose "
                    "site has always been overwritten whole by the next draw that touches a render target runs with "
                    "its target writes off (its vertex shader's memory writes stay): Banjo's particle simulation "
                    "draws before the scene imported 832 EDRAM tiles a frame for nothing. Checked on every use")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_gpu_proof_depth_only, true, "Banjo",
                    "Native renderer (non-CPU enhancement): in the depth-only EDRAM mode a draw's alpha test and "
                    "alpha-to-coverage bits do not stop the overwrite proof (no pixel shader runs there; the "
                    "renderer draws it without a fragment stage). Banjo's loading screen clears depth that way with "
                    "the alpha test left on: the whole depth buffer changed representation twice a frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx: D3D's Clear of an area that does not end on EDRAM tile boundaries clears the whole tiles with a resolve
// and draws what is left over as a rectangle list of two rectangles, an L along the right and bottom edges. The
// renderer gave that draw one area, the box around both rectangles, which is the whole cleared area: every tile of
// it was pulled into the drawn view first (42 depth to color conversions a frame for the 220x220 target in Banjo's
// gameplay), though the draw touches only the 16 edge tiles and the resolve clear overwrites the rest right after.
// A rectangle list draws its rectangles one after the other, each on its own, so the draw is made as one draw per
// rectangle (three vertices at that rectangle's index offset, the registers put back after): the same pixels in the
// same order, and each draw's area is its rectangle.
REXCVAR_DEFINE_BOOL(banjo_gpu_rect_split, true, "Banjo",
                    "Native renderer (non-CPU enhancement): an auto-indexed rectangle list of 2 to 8 rectangles (what "
                    "D3D's Clear draws for the part of its area outside whole EDRAM tiles) is drawn as one draw per "
                    "rectangle, so the tiles pulled into the drawn view are those under each rectangle and not all "
                    "of the box around them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_diag_ring_slow_us, 0, "Nuts&Bolts/Graphics",
                     "Diagnostic: the ring thread spins this many microseconds before every draw, so a PC run has the "
                     "console's balance (the ring behind, the game a frame ahead and waiting). 0 = off")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(banjo_trace_frame_all, false, "Nuts&Bolts/Graphics",
                    "banjo_trace_frame_s lists every draw of the traced frame (with its depth control, colour mask and "
                    "what the overwrite proof made of it), not only the ones into 1280-wide targets or sampling a "
                    "resolve destination.");
REXCVAR_DEFINE_BOOL(banjo_trace_frame_pm4, false, "Nuts&Bolts/Graphics",
                    "banjo_trace_frame_s also lists the structure of the traced frame's command stream: every indirect "
                    "buffer run (address, length, a hash of its words, what it holds) and the ring's own writes of "
                    "the registers a tile pass sets (window offset, scissors, targets, bin select).");
REXCVAR_DEFINE_INT32(banjo_trace_frame_s, 0, "Nuts&Bolts/Graphics",
                     "Diagnostic: this many seconds after the renderer starts, log one whole frame: every resolve, "
                     "every render-target change and every draw that samples a resolve destination (0 = off)");

// Read by MassEffectApp::PatchCoalescedHashForLocalTesting (masseffect_app.h). The Switch has no environment
// variables: debug runs set it in masseffect.toml.
REXCVAR_DEFINE_STRING(masseffect_coalesced_sha1, "", "Mass Effect",
                      "Local testing: SHA-1 (40 hex digits) of a modified Coalesced.ini (tools/coalesced.py, e.g. "
                      "no-startup-movies) accepted instead of the disc's; the same as MASSEFFECT_COALESCED_SHA1")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_shaders_index, true, "Mass Effect",
                    "If masseffect_shaders.mesp.idx matches the package, load only that index (entry table and "
                    "original containers) and read each shader's SPIR-V from the package on first use; otherwise, or "
                    "with false, load the whole package as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_repeated_identity, true, "Mass Effect",
                    "Native renderer: a shader IM_LOAD identical (same address and microcode) to the stage's current "
                    "program keeps its identification instead of hashing and matching again. false = always")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_several_rect, true, "Mass Effect",
                    "Native renderer: auto-index rect lists of 2..8 rectangles (D3D Clear of several regions) are "
                    "proven per rectangle, so mode 4 skips the transfers they overwrite. false = only single rects")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_renderer_native, false,  /* banjo-nx: off until it draws Banjo correctly */ "Mass Effect",
                    "Switch: use the native renderer (true) or ReXGlue's Xenos emulation (false, needs "
                    "gpu_plugin = \"xenos\")")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_stencil_inert, true, "Mass Effect",
                    "EDRAM mode 4: a draw whose stencil test is ALWAYS and writes nothing counts as stencil-off "
                    "(full depth overwrite proof; lazy stencil kept where it is)");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_center_ogl, true, "Mass Effect",
                    "EDRAM mode 4 proofs: honor OpenGL pixel centers (PA_SU_VTX_CNTL.pix_center) instead of always "
                    "assuming D3D centers (which lost the first column/row of full-screen passes)");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_ps_no_kill, true, "Mass Effect",
                    "EDRAM mode 4 proofs: a pixel shader with no KILL anywhere (fetches and branches allowed) cannot "
                    "discard, not only straight-line ALU shaders");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_vs_alu, true, "Mass Effect",
                    "EDRAM mode 4 proofs: the rectangle estimator admits vertex shaders that compute the position "
                    "with ALU math and (non-relative) constants, run on the live constant bank");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_quad_triangles, true, "Mass Effect",
                    "EDRAM mode 4 proofs: a full-screen quad drawn as two triangles (6-vertex list / 4-vertex strip) "
                    "that are exactly an axis-aligned rectangle split on a diagonal is proven like a rectangle");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_clip_inside, true, "Mass Effect",
                    "EDRAM mode 4 proofs: a rectangle drawn with clipping on (no user planes) and every vertex inside "
                    "the clip volume, with an XY viewport transform, is proven like an unclipped screen-space one");
REXCVAR_DEFINE_BOOL(masseffect_native_registers_table, true, "Mass Effect",
                    "Draw records (game thread -> ring) in a fixed bucket table instead of a std::map: no "
                    "allocation per draw under the shared lock")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_fast_registers, true, "Mass Effect",
                    "Ring: runs of register writes into the constant/fetch range skip the per-word special-register "
                    "checks and bump their generation once per run (exact)");
REXCVAR_DEFINE_BOOL(masseffect_native_identity_memo, true, "Mass Effect",
                    "Shader identification memo by microcode content (type, XXH3, size): a program loaded again "
                    "after others skips the library matching; the draw-time identity guard is unchanged");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_exact_edges, true, "Mass Effect",
                    "EDRAM mode 4: a proven rectangle whose edges lie exactly on pixel boundaries (D3D pixel "
                    "centers, no window offset, w = 1) bounds the draw by itself, without the 1-pixel pad; the pad "
                    "made syncs transfer whole tile rows/columns the draw never touches");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_overwrite, true, "Mass Effect",
                    "EDRAM mode 4: skip the ownership transfer of tiles that a proven full-screen rectangle "
                    "overwrites completely (depth ALWAYS without stencil, or opaque full-mask color)");
// Ring-thread CPU switches: every one keeps the old path and checks itself against it for the first
// masseffect_native_verify_n uses (a DIFFERENCE line in the log and the old path for the rest of the session).
REXCVAR_DEFINE_BOOL(masseffect_native_flat_identity, false, "Mass Effect",
                    "Ring: shader identity checks (VS fetch-patch match, PS microcode match) memoized in small "
                    "direct-mapped tables keyed by (library entry, load generation) instead of an unordered_map and a "
                    "word-by-word compare per use; exact (pure function of the loaded microcode and the entry)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_fast_pair, false, "Mass Effect",
                    "Ring: draw/record pairing without the cold cache lines and locks: record start addresses in a "
                    "compact array (the best record of a packet is found in two 32-byte rows), object -> library entry "
                    "through a lock-free table, no draw-queue mutex per unpaired draw, prefetch while draining")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_raw_microcode, false, "Mass Effect",
                    "Ring: IM_LOAD of the same address and size as the last load of that stage compares the guest "
                    "words against a raw copy of the last load (one memcmp) instead of byte-swapping them first; "
                    "exact (the swap is a bijection)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_pm4_fast, false, "Mass Effect",
                    "Ring: runs of constant/fetch register writes (type 0, SET_CONSTANT2/SET_SHADER_CONSTANTS, "
                    "LOAD_ALU_CONSTANT) are byte-swapped, compared and stored four words at a time with NEON "
                    "straight from the guest words; same register contents and generation bumps")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, banjo_cpu_verify);  // src/cpu/banjo_enhancements.cpp
// Port 172 (from fnxf_xiii): see VblankLoop (and the SDK's GraphicsSystem vsync worker). Off by default: every title's vblank timing changes.
REXCVAR_DEFINE_BOOL(banjo_vblank_from_display, false, "GPU",
                    "The guest's vblanks follow the display's refreshes (its vsync event), the 60 Hz timer only while "
                    "none comes for 1.5 periods: on the timer the guest's vblank drifted against the display, and 30 fps "
                    "frames came out uneven (fnxf_xiii). Needs a title that follows the display (banjo_display_sync). "
                    "false: the timer, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace rex::graphics {
// Port 172: the display's refresh count, waited for (set by app/src/renut_engine/display_sync.cpp; read here and by
// sdk/src/graphics/graphics_system.cpp). Defined here: this object is in every build.
uint64_t (*g_display_refresh_wait)(uint64_t seen, uint64_t timeout_ns) = nullptr;
}
REXCVAR_DECLARE(bool, masseffect_native_deferred_recording);  // masseffect_native_deferred.cpp
REXCVAR_DECLARE(bool, banjo_d3d_api_clear);  // src/me/banjo_d3d_clear.h
REXCVAR_DECLARE(bool, banjo_d3d_api_resolve);  // src/me/banjo_d3d_clear.h
REXCVAR_DEFINE_INT32(banjo_diag_ps_alpha, -1, "Banjo",
                     "Diagnostic (heavy scene shaders, 2026-10-15): for the draws of this pixel shader number, the alpha "
                     "test (RB_COLORCONTROL function and enable, RB_ALPHA_REF) and the pixel constants c12 and c50, "
                     "counted by kind every 10 s. -1 = off")
    .range(-1, 65535);
REXCVAR_DEFINE_INT32(banjo_diag_clear_padding, 0, "Banjo",
                     "Diagnostic for banjo_d3d_api_clear: 1 = the pitch padding and the rest of the last tile row that "
                     "a widened Clear takes are filled with magenta or a depth of 0.37 in the view's image right after "
                     "the clear: anything that read them would show (the picture has to be the same as with the key "
                     "off; compare the halves of banjo_ab_target = 20). 0 = off")
    .range(0, 1);
REXCVAR_DEFINE_BOOL(banjo_ring_registers_in_place, true, "Banjo",
                    "Native renderer (CPU enhancement): a proven rectangle list changes the two registers the proof "
                    "needs in the ring's register file and puts them back, instead of copying the whole register file "
                    "for every rectangle list. Same proof")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx: the direct renderer, step 1 (from sa2-nx's sa2_cpu_direct_state). See namespace direct below.
// Off by default: pixel-identical (verify: 3.3 M draws, 0 differences), but on PC the ring's non-draw work was 13 %
// higher with it (1.97 vs 1.74 us per draw, in-run A/B), and the game threads decode as well. configs/banjo_direct_ab.toml
// measures it on the console; it is turned on if it wins there.
REXCVAR_DEFINE_BOOL(banjo_direct_state, false, "Banjo",
                    "Native renderer (CPU enhancement): Direct3D's per-draw state packets become a host record the "
                    "ring applies with the same register functions at the same place in the stream (one NOP packet "
                    "instead of ~14 to parse; tile replays reuse the record; self-checked). Off until measured on the "
                    "console")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-16): step 4 of docs/d3d-full-replacement.md, the direct renderer for every Direct3D writer.
REXCVAR_DEFINE_BOOL(banjo_d3d_records, true, "Banjo",
                    "Native renderer (CPU enhancement): every Direct3D call that writes into a command buffer (the draw "
                    "functions, Swap, Resolve, Clear, the tile functions, fences: hooks of tools/d3d/d3d_record_hooks.py) "
                    "has all its packets made into host records the decoder applies; only indirect buffers and "
                    "WAIT_REG_MEM stay PM4 unless banjo_d3d_records_barriers carries them too. Self-checked at the call and by the "
                    "decoder. "
                    "Replaces banjo_direct_state's draw-state records when both are on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_records_zero_ahead, true, "Banjo",
                    "Native renderer (CPU enhancement, with banjo_d3d_records; from sa2-nx's sa2_api_ring_zero_ahead): "
                    "the ring of records' free space just past its head is made into cache lines of zeros (DC ZVA on "
                    "the Switch: no DRAM read before the records' stores, which the 8 MB ring, four times the L2, "
                    "otherwise costs on every line; a memset on the PC, so the PC tests cover which words are zeroed)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_records_barriers, true, "Banjo",
                    "Native renderer (CPU enhancement, with banjo_d3d_records; step 6b): the records carry the indirect "
                    "buffers and WAIT_REG_MEM too, and the decoder applies them from the records, so no PM4 is left in "
                    "Direct3D's segments")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_records_static, true, "Banjo",
                    "Native renderer (CPU enhancement, with banjo_d3d_records): a buffer without records (Direct3D's "
                    "fixed buffers: per-tile set-up and clears, run before every recorded buffer) that runs twice with "
                    "the same words becomes a record of its own: later runs compare its words with the kept copy and "
                    "apply the operations instead of parsing it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_records_native, true, "Banjo",
                    "Native renderer (CPU enhancement, with banjo_d3d_records): the native Direct3D draw path writes "
                    "its records' operations itself as it writes its packets (no decoding of them on the drawing "
                    "threads); what Direct3D's own helpers write in between is decoded. Self-checked against the packets")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-16): step 6 of docs/d3d-full-replacement.md, the kicks handed over through a host queue.
REXCVAR_DEFINE_BOOL(banjo_d3d_host_submit, true, "Banjo",
                    "Native renderer (CPU enhancement): Direct3D's kick hands the segments it submits to the decoder "
                    "through a host queue (address, length, ring position): the decoder no longer parses the primary "
                    "ring (only what the kick did not write, as before). Self-checked against the ring's words")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_diag_records_check_all, false, "Banjo",
                    "Diagnostic (PC tests of banjo_d3d_records): every record is checked against its packets at the "
                    "call and again by the decoder, without banjo_cpu_verify (which runs every native draw on a device "
                    "copy, so the native draw path's own records are not made there)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_direct_state_ab_s, 0, "Banjo",
                     "Measurement: the enhancement banjo_ab_target names turns itself off and on every N seconds (at a "
                     "Swap) and the ring timer reports the two halves separately ('ring thread A/B'), so one run gives "
                     "both under the same load and scenes. 0 = no A/B (play)")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_ab_target, 1, "Banjo",
                     "banjo_direct_state_ab_s: which enhancement the A/B switches (1 = banjo_direct_state, 2 = "
                     "banjo_ring_edram_memo, 3 = banjo_ring_constants_dedupe, 4 = banjo_gpu_proof_tess_leftover, 5 = "
                     "banjo_gpu_edram4_color_consumer, 6 = banjo_gpu_edram4_alias_by_shape, 7 = "
                     "banjo_gpu_proof_triangle, 8 = banjo_gpu_edram4_copy_batch, 9 = banjo_gpu_tiles_once, 10 = all of "
                     "4 to 9, 11 = banjo_ring_vertex_persistent, 12 = banjo_ring_shader_load_memo, 13 = "
                     "banjo_ring_vertex_epoch, 14 = banjo_gpu_dead_raster, 15 = banjo_gpu_rect_split, 16 = "
                     "masseffect_native_deferred_recording, which has to be on, 17 = banjo_gpu_proof_depth_only, 18 = "
                     "banjo_gpu_edram4_split_tiles, 19 = the native Direct3D draw path, banjo_d3d_native_draw_*, 20 = the "
                     "ring's use of the Clear markers of banjo_d3d_api_clear, 21 = the Resolve markers of banjo_d3d_api_resolve, "
                     "22 = banjo_gpu_skip_dead_draws, 23 = banjo_ring_vertex_lock_rule, 24 = banjo_ring_texture_invalidate_range, 25 = banjo_ring_index_persistent, "
                     "26 = banjo_gpu_early_alpha, 27 = banjo_gpu_texture_signs_per_slot, 28 = banjo_gpu_edram4_clear_merge, 29 = banjo_gpu_vfetch_block, 30 = banjo_gpu_branchless_select, 31 = banjo_gpu_bool_spec, "
                     "32 = banjo_gpu_stencil_fetch_3d, 33 = banjo_gpu_edram4_lean_barriers, 36 = banjo_gpu_resolve_clear_merge, "
                     "37 = banjo_gpu_pred_select, 38 = banjo_gpu_pred_select_ps, 39 = banjo_gpu_tess_grid, 40 = 29 and 37 "
                     "to 39 together, 41 = banjo_ring_core's pin, 42 = (removed: the ring prover), 43 = banjo_ring_decoder, 44 = "
                     "banjo_ring_vs_hash_given, 45 = banjo_d3d_records, 46 = banjo_d3d_records_native, 47 = "
                     "banjo_d3d_host_submit, 48 = (removed: banjo_ring_prover_fine), 49 = banjo_gpu_clamp_cbuf, 50 = "
                     "banjo_gpu_sign_mask_skip, 51 = banjo_gpu_vs_outputs_unwritten, 52 = banjo_gpu_bone_padding, 53 = "
                     "banjo_gpu_robustness_off, 54 = banjo_gpu_edram_checks_cpu, 55 = 49-54 together, "
                     "56 = banjo_gpu_literals_cbuf, "
                     "57 = banjo_gpu_stencil_clear_partial, "
                     "70 = package 26's round together (56-69))")
    .range(1, 70)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx: the ring's WAIT_REG_MEM loop slept 50 us between polls. Its one long wait (~13 ms a frame on PC: memory
// 1FB04006, the frame pacing word the game's vblank interrupt handler writes) took ~260 wake-ups a frame; on the Switch
// each is a kernel sleep and wake-up on a core the game threads share. After 16 polls the loop now sleeps until the
// next vblank interrupt or submission (both wake it) or 250 us, whichever comes first: ~5x fewer wake-ups. Timing
// only.
// banjo-nx (2026-10-16): the ring thread is the frame-rate limit on the console (diagnostics run of package 21b:
// 99.3 % of one thread, 69 ms of work per 72 ms frame), but its affinity was the process's whole mask (0x7, no
// preferred core): Horizon moved it between cores 0-2, its 99 % showed as 72-83 % on the busiest core of the user's
// overlay, and every move cost it its L1 contents. Pinned here to one core; the main (UI) thread is pinned to core 0,
// the background compile threads to core 2 (banjo_background_compile_core), so core 1 is the ring's. The game's guest
// threads (priority 0x3B, below the ring's 0x2C) keep every core and run elsewhere while the ring is busy.
// banjo-nx (2026-10-16, step 3 of docs/d3d-full-replacement.md): the ring split (banjo_ring_split.h).
REXCVAR_DEFINE_BOOL(banjo_ring_decoder, true, "Banjo",
                    "Native renderer (CPU enhancement): a decoder thread decodes the game's PM4 (packets, register writes, "
                    "shader identification, the tile passes' bookkeeping) and hands the ring thread, now the executor, "
                    "the draws in stream order with the register pages written since the item before; fences, memory "
                    "writes, interrupts and the read pointer stay the executor's, in stream order. Every 1024th draw "
                    "(every 16th with banjo_cpu_verify) the executor's register file is compared with the decoder's. "
                    "In-run A/B target 43 (the off half: the decoder waits for each item, as one thread did)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_ring_decoder_core, 2, "Banjo",
                     "banjo_ring_decoder: the core the decoder thread is pinned to (-1: the process's whole mask). The "
                     "executor stays on banjo_ring_core's")
    .range(-1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_ring_core, 1, "Banjo",
                     "Native renderer: the ring thread is pinned to this core (its affinity mask reduced to it). -1 = "
                     "the process's whole mask, as before. In-run A/B target 41 switches between the pin and the "
                     "whole mask")
    .range(-1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_ring_wait_vblank, true, "Banjo",
                    "Native renderer (CPU enhancement): a long WAIT_REG_MEM sleeps until the next vblank interrupt or "
                    "submission (or 250 us) instead of polling every 50 us. Same results, fewer wake-ups")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx: the renderer's same-frame reuse (vertex dedupe, its containment, the index cache) is exact only because the
// game cannot rewrite memory a draw already referenced without first learning that the GPU is done with it; the caches
// forget everything when g_synchronizations_ring moves. Nothing ever moved it (masseffect-nx's native frontend never
// published its wait generation), so only the frame boundary cleared them. The ring now moves it, in stream order, at
// every point where the CPU can learn of GPU progress: fence and query writes (EVENT_WRITE_SHD/ZPD), memory writes
// (MEM_WRITE, REG_TO_MEM, COND_WRITE when it writes), interrupts and swaps (sasrt-nx's set, less EVENT_WRITE_EXT:
// Direct3D's per-draw tiling extents, ~1 per tiled draw, not a fence). Draws before such a point cannot see a rewrite;
// draws after it get no reuse from before it.
REXCVAR_DEFINE_BOOL(banjo_ring_sync_fences, true, "Banjo",
                    "Native renderer (correctness of the same-frame reuse): fence writes, memory writes and interrupts in "
                    "the stream end the reuse of vertex and index copies made before them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx: Direct3D loads a shader program (IM_LOAD) before most draws, about a thousand times a frame with a program
// other than the stage's last one. Each such load copied the words out of guest memory in host order, hashed them
// (XXH3) to find the entry identified for that content before, copied them into the stage's microcode and copied the
// raw words for the "same as last time" test: three copies and a hash of 0.5 to 2 KB (console profile of 2026-10-13:
// XXH3 2.3 % of the ring thread, IdentifyShader 1.5 %, part of memmove's 7.3 % and of the identity checks' 2.3 %).
// The memo keeps, per load address and stage, the raw words, the host-order words, their hash, the entry and the
// identity generation the content memo's hit left behind. A load from that address with the same raw words (one
// memcmp) swaps the vectors into the stage and restores the rest; the identity generation is the one the content had
// before, so the per-candidate identity cells computed for it stay valid.
REXCVAR_DEFINE_BOOL(banjo_ring_shader_load_memo, true, "Banjo",
                    "Native renderer (CPU enhancement): a shader program loaded again from the same address with the "
                    "same words takes its identification from a memo by address (one compare) instead of being copied "
                    "three times and hashed. The first 4096 uses, all in verify mode, are checked against the content "
                    "memo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx: Draw() asked "does the draw's vertex shader entry match the loaded program?" four times per draw (the
// preference test, the acceptance, the final-selection counter and the overwrite proof's filter), each a lookup in
// the identity cells (MatchesVertexShaderIdentity: 2.3 % of the ring thread on the console, 2026-10-13). The answer
// cannot change inside one draw unless the entry is replaced: asked once, and again only after a replacement.
REXCVAR_DEFINE_BOOL(banjo_ring_identity_once, true, "Banjo",
                    "Native renderer (CPU enhancement): a draw asks once whether its vertex shader entry matches the "
                    "loaded program (four lookups before). Same answers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_note_shader_fast, true, "Banjo",
                    "Native renderer (CPU enhancement, sa2-nx): the draw threads' shader-object bookkeeping remembers "
                    "finished objects in one shared table (no thread-local lookup, 8x the slots, fewer mutex takes). "
                    "Same outcome")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_direct_state_keep_swaps, 3, "Banjo",
                     "banjo_direct_state: Swaps a record stays in the ring after the ring indexed it (then the side "
                     "store keeps its head while its marker is in the command buffer). 0 = test the side store")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx: the Draw* pairing inherited from masseffect-nx never pairs a draw in Banjo (the device address and shader
// offsets in NoteDrawCall are Mass Effect's: "paired 0, unpaired 10,176,920" over a PC run); the ring identifies every
// shader from the IM_LOADs by itself. With this key the game thread no longer queues a record per draw and the ring
// no longer searches for one: the same shaders (verify mode runs the search and compares).
REXCVAR_DEFINE_BOOL(banjo_no_draw_pairing, true, "Banjo",
                    "Native renderer (CPU enhancement): skip the Draw* record pairing, which never pairs a draw in "
                    "Banjo (the ring identifies the shaders by itself): no record per draw on the game thread, no "
                    "search per draw on the ring. Same shaders")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_verify_n, 2048, "Mass Effect",
                     "Self-check of the switches above: the old and the new path are both evaluated for the first N "
                     "uses and compared; any difference logs DIFFERENCE and turns that switch off. 0 = no check")
    .range(0, 2147483647)  // banjo-nx: banjo_cpu_verify checks every use
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(masseffect_native_vs_identity_tolerance, 0, "Mass Effect",
                     "Experimental: a vertex shader the ring loaded may differ from its library candidate (same size, "
                     "fetches still checked) in up to N ALU/CF words and still be drawn with that candidate. 0 = "
                     "exact identity (the draw is dropped when no candidate matches, as before). Try 4-16 when the "
                     "log shows 'unresolved draw shader pairs' (e.g. VS CD057930742AFE84, 120 words, one draw per "
                     "frame in the pause menu); see the '[native] VS word diff' lines")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;

// GPU MMIO window and register file.
constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegScratchUmsk = 0x01DC;
constexpr uint32_t kRegScratchAddr = 0x01DD;
constexpr uint32_t kRegScratch0 = 0x0578;
constexpr uint32_t kRegScratch7 = 0x057F;
constexpr uint32_t kRegCoherStatusHost = 0x0A31;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
// Display gamma ramp registers. Native presentation owns the Xenos output path, so it must mirror
// the table written by Direct3D just like the SDK command processor does.
constexpr uint32_t kRegGammaFirst = 0x1921;       // DC_LUT_RW_MODE
constexpr uint32_t kRegGammaIndex = 0x1922;       // DC_LUT_RW_INDEX
constexpr uint32_t kRegGammaSequential = 0x1923;  // DC_LUT_SEQ_COLOR
constexpr uint32_t kRegGamma30 = 0x1925;          // DC_LUT_30_COLOR
constexpr uint32_t kRegGammaMask = 0x1927;        // DC_LUT_WRITE_EN_MASK
constexpr uint32_t kRegGammaLast = 0x1927;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
constexpr uint32_t kRegVgtDmaBase = 0x21FA;
constexpr uint32_t kRegVgtDmaSize = 0x21FB;
constexpr uint32_t kRegVgtDrawInitiator = 0x21FC;
constexpr uint32_t kRegRbModeControl = 0x2208;
constexpr uint32_t kRegRbSampleCountAddr = 0x2325;
constexpr int kMaxIndirectDepth = 4;
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint16_t kExtentMax = 2048 >> 3;
// Registers whose change invalidates viewport/raster state.
constexpr uint32_t kRegViewportFirst = 0x2080;
constexpr uint32_t kRegViewportLast = 0x2302;
constexpr uint32_t kRegFetchFirst = 0x4800;
constexpr uint32_t kRegFetchLast = 0x48BF;
inline bool IsViewportRegister(uint32_t i) {
  if (i - kRegViewportFirst > kRegViewportLast - kRegViewportFirst) return false;
  return i <= 0x2082 || (i >= 0x210F && i <= 0x2114) || (i >= 0x2204 && i <= 0x2206) || i == 0x2302;
}

// banjo_ring_decoder: true on the decoder thread while the ring is split (its PM4 work hands the executor what the
// game can see; see banjo_ring_split.h).
thread_local bool t_decoder = false;

// Big-endian words of a command buffer; the ring wraps (power-of-two size), an indirect buffer
// does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t physical = 0;  // physical address of word 0 (to key packets by address)
  uint32_t mask = 0;
  uint32_t pos = 0;
  uint32_t end = 0;
  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t v = Peek();
    Advance(1);
    return v;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t ref) {
  switch (info & 0x7) {
    case 0x0: return false;
    case 0x1: return value < ref;
    case 0x2: return value <= ref;
    case 0x3: return value == ref;
    case 0x4: return value != ref;
    case 0x5: return value >= ref;
    case 0x6: return value > ref;
    default: return true;
  }
}

// Physical microcode address -> library entry, filled on the game thread by NoteShaderObject and read
// by the ring thread on IM_LOAD.
std::mutex g_by_address_mutex;
std::unordered_map<uint32_t, const masseffect::native::ShaderEntry*> g_by_address;
std::unordered_set<uint32_t> g_objects_seen;
// Immediate loads (IM_LOAD_IMMEDIATE) carry no address: they are found by the XXH3 of the microcode
// (host order) of the objects the game binds, the VS one taken at draw time (already patched). Hashes
// that two different entries share are counted and dropped.
std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> g_by_code;
std::unordered_set<uint32_t> g_code_mapped;  // objects whose (current) microcode is in g_by_code
std::unordered_map<uint32_t, const masseffect::native::ShaderEntry*> g_entry_of_object;
uint64_t g_code_conflicts = 0;

// masseffect_native_fast_pair: g_entry_of_object (written by the game thread under g_by_address_mutex, never erased)
// mirrored in a lock-free table the ring reads without the mutex (me_object_table.h).
ObjectEntryTable<const masseffect::native::ShaderEntry*> g_objects;
inline void PublishObject(uint32_t object, const masseffect::native::ShaderEntry* entry) { g_objects.Publish(object, entry); }
inline bool LookupObject(uint32_t object, const masseffect::native::ShaderEntry*& entry) { return g_objects.Lookup(object, entry); }

// Draw records from the game thread (NoteDrawCall).
struct DrawRecord {
  uint32_t type, r5, r6, r7, vs, ps;
  uint32_t start = 0;  // physical address of Direct3D's command buffer write pointer before the call
};
// The ring runs several frames behind the game thread (~800 records measured), so records are never aged
// out; the window search drops the ones skipped when a later one matches.

// Index/vertex count the ring carries for `prims` primitives of a Xenos primitive type
// (sub_821F7618 takes a primitive count and multiplies it, mullw at +175).
uint32_t CountForPrimitives(uint32_t type, uint32_t prims) {
  switch (type) {
    case 1: return prims;             // point list
    case 2: return prims * 2;         // line list
    case 3: return prims + 1;         // line strip
    case 4: return prims * 3;         // triangle list
    case 5: case 6: return prims + 2; // triangle fan, strip
    case 8: return prims * 3;         // rectangle list
    case 13: return prims * 4;        // quad list
    default: return prims;
  }
}
std::mutex g_draw_mutex;
std::deque<DrawRecord> g_draw_queue;
// The same records keyed by where Direct3D was about to write (device+48, the command buffer write
// pointer): the ring finds the record of a DRAW_INDX by its packet address, whatever the order.
std::map<uint32_t, DrawRecord> g_draw_by_address;
// The same index without allocations (masseffect_native_registers_table): 8 KB address buckets of 8 records;
// a lookup checks the packet's bucket and the previous one (records are at most 0x2000 below the packet).
// (me_record_table.h: the buckets, with a dense array of start addresses beside the slots for masseffect_native_fast_pair.)
DrawRecordTable<DrawRecord> g_records;
using DrawRecordSlot = DrawRecordTable<DrawRecord>::Slot;
bool RecordTable() {
  static const bool table = REXCVAR_GET(masseffect_native_registers_table);
  return table;
}
// Draw records from the game thread to the ring thread without the shared mutex (the record table is then
// owned by the ring thread alone): a bounded multi-producer queue (Vyukov) of (record, start). The ring
// drains it into g_record_table before every lookup. The ring runs a few frames behind (~800 records),
// so 16384 slots never fill in practice; a full queue makes the producer wait.
struct QueuedRecord {
  std::atomic<uint64_t> sequence{0};
  DrawRecord record;
  uint32_t start = 0;
};
constexpr uint64_t kRecordQueueSize = 16384;
std::vector<QueuedRecord> g_record_queue(kRecordQueueSize);
std::atomic<uint64_t> g_record_queue_tail{0};  // next slot to fill (producers)
uint64_t g_record_queue_head = 0;               // next slot to drain (ring thread only)
bool g_record_queue_ready = [] {
  for (uint64_t i = 0; i < kRecordQueueSize; ++i) g_record_queue[i].sequence.store(i, std::memory_order_relaxed);
  return true;
}();

bool g_record_fast = false;  // masseffect_native_fast_pair (set when the graphics system starts)

void InsertRecordInTable(DrawRecord&& record, uint32_t start) {
  g_records.Insert(std::move(record), start, g_record_fast);
}

void PushRecord(DrawRecord&& record, uint32_t start) {
  uint64_t pos = g_record_queue_tail.load(std::memory_order_relaxed);
  for (;;) {
    QueuedRecord& q = g_record_queue[pos & (kRecordQueueSize - 1)];
    const uint64_t seq = q.sequence.load(std::memory_order_acquire);
    if (seq == pos) {
      if (g_record_queue_tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        q.record = std::move(record);
        q.start = start;
        q.sequence.store(pos + 1, std::memory_order_release);
        return;
      }
    } else if (seq < pos) {
      std::this_thread::yield();  // full: the ring thread has not drained yet
      pos = g_record_queue_tail.load(std::memory_order_relaxed);
    } else {
      pos = g_record_queue_tail.load(std::memory_order_relaxed);
    }
  }
}

// Ring thread: moves every queued record into the table, in order.
void DrainRecords() {
  for (;;) {
    QueuedRecord& q = g_record_queue[g_record_queue_head & (kRecordQueueSize - 1)];
    if (q.sequence.load(std::memory_order_acquire) != g_record_queue_head + 1) return;
    if (g_record_fast) {  // the next records were written by the game thread's core: start pulling their lines
      __builtin_prefetch(&g_record_queue[(g_record_queue_head + 1) & (kRecordQueueSize - 1)]);
      __builtin_prefetch(&g_record_queue[(g_record_queue_head + 2) & (kRecordQueueSize - 1)]);
    }
    InsertRecordInTable(std::move(q.record), q.start);
    q.sequence.store(g_record_queue_head + kRecordQueueSize, std::memory_order_release);
    ++g_record_queue_head;
  }
}
std::atomic<bool> g_native_active{false};
std::atomic<uint64_t> g_draw_records{0};
bool g_no_pairing = false;        // banjo_no_draw_pairing active (set when the graphics system starts)
bool g_no_pairing_check = false;  // ... and verify mode: the old pairing still runs and is compared

// banjo_direct_state (the direct renderer, step 1: Direct3D's per-draw state without PM4 on the ring; sa2-nx's
// sa2_cpu_direct_state with Banjo's predicated tiling handled).
//
// Direct3D's draw functions flush the dirty state as PM4 packets (register runs, constants, fetch constants, the
// occasional shader load, alignment fillers) and then write the draw packet. Right after the original draw function
// returns, the hook (EndDrawDirect) decodes the packets it wrote before the draw packet into a host record: register
// runs already in host byte order, and the few other packets (shader loads, waits) as they were. The packets are then
// covered by one NOP packet whose payload names the record; the draw packet stays where it was, and so does every
// address Direct3D keeps. The ring applies the record when it reaches the NOP, with the same register functions the
// packets would have gone through, at the same place in the stream: the same register file at every draw.
//
// Banjo differs from SA2 in two ways. Its engine records most draws into command buffers of its own, on several
// threads (each its own Direct3D "device" object), which the device's command buffer then runs as indirect buffers:
// markers are reached out of record order. And parts of the stream run more than once a frame (predicated tiling: one
// pass per EDRAM tile; the recorded command buffers' draws are predicated). So the ring indexes every published record
// by number (IndexDirectRecords) and finds a marker's record there, as often as it is reached. Records are released
// banjo_direct_state_keep_swaps Swaps after the ring indexed them, unless their marker is still in the command buffer (that command buffer
// may run again): those move to a side store (kept_) until the game writes over their marker. A record carries the
// three words its marker replaced, so the region's original packets can always be rebuilt (the ring's check, below).
//
// Left as it was (and processed as PM4, as before): a draw during which anything was submitted or the command buffer
// changed segment (the ring may already be reading those words), packets the converter does not know, predicated
// packets, a NOP with payload in the first three words (inline data another packet may point at).
//
// Checked: the first records (all of them in verify mode) are re-encoded and compared with the packets Direct3D wrote,
// word for word, at the draw (SameAsWritten), and again by the ring against the packets the command buffer holds when
// the ring reaches them, replays included (a DIFFERENCE stops the conversion; the ring then uses the packets).
namespace direct {
using namespace banjo::records;  // kMagic, kRecordHeader, the operations, Classify / Emit / DecodePiece / SamePiece
uint64_t g_keep_swaps = 3;                 // a record stays in the ring this many Swaps after the ring indexed it
constexpr uint32_t kRingWords = 1u << 21;  // 8 MB of records (the ring runs a few frames behind)
// A record: [number, words, the marker's physical address, the three words the marker replaced (guest order), ops...]
// (banjo_records.h)
constexpr size_t kKeptMax = size_t(1) << 20;  // side store limit (records): above it no new records are made
std::atomic<bool> g_on{false};             // banjo_direct_state active (set when the graphics system starts)
bool g_verify_all = false;                 // banjo_cpu_verify: every record checked at the draw and at the ring
std::atomic<uint64_t> g_wptr_writes{0};    // guest writes of CP_RB_WPTR (WriteMmio)
// Set by the draw thread while it checks g_wptr_writes and writes a marker; a submission (WriteMmio) waits for it to
// clear before it publishes the write pointer. Either the draw thread sees the submission (and leaves its packets) or
// the submission sees the marker being written (and publishes it whole): seq_cst on both sides.
std::atomic<bool> g_marking{false};
// Each on its own cache line: the recording threads write the lock and g_head per draw, the ring reads g_head and
// writes g_tail; sharing a line made every ring read a miss.
alignas(64) std::atomic_flag g_producing = ATOMIC_FLAG_INIT;  // one converting draw at a time (threads take turns)
alignas(64) std::atomic<uint64_t> g_head{0};  // producer: next free word (monotonic)
alignas(64) std::atomic<uint64_t> g_tail{0};  // consumer: first word not yet released (at a Swap)
alignas(64) uint32_t g_seq = 0;               // producer: the last record number (never 0 or kPad)
// The records, word-indexed modulo kRingWords. 4 KB-aligned: banjo_records_zero_ahead's DC ZVA zeroes the whole
// aligned 64-byte block an address is in, so a block position must be a block address (package 24's first console
// runs: a ring from make_unique, 16-byte aligned, had each zeroing wipe up to the last 12 words of the record just
// published; the decoder's check caught it at the first records: "first difference at operation word 2472 of 2480").
struct AlignedFree {
  void operator()(uint32_t* p) const { ::operator delete[](p, std::align_val_t(4096)); }
};
std::unique_ptr<uint32_t[], AlignedFree> g_ring;
std::atomic<bool> g_kept_full{false};      // the ring's side store is at its limit: no new records
std::atomic<int32_t> g_checks_left{4096};  // records still checked at the draw (all of them in verify mode)
std::atomic<bool> g_failed{false};         // a check found a difference: no more records
// Statistics (report): converted draws and why the others were left.
std::atomic<uint64_t> g_converted{0}, g_words_covered{0}, g_skip_submitted{0}, g_skip_unknown{0}, g_skip_small{0},
    g_skip_nop_head{0}, g_skip_no_draw{0}, g_skip_segment{0}, g_skip_full{0}, g_skip_busy{0}, g_checked{0},
    g_kick_waits{0};
std::atomic<uint32_t> g_unknown_opcode{0};  // the last type-3 opcode that stopped a conversion (report)
// Address ranges (report): converted regions and the indirect buffers the ring ran at depth 0, 1 and 2+ (min, max).
struct Span {
  std::atomic<uint32_t> lo{0xFFFFFFFFu}, hi{0};
  void Note(uint32_t a) {
    if (a < lo.load(std::memory_order_relaxed)) lo.store(a, std::memory_order_relaxed);
    if (a > hi.load(std::memory_order_relaxed)) hi.store(a, std::memory_order_relaxed);
  }
};
Span g_span_converted, g_span_ib[3];
std::atomic<uint32_t> g_devices[4] = {};
std::atomic<uint64_t> g_device_draws[4] = {};
// The draw thread's counters: one writer at a time (g_producing), a plain load and store; only a report.
inline void Bump(std::atomic<uint64_t>& c, uint64_t n) { c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }

inline uint8_t* Guest(uint8_t* base, uint32_t address) {
#if defined(_WIN32)
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);  // the SDK's 0xE0000000 view offset on Windows
#else
  return base + address;
#endif
}

// The GPU's (physical) address of a guest virtual address in a physical heap, as MmGetPhysicalAddress gives it (the
// SDK's PhysicalHeap::GetPhysicalAddress: the 0xE0000000 heap is 4 KB up), so records name packets as the ring does.
inline uint32_t Physical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

// The type-3 packets a record may carry before the draw packet: no effect but their own on the register file and the
// shader state, in stream order (the ring runs them through Type3 with their original words).
inline bool Carried(uint32_t opcode) {
  switch (opcode) {
    case xenos::PM4_NOP:
    case xenos::PM4_IM_LOAD:
    case xenos::PM4_IM_LOAD_IMMEDIATE:
    case xenos::PM4_SET_CONSTANT:
    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS:
    case xenos::PM4_LOAD_ALU_CONSTANT:
    case xenos::PM4_INVALIDATE_STATE:
    case xenos::PM4_WAIT_REG_MEM:
    case xenos::PM4_REG_RMW:
    case xenos::PM4_EVENT_WRITE:  // a register write (VGT_EVENT_INITIATOR)
      return true;
    default:
      return ByReference(opcode);
  }
}
inline bool IsDraw(uint32_t opcode) {
  return opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2 || opcode == xenos::PM4_DRAW_INDX_BIN ||
         opcode == xenos::PM4_DRAW_INDX_2_BIN;
}

// The decoder, shared by the draw hook and the ring's check: the packets in `words` guest words at `src` (the first
// at physical address `physical`) as record operations at `out` (room for 2 * words), up to the first draw packet or
// the end. Returns false for a region that must stay PM4 (`why` says which counter), else the number of words before
// the draw packet (`region`; all of them when there is none) and the operations' size.
enum class Decode { kOk, kUnknown, kNopHead, kPartial };
inline Decode DecodeRegion(const uint8_t* src, uint32_t physical, uint32_t words, uint32_t* out, uint32_t& ops,
                           uint32_t& region, bool& draw) {
  uint32_t* const out_start = out;
  draw = false;
  uint32_t i = 0;
  while (i < words) {
    const uint8_t* const hp = src + size_t(i) * 4;
    const uint32_t h = rex::memory::load_and_swap<uint32_t>(hp);
    const uint32_t type = h >> 30;
    if (type == 0) {
      if (!h) { ++i; continue; }  // Packet() skips a zero word
      const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
      if (i + 1 + count > words) return Decode::kPartial;
      *out++ = ((h >> 15) & 1 ? kRunSingle : kRun) | (count << 8);
      *out++ = h & 0x7FFF;
      SwapWords(out, hp + 4, count);
      out += count;
      i += 1 + count;
    } else if (type == 1) {
      if (i + 3 > words) return Decode::kPartial;
      *out++ = kType1;
      *out++ = h;
      SwapWords(out, hp + 4, 2);
      out += 2;
      i += 3;
    } else if (type == 2) {
      ++i;  // filler
    } else {
      const uint32_t opcode = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
      if (IsDraw(opcode)) {
        draw = true;
        break;
      }
      if ((h & 1) || !Carried(opcode)) {
        g_unknown_opcode.store(opcode, std::memory_order_relaxed);
        return Decode::kUnknown;
      }
      if (i + 1 + count > words) return Decode::kPartial;
      if (opcode == xenos::PM4_NOP) {
        // The marker covers the region's first three words: a NOP there may hold data another packet points at.
        if (i < 3) return Decode::kNopHead;
        i += 1 + count;
        continue;
      }
      if (ByReference(opcode)) {
        if (i < 3) return Decode::kNopHead;  // its words would be the marker's: not readable later
        *out++ = kType3Ref | (count << 8);
        *out++ = h;
        *out++ = physical + i * 4;
        i += 1 + count;
        continue;
      }
      *out++ = kType3 | (count << 8);
      *out++ = h;
      *out++ = (physical + i * 4) & 0x1FFFFFFFu;  // the packet's physical address (Type3 reads packet_address_)
      std::memcpy(out, hp + 4, size_t(count) * 4);  // guest byte order: Type3 reads them
      out += count;
      i += 1 + count;
    }
  }
  ops = uint32_t(out - out_start);
  region = i;
  return Decode::kOk;
}

// --- banjo_d3d_records (step 4 of docs/d3d-full-replacement.md): records of whole calls ------------------------------
//
// banjo_direct_state makes a record of the state packets a draw function writes before its draw packet. With
// banjo_d3d_records every Direct3D function that writes into a device's command buffer has all its packets made into
// records: the draw packets and the tile wrapper around them, events, fences, interrupts, the Swap, shader loads, the
// clears' and resolves' packets. Each outermost call (a counter in the thread's guest context: a call inside another
// hooked call is part of the outer one's region) is cut into "pieces", runs of packets between barriers, one record and
// one marker NOP each. The decoder then parses no PM4 for them, only the marker in front of each piece.
//
// What stays PM4 in the command buffer, where it is:
//   - barriers: indirect buffers and WAIT_REG_MEM, unless banjo_d3d_records_barriers (step 6b) carries them too;
//   - a packet whose words must stay in place at the start of a piece, where the marker would overwrite them: a plain
//     NOP (its payload may be inline data that a later packet points at: vertices, constants), a tile mask Direct3D
//     patches after the call (read by reference everywhere else), another record's marker. The piece begins after it.
// Everything else of a piece is carried: register runs in host order, type-3 packets with their words and address
// (the decoder runs them through Type3 with their own header, predicate bit included), the renderer's own marker NOPs
// (Clear and Resolve) as packets, plain NOPs dropped (their words stay in the command buffer, inside the marker's
// region).
constexpr uint32_t kMaxPieces = 64;  // per call (more: the rest stays PM4)
std::atomic<bool> g_whole{false};  // banjo_d3d_records active: records hold whole calls
std::atomic<uint32_t> g_main_device{0};  // the device Swap is called with (the writers' device when r3 is not one)
// Statistics (report): calls made into records, pieces, and the words left as PM4 by reason.
std::atomic<uint64_t> g_calls_converted{0}, g_pieces{0}, g_words_barrier{0}, g_words_head{0}, g_words_small{0},
    g_words_partial{0}, g_calls_nested{0}, g_calls_empty{0}, g_words_capped{0}, g_calls_switched{0},
    g_calls_overlapped{0}, g_parts_converted{0};
// banjo_d3d_records_native: the native draw path's operations go to its device's staging area (one per device slot of
// the hooks' table; a device is drawn on by one thread at a time) and are copied into the ring when published.
// Verify and banjo_diag_records_check_all: each record's writer and its region's words when it was made, by record
// number (a difference at the decoder reports them).
constexpr uint32_t kDiagSlots = 1u << 16;
std::vector<uint32_t> g_diag_writer;                 // writer function address
std::vector<std::array<uint32_t, 48>> g_diag_words;  // the region's first 47 words at the call (guest order), [47]: count
uint32_t g_big_logged = 0;  // (g_producing held)
std::atomic<bool> g_native_ops{false};
// banjo_records_zero_ahead: the free space past the head readied as zeroed cache lines (producer only, g_producing).
bool g_zero_ahead = false;
uint64_t g_ready = 0;  // the ring's first word not readied yet
constexpr uint32_t kReadyAheadWords = 2048;  // about one call's records
inline uint32_t ZeroBlockWords() {
#if defined(__aarch64__)
  static const uint32_t words = [] {
    uint64_t id;
    asm volatile("mrs %0, dczid_el0" : "=r"(id));
    return (id & 16) ? 0u : (1u << (id & 15));  // DZP set: not allowed; else BS = log2 of the block in words
  }();
  return words;
#else
  return 16;
#endif
}
inline void ZeroBlock(uint32_t* p, uint32_t words) {
#if defined(__aarch64__)
  (void)words;
  asm volatile("dc zva, %0" : : "r"(p) : "memory");
#else
  // As DC ZVA does: the whole aligned block the address is in (so the PC's tests see what the console's zeroing does).
  const uintptr_t block = uintptr_t(words) * 4;
  std::memset(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(p) & ~(block - 1)), 0, block);
#endif
}
// After a publication at `head`: whole blocks from the first boundary at or past it, each once, and only free space
// (below the consumer's tail plus the ring's size).
inline void ReadyAhead(uint64_t head) {
  const uint32_t block = ZeroBlockWords();
  if (!block) return;
  // (A block position must be a block address: DC ZVA zeroes the aligned block the address is in.)
  if (reinterpret_cast<uintptr_t>(g_ring.get()) & (uintptr_t(block) * 4 - 1)) [[unlikely]] {
    g_zero_ahead = false;
    REXLOG_ERROR("[native] banjo_records_zero_ahead: the ring of records is not {}-byte aligned; off", block * 4);
    return;
  }
  constexpr uint64_t mask = kRingWords - 1;
  const uint64_t limit = std::min(head + kReadyAheadWords, g_tail.load(std::memory_order_acquire) + kRingWords);
  uint64_t from = std::max(g_ready, (head + block - 1) & ~uint64_t(block - 1));
  if (g_verify_all && from < head) [[unlikely]] {
    ::me::native::VerifyDifference();
    REXLOG_ERROR("[native] DIFFERENCE: banjo_records_zero_ahead would zero word {} below the head {}", from, head);
    g_zero_ahead = false;
    return;
  }
  for (; from + block <= limit; from += block) ZeroBlock(&g_ring[from & mask], block);
  g_ready = std::max(g_ready, from);
}
std::atomic<bool> g_native_failed{false};  // a native record differed from its packets: decoding from then on
std::unique_ptr<uint32_t[]> g_staging[8];
std::atomic<uint64_t> g_native_records{0}, g_native_fallbacks{0}, g_native_checked{0};

}  // namespace direct

// banjo_d3d_host_submit (step 6 of docs/d3d-full-replacement.md): Direct3D's kick (sub_821DF948) writes one indirect
// buffer packet per segment into the primary ring and then the ring's write pointer. Its hook (KickBegin) works out
// those packets from the kick's arguments and the device's ring fields (+10952 the write position, +14900 the mask,
// in words) before the original runs; WriteMmio, when the write pointer the kick stores is the one predicted, puts them
// in this queue before it publishes the pointer. The decoder takes a queued segment where its packet is in the ring
// (by position) and runs it as that packet, without reading the ring; anything else in the ring (what no kick wrote)
// is parsed as before. Single producer at a time (a kick: Direct3D's device is used by one thread at a time), single
// consumer (the decoder, or the ring thread).
//
// Direct3D also copies packets of its own into the primary ring between kicks (sub_8263FD60: the tile select of the
// next segment, 2-3 a frame), which the next kick's write pointer submits. Their hook queues them as record operations
// (decoded there, by value: the ring's words are final once written), applied where they are in the ring.
namespace submit {
struct Entry {
  uint32_t at, after;        // the packet's ring position and the one after it (words)
  uint32_t address, length;  // an indirect buffer's words as the kick writes them; for ring words: their operations'
                             // place in g_ops and how many
  uint32_t kind;             // 0 an indirect buffer, 1 ring words (operations)
};
constexpr uint32_t kOps = 1u << 16;  // the ring words' operations (power of two)
uint32_t g_ops[kOps];
uint64_t g_ops_head = 0;                          // producer
alignas(64) std::atomic<uint64_t> g_ops_tail{0};  // consumer: the first word still needed
constexpr uint32_t kQueue = 4096;  // entries (power of two)
constexpr uint32_t kPending = 256;
std::atomic<bool> g_on{false}, g_failed{false};
Entry g_queue[kQueue];
alignas(64) std::atomic<uint64_t> g_head{0};  // WriteMmio
alignas(64) std::atomic<uint64_t> g_tail{0};  // the decoder
// The kick being made, per thread (two threads can be in Direct3D on the real device: see DeviceCalls): KickBegin,
// read by WriteMmio on the same thread when it stores the predicted write pointer.
struct Pending {
  bool on = false;
  uint32_t wptr = 0, count = 0;
  Entry entries[kPending];
};
thread_local Pending* t_pending = nullptr;  // (allocated on a thread's first kick)
// The queue's producers (WriteMmio of a kick, RingWords) take turns.
alignas(64) std::atomic_flag g_producer = ATOMIC_FLAG_INIT;
struct ProducerLock {
  ProducerLock() {
    while (g_producer.test_and_set(std::memory_order_acquire)) rex::thread::MaybeYield();
  }
  ~ProducerLock() { g_producer.clear(std::memory_order_release); }
};
std::atomic<uint64_t> g_kicks{0}, g_queued{0}, g_unpredicted{0}, g_full{0}, g_other_path{0}, g_words_queued{0},
    g_words_left{0};
}  // namespace submit

uint64_t CodeHash(const uint8_t* big_endian, uint32_t words) {
  std::vector<uint32_t> host(words);
  for (uint32_t i = 0; i < words; ++i) host[i] = rex::memory::load_and_swap<uint32_t>(big_endian + i * 4);
  return XXH3_64bits(host.data(), host.size() * sizeof(uint32_t));
}

// Whole-library index by unmasked microcode (host order), unique hashes only: for immediate loads of
// shaders the game never binds through the device (Direct3D's own clear/copy shaders).
std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> g_library_by_code;

bool OriginalMicrocode(const masseffect::native::ShaderEntry& entry, uint32_t& virtual_size,
                       uint32_t& offset, uint32_t& bytes, std::vector<uint32_t>* words = nullptr) {
  if (!entry.shader || entry.shader->original.size() < 28) return false;
  const auto& o = entry.shader->original;
  virtual_size = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
  const uint32_t shader_header = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
  if (virtual_size > o.size() || shader_header + 8 > virtual_size) return false;
  offset = rex::memory::load_and_swap<uint32_t>(o.data() + shader_header);
  bytes = rex::memory::load_and_swap<uint32_t>(o.data() + shader_header + 4);
  if (!bytes || (bytes & 3) || size_t(virtual_size) + offset + bytes > o.size()) return false;
  if (words) {
    words->resize(bytes / 4);
    for (size_t i = 0; i < words->size(); ++i) {
      (*words)[i] = rex::memory::load_and_swap<uint32_t>(o.data() + virtual_size + offset + i * 4);
    }
  }
  return true;
}

// Direct3D's clear/copy/video shaders are loaded with IM_LOAD_IMMEDIATE after link-time rewriting.
// Their original containers are in the library, but the exact ring microcode is not. Materialize a
// stable container from the closest register-linked template so the normal offline translator can add
// the variant on the next run. Package shaders (with a constant table) are deliberately not candidates.
void DumpMissingImmediate(const masseffect::native::ShadersNative& library, bool vertex,
                          const std::vector<uint32_t>& microcode) {
  const char* folder = std::getenv("MASSEFFECT_SHADER_MISSING");
  if (!folder || !*folder || microcode.empty()) return;
  const uint64_t code_hash = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t)) ^
                             (vertex ? 1ull : 0ull);
  static std::mutex mutex;
  static std::unordered_set<uint64_t> dumped;
  std::lock_guard<std::mutex> lock(mutex);
  if (!dumped.insert(code_hash).second) return;

  const masseffect::native::ShaderEntry* best = nullptr;
  uint64_t best_score = UINT64_MAX;
  uint32_t best_vsize = 0, best_offset = 0, best_bytes = 0, best_different = 0;
  for (uint32_t n = 0;; ++n) {
    const auto* candidate = library.PerNumber(n);
    if (!candidate) break;
    if (candidate->vertices != vertex || !candidate->binding_per_register) continue;
    uint32_t vsize = 0, offset = 0, bytes = 0;
    std::vector<uint32_t> original;
    if (!OriginalMicrocode(*candidate, vsize, offset, bytes, &original) ||
        original.size() != microcode.size()) {
      continue;
    }
    uint32_t different = 0;
    uint64_t bits = 0;
    for (size_t i = 0; i < original.size(); ++i) {
      const uint32_t delta = original[i] ^ microcode[i];
      different += delta != 0;
      bits += std::popcount(delta);
    }
    const uint64_t score = uint64_t(different) * 1000 + bits;
    if (score < best_score) {
      best = candidate;
      best_score = score;
      best_vsize = vsize;
      best_offset = offset;
      best_bytes = bytes;
      best_different = different;
    }
  }
  if (!best || !best->shader) {
    REXLOG_WARN("[native] no internal {} template for an immediate shader of {} words",
                vertex ? "VS" : "PS", microcode.size());
    return;
  }
  std::vector<uint8_t> container = best->shader->original;
  for (size_t i = 0; i < microcode.size(); ++i) {
    rex::memory::store_and_swap<uint32_t>(container.data() + best_vsize + best_offset + i * 4,
                                          microcode[i]);
  }
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const uint64_t h = XXH3_64bits(container.data(), container.size());
  const auto path = std::filesystem::path(folder) /
                    fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", h);
  if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(container.data(), 1, container.size(), f);
    std::fclose(f);
    REXLOG_INFO("[native] immediate {} variant kept: {} (template n{}, {} of {} words changed)",
                vertex ? "VS" : "PS", path.string(), best->number, best_different,
                best_bytes / 4);
  }
}

// Lossless discovery artifact for every shader which the current library cannot identify. Unlike
// DumpMissingImmediate this deliberately does not borrow a container or guess an identity: the file
// is the exact big-endian microcode from the ring plus a small human-readable sidecar. It is input
// for instruction-aware classification of Direct3D's internal shaders.
void DumpRawMicrocode(bool vertex, uint32_t address, const std::vector<uint32_t>& microcode) {
  // banjo-nx: the folder comes from MASSEFFECT_SHADER_DISCOVERY or, where there is no environment (Switch), from the
  // banjo_shader_misses cvar; each unidentified shader is written once, to be wrapped into the library later
  // (tools/wrap_discovered.py) without rebuilding the NRO.
  static const std::string folder_value = [] {
    const char* env = std::getenv("MASSEFFECT_SHADER_DISCOVERY");
    return std::string(env && *env ? env : REXCVAR_GET(banjo_shader_misses).c_str());
  }();
  const char* const folder = folder_value.c_str();
  if (!*folder || microcode.empty()) return;
  const uint64_t hash = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t));
  const uint64_t key = hash ^ (vertex ? 1ull : 0ull);
  static std::mutex mutex;
  static std::unordered_set<uint64_t> dumped;
  std::lock_guard<std::mutex> lock(mutex);
  if (!dumped.insert(key).second) return;

  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::string stem = fmt::format("{}_{:016x}", vertex ? "vs" : "ps", hash);
  const auto raw_path = std::filesystem::path(folder) / (stem + ".ucode");
  if (std::FILE* f = std::fopen(raw_path.string().c_str(), "wb")) {
    for (uint32_t word : microcode) {
      uint8_t bytes[4] = {uint8_t(word >> 24), uint8_t(word >> 16), uint8_t(word >> 8),
                          uint8_t(word)};
      std::fwrite(bytes, 1, sizeof(bytes), f);
    }
    std::fclose(f);
  }
  const auto text_path = std::filesystem::path(folder) / (stem + ".txt");
  if (std::FILE* f = std::fopen(text_path.string().c_str(), "w")) {
    std::fprintf(f, "stage=%s\naddress=%08X\nwords=%zu\nhash=%016llX\n", vertex ? "vs" : "ps",
                 address, microcode.size(), static_cast<unsigned long long>(hash));
    for (size_t i = 0; i < microcode.size(); ++i) std::fprintf(f, "%04zu %08X\n", i, microcode[i]);
    std::fclose(f);
  }
  REXLOG_INFO("[native] raw unidentified {} kept: {} ({} words)", vertex ? "VS" : "PS",
              raw_path.string(), microcode.size());
}

void BuildLibraryCodeIndex(const masseffect::native::ShadersNative& library) {
  std::unordered_set<uint64_t> shared;
  auto register_link_score = [](const masseffect::native::ShaderEntry* e) {
    if (!e || !e->binding_per_register || !e->shader) return uint32_t(0);
    // Current raw-PS wrappers carry all 16 register-linked interpolator mappings after the
    // 32-byte shader header. Older discovery wrappers stopped at the header, so XenosRecomp
    // initialized the missing inputs to zero (in particular UI alpha). Prefer the complete
    // wrapper when several containers contain byte-for-byte identical microcode.
    if (!e->vertices && e->shader->original.size() >= 28) {
      const auto& o = e->shader->original;
      const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
      const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
      if (sh <= vsize && vsize - sh >= 32 + 16 * 4) return uint32_t(3);
    }
    return uint32_t(2);
  };
  for (uint32_t n = 0;; ++n) {
    const masseffect::native::ShaderEntry* e = library.PerNumber(n);
    if (!e) break;
    if (!e->shader) continue;
    const auto& o = e->shader->original;
    if (o.size() < 28) continue;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
    if (sh + 8 > vsize || vsize > o.size()) continue;
    const uint32_t off = rex::memory::load_and_swap<uint32_t>(o.data() + sh);
    const uint32_t bytes = rex::memory::load_and_swap<uint32_t>(o.data() + sh + 4);
    if (size_t(vsize) + off + bytes > o.size() || !bytes) continue;
    const uint64_t h = CodeHash(o.data() + vsize + off, bytes / 4) ^ (e->vertices ? 1 : 0);
    auto [slot, inserted] = g_library_by_code.emplace(h, e);
    if (!inserted) {
      shared.insert(h);
      if (register_link_score(e) > register_link_score(slot->second)) slot->second = e;
    }
  }
  // Hashes several containers share are kept (first entry): identical microcode reads the same
  // constant and fetch registers, only the constant table names differ.
  REXLOG_INFO("[native] library microcode index: {} entries, {} shared by more than one container",
              g_library_by_code.size(), shared.size());
}

const masseffect::native::ShaderEntry* ByCode(const std::vector<uint32_t>& microcode, bool vertex,
    const masseffect::native::ShaderEntry** observed = nullptr) {
  const uint64_t h = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t));
  const masseffect::native::ShaderEntry* entry = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    auto it = g_by_code.find(h);
    if (it != g_by_code.end()) entry = it->second;
  }
  if (!entry) {
    auto it = g_library_by_code.find(h ^ (vertex ? 1 : 0));
    if (it != g_library_by_code.end()) entry = it->second;
  }
  if (observed) *observed = entry;  // Preserve rejected candidates for bounded diagnostics.
  if (!entry) return nullptr;
  // VS fetch-patch compatibility remains checked by IdentifyShader, unchanged.
  if (vertex) return entry->vertices ? entry : nullptr;
  if (!PixelShaderIdentityMatches(
      {ShaderIdentityStage::Pixel, microcode},
      {entry->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel,
       entry->microcode})) return nullptr;
  return entry;
}
std::atomic<const masseffect::native::ShadersNative*> g_library{nullptr};
// Vertex shaders seen only after Direct3D patched their microcode (sub_821FE318) are found by the
// virtual part of their container (header, constant table, vertex elements); hashes shared by more
// than one library entry are left out.
std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> g_vs_by_virtual;
bool g_vs_by_virtual_built = false;

void BuildVirtualIndex(const masseffect::native::ShadersNative* library) {
  std::unordered_set<uint64_t> shared;
  for (uint32_t n = 0;; ++n) {
    const masseffect::native::ShaderEntry* e = library->PerNumber(n);
    if (!e) break;
    if (!e->vertices || !e->shader || e->shader->original.size() < 8) continue;
    const auto& o = e->shader->original;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    if (vsize > o.size()) continue;
    const uint64_t h = XXH3_64bits(o.data(), vsize);
    if (!g_vs_by_virtual.emplace(h, e).second) shared.insert(h);
  }
  for (uint64_t h : shared) g_vs_by_virtual.erase(h);
  g_vs_by_virtual_built = true;
}

const masseffect::native::ShaderEntry* ByAddress(uint32_t physical) {
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  auto it = g_by_address.find(physical);
  return it == g_by_address.end() ? nullptr : it->second;
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) return X_STATUS_SUCCESS;
    app_context_ = app_context;
    if (!provider_) {
      // The XenosRecomp SPIR-V needs device features that must be requested at creation.
      rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[native] could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] native graphics system: no Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ = memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this,
                                                        &MmioRead, &MmioWrite);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) return X_STATUS_SUCCESS;
    g_vs_identity_alu_tolerance.store(uint32_t(REXCVAR_GET(masseffect_native_vs_identity_tolerance)),
                                      std::memory_order_relaxed);
    identity_flat_ = REXCVAR_GET(masseffect_native_flat_identity);
    g_record_fast = pairing_fast_ = REXCVAR_GET(masseffect_native_fast_pair);
    raw_microcode_ = REXCVAR_GET(masseffect_native_raw_microcode);
    pm4_fast_ = REXCVAR_GET(masseffect_native_pm4_fast);
    check_identity_ = check_pairing_ = check_raw_ = check_pm4_ = check_objects_ =
        uint32_t(std::max<int32_t>(0, REXCVAR_GET(masseffect_native_verify_n)));
    verify_initial_ = check_identity_;
    ab_seconds_ = REXCVAR_GET(banjo_direct_state_ab_s);
    ab_target_ = REXCVAR_GET(banjo_ab_target);
    const bool records_whole = banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_records));
    const bool records_draw_state =
        !records_whole && banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_direct_state));
    dead_.key = banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_dead_raster)) &&
                !records_draw_state;
    if (raw_microcode_ && REXCVAR_GET(masseffect_native_identity_memo) &&
        REXCVAR_GET(masseffect_native_repeated_identity) &&
        banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_shader_load_memo))) {
      load_memo_.resize(kLoadMemoSlots);
    }
    // Not with the direct renderer (its records bypass the packets this follows). Deferred recording is no obstacle:
    // it queues the Vulkan calls with their arguments by value, after the draw has read the registers.
    tiles_.key = banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_tiles_once)) &&
                 !records_draw_state;
    ab_switched_ = Clock::now();
    g_no_pairing = banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_no_draw_pairing));
    wait_vblank_ = banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_wait_vblank));
    sync_fences_ = REXCVAR_GET(banjo_ring_sync_fences);  // correctness, not an enhancement: not under the masters
    g_no_pairing_check = g_no_pairing && REXCVAR_GET(banjo_cpu_verify);
    direct::g_verify_all = REXCVAR_GET(banjo_cpu_verify) || REXCVAR_GET(banjo_diag_records_check_all);
    direct_check_ = int32_t(check_identity_);
    if (banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_host_submit))) {
      host_submit_ = true;
      submit_verify_ = REXCVAR_GET(banjo_cpu_verify) || REXCVAR_GET(banjo_diag_records_check_all);
      submit::g_on.store(true, std::memory_order_release);
    }
    if (records_whole || records_draw_state) {
      direct::g_ring.reset(new (std::align_val_t(4096)) uint32_t[direct::kRingWords]());
      direct::g_whole.store(records_whole, std::memory_order_relaxed);
      static_on_ = records_whole &&
                   banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_records_static));
      // banjo_d3d_records_barriers (step 6b).
      banjo::records::g_barriers_in_records.store(
          records_whole && banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_records_barriers)),
          std::memory_order_relaxed);
      if (direct::g_verify_all) {
        direct::g_diag_writer.assign(direct::kDiagSlots, 0);
        direct::g_diag_words.resize(direct::kDiagSlots);
      }
      records_report_ = records_whole;
      direct::g_zero_ahead =
          records_whole && banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_records_zero_ahead));
      if (records_whole &&
          banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_d3d_records_native))) {
        for (auto& area : direct::g_staging) area = std::make_unique<uint32_t[]>(banjo::records::kStagingWords);
        direct::g_native_ops.store(true, std::memory_order_relaxed);
      }
      direct::g_on.store(true, std::memory_order_release);
      direct::g_keep_swaps = uint64_t(REXCVAR_GET(banjo_direct_state_keep_swaps));
    }
    REXLOG_INFO("[native] banjo_direct_state {}, banjo_d3d_records {}, banjo_no_draw_pairing {}{}",
                records_draw_state ? "on" : "off", records_whole ? "on (records of whole calls)" : "off",
                g_no_pairing ? "on" : "off", REXCVAR_GET(banjo_cpu_verify) ? " (verify: every record and draw checked)" : "");
    if (identity_flat_ || pairing_fast_ || raw_microcode_ || pm4_fast_)
      REXLOG_INFO("[native] ring CPU switches: flat identity {}, fast pairing {}, raw microcode compare {}, fast PM4 "
                  "runs {}; self-check of the first {} uses of each", identity_flat_, pairing_fast_, raw_microcode_,
                  pm4_fast_, check_identity_);
    g_native_active.store(true, std::memory_order_release);
    const char* lib = std::getenv("MASSEFFECT_SHADER_LIBRARY");
    const auto lib_path = lib && *lib ? std::filesystem::path(lib)
                                      : rex::filesystem::GetExecutableFolder() / "banjo_shaders.mesp";  // banjo-nx
    if (shaders_.Load(lib_path, REXCVAR_GET(masseffect_shaders_index))) {
      masseffect::native::g_active_library = &shaders_;
      BuildLibraryCodeIndex(shaders_);
      g_library.store(&shaders_, std::memory_order_release);
      REXLOG_INFO("[native] shader library {}", lib_path.string());
    } else {
      REXLOG_WARN("[native] no shader library at {}: nothing will be drawn", lib_path.string());
    }
    start_ = last_report_ = Clock::now();
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync native");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring native");
    ring_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    masseffect::native::NoteCommandBufferPages(ptr & 0x1FFFFFFFu, (uint32_t(1) << (size_log2 + 1)) * 4);  // primary ring
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    g_native_active.store(false, std::memory_order_release);
    if (active_.exchange(false)) {
      { std::lock_guard<std::mutex> lock(ring_mutex_); }
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
      Report(true);
    }
    draw_extent_estimator_.reset();
    extent_diagnostics_.clear();
    targets_.reset();
    if (presenter_) {
      if (app_context_) app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO ---
  static uint32_t MmioRead(void*, void* ctx, uint32_t address) {
    return static_cast<NativeGraphicsSystem*>(ctx)->ReadMmio(address);
  }
  static void MmioWrite(void*, void* ctx, uint32_t address, uint32_t value) {
    static_cast<NativeGraphicsSystem*>(ctx)->WriteMmio(address, value);
  }

  uint32_t ReadMmio(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming: return 0x08100748;
      case kRegRbBcControl: return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus: return 1;
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      default: break;
    }
    if (r >= kRegisterCount) return 0;
    return split_on_ ? exec_regs_[r] : registers_[r];  // banjo_ring_decoder: the state the executor has reached
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      // banjo_d3d_host_submit: the kick's segments, before the pointer that makes them visible.
      if (submit::Pending* pending = submit::g_on.load(std::memory_order_relaxed) ? submit::t_pending : nullptr;
          pending && pending->on) {
        if (value == pending->wptr) {
          submit::ProducerLock lock;
          const uint64_t head = submit::g_head.load(std::memory_order_relaxed);
          if (head + pending->count - submit::g_tail.load(std::memory_order_acquire) <= submit::kQueue) {
            for (uint32_t k = 0; k < pending->count; ++k)
              submit::g_queue[(head + k) & (submit::kQueue - 1)] = pending->entries[k];
            submit::g_head.store(head + pending->count, std::memory_order_release);
            submit::g_queued.fetch_add(pending->count, std::memory_order_relaxed);
          } else {
            submit::g_full.fetch_add(1, std::memory_order_relaxed);  // the decoder parses them
          }
        } else {
          submit::g_unpredicted.fetch_add(1, std::memory_order_relaxed);
        }
        pending->on = false;
      }
      // banjo_direct_state: every submission, whoever makes it. A draw hook only rewrites its own packets when none
      // happened during the draw call (the ring may already be reading a submitted segment); one being written right
      // now is published whole (g_marking).
      direct::g_wptr_writes.fetch_add(1, std::memory_order_seq_cst);
      if (direct::g_marking.load(std::memory_order_seq_cst)) [[unlikely]] {
        direct::g_kick_waits.fetch_add(1, std::memory_order_relaxed);
        for (uint32_t spin = 0; direct::g_marking.load(std::memory_order_seq_cst); ++spin) {
          if (spin > 64) rex::thread::MaybeYield();
        }
      }
      // Wake the ring thread only when it waits (an empty lock + notify on every kick cost the render
      // thread ~2 %). seq_cst on both sides (here and ring_waiting_ in RingLoop): either the ring thread's
      // predicate sees the new pointer or this load sees it waiting; its 5 ms timeout bounds any miss.
      write_pointer_.store(value, std::memory_order_seq_cst);
      if (ring_waiting_.load(std::memory_order_seq_cst)) {
        { std::lock_guard<std::mutex> lock(ring_mutex_); }
        ring_cv_.notify_one();
      }
      WakeRingWait();  // banjo_ring_wait_vblank: a submission may follow the write a WAIT_REG_MEM polls
    }
    if (r < kRegisterCount) {
      registers_[r] = value;  // no side effects through MMIO
      if (split_on_) {  // banjo_ring_decoder: both files (an MMIO write is not in the stream)
        exec_regs_[r] = value;
        split_mmio_[r >> 6].fetch_or(uint64_t(1) << (r & 63), std::memory_order_relaxed);  // the file check skips it
      }
    }
  }

  // banjo_ring_sync_fences: the CPU may learn of GPU progress here; reuse of earlier copies ends (in stream order).
  void NoteGuestSync() {
    if (!sync_fences_) return;
    if (t_decoder) {  // banjo_ring_decoder: the draws before it are the executor's
      SplitPutRaw(kSplitGuestSync, {0, 0});
      return;
    }
    masseffect::native::g_synchronizations_ring.fetch_add(1, std::memory_order_relaxed);
    ++guest_syncs_;
  }
  bool sync_fences_ = false;
  uint64_t guest_syncs_ = 0, report_guest_syncs_ = 0;

  // banjo_ring_wait_vblank: a ring sleeping in WAIT_REG_MEM looks again (after a vblank interrupt or a submission).
  void WakeRingWait() {
    wait_wake_.fetch_add(1, std::memory_order_seq_cst);
    if (ring_in_wait_.load(std::memory_order_seq_cst)) {
      { std::lock_guard<std::mutex> lock(wait_mutex_); }
      wait_cv_.notify_one();
    }
  }

  void NoteGammaRampWrite(uint32_t index, uint32_t value) {
    ++gamma_writes_;
    if (index == kRegGammaIndex) {
      gamma_component_ = 0;
    } else if (index == kRegGammaMask) {
      gamma_mask_ = value & 0b111;
    } else if (index == kRegGammaSequential) {
      const uint32_t i = registers_[kRegGammaIndex] & 0xFF;
      if (gamma_mask_ & (UINT32_C(1) << (2 - gamma_component_))) {
        gamma_ramp_[i][gamma_component_] = uint16_t((value >> 6) & 0x3FF);
        ++gamma_version_;
      }
      if (++gamma_component_ >= 3) {
        gamma_component_ = 0;
        registers_[kRegGammaIndex] =
            (registers_[kRegGammaIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
        if (t_decoder) dirty_.Mark(kRegGammaIndex);
      }
    } else if (index == kRegGamma30) {
      const uint32_t i = registers_[kRegGammaIndex] & 0xFF;
      if (gamma_mask_ & 0b001) gamma_ramp_[i][2] = uint16_t(value & 0x3FF);
      if (gamma_mask_ & 0b010) gamma_ramp_[i][1] = uint16_t((value >> 10) & 0x3FF);
      if (gamma_mask_ & 0b100) gamma_ramp_[i][0] = uint16_t((value >> 20) & 0x3FF);
      if (gamma_mask_) ++gamma_version_;
      gamma_component_ = 0;
      registers_[kRegGammaIndex] =
          (registers_[kRegGammaIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
      if (t_decoder) dirty_.Mark(kRegGammaIndex);
    }
  }

  // --- Registers written by the ring ---
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) return;
    if (index == kRegCoherStatusHost) value |= 0x80000000u;
    if (index - 0x2080u < 3u && tiles_.pass == 1) [[unlikely]] TilesWindowWritten(index - 0x2080u, value);
    if (registers_[index] != value) {  // generations: what a draw must re-upload
      if (index >= 0x4000 && index < 0x4800) {
        ++(index < 0x4400 ? gen_constants_vs_ : gen_constants_ps_);
      } else if (index >= kRegFetchFirst && index <= kRegFetchLast) {
        ++gen_fetch_;
      } else if (IsViewportRegister(index)) {
        ++gen_viewport_;
      }
    }
    if (index >= kRegGammaFirst && index <= kRegGammaLast) {
      NoteGammaRampWrite(index, value);
    }
    registers_[index] = value;
    if (split_on_) dirty_.Mark(index);  // banjo_ring_decoder (the member: a thread-local read is a call on the Switch)
    if (index >= kRegScratch0 && index <= kRegScratch7) {
      const uint32_t n = index - kRegScratch0;
      if ((1u << n) & registers_[kRegScratchUmsk]) {
        if (t_decoder) {  // banjo_ring_decoder: the game sees it, so the executor writes it in stream order
          SplitPutRaw(kSplitMemRaw, {registers_[kRegScratchAddr] + n * 4, value});
        } else {
          rex::memory::store_and_swap<uint32_t>(
              memory_->TranslatePhysical(registers_[kRegScratchAddr] + n * 4), value);
        }
      }
    }
  }

  // A run of register words into the constant/fetch range (0x4000-0x48BF), the bulk of every draw's writes:
  // no per-word special-register checks, and each generation bumped once if any word of its part changed.
  template <typename Next>
  void WriteConstantRun(uint32_t index, uint32_t count, Next next) {
    if (!REXCVAR_GET(masseffect_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      for (uint32_t i = 0; i < count; ++i) WriteRegister(index + i, next());
      return;
    }
    bool vs = false, ps = false, fetch = false;
    uint32_t* regs = registers_.data();
    if (split_on_) dirty_.MarkRange(index, count);  // banjo_ring_decoder
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t r = index + i, v = next();
      if (regs[r] != v) {
        regs[r] = v;
        if (r < 0x4400) vs = true;
        else if (r < 0x4800) ps = true;
        else fetch = true;
      }
    }
    if (vs) ++gen_constants_vs_;
    if (ps) ++gen_constants_ps_;
    if (fetch) ++gen_fetch_;
  }

  // The count guest words (big-endian, as the Reader sees them) at the reader position, if they are contiguous in
  // memory (indirect buffers never wrap; the ring only at its end).
  static const uint32_t* ContiguousWords(const Reader& data, uint32_t count) {
    if (data.mask && uint64_t(data.pos) + count > uint64_t(data.mask) + 1) return nullptr;
    return reinterpret_cast<const uint32_t*>(data.base + size_t(data.pos) * 4);
  }

  // WriteConstantRun over guest words (big-endian) in memory: the constant/fetch range (0x4000-0x48BF) is compared and
  // stored four words at a time; each generation is bumped once if any word of its part changed. Outside that range
  // (or with the fast path off) every word goes through WriteRegister as before.
  void WriteConstantRunRaw(uint32_t index, uint32_t count, const uint32_t* guest) {
    if (!REXCVAR_GET(masseffect_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      for (uint32_t i = 0; i < count; ++i) WriteRegister(index + i, __builtin_bswap32(guest[i]));
      return;
    }
    uint32_t* regs = registers_.data();
    std::vector<uint32_t> before;
    const bool check = check_pm4_ > 0;
    if (check) {
      --check_pm4_;
      before.assign(regs + index, regs + index + count);
    }
    __builtin_prefetch(guest + count);
    __builtin_prefetch(guest + count + 16);
    if (split_on_) dirty_.MarkRange(index, count);  // banjo_ring_decoder
    const RunChange change = ApplyRunRaw(regs, index, count, guest);
    if (change.vs) ++gen_constants_vs_;
    if (change.ps) ++gen_constants_ps_;
    if (change.fetch) ++gen_fetch_;
    if (check) {  // the original loop on the saved words
      std::vector<uint32_t> expected(before);
      uint32_t i = 0;
      const RunChange want = ApplyRunReference(expected.data(), index, count,
                                               [&] { return __builtin_bswap32(guest[i++]); });
      if (!(want == change) || !std::equal(expected.begin(), expected.end(), regs + index)) {
        ::me::native::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: fast PM4 register run (index {:04X} count {}): generations vs {}/{} ps {}/{} "
                     "fetch {}/{} or values differ; the old path from now on", index, count, change.vs, want.vs, change.ps,
                     want.ps, change.fetch, want.fetch);
        pm4_fast_ = false;
        for (uint32_t k = 0; k < count; ++k) regs[index + k] = __builtin_bswap32(guest[k]);
      }
    }
  }

  // WriteConstantRun over host-order words (banjo_direct_state's records): each part of the constant/fetch range
  // (VS constants, PS constants, fetch constants) compared and copied whole, its generation bumped once if it changed,
  // as WriteConstantRun does word by word. Outside that range (or with the fast path off) every word goes through
  // WriteRegister, as there.
  void WriteConstantRunHost(uint32_t index, uint32_t count, const uint32_t* v) {
    if (!REXCVAR_GET(masseffect_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      for (uint32_t i = 0; i < count; ++i) WriteRegister(index + i, v[i]);
      return;
    }
    uint32_t* regs = registers_.data();
    if (split_on_) dirty_.MarkRange(index, count);  // banjo_ring_decoder
    while (count) {
      const uint32_t part_end = index < 0x4400 ? 0x4400u : index < 0x4800 ? 0x4800u : kRegFetchLast + 1;
      const uint32_t n = std::min(count, part_end - index);
      if (std::memcmp(regs + index, v, size_t(n) * 4) != 0) {
        std::memcpy(regs + index, v, size_t(n) * 4);
        ++(index < 0x4400 ? gen_constants_vs_ : index < 0x4800 ? gen_constants_ps_ : gen_fetch_);
      }
      index += n;
      v += n;
      count -= n;
    }
  }

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t v;
    std::memcpy(&v, memory_->TranslatePhysical(address & ~3u), 4);
    return xenos::GpuSwap(v, static_cast<xenos::Endian>(address & 0x3));
  }

  // PM4_EVENT_WRITE_EXT: Direct3D's per-draw tiling extents, as the whole surface.
  void WriteExtents(uint32_t address) {
    const uint16_t extent[] = {0, kExtentMax, 0, kExtentMax, 0, 1};
    uint8_t* dest = memory_->TranslatePhysical(address & ~3u);
    for (size_t i = 0; i < std::size(extent); ++i) {
      rex::memory::store_and_swap<uint16_t>(dest + i * 2, extent[i]);
    }
  }

  // PM4_EVENT_WRITE_ZPD (banjo_ring_decoder: a function, so the executor can run it in stream order).
  void OcclusionQuery(uint32_t address) {
    // D3D9 occlusion queries on Xbox 360:
    // Issue(D3DISSUE_BEGIN): RB_SAMPLE_COUNT_ADDR points to query->begin (+0x20).
    // Issue(D3DISSUE_END):   RB_SAMPLE_COUNT_ADDR points to query->end   (+0x00).
    // D3D places the byte-swapped sentinel 0xFFFFFEED in the end structure before EVENT_WRITE_ZPD.
    if (address) {
      auto* counts =
          memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
      const uint32_t finished = rex::byte_swap(uint32_t(0xFFFFFEED));
      const bool end = counts->ZPass_A == finished || counts->ZPass_B == finished ||
                       counts->ZFail_A == finished || counts->ZFail_B == finished;
      std::memset(counts, 0, sizeof(*counts));
      // SDK fallback: begin = 0, end = 1000 samples.
      if (end) counts->Total_A = counts->ZPass_A = 1000;
      static uint32_t query_reports = 0;
      if (query_reports++ < 30) {
        REXLOG_INFO("[native] ZPD {} at {:08X}: {} samples", end ? "end" : "begin", address,
                    uint32_t(counts->ZPass_A));
      }
    }
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    if (t_decoder) {  // banjo_ring_decoder: the game sees it, so the executor writes it in stream order
      SplitPutRaw(kSplitMem, {address, value});
      return;
    }
    const uint32_t v = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~3u), &v, 4);
  }

  // --- Threads ---
  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    // Port 172 (from fnxf_xiii, its notes of 2026-10-27 and 10-29; banjo_vblank_from_display): a vblank at each of the
    // display's refreshes (its vsync event, counted by app/src/renut_engine/display_sync.cpp); the timer's only while
    // none comes for 1.5 periods (the display asleep). On the timer the guest's vblank drifted against the display and
    // 30 fps frames came out uneven (fnxf_xiii). Looked for at every tick: the display's thread may start later.
    static const bool follow_display = REXCVAR_GET(banjo_vblank_from_display);
    while (active_.load(std::memory_order_acquire)) {
      if (follow_display && rex::graphics::g_display_refresh_wait) {
        const uint64_t period_ns = uint64_t(1e9 / hz);
        uint64_t seen = rex::graphics::g_display_refresh_wait(UINT64_MAX, 0);
        auto last = Clock::now();
        while (active_.load(std::memory_order_acquire)) {
          const uint64_t count = rex::graphics::g_display_refresh_wait(seen, period_ns * 3 / 2);
          const auto now = Clock::now();
          if (count == seen && now - last < interval)
            continue;
          seen = count;
          last = now;
          counter_.fetch_add(1, std::memory_order_relaxed);
          Interrupt(0, 2);
          WakeRingWait();  // banjo_ring_wait_vblank: the interrupt handler has run
          REX_SYS_COUNT(rex::syscount::kVblankTick, 1);
        }
        return 0;
      }
      const auto now = Clock::now();
      if (now - next > std::chrono::milliseconds(250)) next = now;
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
        WakeRingWait();  // banjo_ring_wait_vblank: the interrupt handler has run
      }
      if (REXCVAR_GET(masseffect_vblank_sleep_exact)) {
        // Sleep to the next vblank instead of waking up 1000 times a second (60 of them useful).
        const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(next - Clock::now());
        rex::thread::Sleep(std::clamp(wait, std::chrono::microseconds(200), std::chrono::microseconds(50000)));
      } else {
        rex::thread::Sleep(std::chrono::milliseconds(1));
      }
      REX_SYS_COUNT(rex::syscount::kVblankTick, 1);
    }
    return 0;
  }

  // ---- banjo_ring_decoder (banjo_ring_split.h) ------------------------------------------------------------------
  enum SplitKind : uint32_t {
    kSplitPages = 1, kSplitDraw, kSplitMem, kSplitMemRaw, kSplitMemCounter, kSplitMemNotify, kSplitExtents,
    kSplitInterrupt, kSplitGuestSync, kSplitBatch, kSplitCall, kSplitCheck
  };
  struct SplitWord {
    uint32_t a, b;
  };
  struct SplitDrawItem {
    uint64_t position;
    uint32_t pos, mask;
    uint32_t second, second_window[3];
    masseffect::native::SubmissionDraw request;
  };
  static_assert(alignof(SplitDrawItem) <= 8, "queue items are 8-byte aligned");
  // Decoder: the register pages written since the last item, as an item of their own (the executor applies them first).
  void SplitPages() {
    if (!dirty_.any) return;
    uint32_t n = 0;
    for (uint32_t w = 0; w < dirty_.kWords; ++w) n += uint32_t(std::popcount(dirty_.bits[w]));
    const uint32_t bytes = 4 + n * (4 + 4 * ::me::native::split::kPageRegisters);
    uint8_t* p = split_queue_->Begin(kSplitPages, bytes);
    std::memcpy(p, &n, 4);
    p += 4;
    for (uint32_t w = 0; w < dirty_.kWords; ++w) {
      for (uint64_t bits = dirty_.bits[w]; bits; bits &= bits - 1) {
        const uint32_t page = w * 64 + uint32_t(std::countr_zero(bits));
        const uint32_t first = page << ::me::native::split::kPageShift;
        const uint32_t count = std::min<uint32_t>(::me::native::split::kPageRegisters, kRegisterCount - first);
        std::memcpy(p, &page, 4);
        std::memcpy(p + 4, registers_.data() + first, size_t(count) * 4);
        p += 4 + 4 * ::me::native::split::kPageRegisters;
      }
    }
    split_queue_->End(bytes);
    split_pages_ += n;
    dirty_.Clear();
  }
  void SplitPutRaw(uint32_t kind, SplitWord w) {
    SplitPages();
    std::memcpy(split_queue_->Begin(kind, sizeof(w)), &w, sizeof(w));
    split_queue_->End(sizeof(w));
    ++split_items_;
  }
  // Decoder: runs `f` on the executor after everything queued, and waits for it (copies, the Swap, the reports).
  void SplitCall(std::function<void()> f) {
    SplitPages();
    split_fn_ = std::move(f);
    split_fn_done_.store(false, std::memory_order_release);
    split_queue_->Begin(kSplitCall, 0);
    split_queue_->End(0);
    ++split_calls_;
    const auto t0 = Clock::now();
    // A short spin (most calls are a copy behind a few draws), then the executor's signal: a timed sleep would cost a
    // whole timer tick per call on Windows (about 1 ms: 35 calls a Swap).
    for (uint32_t laps = 0; laps < 256 && !split_fn_done_.load(std::memory_order_acquire); ++laps)
      std::this_thread::yield();
    while (!split_fn_done_.load(std::memory_order_acquire) && active_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(split_call_mutex_);
      split_call_waiting_.store(true, std::memory_order_seq_cst);
      if (!split_fn_done_.load(std::memory_order_seq_cst))
        split_call_cv_.wait_for(lock, std::chrono::milliseconds(2));
      split_call_waiting_.store(false, std::memory_order_relaxed);
    }
    split_call_wait_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
  }
  template <typename F>
  void SplitOrRun(F&& f) {
    if (t_decoder) SplitCall(std::function<void()>(std::forward<F>(f)));
    else f();
  }
  // Decoder: a draw for the executor, with its program words in a copy that stays (the stage's own vectors change
  // with the next load).
  void SplitDraw(masseffect::native::SubmissionDraw& request) {
    if (!request.vs_microcode.empty()) {
      const uint64_t key = current_vs_hash_ ^ (uint64_t(request.vs_microcode.size()) << 48);
      // The last program, without the map (consecutive draws mostly share their vertex program).
      if (key == split_last_key_ && split_last_program_ && (split_draws_ & 1023) != 0) {
        static const bool verify_last = REXCVAR_GET(banjo_cpu_verify);
        if (!verify_last) {
          request.vs_microcode = std::span<const uint32_t>(split_last_program_->data(), split_last_program_->size());
          goto program_kept;
        }
      }
      auto it = split_microcode_.find(key);
      if (it == split_microcode_.end()) {
        it = split_microcode_
                 .emplace(key, std::make_unique<std::vector<uint32_t>>(request.vs_microcode.begin(),
                                                                       request.vs_microcode.end()))
                 .first;
      } else {
        static const bool verify = REXCVAR_GET(banjo_cpu_verify);
        if ((verify || (split_draws_ & 1023) == 0) &&
            (it->second->size() != request.vs_microcode.size() ||
             std::memcmp(it->second->data(), request.vs_microcode.data(), request.vs_microcode.size_bytes()) != 0)) {
          ::me::native::VerifyDifference();
          REXLOG_ERROR("[native] DIFFERENCE: banjo_ring_decoder: the kept copy of a vertex program (hash {:016X}, {} "
                       "words) is not the program; copied again", current_vs_hash_, request.vs_microcode.size());
          it->second->assign(request.vs_microcode.begin(), request.vs_microcode.end());
        }
      }
      request.vs_microcode = std::span<const uint32_t>(it->second->data(), it->second->size());
      split_last_key_ = key;
      split_last_program_ = it->second.get();
    }
  program_kept:
    request.ps_microcode = {};
    request.edram_proof_note = nullptr;
    SplitPages();
    static const bool verify = REXCVAR_GET(banjo_cpu_verify);
    if (split_full_ || (split_draws_ & (verify ? 15 : 1023)) == 0) {  // the executor's file against the decoder's
      const uint32_t bytes = kRegisterCount * 4;
      std::memcpy(split_queue_->Begin(kSplitCheck, bytes), registers_.data(), bytes);
      split_queue_->End(bytes);
    }
    SplitDrawItem* item = new (split_queue_->Begin(kSplitDraw, sizeof(SplitDrawItem))) SplitDrawItem{
        decoder_progress_.position, decoder_progress_.pos, decoder_progress_.mask, 0, {0, 0, 0}, request};
    if (tiles_.second_pending) {  // banjo_gpu_tiles_once: the same draw into the second tile's rows, after it
      tiles_.second_pending = false;
      item->second = 1;
      std::copy(tiles_.second_window, tiles_.second_window + 3, item->second_window);
      ++tiles_.draws_other;
    }
    split_queue_->End(sizeof(SplitDrawItem));
    ++split_draws_;
    ++split_items_;
    if (!::me::native::AbOn(43)) split_queue_->WaitDrained();  // the A/B's off half: one thread's order of work
  }

  // Executor: runs the queue until the ring stops.
  void ExecutorLoop() {
    masseffect::native::RingProgress& progress = masseffect::native::g_ring_progress;
    uint32_t ring_core_tick = 0;
    while (active_.load(std::memory_order_acquire)) {
      if (ring_core_ >= 0) {
        if (::me::native::AbOn(41) != ring_pinned_) RingCorePin(!ring_pinned_);  // banjo_ring_core's A/B
        if ((++ring_core_tick & 63) == 0) {
          const int now = RexSwitchCurrentCore();
          if (now != ring_core_last_) ++ring_core_moves_;
          ring_core_last_ = now;
        }
      }
      const ::me::native::split::Header* h = split_queue_->Next(std::chrono::microseconds(2000));
      if (!h) continue;
      // The executor's time, measured on a random eighth of the items and scaled (a clock read is a system call on
      // the Switch; two per item were about 10,000 a frame). Random, because the items alternate in kinds.
      exec_sample_ ^= exec_sample_ << 13;
      exec_sample_ ^= exec_sample_ >> 7;
      exec_sample_ ^= exec_sample_ << 17;
      const bool timed = (exec_sample_ & 7) == 0;
      const auto t0 = timed ? Clock::now() : Clock::time_point{};
      const uint8_t* p = reinterpret_cast<const uint8_t*>(h + 1);
      SplitWord w{};
      if (h->bytes == sizeof(SplitWord)) std::memcpy(&w, p, sizeof(w));
      switch (h->kind) {
        case kSplitPages: {
          uint32_t n;
          std::memcpy(&n, p, 4);
          p += 4;
          for (uint32_t i = 0; i < n; ++i) {
            uint32_t page;
            std::memcpy(&page, p, 4);
            const uint32_t first = page << ::me::native::split::kPageShift;
            const uint32_t count = std::min<uint32_t>(::me::native::split::kPageRegisters, kRegisterCount - first);
            std::memcpy(exec_regs_.data() + first, p + 4, size_t(count) * 4);
            p += 4 + 4 * ::me::native::split::kPageRegisters;
          }
          break;
        }
        case kSplitCheck: {
          ++split_checks_;
          if (std::memcmp(exec_regs_.data(), p, size_t(kRegisterCount) * 4) != 0) {
            std::string first;
            uint32_t differing = 0;
            for (uint32_t r = 0; r < kRegisterCount; ++r) {
              uint32_t v;
              std::memcpy(&v, p + size_t(r) * 4, 4);
              if (v == exec_regs_[r]) continue;
              if ((split_mmio_[r >> 6].load(std::memory_order_relaxed) >> (r & 63)) & 1u) continue;
              if (++differing <= 8) first += fmt::format(" {:04X}: {:08X} (decoder {:08X})", r, exec_regs_[r], v);
            }
            if (differing && ++split_check_differences_ <= 8) {
              ::me::native::VerifyDifference();
              REXLOG_ERROR("[native] DIFFERENCE: banjo_ring_decoder: the executor's register file differs from the "
                           "decoder's in {} registers:{}; every draw carries the whole file from now on", differing,
                           first);
            }
            if (differing) {
              std::memcpy(exec_regs_.data(), p, size_t(kRegisterCount) * 4);
              split_full_ = true;
            }
          }
          break;
        }
        case kSplitDraw: {
          auto* item = const_cast<SplitDrawItem*>(reinterpret_cast<const SplitDrawItem*>(p));
          progress.position = item->position;
          progress.pos = item->pos;
          progress.mask = item->mask;
          progress.write_pointer = &write_pointer_;
          item->request.registers = exec_regs_.data();
          g_banjo_last_reject_cause = 0;
          const bool drawn = targets_ && targets_->Draw(item->request);
          if (item->second && targets_) {
            item->request.tiles_once = 2;
            std::copy(item->second_window, item->second_window + 3, item->request.tiles_window);
            targets_->Draw(item->request);
          }
          if (!drawn && ++native_draw_failures_ <= 8)
            REXLOG_WARN("[native] draw rejected before completion; visual conformance is not established");
          item->~SplitDrawItem();
          break;
        }
        case kSplitMem: WriteMemory(w.a, w.b); break;
        case kSplitMemRaw: rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(w.a), w.b); break;
        case kSplitMemCounter:
          WriteMemory(w.a, counter_.load(std::memory_order_relaxed));
          NotifyRingProgress();
          break;
        case kSplitMemNotify:
          WriteMemory(w.a, w.b);
          NotifyRingProgress();
          break;
        case kSplitExtents: WriteExtents(w.a); break;
        case kSplitInterrupt:
          for (uint32_t cpu = 0; cpu < 6; ++cpu) {
            if (w.a & (1u << cpu)) Interrupt(1, cpu);
          }
          break;
        case kSplitGuestSync: NoteGuestSync(); break;
        case kSplitBatch: {
          const uint32_t writeback = read_writeback_.load(std::memory_order_acquire);
          if (writeback) rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), w.a);
          NotifyRingProgress();
          masseffect::native::deferred::Flush();  // Batched mode: the worker must not wait for the next WPTR
          break;
        }
        case kSplitCall:
          if (split_fn_) split_fn_();
          split_fn_ = nullptr;
          split_fn_done_.store(true, std::memory_order_seq_cst);
          if (split_call_waiting_.load(std::memory_order_seq_cst)) {
            { std::lock_guard<std::mutex> lock(split_call_mutex_); }
            split_call_cv_.notify_one();
          }
          break;
        default: break;
      }
      split_queue_->Pop(h);
      if (timed)
        exec_busy_ns_ += 8 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    }
    split_fn_done_.store(true, std::memory_order_release);  // a decoder still waiting for a call lets go
  }

  // banjo_ring_core: the pin (or, in the "off" half of A/B 41, the whole mask). Ring thread only.
  void RingCorePin(bool pin) {
    const bool ok = pin ? RexSwitchPinCurrentThreadToCore(int(ring_core_)) : RexSwitchUnpinCurrentThread(-1);
    ring_pinned_ = pin;
    ++ring_core_switches_;
    if (ring_core_logs_ < 4) {
      ++ring_core_logs_;
      REXLOG_INFO("[native] ring thread {} ({}), now on core {}",
                  pin ? fmt::format("pinned to core {}", ring_core_) : std::string("on the process's whole core mask"),
                  ok ? "accepted" : "NOT accepted by the kernel", RexSwitchCurrentCore());
    }
  }

  int RingLoop() {
    ring_core_ = REXCVAR_GET(banjo_ring_core);
    if (ring_core_ >= 0) RingCorePin(::me::native::AbOn(41));
    // banjo_ring_decoder: this thread (the ring's guest thread: interrupts run guest code here) becomes the executor and
    // a new thread decodes. Not with a traced frame (the trace reads the renderer's answers per draw). The direct-state
    // records (step 5) go to the decoder: it indexes and applies them where the ring did; they are released at the
    // Swap, which runs on the executor while the decoder waits.
    split_ = banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_decoder)) &&
             REXCVAR_GET(banjo_trace_frame_s) <= 0;
    if (split_) {
      split_queue_ = std::make_unique<::me::native::split::Queue>(size_t(8) << 20, &active_);
      exec_regs_ = registers_;
      split_on_ = true;
      const int ring_core = RexSwitchCurrentCore();
      // A host thread like the ring's own, with a stack of its own size: the decoder runs the ring's whole PM4 path
      // (shader identification, the tile passes, the proofs, the logs).
      decoder_done_.store(false, std::memory_order_release);
      decoder_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
          kernel_state_, 512 * 1024, 0, [this, ring_core]() {
            const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(0x2C);
            const int32_t core = REXCVAR_GET(banjo_ring_decoder_core);
            const bool core_ok = core < 0 || RexSwitchPinCurrentThreadToCore(int(core));
            REXLOG_INFO("[native] ring split (banjo_ring_decoder): decoder thread at priority 0x2C{}, {}; the executor "
                        "(the ring thread) on core {}, now on core {}", priority_ok ? "" : " (NOT accepted)",
                        core < 0 ? std::string("not pinned")
                                 : fmt::format("pinned to core {}{}", core, core_ok ? "" : " (NOT accepted)"),
                        ring_core, RexSwitchCurrentCore());
            t_decoder = true;
            ParseLoop();
            t_decoder = false;
            decoder_done_.store(true, std::memory_order_release);
            return 0;
          }));
      decoder_thread_->set_name("GPU ring decoder");
      decoder_thread_->Create();
      ExecutorLoop();
      // The decoder leaves its loop with the ring (active_); a call it still waits for has been let go above.
      for (int i = 0; i < 2000 && !decoder_done_.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return 0;
    }
    return ParseLoop();
  }

  // The ring's PM4 loop (with banjo_ring_decoder on the decoder thread).
  int ParseLoop() {
    uint32_t read = 0;
    uint32_t generation = ring_generation_.load(std::memory_order_acquire);
    uint32_t ring_core_tick = 0;
    while (active_.load(std::memory_order_acquire)) {
      if (ring_core_ >= 0 && !t_decoder) {
        if (::me::native::AbOn(41) != ring_pinned_) RingCorePin(!ring_pinned_);  // banjo_ring_core's A/B
        if ((++ring_core_tick & 63) == 0) {  // core changes, sampled (the core number is a system call)
          const int now = RexSwitchCurrentCore();
          if (now != ring_core_last_) ++ring_core_moves_;
          ring_core_last_ = now;
        }
      }
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_waiting_.store(true, std::memory_order_seq_cst);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(5), [&] {
          const uint32_t words = ring_words_.load(std::memory_order_acquire);
          return !active_.load(std::memory_order_acquire) ||
                 (words && (write_pointer_.load(std::memory_order_seq_cst) & (words - 1)) != read);
        });
        ring_waiting_.store(false, std::memory_order_relaxed);
      }
      if (!active_.load(std::memory_order_acquire)) break;
      if (!t_decoder) {
        Report(false);
      } else if (Clock::now() - last_report_ >= std::chrono::seconds(10)) {
        SplitCall([this] { Report(false); });  // banjo_ring_decoder: the reports read the executor's renderer
      }
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;
        if (host_submit_) {  // banjo_d3d_host_submit: positions in the old ring mean nothing in the new one
          while (submit_tail_ != submit::g_head.load(std::memory_order_acquire)) {
            const submit::Entry& e = submit::g_queue[submit_tail_ & (submit::kQueue - 1)];
            if (e.kind == 1)
              submit::g_ops_tail.store(submit::g_ops_tail.load(std::memory_order_relaxed) + e.length,
                                       std::memory_order_release);
            ++submit_tail_;
          }
          submit::g_tail.store(submit_tail_, std::memory_order_release);
        }
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) continue;
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) continue;
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.physical = base;
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      const auto work_start = Clock::now();
      // banjo_ring_vertex_persistent_submitted: where each packet ends in the stream, counted without wrapping
      // (masseffect::native::RingSubmitted). A ring set up again starts far above every position given before.
      masseffect::native::RingProgress& progress =
          t_decoder ? decoder_progress_ : masseffect::native::g_ring_progress;  // banjo_ring_decoder: per draw there
      if (progress.write_pointer != &write_pointer_ || progress.mask != reader.mask || ring_progress_generation_ != gen) {
        ring_progress_generation_ = gen;
        ring_progress_total_ += uint64_t(1) << 32;
        progress.write_pointer = &write_pointer_;
        progress.mask = reader.mask;
      }
      while (reader.Pending()) {
        if (host_submit_ && SubmittedSegment(reader, read, base, progress)) continue;  // banjo_d3d_host_submit
        const uint32_t header = reader.Peek();
        const uint32_t length = (header >> 30) == 0   ? (header ? ((header >> 16) & 0x3FFF) + 1 : 0)
                                : (header >> 30) == 1 ? 2
                                : (header >> 30) == 2 ? 0
                                                      : ((header >> 16) & 0x3FFF) + 1;
        progress.pos = (reader.pos + length + 1) & reader.mask;
        progress.position = ring_progress_total_ + ((reader.pos - read) & reader.mask) + length + 1;
        if (!Packet(reader, 0)) break;  // partial packet: the rest comes with the next WPTR
      }
      if (host_submit_) SubmittedPassed(read, reader.pos, reader.mask);
      ring_progress_total_ += (reader.pos - read) & reader.mask;
      ring_busy_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - work_start).count());
      read = reader.pos;
      if (t_decoder) {  // banjo_ring_decoder: the read pointer and the notifications where the executor is
        SplitPutRaw(kSplitBatch, {read, 0});
        continue;
      }
      const uint32_t writeback = read_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
      }
      NotifyRingProgress();
      masseffect::native::deferred::Flush();  // Batched mode: the worker must not wait for the next WPTR
    }
    return 0;
  }

  // banjo_d3d_host_submit: the front of the kicks' queue, if its packet is the one at the reader: run as that packet
  // (Type3 of the indirect buffer, from the host words) without reading the ring. An entry further on (or of a kick
  // after this write pointer) waits; one the reader went past is dropped at the end of the window (SubmittedPassed).
  bool SubmittedSegment(Reader& reader, uint32_t read, uint32_t base, masseffect::native::RingProgress& progress) {
    {
      if (submit_tail_ == submit::g_head.load(std::memory_order_acquire)) return false;
      const submit::Entry e = submit::g_queue[submit_tail_ & (submit::kQueue - 1)];
      if (e.at == reader.pos && e.kind == 1) {  // ring words: their operations
        const uint32_t words = (e.after - e.at) & reader.mask;
        if (reader.Pending() < words) return false;
        ++submit_tail_;
        submit::g_tail.store(submit_tail_, std::memory_order_release);
        progress.pos = e.after;
        progress.position = ring_progress_total_ + ((reader.pos - read) & reader.mask) + words;
        reader.Advance(words);
        ++submitted_words_entries_;
        uint32_t ops[2 * 64 + 4];
        for (uint32_t k = 0; k < e.length; ++k) ops[k] = submit::g_ops[(e.address + k) & (submit::kOps - 1)];
        submit::g_ops_tail.store(submit::g_ops_tail.load(std::memory_order_relaxed) + e.length,
                                 std::memory_order_release);
        ApplyOps(ops, ops + e.length, 0);
        return true;
      }
      if (e.at == reader.pos) {
        if (reader.Pending() < 3) return false;
        // The check (the first entries, all in verify mode): the ring holds that packet.
        if (submit_checks_ > 0 || submit_verify_) {
          if (submit_checks_ > 0) --submit_checks_;
          ++submit_checked_;
          Reader r = reader;
          const uint32_t h = r.Read(), a = r.Read(), l = r.Read();
          if (h != 0xC0013F00u || a != e.address || l != e.length) {
            ::me::native::VerifyDifference();
            submit::g_failed.store(true, std::memory_order_relaxed);
            host_submit_ = false;
            REXLOG_ERROR("[native] DIFFERENCE: the kick's queued segment (banjo_d3d_host_submit) at ring position {} is "
                         "{:08X} {:08X} {:08X} in the ring, the queue says {:08X} {:08X}; the ring is parsed from now on",
                         e.at, h, a, l, e.address, e.length);
            return false;
          }
        }
        ++submit_tail_;
        submit::g_tail.store(submit_tail_, std::memory_order_release);
        progress.pos = (reader.pos + 3) & reader.mask;
        progress.position = ring_progress_total_ + ((reader.pos - read) & reader.mask) + 3;
        packet_address_ = reader.physical + reader.pos * 4;
        tiles_.packet_predicated = false;
        reader.Advance(3);
        ++packets_;
        ++submitted_segments_;
        const uint32_t words[2] = {__builtin_bswap32(e.address), __builtin_bswap32(e.length)};  // guest order
        Reader r;
        r.base = reinterpret_cast<const uint8_t*>(words);
        r.physical = packet_address_ + 4;
        r.mask = 0;
        r.pos = 0;
        r.end = 2;
        Type3(r, 0xC0013F00u, 2, 0);
        return true;
      }
      return false;  // further on, or not this window's: the packet at the reader is parsed
    }
  }
  // After a window [read, end) of the ring: entries whose packet was in it and that were not run (the ring was parsed
  // there instead: set up again, or the kick's packet was overwritten) are dropped.
  void SubmittedPassed(uint32_t read, uint32_t end, uint32_t mask) {
    while (submit_tail_ != submit::g_head.load(std::memory_order_acquire)) {
      const submit::Entry& e = submit::g_queue[submit_tail_ & (submit::kQueue - 1)];
      if (((e.at - read) & mask) >= ((end - read) & mask)) break;
      if (e.kind == 1)
        submit::g_ops_tail.store(submit::g_ops_tail.load(std::memory_order_relaxed) + e.length,
                                 std::memory_order_release);
      ++submit_tail_;
      ++submit_dropped_;
    }
    submit::g_tail.store(submit_tail_, std::memory_order_release);
  }
  bool host_submit_ = false, submit_verify_ = false;
  uint64_t submit_tail_ = 0, submit_checked_ = 0, submit_dropped_ = 0, submitted_segments_ = 0,
           submitted_words_entries_ = 0;
  int32_t submit_checks_ = 4096;

  // --- PM4 ---
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0: count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0; break;
      case 1: count = 2; break;
      case 2: count = 0; break;
      default: count = ((packet >> 16) & 0x3FFF) + 1; break;
    }
    if (reader.Pending() < count + 1) return false;
    if (pm4_fast_) {  // the stream was written by the game thread's core: pull the lines ahead of the parse
      const uint32_t ahead = reader.pos + count + 1 + 48;
      __builtin_prefetch(reader.base + size_t(reader.mask ? (ahead & reader.mask) : ahead) * 4);
    }
    packet_address_ = reader.physical + reader.pos * 4;
    tiles_.packet_predicated = (packet >> 30) == 3 && (packet & 0x1);  // banjo_gpu_tiles_once
    reader.Advance(1);
    Reader data = reader;
    reader.Advance(count);
    ++packets_;
    ++pm4_packets_;  // banjo_d3d_records: what is still parsed as PM4 (report)
    pm4_words_ += count + 1;
    if (records_report_) [[unlikely]] {
      const uint32_t kind = (packet >> 30) == 3 ? (packet >> 8) & 0x7F : (packet >> 30) == 0 ? 128u : 129u;
      const uint32_t bucket = packet_address_ - 0x1FB00000u < 0x2000u ? 3u : uint32_t(std::min(depth, 2));
      ++pm4_parsed_[bucket][kind];
      pm4_parsed_words_[bucket][kind] += count + 1;
    }
    if (!packet) return true;
    if (trace_pm4_) [[unlikely]] TracePacket(packet, data, count, depth);
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool single = (packet >> 15) & 0x1;
        ++type0_packets_;
        type0_words_ += count;
        type0_single_ += single;
        if (single) {
          for (uint32_t i = 0; i < count; ++i) WriteRegister(index, data.Read());
        } else if (const uint32_t* words = pm4_fast_ ? ContiguousWords(data, count) : nullptr) {
          WriteConstantRunRaw(index, count, words);
        } else {
          WriteConstantRun(index, count, [&] { return data.Read(); });
        }
        break;
      }
      case 1: {
        const uint32_t v1 = data.Read(), v2 = data.Read();
        WriteRegister(packet & 0x7FF, v1);
        WriteRegister((packet >> 11) & 0x7FF, v2);
        break;
      }
      case 2: break;
      default: Type3(data, packet, count, depth); break;
    }
    return true;
  }

  static uint32_t ConstantBase(uint32_t type) {
    switch (type) {
      case 0: return 0x4000;  // ALU
      case 1: return 0x4800;  // fetch
      case 2: return 0x4900;  // bool
      case 3: return 0x4908;  // loop
      case 4: return 0x2000;  // registers
      default: return UINT32_MAX;
    }
  }

  void Type3(Reader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    ++opcodes_[opcode];
    opcode_words_[opcode] += words;
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      // A draw the predicate drops still had its Draw* record: consume it so the pairing stays in step.
      if ((opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2) &&
          words >= (opcode == xenos::PM4_DRAW_INDX ? 2u : 1u)) {
        if (opcode == xenos::PM4_DRAW_INDX) data.Read();
        const uint32_t initiator = data.Read();
        const uint32_t saved = registers_[kRegVgtDrawInitiator];
        registers_[kRegVgtDrawInitiator] = initiator;
        if (t_decoder) dirty_.Mark(kRegVgtDrawInitiator);
        PairDraw();
        if (tiles_.pass == 1 && !tiles_.other_context && !tiles_.first_context && !tiles_.failed &&
            opcode != xenos::PM4_XE_SWAP)
          TilesOtherDraw(data, initiator, words - (opcode == xenos::PM4_DRAW_INDX ? 2u : 1u));
        registers_[kRegVgtDrawInitiator] = saved;
        if (t_decoder) dirty_.Mark(kRegVgtDrawInitiator);
        ++draws_predicated_;
      } else if (tiles_.pass == 1 && !tiles_.other_context && !tiles_.first_context && !tiles_.failed &&
                 opcode != xenos::PM4_XE_SWAP && TilesOtherPacket(data, opcode, words, depth)) {
        return;  // a buffer of the second tile, run here
      }
      return;
    }
    if (tiles_.other_context) [[unlikely]] {
      // banjo_gpu_tiles_once: a buffer of the second tile, run during the first pass for its draws. State and draws
      // run; what tells the game something (events, interrupts, memory writes, waits) waits for the second pass,
      // which runs this buffer again as always. Anything else is not something this handles.
      switch (opcode) {
        case xenos::PM4_NOP: case xenos::PM4_SET_CONSTANT: case xenos::PM4_SET_CONSTANT2:
        case xenos::PM4_SET_SHADER_CONSTANTS: case xenos::PM4_LOAD_ALU_CONSTANT: case xenos::PM4_IM_LOAD:
        case xenos::PM4_IM_LOAD_IMMEDIATE: case xenos::PM4_INVALIDATE_STATE: case xenos::PM4_SET_BIN_MASK:
        case xenos::PM4_SET_BIN_MASK_LO: case xenos::PM4_SET_BIN_MASK_HI: case xenos::PM4_DRAW_INDX:
        case xenos::PM4_DRAW_INDX_2: case xenos::PM4_INDIRECT_BUFFER: case xenos::PM4_INDIRECT_BUFFER_PFD:
        case xenos::PM4_REG_RMW:
          break;
        case xenos::PM4_EVENT_WRITE: case xenos::PM4_EVENT_WRITE_SHD: case xenos::PM4_EVENT_WRITE_CFL:
        case xenos::PM4_EVENT_WRITE_EXT: case xenos::PM4_EVENT_WRITE_ZPD: case xenos::PM4_INTERRUPT:
        case xenos::PM4_WAIT_REG_MEM: case xenos::PM4_REG_TO_MEM: case xenos::PM4_MEM_WRITE:
          return;
        default:
          TilesBack("a packet in a buffer of the second tile that is neither state, a draw nor an event");
          return;
      }
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        NoteGuestSync();
        if (words < 1) break;
        const uint32_t cpus = data.Read();
        if (t_decoder) {  // banjo_ring_decoder: guest code, on the ring's own guest thread, in stream order
          SplitPutRaw(kSplitInterrupt, {cpus, 0});
          break;
        }
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) Interrupt(1, cpu);
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
        DeadUndecided("a Swap");  // banjo_gpu_dead_raster
        NoteGuestSync();
        if (words >= 4) {
          data.Read();
          data.Read();  // frontbuffer
          swap_width_ = data.Read();
          swap_height_ = data.Read();
        }
        SplitOrRun([this] {  // banjo_ring_decoder: the Swap where the executor is (the decoder waits)
          ++swaps_;
          counter_.fetch_add(1, std::memory_order_relaxed);
          TilesPassTime(0);
          tiles_.pass = 0;  // banjo_gpu_tiles_once: a scene does not span a Swap
          tiles_.wide = tiles_.skip = false;
          TraceSwap();
          Present();
          DirectStateAb();
        });
        // The records the decoder is done with (step 7: on the decoder, after the executor's Swap: with the ring split
        // this was the executor's work, a scan of cold ring headers and markers each Swap).
        ReleaseDirectRecords();
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        masseffect::native::NoteCommandBufferPages(address, length * 4);  // command buffers (Direct3D's and the engine's)
        // How much of the command stream runs more than once a frame (predicated tiling's per-tile passes): report.
        direct::g_span_ib[std::min(depth, 2)].Note(address);
        ++indirect_runs_;
        if (!indirect_seen_.insert((uint64_t(address) << 20) | length).second) {
          ++indirect_repeats_;
          indirect_repeat_words_ += length;
        }
        // banjo_gpu_tiles_once: a buffer of the first tile alone: what it sets and draws is not the second tile's.
        const bool first_alone = tiles_.pass == 1 && !tiles_.other_context && (packet & 0x1) &&
                                 tiles_.select_next_known && !(bin_mask_ & tiles_.select_next);
        struct FirstContext {
          int* depth;
          ~FirstContext() { if (depth) --*depth; }
        } first_context{first_alone ? &tiles_.first_context : nullptr};
        if (first_alone) ++tiles_.first_context;
        if (depth < kMaxIndirectDepth) {
          Reader r;
          r.base = memory_->TranslatePhysical(address);
          r.physical = address;
          r.end = length;
          if (trace_pm4_) [[unlikely]]
            trace_ibs_.push_back({address, length, XXH3_64bits(r.base, size_t(length) * 4), draws_, copies_});
          if (!static_on_ || trace_pm4_ || !StaticBuffer(r.base, address, length, depth + 1)) {
            while (r.Pending()) {
              if (!Packet(r, depth + 1)) break;
            }
          }
          if (trace_pm4_ && !trace_ibs_.empty()) [[unlikely]] {
            const TraceIb ib = trace_ibs_.back();
            trace_ibs_.pop_back();
            REXLOG_INFO("[trace] {:{}}IB {:08X} len {} hash {:016X}: {} draws, {} resolves, {} predicated out, {} state "
                        "words, {} constant words{}{}", "", depth * 2, ib.address, ib.length, ib.hash,
                        draws_ - ib.draws, copies_ - ib.copies, ib.predicated, ib.state_words, ib.constant_words,
                        ib.notable.empty() ? "" : " |", ib.notable);
          }
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (words < 5) break;
        const uint32_t info = data.Read(), poll = data.Read(), ref = data.Read(), mask = data.Read();
        const auto wait_start = Clock::now();
        auto limit = wait_start + kWaitRegMemMax;
        // The time spent waiting for the game (not ring work): counted from the first unmet check.
        struct WaitTime {
          uint64_t& total;
          Clock::time_point start;
          WaitStat* stat;
          bool waited = false;
          uint32_t polls = 0;
          ~WaitTime() {
            if (!waited) return;
            const uint64_t ns =
                uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
            total += ns;
            if (stat) {  // banjo-nx: what the ring waits for (report)
              ++stat->waits;
              stat->ns += ns;
              stat->polls += polls;
            }
          }
        } wait_time{ring_wait_ns_, wait_start,
                    &wait_stats_[(uint64_t((info & 0x10) != 0) << 63) | (uint64_t(poll) << 32) | ref]};
        for (;;) {
          if (!(info & 0x10) && poll == kRegCoherStatusHost && (registers_[poll] & 0x80000000u)) {
            registers_[poll] = 0;  // MakeCoherent: no shared memory to synchronize
            if (t_decoder) dirty_.Mark(poll);
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, ref)) break;
          wait_time.waited = true;
          ++wait_time.polls;
          if (t_decoder && !split_queue_->Drained()) limit = Clock::now() + kWaitRegMemMax;  // banjo_ring_decoder
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_timed_out_++ == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, ref, mask);
            }
            break;
          }
          if (!wait_vblank_ || wait_time.polls < 16) {
            rex::thread::Sleep(std::chrono::microseconds(50));
          } else {
            // banjo_ring_wait_vblank: until the next vblank interrupt or submission, or 250 us (the frame pacing word
            // is written by a game thread at another moment: most waits end on the timeout; 250 us bounds the delay).
            std::unique_lock<std::mutex> lock(wait_mutex_);
            const uint64_t seen = wait_wake_.load(std::memory_order_seq_cst);
            ring_in_wait_.store(true, std::memory_order_seq_cst);
            const bool woken = wait_cv_.wait_for(lock, std::chrono::microseconds(250), [&] {
              return wait_wake_.load(std::memory_order_seq_cst) != seen || !active_.load(std::memory_order_acquire);
            });
            ring_in_wait_.store(false, std::memory_order_relaxed);
            if (wait_time.stat) ++(woken ? wait_time.stat->woken : wait_time.stat->timeouts);
          }
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read(), and_v = data.Read(), or_v = data.Read();
        uint32_t v = Register(info & 0x1FFF);
        v &= ((info >> 31) & 0x1) ? Register(and_v & 0x1FFF) : and_v;
        v |= ((info >> 30) & 0x1) ? Register(or_v & 0x1FFF) : or_v;
        WriteRegister(info & 0x1FFF, v);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        NoteGuestSync();
        if (words < 2) break;
        const uint32_t reg = data.Read(), address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        NoteGuestSync();
        if (words < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i, address += 4) WriteMemory(address, data.Read());
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read(), poll = data.Read(), ref = data.Read(), mask = data.Read();
        const uint32_t dest = data.Read(), value = data.Read();
        // banjo_ring_decoder: the memory it tests may be the executor's still to write: tested where the executor is.
        bool memory_write = false;
        SplitOrRun([&] {
          const uint32_t v = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (!Compare(info, v & mask, ref)) return;
          if (info & 0x100) {
            WriteMemory(dest, value);
            memory_write = true;
          } else {
            WriteRegister(dest, value);
            if (split_on_ && dest < kRegisterCount) exec_regs_[dest] = registers_[dest];
          }
        });
        if (memory_write) NoteGuestSync();
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(kRegVgtEventInitiator, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        NoteGuestSync();
        if (words < 3) break;
        const uint32_t initiator = data.Read(), address = data.Read(), value = data.Read();
        WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
        if (t_decoder) {  // banjo_ring_decoder: written (and the counter read) where the executor is
          SplitPutRaw((initiator >> 31) & 0x1 ? kSplitMemCounter : kSplitMemNotify, {address, value});
          break;
        }
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed)
                                                       : value);
        // The D3D fence word the game's wait loop compares (sub_821FB848 / sub_821FEBE0): wake it now, not
        // at the end of the ring segment.
        NotifyRingProgress();
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {  // not a sync point: Direct3D's per-draw tiling extents (~1 per tiled draw)
        if (words < 2) break;
        const uint32_t initiator = data.Read(), address = data.Read();
        WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
        if (t_decoder) {  // banjo_ring_decoder: Direct3D reads them: the executor writes them in stream order
          SplitPutRaw(kSplitExtents, {address, 0});
          break;
        }
        WriteExtents(address);
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        NoteGuestSync();
        if (words < 1) break;
        WriteRegister(kRegVgtEventInitiator, data.Read() & 0x3F);
        {
          const uint32_t zpd_address = Register(kRegRbSampleCountAddr);
          SplitOrRun([this, zpd_address] { OcclusionQuery(zpd_address); });  // banjo_ring_decoder
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase((type_index >> 16) & 0xFF);
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < words; ++i) WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        if (const uint32_t* raw = pm4_fast_ ? ContiguousWords(data, words - 1) : nullptr) {
          WriteConstantRunRaw(index, words - 1, raw);
        } else {
          WriteConstantRun(index, words - 1, [&] { return data.Read(); });
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t n = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase((type_index >> 16) & 0xFF);
        if (base != UINT32_MAX) {
          const uint8_t* src = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          uint32_t i = 0;
          if (pm4_fast_) {
            WriteConstantRunRaw(index, n, reinterpret_cast<const uint32_t*>(src));
          } else {
            WriteConstantRun(index, n, [&] { return rex::memory::load_and_swap<uint32_t>(src + size_t(i++) * 4); });
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        TilesSelectChanged();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        TilesSelectChanged();
        break;
      case xenos::PM4_SET_BIN_MASK:
        if (words >= 2) {
          const uint64_t hi = data.Read(), lo = data.Read();
          bin_mask_ = (hi << 32) | lo;
        }
        break;
      case xenos::PM4_SET_BIN_SELECT:
        if (words >= 2) {
          const uint64_t hi = data.Read(), lo = data.Read();
          bin_select_ = (hi << 32) | lo;
        }
        TilesSelectChanged();
        break;
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        if (skip) data.Read();
        const uint32_t initiator = data.Read();
        WriteRegister(kRegVgtDrawInitiator, initiator);
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA) && words >= skip + 3) {
          WriteRegister(kRegVgtDmaBase, data.Read());
          WriteRegister(kRegVgtDmaSize, data.Read());
        }
        if ((Register(kRegRbModeControl) & 0x7) == uint32_t(xenos::EdramMode::kCopy)) {
          ++copies_;
          resolve_destinations_.insert(Register(rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE) & 0x1FFFF000u);
          if (trace_phase_ == 1) TraceCopy();
          const auto copy_start = Clock::now();
          Copy();
          submit_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - copy_start).count());
        } else {
          ++draws_;
          if (depth > 0) ++draws_indirect_;
          PairDraw();
          trace_result_ = -1;
          tiles_.draw_predicated = (packet & 0x1) != 0;
          if (draw_vs_ && (draw_ps_ || (Register(kRegRbModeControl) & 0x7) == 5)) {
            const auto draw_start = Clock::now();
            Draw();
            submit_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - draw_start).count());
          } else {
            ++draws_unidentified_;
            ++missing_draw_pairs_[{draw_vs_ ? 0 : current_vs_hash_, draw_ps_ ? 0 : current_ps_hash_}];
          }
          if (trace_phase_ == 1) TraceDraw();
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        if (words < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        im_load_address_ = address_type & 0x1FFFFFFCu;
        const uint8_t* src = memory_->TranslatePhysical(address_type & ~3u);
        const uint32_t load_type = address_type & 0x3;
        if (raw_microcode_ && load_type <= 1 && ReloadsSameMicrocode(load_type, src, size)) break;
        if (raw_microcode_ && load_type <= 1 && LoadMemoHit(load_type, src, size)) break;
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
        }
        IdentifyShader(load_type);
        if (raw_microcode_ && load_type <= 1) {
          // Every path of IdentifyShader leaves the stage's microcode equal to this load: keep its raw words.
          auto& raw = load_type == 1 ? raw_ps_ : raw_vs_;
          raw.assign(reinterpret_cast<const uint32_t*>(src), reinterpret_cast<const uint32_t*>(src) + size);
          (load_type == 1 ? raw_ps_valid_ : raw_vs_valid_) = true;
          LoadMemoStore(load_type);
        }
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        if (words < 2) break;
        const uint32_t type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        if (size > words - 2) break;
        im_load_address_ = 0;  // immediate: the microcode is in the ring
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) microcode_[i] = data.Read();
        IdentifyShader(type & 0x3);
        if ((type & 0x3) == 0) raw_vs_valid_ = false;  // the raw copy no longer matches the stage's microcode
        else if ((type & 0x3) == 1) raw_ps_valid_ = false;
        break;
      }
      case xenos::PM4_NOP:
        // banjo_direct_state: the NOP that covers a draw's state packets names the host record that replaces them.
        if (words >= 2 && data.Peek() == direct::kMagic) {
          ++markers_reached_;
          pm4_words_ -= words + 1;  // (the region is not parsed)
          data.Advance(1);
          const uint32_t seq = data.Read();
          ApplyDirectRecord(seq, data, words + 1, depth);
        } else if (words >= 8 && data.Peek() == kApiClearBegin) {
          // banjo_d3d_api_clear: the markers around Direct3D's clear packets (src/me/banjo_d3d_clear.h).
          data.Advance(1);
          if (api_clear_.armed) ++api_clear_.left_open;
          api_clear_.armed = true;
          for (int32_t& v : api_clear_.rect) v = int32_t(data.Read());
          api_clear_.rt0_size = data.Read();
          api_clear_.depth_size = data.Read();
          data.Advance(1);  // Direct3D's clear flags (not needed here)
          ++api_clear_.markers;
        } else if (words >= 1 && data.Peek() == kApiClearEnd) {
          api_clear_.armed = false;
        } else if (words >= 2 && data.Peek() == kApiResolveBegin) {
          // banjo_d3d_api_resolve: the resolved surface's size when the resolve covers it whole (0: not).
          data.Advance(1);
          api_resolve_.armed = true;
          api_resolve_.whole_size = data.Read();
          ++api_resolve_.markers;
        } else if (words >= 1 && data.Peek() == kApiResolveEnd) {
          api_resolve_.armed = false;
        }
        break;
      default: break;  // IM_LOAD, IM_LOAD_IMMEDIATE, NOP, ...: nothing to do without drawing
    }
  }

  // banjo_direct_state, ring thread: an index entry (record number, first word in the ring, the Swap count when the
  // ring indexed it, reached at least once).
  struct DirectEntry {
    uint32_t seq;
    uint64_t pos;
    uint64_t swap;
    bool reached;
  };

  // banjo_direct_state, ring thread: the record a marker NOP names, applied where its packets were (`region` words from
  // packet_address_, the marker's own address; `rest` reads the marker's payload after its two words). Register runs
  // go through the same register writes as in Packet(); type-3 packets through Type3 with their original words and
  // address. The record is found by number in the index of the records in the ring, else in the side store.
  void ApplyDirectRecord(uint32_t seq, Reader rest, uint32_t region, int depth) {
    using namespace direct;
    const uint32_t marker = packet_address_;
    const uint32_t* rec = nullptr;
    auto* entry = FindEntry(seq);
    if (!entry) {  // published after the last indexing (the usual case for the first marker of a batch)
      IndexDirectRecords();
      entry = FindEntry(seq);
    }
    if (entry) {
      auto it = entry;
      if (it->reached) ++direct_replayed_;
      it->reached = true;
      rec = g_ring.get() + (it->pos & (kRingWords - 1));
    } else if (auto* kept = FindKept(seq)) {
      // A command buffer run again a frame or more later: the region's original packets are parsed (the kept head:
      // the marker's address and the three words it replaced), and those words are written back over the marker (the
      // ring is running this range, so the game is not writing it; sasrt-nx's restore): later runs parse the packets.
      ++direct_kept_hits_;
      const std::array<uint32_t, kRecordHeader> head = *kept;
      (*kept)[1] = 0;  // dropped
      --direct_kept_live_;
      ParseRegionAgain(head.data(), rest, region, marker, depth);
      std::memcpy(memory_->TranslatePhysical(marker), head.data() + 3, 12);
      ++direct_restored_;
      return;
    } else {
      if (direct_lost_++ < 8)
        REXLOG_ERROR("[native] direct-state record {} at {:08X} not found (ring {} to {}, {} kept): its draw state is "
                     "missing; no more records", seq, marker,
                     direct_entries_.empty() ? 0 : direct_entries_.front().seq,
                     direct_entries_.empty() ? 0 : direct_entries_.back().seq, direct_kept_live_);
      g_failed.store(true, std::memory_order_relaxed);
      return;
    }
    const uint32_t total = rec[1];
    if (direct_check_ > 0 || g_verify_all) {
      if (direct_check_ > 0) --direct_check_;
      ++direct_ring_checked_;
      if (!RecordMatchesRegion(rec, rest, region, marker)) {
        ::me::native::VerifyDifference();
        g_failed.store(true, std::memory_order_relaxed);
        if (direct_ring_differences_++ < 8) {
          REXLOG_ERROR("[native] DIFFERENCE: direct-state record {} differs from the packets the command buffer holds "
                       "at {:08X} ({} words) when the ring reaches them; those packets are used, and no more records",
                       seq, marker, region);
          RegionWords(rec, rest, region);
          uint32_t n = 0, decoded_region = 0;
          bool draw = false;
          direct_decoded_.resize(size_t(region) * 2 + 4);
          if (g_whole.load(std::memory_order_relaxed)) {
            uint32_t* out = direct_decoded_.data();
            decoded_region = DecodePiece(reinterpret_cast<const uint8_t*>(direct_scratch_.data()), marker, region, 0, out);
            n = uint32_t(out - direct_decoded_.data());
            REXLOG_ERROR("[native] the region decodes now to {} words of {} ({} operation words)", decoded_region,
                         region, n);
            if (!g_diag_writer.empty()) {
              const auto& then = g_diag_words[seq & (kDiagSlots - 1)];
              std::string was, is;
              for (uint32_t k = 0; k < then[47]; ++k) was += fmt::format(" {:08X}", __builtin_bswap32(then[k]));
              for (uint32_t k = 0; k < std::min<uint32_t>(region, 47); ++k)
                is += fmt::format(" {:08X}", __builtin_bswap32(direct_scratch_[k]));
              REXLOG_ERROR("[native] record {} was made by sub_{:08X}; its region then:{}", seq,
                           g_diag_writer[seq & (kDiagSlots - 1)], was);
              REXLOG_ERROR("[native] and now:{}", is);
            }
          } else {
            DecodeRegion(reinterpret_cast<const uint8_t*>(direct_scratch_.data()), marker, region,
                         direct_decoded_.data(), n, decoded_region, draw);
          }
          const uint32_t size = total - kRecordHeader;
          for (uint32_t i = 0; i < std::min(n, size); ++i) {
            if (rec[kRecordHeader + i] != direct_decoded_[i]) {
              // The operation it is in (its kind, header or first register, and the packet's address).
              uint32_t o = 0, at = 0;
              while (o < size) {
                const uint32_t tag = rec[kRecordHeader + o], kind = tag & 0xFF, count = tag >> 8;
                const uint32_t len = kind == kType1 ? 4 : kind == kType3Ref ? 3 : kind == kType3 ? 3 + count : 2 + count;
                if (o + len > i) break;
                o += len;
                ++at;
              }
              const uint32_t tag = o < size ? rec[kRecordHeader + o] : 0;
              REXLOG_ERROR("[native] first difference at operation word {} of {} (decoded now {}): record {:08X}, "
                           "command buffer {:08X}; in operation {} (kind {}, count {}, header or register {:08X}, "
                           "packet {:08X}, word {} of it)", i, size, n, rec[kRecordHeader + i], direct_decoded_[i], at,
                           tag & 0xFF, tag >> 8, o + 1 < size ? rec[kRecordHeader + o + 1] : 0,
                           o + 2 < size ? rec[kRecordHeader + o + 2] : 0, i - o);
              break;
            }
          }
        }
        ParseRegionAgain(rec, rest, region, marker, depth);
        return;
      }
    }
    ApplyOps(rec + kRecordHeader, rec + total, depth);
    ++direct_applied_;
  }

  // banjo_d3d_records_static: an indirect buffer with no record in it (Direct3D's fixed buffers: per-tile set-up, run
  // before every recorded buffer) that runs again with the same words is applied from the operations decoded the
  // second time it ran: its words are compared with a copy kept from that run (exact, not a fingerprint), so a buffer
  // Direct3D rewrites is decoded again. A buffer with a marker, a partial packet, or more than kStaticWords words is
  // parsed as it is.
  static constexpr uint32_t kStaticWords = 4096, kStaticEntries = 1024, kStaticRewrites = 3;
  struct StaticEntry {
    uint32_t length = 0;
    uint32_t rewrites = 0;  // kStaticRewrites: a buffer the game keeps rewriting (a segment): parsed from then on
    bool decoded = false;
    std::vector<uint32_t> words;  // guest order
    std::vector<uint32_t> ops;
  };
  bool StaticBuffer(const uint8_t* src, uint32_t address, uint32_t length, int depth) {
    using namespace banjo::records;
    if (length < 3 || length > kStaticWords) return false;
    {
      uint32_t size = 0;
      if (Classify(src, 0, length, size) == Kind::kOurs) return false;  // a buffer of records
    }
    auto it = static_buffers_.find(address);
    if (it == static_buffers_.end()) {
      if (static_buffers_.size() >= kStaticEntries) return false;
      StaticEntry e;
      e.length = length;
      e.words.assign(reinterpret_cast<const uint32_t*>(src), reinterpret_cast<const uint32_t*>(src) + length);
      static_buffers_.emplace(address, std::move(e));
      return false;
    }
    StaticEntry& e = it->second;
    if (e.rewrites >= kStaticRewrites) return false;
    if (e.length != length || std::memcmp(e.words.data(), src, size_t(length) * 4) != 0) {
      e.length = length;
      e.decoded = false;
      if (++e.rewrites >= kStaticRewrites) {
        e.words = {};
        e.ops = {};
      } else {
        e.words.assign(reinterpret_cast<const uint32_t*>(src), reinterpret_cast<const uint32_t*>(src) + length);
      }
      ++static_changed_;
      return false;
    }
    if (!e.decoded) {
      // Every packet as an operation (no marker: no head rule; barriers are fine here).
      e.ops.resize(size_t(length) * 2 + 4);
      uint32_t* out = e.ops.data();
      for (uint32_t i = 0; i < length;) {
        uint32_t size = 0;
        Kind kind = Classify(src, i, length, size);
        if (kind == Kind::kPartial || kind == Kind::kOurs) {
          e.length = 0;  // (never again: parsed as it is)
          return false;
        }
        if (kind == Kind::kBarrier) kind = Kind::kCopy;
        out = Emit(src, address, i, size, kind, out);
        i += size;
      }
      e.ops.resize(size_t(out - e.ops.data()));
      e.decoded = true;
      ++static_decoded_;
    }
    ++static_runs_;
    static_words_ += length;
    ApplyOps(e.ops.data(), e.ops.data() + e.ops.size(), depth);
    return true;
  }
  bool static_on_ = false;
  std::unordered_map<uint32_t, StaticEntry> static_buffers_;
  uint64_t static_runs_ = 0, static_words_ = 0, static_decoded_ = 0, static_changed_ = 0;

  // A record's operations (banjo_records.h), applied where its packets were: register runs through the same register
  // writes as in Packet(); type-3 packets through Type3 with their own header and address (what Packet() sets for a
  // packet is set for each). Also the operations of a buffer kept as a record (banjo_d3d_records_static).
  void ApplyOps(const uint32_t* op, const uint32_t* const end, int depth) {
    using namespace direct;
    while (op < end) {
      const uint32_t tag = *op++;
      const uint32_t count = tag >> 8;
      switch (tag & 0xFF) {
        case kRun: {
          const uint32_t index = *op++;
          WriteConstantRunHost(index, count, op);
          op += count;
          break;
        }
        case kRunSingle: {
          const uint32_t index = *op++;
          for (uint32_t i = 0; i < count; ++i) WriteRegister(index, op[i]);
          op += count;
          break;
        }
        case kType1: {
          const uint32_t h = op[0];
          WriteRegister(h & 0x7FF, op[1]);
          WriteRegister((h >> 11) & 0x7FF, op[2]);
          op += 3;
          break;
        }
        case kType3: {
          const uint32_t h = op[0];
          packet_address_ = op[1];
          tiles_.packet_predicated = h & 0x1;  // (as Packet() sets them)
          ++packets_;
          ++record_type3_ops_;
          Reader r;
          r.base = reinterpret_cast<const uint8_t*>(op + 2);
          r.physical = op[1] + 4;
          r.mask = 0;
          r.pos = 0;
          r.end = count;
          Type3(r, h, count, depth);
          op += 2 + count;
          break;
        }
        case kType3Ref: {  // the packet's words as the command buffer holds them now
          const uint32_t h = op[0];
          packet_address_ = op[1];
          tiles_.packet_predicated = h & 0x1;
          ++packets_;
          ++record_type3_ops_;
          Reader r;
          r.base = memory_->TranslatePhysical(op[1] + 4);
          r.physical = op[1] + 4;
          r.mask = 0;
          r.pos = 0;
          r.end = count;
          Type3(r, h, count, depth);
          op += 2;
          break;
        }
        default:
          op = end;  // cannot happen: written by the producers
          break;
      }
    }
  }

  // The index entry of a record number: numbers are consecutive (0 and kPad skipped at the wrap), so it is at
  // seq - the first number; else a binary search (across a wrap).
  DirectEntry* FindEntry(uint32_t seq) {
    DirectEntry* const none = nullptr;
    if (direct_entries_.empty()) return none;
    const uint32_t offset = seq - direct_entries_.front().seq;
    if (offset < direct_entries_.size() && direct_entries_[offset].seq == seq) return &direct_entries_[offset];
    if (int32_t(seq - direct_entries_.front().seq) < 0 || int32_t(seq - direct_entries_.back().seq) > 0) return none;
    auto it = std::lower_bound(direct_entries_.begin(), direct_entries_.end(), seq,
                               [](const DirectEntry& e, uint32_t v) { return int32_t(e.seq - v) < 0; });
    return it != direct_entries_.end() && it->seq == seq ? &*it : none;
  }

  // Ring thread: every record published since the last call into the index (they are in number order).
  void IndexDirectRecords() {
    using namespace direct;
    const uint32_t* ring = g_ring.get();
    if (!ring) return;
    constexpr uint64_t mask = kRingWords - 1;
    const uint64_t head = g_head.load(std::memory_order_acquire);
    while (direct_scanned_ < head) {
      const uint32_t s = ring[direct_scanned_ & mask];
      if (s == kPad) {
        direct_scanned_ = (direct_scanned_ | mask) + 1;
        continue;
      }
      direct_entries_.push_back({s, direct_scanned_, swaps_, false});
      direct_scanned_ += ring[(direct_scanned_ & mask) + 1];
    }
  }

  // The region a marker covers as the command buffer holds it now, in guest byte order (direct_scratch_): its first
  // three words from the record (the marker replaced them), the rest from the marker's payload.
  void RegionWords(const uint32_t* rec, Reader rest, uint32_t region) {
    direct_scratch_.resize(std::max<uint32_t>(region, 3));
    for (uint32_t i = 0; i < 3; ++i) direct_scratch_[i] = rec[3 + i];
    for (uint32_t i = 3; i < region; ++i) direct_scratch_[i] = __builtin_bswap32(rest.Read());
  }

  // Parses the region's original packets as PM4 (a record that failed the ring's check).
  void ParseRegionAgain(const uint32_t* rec, Reader rest, uint32_t region, uint32_t marker, int depth) {
    RegionWords(rec, rest, region);
    std::vector<uint32_t> words;
    words.swap(direct_scratch_);
    Reader r;
    r.base = reinterpret_cast<const uint8_t*>(words.data());
    r.physical = marker;
    r.mask = 0;
    r.pos = 0;
    r.end = region;
    while (r.Pending()) {
      if (!Packet(r, depth)) break;
    }
    words.swap(direct_scratch_);
  }

  // The ring's check: the record decodes from the region the command buffer holds now (the packets the GPU would run
  // here, in this pass), operation for operation, and names this marker.
  bool RecordMatchesRegion(const uint32_t* rec, Reader rest, uint32_t region, uint32_t marker) {
    using namespace direct;
    if (rec[2] != marker) return false;
    RegionWords(rec, rest, region);
    const uint32_t size = rec[1] - kRecordHeader;
    direct_decoded_.resize(size_t(region) * 2 + 4);
    if (g_whole.load(std::memory_order_relaxed)) {  // banjo_d3d_records: one piece, the whole region
      const uint8_t* src = reinterpret_cast<const uint8_t*>(direct_scratch_.data());
      uint32_t* out = direct_decoded_.data();
      if (DecodePiece(src, marker, region, 0, out) != region || uint32_t(out - direct_decoded_.data()) != size)
        return false;
      return std::memcmp(direct_decoded_.data(), rec + kRecordHeader, size_t(size) * 4) == 0;
    }
    uint32_t n = 0, decoded_region = 0;
    bool draw = false;
    if (DecodeRegion(reinterpret_cast<const uint8_t*>(direct_scratch_.data()), marker, region, direct_decoded_.data(),
                     n, decoded_region, draw) != Decode::kOk ||
        draw || decoded_region != region || n != size)
      return false;
    return std::memcmp(direct_decoded_.data(), rec + kRecordHeader, size_t(size) * 4) == 0;
  }

  // Whether the command buffer still holds the record's marker (else the game wrote over it: never reached again).
  bool MarkerStillThere(const uint32_t* rec) const {
    const uint8_t* m = memory_->TranslatePhysical(rec[2]);
    const uint32_t h = rex::memory::load_and_swap<uint32_t>(m);
    return (h >> 30) == 3 && ((h >> 8) & 0x7F) == xenos::PM4_NOP &&
           rex::memory::load_and_swap<uint32_t>(m + 4) == direct::kMagic &&
           rex::memory::load_and_swap<uint32_t>(m + 8) == rec[0];
  }

  // banjo_direct_state_ab_s, at a Swap: the ring's time since the last Swap goes to the current half; the halves
  // switch every N seconds.
  void DirectStateAb() {
    if (ab_seconds_ <= 0) return;
    const bool off = ::me::native::g_ab_off_target.load(std::memory_order_relaxed) != 0;
    AbHalf& h = ab_[off ? 1 : 0];
    if (split_on_) {  // banjo_ring_decoder: the executor's time is the ring's (the frame-rate limit)
      h.busy_ns += exec_busy_ns_ - ab_exec_busy_ns_;
      ab_exec_busy_ns_ = exec_busy_ns_;
      const uint64_t decoder = ring_busy_ns_ - ring_wait_ns_ - split_call_wait_ns_;
      h.decoder_ns += decoder - ab_decoder_ns_;
      ab_decoder_ns_ = decoder;
    } else {
      h.busy_ns += ring_busy_ns_ - ab_busy_ns_;
      h.wait_ns += ring_wait_ns_ - ab_wait_ns_;
    }
    h.submit_ns += submit_ns_ - ab_submit_ns_;
    ab_submit_ns_ = submit_ns_;
    h.draws += draws_ - ab_draws_;
    ++h.swaps;
    ab_busy_ns_ = ring_busy_ns_;
    ab_wait_ns_ = ring_wait_ns_;
    ab_draws_ = draws_;
    const auto now = Clock::now();
    if (now - ab_switched_ >= std::chrono::seconds(ab_seconds_)) {
      ab_switched_ = now;
      ::me::native::g_ab_off_target.store(off ? 0u : uint32_t(ab_target_), std::memory_order_relaxed);
      // The estimator holds its switches itself.
      static const bool tess_key =
          banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_tess_leftover));
      if (draw_extent_estimator_) draw_extent_estimator_->TessellationOnlyExplicit(tess_key && ::me::native::AbOn(4));
      // Deferred recording (target 16) is switched in its own layer: the ring makes the calls itself while it is off.
      if (ab_target_ == 16) masseffect::native::deferred::SetDirect(!::me::native::AbOn(16));
      REXLOG_INFO("[native] A/B: target {} now {} (swap {})", ab_target_, off ? "on" : "OFF", swaps_);
    }
  }

  // At a Swap: the records the ring indexed banjo_direct_state_keep_swaps Swaps ago leave the ring (tile passes run in the frame; command
  // buffers recorded ahead run within it); one whose marker is still in its command buffer moves to the side store,
  // which keeps it until the game writes over the marker.
  void ReleaseDirectRecords() {
    using namespace direct;
    indirect_seen_.clear();
    if (!g_ring) return;
    const auto release_start = Clock::now();
    IndexDirectRecords();
    constexpr uint64_t mask = kRingWords - 1;
    // The side store: up to 256 entries a Swap are checked for a marker the game wrote over (round robin).
    if (direct_kept_live_) {
      for (uint32_t n = 0; n < 256 && !direct_kept_.empty(); ++n) {
        auto& head = direct_kept_[direct_prune_at_++ % direct_kept_.size()];
        if (head[1] && !MarkerStillThere(head.data())) {
          head[1] = 0;
          --direct_kept_live_;
        }
      }
    }
    if (direct_kept_.size() > 2 * direct_kept_live_ + 1024) {  // mostly dropped entries: compact (order kept)
      direct_kept_.erase(std::remove_if(direct_kept_.begin(), direct_kept_.end(),
                                        [](const std::array<uint32_t, kRecordHeader>& h) { return h[1] == 0; }),
                         direct_kept_.end());
      direct_prune_at_ = 0;
    }
    // The records leaving the ring: their headers and markers are pulled into the cache first (the markers are in
    // command buffers the CPU has not touched for frames), then each still-present marker's head is kept.
    size_t n = 0;
    while (n < direct_entries_.size() && direct_entries_[n].swap + g_keep_swaps <= swaps_) {
      const uint32_t* rec = g_ring.get() + (direct_entries_[n].pos & mask);
      __builtin_prefetch(rec);
      if (n >= 8) __builtin_prefetch(memory_->TranslatePhysical(g_ring[(direct_entries_[n - 8].pos & mask) + 2]));
      ++n;
    }
    for (size_t i = n >= 8 ? n - 8 : 0; i < n; ++i)
      __builtin_prefetch(memory_->TranslatePhysical(g_ring[(direct_entries_[i].pos & mask) + 2]));
    for (size_t i = 0; i < n; ++i) {
      const DirectEntry& e = direct_entries_[i];
      const uint32_t* rec = g_ring.get() + (e.pos & mask);
      if (!e.reached) ++direct_released_unreached_;
      if (MarkerStillThere(rec)) {
        std::array<uint32_t, kRecordHeader> head;
        std::copy(rec, rec + kRecordHeader, head.begin());
        direct_kept_.push_back(head);  // number order: the ring releases in number order
        ++direct_kept_live_;
        ++direct_kept_total_;
      }
    }
    direct_entries_.erase(direct_entries_.begin(), direct_entries_.begin() + std::ptrdiff_t(n));
    direct_kept_peak_ = std::max<uint64_t>(direct_kept_peak_, direct_kept_live_);
    g_kept_full.store(direct_kept_live_ > kKeptMax, std::memory_order_relaxed);
    g_tail.store(direct_entries_.empty() ? direct_scanned_ : direct_entries_.front().pos, std::memory_order_release);
    direct_release_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - release_start).count());
  }

  // The side store's entry for a record number (binary search; the store is in number order), or null.
  std::array<uint32_t, direct::kRecordHeader>* FindKept(uint32_t seq) {
    auto it = std::lower_bound(direct_kept_.begin(), direct_kept_.end(), seq,
                               [](const std::array<uint32_t, direct::kRecordHeader>& h, uint32_t v) {
                                 return int32_t(h[0] - v) < 0;
                               });
    return it != direct_kept_.end() && (*it)[0] == seq && (*it)[1] ? &*it : nullptr;
  }

  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }

  // --- Native drawing ---
  bool EnsureTargets() {
    if (!targets_ && !targets_failed_ && t_decoder) {  // banjo_ring_decoder: made on the executor
      SplitCall([this] { EnsureTargets(); });
      return targets_ != nullptr;
    }
    if (!targets_ && !targets_failed_) {
      targets_ = masseffect::native::TargetsNative::Create(provider_ ? provider_->vulkan_device() : nullptr,
                                                        memory_);
      if (!targets_) {
        targets_failed_ = true;
        REXLOG_ERROR("[native] could not create the native render targets");
      }
    }
    return targets_ != nullptr;
  }

  bool ComputeVertexIdentity(const masseffect::native::ShaderEntry* candidate) const {
    return VertexShaderIdentityMatches(
        {ShaderIdentityStage::Vertex, vs_microcode_},
        {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel, candidate->microcode},
        candidate->elements, [](const auto& element) { return element.instruction; });
  }
  bool ComputePixelIdentity(const masseffect::native::ShaderEntry* candidate) const {
    return PixelShaderIdentityMatches(
        {ShaderIdentityStage::Pixel, ps_microcode_},
        {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel, candidate->microcode});
  }
  // Direct-mapped memo cell of a library entry for one stage's current load generation.
  struct IdentityCell {
    const masseffect::native::ShaderEntry* candidate = nullptr;
    uint64_t generation = 0;
    bool result = false;
  };
  static constexpr size_t kIdentityCells = 1024;
  static size_t IdentityCellOf(const void* entry) {
    uintptr_t x = uintptr_t(entry) >> 4;
    x ^= x >> 10;
    return size_t(x) & (kIdentityCells - 1);
  }
  // masseffect::native::FetchCoherent(*entry, vs_microcode_), memoized per VS load generation (a pure function of both).
  bool FetchCoherent(const masseffect::native::ShaderEntry* entry) {
    if (!identity_flat_) return masseffect::native::FetchCoherent(*entry, vs_microcode_);
    IdentityCell& cell = fc_cells_[IdentityCellOf(entry)];
    if (cell.candidate != entry || cell.generation != vs_identity_generation_) {
      cell = {entry, vs_identity_generation_, masseffect::native::FetchCoherent(*entry, vs_microcode_)};
    } else if (check_identity_ > 0) {
      --check_identity_;
      if (cell.result != masseffect::native::FetchCoherent(*entry, vs_microcode_)) {
        ::me::native::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: flat fetch-coherence memo disagrees with the direct check (entry n{}); the old "
                     "path from now on", entry->number);
        identity_flat_ = false;
        return masseffect::native::FetchCoherent(*entry, vs_microcode_);
      }
    }
    return cell.result;
  }
  bool MatchesPixelShaderIdentity(const masseffect::native::ShaderEntry* candidate) {
    if (!candidate) return false;
    if (!identity_flat_) return ComputePixelIdentity(candidate);
    IdentityCell& cell = ps_cells_[IdentityCellOf(candidate)];
    if (cell.candidate != candidate || cell.generation != ps_identity_generation_) {
      cell = {candidate, ps_identity_generation_, ComputePixelIdentity(candidate)};
    } else if (check_identity_ > 0) {
      --check_identity_;
      if (cell.result != ComputePixelIdentity(candidate)) {
        ::me::native::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: flat PS identity memo disagrees with the direct compare (entry n{}); the "
                     "old path from now on", candidate->number);
        identity_flat_ = false;
        return ComputePixelIdentity(candidate);
      }
    }
    return cell.result;
  }

  bool MatchesVertexShaderIdentity(const masseffect::native::ShaderEntry* candidate) {
    if (!candidate) return false;
    if (identity_flat_) {
      IdentityCell& cell = vs_cells_[IdentityCellOf(candidate)];
      if (cell.candidate != candidate || cell.generation != vs_identity_generation_) {
        cell = {candidate, vs_identity_generation_, ComputeVertexIdentity(candidate)};
      } else if (check_identity_ > 0) {
        --check_identity_;
        if (cell.result != ComputeVertexIdentity(candidate)) {
          ::me::native::VerifyDifference();
          REXLOG_ERROR("[native] DIFFERENCE: flat VS identity memo disagrees with the direct compare (entry n{}); the "
                       "old path from now on", candidate->number);
          identity_flat_ = false;
          return ComputeVertexIdentity(candidate);
        }
      }
      return cell.result;
    }
    auto& cached = vs_identity_cache_[candidate];
    if (cached.first != vs_identity_generation_) {
      cached.first = vs_identity_generation_;
      cached.second = VertexShaderIdentityMatches(
          {ShaderIdentityStage::Vertex, vs_microcode_},
          {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel,
           candidate->microcode}, candidate->elements,
          [](const auto& element) { return element.instruction; });
    }
    return cached.second;
  }

  // Once per (loaded program, candidate): every differing word between the loaded VS and the library candidate
  // (same size only), so an unresolved shader can be understood or added to the library without a debugger.
  void LogVertexWordDiff(const masseffect::native::ShaderEntry& candidate) {
    if (!candidate.vertices || vs_microcode_.empty() || vs_microcode_.size() != candidate.microcode.size() ||
        vs_diff_logged_.size() >= 40) return;
    const uint64_t key = current_vs_hash_ ^ (uint64_t(candidate.number) * 0x9E3779B97F4A7C15ull);
    if (!vs_diff_logged_.insert(key).second) return;
    std::string diffs;
    size_t count = 0;
    for (size_t w = 0; w < vs_microcode_.size(); ++w) {
      if (vs_microcode_[w] == candidate.microcode[w]) continue;
      if (++count <= 24)
        diffs += fmt::format(" [w{} i{}.{} {:08X}>{:08X}]", w, w / 3, w % 3, vs_microcode_[w], candidate.microcode[w]);
    }
    std::string declared;
    for (const auto& element : candidate.elements) declared += fmt::format(" {}", element.instruction);
    REXLOG_WARN("[native] VS word diff loaded_hash={:016X} words={} candidate n{} differing_words={} "
                "(loaded>library, first 24) declared_fetch_instr:{} diffs:{}",
                current_vs_hash_, vs_microcode_.size(), candidate.number, count, declared, diffs);
  }
  std::unordered_set<uint64_t> vs_diff_logged_;

  bool AcceptVertexShaderIdentity(const masseffect::native::ShaderEntry* candidate,
                                  const char* provenance) {
    if (!candidate) return false;
    if (MatchesVertexShaderIdentity(candidate)) return true;
    DumpLoadedVertexVariant(*candidate);
    LogVertexWordDiff(*candidate);
    const uint64_t mismatch = ++vs_identity_mismatches_;
    if (mismatch <= 32) {
      // Diagnostic only: mirror the predicate's rejection order without relaxing
      // any field. Loaded words are raw; the library words are already masked.
      constexpr uint32_t masks[3]{0x0007FFFFu, 0x80000FFFu, 0x80000000u};
      const char* reason = "unknown";
      size_t failed_word = SIZE_MAX;
      uint32_t failed_mask = UINT32_MAX;
      if (!candidate->vertices) reason = "wrong-stage";
      else if (vs_microcode_.empty()) reason = "empty-loaded";
      else if (vs_microcode_.size() != candidate->microcode.size()) reason = "word-count";
      else {
        bool failed = false;
        for (const auto& element : candidate->elements) {
          const size_t start = size_t(element.instruction) * 3;
          if (start + 3 > candidate->microcode.size()) {
            reason = "declared-fetch-out-of-bounds";
            failed_word = start;
            failed = true;
            break;
          }
          if ((candidate->microcode[start] & 0x1Fu) != 0u) {
            reason = "declared-fetch-opcode";
            failed_word = start;
            failed = true;
            break;
          }
          for (size_t lane = 0; lane < 3; ++lane) {
            if ((candidate->microcode[start + lane] & masks[lane]) !=
                candidate->microcode[start + lane]) {
              reason = "candidate-not-normalized";
              failed_word = start + lane;
              failed_mask = masks[lane];
              failed = true;
              break;
            }
          }
          if (failed) break;
          if (!VertexFetchSwizzleRepresentable(candidate->microcode[start + 1] & 0xFFFu,
                                               vs_microcode_[start + 1] & 0xFFFu)) {
            reason = "unrepresentable-fetch-swizzle";
            failed_word = start + 1;
            failed_mask = 0xFFFu;
            failed = true;
            break;
          }
        }
        if (!failed) {
          for (size_t word = 0; word < vs_microcode_.size(); ++word) {
            uint32_t mask = UINT32_MAX;
            for (const auto& element : candidate->elements) {
              const size_t start = size_t(element.instruction) * 3;
              if (word >= start && word - start < 3) {
                mask = word - start == 1 ? 0x80000000u : masks[word - start];
                break;
              }
            }
            if ((vs_microcode_[word] & mask) != (candidate->microcode[word] & mask)) {
              reason = mask == UINT32_MAX ? "non-fetch-word" : "masked-fetch-word";
              failed_word = word;
              failed_mask = mask;
              break;
            }
          }
        }
      }
      const uint32_t raw = failed_word < vs_microcode_.size() ? vs_microcode_[failed_word] : 0;
      const uint32_t selected = failed_word < candidate->microcode.size()
                                    ? candidate->microcode[failed_word] : 0;
      std::string declared;
      for (const auto& element : candidate->elements)
        declared += fmt::format(" {}", element.instruction);
      REXLOG_WARN("[native] VS identity detail mismatch={} reason={} word={} instruction={} lane={} "
                  "loaded={:08X} selected={:08X} mask={:08X} masked_loaded={:08X} declared_triples={}",
                  mismatch, reason, failed_word, failed_word / 3, failed_word % 3,
                  raw, selected, failed_mask, raw & failed_mask, declared);
    }
    if (mismatch <= 32 || (mismatch & 255) == 0) {
      const uint64_t selected_hash = XXH3_64bits(candidate->microcode.data(),
                                                candidate->microcode.size() * sizeof(uint32_t));
      REXLOG_WARN("[native] VS identity mismatch {} draw={} packet={:08X} "
                  "provenance={} loaded_host_hash={:016X} loaded_words={} selected_n={} "
                  "selected_stage={} selected_normalized_host_hash={:016X} selected_words={} "
                  "selected_container={:016X}",
                  mismatch, draws_, packet_address_, provenance,
                  current_vs_hash_, vs_microcode_.size(), candidate->number,
                  candidate->vertices ? "VS" : "PS", selected_hash, candidate->microcode.size(),
                  candidate->shader ? candidate->shader->fingerprint : 0);
    }
    return false;
  }

  const masseffect::native::ShaderEntry* PreferVertexShaderCandidate(
      const masseffect::native::ShaderEntry* current,
      const masseffect::native::ShaderEntry* candidate, const char* provenance) {
    // Keep diagnostics/variant discovery independent of the rollout policy.
    if (!candidate || !AcceptVertexShaderIdentity(candidate, provenance)) return current;
    return ShouldReplaceVertexCandidate(current != nullptr, MatchesVertexShaderIdentity(current),
                                        true, MatchesVertexShaderIdentity(candidate),
                                        false)
               ? candidate : current;
  }

  bool AcceptPixelShaderIdentity(const masseffect::native::ShaderEntry* candidate,
                                  const char* provenance) {
    if (!candidate) return false;
    const bool matches = MatchesPixelShaderIdentity(candidate);
    if (matches) return true;
    const uint64_t mismatch = ++ps_identity_mismatches_;
    if (mismatch <= 32 || (mismatch & 255) == 0) {
      const uint64_t selected_hash = XXH3_64bits(candidate->microcode.data(),
                                                candidate->microcode.size() * sizeof(uint32_t));
      REXLOG_WARN("[native] PS identity mismatch {} draw={} packet={:08X} "
                  "provenance={} loaded_host_hash={:016X} loaded_words={} selected_n={} "
                  "selected_stage={} selected_host_hash={:016X} selected_words={} "
                  "selected_container={:016X}",
                  mismatch, draws_, packet_address_, provenance,
                  current_ps_hash_, ps_microcode_.size(), candidate->number,
                  candidate->vertices ? "VS" : "PS", selected_hash, candidate->microcode.size(),
                  candidate->shader ? candidate->shader->fingerprint : 0);
    }
    return false;
  }

  // masseffect_native_raw_microcode: IdentifyShader's "same program reloaded from the same address" test on the raw
  // guest words. True = nothing to do (the stage keeps its entry, hashes and generations).
  bool ReloadsSameMicrocode(uint32_t type, const uint8_t* src, uint32_t size) {
    if (!REXCVAR_GET(masseffect_native_repeated_identity)) return false;
    const auto& raw = type == 1 ? raw_ps_ : raw_vs_;
    const auto* previous_entry = type == 1 ? ps_ : vs_;
    if (!previous_entry || !(type == 1 ? raw_ps_valid_ : raw_vs_valid_) || raw.size() != size ||
        (type == 1 ? last_ps_load_address_ : last_vs_load_address_) != im_load_address_ ||
        !SameProgram(raw.data(), src, size)) {
      return false;
    }
    if (check_raw_ > 0) {
      --check_raw_;
      const auto& swapped = type == 1 ? ps_microcode_ : vs_microcode_;
      bool equal = swapped.size() == size;
      for (uint32_t i = 0; equal && i < size; ++i)
        equal = swapped[i] == rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
      if (!equal) {
        ::me::native::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: raw microcode compare said 'same program' ({} words, type {}, address "
                     "{:08X}) but the swapped words differ; the old path from now on", size, type, im_load_address_);
        raw_microcode_ = false;
        return false;
      }
    }
    ++shader_loads_reused_;
    return true;
  }

  // banjo_ring_shader_load_memo. A slot of the memo: what one load address held for one stage.
  struct LoadMemo {
    uint32_t address = 0;  // of the IM_LOAD; 0 = unused
    uint32_t type = 0;
    std::vector<uint32_t> raw;      // the words as they are in guest memory (what a load is compared with)
    std::vector<uint32_t> swapped;  // the same in host order (the stage's microcode while this program is loaded)
    uint64_t hash = 0;              // XXH3 of `swapped` (current_vs_hash_ / current_ps_hash_)
    uint64_t identity_generation = 0;
    const masseffect::native::ShaderEntry* entry = nullptr;
    bool lent = false;  // its two vectors are in the stage right now (the slot holds the stage's old ones)
  };
  // 4096 slots: the game loads from about 1,100 addresses per stage, and with 1024 slots one load in twelve still
  // went the whole way in gameplay because two programs shared a slot (45,000 of 580,000 per 10 s on the PC).
  static constexpr size_t kLoadMemoSlots = 4096;
  static size_t LoadMemoSlot(uint32_t address, uint32_t type) {
    return (size_t(((address >> 2) * 0x9E3779B1u) >> 20) ^ (type * 0x555u)) & (kLoadMemoSlots - 1);
  }

  // The words of a held program against the load at `src` (equal sizes).
  static bool SameProgram(const uint32_t* held, const uint8_t* src, uint32_t size) {
    return std::memcmp(held, src, size_t(size) * 4) == 0;
  }

  // The stage gives back the vectors it took from a slot. Its own are then some older program's: nothing may take
  // them for the current one, so the "same as the last load" tests are disarmed until the next load sets them.
  void LoadMemoReturn(uint32_t type) {
    int32_t& lent = load_memo_lent_[type];
    if (lent < 0) return;
    LoadMemo& m = load_memo_[size_t(lent)];
    (type == 1 ? ps_microcode_ : vs_microcode_).swap(m.swapped);
    (type == 1 ? raw_ps_ : raw_vs_).swap(m.raw);
    m.lent = false;
    lent = -1;
    (type == 1 ? raw_ps_valid_ : raw_vs_valid_) = false;
    (type == 1 ? last_ps_load_address_ : last_vs_load_address_) = 0xFFFFFFFFu;  // (load addresses have bit 31 clear)
  }

  void LoadMemoClear() {
    if (load_memo_.empty()) return;
    LoadMemoReturn(0);
    LoadMemoReturn(1);
    for (LoadMemo& m : load_memo_) m.address = 0;
  }

  // True: the load at im_load_address_ is the program the memo holds for it; the stage is as IdentifyShader's
  // content-memo hit would have left it, without the copies and the hash.
  bool LoadMemoHit(uint32_t type, const uint8_t* src, uint32_t size) {
    if (load_memo_.empty() || load_memo_failed_ || !im_load_address_ || !::me::native::AbOn(12)) return false;
    const size_t index = LoadMemoSlot(im_load_address_, type);
    LoadMemo& m = load_memo_[index];
    // (A slot whose vectors the stage holds is the stage's current program: ReloadsSameMicrocode has just compared
    // it with this load and found other words.)
    if (m.address != im_load_address_ || m.type != type || m.lent || m.raw.size() != size ||
        !SameProgram(m.raw.data(), src, size)) {
      return false;
    }
    static const bool verify = REXCVAR_GET(banjo_cpu_verify);
    if (load_memo_checks_ > 0 || verify) {
      if (load_memo_checks_ > 0) --load_memo_checks_;
      ++load_memo_checked_;
      ::me::native::g_verify_native_checks.fetch_add(1, std::memory_order_relaxed);
      bool same = m.swapped.size() == size;
      for (uint32_t i = 0; same && i < size; ++i)
        same = m.swapped[i] == rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
      const uint64_t hash = same ? XXH3_64bits(m.swapped.data(), m.swapped.size() * sizeof(uint32_t)) : 0;
      const auto it = identity_memo_.find(hash ^ (uint64_t(type) << 63) ^ (uint64_t(size) << 40));
      if (!same || hash != m.hash || it == identity_memo_.end() || it->second != m.entry) {
        ::me::native::VerifyDifference();
        load_memo_failed_ = true;
        REXLOG_ERROR("[native] DIFFERENCE: shader load memo (banjo_ring_shader_load_memo): the program at {:08X} "
                     "({} words, type {}) is not what the content memo gives ({}); the old path from now on",
                     im_load_address_, size, type, !same ? "words" : hash != m.hash ? "hash" : "entry");
        LoadMemoClear();
        return false;
      }
    }
    LoadMemoReturn(type);
    (type == 1 ? ps_microcode_ : vs_microcode_).swap(m.swapped);
    (type == 1 ? raw_ps_ : raw_vs_).swap(m.raw);
    m.lent = true;
    load_memo_lent_[type] = int32_t(index);
    (type == 1 ? raw_ps_valid_ : raw_vs_valid_) = true;
    (type == 1 ? last_ps_load_address_ : last_vs_load_address_) = im_load_address_;
    if (type == 1) {
      ps_identity_generation_ = m.identity_generation;
      current_ps_hash_ = m.hash;
      ps_ = m.entry;
    } else {
      vs_identity_generation_ = m.identity_generation;
      current_vs_hash_ = m.hash;
      vs_ = m.entry;
      gen_vs_ = ++gen_microcode_;
    }
    ++shaders_identified_;
    ++identity_memo_hits_;
    ++load_memo_hits_;
    return true;
  }

  // After a load that went the whole way: the stage holds its program, identified. Only what the content memo keeps
  // too (an identified program), so a hit here is a hit there.
  void LoadMemoStore(uint32_t type) {
    if (load_memo_.empty() || load_memo_failed_ || !im_load_address_) return;
    const masseffect::native::ShaderEntry* const entry = type == 1 ? ps_ : vs_;
    if (!entry || !REXCVAR_GET(masseffect_native_identity_memo)) return;
    LoadMemo& m = load_memo_[LoadMemoSlot(im_load_address_, type)];
    if (m.lent) return;  // (the other stage holds this slot's vectors)
    m.address = im_load_address_;
    m.type = type;
    m.raw = type == 1 ? raw_ps_ : raw_vs_;
    m.swapped = type == 1 ? ps_microcode_ : vs_microcode_;
    m.hash = type == 1 ? current_ps_hash_ : current_vs_hash_;
    m.identity_generation = type == 1 ? ps_identity_generation_ : vs_identity_generation_;
    m.entry = entry;
    ++load_memo_stores_;
  }

  void IdentifyShader(uint32_t type) {
    if (type > 1) return;
    LoadMemoReturn(type);  // banjo_ring_shader_load_memo: the stage's vectors are about to be written
    // The same program reloaded from the same address (D3D re-sends it for most draws): keep the identified
    // entry, the hashes and the generations, instead of copying, hashing twice and matching again.
    if (REXCVAR_GET(masseffect_native_repeated_identity)) {
      const auto& previous = type == 1 ? ps_microcode_ : vs_microcode_;
      const auto* previous_entry = type == 1 ? ps_ : vs_;
      if (previous_entry && previous.size() == microcode_.size() &&
          (type == 1 ? last_ps_load_address_ : last_vs_load_address_) == im_load_address_ &&
          std::memcmp(previous.data(), microcode_.data(), microcode_.size() * sizeof(uint32_t)) == 0) {
        ++shader_loads_reused_;
        return;
      }
    }
    (type == 1 ? last_ps_load_address_ : last_vs_load_address_) = im_load_address_;
    // Memo by content (masseffect_native_identity_memo): the same program loaded again after others, from any
    // address, gets the entry identified before; the draw-time guard still checks it against this microcode.
    const uint64_t memo_hash = XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t));
    const uint64_t memo_key = memo_hash ^ (uint64_t(type) << 63) ^ (uint64_t(microcode_.size()) << 40);
    if (REXCVAR_GET(masseffect_native_identity_memo)) {
      const auto it = identity_memo_.find(memo_key);
      if (it != identity_memo_.end()) {
        if (type == 1) {
          ps_microcode_ = microcode_;
          ps_identity_generation_ = ++identity_generations_;
          current_ps_hash_ = memo_hash;
          ps_ = it->second;
        } else {
          vs_microcode_ = microcode_;
          current_vs_hash_ = memo_hash;
          vs_identity_generation_ = ++identity_generations_;
          vs_ = it->second;
          gen_vs_ = ++gen_microcode_;
        }
        ++shaders_identified_;
        ++identity_memo_hits_;
        return;
      }
    }
    if (type == 1) {
      ps_microcode_ = microcode_;
      ps_identity_generation_ = ++identity_generations_;
      current_ps_hash_ = memo_hash;
    } else {
      // Identity checks below must see THIS IM_LOAD, not the previous VS load.
      vs_microcode_ = microcode_;
      current_vs_hash_ = memo_hash;
      vs_identity_generation_ = ++identity_generations_;
    }
    // A VS address identifies the object, not necessarily the variant currently in the ring: D3D
    // rewrites vertex fetches for each declaration. Prefer the actual loaded microcode (exact, then
    // the library's masked-fetch matcher), and accept the address entry only if it is compatible.
    const masseffect::native::ShaderEntry* by_address =
        im_load_address_ ? ByAddress(im_load_address_) : nullptr;
    const masseffect::native::ShaderEntry* entry = nullptr;
    if (type == 0) {
      const masseffect::native::ShaderEntry* observed = nullptr;
      entry = ByCode(microcode_, true, &observed);
      if (observed && !AcceptVertexShaderIdentity(observed, "identify-code")) entry = nullptr;
      if (entry && (entry->microcode.size() != microcode_.size() ||
                    !masseffect::native::FetchCoherent(*entry, microcode_))) {
        entry = nullptr;
      }
      // Guard OFF is permission for a last-resort legacy fallback, not for a
      // stale fast/address candidate to hide a proven current program.
      if (!MatchesVertexShaderIdentity(entry) && shaders_.loaded()) {
        const auto* matched = shaders_.Identify(true, microcode_);
        entry = PreferVertexShaderCandidate(entry, matched, "identify-library");
      }
      if (by_address && (!entry || (!MatchesVertexShaderIdentity(entry) &&
                                   MatchesVertexShaderIdentity(by_address))) &&
          by_address->microcode.size() == microcode_.size() &&
          masseffect::native::FetchCoherent(*by_address, microcode_)) {
        entry = PreferVertexShaderCandidate(entry, by_address, "identify-address");
      }
      if (entry == by_address && entry) ++shaders_by_address_;
      else if (entry) ++shaders_by_code_;
    } else {
      if (by_address &&
          AcceptPixelShaderIdentity(by_address, "identify-address")) {
        entry = by_address;
        ++shaders_by_address_;
      }
      if (!entry) {
        const masseffect::native::ShaderEntry* observed = nullptr;
        const auto* code_entry = ByCode(microcode_, false, &observed);
        const bool accepted = observed && AcceptPixelShaderIdentity(observed, "identify-code");
        if (code_entry && accepted) {
          entry = code_entry;
          ++shaders_by_code_;
        }
      }
      if (!entry && shaders_.loaded()) {
        const auto* matched = shaders_.Identify(false, microcode_);
        if (matched && AcceptPixelShaderIdentity(matched, "identify-library")) entry = matched;
      }
    }
    (type == 0 ? vs_ : ps_) = entry;
    (type == 0 ? current_vs_hash_ : current_ps_hash_) = memo_hash;
    if (entry && REXCVAR_GET(masseffect_native_identity_memo)) {
      if (identity_memo_.size() > 8192) {
        identity_memo_.clear();
        LoadMemoClear();  // (a hit there has to be a hit here)
      }
      identity_memo_.emplace(memo_key, entry);
    }
    if (type == 0) {
      gen_vs_ = ++gen_microcode_;
    }
    ++(entry ? shaders_identified_ : shaders_unidentified_);
    if (!entry) {
      ++(im_load_address_ ? (type == 0 ? miss_vs_addr_ : miss_ps_addr_) : miss_immediate_);
      DumpRawMicrocode(type == 0, im_load_address_, microcode_);
      if (!im_load_address_ && shaders_.loaded()) {
        DumpMissingImmediate(shaders_, type == 0, microcode_);
      }
    }
    if (!entry && logged_misses_ < 12) {  // diagnostic: what the ring loads that the library lacks
      ++logged_misses_;
      std::string keys;
      {
        std::lock_guard<std::mutex> lock(g_by_address_mutex);
        for (const auto& [k, v] : g_by_address) {
          if ((k >> 12) == (im_load_address_ >> 12)) keys += fmt::format(" {:08X}", k);
        }
      }
      REXLOG_INFO("[native] unidentified {} IM_LOAD at {:08X} (keys in page:{}): {} words, {:08X} {:08X} {:08X} {:08X}",
                  type == 0 ? "VS" : "PS", im_load_address_, keys, microcode_.size(), microcode_.size() > 0 ? microcode_[0] : 0,
                  microcode_.size() > 1 ? microcode_[1] : 0, microcode_.size() > 2 ? microcode_[2] : 0,
                  microcode_.size() > 3 ? microcode_[3] : 0);
    }
  }

  // Opt-in discovery only, not permission to select a mismatching program. Keep
  // the SAME-size template's complete physical payload, including DEF literals;
  // changing only its instruction bytes makes offline translation auditable.
  void DumpLoadedVertexVariant(const masseffect::native::ShaderEntry& candidate) {
    static const char* const folder = std::getenv("MASSEFFECT_VERTEX_VARIANTS");
    if (!folder || !*folder || !candidate.vertices || !candidate.shader || vs_microcode_.empty() ||
        vs_microcode_.size() != candidate.microcode.size() || dumped_vertex_variants_.size() >= 64) return;
    const auto& original = candidate.shader->original;
    if (original.size() < 28 || original.size() > 8u * 1024 * 1024) return;
    const uint32_t flags = rex::memory::load_and_swap<uint32_t>(original.data());
    const uint32_t virtual_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + 4);
    const uint32_t physical_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + 8);
    const uint32_t header = rex::memory::load_and_swap<uint32_t>(original.data() + 24);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u || !(flags & 1u) || virtual_bytes > original.size() ||
        physical_bytes > original.size() - virtual_bytes || header > virtual_bytes ||
        virtual_bytes - header < 8) return;
    const uint32_t code_offset = rex::memory::load_and_swap<uint32_t>(original.data() + header);
    const uint32_t code_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + header + 4);
    if (code_bytes != vs_microcode_.size() * 4 || code_offset > physical_bytes ||
        code_bytes > physical_bytes - code_offset) return;
    const uint64_t loaded_hash = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * 4);
    const uint64_t key = XXH3_64bits_withSeed(original.data(), original.size(), loaded_hash);
    if (!dumped_vertex_variants_.insert(key).second) return;
    std::vector<uint8_t> container(original);
    for (size_t i = 0; i < vs_microcode_.size(); ++i)
      rex::memory::store_and_swap<uint32_t>(container.data() + virtual_bytes + code_offset + i * 4,
                                          vs_microcode_[i]);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) { REXLOG_WARN("[native] vertex variant discovery directory failed: {}", ec.message()); return; }
    const auto path = std::filesystem::path(folder) / fmt::format("vs_from_n{}_{:016x}.bin", candidate.number, key);
    const auto pending_path = std::filesystem::path(path.string() + ".pending");
    bool good = false;
    if (auto* file = std::fopen(pending_path.string().c_str(), "wbx")) {
      good = std::fwrite(container.data(), 1, container.size(), file) == container.size();
      if (std::fclose(file) != 0) good = false;
    }
    if (good) {
      std::filesystem::create_hard_link(pending_path, path, ec);
      good = !ec;
    }
    REXLOG_INFO("[native] vertex variant discovery written={} template_n={} loaded_host_hash={:016X} "
                "words={} (complete template retained, NOT verified compatible): {}",
                good, candidate.number, loaded_hash, vs_microcode_.size(), path.string());
  }

  // With MASSEFFECT_SHADER_MISSING=<folder>: the container of a Direct3D vertex shader as the ring loaded
  // it (rewritten for the vertex declaration), made of the object's container header and the ring's
  // microcode, once per microcode. shaders/tools/build_shader_spirv.sh adds it to the library.
  void DumpRewrittenVertexShader(const masseffect::native::ShaderEntry& object_entry) {
    static const char* const folder = std::getenv("MASSEFFECT_SHADER_MISSING");
    if (!folder || !*folder || !object_entry.shader || vs_microcode_.empty()) return;
    const uint64_t code = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    if (!dumped_rewritten_.insert(code).second) return;
    const auto& o = object_entry.shader->original;
    if (o.size() < 28) return;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
    if (vsize > o.size() || sh + 8 > vsize) return;
    const uint32_t bytes = uint32_t(vs_microcode_.size() * 4);
    std::vector<uint8_t> container(o.begin(), o.begin() + vsize);
    rex::memory::store_and_swap<uint32_t>(container.data() + 8, bytes);   // physical size
    rex::memory::store_and_swap<uint32_t>(container.data() + sh, 0);      // microcode offset
    rex::memory::store_and_swap<uint32_t>(container.data() + sh + 4, bytes);
    container.resize(size_t(vsize) + bytes);
    for (size_t i = 0; i < vs_microcode_.size(); ++i) {
      rex::memory::store_and_swap<uint32_t>(container.data() + vsize + i * 4, vs_microcode_[i]);
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto path = std::filesystem::path(folder) /
                      fmt::format("vs_{:016x}.bin", XXH3_64bits(container.data(), container.size()));
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fwrite(container.data(), 1, container.size(), f);
      std::fclose(f);
      REXLOG_INFO("[native] rewritten Direct3D vertex shader kept: {}", path.string());
    }
  }

  // The library entries of the VS and PS objects a record names (nullptr = unknown object).
  void LookupObjectEntries(uint32_t object_vs, uint32_t object_ps, const masseffect::native::ShaderEntry*& vs,
                           const masseffect::native::ShaderEntry*& ps) {
    vs = ps = nullptr;
    if (pairing_fast_ && LookupObject(object_vs, vs) && LookupObject(object_ps, ps)) {
      if (check_objects_ > 0) {
        --check_objects_;
        const masseffect::native::ShaderEntry* want_vs = nullptr;
        const masseffect::native::ShaderEntry* want_ps = nullptr;
        {
          std::lock_guard<std::mutex> lock(g_by_address_mutex);
          auto a = g_entry_of_object.find(object_vs);
          auto b = g_entry_of_object.find(object_ps);
          if (a != g_entry_of_object.end()) want_vs = a->second;
          if (b != g_entry_of_object.end()) want_ps = b->second;
        }
        if (want_vs != vs || want_ps != ps) {  // the game thread may have just added or replaced an entry: look again
          const masseffect::native::ShaderEntry* again_vs = nullptr;
          const masseffect::native::ShaderEntry* again_ps = nullptr;
          LookupObject(object_vs, again_vs);
          LookupObject(object_ps, again_ps);
          if (again_vs != want_vs || again_ps != want_ps) {
            ::me::native::VerifyDifference();
            REXLOG_ERROR("[native] DIFFERENCE: lock-free object table: VS {:08X} {} / {}, PS {:08X} {} / {} (table / "
                         "map); the old path from now on", object_vs, static_cast<const void*>(again_vs),
                         static_cast<const void*>(want_vs), object_ps, static_cast<const void*>(again_ps),
                         static_cast<const void*>(want_ps));
            pairing_fast_ = false;
          }
          vs = want_vs;
          ps = want_ps;
        }
      }
      return;
    }
    vs = ps = nullptr;
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    auto a = g_entry_of_object.find(object_vs);
    auto b = g_entry_of_object.find(object_ps);
    if (a != g_entry_of_object.end()) vs = a->second;
    if (b != g_entry_of_object.end()) ps = b->second;
  }

  // By packet address: the record whose command-buffer start is the closest one below this DRAW_INDX.
  bool PairByAddress(uint32_t type) {
    DrawRecord r;
    if (RecordTable()) {
      DrainRecords();  // the table belongs to the ring thread: no lock
      DrawRecordSlot* best = nullptr;
      if (pairing_fast_) {
        best = g_records.FindDense(packet_address_);
        if (check_pairing_ > 0) {
          --check_pairing_;
          DrawRecordSlot* old = g_records.FindScan(packet_address_);
          if (old != best) {
            ::me::native::VerifyDifference();
            REXLOG_ERROR("[native] DIFFERENCE: fast record lookup for packet {:08X} chose slot {} but the scan chose {}; "
                         "the old path from now on", packet_address_, best ? int64_t(g_records.IndexOf(best)) : -1,
                         old ? int64_t(g_records.IndexOf(old)) : -1);
            pairing_fast_ = false;
            g_record_fast = false;
            best = old;
          }
        }
      } else {
        best = g_records.FindScan(packet_address_);
      }
      // The map's rule: the nearest record at or below the packet, within 0x2000 and of the same type.
      if (!best || packet_address_ - best->start > 0x2000 || best->record.type != type) return false;
      r = std::move(best->record);
      g_records.Consume(best);
    } else {
      std::lock_guard<std::mutex> lock(g_draw_mutex);
      auto it = g_draw_by_address.upper_bound(packet_address_);
      if (it == g_draw_by_address.begin()) return false;
      --it;
      if (packet_address_ - it->first > 0x2000 || it->second.type != type) return false;
      r = it->second;
      g_draw_by_address.erase(it);
    }
    const masseffect::native::ShaderEntry* vs = nullptr;
    const masseffect::native::ShaderEntry* ps = nullptr;
    LookupObjectEntries(r.vs, r.ps, vs, ps);
    if (vs && !AcceptVertexShaderIdentity(vs, "pair-address-object")) vs = nullptr;
    if (vs && vs->binding_per_register) {
      // Direct3D rewrote this vertex shader for the vertex declaration: the variant the ring loaded is
      // its own library entry, found by microcode (vs_). Without it, keep a copy to extend the library.
      if (vs_ && vs_->binding_per_register) {
        vs = vs_;
      } else {
        DumpRewrittenVertexShader(*vs);
      }
    }
    // Audit the object even when the old register-linked override would hide a
    // stale entry. The guard protects every path, not just shader loading.
    if (ps && !AcceptPixelShaderIdentity(ps, "pair-address-object")) ps = nullptr;
    if (ps && ps->binding_per_register) {
      // Direct3D reuses its small internal PS objects and overwrites the same ring allocation with
      // different microcode. The object entry therefore describes only the first variant observed;
      // the IM_LOAD at this draw is authoritative, exactly as for rewritten register-linked VS.
      ps = ps_ && ps_->binding_per_register ? ps_ : nullptr;
    }
    // The IM_LOAD is the authoritative patched variant. A Draw* record may point at the
    // unpatched object, or at a different variant queued ahead of this indirect buffer.
    // Do not combine such an entry with the current ring microcode: ComputeEntry would
    // reject it with cause 20 (or derive the wrong vertex layout).
    if (vs && AcceptVertexShaderIdentity(vs, "pair-address-selected") &&
        vs->microcode.size() == vs_microcode_.size() &&
        FetchCoherent(vs)) {
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs, "pair-address-preference");
    }
    if (ps && AcceptPixelShaderIdentity(ps, "pair-address-selected")) draw_ps_ = ps;
    ++draws_paired_by_address_;
    if (!vs) {
      ++paired_no_vs_;
      if (missing_objects_.size() < 32 || missing_objects_.count(r.vs)) ++missing_objects_[r.vs];
    }
    if (!ps) {
      ++paired_no_ps_;
      if (missing_objects_.size() < 32 || missing_objects_.count(r.ps)) ++missing_objects_[r.ps];
    }
    return true;
  }

  // The first of up to 8 pending records with the same primitive type and
  // a matching count gives the VS and PS objects; otherwise the ring's own identification.
  void PairDraw() {
    draw_vs_ = vs_ && AcceptVertexShaderIdentity(vs_, "pair-initial-ring") ? vs_ : nullptr;
    draw_ps_ = ps_;
    if (g_no_pairing) {  // banjo_no_draw_pairing: the ring's own identification, as every pairing attempt ended
      if (!g_no_pairing_check) {
        ++draws_unpaired_;
        return;
      }
      const auto* vs = draw_vs_;
      const auto* ps = draw_ps_;
      PairDrawRecords();
      ++no_pairing_checks_;
      if (draw_vs_ != vs || draw_ps_ != ps) {
        ::me::native::VerifyDifference();
        if (no_pairing_differences_++ < 8)
          REXLOG_ERROR("[native] DIFFERENCE: the Draw* pairing chose other shaders than the ring's own identification "
                       "(packet {:08X}); the pairing's are used", packet_address_);
      }
      return;
    }
    PairDrawRecords();
  }

  // The Draw* record search (masseffect-nx): by command-buffer address, a tile replay of a paired packet, or the queue.
  void PairDrawRecords() {
    if (PairByAddress(Register(kRegVgtDrawInitiator) & 0x3F)) return;
    // Predicated tiling replays the same command buffer once per EDRAM tile: a DRAW_INDX packet at an
    // address already paired in this frame reuses that pairing and consumes no record.
    if (auto it = paired_packets_.find(packet_address_);
        it != paired_packets_.end() && it->second.frame == swaps_) {
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, it->second.vs, "pair-replay");
      if (it->second.ps && AcceptPixelShaderIdentity(it->second.ps, "pair-replay"))
        draw_ps_ = it->second.ps;
      ++draws_replayed_;
      return;
    }
    if (!pairing_fast_) {  // nothing ever fills g_draw_queue (records go through the lock-free queue)
      std::lock_guard<std::mutex> lock(g_draw_mutex);
      while (pending_.size() < 4096 && !g_draw_queue.empty()) {
        pending_.push_back(g_draw_queue.front());
        g_draw_queue.pop_front();
      }
    }
    const uint32_t initiator = Register(kRegVgtDrawInitiator);
    const uint32_t type = initiator & 0x3F, count = initiator >> 16;
    const size_t n = std::min<size_t>(pending_.size(), 64);
    for (size_t i = 0; i < n; ++i) {
      const DrawRecord& r = pending_[i];
      if (r.type != type) continue;
      if (r.r5 != count && r.r6 != count && r.r7 != count && CountForPrimitives(type, r.r7) != count &&
          CountForPrimitives(type, r.r6) != count) {
        continue;
      }
      const masseffect::native::ShaderEntry* vs = nullptr;
      const masseffect::native::ShaderEntry* ps = nullptr;
      LookupObjectEntries(r.vs, r.ps, vs, ps);
      pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(i + 1));
      ++draws_paired_;
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs, "pair-queue");
      if (ps && AcceptPixelShaderIdentity(ps, "pair-queue")) draw_ps_ = ps;
      paired_packets_[packet_address_] = {swaps_, draw_vs_, ps};
      return;
    }
    ++draws_unpaired_;
    if (logged_unpaired_ < 30 && draws_ > 60000 && type != 1) {
      ++logged_unpaired_;
      std::string p;
      for (size_t i = 0; i < pending_.size() && i < 4; ++i) {
        p += fmt::format(" [t{} {} {} {}]", pending_[i].type, pending_[i].r5, pending_[i].r6, pending_[i].r7);
      }
      REXLOG_INFO("[native] unpaired ring draw: type {} count {} source {}; pending {}:{}", type, count,
                  (initiator >> 6) & 3, pending_.size(), p);
    }
  }

  // A proven rectangle (CPU-executed VS) that overwrites every covered pixel of its views:
  // no discard, full sample mask, depth written with ALWAYS and stencil off, or color fully masked
  // without blending. The EDRAM tiles fully inside need no transfer from their previous owner
  // (xenia's "spurious ownership transfer round trips"; here the shadow maps and the scene share EDRAM).
  void ProveRectangleList(masseffect::native::SubmissionDraw& request, uint32_t rects) {
    if (!request.ps) return;
    // banjo-nx (from futari-nx): the estimator sees the register file with the draw as a 3-vertex list at each
    // rectangle's index offset. Those two registers are changed in place and put back on every exit (nothing else in
    // the proof reads them); copying the whole register file per rectangle list was a heap allocation and ~100 KB of
    // copying on the ring thread.
    static const bool in_place =
        banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_registers_in_place));
    std::vector<uint32_t> copy;
    if (!in_place) copy = registers_;  // as before: the whole register file copied
    std::vector<uint32_t>& regs = in_place ? registers_ : copy;
    const uint32_t initiator = regs[kRegVgtDrawInitiator];
    const uint32_t offset = regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET];
    struct Restore {
      std::vector<uint32_t>& regs;
      uint32_t initiator, offset;
      ~Restore() {
        regs[kRegVgtDrawInitiator] = initiator;
        regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET] = offset;
      }
    } restore{regs, initiator, offset};
    regs[kRegVgtDrawInitiator] = (initiator & 0xFFFFu) | (3u << 16);
    std::optional<std::array<int32_t, 4>> best, bounds;
    int64_t best_area = -1;
    uint32_t slots = 0;
    uint32_t max_y = 0;
    for (uint32_t k = 0; k < rects; ++k) {
      regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET] = offset + 3u * k;
      NativeDrawExtentEstimator::Diagnostics diagnostic;
      const auto estimate = draw_extent_estimator_->Estimate(regs, vs_microcode_, &diagnostic);
      if (!estimate) { ++multi_rect_rejects_[0]; return; }  // every rectangle must be proven
      max_y = std::max(max_y, *estimate);
      StencilClearOwnershipInput proof;
      proof.depth_control = Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
      proof.stencil_front = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK);
      proof.stencil_back = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
      proof.color_control = Register(rex::graphics::XE_GPU_REG_RB_COLORCONTROL);
      proof.su_sc_mode = Register(rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
      proof.vte_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL);
      const uint32_t tl = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
      const uint32_t br = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
      const uint32_t window_offset = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
      const auto sign15 = [](uint32_t v) { return int32_t(v & 0x7FFF) - int32_t((v & 0x4000) << 1); };
      const int32_t wx = (tl >> 31) ? 0 : sign15(window_offset);
      const int32_t wy = (tl >> 31) ? 0 : sign15(window_offset >> 16);
      const uint32_t screen_tl = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
      const uint32_t screen_br = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
      proof.scissor_x0 = uint32_t(std::max({0, int32_t(tl & 0x3FFF) + wx, sign15(screen_tl)}));
      proof.scissor_y0 = uint32_t(std::max({0, int32_t((tl >> 16) & 0x3FFF) + wy, sign15(screen_tl >> 16)}));
      proof.scissor_x1 = uint32_t(std::max(0, std::min(int32_t(br & 0x3FFF) + wx, sign15(screen_br))));
      proof.scissor_y1 = uint32_t(std::max(0, std::min(int32_t((br >> 16) & 0x3FFF) + wy, sign15(screen_br >> 16))));
      proof.post_vs_position = diagnostic.position;
      proof.xy_raster = diagnostic.xy_raster;
      proof.ps_no_discard = ProveStraightLinePixelNoKill(ps_microcode_) ||
          (REXCVAR_GET(masseffect_native_edram4_ps_no_kill) && ProvePixelNoKillAnywhere(ps_microcode_));
      proof.ps_no_depth_export = !(request.ps->outputs & 0x10);
      proof.sample_coverage_proven_full = (Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK) & 0xFFFFu) == 0xFFFFu;
      masseffect::native::SubmissionDraw one = request;
      one.edram_overwrite_rect.reset();
      ProveFullOverwrite(one, proof);
      if (!one.edram_overwrite_rect) { ++multi_rect_rejects_[1]; return; }
      if (k && one.edram_overwrite_slots != slots) { ++multi_rect_rejects_[2]; return; }
      request.edram_overwrite_rects[k] = *one.edram_overwrite_rect;
      slots = one.edram_overwrite_slots;
      const auto& r = *one.edram_overwrite_rect;
      const int64_t area = int64_t(r[2] - r[0]) * (r[3] - r[1]);
      if (area > best_area) { best_area = area; best = r; }
      const auto& b = one.edram_bounds_rect ? *one.edram_bounds_rect : r;
      if (!bounds) bounds = b;
      else bounds = std::array<int32_t, 4>{std::min((*bounds)[0], b[0]), std::min((*bounds)[1], b[1]),
                                           std::max((*bounds)[2], b[2]), std::max((*bounds)[3], b[3])};
    }
    // Pixels the coverage rectangles exclude (partial pixels at the edges) are not overwritten, but they are
    // covered by the bounds only if covered at all: the proven rects are exactly the covered pixel sets.
    request.edram_used_height_estimate = max_y;
    request.edram_overwrite_rect = best;
    request.edram_overwrite_slots = slots;
    request.edram_bounds_rect = bounds;
    request.edram_overwrite_rect_count = rects;
    ++multi_rect_proven_;
  }
  uint64_t multi_rect_proven_ = 0;
  bool rect_split_inside_ = false;  // banjo_gpu_rect_split
  uint64_t rect_splits_ = 0, rect_split_draws_ = 0;
  std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> identity_memo_;
  uint64_t identity_memo_hits_ = 0;
  uint64_t exact_edges_ = 0;
  uint32_t edges_traces_ = 0, alias_traces_ = 0, traces_depth_ = 0;
  std::array<uint64_t, 3> multi_rect_rejects_{};  // estimator, overwrite proof, slot mismatch

  // banjo_d3d_api_clear (src/me/banjo_d3d_clear.h): the state of the last Clear marker, and its counters.
  static constexpr uint32_t kApiClearBegin = 0x424A434Cu;  // 'BJCL' (banjo::d3d::clear::kBegin)
  static constexpr uint32_t kApiClearEnd = 0x424A4345u;    // 'BJCE'
  // banjo_d3d_api_resolve (step 2): the last Resolve marker.
  static constexpr uint32_t kApiResolveBegin = 0x424A5253u;  // 'BJRS' (banjo::d3d::clear::kResolveBegin)
  static constexpr uint32_t kApiResolveEnd = 0x424A5245u;    // 'BJRE'
  struct ApiResolve {
    bool armed = false;
    uint32_t whole_size = 0;  // width | height << 16 of the resolved surface, when the resolve covers it whole
    uint64_t markers = 0, given = 0, ab_off = 0;
  } api_resolve_;
  struct ApiClear {
    bool armed = false;
    std::array<int32_t, 4> rect{};  // the Clear's rectangle x1, y1, x2, y2 (surface pixels)
    uint32_t rt0_size = 0, depth_size = 0;  // width | height << 16 of the bound colour target 0 and depth surface
    uint64_t markers = 0, widened = 0, left_open = 0, dropped = 0, ab_off = 0, poisoned = 0;
    std::array<uint64_t, 7> declined{};
    std::unordered_set<uint64_t> traced;  // the kinds of Clear logged so far
  } api_clear_;

  // A rectangle of a Clear of the whole surface that reaches the surface's right or bottom edge also takes the rest of
  // its edge tiles: the pitch padding right of the surface, the rows below it to the end of its last tile row. Those
  // pixels belong to no surface position; Direct3D leaves the EDRAM's old content there, which nothing reads through
  // this surface, and every other surface on that EDRAM clears its own area before it uses it. Without the widening
  // those edge tiles are copied in from their previous owner on every Clear (renut_003 of 2026-10-14: the top ten
  // transfer pairs of the console's level, 23 ms of GPU time a frame at stock clocks).
  void WidenApiClear(masseffect::native::SubmissionDraw& request, const std::array<int32_t, 4>& rect, uint32_t slots) {
    static const bool key =
        banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_d3d_api_clear));
    if (!key) return;
    if (!::me::native::AbOn(20)) { ++api_clear_.ab_off; return; }
    namespace g = rex::graphics;
    const auto& c = api_clear_.rect;
    const uint32_t info = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    const int32_t pitch = int32_t(info & 0x3FFFu);
    // Each kind of Clear (decision, rectangle, sizes, pitch) is logged once, the first 64 kinds.
    const auto trace = [&](int reason) {
      if (api_clear_.traced.size() >= 64) return;
      const uint64_t kind = (uint64_t(uint32_t(reason + 1)) << 56) ^ (uint64_t(api_clear_.rt0_size) << 24) ^
                            (uint64_t(api_clear_.depth_size) << 8) ^ (uint64_t(uint32_t(c[2])) << 40) ^
                            (uint64_t(uint32_t(c[3])) << 20) ^ uint64_t(info) ^ (uint64_t(slots) << 60) ^
                            (uint64_t(rect[2] == c[2]) << 62);
      if (!api_clear_.traced.insert(kind).second) return;
      REXLOG_INFO("[native] Clear marker {}: clear {},{}-{},{} colour target 0 {}x{} depth {}x{} rect {},{}-{},{} "
                  "slots {:X} pitch {} msaa {}", reason < 0 ? std::string("taken") : fmt::format("declined ({})", reason),
                  c[0], c[1], c[2], c[3], api_clear_.rt0_size & 0xFFFF, api_clear_.rt0_size >> 16,
                  api_clear_.depth_size & 0xFFFF, api_clear_.depth_size >> 16, rect[0], rect[1], rect[2], rect[3],
                  slots, pitch, (info >> 16) & 3);
    };
    const auto no = [&](uint32_t reason) {
      ++api_clear_.declined[reason];
      trace(int(reason));
    };
    if ((Register(kRegVgtDrawInitiator) & 63u) != uint32_t(xenos::PrimitiveType::kRectangleList)) return no(0);
    if (Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET) != 0) return no(1);  // tiling's window offsets: surface != window
    if (slots & ~3u) return no(5);  // colour targets 1-3: their sizes are not in the marker
    // The surface of each view the rectangle overwrites (depth: the depth surface, colour: colour target 0) has to be
    // covered whole by the Clear (its rectangle may be larger: Direct3D takes colour target 0's size, and a shadow
    // map's depth surface can be smaller than the colour target bound with it), and depth and colour have to be the
    // same size. The rectangle is widened at that surface's edges.
    int32_t sw = 0, sh = 0;
    for (const uint32_t size : {(slots & 1u) ? api_clear_.depth_size : 0u, (slots & 2u) ? api_clear_.rt0_size : 0u}) {
      if (!size) continue;
      const int32_t w = int32_t(size & 0xFFFFu), h = int32_t(size >> 16);
      if (c[0] > 0 || c[1] > 0 || c[2] < w || c[3] < h) return no(2);  // not the whole surface
      if (sw && (sw != w || sh != h)) return no(4);
      sw = w;
      sh = h;
    }
    if (!sw) return no(4);  // a view without its surface in the marker
    if (rect[0] < 0 || rect[1] < 0 || rect[2] > sw || rect[3] > sh) return no(3);  // outside the surface
    const int32_t tile_rows = ((info >> 16) & 3u) ? 8 : 16;  // pixel rows of an EDRAM tile: 16 at 1x, 8 with MSAA
    if (sw > pitch) return no(6);
    std::array<int32_t, 4> w = rect;
    if (rect[2] == sw) w[2] = pitch;
    const int32_t rows = (sh + tile_rows - 1) / tile_rows * tile_rows;
    if (rect[3] == sh) w[3] = rows;
    if (w == rect) return;  // the surface's edges are on tile edges, or the rectangle does not reach them
    if (request.edram_stencil_replace_rect && *request.edram_stencil_replace_rect == rect)
      request.edram_stencil_replace_rect = w;
    request.edram_overwrite_rect = w;
    request.banjo_clear_own_surface = true;
    ++api_clear_.widened;
    trace(-1);
    if (REXCVAR_GET(banjo_diag_clear_padding) == 1) {  // the taken pixels, to be filled with a loud value
      request.banjo_padding_count = 0;
      if (w[2] > rect[2])
        request.banjo_padding_rects[request.banjo_padding_count++] = {rect[2], rect[1], w[2], w[3]};
      if (w[3] > rect[3])
        request.banjo_padding_rects[request.banjo_padding_count++] = {rect[0], rect[3], rect[2], w[3]};
      api_clear_.poisoned += request.banjo_padding_count;
    }
  }

  // banjo_diag_ps_alpha: the alpha test and two pixel constants of one pixel shader's draws, by kind.
  std::map<std::string, uint64_t> diag_ps_alpha_;
  void DiagPsAlpha() {
    namespace g = rex::graphics;
    const uint32_t cc = Register(g::XE_GPU_REG_RB_COLORCONTROL);
    float ref, c12[4], c50[4];
    const uint32_t ref_bits = Register(g::XE_GPU_REG_RB_ALPHA_REF);
    std::memcpy(&ref, &ref_bits, 4);
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t a = registers_[0x4400 + 12 * 4 + i], b = registers_[0x4400 + 50 * 4 + i];
      std::memcpy(&c12[i], &a, 4);
      std::memcpy(&c50[i], &b, 4);
    }
    const std::string kind = fmt::format("alpha test {} func {} ref {:.3f} | c12 {:.3f} {:.3f} {:.3f} {:.3f} | c50.x {:.3f}",
                                         (cc >> 3) & 1, cc & 7, ref, c12[0], c12[1], c12[2], c12[3], c50[0]);
    if (++diag_ps_alpha_[kind] == 1 && diag_ps_alpha_.size() <= 48)
      REXLOG_INFO("[native] banjo_diag_ps_alpha: new kind: {}", kind);
  }

  void ProveFullOverwrite(masseffect::native::SubmissionDraw& request, const StencilClearOwnershipInput& proof) {
    if (!REXCVAR_GET(masseffect_native_edram4_overwrite) || !request.ps) return;
    if (!proof.ps_no_discard) { ++overwrite_rejects_[0]; return; }
    if (!proof.sample_coverage_proven_full) { ++overwrite_rejects_[1]; return; }
    const auto& pos = proof.post_vs_position;
    float x0 = pos[0][0], x1 = pos[0][0], y0 = pos[0][1], y1 = pos[0][1];
    for (const auto& v : pos) {
      x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]);
      y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]);
    }
    // OpenGL pixel centers (PA_SU_VTX_CNTL.pix_center = 1, at i + 0.5): the same coverage as D3D centers
    // with every position half a pixel lower. The rest of the proof uses the D3D convention.
    if (REXCVAR_GET(masseffect_native_edram4_center_ogl) &&
        (Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL) & 1)) {
      x0 -= 0.5f; x1 -= 0.5f; y0 -= 0.5f; y1 -= 0.5f;
    }
    // D3D9 pixel space: pixel i spans [i - 0.5, i + 0.5), so [-0.5, W - 0.5] covers pixels 0..W-1.
    // A small epsilon rejects rectangles that stop short of a pixel by rounding.
    constexpr float kEps = 1.0f / 64.0f;
    std::array<int32_t, 4> rect{int32_t(std::ceil(x0 + 0.5f - kEps)), int32_t(std::ceil(y0 + 0.5f - kEps)),
                                int32_t(std::floor(x1 + 0.5f + kEps)), int32_t(std::floor(y1 + 0.5f + kEps))};
    rect[0] = std::max<int32_t>(rect[0], int32_t(proof.scissor_x0));
    rect[1] = std::max<int32_t>(rect[1], int32_t(proof.scissor_y0));
    rect[2] = std::min<int32_t>(rect[2], int32_t(proof.scissor_x1));
    rect[3] = std::min<int32_t>(rect[3], int32_t(proof.scissor_y1));
    if (rect[2] <= rect[0] || rect[3] <= rect[1]) { ++overwrite_rejects_[2]; return; }
    uint32_t slots = 0;
    const uint32_t dc = proof.depth_control;
    const bool z_enable = dc & 2, z_write = dc & 4, stencil = dc & 1;
    // Stencil is fully replaced when every sample passes (func ALWAYS; depth func ALWAYS so zfail never
    // happens), the pass op is REPLACE and the write mask is 0xFF, on both faces (D3D Clear rectangles).
    const auto stencil_replaced = [&](uint32_t func, uint32_t zpass, uint32_t refmask) {
      // REPLACE (2) writes the reference, ZERO (1) writes 0: either way every bit gets a known value.
      return func == 7 && (zpass == 2 || zpass == 1) && ((refmask >> 16) & 0xFFu) == 0xFFu;
    };
    // An inert stencil (test ALWAYS, every op KEEP or write mask 0, both faces) neither kills nor writes: the
    // draw is a depth-only draw for the overwrite proof (the depth restore pass, Z ALWAYS + stencil ALWAYS/KEEP).
    const auto inert = [&](uint32_t func, uint32_t ops, uint32_t refmask) {
      return func == 7 && (ops == 0 || ((refmask >> 16) & 0xFFu) == 0);
    };
    const bool stencil_inert = stencil && REXCVAR_GET(masseffect_native_edram4_stencil_inert) &&
        inert((dc >> 8) & 7, (dc >> 11) & 0x1FF, proof.stencil_front) &&
        (!(dc & 0x80) || inert((dc >> 20) & 7, (dc >> 23) & 0x1FF, proof.stencil_back));
    bool stencil_ok = !stencil || stencil_inert;
    if (stencil && !stencil_inert) {
      stencil_ok = stencil_replaced((dc >> 8) & 7, (dc >> 14) & 7, proof.stencil_front) &&
                   (!(dc & 0x80) || stencil_replaced((dc >> 20) & 7, (dc >> 26) & 7, proof.stencil_back));
    }
    // Alpha test or alpha-to-coverage can kill samples: then neither depth nor stencil is fully written.
    // banjo_gpu_proof_depth_only: not in the depth-only EDRAM mode (5). No pixel shader runs there, so there is no
    // alpha to test, and the renderer builds those draws without a fragment stage: every covered sample is written
    // whatever the two bits say. Banjo's loading screen clears depth that way (Direct3D's Clear of the depth buffer
    // alone, through its 4x view) with the alpha test left on from the draws before: the clear was not taken as an
    // overwrite, was drawn in the clear view's own representation, and the whole depth buffer went there and back
    // every frame (2,880 tiles, 16.2 ms of GPU time a Swap on the console, 2026-10-14).
    static const bool depth_only_key =
        banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_depth_only));
    const bool depth_only_mode = depth_only_key && ::me::native::AbOn(17) &&
                                 (Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 7) == 5;
    const bool alpha_bits = ((proof.color_control >> 3) & 1) || ((proof.color_control >> 4) & 1);
    const bool killable = !depth_only_mode && alpha_bits;
    if (depth_only_mode && alpha_bits) ++depth_only_proofs_;  // the rule decided this draw (report)
    if (!killable && stencil && stencil_ok && !stencil_inert && (!z_enable || ((dc >> 4) & 7) == 7))
      request.edram_stencil_replace_rect = rect;  // every sample reaches the stencil pass op
    if (!killable && z_enable && z_write && ((dc >> 4) & 7) == 7 && stencil_ok) slots |= 1;
    else if ((dc & 6) == 6) ++overwrite_rejects_[3];
    const uint32_t edram_mode = Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 7;
    const bool alpha_test = (proof.color_control >> 3) & 1;
    if (edram_mode == uint32_t(xenos::EdramMode::kColorDepth) && !alpha_test) {
      const uint32_t mask = Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK);
      static constexpr uint32_t kBlend[4] = {rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL1,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL2,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL3};
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(request.ps->outputs & (1u << i))) continue;
        if (((mask >> (i * 4)) & 15u) == 15u && (Register(kBlend[i]) & 0x1FFF1FFFu) == 0x00010001u)
          slots |= 2u << i;
      }
    }
    {
      // Diagnostic: a clear whose depth and color 0 share the EDRAM base (D3D Clear of both through the 4x alias).
      const uint32_t di = Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO), ci = Register(rex::graphics::XE_GPU_REG_RB_COLOR_INFO);
      if ((di & 0xFFF) == (ci & 0xFFF) && alias_traces_ < 12) {
        ++alias_traces_;
        REXLOG_INFO("[native] aliased depth/color clear proof: slots {:X} mode {} dc {:08X} colorctl {:08X} mask {:08X} "
                    "blend0 {:08X} ps outputs {:X} depthinfo {:08X} colorinfo {:08X} rect {},{}-{},{} killable {}",
                    slots, edram_mode, dc, proof.color_control, Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK),
                    Register(rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0), request.ps->outputs, di, ci,
                    rect[0], rect[1], rect[2], rect[3], killable);
      }
    }
    if (!slots) { ++overwrite_rejects_[4]; return; }
    ++overwrite_rejects_[5];
    // Area bound for mode 4: rounded outward and padded by a pixel, so it can only over-cover (the
    // overwrite rect above is rounded inward on purpose).
    // Edges exactly on pixel boundaries (k - 0.5 with D3D pixel centers) touch no sample of the neighbouring
    // pixel at 1x or 4x (samples at least 0.25 inside), so the inward rect is also the exact bound.
    const auto in_edge = [](float v) {
      const float b = v + 0.5f;
      return std::abs(b - std::round(b)) < 1.0f / 1024.0f;
    };
    const uint32_t vte_xy = proof.vte_cntl;
    const bool w_one = proof.xy_raster || (vte_xy & 0x100) ||
                       (pos[0][3] == 1.0f && pos[1][3] == 1.0f && pos[2][3] == 1.0f);
    const bool exact = REXCVAR_GET(masseffect_native_edram4_exact_edges) && w_one &&
        (!(Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL) & 1) || REXCVAR_GET(masseffect_native_edram4_center_ogl)) &&
        (!(proof.su_sc_mode & (1u << 16)) || !Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET)) &&
        in_edge(x0) && in_edge(x1) && in_edge(y0) && in_edge(y1);
    if (exact) ++exact_edges_;
    else if (REXCVAR_GET(masseffect_native_edram4_exact_edges) && edges_traces_ < 24) {
      ++edges_traces_;
      REXLOG_INFO("[native] edges not exact: x {}..{} y {}..{} w {} {} {} vte {:08X} vtx_cntl {:08X} su_sc {:08X} "
                  "rect {},{}-{},{} scissor {},{}-{},{}", x0, x1, y0, y1, pos[0][3], pos[1][3], pos[2][3], vte_xy,
                  Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL), proof.su_sc_mode, rect[0], rect[1], rect[2],
                  rect[3], proof.scissor_x0, proof.scissor_y0, proof.scissor_x1, proof.scissor_y1);
    }
    request.edram_bounds_rect = exact ? rect : std::array<int32_t, 4>{
        std::max<int32_t>(int32_t(std::floor(x0 + 0.5f)) - 1, int32_t(proof.scissor_x0)),
        std::max<int32_t>(int32_t(std::floor(y0 + 0.5f)) - 1, int32_t(proof.scissor_y0)),
        std::min<int32_t>(int32_t(std::ceil(x1 + 0.5f)) + 1, int32_t(proof.scissor_x1)),
        std::min<int32_t>(int32_t(std::ceil(y1 + 0.5f)) + 1, int32_t(proof.scissor_y1))};
    request.edram_overwrite_rect = rect;
    request.edram_overwrite_slots = slots;
    if (api_clear_.armed) WidenApiClear(request, rect, slots);
    // banjo_gpu_edram4_clear_color0 (sa2-nx): constant colour 0 (Direct3D's Clear): the pixel shader passes
    // interpolator 0 through and the three vertices export the same value bit for bit, so every covered pixel gets
    // that colour (to float rounding of the barycentric sum; the colour clear declines values near an 8-bit rounding
    // tie).
    if ((slots & 2) && request.ps->outputs == 1 && proof.ps_color0_is_r0 && proof.interpolator0_valid &&
        std::memcmp(&proof.post_vs_interpolator0[0], &proof.post_vs_interpolator0[1], sizeof(float) * 4) == 0 &&
        std::memcmp(&proof.post_vs_interpolator0[0], &proof.post_vs_interpolator0[2], sizeof(float) * 4) == 0)
      request.edram_overwrite_color0 = proof.post_vs_interpolator0[0];
    // Constant depth: the same post-viewport Z at all three corners, no polygon offset, no PS depth
    // export. Lets mode 4 apply the rectangle as a depth clear in the current owner's view.
    if ((slots & 1) && proof.ps_no_depth_export && !(proof.su_sc_mode & 0x3800u)) {
      const uint32_t vte = proof.vte_cntl;
      const float zs = (vte & 0x10) ? std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE))
                                    : 1.0f;
      const float zo = (vte & 0x20) ? std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET))
                                    : 0.0f;
      std::optional<float> z;
      bool constant = true;
      for (const auto& v : pos) {
        float zf = v[2];
        if (!(vte & 0x200)) zf = (vte & 0x400) ? zf / v[3] : zf * v[3];
        zf = zf * zs + zo;
        // A hair below 0 (-3.7e-9 in ME's clears) stores the all-zero depth field in both encodings (unorm24
        // clamps, float24 maps z <= 0 to 0).
        if (zf < 0.0f && zf > -1.0e-6f) zf = 0.0f;
        if (!std::isfinite(zf) || zf < 0.0f || zf > 1.0f || (z && *z != zf)) { constant = false; break; }
        z = zf;
      }
      if (constant && z) request.edram_overwrite_depth = z;
    }
    if ((slots & 1) && !request.edram_overwrite_depth && traces_depth_ < 16) {
      ++traces_depth_;
      REXLOG_INFO("[native] constant depth not proven: depthinfo {:08X} ps_no_depth_export {} su_sc {:08X} vte {:08X} "
                  "z {} {} {} w {} {} {} zscale {} zoffset {}", Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                  proof.ps_no_depth_export, proof.su_sc_mode, proof.vte_cntl, pos[0][2], pos[1][2], pos[2][2],
                  pos[0][3], pos[1][3], pos[2][3],
                  std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE)),
                  std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET)));
    }
  }

  std::array<uint64_t, 6> overwrite_rejects_{};  // discard, sample mask, empty rect, depth/stencil, no slot, ok
  uint64_t depth_only_proofs_ = 0;  // banjo_gpu_proof_depth_only: draws its rule let through
  std::string trace_proof_text_;  // frame trace: what the overwrite proof made of the draw in progress
  void TraceSwap() {
#if !REX_PLATFORM_SWITCH && !REX_PLATFORM_MAC
    const int32_t capture_at = REXCVAR_GET(banjo_renderdoc_capture_s);
    if (renderdoc_capturing_) {
      renderdoc_capturing_ = false;
      REXLOG_INFO("[native] RenderDoc capture of swap {} ended: {}", swaps_, banjo::renderdoc::End() ? "ok" : "FAILED");
    }
    if (capture_at > 0 && !renderdoc_triggered_ && Clock::now() - start_ >= std::chrono::seconds(capture_at)) {
      renderdoc_triggered_ = true;
      const auto folder = rex::filesystem::GetExecutableFolder() / "capturas" / "rdc";
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      if (banjo::renderdoc::Start((folder / "banjo").string().c_str())) {
        renderdoc_capturing_ = true;
        REXLOG_INFO("[native] RenderDoc capture started at swap {} ({})", swaps_, folder.string());
      } else {
        REXLOG_WARN("[native] RenderDoc capture requested but RenderDoc is not loaded");
      }
    }
#endif
    const int32_t at = REXCVAR_GET(banjo_trace_frame_s);
    if (trace_phase_ == 0 && at > 0 && Clock::now() - start_ >= std::chrono::seconds(at)) {
      trace_phase_ = 1;
      g_banjo_trace_edram = true;
      trace_pm4_ = REXCVAR_GET(banjo_trace_frame_pm4);
      REXLOG_INFO("[trace] ---- frame after swap {} ----", swaps_);
    } else if (trace_phase_ == 1) {
      trace_phase_ = 2;
      g_banjo_trace_edram = false;
      trace_pm4_ = false;
      REXLOG_INFO("[trace] ---- end at swap {}: {} draws ({} without resolve sampling not listed) ----", swaps_,
                  trace_draws_, trace_quiet_draws_);
    }
  }

  void TraceCopy() {
    namespace g = rex::graphics;
    const uint32_t surface = Register(g::XE_GPU_REG_RB_SURFACE_INFO), color = Register(g::XE_GPU_REG_RB_COLOR_INFO);
    const uint32_t depth = Register(g::XE_GPU_REG_RB_DEPTH_INFO), control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    const uint32_t pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH), info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    const uint32_t tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL), br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    REXLOG_INFO("[trace] #{} RESOLVE src {} (color base {} fmt {} / depth base {}) pitch {} msaa {} -> {:08X} {}x{} "
                "fmt {} | control {:08X} (clear c{} d{}) clear {:08X} | window {},{}-{},{} offset {:08X}",
                draws_, (control & 7) == 4 ? "depth" : "color", color & 0xFFF, (color >> 16) & 0xF, depth & 0xFFF,
                surface & 0x3FFF, (surface >> 16) & 3, Register(g::XE_GPU_REG_RB_COPY_DEST_BASE), pitch & 0x3FFF,
                (pitch >> 16) & 0x3FFF, (info >> 7) & 0x3F, control, (control >> 8) & 1, (control >> 9) & 1,
                Register(g::XE_GPU_REG_RB_COLOR_CLEAR), tl & 0x3FFF, (tl >> 16) & 0x3FFF, br & 0x3FFF,
                (br >> 16) & 0x3FFF, Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET));
  }

  void TraceDraw() {
    namespace g = rex::graphics;
    ++trace_draws_;
    const uint32_t target[4] = {Register(g::XE_GPU_REG_RB_SURFACE_INFO), Register(g::XE_GPU_REG_RB_COLOR_INFO),
                                Register(g::XE_GPU_REG_RB_DEPTH_INFO), Register(kRegRbModeControl)};
    if (std::memcmp(target, trace_target_, sizeof(target))) {
      std::memcpy(trace_target_, target, sizeof(target));
      REXLOG_INFO("[trace] #{} TARGET pitch {} msaa {} color base {} fmt {} depth base {} fmt {} mode {}", draws_,
                  target[0] & 0x3FFF, (target[0] >> 16) & 3, target[1] & 0xFFF, (target[1] >> 16) & 0xF,
                  target[2] & 0xFFF, (target[2] >> 16) & 1, target[3] & 7);
    }
    std::string textures;
    uint32_t used = 0;  // texture slots the pixel shader samples (all slots when it is unknown)
    if (draw_ps_)
      for (const auto& sampler : draw_ps_->samplers) used |= 1u << (sampler.register_value & 31);
    else
      used = ~0u;
    bool any_resolved = false;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!(used >> slot & 1)) continue;
      const uint32_t* f = &registers_[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6];
      if ((f[0] & 3) != 2) continue;
      const uint32_t base = f[1] & 0x1FFFF000u;
      any_resolved |= resolve_destinations_.count(base) != 0;
      textures += fmt::format(" t{}={:08X} {}x{} fmt {} pitch {} tiled {}", slot, base, (f[2] & 0x1FFF) + 1,
                              ((f[2] >> 13) & 0x1FFF) + 1, f[1] & 0x3F, ((f[0] >> 22) & 0x1FF) << 5,
                              (f[0] >> 31) & 1);
      static const bool all_draws = REXCVAR_GET(banjo_trace_frame_all);
      if (all_draws && memory_) {
        // The mip range the fetch constant asks for, and whether the first kilobyte of each address is all zero
        // (a level that has not been streamed in yet).
        const uint32_t mips = f[5] & 0x1FFFF000u;
        const auto zero = [&](uint32_t address) {
          if (!address || address + 1024 > 0x20000000u) return 2;
          const uint8_t* bytes = memory_->TranslatePhysical(address);
          for (uint32_t i = 0; i < 1024; ++i)
            if (bytes[i]) return 0;
          return 1;
        };
        static const char* const kZero[3] = {"", " ZERO", " none"};
        textures += fmt::format(" [mips {:08X}{} base{} levels {}-{} bias {} filters {}/{}/{}]", mips, kZero[zero(mips)],
                                kZero[zero(base)], (f[4] >> 2) & 0xF, (f[4] >> 6) & 0xF,
                                (int32_t(f[4] << 10) >> 22) / 32.0f, (f[3] >> 19) & 0x3, (f[3] >> 21) & 0x3,
                                (f[3] >> 23) & 0x3);
      }
    }
    const uint32_t tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL), br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    static const bool all = REXCVAR_GET(banjo_trace_frame_all);
    if (all) {
      REXLOG_INFO("[trace] #{} DRAW {} VS n{} PS n{} prim {} count {} src {} scissor {},{}-{},{} offset {:08X} dc {:08X} "
                  "mask {:08X} blend0 {:08X} colorctl {:08X} clip {:08X} bin {:08X}/{:08X} | {}{}", draws_,
                  trace_result_ == 1 ? "ok" : trace_result_ == 0 ? fmt::format("REJ{}", g_banjo_last_reject_cause)
                                                                  : std::string("SKIP"),
                  draw_vs_ ? int64_t(draw_vs_->number) : -1, draw_ps_ ? int64_t(draw_ps_->number) : -1,
                  Register(kRegVgtDrawInitiator) & 63, Register(kRegVgtDrawInitiator) >> 16,
                  (Register(kRegVgtDrawInitiator) >> 6) & 3, tl & 0x3FFF, (tl >> 16) & 0x3FFF, br & 0x3FFF,
                  (br >> 16) & 0x3FFF, Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET),
                  Register(g::XE_GPU_REG_RB_DEPTHCONTROL), Register(g::XE_GPU_REG_RB_COLOR_MASK),
                  Register(g::XE_GPU_REG_RB_BLENDCONTROL0), Register(g::XE_GPU_REG_RB_COLORCONTROL),
                  Register(g::XE_GPU_REG_PA_CL_CLIP_CNTL), uint32_t(bin_select_), uint32_t(bin_mask_),
                  trace_proof_text_, textures);
      return;
    }
    // Every draw into the final 1280-wide targets is listed; others only when they sample a resolve destination.
    if (!any_resolved && (target[0] & 0x3FFF) != 1280) {
      ++trace_quiet_draws_;
      return;
    }
    REXLOG_INFO("[trace] #{} DRAW {} VS n{} PS n{} prim {} count {} scissor {},{}-{},{}:{}", draws_,
                trace_result_ == 1 ? "ok" : trace_result_ == 0 ? fmt::format("REJ{}", g_banjo_last_reject_cause)
                                                                : std::string("SKIP"),
                draw_vs_ ? int64_t(draw_vs_->number) : -1, draw_ps_ ? int64_t(draw_ps_->number) : -1,
                Register(kRegVgtDrawInitiator) & 63, Register(kRegVgtDrawInitiator) >> 16, tl & 0x3FFF,
                (tl >> 16) & 0x3FFF, br & 0x3FFF, (br >> 16) & 0x3FFF, textures);
  }

  void Draw() {
    // banjo_d3d_api_clear: only rectangle lists belong to a Clear; anything else after a begin marker means the end
    // marker was not written (no room), and the marker is dropped.
    if (api_clear_.armed && !rect_split_inside_ &&
        (registers_[kRegVgtDrawInitiator] & 63u) != uint32_t(xenos::PrimitiveType::kRectangleList)) {
      api_clear_.armed = false;
      ++api_clear_.dropped;
    }
    // banjo_gpu_rect_split: a rectangle list of several rectangles, one draw per rectangle.
    static const bool rect_split_key =
        banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_rect_split));
    if (rect_split_key && !rect_split_inside_ && !dead_.inside) {
      const uint32_t initiator = registers_[kRegVgtDrawInitiator];
      const uint32_t vertices = initiator >> 16;
      if ((initiator & 63u) == uint32_t(xenos::PrimitiveType::kRectangleList) &&
          ((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kAutoIndex) && vertices > 3u &&
          vertices % 3u == 0 && vertices <= 24u && ::me::native::AbOn(15)) {
        const uint32_t offset = registers_[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET];
        rect_split_inside_ = true;
        registers_[kRegVgtDrawInitiator] = (initiator & 0xFFFFu) | (3u << 16);
        if (t_decoder) dirty_.Mark(kRegVgtDrawInitiator);
        for (uint32_t k = 0; k < vertices / 3u; ++k) {
          registers_[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET] = offset + 3u * k;
          if (t_decoder) dirty_.Mark(rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET);
          Draw();
        }
        rect_split_inside_ = false;
        registers_[kRegVgtDrawInitiator] = initiator;
        registers_[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET] = offset;
        if (t_decoder) {
          dirty_.Mark(kRegVgtDrawInitiator);
          dirty_.Mark(rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET);
        }
        ++rect_splits_;
        rect_split_draws_ += vertices / 3u;
        return;
      }
    }
    // banjo_gpu_dead_raster: a draw of a learned site runs with its target writes off (the two registers are put
    // back when it returns); one of a site still being learned is drawn as it is and only noted.
    bool dead_candidate = false;
    if (dead_.key && !dead_.inside) {
      const int kind = DeadCandidate();
      if (kind == 2) {
        namespace g = rex::graphics;
        const uint32_t mask = registers_[g::XE_GPU_REG_RB_COLOR_MASK], dc = registers_[g::XE_GPU_REG_RB_DEPTHCONTROL];
        registers_[g::XE_GPU_REG_RB_COLOR_MASK] = 0;
        registers_[g::XE_GPU_REG_RB_DEPTHCONTROL] = dc & ~0x6u;
        if (t_decoder) {
          dirty_.Mark(g::XE_GPU_REG_RB_COLOR_MASK);
          dirty_.Mark(g::XE_GPU_REG_RB_DEPTHCONTROL);
        }
        dead_.inside = true;
        Draw();
        dead_.inside = false;
        registers_[g::XE_GPU_REG_RB_COLOR_MASK] = mask;
        registers_[g::XE_GPU_REG_RB_DEPTHCONTROL] = dc;
        if (t_decoder) {
          dirty_.Mark(g::XE_GPU_REG_RB_COLOR_MASK);
          dirty_.Mark(g::XE_GPU_REG_RB_DEPTHCONTROL);
        }
        return;
      }
      dead_candidate = kind == 1;
    }
    // banjo_ring_identity_once: the entry's identity, asked once. When it holds, none of the two blocks below can
    // change draw_vs_ (the first needs it false, the second's acceptance is this same question), so they are skipped
    // and the answer serves the rest of the draw; otherwise everything runs as before and it is asked again after.
    static const bool identity_once =
        banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_identity_once));
    bool vs_proven = identity_once && MatchesVertexShaderIdentity(draw_vs_);
    if (!vs_proven) {
      if (!MatchesVertexShaderIdentity(draw_vs_) && MatchesVertexShaderIdentity(vs_))
        draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs_, "draw-final-ring-preference");
      if (draw_vs_ && !AcceptVertexShaderIdentity(draw_vs_, "draw-final")) {
        // No unchecked fallback: the last loaded variant is the only authority.
        draw_vs_ = vs_ && AcceptVertexShaderIdentity(vs_, "draw-final-ring") ? vs_ : nullptr;
        if (!draw_vs_) { ++draws_unidentified_; return; }
      }
      vs_proven = MatchesVertexShaderIdentity(draw_vs_);
    }
    if (!EnsureTargets()) return;
    // Candidate warnings include discarded object/queue alternatives. Count
    // the actual final selection separately, without changing rollout policy.
    if (!draw_vs_) {
      ++vs_final_missing_;
    } else if (vs_proven) {
      ++vs_final_proven_;
    } else {
      const uint64_t unproven = ++vs_final_unproven_;
      if (unproven <= 8 || (unproven & 255) == 0) {
        REXLOG_WARN("[native] FINAL VS unproven {} draw={} loaded_host_hash={:016X} "
                    "loaded_words={} selected_n={} selected_container={:016X}",
                    unproven, draws_, current_vs_hash_, vs_microcode_.size(),
                    draw_vs_->number, draw_vs_->shader ? draw_vs_->shader->fingerprint : 0);
      }
    }
    masseffect::native::SubmissionDraw request;
    request.registers = registers_.data();
    request.vs = draw_vs_;
    request.ps = draw_ps_;
    {  // banjo_diag_ps_alpha
      static const int32_t diag_ps = REXCVAR_GET(banjo_diag_ps_alpha);
      if (diag_ps >= 0 && draw_ps_ && int32_t(draw_ps_->number) == diag_ps) DiagPsAlpha();
    }
    request.vs_microcode = vs_microcode_;
    request.vs_program_hash = current_vs_hash_;  // banjo_ring_vs_hash_given (the XXH3 of vs_microcode_ at its load)
    request.ps_microcode = ps_microcode_;
    request.generation_vs = gen_vs_;
    request.generation_constants_vs = gen_constants_vs_;
    request.generation_constants_ps = gen_constants_ps_;
    request.generation_fetch = gen_fetch_;
    request.generation_framing = gen_viewport_;
    const bool trace_proof = g_banjo_trace_edram;
    const std::array<uint64_t, 6> trace_rejects = overwrite_rejects_;
    const std::array<uint64_t, 3> trace_multi = multi_rect_rejects_;
    if (trace_proof) [[unlikely]] {
      trace_proof_text_ = !memory_ ? "no memory"
          : !((Register(rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL) & 0x10000u) ||
              REXCVAR_GET(masseffect_native_edram4_clip_inside)) ? "clipping on"
          : !vs_proven ? "VS identity not proven" : "primitive not a candidate";
    }
    if (memory_ &&
        ((Register(rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL) & 0x10000u) ||
         REXCVAR_GET(masseffect_native_edram4_clip_inside)) &&
        vs_proven) {
      const uint32_t initiator = Register(kRegVgtDrawInitiator);
      static const bool triangle_key =
          banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_triangle));
      const bool triangle_proof = triangle_key && ::me::native::AbOn(7);
      // The adapter performs the full admission proof. This outer filter avoids
      // copying/analyzing unrelated normal draws when the opt-in is enabled.
      const uint32_t rect_vertices = initiator >> 16;
      if ((initiator & 63u) == uint32_t(xenos::PrimitiveType::kRectangleList) &&
          ((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kAutoIndex) &&
          rect_vertices > 3u && rect_vertices % 3u == 0 && rect_vertices <= 24u &&
          REXCVAR_GET(masseffect_native_edram4_several_rect)) {
        // Rect lists of several rectangles (D3D Clear of two regions): prove each one with the same
        // single-rectangle estimator on a register copy whose index offset selects that rectangle.
        if (!draw_extent_estimator_) { draw_extent_estimator_ = std::make_unique<NativeDrawExtentEstimator>(*memory_); draw_extent_estimator_->AllowClipInside(REXCVAR_GET(masseffect_native_edram4_clip_inside)); draw_extent_estimator_->AllowQuadTriangles(REXCVAR_GET(masseffect_native_edram4_quad_triangles)); draw_extent_estimator_->AllowAlu(REXCVAR_GET(masseffect_native_edram4_vs_alu)); draw_extent_estimator_->ReasonAsPointer(banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_estimator_reason))); draw_extent_estimator_->TessellationOnlyExplicit(banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_tess_leftover))); draw_extent_estimator_->AllowTriangle(banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_triangle))); }
        ProveRectangleList(request, rect_vertices / 3u);
        if (trace_proof) [[unlikely]] trace_proof_text_ = "rectangle list of several";
      } else if ((((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kAutoIndex) ||
                  (((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kDMA) &&
                   (initiator & 63u) != uint32_t(xenos::PrimitiveType::kRectangleList))) &&
          (((initiator & 63u) == uint32_t(xenos::PrimitiveType::kRectangleList) && (initiator >> 16) == 3u) ||
           (triangle_proof && (initiator & 63u) == uint32_t(xenos::PrimitiveType::kTriangleList) &&
            (initiator >> 16) == 3u && ((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kAutoIndex)) ||
           (REXCVAR_GET(masseffect_native_edram4_quad_triangles) &&
            (((initiator & 63u) == uint32_t(xenos::PrimitiveType::kTriangleList) && (initiator >> 16) == 6u) ||
             ((initiator & 63u) == uint32_t(xenos::PrimitiveType::kTriangleStrip) && (initiator >> 16) == 4u))))) {
        if (!draw_extent_estimator_) { draw_extent_estimator_ = std::make_unique<NativeDrawExtentEstimator>(*memory_); draw_extent_estimator_->AllowClipInside(REXCVAR_GET(masseffect_native_edram4_clip_inside)); draw_extent_estimator_->AllowQuadTriangles(REXCVAR_GET(masseffect_native_edram4_quad_triangles)); draw_extent_estimator_->AllowAlu(REXCVAR_GET(masseffect_native_edram4_vs_alu)); draw_extent_estimator_->ReasonAsPointer(banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_ring_estimator_reason))); draw_extent_estimator_->TessellationOnlyExplicit(banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_tess_leftover))); draw_extent_estimator_->AllowTriangle(banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_proof_triangle))); }
        NativeDrawExtentEstimator::Diagnostics diagnostic;
        request.edram_used_height_estimate = draw_extent_estimator_->Estimate(registers_, vs_microcode_, &diagnostic);
        if (trace_proof) [[unlikely]]
          trace_proof_text_ = request.edram_used_height_estimate
              ? "extent proven" : fmt::format("extent not proven: {}", diagnostic.Reason());
        if (request.edram_used_height_estimate && request.ps) {
          StencilClearOwnershipInput proof;
          proof.depth_control = Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
          proof.stencil_front = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK);
          proof.stencil_back = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
          proof.color_control = Register(rex::graphics::XE_GPU_REG_RB_COLORCONTROL);
          proof.su_sc_mode = Register(rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
          proof.vtx_cntl = Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL);
          proof.vte_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL);
          proof.window_offset = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
          proof.clip_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL);
          const uint32_t surface = Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
          proof.msaa_enum = (surface >> 16) & 3;
          proof.physical_pitch_tiles = (((surface & 0x3FFF) << uint32_t(proof.msaa_enum == 2)) + 79) / 80;
          const uint32_t tl = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
          const uint32_t br = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
          const auto sign15 = [](uint32_t v) { return int32_t(v & 0x7FFF) - int32_t((v & 0x4000) << 1); };
          const int32_t wx = (tl >> 31) ? 0 : sign15(proof.window_offset);
          const int32_t wy = (tl >> 31) ? 0 : sign15(proof.window_offset >> 16);
          const uint32_t screen_tl = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
          const uint32_t screen_br = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
          proof.scissor_x0 = uint32_t(std::max({0, int32_t(tl & 0x3FFF) + wx, sign15(screen_tl)}));
          proof.scissor_y0 = uint32_t(std::max({0, int32_t((tl >> 16) & 0x3FFF) + wy, sign15(screen_tl >> 16)}));
          proof.scissor_x1 = uint32_t(std::max(0, std::min(int32_t(br & 0x3FFF) + wx, sign15(screen_br))));
          proof.scissor_y1 = uint32_t(std::max(0, std::min(int32_t((br >> 16) & 0x3FFF) + wy, sign15(screen_br >> 16))));
          proof.post_vs_position = diagnostic.position;
      proof.xy_raster = diagnostic.xy_raster;
          // banjo_gpu_edram4_clear_color0 (sa2-nx): interpolator 0 of the three vertices and whether the pixel shader
          // is exactly oC0 = r0 (Direct3D's Clear): together they give the clear's colour.
          proof.post_vs_interpolator0 = diagnostic.interpolator0;
          proof.interpolator0_valid = diagnostic.interpolator0_valid;
          proof.ps_color0_is_r0 = ProvePixelColor0IsR0(ps_microcode_);
          proof.rectangle_proven = true;  // Successful bounded adapter proof, not bbox-only.
          proof.ps_identity_proven = MatchesPixelShaderIdentity(request.ps);
          // Counting SPIR-V OpKill is not proof: an optimizer may merge guest
          // and alpha-test kills. Inspect every admitted loaded ALU instead.
          proof.ps_no_discard = ProveStraightLinePixelNoKill(ps_microcode_) ||
          (REXCVAR_GET(masseffect_native_edram4_ps_no_kill) && ProvePixelNoKillAnywhere(ps_microcode_));
          proof.ps_no_depth_export = !(request.ps->outputs & 0x10);
          // Admit only the all-enabled 16-bit AA mask, never an unknown mask.
          // The native pipeline has no additional programmable sample mask.
          proof.sample_coverage_proven_full = (Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK) & 0xFFFFu) == 0xFFFFu;
          request.edram_stencil_clear = ProveStencilClearOwnership(proof);
          ProveFullOverwrite(request, proof);
          if ((proof.depth_control & 7) == 1 && stencil_clear_diagnostics_++ < 16)
            REXLOG_INFO("[native] canonical stencil clear candidate: VS={} PS={} eligible={} reason={} "
                        "aaMask={:08X} physical={}x{} tiles={} ref={}",
                        request.vs->number, request.ps->number, request.edram_stencil_clear.eligible,
                        request.edram_stencil_clear.reason, Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK),
                        request.edram_stencil_clear.physical_width, request.edram_stencil_clear.physical_height,
                        request.edram_stencil_clear.length_tiles, request.edram_stencil_clear.stencil_reference);
        }
        if (extent_diagnostics_.size() < 32) {
          // banjo-nx (from futari-nx): the same fields hashed instead of formatted into a string on every draw.
          const uint64_t fields[4] = {current_vs_hash_, Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                                      Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO),
                                      request.edram_used_height_estimate.value_or(UINT32_MAX)};
          const std::string_view why = diagnostic.Reason();
          const uint64_t key = XXH3_64bits_withSeed(why.data(), why.size(), XXH3_64bits(fields, sizeof(fields)));
          if (extent_diagnostics_.insert(key).second)
            REXLOG_INFO("[native] bounded draw extent: code={:016X} VS={} depth={:08X} surface={:08X} "
                        "accepted={} maxY={} reason={} postVS=[{},{},{},{}; {},{},{},{}; {},{},{},{}]",
                        current_vs_hash_, request.vs->number,
                        Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                        Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO),
                        bool(request.edram_used_height_estimate),
                        request.edram_used_height_estimate.value_or(UINT32_MAX), diagnostic.Reason(),
                        diagnostic.position[0][0], diagnostic.position[0][1], diagnostic.position[0][2], diagnostic.position[0][3],
                        diagnostic.position[1][0], diagnostic.position[1][1], diagnostic.position[1][2], diagnostic.position[1][3],
                        diagnostic.position[2][0], diagnostic.position[2][1], diagnostic.position[2][2], diagnostic.position[2][3]);
        }
      }
    }
    if (trace_proof) [[unlikely]] {
      static const char* const kRejects[6] = {"the pixel shader may discard", "sample mask not full", "empty rectangle",
                                              "depth written but not ALWAYS / stencil not replaced",
                                              "writes neither all of depth nor all of a colour", "proven"};
      for (size_t i = 0; i < 6; ++i)
        if (overwrite_rejects_[i] != trace_rejects[i]) trace_proof_text_ += fmt::format("; overwrite: {}", kRejects[i]);
      static const char* const kMulti[3] = {"a rectangle's extent not proven", "a rectangle not an overwrite",
                                            "rectangles write different views"};
      for (size_t i = 0; i < 3; ++i)
        if (multi_rect_rejects_[i] != trace_multi[i]) trace_proof_text_ += fmt::format("; {}", kMulti[i]);
      if (request.edram_overwrite_rect)
        trace_proof_text_ += fmt::format("; rect {},{}-{},{} slots {:X}{}{}{}", (*request.edram_overwrite_rect)[0],
                                         (*request.edram_overwrite_rect)[1], (*request.edram_overwrite_rect)[2],
                                         (*request.edram_overwrite_rect)[3], request.edram_overwrite_slots,
                                         request.edram_overwrite_depth
                                             ? fmt::format(" depth {}", *request.edram_overwrite_depth) : std::string(),
                                         request.edram_overwrite_color0 ? " colour constant" : "",
                                         request.edram_stencil_replace_rect ? " stencil replaced" : "");
      for (uint32_t k = 0; k < request.edram_overwrite_rect_count && k < request.edram_overwrite_rects.size(); ++k)
        trace_proof_text_ += fmt::format(" [{},{}-{},{}]", request.edram_overwrite_rects[k][0],
                                         request.edram_overwrite_rects[k][1], request.edram_overwrite_rects[k][2],
                                         request.edram_overwrite_rects[k][3]);
      if (request.edram_bounds_rect)
        trace_proof_text_ += fmt::format(" bounds {},{}-{},{}", (*request.edram_bounds_rect)[0],
                                         (*request.edram_bounds_rect)[1], (*request.edram_bounds_rect)[2],
                                         (*request.edram_bounds_rect)[3]);
      request.edram_proof_note = trace_proof_text_.c_str();
    }
    if (!dead_.sites.empty() && !dead_.inside && !dead_candidate) DeadDecide(request);  // banjo_gpu_dead_raster
    if (tiles_.pass && !tiles_.failed && !TilesPrepareDraw(request)) return;  // banjo_gpu_tiles_once
    static const int32_t slow_us = REXCVAR_GET(banjo_diag_ring_slow_us);
    if (slow_us) [[unlikely]] {
      const auto until = Clock::now() + std::chrono::microseconds(slow_us);
      while (Clock::now() < until) {
      }
    }
    ++native_draw_attempts_;
    if (t_decoder) {  // banjo_ring_decoder: the executor draws it
      SplitDraw(request);
      return;
    }
    g_banjo_last_reject_cause = 0;
    const bool drawn = targets_->Draw(request);
    if (tiles_.second_pending) [[unlikely]] {  // banjo_gpu_tiles_once: the same draw, into the second tile's rows
      tiles_.second_pending = false;
      request.tiles_once = 2;
      std::copy(tiles_.second_window, tiles_.second_window + 3, request.tiles_window);
      targets_->Draw(request);
      ++tiles_.draws_other;
    }
    trace_result_ = drawn ? 1 : 0;
    if (!drawn) {
      if (++native_draw_failures_ <= 8)
        REXLOG_WARN("[native] draw rejected before completion; visual conformance is not established");
    }
  }

  // --- banjo_gpu_dead_raster -------------------------------------------------------------------------------------
  struct DeadRaster {
    bool key = false, inside = false, failed = false;
    std::vector<uint64_t> sites;    // the candidates since the last deciding draw, by site
    std::bitset<2048> tiles;        // every EDRAM tile one of them could draw color to (all 32 bits of its words)
    std::bitset<2048> tiles_depth;  // every tile one of them could write depth to (the 24 depth bits; no stencil)
    uint32_t without = 0;           // how many of them ran with their target writes off
    std::unordered_map<uint64_t, uint8_t> confidence;  // per site: times in a row it was overwritten whole
    uint64_t noted = 0, spared = 0, confirmed = 0, tiles_spared = 0, undecided = 0;
    uint64_t report[3] = {};
  } dead_;
  static constexpr uint8_t kDeadConfidence = 4;

  // The EDRAM tiles of rows [0, bottom) of a surface, from the tile at `base`.
  static void DeadTiles(std::bitset<2048>& out, uint32_t surface, uint32_t base, uint32_t bottom) {
    const me::native::EdramLayout layout((surface >> 16) & 0x3, false);
    const uint32_t pitch = layout.pitch_tiles(surface & 0x3FFF);
    const uint32_t rows = (bottom + layout.tile_height() - 1) / layout.tile_height();
    for (uint32_t i = 0; i < pitch * rows && i < 2048; ++i) out.set((base + i) & 2047u);
  }

  // The draw in the registers: 0 = not a candidate, 1 = a candidate whose site is still being learned (drawn as it
  // is), 2 = one of a learned site (to run with its target writes off).
  int DeadCandidate() {
    namespace g = rex::graphics;
    DeadRaster& d = dead_;
    if (d.failed || tiles_.pass || uint32_t(bin_select_) != 0xFFFFFFFFu) return 0;
    const uint32_t initiator = Register(kRegVgtDrawInitiator);
    if ((initiator & 63u) != uint32_t(xenos::PrimitiveType::kPointList) ||
        ((initiator >> 6) & 3u) != uint32_t(xenos::SourceSelect::kAutoIndex)) {  // (any number of points)
      return 0;
    }
    const uint32_t dc = Register(g::XE_GPU_REG_RB_DEPTHCONTROL);
    const uint32_t surface = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t color = Register(g::XE_GPU_REG_RB_COLOR_INFO);
    const uint32_t depth = Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t mask = Register(g::XE_GPU_REG_RB_COLOR_MASK);
    // What it draws has to be what DeadDecide can account for: depth and color 0 of a 32-bit surface in the color +
    // depth mode, no stencil, no other color target, rows bounded by a window scissor with no offset.
    if ((Register(kRegRbModeControl) & 0x7) != uint32_t(xenos::EdramMode::kColorDepth) || (dc & 0x1) ||
        !TilesDrawWrites() || (mask & 0xFFF0u) || !draw_vs_ || ((color >> 16) & 0xF) > 1 ||
        (Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET) & 0x7FFF7FFFu)) {
      return 0;
    }
    const uint32_t bottom = std::min<uint32_t>((Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR) >> 16) & 0x3FFF, 2048);
    if (!bottom) return 0;
    const uint64_t fields[6] = {(uint64_t(draw_vs_->number) << 32) | (draw_ps_ ? draw_ps_->number : 0xFFFFFFFFu),
                                surface, color, depth, dc, mask};
    const uint64_t site = XXH3_64bits(fields, sizeof(fields));
    if ((dc & 0x6) == 0x6) DeadTiles(d.tiles_depth, surface, depth & 0xFFF, bottom);
    if (mask & 0xFu) DeadTiles(d.tiles, surface, color & 0xFFF, bottom);
    d.sites.push_back(site);
    ++d.noted;
    const auto it = d.confidence.find(site);
    if (it == d.confidence.end() || it->second < kDeadConfidence || !::me::native::AbOn(14)) return 1;
    ++d.without;
    ++d.spared;
    return 2;
  }

  // Something that is not the overwrite the candidates were waiting for. Their sites start learning again; if one of
  // them ran with its target writes off, that frame lacks what it would have drawn.
  void DeadUndecided(const char* what) {
    DeadRaster& d = dead_;
    if (d.sites.empty()) return;
    for (const uint64_t site : d.sites) d.confidence[site] = 0;
    if (d.without && !d.failed) {
      ::me::native::VerifyDifference();
      d.failed = true;
      REXLOG_ERROR("[native] DIFFERENCE: dead raster (banjo_gpu_dead_raster): {} point draws ran with their target "
                   "writes off and were followed by {} instead of an overwrite of all their tiles (swap {}, draw {}). "
                   "Off for the rest of the session; this frame lacks those points in the target", d.without, what,
                   swaps_, draws_);
    }
    ++d.undecided;
    d.sites.clear();
    d.tiles.reset();
    d.tiles_depth.reset();
    d.without = 0;
  }

  // The draw about to be made, with candidates waiting: one that touches no render target changes nothing; a proven
  // overwrite of every tile they could draw to, in every bit, confirms them; anything else does not.
  void DeadDecide(const masseffect::native::SubmissionDraw& request) {
    namespace g = rex::graphics;
    DeadRaster& d = dead_;
    const uint32_t dc = Register(g::XE_GPU_REG_RB_DEPTHCONTROL);
    const uint32_t mode = Register(kRegRbModeControl) & 0x7;
    const bool target_mode = mode == uint32_t(xenos::EdramMode::kColorDepth) || mode == 5;
    if (!TilesDrawWrites() && !(dc & 0x2) && target_mode) return;
    const uint32_t surface = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t color = Register(g::XE_GPU_REG_RB_COLOR_INFO);
    // `written`: tiles whose words the draw overwrites in every bit; `written_depth`: tiles whose 24 depth bits it
    // overwrites (enough for what a candidate did to depth: none of them writes stencil).
    std::bitset<2048> written, written_depth;
    if (request.edram_overwrite_rect && target_mode) {
      const auto& o = *request.edram_overwrite_rect;
      const me::native::EdramLayout layout((surface >> 16) & 0x3, false);
      const uint32_t pitch = layout.pitch_tiles(surface & 0x3FFF), tw = layout.tile_width(), th = layout.tile_height();
      // Whole tiles of the rectangle only, and only an aspect whose every bit the draw writes: depth with the
      // stencil replaced over the same rectangle, color 0 with all four channels.
      const bool depth_bits = (request.edram_overwrite_slots & 1) != 0;
      const bool depth_word = depth_bits && request.edram_stencil_replace_rect &&
                              *request.edram_stencil_replace_rect == o;
      const bool color_word = (request.edram_overwrite_slots & 2) && mode == uint32_t(xenos::EdramMode::kColorDepth) &&
                              (Register(g::XE_GPU_REG_RB_COLOR_MASK) & 0xFu) == 0xFu && ((color >> 16) & 0xF) <= 1;
      if (o[0] >= 0 && o[1] >= 0 && o[2] > o[0] && o[3] > o[1]) {
        for (uint32_t ty = (uint32_t(o[1]) + th - 1) / th; ty < uint32_t(o[3]) / th; ++ty) {
          for (uint32_t tx = (uint32_t(o[0]) + tw - 1) / tw; tx < uint32_t(o[2]) / tw && tx < pitch; ++tx) {
            const uint32_t local = ty * pitch + tx;
            if (local >= 2048) break;
            const uint32_t depth_tile = ((Register(g::XE_GPU_REG_RB_DEPTH_INFO) & 0xFFF) + local) & 2047u;
            if (depth_word) written.set(depth_tile);
            if (depth_bits) written_depth.set(depth_tile);
            if (color_word) written.set(((color & 0xFFF) + local) & 2047u);
          }
        }
      }
    }
    const size_t left = (d.tiles & ~written).count(), left_depth = (d.tiles_depth & ~(written | written_depth)).count();
    if (left || left_depth || (d.tiles.none() && d.tiles_depth.none())) {
      if (d.without && !d.failed)
        REXLOG_INFO("[native] dead raster: the draw that followed: VS n{} PS n{} initiator {:08X} surface {:08X} color "
                    "{:08X} depth {:08X} dc {:08X} mask {:08X} mode {}; proven rect {} slots {:X} stencil replaced {}; "
                    "{} color tiles and {} depth tiles of the candidates not overwritten (of {} and {})",
                    draw_vs_ ? int(draw_vs_->number) : -1, draw_ps_ ? int(draw_ps_->number) : -1,
                    Register(kRegVgtDrawInitiator), surface, color, Register(g::XE_GPU_REG_RB_DEPTH_INFO), dc,
                    Register(g::XE_GPU_REG_RB_COLOR_MASK), mode,
                    request.edram_overwrite_rect
                        ? fmt::format("{},{}-{},{}", (*request.edram_overwrite_rect)[0], (*request.edram_overwrite_rect)[1],
                                      (*request.edram_overwrite_rect)[2], (*request.edram_overwrite_rect)[3])
                        : std::string("none"),
                    request.edram_overwrite_slots, bool(request.edram_stencil_replace_rect), left, left_depth,
                    d.tiles.count(), d.tiles_depth.count());
      DeadUndecided("a draw that does not overwrite all their tiles");
      return;
    }
    for (const uint64_t site : d.sites) {
      uint8_t& c = d.confidence[site];
      if (c < 255) ++c;
    }
    if (d.without) {
      d.confirmed += d.without;
      d.tiles_spared += d.tiles.count() + d.tiles_depth.count();
    }
    d.sites.clear();
    d.tiles.reset();
    d.tiles_depth.reset();
    d.without = 0;
  }

  void Copy() {
    if (t_decoder) {  // banjo_ring_decoder: the copy and its answers where the executor is
      SplitCall([this] { Copy(); });
      return;
    }
    DeadUndecided("a resolve");  // banjo_gpu_dead_raster
    if (!EnsureTargets()) return;
    namespace g = rex::graphics;
    masseffect::native::RegistersCopy r;
    r.rb_surface_info = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    r.rb_color_info[0] = Register(g::XE_GPU_REG_RB_COLOR_INFO);
    r.rb_color_info[1] = Register(g::XE_GPU_REG_RB_COLOR1_INFO);
    r.rb_color_info[2] = Register(g::XE_GPU_REG_RB_COLOR2_INFO);
    r.rb_color_info[3] = Register(g::XE_GPU_REG_RB_COLOR3_INFO);
    r.rb_depth_info = Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    r.rb_copy_control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    r.rb_copy_dest_base = Register(g::XE_GPU_REG_RB_COPY_DEST_BASE);
    r.rb_copy_dest_pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    r.rb_copy_dest_info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    r.rb_color_clear = Register(g::XE_GPU_REG_RB_COLOR_CLEAR);
    r.rb_color_clear_lo = Register(g::XE_GPU_REG_RB_COLOR_CLEAR_LO);
    r.rb_depth_clear = Register(g::XE_GPU_REG_RB_DEPTH_CLEAR);
    r.pa_sc_window_offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    r.pa_sc_window_scissor_tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    r.pa_sc_window_scissor_br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    r.pa_su_sc_mode_cntl = Register(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    r.pa_su_vtx_cntl = Register(g::XE_GPU_REG_PA_SU_VTX_CNTL);
    r.fetch_vertices[0] = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    r.fetch_vertices[1] = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1);
    // banjo_d3d_api_resolve: inside a Resolve marker of the whole surface, its size goes with the copy (its clear takes
    // the surface's edge tiles whole).
    if (api_resolve_.armed && api_resolve_.whole_size) {
      static const bool key =
          banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_d3d_api_resolve));
      if (key && ::me::native::AbOn(21)) {
        r.banjo_surface_size = api_resolve_.whole_size;
        ++api_resolve_.given;
      } else if (key) {
        ++api_resolve_.ab_off;
      }
    }
    // banjo_gpu_tiles_once.
    TilesOnce& t = tiles_;
    if (t.pass == 1 && t.other_context) {  // in a buffer of the second tile alone: not something this handles
      TilesBack("a resolve in a buffer of the second tile alone");
      t.ready = false;
      return;
    }
    if (t.pass == 2 && t.skip) {
      // The copy was made by the first pass, to this address; the clear, if there is one, is still made.
      const uint32_t base = r.rb_copy_dest_base & 0x1FFFFFFFu;
      if (t.resolves_compared >= t.resolved.size() || t.resolved[t.resolves_compared] != base)
        TilesFail(fmt::format("resolve {} of the second pass goes to {:08X}, not where the first pass copied for it "
                              "({} copies made: {:08X}; the second tile's destination as last set under its mask "
                              "{:08X})",
                              t.resolves_compared, base, t.resolved.size(),
                              t.resolves_compared < t.resolved.size() ? t.resolved[t.resolves_compared] : 0u,
                              t.other_dest));
      else
        ++t.resolves_compared;
      if (!(r.rb_copy_control & 0x300u)) return;
      r.rb_copy_control |= 3u << 20;  // CopyCommand::kNull
    }
    ++native_copy_attempts_;
    if (!targets_->Copy(r)) {
      if (++native_copy_failures_ <= 8)
        REXLOG_WARN("[native] copy/clear rejected before completion; visual conformance is not established");
    }
    // (Found on 2026-10-13 at a scene change: a first pass that begins with a resolve. Whether the scene is drawn once
    // was only decided at its first drawing draw, so that resolve got no copy for the second tile, the scene then
    // turned wide and the second pass found its first resolve unaccounted for. The decision is made here too.)
    if (t.pass == 1 && t.key && !t.failed) TilesDecide();
    if (t.pass == 1 && t.key && !t.failed && t.other_valid) {
      // The rows the second tile's resolve copies (its window when this resolve is set up), and its whole surface:
      // the first tile's, moved down by the split.
      const int32_t rect[4] = {int32_t(t.other_window[1] & 0x3FFF), int32_t((t.other_window[1] >> 16) & 0x3FFF),
                               int32_t(t.other_window[2] & 0x3FFF), int32_t((t.other_window[2] >> 16) & 0x3FFF)};
      const int32_t width = int32_t(r.pa_sc_window_scissor_br & 0x3FFF);
      const int32_t height = int32_t((r.pa_sc_window_scissor_br >> 16) & 0x3FFF);
      const int32_t spare[4] = {0, height, width, 2 * height};
      if (t.wide) {
        uint32_t base = 0;
        if (!targets_->CopyTilesOnce(r, rect, &base))
          TilesBack("a resolve with no counterpart for the second tile's rows");
        else if (t.other_dest && (t.other_dest & 0x1FFFFFFFu) != base) {
          if (t.backs < 12)
            REXLOG_INFO("[native] tiles once: the copy went to {:08X}; the second tile's RB_COPY_DEST_BASE is {:08X}, "
                        "the first's {:08X}; rows {}..{}, control {:08X}", base, t.other_dest, r.rb_copy_dest_base,
                        rect[1], rect[3], r.rb_copy_control);
          TilesBack("the copy for the second tile's rows did not go where its resolve is told to");
        } else {
          t.resolved.push_back(base);
        }
      }
      if ((r.rb_copy_control & 0x300u) == 0x300u && (r.rb_copy_control & 0x7) == 0) {
        // The clear that leaves the tile as the second pass finds it: the spare rows get it too (they are the
        // second tile's rows of the next scene drawn once), and it has to be the one they got last time.
        const int cleared = targets_->ClearTilesOnce(r, spare);
        if (t.wide && cleared != 2) TilesBack("the clear after the first pass is not the last scene's");
        t.spare_cleared = cleared != 0;
        t.clean_end = cleared != 0;
        t.spare_top = cleared ? spare[1] : 0;
        t.spare_bottom = cleared ? spare[3] : 0;
      } else if (r.rb_copy_control & 0x300u) {
        TilesBack("a clear in the first pass that the second tile's rows cannot mirror");
        t.clean_end = false;
      }
    }
  }

  void Present() {
    if (!presenter_ || !EnsureTargets()) return;
    if (gamma_sent_version_ != gamma_version_) {
      gamma_sent_version_ = gamma_version_;
      targets_->RampGamma(gamma_ramp_);
      if (!gamma_logged_) {
        gamma_logged_ = true;
        REXLOG_INFO("[native] game gamma ramp: {} writes, [0]={}/{}/{}, [64]={}/{}/{}, "
                    "[128]={}/{}/{}, [255]={}/{}/{}",
                    gamma_writes_, gamma_ramp_[0][0], gamma_ramp_[0][1], gamma_ramp_[0][2],
                    gamma_ramp_[64][0], gamma_ramp_[64][1], gamma_ramp_[64][2],
                    gamma_ramp_[128][0], gamma_ramp_[128][1], gamma_ramp_[128][2],
                    gamma_ramp_[255][0], gamma_ramp_[255][1], gamma_ramp_[255][2]);
      }
    }
    masseffect::native::TextureSwap texture;
    for (uint32_t i = 0; i < 6; ++i) {
      texture.dword[i] = Register(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i);
    }
    if (targets_->Present(presenter_.get(), texture, swap_width_, swap_height_)) {
      ++presented_;
    }
  }

  void Report(bool force) {
    const auto now = Clock::now();
    if (!force && now - last_report_ < std::chrono::seconds(10)) return;
    const double secs = std::chrono::duration<double>(now - last_report_).count();
    last_report_ = now;
    // GPU time per category, real milliseconds per Swap of this interval: raw NVK timestamps x 1.627 on the Switch.
    if (targets_) {
      std::array<uint64_t, masseffect::native::kGpuCategories> gpu{}, frag{}, vert{}, prim{};
      targets_->TimeGpuPerCategory(gpu);
      targets_->StatsPipeline(frag, vert, prim);
      constexpr double kScale = 1.627;
      const uint64_t swaps = swaps_ - report_gpu_swaps_;
      if (swaps) {
        static constexpr const char* kNames[masseffect::native::kGpuCategories] = {
            "other",  "shadows",    "scene",        "640",         "320",         "smaller",     "copies",
            "clears", "scene_no_z", "gap",         "edram_import", "edram_export", "edram_alias", "edram_import9"};
        std::string t;
        double total = 0;
        for (uint32_t i = 0; i < masseffect::native::kGpuCategories; ++i) {
          const double ms = double(gpu[i] - report_gpu_ns_[i]) * kScale / 1e6 / double(swaps);
          total += ms;
          t += fmt::format(" {} {:.1f} ms ({:.2f} Mfrag)", kNames[i], ms,
                           double(frag[i] - report_gpu_frag_[i]) / 1e6 / double(swaps));
        }
        REXLOG_INFO("[native] GPU per Swap ({} Swaps): total {:.1f} ms;{}", swaps, total, t);
        masseffect::native::deferred::ReportCalls(swaps);  // banjo_diag_vk_calls (off: nothing)
        REXLOG_INFO("[native] overwrite proof: discard {} samplemask {} emptyrect {} depthstencil {} noslot {} ok {}; "
                    "depth-only mode with alpha bits set (banjo_gpu_proof_depth_only) {}",
                    overwrite_rejects_[0], overwrite_rejects_[1], overwrite_rejects_[2], overwrite_rejects_[3],
                    overwrite_rejects_[4], overwrite_rejects_[5], depth_only_proofs_);
        overwrite_rejects_ = {};
        depth_only_proofs_ = 0;
      }
      report_gpu_ns_ = gpu;
      report_gpu_frag_ = frag;
      report_gpu_swaps_ = swaps_;
    }
    if (dead_.key && (dead_.noted || dead_.failed)) {
      REXLOG_INFO("[native] dead raster (banjo_gpu_dead_raster {}), since the last report: {} candidate point draws, {} "
                  "run with their target writes off, {} of those confirmed by the overwrite that followed ({} tiles "
                  "not needed); {} groups followed by something else",
                  dead_.failed ? "OFF after a difference" : "on", dead_.noted - dead_.report[0],
                  dead_.spared - dead_.report[1], dead_.confirmed - dead_.report[2], dead_.tiles_spared,
                  dead_.undecided);
      dead_.report[0] = dead_.noted;
      dead_.report[1] = dead_.spared;
      dead_.report[2] = dead_.confirmed;
      dead_.tiles_spared = 0;
      dead_.undecided = 0;
    }
    if (!load_memo_.empty() || load_memo_failed_) {
      REXLOG_INFO("[native] shader load memo (banjo_ring_shader_load_memo {}), since the last report: {} loads taken "
                  "from the memo by address, {} stored after going the whole way; {} checked against the content memo "
                  "(cumulative)", load_memo_failed_ ? "OFF after a difference" : "on",
                  load_memo_hits_ - load_memo_report_[0], load_memo_stores_ - load_memo_report_[1], load_memo_checked_);
      load_memo_report_[0] = load_memo_hits_;
      load_memo_report_[1] = load_memo_stores_;
    }
    if (!diag_ps_alpha_.empty()) {
      std::string t;
      for (const auto& [k, n] : diag_ps_alpha_) t += fmt::format(" [{}x {}]", n, k);
      REXLOG_INFO("[native] banjo_diag_ps_alpha (cumulative):{}", t.substr(0, 3000));
    }
    if (api_resolve_.markers) {
      REXLOG_INFO("[native] Direct3D Resolve markers (banjo_d3d_api_resolve), since the last report: {} markers, {} "
                  "copies given their whole surface's size, {} in A/B off halves; resolve clears widened over their "
                  "edge tiles' padding (cumulative): {}", api_resolve_.markers, api_resolve_.given, api_resolve_.ab_off,
                  targets_ ? targets_->ResolveClearsWidened() : 0);
      api_resolve_.markers = api_resolve_.given = api_resolve_.ab_off = 0;
    }
    if (api_clear_.markers) {
      const auto& d = api_clear_.declined;
      REXLOG_INFO("[native] Direct3D Clear markers (banjo_d3d_api_clear), since the last report: {} markers, {} "
                  "rectangles widened over their edge tiles' padding; declined: {} not a rectangle list, {} window "
                  "offset, {} a view's surface not cleared whole, {} outside the surface, {} depth and colour of "
                  "different sizes (or no size), {} colour targets 1-3, {} surface wider than the pitch; {} in A/B off halves; {} markers left open, {} dropped "
                  "at a draw of another kind{}", api_clear_.markers, api_clear_.widened, d[0], d[1], d[2], d[3], d[4],
                  d[5], d[6], api_clear_.ab_off, api_clear_.left_open, api_clear_.dropped,
                  api_clear_.poisoned ? fmt::format("; DIAGNOSTIC: {} padding rectangles filled", api_clear_.poisoned)
                                      : std::string());
      api_clear_.markers = api_clear_.widened = api_clear_.ab_off = api_clear_.left_open = api_clear_.dropped =
          api_clear_.poisoned = 0;
      api_clear_.declined = {};
    }
    if (rect_splits_) {
      REXLOG_INFO("[native] rectangle lists drawn rectangle by rectangle (banjo_gpu_rect_split), since the last "
                  "report: {} lists as {} draws", rect_splits_, rect_split_draws_);
      rect_splits_ = rect_split_draws_ = 0;
    }
    if (tiles_.key && (tiles_.scenes_once || tiles_.scenes_twice)) {
      TilesOnce& t = tiles_;
      REXLOG_INFO("[native] tiles once (banjo_gpu_tiles_once{}), since the last report: {} scenes drawn once, {} in "
                  "one pass per tile{}{}; draws made for both tiles {}, for the second alone {}; second-pass draws "
                  "dropped {} (each matched to the one the first pass made for it); ring thread time in the first "
                  "passes {:.1f} ms, in the later ones {:.1f} ms",
                  t.failed ? ", OFF after a failed check or 32 scenes back" : "", t.scenes_once, t.scenes_twice,
                  t.last_back[0] ? ", last reason: " : "", t.last_back, t.draws_both, t.draws_other,
                  t.draws_dropped, double(tiles_ns_[0]) / 1e6, double(tiles_ns_[1]) / 1e6);
      tiles_ns_ = {};
      t.scenes_once = t.scenes_twice = t.draws_both = t.draws_other = t.draws_dropped = 0;
    }
    {  // PM4 mix of this interval (what the ring parses per frame): type 0 and the busiest type 3 opcodes
      std::array<std::pair<uint64_t, uint32_t>, 128> order;
      for (uint32_t i = 0; i < 128; ++i) order[i] = {opcodes_[i], i};
      std::partial_sort(order.begin(), order.begin() + 8, order.end(), std::greater<>());
      std::string mix;
      for (uint32_t i = 0; i < 8 && order[i].first; ++i)
        mix += fmt::format(" {:02X}:{}pk/{}w", order[i].second, order[i].first, opcode_words_[order[i].second]);
      REXLOG_INFO("[native] PM4 mix (cumulative): {} packets, type 0: {} ({} words, {} single-register); type 3 opcodes "
                  "(packets/words):{}", packets_, type0_packets_, type0_words_, type0_single_, mix);
    }
    REXLOG_INFO("[native] shader loads reused (identical reload): {}; multi-rect draws proven: {} (rejected: "
                "estimator {}, overwrite proof {}, slot mismatch {}); pixel-exact bounds: {}; identity memo hits {}",
                shader_loads_reused_,
                multi_rect_proven_, multi_rect_rejects_[0], multi_rect_rejects_[1], multi_rect_rejects_[2],
                exact_edges_, identity_memo_hits_);
    identity_memo_hits_ = 0;
    multi_rect_proven_ = 0;
    exact_edges_ = 0;
    multi_rect_rejects_ = {};
    shader_loads_reused_ = 0;
    REXLOG_INFO("[native] PS identity audit: {} mismatching candidate checks "
                "(all selection paths, cumulative; not distinct draws)", ps_identity_mismatches_);
    REXLOG_INFO("[native] VS identity audit: {} mismatching candidate checks "
                "(declared fetch patches only; all ALU/CF words compared; cumulative)", vs_identity_mismatches_);
    REXLOG_INFO("[native] FINAL VS selection audit: {} proven, {} unproven, {} missing "
                "(cumulative attempted draws; not GPU/DEF/ABI equivalence proof)",
                vs_final_proven_, vs_final_unproven_, vs_final_missing_);
    uint64_t copies = 0, clears = 0, presented = 0, rejected = 0;
    if (targets_) targets_->Stats(copies, clears, presented, rejected);
    const auto st = targets_ ? targets_->StatsOfDraws() : masseffect::native::StatsDraws{};
    REXLOG_INFO("[native] completion coverage: draw failures {}/{}; copy/clear failures {}/{} "
                "(includes target preparation; successful returns alone do not certify rendering)",
                native_draw_failures_, native_draw_attempts_, native_copy_failures_, native_copy_attempts_);
    REXLOG_INFO("[native] {:.1f} s: {} Swaps, {} draws ({} without shaders), {} copies, {} packets, "
                "{} interrupts, {} WAIT_REG_MEM timeouts; shaders {} identified ({} by address) / {} not; native: "
                "{} drawn, {} rejected, {} pipelines, {} textures, {} presented",
                secs, swaps_ - reported_swaps_, draws_ - reported_draws_, draws_unidentified_,
                copies_ - reported_copies_, packets_ - reported_packets_,
                interrupts_.load() - reported_interrupts_, waits_timed_out_, shaders_identified_,
                shaders_by_address_, shaders_unidentified_, st.drawn, st.rejected, st.pipelines, st.textures,
                presented);
    std::string causes;
    for (size_t i = 0; i < st.causes.size() && i < 6; ++i) {
      causes += fmt::format(" {}x{}", st.causes[i].first, st.causes[i].second);
    }
    if (!causes.empty()) REXLOG_INFO("[native] rejection causes (code x count):{}", causes);
    if (!missing_draw_pairs_.empty()) {
      std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>> ranked(
          missing_draw_pairs_.begin(), missing_draw_pairs_.end());
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
      });
      std::string missing;
      for (size_t i = 0; i < ranked.size() && i < 12; ++i) {
        missing += fmt::format(" {}x(VS={:016X},PS={:016X})", ranked[i].second,
                               ranked[i].first.first, ranked[i].first.second);
      }
      REXLOG_INFO("[native] unresolved draw shader pairs (cumulative, 0 means identified):{}", missing);
    }
    {
      std::lock_guard<std::mutex> lock(g_by_address_mutex);
      REXLOG_INFO("[native] tile replays reusing a pairing {}, draws dropped by predicate {}",
                  draws_replayed_, draws_predicated_);
      std::string miss;
      for (auto& [o, n] : missing_objects_) miss += fmt::format(" {:08X}x{}", o, n);
      REXLOG_INFO("[native] draws paired by command-buffer address {} (no VS entry {}, no PS entry {});"
                  " objects without an entry:{}", draws_paired_by_address_, paired_no_vs_, paired_no_ps_, miss);
      REXLOG_INFO("[native] draws paired with a Draw* record {}, unpaired {}; "
                  "draws inside indirect buffers {} of {}; Draw* records {}",
                  draws_paired_, draws_unpaired_, draws_indirect_, draws_,
                  g_draw_records.load());
      REXLOG_INFO("[native] identified by code {}; unidentified loads: VS by address {}, PS by address "
                  "{}, immediate {}; {} objects seen, {} addresses and {} codes mapped, {} code conflicts",
                  shaders_by_code_, miss_vs_addr_, miss_ps_addr_, miss_immediate_, g_objects_seen.size(),
                  g_by_address.size(), g_by_code.size(), g_code_conflicts);
    }
    {  // The ring thread's own time of this interval: its parse loop, less the WAIT_REG_MEM waits for the game.
      const uint64_t swaps = swaps_ - report_ring_swaps_, draws = draws_ - report_ring_draws_;
      const double busy = double(ring_busy_ns_ - report_ring_busy_ns_) / 1e6;
      const double wait = double(ring_wait_ns_ - report_ring_wait_ns_) / 1e6;
      const double submit = double(submit_ns_ - report_submit_ns_) / 1e6;
      report_submit_ns_ = submit_ns_;
      if (swaps)
        REXLOG_INFO("[native] ring thread: {:.2f} ms of work per Swap ({:.2f} us per draw, {} draws per Swap; Draw/Copy "
                    "{:.2f} us per draw, the rest {:.2f} us per draw), {:.2f} ms per Swap waiting for the game "
                    "(WAIT_REG_MEM){}",
                    (busy - wait) / double(swaps), draws ? (busy - wait) * 1000.0 / double(draws) : 0.0,
                    draws / swaps, draws ? submit * 1000.0 / double(draws) : 0.0,
                    draws ? (busy - wait - submit) * 1000.0 / double(draws) : 0.0, wait / double(swaps),
                    ring_core_ < 0 ? std::string()
                                   : fmt::format("; banjo_ring_core: {} now, on core {}, {} core changes seen (sampled)",
                                                 ring_pinned_ ? "pinned" : "whole mask", ring_core_last_,
                                                 ring_core_moves_));
      if (split_on_ && swaps) {  // banjo_ring_decoder
        const double exec = double(exec_busy_ns_ - report_exec_busy_ns_) / 1e6;
        const double calls = double(split_call_wait_ns_ - report_split_call_wait_ns_) / 1e6;
        const uint64_t now_split[5] = {split_draws_, split_items_, split_pages_, split_calls_, split_checks_};
        uint64_t d[5];
        for (int i = 0; i < 5; ++i) {
          d[i] = now_split[i] - report_split_[i];
          report_split_[i] = now_split[i];
        }
        REXLOG_INFO("[native] ring split (banjo_ring_decoder {}): the executor (ring thread) {:.2f} ms of work per Swap "
                    "({:.2f} us per draw); the decoder {:.2f} ms per Swap (the line above), of it {:.2f} waiting for "
                    "the executor's answers; per Swap {} draws, {} other items, {} register pages, {} calls; {} file "
                    "checks, {} differences (cumulative){}; queue full {} times",
                    ::me::native::AbOn(43) ? "on" : "on, A/B off half: one item at a time", exec / double(swaps),
                    draws ? exec * 1000.0 / double(draws) : 0.0, (busy - wait) / double(swaps), calls / double(swaps),
                    d[0] / swaps, d[1] / swaps, d[2] / swaps, d[3] / swaps, d[4], split_check_differences_,
                    split_full_ ? " (whole file per draw since)" : "", split_queue_->full_waits_);
        report_exec_busy_ns_ = exec_busy_ns_;
        report_split_call_wait_ns_ = split_call_wait_ns_;
        if (ab_seconds_ > 0 && ab_[0].swaps && ab_[1].swaps) {  // any target: both threads of each half
          const auto per = [](uint64_t ns, uint64_t n) { return n ? double(ns) / 1e6 / double(n) : 0.0; };
          REXLOG_INFO("[native] ring split A/B (cumulative, banjo_ab_target {}{}): ON executor {:.2f} ms, decoder "
                      "{:.2f} ms per Swap | OFF executor {:.2f} ms, decoder {:.2f} ms per Swap", ab_target_,
                      ab_target_ == 43 ? ": OFF is one item at a time, their sum one thread's" : "",
                      per(ab_[0].busy_ns, ab_[0].swaps), per(ab_[0].decoder_ns, ab_[0].swaps),
                      per(ab_[1].busy_ns, ab_[1].swaps), per(ab_[1].decoder_ns, ab_[1].swaps));
        }
      }
      ring_core_moves_ = 0;
      report_ring_swaps_ = swaps_;
      report_ring_draws_ = draws_;
      report_ring_busy_ns_ = ring_busy_ns_;
      report_ring_wait_ns_ = ring_wait_ns_;
    }
    if (ab_seconds_ > 0 && (ab_[0].swaps || ab_[1].swaps)) {
      auto half = [](const AbHalf& h) {
        const double work = double(h.busy_ns - std::min(h.busy_ns, h.wait_ns)) / 1e6;
        const double submit = double(h.submit_ns) / 1e6, rest = work - submit;
        return fmt::format("{:.2f} ms of work per Swap ({:.2f} us per draw, {} draws per Swap) over {} Swaps; of it "
                           "Draw/Copy {:.2f} us per draw, the rest (PM4, records, identification) {:.2f} us per draw",
                           h.swaps ? work / double(h.swaps) : 0.0, h.draws ? work * 1000.0 / double(h.draws) : 0.0,
                           h.swaps ? h.draws / h.swaps : 0, h.swaps, h.draws ? submit * 1000.0 / double(h.draws) : 0.0,
                           h.draws ? rest * 1000.0 / double(h.draws) : 0.0);
      };
      REXLOG_INFO("[native] ring thread A/B (cumulative, {} every {} s): ON {} | OFF {}",
                  ab_target_ == 3 ? std::string("banjo_ring_constants_dedupe")
                  : ab_target_ == 2 ? std::string("banjo_ring_edram_memo")
                  : ab_target_ == 1 ? std::string("banjo_direct_state")
                  : ab_target_ == 16 ? std::string("masseffect_native_deferred_recording")
                  : ab_target_ == 19 ? std::string("banjo_d3d_native_draw_*")
                  : ab_target_ == 20 ? std::string("banjo_d3d_api_clear")
                  : ab_target_ == 21 ? std::string("banjo_d3d_api_resolve")
                  : ab_target_ == 22 ? std::string("banjo_gpu_skip_dead_draws")
                  : ab_target_ == 23 ? std::string("banjo_ring_vertex_lock_rule")
                  : ab_target_ == 24 ? std::string("banjo_ring_texture_invalidate_range")
                  : ab_target_ == 25 ? std::string("banjo_ring_index_persistent")
                  : ab_target_ == 26 ? std::string("banjo_gpu_early_alpha")
                  : ab_target_ == 27 ? std::string("banjo_gpu_texture_signs_per_slot")
                  : ab_target_ == 28 ? std::string("banjo_gpu_edram4_clear_merge")
                  : ab_target_ == 29 ? std::string("banjo_gpu_vfetch_block")
                  : ab_target_ == 30 ? std::string("banjo_gpu_branchless_select")
                  : ab_target_ == 31 ? std::string("banjo_gpu_bool_spec")
                  : ab_target_ == 32 ? std::string("banjo_gpu_stencil_fetch_3d")
                  : ab_target_ == 33 ? std::string("banjo_gpu_edram4_lean_barriers")
                  : ab_target_ == 36 ? std::string("banjo_gpu_resolve_clear_merge")
                  : ab_target_ == 37 ? std::string("banjo_gpu_pred_select")
                  : ab_target_ == 38 ? std::string("banjo_gpu_pred_select_ps")
                  : ab_target_ == 39 ? std::string("banjo_gpu_tess_grid")
                  : ab_target_ == 40 ? std::string("the shader round of 2026-10-16 (29, 37-39)")
                  : ab_target_ == 41 ? std::string("banjo_ring_core (the ring thread pinned)")
                  : ab_target_ == 43 ? std::string("banjo_ring_decoder (the ring split in decoder and executor)")
                  : ab_target_ == 44 ? std::string("banjo_ring_vs_hash_given (the program hash from the load)")
                  : ab_target_ == 45 ? std::string("banjo_d3d_records (records of every Direct3D call's packets)")
                  : ab_target_ == 46 ? std::string("banjo_d3d_records_native (the draw path writes its records)")
                  : ab_target_ == 47 ? std::string("banjo_d3d_host_submit (the kicks through a host queue)")
                  : ab_target_ == 49 ? std::string("banjo_gpu_clamp_cbuf (clamp bounds from constants)")
                  : ab_target_ == 50 ? std::string("banjo_gpu_sign_mask_skip (no sign mask without signs)")
                  : ab_target_ == 51 ? std::string("banjo_gpu_vs_outputs_unwritten (unread outputs not written)")
                  : ab_target_ == 52 ? std::string("banjo_gpu_bone_padding (indexed fetches without selects)")
                  : ab_target_ == 53 ? std::string("banjo_gpu_robustness_off (no descriptor index clamps)")
                  : ab_target_ == 54 ? std::string("banjo_gpu_edram_checks_cpu (import image checks on the CPU)")
                  : ab_target_ == 55 ? std::string("the shader round of package 25 (49-54)")
                  : ab_target_ == 56 ? std::string("banjo_gpu_literals_cbuf (literal constants from the constant registers)")
                  : ab_target_ == 57 ? std::string("banjo_gpu_stencil_clear_partial (stencil-replacing draws fetch only partly covered tiles)")
                  : ab_target_ == 70 ? std::string("the shader round of package 26 (56-69)")
                                     : fmt::format("banjo_ab_target {}", ab_target_),
                  ab_seconds_, half(ab_[0]), half(ab_[1]));
      if (targets_) {  // the GPU side of the same halves (each submission counted in the half it was recorded in)
        std::array<uint64_t, masseffect::native::kGpuCategories> on{}, off{};
        targets_->TimeGpuAb(on, off);
        auto gpu = [](const std::array<uint64_t, masseffect::native::kGpuCategories>& ns, uint64_t swaps) {
          if (!swaps) return std::string("no Swaps");
          constexpr double kScale = 1.627;  // raw NVK timestamps on the Switch, as in the GPU per Swap line
          auto ms = [&](uint32_t c) { return double(ns[c]) * kScale / 1e6 / double(swaps); };
          double total = 0;
          for (uint32_t c = 0; c < masseffect::native::kGpuCategories; ++c) total += ms(c);
          return fmt::format("total {:.1f} ms per Swap (scene {:.1f}, smaller {:.1f}, gap {:.1f}, edram import {:.1f}, "
                             "import9 {:.1f}, export {:.1f}, alias {:.1f})",
                             total, ms(2), ms(5), ms(9), ms(10), ms(13), ms(11), ms(12));
        };
        REXLOG_INFO("[native] GPU A/B (cumulative, same halves): ON {} | OFF {}", gpu(on, ab_[0].swaps),
                    gpu(off, ab_[1].swaps));
      }
    }
    if (!wait_stats_.empty()) {  // banjo-nx: the WAIT_REG_MEM waits of this interval, longest first
      std::vector<std::pair<uint64_t, WaitStat>> waits(wait_stats_.begin(), wait_stats_.end());
      std::sort(waits.begin(), waits.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
      std::string text;
      for (size_t i = 0; i < waits.size() && i < 6; ++i) {
        const uint64_t k = waits[i].first;
        text += fmt::format(" | {} {:08X} ref {:08X}: {} waits, {:.1f} ms, {} polls, {} wakes, {} timeouts",
                            (k >> 63) ? "memory" : "register", uint32_t(k >> 32) & 0x7FFFFFFFu, uint32_t(k),
                            waits[i].second.waits, double(waits[i].second.ns) / 1e6, waits[i].second.polls,
                            waits[i].second.woken, waits[i].second.timeouts);
      }
      REXLOG_INFO("[native] WAIT_REG_MEM waits (last interval){}", text);
      wait_stats_.clear();
    }
    if (swaps_ != report_ring_swaps_ || guest_syncs_ != report_guest_syncs_) {
      REXLOG_INFO("[native] guest synchronization points (banjo_ring_sync_fences {}): {} this interval",
                  sync_fences_ ? "on" : "off", guest_syncs_ - report_guest_syncs_);
      report_guest_syncs_ = guest_syncs_;
    }
    REXLOG_INFO("[native] indirect buffers run (cumulative): {}, of them a range already run in the same frame (tile "
                "passes) {} ({} words)", indirect_runs_, indirect_repeats_, indirect_repeat_words_);
    if (direct::g_on.load(std::memory_order_relaxed)) {
      using namespace direct;
      REXLOG_INFO("[native] direct state (cumulative): {} draws converted ({} words of PM4 not parsed), {} records "
                  "applied, {} of them again (tile passes), {} parsed from the side store ({} there now, peak {}, {} moved "
                  "there, {} markers restored), {} released unreached, {} lost{}; release work {:.1f} ms; "
                  "left as PM4: submitted {}, segment {}, "
                  "unknown packet {} (last opcode {:02X}), NOP head {}, small {}, no draw {}, ring or store full {}; "
                  "lock waits {}; records checked at the draw {}, by the ring {} ({} differences); submissions that "
                  "waited for a marker {}",
                  g_converted.load(), g_words_covered.load(), direct_applied_, direct_replayed_, direct_kept_hits_,
                  direct_kept_live_, direct_kept_peak_, direct_kept_total_, direct_restored_,
                  direct_released_unreached_, direct_lost_, g_failed.load() ? " (STOPPED)" : "",
                  double(direct_release_ns_) / 1e6,
                  g_skip_submitted.load(), g_skip_segment.load(), g_skip_unknown.load(), g_unknown_opcode.load(),
                  g_skip_nop_head.load(), g_skip_small.load(), g_skip_no_draw.load(), g_skip_full.load(),
                  g_skip_busy.load(), g_checked.load(), direct_ring_checked_, direct_ring_differences_,
                  g_kick_waits.load());
      if (g_whole.load(std::memory_order_relaxed))
        REXLOG_INFO("[native] Direct3D records (banjo_d3d_records, cumulative): {} calls made into {} records ({} words "
                    "not parsed), {} calls inside another (theirs), {} wrote nothing; left as PM4 (words): barriers {} "
                    "(indirect buffers, WAIT_REG_MEM), unmarkable heads {}, too short {}, partial {}, over the limits {}; "
                    "{} calls changed segment (their new segment's part made into records; {} parts in the segments "
                    "they left too), {} overlapped another call on their device (left as PM4); "
                    "PM4 the decoder parsed outside records: {} packets ({} words; {} with the markers) | {} type-3 "
                    "operations applied from records | native draw records (banjo_d3d_records_native {}): {} written "
                    "by the draw path, {} decoded instead, {} checked against their packets",
                    g_calls_converted.load(), g_pieces.load(), g_words_covered.load(), g_calls_nested.load(),
                    g_calls_empty.load(), g_words_barrier.load(), g_words_head.load(), g_words_small.load(),
                    g_words_partial.load(), g_words_capped.load(), g_calls_switched.load(), g_parts_converted.load(),
                    g_calls_overlapped.load(),
                    pm4_packets_ - markers_reached_, pm4_words_,
                    pm4_packets_, record_type3_ops_,
                    g_native_failed.load() ? "STOPPED" : g_native_ops.load() ? "on" : "off", g_native_records.load(),
                    g_native_fallbacks.load(), g_native_checked.load());
      if (submit::g_on.load(std::memory_order_relaxed))
        REXLOG_INFO("[native] host submissions (banjo_d3d_host_submit {}, cumulative): {} kicks, {} segments queued, "
                    "{} run from the queue, {} dropped; write pointers not predicted {}, queue full {}, kicks of another "
                    "path {}; {} checked against the ring; Direct3D's own ring words: {} queued as operations ({} "
                    "entries run), {} left in the ring",
                    submit::g_failed.load() ? "STOPPED" : host_submit_ ? "on" : "off", submit::g_kicks.load(),
                    submit::g_queued.load(), submitted_segments_, submit_dropped_, submit::g_unpredicted.load(),
                    submit::g_full.load(), submit::g_other_path.load(), submit_checked_,
                    submit::g_words_queued.load(), submitted_words_entries_, submit::g_words_left.load());
      if (static_on_)
        REXLOG_INFO("[native] buffers kept as records (banjo_d3d_records_static, cumulative): {} runs applied ({} words "
                    "not parsed), {} buffers decoded, {} rewritten by the game since (decoded again), {} buffers known",
                    static_runs_, static_words_, static_decoded_, static_changed_, static_buffers_.size());
      if (records_report_) {
        static const char* const kWhere[4] = {"primary ring", "buffers it runs", "deeper buffers", "fixed buffers 1FB0xxxx"};
        for (uint32_t b = 0; b < 4; ++b) {
          std::vector<std::pair<uint64_t, uint32_t>> order;
          for (uint32_t k = 0; k < 130; ++k)
            if (pm4_parsed_words_[b][k]) order.push_back({pm4_parsed_words_[b][k], k});
          if (order.empty()) continue;
          std::sort(order.rbegin(), order.rend());
          std::string mix;
          for (size_t i = 0; i < order.size() && i < 12; ++i) {
            const uint32_t k = order[i].second;
            mix += k == 128 ? fmt::format(" type0:{}pk/{}w", pm4_parsed_[b][k], order[i].first)
                   : k == 129 ? fmt::format(" type1/2:{}pk/{}w", pm4_parsed_[b][k], order[i].first)
                              : fmt::format(" {:02X}:{}pk/{}w", k, pm4_parsed_[b][k], order[i].first);
          }
          REXLOG_INFO("[native] PM4 parsed outside records, {} (cumulative; NOP 10 includes the markers, whose words are "
                      "not parsed):{}", kWhere[b], mix);
        }
      }
      REXLOG_INFO("[native] direct state spans: converted {:08X}-{:08X}; indirect buffers at depth 0 {:08X}-{:08X}, 1 "
                  "{:08X}-{:08X}, 2+ {:08X}-{:08X}; devices {:08X}x{} {:08X}x{} {:08X}x{} {:08X}x{}",
                  g_span_converted.lo.load(), g_span_converted.hi.load(), g_span_ib[0].lo.load(),
                  g_span_ib[0].hi.load(), g_span_ib[1].lo.load(), g_span_ib[1].hi.load(), g_span_ib[2].lo.load(),
                  g_span_ib[2].hi.load(), g_devices[0].load(), g_device_draws[0].load(), g_devices[1].load(),
                  g_device_draws[1].load(), g_devices[2].load(), g_device_draws[2].load(), g_devices[3].load(),
                  g_device_draws[3].load());
    }
    // banjo-nx (sa2-nx's CPU rounds): the generated code's own self-checks and the censuses (PC; nothing on the Switch).
    banjo::CodegenChecksReport(static_cast<int>(secs + 0.5));
    // banjo-nx (from futari-nx): the verify configuration (banjo_cpu_verify: every self-check compares on every use).
    // How many comparisons each ring fast path has made against its reference path, and how many disagreed anywhere
    // (every DIFFERENCE / MISMATCH line and the native game functions' verify modes count): 0 = the same result as the
    // original paths.
    if (REXCVAR_GET(banjo_cpu_verify)) {
      const uint64_t differences = ::me::native::g_verify_differences.load(std::memory_order_relaxed);
      REXLOG_INFO("[verify] checks done: identity memo {}, record pairing {}, raw microcode {}, PM4 runs {}, object "
                  "table {}, native game functions {}, direct-state records {} at the draw and {} at the ring, draws "
                  "without pairing {} | differences: {}{}",
                  verify_initial_ - check_identity_, verify_initial_ - check_pairing_, verify_initial_ - check_raw_,
                  verify_initial_ - check_pm4_, verify_initial_ - check_objects_,
                  ::me::native::g_verify_native_checks.load(std::memory_order_relaxed), direct::g_checked.load(),
                  direct_ring_checked_, no_pairing_checks_, differences,
                  differences == 0 ? " (every fast path gave the same result as the original)"
                                   : " (see the DIFFERENCE / MISMATCH lines above; that path turned itself off)");
    }
    reported_swaps_ = swaps_;
    reported_draws_ = draws_;
    reported_copies_ = copies_;
    reported_packets_ = packets_;
    reported_interrupts_ = interrupts_.load();
  }

  masseffect::native::ShadersNative shaders_;
  std::unique_ptr<masseffect::native::TargetsNative> targets_;
  bool targets_failed_ = false;
  const masseffect::native::ShaderEntry* vs_ = nullptr;
  const masseffect::native::ShaderEntry* ps_ = nullptr;
  std::vector<uint32_t> microcode_, vs_microcode_, ps_microcode_;
  uint64_t gen_vs_ = 0, gen_microcode_ = 0, gen_constants_vs_ = 0, gen_constants_ps_ = 0,
           gen_fetch_ = 0, gen_viewport_ = 0;
  uint32_t swap_width_ = 1280, swap_height_ = 720;
  uint32_t logged_misses_ = 0;
  uint32_t im_load_address_ = 0;
  uint64_t shaders_by_code_ = 0, draws_paired_ = 0, draws_unpaired_ = 0;
  uint64_t native_draw_attempts_ = 0, native_draw_failures_ = 0;
  std::unique_ptr<NativeDrawExtentEstimator> draw_extent_estimator_;
  std::unordered_set<uint64_t> extent_diagnostics_;  // banjo-nx: XXH3 of the fields
  uint32_t stencil_clear_diagnostics_ = 0;
  uint64_t native_copy_attempts_ = 0, native_copy_failures_ = 0;
  uint32_t logged_unpaired_ = 0;
  uint64_t draws_paired_by_address_ = 0, paired_no_vs_ = 0, paired_no_ps_ = 0;
  std::unordered_map<uint32_t, uint64_t> missing_objects_;
  std::unordered_set<uint64_t> dumped_rewritten_;
  std::array<uint64_t, masseffect::native::kGpuCategories> report_gpu_ns_{}, report_gpu_frag_{};
  uint64_t report_gpu_swaps_ = 0;
  std::unordered_set<uint64_t> dumped_vertex_variants_;
  uint64_t draws_indirect_ = 0, draws_replayed_ = 0, draws_predicated_ = 0;
  // banjo_direct_state, ring thread: the index of the published records not released yet (number, first word, the
  // Swap count when indexed, reached at least once), in number order, and the first word not indexed yet.
  std::deque<DirectEntry> direct_entries_;
  uint64_t direct_scanned_ = 0;
  std::vector<uint32_t> direct_scratch_, direct_decoded_;
  int32_t direct_check_ = 0;  // records still checked by the ring (masseffect_native_verify_n; every one in verify)
  // Records released from the ring whose marker is still in its command buffer (number -> the record).
  // Records released from the ring whose marker is still in its command buffer: the record's header (number, the
  // marker's address and the three words it replaced), in number order; pruned round robin.
  std::vector<std::array<uint32_t, direct::kRecordHeader>> direct_kept_;  // [1] = 0: dropped
  size_t direct_kept_live_ = 0, direct_prune_at_ = 0;
  uint64_t direct_restored_ = 0, direct_release_ns_ = 0;
  uint64_t direct_applied_ = 0, direct_replayed_ = 0, direct_released_unreached_ = 0, direct_kept_hits_ = 0,
           direct_kept_total_ = 0, direct_kept_peak_ = 0, direct_lost_ = 0, direct_ring_checked_ = 0,
           direct_ring_differences_ = 0;
  // Indirect buffers run this frame (address, length) and the runs of one already run: tile passes (report).
  std::unordered_set<uint64_t> indirect_seen_;
  uint64_t indirect_runs_ = 0, indirect_repeats_ = 0, indirect_repeat_words_ = 0;
  // banjo_no_draw_pairing in verify mode: the old pairing's result compared with the ring's own identification.
  uint64_t no_pairing_checks_ = 0, no_pairing_differences_ = 0;
  // banjo_direct_state_ab_s: the ring's work in the "on" [0] and "off" [1] halves.
  struct AbHalf {
    uint64_t busy_ns = 0, wait_ns = 0, submit_ns = 0, draws = 0, swaps = 0;
    uint64_t decoder_ns = 0;  // banjo_ring_decoder: the decoder's own work (its waits for the game and the executor out)
  };
  std::array<AbHalf, 2> ab_{};
  int32_t ab_seconds_ = 0, ab_target_ = 1;
  uint64_t ab_busy_ns_ = 0, ab_wait_ns_ = 0, ab_draws_ = 0, ab_submit_ns_ = 0;
  uint64_t submit_ns_ = 0, report_submit_ns_ = 0;  // the ring's time in Draw() and Copy() (the Vulkan side)
  Clock::time_point ab_switched_{};
  // WAIT_REG_MEM waits that were not met at once, by (memory?, poll address or register, reference): count, time, polls.
  struct WaitStat {
    uint64_t waits = 0, ns = 0, polls = 0, woken = 0, timeouts = 0;
  };
  // banjo_ring_wait_vblank: the vblank thread counts interrupts and wakes a ring sleeping in WAIT_REG_MEM.
  bool wait_vblank_ = false;
  // banjo_ring_decoder (banjo_ring_split.h): the queue, the executor's register file and the decoder thread.
  bool split_ = false;     // the key, for the session
  bool split_on_ = false;  // the decoder runs and the stream goes through the queue
  bool split_full_ = false;  // after a register-file difference: every draw carries the whole file
  std::unique_ptr<::me::native::split::Queue> split_queue_;
  ::me::native::split::DirtyPages<kRegisterCount> dirty_;
  std::vector<uint32_t> exec_regs_;
  // Registers the game wrote through MMIO (the write pointer and the like: both files get them at any moment, so a
  // snapshot of the decoder's cannot be compared for them).
  std::array<std::atomic<uint64_t>, (kRegisterCount + 63) / 64> split_mmio_{};
  rex::system::object_ref<rex::system::XHostThread> decoder_thread_;
  std::atomic<bool> decoder_done_{true};
  std::function<void()> split_fn_;
  std::atomic<bool> split_fn_done_{false};
  std::mutex split_call_mutex_;
  std::condition_variable split_call_cv_;
  std::atomic<bool> split_call_waiting_{false};
  masseffect::native::RingProgress decoder_progress_;
  std::unordered_map<uint64_t, std::unique_ptr<std::vector<uint32_t>>> split_microcode_;
  uint64_t split_last_key_ = 0;
  const std::vector<uint32_t>* split_last_program_ = nullptr;
  uint64_t split_draws_ = 0, split_items_ = 0, split_pages_ = 0, split_calls_ = 0, split_checks_ = 0;
  uint64_t split_check_differences_ = 0, exec_busy_ns_ = 0, report_exec_busy_ns_ = 0, ab_exec_busy_ns_ = 0;
  uint64_t split_call_wait_ns_ = 0, report_split_call_wait_ns_ = 0, ab_decoder_ns_ = 0;
  uint64_t exec_sample_ = 0x9E3779B97F4A7C15ull;  // xorshift state of the executor's timing sample
  uint64_t report_split_[5] = {};
  // banjo_ring_core (ring thread only; the report reads the counts).
  int32_t ring_core_ = -1;
  bool ring_pinned_ = false;
  int ring_core_last_ = -1;
  uint32_t ring_core_logs_ = 0;
  uint64_t ring_core_switches_ = 0, ring_core_moves_ = 0;
  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
  std::atomic<uint64_t> wait_wake_{0};
  std::atomic<bool> ring_in_wait_{false};
  std::unordered_map<uint64_t, WaitStat> wait_stats_;
  // The ring thread's time in its parse loop and, of that, in WAIT_REG_MEM waits for the game (Report).
  uint64_t ring_busy_ns_ = 0, ring_wait_ns_ = 0;
  uint64_t report_ring_busy_ns_ = 0, report_ring_wait_ns_ = 0, report_ring_swaps_ = 0, report_ring_draws_ = 0;
  uint32_t packet_address_ = 0;
  // banjo_d3d_records (report): packets parsed as PM4 (markers included), their words (the markers' regions not), the
  // markers reached, and the type-3 operations applied from records.
  uint64_t pm4_packets_ = 0, pm4_words_ = 0, markers_reached_ = 0, record_type3_ops_ = 0;
  // ... and what they are, by where they were (the primary ring, a buffer it runs, deeper, Direct3D's fixed buffers at
  // 1FB00000) and kind (type-3 opcode; 128 type 0; 129 type 1 or 2). Counted while banjo_d3d_records is on.
  bool records_report_ = false;
  std::array<std::array<uint64_t, 130>, 4> pm4_parsed_{}, pm4_parsed_words_{};
  struct PairedPacket {
    uint64_t frame;
    const masseffect::native::ShaderEntry* vs;
    const masseffect::native::ShaderEntry* ps;
  };
  std::unordered_map<uint32_t, PairedPacket> paired_packets_;
  std::deque<DrawRecord> pending_;
  const masseffect::native::ShaderEntry* draw_vs_ = nullptr;
  const masseffect::native::ShaderEntry* draw_ps_ = nullptr;
  uint64_t shaders_by_address_ = 0, miss_vs_addr_ = 0, miss_ps_addr_ = 0, miss_immediate_ = 0;
  uint64_t draws_unidentified_ = 0, shaders_identified_ = 0, shaders_unidentified_ = 0, presented_ = 0;
  uint64_t current_vs_hash_ = 0, current_ps_hash_ = 0;
  uint32_t last_vs_load_address_ = UINT32_MAX, last_ps_load_address_ = UINT32_MAX;
  uint64_t shader_loads_reused_ = 0;
  uint64_t ps_identity_mismatches_ = 0;
  uint64_t vs_identity_mismatches_ = 0;
  uint64_t vs_final_proven_ = 0, vs_final_unproven_ = 0, vs_final_missing_ = 0;
  uint64_t vs_identity_generation_ = 1;
  uint64_t ps_identity_generation_ = 1;
  // The last identity generation given out, for both stages: a generation names one load's content, and
  // banjo_ring_shader_load_memo puts an old one back when that content is loaded again.
  uint64_t identity_generations_ = 1;
  std::vector<LoadMemo> load_memo_;  // banjo_ring_shader_load_memo (empty: off)
  int32_t load_memo_lent_[2] = {-1, -1};  // per stage: the slot whose vectors it holds
  bool load_memo_failed_ = false;
  int32_t load_memo_checks_ = 4096;
  uint64_t load_memo_hits_ = 0, load_memo_stores_ = 0, load_memo_checked_ = 0, load_memo_report_[2] = {};
  std::unordered_map<const masseffect::native::ShaderEntry*, std::pair<uint64_t, bool>> vs_identity_cache_;
  std::array<IdentityCell, kIdentityCells> vs_cells_{}, ps_cells_{}, fc_cells_{};
  // Ring CPU switches (masseffect_native_flat_identity, _fast_pair, _raw_microcode, _pm4_fast) and the
  // remaining self-check uses of each.
  bool identity_flat_ = false, pairing_fast_ = false, raw_microcode_ = false, pm4_fast_ = false;
  uint32_t check_identity_ = 0, check_pairing_ = 0, check_raw_ = 0, check_pm4_ = 0, check_objects_ = 0;
  uint32_t verify_initial_ = 0;  // banjo-nx: masseffect_native_verify_n at start, for the "[verify]" line
  std::vector<uint32_t> raw_vs_, raw_ps_;  // the guest words of the last IM_LOAD of each stage (raw_microcode_)
  bool raw_vs_valid_ = false, raw_ps_valid_ = false;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> missing_draw_pairs_;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;
  std::atomic<bool> active_{false};
  std::vector<uint32_t> registers_;
  std::array<std::array<uint16_t, 3>, 256> gamma_ramp_ = [] {
    std::array<std::array<uint16_t, 3>, 256> ramp{};
    for (uint32_t i = 0; i < 256; ++i) ramp[i].fill(uint16_t(i * 0x3FF / 0xFF));
    return ramp;
  }();
  uint32_t gamma_component_ = 0;
  uint32_t gamma_mask_ = 0b111;
  uint64_t gamma_version_ = 0, gamma_sent_version_ = 0, gamma_writes_ = 0;
  bool gamma_logged_ = false;
  std::atomic<uint32_t> callback_{0}, callback_data_{0};
  std::atomic<uint32_t> ring_base_{0}, ring_words_{0}, ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0}, read_writeback_{0};
  uint64_t ring_progress_total_ = 0;      // primary ring words run before the current batch (never wraps)
  uint32_t ring_progress_generation_ = 0;
  std::atomic<uint32_t> counter_{0};
  std::mutex ring_mutex_;
  std::atomic<bool> ring_waiting_{false};
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_, vblank_thread_;
  uint64_t bin_mask_ = ~0ull, bin_select_ = ~0ull;

  // ---- banjo_gpu_tiles_once ------------------------------------------------------------------------------------
  // Nuts & Bolts draws its scene (1280x720, 2x MSAA) in two predicated tiles because both do not fit in EDRAM: the
  // ring selects the first tile's bins and runs the scene's command buffers, then selects the second tile's and
  // runs the same buffers again. Per tile only three registers differ, set in the buffers under each tile's bin
  // mask: the window offset and the window scissor. The host's images have room for both tiles' rows, so the first
  // pass can draw each draw once for both (one scissor over the two tiles' rows, the window offset of the first):
  //   - a draw both passes would run is drawn once, into the first tile's rows and into the "spare rows" right
  //     below them in the same images, where the second tile's rows would be on a 720-row surface;
  //   - a draw only the second pass would run (its packet, or the buffer it is in, is under the second tile's mask)
  //     is drawn at its place in the stream, into the spare rows alone;
  //   - a resolve is followed by the same copy of the spare rows, to where the second pass's resolve would go;
  //   - in the second pass every draw that writes its targets is dropped (it was made), and so is each resolve's
  //     copy; clears and draws that write no target (memory exports) run as always.
  // The EDRAM bookkeeping only ever sees the first tile: the spare rows are not EDRAM tiles of these views.
  // What it rests on, each checked: the spare rows start as the second tile would (the clear at the end of the
  // first pass, mirrored into them at the end of the previous scene, with the same values this time); the second
  // pass's draws are the ones made for it (a signature of each, compared in order: the draw, its shaders, its
  // targets, masks and depth state, and the second tile's window); its resolves go where the copies went. What is
  // not checked draw by draw: that constants and textures a draw reads are the same in both passes. They are set by
  // the same buffers in both; the picture is compared with the key switched off and on in one run
  // (banjo_ab_target = 9, tools/ab_flicker.py). Anything else under a tile's mask, or a layout other than "second tile right below
  // the first", sends the scene back to two passes before the second one starts (nothing is lost: the second pass
  // then draws as always). A check that fails once the second pass has dropped draws is a DIFFERENCE: the key
  // turns itself off for the session.
  // Not the same picture in one respect: a draw that samples the resolved scene across the tile boundary (water,
  // heat haze) finds this frame's image on the other side, where the console finds what the previous frame left.
  struct TilesOnce {
    bool key = false, failed = false;
    int pass = 0;                   // 0 outside the tile passes; 1 the first; 2 any later one
    uint64_t select = 0;            // the running pass's bin select
    uint64_t select_next = 0;       // the second pass's select, from the last scene
    bool select_next_known = false;
    bool decided = false;           // whether this scene is drawn once has been decided
    bool wide = false;              // first pass: draws go to the next tile's rows too
    bool skip = false;              // second pass: what the first made for it is dropped
    bool ready = false;             // the spare rows hold what the second tile starts from
    bool clean_end = false;         // the first pass's last write to its targets was the resolve that clears them
    bool spare_cleared = false;
    bool other_valid = false;       // the second tile's window, as the stream sets it under that tile's mask
    uint64_t other_mask = 0;
    uint32_t other_window[3] = {};  // offset, scissor tl, scissor br
    uint32_t other_seen = 0;        // which of the three have been set in this pass
    uint32_t tile_offset = 0;       // the window offset the second tile is given under its mask: where its surface
    bool tile_offset_known = false; // starts in the picture (the "split" is minus its y)
    int32_t spare_top = 0, spare_bottom = 0;  // the rows the last clear left as the second tile's surface
    bool packet_predicated = false; // the packet being run carries the predicate bit
    int first_context = 0;          // > 0: running a buffer of the first tile alone
    bool quiet = false;             // window registers being put back or lent: not a write of the stream
    bool second_pending = false;    // the draw in progress is followed by its second-tile twin (second_window)
    uint32_t second_window[3] = {};
    uint32_t other_dest = 0;        // RB_COPY_DEST_BASE as set under that mask (0: not set in this pass)
    int other_context = 0;          // > 0: running a buffer of the second tile alone
    bool draw_predicated = false;   // the draw packet in progress carries the predicate bit
    bool draw_other = false;        // the draw in progress is a packet of the second tile alone
    std::vector<std::array<uint64_t, 11>> drawn;  // first pass: each draw made for the second tile (TilesSignature)
    size_t compared = 0;            // second pass: how many of its draws were matched against them
    std::vector<uint32_t> resolved; // first pass: where each resolve's extra copy went
    size_t resolves_compared = 0;
    size_t other_resolves = 0;      // first pass: resolve packets of the second tile alone met so far
    uint32_t back_streak = 0;       // scenes in a row that went back to two passes
    uint64_t scenes_once = 0, scenes_twice = 0, draws_both = 0, draws_other = 0, draws_dropped = 0, backs = 0;
    const char* last_back = "";
  } tiles_;

  // First pass: a write of the window offset or scissor that the first tile takes (it is in the registers). The
  // second tile takes it too unless the packet, or the buffer it is in, is the first tile's alone.
  void TilesWindowWritten(uint32_t which, uint32_t value) {
    TilesOnce& t = tiles_;
    if (t.quiet || t.other_context || t.first_context) return;
    if (t.packet_predicated && t.select_next_known && !(bin_mask_ & t.select_next)) return;
    t.other_window[which] = value;
    t.other_seen |= 1u << which;
    t.other_valid = (t.other_seen & 7) == 7;
  }
  void TilesSetWindowQuietly(const uint32_t window[3]) {
    namespace g = rex::graphics;
    tiles_.quiet = true;
    WriteRegister(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET, window[0]);
    WriteRegister(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, window[1]);
    WriteRegister(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, window[2]);
    tiles_.quiet = false;
  }

  // Whether the draw in the registers can change its render targets (color, depth or stencil).
  bool TilesDrawWrites() const {
    namespace g = rex::graphics;
    const uint32_t mode = Register(kRegRbModeControl) & 0x7;
    if (mode != uint32_t(xenos::EdramMode::kColorDepth) && mode != 5) return false;
    const uint32_t dc = Register(g::XE_GPU_REG_RB_DEPTHCONTROL);
    if ((dc & 0x1) || (dc & 0x6) == 0x6) return true;  // stencil on, or depth written
    if (mode != uint32_t(xenos::EdramMode::kColorDepth) || !draw_ps_) return false;
    const uint32_t mask = Register(g::XE_GPU_REG_RB_COLOR_MASK);
    for (uint32_t i = 0; i < 4; ++i)
      if (((mask >> (i * 4)) & 0xF) && (draw_ps_->outputs & (1u << i))) return true;
    return false;
  }

  // The first pass stops drawing for the second tile: the second pass will draw as always.
  void TilesBack(const char* reason) {
    TilesOnce& t = tiles_;
    if (!t.wide) return;
    t.wide = false;
    t.ready = false;
    t.last_back = reason;
    if (++t.backs <= 12)
      REXLOG_INFO("[native] tiles once: this scene goes back to one pass per tile at draw {} (swap {}, bin select "
                  "{:X}, mask {:X}, the second tile's mask {:X}): {}", draws_, swaps_, bin_select_, bin_mask_,
                  t.other_mask, reason);
  }

  // A check failed after the second pass had started dropping draws.
  void TilesFail(const std::string& what) {
    TilesOnce& t = tiles_;
    if (t.failed) return;
    t.failed = true;
    t.wide = t.skip = false;
    ::me::native::VerifyDifference();
    REXLOG_ERROR("[native] DIFFERENCE: tiles once (banjo_gpu_tiles_once): {} (swap {}, draw {}). Off for the rest of "
                 "the session; this frame's lower tile may be wrong", what, swaps_, draws_);
  }

  // The draw in the registers, as the tile whose window is (offset, tl, br) sees it: what it draws, with which
  // shaders, into what, under which masks and depth state. Only registers every such draw depends on: the rest of
  // the register file legitimately differs between the passes wherever a draw does not read it (the second pass
  // starts from what the first left).
  using TilesDraw = std::array<uint64_t, 11>;
  TilesDraw TilesSignature(uint32_t offset, uint32_t tl, uint32_t br) const {
    namespace g = rex::graphics;
    return {Register(kRegVgtDrawInitiator), offset, tl, br,
            draw_vs_ ? uint64_t(draw_vs_->number) : ~0ull, draw_ps_ ? uint64_t(draw_ps_->number) : ~0ull,
            // (the index buffer registers only for a draw that reads one: otherwise they are whatever the last
            // indexed draw left, which is not the same draw in the two passes)
            ((Register(kRegVgtDrawInitiator) >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)
                ? (uint64_t(Register(kRegVgtDmaBase)) << 32) | Register(kRegVgtDmaSize) : 0ull,
            (uint64_t(Register(kRegRbModeControl)) << 32) | Register(g::XE_GPU_REG_RB_SURFACE_INFO),
            (uint64_t(Register(g::XE_GPU_REG_RB_COLOR_INFO)) << 32) | Register(g::XE_GPU_REG_RB_DEPTH_INFO),
            (uint64_t(Register(g::XE_GPU_REG_RB_COLOR_MASK)) << 32) | Register(g::XE_GPU_REG_RB_DEPTHCONTROL),
            (uint64_t(Register(g::XE_GPU_REG_VGT_INDX_OFFSET)) << 32) | Register(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL)};
  }

  void TilesDecide() {
    namespace g = rex::graphics;
    TilesOnce& t = tiles_;
    if (t.decided) return;
    t.decided = true;
    t.wide = t.ready && t.select_next_known && t.other_valid && ::me::native::AbOn(9) && targets_ &&
             t.spare_bottom > 0;
    if (t.wide) {
      bool check = false;
      SplitOrRun([&] {  // banjo_ring_decoder: the renderer is the executor's
        check = targets_->TilesOnceCheck(Register(g::XE_GPU_REG_RB_SURFACE_INFO),
                                         Register(g::XE_GPU_REG_RB_COLOR_INFO), Register(g::XE_GPU_REG_RB_DEPTH_INFO),
                                         uint32_t(t.spare_bottom));
      });
      t.wide = check;
    }
  }

  // At a change of the bin select.
  void TilesSelectChanged() {
    TilesOnce& t = tiles_;
    if (!t.key || t.failed || t.other_context) return;
    if (uint32_t(bin_select_) == 0xFFFFFFFFu) {  // every bin: outside the tile passes
      if (t.pass == 2) {
        if (t.skip && (t.compared != t.drawn.size() || t.resolves_compared != t.resolved.size()))
          TilesFail(fmt::format("the second pass ended after {} of the {} draws and {} of the {} resolves the first "
                                "pass made for it", t.compared, t.drawn.size(), t.resolves_compared,
                                t.resolved.size()));
        if (t.skip) {
          ++t.scenes_once;
          t.back_streak = 0;
        } else {
          ++t.scenes_twice;
          if (t.last_back[0] && ++t.back_streak == 32) {
            t.failed = true;
            REXLOG_INFO("[native] tiles once (banjo_gpu_tiles_once): 32 scenes in a row went back to one pass per "
                        "tile ({}): off for the rest of the session", t.last_back);
          }
        }
      }
      TilesPassTime(0);
      t.pass = 0;
      t.wide = t.skip = false;
      return;
    }
    if (t.pass == 0) {
      TilesPassTime(1);
      t.pass = 1;
      t.select = bin_select_;
      t.decided = t.wide = t.skip = t.clean_end = t.spare_cleared = t.other_valid = false;
      t.other_mask = 0;
      t.other_seen = t.other_dest = 0;
      t.tile_offset_known = false;
      t.first_context = 0;
      t.second_pending = false;
      t.drawn.clear();
      t.resolved.clear();
      t.compared = t.resolves_compared = t.other_resolves = 0;
      t.last_back = "";
      return;
    }
    if (bin_select_ == t.select) return;
    if (t.pass == 1) {
      if (t.wide && !t.clean_end) TilesBack("the first pass did not end with the resolve that clears its targets");
      if (t.wide && bin_select_ != t.select_next) TilesBack("the second pass selects other bins than last scene's");
      t.skip = t.wide;
      t.wide = false;
      // For the next scene: the spare rows are as the second tile starts if the clear reached them last.
      t.ready = t.clean_end && t.spare_cleared;
      t.select_next = bin_select_;
      t.select_next_known = true;
    } else if (t.skip) {
      TilesFail("a third tile pass");
    }
    TilesPassTime(2);
    t.pass = 2;
    t.select = bin_select_;
  }
  // Ring thread time inside the tile passes (report): [0] the first pass, [1] the later ones.
  void TilesPassTime(int pass_now) {
    const auto now = Clock::now();
    if (tiles_time_pass_ == 1 || tiles_time_pass_ == 2)
      tiles_ns_[tiles_time_pass_ - 1] +=
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - tiles_time_mark_).count());
    tiles_time_pass_ = pass_now;
    tiles_time_mark_ = now;
  }
  int tiles_time_pass_ = 0;
  Clock::time_point tiles_time_mark_{};
  std::array<uint64_t, 2> tiles_ns_{};

  // First pass: a packet the predicate drops, of a later tile. True if it was run here (a buffer of the second tile).
  bool TilesOtherPacket(Reader data, uint32_t opcode, uint32_t words, int depth) {
    TilesOnce& t = tiles_;
    if (!(bin_mask_ & ~t.select)) return false;
    if (opcode == xenos::PM4_SET_CONSTANT) {
      // What each tile is given under its own mask: the window (0x2080 offset, 0x2081 and 0x2082 scissor) and
      // where its resolves go (0x2319, RB_COPY_DEST_BASE). The first pass uses the second tile's window for the
      // draws it makes for it, and checks its own copies against that destination. Any other register: not
      // something one draw can serve both tiles with.
      const uint32_t type_index = words >= 1 ? data.Read() : 0;
      const uint32_t first = 0x2000 + (type_index & 0x7FF);
      bool known = words >= 2 && ((type_index >> 16) & 0xFF) == 4;
      for (uint32_t i = 0; known && i + 1 < words; ++i)
        known = (first + i >= 0x2080 && first + i <= 0x2082) || first + i == 0x2319;
      if (!known) {
        if (t.wide && t.backs < 12)
          REXLOG_INFO("[native] tiles once: SET_CONSTANT under mask {:X}: {} words, first {:08X}", bin_mask_, words,
                      type_index);
        TilesBack("state set for the second tile alone that is not its window or its resolve destination");
        t.ready = false;
        return false;
      }
      if (t.other_mask && bin_mask_ != t.other_mask) {
        TilesBack("more than one other tile");
        t.ready = false;
      }
      t.other_mask = bin_mask_;
      for (uint32_t i = 0; i + 1 < words; ++i) {
        const uint32_t value = data.Read();
        if (first + i == 0x2319) {
          t.other_dest = value;
          continue;
        }
        const uint32_t which = first + i - 0x2080;
        if (which == 0) {
          if (t.tile_offset_known && value != t.tile_offset) {
            TilesBack("the second tile's window offset changes");
            t.ready = false;
          }
          t.tile_offset = value;
          t.tile_offset_known = true;
        }
        t.other_window[which] = value;
        t.other_seen |= 1u << which;
      }
      t.other_valid = (t.other_seen & 7) == 7;
      return false;
    }
    if (opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2) return false;  // see the caller
    if (opcode == xenos::PM4_INDIRECT_BUFFER || opcode == xenos::PM4_INDIRECT_BUFFER_PFD) {
      if (words < 2) return false;
      TilesDecide();
      if (!t.wide) return false;
      if (!t.other_valid || !(bin_mask_ & t.other_mask) || depth >= kMaxIndirectDepth) {
        TilesBack("a buffer under a mask that is not the second tile's");
        return false;
      }
      // The second pass would run this buffer and the first would not: it is run now, as the second pass would
      // (its bin select), its draws going to the spare rows. What the first pass's stream does not have, it must
      // not keep: the select, the mask and the window come back as they were.
      namespace g = rex::graphics;
      const uint32_t address = data.Read() & 0x1FFFFFFF;
      const uint32_t length = data.Read() & 0xFFFFF;
      const uint64_t select = bin_select_, mask = bin_mask_;
      const uint32_t window[3] = {Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET),
                                  Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL),
                                  Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR)};
      masseffect::native::NoteCommandBufferPages(address, length * 4);
      bin_select_ = t.select_next;
      TilesSetWindowQuietly(t.other_window);
      ++t.other_context;
      Reader r;
      r.base = memory_->TranslatePhysical(address);
      r.physical = address;
      r.end = length;
      while (r.Pending()) {
        if (!Packet(r, depth + 1)) break;
      }
      --t.other_context;
      bin_select_ = select;
      bin_mask_ = mask;
      t.other_window[0] = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
      t.other_window[1] = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
      t.other_window[2] = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
      TilesSetWindowQuietly(window);
      return true;
    }
    if (bin_mask_ & t.other_mask) {
      TilesBack("a packet for the second tile alone that is not its window, a draw or a buffer");
      t.ready = false;
    }
    return false;
  }

  // First pass: a draw packet the predicate drops (its initiator is in the registers, the shaders paired). If it is
  // the second tile's, it is drawn now, into the spare rows.
  void TilesOtherDraw(Reader data, uint32_t initiator, uint32_t words_left) {
    TilesOnce& t = tiles_;
    if (!(bin_mask_ & ~t.select)) return;
    if ((Register(kRegRbModeControl) & 0x7) == uint32_t(xenos::EdramMode::kCopy)) {
      // The second tile's own resolve packet, where the game gives each tile one: its copy is the one made after
      // the first tile's resolve just before it (checked against its destination there, and against this resolve
      // itself when the second pass runs it).
      if (t.wide && ++t.other_resolves > t.resolved.size())
        TilesBack("a resolve for the second tile with no resolve of the first before it");
      return;
    }
    if (!TilesDrawWrites()) return;
    TilesDecide();
    if (!t.wide) return;
    if (!t.other_valid || !(bin_mask_ & t.other_mask)) {
      TilesBack("a draw under a mask that is not the second tile's");
      return;
    }
    const uint32_t saved[2] = {registers_[kRegVgtDmaBase], registers_[kRegVgtDmaSize]};
    if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA) && words_left >= 2) {
      registers_[kRegVgtDmaBase] = data.Read();
      registers_[kRegVgtDmaSize] = data.Read();
      if (t_decoder) {
        dirty_.Mark(kRegVgtDmaBase);
        dirty_.Mark(kRegVgtDmaSize);
      }
    }
    if (draw_vs_ && (draw_ps_ || (Register(kRegRbModeControl) & 0x7) == 5)) {
      t.draw_other = true;
      Draw();
      t.draw_other = false;
    }
    registers_[kRegVgtDmaBase] = saved[0];
    registers_[kRegVgtDmaSize] = saved[1];
    if (t_decoder) {
      dirty_.Mark(kRegVgtDmaBase);
      dirty_.Mark(kRegVgtDmaSize);
    }
  }

  // In a tile pass, before a draw goes to the renderer. False: the draw is dropped.
  bool TilesPrepareDraw(masseffect::native::SubmissionDraw& request) {
    namespace g = rex::graphics;
    TilesOnce& t = tiles_;
    const bool writes = TilesDrawWrites();
    const uint32_t offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    const uint32_t tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    const uint32_t br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    const auto empty = [](uint32_t a, uint32_t b) {
      return (b & 0x3FFF) <= (a & 0x3FFF) || ((b >> 16) & 0x3FFF) <= ((a >> 16) & 0x3FFF);
    };
    const auto offset_x = [](uint32_t o) { return int32_t((o & 0x7FFF) << 17) >> 17; };
    const auto offset_y = [](uint32_t o) { return int32_t(((o >> 16) & 0x7FFF) << 17) >> 17; };
    const auto row = [](uint32_t word) { return int32_t((word >> 16) & 0x3FFF); };
    const auto with_row = [](uint32_t word, int32_t y) { return (word & 0x8000FFFFu) | (uint32_t(y) << 16); };
    if (t.pass == 1) {
      // The second tile's surface is the same EDRAM rows as the first's. In the host images its rows are the
      // "spare rows", `split` rows further down: the screen row that the window offset given to it under its mask
      // brings to the top of the surface. So a second-tile draw is the same draw with its window offset moved
      // down by the split, whatever window it has: the offset of its mask (then 0 here: the rows of the picture)
      // or any other (a draw in surface coordinates: +split here), its scissor kept inside the tile's rows.
      const int32_t split = t.tile_offset_known && offset_x(t.tile_offset) == 0 ? -offset_y(t.tile_offset) : -1;
      const bool follows = (Register(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL) >> 16) & 0x1;  // vtx_window_offset_enable
      // 0: the window is in `out`. 1: nothing of the draw in the tile. Otherwise why it cannot be done.
      const auto second_window = [&](uint32_t offset2, uint32_t tl2, uint32_t br2, uint32_t out[3], const char*& why) {
        why = nullptr;
        if (empty(tl2, br2)) return 1;
        if (split <= 0 || t.spare_top != split || t.spare_bottom <= split)
          why = "the second tile's window offset is not a move up of its rows by the first tile's height";
        else if ((tl2 >> 31) || !follows)
          why = "a second-tile draw whose scissor or viewport ignores the window offset";
        else if (offset_x(offset2) != 0 || offset_y(offset2) + split < 0)
          why = "a second-tile draw with a window offset outside its surface";
        if (why) return 2;
        // Rows of the surface the scissor allows, kept to the surface.
        const int32_t first_row = std::max(row(tl2) + offset_y(offset2), 0);
        const int32_t last_row = std::min(row(br2) + offset_y(offset2), t.spare_bottom - split);
        if (last_row <= first_row) return 1;
        out[0] = (uint32_t(offset_y(offset2) + split) & 0x7FFF) << 16;
        out[1] = with_row(tl2, first_row - offset_y(offset2));
        out[2] = with_row(br2, last_row - offset_y(offset2));
        return 0;
      };
      if (t.other_context || t.draw_other) {  // a draw of the second tile alone
        if (writes) TilesDecide();  // (it may be the pass's first drawing draw: the decision cannot wait for a later one)
        if (!writes || !t.wide) return false;  // (one that writes no target runs in the second pass, as always)
        t.clean_end = false;
        const uint32_t offset2 = t.other_context ? offset : t.other_window[0];
        const uint32_t tl2 = t.other_context ? tl : t.other_window[1];
        const uint32_t br2 = t.other_context ? br : t.other_window[2];
        t.drawn.push_back(TilesSignature(offset2, tl2, br2));
        const char* why = nullptr;
        const int result = second_window(offset2, tl2, br2, request.tiles_window, why);
        if (result == 2) {
          t.drawn.pop_back();
          TilesBack(why);
        }
        if (result != 0) return false;
        request.tiles_once = 2;
        ++t.draws_other;
        return true;
      }
      if (!writes) return true;
      t.clean_end = false;
      TilesDecide();
      if (!t.wide) return true;
      // The first tile's own draw stays in the first tile's rows: the rows below are the second tile's now.
      const int32_t down1 = (tl >> 31) ? 0 : offset_y(offset);
      if (split > 0 && row(br) + down1 > split) {
        request.tiles_once = 4;
        request.tiles_window[0] = offset;
        request.tiles_window[1] = tl;
        request.tiles_window[2] = with_row(br, std::max(split - down1, 0));
      }
      const bool both = !t.first_context && (!t.draw_predicated || (bin_mask_ & t.select_next) != 0);
      if (!both) return true;
      if (!t.other_valid) {
        TilesBack("a draw for both tiles before the second tile's window is set");
        return true;
      }
      const uint32_t offset2 = t.other_window[0], tl2 = t.other_window[1], br2 = t.other_window[2];
      t.drawn.push_back(TilesSignature(offset2, tl2, br2));
      const char* why = nullptr;
      const int result = second_window(offset2, tl2, br2, t.second_window, why);
      if (result == 2) {
        t.drawn.pop_back();
        TilesBack(why);
      }
      if (result != 0) return true;  // nothing of it in the second tile
      // One draw for both when the second tile's part is the same picture rows continued: its window is the tile's
      // (so its rows here are the picture's), the first tile's has no offset, and the two scissors are one
      // rectangle cut at the split. Otherwise the draw is made twice, once per tile's rows.
      if (offset2 == t.tile_offset && !(offset & 0x7FFF7FFFu) && !(tl >> 31) && !empty(tl, br) &&
          (tl & 0x3FFF) == (tl2 & 0x3FFF) && (br & 0x3FFF) == (br2 & 0x3FFF) && row(br) == split &&
          row(t.second_window[1]) == split) {
        request.tiles_once = 1;
        request.tiles_window[0] = offset;
        request.tiles_window[1] = tl;
        request.tiles_window[2] = with_row(br, row(t.second_window[2]));
        ++t.draws_both;
        return true;
      }
      t.second_pending = true;
      return true;
    }
    if (t.pass == 2 && t.skip && writes) {
      // The draw the first pass made for this tile.
      if (t.compared >= t.drawn.size()) {
        TilesFail("the second pass has a draw the first pass did not make for it");
        return true;
      }
      const TilesDraw now = TilesSignature(offset, tl, br);
      if (t.drawn[t.compared] != now) {
        static const char* const kFields[11] = {"initiator", "window offset", "scissor tl", "scissor br", "VS", "PS",
                                                "index buffer", "mode / surface", "color / depth info",
                                                "color mask / depth control", "index offset / raster mode"};
        std::string differing;
        for (size_t i = 0; i < now.size(); ++i)
          if (t.drawn[t.compared][i] != now[i])
            differing += fmt::format(" {}: {:X} then, {:X} now;", kFields[i], t.drawn[t.compared][i], now[i]);
        TilesFail(fmt::format("draw {} of the second pass is not the one the first pass made for it ({})", t.compared,
                              differing));
        return true;
      }
      ++t.compared;
      ++t.draws_dropped;
      return false;
    }
    return true;
  }
  // Ring-thread counters.
  uint64_t packets_ = 0, draws_ = 0, copies_ = 0, swaps_ = 0, waits_timed_out_ = 0;
  uint64_t opcodes_[128] = {};
  uint64_t opcode_words_[128] = {}, type0_packets_ = 0, type0_words_ = 0, type0_single_ = 0;  // PM4 mix report
  std::atomic<uint64_t> interrupts_{0};
  uint64_t reported_swaps_ = 0, reported_draws_ = 0, reported_copies_ = 0, reported_packets_ = 0,
           reported_interrupts_ = 0;
  Clock::time_point start_, last_report_;
  // banjo_trace_frame_s: 0 waiting, 1 tracing the current frame, 2 done.
  int trace_phase_ = 0;
  // banjo_trace_frame_pm4: the indirect buffers being run (innermost last) and what each has held so far.
  struct TraceIb {
    uint32_t address = 0, length = 0;
    uint64_t hash = 0;
    uint64_t draws = 0, copies = 0;  // the counters when it started
    uint32_t predicated = 0, state_words = 0, constant_words = 0;
    std::string notable;
  };
  std::vector<TraceIb> trace_ibs_;
  bool trace_pm4_ = false;

  static const char* TraceRegisterName(uint32_t index) {
    namespace g = rex::graphics;
    switch (index) {
      case g::XE_GPU_REG_PA_SC_WINDOW_OFFSET: return "window_offset";
      case g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL: return "scissor_tl";
      case g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR: return "scissor_br";
      case g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL: return "screen_tl";
      case g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR: return "screen_br";
      case g::XE_GPU_REG_RB_SURFACE_INFO: return "surface";
      case g::XE_GPU_REG_RB_COLOR_INFO: return "color0";
      case g::XE_GPU_REG_RB_DEPTH_INFO: return "depth";
      case g::XE_GPU_REG_RB_COPY_CONTROL: return "copy_control";
      case g::XE_GPU_REG_RB_COPY_DEST_BASE: return "copy_dest";
      case g::XE_GPU_REG_RB_COPY_DEST_PITCH: return "copy_pitch";
      case g::XE_GPU_REG_PA_CL_VPORT_YSCALE: return "vp_yscale";
      case g::XE_GPU_REG_PA_CL_VPORT_YOFFSET: return "vp_yoffset";
      default: return index == kRegRbModeControl ? "mode" : nullptr;
    }
  }
  void TraceNote(int depth, const std::string& text) {
    if (depth > 0 && !trace_ibs_.empty()) {
      if (trace_ibs_.back().notable.size() < 2400) trace_ibs_.back().notable += text;
    } else {
      REXLOG_INFO("[trace] ring:{}", text);
    }
  }
  void TracePacket(uint32_t packet, Reader data, uint32_t count, int depth) {
    TraceIb* const ib = depth > 0 && !trace_ibs_.empty() ? &trace_ibs_.back() : nullptr;
    const auto write = [&](uint32_t index, uint32_t value) {
      if (const char* name = TraceRegisterName(index)) TraceNote(depth, fmt::format(" {}={:08X}", name, value));
    };
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool single = (packet >> 15) & 0x1;
        if (ib) (index >= 0x4000 ? ib->constant_words : ib->state_words) += count;
        if (index < 0x4000)
          for (uint32_t i = 0; i < count; ++i) write(single ? index : index + i, data.Read());
        break;
      }
      case 1: {
        if (ib) ib->state_words += 2;
        const uint32_t v1 = data.Read(), v2 = data.Read();
        write(packet & 0x7FF, v1);
        write((packet >> 11) & 0x7FF, v2);
        break;
      }
      case 3: {
        const uint32_t opcode = (packet >> 8) & 0x7F;
        const bool predicated_out = (packet & 0x1) && !(bin_select_ & bin_mask_);
        if (opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2) {
          if (predicated_out && ib) ++ib->predicated;
          if (depth == 0) TraceNote(0, predicated_out ? " draw (predicated out)" : " draw");
          // A draw under a mask that is not every bin, or dropped: with its mask and the targets it would write.
          if ((packet & 0x1) && (predicated_out || uint32_t(bin_mask_) != 0xFFFFFFFFu) && depth > 0)
            TraceNote(depth, fmt::format(" [draw mask {:08X}{} mode {} colormask {:X} dc {:X}]", uint32_t(bin_mask_),
                                         predicated_out ? " OUT" : "", Register(kRegRbModeControl) & 7,
                                         Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK) & 0xFFFF,
                                         Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL) & 0x7));
          break;
        }
        if ((opcode == xenos::PM4_INDIRECT_BUFFER || opcode == xenos::PM4_INDIRECT_BUFFER_PFD) && (packet & 0x1) &&
            uint32_t(bin_mask_) != 0xFFFFFFFFu && count >= 2) {
          // A buffer one tile pass runs and another does not.
          Reader peek = data;
          const uint32_t address = peek.Read() & 0x1FFFFFFF, length = peek.Read() & 0xFFFFF;
          TraceNote(depth, fmt::format(" [buffer {:08X} len {} under mask {:08X}{}]", address, length,
                                       uint32_t(bin_mask_), predicated_out ? " OUT" : ""));
        }
        if (opcode == xenos::PM4_INDIRECT_BUFFER || opcode == xenos::PM4_INDIRECT_BUFFER_PFD ||
            opcode == xenos::PM4_NOP) break;
        if (opcode == xenos::PM4_SET_CONSTANT || opcode == xenos::PM4_SET_CONSTANT2 ||
            opcode == xenos::PM4_SET_SHADER_CONSTANTS) {
          // Registers (state) or constants, as Type3 writes them: the notable registers are listed.
          if (count < 1) break;
          const uint32_t first = data.Read();
          const uint32_t base = opcode == xenos::PM4_SET_CONSTANT ? ConstantBase((first >> 16) & 0xFF) : 0u;
          if (base == UINT32_MAX) break;
          const uint32_t index = base + (opcode == xenos::PM4_SET_CONSTANT ? (first & 0x7FF) : (first & 0xFFFF));
          // A write under a bin mask (the predicate bit, and not every bin): what one tile pass sets and another
          // does not. Listed whatever the register, with the mask and whether this pass runs it.
          if ((packet & 0x1) && uint32_t(bin_mask_) != 0xFFFFFFFFu) {
            std::string values;
            Reader peek = data;
            for (uint32_t i = 1; i < count && i <= 4; ++i) values += fmt::format(" {:08X}", peek.Read());
            TraceNote(depth, fmt::format(" [mask {:08X}{}: reg {:04X} x{}{}]", uint32_t(bin_mask_),
                                         predicated_out ? " OUT" : "", index, count - 1, values));
          }
          if (predicated_out) break;
          if (ib) (index >= 0x4000 ? ib->constant_words : ib->state_words) += count - 1;
          if (index < 0x4000)
            for (uint32_t i = 1; i < count; ++i) write(index + i - 1, data.Read());
          break;
        }
        if (opcode == xenos::PM4_LOAD_ALU_CONSTANT || opcode == xenos::PM4_IM_LOAD ||
            opcode == xenos::PM4_IM_LOAD_IMMEDIATE) {
          if (ib) ib->constant_words += count;
          break;
        }
        std::string words;
        for (uint32_t i = 0; i < count && i < 3; ++i) words += fmt::format(" {:08X}", data.Read());
        TraceNote(depth, fmt::format(" op{:02X}{}({}{})", opcode, predicated_out ? "/out" : "", count, words));
        break;
      }
      default: break;
    }
  }
  bool renderdoc_triggered_ = false;
  bool renderdoc_capturing_ = false;
  int trace_result_ = -1;  // last draw: -1 not attempted, 0 rejected, 1 drawn
  uint64_t trace_draws_ = 0, trace_quiet_draws_ = 0;
  uint32_t trace_target_[4] = {~0u, ~0u, ~0u, ~0u};
  std::unordered_set<uint32_t> resolve_destinations_;  // 4 KB pages of every resolve destination seen
};

}  // namespace

bool HasEntry(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  return g_entry_of_object.count(object) != 0;
}

// banjo-nx: host pointer of a guest virtual address in a physical-memory view (the shader microcode, 0xA0000000 and
// up). On the Switch and on macOS the views map 1:1 onto `base`; on Windows the SDK shifts the 0xE0000000 view by
// 0x1000 (its 64 KB allocation granularity), so `base + address` reads the wrong bytes there. TranslateVirtual
// applies each heap's offset.
static const uint8_t* GuestPhysicalView(const uint8_t* base, uint32_t address) {
#if defined(_WIN32)
  return rex::system::kernel_state()->memory()->TranslateVirtual(address);
#else
  return base + address;
#endif
}

// banjo_note_shader_fast off: the old per-thread table of finished objects.
[[gnu::noinline]] static uint32_t& FinishedThreadSlot(uint32_t object) {
  thread_local std::array<uint32_t, 512> finished_cache{};
  return finished_cache[(object >> 4) & 511];
}

void NoteShaderObject(const uint8_t* base, uint32_t object, bool vertex) {
  const masseffect::native::ShadersNative* library = g_library.load(std::memory_order_acquire);
  if (!library || object < 0x40000000 || object >= 0x7F000000) return;
  auto load32 = [base](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(base + a); };
  if ((load32(object) & 0xFF) != (vertex ? 6u : 7u)) return;
  // Objects whose work is finished (seen and code-mapped: the locked path below only returns for them) are
  // remembered per game thread, so the per-draw call skips the shared mutex (contended with the ring thread).
  // banjo_note_shader_fast (from sa2-nx's sa2_cpu_note_shader_fast): "finished" is a property of the object, not of the
  // thread (seen and code-mapped, or seen and never identified; nothing is ever erased from those sets), so one shared
  // table of 4096 relaxed atomics (a plain load on ARM) serves every draw thread without a TLS lookup (a call to
  // __aarch64_read_tp per access on the Switch), and with 8x the slots of the old per-thread table, whose misses each
  // took the shared mutex and a hash-set insert that found the object already there (4-5 % of the PC draw threads). A
  // hit returns exactly where the locked path below would; a racing reader at worst misses. Off: the old table.
  static const bool fast =
      banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_note_shader_fast));
  static std::array<std::atomic<uint32_t>, 4096> finished_shared{};
  std::atomic<uint32_t>* const finished_slot = fast ? &finished_shared[(object * 0x9E3779B1u) >> 20] : nullptr;
  uint32_t local_unused = 0;
  // The old per-thread table only behind the key, in its own function, so the fast path never computes a TLS address.
  uint32_t& finished_local = fast ? local_unused : FinishedThreadSlot(object);
  if (fast ? finished_slot->load(std::memory_order_relaxed) == object : finished_local == object) return;
  struct Finished {
    std::atomic<uint32_t>* slot;
    uint32_t& local;
    Finished& operator=(uint32_t v) {
      if (slot) slot->store(v, std::memory_order_relaxed);
      else local = v;
      return *this;
    }
  } finished{finished_slot, finished_local};
  {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    if (!g_objects_seen.insert(object).second) {
      // Already identified: map its current microcode once (for VS, after D3D patched it).
      auto it = g_entry_of_object.find(object);
      if (it != g_entry_of_object.end() && g_code_mapped.count(object)) finished = object;
      // Seen but never identified: only the first sighting identifies an object (below), so this stays
      // unidentified; remember it too instead of taking the mutex on every draw (~0.5 % of the render thread).
      if (it == g_entry_of_object.end()) finished = object;
      if (it == g_entry_of_object.end() || g_code_mapped.count(object)) return;
      const uint32_t mc = load32(object + (vertex ? 0x20 : 0x18));
      const uint32_t header = object + (vertex ? 0x368 : 0x28);
      const uint32_t sh = header + load32(header + 24);
      const uint32_t mc_offset = load32(sh), mc_bytes = load32(sh + 4);
      if (mc < 0xA0000000u || !mc_bytes || mc_bytes > 0x40000) return;
      const uint64_t h = CodeHash(GuestPhysicalView(base, mc) + mc_offset, mc_bytes / 4);
      auto [slot, inserted] = g_by_code.emplace(h, it->second);
      if (!inserted && slot->second != it->second) ++g_code_conflicts;
      g_code_mapped.insert(object);
      finished = object;
      return;
    }
  }
  // Same layout as src/me_shader_dump.cpp: container copy in the object, microcode pointer.
  const uint32_t header = object + (vertex ? 0x368 : 0x28);
  const uint32_t virtual_size = load32(header + 4);
  const uint32_t physical_size = load32(header + 8);
  const uint32_t microcode = load32(object + (vertex ? 0x20 : 0x18));
  if ((load32(header) & 0xFFFFFF00u) != 0x102A1100u || virtual_size < 28 || virtual_size > 0x40000 ||
      !physical_size || physical_size > 0x40000 || microcode < 0xA0000000u) {
    return;
  }
  std::vector<uint8_t> container(virtual_size + physical_size);
  std::memcpy(container.data(), base + header, virtual_size);
  std::memcpy(container.data() + virtual_size, GuestPhysicalView(base, microcode), physical_size);
  const masseffect::native::ShaderEntry* entry = library->IdentifyContainer(container);
  if (!entry && vertex) {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    if (!g_vs_by_virtual_built) BuildVirtualIndex(library);
    auto it = g_vs_by_virtual.find(XXH3_64bits(container.data(), virtual_size));
    if (it != g_vs_by_virtual.end()) entry = it->second;
  }
  // Direct3D's internal containers may not be in the package byte-for-byte, because discovery
  // replaces their incomplete virtual metadata with a conservative register-linked wrapper.
  // Their physical microcode is still authoritative. The library code index deliberately prefers
  // the complete wrapper when the same code occurs in multiple containers.
  if (!entry) {
    const uint32_t shader_header = virtual_size >= 28 ? load32(header + 24) : 0;
    const uint32_t physical_offset =
        shader_header + 8 <= virtual_size ? load32(header + shader_header) : 0;
    const uint32_t shader_bytes =
        shader_header + 8 <= virtual_size ? load32(header + shader_header + 4) : 0;
    if (shader_bytes && !(shader_bytes & 3) &&
        uint64_t(physical_offset) + shader_bytes <= physical_size) {
      const uint64_t code = CodeHash(container.data() + virtual_size + physical_offset,
                                     shader_bytes / 4) ^ (vertex ? 1ull : 0ull);
      if (auto it = g_library_by_code.find(code); it != g_library_by_code.end()) {
        entry = it->second;
        REXLOG_INFO("[native] shader object {:08X} matched register-linked {} n{} by microcode",
                    object, vertex ? "VS" : "PS", entry->number);
      }
    }
  }
  if (!entry) {
    const uint32_t shader_header = virtual_size >= 28 ? load32(header + 24) : 0;
    const uint32_t physical_offset = shader_header + 8 <= virtual_size ? load32(header + shader_header) : 0;
    const uint32_t shader_bytes = shader_header + 8 <= virtual_size ? load32(header + shader_header + 4) : 0;
    REXLOG_INFO("[native] shader object {:08X} not in the library: {}_{:016x}; mc {:08X}, "
                "virtual {} physical {}, shader header +{:X}, code +{:X}/{} -> ring {:08X}",
                object, vertex ? "vs" : "ps", XXH3_64bits(container.data(), container.size()),
                microcode, virtual_size, physical_size, shader_header, physical_offset, shader_bytes,
                (microcode & 0x1FFFFFFFu) + physical_offset);
    // Not in the library (containers Direct3D or the engine build at startup): keep a copy with
    // MASSEFFECT_SHADER_MISSING=<folder> so the library can be extended (shaders/tools/build_shader_spirv.sh).
    if (const char* folder = std::getenv("MASSEFFECT_SHADER_MISSING"); folder && *folder) {
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      const uint64_t h = XXH3_64bits(container.data(), container.size());
      const auto path = std::filesystem::path(folder) /
                        fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", h);
      if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(container.data(), 1, container.size(), f);
        std::fclose(f);
      }
    }
    return;
  }
  const uint32_t shader_header = rex::memory::load_and_swap<uint32_t>(container.data() + 24);
  const uint32_t physical_offset =
      shader_header + 4 <= virtual_size
          ? rex::memory::load_and_swap<uint32_t>(container.data() + shader_header)
          : 0;
  const uint32_t mc_bytes = shader_header + 8 <= virtual_size
                                ? rex::memory::load_and_swap<uint32_t>(container.data() + shader_header + 4)
                                : 0;
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  g_by_address[(microcode & 0x1FFFFFFFu) + physical_offset] = entry;
  g_entry_of_object[object] = entry;
  PublishObject(object, entry);
  if (!vertex) {  // pixel shaders are never patched: map their microcode now
    if (mc_bytes && virtual_size + physical_offset + mc_bytes <= container.size()) {
      const uint64_t h = CodeHash(container.data() + virtual_size + physical_offset, mc_bytes / 4);
      auto [slot, inserted] = g_by_code.emplace(h, entry);
      if (!inserted && slot->second != entry) ++g_code_conflicts;
      g_code_mapped.insert(object);
    }
  }
}

void NoteDrawCall(const uint8_t* base, uint32_t type, uint32_t r5, uint32_t r6, uint32_t r7) {
  if (!g_native_active.load(std::memory_order_acquire)) return;
  if (g_no_pairing && !g_no_pairing_check) {  // banjo_no_draw_pairing: the count for the report only
    g_draw_records.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  constexpr uint32_t kDevice = 0x4004B000;  // the D3D device object
  const uint32_t vs = rex::memory::load_and_swap<uint32_t>(base + kDevice + 12416);
  const uint32_t ps = rex::memory::load_and_swap<uint32_t>(base + kDevice + 12412);
  if (g_no_pairing_check) {
    // banjo_no_draw_pairing in verify mode: these calls are skipped in play; a shader object they would note first
    // (one the draw-time shader hook has not) would change what the ring knows.
    auto fresh = [base](uint32_t object, bool vertex) {
      if (object < 0x40000000 || object >= 0x7F000000) return false;
      if ((rex::memory::load_and_swap<uint32_t>(base + object) & 0xFF) != (vertex ? 6u : 7u)) return false;
      std::lock_guard<std::mutex> lock(g_by_address_mutex);
      return g_objects_seen.count(object) == 0;
    };
    if (fresh(vs, true) || fresh(ps, false)) {
      ::me::native::VerifyDifference();
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 8)
        REXLOG_ERROR("[native] DIFFERENCE: NoteDrawCall would note a shader object first (VS {:08X} PS {:08X})", vs, ps);
    }
  }
  NoteShaderObject(base, vs, true);
  NoteShaderObject(base, ps, false);
  g_draw_records.fetch_add(1, std::memory_order_relaxed);
  const uint32_t start = rex::memory::load_and_swap<uint32_t>(base + kDevice + 48) & 0x1FFFFFFFu;
  DrawRecord record{type, r5, r6, r7, vs, ps, start};
  if (RecordTable()) {
    PushRecord(std::move(record), start);  // lock-free; the ring inserts it into its table
    return;
  }
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  g_draw_by_address[start] = std::move(record);
  if (g_draw_by_address.size() > 65536) g_draw_by_address.erase(g_draw_by_address.begin());
}

// banjo_direct_state / banjo_d3d_records, game thread: what the hook needs from before the original function runs.
namespace {
// The devices the draw hooks have seen (r3 of a draw call is always one): a writer's r3 is its device when it is one.
std::atomic<uint32_t> g_known_devices[8] = {};
// banjo_d3d_records: the outermost calls on each device (slot of g_known_devices): low 32 bits the calls in progress,
// high 32 the calls begun. Two threads can be inside Direct3D calls on one device at once (found on 2026-10-16: the
// game's save thread on the real device while the main thread's call waited inside Direct3D): their regions overlap,
// each holding the other's packets. Only a call no other call overlapped makes records (a call that began alone and
// ends alone): the regions of overlapped calls stay PM4 (an earlier record's marker inside a later region is kept out
// of its pieces, as always). One read-modify-write at each end of a call, on a line no other device uses.
struct alignas(64) DeviceCalls {
  std::atomic<uint64_t> v{0};
};
DeviceCalls g_device_calls[8];
inline int DeviceSlot(uint32_t device) {
  for (int i = 0; i < 8; ++i) {
    const uint32_t v = g_known_devices[i].load(std::memory_order_relaxed);
    if (v == device) return i;
    if (!v) return -1;
  }
  return -1;
}
inline bool KnownDevice(uint32_t device) {
  for (const auto& k : g_known_devices) {
    const uint32_t v = k.load(std::memory_order_relaxed);
    if (v == device) return true;
    if (!v) return false;
  }
  return false;
}
inline void NoteDevice(uint32_t device) {
  if (KnownDevice(device)) return;
  for (auto& k : g_known_devices) {
    uint32_t expected = 0;
    if (k.compare_exchange_strong(expected, device, std::memory_order_relaxed) || expected == device) return;
  }
}

// banjo_d3d_records: the outermost call's segment, per thread. Direct3D's space functions (their hooks call
// NoteSegmentSwitch) move it to the new segment when the call runs out of room: what the call writes there is made
// into records (the words in the old segment, which the space function may have submitted, stay PM4).
struct SegmentScope {  // (plain data: thread-local storage zero-initialised, no guard; ARM audit)
  uint32_t device, start, limit, soft;
  uint64_t submissions;
  uint32_t switches;
  // The parts the call wrote in the segments it left: [start, end] (last words written) and that segment's limits. A
  // recording device's space function only chains its segments (nothing is submitted): when no submission happened
  // during the call, those parts are made into records too.
  static constexpr uint32_t kParts = 4;
  struct Part {
    uint32_t start, end, limit, soft;
  } parts[kParts];
  uint32_t part_count;
  bool parts_lost;             // a part that did not fit kParts
  uint64_t begin_submissions;  // at the call's start
};
thread_local SegmentScope t_segment;

DrawDirect BeginDirect(const uint8_t* base, uint32_t device, uint32_t& depth) {
  DrawDirect d;
  if (!direct::g_on.load(std::memory_order_relaxed)) return d;
  d.counted = true;
  if (depth++ != 0) {  // inside another hooked call: part of its region
    direct::Bump(direct::g_calls_nested, 1);  // (a report: a plain load and store, not an exclusive loop)
    return d;
  }
  const uint32_t target = direct::g_whole.load(std::memory_order_relaxed) ? 45u : 1u;  // banjo_ab_target
  if (direct::g_failed.load(std::memory_order_relaxed) || direct::g_kept_full.load(std::memory_order_relaxed) ||
      ::me::native::g_ab_off_target.load(std::memory_order_relaxed) == target ||
      !g_native_active.load(std::memory_order_acquire) || device < 0x40000000 || device >= 0x7F000000)
    return d;
  if (direct::g_whole.load(std::memory_order_relaxed)) {  // (the calls on this device: see DeviceCalls)
    const int slot = DeviceSlot(device);
    if (slot < 0) return d;  // a device the hooks have not met: no records
    const uint64_t before = g_device_calls[slot].v.fetch_add(1 + (uint64_t(1) << 32), std::memory_order_acq_rel);
    d.slot = uint32_t(slot) + 1;
    d.epoch = uint32_t(before >> 32) + 1;
    d.alone = uint32_t(before) == 0;
  }
  d.device = device;
  d.start = rex::memory::load_and_swap<uint32_t>(base + device + 48);  // the last word written so far
  d.limit = rex::memory::load_and_swap<uint32_t>(base + device + 52);
  d.soft_limit = rex::memory::load_and_swap<uint32_t>(base + device + 56);
  d.submissions = direct::g_wptr_writes.load(std::memory_order_relaxed);
  // A recording device (flag 0x80 of +10940: the engine's command buffers) never submits its segments: they run as
  // indirect buffers of the real device's once recorded, so no kick makes them visible to the decoder early.
  d.recording = direct::g_whole.load(std::memory_order_relaxed) && (base[device + 10940] & 0x80) != 0;
  if (direct::g_whole.load(std::memory_order_relaxed)) {
    SegmentScope& t = t_segment;  // (the one thread-pointer read of the call: kept in d.scope for its end)
    d.scope = &t;
    t.device = device;
    t.start = d.start;
    t.limit = d.limit;
    t.soft = d.soft_limit;
    t.submissions = t.begin_submissions = d.submissions;
    t.switches = 0;
    t.part_count = 0;
    t.parts_lost = false;
  }
  return d;
}
}  // namespace

void NoteSegmentSwitch(const uint8_t* base, uint32_t device, uint32_t depth, uint32_t old_end) {
  if (!depth || !direct::g_whole.load(std::memory_order_relaxed)) return;
  SegmentScope& t = t_segment;
  if (t.device != device || device < 0x40000000 || device >= 0x7F000000) return;
  if (old_end > t.start) {  // (nothing written in the old segment: no part)
    if (t.part_count < SegmentScope::kParts) t.parts[t.part_count++] = {t.start, old_end, t.limit, t.soft};
    else t.parts_lost = true;
  }
  t.start = rex::memory::load_and_swap<uint32_t>(base + device + 48);
  t.limit = rex::memory::load_and_swap<uint32_t>(base + device + 52);
  t.soft = rex::memory::load_and_swap<uint32_t>(base + device + 56);
  t.submissions = direct::g_wptr_writes.load(std::memory_order_seq_cst);
  ++t.switches;
}

DrawDirect BeginDrawDirect(const uint8_t* base, uint32_t device, uint32_t& depth, uint32_t writer) {
  if (!direct::g_on.load(std::memory_order_relaxed)) return DrawDirect{};
  if (device >= 0x40000000 && device < 0x7F000000) NoteDevice(device);
  DrawDirect d = BeginDirect(base, device, depth);
  d.writer = writer;
  // banjo_d3d_records_native: the native draw path writes the record's operations into this device's staging area.
  if (d.device && d.alone && direct::g_native_ops.load(std::memory_order_relaxed) &&
      !direct::g_native_failed.load(std::memory_order_relaxed) &&
      ::me::native::g_ab_off_target.load(std::memory_order_relaxed) != 46u) {
    for (uint32_t i = 0; i < 8; ++i) {
      if (g_known_devices[i].load(std::memory_order_relaxed) != device) continue;
      d.ops.begin = direct::g_staging[i].get();
      d.ops.end = d.ops.begin + banjo::records::kStagingWords;
      break;
    }
  }
  return d;
}

DrawDirect BeginWriterDirect(const uint8_t* base, uint32_t r3, uint32_t& depth, uint32_t writer) {
  if (!direct::g_whole.load(std::memory_order_relaxed)) return DrawDirect{};  // banjo_d3d_records only
  DrawDirect d = BeginDirect(base, KnownDevice(r3) ? r3 : direct::g_main_device.load(std::memory_order_relaxed), depth);
  d.writer = writer;
  return d;
}

bool KickBegin(const uint8_t* base, uint32_t device, uint32_t list, uint32_t count) {
  if (!submit::g_on.load(std::memory_order_relaxed) || submit::g_failed.load(std::memory_order_relaxed)) return false;
  submit::g_kicks.fetch_add(1, std::memory_order_relaxed);
  if (::me::native::g_ab_off_target.load(std::memory_order_relaxed) == 47u) return false;  // A/B: the ring is parsed
  if (!count || count > submit::kPending || device < 0x40000000 || device >= 0x7F000000 || list < 0x10000 ||
      list >= 0xE0000000u - 8 * submit::kPending || (base[device + 10941] & 2)) {  // (the callback path: no packets)
    submit::g_other_path.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t at = rex::memory::load_and_swap<uint32_t>(base + device + 10952);
  const uint32_t mask = rex::memory::load_and_swap<uint32_t>(base + device + 14900);
  if (!submit::t_pending) submit::t_pending = new submit::Pending();
  submit::Pending& pending = *submit::t_pending;
  for (uint32_t k = 0; k < count; ++k) {
    submit::Entry& e = pending.entries[k];
    e.at = (at + 3 * k) & mask;
    e.after = (at + 3 * k + 3) & mask;
    e.length = rex::memory::load_and_swap<uint32_t>(base + list + 8 * k) & 0x00FFFFFFu;  // clrlwi 8
    e.address = rex::memory::load_and_swap<uint32_t>(base + list + 8 * k + 4);
    e.kind = 0;
  }
  pending.count = count;
  pending.wptr = (at + 3 * count) & mask;
  pending.on = true;
  return true;
}

void KickEnd() {
  if (submit::t_pending) submit::t_pending->on = false;
}

// sub_8263FD60 (r3 the device, r4 the words, r5 how many), before the original: its words as operations, queued at
// the ring position they will have (the next kick's write pointer submits them; the decoder does not reach them
// before). Words that are not whole packets of the record's kinds stay in the ring to be parsed.
void RingWords(const uint8_t* base, uint32_t device, uint32_t words, uint32_t count) {
  using namespace banjo::records;
  if (!submit::g_on.load(std::memory_order_relaxed) || submit::g_failed.load(std::memory_order_relaxed) ||
      ::me::native::g_ab_off_target.load(std::memory_order_relaxed) == 47u)
    return;
  if (!count || count > 64 || device < 0x40000000 || device >= 0x7F000000 || words < 0x10000 ||
      words >= 0xE0000000u - 4 * 64) {
    submit::g_words_left.fetch_add(count, std::memory_order_relaxed);
    return;
  }
  const uint8_t* const src = base + words;
  uint32_t ops[2 * 64 + 4];
  uint32_t* out = ops;
  const uint32_t at = rex::memory::load_and_swap<uint32_t>(base + device + 10952);
  const uint32_t mask = rex::memory::load_and_swap<uint32_t>(base + device + 14900);
  const uint32_t ring = rex::memory::load_and_swap<uint32_t>(base + device + 14896);
  for (uint32_t i = 0; i < count;) {
    uint32_t size = 0;
    Kind kind = Classify(src, i, count, size);
    if (kind == Kind::kPartial || kind == Kind::kOurs || kind == Kind::kBarrier) {
      submit::g_words_left.fetch_add(count, std::memory_order_relaxed);
      return;
    }
    if (kind == Kind::kRef) kind = Kind::kCopy;  // (by value: the ring's words are not patched)
    // The packet's address as the decoder has it: the ring's physical address of its position.
    out = Emit(src, direct::Physical(ring) + ((at + i) & mask) * 4 - i * 4, i, size, kind, out);
    i += size;
  }
  const uint32_t n = uint32_t(out - ops);
  submit::ProducerLock lock;
  const uint64_t head = submit::g_head.load(std::memory_order_relaxed);
  if (head + 1 - submit::g_tail.load(std::memory_order_acquire) > submit::kQueue ||
      submit::g_ops_head + n - submit::g_ops_tail.load(std::memory_order_acquire) > submit::kOps ||
      ((at + count) & mask) < at) {  // (a ring wrap inside the words: parsed)
    submit::g_words_left.fetch_add(count, std::memory_order_relaxed);
    return;
  }
  const uint32_t place = uint32_t(submit::g_ops_head & (submit::kOps - 1));
  for (uint32_t k = 0; k < n; ++k) submit::g_ops[(place + k) & (submit::kOps - 1)] = ops[k];
  submit::g_ops_head += n;
  submit::g_queue[head & (submit::kQueue - 1)] = {at & mask, (at + count) & mask, place, n, 1};
  submit::g_head.store(head + 1, std::memory_order_release);
  submit::g_words_queued.fetch_add(count, std::memory_order_relaxed);
}

void NoteMainDevice(uint32_t device) {
  if (device >= 0x40000000 && device < 0x7F000000 &&
      direct::g_main_device.load(std::memory_order_relaxed) != device) {
    direct::g_main_device.store(device, std::memory_order_relaxed);
    NoteDevice(device);
  }
}

namespace {
// Re-encodes the record's operations as packets and compares them with what Direct3D wrote (zero words, type-2
// fillers and NOPs aside): the decoder's own check at the draw, for the first records and every record in verify mode.
bool SameAsWritten(uint8_t* base, uint32_t first, uint32_t draw_at, const uint32_t* ops, size_t size) {
  size_t o = 0;
  for (uint32_t p = first; p < draw_at;) {
    const uint32_t h = rex::memory::load_and_swap<uint32_t>(direct::Guest(base, p));
    const uint32_t type = h >> 30;
    if ((type == 0 && !h) || type == 2) { p += 4; continue; }
    if (type == 3 && ((h >> 8) & 0x7F) == xenos::PM4_NOP) { p += 4 * (2 + ((h >> 16) & 0x3FFF)); continue; }
    if (o >= size) return false;
    const uint32_t tag = ops[o], kind = tag & 0xFF, count = tag >> 8;
    if (type == 0) {
      const uint32_t n = ((h >> 16) & 0x3FFF) + 1;
      if (kind != (((h >> 15) & 1) ? direct::kRunSingle : direct::kRun) || count != n || ops[o + 1] != (h & 0x7FFF))
        return false;
      for (uint32_t i = 0; i < n; ++i)
        if (ops[o + 2 + i] != rex::memory::load_and_swap<uint32_t>(direct::Guest(base, p + 4 + 4 * i))) return false;
      o += 2 + n;
      p += 4 * (1 + n);
    } else if (type == 1) {
      if (kind != direct::kType1 || ops[o + 1] != h ||
          ops[o + 2] != rex::memory::load_and_swap<uint32_t>(direct::Guest(base, p + 4)) ||
          ops[o + 3] != rex::memory::load_and_swap<uint32_t>(direct::Guest(base, p + 8)))
        return false;
      o += 4;
      p += 12;
    } else if (direct::ByReference((h >> 8) & 0x7F)) {
      const uint32_t n = ((h >> 16) & 0x3FFF) + 1;
      if (kind != direct::kType3Ref || count != n || ops[o + 1] != h || ops[o + 2] != direct::Physical(p)) return false;
      o += 3;
      p += 4 * (1 + n);
    } else {
      const uint32_t n = ((h >> 16) & 0x3FFF) + 1;
      if (kind != direct::kType3 || count != n || ops[o + 1] != h || ops[o + 2] != direct::Physical(p) ||
          std::memcmp(&ops[o + 3], direct::Guest(base, p + 4), size_t(n) * 4) != 0)
        return false;
      o += 3 + n;
      p += 4 * (1 + n);
    }
  }
  return o == size;
}

// The devices records are made on (report; the engine's recording threads), with their calls. g_producing held.
void NoteDeviceCall(uint32_t device) {
  using namespace direct;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t seen = g_devices[i].load(std::memory_order_relaxed);
    if (!seen) g_devices[i].store(device, std::memory_order_relaxed);
    if (!seen || seen == device) {
      Bump(g_device_draws[i], 1);
      break;
    }
  }
}

void EndDrawDirectLocked(uint8_t* base, const DrawDirect& d) {
  using namespace direct;
  NoteDeviceCall(d.device);
  const uint32_t end = rex::memory::load_and_swap<uint32_t>(base + d.device + 48);  // the last word written
  if (rex::memory::load_and_swap<uint32_t>(base + d.device + 52) != d.limit ||
      rex::memory::load_and_swap<uint32_t>(base + d.device + 56) != d.soft_limit) {  // another segment
    Bump(g_skip_segment, 1);
    return;
  }
  if (end <= d.start || end - d.start > 0x10000u) {
    Bump(g_skip_no_draw, 1);
    return;
  }
  const uint32_t first = d.start + 4;  // the first word the draw function wrote
  // The record is decoded in place, in the ring of records: contiguous, never waiting for the ring thread (its records
  // may be in command buffers not submitted yet); when there is no room, this draw keeps its packets. A packet's record
  // is at most twice its words (a one-word type-3 packet becomes four), plus the record's header.
  const uint32_t written = (end - first) / 4 + 1;
  const uint32_t room = kRecordHeader + 2 * written + 4;
  constexpr uint64_t mask = kRingWords - 1;
  const uint64_t pos = g_head.load(std::memory_order_relaxed);
  const uint64_t tail = g_tail.load(std::memory_order_acquire);
  uint64_t start_record = pos;
  if ((pos & mask) + room > kRingWords) start_record = (pos | mask) + 1;  // wrap
  if (start_record + room - tail > kRingWords || room > kRingWords / 4) {
    Bump(g_skip_full, 1);
    return;
  }
  uint32_t* const rec = &g_ring[start_record & mask];
  uint32_t ops = 0, region = 0;
  bool draw = false;
  const Decode result =
      DecodeRegion(Guest(base, first), Physical(first), written, rec + kRecordHeader, ops, region, draw);
  if (result == Decode::kUnknown) {
    Bump(g_skip_unknown, 1);
    return;
  }
  if (result == Decode::kNopHead) {
    Bump(g_skip_nop_head, 1);
    return;
  }
  if (result != Decode::kOk || !draw) {
    Bump(g_skip_no_draw, 1);
    return;
  }
  if (region < 3 || region - 1 > 0x4000) {
    Bump(g_skip_small, 1);
    return;
  }
  const uint32_t draw_at = first + region * 4;
  const uint32_t need = ops + kRecordHeader;
  // The decoder's own check (first records, every record in verify mode).
  if (g_checks_left.load(std::memory_order_relaxed) > 0 || g_verify_all) {
    g_checks_left.store(g_checks_left.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
    Bump(g_checked, 1);
    if (!SameAsWritten(base, first, draw_at, rec + kRecordHeader, ops)) {
      ::me::native::VerifyDifference();
      g_failed.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[native] DIFFERENCE: a direct-state record does not re-encode to the packets Direct3D wrote "
                   "(region {:08X}, {} words); banjo_direct_state off from now on (records already made are still "
                   "applied)", first, region);
      return;
    }
  }
  uint32_t seq = g_seq + 1;
  if (seq == kPad || seq == 0) seq = 1;
  rec[2] = Physical(first);
  std::memcpy(rec + 3, Guest(base, first), 12);  // the three words the marker replaces, guest byte order
  // From here to the marker's last word no submission may be published (g_marking, WriteMmio): checked once more now.
  g_marking.store(true, std::memory_order_seq_cst);
  if (g_wptr_writes.load(std::memory_order_seq_cst) != d.submissions) {
    g_marking.store(false, std::memory_order_release);
    Bump(g_skip_submitted, 1);
    return;  // the words decoded into the ring are not published: the next record overwrites them
  }
  if (start_record != pos) g_ring[pos & mask] = kPad;
  g_seq = seq;
  rec[0] = seq;
  rec[1] = need;
  g_head.store(start_record + need, std::memory_order_release);
  // The marker: one NOP covering the region; the old packets stay inside it as payload, the draw packet after it.
  uint8_t* marker = Guest(base, first);
  rex::memory::store_and_swap<uint32_t>(marker, 0xC0000000u | ((region - 2) << 16) | (xenos::PM4_NOP << 8));
  rex::memory::store_and_swap<uint32_t>(marker + 4, kMagic);
  rex::memory::store_and_swap<uint32_t>(marker + 8, seq);
  g_marking.store(false, std::memory_order_release);
  g_span_converted.Note(Physical(first));
  Bump(g_converted, 1);
  Bump(g_words_covered, region);
}

// banjo_d3d_records, game thread (g_producing held): everything the outermost call wrote, as records, one a piece
// (namespace direct above: barriers and unmarkable heads stay PM4). All pieces are decoded into the ring first; they
// are published together, with their markers, only if nothing was submitted meanwhile.
void EndWholeLocked(uint8_t* base, const DrawDirect& d, uint32_t part_end = 0) {
  using namespace direct;
  if (!part_end) NoteDeviceCall(d.device);
  // The last word written: the device's, or (a part in a segment the call left) the one before the switch.
  const uint32_t end = part_end ? part_end : rex::memory::load_and_swap<uint32_t>(base + d.device + 48);
  if (!part_end && (rex::memory::load_and_swap<uint32_t>(base + d.device + 52) != d.limit ||
                    rex::memory::load_and_swap<uint32_t>(base + d.device + 56) != d.soft_limit)) {  // another segment
    Bump(g_skip_segment, 1);
    return;
  }
  if (part_end && (end > d.limit + 96u || (d.start >= 0xE0000000u) != (end >= 0xE0000000u))) {  // (not that segment)
    Bump(g_skip_segment, 1);
    return;
  }
  if (end <= d.start) {
    Bump(g_calls_empty, 1);
    return;
  }
  if (end - d.start > 0x40000u) {
    Bump(g_skip_no_draw, 1);  // (not a sane region)
    return;
  }
  const uint32_t first = d.start + 4;  // the first word the call wrote
  const uint32_t written = (end - first) / 4 + 1;
  const uint8_t* const src = Guest(base, first);
  const uint32_t physical = Physical(first);
  constexpr uint64_t mask = kRingWords - 1;
  const uint64_t head = g_head.load(std::memory_order_relaxed);
  const uint64_t tail = g_tail.load(std::memory_order_acquire);
  struct Piece {
    uint64_t at;
    uint32_t begin, region, need, seq;
  };
  Piece pieces[kMaxPieces];
  uint32_t n = 0;
  uint64_t cursor = head;
  uint32_t i = 0;
  bool native = false;
  // banjo_d3d_records_native: the native draw path wrote the operations of the whole region (one piece).
  if (d.ops.cur) {
    uint32_t size = 0;
    const uint32_t count = uint32_t(d.ops.cur - d.ops.begin);
    const uint32_t need = kRecordHeader + count;
    uint64_t at = cursor;
    if ((at & mask) + need > kRingWords) at = (cursor | mask) + 1;
    // (The marker's three words: only packets the record copies may start there, as DecodePiece requires; a tile mask
    // there, which Direct3D patches after the call, would land on the marker. Then the region is decoded.)
    bool head = written >= 3 && written <= kMaxRegion;
    for (uint32_t k = 0; head && k < 3; k += size) head = MayStartAt(k, Classify(src, k, written, size));
    if (head && at + need - tail <= kRingWords) {
      uint32_t* const rec = &g_ring[at & mask];
      std::memcpy(rec + kRecordHeader, d.ops.begin, size_t(count) * 4);
      rec[1] = need;
      rec[2] = Physical(first);
      std::memcpy(rec + 3, src, 12);
      pieces[n++] = {at, 0, written, need, 0};
      cursor = at + need;
      i = written;
      native = true;
    } else {
      Bump(g_native_fallbacks, 1);
    }
  } else if (d.ops.begin) {
    Bump(g_native_fallbacks, 1);  // the native code could not write them all (or the original ran)
  }
  while (i < written) {
    uint32_t size = 0;
    const Kind kind = Classify(src, i, written, size);
    if (kind == Kind::kPartial) {
      Bump(g_words_partial, written - i);
      break;
    }
    if (kind != Kind::kCopy) {  // stays PM4 where it is
      Bump(kind == Kind::kBarrier ? g_words_barrier : g_words_head, size);
      i += size;
      continue;
    }
    if (n == kMaxPieces) {
      Bump(g_words_capped, written - i);
      break;
    }
    const uint32_t room = kRecordHeader + 2 * std::min(written - i, kMaxRegion) + 4;
    uint64_t at = cursor;
    if ((at & mask) + room > kRingWords) at = (cursor | mask) + 1;  // wrap
    if (at + room - tail > kRingWords) {
      Bump(g_skip_full, 1);
      Bump(g_words_capped, written - i);
      break;
    }
    uint32_t* const rec = &g_ring[at & mask];
    uint32_t* out = rec + kRecordHeader;
    const uint32_t stop = DecodePiece(src, physical, written, i, out);
    if (stop - i < 3) {  // too short for a marker (or one packet longer than a marker can cover)
      Bump(g_words_small, stop > i ? stop - i : size);
      i = stop > i ? stop : i + size;
      continue;
    }
    const uint32_t need = uint32_t(out - rec);
    rec[1] = need;
    rec[2] = Physical(first + i * 4);
    std::memcpy(rec + 3, src + size_t(i) * 4, 12);  // the three words the marker replaces, guest byte order
    pieces[n++] = {at, i, stop - i, need, 0};
    cursor = at + need;
    i = stop;
  }
  if (!n) return;
  // The producer's check (first records, every record in verify mode): the operations re-encode to the packets.
  if (g_checks_left.load(std::memory_order_relaxed) > 0 || g_verify_all ||
      (native && g_native_checked.load(std::memory_order_relaxed) < 4096)) {
    g_checks_left.store(g_checks_left.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
    if (native) Bump(g_native_checked, 1);
    for (uint32_t k = 0; k < n; ++k) {
      Bump(g_checked, 1);
      const uint32_t* rec = &g_ring[pieces[k].at & mask];
      if (!SamePiece(src + size_t(pieces[k].begin) * 4, pieces[k].region, rec + kRecordHeader,
                     pieces[k].need - kRecordHeader)) {
        ::me::native::VerifyDifference();
        if (native) {  // the native draw path's operations: those stop, the decoding goes on
          g_native_failed.store(true, std::memory_order_relaxed);
          REXLOG_ERROR("[native] DIFFERENCE: the native draw path's record operations (banjo_d3d_records_native) do "
                       "not re-encode to the packets it wrote (at {:08X}, {} words); the records are decoded from the "
                       "packets from now on", first, pieces[k].region);
          return;
        }
        g_failed.store(true, std::memory_order_relaxed);
        REXLOG_ERROR("[native] DIFFERENCE: a Direct3D record (banjo_d3d_records) does not re-encode to the packets "
                     "the call wrote (piece at {:08X}, {} words); no more records from now on (those made are still "
                     "applied)", first + pieces[k].begin * 4, pieces[k].region);
        return;
      }
    }
  }
  // From here to the last marker's last word no submission may be published (g_marking, WriteMmio). (A recording
  // device's segments are not submitted: a kick meanwhile does not matter to them.)
  g_marking.store(true, std::memory_order_seq_cst);
  if (!d.recording && g_wptr_writes.load(std::memory_order_seq_cst) != d.submissions) {
    g_marking.store(false, std::memory_order_release);
    Bump(g_skip_submitted, 1);
    return;  // the words decoded into the ring are not published: the next records overwrite them
  }
  uint32_t seq = g_seq;
  uint64_t prev = head;
  uint32_t covered = 0;
  for (uint32_t k = 0; k < n; ++k) {
    if (pieces[k].at != prev) g_ring[prev & mask] = kPad;
    if (++seq == kPad || seq == 0) seq = 1;
    g_ring[pieces[k].at & mask] = seq;
    pieces[k].seq = seq;
    if (!g_diag_writer.empty()) {
      g_diag_writer[seq & (kDiagSlots - 1)] = d.writer;
      auto& words = g_diag_words[seq & (kDiagSlots - 1)];
      const uint32_t n = std::min<uint32_t>(pieces[k].region, 47);
      std::memcpy(words.data(), src + size_t(pieces[k].begin) * 4, size_t(n) * 4);
      words[47] = n;
    }
    prev = pieces[k].at + pieces[k].need;
    covered += pieces[k].region;
  }
  g_seq = seq;
  g_head.store(prev, std::memory_order_release);
  if (g_zero_ahead) ReadyAhead(prev);
  for (uint32_t k = 0; k < n; ++k) {  // the markers: one NOP covering each piece; its old words stay as payload
    uint8_t* const marker = Guest(base, first + pieces[k].begin * 4);
    rex::memory::store_and_swap<uint32_t>(marker,
                                          0xC0000000u | ((pieces[k].region - 2) << 16) | (xenos::PM4_NOP << 8));
    rex::memory::store_and_swap<uint32_t>(marker + 4, kMagic);
    rex::memory::store_and_swap<uint32_t>(marker + 8, pieces[k].seq);
  }
  g_marking.store(false, std::memory_order_release);
  for (uint32_t k = 0; k < n && g_big_logged < 32; ++k) {  // (large records: which writer, where; a report)
    if (pieces[k].region < 1024) continue;
    ++g_big_logged;
    REXLOG_INFO("[native] Direct3D record {}: {} words at {:08X} by sub_{:08X} (device {:08X}{})", pieces[k].seq,
                pieces[k].region, Physical(first + pieces[k].begin * 4), d.writer, d.device,
                d.recording ? ", recording" : "");
  }
  g_span_converted.Note(physical);
  Bump(g_calls_converted, 1);
  Bump(g_pieces, n);
  Bump(g_words_covered, covered);
  if (native) Bump(g_native_records, 1);
}
}  // namespace

// banjo_direct_state, game thread, right after the original draw function: its state packets become a host record
// and one NOP packet (namespace direct above). The engine's recording threads convert one at a time.
void EndDrawDirect(uint8_t* base, const DrawDirect& call, uint32_t& depth) {
  using namespace direct;
  if (call.counted) --depth;
  if (!call.device) return;
  DrawDirect d = call;
  SegmentScope::Part old_parts[SegmentScope::kParts];
  uint32_t parts = 0;
  if (d.slot) {  // banjo_d3d_records: ended alone, and no other call began meanwhile (DeviceCalls)
    const uint64_t now = g_device_calls[d.slot - 1].v.fetch_sub(1, std::memory_order_acq_rel);
    if (uint32_t(now) != 1 || uint32_t(now >> 32) != d.epoch || !d.alone) {
      Bump(g_calls_overlapped, 1);
      return;
    }
  }
  if (d.scope) {  // the segment the call ended in (NoteSegmentSwitch)
    SegmentScope& t = *static_cast<SegmentScope*>(d.scope);
    if (t.device == d.device) {
      if (t.switches) {
        // The parts in the segments it left, when nothing was submitted during the call (a recording device).
        if (t.part_count && !t.parts_lost &&
            (d.recording || g_wptr_writes.load(std::memory_order_relaxed) == t.begin_submissions)) {
          parts = t.part_count;
          for (uint32_t k = 0; k < parts; ++k) old_parts[k] = t.parts[k];
        }
        d.start = t.start;
        d.limit = t.limit;
        d.soft_limit = t.soft;
        d.submissions = t.submissions;
        d.ops.cur = nullptr;  // (the native operations began in the old segment)
        Bump(g_calls_switched, 1);
      }
      t.device = 0;
    }
  }
  if (!d.recording && g_wptr_writes.load(std::memory_order_relaxed) != d.submissions) {  // the ring may be reading it
    g_skip_submitted.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (g_producing.test_and_set(std::memory_order_acquire)) {
    g_skip_busy.fetch_add(1, std::memory_order_relaxed);  // a lock wait (report)
    for (uint32_t spin = 0; g_producing.test_and_set(std::memory_order_acquire); ++spin) {
      if (spin > 64) rex::thread::MaybeYield();
    }
  }
  if (g_whole.load(std::memory_order_relaxed)) {
    for (uint32_t k = 0; k < parts; ++k) {  // (stream order: the segments the call left first)
      DrawDirect part = d;
      part.start = old_parts[k].start;
      part.limit = old_parts[k].limit;
      part.soft_limit = old_parts[k].soft;
      part.ops.cur = nullptr;
      EndWholeLocked(base, part, old_parts[k].end);
      Bump(g_parts_converted, 1);
    }
    EndWholeLocked(base, d);
  } else {
    EndDrawDirectLocked(base, d);
  }
  g_producing.clear(std::memory_order_release);
}

bool Enabled() {
  // Horizon does not launch an NRO with a useful process environment: the toml decides. Native by default;
  // masseffect_renderer_native = false + gpu_plugin = "xenos" runs ReXGlue's Xenos emulation instead.
  return REXCVAR_GET(masseffect_renderer_native);
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace me::native
