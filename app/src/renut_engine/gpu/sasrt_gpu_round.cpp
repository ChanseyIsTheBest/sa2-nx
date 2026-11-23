// sasrt-nx (2026-11-08): the GPU round after the move to the ultimate base (sasrt_gpu_round.h, docs/gpu-round-1108.md).
#include "renut_engine/gpu/sasrt_gpu_round.h"
#include "banjo_switch_default.h"  // banjo-nx (port 251): BANJO_SWITCH_DEFAULT

#include <fmt/format.h>
#include <rex/cvar.h>

#include "cpu/banjo_enhancements.h"
#include "renut_engine/gpu/ultimate.h"  // sasrt-nx (2026-11-11, port 82; its GPU list's row 4; UNTESTED): CullBothOn

REXCVAR_DEFINE_BOOL(sasrt_gpu_skip_inert_draws, true, "SASRT/GPU",
                    "Renut's renderer (GPU round of 2026-11-08, item 0, A/B 90): a draw that can write nothing (no colour "
                    "channel in its colour mask, no depth write, no stencil operation, no memory export in either "
                    "shader) is not drawn, and its targets are not bound for it. The race's two draws a frame into the "
                    "576x272 4x-multisampled depth target (depth test NEVER, nothing written) cost the console 1.85 ms a "
                    "frame with their target changes. Nothing the draw could change exists: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_precise_depth_resolve, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 1, A/B 91): with sasrt_deko3d_depth_resolve, a "
                    "compressed depth target is decompressed before a read or when it leaves the bind point only if a "
                    "draw, clear or copy wrote it since its last decompression; before, every bind marked it. The race's "
                    "lighting passes take the scene depth (1152x544) on and off dozens of times a frame and only test "
                    "it: each change decompressed it again. Decompressing tiles that are already plain changes nothing: "
                    "same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_plain_fetches, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 2, A/B 92): a pixel shader's texture fetches whose "
                    "fetch constants have no signs, no gamma and no exponent adjust for the draw are compiled without "
                    "that code (constant 926, a bit a fetch constant; tools/deko3d_glsl_opt.py pass plainfetch): the "
                    "shader skipped it for them anyway, but its temporaries counted in its registers. The race's "
                    "G-buffer shader 33 -> 28 GPRs and the final pass 42 -> 31 (32 or fewer: every warp slot of the SM "
                    "in use, to hide texture latency with). New pipelines, built in the background; until one is "
                    "built the draw uses the generic one. Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_alpha_spec, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 3, A/B 93): banjo-nx's alpha test specialization "
                    "(banjo_deko3d_alpha_spec, off here since sa2-nx saw flat colours on SA2 in Ryujinx) for this game: "
                    "the alpha test's function is the pixel shader's constant 903, so a draw without the test has no "
                    "compare and no discard, its shader no longer 'kills pixels', and the GPU tests depth before "
                    "shading instead of after: hidden pixels of opaque draws are not shaded. Checked in Ryujinx for "
                    "this game's race. Same pixels (the test passes every pixel when it is off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_gamma_upload_once, true, "SASRT/GPU",
                    "Renut's renderer (GPU round of 2026-11-08, item 4, A/B 94): the game's display gamma ramp "
                    "(sa2_renut_gamma_ramp) is copied to its texture when the game changes it and in the first 300 "
                    "frames (a frame's list can be dropped at start-up), not every frame: each copy was a copy-engine "
                    "upload between two barriers, the first a full one. kameo-nx's banjo_renut_gamma_upload_once for the "
                    "base's ramp code; this port's ramp code is sa2-nx's. The texture holds the same texels: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_decompress_merge, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 5, A/B 95): when a compressed depth target is "
                    "decompressed as the targets change (sasrt_deko3d_depth_resolve) and a barrier is already waiting to "
                    "be emitted (not a full one), the decompression is recorded first and the two barriers are one: "
                    "the waiting one, which went out before the decompression, and the one after it. The decompression "
                    "is ordered after the draws before it either way (as when nothing is waiting). Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_readback_barrier_merge, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 6, A/B 96): an image read back to a buffer for the "
                    "CPU (the race's 1x1 average luminance, every frame: sasrt_renut_resolve_readback) had its full "
                    "barrier before the copy emitted right after the full barrier of the texture's own move into the "
                    "copy layout, and its full barrier after the copy right before the full barrier of the move back: "
                    "four drains of the whole GPU with nothing between the pairs. Each pair is one barrier now (the "
                    "stronger wait and both invalidations, in the same place): two. Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_fetch_copy_compute, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 7, A/B 97): the copies the memory exports need are "
                    "made by a compute shader on the 3D engine (fetch_copy_cs) instead of the copy engine: each memexport "
                    "window's seed (its bytes from guest memory, before the export draw writes into it: one to three a "
                    "character, about 40 a frame in the race) and the fetch data of a draw that reads exported vertices "
                    "from memory. A wait for the 3D engine on each side instead of a full barrier (the whole GPU idle, the "
                    "copy engine with it); and the export draw no longer races the seed copy, nor a vertex shader its "
                    "fetch copy (the barriers after them waited for the 3D engine only, not the copy engine). The same "
                    "bytes in the same places: same pixels. The 10 s count: fetch data copies / seeded export draws")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_data_cache_invalidate, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 8, A/B 98): a barrier for a buffer (memory exports, "
                    "the exported streams' conversions, fetch data: about 100 a frame in the race) invalidates the "
                    "shaders' global-data and constant caches (Maxwell's InvalidateShaderCaches with those two bits), "
                    "no longer their instruction cache as well (deko3d's DkInvalidateFlags_Shader sets all three): a "
                    "buffer holds no shader code, and the shaders refetched theirs after every such barrier. New code "
                    "(a compiled variant) still invalidates it (the descriptor and code epochs). Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_one_x, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 9, A/B 99): a draw whose targets are 1x and whose "
                    "sampled textures all have scale code 0 (every gameplay draw in handheld mode; the HUD docked is "
                    "1.5x) has its pixel shader compiled with both folded (constant 926 bit 31; tools/deko3d_glsl_opt.py "
                    "pass onex, banjo-nx's scale1 and fragscale as a draw's choice): the scale factor's seven-way select "
                    "before a texel-coordinate fetch or an offset sample, and the reciprocal and multiplies of the pixel "
                    "position's scale. The final FXAA pass 150 -> 121 instructions. The same values: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_face_sign, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 10; no in-run A/B: the pipelines keep their keys, "
                    "compare with it off): the Xenos face register's sign in the pixel shaders that read it "
                    "(clamp(1 / Y, -FLT_MAX, FLT_MAX) >= 0, Y the pixel position times +1 or -1 by the facing) as Y's "
                    "sign bit (tools/deko3d_glsl_opt.py pass facesign, constant 928): a reciprocal (a quarter-rate "
                    "instruction), two clamps and a compare a pixel fewer in the race's most-drawn G-buffer shader and "
                    "nine others. For any Y that is not NaN, and it never is, the same answer: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_readback_compute, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 11, A/B 101): the memexport results the CPU reads "
                    "back (banjo_renut_batched_readbacks: one group at a list's end) are copied to the readback buffer by "
                    "fetch_copy_cs on the 3D engine instead of the copy engine: a wait for the 3D engine before instead of "
                    "a full barrier, then a wait and the GPU's L2 written back to memory for the CPU. The same bytes: same "
                    "pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renut-ultimate (2026-11-16, port 145): kept OFF although tw2de_nx runs it on (its console: prologue 23.7-23.8 ->
// 24.9-25.2 fps, GPU 40.4 -> 35.8-36.8 ms a frame): vivapinata-nx measured it slower (19.0 -> 17.7 fps) and sasrt-nx no
// better (docs/off-by-default.md, section 3a). Game by game.
REXCVAR_DEFINE_BOOL(sasrt_gpu_tiled_cache, false, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 12, A/B 102; OFF by default until a console run says "
                    "the picture is the same: Ryujinx does not emulate it): Maxwell's tiled caching (binning: the GPU "
                    "renders each screen tile's primitives together, keeping its colour and depth on chip), which NVN "
                    "uses and deko3d leaves off. The race's G-buffer (three targets and depth, overdraw) and its additive "
                    "lights (a read and a write of the 64-bit target a pixel per light) are the bandwidth it saves. "
                    "Same pixels by the hardware's design (each pixel's order is kept; deko3d's barriers and fences flush "
                    "the tiles; a full barrier gets a flush ahead of it)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_texel_readback_compute, true, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 13, A/B 103): the 1x1 float resolve the game's CPU "
                    "reads every frame (the race's average luminance, sasrt_renut_resolve_readback) is read back by a "
                    "compute shader on the 3D engine (texel_readback_cs: the texel's 32 bits as they are) instead of a "
                    "copy-engine copy: no move into and out of the copy layout, each with a full barrier (the whole GPU "
                    "idle), but a wait for the 3D engine and the L2 written back for the CPU. The same bits: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_lazy_depth_resolve, false, "SASRT/GPU",
                    "deko3d renderer (GPU round of 2026-11-08, item 14, A/B 104; OFF by default until a console run says "
                    "the picture is the same: Ryujinx emulates no depth compression): with sasrt_deko3d_depth_resolve and "
                    "sasrt_gpu_precise_depth_resolve, a written compressed depth target is decompressed right before "
                    "something reads it (bound alone for it, then the targets bound again), not each time it leaves the "
                    "bind point or a command list ends: the race's remaining 13 decompressions a frame, most of them of a "
                    "target bound again and written again before any read. The read sees decompressed tiles either way: "
                    "same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The second round (2026-11-08): items 15 and up, A/B 105 and up.
REXCVAR_DEFINE_BOOL(sasrt_gpu_seed_no_lead_wait, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round of 2026-11-08, item 15, A/B 105): the memexport seeds that "
                    "sasrt_gpu_fetch_copy_compute dispatches (guest memory into a draw's export windows, 46-63 a race "
                    "frame) start without the wait for the 3D engine before them. The windows they write are fresh: past "
                    "the slot's last window (256-byte aligned, no shared cache line), so no earlier draw, conversion or copy "
                    "of this list read or wrote those bytes; the wait after the seeds (before the export draw) stays and "
                    "orders everything before it. Same bytes, same pixels; one wait for idle fewer a seeded draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_convert_lead_wait, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 16, A/B 106): an exported stream conversion (a draw that "
                    "reads vertices a memexport draw wrote) skips its leading wait for the 3D engine when the command "
                    "list has recorded a buffer barrier (a wait for idle with the data caches dropped) since its last "
                    "memexport draw: the exports it reads are written and visible already, and it writes a fresh part of "
                    "the stream buffer. The wait after it (before the draw) stays. Same bytes, same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_identity_ramp, true, "SASRT/GPU",
                    "Renut renderer (second GPU round, item 17, A/B 107; deko3d and D3D12): when the game's display gamma "
                    "ramp is the linear one (every channel's entry i within a quarter of level i at 8 bits: the XDK's ramp "
                    "repeats i's bits, i << 2 | i >> 6, at most 0.185 off; sasrt_display_gamma 1) and the scan-out applies "
                    "no output gamma, the present shader writes the colour's 8-bit level i / 255 instead of looking the "
                    "ramp up (three dependent texture loads a pixel): the 8-bit target, rounding to the nearest level, "
                    "stores i either way. Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_convert_memo_keep, true, "SASRT/GPU",
                    "Renut renderer (second GPU round, item 18, A/B 108; both back ends, with banjo_renut_convert_reuse): "
                    "an exported stream conversion made earlier in the frame is used again by a later draw that needs the "
                    "same one (the same export window, offset, vertices, strides and elements) even when other memexport "
                    "draws ran in between: a new export writes a fresh window (another offset, so another key), never "
                    "one converted already. The conversions are forgotten as before when a window is written again in "
                    "place, a slot's exports complete (its windows are used again from 0), an export or stream buffer is "
                    "replaced, or the frame changes. banjo-nx's memo was cleared by every export (88 conversions and 11 "
                    "used again a race frame in Ryujinx). The same bytes: same pixels; a dispatch and its two waits fewer "
                    "a hit")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_depth_resolve_float_ps, true, "SASRT/GPU",
                    "Renut renderer (second GPU round, item 19, A/B 109; both back ends): a single-sampled depth resolve "
                    "whose words nothing reads (banjo_renut_depth_words_lazy: the race's shadow cascades and scene depth, "
                    "the words target bound and write-masked) is drawn by depth_resolve_float_ps, which loads the depth "
                    "and writes it, without the stencil load, the 24-bit / 20e4 encode and the words output the masked "
                    "target threw away. The float is the same load: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_same_targets_kept, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 20, A/B 110): the render targets are not bound again when the "
                    "framebuffer asked for is the one this command list bound last: the renderer asks again after every "
                    "compute dispatch (each memexport seed and stream conversion, about 120 a race frame: its trace) and "
                    "every resolve, and each bind rewrote the colour, depth and Zcull set-up of the same images. A "
                    "framebuffer's views never change and the one in use is alive; plume's own binds (the lazy depth "
                    "decompression) forget it. Same state: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_readback_spans, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 21, A/B 111; with sasrt_gpu_readback_compute): the memexport "
                    "windows read back at a list's end that follow one another in an export buffer (each starting where the "
                    "last ends, 256-byte aligned) are copied by one fetch_copy_cs dispatch, the padding between them with "
                    "them (bytes the CPU never reads: it takes the windows alone), not one dispatch a window (44 a race "
                    "frame). The windows' bytes are the same: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_seed_wait_only, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 22, A/B 112; with sasrt_gpu_fetch_copy_compute): the barrier "
                    "between a draw's memexport seeds and the export draw is a wait for the 3D engine alone, not a buffer "
                    "barrier that also drops the texture cache, the shaders' data caches and the constant cache (about 50 "
                    "times a race frame). The export draw only writes the windows; every later reader of them (the stream "
                    "conversions, the readback) has its own barrier that drops those caches. Same order: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_readback_fence_flush, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 23, A/B 113): the compute readbacks of round 1 "
                    "(sasrt_gpu_readback_compute, sasrt_gpu_texel_readback_compute: 4 a race frame) record no L2 flush "
                    "of their own (a wait for idle, the L2's dirty lines written out and every system-memory line of it "
                    "dropped: on the Tegra X1 that is the whole L2). The CPU reads those buffers only after waiting for the "
                    "slot's fence, which plume signals with deko3d's cache flush (SyncptAction FlushCache: the L2 written "
                    "out before the fence signals). The same bytes: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_convert_tail_vertex_only, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 24, A/B 114): the barrier between an exported stream "
                    "conversion and its draw (85 a race frame) waits for the 3D engine and drops the texture cache, as it "
                    "did, but not the shaders' global-data and constant caches: the stream buffer's only reader is the "
                    "draw's vertex fetch, which reads neither (its constants come in through the command buffer). Same "
                    "order, same bytes: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_convert_lead_data_only, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 25, A/B 115): the barrier before an exported stream "
                    "conversion (when sasrt_gpu_convert_lead_wait keeps it) waits for the 3D engine and drops the shaders' "
                    "global-data cache alone (the conversion loads the export windows as storage buffers), not the "
                    "texture cache and the constant cache: its push constants are fresh transient memory, and the constant "
                    "cache is dropped at every list's start for them (plume's freshComputeConstants). Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_clip_spec, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 26, A/B 116): a draw without a user clip plane (every draw of "
                    "this game so far: the shared constants' plane is zeros) gets its vertex shader without the clip "
                    "distance (constant 929 = 0, tools/deko3d_glsl_opt.py pass clipspec: no dot product with the plane "
                    "a vertex), and plume sets the hardware clip slot off for it (SET_USER_CLIP_ENABLE 0; rr6-nx's "
                    "banjo_deko3d_user_clip_enable turned slot 0 on for every draw). A zero plane gave every vertex a "
                    "distance of 0, which clips nothing: same pixels. A draw with a plane keeps both (PipelineKey bit 3)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_lean_barrier_skip, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 27, A/B 117): a barrier that only moves textures into a "
                    "colour or depth target layout (a fragment barrier, no cache dropped: banjo_deko3d_lean_barriers) is "
                    "not recorded when no draw, dispatch, copy or depth decompression was recorded since the last barrier "
                    "that waited for the fragments: the reads it would put before the writes are done already (the "
                    "trace's runs of target binds and clears at a pass's start, T12 BFC Cc T13 BFC Cc ...). Clears read "
                    "no texture and write in order with the draws. Same order: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_readback_barriers_trim, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 28, A/B 118; with sasrt_gpu_readback_compute): the memexport "
                    "readback at a list's end waits for the 3D engine dropping the global-data cache alone (its copies "
                    "load the windows as storage buffers), not the texture and constant caches, and records no barrier "
                    "after its copies: the list ends there, and the slot's windows are written again only after its "
                    "fence was waited for. Same bytes: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_export_raster_discard, true, "SASRT/GPU",
                    "deko3d renderer (second GPU round, item 29, A/B 119; with banjo_renut_export_only_draws): a draw "
                    "whose vertex shader only exports to memory (the race's skinning exports) is drawn with the rasterizer "
                    "off (3D method RasterizerEnable 0, then on again) as well as the empty scissor: the vertex shader "
                    "runs and exports as before; its primitives are dropped before clipping and triangle setup instead "
                    "of reaching the scissor. No pixel either way: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-11-08): the third GPU round (docs/gpu-round-1108.md "Round 3"; the user: "Try to aim for 20 more pixel
// identical gpu optimisations"). A/B 120 and up.
REXCVAR_DEFINE_BOOL(sasrt_gpu_seed_cpu_direct, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 30, A/B 120; with sasrt_gpu_fetch_copy_compute): a memexport "
                    "draw's fresh windows get their seed (guest memory's bytes there) written by the CPU straight into the "
                    "slot's export buffer as the draw is recorded, not copied into the upload ring and then by a compute "
                    "dispatch: no dispatch, and no wait between it and the export draw. The buffer is the GPU's and the "
                    "CPU's (uncached for the CPU); the windows are fresh (nothing recorded reads or writes them before), "
                    "the slot was last used before its fence, which wrote the GPU's caches out, and every queue flush drops "
                    "the GPU's L2 before the next one's work, as for the upload ring. The wait before the export draw stays "
                    "when a memexport draw of the same list has had no buffer wait since (a draw may read its windows). "
                    "Same bytes in the same places: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_convert_tail_no_texture_drop, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 31, A/B 121; with sasrt_gpu_convert_tail_vertex_only): the wait "
                    "after an exported stream conversion no longer drops the texture cache (deko3d's Image invalidate): the "
                    "conversion writes a part of the slot's stream buffer past everything written or read in the frame "
                    "(its 'used' end, 256-byte aligned), so no cache of the GPU holds a line of it; the slot was last used "
                    "frames ago, and every queue flush since dropped the texture and L2 caches (deko3d's postSubmitFlush). "
                    "The wait itself (all 3D work done, the stream in L2) stays. Same bytes read: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-21) turns it off for N&B in its own settings: two in-run A/Bs of 122 in Showdown Town (console
// c28, c34) had it on worse (render thread +0.6 % and +0.8 % a frame, fps -1.1 % and -0.8 %, GPU +0.3 %): the per-draw
// mask costs more CPU than the attribute fetches it saves on the GPU there. The base keeps it on (port 251; its rule
// for one port's finding: docs/off-by-default.md section 7).
REXCVAR_DEFINE_BOOL(sasrt_gpu_vs_input_mask, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 32, A/B 122): a pipeline's vertex attributes that the bound "
                    "vertex shader variant never reads (its shader program header's input map, read from the DKSH at load: "
                    "the outputs a pixel shader does not read, constant 905, take their inputs out with them) are bound as "
                    "constants, which the vertex attribute fetch does not read from memory. The shader reads the same "
                    "attributes as before: same pixels. banjo-nx turns it off for N&B (two A/Bs: on cost the render "
                    "thread more than it saved the GPU there)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_export_no_pixel_shader, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 33, A/B 123; with sasrt_gpu_export_raster_discard): an "
                    "export-only draw (its vertex shader only exports to memory; drawn with the rasterizer off and an empty "
                    "scissor) is built without its pixel shader, which never ran: its vertex shader then writes no output "
                    "for it (constant 905 = 0), its textures and constants are not set up, and its draws share one "
                    "pipeline per vertex shader and state. Not when the pixel shader exports itself. Same pixels (none)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_vs_screen_space_spec, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 34, A/B 124): every vertex shader's epilogue tests the draw's "
                    "screen-space flag (SharedConstants.screen_space, the viewport transform off) at run time; the draw's "
                    "value goes into the pipeline key (PipelineKey::fetch_types[0] bits 0-1 of a draw that is not "
                    "tessellated) and the shader gets it as constant 932 (tools/deko3d_glsl_opt.py pass vsepi): the "
                    "branch, and the scale and offset it reads, fold. A key not built yet draws with the generic pipeline. "
                    "The same value as the run-time test: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_vs_depth_remap_spec, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 35, A/B 125): the same for the epilogue's depth remap test "
                    "(SharedConstants.depth_remap[0] != 0; never on deko3d: a backwards depth range is the PC's): bits 2-3 "
                    "of the word, constant 932. Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_stacked_3d_spec, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 36, A/B 126): a pixel shader's 3D fetch picks at run time between "
                    "a stacked texture (a 2D array: two fetches and a blend between layers) and a volume texture, by the "
                    "draw's texture word (bit 31, kStackedTextureBit); the draw's bits for the fetch constants its pixel "
                    "shader samples go into the key (PipelineKey::fetch_types[1] of a draw that is not tessellated, bit 31 "
                    "set) and the shader gets them as constant 934 (pass stack3d): the path not taken and its size query "
                    "fold. Seven of this game's pixel shaders, the race's full-screen ED0A8D53 and the composite among "
                    "them. The same choice: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_stacked_filter_spec, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 37, A/B 127): the same for a stacked fetch's filter between "
                    "layers and its Z wrap (texture word bits 30 and 29): PipelineKey::fetch_types[2] and [3], constants 935 "
                    "and 936 (pass stack3d). Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_fetch_endian_spec, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 38, A/B 128): a vertex shader that fetches by address "
                    "(vfetchMemory: the race's skinning exports read their bone matrices so, 64 words a vertex) swaps each "
                    "word every way and picks the byte order at run time from the fetch slot's word "
                    "(SharedConstants.vertex_fetch_slots, bits 0-1); the draw's order for the slots its shader reads goes "
                    "into the key (PipelineKey::sasrt_words[4-6]) and the shader gets it as constants 937-939 "
                    "(tools/deko3d_glsl_opt.py pass fetchendian): one swap a word instead of four and three selects. "
                    "Renut's translator output, not this game's: any port's vertex fetches by address. Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_gpr_cap, false, "SASRT/GPU",
                    "OFF: SLOWER ON THE CONSOLE (2026-11-08, runs r3_bis2 / r3_nocap: the G-buffer pass 23.7 ms with the caps, 19.4 without; the 576x272 pass 1.20 / 1.09): the spills cost more than the occupancy gives. "
                    "deko3d renderer (third GPU round, item 39, A/B 129): pixel shaders 364C9BF8 (the light volumes, 1.75 ms "
                    "a race frame on the console), F646B7A8 and 92D1F7E4 use 34, 33 and 38 registers with the race's plain "
                    "fetches: an SM then holds 48 of its 64 warps of them. Compiled with 32 (constant 910, uam's register "
                    "cap: tools/gpr32_shaders.txt), it holds all 64 to hide texture latency with, for one or two registers "
                    "spilled to local memory (512 or 1024 bytes a warp). The same instructions' results: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_gpr_cap_filter, false, "SASRT/GPU",
                    "OFF: SLOWER ON THE CONSOLE (2026-11-08, runs r3_bis2 / r3_nocap: the G-buffer pass 23.7 ms with the caps, 19.4 without; the 576x272 pass 1.20 / 1.09): the spills cost more than the occupancy gives. "
                    "deko3d renderer (third GPU round, item 40, A/B 130): the 34-fetch full-screen filter 9041AFF6 (2.1 ms a "
                    "race frame on the console) uses 60 registers: 32 warps an SM. Compiled with 32 (constant 910) it gets "
                    "64, for about eight registers spilled (4 KB a warp of local memory). Whether that is faster is the "
                    "console's A/B to say (target 130). Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_export_format_spec, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 41, A/B 131): a vertex shader that exports to memory forms its "
                    "export address register from a float constant (eA = c<N> + the index), whose z word holds the export's "
                    "format and number flags; Renut's translator then packs every exported value through a switch over "
                    "the formats and a loop over the components with arrays indexed at run time (local memory: 6 KB a warp "
                    "in the race's skinning export shader). The draw's z word of up to three such registers (as the "
                    "constant buffer holds it: the literal's if the shader defines one, NaN-flushed as uploaded) goes "
                    "into the key (PipelineKey::sasrt_words[8-11]) and the shader gets it as constants 940-943 in place "
                    "of that operand (tools/deko3d_glsl_opt.py pass expfmt): the same operations on the same bits, which "
                    "uam folds to the one format's pack. Renut's translator output: any port's memory exports. Same bytes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(sasrt_gpu_texture_drop_tracked, false, "SASRT/GPU",
                    "OFF: WRONG ON THE CONSOLE (2026-11-08, run r3_all: the menu's 3D scene and sky washed out white; "
                    "bisected to this key, runs r3_bis1 / r3_bis2). The render targets are hardware-compressed, and the "
                    "texture-cache drop after rendering into one is needed whatever was sampled before (most likely the "
                    "texture unit's compression state). Kept for study. "
                    "deko3d renderer (third GPU round, item 42, A/B 132): a barrier that makes a texture readable (a "
                    "render target, a decompressed depth, a copy's destination, about 60 a race frame) dropped the texture "
                    "cache every time (deko3d: with a wait for idle before it, on a fragment barrier). The cache can hold "
                    "lines of an image only if a draw sampled it since the last drop: plume now notes, per texture, the "
                    "drop epoch it was last bound for sampling in (at each bind, and for everything still bound at each "
                    "drop that waits for the GPU), and the barrier drops the cache only when one of its textures was "
                    "sampled in the current epoch. Without the drop a fragment barrier waits for the fragments alone. The "
                    "same data read: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_target_wait_tracked, true, "SASRT/GPU",
                    "deko3d renderer (third GPU round, item 43, A/B 133; with sasrt_gpu_lean_barrier_skip): a barrier that "
                    "only moves textures into a colour or depth target layout (no cache dropped: a fragment barrier, about "
                    "20 a race frame) orders the earlier draws' reads of them before the new writes. Item 27 skipped it "
                    "when no work at all came since the last fragment wait; now it is skipped when none of its textures "
                    "was bound for sampling, copied, resolved or decompressed since that wait (plume: per texture, the "
                    "fragment-wait epoch of its last such use; everything bound is marked again at each wait). Nothing "
                    "then reads them while they are written. Same order of every read and write: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sasrt_gpu_read_wait_tracked, false, "SASRT/GPU",
                    "OFF (2026-11-08): it skips a read move only when sasrt_gpu_texture_drop_tracked (wrong on the console, "
                    "off) leaves no drop due, so it does nothing without it. "
                    "deko3d renderer (third GPU round, item 44, A/B 134): a barrier that makes a texture readable waits for "
                    "the fragments so the draws that wrote it are done before it is sampled. It now waits only when the "
                    "texture was written (bound as a target, cleared, copied or resolved into, decompressed) since the last "
                    "fragment wait, or its texture-cache drop is due (sasrt_gpu_texture_drop_tracked); with neither the "
                    "barrier records nothing. Plume notes per texture the fragment-wait epoch of its last write, and marks "
                    "the bound targets again at each wait. Same order of every write and read: same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-11-08, night): the fourth GPU round (the user: "our goal is getting down to 33 ms GPU time, pixel
// identical"). A/B 135 and up.
REXCVAR_DEFINE_BOOL(sasrt_gpu_export_window_ubo, true, "SASRT/GPU",
                    "deko3d renderer (fourth GPU round, item 46, A/B 136): a memexport vertex shader finds each export's "
                    "window in a loop over a local copy of the four windows (SharedConstants 61-64), indexed at run time, "
                    "so in local memory (192 bytes a thread in SASRT's skinning shader, whose draws cost ~180 ns a vertex). "
                    "With constant 944 (pass expwin) the loop reads the constant buffer itself: the same words, no local "
                    "memory. Needs the shader package and the DKSH cache made with pass expwin")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-09 night; port 58): plume_deko3d.h, Deko3DSetRegionBestFit.
