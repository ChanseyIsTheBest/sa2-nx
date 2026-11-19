// renut-ultimate: the switches of the fixes ported into this base from the other ports (see ultimate.h).
#include "renut_engine/gpu/ultimate.h"
#include "renut_engine/gpu/sasrt_gpu_round.h"  // sasrt-nx's GPU rounds (port 49): PlainFetchesOn reads item 2

#include <rex/cvar.h>

// rr6-nx (2026-11-05, 44466322; proved on the console: 3,179,991 -> 3,413,348 samples passed in a frame against the
// emulator's 3,413,797).
REXCVAR_DEFINE_INT32(banjo_deko3d_user_clip_enable, 1, "Banjo/Enhancements",
                     "deko3d renderer (from rr6-nx): the GPU's user clip enable, bit i for clip distance i, written at the start of "
                     "every command list. deko3d never writes it, and on the console every slot then clips: a vertex shader that "
                     "writes only gl_ClipDistance[0] has its triangles cut by its unwritten outputs (holes, missing sky, broken "
                     "cables, a chrome car in Ridge Racer 6). Ryujinx clips only with what a shader writes, so it never shows. "
                     "1: slot 0, the only one the shaders write. -1: left as deko3d leaves it. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// kameo-nx (2026-11-04, 776227708d): the coloured ground patches of Kameo's Badlands.
REXCVAR_DEFINE_BOOL(banjo_renut_const_nan_flush, true, "Banjo/Enhancements",
                    "Float constants that are NaN or infinity reach the shaders as 0 (from kameo-nx, the rule of Kameo: RePowered). "
                    "The console's GPU multiplies anything by 0 to 0 and the host's keeps the NaN: a shader multiplying a register "
                    "the game never wrote as a float by 0 got NaN (coloured patches). Off: the words as they are")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// rr6-nx (2026-11-03, f21694b6): torn meshes and wedges on deko3d.
REXCVAR_DEFINE_BOOL(rr6_strip_markers, true, "Banjo/Enhancements",
                    "From rr6-nx: an indexed strip's restart markers (the all-ones index of the guest format) are kept out of the "
                    "vertex range a draw converts and rewritten to the host format's marker, as the console's GPU cuts the strip "
                    "there. Off, as Renut had it: the marker counted as vertex 65535, the range became the buffer's tail, the host "
                    "indices 32-bit, and 0xFFFF in them restarts nothing on deko3d or Vulkan: torn meshes and wedges. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// rr6-nx (2026-11-03, 9f33ee3f): Ridge Racer 6's bloom and lit surfaces.
REXCVAR_DEFINE_BOOL(rr6_fetch_over_pointer, true, "Banjo/Enhancements",
                    "From rr6-nx: a draw after SetTexture(NULL) samples the texture its fetch constant still holds, as the GPU "
                    "does (it reads the fetch constant, never the device's pointer, and SetTexture(NULL) leaves the constant as it "
                    "was). Off, as Renut had it: the null texture (Ridge Racer 6: its second bloom blur read black and lit surfaces "
                    "came out about a quarter too dark). Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renut-ultimate (port 63, 2026-11-10; vivapinata-nx's finding: the title logo's dark shadow).
REXCVAR_DEFINE_BOOL(renut_fetch_owner_same_data, true, "Banjo/Enhancements",
                    "With rr6_fetch_over_pointer: a slot after SetTexture(NULL) keeps its last texture only while the fetch "
                    "constant still describes that texture's data (format, address, size, dimension); a fetch of other data is "
                    "sampled as the fetch alone (sasrt_renut_texture_fetch_only), so the old texture's content (a resolve's) is "
                    "kept. Off: the last texture whatever the fetch names (port 04 as it was). Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renut-ultimate (port 64, 2026-11-10; sasrt-nx's GPU hang: docs/handoff/gpu_hang_1110 there).
REXCVAR_DEFINE_BOOL(sasrt_texture_scale_size_check, true, "Banjo/Enhancements",
                    "From sasrt-nx: a texture over a depth resolve whose image is of another size than its dynamic-resolution "
                    "code's (codes 4 and 6 change their ratio at each step) is made again at its bind; before, its depth words "
                    "were copied into it past its end, and the GPU could hang. false = as before (tests only). Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// kameo-nx (2026-11-04, 71d0abaf; the user's console run: exploded far-away models, broken portraits).
REXCVAR_DEFINE_BOOL(banjo_renut_resolve_clears_both, true, "Banjo/Enhancements",
                    "From kameo-nx: a resolve's clear flags clear both surfaces they name, as on the console (an imposter renderer "
                    "resolving colour with 0x300 expects the depth cleared too). Off clears only the surface resolved")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// kameo-nx (2026-11-03, d4a836ec): found in all 52 of Kameo's 5,861 shaders whose GLSL declares a sampler3D.
REXCVAR_DEFINE_BOOL(banjo_renut_fetch3d_on_2d, true, "Banjo/Renut",
                    "From kameo-nx: a shader's tfetch3D on a 2D fetch constant samples the texture, as the GPU does (the slot is "
                    "bound stacked, which the 3D table's stacked path samples). Off: the 3D table's null texture (Kameo lost the "
                    "sun's glow on its clouds)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// kameo-nx (2026-11-04, 2b378904; the audit against Kameo: RePowered, MIT).
REXCVAR_DEFINE_BOOL(banjo_renut_cull_both, true, "Banjo/Enhancements",
                    "From kameo-nx: PA_SU_SC_MODE_CNTL with both cull bits set culls every face, as on the console (the vertex "
                    "shader still runs). Off draws both faces, as Renut did. Direct3D 12 has no such mode and draws both")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// rr6-nx (2026-11-03, 42516bee).
REXCVAR_DEFINE_BOOL(rr6_depth_bias, true, "Banjo/Enhancements",
                    "From rr6-nx: the game's depth bias (polygon offset) reaches the GPU as dynamic state, set when its words change; "
                    "no value of it makes a pipeline. The units are the emulated GPU's (slope in 1/16 subpixels, the constant in the "
                    "depth format's units rounded away from 0). Off, as Renut had it: no bias (decals z-fight). Needs dynamic depth "
                    "bias (Vulkan, deko3d); Direct3D 12 draws without. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-21; the console's shadow maps).
REXCVAR_DEFINE_BOOL(sasrt_deko3d_depth_resolve, true, "Banjo/Enhancements",
                    "From sasrt-nx: a depth target with hardware compression (banjo_deko3d_depth_compression) is "
                    "decompressed (dkCmdBufResolveDepthValues) before it is read as a texture or copied. Off: a sampled "
                    "depth target reads compressed tiles as garbage on the console (Sonic Transformed's shadow maps: black "
                    "ground in whole cascades). Ryujinx shows nothing either way")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-11-04).
REXCVAR_DEFINE_BOOL(sasrt_deko3d_copy_wait, true, "Banjo/Enhancements",
                    "From sasrt-nx: on deko3d, a texture or render target leaving a copy layout (an upload, a copy, a "
                    "resolve copy) moves with a full barrier, which waits for the copy engine, before the 3D engine reads "
                    "or writes it. Off: a fragment barrier, which waits for the 3D engine only (a draw could read a "
                    "texture the copy engine was still writing; console only)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-20).
REXCVAR_DEFINE_BOOL(sasrt_renut_deko3d_clear_new, true, "Banjo/Enhancements",
                    "From sasrt-nx: on deko3d a new colour render target is cleared to 0 once, as the PC back ends give "
                    "it (deko3d images come uncleared: a pass reading what was never drawn read another image's "
                    "leftovers). Off: as Renut had it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-20); pacman-nx (2026-11-02) and halo3-nx as banjo_renut_volume_stacked.
REXCVAR_DEFINE_BOOL(sasrt_renut_deko3d_volume_as_array, true, "Banjo/Enhancements",
                    "From sasrt-nx: on deko3d a single-level volume (3D) texture of two slices or more is held as a 2D "
                    "array of its slices and sampled through the shaders' stacked path (colour-grading LUTs), not as a "
                    "deko3d 3D image. The patched Ryubing's copy engine ignores Z (every slice lands on slice 0). The "
                    "same picture on the console, two fetches for one where a LUT is read. Off: a 3D image")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// vivapinata-nx (2026-11-03, 6eecb25a).
REXCVAR_DEFINE_BOOL(banjo_deko3d_vs_kill_and, true, "Banjo/Enhancements",
                    "From vivapinata-nx: on deko3d a vertex kill drops a primitive only when all its vertices are killed, "
                    "as the GPU and D3D12 do (constant 914: the pixel shader discards; needs a shader package made with "
                    "deko3d_glsl_opt.py's vskill pass). Off: any killed vertex drops the primitive (Viva Pinata's ground "
                    "layers ended in hard steps). Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// vivapinata-nx (2026-11-03, fe12c548).
REXCVAR_DEFINE_BOOL(banjo_renut_export_only_draws, true, "Banjo/Enhancements",
                    "From vivapinata-nx: a multipass vertex shader's draw (export mode 7) rasterizes nothing, as on the "
                    "Xbox 360: it only exports to memory. Off, as Renut had it: drawn (Viva Pinata's white undercoat on "
                    "distant creatures)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// vivapinata-nx (2026-11-03, fe12c548).
REXCVAR_DEFINE_BOOL(banjo_renut_vertex_finite, true, "Banjo/Enhancements",
                    "From vivapinata-nx: single 32-bit float vertex elements have infinities and NaNs made the largest "
                    "finite value: on the Xenos 0 times anything is 0, so they make no NaN there. Off, as Renut had it "
                    "(Viva Pinata's distant creatures draw dark)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// vivapinata-nx (2026-11-04, e242c74b).
REXCVAR_DEFINE_BOOL(vivapinata_edram_clear_overlap, true, "Banjo/Enhancements",
                    "From vivapinata-nx: a colour clear that covers every tile of a target at the same EDRAM base in another "
                    "layout (samples, pitch) also clears that target as far as a resolve can tell, as on the console: done "
                    "only when a resolve reads it before anything writes to it (Viva Pinata's fades blank their mask this "
                    "way). Off, as Renut had it: the layouts kept apart")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// rr6-nx (2026-11-02, 5da4b39f).
REXCVAR_DEFINE_BOOL(rr6_depth_covers_colour, true, "Banjo/Enhancements",
                    "From rr6-nx: a depth target smaller than the colour target bound over it grows to that size, once "
                    "(Ridge Racer 6 draws 1440x800 over a depth surface made 1280x720, which the console's EDRAM allows). "
                    "Off: black bars on Direct3D 12 and device lost on Vulkan. Runs only when the targets change")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// keflings-nx (2026-10-30, cd71f403).
REXCVAR_DEFINE_BOOL(keflings_renut_missing_input_w1, true, "Banjo/Enhancements",
                    "From keflings-nx: vertex shader inputs the vertex declaration does not provide read (0, 0, 0, 1), as a "
                    "missing component does (D3D), instead of zeros (Keflings' models read a COLOR0 alpha they do not "
                    "declare). Off: zeros, as Renut had it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// halo3-nx (2026-11-02, ca6c7fc8).
REXCVAR_DEFINE_BOOL(halo3_renut_loop_constants, true, "Banjo/Enhancements",
                    "From halo3-nx: the device's loop constants (i0-i31) reach the shaders (shared constants 1360-1487), "
                    "for the loops a shader does not define itself (Halo 3's materials loop on counts set per draw). Off: "
                    "not written (such loops read zeros)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// rr6-nx (2026-11-03, 45bbd0e09c) and vivapinata-nx (f346dd4963), for every vertex shader here.
REXCVAR_DEFINE_BOOL(rr6_user_clip_plane, true, "Banjo/Enhancements",
                    "From rr6-nx and vivapinata-nx: the game's user clip plane 0 (SetClipPlane, PA_CL_CLIP_CNTL bit 0) reaches the "
                    "vertex shaders, as the GPU clips with it (Ridge Racer 6's car reflections, Viva Pinata's water). Every vertex "
                    "shader made by this base writes the distance; a draw without a plane gives zeros. Off: zeros always (no clipping)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-20); halo3-nx's gamma fetch (094a0011) is the same curve.
REXCVAR_DEFINE_BOOL(sasrt_renut_texture_signs, true, "Banjo/Enhancements",
                    "From sasrt-nx and halo3-nx: a texture component the fetch constant marks gamma reads through Xenos' "
                    "piecewise-linear gamma to linear, and one marked unsigned biased as 2x - 1, in the shader after "
                    "filtering, as the emulated GPU does (gamma textures read as linear made Halo 3 far too bright). Off: as "
                    "the host format reads them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_renut_texture_exp_adjust, true, "Banjo/Enhancements",
                    "From sasrt-nx: a texture fetch constant's exponent adjust: the fetched value times 2^adjust, as the "
                    "console's fetch scales its result (Sonic Transformed's FXAA reads its input with -1 and -2: white "
                    "outlines without). Off: ignored")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_renut_texture_int_format, true, "Banjo/Enhancements",
                    "From sasrt-nx: a 3D, cube or stacked fetch whose fetch constant asks for the integer number format "
                    "reads a 16- or 8-bit normalized texture as its integers (0..65535, 0..255), as the hardware does "
                    "(Sonic Transformed's lighting volumes). Off: normalized")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// halo3-nx (2026-11-03, 094a0011).
REXCVAR_DEFINE_BOOL(halo3_renut_target_gamma_bias, true, "Banjo/Enhancements",
                    "From halo3-nx: a colour target of the guest's gamma format has its writes gamma-encoded (Xenos' piecewise-"
                    "linear curve), and a target's colour exponent bias scales the alpha written, as the GPU does (Vulkan and "
                    "deko3d; Halo 3's menu was nearly transparent without the bias). Off: written as the host format takes them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-17, the menu backdrop): with halo3_renut_target_gamma_bias, the colour channels scaled by the target's
// colour exponent bias as well (shader_common.h banjoTrim, constant 927; render/pipeline.cpp).
REXCVAR_DEFINE_BOOL(blur_renut_target_bias_rgb, true, "Banjo/Enhancements",
                    "A colour target's exponent bias (RB_COLOR_INFO) scales the colour a pixel shader writes as well as its "
                    "alpha, as the GPU scales all of it (Vulkan and deko3d; needs halo3_renut_target_gamma_bias). Blur's menu "
                    "backdrop was black without it: its blur passes draw into targets of bias 5. Off: the alpha alone, as "
                    "halo3-nx did")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-20).
REXCVAR_DEFINE_BOOL(sasrt_renut_fixed16_float, true, "Banjo/Enhancements",
                    "From sasrt-nx: Xenos' 16-bit fixed-point render targets (k_16_16, k_16_16_16_16; -32..32) as FP16. Off: "
                    "SNORM, which clips at 1 (Sonic Transformed's HDR scene: no highlights, the exposure washes the frame out)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_renut_fixed16_clamp, true, "Banjo/Enhancements",
                    "From sasrt-nx: a resolve of a 16-bit fixed-point target (held as FP16) clamps to -32..32, the range the "
                    "console stores (unclamped, Sonic Transformed's highlights fed the bloom and exposure: pale and hazy)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-20). renut-ultimate (2026-11-20, port 202): OFF by default under the rule of 2026-11-11
// (docs/off-by-default.md section 1). S4ep1-nx (github.com/FurryLilManChild/S4ep1-nx, GPL-3.0, its 36c65f40) found it
// draws Sonic 4's title sky white: its reflection pass draws a resolved depth back as colour into EDRAM the main pass's
// depth target shares, and taken as the depth every sky and island draw above the horizon failed; its PC A/B with it
// off gives the console's picture. Katamari never reaches it. sasrt-nx, which it was made for, sets it in its toml.
REXCVAR_DEFINE_BOOL(sasrt_renut_depth_restore, false, "Banjo/Enhancements",
                    "From sasrt-nx: the EDRAM depth restore: a depth target bound over the EDRAM a 32-bit colour target wrote "
                    "since takes its words as its depth, as on the console, where they are one memory (a game drawing a "
                    "resolved depth back as colour to restore it). Off: the depth target keeps its own")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-20).
REXCVAR_DEFINE_BOOL(sasrt_renut_reverse_depth_remap, true, "Banjo/Enhancements",
                    "From sasrt-nx: a viewport whose depth range runs backwards (MinZ 1, MaxZ 0) on the PC's back ends: the "
                    "host viewport spans 0-1 and the vertex shader writes the guest's depth (Direct3D 12 cannot take a "
                    "backwards range). deko3d takes it as it is. Off: the range as the game gives it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// kameo-nx (2026-11-04, fbbcdd3a).
REXCVAR_DEFINE_INT32(banjo_renut_split_frame_mb, 192, "Banjo/Enhancements",
                     "From kameo-nx: a frame that has written this much of its upload ring is split: what it drew so far "
                     "finishes on the GPU and the next ring slot takes over, nothing presented. The same picture. 0: off, and "
                     "a big level's first frames can run the console out of memory (deko3d)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-21).
REXCVAR_DEFINE_BOOL(sasrt_renut_texture_keep_on_signs, true, "Banjo/Enhancements",
                    "From sasrt-nx: a texture bound through a fetch constant that differs from the one it was made with only "
                    "in the component signs (gamma, biased, unsigned; not all-signed against not) is kept: the signs are "
                    "applied when sampling (sasrt_renut_texture_signs). Off: retired and made and uploaded again")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-21).
REXCVAR_DEFINE_BOOL(sasrt_renut_constant_f4_pending, true, "Banjo/Enhancements",
                    "From sasrt-nx: constants a game writes through D3DDevice_BeginShaderConstantF4 stay pending for the draw "
                    "(the original clears their bits: the ring has them), so a worker's recorded draw takes them from its own "
                    "device instead of the title device's. Needs the game's hook calling KeepConstantF4Pending (guest/"
                    "command_buffers.h). Off: as the original leaves them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-30).
REXCVAR_DEFINE_BOOL(sasrt_renut_resolve_readback, true, "Banjo/Enhancements",
                    "From sasrt-nx: a resolve into a 1x1 32-bit float texture (an average scene luminance) is also copied back "
                    "to its guest memory when its frame's GPU work is done, where the game's CPU locks and reads it (Sonic "
                    "Transformed's glow scales by it). Off: the CPU reads what the memory held (0 there: every glow 20x too dark)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-21).
REXCVAR_DEFINE_BOOL(sasrt_renut_exports_held, true, "Banjo/Enhancements",
                    "From sasrt-nx: memexport results (GPU-skinned meshes) are kept on the host after their write-back to "
                    "guest memory, and a vertex stream inside one reads them: recorded draws of them can replay a frame "
                    "after the export, when the game has already cleared that memory for its next export (Sonic "
                    "Transformed's meshes collapsing to a point). Off: guest memory")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-10-20, 2026-10-29).
REXCVAR_DEFINE_BOOL(sasrt_renut_texture_fetch_only, true, "Banjo/Enhancements",
                    "From sasrt-nx: a texture fetch constant set without SetTexture (no header pointer in the device: how "
                    "Sonic Transformed binds its textures) is sampled as the GPU reads it, its own texture keyed by its "
                    "base address. Off: such a slot samples the null texture")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_renut_texture_memo_fetch_only, true, "Banjo/Enhancements",
                    "From sasrt-nx: a texture slot bound by its fetch constant alone (sasrt_renut_texture_fetch_only) is "
                    "answered by the texture memo, keyed by its fetch words, instead of a bind at every draw. Same "
                    "words (the verify switch below). Off: every such slot bound at every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sasrt_renut_texture_memo_fetch_only_verify, false, "Banjo/Enhancements",
                    "Diagnostics (sasrt_renut_texture_memo_fetch_only): every memo answer for a slot without a header "
                    "pointer is bound again and its words compared; the counts are logged every 262,144 answers and the "
                    "first differences in full")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renut-ultimate (audit, 2026-11-07).
REXCVAR_DEFINE_BOOL(ultimate_renut_replay_inherit_same, true, "Banjo/Enhancements",
                    "renut-ultimate: a replay whose running device holds every word the recording did not set as the "
                    "recording had it keeps the record's answers (resource objects, shadow sort, packet keys, slot "
                    "extents), which banjo_renut_replay_inherits turns off for every replay otherwise. The same device "
                    "either way. Off: every replay with banjo_renut_replay_inherits takes the slow path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renut-ultimate (audit, 2026-11-07).
REXCVAR_DEFINE_BOOL(ultimate_renut_held_exports_guard, true, "Banjo/Enhancements",
                    "renut-ultimate: a kept export (sasrt_renut_exports_held) that the game's CPU has written other "
                    "data over since its write-back is read from guest memory again; a clear (zeros, or one vertex "
                    "repeated) still reads the kept results. Off: the kept results for four frames, as in sasrt-nx")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renut-ultimate (port 44, 2026-11-08): sasrt-nx's design of the same day (its GPU round, item 2), measured by rr6-nx on
// the console: port 25's per-fetch sign path folded away per pipeline where the draw's fetch constants have none of it.
// Ridge Racer 6's race on the ultimate renderer drew 3.2 ms a frame more on the GPU without it (its pixel shaders weighted
// by GPU time 3.8 times the instructions: about 120 more per texture fetch, and more registers); with it, its game passes
// are as rr6-nx's master build draws them (main pass 7.89 ms against 7.89). Since sasrt-nx's GPU rounds came in (port 49,
// 2026-11-08, night): the key is the round's item 2 (sasrt_gpu_round.cpp defines it; A/B target 92), read through it.

// renut-ultimate (port 47, 2026-11-08): rr6-nx's lock-free answer to "is this address one of Renut's objects".
REXCVAR_DEFINE_BOOL(rr6_records_lockfree, true, "Banjo/Enhancements",
                    "From rr6-nx: records::Lookup and OwnsHeader answered from a table over the record heap, "
                    "without locks (a byte per 16-byte granule, marked under the existing locks). Ridge Racer 6's "
                    "drawing thread spent about 30 % of its time on the console in the read-write lock's unlock "
                    "behind them. The same answers. Off: the locked set and map")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renut-ultimate (2026-11-08): sasrt_deko3d_depth_resolve's decompress only when something reads the target.
REXCVAR_DEFINE_BOOL(ultimate_deko3d_depth_resolve_lazy, true, "Banjo/Enhancements",
                    "renut-ultimate: with sasrt_deko3d_depth_resolve, a compressed depth target leaving the bind point (or "
                    "bound at the end of a list) is decompressed only when something reads it: when it next moves to a "
                    "read layout, or before it is bound through another view. The same pixels; a game that reads no "
                    "depth pays nothing (Ridge Racer 6 paid 0.46 ms a frame on the console for the eager form). Off: "
                    "decompressed at every unbind, as in sasrt-nx")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-11-11, port 75; its formats batch, docs/360-precision-differences.md there): where the renderer kept
// more than the Xenos did at a speed cost, or less than it keeps. Universal (any game's targets and textures of these
// formats). Rows 6, 8, 10, 74, 93.
REXCVAR_DEFINE_BOOL(renut_format_10bit_exact, true, "Banjo/Enhancements",
                    "From sasrt-nx: Xenos 2_10_10_10 render targets and textures in the host's 10:10:10:2 format (the "
                    "console's own 32 bits a pixel, the same values) instead of 16 bits a channel; their resolves "
                    "follow. Off: 16 bits a channel, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 75; its formats batch, found on the way).
REXCVAR_DEFINE_BOOL(renut_format_edram_as_variants, true, "Banjo/Enhancements",
                    "From sasrt-nx: the Xenos target formats 2_10_10_10_AS_10_10_10_10 and 2_10_10_10_FLOAT_AS_16_16_16_16 "
                    "as 10:10:10:2 (16 bits a channel without renut_format_10bit_exact) and FP16; off: 8-bit RGBA, as "
                    "before (less than the console keeps)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 75; its formats batch, rows 7, 66, 73, 95, 96).
REXCVAR_DEFINE_BOOL(renut_format_7e3_rg11b10, false, "Banjo/Enhancements",
                    "From sasrt-nx, NOT exact: Xenos 7e3 targets (2_10_10_10_FLOAT) as 11:11:10 float, half FP16's "
                    "bytes. No alpha (the 7e3's 2 bits read as 1) and one mantissa bit less on red and green, two on "
                    "blue; more range. Off: FP16")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 75; its formats batch, row 26).
REXCVAR_DEFINE_BOOL(renut_depth_float_no_stencil, false, "Banjo/Enhancements",
                    "From sasrt-nx, a speed trade: Xenos float depth targets (D24FS8) as 32-bit float depth without a "
                    "stencil plane (half the bytes of D32F_S8). Stencil on them then does nothing: the first uses are "
                    "logged (renut_diag_formats). Off: D32F with stencil")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 75; its formats batch, row 38).
REXCVAR_DEFINE_BOOL(renut_float32_point_filter, true, "Banjo/Enhancements",
                    "From sasrt-nx: 32-bit float textures point-sampled whatever the sampler asks, as the Xenos (which "
                    "cannot filter them) samples them; off: filtered linearly when asked")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-11, port 75; its formats batch, the diagnostic).
REXCVAR_DEFINE_BOOL(renut_diag_formats, true, "Banjo/Enhancements",
                    "From sasrt-nx, diagnostic, logging only: the first targets and textures of each format a session "
                    "makes (guest and host formats), the first stencil use on each depth target, the first float "
                    "textures asked for linear filtering")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// dishonored-nx (2026-10-31; port 104): tiled textures uploaded with an all-zero level watched until their data comes in.
REXCVAR_DEFINE_BOOL(dishonored_renut_zero_level_recheck, true, "Banjo/Enhancements",
                    "From dishonored-nx: a tiled texture with a level whose guest data was all zeros at its upload (the "
                    "game streams the small levels in after the first bind, where the sampled content check does not "
                    "look) has its whole data hashed every 8 frames, at most 24 textures a frame, until it changes, then "
                    "is uploaded again (black walls and heads at a distance in Dishonored). Off: the sampled check only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// dishonored-nx (2026-10-30; port 101): vertex fills and fetch data copies read only committed guest memory.
REXCVAR_DEFINE_BOOL(dishonored_renut_vertex_readable_check, true, "Banjo/Enhancements",
                    "From dishonored-nx: a vertex stream fill or fetch data copy whose range runs past the guest memory "
                    "the game committed converts the committed part and zeros the rest, where the read faulted the render "
                    "thread before (the Xbox 360's GPU reads whatever is there). Each fill asks the heap's page table (its "
                    "lock and a walk over the range's pages). Off: the whole range read as before. The Switch only (the "
                    "PC's heaps are not a plain offset from the base)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// dishonored-nx (2026-10-31; port 106): a 2_10_10_10_FLOAT (7e3) target's pixel shader output clamped to the 7e3 range.
REXCVAR_DEFINE_BOOL(dishonored_renut_clamp_7e3, true, "Banjo/Enhancements",
                    "From dishonored-nx: a pixel shader drawing into a 2_10_10_10_FLOAT (7e3) target with exponent bias 0 "
                    "clamps that output to the 7e3 range (colour 0 to 31.875, alpha 0 to 1, NaN to 0), as the Xenos stores "
                    "it. The host target is 16-bit float, which kept negative light passes (black walls in Dishonored) and "
                    "highlights far above the range (a washed-out frame). Needs the shader package with the clamp7e3 pass "
                    "(constant 990). Blending past the range is not clamped. Off: the output as the shader computes it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::renut_gpu {
int Deko3DUserClipEnable() { return REXCVAR_GET(banjo_deko3d_user_clip_enable); }
bool ConstNanFlushOn() {
  static const bool on = REXCVAR_GET(banjo_renut_const_nan_flush);
  return on;
}
bool StripMarkersOn() {
  static const bool on = REXCVAR_GET(rr6_strip_markers);
  return on;
}
bool FetchOverPointerOn() {
  static const bool on = REXCVAR_GET(rr6_fetch_over_pointer);
  return on;
}
bool FetchOwnerSameDataOn() {
  static const bool on = REXCVAR_GET(renut_fetch_owner_same_data);
  return on;
}
bool TextureScaleSizeCheckOn() {
  static const bool on = REXCVAR_GET(sasrt_texture_scale_size_check);
  return on;
}
bool ResolveClearsBothOn() {
  static const bool on = REXCVAR_GET(banjo_renut_resolve_clears_both);
  return on;
}
bool Fetch3DOn2DOn() {
  static const bool on = REXCVAR_GET(banjo_renut_fetch3d_on_2d);
  return on;
}
bool CullBothOn() {
  static const bool on = REXCVAR_GET(banjo_renut_cull_both);
  return on;
}
bool DepthBiasOn() {
  static const bool on = REXCVAR_GET(rr6_depth_bias);
  return on;
}
bool Deko3DDepthResolveLazyOn() {
  static const bool on = REXCVAR_GET(ultimate_deko3d_depth_resolve_lazy);
  return on;
}
bool Deko3DDepthResolveOn() {
  static const bool on = REXCVAR_GET(sasrt_deko3d_depth_resolve);
  return on;
}
bool Deko3DCopyWaitOn() {
  static const bool on = REXCVAR_GET(sasrt_deko3d_copy_wait);
  return on;
}
bool Deko3DClearNewOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_deko3d_clear_new);
  return on;
}
bool Deko3DVolumeAsArrayOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_deko3d_volume_as_array);
  return on;
}
bool VsKillAndOn() {
  static const bool on = REXCVAR_GET(banjo_deko3d_vs_kill_and);
  return on;
}
bool ExportOnlyDrawsOn() {
  static const bool on = REXCVAR_GET(banjo_renut_export_only_draws);
  return on;
}
bool VertexFiniteOn() {
  static const bool on = REXCVAR_GET(banjo_renut_vertex_finite);
  return on;
}
bool EdramClearOverlapOn() {
  static const bool on = REXCVAR_GET(vivapinata_edram_clear_overlap);
  return on;
}
bool DepthCoversColourOn() {
  static const bool on = REXCVAR_GET(rr6_depth_covers_colour);
  return on;
}
bool MissingInputW1On() {
  static const bool on = REXCVAR_GET(keflings_renut_missing_input_w1);
  return on;
}
bool LoopConstantsOn() {
  static const bool on = REXCVAR_GET(halo3_renut_loop_constants);
  return on;
}
bool UserClipPlaneOn() {
  static const bool on = REXCVAR_GET(rr6_user_clip_plane);
  return on;
}
bool TextureSignsOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_texture_signs);
  return on;
}
bool TextureExpAdjustOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_texture_exp_adjust);
  return on;
}
bool TextureIntFormatOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_texture_int_format);
  return on;
}
bool TargetGammaBiasOn() {
  static const bool on = REXCVAR_GET(halo3_renut_target_gamma_bias);
  return on;
}
bool TargetBiasRgbOn() {  // blur-nx (2026-11-17)
  static const bool on = REXCVAR_GET(halo3_renut_target_gamma_bias) && REXCVAR_GET(blur_renut_target_bias_rgb);
  return on;
}
bool Fixed16FloatOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_fixed16_float);
  return on;
}
bool Fixed16ClampOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_fixed16_clamp);
  return on && Fixed16FloatOn();
}
bool DepthRestoreOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_depth_restore);
  return on;
}
bool ReverseDepthRemapOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_reverse_depth_remap);
  return on;
}
int SplitFrameMb() {
  static const int mb = REXCVAR_GET(banjo_renut_split_frame_mb);
  return mb;
}
bool TextureKeepOnSignsOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_texture_keep_on_signs);
  return on && TextureSignsOn();
}
bool ConstantF4PendingOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_constant_f4_pending);
  return on;
}
bool ResolveReadbackOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_resolve_readback);
  return on;
}
bool ExportsHeldOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_exports_held);
  return on;
}
bool TextureFetchOnlyOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_texture_fetch_only);
  return on;
}
bool TextureMemoFetchOnlyOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_texture_memo_fetch_only) && TextureFetchOnlyOn();
  return on;
}
bool TextureMemoFetchOnlyVerifyOn() {
  static const bool on = REXCVAR_GET(sasrt_renut_texture_memo_fetch_only_verify) && TextureMemoFetchOnlyOn();
  return on;
}
bool ReplayInheritSameOn() {
  static const bool on = REXCVAR_GET(ultimate_renut_replay_inherit_same);
  return on;
}
bool HeldExportsGuardOn() {
  static const bool on = REXCVAR_GET(ultimate_renut_held_exports_guard);
  return on;
}
bool PlainFetchesOn() {
  return sasrt::gpu_round::On(sasrt::gpu_round::kPlainFetches);  // sasrt_gpu_plain_fetches, the GPU round's item 2
}
bool RecordsLockFreeOn() {
  static const bool on = REXCVAR_GET(rr6_records_lockfree);
  return on;
}
bool Format10BitExactOn() {
  static const bool on = REXCVAR_GET(renut_format_10bit_exact);
  return on;
}
bool FormatEdramAsVariantsOn() {
  static const bool on = REXCVAR_GET(renut_format_edram_as_variants);
  return on;
}
bool Format7e3Rg11b10On() {
  static const bool on = REXCVAR_GET(renut_format_7e3_rg11b10);
  return on;
}
bool DepthFloatNoStencilOn() {
  static const bool on = REXCVAR_GET(renut_depth_float_no_stencil);
  return on;
}
bool Float32PointFilterOn() {
  static const bool on = REXCVAR_GET(renut_float32_point_filter);
  return on;
}
bool DiagFormatsOn() {
  static const bool on = REXCVAR_GET(renut_diag_formats);
  return on;
}
bool ZeroLevelRecheckOn() {
  static const bool on = REXCVAR_GET(dishonored_renut_zero_level_recheck);
  return on;
}
bool VertexReadableCheckOn() {
  static const bool on = REXCVAR_GET(dishonored_renut_vertex_readable_check);
  return on;
}
bool Clamp7e3On() {
  static const bool on = REXCVAR_GET(dishonored_renut_clamp_7e3);
  return on;
}
}  // namespace banjo::renut_gpu