REXCVAR_DEFINE_BOOL(sasrt_region_best_fit, true, "SASRT/GPU",
                    "The GPU region places a block in the smallest free range it fits (its bottom range last) instead of "
                    "the highest: fewer, larger free ranges (SASRT's run mem12: 200-300 MB free in 40-54 ranges, the "
                    "largest 28-55 MB, and a 27 MB target found no room)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-09 night; port 58): plume_deko3d.h, Deko3DSetImageBestFit.
REXCVAR_DEFINE_BOOL(sasrt_image_best_fit, true, "SASRT/GPU",
                    "An image goes to the smallest free range it fits among the image heap's shared chunks (the first "
                    "that fit before): fewer holes, and chunks that empty go back to the GPU region")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-18, for 1 % lows, every port): plume_deko3d.h, Deko3DSetImageSpareChunk.
REXCVAR_DEFINE_BOOL(blur_renut_image_spare_chunk, true, "SASRT/GPU",
                    "The image heap keeps one empty 32 MB chunk ready, made ahead on a thread of its own (the uam "
                    "thread's core and priority): a texture that finds no room in the chunks takes it, instead of "
                    "making a chunk where it is (20-38 ms on Blur's console, 4-5 times a race, the heap's lock held)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-09; port 58): plume_deko3d.h, Deko3DSetDedicatedTargets.
// sasrt-nx (2026-11-11, port 79; the stutter hunt, run slow2: 55 of the race's 60 frames over 60 ms followed a dynamic-resolution
// step, each step ~33 dedicated blocks made in one frame and the old ones destroyed two frames later). plume_deko3d.h,
// Deko3DSetTargetBlockCacheMb.
REXCVAR_DEFINE_INT32(sasrt_target_block_cache_mb, 160, "SASRT/GPU",
                     "MB of emptied render-target memory blocks kept for the next targets (a resolution step's new set "
                     "takes the last set's blocks instead of making ~33 new ones in one frame); given back at the memory "
                     "governor's low mark and when a block cannot be made. 0: off")
    .range(0, 1024)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_dedicated_targets, true, "SASRT/GPU",
                    "Render and depth targets of 1 MB or more in a memory chunk of their own, given back to the GPU region "
                    "(for buffers too) as soon as the target is freed, instead of a hole in a shared image chunk only "
                    "images can use (SASRT's docked race's resolution changes left ~500 MB of such holes)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 82; its GPU list's rows 1, 2 and 4; UNTESTED): pixel-identical by construction and
// universal, but run by no game yet (SASRT's race census: 0 cases of each). Each has its own key, on by default and
// read at start; not GPU round items (rows 1 and 2 are decided as a pipeline is built: no in-run A/B). Their counts are
// the 10 s "[renut] untested GPU rows" line (UntestedReport, below), so a port can see whether its game hits them.
// sasrt-nx (2026-11-11, port 82; its GPU list's row 1; UNTESTED): render/pipeline.cpp Build (StencilNoopOn).
REXCVAR_DEFINE_BOOL(sasrt_gpu_stencil_noop, true, "SASRT/GPU",
                    "UNTESTED ON THE CONSOLE (exact by construction; Beautiful Katamari hits it: the same frames with "
                    "it on and off on the PC and in Ryujinx): a stencil test that passes every "
                    "pixel (compare always) and cannot change the stencil plane (write mask 0, or keep on every "
                    "outcome that can happen) is built with the stencil off: the same pixels and the same stencil, "
                    "without the stencil work (the depth half of the rule is banjo-nx's)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 82; its GPU list's row 2; UNTESTED): plume_deko3d.h, Deko3DOptions::preciseStencilWrite
// (ApplyPlume).
REXCVAR_DEFINE_BOOL(sasrt_gpu_precise_stencil_write, true, "SASRT/GPU",
                    "UNTESTED (exact by construction; no game has hit it yet): a pipeline marks the depth target "
                    "written (its next read decompresses it) only when a depth write or a stencil change can happen: "
                    "no mark for a stencil write mask of 0 or for stencil operations only on outcomes that cannot "
                    "happen (fail when it compares always, depth-fail with no depth test that can fail). The same "
                    "depth and stencil values; fewer decompressions")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 82; its GPU list's row 4; UNTESTED): render/draw.cpp DrawStateLocked (SkipCullBothOn).
REXCVAR_DEFINE_BOOL(sasrt_gpu_skip_cull_both, true, "SASRT/GPU",
                    "UNTESTED (exact by construction; no game has hit it yet): a triangle draw that culls both "
                    "faces (with banjo_renut_cull_both, which builds such draws culling everything) and exports no "
                    "memory is left out: it draws no pixel either way")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sasrt_gpu_exp_sph_no_global_store, 0, "SASRT/GPU",
                     "EXPERIMENT (2026-11-09): vertex programs loaded with their header's DoesGlobalStore bit cleared (1), "
                     "and DoesLoadOrStore too (3): whether the store flag makes the GPU run SASRT's memexport vertex "
                     "shaders (~120 ns a vertex whatever they compute) one way that is slow");
REXCVAR_DEFINE_BOOL(sasrt_gpu_export_list_topology, false, "SASRT/GPU",
                    "deko3d renderer (fourth GPU round, item 47, A/B 137): a memexport draw recorded with the rasterizer "
                    "off (item 29) that is a point list is drawn as a triangle list when its count is a multiple of 3, "
                    "else as a line list when it is even: only its vertex shader runs, once for each vertex either way, "
                    "so the exports are the same and no pixel is drawn. SASRT's skinning exports are point lists of up "
                    "to 34,000 vertices that cost ~150 ns a vertex")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_export_no_point_gs, true, "SASRT/GPU",
                    "deko3d renderer (fourth GPU round, item 48, A/B 138): an export draw recorded with the rasterizer off "
                    "(item 29) that is a point list is built without Renut's point-sprite geometry shader, which made a "
                    "quad of every point for a rasterizer that is off. SASRT's skinning exports (~47,000 points a frame) "
                    "cost ~120 ns a point whatever their vertex shader did. The vertex shader runs once a point either way: "
                    "the same exports, and no pixel either way. Its vertex variant then gets constant 905 (no outputs)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-22, console run c44, N&B with SMAA on): A/B 135 gave GPU -1.1 % a draw (9 of 9 pairs lower),
// the render thread and fps flat, the town's picture right in 22 captures: on by default on the Switch.
REXCVAR_DEFINE_BOOL(sasrt_gpu_handover_with_smaa, BANJO_SWITCH_DEFAULT(true, false), "SASRT/GPU",
                    "deko3d renderer (fourth GPU round, item 45, A/B 135): banjo's colour hand-over "
                    "(banjo_deko3d_color_handover: a whole resolve into a texture of its size and format left in the "
                    "target's image, not copied) was off whenever SMAA was on, so every resolve was drawn since the SMAA "
                    "swap (+0.8 ms of resolves). Here SMAA runs only right after the composite draw, on that draw's first "
                    "target; any texture still sharing that target's image is settled (its resolve drawn) before SMAA "
                    "writes it, so the hand-over stays on with SMAA. Every texel is the one the copy would write: same "
                    "pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx's fifth GPU round (2026-11-09; rr6-nx's port of its pixel-identical items). Items 49 and 53 have no code
// here (49 is SASRT's own restore copy, A393B1E0; 53 is flat on its console): placeholders, so the A/B numbers stay
// SASRT's (139-143). Item 50 since port 52 (the base's EDRAM depth restore, port 29, for any game).
REXCVAR_DEFINE_BOOL(sasrt_gpu_depth_restore_direct, false, "SASRT/GPU", "SASRT only (fifth GPU round, item 49, A/B 139)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_depth_restore_whole_clear, true, "SASRT/GPU",
                    "Renut's renderer (fifth GPU round, item 50, A/B 140; port 52): the EDRAM depth restore "
                    "(sasrt_renut_depth_restore) cleared the stencil alone (depth kept) before drawing every pixel's depth "
                    "with the test off: a stencil-only clear keeps the depth, so it cannot be a fast clear. When the "
                    "restore's rectangle is the whole depth target, depth and stencil are cleared together (the depth to "
                    "0, then every pixel drawn over it, as before): the same depth and stencil 0 in every pixel")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_fetch_conversion_spec, true, "SASRT/GPU",
                    "deko3d renderer (fifth GPU round, item 51, A/B 141): a pixel shader's texture fetch applies Xenos' "
                    "signs, gamma and exponent adjust from the draw's words (SharedConstants: sampler bits 13-19, the 2D "
                    "texture word's bits 20-25) for each of the four channels, choosing among the conversions at run time; "
                    "item 3 (sasrt_gpu_plain_fetches) drops the block where a fetch constant has none of them. Here the "
                    "words of fetch constants 0-7 are constants of the draw (950-957, pass fetchconv), so the shader keeps "
                    "only the conversions the draw's texture has, on the channels that have them. The same operations on "
                    "the same values: the same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_fetch_exp_3d_spec, true, "SASRT/GPU",
                    "deko3d renderer (fifth GPU round, item 52, A/B 142): item 51 for the exponent adjust a 3D or cube fetch "
                    "reads from its own word (texture_indices[1] / [2] bits 20-25): constants 958-965 (3D) and 966-973 "
                    "(cube) for fetch constants 0-7 (pass fetchconv). Same values: the same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_gpu_convert_vertex_pass, false, "SASRT/GPU",
                    "SASRT only (fifth GPU round, item 53, A/B 143: flat on its console; no code here)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx's render-thread round (2026-11-09; port 54): items 54 and 55, A/B 144 and 145; target 200 switches both.
REXCVAR_DEFINE_BOOL(sasrt_rt_restore_candidates, true, "SASRT/RenderThread",
                    "Render thread (sasrt-nx's render-thread round, RT1; item 54, A/B 144): the EDRAM depth restore "
                    "(sasrt_renut_depth_restore) asks, for every draw with a depth target bound whose surfaces were written "
                    "since (the fast path's check; nearly every draw), which RGBA8 colour target over the same EDRAM base "
                    "and pitch was written after the depth (DepthRestoreSourceLocked). That scanned the whole target table "
                    "twice (to find the depth's own key, then every target; ~2.6 % of Sonic Transformed's render thread). "
                    "With this key each depth target keeps its own few candidates (same base and pitch, colour, one "
                    "sample, its scale, RGBA8 or BGRA8), found again only when a target is built or retired (an epoch of "
                    "its own): the same answer from the same write sequences")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_rt_cached_export_seeds, true, "SASRT/RenderThread",
                    "Render thread (sasrt-nx's render-thread round, RT5; item 55; chosen at start, so no in-run A/B: "
                    "target 145 does nothing): with sasrt_gpu_seed_cpu_direct (item 30) the CPU writes each fresh memexport "
                    "window's seed (its guest bytes) into the slot's export buffer, which was CPU-uncached memory: "
                    "memcpy's bulk loop at uncached speed was ~2 % of Sonic Transformed's render thread. With this key the "
                    "buffer is CPU-cached (plume: an upload buffer with HOST_CACHED, flushed at creation so no older dirty "
                    "line can land on the GPU's results) and each seed's lines are cleaned right after it is written, "
                    "before anything is submitted. The windows start on 256-byte boundaries, so no line holds two windows; "
                    "every line a seed touches is wholly its own: the GPU sees the same bytes. The CPU never reads the "
                    "buffer (readbacks go through a GPU copy). For games with memory exports")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>

#include "renut_engine/gpu/banjo_renut_record.h"  // banjo-nx (2026-11-21): StageActive
#endif

namespace sasrt::gpu_round {

Latched g_latched;
std::atomic<bool> g_latched_ready{false};
std::atomic<uint64_t> g_counts[kCount][2] = {};

void LatchNow() {
  using banjo::EnhancementActive;
  using banjo::EnhancementGroup;
  Latched l;
#define SASRT_GPU_ROUND_LATCH(name, key, what) l.on[name] = EnhancementActive(EnhancementGroup::kNonCpu, REXCVAR_GET(key));
  SASRT_GPU_ROUND_ITEMS(SASRT_GPU_ROUND_LATCH)
#undef SASRT_GPU_ROUND_LATCH
  g_latched = l;
  g_latched_ready.store(true, std::memory_order_release);
  RefreshEffective();
}

// banjo-nx (2026-11-20): On()'s answers (see the header): the latched key, and not this item's A/B "off" half.
uint8_t g_effective[kCount] = {};  // constant-initialised to 0: not latched
void RefreshEffective() {
  const int ab = banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed);
  for (int i = 0; i < kCount; ++i) {
    const bool on = g_latched.on[i] && ab != kAbBase + i && !(ab == kAbRtGroup && i >= kRtRestoreCandidates);
    g_effective[i] = on ? 2 : 1;
  }
}

// banjo-nx (2026-11-21, the render thread to 33 ms; A/B 173): plume's Deko3DOptions::cachedCommands (see there).
// Console, Showdown Town, in-run pairs (median, render thread a draw with it on): c36 +0.11 % (21 pairs), c39 +0.90 %
// (16); GPU -1.0 % and -0.9 % a draw (34 of 37 pairs lower). The ranges' clean at each list's end is the render
// thread's, and the render thread sets the frame rate in the town: off by default (a GPU-bound scene may want it).
REXCVAR_DEFINE_BOOL(banjo_deko3d_cached_commands, false, "Banjo/Enhancements",
                    "deko3d renderer (2026-11-21, A/B 173): command memory CPU-cached, each list's written ranges cleaned "
                    "to memory at its end (uncached before: every command word a store to the bus). The same commands. "
                    "Console c36/c39: GPU -1 % a draw, render thread +0.1/+0.9 % a draw (off: the town is render-bound)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-21, console run c43): with the core-3 command stage the list's end, and so the clean, is the
// stage's: A/B 173 then gave render thread -1.2 % a draw (10 of 10 pairs lower), GPU -1.4 %, all threads' CPU -1.8 %.
REXCVAR_DEFINE_BOOL(banjo_deko3d_cached_commands_with_stage, true, "Banjo/Enhancements",
                    "deko3d renderer: command memory CPU-cached (banjo_deko3d_cached_commands) whenever the core-3 "
                    "command stage plays the lists (banjo_deko3d_command_stage), its clean then off the render thread")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

void ApplyPlume() {
#if defined(PLUME_DEKO3D)
  {
    static const bool cached = REXCVAR_GET(banjo_deko3d_cached_commands);
    static const bool with_stage = REXCVAR_GET(banjo_deko3d_cached_commands_with_stage);
    plume::g_deko3dOptions.cachedCommands =
        (cached || (with_stage && banjo::renut_gpu::record::StageActive())) &&
        banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 173;  // banjo-nx (2026-11-21)
  }
  plume::g_deko3dCounts = banjo::renut_gpu::Latch().counts;  // banjo-nx (2026-11-20): plume's stats bumps
  plume::g_deko3dOptions.preciseDepthResolve = On(kPreciseDepth);
  plume::g_deko3dOptions.decompressMerge = On(kDecompressMerge);
  plume::g_deko3dOptions.readbackMerge = On(kReadbackMerge);
  plume::g_deko3dOptions.dataOnlyInvalidate = On(kDataInvalidate);
  plume::g_deko3dOptions.tiledCache = On(kTiledCache);
  plume::g_deko3dOptions.lazyDepthResolve = On(kLazyDepth);
  plume::g_deko3dOptions.sameTargetsKept = On(kSameTargets);
  plume::g_deko3dOptions.leanBarrierSkip = On(kLeanSkip);
  plume::g_deko3dOptions.freshComputeConstants = On(kSeedLead) || On(kConvertLead) || On(kConvertLeadData);
  plume::g_deko3dOptions.vsInputMask = On(kVsInputMask);  // sasrt-nx (2026-11-08): the third round
  plume::g_deko3dOptions.trackedImageInvalidate = On(kTexDropTracked);
  plume::g_deko3dOptions.trackedTargetWait = On(kTargetWaitTracked);
  plume::g_deko3dOptions.trackedReadWait = On(kReadWaitTracked);
  // sasrt-nx (2026-11-11, port 82; its GPU list's row 2; UNTESTED): read as each pipeline is built.
  plume::g_deko3dOptions.preciseStencilWrite = REXCVAR_GET(sasrt_gpu_precise_stencil_write);
  plume::g_deko3dOptions.expSphStoreBits = uint32_t(REXCVAR_GET(sasrt_gpu_exp_sph_no_global_store));  // an experiment
  plume::Deko3DSetDedicatedTargets(REXCVAR_GET(sasrt_dedicated_targets));  // sasrt-nx (2026-11-09; port 58)
  plume::Deko3DSetTargetBlockCacheMb(uint32_t(REXCVAR_GET(sasrt_target_block_cache_mb)));  // sasrt-nx (2026-11-11)
  plume::Deko3DSetRegionBestFit(REXCVAR_GET(sasrt_region_best_fit));      // sasrt-nx (2026-11-09 night; port 58)
  plume::Deko3DSetImageBestFit(REXCVAR_GET(sasrt_image_best_fit));        // sasrt-nx (2026-11-09 night; port 58)
  plume::Deko3DSetImageSpareChunk(REXCVAR_GET(blur_renut_image_spare_chunk));  // blur-nx (2026-11-18, 1 % lows)
#endif
}

const char *AbName(int target) {
  static const char *const kNames[] = {
#define SASRT_GPU_ROUND_NAME(name, key, what) what " (" #key ")",
      SASRT_GPU_ROUND_ITEMS(SASRT_GPU_ROUND_NAME)
#undef SASRT_GPU_ROUND_NAME
  };
  if (target == kAbRtGroup)  // sasrt-nx (2026-11-09; port 54)
    return "the render-thread round's items, all at once (sasrt_rt_*, A/B 200)";
  const int i = target - kAbBase;
  return i >= 0 && i < kCount ? kNames[i] : nullptr;
}

std::string Report() {
  static const char *const kKeys[] = {
#define SASRT_GPU_ROUND_KEY(name, key, what) #key,
      SASRT_GPU_ROUND_ITEMS(SASRT_GPU_ROUND_KEY)
#undef SASRT_GPU_ROUND_KEY
  };
  if (!g_latched_ready.load(std::memory_order_acquire))
    LatchNow();
  std::string text = "[sasrt gpu] GPU round (2026-11-08), in the last 10 s:";
#if defined(PLUME_DEKO3D)
  // plume's items count in its statistics (cumulative): their part since the last report.
  static uint64_t plume_last[64] = {};
  uint64_t plume_now[64] = {};
  for (int i = 0; i < 64; ++i)
    plume_now[i] = plume::GetDeko3DStats().sasrtRound[i].load(std::memory_order_relaxed);
#endif
  for (int i = 0; i < kCount; ++i) {
    uint64_t a = g_counts[i][0].exchange(0, std::memory_order_relaxed);
#if defined(PLUME_DEKO3D)
    if (i < 64) {
      a += plume_now[i] - plume_last[i];
      plume_last[i] = plume_now[i];
    }
#endif
    const uint64_t b = g_counts[i][1].exchange(0, std::memory_order_relaxed);
    text += fmt::format(" {} {}{}{}", kKeys[i], g_latched.on[i] ? "" : "(off) ", a, b ? fmt::format("/{}", b) : "");
    text += i + 1 < kCount ? ',' : '.';
  }
  return text;
}

// sasrt-nx (2026-11-11, port 82; its GPU list's rows 1, 2 and 4; UNTESTED): the keys as read at their first use, the
// counts (sasrt-nx's census names) and their 10 s line.
bool StencilNoopOn() {
  static const bool on = REXCVAR_GET(sasrt_gpu_stencil_noop);
  return on;
}
bool SkipCullBothOn() {
  static const bool on = REXCVAR_GET(sasrt_gpu_skip_cull_both) && banjo::renut_gpu::CullBothOn();
  return on;
}
std::atomic<uint64_t> g_census_stencil_noop{0}, g_census_cull_both{0}, g_census_cull_both_skipped{0};

std::string UntestedReport() {
  const auto off = [](bool on) { return on ? "" : " off"; };
#if defined(PLUME_DEKO3D)
  const uint64_t marks = plume::g_deko3dPreciseStencilSaved.load(std::memory_order_relaxed);
#else
  const uint64_t marks = 0;  // row 2 is the deko3d back end's
#endif
  // The game thread's (StatsGameFrame, under its stats mutex): the draws left out since the last line.
  static uint64_t skipped_last = 0;
  const uint64_t skipped = g_census_cull_both_skipped.load(std::memory_order_relaxed);
  const uint64_t skipped_recent = skipped - skipped_last;
  skipped_last = skipped;
  return fmt::format("[renut] untested GPU rows (port 82; sasrt-nx's rows 1, 2 and 4), since the start: {} no-op "
                     "stencil pipelines built off (sasrt_gpu_stencil_noop{}), {} depth marks dropped "
                     "(sasrt_gpu_precise_stencil_write{}, deko3d), {} cull-both pipelines (banjo_renut_cull_both{}), "
                     "{} cull-both draws skipped (sasrt_gpu_skip_cull_both{}; {} in the last 10 s)",
                     g_census_stencil_noop.load(std::memory_order_relaxed), off(StencilNoopOn()), marks,
                     off(REXCVAR_GET(sasrt_gpu_precise_stencil_write)),
                     g_census_cull_both.load(std::memory_order_relaxed), off(banjo::renut_gpu::CullBothOn()), skipped,
                     off(SkipCullBothOn()), skipped_recent);
}

}  // namespace sasrt::gpu_round
