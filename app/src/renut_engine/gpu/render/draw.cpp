/**
 * @file    gpu/render/draw.cpp
 */
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/stage_census.h"  // blur-nx (2026-11-17, the Renut audit's census)
#include "renut_engine/gpu/host_batch.h"  // blur-nx (2026-11-18, the Renut host batch)
#include "renut_engine/gpu/render/smaa.h"
#include "renut_engine/gpu/render/ui_scale.h"  // banjo-nx (2026-10-21, internal resolution)  // banjo-nx (2026-10-21): banjo_renut_smaa

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <vector>

#include <plume_render_interface.h>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/system/xmemory.h>
#if defined(__SWITCH__)
#include <unistd.h>  // banjo-nx (2026-10-28): sbrk (the heap's room, AdvanceStreamCacheFrameLocked)
extern "C" char *fake_heap_end;  // libnx: the end of the malloc heap
#endif

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/line_compare.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/render/shader_library.h"  // banjo-nx: ShaderBoolSpecMask
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/render/vertex_format.h"
#include "renut_engine/gpu/backend/video_internal.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/sasrt_gpu_round.h"  // sasrt-nx (2026-11-08): the GPU round
#include "renut_engine/gpu/ultimate.h"
// renut-ultimate: the host shaders this file makes itself (sasrt-nx's 16-bit fixed-point resolve).
#include "host_shaders/resolve_clamp_ps.spirv.h"
#include "host_shaders/depth_restore_ps.spirv.h"
#include "host_shaders/smaa_vs.spirv.h"  // port 179 (banjo_renut_edram_carry): the UI upscale's bilinear pass
#include "host_shaders/ui_upscale_ps.spirv.h"
#if defined(PLUME_DEKO3D)
#include "host_shaders/resolve_clamp_ps.deko3d.h"
#include "host_shaders/depth_restore_ps.deko3d.h"
#include "host_shaders/smaa_vs.deko3d.h"
#include "host_shaders/ui_upscale_ps.deko3d.h"
#endif
#if defined(_WIN32)
#include "host_shaders/resolve_clamp_ps.dxil.h"
#include "host_shaders/depth_restore_ps.dxil.h"
#include "host_shaders/smaa_vs.dxil.h"
#include "host_shaders/ui_upscale_ps.dxil.h"
#endif
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // banjo-nx (2026-10-17): Deko3DBindMemExport
#endif
#include "renut_engine/gpu/banjo_renut_record.h"  // banjo-nx (package 28b)
#include <rex/cvar.h>
// sasrt-nx (2026-11-08): renut-ultimate's keys this file reads (defined in gpu/ultimate.cpp).
REXCVAR_DECLARE(bool, sasrt_renut_texture_memo_fetch_only);
REXCVAR_DECLARE(bool, sasrt_renut_texture_memo_fetch_only_verify);

// banjo-nx (2026-10-17, a diagnostic for the deko3d renderer's missing river water): draws that read memexport results
// as vertices take them through the CPU path (wait for the export, read it back) instead of stream_convert_cs.
REXCVAR_DEFINE_BOOL(banjo_renut_diag_no_stream_convert, false, "Banjo/Renut",
                    "Diagnostic: exported streams reach their draws through the CPU (no stream_convert_cs)");
// banjo-nx (2026-10-17, a diagnostic for the depth of field that banjo_gpu_blend_skip breaks): pixel shaders (their
// 16-digit hashes, comma separated) left out of the blend skip.
REXCVAR_DEFINE_STRING(banjo_renut_diag_stream_dump_vs, "", "Banjo/Renut",
                      "Diagnostic: log the vertex streams of draws with this vertex shader (hex hash): guest and "
                      "converted words of the first vertices ([streamdump], first 16 binds)");
REXCVAR_DEFINE_INT32(banjo_renut_diag_draw_log_s, 0, "Banjo/Renut",
                     "Diagnostic: this many seconds after the first draw, log every draw of the next two frames "
                     "([drawlog]: shaders, counts, target, state; 0 = off)");
REXCVAR_DEFINE_STRING(banjo_renut_diag_blend_skip_exclude, "", "Banjo/Renut",
                      "Diagnostic: pixel shader hashes (hex, comma separated) banjo_gpu_blend_skip leaves alone");

// banjo-nx (2026-10-20, from sa2-nx): see DrawStateLocked.
REXCVAR_DEFINE_BOOL(banjo_renut_chain_reuse, true, "Banjo/Enhancements",
                    "ON again since 2026-10-26 (incremental draw state; in-run A/B target 20 turns it off for a half): "
                    "its checks are one AND each, nothing is copied for a texture miss, and the replayed draws give "
                    "hints. It was off since package 36r: the console's 36q profile (render thread) had DrawStateLocked "
                    "9.5 -> 12.6 % and BindTargets +0.8 %, its checks costing more than they saved. Renut's renderer (with banjo_renut_snapshot_deltas): a draw of the delta chain takes the previous "
                    "draw's pipeline key when nothing the key is made from changed (the device's render state chunks, "
                    "the shaders, the declaration, the targets, the primitive) instead of making it again")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_chain_reuse_verify, false, "Banjo/Enhancements",
                    "Tests (banjo_renut_chain_reuse): the key is made anyway and compared with the one taken; a "
                    "difference is logged and the made one used")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// SLOWER ON THE CONSOLE when it had an effect (package 36q: PushSharedConstants 2.7 -> 4.9 % of the render thread); no
// effect since 2026-10-23.
REXCVAR_DEFINE_BOOL(banjo_renut_inline_line_compare, false, "Banjo/Enhancements",
                    "No effect since 2026-10-23: the pushed shared constants are compared as one mask of their lines "
                    "(line_compare.h Lines64DifferMask), neither a memcmp nor a branch per line. Before: OFF by default since package 36r: the console's 36q profile had PushSharedConstants 2.7 -> 4.9 % of "
                    "the render thread (memcmp stops at the first differing word; the whole-line compare does not). Renut's renderer on deko3d: the pushed shared constants' 64-byte lines are compared with what the "
                    "GPU holds by inline loads instead of a memcmp call per line (21 a draw). Off: memcmp")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_stream_mask, true, "Banjo/Enhancements",
                    "Renut's renderer: a draw's vertex streams are walked by the layout's mask of used streams instead "
                    "of a check of all 16")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_texture_defaults_copy, true, "Banjo/Enhancements",
                    "Renut's renderer: a draw's default texture and sampler words are one copy of a block made once")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// renut-ultimate (2026-11-16, port 145; tw2de_nx's seen19, ON at the user's word: "port the speed ups as on by default"): RedeemQuietStream below.
REXCVAR_DEFINE_BOOL(banjo_renut_stream_redeem, true, "Banjo/Renut",
                    "tw2de_nx (exact): a vertex stream that turned dynamic (rewritten 3 times) becomes cacheable again "
                    "after 60 conversions in a row found its memory unwritten (the store cards): it then takes the usual "
                    "way to a cached copy. tw2de_nx: 7-7.7 ms of render thread a frame converting streams the game had "
                    "stopped writing. false: dynamic for good, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// renut-ultimate (2026-11-16, port 145; tw2de_nx's, off by default at the user's word): FetchFiltersOn below.
REXCVAR_DEFINE_BOOL(banjo_renut_fetch_filters, false, "Banjo/Renut",
                    "tw2de_nx (exact, as the console samples): a texture is sampled with the filters its shader's fetch "
                    "instructions give (mag / min / mip filter, anisotropy) where they give one, not only the fetch "
                    "constant's (FINAL FANTASY XIII's light shafts: a point-sampled mask blurred into steps). The draws "
                    "whose shaders give them go the full draw path. false: the fetch constant's alone, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_fetch_slots_skip, true, "Banjo/Enhancements",
                    "Renut's renderer on deko3d: a draw without vertex fetches skips the compare of the shared "
                    "constants' fetch slots when the last push had none either (both zero)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_repeat_census, false, "Banjo/Diagnostics",
                    "Renut's renderer (2026-10-23, batch 6b): two frames in every 32 hashed draw by draw, and the 10 s "
                    "report's repeat census line says how many draws repeat from one frame to the next: their state "
                    "and call, and (for each target) with the same constants and guest data too, by shadow maps "
                    "(depth targets alone) and the rest. Costs a few ms in those two frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_kick_reuse, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-23, batch 6): a fetch-data copy or converted vertex stream an earlier "
                    "kick of the frame made is drawn again instead of a new one when the store cards prove its guest "
                    "bytes unchanged since (the same bytes; with banjo_renut_store_cards_verify they are compared too)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_fetch_data_unread, true, "Banjo/Enhancements",
                    "Renut's renderer on deko3d: a draw whose shaders fetch nothing by address gets fetch data address "
                    "and size 0 (they never read them) instead of a new allocation's, and no fetch binding")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_whole_buffers, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26, API-level stage 2, Unleashed's bindings): a draw whose every stream "
                    "and whose index buffer have a converted copy of the whole buffer (made once the store cards show "
                    "it unwritten for 3 kicks) binds those and draws with the game's own first index and base vertex, "
                    "instead of converting, caching and binding the range each draw reads (a copy per range, indices "
                    "rebased per draw). Dynamic, Draw*UP, fan and quad, tessellated, fur skin-once and memexport draws "
                    "keep the ranges. Off: ranges for every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-27, the user's request: #8, render resource objects). MeshObject, below.
// banjo-nx (2026-10-27, the user's Phase A): render/draw.h DrawResources.
REXCVAR_DEFINE_BOOL(banjo_renut_resource_objects, true, "Banjo/Enhancements",
                    "Renut's renderer (Phase A, Unleashed's model where N&B allows it): the recording threads resolve "
                    "each bound texture to an object (its header's address and words, made on first use) and take each "
                    "draw's index buffer header; the render thread's draws read one object a texture (its bind answer, "
                    "kept while its fetch words, binding generation, frame and layouts hold) instead of the slot memo, "
                    "the guest header and the epoch tables. Exact (verify). In-run A/B target 45")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_resource_objects_verify, false, "Banjo/Debug",
                    "Debug: each object answer is checked against the memo's (same inputs) and the guest header read "
                    "now; each recorded index buffer header against the guest's; differences are counted and logged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-27, the user's shadow-caster culling, step 0): what the depth-only passes are, for choosing what
// to cull (ShadowCensusNote).
REXCVAR_DEFINE_INT32(banjo_renut_shadow_cull_indices, 0, "Banjo/Enhancements",
                     "NOT pixel-identical (the user's call, 2026-10-27): shadow casters of at most this many indices (or "
                     "vertices for a draw without indices) are not drawn into the shadow maps (depth-only draws into a "
                     "square depth target): small objects lose their shadows, the render thread and the GPU skip "
                     "those draws. 0 = off (every shadow drawn). The 10 s line counts the draws skipped")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_push_lines, false, "Banjo/Debug",
                    "Diagnostic (2026-10-28, GPU block): the 10 s fast-draw line also counts the shared constants' "
                    "64-byte lines pushed, by line, and the float constant bytes pushed for vertex and pixel shaders "
                    "(the vertex ones of draws without a pixel shader beside the groups their shaders read)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_shadow_census, false, "Banjo/Debug",
                    "Diagnostic: the fast path's depth-only draws by depth target (its size, whether colour targets are "
                    "bound) and by index count, in the 10 s report")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-27, the user's "export checks first"): render/draw.h SlotExtent.
REXCVAR_DEFINE_BOOL(banjo_renut_record_extents, true, "Banjo/Enhancements",
                    "Renut's renderer: the recording threads work out the guest extent of each texture slot whose fetch "
                    "words a recorded op changed, and a replayed draw's memexport check tests those against the export "
                    "bitmap, instead of a table of answers by fetch words that the pending exports' changes (several a "
                    "frame) emptied. Exact. In-run A/B target 43")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_record_extents_verify, false, "Banjo/Debug",
                    "Debug: every extent the export check takes from the recording is compared with TextureGuestExtent "
                    "of the draw's fetch words; differences are logged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_mesh_objects, true, "Banjo/Enhancements",
                    "Renut's renderer: each mesh drawn from whole-buffer copies keeps an object of what its geometry "
                    "resolves to (its streams' and index buffer's copies), found by one probe of what it is made of; "
                    "every copy dropped retires the objects (pushed, not checked per draw). Exact. In-run A/B target 42")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_fetch_cache, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-27, the copies): the guest buffers vertex shaders fetch by address "
                    "(bone and instance data, about 3 MB a frame in the city, copied from guest memory at every kick "
                    "before) are kept in the stream cache's pool once unwritten for three kicks, proven once per kick by "
                    "the store cards (or the exact hash), as the whole-buffer copies are; a draw whose fetch buffers "
                    "are all kept binds the pool. Exact. In-run A/B target 47")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_geometry_objects, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-27, geometry like Unleashed's): the recording threads resolve each draw's "
                    "stream sources and index buffer to objects (by the words the hardware takes at the draw); the "
                    "render thread keeps each object's whole copy (for the layout and shader it was found for) and a "
                    "draw reads one object a buffer instead of finding its source, key, memo slot and entry. Replaces "
                    "the per-mesh objects (banjo_renut_mesh_objects) while on. Exact. In-run A/B target 48")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_point_fast_state, true, "Banjo/Enhancements",
                    "Renut's renderer: a point-sprite draw (its constants in ring uploads, not pushed) leaves the fast "
                    "path a state to start from, so the draw after it is not sent the full way; the fast draw puts "
                    "back the pushed buffers' bindings, compares its constant models whole and refills its texture "
                    "slots and fetch data. Exact. In-run A/B target 59")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29): Bangai-O's missiles and particles are point sprites (41 % of the draws at a heavy moment).
REXCVAR_DEFINE_BOOL(banjo_renut_point_fast_draw, true, "Banjo/Enhancements",
                    "Renut's renderer (deko3d): point-sprite draws take the pushed constants (point_sprite_gs reads "
                    "the shared constants from UBO binding 2 there, as the game shaders do) and the fast draw path, "
                    "which sets their point size state from the tessellator packet as the full path does. Off: their "
                    "constants go to ring uploads and every point draw the full way. Exact. In-run A/B target 80 (69 in bangaio-nx)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29): see RangeEntry.
REXCVAR_DEFINE_BOOL(banjo_renut_stream_second_sighting, true, "Banjo/Enhancements",
                    "Renut's renderer: a vertex range's stream cache entry is made when a draw reads that range (the "
                    "same guest address, count, stride and layout) in a later frame than its first, not at its first "
                    "(v5; v4: at its second sighting, which Bangai-O's two passes a frame gave every range). Bangai-O draws sprites "
                    "from moving offsets of buffers it rewrites, so nearly every such range is read once: the console's "
                    "missile storm held 193,150 entries, each allocated, hashed at the kick, walked by every idle and "
                    "replaced-range pass and freed. A first sighting is drawn as a fresh entry is (converted into the "
                    "upload ring), and a copy is made after the same proven flushes, one sighting later. v5 also drops "
                    "the entry of a range whose bytes were new at three kicks in a row and that has no copy (the game "
                    "rewrites its sprite buffers in place: those ranges are never cached), and gives it no new entry "
                    "while its mark holds. Exact. In-run A/B target 71")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29): see QuadIndexBuffer.
REXCVAR_DEFINE_BOOL(banjo_renut_quad_indices, true, "Banjo/Enhancements",
                    "Renut's renderer: a non-indexed quad-list draw (Bangai-O's sprites and missiles: 79k of 129k draws "
                    "a second in a PC gameplay run) is drawn with one index buffer made once, 0,1,2,0,2,3 then 4,5,6,4,"
                    "6,7 and on, instead of its own indices made and written into the upload ring at every draw: the "
                    "same 16-bit indices ExpandIndices made. Exact. In-run A/B target 72")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// SLOWER ON THE CONSOLE (A/B 53, night_final city: render thread +0.64 ms a frame; night_gpu5's check, 30 s halves:
// +0.69 ms, +1.4 / +0.7 / 0.0, -0.53 fps): off by default.
REXCVAR_DEFINE_BOOL(banjo_renut_texture_slot_skip, false, "Banjo/Enhancements",
                    "SLOWER ON THE CONSOLE, OFF by default (2026-10-28 console A/B, night_final, target 53: render thread +0.64 ms a frame "
                    "with it on, static city). Renut's renderer (2026-10-28, per-draw night): a draw that changes some of its texture slots no "
                    "longer takes the others the whole way through FillTextures: a slot whose texture object the object "
                    "path answered last, with the same fetch words and nothing that answer read changed since (frame, "
                    "binding generation, layout epoch), keeps its words and effects. The same words. In-run A/B "
                    "target 53")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_texture_slot_skip_verify, false, "Banjo/Debug",
                    "Debug (banjo_renut_texture_slot_skip): each kept slot is checked as the object path would answer "
                    "it (the object still holds, nothing refreshed or transitioned, the same effects, the block's "
                    "words the object's); the 10 s texture slots line counts those that differ")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_record_lookups, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-28, total CPU): the recording threads find an op's texture, stream and "
                    "index buffer objects together (their guest headers, then their table lines, fetched at once so the "
                    "misses overlap; the tables hold the keys, a line a lookup), and each recording device remembers the "
                    "object it found last for each texture slot, stream and the index buffer (the same words: no "
                    "lookup). The c4 console run's recording threads spent 46 % of NewOp (about 6 ms a frame) in these "
                    "lookups. The same objects. In-run A/B target 50 (the halves give the CPU of all threads a frame)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_fetch_cache_verify, false, "Banjo/Debug",
                    "Debug (banjo_renut_fetch_cache): each kept fetch copy a draw binds is compared with the guest bytes "
                    "now; differences are counted in the fetch cache line and logged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_convert_reuse, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26, GPU): a memexport stream conversion (stream_convert_cs, a compute "
                    "dispatch between two full barriers) already made in this frame from the same export window, "
                    "vertices and layout, with no export recorded since, is used again instead of dispatched again. "
                    "Off: every draw that reads an export window converts it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_batched_readbacks, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26, GPU): a memexport draw's copy of its windows to the readback buffer "
                    "(read by the CPU only once the slot's fence is waited) is made with the others of the list at its "
                    "end, in one group between two barriers, instead of after each draw (a copy-engine copy between "
                    "two full barriers each). Nothing writes a window after its draw (a slot's windows only grow). "
                    "Off: one group after each memexport draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_whole_buffers_verify, false, "Banjo/Diagnostics",
                    "Tests (banjo_renut_whole_buffers): each whole-buffer draw's indices compared with the guest's, and "
                    "its vertices' rows with the range converted again; a difference is logged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Port 170 (from fnxf_xiii): see SettleExportsForFence. A fix, so off by default (fnxf_xiii: on).
REXCVAR_DEFINE_BOOL(banjo_fence_waits_exports, false, "Banjo/Renut",
                    "A D3D fence completes only once every pending memexport result is in guest memory (fnxf_xiii: the "
                    "game freed memory at the fence and a late GPU copy overwrote what it wrote there). A GPU sync on "
                    "every fence while exports are pending. false: as soon as the render thread has read the work, as "
                    "before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Port 166 (from fnxf_xiii): see SetSampleMaskLocked. Accuracy, so off by default here (fnxf_xiii: on).
REXCVAR_DEFINE_BOOL(banjo_deko3d_sample_mask, false, "Banjo/Enhancements",
                    "deko3d: the game's MSAA sample mask (PA_SC_AA_MASK) for each draw on a multisampled target (fnxf_xiii: "
                    "per-sample depth restores drew every sample). false: every sample written, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_fast_draw, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26, the record-time draw path's stage 1, docs/record-time-draw-path.md): "
                    "a draw starts from what the draw before left and redoes only what the recording's field bits and "
                    "the render thread's epochs say changed (FastDrawLocked); new targets, declarations, the "
                    "predication, Draw*UP, points, tessellation, memexport, the fur, the composite and range geometry "
                    "keep DrawStateLocked. A/B target 25. Off: DrawStateLocked for every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-27): the shadow-path census (draw.h DrawKindsReport).
REXCVAR_DEFINE_BOOL(banjo_renut_diag_draw_kinds, false, "Banjo/Diagnostics",
                    "Renut's renderer (2026-10-27): the replayed draws by kind (shadow maps: a depth target and no pixel "
                    "shader; other draws without a pixel shader; colour draws) in the 10 s report: their share, the "
                    "render thread's time a draw (the op's state and the draw), the fields that changed and the runs of "
                    "one kind. Costs two timer reads a draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-27, render thread night): guest/command_buffers.cpp Replay.
REXCVAR_DEFINE_BOOL(banjo_renut_merge_draws, false, "Banjo/Enhancements",
                    "OFF since the census (2026-10-27): no N&B draw is the draw before's state and constants again (each "
                    "part has its own index buffer). Renut's renderer (2026-10-27): a replayed draw takes the draws right after it that change no state "
                    "and write no constant and draw the next indices (or vertices) of the same list with the same base: "
                    "one host draw of the same primitives in the same order (the shadow maps' parts of one model). Not "
                    "for shaders that export, skin once, draw fur or the composite. A/B 33. Off: one host draw each")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_terrain, false, "Banjo/Diagnostics",
                    "2026-10-28 (the tessellated terrain, PS 9ED9E1A8 with VS F09B139E / 8EDDB323): the 10 s line counts "
                    "its draws, vertices a draw, and how many draws of a frame have exactly the vertex inputs of an "
                    "earlier one (the draw and tessellation counts, the float constants and textures the vertex shader "
                    "reads, the control points by their bytes, the boolean and tessellation shared words), and which of "
                    "those differ from the terrain draw before. Decides whether the terrain can be evaluated once a "
                    "frame for its layers (as the fur is skinned once)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_fast_draw_verify, false, "Banjo/Diagnostics",
                    "Tests (banjo_renut_fast_draw): each part a fast draw kept (texture words, bools and alpha test, "
                    "viewport, pipeline key) derived again and compared; a difference is logged and the derived value "
                    "used")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_lazy_resolve_mips, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-28, from sa2-nx): a scaled colour resolve destination's host mip levels "
                    "(docked) made when a draw first samples it, not after every resolve into it. Same pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-28): NOT slower after all. night_gpu4's +2.4 / +1.7 ms were 15 s samples inside the noise (A/B 1, a
// no-op on the Switch, measured +1.8 / +1.3 ms in night_gpu5's first run). night_gpu5's check (four-core, 30 s halves,
// the control at +0.3 ms): -0.34 ms (-1.0, -0.2, +0.1). On by default again.
REXCVAR_DEFINE_BOOL(banjo_renut_literals_resident, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-27): a shader whose literal registers all still hold its values (their "
                    "owner) takes them as held instead of pushing them again. Console (night_gpu5 check, 30 s halves): "
                    "render thread -0.34 ms a frame, within the noise; night_gpu4's 'slower' was noise. In-run A/B 34. "
                    "Same pixels either way")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_clear_repeat, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-28, from sa2-nx): a whole clear of a target that holds exactly that clear "
                    "already (its last whole clear had the same values, and no draw, clear, hand-over or rebuild since) "
                    "is not sent. Same pixels. A/B 67")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-28): OFF by default, the user's call after night_gpu5's console runs: neutral (A/B 65: -0.54 ms a
// frame in each of three rounds, 3-core, 15 s halves; +0.45 ms, four-core, 30 s halves with a control at +0.3). The
// packet table (8,192 entries, about 1.6 MB) takes much of the A57's 2 MB L2 for what it saves.
REXCVAR_DEFINE_BOOL(banjo_renut_host_packets, false, "Banjo/Enhancements",
                    "OFF by default: neutral on the console (night_gpu5, A/B 65: -0.54 ms and +0.45 ms a frame in two runs). "
                    "Renut's renderer (2026-10-28, the user's mandate): host draw packets. A replayed draw's derived state "
                    "(shaders, declaration, targets, textures' words, bools, viewport, pipeline) kept by the draw's whole "
                    "device state (a key the recording threads make as they record); a later draw of the same state "
                    "takes it instead of deriving it again. Exact: the same state, and every epoch the answers depend on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// kameo-nx (2026-11-04): WholeCopyFits.
REXCVAR_DEFINE_INT32(banjo_renut_whole_over_cap_mb, 256, "Banjo/Enhancements",
                     "A whole-buffer copy may take the stream cache this far past banjo_renut_stream_cache_mb, so that "
                     "the range copies it replaces can go. The same picture. 0: refused at the limit like any other copy")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_fast_census, false, "Banjo/Diagnostics",
                    "Renut's renderer (2026-10-26, the record-time draw path's stage 1): every 10 s, what changed from "
                    "draw to draw (the recording's field bits, the chain's breaks, the float constant groups) and how "
                    "often each case the lean executor would hand to DrawStateLocked comes up")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_unleashed_census, false, "Banjo/Diagnostics",
                    "Renut's renderer (2026-10-26): what Unleashed's resource model and record-time translation would "
                    "find in N&B. Every 8th frame, each draw's streams, index buffer and textures by where their headers "
                    "came from (our Create hooks, D3D_SetTextureHeader, or the game's own code) and whether their data "
                    "was written since the frame before (the store cards); the Lock calls; and each RunCommandBuffer's "
                    "recordings, run before or not. The 10 s report's \"[renut] Unleashed census\" line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_keep_fetches, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26, Unleashed-style step 1): a draw's converted fetch constants kept "
                    "for the next draw while the recording says no fetch constant changed (and its header prefetches "
                    "skipped). Off: converted at every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_persistent_shared, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26, Unleashed-style step 1): the draws' SharedConstants kept from one "
                    "draw to the next (Unleashed's g_sharedConstants), each draw writing the words it sets and putting "
                    "back what the draw before set and it does not (texture slots, fetch slots, bound fetches, "
                    "exports, point sprites, the skin mode), instead of zeroing 848 bytes and copying 640 bytes of "
                    "defaults at every draw. The same words. Off: a new block a draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_persistent_shared_verify, false, "Banjo/Diagnostics",
                    "Tests (banjo_renut_persistent_shared): at every draw, every word the draw did not set is checked "
                    "to be what a new block would hold (the defaults, zero); a difference is logged and put right")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_shared_partial_clear, true, "Banjo/Enhancements",
                    "Renut's renderer: a draw's shared constants are zeroed from the booleans on (848 of 1,360 bytes); "
                    "the texture and sampler words before them are all written by FillTextures. Off: all zeroed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_lean_diagnostics, true, "Banjo/Enhancements",
                    "Renut's renderer: the per-draw counts only the 10 s log lines read (the draw kinds, the fetch "
                    "alignment census) are kept only while those lines are logged (log_level info or lower), and the "
                    "draw and stream dumps' checks are made once instead of a call per draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vivapinata_resolve_from_sibling, true, "Banjo/Enhancements",
                    "vivapinata-nx (2026-11-11, the GPU round; A/B 87): a multisampled colour resolve of a target unchanged "
                    "(but for colour clears elsewhere) since the API last resolved it to its own place, over a rect inside "
                    "that one: into the same destination, which still holds that resolve, nothing is done (those texels "
                    "are there); into another destination of the same format and scale that still holds it, its texels "
                    "are copied (one sample read each) instead of resolved. Viva Pinata resolves its scene whole into a "
                    "texture and then, nothing drawn between, per tile into the same texture, each tile cleared after "
                    "(EndTiling). The same texels either way. Off: resolved again")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vivapinata_shader_round, true, "Banjo/Enhancements",
                    "vivapinata-nx (2026-11-12, the shader round; A/B 150): the shader round's GLSL passes (samplerk, "
                    "onexsize: vivapinata-nx's docs/shader-round.md) act in the pixel shaders, constant 975 at its default 1. Off (or the "
                    "A/B's off half): the pipelines are built with 975 = 0 and each of those passes' code does what it did "
                    "before the pass. The same pixels either way: a switch to measure them in one run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vivapinata_shader_noearly, false, "Banjo/Enhancements",
                    "vivapinata-nx (2026-11-12, the shader round; A/B 151): the pixel shaders without Renut's early alpha "
                    "test (constant 976 = 0, tools/deko3d_glsl_opt.py noearly): a quad that test would discard is "
                    "discarded by the shader's own alpha test instead, so the same pixels. It pays where the shader after "
                    "the test is short (the grass shells: the test is 19 of their 80 instructions). Off: as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renut-ultimate (2026-11-12, port 99): on by default in the base (vivapinata-nx ships it on in its tomls: -0.33 ms of
// GPU a frame in its garden, run c24, A/B 152, steady over 5-6 cycles; the same pixels. Its first number, -4.32 ms,
// had the plain fetches off in the A/B's off half too: the A/B was then numbered 92, sasrt::gpu_round's item 2).
REXCVAR_DEFINE_BOOL(vivapinata_shader_earlykill, true, "Banjo/Enhancements",
                    "vivapinata-nx (2026-11-12, the shader round; A/B 152): the pixel shaders' alpha test moved up to right "
                    "after what it needs, the colour arithmetic it does not need after it (constant 977 = 1, "
                    "tools/deko3d_glsl_opt.py earlykill): a discarded pixel skips that arithmetic, the same pixels. Off: "
                    "the test last, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Port 179 (from fnxf_xiii, its render/draw.cpp of 2026-10-31 and 2026-11-01): CarryEdramBeforeDrawLocked. A heuristic,
// so off by default (the user's rule for keys that can change a game's picture): a game that shows black or noise where
// the console shows the frame before turns it on in its toml.
REXCVAR_DEFINE_BOOL(banjo_renut_edram_carry, false, "Banjo/Renut",
                    "From fnxf_xiii: a colour target first drawn in a frame without being cleared in it starts with the "
                    "picture of the target last written over the same EDRAM base in an earlier frame (the same format, at "
                    "least as large, single-sampled), scaled to fit: the console's EDRAM kept that picture there, a host "
                    "target only what it drew itself. Not with blur_renut_edram_inherit (the exact, tile-by-tile rule). "
                    "false: such a target holds what it last drew, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_edram_carry_always, false, "Banjo/Renut",
                    "banjo_renut_edram_carry, a test: every target's first draw in a frame takes the picture of the "
                    "other colour target written last (the same format, at least as large), whatever its EDRAM base, "
                    "frame and order, and the first 16 checks are logged. The picture can be wrong: it checks that the "
                    "carry's pass runs, on a game that never needs it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, banjo_renut_neon_compare);  // banjo-nx (2026-10-20, from sa2-nx): backend/queued.cpp

namespace renut::gpu {

// vivapinata-nx (2026-11-11, vivapinata_resolve_from_sibling / A/B 87): a colour clear of 's' (its writes counted already): its
// rect goes with the sibling, or the sibling ends when four are taken.
void NoteSiblingClear(HostSurface &s, const plume::RenderRect &host_rect) {
  ++s.rect_clears;
  HostSurface::MsSibling &sib = s.ms_sibling;
  if (!sib.dest_va)
    return;
  if (sib.cleared_count >= 4) {
    sib = {};
    return;
  }
  int32_t *r = sib.cleared[sib.cleared_count++];
  r[0] = host_rect.left;
  r[1] = host_rect.top;
  r[2] = host_rect.right;
  r[3] = host_rect.bottom;
}

bool ResolveFromSiblingOn() {
  static const bool on = REXCVAR_GET(vivapinata_resolve_from_sibling);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 87;
}
// 'why': 0 copied from another destination, 1 not resolved (the destination holds it), 2 no record, 3 the target written
// since (not only cleared), 4 a clear since over this rect, 5 another image or a rect outside the record's, 6 the same
// destination changed since or elsewhere, 7 the other destination changed since or not copyable. Render thread only.
void CountResolveFromSibling(int why) {
  static uint64_t n[8] = {};
  static auto last = std::chrono::steady_clock::now();
  ++n[why & 7];
  const auto now = std::chrono::steady_clock::now();
  if (now - last >= std::chrono::seconds(10)) {
    RGPU_INFO("[renut] resolve from the last one (vivapinata_resolve_from_sibling, A/B 87) in the last {:.0f} s: {} "
              "not resolved (in the destination already), {} copied from another destination; not: {} no record, {} "
              "written since, {} cleared over, {} outside, {} destination changed, {} other changed",
              std::chrono::duration<double>(now - last).count(), n[1], n[0], n[2], n[3], n[4], n[5], n[6], n[7]);
    std::fill(std::begin(n), std::end(n), 0);
    last = now;
  }
}

ChainHint g_chain_hint;  // banjo-nx (2026-10-20, from sa2-nx): draw.h
// banjo-nx (2026-10-26): the chain reuse's answers (CountChainReuse's 'what') counted in plain words on the render
// thread and handed to the 10 s line's counts every 1,024 draws (a call and two atomic updates an answer, five a draw,
// before).
struct ChainReuseCounts {
  uint64_t n[5][2] = {};
  u32 draws = 0;
};
static ChainReuseCounts g_reuse_counts;
inline void CountReuse(int what, bool reused) {
  ChainReuseCounts &c = g_reuse_counts;
  ++c.n[what][0];
  c.n[what][1] += reused;
  if (what == 0 && ++c.draws >= 1024) {
    banjo::renut_gpu::CountChainReuseN(c.n);
    c = {};
  }
}
// banjo-nx (2026-10-26, Unleashed-style step 1): the fields the reuse tests stand for are the byte ranges they tested.
static_assert(FieldsOfRange(10112, 10600 - 10112) == (kFieldBools | kFieldDest | kFieldViewPackets));
static_assert(FieldsOfRange(10432, 10600 - 10432) == kFieldViewPackets && FieldOfOffset(0x29C0) == kField29C0 &&
              FieldOfOffset(d3d::kDeviceOffsetPredication) == kFieldPredication);
static_assert(FieldsOfRange(d3d::kDeviceOffsetRenderTargets, 20) == kFieldTargets &&
              FieldsOfRange(1152, 1776 - 1152) == kFieldFetch &&
              FieldsOfRange(d3d::kDeviceOffsetTextures, 4 * d3d::kTextureFetchSlots) == kFieldTexWords);
// banjo-nx (2026-10-26): banjo_renut_chain_reuse as configured, and not in the off half of its in-run A/B (target 20).
inline bool ChainReuseOn() {
  static const bool on = REXCVAR_GET(banjo_renut_chain_reuse);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 20;
}

// sasrt-nx's game-frame count (defined further down, where SasrtNoteExport uses it): declared here for the fourth round's
// sasrt_gpu_diag_export_order, which reads it earlier in the file (rr6-nx, 2026-11-09).
extern std::atomic<uint64_t> g_sasrt_game_frames;

namespace {

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_lean_diagnostics): whether the per-draw diagnostic counts are kept (their lines are
// logged), once.
// banjo-nx (2026-10-20, package 36r): a byte read after the first call (the console's 36q profile: 0.4 % of the render
// thread in the call with its static's guard, three calls a draw).
int8_t g_diag_counts = 0;  // 1 on, -1 off, 0 not read yet
bool DiagCountsInit() {
  bool on = true;
  if (REXCVAR_GET(banjo_renut_lean_diagnostics)) {
    auto *logger = ::rex::GetLoggerRaw(::rex::log::core());
    on = logger && logger->should_log(spdlog::level::info);
  }
  g_diag_counts = on ? 1 : -1;
  return on;
}
inline bool DiagCountsOn() {
  const int8_t v = g_diag_counts;
  return v ? v > 0 : DiagCountsInit();
}

// banjo_renut_diag_blend_skip_exclude: the pixel shader's hash is in the list (read once).
bool BlendSkipExcluded(const void *pixel_shader) {
  static const std::unordered_set<uint64_t> excluded = [] {
    std::unordered_set<uint64_t> set;
    const std::string list = REXCVAR_GET(banjo_renut_diag_blend_skip_exclude);
    size_t at = 0;
    while (at < list.size()) {
      const size_t comma = list.find(',', at);
      const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      if (!item.empty())
        set.insert(std::strtoull(item.c_str(), nullptr, 16));
      if (comma == std::string::npos)
        break;
      at = comma + 1;
    }
    return set;
  }();
  const auto *entry = static_cast<const ShaderCacheEntry *>(pixel_shader);
  return entry && !excluded.empty() && excluded.count(entry->hash);
}

constexpr u32 kMaxDrawVertices = 1u << 21;
constexpr u32 kMaxDrawIndices = 1u << 22;
constexpr u32 kMaxFetchCopyBytes = 32u << 20;
constexpr u32 kUpEndian = 2;  // Begin*Vertices fetch constants use 8in32

// Xenos primitive types (D3DPRIMITIVETYPE).
enum Primitive : u32 {
  kPointList = 1,
  kLineList = 2,
  kLineStrip = 3,
  kTriangleList = 4,
  kTriangleFan = 5,
  kTriangleStrip = 6,
  kRectangleList = 8,
  kLineLoop = 12,
  kQuadList = 13,
  kTrianglePatch = 17,  // tessellated draws only
  kQuadPatch = 18,
};

// SharedConstants as shader_common.h lays it out for RENUT_RECOMP (both the
// SPIR-V raw loads and the DXIL cbuffer packoffsets).
struct SharedConstants {
  u32 texture_indices[3][32];  // 0: [2D, 3D, cube][fetch constant]
  u32 sampler_indices[32];     // 384
  u32 booleans[8];             // 512: VS bools 0-127, PS bools 128-255
  // 544 / 568: screen-space draws (screen_space): what the Xenos viewport
  // transform left of scale and offset, per axis, in pixels (see
  // ComputeViewport).
  float screen_offset[2];      // 544
  float alpha_threshold;       // 552
  u32 alpha_test_func;         // 556: CompareFunction, 7 (always) when disabled
  uint64_t vertex_fetch_data;  // 560: device address of this draw's fetch copies (SPIR-V)
  float screen_scale[2];       // 568
  u32 vertex_fetch_slots[96];  // 576: per fetch index, copy offset | endian
  uint64_t memexport_address;  // 960: device address of the export buffer (SPIR-V)
  // 968: 1 when the vertex shader's positions are in pixels (the viewport
  // transform off) and it maps them to clip space itself; 0 when the host
  // viewport does it.
  u32 screen_space;
  // 972, banjo-nx (package 27; was padding): the byte size of the fetch data from vertex_fetch_data, the bound of
  // vfetchMemoryClamped (shader_common.h): the predicated-block pass runs bone fetches unconditionally in that form.
  u32 vertex_fetch_bytes;
  // 976: memexport windows: guest byte address, size, byte offset in the
  // export buffer. None yet, so memExport stores nothing.
  u32 memexport_streams[4][4];
  // 1040: tessellated draws: patch kind (0 none, 1 triangles, 2 quads),
  // segments per edge, first patch index; other draws: [3] what the host vertex
  // ID lacks of the guest vertex index (renutVertexRegisters).
  u32 tessellation[4];
  // 1056: per vertex input the shader reads with vfetchBound, its element as the
  // bound declaration describes it: stream fetch index, offset and stride in
  // dwords, D3DDECLTYPE (see DescribeBoundFetches).
  u32 bound_fetch[16][4];
  // 1312: point sprites (point_sprite_gs.hlsl): the render state's point width
  // and height, minimum and maximum size, in pixels (PA_SU_POINT_SIZE holds
  // half the size in 12.4 fixed point, PA_SU_POINT_MINMAX the size).
  float point_state[4];
  // 1328: the size in guest pixels of what clip space spans (the host
  // viewport, which is 'resolution_scale' times that), and padding.
  float viewport_size[2];
  float resolution_scale;  // 1336: of the render targets (ResolutionScale)
  // 1340, banjo-nx: FLT_MAX, the Xenos clamps' bound the SPIR-V shaders read here (shader_common.h; was padding).
  float flt_max;
  // 1344 (Renut upstream 574b488, Beta-Nut 4: "fixed an issue where jinjo bingo would show a black square instead of
  // the silhouette"; banjo-nx 2026-10-19): in guest pixels, what the pixel position is relative to: a draw predicated
  // to one tile has the tile's corner (ViewportState::pixel_origin).
  float pixel_origin[2];
  // [0] banjo_deko3d_skin_once's mode; [1] banjo-nx (2026-10-21): the internal heights (InternalHeightsWord).
  float pad_1352[2];
  // 1360, halo3-nx (2026-11-02): the device's loop constants (i0-i15 the vertex shaders', i16-i31 the pixel shaders'), for the loops
  // a shader does not define itself (shader_common.h g_LoopConstant): Halo 3's materials loop on counts the game sets per draw.
  u32 loop_constants[32];
  // 1488, renut-ultimate (rr6-nx's and vivapinata-nx's plane, which they put in point_state): user clip plane 0 (shader_common.h
  // g_UserClipPlane), zero when the draw has none; 1-3 are not used yet (64 bytes keep the 16-byte tail).
  float user_clip_planes[1][4];
  // 1504, sasrt-nx (2026-10-20, port 31; 1352 there): g_DepthRemap, a backwards depth range done in the vertex shader on the
  // PC (ComputeViewportSlow), in the room after the clip plane.
  float depth_remap[2];
  float pad_1512[10];  // to 1552: the 16-byte tail
};
static_assert(offsetof(SharedConstants, sampler_indices) == 384);
static_assert(offsetof(SharedConstants, booleans) == 512);
static_assert(offsetof(SharedConstants, screen_offset) == 544);
static_assert(offsetof(SharedConstants, screen_scale) == 568);
static_assert(offsetof(SharedConstants, screen_space) == 968);
static_assert(offsetof(SharedConstants, alpha_test_func) == 556);
static_assert(offsetof(SharedConstants, vertex_fetch_data) == 560);
static_assert(offsetof(SharedConstants, vertex_fetch_slots) == 576);
static_assert(offsetof(SharedConstants, memexport_address) == 960);
static_assert(offsetof(SharedConstants, memexport_streams) == 976);
static_assert(offsetof(SharedConstants, tessellation) == 1040);
static_assert(offsetof(SharedConstants, bound_fetch) == 1056);
static_assert(offsetof(SharedConstants, point_state) == 1312);
static_assert(offsetof(SharedConstants, viewport_size) == 1328);
static_assert(offsetof(SharedConstants, resolution_scale) == 1336);
static_assert(offsetof(SharedConstants, pixel_origin) == 1344);
static_assert(offsetof(SharedConstants, loop_constants) == 1360);
static_assert(offsetof(SharedConstants, user_clip_planes) == 1488);
static_assert(offsetof(SharedConstants, depth_remap) == 1504);
static_assert(sizeof(SharedConstants) == 1552);
static_assert(sizeof(SharedConstants) == kSharedConstantsUboBytes);

// The guest D3DDevice a draw reads its state from: the live device, or a
// recorded copy of its first kDeviceSnapshotSize bytes.
class GuestDevice {
public:
  explicit GuestDevice(const u8 *bytes) : bytes_(bytes) {}
  bool valid() const { return bytes_ != nullptr; }
  const d3d::Device &dev() const { return *reinterpret_cast<const d3d::Device *>(bytes_); }
  u32 U32(u32 offset) const { return *reinterpret_cast<const be_u32 *>(bytes_ + offset); }
  u8 U8(u32 offset) const { return bytes_[offset]; }
  const u8 *Bytes(u32 offset) const { return bytes_ + offset; }  // katamari-nx: DeviceDeclarationLocked
  float F32(u32 offset) const {
    const u32 bits = U32(offset);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }
  // Xenos fetch constant 'index' (0-95) word 0/1 as a vertex fetch constant.
  u32 VertexFetchWord(u32 index, u32 word) const {
    return U32(u32(offsetof(d3d::Device, m_Constants)) + index * 8 + word * 4);
  }

private:
  const u8 *bytes_;
};

// A shader stage's last float constant upload: the guest bytes of the registers
// it holds, reused while the next draw's shader reads only registers it holds
// with the same bytes.
// banjo-nx (2026-10-28, cache-miss audit 2): the fields every push reads first, ahead of the 4 KB copy (they followed it:
// a line of their own a stage, about 0.5 % of the render thread in loads in the c4 console run).
struct ConstantUpload {
  UploadAllocation alloc;
  u32 valid[8] = {};  // registers written to alloc, bit i = register i
  // The replay (ReplayConstants::replay) whose draw last made 'valid' match
  // its constants, since when that replay's dirty groups are the only ones
  // that can differ; 0 for none.
  uint64_t replay = 0;
  // banjo-nx (banjo_gpu_literals_cbuf): the shader whose literal registers alloc holds (UploadFloatConstants).
  const ShaderCacheEntry *literals = nullptr;
  // banjo-nx (2026-10-19, banjo_deko3d_push_constants): the registers of the pushed uniform buffer that hold
  // 'literals''s literal values (cleared where a group update overwrites one).
  u32 literal_regs[8] = {};
  u8 bytes[256 * 16];  // their guest (big-endian) bytes
  // banjo-nx (2026-10-27, render thread night, A/B 34): per register of the pushed buffer, the shader whose literal value
  // it holds (set by its literal pushes, cleared by a group update over it), so a shader whose literal registers all
  // still hold its values takes them back without pushing them again (draws that switch between shaders pushed them at
  // every switch).
  const ShaderCacheEntry *lit_owner[256] = {};
};

// State bound on the current command list, reset whenever a new one begins.
struct Bound {
  uint64_t generation = UINT64_MAX;
  const plume::RenderFramebuffer *framebuffer = nullptr;
  const plume::RenderPipeline *pipeline = nullptr;
  bool layout = false;
  UploadAllocation zero_stream;
  ConstantUpload vs_constants, ps_constants;
  // banjo-nx (2026-10-19, banjo_deko3d_push_constants): the models of the pushed uniform buffers, apart from the ring
  // uploads' (point-sprite draws keep those, and the two must not share one model: emulator verify run 60).
  ConstantUpload pushed_vs, pushed_ps;
  bool blend_constant_set = false;
  // rr6-nx (2026-11-03, rr6_depth_bias): the device words SetDrawDepthBias last set.
  bool depth_bias_set = false;
  u32 depth_bias_scale = 0, depth_bias_offset = 0, depth_bias_float24 = 0;
  float blend_constant[4] = {};

  // State as the draws last set it on the list, which most draws in a row
  // share (the viewport, the zero stream, the memexport buffer): setting it
  // again cost a driver call each. Both backends keep all of it across
  // pipeline and framebuffer changes; D3D12 root descriptors are dropped with
  // the root signature, so they go with 'layout'.
  bool viewport_set = false;
  plume::RenderViewport viewport;
  plume::RenderRect scissor;
  uint32_t viewport_count = 0;  // viewports and scissors set (SetViewport)
  bool root_set[kDrawRootMemExport + 1] = {};
  plume::RenderBufferReference roots[kDrawRootMemExport + 1];
  struct Stream {
    bool set = false;
    plume::RenderVertexBufferView view;
    plume::RenderInputSlot slot;
  };
  Stream streams[kZeroStreamSlot + 1];
  bool index_set = false;
  plume::RenderIndexBufferView index;
  // banjo-nx (banjo_renut_shared_dedupe): the last SharedConstants upload and its bytes, which a draw with the same
  // bytes reuses (no upload, and set 7 stays bound).
  UploadAllocation shared_alloc;
  bool shared_valid = false;
  SharedConstants shared_bytes;
  // banjo-nx (2026-10-19, banjo_deko3d_push_constants): what the pushed shared uniform buffer holds (valid once the
  // list's first pushed draw sent it whole), and whether plume reads bound textures from it.
  bool pushed_shared_valid = false;
  bool shared_words_set = false;
  SharedConstants pushed_shared;
  // banjo-nx: the uniform buffer sets 5-7 last bound (Vulkan), and their offsets.
  const plume::RenderDescriptorSet *constant_sets[3] = {};
  uint32_t constant_offsets[3] = {};
  // banjo-nx (2026-10-27, render thread night): the pushed uniform buffers bound at bindings 0-2 by FastDrawLocked and
  // nothing bound there since (BindConstantSet and a layout bind clear it; a new list resets the whole struct).
  bool pushed_ubos = false;
};
Bound g_bound;

// banjo-nx: binds a constant upload's uniform buffer set at its offset (Vulkan), unless it is bound there already:
// most draws in a row reuse the vertex shader's constants, and many the pixel shader's.
void BindConstantSet(plume::RenderCommandList *cmd, uint32_t set_index, plume::RenderDescriptorSet *set,
                     uint64_t offset) {
  const uint32_t slot = set_index - kVertexConstantsSet;
  const uint32_t offset32 = uint32_t(offset);
  if (g_bound.constant_sets[slot] == set && g_bound.constant_offsets[slot] == offset32)
    return;
  g_bound.pushed_ubos = false;  // banjo-nx (2026-10-27): its buffer takes the binding
  cmd->setGraphicsDescriptorSetDynamic(set, set_index, &offset32, 1);
  g_bound.constant_sets[slot] = set;
  g_bound.constant_offsets[slot] = offset32;
}

// The last declaration a draw looked up by VA (otherwise a registry lookup
// under its lock per draw): draws in a row mostly use the same one. Cleared
// when a declaration goes (ForgetVertexDeclarationLocked).
u32 g_last_declaration_va = 0;
VertexDeclarationRecord *g_last_declaration = nullptr;

bool SameReference(const plume::RenderBufferReference &a, const plume::RenderBufferReference &b) {
  return a.ref == b.ref && a.offset == b.offset;
}

// plume's Vulkan pipelines have one viewport and scissor per color target
// (all dynamic), each of which a draw must have set; they all get the same.
// sasrt-nx (2026-11-08): EXPERIMENTS for the 33 ms goal, NOT pixel-identical, off by default (measurements only).
REXCVAR_DEFINE_BOOL(sasrt_gpu_exp_shadow_nofill, false, "SASRT/GPU",
                    "EXPERIMENT, NOT pixel-identical (2026-11-08): depth-only draws into a 1024x1024 viewport (the shadow "
                    "cascades) get a 1x1 scissor: their vertex work stays, their pixels go. Measures the cascades' fill");
REXCVAR_DEFINE_BOOL(sasrt_gpu_exp_no_exports, false, "SASRT/GPU",
                    "EXPERIMENT, NOT pixel-identical (2026-11-08): export-only draws (the skinning) are not drawn at all: "
                    "measures the skinning exports' own cost");
REXCVAR_DEFINE_INT32(sasrt_gpu_exp_export_parts, 0, "SASRT/GPU",
                     "EXPERIMENT, NOT pixel-identical (2026-11-09): memexport vertex shaders (constant 945, pass "
                     "expparts) with bit 0 their export stores skipped, bit 1 their bone-row loads replaced by a constant, "
                     "bit 2 their vertex inputs zero, bit 3 no export stores at all: what each part of a skinning export costs");
REXCVAR_DEFINE_BOOL(sasrt_gpu_exp_export_twice, false, "SASRT/GPU",
                    "EXPERIMENT (2026-11-08), same pixels: every memexport draw is drawn twice (the second writes the "
                    "same bytes): what the export draws themselves cost the GPU, apart from the waits around them");
bool SasrtExpShadowNofill() {
  static const bool on = REXCVAR_GET(sasrt_gpu_exp_shadow_nofill);
  return on;
}
bool SasrtExpNoExports() {
  static const bool on = REXCVAR_GET(sasrt_gpu_exp_no_exports);
  return on;
}

void SetViewport(plume::RenderCommandList *cmd, const plume::RenderViewport &viewport,
                 const plume::RenderRect &scissor_in, uint32_t color_count) {
  plume::RenderRect scissor = scissor_in;
  if (SasrtExpShadowNofill() && color_count == 0 && viewport.width == 1024.0f && viewport.height == 1024.0f) [[unlikely]]
    scissor = plume::RenderRect(0, 0, 1, 1);
  // Every index set so far is rewritten, so none keeps an older value.
  uint32_t count = std::clamp(color_count, 1u, uint32_t(d3d::kMaxRenderTargets));
  if (g_bound.viewport_set)
    count = std::max(count, g_bound.viewport_count);
  const plume::RenderViewport &v = g_bound.viewport;
  const plume::RenderRect &r = g_bound.scissor;
  const bool more = count > g_bound.viewport_count;
  if (!g_bound.viewport_set || more || v.x != viewport.x || v.y != viewport.y ||
      v.width != viewport.width || v.height != viewport.height ||
      v.minDepth != viewport.minDepth || v.maxDepth != viewport.maxDepth) {
    const plume::RenderViewport viewports[d3d::kMaxRenderTargets] = {viewport, viewport, viewport,
                                                                     viewport};
    cmd->setViewports(viewports, count);
    g_bound.viewport = viewport;
  }
  if (!g_bound.viewport_set || more || r.left != scissor.left || r.top != scissor.top ||
      r.right != scissor.right || r.bottom != scissor.bottom) {
    const plume::RenderRect scissors[d3d::kMaxRenderTargets] = {scissor, scissor, scissor, scissor};
    cmd->setScissors(scissors, count);
    g_bound.scissor = scissor;
  }
  if (!g_bound.viewport_set || more)
    g_bound.viewport_count = count;
  g_bound.viewport_set = true;
}

void SetRootDescriptor(plume::RenderCommandList *cmd, const plume::RenderBufferReference &ref,
                       uint32_t index) {
  if (g_bound.root_set[index] && SameReference(g_bound.roots[index], ref))
    return;
  cmd->setGraphicsRootDescriptor(ref, index);
  g_bound.roots[index] = ref;
  g_bound.root_set[index] = true;
}

void SetVertexBuffer(plume::RenderCommandList *cmd, const plume::RenderVertexBufferView &view,
                     const plume::RenderInputSlot &slot) {
  Bound::Stream &b = g_bound.streams[slot.index];
  if (b.set && SameReference(b.view.buffer, view.buffer) && b.view.size == view.size &&
      b.slot.stride == slot.stride && b.slot.classification == slot.classification)
    return;
  cmd->setVertexBuffers(slot.index, &view, 1, &slot);
  b.set = true;
  b.view = view;
  b.slot = slot;
}

void SetIndexBuffer(plume::RenderCommandList *cmd, const plume::RenderIndexBufferView &view) {
  const plume::RenderIndexBufferView &b = g_bound.index;
  if (g_bound.index_set && SameReference(b.buffer, view.buffer) && b.size == view.size &&
      b.format == view.format)
    return;
  cmd->setIndexBuffer(&view);
  g_bound.index = view;
  g_bound.index_set = true;
}

// rr6-nx (2026-11-03, rr6_depth_bias): the game's depth bias, which the setters stored and no draw read. Dynamic state, never in
// the pipeline key (Ridge Racer 6 sets a fresh slope for about 13 draws a frame: keyed, that made about 770 pipelines a second).
// Set only when its words change. The units are the emulated GPU's (draw_util's GetD3D10IntegerPolygonOffset, pipeline_cache's
// slope): the slope register is in 1/16 subpixels; the constant is in the depth format's units, rounded away from 0.
void SetDrawDepthBias(plume::RenderCommandList *cmd, const d3d::Device &dev) {
  static const bool on = banjo::renut_gpu::DepthBiasOn() && Video::Device()->getCapabilities().dynamicDepthBias;
  if (!on)
    return;
  u32 scale = 0, offset = 0;
  // PA_SU_SC_MODE_CNTL's polygon offset enables, front (bit 11) and back (bit 12). The setters write both faces alike.
  if (u32(dev.m_ControlPacket.ModeControl) & 0x1800u) {
    const auto *words = reinterpret_cast<const be_u32 *>(dev.m_PointPacket);
    scale = words[0];
    offset = words[1];
  }
  const u32 float24 = (u32(dev.m_DestinationPacket.DepthInfo) >> 16) & 1;  // RB_DEPTH_INFO: D24FS8
  if (g_bound.depth_bias_set && g_bound.depth_bias_scale == scale && g_bound.depth_bias_offset == offset &&
      g_bound.depth_bias_float24 == float24)
    return;
  g_bound.depth_bias_set = true;
  g_bound.depth_bias_scale = scale;
  g_bound.depth_bias_offset = offset;
  g_bound.depth_bias_float24 = float24;
  const float polygon_offset = std::bit_cast<float>(offset);
  const float magnitude = std::fabs(polygon_offset);
  int32_t constant = float24 ? int32_t(std::ceil(magnitude * float(1u << 21))) << 3
                             : int32_t(std::ceil(magnitude * float((1u << 24) - 1)));
  if (polygon_offset < 0.0f)
    constant = -constant;
  cmd->setDepthBias(float(constant), 0.0f, std::bit_cast<float>(scale) * (1.0f / 16.0f));
}

// The blend constant (RB_BLEND_RED..ALPHA) the draw's blend controls read, or
// false if none reads it. The host has a single constant for BLEND_FACTOR, so a
// colour factor that reads the constant alpha gets the alpha in rgb as well.
// banjo-nx (2026-10-27, render thread night): whether DrawBlendConstant finds a factor reading the constant (its first
// loop): a fact of the key alone.
bool BlendConstantUsed(const PipelineKey &key) {
  for (u32 i = 0; i < key.color_count; ++i) {
    for (u32 shift : {0u, 8u, 16u, 24u}) {
      const u32 factor = (key.blend_control[i] >> shift) & 0x1F;
      if (factor >= u32(D3DBlend::BlendFactor) && factor <= u32(D3DBlend::InvConstantAlpha))
        return true;
    }
  }
  return false;
}

bool DrawBlendConstant(const d3d::Device &dev, const PipelineKey &key, float rgba[4]) {
  constexpr u32 kConstantColor = u32(D3DBlend::BlendFactor);
  constexpr u32 kOneMinusConstantAlpha = u32(D3DBlend::InvConstantAlpha);
  constexpr u32 kConstantAlpha = u32(D3DBlend::ConstantAlpha);
  bool used = false, color_reads_color = false, color_reads_alpha = false;
  for (u32 i = 0; i < key.color_count; ++i) {
    // RB_BLENDCONTROL factors: colour src 0, colour dst 8, alpha src 16, alpha dst 24.
    for (u32 shift : {0u, 8u, 16u, 24u}) {
      const u32 factor = (key.blend_control[i] >> shift) & 0x1F;
      if (factor < kConstantColor || factor > kOneMinusConstantAlpha)
        continue;
      used = true;
      if (shift < 16)
        (factor >= kConstantAlpha ? color_reads_alpha : color_reads_color) = true;
    }
  }
  if (!used)
    return false;
  const d3d::ValuesPacket &v = dev.m_ValuesPacket;
  const float alpha = v.BlendAlpha;
  const bool replicate_alpha = color_reads_alpha && !color_reads_color;
  if (color_reads_alpha && color_reads_color) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("Blend control {:08X} reads both the constant colour and alpha; the host uses the colour",
                key.blend_control[0]);
    }
  }
  rgba[0] = replicate_alpha ? alpha : float(v.BlendRed);
  rgba[1] = replicate_alpha ? alpha : float(v.BlendGreen);
  rgba[2] = replicate_alpha ? alpha : float(v.BlendBlue);
  rgba[3] = alpha;
  return true;
}

std::vector<u32> g_indices;  // scratch for index reads

// SetTilesLocked / SetReplayPredicationLocked.
std::vector<ClearRect> g_tiles;
u32 g_replay_predication = ~0u;

// A vertex stream as a draw reads it: guest vertices through a host layout.
struct StreamKey {
  const u8 *guest;
  uint64_t layout_hash;
  u32 stream, stride, count, endian;
  bool operator==(const StreamKey &) const = default;
};
struct StreamKeyHash {
  size_t operator()(const StreamKey &k) const {
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(k.guest)) * 0x9E3779B97F4A7C15ull;
    h ^= k.layout_hash + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
    h ^= (uint64_t(k.stream) << 48 | uint64_t(k.endian) << 40 | k.stride) + (h << 6) + (h >> 2);
    h ^= uint64_t(k.count) * 0xFF51AFD7ED558CCDull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};

// Vertex data a draw reads from guest buffers, read when the ring is kicked
// off (FlushVertexFillsLocked) rather than at the draw call: the GPU starts on
// a draw only after the kick, and the game may still write the buffers the
// draw uses until then.
// banjo-nx (2026-10-23, batch 6): the stream's elements in g_fill_elements (first, count) and its host stride, not a
// copy of the declaration's HostVertexStream (a vector: a heap allocation and free per fill).
struct PendingStreamFill {
  u32 host_stride = 0, first_element = 0, element_count = 0;
  const u8 *guest = nullptr;
  u32 stride = 0, count = 0, endian = 0;
  u8 *host = nullptr;
  StreamKey key{};   // cacheable: the g_stream_cache entry
  bool cacheable = false;
};
struct PendingCopy {
  const u8 *src = nullptr;
  u8 *dst = nullptr;
  u32 size = 0;
};
std::vector<PendingStreamFill> g_stream_fills;
std::vector<HostVertexElement> g_fill_elements;  // banjo-nx (2026-10-23, batch 6): the fills' elements, emptied with them
std::vector<PendingCopy> g_copy_fills;

// banjo-nx (2026-10-17): FlushVertexFillsLocked's calls (every kick, and the end of every command list): the points
// after which the game may learn the GPU is done with what the draws before read, and write those buffers again.
// banjo_renut_exact_streams proves a cached copy once per interval between two of them.
uint64_t g_flush_serial = 0;
// banjo-nx (2026-10-27, #8: mesh objects): raised by every cached copy dropped and every cache entry erased; a mesh
// object of an older generation answers nothing (MeshObjectHit).
uint64_t g_mesh_gen = 1;

// Fetch data copies (UploadVertexFetchData) queued since the last flush, by
// guest bytes: they all read memory at the flush, so a later draw that reads
// the same bytes takes the queued copy rather than another. Where in its
// upload chunk the copy is (the draw's fetch slots count from the chunk's
// start). A generation, bumped at each flush, retires them all at once.
// banjo-nx (2026-10-23, batch 6, banjo_renut_kick_reuse): kept past the flush while the upload rings are not reset
// (FetchCopyHeld): the interval the flush that copies it starts (made_serial), the rings' resets then, the interval it
// was last proven unchanged in, and the copy itself (verify mode).
struct FetchCopy {
  const u8 *src = nullptr;
  u32 size = 0;
  uint64_t generation = 0;
  const plume::RenderBuffer *buffer = nullptr;
  u32 offset = 0;
  uint64_t made_serial = 0;
  uint64_t ring_resets = 0;
  uint64_t checked_serial = 0;
  const u8 *host = nullptr;
};
constexpr u32 kFetchCopyBits = 10;  // banjo-nx (2026-10-23): 1,024 (256 before: the city's skinned draws conflicted)
FetchCopy g_fetch_copies[1u << kFetchCopyBits];
uint64_t g_fetch_copy_generation = 1;

FetchCopy &FetchCopyFor(const u8 *src, u32 size) {
  const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(src)) ^ (uint64_t(size) << 40);
  return g_fetch_copies[(key * 0x9E3779B97F4A7C15ull) >> (64 - kFetchCopyBits)];
}

// Streams queued in g_stream_fills since the last flush. The fills all read
// guest memory at the flush, so a later draw that reads the same guest
// vertices through the same host layout draws from the upload already queued.
// banjo-nx (2026-10-19, render-thread proposal 20): an open-addressed table with a generation (no node allocated per
// stream and freed at each flush); a stream that finds no free slot within kQueuedProbe is converted again (as without
// the table: only slower).
// banjo-nx (2026-10-23, batch 6, banjo_renut_kick_reuse): an entry stays for the frame (its ring's resets), drawn again
// after later flushes while the store cards prove its guest bytes unchanged (FindQueuedStream); 4,096 slots for a
// frame's worth (2,048 for a flush's before).
struct QueuedStream {
  StreamKey key{};
  UploadAllocation alloc;
  uint64_t generation = 0;
  uint64_t made_serial = 0;
  uint64_t ring_resets = 0;
  uint64_t checked_serial = 0;
};
constexpr uint32_t kQueuedStreamSlots = 4096;  // a power of two
constexpr uint32_t kQueuedProbe = 16;
std::vector<QueuedStream> g_queued_streams(kQueuedStreamSlots);
uint64_t g_queued_generation = 1;

bool KickReuseOn() {
  static const bool on = REXCVAR_GET(banjo_renut_kick_reuse);
  return on;
}

// An entry in use: this frame's (kick reuse), or this flush interval's.
inline bool QueuedLive(const QueuedStream &q) {
  return KickReuseOn() ? q.ring_resets == UploadRing::Resets() : q.generation == g_queued_generation;
}

bool QueuedStillHeld(QueuedStream &q);  // below PagesClean

// The upload holding the key's converted vertices: queued since the last flush, or (kick reuse) converted by an earlier
// flush of this frame and proven unchanged since; 'older' says which.
const UploadAllocation *FindQueuedStream(const StreamKey &key, bool *older = nullptr) {
  const size_t h = StreamKeyHash{}(key);
  for (uint32_t i = 0; i < kQueuedProbe; ++i) {
    QueuedStream &q = g_queued_streams[(h + i) & (kQueuedStreamSlots - 1)];
    if (!QueuedLive(q))
      return nullptr;
    if (q.key == key) {
      const bool earlier = q.generation != g_queued_generation;
      if (older)
        *older = earlier;
      return !earlier || QueuedStillHeld(q) ? &q.alloc : nullptr;
    }
  }
  return nullptr;
}

void AddQueuedStream(const StreamKey &key, const UploadAllocation &alloc) {
  const size_t h = StreamKeyHash{}(key);
  for (uint32_t i = 0; i < kQueuedProbe; ++i) {
    QueuedStream &q = g_queued_streams[(h + i) & (kQueuedStreamSlots - 1)];
    if (!QueuedLive(q) || q.key == key) {
      q.key = key;
      q.alloc = alloc;
      q.generation = g_queued_generation;
      q.made_serial = g_flush_serial + 1;  // the flush that converts it starts that interval
      q.ring_resets = UploadRing::Resets();
      q.checked_serial = 0;
      return;
    }
  }
}

// Converted streams (and index ranges, below) kept across frames. Most are
// geometry the game never rewrites, which every draw would otherwise convert
// again. A stream whose guest bytes hash the same at kStableFlushes flushes in
// a row gets a persistent converted copy, which draws bind while the bytes
// still hash the same, checked off the render thread once every
// kCacheVerifyFrames frames a draw reads it: hashing every copy every frame
// cost more than any other part of a draw. A stream the game rewrites
// kMaxCacheDrops times is never cached again. A copy no draw has read for
// kStreamCacheIdleFrames goes: turning back to a scene within that converts
// nothing again.
constexpr u32 kStableFlushes = 3;
constexpr uint64_t kCacheVerifyFrames = 4;
constexpr u32 kMaxCacheDrops = 3;
constexpr uint64_t kStreamCacheIdleFrames = 3600;
constexpr uint64_t kStreamCacheMaxBytes = 768ull << 20;
uint64_t g_stream_cache_bytes = 0;
uint64_t g_stream_cache_frame = 0;
// sasrt-nx (2026-10-21, sasrt_renut_exports_held): written-back export results by physical base, the frame they were written
// back, and the replaced or expired ones the pending fills may still point into (freed at the flush).
struct HeldExport {
  uint64_t frame = 0;
  std::shared_ptr<std::vector<u8>> data;
  uint64_t checked = UINT64_MAX;  // renut-ultimate (audit): the frame HeldExportOverwritten last looked
  bool overwritten = false;
};
std::map<u32, HeldExport> g_held_exports;
std::vector<std::shared_ptr<std::vector<u8>>> g_held_retired;
// sasrt-nx (2026-11-12, batch C/D, item D2; port 93): retired buffers nothing points into any more (the flush), for the
// next holds.
std::vector<std::shared_ptr<std::vector<u8>>> g_held_pool;
uint64_t g_held_reads = 0;
// Keeps an export's results ('size' bytes at 'data', physical base 'base'), dropping kept ones it overlaps.
void HoldExport(u32 base, const u8 *data, u32 size, uint64_t frame) {
  if (!size || size > (8u << 20))
    return;
  auto it = g_held_exports.lower_bound(base);
  if (it != g_held_exports.begin())
    --it;
  while (it != g_held_exports.end() && it->first < base + size) {
    if (it->first + u32(it->second.data->size()) <= base) {
      ++it;
      continue;
    }
    g_held_retired.push_back(std::move(it->second.data));
    it = g_held_exports.erase(it);
  }
  HeldExport &h = g_held_exports[base];
  if (!g_held_pool.empty()) {  // sasrt-nx (2026-11-12, batch C/D, item D2; port 93): a pooled buffer, the same bytes
    h.data = std::move(g_held_pool.back());
    g_held_pool.pop_back();
    h.data->assign(data, data + size);
  } else {
    h.data = std::make_shared<std::vector<u8>>(data, data + size);
  }
  h.frame = frame;
  h.checked = UINT64_MAX;
  h.overwritten = false;
}
// renut-ultimate (audit, 2026-11-07, ultimate_renut_held_exports_guard): whether the game's CPU wrote other data over a
// kept export (physical base 'base') since its write-back: four sampled words that differ from the kept results and from
// the word a vertex ('stride' bytes, the reading stream's) before them. A clear (zeros, one vertex repeated) is periodic.
uint64_t g_held_overwritten = 0;
bool HeldExportOverwritten(u32 base, HeldExport &h, u32 stride) {
  if (!banjo::renut_gpu::HeldExportsGuardOn())
    return false;
  if (h.overwritten || h.checked == g_stream_cache_frame)
    return h.overwritten;
  h.checked = g_stream_cache_frame;
  const u8 *guest = GuestPhysicalPtr<const u8>(base);
  const u8 *kept = h.data->data();
  const size_t words = h.data->size() / 4;
  const size_t period = stride >= 4 && stride % 4 == 0 ? stride / 4 : 1;
  constexpr size_t kBlockWords = 128, kBlocks = 8;
  const size_t step = words > kBlockWords * kBlocks ? (words - kBlockWords) / (kBlocks - 1) : kBlockWords;
  u32 other = 0;
  for (size_t start = 0; start < words && other < 4; start += step) {
    const size_t end = std::min(words, start + kBlockWords);
    for (size_t i = std::max(start, period); i < end && other < 4; ++i) {
      u32 g, k, before;
      std::memcpy(&g, guest + i * 4, 4);
      std::memcpy(&k, kept + i * 4, 4);
      if (g == k)
        continue;
      std::memcpy(&before, guest + (i - period) * 4, 4);
      other += g != before;
    }
  }
  h.overwritten = other >= 4;
  if (h.overwritten && g_held_overwritten++ < 8)
    RGPU_INFO("[renut] kept export {:08X} ({} bytes) written over by the CPU: read from guest memory "
              "(ultimate_renut_held_exports_guard)", base, h.data->size());
  return h.overwritten;
}
// banjo-nx (2026-10-23, console city run: a new area after the city converted 15-40 MB of vertices a frame for over a
// minute): copies refused at banjo_renut_stream_cache_mb since the last 60-frame pass. With any, that pass also drops
// the copies no draw read for kPressureIdleFrames (banjo_renut_stream_cache_pressure), so the scene being drawn gets
// room within seconds instead of after banjo_renut_stream_cache_idle_s of the old scene going unused.
uint64_t g_stream_cache_refused = 0;
// 150 frames (2026-10-23, batch 5): 60 was 2 s at the overclocked console's 30 fps, so the pass dropped copies of the
// scene still being drawn and they were converted again (15-50 thousand refusals every 10 s at the cap).
constexpr uint64_t kPressureIdleFrames = 150;
// sasrt-nx (2026-11-10; port 58): see the drain in the stream cache's idle pass.
REXCVAR_DEFINE_BOOL(sasrt_stream_cache_drain, true, "SASRT/Graphics",
                    "With the stream cache's pool past 1.5 times its copies (and 64 MB), its sparsest chunk is drained: "
                    "new copies go elsewhere and its copies not drawn in the last frame are dropped (made again when "
                    "needed), so the chunk empties and goes back (SASRT's run mem14: 104-146 MB of copies in a 288 MB "
                    "pool)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (package 28, banjo_renut_diag_resolve_trace): draws since the last change of targets.
uint32_t g_trace_draws = 0;

// A cached copy's memory: part of one of the pool's buffers.
struct PoolSpan {
  plume::RenderBuffer *buffer = nullptr;
  u8 *cpu = nullptr;
  u32 chunk = 0;
  uint64_t offset = 0;
  u32 size = 0;
  explicit operator bool() const { return buffer != nullptr; }
  plume::RenderBufferReference Ref() const { return plume::RenderBufferReference(buffer, offset); }
};

// Where the cached copies live: a few large upload buffers, mapped once, that
// copies are carved from. A buffer of its own per copy cost a driver
// allocation each, and a scene coming into view made hundreds of copies in a
// frame. A freed span is reused once the frame slot it was freed in has
// finished on the GPU (DrainSlot), as retired resources are. A chunk left
// with nothing in it is released (the first one stays): the copies of a
// scene the game has left go after kStreamCacheIdleFrames, and their memory
// with them.
class CopyPool {
public:
  PoolSpan Allocate(u32 size) {
    const uint64_t need = (uint64_t(size) + kAlign - 1) & ~(kAlign - 1);
    for (u32 i = 0; i < chunks_.size(); ++i) {
      if (i == draining_)
        continue;  // sasrt-nx (2026-11-10, sasrt_stream_cache_drain; port 58): taken last, below
      if (PoolSpan span = Take(i, need); span)
        return span;
    }
    // banjo-nx (2026-10-28, night_gpu2 console freeze): no new chunk for kNoChunkFrames after one could not be made
    // (every caller then takes the uncached path). The heap had 577 MB free in 170,000 pieces and no 32 MB one, and the
    // pool asked again for every new stream: 3,812 failures of 22 ms each, 0.1 fps for 95 s.
    if (g_stream_cache_frame < no_chunk_until_)
      return {};
    u32 index = AddChunk(need);
    if (index == UINT32_MAX && need <= kSmallChunkSize)
      index = AddChunk(0, kSmallChunkSize);  // a smaller chunk may still fit in a fragmented heap
    if (index == UINT32_MAX && draining_ < chunks_.size()) {
      if (PoolSpan span = Take(draining_, need); span)
        return span;  // sasrt-nx (2026-11-10; port 58): no other room: the chunk being drained after all
    }
    if (index == UINT32_MAX) {
      no_chunk_until_ = g_stream_cache_frame + kNoChunkFrames;
      if (chunk_failures_++ < 4)
        RGPU_WARN("[renut] stream cache: no new chunk for {} KB: copies made only in the chunks there are for {} frames",
                  need >> 10, kNoChunkFrames);
      return {};
    }
    return Take(index, need);
  }

  void Free(const PoolSpan &span) {
    if (span)
      deferred_[State().frame].push_back(span);
  }

  // The slot's lists have finished: what was freed while it recorded is free.
  void ReleaseSlot(uint32_t slot) {
    for (const PoolSpan &span : deferred_[slot]) {
      Chunk &chunk = chunks_[span.chunk];
      Insert(chunk, span.offset, span.size);
      // Spans hold their chunk's index, so the entry stays for AddChunk to
      // reuse. The other slot may still draw from it: retired, not freed.
      if (span.chunk != 0 && chunk.free.size() == 1 && chunk.free.begin()->second == chunk.size) {
        Video::RetireLocked(std::move(chunk.buffer));
        chunk.cpu = nullptr;
        chunk.free.clear();
        chunk.size = 0;
        no_chunk_until_ = 0;  // banjo-nx (2026-10-28): its memory goes back: a new chunk may fit again
        if (span.chunk == draining_) {  // sasrt-nx (2026-11-10, sasrt_stream_cache_drain; port 58): drained
          ++drained_;
          draining_ = UINT32_MAX;
        }
      }
    }
    deferred_[slot].clear();
  }

  // banjo-nx (2026-10-27, banjo_renut_fetch_cache): a chunk's size (a draw binding the chunk binds all of it).
  uint64_t ChunkBytes(u32 index) const { return index < chunks_.size() ? chunks_[index].size : 0; }

  // sasrt-nx (2026-11-10, sasrt_stream_cache_drain; port 58): the chunk (not 0, which stays) with the fewest bytes in
  // use, or UINT32_MAX; the chunk being drained (taken last by Allocate) and the drains finished.
  u32 SparsestChunk() const {
    u32 best = UINT32_MAX;
    uint64_t best_used = UINT64_MAX;
    for (u32 i = 1; i < chunks_.size(); ++i) {
      const Chunk &c = chunks_[i];
      if (!c.buffer)
        continue;
      uint64_t free = 0;
      for (const auto &range : c.free)
        free += range.second;
      const uint64_t used = c.size > free ? c.size - free : 0;
      if (used < best_used) {
        best_used = used;
        best = i;
      }
    }
    return best;
  }
  u32 draining_ = UINT32_MAX;
  u32 drain_passes_ = 0;
  uint64_t drained_ = 0;

  // Host memory the chunks hold.
  uint64_t Bytes() const {
    uint64_t bytes = 0;
    for (const Chunk &chunk : chunks_)
      bytes += chunk.size;
    return bytes;
  }

private:
  // banjo-nx (2026-10-28, per-draw night, the heap): 32 MB chunks (64 before). A chunk goes back to the heap only when
  // every copy in it has gone, and the d4 city held 112 MB of copies in 192 MB of chunks with the heap at 2.9 of
  // 3.08 GB in 260,000 free pieces: smaller chunks leave less slack and find room in a fragmented heap. (A draw's
  // fetch buffers are kept together only in one chunk: not smaller than this.)
  static constexpr uint64_t kChunkSize = 32ull << 20;
  static constexpr uint64_t kAlign = 256;
  // banjo-nx (2026-10-28, night_gpu2 console freeze): see Allocate.
  static constexpr uint64_t kSmallChunkSize = 8ull << 20;
  static constexpr uint64_t kNoChunkFrames = 600;

  struct Chunk {
    std::unique_ptr<plume::RenderBuffer> buffer;  // null once released
    u8 *cpu = nullptr;
    uint64_t size = 0;
    std::map<uint64_t, uint64_t> free;  // offset -> size, adjacent ranges merged
  };

  // The index of a new chunk of at least 'min_size' bytes (and 'chunk_size'), or UINT32_MAX.
  u32 AddChunk(uint64_t min_size, uint64_t chunk_size = kChunkSize) {
    const uint64_t size = std::max(chunk_size, min_size);
    auto buffer = State().device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX));
    void *mapped = buffer ? buffer->map() : nullptr;
    if (!mapped)
      return UINT32_MAX;
    u32 index = 0;
    while (index < chunks_.size() && chunks_[index].buffer)
      ++index;
    if (index == chunks_.size())
      chunks_.emplace_back();
    buffer->setName("renut stream cache #" + std::to_string(index));
    Chunk &chunk = chunks_[index];
    chunk.buffer = std::move(buffer);
    chunk.cpu = static_cast<u8 *>(mapped);
    chunk.size = size;
    chunk.free.emplace(0, size);
    return index;
  }

  // First fit in chunk 'index'.
  PoolSpan Take(u32 index, uint64_t size) {
    Chunk &chunk = chunks_[index];
    for (auto it = chunk.free.begin(); it != chunk.free.end(); ++it) {
      if (it->second < size)
        continue;
      const uint64_t offset = it->first, left = it->second - size;
      chunk.free.erase(it);
      if (left)
        chunk.free.emplace(offset + size, left);
      PoolSpan span;
      span.buffer = chunk.buffer.get();
      span.cpu = chunk.cpu + offset;
      span.chunk = index;
      span.offset = offset;
      span.size = u32(size);
      return span;
    }
    return {};
  }

  static void Insert(Chunk &chunk, uint64_t offset, uint64_t size) {
    auto next = chunk.free.lower_bound(offset);
    if (next != chunk.free.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == offset) {
        offset = prev->first;
        size += prev->second;
        chunk.free.erase(prev);
      }
    }
    if (next != chunk.free.end() && offset + size == next->first) {
      size += next->second;
      chunk.free.erase(next);
    }
    chunk.free.emplace(offset, size);
  }

  std::vector<Chunk> chunks_;
  std::vector<PoolSpan> deferred_[kFramesInFlight];
  uint64_t no_chunk_until_ = 0;  // banjo-nx (2026-10-28): g_stream_cache_frame
  uint32_t chunk_failures_ = 0;
};
CopyPool g_copy_pool;

// A direct-mapped memo of a node map's entries by key hash, in front of the
// stream and index caches: draws look up the same keys every frame, and the
// map's bucket and node chain were cache misses each time (a tenth of the
// render thread). Here a repeat is one line and the entry itself, which the
// draw reads anyway. The entries stay where they are until erased (the map is
// node-based, a rehash moves none), so the memo is cleared whenever the map
// erases any.
template <typename Map, typename Hash, u32 kBits>
class EntryMemo {
public:
  using Entry = typename Map::value_type;

  // banjo-nx (2026-10-23, render-thread round): the slot holds the key's hash and the entry; the key is compared in the
  // node, whose line the draw reads next for the entry's fields anyway (libstdc++ keeps the key at the node's start).
  // Slots of 16 bytes rather than a key each: four times as many in less memory. The city's 5,000 draws a frame missed
  // the 4,096 / 2,048 slots of before often enough to spend 2.5 % of the console's render thread in the maps' buckets.
  Entry *Find(const typename Map::key_type &key, size_t hash) const {
    const Slot &slot = slots_[Index(hash)];
    return slot.hash == hash && slot.entry && slot.entry->first == key ? slot.entry : nullptr;
  }

  // The map's entry for 'key', made if 'make' (else null when it has none).
  Entry *Get(Map &map, const typename Map::key_type &key, bool make) { return Get(map, key, Hash{}(key), make); }
  // bangaio-nx (2026-10-29): with the key's hash made already (RangeEntry).
  Entry *Get(Map &map, const typename Map::key_type &key, size_t hash, bool make) {
    if (Entry *entry = Find(key, hash))
      return entry;
    auto it = map.find(key);
    if (it == map.end()) {
      if (!make)
        return nullptr;
      it = map.try_emplace(key).first;
    }
    slots_[Index(hash)] = {hash, &*it};
    return &*it;
  }
  // bangaio-nx (2026-10-29): a new entry for a key Get(.., false) just found none for.
  Entry *Make(Map &map, const typename Map::key_type &key, size_t hash) {
    Entry *entry = &*map.try_emplace(key).first;
    slots_[Index(hash)] = {hash, entry};
    return entry;
  }
  // bangaio-nx (2026-10-29, v5): the slot of an entry about to be erased, if it names it (one entry, not Clear()).
  void Forget(size_t hash, const Entry *entry) {
    Slot &slot = slots_[Index(hash)];
    if (slot.entry == entry)
      slot = Slot{};
  }

  void Clear() { std::fill(std::begin(slots_), std::end(slots_), Slot{}); }

  // banjo-nx (2026-10-23, render-thread round): prefetching ahead of Get. The slot 'key' hashes to; then, once that
  // line is in, the entry the slot names for the hash (the key is compared when Get reads it).
  void PrefetchSlot(const typename Map::key_type &key) const { __builtin_prefetch(&slots_[Index(Hash{}(key))]); }
  void PrefetchEntry(const typename Map::key_type &key) const {
    const size_t hash = Hash{}(key);
    const Slot &slot = slots_[Index(hash)];
    if (slot.hash == hash && slot.entry) {
      __builtin_prefetch(slot.entry);
      __builtin_prefetch(reinterpret_cast<const char *>(slot.entry) + 64);
    }
  }

private:
  struct Slot {
    size_t hash = 0;
    Entry *entry = nullptr;
  };
  static u32 Index(size_t hash) {
    return u32((uint64_t(hash) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
  }
  Slot slots_[1u << kBits];
};

struct CachedStream {
  uint64_t whole_seen = UINT64_MAX;  // banjo-nx (2026-10-26, stage 2): g_flush_serial its whole range was clean from
  uint64_t hash = 0;  // guest bytes at the last conversion, or of the copy
  u32 stable = 0;     // flushes in a row that converted the same bytes
  u32 drops = 0;      // copies dropped because the guest bytes changed
  bool dynamic = false;
  bool whole = false;  // banjo-nx (2026-10-28): a whole buffer's entry (WholeStreamCopy): objects and memos may hold it
  PoolSpan copy;
  u32 bytes = 0;
  uint64_t last_used = 0;               // g_stream_cache_frame
  uint64_t checked_frame = UINT64_MAX;  // g_stream_cache_frame the copy was last checked in
  uint64_t proven_flush = UINT64_MAX;   // banjo-nx: g_flush_serial the copy was last hashed equal in
  uint64_t exact_frame = UINT64_MAX;    // banjo-nx: g_stream_cache_frame of its last exact check (repeat count)
  // bangaio-nx (2026-10-29, v5): kicks in a row at which its bytes differed from the kick before (ForgetChangingRanges),
  // and whether they were hashed yet.
  u8 changes = 0;
  bool hashed = false;
  // renut-ultimate (2026-11-16, port 145; tw2de_nx's seen19; banjo_renut_stream_redeem): a dynamic stream's quiet run
  // (RedeemQuietStream): the flush it was last converted at, how many conversions in a row found its memory unwritten
  // since the one before, and how often it came back.
  uint64_t quiet_flush = UINT64_MAX;
  u32 quiet = 0;
  u32 redeemed = 0;
};
using StreamCache = std::unordered_map<StreamKey, CachedStream, StreamKeyHash>;
StreamCache g_stream_cache;
EntryMemo<StreamCache, StreamKeyHash, 14> g_stream_memo;  // banjo-nx: 16,384 slots (2026-10-23; 4,096 before)

// bangaio-nx (2026-10-29, banjo_renut_stream_second_sighting, A/B 71): the ranges read once and not in the cache, by
// key hash. Direct-mapped, a slot a 16-bit tag and the low 16 bits of the frame of the range's first sighting. A slot
// taken by another range only delays the entry of the one it held; a tag two ranges share (or the frame number come
// round again) only makes or delays an entry by a sighting. Either way the draw converts into the upload ring as a
// fresh entry's does, so the picture is the same.
// v5 (the second console run: still 108,892 entries): the entry is made at a sighting in a LATER frame. Bangai-O draws
// its sprites twice a frame (the 1280x720 pass and the 640x360 glow pass), so a range read only in one frame was
// sighted twice there. A copy needs kStableFlushes flushes in a row with the same bytes, which a range of one frame
// never has; its second draw in the frame takes the first's conversion anyway (FindQueuedStream).
// v5 also: a range whose entry ForgetChangingRanges dropped (its bytes new at every kick) is marked here, and gets no
// entry while the mark holds.
class RangeSightings {
public:
  // Whether the range was seen in an earlier frame (its slot holds its tag and another frame); else its tag and this
  // frame go there (a sighting in the frame of the first keeps the slot, and so does a changing range's mark).
  bool SeenBefore(size_t hash, uint64_t frame) {
    u32 &slot = tags_[Index(hash)];
    const u32 tag = Tag(hash);
    const u32 now = tag | u32(frame & kFrameBits);
    if ((slot & 0xFFFF0000u) == tag && slot != 0) {
      if (slot & kChanging)
        return false;
      return slot != now;
    }
    slot = now;
    return false;
  }
  void MarkChanging(size_t hash) { tags_[Index(hash)] = Tag(hash) | kChanging; }

private:
  static constexpr u32 kFrameBits = 0x7FFFu;  // the frame's low 15 bits
  static constexpr u32 kChanging = 0x8000u;
  static u32 Tag(size_t hash) { return u32(uint64_t(hash) >> 48) << 16; }
  static constexpr u32 kBits = 15;  // 32,768 slots, 128 KB: the storm's ranges, ~4,000 a frame, and the marks
  static u32 Index(size_t hash) { return u32((uint64_t(hash) * 0xD6E8FEB86659FD93ull) >> (64 - kBits)); }
  u32 tags_[1u << kBits] = {};
};
RangeSightings g_range_sightings;
uint64_t g_first_sightings = 0;  // the 10 s line's: ranges drawn without an entry
// bangaio-nx (2026-10-29, v5): TrackConvertedStream's entries whose bytes changed at kRangeChanges kicks in a row,
// dropped at the frame's start (ForgetChangingRanges); the 10 s line's count of them.
constexpr u8 kRangeChanges = 3;
constexpr uint64_t kRangeIdleFrames = 120;  // a range entry with no copy, unused this long: dropped (the idle pass)
// bangaio-nx (2026-10-29, v6): every range entry RangeEntry makes, with the frame it was made in, oldest first
// (AgeRanges). The v5 console storm still kept 131,408 entries: the game's sprite ranges came round again within 120
// frames, so neither the idle rule nor the changing rule (3 kicks in a row) dropped them.
struct RangeAge {
  StreamKey key;
  uint64_t frame;
};
std::vector<RangeAge> g_range_ages;
size_t g_range_age_head = 0;
constexpr uint64_t kRangeAgeFrames = 8;
std::vector<StreamKey> g_doomed_ranges;
uint64_t g_ranges_forgotten = 0;

inline bool SecondSightingOn() {
  static const bool on = REXCVAR_GET(banjo_renut_stream_second_sighting);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 71;
}

// The cache entry BindStreams reads a range through: null on its first sighting (no entry made).
CachedStream *RangeEntry(const StreamKey &key) {
  if (!SecondSightingOn())
    return &g_stream_memo.Get(g_stream_cache, key, true)->second;
  const size_t hash = StreamKeyHash{}(key);
  if (StreamCache::value_type *entry = g_stream_memo.Get(g_stream_cache, key, hash, false))
    return &entry->second;
  if (!g_range_sightings.SeenBefore(hash, g_stream_cache_frame)) {
    ++g_first_sightings;
    return nullptr;
  }
  g_range_ages.push_back({key, g_stream_cache_frame});  // bangaio-nx (2026-10-29, v6): AgeRanges looks at it
  return &g_stream_memo.Make(g_stream_cache, key, hash)->second;
}

// Whether a cached copy last checked in 'checked_frame' is due a check now.
bool CacheCheckDue(uint64_t checked_frame) {
  return checked_frame == UINT64_MAX || g_stream_cache_frame - checked_frame >= kCacheVerifyFrames;
}

// banjo-nx (package 27c): banjo_renut_exact_streams' checks, through a memo of the ranges hashed in this interval
// (between two kicks): a buffer cached under several stream keys (layouts, stream slots) or index keys is read once.
// Exact as the checks themselves: they hash at the first use in the interval, and so does the memo's first.
struct IntervalHash {
  const void *data = nullptr;
  size_t size = 0;
  uint64_t serial = UINT64_MAX;
  uint64_t hash = 0;
};
IntervalHash g_interval_hashes[4096];

// banjo-nx (2026-10-19, CPU proposal 1, banjo_renut_store_cards): the store cards (rex/system/xmemory.h,
// tools/store_cards.py). Every guest store outside the stack zeroes the card of its page (the byte at its guest page
// number in the first MB of the guest window), and so does every host writer of guest memory. A copy's check reads the
// cards of its pages, sets those it finds written back to 1 and records the interval (g_flush_serial) it found them in;
// the copy is current when no page of it was found written in or after the interval it was last proven in. A write
// whose card the check reads before its data is visible is found in that interval, so the copy is hashed again in the
// next one, when the data has long landed: the cards never answer for an interval in which they were cleared.
//
// A physical page's cards are those of the views the guest writes it through: 0xA0000000, 0xC0000000, 0xE0000000
// (4 KB lower: guest 0xE0000000 is physical 0x1000) and, in the first 16 MB, 0x7F000000. A store marks its first
// byte's page only, so an unaligned store that runs into the next page leaves that page's cards clean: a page also
// counts as written when the page before it was.
uint32_t g_page_serial[0x20000];

// banjo_renut_store_cards_verify (PC): what the checks found. [0] pages read, [1] found written, [2..5] by the view
// 0xA0000000 / 0xC0000000 / 0xE0000000 / 0x7F000000, [6] copies written only through the page before theirs,
// [7] copies hashed after a written page whose bytes had not changed, [8] copies not in physical memory.
uint64_t g_card_diag[9];
uint32_t g_card_samples[8];
uint32_t g_card_sample_count;

bool LogLimited(uint32_t &counter, uint32_t limit);

// The cards: the guest window's first MB (rex::memory::g_store_cards, a runtime variable this file cannot link to on
// Windows, where the runtime is a DLL).
u8 *StoreCards() {
  static u8 *const cards = REX_KERNEL_MEMORY()->virtual_membase();
  return cards;
}

// The physical address of the guest memory at 'data' (a pointer into the raw physical view, or into the guest window at
// a guest address of one of the views of physical memory: a vertex or index buffer's CPU address, where the game wrote
// it), or UINT32_MAX for anything else.
u32 PhysicalOf(const u8 *data) {
  static const u8 *const physical = GuestPhysicalPtr<const u8>(0);
  const uintptr_t raw = uintptr_t(data) - uintptr_t(physical);
  if (raw < 0x20000000u)
    return u32(raw);
  const uint64_t window = uint64_t(uintptr_t(data) - uintptr_t(StoreCards()));
  if (window >= 0x100000000ull)
    return UINT32_MAX;
  u32 va = u32(window);
  if (rex::memory::detail::PhysicalHostOffset(va) && va >= 0xE0001000u)
    va -= 0x1000u;  // the PC's host offset of the 0xE0000000 view
  if (va >= 0xA0000000u)
    return (va & 0x1FFFFFFFu) + (va >= 0xE0000000u ? 0x1000u : 0u);
  if (va >= 0x7F000000u && va < 0x80000000u)
    return va - 0x7F000000u;
  return UINT32_MAX;
}

// banjo-nx (2026-10-23, render-thread round): the lines PagesClean reads first for a range: its first pages' cards in
// the four views and their serials. Prefetches only.
void PrefetchCards(const u8 *data, size_t bytes) {
  u8 *cards = StoreCards();
  const u32 offset = cards && bytes ? PhysicalOf(data) : UINT32_MAX;
  if (offset == UINT32_MAX)
    return;
  const u32 page = (offset >> 12) - ((offset >> 12) != 0);
  __builtin_prefetch(cards + 0xA0000u + page);
  __builtin_prefetch(cards + 0xC0000u + page);
  __builtin_prefetch(cards + 0xE0000u + page);
  if (page < 0x1000u)
    __builtin_prefetch(cards + 0x7F000u + page);
  __builtin_prefetch(&g_page_serial[page]);
}

bool PagesClean(const u8 *data, size_t bytes, uint64_t proven) {
  u8 *cards = StoreCards();
  const u32 offset = cards && bytes ? PhysicalOf(data) : UINT32_MAX;
  const bool diag = banjo::renut_gpu::StoreCardsVerifyOn();
  if (offset == UINT32_MAX || bytes > 0x20000000u - offset) {
    if (diag)
      ++g_card_diag[7 + 1];  // not physical memory
    return false;
  }
  const u32 serial = u32(g_flush_serial);
  const u32 last = u32((uint64_t(offset) + bytes - 1) >> 12);
  u32 page = offset >> 12;
  page -= page != 0;
  bool clean = true;
  const u32 own_first = offset >> 12;
  bool own_written = false;
  for (; page <= last; ++page) {
#if defined(__aarch64__)
    // banjo-nx (2026-10-23, render-thread round): sixteen pages at a time where none of their cards is written (all
    // four views' bytes non-zero: the scalar loop below would store nothing for them); then only their serials. Not
    // for page 0 (its 0xE0000000 card is another view's), nor across the 16 MB the 0x7F000000 view covers.
    if (!diag && page != 0 && last - page >= 15 && (page + 16 <= 0x1000u || page >= 0x1000u)) {
      const uint8x16_t ca = vld1q_u8(cards + 0xA0000u + page), cc = vld1q_u8(cards + 0xC0000u + page);
      const uint8x16_t ce = vld1q_u8(cards + 0xE0000u + page - 1);
      const uint8x16_t cf = page < 0x1000u ? vld1q_u8(cards + 0x7F000u + page) : ca;
      if (vminvq_u8(vminq_u8(vminq_u8(ca, cc), vminq_u8(ce, cf))) != 0) {
        const uint32x4_t p = vdupq_n_u32(u32(proven));
        const u32 *serials = g_page_serial + page;
        const uint32x4_t newer = vorrq_u32(vorrq_u32(vcgeq_u32(vld1q_u32(serials), p), vcgeq_u32(vld1q_u32(serials + 4), p)),
                                           vorrq_u32(vcgeq_u32(vld1q_u32(serials + 8), p), vcgeq_u32(vld1q_u32(serials + 12), p)));
        if (vmaxvq_u32(newer) != 0)
          clean = false;
        page += 15;  // the loop's ++page makes 16
        continue;
      }
    }
    // banjo-nx (2026-10-28, per-draw night): eight at a time the same way (a range's last 8-15 pages, and ranges of
    // 8-15 pages, went a page at a time).
    if (!diag && page != 0 && last - page >= 7 && (page + 8 <= 0x1000u || page >= 0x1000u)) {
      const uint8x8_t ca = vld1_u8(cards + 0xA0000u + page), cc = vld1_u8(cards + 0xC0000u + page);
      const uint8x8_t ce = vld1_u8(cards + 0xE0000u + page - 1);
      const uint8x8_t cf = page < 0x1000u ? vld1_u8(cards + 0x7F000u + page) : ca;
      if (vminv_u8(vmin_u8(vmin_u8(ca, cc), vmin_u8(ce, cf))) != 0) {
        const uint32x4_t p = vdupq_n_u32(u32(proven));
        const u32 *serials = g_page_serial + page;
        const uint32x4_t newer = vorrq_u32(vcgeq_u32(vld1q_u32(serials), p), vcgeq_u32(vld1q_u32(serials + 4), p));
        if (vmaxvq_u32(newer) != 0)
          clean = false;
        page += 7;  // the loop's ++page makes 8
        continue;
      }
    }
#endif
    u8 *a = cards + 0xA0000u + page, *c = cards + 0xC0000u + page;
    u8 *e = page ? cards + 0xE0000u + page - 1 : a, *f = page < 0x1000u ? cards + 0x7F000u + page : a;
    if (diag) {
      ++g_card_diag[0];
      const bool va = !*a, vc = !*c, ve = !*e, vf = !*f;
      if (va || vc || ve || vf) {
        ++g_card_diag[1];
        g_card_diag[2] += va;
        g_card_diag[3] += vc;
        g_card_diag[4] += ve;
        g_card_diag[5] += vf;
        own_written |= page >= own_first;
        if (g_card_sample_count < 8)
          g_card_samples[g_card_sample_count++] = page | (va ? 1u << 28 : 0) | (vc ? 1u << 29 : 0) |
                                                  (ve ? 1u << 30 : 0) | (vf ? 1u << 31 : 0);
      }
    }
    if (!__atomic_load_n(a, __ATOMIC_RELAXED) || !__atomic_load_n(c, __ATOMIC_RELAXED) ||
        !__atomic_load_n(e, __ATOMIC_RELAXED) || !__atomic_load_n(f, __ATOMIC_RELAXED)) {
      __atomic_store_n(a, u8(1), __ATOMIC_RELAXED);
      __atomic_store_n(c, u8(1), __ATOMIC_RELAXED);
      __atomic_store_n(e, u8(1), __ATOMIC_RELAXED);
      __atomic_store_n(f, u8(1), __ATOMIC_RELAXED);
      g_page_serial[page] = serial;
    }
    if (g_page_serial[page] >= u32(proven))
      clean = false;
  }
  if (diag && !clean && !own_written && own_first != 0 && g_page_serial[own_first - 1] >= u32(proven))
    ++g_card_diag[6];
  return clean;
}

// The diagnostics' 10 s line (banjo_renut_tuning.cpp calls it in verify mode).
extern "C" uint64_t RexStoreCardsHostBytes(int which);

// Whether the cards prove a cached copy last proven in interval 'proven' (UINT64_MAX: never, made at a flush) whose
// guest bytes hashed 'hash' then. The cards are read, and cleared, before any hash of the range in this interval.
bool CardsProve(const u8 *data, size_t bytes, uint64_t proven, uint64_t hash) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kCardsProve);  // blur-nx (2026-11-17)
  if (!banjo::renut_gpu::StoreCardsOn())
    return false;
  const bool clean = PagesClean(data, bytes, proven) && proven != UINT64_MAX;
  if (clean && banjo::renut_gpu::StoreCardsVerifyOn() && banjo::renut_gpu::ContentHash64(data, bytes) != hash) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 32)) {
      static const u8 *const physical = GuestPhysicalPtr<const u8>(0);
      RGPU_WARN("[cards] physical {:08X}-{:08X} ({} bytes) changed under clean cards: a writer of guest memory that "
                "marks none (banjo_renut_store_cards_verify)",
                u32(data - physical), u32(data - physical + bytes - 1), bytes);
    }
    banjo::renut_gpu::CountCardsMissed(bytes);
    return false;
  }
  banjo::renut_gpu::CountCards(bytes, clean);
  return clean;
}

// banjo-nx (2026-10-23, batch 6, banjo_renut_kick_reuse): whether an upload an earlier flush of this frame filled from
// [data, data + bytes) (the flush that started interval 'made') still holds those bytes now: the upload rings were not
// reset since (they keep what is written until then) and the guest pages were not written since the interval before
// 'made' (the store cards; that interval too, for a write whose card was taken before its bytes landed). Proven once per
// interval ('checked'). The game writes a buffer after the draws that read it only once the GPU is done with them (after
// a kick): a write in this interval before the draw is in the cards now, and none comes between the draw and its kick.
bool KickHeld(const u8 *data, size_t bytes, uint64_t made, uint64_t ring_resets, uint64_t &checked) {
  if (!KickReuseOn() || ring_resets != UploadRing::Resets() || !banjo::renut_gpu::StoreCardsOn() || made == 0 ||
      made > g_flush_serial)
    return false;
  if (checked == g_flush_serial)
    return true;
  if (!PagesClean(data, bytes, made - 1))
    return false;
  checked = g_flush_serial;
  return true;
}

// The fetch copy 'known' holds [src, src + size) for a draw whose fetch slots count from 'chunk': queued since the last
// flush (as before), or kept from an earlier flush of the frame (KickHeld).
bool FetchCopyHeld(FetchCopy &known, const u8 *src, u32 size, const plume::RenderBuffer *chunk) {
  if (known.src != src || known.size != size || known.buffer != chunk || !src)
    return false;
  if (known.generation == g_fetch_copy_generation)
    return true;
  const bool first = known.checked_serial != g_flush_serial;
  if (!KickHeld(src, size, known.made_serial, known.ring_resets, known.checked_serial))
    return false;
  if (first) {
    if (banjo::renut_gpu::StoreCardsVerifyOn() && known.host && std::memcmp(known.host, src, size) != 0) {
      banjo::renut_gpu::CountKickReuse(2, size);
      known.checked_serial = 0;
      known.ring_resets = 0;  // made again
      return false;
    }
    banjo::renut_gpu::CountKickReuse(0, size);
  }
  return true;
}

bool QueuedStillHeld(QueuedStream &q) {
  const bool first = q.checked_serial != g_flush_serial;
  if (!KickHeld(q.key.guest, size_t(q.key.count) * q.key.stride, q.made_serial, q.ring_resets, q.checked_serial))
    return false;
  if (first)
    banjo::renut_gpu::CountKickReuse(1, q.alloc.size);
  return true;
}

uint64_t ExactHash(const void *data, size_t size) {
  const uintptr_t k = (reinterpret_cast<uintptr_t>(data) >> 4) ^ (uintptr_t(size) * 0x9E3779B1u);
  IntervalHash &e = g_interval_hashes[(k ^ (k >> 12)) & 4095];
  if (e.serial == g_flush_serial && e.data == data && e.size == size) {
    banjo::renut_gpu::CountExactMemoHit(size);
    return e.hash;
  }
  e = {data, size, g_flush_serial, banjo::renut_gpu::ContentHash64(data, size)};
  return e.hash;
}

uint64_t HashGuestStream(const StreamKey &key) {
  // banjo-nx: banjo_renut_pmull_hash (the PMULL CRC-64 on ARM).
  return banjo::renut_gpu::ContentHash64(key.guest, size_t(key.count) * key.stride);
}

// The cached copies' checks, hashed off the render thread: every copy's guest
// bytes each kCacheVerifyFrames frames was a twentieth of its time. A copy
// found stale is dropped when the render thread takes the results, at the next
// frame (AdvanceStreamCacheFrameLocked), so a rewritten range can draw from
// its old copy for about a frame longer than the check interval alone allows.
template <typename Key, uint64_t (*Hash)(const Key &)>
class ContentVerifier {
public:
  struct Check {
    Key key;
    uint64_t hash;  // the copy's hash when submitted
  };

  ~ContentVerifier() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable())
      thread_.join();
  }

  // Render thread. Collected until the frame's Flush: waking the worker for
  // each check was a system call per draw.
  void Submit(const Key &key, uint64_t hash) { submitted_.push_back({key, hash}); }

  // Render thread, once a frame: hands the frame's checks to the worker.
  void Flush() {
    if (submitted_.empty())
      return;
    {
      std::lock_guard lock(mutex_);
      if (!thread_.joinable())
        thread_ = std::thread([this] { Run(); });
      pending_.insert(pending_.end(), submitted_.begin(), submitted_.end());
    }
    submitted_.clear();
    wake_.notify_one();
  }

  // Render thread: the checks whose bytes no longer hash as submitted.
  void TakeStale(std::vector<Check> &out) {
    std::lock_guard lock(mutex_);
    out.swap(stale_);
    stale_.clear();
  }

private:
  void Run() {
    // banjo-nx: the background core and priority on the Switch (banjo_renut_tuning.h).
    banjo::renut_gpu::OnBackgroundThreadStart("content verifier");
    std::vector<Check> batch;
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return stop_ || !pending_.empty(); });
      if (stop_)
        return;
      batch.swap(pending_);
      lock.unlock();
      // Guest memory stays mapped; a read torn by a game write only fails
      // the check, which converts the range again.
      std::erase_if(batch, [](const Check &c) { return Hash(c.key) == c.hash; });
      lock.lock();
      stale_.insert(stale_.end(), batch.begin(), batch.end());
      batch.clear();
    }
  }

  std::vector<Check> submitted_;  // render thread only
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Check> pending_, stale_;
  bool stop_ = false;
  std::thread thread_;
};
ContentVerifier<StreamKey, HashGuestStream> g_stream_verifier;

void DropCachedCopy(CachedStream &c) {
  if (!c.copy)
    return;
  // blur-nx (2026-11-18, the Renut host batch's #36, blur_renut_mesh_gen_whole_only / A/B 240): mesh objects (and the
  // geometry objects, the whole-repeat answer) name whole copies alone (WholeStreamCopy marks each entry it answers
  // 'whole' before it returns), so a range copy's drop leaves them all valid. Every drop raised the generation: the
  // console race's mesh objects missed 16,562 draws a 10 s as "of an older generation" (generation 5,555 by the end).
  if (c.whole || !::renut::gpu::host_batch::On(::renut::gpu::host_batch::kMeshGenWhole))
    ++g_mesh_gen;  // banjo-nx (2026-10-27): mesh objects may name it
  g_stream_cache_bytes -= c.bytes;
  g_copy_pool.Free(c.copy);
  c.copy = {};
  c.bytes = 0;
}

// renut-ultimate (2026-11-16, port 145; tw2de_nx's seen19 of 2026-11-06, banjo_renut_stream_redeem, on by default):
// a stream turned dynamic (its guest bytes changed kMaxCacheDrops times) stays so for good and is converted at every
// kick (tw2de_nx: 13.7-15.4 MB of streams a frame, 7-7.7 ms of the render thread): a stream the game rewrote a few
// times (an area's first seconds) and then left alone paid that for the rest of the area. At each conversion of a
// dynamic stream the store cards say whether its memory was written since the conversion before (PagesClean, as the
// cached copies are proven); after kRedeemFlushes such conversions in a row it is a plain stream again (no drops, no
// stable count) and takes the usual way to a cached copy (kStableFlushes equal hashes, then the copy and its checks:
// exact). A stream that turns dynamic again after kMaxRedeems returns stays so.
constexpr u32 kRedeemFlushes = 60;
constexpr u32 kMaxRedeems = 4;
uint64_t g_redeemed_streams = 0;
void RedeemQuietStream(const PendingStreamFill &f) {
  auto it = g_stream_cache.find(f.key);
  if (it == g_stream_cache.end() || !it->second.dynamic)
    return;
  CachedStream &c = it->second;
  const size_t bytes = size_t(f.count) * f.stride;
  if (!banjo::renut_gpu::StoreCardsOn() || c.redeemed >= kMaxRedeems) {
    c.quiet = 0;
    return;
  }
  const bool clean = c.quiet_flush != UINT64_MAX && PagesClean(f.guest, bytes, c.quiet_flush);
  c.quiet_flush = g_flush_serial;
  if (!clean) {
    c.quiet = 0;
    return;
  }
  if (++c.quiet < kRedeemFlushes)
    return;
  c.dynamic = false;
  c.drops = 0;
  c.stable = 0;
  c.hash = 0;
  c.quiet = 0;
  c.quiet_flush = UINT64_MAX;
  ++c.redeemed;
  if (++g_redeemed_streams <= 16 || (g_redeemed_streams & 1023) == 0)
    RGPU_INFO("[renut] banjo_renut_stream_redeem: {} dynamic streams quiet for {} conversions made cacheable again",
              g_redeemed_streams, kRedeemFlushes);
}

// After a flush converted 'f': count how long its bytes have held, and give a
// stream that has held long enough its persistent copy.
void TrackConvertedStream(const PendingStreamFill &f) {
  auto it = g_stream_cache.find(f.key);
  if (it == g_stream_cache.end())
    return;
  CachedStream &c = it->second;
  if (c.dynamic || c.copy)
    return;
  // banjo-nx (2026-10-23, batch 5): at the cap a stream gets no copy whatever its hash says: the hash (its guest bytes,
  // every flush) is not made then. Its count of stable flushes starts again once there is room.
  const u32 bytes = f.count * f.host_stride;
  if (g_stream_cache_bytes + bytes > banjo::renut_gpu::StreamCacheMaxBytes(kStreamCacheMaxBytes)) {  // banjo-nx
    ++g_stream_cache_refused;
    c.stable = 0;
    return;
  }
  const uint64_t hash = HashGuestStream(f.key);
  // bangaio-nx (2026-10-29, v5, banjo_renut_stream_second_sighting): a range entry (no copy, see above) whose bytes were
  // new at kRangeChanges kicks in a row is dropped at the frame's start (ForgetChangingRanges): Bangai-O rewrites its
  // sprite buffers at the same places every frame, so those ranges recur with new bytes and were never cached, but
  // kept ~100,000 entries that the replaced-range and idle passes walked (the v4 console storm). Not here: a kick may
  // come in the middle of a draw's BindStreams, which holds its entry.
  const bool same = hash == c.hash;
  if (same)
    c.changes = 0;
  else if (c.hashed && !c.whole && ++c.changes == kRangeChanges && SecondSightingOn())
    g_doomed_ranges.push_back(f.key);
  c.hashed = true;
  c.stable = same ? c.stable + 1 : 0;
  c.hash = hash;
  if (c.stable + 1 < kStableFlushes)
    return;
  const PoolSpan copy = g_copy_pool.Allocate(bytes);
  if (!copy)
    return;
  // Converted again rather than copied from f.host: upload memory is
  // write-combined, slow to read back.
  ConvertVertices(g_fill_elements.data() + f.first_element, f.element_count, f.host_stride, f.guest, f.stride, f.count,
                  f.endian, copy.cpu);
  c.copy = copy;
  c.bytes = bytes;
  c.checked_frame = g_stream_cache_frame;
  c.proven_flush = UINT64_MAX;  // banjo-nx: made at a flush; proven again at its first use after it
  g_stream_cache_bytes += bytes;
}

// banjo-nx (2026-10-27, render thread night): the reset out of line and cold: Bound is about 25 KB, and inlined into the
// draw functions its temporary made FastDrawLocked's stack frame 31 KB (3.4 KB before; b12's objdump).
[[gnu::cold, gnu::noinline]] void ResetBound(uint64_t generation) {
  g_bound = Bound{};
  g_bound.generation = generation;
}
[[gnu::always_inline]] inline void SyncBound(VideoState &s) {  // banjo-nx (2026-10-27): inline (a call a draw)
  if (g_bound.generation != s.list_generation) [[unlikely]]
    ResetBound(s.list_generation);
}

bool LogLimited(uint32_t &counter, uint32_t limit = 8) {
  return counter++ < limit;
}

// Guest VA or GPU physical address to a host pointer.
const u8 *AddressPtr(u32 address) {
  if (address < 0x20000000u)
    return GuestPhysicalPtr<const u8>(address);
  return GuestPtr<const u8>(address);
}

plume::RenderPrimitiveTopology Topology(u32 primitive) {
  switch (primitive) {
  case kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case kLineList:
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case kLineStrip:
  case kLineLoop:
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  default:  // lists, and fans / quads expanded to lists
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
}

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------

struct Targets {
  HostSurface *colors[d3d::kMaxRenderTargets] = {};
  uint32_t color_count = 0;
  HostSurface *depth = nullptr;
  // In guest pixels, as everything the draws and clears work out; host
  // viewports, scissors and rects are 'scale' times that (HostSurface::scale).
  uint32_t width = 0, height = 0;
  RenderScale scale;
  // The size the guest surface header gives; smaller than width x height for
  // a target grown for tiling (SetTilingExtentLocked).
  uint32_t guest_width = 0, guest_height = 0;
  // banjo-nx (2026-10-21, internal resolution): the device's depth target was left out (DropSmallerDepth); the pipeline
  // key keeps the draw's depth and stencil words, so its shader variants are the ones it had with the target bound
  // (constant 909 is decided by them; without a depth format the pipeline takes no depth state from them).
  bool depth_dropped = false;
};

// banjo-nx (2026-10-19, banjo_deko3d_depth_handover; "Depth hand-over" below): the targets of the draw whose textures
// FillTextures binds (a destination is not sampled from the image of a target of its own draw), and what is done
// before a depth target is bound again.
const Targets *g_handover_targets = nullptr;

// sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order; the fourth GPU round's census): one game frame's memexport draws,
// stream conversions (their waits) and the draws between them, every N seconds, as one token line: E<window>[/n] an
// export draw (its first window's offset / 256, n windows), C<w>x<h>[L] a conversion recorded for a draw into a w x h
// target (L: with the wait before it) and its jobs <source offset / 256>:<vertices>, H<offset> a conversion found in
// the memo (no dispatch, no wait), d<n> n other draws, T<w>x<h>[z] a full-path draw into another target size (z: depth
// only). Nothing is changed.
REXCVAR_DEFINE_INT32(sasrt_gpu_diag_export_order, 0, "SASRT/Diagnostics",
                     "Diagnostic (2026-11-08): every N seconds (0 off), log one game frame's memexport draws, stream "
                     "conversions and the draws between them in order ([sasrt] export order): whether the conversions' "
                     "waits could be shared");
uint64_t g_sasrt_order_draws = 0;  // every draw recorded (fast and full)
struct SasrtOrder {
  uint64_t frame = ~0ull, draws_at = 0;
  bool on = false;
  u32 tw = 0, th = 0;
  std::string line;
  std::chrono::steady_clock::time_point next;
};
SasrtOrder g_sasrt_order;
int32_t SasrtOrderEvery() {
  static const int32_t every = REXCVAR_GET(sasrt_gpu_diag_export_order);
  return every;
}
void SasrtOrderToken(const std::string &token) {
  SasrtOrder &o = g_sasrt_order;
  const uint64_t frame = g_sasrt_game_frames.load(std::memory_order_relaxed);
  if (frame != o.frame) {
    if (o.on) {
      RGPU_INFO("[sasrt] export order (sasrt_gpu_diag_export_order), game frame {}: {} characters", o.frame,
                o.line.size());
      for (size_t at = 0; at < o.line.size(); at += 900)
        RGPU_INFO("[sasrt] export order {}: {}", at / 900, o.line.substr(at, 900));
      o.on = false;
    }
    o.frame = frame;
    const auto now = std::chrono::steady_clock::now();
    if (now >= o.next) {
      o.on = o.next.time_since_epoch().count() != 0;  // the first frame seen only starts the clock
      o.next = now + std::chrono::seconds(SasrtOrderEvery());
      o.line.clear();
      o.draws_at = g_sasrt_order_draws;
      o.tw = o.th = 0;
    }
  }
  if (!o.on)
    return;
  if (g_sasrt_order_draws > o.draws_at)
    o.line += fmt::format("d{} ", g_sasrt_order_draws - o.draws_at);
  o.draws_at = g_sasrt_order_draws;
  if (!token.empty()) {
    o.line += token;
    o.line += ' ';
  }
}
// kameo-nx (2026-11-03, banjo_deko3d_depth_handover_sampled): the draw writes neither depth nor stencil, so it may sample a
// resolve left in the image of its own depth target (HandOverBeforeDrawLocked sets it, before the draw's textures).
bool g_handover_depth_readonly = false;
// Whether a resolve left in 'source' cannot be sampled from there by the draw whose textures are being bound.
inline bool HandOverSourceIsDrawTarget(const HostSurface *source) {
  const Targets *t = g_handover_targets;
  if (!t)
    return false;
  if (t->depth == source)
    return !g_handover_depth_readonly;
  for (uint32_t i = 0; i < t->color_count; ++i)
    if (t->colors[i] == source)
      return true;
  return false;
}
void DepthHandOverBeforeBindLocked(HostSurface *surface, const plume::RenderRect *clear_rect, u32 clear_flags);
// banjo-nx (2026-10-28, GPU block, diagnostic): why the next MaterialiseLocked draws (the hand-over line's causes): 7 a
// bind for a draw writing depth or stencil, 8 a draw sampling it with its target bound, 9 other.
int g_materialise_cause = 9;

// kameo-nx (2026-11-03, banjo_deko3d_color_handover, banjo_deko3d_depth_handover_sampled): before a draw looks at its
// textures. A colour target holding a resolve left in its image, or a depth target when the draw writes depth or stencil,
// gets that resolve drawn now, so the texture is its own by the time the draw binds it and no answer kept from an earlier
// draw can name the image the draw is about to write. Otherwise a depth target's image can be sampled while it is only
// tested. The same words as FastDrawLocked's and the full path's depth writes (the raw device's: a superset of the key's).
[[gnu::always_inline]] inline void HandOverBeforeDrawLocked(const Targets &t, u32 depth_control, u32 stencil_ref_mask) {
  const bool writes = ((depth_control & 2) && (depth_control & 4)) ||
                      ((depth_control & 1) && ((stencil_ref_mask >> 16) & 0xFF) != 0);
  const bool sampled = banjo::renut_gpu::DepthHandOverSampledOn();
  g_handover_depth_readonly = sampled && !writes;
  for (uint32_t i = 0; i < t.color_count; ++i) {
    if (t.colors[i]->handed) [[unlikely]] {
      g_materialise_cause = 7;
      DepthHandOverBeforeBindLocked(t.colors[i], nullptr, 0);
      g_materialise_cause = 9;
    }
  }
  if (sampled && writes && t.depth && t.depth->handed) [[unlikely]] {
    g_materialise_cause = 7;
    DepthHandOverBeforeBindLocked(t.depth, nullptr, 0);
    g_materialise_cause = 9;
  }
}

// A rect in guest pixels as host pixels of targets 'scale' times the size.
plume::RenderRect ScaleRect(const plume::RenderRect &rect, RenderScale scale) {
  return plume::RenderRect(scale.Coord(rect.left), scale.Coord(rect.top), scale.Coord(rect.right),
                           scale.Coord(rect.bottom));
}

bool RestoreDepthFromColorLocked(HostSurface *depth, HostSurface *color);  // sasrt-nx (2026-10-20): below

// blur-nx (2026-11-13, #55): GatherTargets on the target words (draw.h kResolveTargetWords: the four render targets,
// then the depth-stencil surface), as a queued clear keeps them.
bool GatherTargetsWords(const u32 *words, Targets &t);
bool GatherTargets(const GuestDevice &device, Targets &t) {
  u32 words[kResolveTargetWords];
  ReadResolveTargets(device.Bytes(0), words);
  return GatherTargetsWords(words, t);
}
bool GatherTargetsWords(const u32 *words, Targets &t) {
  for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i) {
    const u32 va = words[i];
    if (!va)
      break;
    HostSurface *surface = GetHostSurfaceLocked(va);
    if (!surface || surface->depth)
      break;
    t.colors[t.color_count++] = surface;
  }
  HostSurface *depth = GetHostSurfaceLocked(words[4]);  // kDeviceOffsetDepthStencil
  if (depth && depth->depth) {
    // rr6-nx (2026-11-02, rr6_depth_covers_colour): a colour target larger than the depth surface under it.
    if (t.color_count && depth->scale == t.colors[0]->scale &&
        (depth->width < t.colors[0]->width || depth->height < t.colors[0]->height) &&
        banjo::renut_gpu::DepthCoversColourOn())
      depth = GrowDepthToColourLocked(words[4], depth, t.colors[0]->width, t.colors[0]->height);
    t.depth = depth;
  }
  if (!t.color_count && !t.depth)
    return false;
  const HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  t.width = first->width;
  t.height = first->height;
  t.scale = first->scale;
  // A pass over targets rendered at different scales (graphics_options) would
  // address them alike; the options pick only targets drawn on their own. The
  // post-processing buffers render at 1x with the scene's depth still bound,
  // untested (on the console too its pitch is not theirs), so only colors count.
  bool mixed = false;
  for (uint32_t i = 1; i < t.color_count; ++i)
    mixed = mixed || t.colors[i]->scale != t.scale;
  if (mixed) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 8)) {
      RGPU_WARN("Draw binds color targets of different render scales ({}x{} at {:.3g}x)",
                first->width, first->height, t.scale.Factor());
    }
  }
  t.guest_width = d3d::SurfaceWidth(first->size_word);
  t.guest_height = d3d::SurfaceHeight(first->size_word);
  if (t.depth && banjo::renut_gpu::DepthRestoreOn()) {  // sasrt-nx (2026-10-20): the EDRAM depth restore
    if (HostSurface *color = DepthRestoreSourceLocked(t.depth))
      RestoreDepthFromColorLocked(t.depth, color);
  }
  return true;
}

// banjo-nx (2026-10-21, internal resolution): see DrawStateLocked. Only a depth target smaller than the first colour
// target in host pixels (the post-processing buffers at 1x keep the scene's larger one bound, as before).
void DropSmallerDepth(const GuestDevice &device, Targets &t, uint64_t vs_hash, uint64_t ps_hash) {
  const HostSurface *c = t.colors[0];
  HostSurface *d = t.depth;
  if (d->scale.Size(d->width) >= c->scale.Size(c->width) && d->scale.Size(d->height) >= c->scale.Size(c->height))
    return;
  // The depth target is used by a stencil test, or a Z test that can fail or writes (a test with ZFUNC ALWAYS and no
  // writes is none: render/pipeline.cpp leaves such a test off too).
  const u32 dc = u32(device.dev().m_ControlPacket.DepthControl);
  if ((dc & 1) || ((dc & 2) && (((dc >> 4) & 7) != 7 || (dc & 4)))) {
    // After the composite, into its UI target: the depth target at the display's resolution too (the menus' 3D icons).
    if (UiRedirectedLocked() && c->scale == UiScale()) {
      if (d->handed)
        DepthHandOverBeforeBindLocked(d, nullptr, 0);  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover)
      if (HostSurface *ui = UiDepthAfterCompositeLocked(d)) {
        t.depth = ui;
        g_bound.framebuffer = nullptr;
        g_bound.pipeline = nullptr;
        g_bound.layout = false;
        g_bound.viewport_set = false;
        return;
      }
    }
    banjo::renut_gpu::CountUiDepth(false);
    static uint32_t logged = 0;
    if (LogLimited(logged, 16))
      RGPU_WARN("A draw tests depth or stencil against a depth target smaller than its colour target ({}x{} at "
                "{:.3g}x, depth {}x{} at {:.3g}x; VS {:016X} PS {:016X}, RB_DEPTHCONTROL {:08X}): kept, the draw is "
                "cut to the depth's size", c->width, c->height, c->scale.Factor(), d->width, d->height,
                d->scale.Factor(), vs_hash, ps_hash, dc);
    return;
  }
  t.depth = nullptr;
  t.depth_dropped = true;
}

// gpu_profile: draws into 't', by their host size and attachments.
uint32_t TargetsProfileLabel(const Targets &t) {
  HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  const uint32_t variant = t.color_count ? (t.depth ? 1 : 0) : 2;
  uint32_t &label = first->profile_labels[variant];
  if (label == UINT32_MAX) {
    static const char *const kKinds[3] = {"color", "color+depth", "depth"};
    label = gpu_profile::Label(fmt::format("draws {}x{} {}", first->scale.Size(first->width),
                                           first->scale.Size(first->height), kKinds[variant]));
  }
  return label;
}

// sasrt-nx (2026-11-08, sasrt_gpu_diag_profile_vs; the fourth GPU round): with banjo_renut_diag_gpu_shaders, the draws
// without a pixel shader (the shadow cascades, the light volumes' stencil marking) split by their vertex shader too.
REXCVAR_DEFINE_BOOL(sasrt_gpu_diag_profile_vs, false, "SASRT/Diagnostics",
                    "Diagnostic (2026-11-08): with banjo_renut_diag_gpu_shaders, the GPU profile splits the draws without "
                    "a pixel shader by their vertex shader (the shadow cascades' geometry cost)");
bool SasrtProfileVsOn() {
  static const bool on = REXCVAR_GET(sasrt_gpu_diag_profile_vs);
  return on;
}
// renut-ultimate (2026-11-13, sasrt-nx's resolve project: finer timing): with banjo_renut_diag_gpu_shaders and
// sasrt_gpu_diag_profile_vs, each resolve's span splits into its parts: up to its draw (the transitions' barriers, which
// wait for the work before them), the copy / depth draw itself, what follows it, and (a depth resolve) after its
// destination's move to shader read; and each depth clear has its own label. Off: nothing changes.
REXCVAR_DEFINE_BOOL(sasrt_gpu_diag_resolve_split, false, "SASRT/Diagnostics",
                    "Diagnostic (sasrt-nx, 2026-11-13): with banjo_renut_diag_gpu_shaders and sasrt_gpu_diag_profile_vs, "
                    "a resolve's GPU time splits into its barriers, its draw and what follows it (with its resolves a "
                    "frame), and every depth clear is under its own \"clears | depth WxH\" label")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
bool SasrtResolveSplitOn() {
  static const bool on = REXCVAR_GET(sasrt_gpu_diag_resolve_split);
  return on && banjo::renut_gpu::DiagGpuShadersOn() && SasrtProfileVsOn() && gpu_profile::Timed();
}
// The label for one part of a resolve: the source's kind, size and format, and which part.
uint32_t SasrtResolvePartLabel(const HostSurface *surface, int part) {
  static std::unordered_map<uint64_t, uint32_t> labels;
  const uint64_t k = uint64_t(surface->width) | uint64_t(surface->height) << 14 |
                     uint64_t(int(surface->format)) << 28 | uint64_t(surface->depth) << 40 | uint64_t(part) << 41;
  uint32_t &label = labels[k];
  static const char *const kParts[] = {"barriers", "draw", "after", "after the move to shader read (the clear, ...)"};
  if (!label)
    label = gpu_profile::Label(fmt::format("resolve parts | {} {}x{} format {}: {}", surface->depth ? "depth" : "colour",
                                           surface->width, surface->height, int(surface->format), kParts[part])) + 1;
  return label - 1;
}
// sasrt-nx (2026-11-08, sasrt_gpu_diag_profile_vs): a memexport draw's label also names its size (the draw's vertex
// count rounded down to a power of two, its primitive type and instances): set by DrawStateLocked around the label.
u32 g_sasrt_label_export_count = 0, g_sasrt_label_export_prim = 0, g_sasrt_label_export_inst = 0;

// banjo-nx (banjo_renut_diag_gpu_shaders, package 28b): the draw's pass split by its pixel shader and blending.
uint32_t ShaderProfileLabel(const Targets &t, const ShaderRecord *ps, const PipelineKey &key,
                            const ShaderRecord *vs = nullptr) {
  const uint32_t target = TargetsProfileLabel(t);
  // RB_BLENDCONTROL ONE / ZERO / ADD for colour and alpha (BlendDesc's no-blend case).
  const bool blended = key.color_count && (key.blend_control[0] & 0x1FFF1FFF) != 0x00010001;
  const ShaderRecord *by_vs = !ps && vs && SasrtProfileVsOn() ? vs : nullptr;  // sasrt-nx (sasrt_gpu_diag_profile_vs)
  const u32 size_bits = by_vs && g_sasrt_label_export_count
                            ? (31u - u32(std::countl_zero(g_sasrt_label_export_count))) |
                                  (g_sasrt_label_export_prim & 15u) << 5 | std::min(g_sasrt_label_export_inst, 15u) << 9
                            : 0u;
  const uint64_t k = (uint64_t(target) << 44) ^ (ps ? ps->hash : 0x5A5A5A5Au) ^ (blended ? (1ull << 63) : 0) ^
                     (by_vs ? (by_vs->hash << 17 | by_vs->hash >> 47) * 0x9E3779B97F4A7C15ull : 0) ^
                     (uint64_t(size_bits + 1) * 0xD6E8FEB86659FD93ull * (size_bits != 0));
  // banjo-nx (2026-10-23, batch 6): a direct-mapped memo of the map's answers (a map search per draw of the timed frames).
  struct LabelMemo {
    uint64_t k = 0;
    uint32_t label = 0;
    bool valid = false;
  };
  static LabelMemo memo[256];
  LabelMemo &m = memo[(k * 0x9E3779B97F4A7C15ull) >> 56];
  if (m.valid && m.k == k)
    return m.label;
  static std::unordered_map<uint64_t, uint32_t> labels;
  if (auto it = labels.find(k); it != labels.end()) {
    m = {k, it->second, true};
    return it->second;
  }
  const std::string name = ps ? fmt::format("{} | PS {:016X}{}", gpu_profile::Name(target), ps->hash,
                                            blended ? " blended" : "")
                              : by_vs && size_bits
                                  ? fmt::format("{} | no pixel shader | VS {:016X} | export draw of {}-{} vertices, primitive "
                                                "{}, {} instances",
                                                gpu_profile::Name(target), by_vs->hash, 1u << (size_bits & 31u),
                                                (2u << (size_bits & 31u)) - 1u, (size_bits >> 5) & 15u, size_bits >> 9)
                              : by_vs ? fmt::format("{} | no pixel shader | VS {:016X}", gpu_profile::Name(target),
                                                    by_vs->hash)
                                      : fmt::format("{} | no pixel shader", gpu_profile::Name(target));
  const uint32_t label = gpu_profile::Label(name);
  labels.emplace(k, label);
  m = {k, label, true};
  return label;
}

// banjo-nx (2026-10-28, GPU block): 'depth_writes': the draw it binds for writes depth or stencil (a clear's binds do).
bool BindTargets(const Targets &t, bool depth_writes = true) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kBindTargets);  // blur-nx (2026-11-17)
  // kameo-nx (2026-11-03, banjo_deko3d_color_handover): a colour target holding a resolve left in its image; a whole clear
  // has given the image to the destination by now (ClearTargets), and a draw has drawn it (HandOverBeforeDrawLocked).
  for (uint32_t i = 0; i < t.color_count; ++i) {
    if (t.colors[i]->handed) [[unlikely]]
      DepthHandOverBeforeBindLocked(t.colors[i], nullptr, 0);
  }
  if (t.depth && t.depth->handed) {
    // banjo-nx (2026-10-28, banjo_deko3d_depth_handover_readonly): a draw that writes neither leaves the image as the
    // resolve read it ('writes', which the hand-over checks, moves only for those): the resolve stays left out.
    if (!depth_writes && banjo::renut_gpu::DepthHandOverReadOnlyOn()) {
      banjo::renut_gpu::CountHandOver(6);
    } else {
      g_materialise_cause = depth_writes ? 7 : 9;  // banjo-nx (2026-10-28, diagnostic)
      DepthHandOverBeforeBindLocked(t.depth, nullptr, 0);  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover)
      g_materialise_cause = 9;
    }
  }
  // banjo-nx: with banjo_renut_diag_gpu_shaders the draw marks its own label (ShaderProfileLabel), the pass's name
  // in it, which saves a timestamp a draw.
  if (!banjo::renut_gpu::DiagGpuShadersOn() && gpu_profile::Timed())
    gpu_profile::Mark(State(), TargetsProfileLabel(t));
  for (uint32_t i = 0; i < t.color_count; ++i)
    TransitionSurfaceLocked(t.colors[i], plume::RenderTextureLayout::COLOR_WRITE);
  // kameo-nx (2026-11-03, banjo_deko3d_depth_read_layout): a draw that writes neither depth nor stencil leaves a depth target
  // that was just sampled as it is: its image does not change, so nothing waits and no read after it needs its caches
  // dropped. A draw that writes comes back here (FastDrawLocked: the target's layout is one of the bind's reasons).
  if (t.depth && (depth_writes || t.depth->layout != plume::RenderTextureLayout::SHADER_READ ||
                  !banjo::renut_gpu::DepthReadLayoutOn()))
    TransitionSurfaceLocked(t.depth, plume::RenderTextureLayout::DEPTH_WRITE);
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): the last bind's framebuffer again for the same surfaces while no target
  // was built, retired or rebound (their generations with it) and no framebuffer retired: GetFramebufferLocked would
  // find that one.
  struct FramebufferReuse {
    HostSurface *colors[d3d::kMaxRenderTargets] = {};
    uint32_t count = 0;
    HostSurface *depth = nullptr;
    uint64_t surface_epoch = 0, framebuffer_epoch = 0;
    plume::RenderFramebuffer *framebuffer = nullptr;
  };
  static FramebufferReuse last;
  const bool fb_reuse = ChainReuseOn();  // banjo-nx (2026-10-26): with its A/B
  static const bool fb_verify = REXCVAR_GET(banjo_renut_chain_reuse) && REXCVAR_GET(banjo_renut_chain_reuse_verify);
  const uint64_t surface_epoch = SurfaceEpochLocked(), framebuffer_epoch = FramebufferEpochLocked();
  bool same = fb_reuse && last.framebuffer && last.count == t.color_count && last.depth == t.depth &&
              last.surface_epoch == surface_epoch && last.framebuffer_epoch == framebuffer_epoch;
  for (uint32_t i = 0; same && i < t.color_count; ++i)
    same = last.colors[i] == t.colors[i];
  plume::RenderFramebuffer *framebuffer = nullptr;
  if (same) {
    framebuffer = last.framebuffer;
    if (fb_verify) {
      plume::RenderFramebuffer *made = GetFramebufferLocked(t.colors, t.color_count, t.depth);
      if (made != framebuffer) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("banjo_renut_chain_reuse_verify: the reused framebuffer differs from the one looked up");
        framebuffer = made;
      }
    }
  } else {
    framebuffer = GetFramebufferLocked(t.colors, t.color_count, t.depth);
    if (fb_reuse) {
      for (uint32_t i = 0; i < d3d::kMaxRenderTargets; ++i)
        last.colors[i] = i < t.color_count ? t.colors[i] : nullptr;
      last.count = t.color_count;
      last.depth = t.depth;
      last.surface_epoch = SurfaceEpochLocked();
      last.framebuffer_epoch = FramebufferEpochLocked();
      last.framebuffer = framebuffer;
    }
  }
  if (!framebuffer)
    return false;
  // halo3-nx (2026-11-03): which of the formats over an EDRAM view was bound last (SyncColorViewLocked).
  if (g_bound.framebuffer != framebuffer) {
    static uint64_t serial = 0;
    ++serial;
    for (uint32_t i = 0; i < t.color_count; ++i)
      t.colors[i]->bound_serial = serial;
  }
  if (banjo::renut_gpu::ResolveTraceOn() && g_bound.framebuffer != framebuffer) {
    RGPU_INFO("[trace] targets: {} draws before; now {} colour {:08X} {:08X} ({}x{} fmt {} samples {}), depth {:08X} "
              "({}x{} samples {})",
              g_trace_draws, t.color_count, t.color_count ? t.colors[0]->guest_va : 0,
              t.color_count > 1 ? t.colors[1]->guest_va : 0, t.color_count ? t.colors[0]->width : 0,
              t.color_count ? t.colors[0]->height : 0, t.color_count ? u32(t.colors[0]->format) : 0,
              t.color_count ? u32(t.colors[0]->samples) : 0, t.depth ? t.depth->guest_va : 0,
              t.depth ? t.depth->width : 0, t.depth ? t.depth->height : 0, t.depth ? u32(t.depth->samples) : 0);
    g_trace_draws = 0;
  }
  if (g_bound.framebuffer != framebuffer) {
    State().command_list->setFramebuffer(framebuffer);
    g_bound.framebuffer = framebuffer;
  }
  return true;
}

// blur-nx (2026-11-11, port 76; its key 220: blur_renut_edram_inherit): before a draw that keeps part of a colour target (a
// write mask without every channel, or blending, which reads it), the target takes what other views of its EDRAM tiles
// wrote since its own last write (EdramInheritSourcesLocked, oldest first), through the console's tile and sample layout
// (edram_inherit.hlsli). On the console the views are one memory; here each is its own host target, which held only what
// it drew. Blur's environment panorama: drawn into a 512x256 4x target cleared to alpha 1, its RGB then drawn through a
// 320x256 view of the same EDRAM with alpha left out, and that view resolved into the 256x128 texture the ground, car
// glass and car paint read: its alpha is the clear's (and, over the tiles the panorama drew, the panorama's). A draw that
// writes every channel without blending takes nothing (it covers what it draws; the rest stays as before this key).
// Called by both draw paths before BindTargets; the draw's own binds follow (g_bound is reset).
void InheritEdramBeforeDrawLocked(const Targets &t, u32 color_mask, const u32 *blend_control) {
  if (!t.color_count || !banjo::renut_gpu::EdramInheritOn())
    return;
  auto &s = State();
  static std::vector<HostSurface *> sources;
  for (u32 i = 0; i < t.color_count; ++i) {
    HostSurface *target = t.colors[i];
    // Nothing written to any colour target since this one's last write or look: nothing to take (the common case, O(1)).
    if (!target || target->depth || g_blur_color_write_last <= target->write_seq ||
        g_blur_color_write_last <= target->blur_inherit_checked)
      continue;
    const u32 mask = (color_mask >> (4 * i)) & 0xF;
    const bool blended = blend_control && (blend_control[i] & 0x1FFF1FFF) != 0x00010001;
    // The channels the EDRAM format has: k_32_FLOAT one; k_16_16, k_16_16_FLOAT and k_32_32_FLOAT two; the rest four.
    const u32 edram_format = (u32(target->color_info_word) >> 16) & 0xF;
    const u32 every = edram_format == 14 ? 0x1u : (edram_format == 4 || edram_format == 6 || edram_format == 15) ? 0x3u : 0xFu;
    if (!mask || ((mask & every) == every && !blended))
      continue;  // writes nothing, or every channel without reading the target: looked at again by a draw that keeps
    target->blur_inherit_checked = g_blur_color_write_last;
    u32 skipped = 0;
    EdramInheritSourcesLocked(target, sources, skipped);
    if (skipped) {
      static u32 logged = 0;
      if (LogLimited(logged, 8))
        RGPU_WARN("[renut] {}x{} target (EDRAM format {}) kept in part: {} other view(s) wrote its EDRAM since, of another "
                  "format or scale: not taken (blur_renut_edram_inherit)", target->width, target->height,
                  (u32(target->color_info_word) >> 16) & 0xF, skipped);
    }
    if (sources.empty())
      continue;
    u32 dst_bits = 0, dst_rows = 0;
    if (!EdramInheritSpanLocked(target, dst_bits, dst_rows))
      continue;
    const bool dst_ms = target->samples != plume::RenderSampleCount::COUNT_1;
    // renut-ultimate: a resolve handed over in this target's image is drawn out first; the copy writes the image.
    if (target->handed)
      DepthHandOverBeforeBindLocked(target, nullptr, 0);
    HostSurface *const colors[] = {target};
    plume::RenderFramebuffer *framebuffer = GetFramebufferLocked(colors, 1, nullptr);
    if (!framebuffer)
      continue;
    u32 taken = 0;
    for (HostSurface *source : sources) {
      u32 src_bits = 0, src_rows = 0;
      if (!EdramInheritSpanLocked(source, src_bits, src_rows))
        continue;
      const bool src_ms = source->samples != plume::RenderSampleCount::COUNT_1;
      plume::RenderShader *ps = src_ms ? (dst_ms ? s.edram_inherit_ms_ms_ps.get() : s.edram_inherit_src_ms_ps.get())
                                       : (dst_ms ? s.edram_inherit_dst_ms_ps.get() : s.edram_inherit_ps.get());
      const uint32_t source_slot = ps ? SurfaceSampleSlotLocked(source) : kInvalidDescriptor;
      if (source_slot == kInvalidDescriptor)
        continue;
      auto &pipeline = s.edram_inherit_pipelines[uint32_t(target->format) | (uint32_t(target->samples) << 20) |
                                                 (src_ms ? 0x80000000u : 0u)];
      if (!pipeline) {
        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = s.utility_layout.get();
        desc.vertexShader = s.present_vs.get();
        desc.pixelShader = ps;
        desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(target->samples));
        desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
        desc.cullMode = plume::RenderCullMode::NONE;
        desc.renderTargetCount = 1;
        desc.renderTargetFormat[0] = target->format;
        desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
        desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
        pipeline = s.device->createGraphicsPipeline(desc);
        if (!pipeline)
          continue;
        pipeline->setName("renut edram inherit");
      }
      plume::RenderCommandList *cmd = s.command_list;
      TransitionSurfaceLocked(source, plume::RenderTextureLayout::SHADER_READ);
      TransitionSurfaceLocked(target, plume::RenderTextureLayout::COLOR_WRITE);
      cmd->setFramebuffer(framebuffer);
      g_bound.framebuffer = framebuffer;
      cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(target->width), float(target->height)));
      cmd->setScissors(plume::RenderRect(0, 0, int32_t(target->width), int32_t(target->height)));
      cmd->setGraphicsPipelineLayout(s.utility_layout.get());
      cmd->setPipeline(pipeline.get());
      cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
      cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
      UtilityPush push{};
      std::memcpy(&push.scale[0], &dst_bits, 4);
      std::memcpy(&push.scale[1], &src_bits, 4);
      const u32 src_w = source->width, src_h = source->height;
      std::memcpy(&push.translate[0], &src_w, 4);
      std::memcpy(&push.translate[1], &src_h, 4);
      std::memcpy(&push.param0, &src_rows, 4);
      // blur-nx (2026-11-13, MSAA lowered): the host targets' sample counts (edram_inherit.hlsli): a 4x view made 2x by
      // msaa_level read sample indices 2 and 3 of a 2-sample image (nothing there).
      const u32 host_samples = (u32(target->samples) & 0xFu) | ((u32(source->samples) & 0xFu) << 4);
      std::memcpy(&push.param1, &host_samples, 4);
      push.texture_index = source_slot;
      push.sampler_index = kPointClampSamplerSlot;
#if defined(PLUME_DEKO3D)
      // renut-ultimate (port 76, not in blur-nx): the multisampled source forms' deko3d shaders read the source as a
      // BOUND texture (binding 0: tools/deko3d_glsl.py makes a host shader's bindless Texture2DMS dk_ms_b0), as the depth
      // resolve's multisampled variant does (katamari-nx: Ryujinx cannot follow a bindless multisampled fetch).
      // Without it they read whatever binding 0 last held.
      if (src_ms && IsDeko3D())
        plume::Deko3DBindFragmentTextures(cmd, 0, &source_slot, 1);
#endif
      cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
      cmd->drawInstanced(3, 1, 0, 0);
      ++taken;
      static u32 logged = 0;
      if (LogLimited(logged, 16))
        RGPU_INFO("[renut] {}x{} {}x target (base {:03X}, EDRAM format {}) kept in part takes what a {}x{} {}x view wrote "
                  "over its tiles since (base {:03X}; blur_renut_edram_inherit)", target->width, target->height,
                  u32(target->samples), u32(target->color_info_word) & 0xFFF, (u32(target->color_info_word) >> 16) & 0xF,
                  source->width, source->height, u32(source->samples), u32(source->color_info_word) & 0xFFF);
    }
    if (taken) {
      static uint64_t total = 0;  // how often: a copy of the target each time (logged as the count passes a power of two)
      const uint64_t before = total;
      total += taken;
      if (std::bit_width(before) != std::bit_width(total))
        RGPU_INFO("[renut] blur_renut_edram_inherit: {} copies so far", total);
      ++target->writes;  // banjo-nx (banjo_renut_resolve_skip): it holds other pixels now
      target->write_seq = ++g_surface_write_seq;
      g_blur_color_write_last = target->write_seq;
      target->blur_inherit_checked = g_blur_color_write_last;
      // The draw path binds its own layout, pipeline, targets and viewport again.
      g_bound.framebuffer = nullptr;
      g_bound.pipeline = nullptr;
      g_bound.layout = false;
      g_bound.viewport_set = false;
    }
  }
}

// Port 179 (from fnxf_xiii, its render/draw.cpp EdramCarryLocked of 2026-10-31 and 2026-11-01; banjo_renut_edram_carry,
// off by default). On the console a colour target is a view of EDRAM, which keeps what was last written there: a game that
// draws into a target without clearing it leaves the pixels its geometry misses as the EDRAM held them, often another
// target's picture from the frame before. Here each view is a host image of its own that holds only what it drew (black
// at first: sasrt_renut_deko3d_clear_new). FINAL FANTASY XIII's Vestige, behind the crystal: no geometry there and the
// scene's colour never cleared, so the console showed the last frame's picture (a soft light) and fnxf_xiii bright noise
// or black. So: a colour target first drawn in a frame without a clear of it in that frame, where a different target of
// the same EDRAM base and host format, at least as large (a smaller one does not cover this one's EDRAM), was written
// after it in an earlier frame, starts with that target's picture scaled to its size (bilinear: smaa_vs and
// ui_upscale_ps, the UI upscale's pass). Once a target and frame at most, across frames only (within a frame the game's
// passes resolve what they need), single-target draws only. The base's, not fnxf_xiii's: the source single-sampled (it
// is sampled as a 2D texture), the targets an alias clear reaches counted as cleared, a colour-only framebuffer of its
// own rather than the draw's, and nothing while blur_renut_edram_inherit is on. Called by both draw paths before
// BindTargets (the fast path whatever it then binds, so every draw marks its targets); the draw's own binds follow
// (g_bound is reset).
std::unordered_map<u32, uint64_t> g_carry_last;  // EDRAM base -> the generation of the colour target written there last
uint64_t g_carry_order = 0;
// banjo_renut_diag_edram_carry_always: the two colour targets written last, any base (the last first).
uint64_t g_carry_last_any[2] = {};
void NoteCarryAny(uint64_t generation) {
  if (g_carry_last_any[0] != generation) {
    g_carry_last_any[1] = g_carry_last_any[0];
    g_carry_last_any[0] = generation;
  }
}

inline bool EdramCarryOn() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(banjo_renut_edram_carry);
    if (v)
      RGPU_INFO("[renut] banjo_renut_edram_carry on{}", REXCVAR_GET(banjo_renut_diag_edram_carry_always)
                                                             ? ", with banjo_renut_diag_edram_carry_always (a test)"
                                                             : "");
    return v;
  }();
  return on && !banjo::renut_gpu::EdramInheritOn();
}

void NoteEdramCarryClearLocked(HostSurface *c) {
  if (!c || c->depth)
    return;
  c->carry_cleared_frame = TextureFrameLocked();
  c->carry_order = ++g_carry_order;
  g_carry_last[u32(c->color_info_word) & 0xFFFu] = c->generation;
  NoteCarryAny(c->generation);
}

void CarryEdramBeforeDrawLocked(const Targets &t) {
  auto &s = State();
  const uint64_t frame = TextureFrameLocked();
  static const bool always = REXCVAR_GET(banjo_renut_diag_edram_carry_always);
  for (uint32_t i = 0; i < t.color_count; ++i) {
    HostSurface *c = t.colors[i];
    if (!c || c->depth || c->carry_drawn_frame == frame)
      continue;
    uint64_t &last = g_carry_last[u32(c->color_info_word) & 0xFFFu];
    // By generation: a target retired or rebuilt since is not found.
    const uint64_t source =
        !always ? last : g_carry_last_any[0] != c->generation ? g_carry_last_any[0] : g_carry_last_any[1];
    HostSurface *from = source && source != c->generation ? SurfaceByGenerationLocked(source) : nullptr;
    const bool carry = t.color_count == 1 && (always || c->carry_cleared_frame != frame) && from && from != c &&
                       !from->depth && from->texture && c->texture && from->format == c->format &&
                       from->samples == plume::RenderSampleCount::COUNT_1 &&
                       (always || (from->carry_drawn_frame < frame && from->carry_order > c->carry_order)) &&
                       from->width >= c->width && from->height >= c->height;
    if (always) {  // the test logs its first checks: what each target found
      static u32 checks = 0;
      if (LogLimited(checks, 16))
        RGPU_INFO("[renut] banjo_renut_diag_edram_carry_always: {}x{} target {:08X} (format {}, samples {}, base {:03X}, "
                  "{} targets) {}: {}", c->width, c->height, c->guest_va, u32(c->format), u32(c->samples),
                  u32(c->color_info_word) & 0xFFFu, t.color_count, carry ? "carried" : "not carried",
                  from ? fmt::format("from {:08X} ({}x{}, format {}, samples {}, order {} against {})", from->guest_va,
                                     from->width, from->height, u32(from->format), u32(from->samples), from->carry_order,
                                     c->carry_order)
                       : fmt::format("no source (generation {})", source));
    }
    c->carry_drawn_frame = frame;
    c->carry_order = ++g_carry_order;
    last = c->generation;
    NoteCarryAny(c->generation);
    if (!carry)
      continue;
    static std::unique_ptr<plume::RenderShader> vs, ps;
    static bool tried = false;
    if (!tried) {
      tried = true;
      vs = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_vs), "smaa_vs");
      ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(ui_upscale_ps), "ui_upscale_ps");
      if (!vs || !ps)
        RGPU_WARN("[renut] banjo_renut_edram_carry: its shaders could not be made, so nothing is carried");
    }
    if (!vs || !ps)
      return;
    static std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
    auto &pipeline = pipelines[uint64_t(c->format) | uint64_t(u32(c->samples)) << 32];
    if (!pipeline) {
      plume::RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = s.utility_layout.get();
      desc.vertexShader = vs.get();
      desc.pixelShader = ps.get();
      desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(c->samples));
      desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      desc.cullMode = plume::RenderCullMode::NONE;
      desc.renderTargetCount = 1;
      desc.renderTargetFormat[0] = c->format;
      desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
      desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
      pipeline = s.device->createGraphicsPipeline(desc);
      if (!pipeline)
        continue;
      pipeline->setName("renut edram carry");
    }
    const uint32_t source_slot = SurfaceSampleSlotLocked(from);
    if (source_slot == kInvalidDescriptor)
      continue;
    // A resolve handed over in this target's image is drawn out first: the carry writes the image.
    if (c->handed)
      DepthHandOverBeforeBindLocked(c, nullptr, 0);
    HostSurface *const colors[] = {c};
    plume::RenderFramebuffer *framebuffer = GetFramebufferLocked(colors, 1, nullptr);
    if (!framebuffer)
      continue;
    plume::RenderCommandList *cmd = s.command_list;
    TransitionSurfaceLocked(from, plume::RenderTextureLayout::SHADER_READ);
    TransitionSurfaceLocked(c, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->setFramebuffer(framebuffer);
    g_bound.framebuffer = framebuffer;
    const uint32_t w = c->scale.Size(c->width), h = c->scale.Size(c->height);  // host pixels
    cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(w), float(h)));
    cmd->setScissors(plume::RenderRect(0, 0, int32_t(w), int32_t(h)));
    cmd->setGraphicsPipelineLayout(s.utility_layout.get());
    cmd->setPipeline(pipeline.get());
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
    UtilityPush push{};
    push.texture_index = source_slot;
    push.sampler_index = kLinearClampSamplerSlot;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    cmd->drawInstanced(3, 1, 0, 0);
    ++c->writes;  // banjo-nx (banjo_renut_resolve_skip): it holds other pixels now
    c->write_seq = ++g_surface_write_seq;
    g_blur_color_write_last = c->write_seq;
    static uint64_t total = 0;
    static u32 logged = 0;
    ++total;
    if (LogLimited(logged, 8))
      RGPU_INFO("[renut] {}x{} target {:08X} (EDRAM base {:03X}, samples {}) starts from {:08X}'s picture ({}x{}), as "
                "the console's EDRAM held it (banjo_renut_edram_carry)", c->width, c->height, c->guest_va,
                u32(c->color_info_word) & 0xFFFu, u32(c->samples), from->guest_va, from->width, from->height);
    else if (std::has_single_bit(total))
      RGPU_INFO("[renut] banjo_renut_edram_carry: {} carries so far", total);
    // The draw path binds its own layout, pipeline, targets and viewport again.
    g_bound.framebuffer = nullptr;
    g_bound.pipeline = nullptr;
    g_bound.layout = false;
    g_bound.viewport_set = false;
  }
}

// A draw's host viewport and scissor. The game's D3D sets the viewport one of
// two ways (N&B database):
// - D3DDevice_SetViewport (0x821F2110) writes scale w/2, -h/2, maxZ - minZ
//   and offset x + w/2, y + h/2, minZ, all enabled in PA_CL_VTE_CNTL: the
//   host viewport is that rectangle, and the shaders pass positions through.
// - D3DRS_VIEWPORTENABLE false (SetRenderState_ViewportEnable, 0x821D53E8)
//   disables the transform (and clipping): the vertices are in pixels. The
//   host viewport then covers the target, and the vertex shader maps the
//   pixels to clip space itself (screen_space; any enabled part of the
//   transform comes along as screen_scale / screen_offset).
struct ViewportState {
  // Host pixels (Targets::scale).
  plume::RenderViewport viewport;
  plume::RenderRect scissor;
  // The viewport's size in guest pixels, which screen-space positions and
  // point sizes are in.
  float guest_size[2] = {};
  bool screen_space = false;
  float screen_scale[2] = {1.0f, 1.0f};
  float screen_offset[2] = {0.0f, 0.0f};
  // sasrt-nx (2026-10-20): SharedConstants::depth_remap (g_DepthRemap): a backwards depth range's scale and offset, 0 for none.
  float depth_remap[2] = {0.0f, 0.0f};
  // Guest pixels (Renut upstream 574b488): the pixel position (VPOS) the GPU gives the pixel shader is in the
  // coordinates of the EDRAM it renders to. While tiling that is one tile, and N&B adds the tile's corner back from a
  // per-tile constant (the deferred shadow pass, vs_f6965212d74a7552 / ps_d86a35e4f2b956b3: c0.x = 384 in the lower
  // tile). The host renders every tile into one target, so a draw predicated to one tile has its position made
  // relative to that tile.
  float pixel_origin[2] = {0.0f, 0.0f};
};

// blur-nx (2026-11-13, MSAA lowered; banjo_renut_alpha_to_mask_test): alpha to coverage on a single-sample target covers
// the pixel when alpha >= 0.5 (MakeKey leaves the pipeline's alpha to coverage off for it). Combined with the game's own
// alpha test: off (7) -> alpha >= 0.5 as before; greater (4) or greater-or-equal (6) below 0.5 -> alpha >= 0.5 (both must
// pass); a test at 0.5 or above, or of another kind, stays the game's. Blur's foliage has alpha to coverage AND an
// alpha test near 0: with msaa_level 1 its cards were drawn whole (the coverage dropped, only the near-zero test left).
void AlphaToMaskTestSingleSample(u32 &func, float &threshold) {
  if (func == 7 || ((func == 4 || func == 6) && !(threshold >= 0.5f))) {
    func = 6;  // >=
    threshold = 0.5f;
  }
}

// The screen rect of the one tile a draw is predicated to while tiling
// (SetTilesLocked); false for a draw that runs in more tiles, or none.
bool PredicatedTile(const GuestDevice &device, ClearRect &rect) {
  if (g_tiles.size() < 2)
    return false;
  u32 mask = device.U32(d3d::kDeviceOffsetPredication);
  mask = (mask ? mask : ~0u) & g_replay_predication;
  int32_t tile = -1;
  for (size_t i = 0; i < g_tiles.size() && i < 16; ++i) {
    if (!(mask & (3u << (2 * i))))
      continue;
    if (tile >= 0)
      return false;
    tile = int32_t(i);
  }
  if (tile < 0)
    return false;
  rect = g_tiles[tile];
  return true;
}

ViewportState ComputeViewportSlow(const GuestDevice &device, const Targets &t);

// banjo-nx (2026-10-19, render-thread proposal 8): the last draw's viewport again when everything ComputeViewportSlow
// reads is the same: the device words (VteControl, the six viewport values, ModeControl, WindowOffset, the window
// scissor, 0x29C0), the predicated tile (worked out first: it reads the replay's predication and the tiles), the
// targets' sizes and scale, and the half-pixel switch.
ViewportState ComputeViewport(const GuestDevice &device, const Targets &t) {
  const d3d::Device &dev = device.dev();
  const d3d::ValuesPacket &v = dev.m_ValuesPacket;
  ClearRect tile{};
  const bool one_tile = PredicatedTile(device, tile);
  u32 key[24];
  key[0] = dev.m_ControlPacket.VteControl;
  std::memcpy(&key[1], &v.VportXScale, 4);
  std::memcpy(&key[2], &v.VportXOffset, 4);
  std::memcpy(&key[3], &v.VportYScale, 4);
  std::memcpy(&key[4], &v.VportYOffset, 4);
  std::memcpy(&key[5], &v.VportZScale, 4);
  std::memcpy(&key[6], &v.VportZOffset, 4);
  key[7] = u32(dev.m_ControlPacket.ModeControl);
  key[8] = dev.m_WindowPacket.WindowOffset;
  key[9] = dev.m_WindowPacket.WindowScissorTL;
  key[10] = dev.m_WindowPacket.WindowScissorBR;
  key[11] = device.U32(0x29C0);
  key[12] = u32(one_tile) | (banjo::renut_gpu::HalfPixelOffsetOn() ? 2u : 0u);
  key[13] = one_tile ? u32(tile.x1) : 0;
  key[14] = one_tile ? u32(tile.y1) : 0;
  key[15] = one_tile ? u32(tile.x2) : 0;
  key[16] = one_tile ? u32(tile.y2) : 0;
  key[17] = t.width;
  key[18] = t.height;
  key[19] = t.guest_width;
  key[20] = t.guest_height;
  key[21] = t.scale.code;
  key[22] = 0;
  key[23] = 0;
  static u32 last_key[24];
  static ViewportState last;
  static bool valid = false;
  if (valid && std::memcmp(key, last_key, sizeof(key)) == 0)
    return last;
  // blur-nx (2026-11-18, the Renut host batch's #32, blur_renut_viewport_memo / A/B 239): the last eight answers by
  // their inputs (the frame goes back and forth between a few viewports: the shadow maps, the scene, the passes after
  // it). ComputeViewportSlow reads only what the key holds, as the single answer above assumed.
  if (::renut::gpu::host_batch::On(::renut::gpu::host_batch::kViewportMemo)) {
    static u32 keys[8][24];
    static ViewportState states[8];
    static u32 count = 0, next = 0;
    for (u32 e = 0; e < count; ++e) {
      if (std::memcmp(key, keys[e], sizeof(key)) == 0) {
        if (::renut::gpu::host_batch::VerifyOn()) {  // blur-nx (2026-11-18): the kept answer against one made now
          const ViewportState now = ComputeViewportSlow(device, t);
          if (std::memcmp(&now, &states[e], sizeof(now)) != 0) {
            ::renut::gpu::host_batch::VerifyDiffer("#32: a kept viewport differs from the one computed");
            states[e] = now;
          }
        }
        last = states[e];
        std::memcpy(last_key, key, sizeof(key));
        valid = true;
        return last;
      }
    }
    last = ComputeViewportSlow(device, t);
    std::memcpy(keys[next], key, sizeof(key));
    states[next] = last;
    next = (next + 1) & 7;
    count = std::min<u32>(count + 1, 8);
    std::memcpy(last_key, key, sizeof(key));
    valid = true;
    return last;
  }
  last = ComputeViewportSlow(device, t);
  std::memcpy(last_key, key, sizeof(key));
  valid = true;
  return last;
}

ViewportState ComputeViewportSlow(const GuestDevice &device, const Targets &t) {
  const d3d::Device &dev = device.dev();
  const u32 vte = dev.m_ControlPacket.VteControl;
  const d3d::ValuesPacket &v = dev.m_ValuesPacket;
  const float xs = (vte & 0x01) ? float(v.VportXScale) : 1.0f;
  float xo = (vte & 0x02) ? float(v.VportXOffset) : 0.0f;
  const float ys = (vte & 0x04) ? float(v.VportYScale) : 1.0f;
  float yo = (vte & 0x08) ? float(v.VportYOffset) : 0.0f;
  const float zs = (vte & 0x10) ? float(v.VportZScale) : 1.0f;
  const float zo = (vte & 0x20) ? float(v.VportZOffset) : 0.0f;

  // PA_SU_SC_MODE_CNTL.vtx_window_offset_enable adds PA_SC_WINDOW_OFFSET.
  int32_t wx = 0, wy = 0;
  if (u32(dev.m_ControlPacket.ModeControl) & 0x10000) {
    const u32 offset = dev.m_WindowPacket.WindowOffset;
    wx = int32_t(offset << 17) >> 17;
    wy = int32_t(offset << 1) >> 17;
    xo += float(wx);
    yo += float(wy);
  }
  // A draw predicated to one tile renders into that tile's EDRAM only, which
  // the GPU resolves to the tile's rect of the screen: it is cut to the rect.
  // Most such draws are in the tile's own coordinates, a viewport the size of
  // the tile-sized surface (N&B's per-tile scene command buffers: the vehicle
  // parts), which the host offsets to the rect. A viewport larger than that
  // surface addresses the whole screen (N&B's water, sub_82386BF8, drawn once
  // per tile with a full-screen viewport and that tile's constants): it stays.
  ClearRect tile_rect{};
  const bool one_tile = PredicatedTile(device, tile_rect);
  const bool screen_wide = one_tile && (vte & 0x0F) == 0x0F && t.guest_width && t.guest_height &&
                           (2.0f * xs > float(t.guest_width) + 0.5f ||
                            -2.0f * ys > float(t.guest_height) + 0.5f);
  const bool tile_local = one_tile && !screen_wide;
  int32_t tx = 0, ty = 0;
  if (tile_local) {
    tx = tile_rect.x1;
    ty = tile_rect.y1;
  }
  xo += float(tx);
  yo += float(ty);

  ViewportState vs;
  if (one_tile) {  // Renut upstream 574b488 (the Jinjo Bingo silhouette)
    vs.pixel_origin[0] = float(tile_rect.x1);
    vs.pixel_origin[1] = float(tile_rect.y1);
  }
  const float min_z = std::clamp(zo, 0.0f, 1.0f), max_z = std::clamp(zo + zs, 0.0f, 1.0f);
  // banjo-nx (2026-10-18, banjo_renut_half_pixel_offset): PA_SU_VTX_CNTL.pix_center (device 0x29C0 bit 0, which
  // D3DRS_HALFPIXELOFFSET sets) 0 puts the pixel centres at integer window coordinates (D3D9's), so the GPU's window
  // positions are the host's plus half a pixel, as Xenia offsets the viewport (draw_util). Without it everything drew
  // half a pixel up-left; the depth of field's 320x180 passes, whose blur taps are one-sided, shifted their result a
  // whole texel: the orange halo above Kazooie's crest. Not in the scissor (whole pixels).
  // banjo-nx (2026-10-21, package 36z2): half a host pixel, in guest pixels (0.5 / scale): half a guest pixel at a scale
  // above 1x put the game's full-screen quads (edges on the guest's pixel centres) past the centres of the host's top
  // row and left column, which were never drawn (the docked white line at 1.25x and 1.5x); at 1x the same as before.
  const float pixel_center =
      (banjo::renut_gpu::HalfPixelOffsetOn() && !(device.U32(0x29C0) & 1)) ? 0.5f / t.scale.Factor() : 0.0f;
  // SetViewport's rectangle back from its scale and offset: x = offset - scale
  // and width = 2 * scale; y = offset + scale and height = -2 * scale (the
  // scale is negative: y goes down).
  vs.screen_space = (vte & 0x0F) != 0x0F || !(xs > 0.0f) || !(ys < 0.0f);
  if (!vs.screen_space) {
    vs.viewport = plume::RenderViewport(xo + pixel_center - xs, yo + pixel_center + ys, 2.0f * xs, -2.0f * ys,
                                        min_z, max_z);
    // sasrt-nx (2026-10-20): a backwards depth range (MinZ 1 to MaxZ 0) on a back end that cannot take one (D3D12): 0-1 here,
    // and the vertex shader writes the guest's depth (g_DepthRemap).
    if (min_z > max_z && !IsDeko3D() && banjo::renut_gpu::ReverseDepthRemapOn()) {
      vs.viewport.minDepth = 0.0f;
      vs.viewport.maxDepth = 1.0f;
      vs.depth_remap[0] = zs;
      vs.depth_remap[1] = zo;
    }
  } else {
    vs.viewport = plume::RenderViewport(0.0f, 0.0f, float(t.width), float(t.height), min_z, max_z);
    vs.screen_scale[0] = xs;
    vs.screen_scale[1] = ys;
    vs.screen_offset[0] = xo + pixel_center;
    vs.screen_offset[1] = yo + pixel_center;
  }

  // PA_SC_WINDOW_SCISSOR_TL/BR: x 0-14, y 16-30.
  const u32 tl = dev.m_WindowPacket.WindowScissorTL;
  const u32 br = dev.m_WindowPacket.WindowScissorBR;
  int32_t sx1 = int32_t(tl & 0x7FFF) + wx + tx, sy1 = int32_t((tl >> 16) & 0x7FFF) + wy + ty;
  int32_t sx2 = int32_t(br & 0x7FFF) + wx + tx, sy2 = int32_t((br >> 16) & 0x7FFF) + wy + ty;
  // A target grown for tiling: D3D bounds the window scissor by the tile-sized
  // surface, and the GPU applies it per tile in window coordinates, so across
  // the tiles that bound covers the whole target (a one-tile draw is cut to
  // its tile below; one in tile coordinates has the bound offset there).
  if (!tile_local && t.guest_width && t.width > t.guest_width && sx2 >= int32_t(t.guest_width))
    sx2 = int32_t(t.width);
  if (!tile_local && t.guest_height && t.height > t.guest_height &&
      sy2 >= int32_t(t.guest_height))
    sy2 = int32_t(t.height);
  // A screen-space draw's clip space spans the whole target: what a partly
  // enabled transform would clip is cut by the scissor instead.
  if (vs.screen_space && (vte & 0x0F) == 0x0F) {
    sx1 = std::max(sx1, int32_t(std::floor(xo - std::fabs(xs))));
    sx2 = std::min(sx2, int32_t(std::ceil(xo + std::fabs(xs))));
    sy1 = std::max(sy1, int32_t(std::floor(yo - std::fabs(ys))));
    sy2 = std::min(sy2, int32_t(std::ceil(yo + std::fabs(ys))));
  }
  if (one_tile) {
    sx1 = std::max(sx1, tile_rect.x1);
    sy1 = std::max(sy1, tile_rect.y1);
    sx2 = std::min(sx2, tile_rect.x2);
    sy2 = std::min(sy2, tile_rect.y2);
  }
  sx1 = std::clamp(sx1, 0, int32_t(t.width));
  sy1 = std::clamp(sy1, 0, int32_t(t.height));
  sx2 = std::clamp(sx2, sx1, int32_t(t.width));
  sy2 = std::clamp(sy2, sy1, int32_t(t.height));

  // All of the above is in guest pixels; the host targets are t.scale times
  // that. Depth is not scaled.
  vs.guest_size[0] = vs.viewport.width;
  vs.guest_size[1] = vs.viewport.height;
  const float scale = t.scale.Factor();
  vs.viewport.x *= scale;
  vs.viewport.y *= scale;
  vs.viewport.width *= scale;
  vs.viewport.height *= scale;
  vs.scissor = ScaleRect(plume::RenderRect(sx1, sy1, sx2, sy2), t.scale);
  return vs;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Where a draw's float constant registers are (ReplayConstants).
struct FloatConstants {
  const u8 *state = nullptr;
  const u8 *running = nullptr;  // null: all in 'state'
  uint64_t set = 0;
  u32 offset = 0;  // of VertexShaderF or PixelShaderF in the device layout
  uint64_t *dirty = nullptr;    // ReplayConstants::vs_dirty / ps_dirty
  uint64_t replay = 0;
  const u8 *const *groups = nullptr;  // banjo-nx (2026-10-28): the stage's 64 (ReplayConstants::groups), or null

  const u8 *Register(u32 r) const {
    const bool own = !running || ((set >> (63 - r / 4)) & 1);
    if (!own)
      return running + offset + r * 16;
    // banjo-nx (2026-10-28, banjo_renut_replay_group_pointers): a set group's bytes where its op recorded them.
    if (groups) {
      if (const u8 *g = groups[r / 4]) {
        if (g_group_pointers_verify) [[unlikely]] {
          ++g_group_pointer_reads;
          g_group_pointer_differ += std::memcmp(g + (r % 4) * 16, state + offset + r * 16, 16) != 0;
        }
        return g + (r % 4) * 16;
      }
    }
    return state + offset + r * 16;
  }
};

FloatConstants DrawFloatConstants(const u8 *state, const ReplayConstants *replay, bool pixel) {
  FloatConstants c;
  c.state = state;
  c.offset = u32(offsetof(d3d::Device, m_Constants)) +
             u32(pixel ? offsetof(d3d::Constants, PixelShaderF) : offsetof(d3d::Constants, VertexShaderF));
  if (replay) {
    c.running = replay->running;
    c.set = pixel ? replay->ps_set : replay->vs_set;
    c.dirty = pixel ? replay->ps_dirty : replay->vs_dirty;
    c.replay = c.dirty ? replay->replay : 0;
    c.groups = replay->groups ? replay->groups + (pixel ? 64 : 0) : nullptr;
  }
  return c;
}

// Float constants go by groups of four registers (64 bytes): the unit m_Pending
// marks dirty, and the one a replayed draw takes from its snapshot or from the
// running device (FloatConstants::Register), so a group's bytes are contiguous.
// Bit 63 - group, as in m_Pending.
constexpr u32 kConstantGroupBytes = 4 * 16;

// banjo-nx (2026-10-28, per-draw night): a group's 64 guest bytes to 'held' as they are and to 'swapped' with each word's
// bytes swapped (the host's little-endian lanes), from one load: four 16-byte loads, four stores, four REV32s and four
// stores on the A57 (the loops before compiled to sixteen scalar load / REV / store triples and a separate copy). The
// same bytes.
// kameo-nx (2026-11-04, banjo_renut_const_nan_flush; the rule from Kameo: RePowered's constant_buffers.cpp SwapConstantChunk,
// MIT): a word with every exponent bit set goes to the shader as 0. Xenos multiplies anything by 0 to 0, NaN and infinity
// included; the host's GPU keeps the NaN. 'held' keeps the guest's bytes (the next compare is against those).
[[gnu::always_inline]] inline void CopyAndSwapGroup(const u8 *guest, u8 *held, u8 *swapped, bool nan_flush) {
  static_assert(kConstantGroupBytes == 64);
#if defined(__aarch64__)
  const uint8x16_t a = vld1q_u8(guest), b = vld1q_u8(guest + 16), c = vld1q_u8(guest + 32), d = vld1q_u8(guest + 48);
  vst1q_u8(held, a);
  vst1q_u8(held + 16, b);
  vst1q_u8(held + 32, c);
  vst1q_u8(held + 48, d);
  uint32x4_t wa = vreinterpretq_u32_u8(vrev32q_u8(a)), wb = vreinterpretq_u32_u8(vrev32q_u8(b)),
             wc = vreinterpretq_u32_u8(vrev32q_u8(c)), wd = vreinterpretq_u32_u8(vrev32q_u8(d));
  if (nan_flush) {
    const uint32x4_t exponent = vdupq_n_u32(0x7F800000u);
    wa = vbicq_u32(wa, vceqq_u32(vandq_u32(wa, exponent), exponent));
    wb = vbicq_u32(wb, vceqq_u32(vandq_u32(wb, exponent), exponent));
    wc = vbicq_u32(wc, vceqq_u32(vandq_u32(wc, exponent), exponent));
    wd = vbicq_u32(wd, vceqq_u32(vandq_u32(wd, exponent), exponent));
  }
  vst1q_u8(swapped, vreinterpretq_u8_u32(wa));
  vst1q_u8(swapped + 16, vreinterpretq_u8_u32(wb));
  vst1q_u8(swapped + 32, vreinterpretq_u8_u32(wc));
  vst1q_u8(swapped + 48, vreinterpretq_u8_u32(wd));
#else
  u32 lanes[kConstantGroupBytes / 4];
  std::memcpy(lanes, guest, kConstantGroupBytes);
  std::memcpy(held, lanes, kConstantGroupBytes);
  for (u32 &lane : lanes) {
    lane = std::byteswap(lane);
    if (nan_flush && (lane & 0x7F800000u) == 0x7F800000u)
      lane = 0;
  }
  std::memcpy(swapped, lanes, kConstantGroupBytes);
#endif
}

// The groups holding a register the shader reads.
constexpr uint64_t UsedGroups(const u32 used[8]) {
  uint64_t groups = 0;
  for (u32 word = 0; word < 8; ++word) {
    for (u32 bits = used[word]; bits;) {
      const u32 k = u32(std::countr_zero(bits)) / 4;
      groups |= uint64_t(1) << (63 - (word * 8 + k));
      bits &= ~(0xFu << (k * 4));
    }
  }
  return groups;
}

// banjo-nx (2026-10-19, render-thread proposal 12): UsedGroups and the registers of those groups (an upload's 'valid')
// once per shader: 'used' is a shader's ShaderCacheEntry::floatConstants (static) or one of the two static masks, so
// its address names it.
// banjo-nx (2026-10-23): the same structure is each shader record's (ShaderRecord::DrawSummary, DrawSummaryOf); this
// memo is left for the two static masks (no shader, or one not in the cache).
using GroupSummary = ShaderRecord::DrawSummary;

void MakeGroupSummary(const u32 used[8], GroupSummary &g) {
  g.groups = UsedGroups(used);
  for (u32 word = 0; word < 8; ++word) {
    const u32 word_groups = u32(g.groups >> (56 - word * 8)) & 0xFFu;  // group word * 8 at bit 7
    u32 valid = 0;
    for (u32 k = 0; k < 8; ++k) {
      if ((word_groups >> (7 - k)) & 1)
        valid |= 0xFu << (k * 4);
    }
    g.valid[word] = valid;
  }
  bool all = true;
  for (u32 word = 0; word < 8; ++word)
    all &= used[word] == ~0u;
  g.bytes = all ? kFloatConstantsUboBytes
        : g.groups ? (64u - u32(std::countr_zero(g.groups))) * u32(kConstantGroupBytes)  // last group + 1
                   : 16u;
  g.used = used;
  std::memcpy(g.used_words, used, sizeof(g.used_words));  // banjo-nx (2026-10-27)
}

GroupSummary g_group_summaries[512];

inline GroupSummary &SummarySlot(const u32 used[8]) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(used);
  return g_group_summaries[(((a >> 3) ^ (a >> 15)) * 0x9E3779B1u >> 23) & 511];
}

const GroupSummary &SummaryOf(const u32 used[8]) {
  GroupSummary &g = SummarySlot(used);
  if (g.used != used)
    MakeGroupSummary(used, g);
  return g;
}

// banjo-nx (2026-10-23, batch 6): a shader's literal registers (below 256) as PushFloatConstants pushes them: the runs of
// consecutive registers in the list's order (an entry at 256 or above skipped, not ending a run), their values in one
// block, and the registers as a 'valid' mask. Made once per cache entry (the list is the entry's, fixed); the push made
// them from the list at every draw that needed them.
// banjo-nx (2026-10-28, render-thread round): one block, 64-byte aligned: this header, then the runs, then the values
// (from 16 bytes on). Before, two vectors in their own allocations: the draw's prefetch of them loaded the object and
// both vectors' pointers first (draw.cpp's 0.49 % line in night_gpu5's profile), and the push read three places.
struct LiteralRuns {
  struct Run {
    u32 first, count, value_word;
  };
  struct RunRange {
    const Run *b, *e;
    const Run *begin() const { return b; }
    const Run *end() const { return e; }
    bool empty() const { return b == e; }
  };
  u32 mask[8] = {};
  u32 run_count = 0;
  u32 value_offset = 0;  // bytes from the block's start to the values (four words a register)
  RunRange runs() const {
    const Run *r = reinterpret_cast<const Run *>(this + 1);
    return {r, r + run_count};
  }
  const u32 *values() const {
    return reinterpret_cast<const u32 *>(reinterpret_cast<const char *>(this) + value_offset);
  }
};

uint64_t g_literals_resident = 0;  // banjo-nx (2026-10-27): literal pushes not made (A/B 34), the 10 s line's
const LiteralRuns *LiteralRunsOf(const ShaderCacheEntry *entry) {
  // Made once per cache entry and kept for the session (as before: the map was never emptied).
  static std::unordered_map<const ShaderCacheEntry *, LiteralRuns *> &made =
      *new std::unordered_map<const ShaderCacheEntry *, LiteralRuns *>();
  LiteralRuns *&slot = made[entry];
  if (!slot) {
    std::vector<LiteralRuns::Run> runs;
    std::vector<u32> values;
    u32 mask[8] = {};
    for (u32 i = 0; i < entry->literalCount; ++i) {
      const ShaderLiteral &l = g_activeShaderLiterals[entry->literalOffset + i];
      if (l.reg >= 256)
        continue;
      if (runs.empty() || l.reg != runs.back().first + runs.back().count)
        runs.push_back({l.reg, 0, u32(values.size())});
      ++runs.back().count;
      u32 words[4];
      std::memcpy(words, l.value, 16);
      values.insert(values.end(), words, words + 4);
      mask[l.reg / 32] |= 1u << (l.reg % 32);
    }
    const size_t runs_end = sizeof(LiteralRuns) + runs.size() * sizeof(LiteralRuns::Run);
    const size_t value_offset = (runs_end + 15) & ~size_t(15);
    const size_t bytes = value_offset + values.size() * sizeof(u32);
    void *block = ::operator new(bytes, std::align_val_t(64));
    LiteralRuns *lr = new (block) LiteralRuns();
    std::memcpy(lr->mask, mask, sizeof(mask));
    lr->run_count = u32(runs.size());
    lr->value_offset = u32(value_offset);
    if (!runs.empty())
      std::memcpy(static_cast<char *>(block) + sizeof(LiteralRuns), runs.data(), runs.size() * sizeof(LiteralRuns::Run));
    if (!values.empty())
      std::memcpy(static_cast<char *>(block) + value_offset, values.data(), values.size() * sizeof(u32));
    slot = lr;
  }
  return slot;
}

// banjo-nx (2026-10-23, render-thread round): a shader record's draw summary (ShaderRecord::DrawSummary), made at its
// first draw: its float constant groups (as SummaryOf of the registers it reads, all of them without a cache entry),
// whether it exports (GatherMemExportStreams) and whether an input is fetched by another index (DescribeBoundFetches).
bool IsSkinOnceVs(uint64_t hash);
// banjo-nx (2026-10-27, render thread night): made once (cold, out of line); the known answer read inline (GCC kept
// the whole function out of line: a call for a test and a return, several times a draw).
[[gnu::cold, gnu::noinline]] const GroupSummary &MakeDrawSummary(const ShaderRecord &r) {
  GroupSummary &g = r.draw_summary;
  {
    static constexpr u32 kAll[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    MakeGroupSummary(r.entry ? r.entry->floatConstants : kAll, g);
    u32 exports = 0;
    bool bound = false;
    if (r.entry) {
      for (u32 word = 0; word < 8; ++word)
        exports |= r.entry->memExportConstants[word];
      const ShaderVertexInput *inputs = g_activeShaderVertexInputs + r.entry->vertexInputOffset;
      const u32 count = std::min<u32>(r.entry->vertexInputCount, kMaxBoundFetches);
      for (u32 i = 0; i < count; ++i)
        bound |= (inputs[i].flags & kVertexInputBoundFetch) != 0;
    }
    g.exports = exports != 0;
    g.bound_fetches = bound;
    g.composite = r.pixel() && CompositePs(r.hash);
    g.skin_once = !r.pixel() && IsSkinOnceVs(r.hash);  // banjo-nx (2026-10-27)
    g.literal_runs = r.entry && r.entry->literalCount ? LiteralRunsOf(r.entry) : nullptr;
    g.has_literals = r.entry && r.entry->literalCount;  // banjo-nx (2026-10-27)
    g.known = true;
  }
  return g;
}
[[gnu::always_inline]] inline const GroupSummary &DrawSummaryOf(const ShaderRecord &r) {
  return r.draw_summary.known ? r.draw_summary : MakeDrawSummary(r);
}

// The registers of a group the shader reads, bit i = the group's register i.
u32 GroupRegisters(const u32 used[8], u32 group) {
  return (used[group / 8] >> (group % 8 * 4)) & 0xFu;
}

// The registers a shader reads ('used', ShaderCacheEntry::floatConstants) as a
// little-endian upload of all 256, reusing the last upload on this command list
// when it holds them with the same bytes. Only the groups holding a register
// the shader reads are written, whole: fixed-size copies the compiler keeps
// inline, where a run of registers at a time cost a branch per register.
UploadAllocation UploadFloatConstants(const FloatConstants &constants, const u32 used[8],
                                      ConstantUpload &last, const ShaderCacheEntry *entry) {
  // banjo-nx (banjo_gpu_literals_cbuf): the shader's literal registers it reads from the constant buffer hold its
  // literal values, as the console's Direct3D loads them at SetShader. An upload holding another shader's (or none) is
  // not reused for it.
  const ShaderCacheEntry *literals = entry && entry->literalCount ? entry : nullptr;
  bool reuse = bool(last.alloc) && (!literals || last.literals == literals);
  for (u32 word = 0; reuse && word < 8; ++word)
    reuse = !(used[word] & ~last.valid[word]);
  // Within the replay that last matched 'last', only groups written since can
  // differ (the running device is the same throughout a replay).
  const bool tracked = constants.replay && last.replay == constants.replay;
  const uint64_t dirty = tracked ? *constants.dirty : ~uint64_t(0);
  const GroupSummary &summary = SummaryOf(used);  // banjo-nx (2026-10-19, render-thread proposal 12)
  const uint64_t groups = summary.groups;
  // Only the registers the shader reads count: the rest of a group may be
  // another shader's, and differ.
  for (uint64_t check = reuse ? groups & dirty : 0; reuse && check; check &= check - 1) {
    const u32 group = 63 - u32(std::countr_zero(check));
    const u32 registers = GroupRegisters(used, group);
    const u8 *cached = last.bytes + group * kConstantGroupBytes;
    const u8 *guest = constants.Register(group * 4);
    uint64_t diff = 0;
    for (u32 i = 0; i < 4; ++i) {
      if (!((registers >> i) & 1))
        continue;
      uint64_t a[2], b[2];
      std::memcpy(a, cached + i * 16, 16);
      std::memcpy(b, guest + i * 16, 16);
      diff |= (a[0] ^ b[0]) | (a[1] ^ b[1]);
    }
    reuse = diff == 0;
  }
  if (reuse) {
    if (constants.replay && !tracked) {
      // Every register the draw reads now matches: keep just those, and
      // track the replay's writes from here.
      std::memcpy(last.valid, used, sizeof(last.valid));
      last.replay = constants.replay;
      *constants.dirty = 0;
    }
    return last.alloc;
  }

  // banjo-nx (2026-10-19, render-thread proposal 10): only the bytes the shader reads (its groups, its literal
  // registers; all of them when it indexes) are allocated, so only those are cleaned at the submit; the uniform buffer
  // stays bound at its full size, which the allocation keeps room for in the chunk. The registers past what is
  // allocated are never read by this shader (it reads only those in floatConstants, all of them when it indexes).
  uint32_t need = summary.bytes;
  if (literals) {
    for (u32 i = 0; i < literals->literalCount; ++i) {
      const ShaderLiteral &l = g_activeShaderLiterals[literals->literalOffset + i];
      if (l.reg < 256)
        need = std::max(need, (l.reg + 1) * 16u);
    }
  }
  UploadAllocation alloc = Video::UploadsLocked().AllocateWithRoom(need, kFloatConstantsUboBytes, 256);
  if (!alloc)
    return alloc;
  auto *out = static_cast<u8 *>(alloc.cpu);
  // banjo-nx (2026-10-18): register 256, zero (pass bonerow of tools/deko3d_glsl_opt.py), for a shader that indexes.
  if (need > 256 * 16)
    std::memset(out + 256 * 16, 0, kFloatConstantsUboBytes - 256 * 16);
  // The upload memory is write-combined: written in order, never read.
  for (uint64_t remaining = groups; remaining;) {
    const u32 group = u32(std::countl_zero(remaining));
    remaining &= ~(uint64_t(1) << (63 - group));
    CopyAndSwapGroup(constants.Register(group * 4), last.bytes + group * kConstantGroupBytes,
                     out + group * kConstantGroupBytes, banjo::renut_gpu::ConstNanFlushOn());  // banjo-nx (2026-10-28)
  }
  // Every register of those groups is in the upload, with the bytes it has
  // now.
  std::memcpy(last.valid, summary.valid, sizeof(last.valid));
  // banjo-nx (banjo_gpu_literals_cbuf): the literal registers, after the groups (a group may hold one). They are not
  // the device's values: out of 'valid', so a shader reading them as registers takes a new upload.
  last.literals = literals;
  if (literals) {
    for (u32 i = 0; i < literals->literalCount; ++i) {
      const ShaderLiteral &l = g_activeShaderLiterals[literals->literalOffset + i];
      if (l.reg >= 256)
        continue;
      std::memcpy(out + l.reg * 16, l.value, 16);
      last.valid[l.reg / 32] &= ~(1u << (l.reg % 32));
    }
  }
  last.alloc = alloc;
  last.replay = constants.replay;
  if (constants.replay)
    *constants.dirty = 0;
  return alloc;
}

// ---------------------------------------------------------------------------
// Pushed constants (banjo-nx, 2026-10-19, banjo_deko3d_push_constants; render-thread proposals 2 and 5)
// ---------------------------------------------------------------------------
// On deko3d the vertex shader's float constants, the pixel shader's and SharedConstants live in three fixed uniform
// buffers (zeroed once, so register 256 stays zero), bound at the bindings the game shaders read (0, 1, 2) and updated
// with pipelined constant-buffer updates (plume::Deko3DPushUniform: dkCmdBufPushConstants; a draw reads the values
// pushed before it in command order). ConstantUpload is then a model of what the GPU's buffer holds after the list's
// updates so far (valid: registers holding the device's bytes in 'bytes'; literal_regs: registers holding 'literals''s
// literal values), reset with each list (Bound). A draw pushes each 64-byte group holding a register the shader reads
// that is not valid or whose guest bytes differ; within a command buffer replay, a valid group the replay has not
// written since (its dirty mask) needs no compare. Then the shader's literal registers, unless they hold them already.
// Every register a shader reads holds what UploadFloatConstants would have written: the device's bytes, byte-swapped,
// or the literal. SharedConstants: the 64-byte lines that differ from what the buffer holds; plume reads bound
// textures from the renderer's copy (Deko3DSetSharedWords). Draws whose point_sprite_gs reads SharedConstants through
// its address keep the ring uploads (an update is ordered for constant-bank reads, not for memory reads).
#if defined(PLUME_DEKO3D)
struct PushedBuffers {
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint64_t address = 0;
};
PushedBuffers g_pushed_buffers;
constexpr uint64_t kPushedFloatStride = (kFloatConstantsUboBytes + 255u) & ~uint64_t(255);
constexpr uint64_t kPushedSharedOffset = 2 * kPushedFloatStride;

bool EnsurePushedBuffersSlow() {
  auto &s = State();
  const uint64_t size = kPushedSharedOffset + ((sizeof(SharedConstants) + 255u) & ~uint64_t(255));
  g_pushed_buffers.buffer = s.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(size));
  if (!g_pushed_buffers.buffer)
    return false;
  if (void *cpu = g_pushed_buffers.buffer->map()) {
    std::memset(cpu, 0, size);  // register 256 (and everything not pushed yet) zero
    g_pushed_buffers.buffer->unmap();
  }
  g_pushed_buffers.address = g_pushed_buffers.buffer->getDeviceAddress();
  return g_pushed_buffers.address != 0;
}
// banjo-nx (2026-10-27, render thread night): made once (out of line), its address tested inline at every draw.
[[gnu::always_inline]] inline bool EnsurePushedBuffers() {
  return g_pushed_buffers.address || EnsurePushedBuffersSlow();
}

// The registers of the groups in a byte of a group mask (group word * 8 + k at bit 7 - k, as SummaryOf takes them),
// as a 'valid' word.
constexpr std::array<u32, 256> kGroupByteRegisters = [] {
  std::array<u32, 256> t{};
  for (u32 b = 0; b < 256; ++b) {
    for (u32 k = 0; k < 8; ++k) {
      if ((b >> (7 - k)) & 1)
        t[b] |= 0xFu << (k * 4);
    }
  }
  return t;
}();

// banjo-nx (2026-10-26, Unleashed-style step 3): per byte of a register word, its two groups (the low nibble the even
// one) as bits 1 and 0 of a group mask byte read from the top (group k of a word at bit 7 - k).
constexpr std::array<u8, 256> kNibblePairGroups = [] {
  std::array<u8, 256> t{};
  for (u32 b = 0; b < 256; ++b)
    t[b] = u8(((b & 0xFu) ? 2u : 0u) | ((b >> 4) ? 1u : 0u));
  return t;
}();
// The groups (bit 63 - group, as UsedGroups) holding a register of 'regs' (a register mask, 8 words).
constexpr uint64_t RegisterGroups(const u32 regs[8]) {
  uint64_t mask = 0;
  for (u32 w = 0; w < 8; ++w) {
    const u32 x = regs[w];
    if (!x)
      continue;
    const u32 byte = u32(kNibblePairGroups[x & 0xFFu]) << 6 | u32(kNibblePairGroups[(x >> 8) & 0xFFu]) << 4 |
                     u32(kNibblePairGroups[(x >> 16) & 0xFFu]) << 2 | u32(kNibblePairGroups[x >> 24]);
    mask |= uint64_t(byte) << (56 - 8 * w);
  }
  return mask;
}

static_assert([] {
  constexpr u32 kPatterns[6] = {0x1u, 0x80000000u, 0x0F0F0F0Fu, 0x12345678u, 0xF0000001u, 0x00010000u};
  for (u32 p = 0; p < 6; ++p) {
    for (u32 w = 0; w < 8; ++w) {
      u32 regs[8] = {};
      regs[w] = kPatterns[p];
      regs[(w + 3) % 8] |= kPatterns[(p + 1) % 6];
      if (RegisterGroups(regs) != UsedGroups(regs))
        return false;
    }
  }
  return true;
}(), "RegisterGroups is UsedGroups");

// banjo-nx (2026-10-28, GPU block): banjo_renut_diag_push_lines' counts (see the key).
uint64_t g_diag_push_lines[22] = {};
uint64_t g_diag_push_float[4] = {};  // [0] VS bytes, [1] PS bytes, [2] VS bytes of draws without a PS, [3] their read groups' bytes
// banjo-nx (2026-10-28, census for a record-time compare of constants): the written groups PushFloatConstants compared
// with the pushed bytes, and those the same (the game set them again unchanged: copied by the recording, pointed at by
// the replay and compared here for nothing).
uint64_t g_const_compared = 0, g_const_same = 0;
bool g_diag_draw_no_ps = false;
inline bool DiagPushOn() {
  static const bool on = REXCVAR_GET(banjo_renut_diag_push_lines);
  return on;
}
inline void DiagPushLines(u32 first, u32 run) {
  if (DiagPushOn()) [[unlikely]]
    for (u32 l = first; l < first + run && l < 22; ++l)
      ++g_diag_push_lines[l];
}
// One stage's float constants (see above); 'ubo' its buffer's address.
void PushFloatConstants(plume::RenderCommandList *cmd, const FloatConstants &constants, const u32 used[8],
                        ConstantUpload &last, const ShaderCacheEntry *entry, uint64_t ubo, const GroupSummary &summary) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kPushConstants);  // blur-nx (2026-11-17)
  // banjo-nx (2026-10-27, constants): whether the shader has literals and the registers it reads from its summary (the
  // record's first lines, prefetched with the shader lookup), not from its cache entry (a miss at most shader changes).
  // 'used' names the same registers as the summary's copy when it is the summary's own mask.
  const ShaderCacheEntry *literals = summary.known ? (summary.has_literals ? entry : nullptr)
                                                   : (entry && entry->literalCount ? entry : nullptr);
  if (summary.used == used)
    used = summary.used_words;
  // banjo-nx (2026-10-27, A/B 34): its literal registers all still holding its values (lit_owner): they are its again.
  // Their 'valid' bits were cleared when they were pushed, and only a group update (which clears the owner) sets them.
  // 2026-10-28: banjo_renut_literals_resident, off (the console: the check cost more than the pushes it saved).
  static const bool literals_resident = REXCVAR_GET(banjo_renut_literals_resident);
  if (literals_resident && literals && last.literals != literals && summary.literal_runs &&
      banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 34) {
    const auto *lr = static_cast<const LiteralRuns *>(summary.literal_runs);
    bool resident = !lr->runs().empty();
    for (const LiteralRuns::Run &run : lr->runs()) {
      for (u32 k = 0; k < run.count && resident; ++k)
        resident = run.first + k < 256 && last.lit_owner[run.first + k] == literals;
      if (!resident)
        break;
    }
    if (resident) {
      last.literals = literals;
      std::memcpy(last.literal_regs, lr->mask, sizeof(last.literal_regs));
      ++g_literals_resident;
    }
  }
  // The shader's literal registers, when the buffer holds them (literal_regs is exactly one shader's): a shader that
  // indexes reads every register, its literal ones included, and those hold the literal, not the device's bytes.
  const u32 *own_literals = literals && last.literals == literals ? last.literal_regs : nullptr;
  const bool tracked = constants.replay && last.replay == constants.replay;
  const uint64_t dirty = tracked ? *constants.dirty : ~uint64_t(0);
  // The groups to update (bit 63 - group, as UsedGroups).
  uint64_t push = 0;
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas_constants): within a tracked replay (the chain's draws are one) with
  // none of the shader's groups dirty, a group is pushed only for a register the buffer does not hold: when every
  // register the shader reads (but its own literal ones) is held, the loop below would push nothing. Word by word.
  // banjo-nx (2026-10-26, Unleashed-style step 3): the groups with a register the shader reads that the buffer does not
  // hold, found word by word (RegisterGroups); only those and the dirty ones are looked at below: a held group the
  // replay has not written since needs nothing (the loop went through every group the shader reads, at every draw).
  u32 need[8];
  for (u32 w = 0; w < 8; ++w)
    need[w] = used[w] & ~(own_literals ? own_literals[w] : 0u) & ~last.valid[w];
  const uint64_t unheld = RegisterGroups(need);
  const bool all_held = tracked && (dirty & summary.groups) == 0 && unheld == 0;
  for (uint64_t check = all_held ? 0 : summary.groups & (unheld | dirty); check; check &= check - 1) {
    const u32 bit = u32(std::countr_zero(check));
    const u32 group = 63 - bit;
    const u32 lits = own_literals ? (own_literals[group / 8] >> (group % 8 * 4)) & 0xFu : 0u;
    const u32 registers = GroupRegisters(used, group) & ~lits;
    const u32 held = (last.valid[group / 8] >> (group % 8 * 4)) & 0xFu;
    if (registers & ~held) {
      push |= uint64_t(1) << bit;
      continue;
    }
    if (!((dirty >> bit) & 1))
      continue;  // valid, and the replay has not written it since
    const u8 *cached = last.bytes + group * kConstantGroupBytes;
    const u8 *guest = constants.Register(group * 4);
    uint64_t diff = 0;
    for (u32 i = 0; i < 4; ++i) {
      if (!((registers >> i) & 1))
        continue;
      uint64_t a[2], b[2];
      std::memcpy(a, cached + i * 16, 16);
      std::memcpy(b, guest + i * 16, 16);
      diff |= (a[0] ^ b[0]) | (a[1] ^ b[1]);
    }
    ++g_const_compared;
    g_const_same += diff == 0;
    if (diff)
      push |= uint64_t(1) << bit;
  }
  // The updates, a run of consecutive groups at a time.
  uint64_t pushed_bytes = 0;
  bool overwrote_literal = false;
  for (uint64_t remaining = push; remaining;) {
    const u32 first = u32(std::countl_zero(remaining));  // the lowest group left
    // The run's length: the set bits from the first one down (group g is bit 63 - g). banjo-nx (2026-10-27): one
    // count instead of a test a group.
    const u32 count = u32(std::countl_one(remaining << first));
    alignas(16) u32 lanes[64 * kConstantGroupBytes / 4];
    for (u32 k = 0; k < count; ++k) {
      const u32 group = first + k;
      // banjo-nx (2026-10-27, render thread night): swapped on the way (a separate pass over the run before);
      // banjo-nx (2026-10-28): both copies from one NEON load (CopyAndSwapGroup).
      CopyAndSwapGroup(constants.Register(group * 4), last.bytes + group * kConstantGroupBytes,
                       reinterpret_cast<u8 *>(lanes + k * (kConstantGroupBytes / 4)), banjo::renut_gpu::ConstNanFlushOn());
      last.valid[group / 8] |= 0xFu << (group % 8 * 4);
      for (u32 q = 0; q < 4; ++q)
        last.lit_owner[group * 4 + q] = nullptr;  // banjo-nx (2026-10-27): the device's bytes now
      const u32 lits = (last.literal_regs[group / 8] >> (group % 8 * 4)) & 0xFu;
      if (lits) {
        overwrote_literal = true;
        last.literal_regs[group / 8] &= ~(0xFu << (group % 8 * 4));
      }
      remaining &= ~(uint64_t(1) << (63 - group));
    }
    plume::Deko3DPushUniform(cmd, ubo, kFloatConstantsUboBytes, first * kConstantGroupBytes, lanes,
                             count * kConstantGroupBytes);
    pushed_bytes += count * kConstantGroupBytes;
  }
  // The literal registers: after the groups (a group may hold one, and its update overwrote it).
  if (overwrote_literal && last.literals != literals)
    last.literals = nullptr;  // another shader's literals, no longer all in place
  if (literals && (last.literals != literals || overwrote_literal)) {
    if (last.literals != literals) {
      // Another shader's literal registers left in the buffer are out of 'valid' already; literal_regs is this one's.
      for (u32 &word : last.literal_regs)
        word = 0;
    }
    // banjo-nx (2026-10-23, batch 6): the runs made once for the shader (LiteralRuns), pushed as the loop below
    // would push them, and its registers' mask words.
    if (const auto *lr = static_cast<const LiteralRuns *>(summary.literal_runs)) {
      for (const LiteralRuns::Run &run : lr->runs()) {
        plume::Deko3DPushUniform(cmd, ubo, kFloatConstantsUboBytes, run.first * 16, lr->values() + run.value_word,
                                 run.count * 16);
        pushed_bytes += run.count * 16;
        for (u32 k = 0; k < run.count && run.first + k < 256; ++k)
          last.lit_owner[run.first + k] = literals;  // banjo-nx (2026-10-27)
      }
      for (u32 w = 0; w < 8; ++w) {
        last.valid[w] &= ~lr->mask[w];
        last.literal_regs[w] |= lr->mask[w];
      }
      last.literals = literals;
    }
  }
  if (literals && (last.literals != literals || overwrote_literal) && !summary.literal_runs) {
    if (last.literals != literals) {
      for (u32 &word : last.literal_regs)
        word = 0;
    }
    // banjo-nx (2026-10-23, render-thread round): a run of consecutive literal registers in one update (one per
    // register before: the same bytes at the same places, in the same order for any register written twice).
    alignas(16) u32 run_values[256 * 4];
    u32 run_first = 0, run_count = 0;
    auto flush_run = [&] {
      if (run_count) {
        plume::Deko3DPushUniform(cmd, ubo, kFloatConstantsUboBytes, run_first * 16, run_values, run_count * 16);
        pushed_bytes += run_count * 16;
        run_count = 0;
      }
    };
    for (u32 i = 0; i < literals->literalCount; ++i) {
      const ShaderLiteral &l = g_activeShaderLiterals[literals->literalOffset + i];
      if (l.reg >= 256)
        continue;
      if (run_count && l.reg != run_first + run_count)
        flush_run();
      if (!run_count)
        run_first = l.reg;
      std::memcpy(run_values + run_count * 4, l.value, 16);
      ++run_count;
      last.valid[l.reg / 32] &= ~(1u << (l.reg % 32));
      last.literal_regs[l.reg / 32] |= 1u << (l.reg % 32);
      last.lit_owner[l.reg] = literals;  // banjo-nx (2026-10-27)
    }
    flush_run();
    last.literals = literals;
  }
  // The replay's writes from here: the next draw of this replay skips a valid group the replay has not written since,
  // so 'valid' keeps only registers that hold the device's bytes now: those of the groups pushed, those this draw
  // compared, and (tracked) those of groups the replay has not written since this replay's last draw. A register of a
  // group compared for another (a shader reads some of a group's registers) is not known (emulator verify run 61: 8 %
  // of the draws read one stale).
  if (constants.replay) {
    const uint64_t unsure = (tracked ? dirty : ~uint64_t(0)) & ~push;
    // banjo-nx (2026-10-20, from sa2-nx): with no group unsure (a chain draw whose groups are all clean) the loop changes nothing.
    for (u32 word = 0; unsure && word < 8; ++word)
      last.valid[word] &= ~kGroupByteRegisters[u32(unsure >> (56 - word * 8)) & 0xFFu] | used[word];
    last.replay = constants.replay;
    *constants.dirty = 0;
  } else {
    last.replay = 0;
  }
  banjo::renut_gpu::CountPushedConstants(pushed_bytes, 0);
  if (DiagPushOn()) [[unlikely]] {  // banjo-nx (2026-10-28, diagnostic)
    const bool vs_stage = ubo == g_pushed_buffers.address;
    g_diag_push_float[vs_stage ? 0 : 1] += pushed_bytes;
    if (vs_stage && g_diag_draw_no_ps) {
      g_diag_push_float[2] += pushed_bytes;
      g_diag_push_float[3] += uint64_t(std::popcount(summary.groups)) * kConstantGroupBytes;
    }
  }
  if (banjo::renut_gpu::PushConstantsVerifyOn()) {
    bool ok = true;
    const u32 *literal_regs = literals && last.literals == literals ? last.literal_regs : nullptr;
    for (u32 r = 0; r < 256 && ok; ++r) {
      if (!((used[r / 32] >> (r % 32)) & 1))
        continue;
      if (literal_regs && ((literal_regs[r / 32] >> (r % 32)) & 1))
        continue;  // the literal (checked below)
      ok = ((last.valid[r / 32] >> (r % 32)) & 1) &&
           std::memcmp(last.bytes + r * 16, constants.Register(r), 16) == 0;
    }
    if (literals) {
      for (u32 i = 0; i < literals->literalCount && ok; ++i) {
        const u32 reg = g_activeShaderLiterals[literals->literalOffset + i].reg;
        ok = reg >= 256 || (last.literals == literals && ((last.literal_regs[reg / 32] >> (reg % 32)) & 1));
      }
    }
    if (!ok) {
      static uint32_t logged = 0;
      if (logged++ < 16)
        RGPU_WARN("[pushed] a register the shader reads is not what the model says the GPU holds (banjo_deko3d_push_"
                  "constants_verify)");
      banjo::renut_gpu::CountPushedConstants(1, 3);
    }
  }
}

#endif  // PLUME_DEKO3D (the pushed buffers; banjo-nx 2026-10-29: the helpers below are every build's, the PC's too)

// banjo-nx (2026-10-27, render thread night): fixed-size equality as 8-byte loads, inline: GCC called the C library's
// memcmp for these (the texture slots' device and header words at every slot of every draw, the render state's words).
template <size_t N>
[[gnu::always_inline]] inline bool SameBytes(const void *a, const void *b) {
  static_assert(N % 8 == 0);
  uint64_t diff = 0;
  for (size_t i = 0; i < N; i += 8) {
    uint64_t x, y;
    std::memcpy(&x, static_cast<const u8 *>(a) + i, 8);
    std::memcpy(&y, static_cast<const u8 *>(b) + i, 8);
    diff |= x ^ y;
  }
  return diff == 0;
}

// SharedConstants: the lines that differ from what the buffer holds (PushSharedConstants).
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): the pushes counted; 'equal_lines' (bit i = 64-byte line i) are lines the
// buffer holds already: the texture words of a chain draw that are the previous draw's when it pushed last, and the
// vertex fetch slots a draw without fetches leaves zero when the last push's were zero too.
uint64_t g_shared_pushes = 0;
bool g_held_fetch_slots_zero = false;  // the buffer's vertex fetch slots (lines 9-14) are zero
constexpr u32 kTextureLines = 0xFFu;      // lines 0-7: the texture and sampler words
constexpr u32 kFetchSlotLines = 0x7E00u;  // lines 9-14: vertex_fetch_slots
static_assert(offsetof(SharedConstants, vertex_fetch_slots) == 9 * 64 &&
              sizeof(SharedConstants::vertex_fetch_slots) == 6 * 64);

// banjo-nx (2026-10-26, banjo_renut_persistent_shared): the draws' SharedConstants, kept from one draw to the next
// (DrawSharedLocked).
alignas(64) SharedConstants g_draw_sc;
// banjo-nx (2026-10-26, API-level stage 1, Unleashed's SetDirtyValue): the 64-byte lines of the persistent block
// (bit 21: the 16-byte tail) written with a new value since the block was last pushed; PushSharedConstants pushes
// exactly those, comparing nothing. All of them until the first push.
constexpr u32 kSharedLines = (u32(sizeof(SharedConstants)) + 63) / 64;
constexpr u32 kAllSharedLines = (1u << kSharedLines) - 1;
static_assert(kSharedLines == 25);  // halo3-nx: 22 before the loop constants (24); renut-ultimate: the clip planes; the tail, bit 24, is still 16 bytes
u32 g_sc_dirty = kAllSharedLines;
// banjo-nx (2026-10-28, per-draw night): the lines from the first to the last as one mask (a loop a line before, run at
// every mark the compiler could not fold: FillTextures' slot words, the fields of a block passed by reference).
constexpr u32 SharedLinesOf(size_t offset, size_t size) {
  if (!size)
    return 0;
  const u32 first = u32(offset / 64), last = u32((offset + size - 1) / 64);
  return ((2u << last) - 1u) & ~((1u << first) - 1u);
}
static_assert(SharedLinesOf(0, 1) == 1u && SharedLinesOf(60, 8) == 3u && SharedLinesOf(64, 64) == 2u &&
              SharedLinesOf(1344, 16) == (1u << 21) && SharedLinesOf(100, 300) == 0x7Eu, "SharedLinesOf");
inline void MarkShared(const SharedConstants &sc, const void *p, size_t size) {
  if (&sc == &g_draw_sc)
    g_sc_dirty |= SharedLinesOf(size_t(static_cast<const u8 *>(p) - reinterpret_cast<const u8 *>(&sc)), size);
}
// A field set to 'value', its line marked when that changes it (the bits: -0.0 is not 0.0).
template <typename T>
inline void SetShared(SharedConstants &sc, T &field, const T &value) {
  bool same;
  if constexpr (sizeof(T) % 8 == 0)
    same = SameBytes<sizeof(T)>(&field, &value);  // banjo-nx (2026-10-27): inline (a memcmp call for 16 and 32 bytes)
  else
    same = std::memcmp(&field, &value, sizeof(T)) == 0;
  if (!same) {
    std::memcpy(&field, &value, sizeof(T));
    MarkShared(sc, &field, sizeof(T));
  }
}

// halo3-nx (2026-11-02): the device's loop constants, byte-swapped to the words the shaders unpack (g_LoopConstant).
inline void SetLoopConstants(SharedConstants &sc, const d3d::Device &dev) {
  if (!banjo::renut_gpu::LoopConstantsOn())
    return;
  const u8 *from = reinterpret_cast<const u8 *>(&dev.m_Constants.VertexShaderI[0]);
  u32 loops[32];
  for (u32 i = 0; i < 32; ++i) {
    u32 word;
    std::memcpy(&word, from + 4 * i, 4);
    loops[i] = __builtin_bswap32(word);
  }
  SetShared(sc, sc.loop_constants, loops);
}

// rr6-nx (2026-11-03) and vivapinata-nx: this draw's user clip plane 0, or zeros (a distance of 0 clips nothing). Plane 0 only, all
// those games use; the log says if a draw asks for more. Bit 16 of PA_CL_CLIP_CNTL is clip_disable, as the emulated GPU reads it.
inline void SetUserClipPlane(SharedConstants &sc, const d3d::Device &dev, uint64_t vs_hash) {
  float plane[4] = {};
  const u32 clip_control = dev.m_ControlPacket.ClipControl;
  const u32 bits = clip_control & 0x3Fu;
  if (bits && !(clip_control & 0x10000u) && banjo::renut_gpu::UserClipPlaneOn()) {
    if (bits & 1u) {
      for (u32 i = 0; i < 4; ++i)
        plane[i] = float(dev.m_ClipPlanes[0][i]);
    }
    static uint64_t said[16];
    static u32 said_count = 0;
    if (said_count < 16 && std::find(said, said + said_count, vs_hash ^ bits) == said + said_count) {
      said[said_count++] = vs_hash ^ bits;
      RGPU_INFO("[renut] user clip plane: VS {:016X} planes {:02X}{}, plane 0 ({:.3f},{:.3f},{:.3f},{:.3f})", vs_hash, bits,
                (bits & ~1u) ? " (only plane 0 is applied)" : "", plane[0], plane[1], plane[2], plane[3]);
    }
  }
  SetShared(sc, sc.user_clip_planes[0], plane);
}

inline bool PersistentSharedVerifyOn() {
  static const bool on = REXCVAR_GET(banjo_renut_persistent_shared_verify);
  return on;
}

#if defined(PLUME_DEKO3D)
void PushSharedConstants(plume::RenderCommandList *cmd, const SharedConstants &sc, u32 equal_lines = 0) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kPushConstants);  // blur-nx (2026-11-17)
  constexpr u32 kLine = 64;
  constexpr u32 kSize = u32(sizeof(SharedConstants));
  ++g_shared_pushes;
  const uint64_t ubo = g_pushed_buffers.address + kPushedSharedOffset;
  const u8 *now = reinterpret_cast<const u8 *>(&sc);
  u8 *held = reinterpret_cast<u8 *>(&g_bound.pushed_shared);
  uint64_t pushed_bytes = 0;
  // banjo-nx (2026-10-26, API-level stage 1): the persistent block's lines are known: those written with a new value
  // since its last push (g_sc_dirty), the held copy being the block in every other line.
  const bool tracked = &sc == &g_draw_sc;
  if (!g_bound.pushed_shared_valid) {
    DiagPushLines(0, kSharedLines);  // banjo-nx (2026-10-28, diagnostic)
    plume::Deko3DPushUniform(cmd, ubo, kSize, 0, now, kSize);
    std::memcpy(held, now, kSize);
#if defined(PLUME_DEKO3D)
    plume::Deko3DSharedLinesWritten(cmd, ~0u);  // banjo-nx (2026-10-27): the held copy's lines bindTextures reads
#endif
    g_bound.pushed_shared_valid = true;
    pushed_bytes = kSize;
    if (tracked)
      g_sc_dirty = 0;
  } else if (tracked && !PersistentSharedVerifyOn()) {
    u32 differ = g_sc_dirty & ~equal_lines;
    g_sc_dirty = 0;
#if defined(PLUME_DEKO3D)
    if (differ)
      plume::Deko3DSharedLinesWritten(cmd, differ);  // banjo-nx (2026-10-27)
#endif
    while (differ) {
      const u32 first = u32(std::countr_zero(differ));
      const u32 run = u32(std::countr_one(differ >> first));
      const u32 at = first * kLine, end = std::min((first + run) * kLine, kSize);
      DiagPushLines(first, run);  // banjo-nx (2026-10-28, diagnostic)
      plume::Deko3DPushUniform(cmd, ubo, kSize, at, now + at, end - at);
      u32 off = at;
      for (; off + kLine <= end; off += kLine)
        std::memcpy(held + off, now + off, kLine);
      if (off < end)
        std::memcpy(held + off, now + off, 16);
      pushed_bytes += end - at;
      differ &= ~(((1u << run) - 1u) << first);
    }
  } else {
    // banjo-nx (2026-10-23, render-thread round): the lines that differ as one mask (line_compare.h
    // Lines64DifferMask: no branch per line), then its runs: the same updates as a line at a time.
    constexpr u32 kWhole = kSize / kLine;  // 21 whole lines, then the 16-byte tail
    static_assert(kWhole < 31 && kSize % kLine == 16);
    u32 differ = banjo::renut_gpu::Lines64DifferMask(held, now, kWhole);
    uint64_t tail[2], held_tail[2];
    std::memcpy(tail, now + kWhole * kLine, 16);
    std::memcpy(held_tail, held + kWhole * kLine, 16);
    differ |= u32(((tail[0] ^ held_tail[0]) | (tail[1] ^ held_tail[1])) != 0) << kWhole;
    differ &= ~equal_lines;
#if defined(PLUME_DEKO3D)
    if (differ)
      plume::Deko3DSharedLinesWritten(cmd, differ);  // banjo-nx (2026-10-27)
#endif
    if (tracked) {  // banjo-nx (2026-10-26): the verify form (banjo_renut_persistent_shared_verify)
      if (differ & ~g_sc_dirty) {
        static uint32_t logged = 0;
        if (logged++ < 16)
          RGPU_WARN("banjo_renut_persistent_shared_verify: shared constant lines {:06X} changed without a mark",
                    differ & ~g_sc_dirty);
      }
      g_sc_dirty = 0;
    }
    while (differ) {
      const u32 first = u32(std::countr_zero(differ));
      const u32 run = u32(std::countr_one(differ >> first));
      const u32 at = first * kLine, end = std::min((first + run) * kLine, kSize);
      plume::Deko3DPushUniform(cmd, ubo, kSize, at, now + at, end - at);
      // banjo-nx (2026-10-23, batch 6): line by line at a fixed size (inline), the tail's 16 bytes last: no C library
      // call per run.
      u32 off = at;
      for (; off + kLine <= end; off += kLine)
        std::memcpy(held + off, now + off, kLine);
      if (off < end)
        std::memcpy(held + off, now + off, 16);
      pushed_bytes += end - at;
      differ &= ~(((1u << run) - 1u) << first);
    }
  }
  banjo::renut_gpu::CountPushedConstants(pushed_bytes, 1);
}
#endif  // PLUME_DEKO3D

TextureFetch DeviceTextureFetch(const d3d::Device &dev, u32 r) {
  return TextureFetch::FromGuest(dev.m_Constants.TextureFetch[r]);
}

// rr6-nx (2026-11-03, rr6_fetch_over_pointer): the texture a slot samples. The GPU reads the fetch constant and never the
// device's pointer, and SetTexture(NULL) leaves the fetch constant as it was, so a draw after it still samples that texture.
// The slot's last texture is the one those fetch words came from.
u32 g_fetch_owner[d3d::kTextureFetchSlots] = {};
inline u32 SlotTextureVa(const GuestDevice &device, u32 r) {
  const u32 va = device.U32(d3d::kDeviceOffsetTextures + r * 4);
  if (va) {
    g_fetch_owner[r] = va;
    return va;
  }
  if (!g_fetch_owner[r] || !banjo::renut_gpu::FetchOverPointerOn())
    return 0;
  const TextureFetch fetch = DeviceTextureFetch(device.dev(), r);
  if (!fetch.IsTexture())
    return 0;
  // renut-ultimate (port 63, 2026-11-10, renut_fetch_owner_same_data; Viva Pinata's finding): the last texture only while
  // the fetch constant still describes its data (textures.cpp SameData: format, endian and base address in dword 1, the
  // size in dword 2, the dimension in dword 5). A fetch constant naming other data (Viva Pinata: object BCCA0550, a
  // 1280x720 resolve destination, with a fetch of a 16x64 texture at 0F99B000) was bound through the old texture's
  // host texture, which textures.cpp then remade for the fetch, throwing the resolve away (the title logo's dark shadow,
  // "[flash] resolve destination ... dropped"). Without a header pointer the slot takes port 37's fetch-only path: the
  // fetch's own texture, and a resolve's content found by its address (ResolvedAlias).
  if (banjo::renut_gpu::FetchOwnerSameDataOn()) {
    const TextureFetch owner = TextureFetch::FromHeader(GuestPtr<const d3d::BaseTexture>(g_fetch_owner[r])->Format);
    if (owner.dword[1] != fetch.dword[1] || owner.dword[2] != fetch.dword[2] ||
        ((owner.dword[5] ^ fetch.dword[5]) & 0x600u) != 0) {
      static std::atomic<uint32_t> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
        RGPU_INFO("[renut] slot {} after SetTexture(NULL): the fetch constant names other data than its last texture "
                  "{:08X} (fetch {:08X} {:08X}, header {:08X} {:08X}): sampled as the fetch alone "
                  "(renut_fetch_owner_same_data)", r, g_fetch_owner[r], fetch.dword[1], fetch.dword[2], owner.dword[1],
                  owner.dword[2]);
      return 0;
    }
  }
  return g_fetch_owner[r];
}

// banjo-nx (deko3d renderer, docs/deko3d-renderer.md): the sampler words hold the sampler id where a texture handle has
// it (bit 20 up), so the deko3d shaders make the handle as (texture word & 0xFFFFF) | (sampler word & 0xFFF00000): no
// shift, and the form Ryujinx's shader translator follows back to the constant buffer (the emulator tests).
inline u32 SamplerWord(u32 slot) { return IsDeko3D() ? slot << 20 : slot; }

// banjo-nx (2026-10-27, the user's Phase A): a texture object (render/draw.h). Line 0 is its identity, written once by
// the recording thread that makes it before it is published (and read by the others' lookups); lines 1-2 are the
// render thread's answer for it (what a memo entry holds: the slot words, the bind's effects, what they hold for).
// (Outside the anonymous namespace: render/draw.h declares it for command_buffers.cpp.)
}  // namespace
struct alignas(64) TextureObject {
  u32 va = 0;
  u32 header[6] = {};  // the header's Format words (raw), when the recording made it
  // Line 1: the answer.
  alignas(64) bool valid = false;
  u8 dim = 0;
  u32 index = 0, sampler = 0;
  u32 device[6] = {};          // the fetch words it was made with
  uint64_t checked_gen = 0;    // g_texture_bind_gen when its epoch was last found the same
  uint64_t frame = 0;          // TextureFrameLocked of its content check
  // Line 2.
  alignas(64) uint64_t epoch = 0;  // TextureBindingEpoch when made
  uint64_t layout_epoch = 0;       // g_layout_epoch when its transitions last held
  BindEffects fx;
};
static_assert(sizeof(TextureObject) == 192);
// banjo-nx (2026-10-27, geometry objects, banjo_renut_geometry_objects): a stream source or index buffer as the
// recording resolved it (render/draw.h FindStreamObject / FindIndexObject; line 0, fixed once published) and the render
// thread's answer for it (line 1: the whole copy the old path found, and what that answer holds for). See
// GeometryObjectsHit.
// An answer: a line each. Two a stream object: a buffer is drawn by two vertex shaders in turn (the shadow pass's and
// the main pass's; with one answer the emulator's title screen stored as many answers as it took, 40 % of the draws
// finding the other pass's); the index buffer object's answer does not depend on the shader (slot 0).
struct alignas(64) GeometryAnswer {
  const void *copy = nullptr;  // the CachedStream (a stream) or CachedIndices (an index buffer)
  uint64_t entry_gen = 0;      // g_geometry_gen when answered (0: none yet)
  uint64_t layout_hash = 0;    // a stream's: the layout, vertex shader and strides it was found for
  const ShaderRecord *vs = nullptr;
  const u32 *strides = nullptr;
  const u8 *guest = nullptr;  // the copy's guest range (its interval proof)
  u32 bytes = 0;
  u32 count = 0;  // a stream's source vertices (a draw's base is below), an index buffer's indices
  u32 proven = 0;  // banjo-nx (2026-10-28): g_flush_serial's low word when its copy was last proven through it
};
struct alignas(64) GeometryObject {
  u32 key[6] = {};
  u8 next = 0;  // the answer slot a new answer takes (streams)
  bool is32 = false;
  GeometryAnswer answer[2];  // lines 1 and 2
};
static_assert(sizeof(GeometryAnswer) == 64 && sizeof(GeometryObject) == 192);
namespace {
// banjo-nx (2026-10-28, the heap): 32,768 (65,536): the d4 console run made 1,648 in twelve minutes of title and city,
// and static tables come out of the heap a loader gives the NRO; the geometry objects below took the room.
constexpr u32 kTextureObjects = 32768, kTextureObjectSlots = 65536;  // 6 MB of objects, 2 MB of table
TextureObject g_texture_objects[kTextureObjects];
std::atomic<u32> g_texture_objects_made{0};
// banjo-nx (2026-10-28, total CPU: the recording threads' lookups, banjo_renut_record_lookups): a table slot holds its
// object's key words beside the object's number, so a lookup reads one line (the slot's) where it read two (the slot's
// pointer, then the object's key line): the c4 console run's two recording threads spent 46 % of NewOp (about 6 ms a
// frame) in those two misses. 'id' is the object's index + 1 (0: empty; kSlotBusy: claimed, its key being written);
// the key words are written before 'id' is published (release) and read only after it is seen (acquire).
constexpr u32 kSlotBusy = 0xFFFFFFFFu;
struct alignas(32) TextureSlot {
  std::atomic<u32> id;
  u32 va;
  u32 header[6];
};
static_assert(sizeof(TextureSlot) == 32);
TextureSlot g_texture_slots[kTextureObjectSlots];
// banjo-nx (2026-10-27, geometry objects): 3 MB of objects; the recording threads' tables (streams, index buffers).
// banjo-nx (2026-10-28): 32,768 (16,384): objects are never given back, and the d4 city made 15,456 in twelve minutes
// (a buffer past the last has no object: the slower lookups). Still a limit for very long sessions.
constexpr u32 kGeometryObjects = 32768, kStreamObjectSlots = 65536, kIndexObjectSlots = 32768;
GeometryObject g_geometry_objects[kGeometryObjects];
std::atomic<u32> g_geometry_objects_made{0};
template <u32 kWords>
struct alignas(kWords == 6 ? 32 : 16) GeometrySlot {
  std::atomic<u32> id;
  u32 key[kWords];
};
static_assert(sizeof(GeometrySlot<6>) == 32 && sizeof(GeometrySlot<3>) == 16);
GeometrySlot<6> g_stream_slots[kStreamObjectSlots];
GeometrySlot<3> g_index_slots[kIndexObjectSlots];
// Raised whenever stream or index cache entries may be erased (ClearWholeLast): every answer older than it is void (its
// copy pointer may name an erased entry). Copies dropped from an entry are not: the entry stays, and its proof fails.
uint64_t g_geometry_gen = 1;
uint64_t g_geo_hits = 0, g_geo_misses = 0, g_geo_answers = 0;  // the 10 s line's
uint64_t g_tex_obj = 0, g_tex_obj_refresh = 0, g_tex_obj_stored = 0, g_tex_obj_verified = 0, g_tex_obj_differ = 0,
         g_tex_obj_header_differ = 0, g_tex_obj_rewritten_differ = 0, g_ib_recorded = 0, g_ib_verify_differ = 0;

// banjo-nx (2026-10-19, CPU proposal 2, banjo_renut_cpu_memos): FillTextures' answer per fetch slot (the shared-constant
// index and sampler words), keyed by the texture address, the device's fetch constant and the guest header's words,
// valid for the frame (the first bind of a frame runs the content check) while g_texture_epoch is unchanged. A hit does
// the bind's transitions and counters again.
// banjo-nx (2026-10-23, render-thread round): two whole cache lines, the compared words and the answer in the first
// (136 bytes before, most entries over three lines).
struct alignas(64) TextureSlotMemo {
  u32 va = 0;
  u32 dim = 0;
  u32 index = 0;
  u32 sampler = 0;
  u32 device[6] = {};
  u32 header[6] = {};
  uint64_t frame = 0;
  uint64_t epoch = 0;
  uint64_t depth_epoch = 0;  // banjo-nx (2026-10-19, render-thread proposal 3): for binds through depth words
  BindEffects fx;
};
static_assert(sizeof(TextureSlotMemo) == 128);
// banjo-nx (2026-10-19, render-thread proposal 3): by texture (its header address and the fetch constant's first
// words), not by fetch slot: a texture bound earlier in the frame, through any slot, is a hit.
// banjo-nx (2026-10-23, render-thread round): 2,048 entries (256 KB; 512 before): the city binds 600-odd textures and
// fetch constants a frame, which the 512 direct-mapped entries conflicted on, each conflict a bind's slow path.
// banjo-nx (2026-10-27, render thread night): 4,096 (512 KB): the setters2 city run's binds missed 2,048 by another
// entry 130 times a frame, each a bind's slow path; the entries a frame touches are the same either way.
constexpr uint32_t kTextureMemoEntries = 4096;
TextureSlotMemo g_texture_slot_memo[kTextureMemoEntries];

inline TextureSlotMemo &TextureMemoSlot(u32 texture_va, const TextureFetch &fetch) {
  static_assert(kTextureMemoEntries == 1u << 12);
  return g_texture_slot_memo[((texture_va ^ (fetch.dword[0] * 0x85EBCA6Bu) ^ fetch.dword[1]) * 0x9E3779B1u) >> 20];
}

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): 'slot_fx' (32 entries, or null) receives each bound slot's effects (its
// memo entry's on a hit, the bind's otherwise) and 'all_memo' whether every bound slot went through the memo (a
// texture header to key it), for the chain's reuse of the next draw.
// banjo-nx (2026-10-23, batch 6): 'draw_fetches' (or null), the fetch constants of the mask's slots as DrawStateLocked
// converted them for the draw (they were converted three times a draw: its prefetches, DrawNeedsExportFlush, here).
// banjo-nx (2026-10-23, batch 6): 'same_as_last' (or null) receives whether the texture and sampler words came out as
// the previous call's (the same slots, each slot's four words the same): g_texture_fills counts the calls.
// banjo-nx (2026-10-26, Unleashed-style step 1, banjo_renut_persistent_shared): the draws' SharedConstants, kept
// from one draw to the next, and what the last draws left in it beyond the words every draw sets.
struct DrawSharedTrack {
  bool ready = false;
  u32 texture_slots = 0;  // FillTextures' last mask: the slots whose words may not be the defaults
  u32 fetch_slots = 0;    // vertex_fetch_slots[64 + i] UploadVertexFetchData last set
  u32 bound_entries = 0;  // bound_fetch entries DescribeBoundFetches last set
  bool memexport = false, point = false, skin = false;
};
DrawSharedTrack g_draw_sc_track;
constexpr u32 kDefaultTextureWords[3] = {kNullTextureSlot, kNullTexture3DSlot, kNullTextureCubeSlot};

// A slot's three texture words (the dimension's index, the others' defaults) and its sampler word.
inline void SetSlotWords(SharedConstants &sc, u32 r, u32 dim, u32 index, u32 sampler) {
  for (u32 d = 0; d < 3; ++d)
    SetShared(sc, sc.texture_indices[d][r], d == dim ? index : kDefaultTextureWords[d]);
  SetShared(sc, sc.sampler_indices[r], sampler);
}
inline void SetSlotDefaults(SharedConstants &sc, u32 r);

// banjo-nx (2026-10-26, API-level stage 1): each fetch slot's last answer, for the persistent block (whose words for the
// slot are that answer's): a slot whose inputs are the same as then (the memo hit's test, word for word) takes it here
// and does the memo hit's work, without the memo's entry (256 KB of them, rarely still in a cache line; and with the
// fetch constants unchanged, DrawStateLocked no longer prefetches it).
struct SlotLast {
  bool valid = false;
  u32 va = 0;
  u32 device[6] = {};
  u32 header[6] = {};
  uint64_t frame = 0, epoch = 0, depth_epoch = 0;
  BindEffects fx;
  uint64_t layout_epoch = 0;  // banjo-nx (2026-10-26, round 4): g_layout_epoch when the slot's transitions last held
  const TextureObject *obj = nullptr;  // banjo-nx (2026-10-27, Phase A): the object whose answer its words are
  // banjo-nx (2026-10-28, per-draw night, banjo_renut_texture_slot_skip): set only by the object path's answer: the
  // caller's effects array it stored the object's effects into, and the binding generation, frame and layout epoch it
  // answered at (null: no skip; every other writer of the slot clears it).
  const BindEffects *obj_fx = nullptr;
  uint64_t obj_gen = 0, obj_frame = 0, obj_layout = 0;
};
SlotLast g_slot_last[d3d::kTextureFetchSlots];
uint64_t g_tex_slot_skips = 0;  // the 10 s line's (TextureAnswersReport)
uint64_t g_tex_slot_skip_checked = 0, g_tex_slot_skip_differ = 0, g_tex_slot_skip_unheld = 0;  // (its verify)
// The fast path's effects array (FastState::fx, set by FastDrawLocked): the only caller array the skip trusts (another
// caller's stack array can sit at the same address on a later call with other contents).
const BindEffects *g_skip_fx = nullptr;
// banjo-nx (2026-10-28, per-draw night): banjo_renut_point_fast_state (A/B 59).
inline bool PointFastStateOn() {
  static const bool on = REXCVAR_GET(banjo_renut_point_fast_state);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 59;
}
// bangaio-nx (2026-10-29): banjo_renut_point_fast_draw (A/B 80 here; 69 in bangaio-nx, katamari-nx's here).
inline bool PointFastDrawOn() {
  static const bool on = REXCVAR_GET(banjo_renut_point_fast_draw);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 80;
}
// The point sprites' render state as point_sprite_gs reads it (SharedConstants::point_state), from the tessellator
// packet: DrawStateLocked's and (bangaio-nx) FastDrawLocked's.
// PA_SU_POINT_SIZE: height 0-15, width 16-31, half the size in 12.4 fixed
// point (SetRenderState_PointSize 0x8264C6C0 stores the size * 8);
// PA_SU_POINT_MINMAX: minimum 0-15, maximum 16-31, the size in 12.4
// (SetRenderState_PointSizeMin 0x8264C720 stores the minimum * 16).
inline void PointStateOf(const d3d::Device &dev, float point_state[4]) {
  const auto *tessellator = reinterpret_cast<const be_u32 *>(dev.m_TessellatorPacket);
  const u32 point_size = tessellator[0], point_min_max = tessellator[1];
  point_state[0] = float(point_size >> 16) / 8.0f;
  point_state[1] = float(point_size & 0xFFFF) / 8.0f;
  // banjo-nx (2026-10-18, the depth of field's bokeh sprites): the minimum and maximum are radii in 12.4 as well, as
  // the hardware (and Xenia, the emulated renderer: point_vertex_diameter_min/max = value * 2 / 16) reads them; / 16
  // halved the largest sprites.
  const float minmax_scale = banjo::renut_gpu::PointMinMaxRadiusOn() ? 8.0f : 16.0f;
  point_state[2] = float(point_min_max & 0xFFFF) / minmax_scale;
  point_state[3] = float(point_min_max >> 16) / minmax_scale;
}
// banjo-nx (2026-10-28): OFF by default: it HURT on the console (A/B 53, night_final: render thread +0.64 ms a frame
// with it on in the static city); kept for measuring again.
inline bool TextureSlotSkipOn() {
  static const bool on = REXCVAR_GET(banjo_renut_texture_slot_skip);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 53;
}

// banjo-nx (2026-10-27, Phase A): an object's answer for this draw: the same fetch words, an epoch check at this binding
// generation (else the buckets, as BindingEpoch), the frame's content check done (else RefreshBindingLocked, as the
// memo's refresh, A/B 32). Only answers without depth words or hand-overs are stored (StoreTextureObject).
inline bool TextureObjectHolds(TextureObject &o, const TextureFetch &fetch, u32 va, bool &refreshed) {
  if (!o.valid || !SameBytes<24>(o.device, fetch.dword))
    return false;
  if (o.checked_gen != g_texture_bind_gen) {
    if ((TextureBindingEpoch(fetch.BasePage(), va) & ~(uint64_t(1) << 63)) != o.epoch)
      return false;
    o.checked_gen = g_texture_bind_gen;
  }
  if (o.frame != TextureFrameLocked()) {
    if (banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 32 || !RefreshBindingLocked(va, fetch, o.fx))
      return false;
    o.frame = TextureFrameLocked();
    refreshed = true;
  }
  return true;
}
inline void StoreTextureObject(TextureObject &o, const TextureFetch &fetch, u32 va, u32 dim, u32 index, u32 sampler,
                               const BindEffects &fx, uint64_t frame) {
  if (fx.depth_source || fx.handover_surface || fx.handover_image)
    return;
  std::memcpy(o.device, fetch.dword, sizeof(o.device));
  o.dim = u8(dim);
  o.index = index;
  o.sampler = sampler;
  o.fx = fx;
  o.frame = frame;
  o.epoch = TextureBindingEpoch(fetch.BasePage(), va) & ~(uint64_t(1) << 63);
  o.checked_gen = g_texture_bind_gen;
  o.layout_epoch = g_layout_epoch;  // (its transitions were just applied)
  o.valid = true;
  ++g_tex_obj_stored;
}
// banjo-nx (2026-10-27, A/B 35): the epoch a memo entry or slot answer of this binding holds: the global count for one
// through depth words or a hand-over, its page's and address's counts otherwise (textures.h TextureBindingEpoch).
inline bool FineTextureEpochs() { return banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 35; }
inline uint64_t BindingEpoch(const BindEffects &fx, const TextureFetch &fetch, u32 va) {
  return fx.depth_source || fx.handover_surface || fx.handover_image || !FineTextureEpochs()
             ? g_texture_epoch | (uint64_t(1) << 63)
             : TextureBindingEpoch(fetch.BasePage(), va) & ~(uint64_t(1) << 63);
}
inline bool SlotLastHolds(const SlotLast &l, u32 va, const TextureFetch &fetch, const d3d::BaseTexture *header) {
  return l.valid && l.va == va && header && l.frame == TextureFrameLocked() && l.epoch == BindingEpoch(l.fx, fetch, va) &&
         (!l.fx.depth_source || l.depth_epoch == g_depth_epoch) &&
         SameBytes<24>(l.device, fetch.dword) &&
         SameBytes<24>(l.header, &header->Format) &&
         !(l.fx.handover_surface && HandOverSourceIsDrawTarget(l.fx.handover_surface));
}

// banjo-nx (2026-10-27, Phase A): the draw's objects when they apply (FillTextures), and the dimension the slot's
// words give (the dimension's word is the one not a default).
inline const DrawResources *memo_on_objects(const DrawResources *r) {
  return r && banjo::renut_gpu::CpuMemosOn() && FineTextureEpochs() ? r : nullptr;
}
inline u32 last_dim(const SharedConstants &sc, u32 r);
uint64_t g_texture_fills = 0;
// banjo-nx (2026-10-27, setter rewrite): how FillTextures answered each bound slot (the 10 s line's): by the slot's last
// answer, by the memo, or by a bind, and why the memo did not answer (no entry for this texture and fetch constant, the
// entry of an earlier frame, of an earlier texture epoch, of an earlier depth epoch, other words).
uint64_t g_tex_last = 0, g_tex_memo = 0, g_tex_bind = 0, g_tex_refresh = 0;
uint64_t g_tex_miss_other = 0, g_tex_miss_frame = 0, g_tex_miss_epoch = 0, g_tex_miss_depth = 0, g_tex_miss_words = 0;
std::string TextureAnswersReportLocal() {
  const uint64_t all = g_tex_last + g_tex_memo + g_tex_bind;
  if (!all)
    return {};
  std::string out = fmt::format("texture slots: {} answered by the slot's last ({:.1f} %), {} by the memo ({:.1f} %), {} "
                                "by the memo with the frame's check only (A/B 32), {} bound ({:.1f} %); the memo missed "
                                "for: another entry {}, an earlier frame {}, an earlier texture epoch {}, an earlier depth "
                                "epoch {}, other words {}",
                                g_tex_last, 100.0 * double(g_tex_last) / double(all), g_tex_memo,
                                100.0 * double(g_tex_memo) / double(all), g_tex_refresh, g_tex_bind,
                                100.0 * double(g_tex_bind) / double(all), g_tex_miss_other, g_tex_miss_frame,
                                g_tex_miss_epoch, g_tex_miss_depth, g_tex_miss_words);
  g_tex_last = g_tex_memo = g_tex_bind = g_tex_refresh = 0;
  g_tex_miss_other = g_tex_miss_frame = g_tex_miss_epoch = g_tex_miss_depth = g_tex_miss_words = 0;
  if (g_tex_slot_skips)  // banjo-nx (2026-10-28)
    out += fmt::format("; {} object slots kept (banjo_renut_texture_slot_skip)", std::exchange(g_tex_slot_skips, 0));
  if (g_tex_slot_skip_checked)
    out += fmt::format("; slot skip verify: {} checked, {} differ, {} with the object answering other words since",
                       std::exchange(g_tex_slot_skip_checked, 0), std::exchange(g_tex_slot_skip_differ, 0),
                       std::exchange(g_tex_slot_skip_unheld, 0));
  return out;
}
// A bind's slot words (dimension, texture index word, sampler word): FillTextures' bind path and Phase A's verify.
void BindingWords(const TextureBinding &binding, const TextureFetch &fetch, u32 &dim, u32 &index, u32 &sampler) {
  dim = binding.dimension == TextureDimension::Tex3D  ? 1
        : binding.dimension == TextureDimension::Cube ? 2
                                                      : 0;
  // Stacked textures go through the 3D fetches, flagged (tfetchStacked in
  // shader_common.h), with the fetch constant's filter between layers and
  // its Z address mode.
  index = binding.slot | binding.scale.code << kResolutionScaleShift;
  if (binding.stacked) {
    index |= kStackedTextureBit;
    if (fetch.VolumeMagFilter() != TextureFilter::Point ||
        fetch.VolumeMinFilter() != TextureFilter::Point) {
      index |= kStackedLayerLinearBit;
    }
    if (fetch.Address(2) == TextureAddress::Wrap)
      index |= kStackedLayerWrapBit;
  }
  // sasrt-nx (2026-10-20), renut-ultimate port 25 (shader_common.h renutSigns): the fetch constant's gamma and biased signs per
  // result component (through its swizzle), bits 16-19 gamma, 13-15 biased X-Z: in the texture word on the PC, in the sampler
  // word on deko3d (below its slot at bit 20), whose texture word's low 20 bits are the image id.
  static_assert(kTextureDescriptorCount <= (1u << 13), "the slot must stay below bit 13");
  u32 sign_flags = 0;
  if ((fetch.dword[0] & 0x3FCu) && banjo::renut_gpu::TextureSignsOn()) {
    const u32 swizzle = (fetch.dword[3] >> 1) & 0xFFFu;
    u32 gamma = 0, biased = 0;
    for (u32 c = 0; c < 4; ++c) {
      const u32 source = (swizzle >> (c * 3)) & 7u;
      if (source >= 4)
        continue;
      const TextureSign sign = fetch.Sign(source);
      if (sign == TextureSign::Gamma)
        gamma |= 1u << c;
      else if (sign == TextureSign::Biased && c < 3)
        biased |= 1u << c;
    }
    sign_flags = gamma << 16 | biased << 13;
    if (!IsDeko3D())
      index |= sign_flags;
  }
  // sasrt-nx (2026-10-20): the fetch's scale as a power of two in bits 20-25 (signed): the fetch constant's exponent adjust (every
  // dimension here: sasrt-nx copied a 2D texture scaled instead), plus, for a 3D, cube or stacked fetch, the range of an integer
  // number format (a 16- or 8-bit normalized format read as its integers).
  int exponent = banjo::renut_gpu::TextureExpAdjustOn() ? int(i32(fetch.dword[3] << 13) >> 26) : 0;
  if ((dim != 0 || binding.stacked) && (fetch.dword[3] & 1u) && banjo::renut_gpu::TextureIntFormatOn()) {
    const u32 format = fetch.dword[1] & 63u;
    const bool sign = ((fetch.dword[0] >> 2) & 3u) == 1u;  // the first component signed
    if (format == 24 || format == 25 || format == 26)  // k_16, k_16_16, k_16_16_16_16
      exponent += sign ? 15 : 16;
    else if (format == 2 || format == 6 || format == 10)  // k_8, k_8_8_8_8, k_8_8
      exponent += sign ? 7 : 8;
  }
  exponent = std::max(-32, std::min(31, exponent));
  index |= u32(exponent & 63) << 20;
  sampler = SamplerWord(SamplerSlotLocked(fetch, binding.lod_scale));
  if (IsDeko3D())
    sampler |= sign_flags;  // sasrt-nx: below the sampler id's bit 20
}
// sasrt-nx (2026-10-29, sasrt_renut_texture_memo_fetch_only): a slot's guest header, or for a slot bound by its fetch
// constant alone a zero header standing in for it: the memo, the slot's last answer, the recorded draw's and the chain's
// reuse checks answer it by its fetch words, which every one of them compares too.
const d3d::BaseTexture *SlotHeader(u32 va) {
  static const d3d::BaseTexture kNone{};
  return va ? GuestPtr<const d3d::BaseTexture>(va) : banjo::renut_gpu::TextureMemoFetchOnlyOn() ? &kNone : nullptr;
}
// sasrt-nx (2026-10-29, sasrt_renut_texture_memo_fetch_only_verify): a memo answer for a slot without a header pointer
// against a bind now.
uint64_t g_fetch_only_checked = 0, g_fetch_only_differ = 0;
void VerifyFetchOnlyAnswer(const TextureFetch &fetch, u32 dim, u32 index, u32 sampler) {
  if (!banjo::renut_gpu::TextureMemoFetchOnlyVerifyOn()) [[likely]]
    return;
  BindEffects fx;
  u32 vdim, vindex, vsampler;
  BindingWords(BindTextureLocked(0, fetch, &fx), fetch, vdim, vindex, vsampler);
  if ((++g_fetch_only_checked & 0x3FFFF) == 0)
    RGPU_INFO("[renut] sasrt_renut_texture_memo_fetch_only_verify: {} memo answers bound again, {} differ",
              g_fetch_only_checked, g_fetch_only_differ);
  if (vdim == dim && vindex == index && vsampler == sampler)
    return;
  ++g_fetch_only_differ;
  static uint32_t logged = 0;
  if (logged++ < 16)
    RGPU_WARN("sasrt_renut_texture_memo_fetch_only_verify: the memo's words {}/{:X}/{:X}, a bind now {}/{:X}/{:X} "
              "(fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})", dim, index, sampler, vdim, vindex, vsampler,
              fetch.dword[0], fetch.dword[1], fetch.dword[2], fetch.dword[3], fetch.dword[4], fetch.dword[5]);
}
// kameo-nx (2026-11-03, banjo_renut_fetch3d_on_2d, the idea from Kameo: RePowered's texture_upload.cpp, MIT): the slots this
// draw's shaders read through tfetch3D. XenosRecomp samples those through the 3D table, where a 2D texture reads the null
// slot, whereas the GPU samples whatever the fetch constant describes. Marked stacked, the texture takes the 3D table's
// stacked path (tfetchStacked), there on deko3d too, with no change to any shader.
u32 g_fetch_3d_mask = 0;

inline u32 Fetch3DMaskOf(const ShaderRecord *vs, const ShaderRecord *ps) {
  if (!banjo::renut_gpu::Fetch3DOn2DOn())
    return 0;
  return (vs ? vs->fetch_3d_mask : 0) | (ps ? ps->fetch_3d_mask : 0);
}

[[gnu::noinline]] void SampleAsStacked(TextureFetch &fetch, u32 r) {
  if (!fetch.IsTexture() || fetch.Dimension() != TextureDimension::Tex2D || fetch.Stacked())
    return;
  fetch.dword[1] |= 1u << 10;
  static std::unordered_map<u32, bool> logged;
  if (logged.size() < 32 && logged.emplace(fetch.BasePage(), true).second) {
    RGPU_INFO("[renut] a 2D texture read through tfetch3D (banjo_renut_fetch3d_on_2d): slot {} format {} size {}x{} base {:08X}", r,
              u32(fetch.Format()), (fetch.dword[2] & 0x1FFF) + 1, ((fetch.dword[2] >> 13) & 0x1FFF) + 1, fetch.BasePage() << 12);
  }
}

// renut-ultimate (2026-11-16, port 145; tw2de_nx's fix of 2026-10-31, off by default at the user's word): the texture
// filters the shaders' fetch instructions give (render/fetch_filters.h, read when a shader is made). The hardware
// samples with the instruction's mag / min / mip filter and anisotropy where it gives one; Renut built every sampler
// from the fetch constant alone (FINAL FANTASY XIII's light shafts: a linear blur of a point-sampled mask came out
// stepped). FillTextures puts them in the slot's fetch words, so every memo after it (the slot's last answer, the
// texture objects, the slot memo, the sampler's own) keys on them; DrawStateLocked's chain reuse keys on the shaders;
// FastDrawLocked leaves such draws to DrawStateLocked.
bool FetchFiltersOn() {
  static const bool on = REXCVAR_GET(banjo_renut_fetch_filters);
  return on;
}
// The draw's: the vertex shader's, the pixel shader's over them. nullptr when off or none; 'key' names the pair.
const FetchFilterOverrides *DrawFetchFiltersLocked(const ShaderRecord *vs, const ShaderRecord *ps, uint64_t &key) {
  key = 0;
  if (!FetchFiltersOn() || !vs || (vs->fetch_filters.Empty() && (!ps || ps->fetch_filters.Empty())))
    return nullptr;
  static FetchFilterOverrides merged;  // render thread
  merged = vs->fetch_filters;
  if (ps)
    MergeFetchFilters(merged, ps->fetch_filters);
  key = ((uint64_t(vs->self_va) << 32 | (ps ? ps->self_va : 0)) ^ vs->hash ^ (ps ? ps->hash * 3 : 0)) | 1;
  static std::atomic<int> logged{0};
  if (logged.load(std::memory_order_relaxed) < 16 && logged.fetch_add(1, std::memory_order_relaxed) < 16)
    RGPU_INFO("[filters] shaders {:016X} / {:016X}: their fetches give their own filters for fetch constants {:08X} "
              "(banjo_renut_fetch_filters)", vs->hash, ps ? ps->hash : 0, merged.slots);
  return &merged;
}

void FillTextures(const GuestDevice &device, u32 fetch_mask, SharedConstants &sc, BindEffects *slot_fx = nullptr,
                  bool *all_memo = nullptr, const TextureFetch *draw_fetches = nullptr, bool *same_as_last = nullptr,
                  const FetchFilterOverrides *filters = nullptr) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kFillTextures);  // blur-nx (2026-11-17)
  const u32 as_stacked = g_fetch_3d_mask & fetch_mask;  // kameo-nx (2026-11-03)
  if (as_stacked)
    draw_fetches = nullptr;  // made here, with those slots marked
  // Port 145 (banjo_renut_fetch_filters): the shaders' filters in word 3 of the slots they cover (mag 19-20, min 21-22,
  // mip 23-24, anisotropy 25-27). The draw's fetch words copied first when the caller made them.
  if (filters && !(filters->slots & fetch_mask))
    filters = nullptr;
  union FilteredSlots {
    FilteredSlots() {}
    TextureFetch f[d3d::kTextureFetchSlots];
  } filtered;
  if (filters && draw_fetches) [[unlikely]] {
    for (u32 bits = fetch_mask & ((1u << d3d::kTextureFetchSlots) - 1u); bits; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      filtered.f[r] = draw_fetches[r];
      filtered.f[r].dword[3] = filters->Apply(r, filtered.f[r].dword[3]);
    }
    draw_fetches = filtered.f;
  }
  if (all_memo)
    *all_memo = banjo::renut_gpu::CpuMemosOn();
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_texture_defaults_copy): the null words as one copy of a block made once (160 stores a
  // draw before).
  struct DefaultWords {
    u32 indices[3][32];
    u32 samplers[32];
  };
  static const DefaultWords kDefaults = [] {
    DefaultWords w{};
    for (u32 r = 0; r < 32; ++r) {
      w.indices[0][r] = kNullTextureSlot;
      w.indices[1][r] = kNullTexture3DSlot;
      w.indices[2][r] = kNullTextureCubeSlot;
      w.samplers[r] = SamplerWord(kLinearClampSamplerSlot);
    }
    return w;
  }();
  static const bool defaults_copy = REXCVAR_GET(banjo_renut_texture_defaults_copy);
  // banjo-nx (2026-10-26, banjo_renut_persistent_shared): the persistent block holds the defaults but in the last
  // call's slots: those not set again are put back, below; the set ones write all their words.
  const bool persistent = &sc == &g_draw_sc;
  if (persistent) {
  } else if (defaults_copy) {
    std::memcpy(sc.texture_indices, kDefaults.indices, sizeof(sc.texture_indices));
    std::memcpy(sc.sampler_indices, kDefaults.samplers, sizeof(sc.sampler_indices));
  } else {
    for (u32 r = 0; r < 32; ++r) {
      sc.texture_indices[0][r] = kNullTextureSlot;
      sc.texture_indices[1][r] = kNullTexture3DSlot;
      sc.texture_indices[2][r] = kNullTextureCubeSlot;
      sc.sampler_indices[r] = SamplerWord(kLinearClampSamplerSlot);
    }
  }
  const d3d::Device &dev = device.dev();
  // banjo-nx (2026-10-23, render-thread round): the host textures a memo hit checks the layout of, fetched before the
  // slots are gone through (the memo entries came with DrawStateLocked's prefetches). Prefetches only.
  // banjo-nx (2026-10-23, batch 5): each set slot's fetch constant converted once (it was twice: here and below), and
  // only the set slots gone through below (all 26 were tested).
  fetch_mask &= (1u << d3d::kTextureFetchSlots) - 1u;
  if (persistent) {
    for (u32 bits = g_draw_sc_track.texture_slots & ~fetch_mask; bits; bits &= bits - 1) {
      SetSlotDefaults(sc, u32(std::countr_zero(bits)));
      g_slot_last[std::countr_zero(bits)].valid = false;
      g_slot_last[std::countr_zero(bits)].obj = nullptr;  // banjo-nx (2026-10-27, Phase A)
      g_slot_last[std::countr_zero(bits)].obj_fx = nullptr;  // banjo-nx (2026-10-28)
    }
    g_draw_sc_track.texture_slots = fetch_mask;
  }
  // Storage only (TextureFetch's member initialisers would zero all 26, 624 bytes, at every draw); set slots are written.
  union FetchSlots {
    FetchSlots() {}
    TextureFetch f[d3d::kTextureFetchSlots];
  } slots;
  const TextureFetch *const fetches = draw_fetches ? draw_fetches : slots.f;
  // banjo-nx (2026-10-27, Phase A): the replayed draw's texture objects (not with A/B 35's off half: its answers hold
  // by g_texture_epoch, not the buckets the objects check).
  static const bool objects_verify = REXCVAR_GET(banjo_renut_resource_objects_verify);
  const DrawResources *const objs = memo_on_objects(g_draw_resources);
  // banjo-nx (2026-10-28): banjo_renut_texture_slot_skip, not in the verify forms (they compare the whole way's words).
  const bool slot_skip = slot_fx && slot_fx == g_skip_fx && TextureSlotSkipOn() && !objects_verify &&
                         !PersistentSharedVerifyOn();
  for (u32 bits = fetch_mask; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    if (!draw_fetches) {
      slots.f[r] = DeviceTextureFetch(dev, r);
      if ((as_stacked >> r) & 1) [[unlikely]]
        SampleAsStacked(slots.f[r], r);
      if (filters) [[unlikely]]  // port 145
        slots.f[r].dword[3] = filters->Apply(r, slots.f[r].dword[3]);
    }
    const u32 va = SlotTextureVa(device, r);
    if (!va)
      continue;
    if (objs && ((objs->mask >> r) & 1)) {
      __builtin_prefetch(reinterpret_cast<const char *>(objs->slot[r]) + 64);
      __builtin_prefetch(reinterpret_cast<const char *>(objs->slot[r]) + 128);
      continue;
    }
    const SlotLast &last = g_slot_last[r];
    if (persistent && last.valid && last.va == va && SameBytes<24>(last.device, fetches[r].dword))
      continue;  // banjo-nx (2026-10-26): answered by the slot's last (most likely)
    const TextureSlotMemo &m = TextureMemoSlot(va, fetches[r]);
    if (m.va == va && m.fx.transition[0])
      __builtin_prefetch(&m.fx.transition[0]->layout);
  }
  // blur-nx (2026-11-18, the census's #5): how often a call binds the same slots, textures and fetch words as the call
  // before, or as one of the 16 before it (what a cache of whole texture sets would answer).
  if (::renut::gpu::host_batch::CensusOn() && fetch_mask) {
    uint64_t h = uint64_t(fetch_mask) * 0x9E3779B97F4A7C15ull;
    for (u32 bits = fetch_mask; bits; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      h = (h ^ SlotTextureVa(device, r)) * 0xFF51AFD7ED558CCDull;
      for (u32 k = 0; k < 6; ++k)
        h = (h ^ fetches[r].dword[k]) * 0xC4CEB9FE1A85EC53ull;
    }
    static uint64_t ring[16] = {};
    static uint64_t last_set = 0;
    static u32 at = 0;
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kTextureSetCalls);
    if (h == last_set) {
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kTextureSetSame);
    } else {
      for (const uint64_t e : ring) {
        if (e == h) {
          ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kTextureSetSeen);
          break;
        }
      }
      ring[at++ & 15] = last_set;
      last_set = h;
    }
  }
  for (u32 bits = fetch_mask; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    const TextureFetch &fetch = fetches[r];
    const u32 texture_va = SlotTextureVa(device, r);
    const auto *header = SlotHeader(texture_va);  // sasrt-nx (2026-10-29, sasrt_renut_texture_memo_fetch_only)
    const bool memo = banjo::renut_gpu::CpuMemosOn() && header;
    if (all_memo && !memo)
      *all_memo = false;
    SlotLast &last = g_slot_last[r];
    // banjo-nx (2026-10-27, Phase A): the slot's texture object's answer (no guest header, no memo, no epoch buckets
    // while the binding generation holds).
    // (The record made it from this slot's address word, which the op's chunks give the device here: its identity line
    // is not read on this path, only in verify.)
    TextureObject *const obj = objs && ((objs->mask >> r) & 1) ? objs->slot[r] : nullptr;
    // banjo-nx (2026-10-28, per-draw night, banjo_renut_texture_slot_skip / A/B 53): the slot's object as this path last
    // answered it, for the same fetch words, in the same frame, binding generation and layout epoch, into the same
    // caller's effects array: the object path below would find the same answer (TextureObjectHolds: the epoch and the
    // frame's content check held then and nothing they read changed), apply no transition (the layout epoch is the
    // same), write no word (the block holds this object's words) and store the same effects. Only the depth counters
    // count again. A draw changing one texture slot made every other slot of its mask go the whole way.
    if (obj && memo && persistent && slot_skip && last.obj == obj && last.obj_fx == slot_fx &&
        last.obj_gen == g_texture_bind_gen && last.obj_frame == TextureFrameLocked() &&
        last.obj_layout == g_layout_epoch && last.va == texture_va && SameBytes<24>(last.device, fetch.dword)) {
      static const bool skip_verify = REXCVAR_GET(banjo_renut_texture_slot_skip_verify);
      if (skip_verify) [[unlikely]] {
        ++g_tex_slot_skip_checked;
        bool refreshed = false;
        if (TextureObjectHolds(*obj, fetch, texture_va, refreshed)) {
          bool same = !refreshed && obj->layout_epoch == g_layout_epoch &&
                      SameBytes<sizeof(BindEffects)>(&obj->fx, &slot_fx[r]) && sc.sampler_indices[r] == obj->sampler;
          for (u32 d = 0; d < 3; ++d)
            same = same && sc.texture_indices[d][r] == (d == obj->dim ? obj->index : kDefaultTextureWords[d]);
          g_tex_slot_skip_differ += !same;
        } else {
          ++g_tex_slot_skip_unheld;  // (the object answered other fetch words since: this slot's words stay right)
        }
      }
      if (slot_fx[r].float_sampled)
        banjo::renut_gpu::CountDepthFloatSampled();
      if (slot_fx[r].words_viewed)
        banjo::renut_gpu::CountDepthWords(true);
      ++g_tex_obj;
      ++g_tex_slot_skips;
      continue;
    }
    if (obj && memo) {
      bool refreshed = false;
      if (TextureObjectHolds(*obj, fetch, texture_va, refreshed)) {
        if (obj->layout_epoch != g_layout_epoch || refreshed) {
          for (HostTexture *t : obj->fx.transition)
            if (t && t->layout != plume::RenderTextureLayout::SHADER_READ)
              TransitionTextureLocked(t, plume::RenderTextureLayout::SHADER_READ);
          obj->layout_epoch = g_layout_epoch;
        }
        if (obj->fx.float_sampled)
          banjo::renut_gpu::CountDepthFloatSampled();
        if (obj->fx.words_viewed)
          banjo::renut_gpu::CountDepthWords(true);
        if (objects_verify) [[unlikely]] {
          ++g_tex_obj_verified;
          const bool header_same = obj->va == texture_va && SameBytes<24>(obj->header, &header->Format);
          g_tex_obj_header_differ += !header_same;
          // A header the game rewrote since the recording: the path without objects binds from the header as it is
          // now (with the device's words where dwords 0, 3 or 5 disagree): bind that way too and compare the words.
          // (The bind's own work is done here, in verify only.)
          if (!header_same && obj->va == texture_va) {
            BindEffects vfx;
            u32 vdim, vindex, vsampler;
            BindingWords(BindTextureLocked(texture_va, fetch, &vfx), fetch, vdim, vindex, vsampler);
            if (vdim != obj->dim || vindex != obj->index || vsampler != obj->sampler) {
              ++g_tex_obj_rewritten_differ;
              static uint32_t logged = 0;
              if (logged++ < 16)
                RGPU_WARN("banjo_renut_resource_objects_verify: slot {} texture {:08X} rewritten since recorded: the "
                          "object's words {}/{:X}/{:X}, a bind now {}/{:X}/{:X}; header recorded {:08X} {:08X} {:08X} "
                          "{:08X} {:08X} {:08X}", r, texture_va, obj->dim, obj->index, obj->sampler, vdim, vindex,
                          vsampler, obj->header[0], obj->header[1], obj->header[2], obj->header[3], obj->header[4],
                          obj->header[5]);
            }
          }
          const TextureSlotMemo &vm = TextureMemoSlot(texture_va, fetch);
          if (header_same && vm.va == texture_va && vm.frame == TextureFrameLocked() &&
              SameBytes<24>(vm.device, fetch.dword) && SameBytes<24>(vm.header, &header->Format) &&
              (vm.dim != obj->dim || vm.index != obj->index || vm.sampler != obj->sampler)) {
            ++g_tex_obj_differ;
            static uint32_t logged = 0;
            if (logged++ < 16)
              RGPU_WARN("banjo_renut_resource_objects_verify: slot {} texture {:08X}: the object's words {}/{:X}/{:X}, "
                        "the memo's {}/{:X}/{:X}", r, texture_va, obj->dim, obj->index, obj->sampler, vm.dim, vm.index,
                        vm.sampler);
          }
        }
        // The words: already in the block when they are this object's answer for these fetch words (last.obj is cleared
        // by every other writer of the slot's words).
        if (!(persistent && last.obj == obj && last.va == texture_va && SameBytes<24>(last.device, fetch.dword)))
          SetSlotWords(sc, r, obj->dim, obj->index, obj->sampler);
        if (slot_fx)
          slot_fx[r] = obj->fx;
        if (persistent) {
          // The slot's last for the object path; the path without objects vouches only for answers it made itself
          // (SlotLastHolds compares the header it kept: not taken here, to keep the object's identity line unread).
          last.valid = false;
          last.obj = obj;
          last.va = texture_va;
          std::memcpy(last.device, fetch.dword, sizeof(last.device));
          last.obj_fx = slot_fx;  // banjo-nx (2026-10-28): what the skip above needs
          last.obj_gen = g_texture_bind_gen;
          last.obj_frame = TextureFrameLocked();
          last.obj_layout = g_layout_epoch;
        }
        ++g_tex_obj;
        g_tex_obj_refresh += refreshed;
        continue;
      }
    }
    if (persistent && memo && SlotLastHolds(last, texture_va, fetch, header)) {
      // banjo-nx (2026-10-26, API-level stage 1): the memo hit's work with the slot's last answer; its words are in the
      // block already. (Round 4, ARM: the transitions only after a layout changed somewhere since they last held for
      // this slot: each bound texture's layout read was a cache miss a slot a draw.)
      if (last.layout_epoch != g_layout_epoch) {
        for (HostTexture *t : last.fx.transition)
          if (t && t->layout != plume::RenderTextureLayout::SHADER_READ)
            TransitionTextureLocked(t, plume::RenderTextureLayout::SHADER_READ);
        if (last.fx.handover_surface)
          TransitionSurfaceLocked(last.fx.handover_surface, plume::RenderTextureLayout::SHADER_READ);
        if (last.fx.handover_image)
          TransitionDepthImageLocked(*last.fx.handover_image, plume::RenderTextureLayout::SHADER_READ);
        last.layout_epoch = g_layout_epoch;
      }
      if (last.fx.float_sampled)
        banjo::renut_gpu::CountDepthFloatSampled();
      if (last.fx.words_viewed)
        banjo::renut_gpu::CountDepthWords(true);
      if (slot_fx)
        slot_fx[r] = last.fx;
      ++g_tex_last;
      if (obj && SameBytes<24>(obj->header, &header->Format)) {  // banjo-nx (2026-10-27, Phase A)
        StoreTextureObject(*obj, fetch, texture_va, last_dim(sc, r), sc.texture_indices[last_dim(sc, r)][r],
                           sc.sampler_indices[r], last.fx, last.frame);
        last.obj = obj;
        last.obj_fx = nullptr;  // banjo-nx (2026-10-28)
      }
      if (PersistentSharedVerifyOn()) {  // the memo's answer, when it has one, is the block's words
        const TextureSlotMemo &vm = TextureMemoSlot(texture_va, fetch);
        if (vm.va == texture_va && vm.frame == last.frame && vm.epoch == last.epoch &&
            std::memcmp(vm.device, fetch.dword, sizeof(vm.device)) == 0 &&
            (sc.texture_indices[vm.dim][r] != vm.index || sc.sampler_indices[r] != vm.sampler)) {
          static uint32_t logged = 0;
          if (logged++ < 16)
            RGPU_WARN("banjo_renut_persistent_shared_verify: slot {}'s kept words are not the memo's answer", r);
        }
      }
      continue;
    }
    TextureSlotMemo &m = TextureMemoSlot(texture_va, fetch);
    // The slot's last answer from here on: the memo's hit or the bind's (both store the same as the memo entry).
    auto keep_last = [&](const TextureSlotMemo &from) {
      if (!persistent)
        return;
      last.valid = memo;
      last.va = from.va;
      std::memcpy(last.device, from.device, sizeof(last.device));
      std::memcpy(last.header, from.header, sizeof(last.header));
      last.frame = from.frame;
      last.epoch = from.epoch;
      last.depth_epoch = from.depth_epoch;
      last.fx = from.fx;
      last.layout_epoch = g_layout_epoch;  // its transitions were just applied (round 4)
      last.obj = nullptr;
      last.obj_fx = nullptr;  // banjo-nx (2026-10-28)
      // banjo-nx (2026-10-27, Phase A): the answer into the slot's texture object too (when its header is the one the
      // answer was made from).
      if (obj && memo && SameBytes<24>(obj->header, from.header)) {
        StoreTextureObject(*obj, fetch, texture_va, from.dim, from.index, from.sampler, from.fx, from.frame);
        last.obj = obj;
      }
    };
    if (memo && m.frame == TextureFrameLocked() && m.va == texture_va && m.epoch == BindingEpoch(m.fx, fetch, texture_va) &&
        (!m.fx.depth_source || m.depth_epoch == g_depth_epoch) &&
        SameBytes<24>(m.device, fetch.dword) &&
        SameBytes<24>(m.header, &header->Format) &&
        !(m.fx.handover_surface && HandOverSourceIsDrawTarget(m.fx.handover_surface))) {
      // banjo-nx (2026-10-23): the call only when it can change something (it returns at once for a texture in the
      // layout already).
      for (HostTexture *t : m.fx.transition)
        if (t && t->layout != plume::RenderTextureLayout::SHADER_READ)
          TransitionTextureLocked(t, plume::RenderTextureLayout::SHADER_READ);
      // banjo-nx (2026-10-19, banjo_deko3d_depth_handover): the target image it was sampled through.
      if (m.fx.handover_surface)
        TransitionSurfaceLocked(m.fx.handover_surface, plume::RenderTextureLayout::SHADER_READ);
      if (m.fx.handover_image)
        TransitionDepthImageLocked(*m.fx.handover_image, plume::RenderTextureLayout::SHADER_READ);
      if (m.fx.float_sampled)
        banjo::renut_gpu::CountDepthFloatSampled();
      if (m.fx.words_viewed)
        banjo::renut_gpu::CountDepthWords(true);
      if (!texture_va)  // sasrt-nx (2026-10-29): sasrt_renut_texture_memo_fetch_only_verify
        VerifyFetchOnlyAnswer(fetch, m.dim, m.index, m.sampler);
      SetSlotWords(sc, r, m.dim, m.index, m.sampler);  // banjo-nx (2026-10-26): every word of the slot
      if (slot_fx)
        slot_fx[r] = m.fx;
      keep_last(m);
      ++g_tex_memo;
      continue;
    }
    // banjo-nx (2026-10-27, render thread night, A/B 32): the entry of an earlier frame, all else the same: only the
    // frame's part of the bind (RefreshBindingLocked: the content check), not the bind's lookups again (340 binds a frame
    // in the setters2 city run went through BindTextureLocked for this alone).
    if (memo && m.va == texture_va && m.frame != TextureFrameLocked() && m.epoch == BindingEpoch(m.fx, fetch, texture_va) &&
        !m.fx.depth_source && SameBytes<24>(m.device, fetch.dword) &&
        SameBytes<24>(m.header, &header->Format) &&
        !(m.fx.handover_surface && HandOverSourceIsDrawTarget(m.fx.handover_surface)) &&
        banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 32 &&
        RefreshBindingLocked(texture_va, fetch, m.fx)) {
      m.frame = TextureFrameLocked();
      for (HostTexture *t : m.fx.transition)
        if (t && t->layout != plume::RenderTextureLayout::SHADER_READ)
          TransitionTextureLocked(t, plume::RenderTextureLayout::SHADER_READ);
      if (m.fx.float_sampled)
        banjo::renut_gpu::CountDepthFloatSampled();
      SetSlotWords(sc, r, m.dim, m.index, m.sampler);
      if (slot_fx)
        slot_fx[r] = m.fx;
      keep_last(m);
      ++g_tex_refresh;
      continue;
    }
    ++g_tex_bind;
    if (memo) {
      if (m.va != texture_va || !SameBytes<24>(m.device, fetch.dword))
        ++g_tex_miss_other;
      else if (m.frame != TextureFrameLocked())
        ++g_tex_miss_frame;
      else if (m.epoch != BindingEpoch(m.fx, fetch, texture_va))
        ++g_tex_miss_epoch;
      else if (m.fx.depth_source && m.depth_epoch != g_depth_epoch)
        ++g_tex_miss_depth;
      else
        ++g_tex_miss_words;
    }
    BindEffects fx;
    const TextureBinding binding = BindTextureLocked(texture_va, fetch, &fx);
    if (slot_fx)
      slot_fx[r] = fx;
    u32 dim, index, sampler;
    BindingWords(binding, fetch, dim, index, sampler);
    SetSlotWords(sc, r, dim, index, sampler);  // banjo-nx (2026-10-26)
    if (memo) {
      m.va = texture_va;
      std::memcpy(m.device, fetch.dword, sizeof(m.device));
      std::memcpy(m.header, &header->Format, sizeof(m.header));
      m.frame = TextureFrameLocked();
      m.fx = fx;
      m.epoch = BindingEpoch(fx, fetch, texture_va);  // after the bind (its own bumps)
      m.depth_epoch = g_depth_epoch;
      m.dim = dim;
      m.index = index;
      m.sampler = sc.sampler_indices[r];
      m.fx = fx;
      keep_last(m);
    } else if (persistent) {
      last.valid = false;
      last.obj = nullptr;  // banjo-nx (2026-10-27, Phase A)
      last.obj_fx = nullptr;  // banjo-nx (2026-10-28)
    }
  }
  // banjo-nx (2026-10-23, batch 6): see same_as_last. The slots outside the mask hold the defaults in both.
  static u32 last_mask = ~0u;
  static u32 last_words[d3d::kTextureFetchSlots][4];
  bool same = fetch_mask == last_mask;
  for (u32 bits = fetch_mask; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    const u32 words[4] = {sc.texture_indices[0][r], sc.texture_indices[1][r], sc.texture_indices[2][r],
                          sc.sampler_indices[r]};
    same = same && std::memcmp(words, last_words[r], sizeof(words)) == 0;
    std::memcpy(last_words[r], words, sizeof(words));
  }
  last_mask = fetch_mask;
  ++g_texture_fills;
  if (same_as_last)
    *same_as_last = same;
}

inline void SetSlotDefaults(SharedConstants &sc, u32 r) {
  SetSlotWords(sc, r, 3, 0, SamplerWord(kLinearClampSamplerSlot));
}
inline u32 last_dim(const SharedConstants &sc, u32 r) {
  return sc.texture_indices[1][r] != kNullTexture3DSlot ? 1u : sc.texture_indices[2][r] != kNullTextureCubeSlot ? 2u : 0u;
}

// banjo-nx (2026-10-26, banjo_renut_persistent_shared): the persistent block, made at the first draw as a new one.
// banjo-nx (2026-10-28, per-draw night): the test inline and the set-up out of line: a call at every draw before, and,
// the block coming back by reference from a call, every mark of its fields worked its offset and lines out at run time
// (inline, the fast path's marks are constants).
[[gnu::cold, gnu::noinline]] void InitDrawSharedLocked() {
  std::memset(&g_draw_sc, 0, sizeof(g_draw_sc));
  for (u32 r = 0; r < 32; ++r)
    SetSlotDefaults(g_draw_sc, r);
  g_draw_sc.flt_max = std::numeric_limits<float>::max();
  g_draw_sc_track = {};
  g_draw_sc_track.ready = true;
  g_sc_dirty = kAllSharedLines;
}
[[gnu::always_inline]] inline SharedConstants &DrawSharedLocked() {
  if (!g_draw_sc_track.ready) [[unlikely]]
    InitDrawSharedLocked();
  return g_draw_sc;
}

// banjo-nx (2026-10-26, banjo_renut_persistent_shared_verify): the words of the persistent block the draw did not set,
// as a new block would hold them. 'texture_mask', 'fetch_mask', 'bound_entries': what the draw set.
bool g_cp_fetch_on = false;  // vivapinata-nx (2026-11-03): this draw is a tessellated list with its indices' fetch (ControlPointFetchMask)
void VerifyDrawShared(SharedConstants &sc, u32 texture_mask, u32 fetch_mask, u32 bound_entries, bool exports,
                      bool point, bool tessellation, bool skin) {
  u32 wrong = 0;
  for (u32 r = 0; r < 32; ++r) {
    if (r < d3d::kTextureFetchSlots && (texture_mask >> r) & 1)
      continue;
    const u32 sampler = SamplerWord(kLinearClampSamplerSlot);
    if (sc.texture_indices[0][r] != kNullTextureSlot || sc.texture_indices[1][r] != kNullTexture3DSlot ||
        sc.texture_indices[2][r] != kNullTextureCubeSlot || sc.sampler_indices[r] != sampler) {
      ++wrong;
      SetSlotDefaults(sc, r);
    }
  }
  for (u32 i = 0; i < 96; ++i) {
    if (i >= 64 && ((fetch_mask >> (i - 64)) & 1))
      continue;
    if (sc.vertex_fetch_slots[i]) {
      ++wrong;
      sc.vertex_fetch_slots[i] = 0;
    }
  }
  for (u32 i = 0; i < 16; ++i) {
    if ((bound_entries >> i) & 1)
      continue;
    for (u32 k = 0; k < 4; ++k) {
      if (sc.bound_fetch[i][k]) {
        ++wrong;
        sc.bound_fetch[i][k] = 0;
      }
    }
  }
  auto zero = [&wrong](void *p, size_t n) {
    const u8 *b = static_cast<const u8 *>(p);
    for (size_t i = 0; i < n; ++i) {
      if (b[i]) {
        ++wrong;
        std::memset(p, 0, n);
        return;
      }
    }
  };
  if (!exports)
    zero(sc.memexport_streams, sizeof(sc.memexport_streams));
  for (u32 i = 0; i < 4; ++i)
    if (exports && sc.memexport_streams[i][3])
      zero(&sc.memexport_streams[i][3], 4);
  if (!point)
    zero(sc.point_state, sizeof(sc.point_state));
  if (!tessellation)
    zero(sc.tessellation, sizeof(u32) * 3);
  else if (!g_cp_fetch_on)  // vivapinata-nx: a tessellated list's word 3 names its indices' fetch (renutControlPoints)
    zero(&sc.tessellation[3], sizeof(u32));
  if (!skin)
    zero(&sc.pad_1352[0], sizeof(float));
  if (sc.flt_max != std::numeric_limits<float>::max()) {
    ++wrong;
    sc.flt_max = std::numeric_limits<float>::max();
  }
  if (wrong) {
    g_sc_dirty = kAllSharedLines;  // put right: pushed whole
    static uint32_t logged = 0;
    if (logged++ < 16)
      RGPU_WARN("banjo_renut_persistent_shared_verify: {} words of the kept SharedConstants differ from a new block's",
                wrong);
  }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Geometry {
  bool indexed = false;
  u32 index_count = 0;
  plume::RenderIndexBufferView index_view;  // indices from first_vertex
  u32 vertex_count = 0;  // non-indexed host draw count
  u32 first_vertex = 0;  // first guest vertex converted
  u32 vertex_range = 0;  // vertices converted per stream
  // Tessellated draws: one instance per patch, the streams per instance.
  u32 instance_count = 1;
  bool per_instance = false;
  // banjo-nx (2026-10-26, API-level stage 2): drawn from whole-buffer copies (WholeGeometryLocked): the indices the game's
  // own, drawn from first_index; a non-indexed draw from vertex 0. The streams' copies hold vertex 0 on and are bound
  // from whole_base (the base vertex, or the first vertex of a non-indexed draw), so the host draws with no base vertex
  // and its vertex index plus whole_base (SharedConstants::tessellation[3]) is the guest's, as for the range copies:
  // whether the GPU's vertex index includes a draw's base vertex never matters (the console showed it does not where the
  // emulator did: grass drawn from the wrong vertices, 2026-10-26).
  bool whole = false;
  u32 first_index = 0;
  i32 base_vertex = 0;
  u32 whole_base = 0;
};

// Reads 'count' guest indices into g_indices.
bool ReadIndices(const u8 *src, u32 count, bool is32, u32 endian) {
  if (!src || count > kMaxDrawIndices)
    return false;
  g_indices.resize(count);
  if (is32) {
    SwapEndian(endian, src, g_indices.data(), size_t(count) * 4);
  } else {
    const bool swap = endian == 1 || endian == 2;
    for (u32 i = 0; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, src + i * 2, 2);
      g_indices[i] = swap ? uint16_t((v >> 8) | (v << 8)) : v;
    }
  }
  return true;
}

// banjo-nx (2026-10-23, batch 6): 16-bit indices read into g_indices with their smallest and largest found in the same
// pass (NEON: eight at a time), as ReadIndices and the range loop that followed it give them.
bool ReadIndices16WithRange(const u8 *src, u32 count, u32 endian, u32 &lo, u32 &hi) {
  if (!src || count > kMaxDrawIndices || !count)
    return false;
  g_indices.resize(count);
  const bool swap = endian == 1 || endian == 2;
  u32 *out = g_indices.data();
  u32 i = 0;
  uint16_t mn = 0xFFFF, mx = 0;
#if defined(__aarch64__)
  uint16x8_t vmn = vdupq_n_u16(0xFFFF), vmx = vdupq_n_u16(0);
  for (; i + 8 <= count; i += 8) {
    uint16x8_t v = vld1q_u16(reinterpret_cast<const uint16_t *>(src + i * 2));
    if (swap)
      v = vreinterpretq_u16_u8(vrev16q_u8(vreinterpretq_u8_u16(v)));
    vmn = vminq_u16(vmn, v);
    vmx = vmaxq_u16(vmx, v);
    vst1q_u32(out + i, vmovl_u16(vget_low_u16(v)));
    vst1q_u32(out + i + 4, vmovl_high_u16(v));
  }
  mn = vminvq_u16(vmn);
  mx = vmaxvq_u16(vmx);
#endif
  for (; i < count; ++i) {
    uint16_t v;
    std::memcpy(&v, src + i * 2, 2);
    v = swap ? uint16_t((v >> 8) | (v << 8)) : v;
    out[i] = v;
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  lo = mn;
  hi = mx;
  return true;
}

// banjo-nx (2026-10-23, batch 6): g_indices - lo as 16-bit host indices (every one fits: max_index <= 0xFFFE), eight at
// a time with NEON.
void RebaseIndices16(uint16_t *out, u32 lo) {
  const u32 *in = g_indices.data();
  const size_t n = g_indices.size();
  size_t i = 0;
#if defined(__aarch64__)
  const uint32x4_t base = vdupq_n_u32(lo);
  for (; i + 8 <= n; i += 8) {
    const uint32x4_t a = vsubq_u32(vld1q_u32(in + i), base), b = vsubq_u32(vld1q_u32(in + i + 4), base);
    vst1q_u16(out + i, vcombine_u16(vmovn_u32(a), vmovn_u32(b)));
  }
#endif
  for (; i < n; ++i)
    out[i] = uint16_t(in[i] - lo);
}

// Rewrites g_indices for fans and quads as triangle lists.
void ExpandIndices(u32 primitive) {
  // banjo-nx (2026-10-19, render-thread proposal 20): a scratch vector kept between draws (swapped with g_indices, so
  // both keep their capacity), not a new one per fan or quad draw.
  static std::vector<u32> out;
  if (primitive == kTriangleFan) {
    const size_t n = g_indices.size() > 2 ? g_indices.size() - 2 : 0;
    out.resize(n * 3);
    for (size_t i = 2, o = 0; i < g_indices.size(); ++i, o += 3) {
      out[o] = g_indices[0];
      out[o + 1] = g_indices[i - 1];
      out[o + 2] = g_indices[i];
    }
    g_indices.swap(out);
  } else if (primitive == kQuadList) {
    const size_t n = g_indices.size() / 4;
    out.resize(n * 6);
    for (size_t i = 0, o = 0; i + 3 < g_indices.size(); i += 4, o += 6) {
      const u32 a = g_indices[i], b = g_indices[i + 1], c = g_indices[i + 2], d = g_indices[i + 3];
      out[o] = a;
      out[o + 1] = b;
      out[o + 2] = c;
      out[o + 3] = a;
      out[o + 4] = c;
      out[o + 5] = d;
    }
    g_indices.swap(out);
  }
}

// bangaio-nx (2026-10-29, banjo_renut_quad_indices, A/B 72): the indices ExpandIndices makes for a non-indexed quad
// list of up to kQuadIndexVertices vertices (0..count-1 expanded, rebased by 0, 16-bit as PrepareGeometry writes them
// below 0xFFFF), made once in the copy pool and never freed. A draw binds all of it (the same view for every quad draw,
// so the bind is kept) and draws its first count / 4 * 6.
constexpr u32 kQuadIndexVertices = 0xFFFF;  // max index 0xFFFE: PrepareGeometry's 16-bit limit
constexpr u32 kQuadIndexCount = kQuadIndexVertices / 4 * 6;
uint64_t g_quad_static_draws = 0;  // the 10 s line's

inline bool QuadIndicesOn() {
  static const bool on = REXCVAR_GET(banjo_renut_quad_indices);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 72;
}

// The render thread's (namespace scope, not function statics: no guard read at every quad draw on the console).
PoolSpan g_quad_span;
plume::RenderIndexBufferView g_quad_view;
uint64_t g_quad_retry_frame = 0;

// The buffer's view, or null while it cannot be made (tried again kQuadRetryFrames later).
const plume::RenderIndexBufferView *QuadIndexBuffer() {
  if (g_quad_span) [[likely]]
    return &g_quad_view;
  constexpr uint64_t kQuadRetryFrames = 600;
  if (g_stream_cache_frame < g_quad_retry_frame)
    return nullptr;
  constexpr u32 bytes = kQuadIndexCount * 2;
  PoolSpan &span = g_quad_span;
  span = g_copy_pool.Allocate(bytes);
  if (!span) {
    g_quad_retry_frame = g_stream_cache_frame + kQuadRetryFrames;
    return nullptr;
  }
  auto *out = reinterpret_cast<uint16_t *>(span.cpu);
  for (u32 q = 0, o = 0; o < kQuadIndexCount; ++q, o += 6) {
    const uint16_t a = uint16_t(q * 4);
    out[o] = a;
    out[o + 1] = uint16_t(a + 1);
    out[o + 2] = uint16_t(a + 2);
    out[o + 3] = a;
    out[o + 4] = uint16_t(a + 2);
    out[o + 5] = uint16_t(a + 3);
  }
  g_quad_view = plume::RenderIndexBufferView(span.Ref(), bytes, plume::RenderFormat::R16_UINT);
  return &g_quad_view;
}

// Guest index ranges kept across frames, rebased and in the host format, like
// the vertex streams (g_stream_cache): most draws read index buffers the game
// never rewrites. A range that hashes the same in kStableFlushes frames in a
// row gets a persistent copy, which draws use while the range still hashes the
// same, checked like the streams' (CacheCheckDue, off the render thread), and
// a range rewritten kMaxCacheDrops times is never cached again.
struct IndexKey {
  const u8 *src;
  u32 count, primitive, endian;
  bool is32;
  bool operator==(const IndexKey &) const = default;
};
struct IndexKeyHash {
  size_t operator()(const IndexKey &k) const {
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(k.src)) * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t(k.count) << 16 | k.primitive << 3 | k.endian << 1 | u32(k.is32)) +
         0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};
struct CachedIndices {
  uint64_t whole_seen = UINT64_MAX;  // banjo-nx (2026-10-26, stage 2): as CachedStream's
  uint64_t hash = 0;
  u32 stable = 0;  // frames in a row the range hashed the same
  u32 drops = 0;   // copies dropped because the guest range changed
  bool dynamic = false;
  PoolSpan copy;
  u32 bytes = 0, index_count = 0, lo = 0, max_index = 0;
  bool wide = false;
  bool whole = false;  // blur-nx (2026-11-18, #36): a whole buffer's entry (WholeIndexCopy), as CachedStream's
  uint64_t last_used = 0;               // g_stream_cache_frame
  uint64_t checked_frame = UINT64_MAX;  // frame the range was last hashed or checked in
  uint64_t proven_flush = UINT64_MAX;   // banjo-nx: g_flush_serial the copy was last hashed equal in
  uint64_t exact_frame = UINT64_MAX;    // banjo-nx: g_stream_cache_frame of its last exact check (repeat count)
};
using IndexCache = std::unordered_map<IndexKey, CachedIndices, IndexKeyHash>;
IndexCache g_index_cache;
EntryMemo<IndexCache, IndexKeyHash, 14> g_index_memo;  // banjo-nx: 16,384 slots (2026-10-23; 2,048 before)

uint64_t HashIndices(const IndexKey &key) {
  return banjo::renut_gpu::ContentHash64(key.src, size_t(key.count) * (key.is32 ? 4 : 2));  // banjo-nx
}

ContentVerifier<IndexKey, HashIndices> g_index_verifier;

void DropCachedIndices(CachedIndices &c) {
  if (!c.copy)
    return;
  if (c.whole || !::renut::gpu::host_batch::On(::renut::gpu::host_batch::kMeshGenWhole))  // blur-nx (2026-11-18, #36): as DropCachedCopy
    ++g_mesh_gen;  // banjo-nx (2026-10-27): mesh objects may name it
  g_stream_cache_bytes -= c.bytes;
  g_copy_pool.Free(c.copy);
  c.copy = {};
  c.bytes = 0;
}

// The draw's indices from the cache, if it has a copy of the guest range.
bool CachedIndexDraw(const IndexKey &key, const DrawCall &call, Geometry &g) {
  auto *entry = g_index_memo.Get(g_index_cache, key, false);
  if (!entry || !entry->second.copy)
    return false;
  CachedIndices &c = entry->second;
  c.last_used = g_stream_cache_frame;
  if (banjo::renut_gpu::ExactStreamsOn()) {
    // banjo-nx (2026-10-17): banjo_renut_exact_streams, as for streams. A changed range is dropped and the draw reads
    // it itself (TrackIndices hashes it again).
    if (c.proven_flush != g_flush_serial &&
        CardsProve(key.src, size_t(key.count) * (key.is32 ? 4 : 2), c.proven_flush, c.hash)) {
      c.proven_flush = g_flush_serial;  // banjo-nx: banjo_renut_store_cards
    } else if (c.proven_flush != g_flush_serial) {
      const bool same = ExactHash(key.src, size_t(key.count) * (key.is32 ? 4 : 2)) == c.hash;
      g_card_diag[7] += same && c.proven_flush != UINT64_MAX && banjo::renut_gpu::StoreCardsVerifyOn();
      banjo::renut_gpu::CountExactCheck(uint64_t(key.count) * (key.is32 ? 4 : 2), !same,
                                        c.exact_frame == g_stream_cache_frame);
      c.exact_frame = g_stream_cache_frame;
      if (!same) {
        DropCachedIndices(c);
        c.stable = 0;
        c.checked_frame = UINT64_MAX;
        c.dynamic = ++c.drops >= kMaxCacheDrops;
        return false;
      }
      c.proven_flush = g_flush_serial;
    }
  } else if (CacheCheckDue(c.checked_frame)) {
    c.checked_frame = g_stream_cache_frame;
    g_index_verifier.Submit(key, c.hash);
  }
  g.first_vertex = u32(int64_t(call.base_vertex) + c.lo);
  g.vertex_range = c.max_index + 1;
  g.indexed = true;
  g.index_count = c.index_count;
  g.index_view = plume::RenderIndexBufferView(
      c.copy.Ref(), c.bytes,
      c.wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  return true;
}

// After a draw read the range into g_indices: count how long it has held, and
// give a range that has held long enough its persistent copy.
void TrackIndices(const IndexKey &key, u32 lo, u32 max_index) {
  CachedIndices &c = g_index_memo.Get(g_index_cache, key, true)->second;
  c.last_used = g_stream_cache_frame;
  if (c.dynamic || c.copy || c.checked_frame == g_stream_cache_frame)
    return;
  c.checked_frame = g_stream_cache_frame;
  // banjo-nx (2026-10-23, batch 5): the cap first, as for streams: no hash of a range that gets no copy anyway.
  const bool wide = max_index > 0xFFFE;
  const u32 bytes = u32(g_indices.size()) * (wide ? 4 : 2);
  if (g_stream_cache_bytes + bytes > banjo::renut_gpu::StreamCacheMaxBytes(kStreamCacheMaxBytes)) {  // banjo-nx
    ++g_stream_cache_refused;
    c.stable = 0;
    return;
  }
  const uint64_t hash = HashIndices(key);
  c.stable = hash == c.hash ? c.stable + 1 : 0;
  c.hash = hash;
  if (c.stable + 1 < kStableFlushes)
    return;
  const PoolSpan copy = g_copy_pool.Allocate(bytes);
  if (!copy)
    return;
  if (wide) {
    auto *out = reinterpret_cast<u32 *>(copy.cpu);
    for (size_t i = 0; i < g_indices.size(); ++i)
      out[i] = g_indices[i] - lo;
  } else {
    RebaseIndices16(reinterpret_cast<uint16_t *>(copy.cpu), lo);  // banjo-nx (2026-10-23, batch 6)
  }
  c.copy = copy;
  c.bytes = bytes;
  c.index_count = u32(g_indices.size());
  c.lo = lo;
  c.max_index = max_index;
  c.wide = wide;
  g_stream_cache_bytes += bytes;
}

bool PrepareGeometry(const GuestDevice &device, const DrawCall &call, Geometry &g) {
  const bool expand = call.primitive == kTriangleFan || call.primitive == kQuadList;
  static uint32_t rect_logged = 0;
  if (call.primitive == kRectangleList && !State().rectangle_list_gs && LogLimited(rect_logged, 1))
    RGPU_WARN("Rectangle lists are drawn as triangle lists");

  u32 lo = 0, max_index = 0;  // host indices are g_indices - lo, the largest max_index
  IndexKey index_key{};
  bool cacheable_indices = false;
  // rr6-nx (2026-11-03, rr6_strip_markers): the strip's restart markers among g_indices, and the guest's marker value.
  const bool strip = !expand && (call.primitive == kLineStrip || call.primitive == kTriangleStrip);
  u32 markers = 0, guest_marker = 0xFFFFu;
  if (call.indexed) {
    const u8 *src = nullptr;
    bool is32 = false;
    u32 endian = 0;
    if (call.up_indices) {
      is32 = call.up_indices_32;
      endian = is32 ? 2 : 1;
      src = call.up_indices + size_t(call.start_index) * (is32 ? 4 : 2);
    } else {
      const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
      if (!ib)
        return false;
      const u32 common = ib->resource.Common;
      is32 = (common & d3d::kIndexBufferCommon32Bit) != 0;
      endian = d3d::IndexBufferEndian(common);
      const u8 *base = AddressPtr(ib->Address);
      if (!base)
        return false;
      src = base + size_t(call.start_index) * (is32 ? 4 : 2);
      // Draw*IndexedPrimitiveUP indices are the game's own memory, only valid
      // during the call; these are an index buffer's.
      index_key = IndexKey{src, call.count, call.primitive, endian, is32};
      cacheable_indices = call.count <= kMaxDrawIndices;
      if (cacheable_indices && CachedIndexDraw(index_key, call, g))
        return true;
    }
    guest_marker = is32 ? 0xFFFFFFFFu : 0xFFFFu;
    u32 hi = 0;
    if (!is32 && !expand) {
      // banjo-nx (2026-10-23, batch 6): read and ranged in one NEON pass (ReadIndices16WithRange).
      if (!ReadIndices16WithRange(src, call.count, endian, lo, hi))
        return false;
    } else {
      if (!ReadIndices(src, call.count, is32, endian))
        return false;
      if (expand)
        ExpandIndices(call.primitive);
      if (g_indices.empty())
        return false;
      lo = UINT32_MAX;
      for (u32 index : g_indices) {
        lo = std::min(lo, index);
        hi = std::max(hi, index);
      }
    }
    // rr6-nx (2026-11-03, rr6_strip_markers): a strip's restart marker (the all-ones index of the guest format; plume restarts
    // indexed strips at the host format's all-ones on every back end) is not a vertex. Taken into the range it made every strip
    // with a cut read vertex 65535: the range became the buffer's tail, the host indices 32-bit, and 0xFFFF in a 32-bit index
    // buffer restarts nothing on deko3d or Vulkan: the strip ran on through the cut (torn meshes, wedges). D3D12's cut value is
    // 0xFFFF whatever the width, so the PC never showed it. The markers leave the range and become the host format's below.
    if (strip && hi == guest_marker && banjo::renut_gpu::StripMarkersOn()) {
      lo = UINT32_MAX;
      hi = 0;
      for (const u32 index : g_indices) {
        if (index == guest_marker) {
          ++markers;
          continue;
        }
        lo = std::min(lo, index);
        hi = std::max(hi, index);
      }
      if (lo == UINT32_MAX)
        return false;  // markers only: nothing to draw
    }
    if (hi - lo >= kMaxDrawVertices)
      return false;
    g.first_vertex = u32(int64_t(call.base_vertex) + lo);
    g.vertex_range = hi - lo + 1;
    max_index = hi - lo;
  } else {
    if (!call.count || call.count > kMaxDrawVertices)
      return false;
    g.first_vertex = call.start_vertex;
    g.vertex_range = call.count;
    // bangaio-nx (2026-10-29, banjo_renut_quad_indices): a quad list's indices from the buffer made once. Under 4
    // vertices ExpandIndices leaves none (no draw, below).
    if (call.primitive == kQuadList && call.count >= 4 && call.count <= kQuadIndexVertices && QuadIndicesOn()) {
      if (const plume::RenderIndexBufferView *quads = QuadIndexBuffer()) {
        g.indexed = true;
        g.index_count = call.count / 4 * 6;
        g.index_view = *quads;
        ++g_quad_static_draws;
        return true;
      }
    }
    if (expand) {
      g_indices.resize(call.count);
      for (u32 i = 0; i < call.count; ++i)
        g_indices[i] = i;
      ExpandIndices(call.primitive);
      if (g_indices.empty())
        return false;
      max_index = call.count - 1;
    } else {
      g.vertex_count = call.count;
    }
  }

  if (call.indexed || expand) {
    // The host indices are rebased to the range rather than drawn with a base
    // vertex of -lo: D3D12's SV_VertexID leaves the base vertex out (Vulkan's
    // vertex index includes it), and vertex shaders that fetch by address
    // take the guest index from the vertex ID (renutVertexRegisters).
    // 16-bit when every index fits, like the guest's own format mostly is.
    const bool wide = max_index > 0xFFFE;
    const u32 bytes = u32(g_indices.size()) * (wide ? 4 : 2);
    UploadAllocation alloc = Video::UploadsLocked().Allocate(bytes, 16);
    if (!alloc)
      return false;
    // rr6-nx (2026-11-03, rr6_strip_markers): each marker becomes lo + the host marker, so the rebase below (index - lo, in
    // u32, then narrowed) leaves the host format's marker: 0xFFFF for 16-bit host indices (never a vertex: max_index is below
    // it); for 32-bit ones 0xFFFFFFFF, except on D3D12, whose cut value is 0xFFFF for every width. The cached copy
    // (TrackIndices) is rebased from the same g_indices.
    if (markers) {
      const u32 host_marker = wide && UsesVulkanBindings() ? 0xFFFFFFFFu : 0xFFFFu;
      for (u32 &index : g_indices)
        if (index == guest_marker)
          index = lo + host_marker;
    }
    if (wide) {
      auto *out = static_cast<u32 *>(alloc.cpu);
      for (size_t i = 0; i < g_indices.size(); ++i)
        out[i] = g_indices[i] - lo;
    } else {
      RebaseIndices16(static_cast<uint16_t *>(alloc.cpu), lo);  // banjo-nx (2026-10-23, batch 6)
    }
    g.indexed = true;
    g.index_count = u32(g_indices.size());
    g.index_view = plume::RenderIndexBufferView(
        alloc.ref, bytes, wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
    if (cacheable_indices)
      TrackIndices(index_key, lo, max_index);
  }
  return true;
}

// banjo-nx (2026-10-23, render-thread round): the index cache key PrepareGeometry will look up for this draw (an index
// buffer's indices, few enough to cache), for prefetching; false for any other draw.
bool DrawIndexKey(const GuestDevice &device, const DrawCall &call, IndexKey &key) {
  if (!call.indexed || call.up_indices || call.tessellation || call.count > kMaxDrawIndices)
    return false;
  const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
  if (!ib)
    return false;
  const u32 common = ib->resource.Common;
  const bool is32 = (common & d3d::kIndexBufferCommon32Bit) != 0;
  const u8 *base = AddressPtr(ib->Address);
  if (!base)
    return false;
  key = IndexKey{base + size_t(call.start_index) * (is32 ? 4 : 2), call.count, call.primitive,
                 d3d::IndexBufferEndian(common), is32};
  return true;
}

// banjo-nx (banjo_gpu_tess_grid, package 27): the index list of a tessellation grid, made once per kind and level: the
// triangles renutVertexRegisters' kinds 1 and 2 place, in their order, with their corners, each corner as its grid
// vertex's number (kinds 3 and 4 there). Render thread; the buffers live as long as the device.
struct TessellationGrid {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u32 index_count = 0;
  bool wide = false;
};
std::map<u32, TessellationGrid> g_tess_grids;  // key: triangles << 8 | level

const TessellationGrid *TessellationGridFor(bool triangles, u32 level) {
  TessellationGrid &grid = g_tess_grids[(triangles ? 1u : 0u) << 8 | level];
  if (grid.buffer)
    return &grid;
  std::vector<u32> indices;
  if (triangles) {
    auto number = [](u32 a, u32 b) { return a * (a + 1) / 2 + b; };
    for (u32 t = 0; t < level * level; ++t) {
      u32 row = u32(std::sqrt(double(t)));
      while (row * row > t)
        --row;
      while ((row + 1) * (row + 1) <= t)
        ++row;
      const u32 k = t - row * row, b = k / 2;
      if ((k & 1) == 0) {
        indices.insert(indices.end(), {number(row, b), number(row + 1, b), number(row + 1, b + 1)});
      } else {
        indices.insert(indices.end(), {number(row, b), number(row + 1, b + 1), number(row, b + 1)});
      }
    }
  } else {
    auto number = [level](u32 u, u32 v) { return v * (level + 1) + u; };
    for (u32 t = 0; t < 2 * level * level; ++t) {
      const u32 cell = t / 2, u = cell % level, v = cell / level;
      if ((t & 1) == 0)
        indices.insert(indices.end(), {number(u, v), number(u + 1, v), number(u + 1, v + 1)});
      else
        indices.insert(indices.end(), {number(u, v), number(u + 1, v + 1), number(u, v + 1)});
    }
  }
  const u32 points = triangles ? (level + 1) * (level + 2) / 2 : (level + 1) * (level + 1);
  grid.wide = points > 0xFFFF;
  const size_t bytes = indices.size() * (grid.wide ? 4 : 2);
  auto buffer = State().device->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(bytes, plume::RenderBufferFlag::INDEX));
  void *mapped = buffer ? buffer->map() : nullptr;
  if (!mapped)
    return nullptr;
  if (grid.wide) {
    std::memcpy(mapped, indices.data(), bytes);
  } else {
    auto *out = static_cast<uint16_t *>(mapped);
    for (size_t i = 0; i < indices.size(); ++i)
      out[i] = uint16_t(indices[i]);
  }
  buffer->unmap();
  buffer->setName(fmt::format("banjo-nx tessellation grid {} {}", triangles ? "triangles" : "quads", level));
  grid.buffer = std::move(buffer);
  grid.index_count = u32(indices.size());
  return &grid;
}

// Tessellated draws (DrawCall::tessellation): the patches become instances of a
// grid the vertex shader places (renutVertexRegisters in shader_common.h), at
// one level for the whole draw, the highest any patch edge asks for.
// vivapinata-nx (2026-11-03): a tessellated triangle list's index buffer, as this draw's extra fetch: its slot (the first of 64-95
// the vertex shader does not fetch from), set by PrepareTessellation for ControlPointFetchMask and UploadVertexFetchData.
struct ControlPointFetch {
  const u8 *src = nullptr;
  u32 size = 0;
  u32 endian = 0;
  u32 word = 0;  // SharedConstants.tessellation[3] without the slot (renutControlPoints, shader_common.h)
};
ControlPointFetch g_cp_fetch;
u32 g_cp_fetch_index = 0;

// The draw's fetch mask with the control points' slot. tessellation[3] gets the slot.
u32 ControlPointFetchMask(u32 mask, u32 tessellation[4]) {
  g_cp_fetch_index = 0;
  if (!g_cp_fetch.src)
    return mask;
  const u32 free_bits = ~mask;
  if (!free_bits) {
    tessellation[3] = 0;
    g_cp_fetch = {};
    return mask;
  }
  const u32 bit = u32(std::countr_zero(free_bits));
  g_cp_fetch_index = 64 + bit;
  g_cp_fetch_on = true;
  tessellation[3] = g_cp_fetch_index << 8 | g_cp_fetch.word;
  return mask | (1u << bit);
}

bool PrepareTessellation(const GuestDevice &device, const DrawCall &call, Geometry &g,
                         u32 tessellation[4]) {
  g_cp_fetch = {};
  g_cp_fetch_on = false;
  // vivapinata-nx (2026-11-03): each three indices of a tessellated triangle list are one patch. Its levels come from the
  // device as for a patch draw; an adaptive list has no known meaning (its index buffer would hold factors), so it stays undrawn.
  const u32 tess_mode = device.U32(d3d::kDeviceOffsetTessellationControl) & 3;
  const bool control_points = call.primitive == kTriangleList && tess_mode != 2;
  const bool triangles = call.primitive == kTrianglePatch || control_points;
  if (!triangles && call.primitive != kQuadPatch) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 8))
      RGPU_WARN("Tessellated primitive {} not drawn (vertex shader {:08X}, pixel shader {:08X}, count {}, control {:08X})",
                call.primitive, device.U32(d3d::kDeviceOffsetVertexShader), device.U32(d3d::kDeviceOffsetPixelShader),
                call.count, device.U32(d3d::kDeviceOffsetTessellationControl));
    return false;
  }
  if (control_points) {
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    const u8 *base = ib ? AddressPtr(ib->Address) : nullptr;
    if (!base || call.count < 3)
      return false;
    const bool wide = (ib->resource.Common & d3d::kIndexBufferCommon32Bit) != 0;
    const u32 width = wide ? 4 : 2;
    const u32 first_byte = call.start_index * width;
    g_cp_fetch.src = base + (first_byte & ~3u);
    g_cp_fetch.size = (((first_byte & 3u) + call.count * width) + 3u) & ~3u;
    g_cp_fetch.endian = d3d::IndexBufferEndian(ib->resource.Common);
    g_cp_fetch.word = (wide ? 2u : 1u) | ((first_byte & 3u) ? 4u : 0u);
    static uint32_t logged_lists = 0;
    if (LogLimited(logged_lists, 8))
      RGPU_INFO("[tess] triangle list: {} indices ({} wide, endian {}), mode {}, levels {} to {} (vertex shader {:08X}, pixel shader {:08X})",
                call.count, width, g_cp_fetch.endian, tess_mode, device.F32(d3d::kDeviceOffsetMinTessellationLevel),
                device.F32(d3d::kDeviceOffsetMaxTessellationLevel), device.U32(d3d::kDeviceOffsetVertexShader),
                device.U32(d3d::kDeviceOffsetPixelShader));
  }
  // Segments per edge are the tessellation level plus one.
  const float min_level = std::max(device.F32(d3d::kDeviceOffsetMinTessellationLevel), 0.0f) + 1.0f;
  const float max_level =
      std::max(device.F32(d3d::kDeviceOffsetMaxTessellationLevel) + 1.0f, min_level);
  float level = max_level;
  // The count is the patch count. (N&B's water: each section's patch stream is
  // sized 36 bytes x the count it draws, and its factor pass exports one
  // patch's edge factors per invocation.)
  const u32 patches = control_points ? call.count / 3 : call.count;  // vivapinata-nx: a list's patches are three indices each
  if ((device.U32(d3d::kDeviceOffsetTessellationControl) & 3) == 2) {
    // Adaptive: the index buffer holds each patch's edge factors (floats), from
    // the start index on. When a memexport draw in flight writes them, these
    // are from a frame or two before (DrawNeedsExportFlush does not wait for
    // them).
    const u32 edges = triangles ? 3 : 4;
    // Without them, the maximum level.
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    const u8 *base = ib ? AddressPtr(ib->Address) : nullptr;
    if (base && ReadIndices(base + size_t(call.start_index) * 4, patches * edges, true,
                            d3d::IndexBufferEndian(ib->resource.Common))) {
      float highest = 0.0f;
      for (u32 bits : g_indices) {
        float factor;
        std::memcpy(&factor, &bits, 4);
        if (factor > highest && factor < 1024.0f)
          highest = factor;
      }
      level = std::clamp(highest + 1.0f, min_level, max_level);
    }
  }
  if (!patches || patches > kMaxDrawVertices)
    return false;
  constexpr u32 kMaxTessellatedVertices = 1u << 22;
  u32 segments = std::clamp(u32(std::ceil(level)), 1u, 64u);
  const u32 per_segment = triangles ? 3 : 6;  // vertices per segment^2
  while (segments > 1 &&
         uint64_t(patches) * segments * segments * per_segment > kMaxTessellatedVertices)
    --segments;

  g.vertex_count = segments * segments * per_segment;
  g.instance_count = patches;
  g.per_instance = true;
  g.first_vertex = u32(call.base_vertex);
  g.vertex_range = patches;
  tessellation[0] = triangles ? 1 : 2;
  tessellation[1] = segments;
  tessellation[2] = u32(call.base_vertex);
  // banjo-nx (banjo_gpu_tess_grid): the same triangles, indexed by grid vertex.
  if (banjo::renut_gpu::TessGridOn()) {
    if (const TessellationGrid *grid = TessellationGridFor(triangles, segments)) {
      g.indexed = true;
      g.index_count = grid->index_count;
      g.index_view = plume::RenderIndexBufferView(plume::RenderBufferReference(grid->buffer.get()),
                                                  grid->index_count * (grid->wide ? 4 : 2),
                                                  grid->wide ? plume::RenderFormat::R32_UINT
                                                             : plume::RenderFormat::R16_UINT);
      tessellation[0] = triangles ? 3 : 4;
      banjo::renut_gpu::CountTessGrid(uint64_t(patches) * g.vertex_count,
                                      uint64_t(patches) * (triangles ? (segments + 1) * (segments + 2) / 2
                                                                     : (segments + 1) * (segments + 1)));
    }
  }
  return true;
}

struct StreamSource {
  const u8 *data = nullptr;  // guest vertex 0 of the stream
  u32 stride = 0;
  u32 endian = 0;
  u32 vertex_count = 0;      // vertices available from data
  u32 cpu_address = 0;       // guest VA of data, when read through the vertex buffer
  bool held = false;         // sasrt-nx (sasrt_renut_exports_held): data is a kept export's, not guest memory
};

// 'shader_strides': the strides in the vertex shader's microcode when the draw
// fetches as the shader was bound (or baked), with no declaration set on the
// device. The GPU uses those, whatever SetStreamSource was given; D3D patches
// the stream strides in only when it binds the shader to a device
// declaration.
StreamSource GetStreamSource(const GuestDevice &device, const DrawCall &call,
                             const ShaderRecord &vs, const u32 *shader_strides, u32 stream) {
  StreamSource src;
  if (stream == 0 && call.up_vertices) {
    src.data = call.up_vertices;
    src.stride = call.up_stride;
    src.endian = kUpEndian;
    src.vertex_count = call.up_vertex_count;
    return src;
  }
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
  const u32 w0 = device.VertexFetchWord(fetch_index, 0);
  const u32 w1 = device.VertexFetchWord(fetch_index, 1);
  if (shader_strides)
    src.stride = shader_strides[stream];
  if (!src.stride)
    src.stride = u32(device.U8(d3d::kDeviceOffsetStreamStrides + stream)) * 4;
  if (!src.stride)
    src.stride = vs.bound_strides[stream];  // SetStreamSource(..., 0): the Bind stride
  if (!src.stride)
    src.stride = vs.fetch_strides[stream];  // or the one the shader's vfetch encodes
  src.endian = w1 & 3;
  const u32 size = w1 & 0x3FFFFFCu;
  const u32 gpu_address = w0 & ~3u;
  src.data = gpu_address ? GuestPhysicalPtr<const u8>(gpu_address) : nullptr;
  // Read through the vertex buffer's CPU address, where the game wrote it,
  // when the fetch constant is the one SetStreamSource made from that buffer
  // (its GPU address plus the stream offset).
  // banjo-nx (2026-10-27, Phase A): the header's two words as the recording took them for a replayed draw (no guest
  // header read here); the header itself otherwise.
  u32 vb_words[2];
  bool have_vb = false;
  if (const DrawResources *res = g_draw_resources; res && stream < 16 && ((res->vb_mask >> stream) & 1)) {
    vb_words[0] = res->vb[stream][0];
    vb_words[1] = res->vb[stream][1];
    have_vb = true;
    static const bool vb_verify = REXCVAR_GET(banjo_renut_resource_objects_verify);
    if (vb_verify) [[unlikely]] {
      const auto *g = GuestPtr<const d3d::VertexBuffer>(device.U32(d3d::kDeviceOffsetStreamSources + stream * 4));
      if (!g || u32(g->Format.dword[0]) != vb_words[0] || u32(g->Format.dword[1]) != vb_words[1])
        ++g_ib_verify_differ;  // (the index and vertex buffer headers' count)
    }
  } else if (const auto *vb = GuestPtr<const d3d::VertexBuffer>(
                 device.U32(d3d::kDeviceOffsetStreamSources + stream * 4))) {
    vb_words[0] = u32(vb->Format.dword[0]);
    vb_words[1] = u32(vb->Format.dword[1]);
    have_vb = true;
  }
  if (have_vb) {
    const u32 cpu_base = vb_words[0] & ~3u;
    const u32 gpu_base = d3d::CpuToGpuAddress(cpu_base);
    if (cpu_base >= 0x20000000u && gpu_address >= gpu_base &&
        gpu_address - gpu_base < (vb_words[1] & 0x3FFFFFCu)) {
      src.cpu_address = cpu_base + (gpu_address - gpu_base);
      src.data = GuestPtr<const u8>(src.cpu_address);
    }
  }
  // sasrt-nx (2026-10-21, sasrt_renut_exports_held): inside a written-back export: its results as the GPU made them.
  if (gpu_address && !g_held_exports.empty() && banjo::renut_gpu::ExportsHeldOn()) {
    const u32 phys = gpu_address & 0x1FFFFFFFu;
    auto it = g_held_exports.upper_bound(phys);
    if (it != g_held_exports.begin()) {
      --it;
      const size_t held = it->second.data->size(), off = phys - it->first;
      if (off < held && size <= held - off &&
          !HeldExportOverwritten(it->first, it->second, src.stride)) {  // renut-ultimate (audit, 2026-11-07)
        src.data = it->second.data->data() + off;
        src.held = true;
        ++g_held_reads;
      }
    }
  }
  src.vertex_count = src.stride ? size / src.stride : (size ? 1 : 0);
  return src;
}

// Host vertices of the draw's streams built on the GPU from export windows
// (ConvertExportedStreamsLocked), from its first vertex; 'bytes' 0 for the
// others.
struct GpuStreams {
  plume::RenderBufferReference ref[kMaxVertexStreams];
  u32 bytes[kMaxVertexStreams] = {};
};

// banjo-nx (2026-10-18, the deko3d water): banjo_renut_diag_stream_dump_vs names a vertex shader (hex hash); its draws'
// streams are logged (first 16): the guest source, the first two vertices as the guest has them and as converted for
// the host, and whether a cached copy is bound.
bool StreamDumpLog(const ShaderRecord &vs, u32 stream, const StreamSource &src, const HostVertexStream &hs,
                   const u8 *guest, u32 first, u32 count, const DrawCall &call) {
  static const uint64_t want = [] {
    const std::string v = REXCVAR_GET(banjo_renut_diag_stream_dump_vs);
    return v.empty() ? 0ull : uint64_t(std::strtoull(v.c_str(), nullptr, 16));
  }();
  if (!want || vs.hash != want)
    return false;
  static uint32_t seen = 0;
  if (seen++ >= 16 && seen % 64 != 0)  // the first 16, then every 64th
    return false;
  std::string guest_words, host_words;
  if (guest && count) {
    const u32 n = std::min<u32>(src.stride / 4, 16);
    for (u32 v = 0; v < std::min<u32>(count, 2); ++v) {
      for (u32 i = 0; i < n; ++i) {
        u32 w;
        std::memcpy(&w, guest + size_t(v) * src.stride + i * 4, 4);
        guest_words += fmt::format("{:08X} ", w);
      }
      guest_words += "| ";
    }
    std::vector<u8> host(size_t(hs.host_stride) * 2 + 16);
    ConvertVertices(hs, guest, src.stride, std::min<u32>(count, 2), src.endian, host.data());
    for (u32 i = 0; i < std::min<u32>(hs.host_stride * 2 / 4, 32); ++i) {
      u32 w;
      std::memcpy(&w, host.data() + i * 4, 4);
      host_words += fmt::format("{:08X} ", w);
    }
  }
  RGPU_INFO("[streamdump] vs {:016X} stream {} cpu {:08X} stride {} host stride {} endian {} first {} count {} prim {} "
            "draw count {}: guest {}host {}",
            vs.hash, stream, src.cpu_address, src.stride, hs.host_stride, src.endian, first, count, call.primitive,
            call.count, guest_words, host_words);
  return true;
}

// banjo-nx (2026-10-20, banjo_deko3d_skin_once): the fur's vertex shaders (tools/skinonce_shaders.txt), and what the
// last BindStreams bound: FNV-1a of each stream's GPU address, size, stride and slot (FurSkinLocked's key: a draw that
// binds the same vertex data has the same hash within a frame, the upload ring never handing out a range twice).
// (tools/skinonce_shaders.txt, sorted: the pass gives these constant 913; each checked by tools/difftest/
// difftest_vs_skin.py.)
constexpr uint64_t kSkinOnceVs[] = {
    // the fur (package 36g). 2026-10-20: the shadow maps' skinned vertex shaders were tried and taken out (emulator run
    // 109: one draw in five read the rows the others wrote; docs/deko3d-renderer.md section 22).
    0x12399EE21A0440AEull, 0x146DA1F10B7D746Eull, 0x7F73E48B8EA43715ull, 0xB814D7237E615990ull,
};
bool IsFurVs(uint64_t hash) {
  return hash == 0x12399EE21A0440AEull || hash == 0x146DA1F10B7D746Eull || hash == 0x7F73E48B8EA43715ull ||
         hash == 0xB814D7237E615990ull;
}
bool IsSkinOnceVs(uint64_t hash) {
  static_assert(std::is_sorted(std::begin(kSkinOnceVs), std::end(kSkinOnceVs)));
  return std::binary_search(std::begin(kSkinOnceVs), std::end(kSkinOnceVs), hash);
}
// banjo-nx (2026-10-23, render-thread round): the bindings themselves, hashed only when FurSkinLocked asks
// (BoundStreamsHash): the hash per binding was an FNV-1a of 24 bytes a byte at a time and a virtual call for the
// address, at every stream of every draw, for the fur's few draws.
struct BoundStreamRecord {
  const plume::RenderBuffer *buffer;
  uint64_t offset;
  uint64_t size_stride;  // size << 32 | stride
  uint64_t slot;         // index << 8 | classification
};
BoundStreamRecord g_bound_stream_list[kMaxVertexStreams];
u32 g_bound_stream_count = 0;
// The last UploadVertexFetchData's copies (guest source, size, endian a fetch constant), for FurSkinLocked.
struct FetchRecordCopy {
  u32 index = 0;
  const u8 *src = nullptr;
  u32 size = 0;
  u32 endian = 0;
  bool operator==(const FetchRecordCopy &o) const {
    return index == o.index && src == o.src && size == o.size && endian == o.endian;
  }
};
struct FetchRecord {
  u32 count = 0;
  bool unusable = false;  // a copy from a memexport window, or DrawUP vertices
  FetchRecordCopy copies[32];
};
FetchRecord g_fetch_record;

inline uint64_t HashMix(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i, v >>= 8)
    h = (h ^ (v & 0xFF)) * 1099511628211ull;
  return h;
}

void BindHashed(plume::RenderCommandList *cmd, const plume::RenderVertexBufferView &view,
                const plume::RenderInputSlot &slot) {
  if (g_bound_stream_count < kMaxVertexStreams) {
    g_bound_stream_list[g_bound_stream_count++] = {view.buffer.ref, view.buffer.offset,
                                                   uint64_t(view.size) << 32 | slot.stride,
                                                   uint64_t(slot.index) << 8 | uint64_t(slot.classification)};
  }
  SetVertexBuffer(cmd, view, slot);
}

// FNV-1a of each stream the last BindStreams bound: its GPU address, size, stride and slot (as computed at each bind
// before 2026-10-23).
uint64_t BoundStreamsHash() {
  uint64_t h = 1469598103934665603ull;
  for (u32 i = 0; i < g_bound_stream_count; ++i) {
    const BoundStreamRecord &r = g_bound_stream_list[i];
    const uint64_t address = r.buffer ? r.buffer->getDeviceAddress() + r.offset : 0;
    h = HashMix(HashMix(HashMix(h, address), r.size_stride), r.slot);
  }
  return h;
}

// ---------------------------------------------------------------------------
// banjo-nx (2026-10-26, API-level stage 2, banjo_renut_whole_buffers): Unleashed binds a buffer the game made, whole,
// and draws with the game's first index and base vertex. N&B's buffers are its own memory (no Create or Lock: the
// Unleashed census), so a whole buffer's copy is made once the store cards show it unwritten for kWholeStableFlushes
// kicks, and proven at each kick's first use as the range copies are (or dropped, and the draw takes the ranges).
// ---------------------------------------------------------------------------
constexpr uint64_t kWholeStableFlushes = 3;
constexpr u32 kWholeIndexPrimitive = 0x80000000u;  // a whole index buffer's key (not a draw's range)

// A copy proven for this kick (the range copies' rule: the cards, or the exact hash); false when it is gone.
bool ProveStreamCopy(const StreamKey &key, CachedStream &c) {
  if (!c.copy)
    return false;
  if (banjo::renut_gpu::ExactStreamsOn()) {
    if (c.proven_flush != g_flush_serial &&
        CardsProve(key.guest, size_t(key.count) * key.stride, c.proven_flush, c.hash)) {
      c.proven_flush = g_flush_serial;
    } else if (c.proven_flush != g_flush_serial) {
      const bool same = ExactHash(key.guest, size_t(key.count) * key.stride) == c.hash;
      banjo::renut_gpu::CountExactCheck(uint64_t(key.count) * key.stride, !same,
                                        c.exact_frame == g_stream_cache_frame);
      c.exact_frame = g_stream_cache_frame;
      if (!same) {
        DropCachedCopy(c);
        c.stable = 0;
        c.checked_frame = UINT64_MAX;
        c.whole_seen = UINT64_MAX;
        c.dynamic = ++c.drops >= kMaxCacheDrops;
        return false;
      }
      c.proven_flush = g_flush_serial;
    }
  } else if (CacheCheckDue(c.checked_frame)) {
    c.checked_frame = g_stream_cache_frame;
    g_stream_verifier.Submit(key, c.hash);
  }
  return true;
}

bool ProveIndexCopy(const IndexKey &key, CachedIndices &c) {
  if (!c.copy)
    return false;
  const size_t bytes = size_t(key.count) * (key.is32 ? 4 : 2);
  if (banjo::renut_gpu::ExactStreamsOn()) {
    if (c.proven_flush != g_flush_serial && CardsProve(key.src, bytes, c.proven_flush, c.hash)) {
      c.proven_flush = g_flush_serial;
    } else if (c.proven_flush != g_flush_serial) {
      const bool same = ExactHash(key.src, bytes) == c.hash;
      banjo::renut_gpu::CountExactCheck(bytes, !same, c.exact_frame == g_stream_cache_frame);
      c.exact_frame = g_stream_cache_frame;
      if (!same) {
        DropCachedIndices(c);
        c.stable = 0;
        c.checked_frame = UINT64_MAX;
        c.whole_seen = UINT64_MAX;
        c.dynamic = ++c.drops >= kMaxCacheDrops;
        return false;
      }
      c.proven_flush = g_flush_serial;
    }
  } else if (CacheCheckDue(c.checked_frame)) {
    c.checked_frame = g_stream_cache_frame;
    g_index_verifier.Submit(key, c.hash);
  }
  return true;
}

// Whether a whole range is due its copy: unwritten (the cards) since 'seen', kWholeStableFlushes kicks ago or more;
// 'seen' restarts when it was written.
bool WholeRangeHeld(const u8 *data, size_t bytes, uint64_t &seen) {
  if (seen == UINT64_MAX || !PagesClean(data, bytes, seen)) {
    seen = g_flush_serial;
    return false;
  }
  return g_flush_serial >= seen + kWholeStableFlushes;
}

// ---------------------------------------------------------------------------
// banjo-nx (2026-10-27, the user's "copies" item, banjo_renut_fetch_cache / A/B 47): the fetch cache. The buffers
// vertex shaders fetch by address (UploadVertexFetchData: bone palettes, instance data; about 400 copies of 8 KB a
// frame in the city) were copied from guest memory into the upload ring at every kick (FlushVertexFillsLocked: the
// bulk memcpy, ~5 % of the console's render thread in b35). Here they get the whole-buffer copies' rules: a range
// unwritten for kWholeStableFlushes kicks gets a copy in the stream cache's pool, proven at its first use in each kick
// interval by the store cards (else the exact hash; changed: dropped, and after kMaxCacheDrops never kept again). A
// draw whose fetch buffers are all kept in one pool chunk binds that chunk as its fetch data; any other draw copies as
// before. Entries are direct-mapped by (source, size); an entry another key takes is dropped (its span freed when the
// frame slot drains, so a draw recorded before still reads it).
// ---------------------------------------------------------------------------
struct FetchPoolEntry {
  const u8 *src = nullptr;
  u32 size = 0;
  bool dynamic = false;
  u32 drops = 0;
  PoolSpan copy;
  uint64_t base = 0;                   // the chunk's device address (the draw's fetch data)
  uint64_t chunk_bytes = 0;            // and its size (the binding's)
  uint64_t seen = UINT64_MAX;          // WholeRangeHeld's
  uint64_t looked = UINT64_MAX;        // the interval its range was last found not ready (once an interval)
  uint64_t proven_flush = UINT64_MAX;  // the interval it was last proven in
  uint64_t hash = 0;                   // the guest bytes copied
  uint64_t exact_frame = UINT64_MAX;
  uint64_t last_used = 0;  // g_stream_cache_frame
};
constexpr u32 kFetchPoolBits = 12;
FetchPoolEntry g_fetch_pool[1u << kFetchPoolBits];
uint64_t g_fetch_pool_draws = 0, g_fetch_pool_bytes_used = 0, g_fetch_pool_made = 0, g_fetch_pool_dropped = 0;
uint64_t g_fetch_pool_differ = 0, g_fetch_pool_held_bytes = 0;  // (held: the pool bytes now, the 10 s line's)
bool g_fetch_from_pool = false;  // the last UploadVertexFetchData bound the pool (its caller keeps the draw's fetch data)

bool FetchCacheOn(int ab) {
  static const bool on = REXCVAR_GET(banjo_renut_fetch_cache);
  return on && ab != 47;
}

FetchPoolEntry &FetchPoolFor(const u8 *src, u32 size) {
  const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(src)) ^ (uint64_t(size) << 40);
  return g_fetch_pool[(key * 0x9E3779B97F4A7C15ull) >> (64 - kFetchPoolBits)];
}

void DropFetchCopy(FetchPoolEntry &e) {
  if (!e.copy)
    return;
  g_stream_cache_bytes -= e.copy.size;
  g_fetch_pool_held_bytes -= e.copy.size;
  g_copy_pool.Free(e.copy);
  e.copy = {};
  ++g_fetch_pool_dropped;
}

// The kept copy of [src, src + size) for this interval: proven (ProveStreamCopy's rule) or made now; null for none.
const FetchPoolEntry *FetchPoolCopy(const u8 *src, u32 size) {
  FetchPoolEntry &e = FetchPoolFor(src, size);
  if (e.src != src || e.size != size) {
    DropFetchCopy(e);
    e = {};
    e.src = src;
    e.size = size;
  }
  if (e.last_used != g_stream_cache_frame)  // banjo-nx (2026-10-28, render-thread round): only when it changes
    e.last_used = g_stream_cache_frame;
  if (e.copy) {
    if (e.proven_flush == g_flush_serial)
      return &e;
    if (CardsProve(src, size, e.proven_flush, e.hash)) {
      e.proven_flush = g_flush_serial;
      return &e;
    }
    const bool same = ExactHash(src, size) == e.hash;
    banjo::renut_gpu::CountExactCheck(size, !same, e.exact_frame == g_stream_cache_frame);
    e.exact_frame = g_stream_cache_frame;
    if (same) {
      e.proven_flush = g_flush_serial;
      return &e;
    }
    DropFetchCopy(e);
    e.seen = UINT64_MAX;
    e.dynamic = ++e.drops >= kMaxCacheDrops;
    return nullptr;
  }
  if (e.dynamic || e.looked == g_flush_serial || !banjo::renut_gpu::StoreCardsOn() ||
      !banjo::renut_gpu::ExactStreamsOn())
    return nullptr;
  if (!WholeRangeHeld(src, size, e.seen)) {
    e.looked = g_flush_serial;
    return nullptr;
  }
  if (g_stream_cache_bytes + size > banjo::renut_gpu::StreamCacheMaxBytes(kStreamCacheMaxBytes)) {
    ++g_stream_cache_refused;
    e.looked = g_flush_serial;
    return nullptr;
  }
  const PoolSpan copy = g_copy_pool.Allocate(size);
  if (!copy)
    return nullptr;
  std::memcpy(copy.cpu, src, size);
  e.copy = copy;
  e.base = copy.buffer->getDeviceAddress();
  e.chunk_bytes = g_copy_pool.ChunkBytes(copy.chunk);
  e.hash = banjo::renut_gpu::ContentHash64(src, size);
  e.proven_flush = g_flush_serial;  // the cards held from 'seen' to now
  g_stream_cache_bytes += copy.size;
  g_fetch_pool_held_bytes += copy.size;
  g_fetch_pool_made += size;
  return &e;
}

// The idle pass's share (StreamCacheFrameLocked): entries no draw used for 'idle_frames' go.
void DropIdleFetchCopies(uint64_t idle_frames) {
  for (FetchPoolEntry &e : g_fetch_pool) {
    if (e.copy && g_stream_cache_frame - e.last_used >= idle_frames) {
      DropFetchCopy(e);
      e = {};
    }
  }
}

// banjo-nx (2026-10-26): why a draw took the ranges (the 10 s line's), by the first reason met.
enum WholeWhy : u32 {
  kWhyKind, kWhyStreamSource, kWhyStreamExport, kWhyStreamDynamic, kWhyStreamWritten, kWhyStreamFull,
  kWhyStreamDropped, kWhyIndexRange, kWhyIndexExport, kWhyIndexDynamic, kWhyIndexWritten, kWhyIndexFull,
  kWhyIndexDropped, kWhyAlloc, kWhyCount
};
uint64_t g_whole_why[kWhyCount] = {};
u32 g_whole_why_last = kWhyKind;
template <typename T>
inline T *WholeMiss(u32 why) {
  g_whole_why_last = why;
  return nullptr;
}
// banjo-nx (2026-10-26): the guest ranges of the whole copies made lately. The range copies inside one (same layout,
// stream and stride; for indices the same format) are no longer drawn once the buffer is drawn whole; they are dropped
// kWholeReplaceFrames frames after the whole copy (DropReplacedRanges, if no draw used them since) instead of holding
// banjo_renut_stream_cache_mb until the idle pass (the emulator's city run filled the 128 MB that way: whole copies
// refused for a minute).
struct WholeSpan {
  const u8 *lo, *hi;
  uint64_t layout_hash;  // streams; 0 for indices
  u32 stream, stride;    // streams; for indices stride = 4 or 2 and stream = the endian | 0x100 (never a stream's)
  uint64_t frame;
};
std::vector<WholeSpan> g_whole_spans;
uint64_t g_whole_bytes = 0, g_replaced_bytes = 0;  // the 10 s line's: whole copies made, range copies dropped
constexpr uint64_t kWholeReplaceFrames = 4;
// banjo-nx (2026-10-28, per-draw night): DropReplacedRanges' pass at most every kWholeReplacePeriod frames (it walks
// every node of both caches: 0.8 ms a frame on the console's render thread in the d4 profile, as whole copies are made
// in most frames). The ranges it drops stay up to that much longer (memory only: what a draw reads does not change).
constexpr uint64_t kWholeReplacePeriod = 16;

// banjo-nx (2026-10-27, A/B 37): WholeGeometryTry's last source per stream slot and what it was made of.
struct LastStreamSource {
  bool valid = false;
  u32 w0 = 0, w1 = 0, sva = 0, sbyte = 0;
  const ShaderRecord *vs = nullptr;
  const u32 *shader_strides = nullptr;
  StreamSource src;
};
LastStreamSource g_last_stream_sources[kMaxVertexStreams];

// banjo-nx (2026-10-26, console profile: WholeStreamCopy + WholeIndexCopy 7.6 % of the render thread, mostly the memo's
// slot and the map's node missing the cache): the last whole copy each stream slot and the index buffer took. Draws in a
// row mostly draw meshes of the same buffers, which whole copies share: the entry without a hash or a lookup. Cleared
// with the memos wherever entries are erased (ClearWholeLast).
struct WholeLastStream {
  StreamKey key{};
  CachedStream *c = nullptr;
};
WholeLastStream g_whole_last_stream[kMaxVertexStreams];
IndexKey g_whole_last_index_key{};
CachedIndices *g_whole_last_index = nullptr;
void ClearWholeLast() {
  for (WholeLastStream &l : g_whole_last_stream)
    l.c = nullptr;
  g_whole_last_index = nullptr;
  ++g_mesh_gen;  // banjo-nx (2026-10-27): entries may be erased next; mesh objects name them
  ++g_geometry_gen;  // (and geometry objects' answers)
}

// kameo-nx (2026-11-04, console rc16 run, 3 fps in the big level): range copies reach the cache's limit first, because a
// buffer written by the load can't have a whole copy yet, and a range copy needs only kStableFlushes kicks, which a split
// frame gets through at once. Then the whole copies that would replace those ranges were refused, and the ranges, still
// drawn every frame, never went idle: 600 MB of vertices converted a frame. A bigger limit only moved the point where it
// happens. In the same run, once the idle pass made room, 33 MB of whole copies replaced 197.5 MB of ranges, and 1 fps
// became 48. So a whole copy may pass the limit by banjo_renut_whole_over_cap_mb; the range copies stay refused until
// DropReplacedRanges brings the total back down.
bool WholeCopyFits(uint64_t bytes) {
  static const uint64_t over = uint64_t(std::max(REXCVAR_GET(banjo_renut_whole_over_cap_mb), 0)) << 20;
  return g_stream_cache_bytes + bytes <= banjo::renut_gpu::StreamCacheMaxBytes(kStreamCacheMaxBytes) + over;
}

CachedStream *WholeStreamCopy(const StreamKey &key, const HostVertexStream &hs) {
  WholeLastStream &last = g_whole_last_stream[key.stream % kMaxVertexStreams];
  if (!last.c || !(last.key == key))
    last = {key, &g_stream_memo.Get(g_stream_cache, key, true)->second};
  CachedStream &c = *last.c;
  c.last_used = g_stream_cache_frame;
  c.whole = true;  // banjo-nx (2026-10-28): the answers, mesh objects and last pointers that may hold it
  if (c.copy)
    return ProveStreamCopy(key, c) ? &c : WholeMiss<CachedStream>(kWhyStreamDropped);
  if (c.dynamic || !banjo::renut_gpu::StoreCardsOn() || !banjo::renut_gpu::ExactStreamsOn())
    return WholeMiss<CachedStream>(kWhyStreamDynamic);
  if (!WholeRangeHeld(key.guest, size_t(key.count) * key.stride, c.whole_seen))
    return WholeMiss<CachedStream>(kWhyStreamWritten);
  const u32 bytes = key.count * hs.host_stride;
  if (!WholeCopyFits(bytes)) {
    ++g_stream_cache_refused;
    return WholeMiss<CachedStream>(kWhyStreamFull);
  }
  const PoolSpan copy = g_copy_pool.Allocate(bytes);
  if (!copy)
    return WholeMiss<CachedStream>(kWhyAlloc);
  if (::renut::gpu::host_batch::CensusOn())  // blur-nx (2026-11-18, the census's #12)
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kConvertWholeBytes, bytes);
  ConvertVertices(hs, key.guest, key.stride, key.count, key.endian, static_cast<u8 *>(copy.cpu));
  c.copy = copy;
  c.bytes = bytes;
  c.hash = HashGuestStream(key);
  c.stable = kStableFlushes;
  c.checked_frame = g_stream_cache_frame;
  c.proven_flush = g_flush_serial;  // the cards held from whole_seen to now
  g_stream_cache_bytes += bytes;
  g_whole_bytes += bytes;
  g_whole_spans.push_back({key.guest, key.guest + size_t(key.count) * key.stride, key.layout_hash, key.stream,
                           key.stride, g_stream_cache_frame});
  return &c;
}

CachedIndices *WholeIndexCopy(const IndexKey &key) {
  if (!g_whole_last_index || !(g_whole_last_index_key == key)) {
    g_whole_last_index_key = key;
    g_whole_last_index = &g_index_memo.Get(g_index_cache, key, true)->second;
  }
  CachedIndices &c = *g_whole_last_index;
  c.last_used = g_stream_cache_frame;
  c.whole = true;  // blur-nx (2026-11-18, #36): the mesh objects and the whole-repeat answer may hold it
  if (c.copy)
    return ProveIndexCopy(key, c) ? &c : WholeMiss<CachedIndices>(kWhyIndexDropped);
  if (c.dynamic || !banjo::renut_gpu::StoreCardsOn() || !banjo::renut_gpu::ExactStreamsOn())
    return WholeMiss<CachedIndices>(kWhyIndexDynamic);
  const size_t guest_bytes = size_t(key.count) * (key.is32 ? 4 : 2);
  if (!WholeRangeHeld(key.src, guest_bytes, c.whole_seen))
    return WholeMiss<CachedIndices>(kWhyIndexWritten);
  const u32 bytes = u32(guest_bytes);
  if (!WholeCopyFits(bytes)) {
    ++g_stream_cache_refused;
    return WholeMiss<CachedIndices>(kWhyIndexFull);
  }
  if (!ReadIndices(key.src, key.count, key.is32, key.endian))
    return WholeMiss<CachedIndices>(kWhyIndexRange);
  const PoolSpan copy = g_copy_pool.Allocate(bytes);
  if (!copy)
    return WholeMiss<CachedIndices>(kWhyAlloc);
  if (key.is32) {
    std::memcpy(copy.cpu, g_indices.data(), bytes);
  } else {
    RebaseIndices16(reinterpret_cast<uint16_t *>(copy.cpu), 0);  // as they are: 16-bit
  }
  c.copy = copy;
  c.bytes = bytes;
  c.index_count = key.count;
  c.lo = 0;
  c.max_index = 0;
  c.wide = key.is32;
  c.hash = banjo::renut_gpu::ContentHash64(key.src, guest_bytes);
  c.stable = kStableFlushes;
  c.checked_frame = g_stream_cache_frame;
  c.proven_flush = g_flush_serial;
  g_stream_cache_bytes += bytes;
  g_whole_bytes += bytes;
  g_whole_spans.push_back({key.src, key.src + guest_bytes, 0, 0x100u | key.endian, key.is32 ? 4u : 2u,
                           g_stream_cache_frame});
  return &c;
}

uint64_t g_whole_draws = 0, g_range_draws = 0;  // the 10 s line's (WholeBuffersReport)
uint64_t g_base_draws = 0;  // banjo-nx (2026-10-27): of the whole draws, those drawn with their base vertex (FastDrawLocked)
uint64_t g_fetch_fill_bytes = 0;  // banjo-nx (2026-10-27): fetch copies' bytes copied at the kicks (FlushVertexFillsLocked)
uint64_t g_fetch_kept_draws = 0;  // banjo-nx (2026-10-27): draws that kept the fetch data of the draw before (A/B 36)
bool AnyPendingExports();
u32 PendingExportSlotsMemo(u32 which, u32 address, u32 size);
constexpr u32 kExportAnswerFetch = 0, kExportAnswerCopy = 32, kExportAnswerIndex = 64;


// ---------------------------------------------------------------------------
// banjo-nx (2026-10-27, the user's request: #8 of the CPU-gap table, render resource objects; banjo_renut_mesh_objects,
// A/B 42). A mesh object: what a whole-buffer draw's geometry resolves to (each used stream's whole copy, the index
// buffer's whole copy), kept under what it is resolved from (the declaration's layout, the vertex shader and its
// strides, each used stream's fetch constant words, source and stride byte, the draw's base, count and first index,
// the index buffer header's words). A draw of a mesh drawn before takes its object: one probe of a direct-mapped table
// instead of, per stream, the source (GetStreamSource reads the buffer's guest header), the key, the stream memo's slot
// and entry and the copy's entry, and the same for the index buffer: most of those are cache misses (WholeGeometryTry
// was 8.8 % of the render thread in the shadowsort city profile, a third of it the memos' slots and entries).
// Validity is pushed, not checked: every copy dropped or entry erased (DropCachedCopy, DropCachedIndices,
// ClearWholeLast) raises g_mesh_gen, which retires every object at once; the copies' contents are proven once per kick
// interval as before (the store cards, or the exact hash: ProveCopyRange), for the object's parts together.
// Exact: an object answers only for the same inputs, with copies proven by the same rule, and the old path's answer
// for those inputs is the same copies (WholeStreamCopy / WholeIndexCopy key them by what the object's key determines:
// the stream source comes from the fetch words, source, stride byte, shader and strides, as A/B 37's per-slot reuse
// already relies on). With exports pending a hit also passes the old path's export-window tests (MeshExportFree);
// not used without banjo_renut_exact_streams (the verifier needs each key).
// ---------------------------------------------------------------------------
constexpr u32 kMeshStreams = 3;     // meshes of more streams take the old path
// 2 MB. banjo-nx (2026-10-27): 8,192 (4,096 before): the console's city made about 340,000 objects per 10 s for about
// 300,000 answers (each submesh is its own key: its base, count and first index).
constexpr u32 kMeshEntries = 8192;
// banjo-nx (2026-10-27, ARM pass): a hit within an interval reads lines 0-2 only (it read all four before: the streams,
// the index copy and is32 sat on line 3 with the guest ranges, which only a new interval's proof reads).
struct alignas(64) MeshObject {
  // Line 0: the key.
  uint64_t hash = 0;  // 0: empty
  const HostVertexLayout *layout = nullptr;
  const ShaderRecord *vs = nullptr;
  const u32 *strides = nullptr;
  uint64_t layout_hash = 0, vs_hash = 0;
  u32 base = 0, count = 0, start_index = 0, flags = 0;  // flags: indexed (bit 8), the stream count (bits 0-7)
  // Line 1: the key's words (zero past the streams, and the index words for a draw without indices).
  u32 words[kMeshStreams][4] = {};  // fetch words 0 and 1, the source, the stride byte
  u32 ib[4] = {};                   // the index buffer header's Common, Address, Size (and a zero)
  // Line 2: what a hit takes.
  uint64_t gen = 0, proven_flush = UINT64_MAX;
  CachedStream *copies[kMeshStreams] = {};
  CachedIndices *indices = nullptr;
  u8 streams[kMeshStreams] = {};
  bool is32 = false;
  // Line 3: the parts' guest ranges, for a new interval's proof.
  alignas(64) const u8 *guest[kMeshStreams] = {};
  const u8 *index_src = nullptr;
  u32 guest_bytes[kMeshStreams] = {};
  u32 index_bytes = 0;
};
static_assert(sizeof(MeshObject) == 256 && offsetof(MeshObject, words) == 64 && offsetof(MeshObject, gen) == 128 &&
              offsetof(MeshObject, guest) == 192);
// banjo-nx (2026-10-28, the heap): made at the first probe (2 MB that the geometry objects, on by default, never use).
MeshObject *g_mesh_objects = nullptr;
uint64_t g_mesh_hits = 0, g_mesh_reproved = 0, g_mesh_kept = 0, g_mesh_stale = 0, g_mesh_misses = 0;

// The probe's inputs, read once: the key's words, its hash and the entry they name.
struct MeshProbe {
  MeshObject *entry = nullptr;
  uint64_t hash = 0;
  u32 count = 0;                // the used streams
  u8 streams[kMeshStreams] = {};
  u32 words[kMeshStreams][4] = {};
  u32 ib[4] = {};
};

inline bool MeshObjectsOn(int ab) {
  static const bool on = REXCVAR_GET(banjo_renut_mesh_objects);
  return on && ab != 42 && banjo::renut_gpu::ExactStreamsOn();
}

inline uint64_t MeshMix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

// The key's words from the device (and the index buffer's header), the hash and the entry. False: no object for this
// draw (more streams than an object holds).
bool MeshProbeMake(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs, const u32 *shader_strides,
                   const HostVertexLayout &layout, uint64_t layout_hash, u32 base, bool have_ib, const u32 ib_words[3],
                   MeshProbe &p) {
  uint64_t h = MeshMix(layout_hash, vs.hash);
  h = MeshMix(h, uint64_t(reinterpret_cast<uintptr_t>(&layout)) ^ uint64_t(reinterpret_cast<uintptr_t>(shader_strides)) << 1);
  h = MeshMix(h, uint64_t(reinterpret_cast<uintptr_t>(&vs)));
  h = MeshMix(h, uint64_t(base) << 32 | call.count);
  h = MeshMix(h, uint64_t(call.start_index) << 1 | u32(call.indexed));
  for (u32 left = layout.used_mask; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    if (!layout.streams[stream].used)
      continue;
    if (p.count == kMeshStreams)
      return false;
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    u32 *w = p.words[p.count];
    w[0] = device.VertexFetchWord(fetch_index, 0);
    w[1] = device.VertexFetchWord(fetch_index, 1);
    w[2] = device.U32(d3d::kDeviceOffsetStreamSources + stream * 4);
    w[3] = u32(device.U8(d3d::kDeviceOffsetStreamStrides + stream)) | stream << 8;
    h = MeshMix(h, uint64_t(w[0]) << 32 | w[1]);
    h = MeshMix(h, uint64_t(w[2]) << 32 | w[3]);
    p.streams[p.count++] = u8(stream);
  }
  // The entry's lines, while the index buffer's header comes in.
  if (!g_mesh_objects) [[unlikely]]
    g_mesh_objects = new MeshObject[kMeshEntries]();
  p.entry = &g_mesh_objects[(h * 0x9E3779B97F4A7C15ull) >> 51];
  static_assert(kMeshEntries == 1u << 13);
  for (u32 line = 0; line < sizeof(MeshObject); line += 64)
    __builtin_prefetch(reinterpret_cast<const char *>(p.entry) + line);
  if (call.indexed) {
    if (!have_ib)
      return false;
    p.ib[0] = ib_words[0];
    p.ib[1] = ib_words[1];
    p.ib[2] = ib_words[2];
    h = MeshMix(h, uint64_t(p.ib[0]) << 32 | p.ib[1]);
    h = MeshMix(h, p.ib[2]);
  }
  p.hash = h | 1;  // never 0 (empty)
  return true;
}

// A copy proven for this kick interval by the rule of ProveStreamCopy / ProveIndexCopy in exact mode (the cards, or the
// exact hash); dropped, and false, when its guest bytes changed.
inline void DropCopyOf(CachedStream &c) { DropCachedCopy(c); }
inline void DropCopyOf(CachedIndices &c) { DropCachedIndices(c); }
template <class C>
bool ProveCopyRange(const u8 *guest, size_t bytes, C &c) {
  if (!c.copy)
    return false;
  if (c.proven_flush != g_flush_serial && CardsProve(guest, bytes, c.proven_flush, c.hash)) {
    c.proven_flush = g_flush_serial;
  } else if (c.proven_flush != g_flush_serial) {
    const bool same = ExactHash(guest, bytes) == c.hash;
    banjo::renut_gpu::CountExactCheck(uint64_t(bytes), !same, c.exact_frame == g_stream_cache_frame);
    c.exact_frame = g_stream_cache_frame;
    if (!same) {
      DropCopyOf(c);
      c.stable = 0;
      c.checked_frame = UINT64_MAX;
      c.whole_seen = UINT64_MAX;
      c.dynamic = ++c.drops >= kMaxCacheDrops;
      return false;
    }
    c.proven_flush = g_flush_serial;
  }
  return true;
}

// With exports pending: none of the probe's streams nor its index buffer in an export window (WholeGeometryTry's tests).
bool MeshExportFree(const DrawCall &call, const MeshProbe &p) {
  for (u32 i = 0; i < p.count; ++i) {
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(p.streams[i]);
    if (PendingExportSlotsMemo(kExportAnswerFetch + ((fetch_index - 64) & 31), p.words[i][0] & ~3u,
                               p.words[i][1] & 0x3FFFFFCu))
      return false;
  }
  return !call.indexed || !PendingExportSlotsMemo(kExportAnswerIndex, p.ib[1], p.ib[2]);
}

// The draw's geometry from its mesh object, when it has one of this generation whose copies hold.
bool MeshObjectHit(const DrawCall &call, MeshProbe &p, const HostVertexLayout &layout, uint64_t layout_hash,
                   const ShaderRecord &vs, const u32 *shader_strides, u32 base, Geometry &g,
                   CachedStream *copies[kMaxVertexStreams]) {
  MeshObject &e = *p.entry;
  // (Words past the streams and the index words of a draw without indices are zero in both: one 64-byte compare.)
  static_assert(sizeof(e.words) + sizeof(e.ib) == 64 && sizeof(p.words) + sizeof(p.ib) == 64 &&
                offsetof(MeshProbe, ib) == offsetof(MeshProbe, words) + sizeof(p.words) &&
                offsetof(MeshObject, ib) == offsetof(MeshObject, words) + sizeof(e.words));
  if (e.hash != p.hash || e.layout != &layout || e.vs != &vs || e.strides != shader_strides ||
      e.layout_hash != layout_hash || e.vs_hash != vs.hash || e.base != base || e.count != call.count ||
      e.start_index != call.start_index || e.flags != (p.count | (call.indexed ? 0x100u : 0u)) ||
      !SameBytes<64>(e.words, p.words)) {
    ++g_mesh_misses;
    return false;
  }
  if (e.gen != g_mesh_gen) {
    ++g_mesh_stale;
    return false;
  }
  if (e.proven_flush != g_flush_serial) {
    // The interval's proof of every part (and their use for the idle passes), as WholeStreamCopy / WholeIndexCopy do.
    for (u32 i = 0; i < p.count; ++i) {
      CachedStream &c = *e.copies[i];
      c.last_used = g_stream_cache_frame;
      if (!ProveCopyRange(e.guest[i], e.guest_bytes[i], c))
        return false;  // (dropped: g_mesh_gen is raised; the old path decides)
    }
    if (call.indexed) {
      CachedIndices &ci = *e.indices;
      ci.last_used = g_stream_cache_frame;
      if (!ProveCopyRange(e.index_src, e.index_bytes, ci))
        return false;
    }
    e.proven_flush = g_flush_serial;
    ++g_mesh_reproved;
  }
  for (u32 i = 0; i < p.count; ++i) {
    copies[e.streams[i]] = e.copies[i];
    __builtin_prefetch(e.copies[i]);  // BindWholeStreams reads the copy's span
  }
  if (call.indexed) {
    const CachedIndices &ci = *e.indices;
    g.indexed = true;
    g.index_count = call.count;
    g.first_index = call.start_index;
    g.base_vertex = call.base_vertex;
    g.index_view = plume::RenderIndexBufferView(ci.copy.Ref(), ci.bytes,
                                                e.is32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  } else {
    g.vertex_count = call.count;
    g.first_vertex = call.start_vertex;
  }
  g.whole_base = base;
  g.whole = true;
  ++g_mesh_hits;
  return true;
}

// The old path's answer, kept as the mesh's object.
void MeshObjectKeep(const DrawCall &call, const MeshProbe &p, const HostVertexLayout &layout, uint64_t layout_hash,
                    const ShaderRecord &vs, const u32 *shader_strides, u32 base, const StreamKey *keys,
                    CachedStream *const copies[kMaxVertexStreams], CachedIndices *indices, const IndexKey &index_key) {
  MeshObject &e = *p.entry;
  e.hash = p.hash;
  e.layout = &layout;
  e.vs = &vs;
  e.strides = shader_strides;
  e.layout_hash = layout_hash;
  e.vs_hash = vs.hash;
  e.base = base;
  e.count = call.count;
  e.start_index = call.start_index;
  e.flags = p.count | (call.indexed ? 0x100u : 0u);
  std::memcpy(e.words, p.words, sizeof(e.words));
  std::memcpy(e.ib, p.ib, sizeof(e.ib));
  e.gen = g_mesh_gen;
  e.proven_flush = g_flush_serial;  // the old path proved (or made) every part in this interval
  for (u32 i = 0; i < p.count; ++i) {
    e.streams[i] = p.streams[i];
    e.copies[i] = copies[p.streams[i]];
    e.guest[i] = keys[i].guest;
    e.guest_bytes[i] = keys[i].count * keys[i].stride;
  }
  e.indices = call.indexed ? indices : nullptr;
  e.is32 = index_key.is32;
  e.index_src = index_key.src;
  e.index_bytes = index_key.count * (index_key.is32 ? 4u : 2u);
  ++g_mesh_kept;
}

// ---------------------------------------------------------------------------
// banjo-nx (2026-10-27, the user's "geometry like Unleashed", banjo_renut_geometry_objects / A/B 48). Unleashed binds a
// vertex buffer as an object whose host copy it finds through it. N&B's buffers are the game's own memory, but each
// draw's stream sources and index buffer are named by words the hardware takes at the draw (the stream's fetch words,
// its source object and stride byte, the source header's base and size; the index buffer header's Common, Address and
// Size): the recording threads resolve those to objects (render/draw.h FindStreamObject, FindIndexObject: one per
// buffer, not per draw: the mesh objects were keyed by each submesh's base, count and first index, 340,000 made per
// 10 s in the city for 300,000 answers, and saved nothing on the console, A/B 42 -0.07 ms). The render thread keeps in
// each object the whole copy the old path found for it, and for a stream the layout, vertex shader and strides it was
// found for (GetStreamSource's other inputs; StreamKey's layout). A draw whose objects all answer takes their copies:
// a pointer and a few compares a buffer instead of the source, the key, the memo's slot and entry. The copies' proofs
// are the old path's, once per kick interval (the store cards, or the exact hash); an answer is void once any stream
// or index cache entry may have been erased (g_geometry_gen), not when a copy is dropped (its entry stays, its proof
// fails, and the old path decides). Exact: the same copies for the same inputs, proven by the same rule.
// ---------------------------------------------------------------------------
inline bool GeometryObjectsOn(int ab) {
  static const bool on = REXCVAR_GET(banjo_renut_geometry_objects);
  return on && ab != 48 && banjo::renut_gpu::ExactStreamsOn();
}

bool GeometryObjectsHit(const DrawCall &call, const ShaderRecord &vs, const u32 *shader_strides,
                        const HostVertexLayout &layout, uint64_t layout_hash, int64_t base, bool exports_pending,
                        const DrawResources &res, Geometry &g, CachedStream *copies[kMaxVertexStreams]) {
  for (u32 left = layout.used_mask; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    if (!layout.streams[stream].used)
      continue;
    const GeometryObject *o = ((res.obj_mask >> stream) & 1) ? res.streams[stream] : nullptr;
    const GeometryAnswer *a = nullptr;
    if (o) {
      for (const GeometryAnswer &k : o->answer) {
        if (k.entry_gen == g_geometry_gen && k.vs == &vs && k.layout_hash == layout_hash && k.strides == shader_strides) {
          a = &k;
          break;
        }
      }
    }
    if (!a || uint64_t(base) >= a->count) {
      ++g_geo_misses;
      return false;
    }
    if (exports_pending) {
      const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
      if (PendingExportSlotsMemo(kExportAnswerFetch + ((fetch_index - 64) & 31), o->key[0] & ~3u,
                                 o->key[1] & 0x3FFFFFCu))
        return false;
    }
    CachedStream &c = *static_cast<CachedStream *>(const_cast<void *>(a->copy));
    // banjo-nx (2026-10-28, render-thread round): stamps stored only when they change (each store dirtied the entry's
    // and the answer's lines at every draw, written back later).
    if (c.last_used != g_stream_cache_frame)
      c.last_used = g_stream_cache_frame;
    if (!ProveCopyRange(a->guest, a->bytes, c))
      return false;
    if (a->proven != u32(g_flush_serial))
      const_cast<GeometryAnswer *>(a)->proven = u32(g_flush_serial);  // (PrefetchGeometryCopies' cards)
    copies[stream] = &c;
    __builtin_prefetch(&c.copy);  // BindWholeStreams reads the copy's span
  }
  if (call.indexed) {
    const GeometryObject *o = res.index_obj;
    const GeometryAnswer *a = o ? &o->answer[0] : nullptr;
    if (!a || a->entry_gen != g_geometry_gen || call.count > a->count || call.start_index > a->count - call.count) {
      ++g_geo_misses;
      return false;
    }
    if (exports_pending && PendingExportSlotsMemo(kExportAnswerIndex, o->key[1], o->key[2]))
      return false;
    CachedIndices &ci = *static_cast<CachedIndices *>(const_cast<void *>(a->copy));
    if (ci.last_used != g_stream_cache_frame)  // banjo-nx (2026-10-28): only when it changes
      ci.last_used = g_stream_cache_frame;
    if (!ProveCopyRange(a->guest, a->bytes, ci))
      return false;
    if (a->proven != u32(g_flush_serial))
      const_cast<GeometryAnswer *>(a)->proven = u32(g_flush_serial);
    g.indexed = true;
    g.index_count = call.count;
    g.first_index = call.start_index;
    g.base_vertex = call.base_vertex;
    g.index_view = plume::RenderIndexBufferView(ci.copy.Ref(), ci.bytes,
                                                o->is32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  } else {
    g.vertex_count = call.count;
    g.first_vertex = call.start_vertex;
  }
  g.whole_base = u32(base);
  g.whole = true;
  ++g_geo_hits;
  return true;
}

// bangaio-nx (2026-10-29, the console's missile storm: "kind" 901k of 990k range draws): what the "kind" reason was,
// for the 10 s line. The draw's own properties first, in WholeGeometryTry's order, so that a PC run (not deko3d) splits
// them too; "off" is the rest (whole buffers off, A/B 22, not deko3d).
enum KindWhy : u32 { kKindTess, kKindUpVertices, kKindUpIndices, kKindNoCount, kKindFan, kKindQuads, kKindSkin, kKindOff,
                     kKindWhyCount };
uint64_t g_kind_why[kKindWhyCount] = {};
uint64_t g_kind_up_points = 0;  // of kKindUpVertices: point lists
void CountKindWhy(const DrawCall &call, const ShaderRecord &vs) {
  KindWhy why = kKindOff;
  if (call.tessellation)
    why = kKindTess;
  else if (call.up_vertices)
    why = kKindUpVertices, g_kind_up_points += call.primitive == kPointList;
  else if (call.up_indices)
    why = kKindUpIndices;
  else if (!call.count)
    why = kKindNoCount;
  else if (call.primitive == kTriangleFan)
    why = kKindFan;
  else if (call.primitive == kQuadList)
    why = kKindQuads;
  else if (DrawSummaryOf(vs).skin_once)
    why = kKindSkin;
  ++g_kind_why[why];
}

// The draw from whole-buffer copies when every stream and the index buffer have one (see above); 'copies' the streams'.
// blur-nx (2026-11-18, the Renut host batch's #4, blur_renut_whole_repeat / A/B 232): the last answer of
// WholeGeometryTry's full way (its keys' lookups and proofs), kept with what it was made from. A later draw of the same
// kick and frame (the proofs are a kick's; g_mesh_gen moves with every whole copy's drop) whose shader, strides, layout,
// streams' words and index buffer header are the same, with no export pending, finds the same entries with the same
// copies, proven in this kick already: it takes them without the mesh probe, the stream sources, the keys and the
// lookups. Each copy is checked to be there and proven in this kick (a drop since would show there too).
struct WholeRepeat {
  bool valid = false;
  const ShaderRecord *vs = nullptr;
  const u32 *strides = nullptr;
  const HostVertexLayout *layout = nullptr;
  uint64_t layout_hash = 0, flush = 0, frame = 0, mesh_gen = 0;
  u32 used = 0;  // the streams answered, a bit each
  u32 words[kMaxVertexStreams][4] = {};
  uint64_t min_vertices = 0;
  bool indexed = false;
  u32 ib[3] = {};
  u32 index_total = 0;
  plume::RenderIndexBufferView index_view{};
  CachedIndices *ci = nullptr;
  CachedStream *copies[kMaxVertexStreams] = {};
};
WholeRepeat g_whole_repeat;
[[gnu::always_inline]] inline void WholeRepeatWords(const GuestDevice &device, u32 stream, u32 w[4]) {
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
  w[0] = device.VertexFetchWord(fetch_index, 0);
  w[1] = device.VertexFetchWord(fetch_index, 1);
  w[2] = device.U32(d3d::kDeviceOffsetStreamSources + stream * 4);
  w[3] = device.U8(d3d::kDeviceOffsetStreamStrides + stream);
}
bool WholeRepeatHit(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs, const u32 *strides,
                    const HostVertexLayout &layout, uint64_t layout_hash, int64_t base, Geometry &g,
                    CachedStream *copies[kMaxVertexStreams]) {
  const WholeRepeat &r = g_whole_repeat;
  if (!r.valid || r.vs != &vs || r.strides != strides || r.layout != &layout || r.layout_hash != layout_hash ||
      r.flush != g_flush_serial || r.frame != g_stream_cache_frame || r.mesh_gen != g_mesh_gen ||
      r.indexed != call.indexed || uint64_t(base) >= r.min_vertices)
    return false;
  for (u32 left = r.used; left; left &= left - 1) {
    const u32 s = u32(std::countr_zero(left));
    u32 w[4];
    WholeRepeatWords(device, s, w);
    if (w[0] != r.words[s][0] || w[1] != r.words[s][1] || w[2] != r.words[s][2] || w[3] != r.words[s][3])
      return false;
    const CachedStream *c = r.copies[s];
    if (!c || !c->copy || c->proven_flush != g_flush_serial)
      return false;
  }
  if (call.indexed) {
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    if (!ib || u32(ib->resource.Common) != r.ib[0] || u32(ib->Address) != r.ib[1] || u32(ib->Size) != r.ib[2] ||
        call.count > r.index_total || call.start_index > r.index_total - call.count || !r.ci || !r.ci->copy ||
        r.ci->proven_flush != g_flush_serial)
      return false;
    g.indexed = true;
    g.index_count = call.count;
    g.first_index = call.start_index;
    g.base_vertex = call.base_vertex;
    g.index_view = r.index_view;
  } else {
    g.vertex_count = call.count;
    g.first_vertex = call.start_vertex;
  }
  for (u32 left = r.used; left; left &= left - 1) {
    const u32 s = u32(std::countr_zero(left));
    copies[s] = r.copies[s];
  }
  g.whole_base = u32(base);
  g.whole = true;
  return true;
}

bool WholeGeometryTry(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                      const u32 *shader_strides, const HostVertexLayout &layout, uint64_t layout_hash, Geometry &g,
                      CachedStream *copies[kMaxVertexStreams]) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kWholeGeometry);  // blur-nx (2026-11-17)
  g_whole_why_last = kWhyKind;
  static const bool on = REXCVAR_GET(banjo_renut_whole_buffers);
  // A/B target 22 (banjo_renut_ab_targets) turns it off for a half.
  if (!on || banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 22 || !IsDeko3D() || call.tessellation ||
      call.up_vertices || call.up_indices || !call.count || call.primitive == kTriangleFan ||
      call.primitive == kQuadList || DrawSummaryOf(vs).skin_once) {
    CountKindWhy(call, vs);
    return false;
  }
  // banjo-nx (2026-10-26, API-level stage 4): with exports pending, a draw none of whose buffers a pending export
  // writes (DrawNeedsExportFlush's own test, by each stream's fetch constant and the index buffer) is drawn whole too;
  // one that reads an export window takes the ranges (its stream built on the GPU, ConvertExportedStreamsLocked).
  const bool exports_pending = AnyPendingExports();
  const int64_t base = call.indexed ? int64_t(call.base_vertex) : int64_t(call.start_vertex);
  if (base < 0)
    return WholeMiss<void>(kWhyIndexRange) != nullptr;
  // blur-nx (2026-11-18, #4): the last full answer, when it holds (see WholeRepeat). Not with the recording's resources
  // (the geometry objects' path), nor with exports pending, nor without the exact rule (its proofs are the kick's).
  const bool hb_census = ::renut::gpu::host_batch::CensusOn();
  if (hb_census)
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kWholeTries);
  const bool repeat_on = !exports_pending && !g_draw_resources && banjo::renut_gpu::ExactStreamsOn() &&
                         ::renut::gpu::host_batch::On(::renut::gpu::host_batch::kWholeRepeat);
  if (repeat_on && WholeRepeatHit(device, call, vs, shader_strides, layout, layout_hash, base, g, copies)) {
    if (hb_census)
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kWholeRepeats);
    return true;
  }
  // banjo-nx (2026-10-26, the record-time draw path's round 4, ARM): in three passes, so that the cache misses of the
  // streams and the index buffer overlap instead of following one another (the console profile: the memo slots, the
  // maps' nodes and the copies' fields were most of WholeGeometryLocked's 10 % of the render thread): the keys and the
  // checks, with the index buffer's header, each last copy's entry or each memo slot fetched; then the entries the
  // slots name; then the lookups and proofs (WholeStreamCopy, WholeIndexCopy) as before.
  const d3d::IndexBuffer *ib = nullptr;
  // banjo-nx (2026-10-27, Phase A): the index buffer header's words as the recording took them (no guest header read
  // here); the header itself otherwise.
  const DrawResources *const res = g_draw_resources;
  const bool recorded_ib = call.indexed && res && res->ib_valid;
  u32 ib_words[3] = {};
  if (recorded_ib) {
    std::memcpy(ib_words, res->ib, sizeof(ib_words));
    ++g_ib_recorded;
    static const bool ib_verify = REXCVAR_GET(banjo_renut_resource_objects_verify);
    if (ib_verify) [[unlikely]] {
      const auto *g = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
      if (!g || u32(g->resource.Common) != ib_words[0] || u32(g->Address) != ib_words[1] || u32(g->Size) != ib_words[2])
        ++g_ib_verify_differ;
    }
  } else if (call.indexed) {
    ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    if (ib) {
      __builtin_prefetch(ib);
      __builtin_prefetch(reinterpret_cast<const char *>(ib) + sizeof(d3d::IndexBuffer) - 1);
      ib_words[0] = u32(ib->resource.Common);
      ib_words[1] = u32(ib->Address);
      ib_words[2] = u32(ib->Size);
    }
  }
  const bool have_ib = recorded_ib || ib;
  // banjo-nx (2026-10-27, geometry objects): the buffers' objects' copies, when they all answer (GeometryObjectsHit).
  const int geo_ab = banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed);
  const bool geo = res && GeometryObjectsOn(geo_ab);
  if (geo && GeometryObjectsHit(call, vs, shader_strides, layout, layout_hash, base, exports_pending, *res, g, copies))
    return true;
  // banjo-nx (2026-10-27, #8): the mesh's object, when it has one (see MeshObject). With exports pending, only when no
  // stream and not the index buffer reads an export window (the old path's own tests, below, by the same words:
  // the emulator's title screen has exports pending at nearly every draw). (Not with the geometry objects on.)
  MeshProbe probe;
  const bool mesh = !geo && MeshObjectsOn(geo_ab) &&
                    MeshProbeMake(device, call, vs, shader_strides, layout, layout_hash, u32(base), have_ib, ib_words,
                                  probe);
  if (mesh && (!exports_pending || MeshExportFree(call, probe)) &&
      MeshObjectHit(call, probe, layout, layout_hash, vs, shader_strides, u32(base), g, copies))
    return true;
  StreamKey keys[kMaxVertexStreams];
  u32 key_streams[kMaxVertexStreams];
  u32 key_count = 0, memo_streams = 0;  // memo_streams: the keys (by their index) not their slot's last copy
  for (u32 left = layout.used_mask; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    const HostVertexStream &hs = layout.streams[stream];
    if (!hs.used)
      continue;
    // banjo-nx (2026-10-27, render thread night, A/B 37): the stream's source as the draw before found it, when what it
    // is made of is the same (the fetch constant's words, the stream's buffer object and stride byte, the shader and its
    // strides): GetStreamSource reads the buffer object's header in guest memory (a cache miss a stream a draw) only to
    // read the same memory through its CPU address.
    StreamSource src;
    {
      const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
      const u32 w0 = device.VertexFetchWord(fetch_index, 0), w1 = device.VertexFetchWord(fetch_index, 1);
      const u32 sva = device.U32(d3d::kDeviceOffsetStreamSources + stream * 4);
      const u32 sbyte = device.U8(d3d::kDeviceOffsetStreamStrides + stream);
      LastStreamSource &ls = g_last_stream_sources[stream % kMaxVertexStreams];
      if (ls.valid && ls.w0 == w0 && ls.w1 == w1 && ls.sva == sva && ls.sbyte == sbyte && ls.vs == &vs &&
          ls.shader_strides == shader_strides && !(stream == 0 && call.up_vertices) &&
          banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 37) {
        src = ls.src;
      } else {
        src = GetStreamSource(device, call, vs, shader_strides, stream);
        ls = {true, w0, w1, sva, sbyte, &vs, shader_strides, src};
        if (stream == 0 && call.up_vertices)
          ls.valid = false;
      }
    }
    if (!src.data || !src.stride || !src.vertex_count || uint64_t(base) >= src.vertex_count)
      return WholeMiss<void>(kWhyStreamSource) != nullptr;
    if (exports_pending) {
      const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
      if (PendingExportSlotsMemo(kExportAnswerFetch + ((fetch_index - 64) & 31),
                                 device.VertexFetchWord(fetch_index, 0) & ~3u,
                                 device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu))
        return WholeMiss<void>(kWhyStreamExport) != nullptr;
    }
    const StreamKey key{src.data, layout_hash, stream, src.stride, src.vertex_count, src.endian};
    const WholeLastStream &last = g_whole_last_stream[stream % kMaxVertexStreams];
    if (last.c && last.key == key) {
      __builtin_prefetch(last.c);
      __builtin_prefetch(reinterpret_cast<const char *>(last.c) + 64);
    } else {
      g_stream_memo.PrefetchSlot(key);
      memo_streams |= 1u << key_count;
    }
    key_streams[key_count] = stream;
    keys[key_count++] = key;
  }
  IndexKey index_key{};
  if (call.indexed) {
    if (!have_ib)
      return WholeMiss<void>(kWhyIndexRange) != nullptr;
    const u32 common = ib_words[0];
    const bool is32 = (common & d3d::kIndexBufferCommon32Bit) != 0;
    const u8 *index_base = AddressPtr(ib_words[1]);
    const u32 total = ib_words[2] / (is32 ? 4u : 2u);
    if (!index_base || total > kMaxDrawIndices || call.count > total || call.start_index > total - call.count)
      return WholeMiss<void>(kWhyIndexRange) != nullptr;
    if (exports_pending && PendingExportSlotsMemo(kExportAnswerIndex, ib_words[1], ib_words[2]))
      return WholeMiss<void>(kWhyIndexExport) != nullptr;
    index_key = IndexKey{index_base, total, kWholeIndexPrimitive, d3d::IndexBufferEndian(common), is32};
    if (g_whole_last_index && g_whole_last_index_key == index_key)
      __builtin_prefetch(g_whole_last_index);
    else
      g_index_memo.PrefetchSlot(index_key);
  }
  for (u32 bits = memo_streams; bits; bits &= bits - 1)
    g_stream_memo.PrefetchEntry(keys[std::countr_zero(bits)]);
  if (call.indexed && !(g_whole_last_index && g_whole_last_index_key == index_key))
    g_index_memo.PrefetchEntry(index_key);
  for (u32 i = 0; i < key_count; ++i) {
    const u32 stream = key_streams[i];
    CachedStream *c = WholeStreamCopy(keys[i], layout.streams[stream]);
    if (!c)
      return false;
    copies[stream] = c;
  }
  CachedIndices *ci = nullptr;
  if (call.indexed) {
    const bool is32 = index_key.is32;
    ci = WholeIndexCopy(index_key);
    if (!ci)
      return false;
    g.indexed = true;
    g.index_count = call.count;
    g.first_index = call.start_index;
    g.base_vertex = call.base_vertex;
    g.index_view = plume::RenderIndexBufferView(ci->copy.Ref(), ci->bytes,
                                                is32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  } else {
    g.vertex_count = call.count;
    g.first_vertex = call.start_vertex;
  }
  g.whole_base = u32(base);
  g.whole = true;
  // banjo-nx (2026-10-27, geometry objects): the answers into the draw's buffers' objects.
  if (geo) {
    for (u32 i = 0; i < key_count; ++i) {
      const u32 stream = key_streams[i];
      GeometryObject *o = ((res->obj_mask >> stream) & 1) ? res->streams[stream] : nullptr;
      if (!o)
        continue;
      // The slot answering for this shader, layout and strides (an older answer for them), else the next one.
      u32 slot = o->next;
      for (u32 k = 0; k < 2; ++k) {
        const GeometryAnswer &old = o->answer[k];
        if (old.vs == &vs && old.layout_hash == layout_hash && old.strides == shader_strides) {
          slot = k;
          break;
        }
      }
      if (slot == o->next)
        o->next ^= 1;
      GeometryAnswer &a = o->answer[slot];
      a.copy = copies[stream];
      a.entry_gen = g_geometry_gen;
      a.layout_hash = layout_hash;
      a.vs = &vs;
      a.strides = shader_strides;
      a.guest = keys[i].guest;
      a.bytes = keys[i].count * keys[i].stride;
      a.count = keys[i].count;
      ++g_geo_answers;
    }
    if (call.indexed && res->index_obj) {
      GeometryObject *o = res->index_obj;
      GeometryAnswer &a = o->answer[0];
      a.copy = ci;
      a.entry_gen = g_geometry_gen;
      a.guest = index_key.src;
      a.bytes = index_key.count * (index_key.is32 ? 4u : 2u);
      a.count = index_key.count;
      o->is32 = index_key.is32;
      ++g_geo_answers;
    }
  }
  // banjo-nx (2026-10-27, #8): kept as the mesh's object (the probe's streams are the keys', in the same order).
  if (mesh && probe.count == key_count)
    MeshObjectKeep(call, probe, layout, layout_hash, vs, shader_strides, u32(base), keys, copies, ci, index_key);
  // blur-nx (2026-11-18, #4): this answer for the next draws of the kick (WholeRepeat).
  if (repeat_on) {
    WholeRepeat &r = g_whole_repeat;
    r.valid = true;
    r.vs = &vs;
    r.strides = shader_strides;
    r.layout = &layout;
    r.layout_hash = layout_hash;
    r.flush = g_flush_serial;
    r.frame = g_stream_cache_frame;
    r.mesh_gen = g_mesh_gen;
    r.used = 0;
    r.min_vertices = UINT64_MAX;
    for (u32 i = 0; i < key_count; ++i) {
      const u32 s = key_streams[i];
      r.used |= 1u << s;
      WholeRepeatWords(device, s, r.words[s]);
      r.copies[s] = copies[s];
      r.min_vertices = std::min<uint64_t>(r.min_vertices, keys[i].count);
    }
    r.indexed = call.indexed;
    if (call.indexed) {
      std::memcpy(r.ib, ib_words, sizeof(r.ib));
      r.index_total = index_key.count;
      r.index_view = g.index_view;
      r.ci = ci;
    }
  }
  return true;
}
bool WholeGeometryLocked(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                         const u32 *shader_strides, const HostVertexLayout &layout, uint64_t layout_hash, Geometry &g,
                         CachedStream *copies[kMaxVertexStreams]) {
  if (WholeGeometryTry(device, call, vs, shader_strides, layout, layout_hash, g, copies))
    return true;
  ++g_whole_why[g_whole_why_last];
  return false;
}

// banjo-nx (2026-10-26, banjo_renut_whole_buffers_verify): the whole draw's indices against the guest's, and its
// vertices' rows against the range converted again.
void VerifyWholeGeometry(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                         const u32 *shader_strides, const HostVertexLayout &layout, const Geometry &g,
                         CachedStream *const copies[kMaxVertexStreams]) {
  u32 lo = call.start_vertex, hi = call.start_vertex + call.count - 1;
  bool ok = true;
  if (g.indexed) {
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    const u32 common = ib->resource.Common;
    const bool is32 = (common & d3d::kIndexBufferCommon32Bit) != 0;
    const u8 *src = AddressPtr(ib->Address) + size_t(call.start_index) * (is32 ? 4 : 2);
    if (!ReadIndices(src, call.count, is32, d3d::IndexBufferEndian(common)))
      return;
    // The whole copy's indices from the draw's first: the guest's as they are.
    const IndexKey key{AddressPtr(ib->Address), u32(ib->Size) / (is32 ? 4u : 2u), kWholeIndexPrimitive,
                       d3d::IndexBufferEndian(common), is32};
    if (auto *entry = g_index_memo.Get(g_index_cache, key, false); entry && entry->second.copy) {
      const u8 *copy = static_cast<const u8 *>(entry->second.copy.cpu);
      for (u32 i = 0; i < call.count && ok; ++i) {
        u32 v;
        if (is32) {
          std::memcpy(&v, copy + (size_t(call.start_index) + i) * 4, 4);
        } else {
          uint16_t h;
          std::memcpy(&h, copy + (size_t(call.start_index) + i) * 2, 2);
          v = h;
        }
        ok = v == g_indices[i];
      }
    }
    lo = UINT32_MAX;
    hi = 0;
    for (u32 i = 0; i < call.count; ++i) {
      lo = std::min(lo, g_indices[i]);
      hi = std::max(hi, g_indices[i]);
    }
    lo = u32(i64(lo) + g.base_vertex);
    hi = u32(i64(hi) + g.base_vertex);
  }
  static std::vector<u8> fresh;
  for (u32 left = layout.used_mask; left && ok; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    const HostVertexStream &hs = layout.streams[stream];
    if (!hs.used || !copies[stream])
      continue;
    const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
    if (hi >= src.vertex_count || lo > hi)
      continue;  // past the buffer: neither form reads defined data
    const u32 count = hi - lo + 1;
    fresh.resize(size_t(count) * hs.host_stride);
    ConvertVertices(hs, src.data + size_t(lo) * src.stride, src.stride, count, src.endian, fresh.data());
    ok = std::memcmp(static_cast<const u8 *>(copies[stream]->copy.cpu) + size_t(lo) * hs.host_stride, fresh.data(),
                     fresh.size()) == 0;
  }
  if (!ok) {
    static uint32_t logged = 0;
    if (logged++ < 16)
      RGPU_WARN("banjo_renut_whole_buffers_verify: a whole-buffer draw's vertices differ from its range converted "
                "again (VS {:016X}, vertices {}-{})", vs.hash, lo, hi);
  }
}

// The whole copies bound (BindStreams' work for a whole draw), then the zero stream as BindStreams binds it.
// keflings-nx (2026-10-30, keflings_renut_missing_input_w1): what a vertex shader input the declaration does not provide
// reads. Renut fed such inputs from a zero buffer, (0, 0, 0, 0). D3D's rule for missing components, which Renut already
// follows for a short format (vertex_format.cpp: 0 for y and z, 1 for w), gives a wholly missing element (0, 0, 0, 1).
static void FillMissingInputDefaults(void *cpu) {
  const float values[4] = {0.0f, 0.0f, 0.0f, banjo::renut_gpu::MissingInputW1On() ? 1.0f : 0.0f};
  static_assert(sizeof(values) == kZeroStreamSize);
  std::memcpy(cpu, values, sizeof(values));
}

bool BindWholeStreams(const HostVertexLayout &layout, CachedStream *const copies[kMaxVertexStreams], u32 base) {
  plume::RenderCommandList *cmd = State().command_list;
  g_bound_stream_count = 0;
  for (u32 left = layout.used_mask; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    const HostVertexStream &hs = layout.streams[stream];
    if (!hs.used)
      continue;
    // From the draw's base vertex (Geometry::whole_base; WholeGeometryLocked checked it is inside the copy).
    plume::RenderBufferReference ref = copies[stream]->copy.Ref();
    const u32 skip = base * hs.host_stride;
    ref.offset += skip;
    const plume::RenderVertexBufferView view(ref, copies[stream]->bytes - skip);
    const plume::RenderInputSlot slot(stream, hs.host_stride, plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    BindHashed(cmd, view, slot);
  }
  if (!g_bound.zero_stream) {
    g_bound.zero_stream = Video::UploadsLocked().Allocate(kZeroStreamSize, 16);
    if (!g_bound.zero_stream)
      return false;
    FillMissingInputDefaults(g_bound.zero_stream.cpu);  // keflings-nx (2026-10-30)
  }
  const plume::RenderVertexBufferView zero_view(g_bound.zero_stream.ref, kZeroStreamSize);
  const plume::RenderInputSlot zero_slot(kZeroStreamSlot, 0);
  SetVertexBuffer(cmd, zero_view, zero_slot);
  return true;
}

// banjo-nx (2026-10-23, batch 6): 'sources' (or null), the used streams' sources as DrawStreamKeys made them for the
// draw (GetStreamSource ran twice a stream).
bool BindStreams(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                 const u32 *shader_strides, const HostVertexLayout &layout, uint64_t layout_hash,
                 const Geometry &g, const GpuStreams &gpu_streams, const StreamSource *sources = nullptr) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kBindStreams);  // blur-nx (2026-11-17)
  plume::RenderCommandList *cmd = State().command_list;
  g_bound_stream_count = 0;
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_stream_mask): the used streams by their mask, in the same order (16 checks a draw
  // before, 1.3 % of the render thread in the second console run).
  static const bool stream_mask = REXCVAR_GET(banjo_renut_stream_mask);
  for (u32 left = stream_mask ? layout.used_mask : (1u << kMaxVertexStreams) - 1u; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    const HostVertexStream &hs = layout.streams[stream];
    if (!hs.used)
      continue;
    if (gpu_streams.bytes[stream]) {
      {
        static const uint64_t want = [] {
          const std::string v = REXCVAR_GET(banjo_renut_diag_stream_dump_vs);
          return v.empty() ? 0ull : uint64_t(std::strtoull(v.c_str(), nullptr, 16));
        }();
        static uint32_t seen = 0;
        if (want && vs.hash == want && (seen++ < 16 || seen % 64 == 0))
          RGPU_INFO("[streamdump] vs {:016X} stream {}: the GPU-converted exported stream ({} bytes)", vs.hash, stream,
                    gpu_streams.bytes[stream]);
      }
      const plume::RenderVertexBufferView view(gpu_streams.ref[stream], gpu_streams.bytes[stream]);
      const plume::RenderInputSlot slot(
          stream, hs.host_stride,
          g.per_instance ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                         : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
      BindHashed(cmd, view, slot);
      continue;
    }
    const StreamSource src = sources ? sources[stream] : GetStreamSource(device, call, vs, shader_strides, stream);
    // A zero stride repeats one vertex for every index.
    const bool constant = src.stride == 0;
    const u32 first = constant ? 0 : g.first_vertex;
    u32 count = constant ? 1 : g.vertex_range;
    if (first < src.vertex_count)
      count = std::min(count, src.vertex_count - first);
    else
      count = src.data ? 0 : count;
    const u32 bytes = std::max<u32>(count, 1) * hs.host_stride;
    const bool fills = src.data && count;
    const u8 *guest = fills ? src.data + size_t(first) * src.stride : nullptr;
    const bool queued = fills && !(stream == 0 && call.up_vertices);
    const StreamKey key{guest, layout_hash, stream, src.stride, count, src.endian};
    // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_lean_diagnostics): the dump's check once, not a call per stream.
    static const bool stream_dump = !REXCVAR_GET(banjo_renut_lean_diagnostics) ||
                                    !REXCVAR_GET(banjo_renut_diag_stream_dump_vs).empty();
    const bool dumped = stream_dump && StreamDumpLog(vs, stream, src, hs, guest, first, count, call);
    // bangaio-nx (2026-10-29, banjo_renut_stream_second_sighting): none on a range's first sighting; it converts below as
    // a fresh entry's draw does, uncached.
    CachedStream *const cached = queued && src.stride && !src.held ? RangeEntry(key) : nullptr;  // sasrt-nx: a kept export's is not cached
    if (cached) {
      cached->last_used = g_stream_cache_frame;
      if (cached->copy && banjo::renut_gpu::ExactStreamsOn()) {
        // banjo-nx (2026-10-17): banjo_renut_exact_streams. The copy's guest bytes hashed whole on its first use
        // after each kick; changed, it is dropped and the stream converted below, as an uncached one.
        if (cached->proven_flush != g_flush_serial &&
            CardsProve(key.guest, size_t(key.count) * key.stride, cached->proven_flush, cached->hash)) {
          cached->proven_flush = g_flush_serial;  // banjo-nx: banjo_renut_store_cards
        } else if (cached->proven_flush != g_flush_serial) {
          const bool same = ExactHash(key.guest, size_t(key.count) * key.stride) == cached->hash;
          g_card_diag[7] += same && cached->proven_flush != UINT64_MAX && banjo::renut_gpu::StoreCardsVerifyOn();
          banjo::renut_gpu::CountExactCheck(uint64_t(key.count) * key.stride, !same,
                                            cached->exact_frame == g_stream_cache_frame);
          cached->exact_frame = g_stream_cache_frame;
          if (same) {
            cached->proven_flush = g_flush_serial;
          } else {
            DropCachedCopy(*cached);
            cached->stable = 0;
            cached->checked_frame = UINT64_MAX;
            cached->dynamic = ++cached->drops >= kMaxCacheDrops;
          }
        }
      } else if (cached->copy && CacheCheckDue(cached->checked_frame)) {
        cached->checked_frame = g_stream_cache_frame;
        g_stream_verifier.Submit(key, cached->hash);
      }
      if (cached->copy) {
        if (dumped)
          RGPU_INFO("[streamdump] stream {}: bound the cached copy ({} bytes)", stream, cached->bytes);
        const plume::RenderVertexBufferView view(cached->copy.Ref(), cached->bytes);
        const plume::RenderInputSlot slot(
            stream, hs.host_stride,
            g.per_instance ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                           : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
        BindHashed(cmd, view, slot);
        continue;
      }
    }
    bool older = false;
    const UploadAllocation *queued_alloc = queued ? FindQueuedStream(key, &older) : nullptr;
    // banjo-nx (2026-10-23, batch 6, banjo_renut_kick_reuse): verify mode converts a kept stream again and compares.
    if (queued_alloc && older && banjo::renut_gpu::StoreCardsVerifyOn()) {
      static std::vector<u8> fresh;
      fresh.resize(bytes);
      ConvertVertices(hs, guest, src.stride, count, src.endian, fresh.data());
      if (queued_alloc->size < bytes || std::memcmp(fresh.data(), queued_alloc->cpu, bytes) != 0) {
        banjo::renut_gpu::CountKickReuse(2, bytes);
        queued_alloc = nullptr;
      }
    }
    const bool reused = queued_alloc != nullptr;
    UploadAllocation alloc = reused ? *queued_alloc : Video::UploadsLocked().Allocate(bytes, 16);
    if (!alloc)
      return false;
    // Conversion writes every vertex; only an unfilled buffer needs zeros.
    if (!fills)
      std::memset(alloc.cpu, 0, bytes);
    if (fills && !reused) {
      if (call.up_vertices && stream == 0) {
        // A copy taken at the call (queued.cpp).
        if (::renut::gpu::host_batch::CensusOn())  // blur-nx (2026-11-18, the census's #12)
          ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kConvertUpBytes, uint64_t(count) * hs.host_stride);
        ConvertVertices(hs, guest, src.stride, count, src.endian, static_cast<u8 *>(alloc.cpu));
      } else {
        PendingStreamFill fill;
        fill.host_stride = hs.host_stride;
        fill.first_element = u32(g_fill_elements.size());
        fill.element_count = u32(hs.elements.size());
        g_fill_elements.insert(g_fill_elements.end(), hs.elements.begin(), hs.elements.end());
        fill.guest = guest;
        fill.stride = src.stride;
        fill.count = count;
        fill.endian = src.endian;
        fill.host = static_cast<u8 *>(alloc.cpu);
        fill.key = key;
        fill.cacheable = cached && !cached->dynamic;
        g_stream_fills.push_back(fill);
        AddQueuedStream(key, alloc);
      }
    }
    const plume::RenderVertexBufferView view(alloc.ref, bytes);
    const plume::RenderInputSlot slot(stream, constant ? 0 : hs.host_stride,
                                      g.per_instance
                                          ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                                          : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    BindHashed(cmd, view, slot);
  }

  if (!g_bound.zero_stream) {
    g_bound.zero_stream = Video::UploadsLocked().Allocate(kZeroStreamSize, 16);
    if (!g_bound.zero_stream)
      return false;
    FillMissingInputDefaults(g_bound.zero_stream.cpu);  // keflings-nx (2026-10-30)
  }
  const plume::RenderVertexBufferView zero_view(g_bound.zero_stream.ref, kZeroStreamSize);
  const plume::RenderInputSlot zero_slot(kZeroStreamSlot, 0);
  SetVertexBuffer(cmd, zero_view, zero_slot);
  return true;
}

// banjo-nx (2026-10-23, render-thread round): the stream cache keys BindStreams will look up for this draw (its strided
// streams read from guest memory), as it makes them, for prefetching. Returns how many.
u32 DrawStreamKeys(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs, const u32 *shader_strides,
                   const HostVertexLayout &layout, uint64_t layout_hash, const Geometry &g,
                   StreamKey keys[kMaxVertexStreams], StreamSource sources[kMaxVertexStreams]) {
  u32 n = 0;
  for (u32 left = layout.used_mask; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    if (!layout.streams[stream].used)
      continue;
    const StreamSource src = sources[stream] = GetStreamSource(device, call, vs, shader_strides, stream);
    if (!src.stride)
      continue;
    const u32 first = g.first_vertex;
    u32 count = g.vertex_range;
    if (first < src.vertex_count)
      count = std::min(count, src.vertex_count - first);
    else
      count = src.data ? 0 : count;
    if (!src.data || !count || (stream == 0 && call.up_vertices))
      continue;
    keys[n++] = StreamKey{src.data + size_t(first) * src.stride, layout_hash, stream, src.stride, count, src.endian};
  }
  return n;
}

// The vertex inputs the shader reads from its declaration by an index other
// than the vertex index (vfetchBound): each described from the declaration's
// element for it. Returns the fetch constants (bit i = index 64 + i) whose
// buffers those reads need copied.
u32 DescribeBoundFetches(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                         const VertexDeclarationRecord &declaration, const u32 *shader_strides,
                         SharedConstants &sc, u32 *entries = nullptr) {
  // banjo-nx (2026-10-19, render-thread proposal 21; 2026-10-23 from the shader's record, DrawSummaryOf): whether the
  // shader has any input fetched by another index: most have none, and their draws skip the scan of cold input records.
  if (!vs.entry || !DrawSummaryOf(vs).bound_fetches)
    return 0;
  const ShaderVertexInput *inputs = g_activeShaderVertexInputs + vs.entry->vertexInputOffset;
  const u32 count = std::min<u32>(vs.entry->vertexInputCount, kMaxBoundFetches);
  u32 mask = 0;
  for (u32 i = 0; i < count; ++i) {
    if (!(inputs[i].flags & kVertexInputBoundFetch))
      continue;
    for (const GuestVertexElement &e : declaration.elements) {
      if (e.Usage != inputs[i].usage || e.UsageIndex != inputs[i].usageIndex)
        continue;
      const u32 stream = e.Stream;
      if (stream >= kMaxVertexStreams)
        break;
      const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
      const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
      sc.bound_fetch[i][0] = fetch_index;
      sc.bound_fetch[i][1] = u32(e.Offset) / 4;
      sc.bound_fetch[i][2] = src.stride / 4;
      sc.bound_fetch[i][3] = BoundDeclType(u32(e.Type)) & 0x3FFFFFu;
      mask |= 1u << (fetch_index - 64);
      if (entries)
        *entries |= 1u << i;  // banjo-nx (2026-10-26)
      break;
    }
  }
  return mask;
}

bool FetchFromExportWindow(const GuestDevice &device, const DrawCall &call, u32 fetch_index,
                           u32 &slot, uint64_t &offset);
bool EnsureBuffer(std::unique_ptr<plume::RenderBuffer> &buffer, uint64_t &capacity, uint64_t size,
                  const plume::RenderBufferDesc &desc_template, const char *name);

// Copies the buffers of the fetch constants the vertex shader reads by address
// into one allocation and fills their SharedConstants slots. A buffer whose
// newest data is in a pending export's window comes from there instead: then
// the whole fetch data is put together on the GPU, in the slot's
// gpu_fetch_data, and the returned allocation's ref and gpu_address point
// there. Records copies: before the framebuffer is bound.
// banjo-nx (2026-10-18, the deko3d water): set by DrawStateLocked for draws of banjo_renut_diag_stream_dump_vs's vertex
// shader; UploadVertexFetchData then logs its fetch copies.
bool g_dump_fetch = false;

UploadAllocation UploadVertexFetchData(const GuestDevice &device, const DrawCall &call, u32 mask,
                                       SharedConstants &sc) {
  struct Copy {
    u32 index;
    const u8 *src;
    u32 size;
    u32 endian;
    bool exported;        // from the export window below
    u32 export_slot;
    uint64_t export_offset;
  };
  Copy copies[32];
  u32 copy_count = 0;
  u32 total = 16;
  bool any_exported = false;
  const u32 up_fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(0);
  for (u32 bits = mask; bits; bits &= bits - 1) {  // banjo-nx (2026-10-23): the set bits, in the same order
    const u32 index = 64 + u32(std::countr_zero(bits));
    Copy c{index, nullptr, 0, 0, false, 0, 0};
    const bool cp_fetch = call.tessellation && index == g_cp_fetch_index;  // vivapinata-nx: the control points' index buffer
    if (cp_fetch) {
      c.src = g_cp_fetch.src;
      c.size = g_cp_fetch.size;
      c.endian = g_cp_fetch.endian;
    } else if (index == up_fetch_index && call.up_vertices) {
      c.src = call.up_vertices;
      c.size = call.up_vertex_count * call.up_stride;
      c.endian = kUpEndian;
    } else {
      const u32 w0 = device.VertexFetchWord(index, 0);
      const u32 w1 = device.VertexFetchWord(index, 1);
      c.src = (w0 & ~3u) ? GuestPhysicalPtr<const u8>(w0 & ~3u) : nullptr;
      c.size = w1 & 0x3FFFFFCu;
      c.endian = w1 & 3;
    }
    if (!c.src)
      c.size = 0;
    c.size = std::min(c.size, kMaxFetchCopyBytes);
    // banjo-nx (2026-10-28, cache-miss audit 2): the fetch cache's entry for it (FetchPoolCopy's first load was a miss
    // at most fetch draws) and the cards its proof reads, while the other copies are gathered.
    if (c.size && !(index == up_fetch_index && call.up_vertices)) {
      const char *pe = reinterpret_cast<const char *>(&FetchPoolFor(c.src, c.size));
      __builtin_prefetch(pe);
      __builtin_prefetch(pe + 64);  // (its cards no longer: trimmed after the d4 profile)
    }
    if (c.size && !cp_fetch && FetchFromExportWindow(device, call, index, c.export_slot, c.export_offset))
      any_exported = c.exported = true;
    copies[copy_count++] = c;
    total += (c.size + 15u) & ~15u;
  }

  // banjo-nx (2026-10-20, banjo_deko3d_skin_once): the draw's fetch copies, for FurSkinLocked's key (the bone data
  // by its guest bytes: each kick copies it again, to another offset).
  g_fetch_record.count = copy_count;
  g_fetch_record.unusable = any_exported || call.up_vertices;
  for (u32 i = 0; i < copy_count; ++i)
    g_fetch_record.copies[i] = {copies[i].index, copies[i].src, copies[i].size, copies[i].endian};

  // Without exports the slots count from the start of the upload chunk, so a
  // buffer an earlier draw has queued a copy of in the same chunk is read
  // there (FetchCopy) and not copied again.
  const bool shared = !any_exported;
  // banjo-nx (2026-10-27, banjo_renut_fetch_cache / A/B 47): every buffer kept in the fetch cache, in one pool chunk:
  // the chunk is the draw's fetch data and the slots count from its start. Not for a draw with Draw*UP data in a fetch
  // or a fetch of no bytes (the ring's slot for it points at the draw's other copies; the pool's would not).
  g_fetch_from_pool = false;
  if (shared && copy_count && FetchCacheOn(banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed))) {
    u32 offsets[32];
    uint64_t base = 0, chunk_bytes = 0;
    u8 *chunk_cpu = nullptr;
    const plume::RenderBuffer *chunk_buffer = nullptr;
    u32 chunk = UINT32_MAX;
    bool all = true;
    for (u32 i = 0; i < copy_count && all; ++i) {
      const Copy &c = copies[i];
      if (!c.size || (c.index == up_fetch_index && call.up_vertices)) {
        all = false;
        break;
      }
      const FetchPoolEntry *e = FetchPoolCopy(c.src, c.size);
      all = e && (chunk == UINT32_MAX || e->copy.chunk == chunk);
      if (all) {
        // (values now: a later buffer of this draw may take the same entry)
        chunk = e->copy.chunk;
        offsets[i] = u32(e->copy.offset);
        base = e->base;
        chunk_bytes = e->chunk_bytes;
        chunk_cpu = e->copy.cpu - e->copy.offset;
        chunk_buffer = e->copy.buffer;
      }
    }
    if (all && chunk_cpu) {
      static const bool verify = REXCVAR_GET(banjo_renut_fetch_cache_verify);
      u32 bytes = 0;
      for (u32 i = 0; i < copy_count; ++i) {
        const Copy &c = copies[i];
        sc.vertex_fetch_slots[c.index] = offsets[i] | c.endian;
        bytes += c.size;
        if (verify && std::memcmp(chunk_cpu + offsets[i], c.src, c.size) != 0) {
          ++g_fetch_pool_differ;
          static uint32_t logged = 0;
          if (logged++ < 16)
            RGPU_WARN("banjo_renut_fetch_cache_verify: fetch {} ({} bytes) kept in the pool differs from guest memory",
                      c.index, c.size);
        }
      }
      UploadAllocation alloc;
      alloc.ref = plume::RenderBufferReference(const_cast<plume::RenderBuffer *>(chunk_buffer), 0);
      alloc.cpu = chunk_cpu;
      alloc.gpu_address = base;
      alloc.chunk_size = chunk_bytes;
      sc.vertex_fetch_data = base;
      sc.vertex_fetch_bytes = u32(std::min<uint64_t>(chunk_bytes, 0xFFFFFFF0ull));
      ++g_fetch_pool_draws;
      g_fetch_pool_bytes_used += bytes;
      g_fetch_from_pool = true;
      return alloc;
    }
  }
  // banjo-nx (2026-10-19, render-thread proposal 10): room only for the copies to make: those already queued in the
  // chunk the allocation goes to are read there (before, their room was taken anyway, and cleaned at the submit). If
  // the allocation lands in another chunk, none is (that chunk has none of them): then room for all.
  bool reused[32] = {};
  // mk9-nx (2026-10-21; port 56): the queued copy's offset, taken when it is found. g_fetch_copies is direct-mapped: a
  // copy this draw makes below for another buffer whose key lands on the same entry overwrites it, and reading the entry
  // again then gave the reused stream the other buffer's offset (two streams of a draw with one slot, the other's bytes
  // decoded as positions: mk9-nx's huge flat triangles on the Switch, where host pointers collide differently).
  u32 reused_offset[32] = {};
  u32 needed = total;
  const plume::RenderBuffer *chunk = shared ? Video::UploadsLocked().CurrentBuffer() : nullptr;
  if (shared && chunk) {
    needed = 16;
    for (u32 i = 0; i < copy_count; ++i) {
      const Copy &c = copies[i];
      if (c.size && !c.exported && !(c.index == up_fetch_index && call.up_vertices)) {
        // banjo-nx (2026-10-23, batch 6): or kept from an earlier flush of the frame (FetchCopyHeld).
        FetchCopy &known = FetchCopyFor(c.src, c.size);
        reused[i] = FetchCopyHeld(known, c.src, c.size, chunk);
        reused_offset[i] = known.offset;
      }
      if (!reused[i])
        needed += (c.size + 15u) & ~15u;
    }
  }
  UploadAllocation alloc = Video::UploadsLocked().Allocate(needed, 256);
  if (alloc && shared && chunk && alloc.ref.ref != chunk) {
    std::fill(std::begin(reused), std::end(reused), false);
    if (needed < total)
      alloc = Video::UploadsLocked().Allocate(total, 256);
  }
  if (!alloc)
    return alloc;
  const uint64_t chunk_offset = shared ? alloc.ref.offset : 0;
  u32 offset = 0;
  for (u32 i = 0; i < copy_count; ++i) {
    const Copy &c = copies[i];
    u8 *dst = static_cast<u8 *>(alloc.cpu) + offset;
    u32 at = u32(chunk_offset) + offset;
    if (reused[i]) {
      // Queued in this chunk by an earlier draw: read there, no room here.
      sc.vertex_fetch_slots[c.index] = reused_offset[i] | c.endian;  // port 56: as found (see reused_offset)
      continue;
    }
    if (c.size && !c.exported) {
      if (c.index == up_fetch_index && call.up_vertices) {
        std::memcpy(dst, c.src, c.size);
      } else if (shared) {
        FetchCopy &known = FetchCopyFor(c.src, c.size);
        if (FetchCopyHeld(known, c.src, c.size, alloc.ref.ref)) {
          at = known.offset;
        } else {
          g_copy_fills.push_back({c.src, dst, c.size});
          known = {c.src, c.size, g_fetch_copy_generation, alloc.ref.ref, at, g_flush_serial + 1, UploadRing::Resets(),
                   0, dst};
        }
      } else {
        g_copy_fills.push_back({c.src, dst, c.size});
      }
    }
    sc.vertex_fetch_slots[c.index] = at | c.endian;
    if (g_dump_fetch) {
      static uint32_t logged = 0;
      if (logged++ < 48) {
        u32 w[4] = {};
        if (c.src && c.size >= 16)
          std::memcpy(w, c.src, 16);
        RGPU_INFO("[fetchdump] fetch {} src {} size {} endian {} exported {} (slot {} offset {}) shared {} at {} chunk "
                  "offset {} guest {:08X} {:08X} {:08X} {:08X}",
                  c.index, fmt::ptr(c.src), c.size, c.endian, c.exported, c.export_slot, c.export_offset, shared, at,
                  chunk_offset, w[0], w[1], w[2], w[3]);
      }
    }
    offset += (c.size + 15u) & ~15u;
  }
  if (shared) {
    alloc.ref.offset = 0;
    alloc.gpu_address = alloc.gpu_address ? alloc.gpu_address - chunk_offset : 0;
    sc.vertex_fetch_data = alloc.gpu_address;
    // banjo-nx: the whole chunk from its start (the base), all of it allocated memory: the same for every draw of the
    // chunk, so draws keep the same SharedConstants (banjo_renut_shared_dedupe). 0 for a draw with no copies (its
    // shaders fetch nothing from memory).
    sc.vertex_fetch_bytes = copy_count ? u32(std::min<uint64_t>(alloc.chunk_size, 0xFFFFFFF0ull)) : 0u;
    return alloc;
  }
  sc.vertex_fetch_data = alloc.gpu_address;
  sc.vertex_fetch_bytes = copy_count ? total : 0u;  // banjo-nx: the region below, from its start

  // The guest buffers' copies from the upload, the exported ones from their
  // windows (raw bytes, as guest memory would hold them), each into its own
  // part of the region.
  auto &s = State();
  uint64_t &used = s.gpu_fetch_data_used[s.frame];
  if (!s.gpu_fetch_data[s.frame] || used + total > s.gpu_fetch_data_capacity[s.frame]) {
    // The draws before still read the old buffer, retired with the slot.
    if (!EnsureBuffer(s.gpu_fetch_data[s.frame], s.gpu_fetch_data_capacity[s.frame], used + total,
                      plume::RenderBufferDesc::DefaultBuffer(0, AddressableBufferFlags()),
                      "renut gpu fetch data")) {
      return UploadAllocation{};
    }
    used = 0;
  }
  plume::RenderBuffer *dest = s.gpu_fetch_data[s.frame].get();
  const uint64_t region = used;
  used = (used + total + 255u) & ~uint64_t(255);
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier to_copy[kFramesInFlight + 1];
  u32 barrier_count = 0;
  to_copy[barrier_count++] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE);
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (s.memexport_buffer[slot]) {
      to_copy[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::READ);
    }
  }
  // sasrt-nx (2026-11-08, sasrt_gpu_fetch_copy_compute; the GPU round): on deko3d the copies are the 3D engine's
  // (fetch_copy_cs): its own work waited for before (the exports' writes) and after (the vertex shader's reads), not the
  // whole GPU with the copy engine; and the vertex shader's reads no longer race the copy engine, which the barrier
  // after the copies (a wait for the 3D engine) did not wait for. The same words land in the same places.
  if (IsDeko3D() && s.fetch_copy_pipeline && sasrt::gpu_round::On(sasrt::gpu_round::kFetchCopyCompute)) {
    plume::RenderBuffer *const fallback = dest;  // a slot without an export buffer: never read
    const plume::RenderBuffer *buffers[kFetchCopyBuffers] = {
        s.memexport_buffer[0] ? s.memexport_buffer[0].get() : fallback,
        kFramesInFlight > 1 && s.memexport_buffer[1] ? s.memexport_buffer[1].get() : fallback,
        alloc.ref.ref ? alloc.ref.ref : fallback, dest};
    std::unique_ptr<plume::RenderDescriptorSet> &set = s.fetch_copy_set[s.frame];
    if (!set || std::memcmp(buffers, s.fetch_copy_set_buffers[s.frame], sizeof(buffers)) != 0) {
      if (set)
        Video::RetireLocked(std::move(set));
      set = detail::CreateFetchCopySet(s);
      if (set) {
        for (u32 i = 0; i < kFetchCopyBuffers; ++i)
          set->setBuffer(i, const_cast<plume::RenderBuffer *>(buffers[i]));
        std::memcpy(s.fetch_copy_set_buffers[s.frame], buffers, sizeof(buffers));
      }
    }
    bool aligned = set != nullptr && (alloc.ref.offset & 3) == 0;
    for (u32 i = 0; aligned && i < copy_count; ++i)
      aligned = !copies[i].exported || (copies[i].export_offset & 3) == 0;
    if (aligned) {
      cmd->barriers(plume::RenderBarrierStage::COMPUTE, to_copy, barrier_count);
      cmd->setComputePipelineLayout(s.fetch_copy_layout.get());
      cmd->setPipeline(s.fetch_copy_pipeline.get());
      cmd->setComputeDescriptorSet(set.get(), 0);
      offset = 0;
      for (u32 i = 0; i < copy_count; ++i) {
        const Copy &c = copies[i];
        if (c.size) {
          FetchCopyPush push{};
          push.source = c.exported ? c.export_slot : 2u;
          push.source_offset = c.exported ? u32(c.export_offset) : u32(alloc.ref.offset + offset);
          push.dest_offset = u32(region + offset);
          push.words = (c.size + 3u) / 4u;
          cmd->setComputePushConstants(0, &push, 0, sizeof(push));
          cmd->dispatch((push.words + 63u) / 64u, 1, 1);
        }
        offset += (c.size + 15u) & ~15u;
      }
      to_copy[0] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ);
      for (u32 i = 1; i < barrier_count; ++i)
        to_copy[i].accessBits = plume::RenderBufferAccess::WRITE;
      cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_copy, barrier_count);
      sasrt::gpu_round::Count(sasrt::gpu_round::kFetchCopyCompute);
      // The compute pipeline replaced the graphics one, as in ConvertExportedStreamsLocked.
      g_bound.pipeline = nullptr;
      g_bound.framebuffer = nullptr;
      alloc.ref = plume::RenderBufferReference(dest, region);
      alloc.gpu_address = dest->getDeviceAddress() + region;
      sc.vertex_fetch_data = alloc.gpu_address;
      return alloc;
    }
  }
  cmd->barriers(plume::RenderBarrierStage::COPY, to_copy, barrier_count);
  offset = 0;
  for (u32 i = 0; i < copy_count; ++i) {
    const Copy &c = copies[i];
    if (c.size) {
      const plume::RenderBufferReference to(dest, region + offset);
      if (c.exported) {
        cmd->copyBufferRegion(
            to, plume::RenderBufferReference(s.memexport_buffer[c.export_slot].get(), c.export_offset),
            c.size);
      } else {
        cmd->copyBufferRegion(
            to, plume::RenderBufferReference(alloc.ref.ref, alloc.ref.offset + offset), c.size);
      }
    }
    offset += (c.size + 15u) & ~15u;
  }
  // Read by the vertex shader (as a root SRV on D3D12).
  to_copy[0] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ);
  for (u32 i = 1; i < barrier_count; ++i)
    to_copy[i].accessBits = plume::RenderBufferAccess::WRITE;
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, to_copy, 1);
  if (barrier_count > 1)
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_copy + 1, barrier_count - 1);
  g_bound.framebuffer = nullptr;  // the copies end any render pass
  alloc.ref = plume::RenderBufferReference(dest, region);
  alloc.gpu_address = UsesVulkanBindings() ? dest->getDeviceAddress() + region : 0;  // banjo-nx: and deko3d
  sc.vertex_fetch_data = alloc.gpu_address;
  return alloc;
}

// ---------------------------------------------------------------------------

// banjo-nx (banjo_gpu_bool_spec): the bools a shader's SPIR-V specialises, read once.
const u32 *BoolSpecMask(const ShaderRecord *r) {
  if (!r->bool_spec_read) {
    r->bool_spec_read = true;
    if (r->entry)
      ShaderBoolSpecMask(*r->entry, r->bool_spec_mask);
  }
  return r->bool_spec_mask;
}

// vivapinata-nx (2026-11-03, banjo-nx's banjo_gpu_fetch_spec): a tessellated draw's PipelineKey::fetch_types and tess_kind, the words
// its vertex shader reads at run time (SharedConstants.bound_fetch[k][3] and tessellation[0], both written before the key is
// made): each + 1, 0 for none. The types only for a vertex shader with bound fetches. The pipeline caches that skip MakeKey
// compare these too (a declaration's layout hash names its host formats, not its D3D types).
struct FetchSpec {
  u32 types[4];
  u32 kind;
};
// vivapinata-nx (2026-11-12, the shader round; A/B 150): with vivapinata_shader_round the types for every draw whose vertex
// shader has bound fetches, not only the tessellated ones (DescribeBoundFetches wrote them before both key paths call
// this): such a shader decodes its vertex data from memory in every format its fetches could have (98381523's vertex
// shaders: 1,561 instructions, about 205 with the type known). The off half of A/B 150: as before (tessellated only).
FetchSpec FetchSpecWords(const SharedConstants &sc, const ShaderRecord &vs, bool per_instance, int ab_off) {
  FetchSpec s{};
  if (!banjo::renut_gpu::FetchSpecOn() || ab_off == 83)  // A/B 83 here (69 in vivapinata-nx)
    return s;
  static const bool round_on = REXCVAR_GET(vivapinata_shader_round);
  if (per_instance)
    s.kind = sc.tessellation[0] + 1;
  else if (!round_on || ab_off == 150 || !IsDeko3D())  // deko3d's GLSL has the types (pass fetchspec); Vulkan's not
    return s;
  if (DrawSummaryOf(vs).bound_fetches) {
    for (u32 k = 0; k < 4; ++k)
      s.types[k] = sc.bound_fetch[k][3] + 1;
  }
  return s;
}
bool FetchSpecDiffers(const PipelineKey &key, const FetchSpec &s) {
  return key.tess_kind != s.kind || std::memcmp(key.fetch_types, s.types, sizeof(s.types)) != 0;
}

// sasrt-nx (2026-11-08, sasrt_gpu_clip_spec; the second GPU round): PipelineKey::reserved bit 3, set when the draw's
// vertex shader must write its user clip distance: a plane is set (SetUserClipPlane wrote it into the shared constants
// before the key is made), or the key is off (constant 929 stays 1, the clip slot as before).
inline u32 SasrtClipBit(const SharedConstants &sc) {
  if (!sasrt::gpu_round::On(sasrt::gpu_round::kClipSpec))
    return 8u;
  const float *p = sc.user_clip_planes[0];
  return p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f || p[3] != 0.0f ? 8u : 0u;
}

// sasrt-nx (2026-11-08, sasrt_gpu_plain_fetches; the GPU round): the pixel shader's constant 926, bit r for each fetch
// constant it samples (its entry's textureFetchMask) whose words for this draw have no signs or gamma (sampler_indices
// bits 13-19) and no exponent adjust (texture_indices bits 20-25, of every dimension): the shaders' fetch code skips its
// sign, gamma and exponent block exactly then (tools/deko3d_glsl_opt.py pass plainfetch). From the draw's
// SharedConstants after FillTextures; set on the key after it is made or reused (the key's other inputs do not decide
// it). 0 off deko3d, without a pixel shader, or with the key off.
// sasrt-nx (2026-11-08, sasrt_gpu_one_x): bit 31 (fetch constant 31 is never a pixel shader's, so never plain here): the
// draw's targets are 1x (SharedConstants.resolution_scale) and every texture word its pixel shader samples has scale
// code 0 (bits 26-28, kResolutionScaleShift): tools/deko3d_glsl_opt.py pass onex folds both.
// vivapinata-nx (2026-11-12, the shader round): with vivapinata_shader_round, the vertex shader's fetches too (vs_entry):
// the same words (SharedConstants is the draw's, whichever stage samples a fetch constant), so bit r says the same of
// fetch constant r for both stages, and bit 31 also needs the vertex shader's words at scale code 0. The vertex shaders'
// passes act on it only with constant 975 (A/B 150). The grass shells' tessellated vertex shader samples its heightmap
// three times through fetch constant 16, which no pixel shader of theirs samples.
[[gnu::always_inline]] inline u32 SasrtPlainFetchMask(const SharedConstants &sc, const ShaderCacheEntry *ps_entry,
                                                      const ShaderCacheEntry *vs_entry = nullptr) {
  if (!ps_entry || !IsDeko3D())
    return 0;
  const bool plain_on = sasrt::gpu_round::On(sasrt::gpu_round::kPlainFetches);
  const bool one_x_on = sasrt::gpu_round::On(sasrt::gpu_round::kOneX);
  if (!plain_on && !one_x_on)
    return 0;
  static const bool round_on = REXCVAR_GET(vivapinata_shader_round);
  const u32 fetches = ps_entry->textureFetchMask | (round_on && vs_entry ? vs_entry->textureFetchMask : 0u);
  u32 mask = 0, codes = 0;
  for (u32 bits = fetches & 0x7FFFFFFFu; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    const u32 words = sc.texture_indices[0][r] | sc.texture_indices[1][r] | sc.texture_indices[2][r];
    const u32 any = (sc.sampler_indices[r] & 0x000FE000u) | (words & 0x03F00000u);
    mask |= u32(any == 0) << r;
    codes |= words & 0x1C000000u;
  }
  if (!plain_on)
    mask = 0;
  if (one_x_on && codes == 0 && sc.resolution_scale == 1.0f && !(fetches >> 31))
    mask |= 1u << 31;
  return mask;
}

// sasrt-nx (2026-11-08, the third GPU round: sasrt_gpu_vs_screen_space_spec, sasrt_gpu_vs_depth_remap_spec,
// sasrt_gpu_stacked_3d_spec, sasrt_gpu_stacked_filter_spec, sasrt_gpu_fetch_endian_spec): PipelineKey::sasrt_words, from
// the draw's SharedConstants as its shaders read them (set after FillTextures, the fetch uploads and the viewport, as
// fetch_pad):
//   [0] the vertex shader's constant 932: bit 0 set: bit 1 is screen_space != 0; bit 2 set: bit 3 is depth_remap[0] != 0
//   [1] the pixel shader's 934: bit 31 set: bit r is fetch constant r's 3D word's stacked bit (kStackedTextureBit)
//   [2] / [3] 935 / 936: the same for the layer filter (kStackedLayerLinearBit) and Z wrap (kStackedLayerWrapBit)
//   [4] 937: bit i set: fetch slot 64 + i (one the vertex shader reads by address) has its byte order (the slot word's
//       bits 0-1) in [5] (938, i < 16) or [6] (939) at bits 2 * (i % 16)
// [1]-[3] only when the pixel shader samples a 3D word that is not the null slot's, and not fetch constant 31 (bit 31).
// 0: read at run time (the generic pipeline, which a key not built yet draws with).
//   [8] 940: bit r: float constant r < 31 is an export address register (memExportConstants) whose z word as the
//       vertex shader's constant buffer holds it (its literal's if it defines one; byte-swapped and NaN-flushed as
//       CopyAndSwapGroup uploads it) is [9], [10] or [11] (941-943), by its order among the set bits (three at most)
struct SasrtSpecWords {
  u32 w[20];  // sasrt-nx (2026-11-09, the fifth round): [12]-[19] constants 950-973
};
static_assert(sizeof(SasrtSpecWords::w) == sizeof(PipelineKey::sasrt_words));
// sasrt-nx (2026-11-08, sasrt_gpu_export_format_spec): [8]-[11] (cold: export shaders only).
[[gnu::noinline]] void SasrtExportFormatWords(SasrtSpecWords &s, const ShaderCacheEntry &vs, const FloatConstants &vsc) {
  u32 n = 0;
  for (u32 bits = vs.memExportConstants[0] & 0x7FFFFFFFu; bits && n < 3; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    bool literal = false;
    u32 z = 0;
    for (u32 i = 0; i < vs.literalCount; ++i) {
      const ShaderLiteral &l = g_activeShaderLiterals[vs.literalOffset + i];
      if (l.reg == r) {
        z = l.value[2];
        literal = true;
      }
    }
    if (!literal) {
      z = reinterpret_cast<const be_u32 *>(vsc.Register(r))[2];
      if (banjo::renut_gpu::ConstNanFlushOn() && (z & 0x7F800000u) == 0x7F800000u)
        z = 0;
    }
    // Only a zero or normal float: a denormal, infinity or NaN could fold at compile time otherwise than the GPU
    // computes it at run time (flush, canonical NaN); that register stays read at run time.
    const u32 e = z & 0x7F800000u;
    if (z != 0 && (e == 0 || e == 0x7F800000u)) {
      sasrt::gpu_round::Count(sasrt::gpu_round::kExportFmtSpec, 1, 1);  // left at run time (the 10 s line's /n)
      continue;
    }
    s.w[8] |= 1u << r;
    s.w[9 + n++] = z;
    sasrt::gpu_round::Count(sasrt::gpu_round::kExportFmtSpec);
  }
}
[[gnu::always_inline]] inline SasrtSpecWords SasrtSpecWordsOf(const SharedConstants &sc, const ShaderCacheEntry *vs_entry,
                                                              const ShaderCacheEntry *ps_entry,
                                                              const FloatConstants *vsc = nullptr,
                                                              const Targets *targets = nullptr) {
  SasrtSpecWords s{};
  if (!IsDeko3D())
    return s;
  // dishonored-nx (2026-10-31, dishonored_renut_clamp_7e3; port 106): [7] bits 28-31, colour target i a
  // 2_10_10_10_FLOAT (7e3) one with colour exponent bias 0 (RB_COLOR_INFO format 3, or 12,
  // 2_10_10_10_FLOAT_AS_16_16_16_16, the same storage on the Xenos and here: render/format.cpp; dishonored-nx: 3 only;
  // bits 20-25 zero): the pixel shader clamps that output to the 7e3 range (constant 990, tools/deko3d_glsl_opt.py
  // clamp7e3). The host target is 16-bit float: Dishonored's negative light passes stayed negative (black walls) and
  // its highlights far above 31.875 (a washed-out frame). With a bias the Xenos clamps the biased value, which Renut
  // scales only at the resolve: left unclamped, as dishonored-nx does. In the spec words, not the key's reserved bits
  // (all of them taken), so the fast paths' word compares see it.
  if (ps_entry && targets && banjo::renut_gpu::Clamp7e3On()) {
    for (u32 i = 0; i < targets->color_count && i < 4; ++i) {
      const HostSurface *c = targets->colors[i];
      if (c && (((c->color_info_word >> 16) & 0xFu) == 3u || ((c->color_info_word >> 16) & 0xFu) == 12u) &&
          ((c->color_info_word >> 20) & 0x3Fu) == 0u)
        s.w[7] |= 0x10000000u << i;
    }
  }
  if (vs_entry && vsc && vs_entry->memExportConstants[0] && sasrt::gpu_round::On(sasrt::gpu_round::kExportFmtSpec))
    [[unlikely]] SasrtExportFormatWords(s, *vs_entry, *vsc);
  // sasrt-nx (2026-11-08, sasrt_gpu_export_window_ubo; the fourth GPU round): [7] bit 2, a memexport vertex shader's export
  // windows read from the constant buffer (constant 944, pass expwin).
  if (vs_entry && vs_entry->memExportConstants[0] && sasrt::gpu_round::On(sasrt::gpu_round::kExportWindowUbo)) {
    s.w[7] |= 4u;
    sasrt::gpu_round::Count(sasrt::gpu_round::kExportWindowUbo);
  }
  // sasrt-nx (2026-11-09, sasrt_gpu_exp_export_parts): an EXPERIMENT (pixels change): [7] bits 3-5, constant 945.
  if (vs_entry && vs_entry->memExportConstants[0]) {
    static const int32_t parts = REXCVAR_GET(sasrt_gpu_exp_export_parts);
    s.w[7] |= u32(parts & 31) << 3;
  }
  // sasrt-nx (2026-11-08, sasrt_gpu_gpr_cap, sasrt_gpu_gpr_cap_filter): [7] the listed pixel shaders' register cap.
  if (ps_entry) {
    const uint64_t h = ps_entry->hash;
    if ((h == 0x364C9BF8579CE602ull || h == 0xF646B7A8A3A7218Bull || h == 0x92D1F7E40EBB8E86ull) &&
        sasrt::gpu_round::On(sasrt::gpu_round::kGprCap))
      s.w[7] |= 1u;
    else if (h == 0x9041AFF62011FC58ull && sasrt::gpu_round::On(sasrt::gpu_round::kGprCapFilter))
      s.w[7] |= 2u;
    // vivapinata-nx (2026-11-12, the shader round; A/B 150): [7] bit 8, the pixel shader built with constant 975 = 0 (the
    // shader round's passes as before them) when vivapinata_shader_round is off or in the A/B's off half.
    static const bool round_on = REXCVAR_GET(vivapinata_shader_round);
    if (!round_on || banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 150)
      s.w[7] |= 0x100u;
    // vivapinata-nx (2026-11-12, the shader round; A/B 151): [7] bit 9, the pixel shader built with constant 976 = 0 (its
    // early alpha test skipped, tools/deko3d_glsl_opt.py noearly) when vivapinata_shader_noearly is on, but in the A/B's
    // off half.
    static const bool noearly_on = REXCVAR_GET(vivapinata_shader_noearly);
    if (noearly_on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 151)
      s.w[7] |= 0x200u;
    // vivapinata-nx (2026-11-12, A/B 152): [7] bit 10, the pixel shader with constant 977 = 1 (its alpha test moved up
    // ahead of the colour arithmetic, tools/deko3d_glsl_opt.py earlykill) when vivapinata_shader_earlykill is on, but in
    // the A/B's off half.
    static const bool earlykill_on = REXCVAR_GET(vivapinata_shader_earlykill);
    if (earlykill_on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 152)
      s.w[7] |= 0x400u;
  }
  if (vs_entry && vs_entry->vertexFetchMemoryMask && sasrt::gpu_round::On(sasrt::gpu_round::kFetchEndianSpec)) {
    for (u32 bits = vs_entry->vertexFetchMemoryMask; bits; bits &= bits - 1) {
      const u32 i = u32(std::countr_zero(bits));
      s.w[4] |= 1u << i;
      s.w[5 + i / 16] |= (sc.vertex_fetch_slots[64 + i] & 3u) << (2 * (i % 16));
    }
  }
  using sasrt::gpu_round::On;
  if (On(sasrt::gpu_round::kVsScreenSpec))
    s.w[0] |= 1u | (sc.screen_space != 0 ? 2u : 0u);
  if (On(sasrt::gpu_round::kVsRemapSpec))
    s.w[0] |= 4u | (sc.depth_remap[0] != 0.0f ? 8u : 0u);
  const bool stack = On(sasrt::gpu_round::kStackSpec), filter = On(sasrt::gpu_round::kStackFilterSpec);
  if (ps_entry && (stack || filter) && !(ps_entry->textureFetchMask >> 31)) {
    u32 any = 0, stacked = 0, linear = 0, wrap = 0;
    for (u32 bits = ps_entry->textureFetchMask; bits; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      const u32 word = sc.texture_indices[1][r];
      any |= u32(word != kNullTexture3DSlot);
      stacked |= ((word >> 31) & 1u) << r;
      linear |= ((word >> 30) & 1u) << r;
      wrap |= ((word >> 29) & 1u) << r;
    }
    if (any) {
      if (stack)
        s.w[1] = 0x80000000u | stacked;
      if (filter) {
        s.w[2] = 0x80000000u | linear;
        s.w[3] = 0x80000000u | wrap;
      }
    }
  }
  // sasrt-nx (2026-11-09, sasrt_gpu_fetch_conversion_spec; the fifth round): [12]-[15]. The sampler word packs Xenos'
  // gamma at bits 16-19 and the biased signs at 13-15 below the sampler slot (bit 20), as sasrt-nx's tree does
  // (checked by rr6-nx, 2026-11-09).
  if (ps_entry && On(sasrt::gpu_round::kFetchConvSpec)) {
    for (u32 bits = ps_entry->textureFetchMask & 0xFFu; bits; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      const u32 sampler = sc.sampler_indices[r], word = sc.texture_indices[0][r];
      const u32 field = 0x8000u | ((sampler >> 16) & 15u) | ((sampler >> 13) & 7u) << 4 | ((word >> 20) & 63u) << 7;
      s.w[12 + r / 2] |= field << (16 * (r & 1));
    }
  }
  // sasrt-nx (2026-11-09, sasrt_gpu_fetch_exp_3d_spec): [16]-[19].
  if (ps_entry && On(sasrt::gpu_round::kFetchExpSpec)) {
    for (u32 bits = ps_entry->textureFetchMask & 0xFFu; bits; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      s.w[16 + r / 4] |= (0x80u | ((sc.texture_indices[1][r] >> 20) & 63u)) << (8 * (r & 3));
      s.w[18 + r / 4] |= (0x80u | ((sc.texture_indices[2][r] >> 20) & 63u)) << (8 * (r & 3));
    }
  }
  return s;
}

PipelineKey MakeKey(const GuestDevice &device, const ShaderRecord *vs, const ShaderRecord *ps,
                    const VertexDeclarationRecord *declaration, const Targets &t, u32 primitive,
                    bool per_instance, const u32 *booleans, u32 alpha_test_func, const FetchSpec &fetch_spec) {
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  std::memcpy(key.fetch_types, fetch_spec.types, sizeof(key.fetch_types));  // banjo-nx (banjo_gpu_fetch_spec)
  key.tess_kind = fetch_spec.kind;
  // banjo-nx (banjo_gpu_bool_spec): the draw's values of the bools its shaders specialise (the same words the shaders
  // read at run time, SharedConstants.booleans).
  if (banjo::renut_gpu::BoolSpecOn()) {
    const u32 *vm = BoolSpecMask(vs);
    const u32 *pm = ps ? BoolSpecMask(ps) : nullptr;
    bool any = false;
    for (u32 w = 0; w < 8; ++w) {
      key.bool_mask[w] = vm[w] | (pm ? pm[w] : 0u);
      key.bool_values[w] = booleans[w] & key.bool_mask[w];
      any |= key.bool_mask[w] != 0;
    }
    if (any)
      banjo::renut_gpu::CountBoolSpec(banjo::renut_gpu::BoolSpecEvent::kDraw);
  }
  const d3d::Device &dev = device.dev();
  key.vertex_shader = vs->entry;
  key.pixel_shader = ps ? ps->entry : nullptr;
  key.layout_hash = declaration->layout_hash;
  key.topology = Topology(primitive);
  key.color_count = t.color_count;
  for (uint32_t i = 0; i < t.color_count; ++i) {
    key.color_formats[i] = t.colors[i]->format;
    // halo3-nx (2026-11-03, halo3_renut_target_gamma_bias): bits 4-7 a target of the guest format k_8_8_8_8_GAMMA, the pixel
    // shader's constant 924; its host format is plain RGBA8, so a key says it: RB_COLOR_INFO bits 16-19. Bits 8-31, 6 a target:
    // its colour exponent bias, constant 925. (Bits 0-2 are the in-run A/B's off halves.)
    if (banjo::renut_gpu::TargetGammaBiasOn() && i < 4) {
      if (((t.colors[i]->color_info_word >> 16) & 0xF) == 1)
        key.reserved |= 0x10u << i;
      key.reserved |= ((t.colors[i]->color_info_word >> 20) & 0x3Fu) << (8 + 6 * i);
    }
  }
  key.depth_format = t.depth ? t.depth->format : plume::RenderFormat::UNKNOWN;
  const HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  key.samples = uint32_t(first->samples);
  const d3d::ControlPacket &cp = dev.m_ControlPacket;
  key.blend_control[0] = cp.BlendControl0;
  key.blend_control[1] = cp.BlendControl1;
  key.blend_control[2] = cp.BlendControl2;
  key.blend_control[3] = cp.BlendControl3;
  key.color_mask = u32(dev.m_ValuesPacket.ColorMask) & 0xFFFF;
  const bool depth_words = t.depth || t.depth_dropped;  // banjo-nx (2026-10-21, Targets::depth_dropped)
  key.depth_control = depth_words ? u32(cp.DepthControl) : 0;
  key.stencil_ref_mask = depth_words ? u32(dev.m_ValuesPacket.StencilRefMask) & 0xFFFFFF : 0;
  key.stencil_ref_mask_bf = depth_words ? u32(dev.m_ValuesPacket.StencilRefMaskBF) & 0xFFFFFF : 0;
  key.mode_control = u32(cp.ModeControl) & 0x7;
  key.alpha_to_coverage = (u32(cp.ColorControl) >> 4) & 1;
  if (primitive == kRectangleList) {
    if (State().rectangle_list_gs)
      key.rectangle_list = 1;
    // Rectangles are not culled. N&B's D3D clears through rectangles
    // (ClearRects_D3D, 0x821D3D20) whose corners run clockwise on screen, under
    // whatever cull mode the game left set: with back faces culled and
    // counterclockwise front faces (after hitting some enemies) the clear of
    // the post-process surface was culled, so its resolve took the previous
    // frame's final image back into the bloom, which grew to white.
    key.mode_control &= ~3u;
  }
  if (primitive == kPointList && State().point_sprite_gs)
    key.point_sprite = 1;
  if (per_instance) {
    key.per_instance_streams = 1;
    // Which way the tessellated grid winds depends on how the shader weights
    // the control points, so it is not culled.
    key.mode_control &= ~3u;
  }
  // banjo-nx (banjo_gpu_blend_skip): from the fields above.
  // banjo-nx (2026-10-19, render-thread proposal 6): both masks again for the same fields they read (a one-entry memo:
  // OutputTrimMask built a BlendDesc per target at every draw). The fields: whether there is a pixel shader, the
  // targets' count and formats, their blend controls and write mask, alpha to coverage, the depth format and control.
  {
    struct MaskInputs {
      u32 has_ps, color_count, color_mask, alpha_to_coverage, depth_control;
      plume::RenderFormat color_formats[d3d::kMaxRenderTargets];
      plume::RenderFormat depth_format;
      u32 blend_control[d3d::kMaxRenderTargets];
    };
    MaskInputs in;
    std::memset(&in, 0, sizeof(in));
    in.has_ps = key.pixel_shader != nullptr;
    in.color_count = key.color_count;
    in.color_mask = key.color_mask;
    in.alpha_to_coverage = key.alpha_to_coverage;
    in.depth_control = key.depth_control;
    std::memcpy(in.color_formats, key.color_formats, sizeof(in.color_formats));
    in.depth_format = key.depth_format;
    std::memcpy(in.blend_control, key.blend_control, sizeof(in.blend_control));
    static MaskInputs last_in;
    static u32 last_skip = 0, last_trim = 0;
    static bool last_valid = false;
    if (!last_valid || std::memcmp(&in, &last_in, sizeof(in)) != 0) {
      last_skip = BlendSkipMode(key);
      last_trim = OutputTrimMask(key);
      last_in = in;
      last_valid = true;
    }
    if (banjo::renut_gpu::BlendSkipOn() && !BlendSkipExcluded(key.pixel_shader))
      key.blend_skip = last_skip;
    // banjo-nx (banjo_gpu_output_trim, package 28): from the fields above too (the same pipelines: they differ by them).
    if (banjo::renut_gpu::OutputTrimOn())
      key.output_trim = last_trim;
  }
  // banjo-nx (banjo_deko3d_alpha_spec, 2026-10-18): the alpha test's function as the pixel shader's constant 903 (+1):
  // without the test (7) no compare and no discard, and the GPU tests depth before the pixel shader.
  if (ps && (banjo::renut_gpu::AlphaSpecOn() ||
             (IsDeko3D() && sasrt::gpu_round::On(sasrt::gpu_round::kAlphaSpec))))  // sasrt-nx (2026-11-08)
    key.alpha_func = alpha_test_func + 1;
  // banjo-nx (2026-10-20, in-run A/B targets 15 / 16): in the off half of banjo_deko3d_light_branch's or
  // banjo_deko3d_gpr32's A/B the pipelines are built without its constant (911 / 910): keys of their own (reserved
  // bits 0 / 1), so both halves' pipelines stay built. 0 otherwise (the keys as before).
  if (ps) {
    const int ab_off = banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed);
    if (ab_off == 15 && banjo::renut_gpu::LightBranchOn())
      key.reserved |= 1u;
    else if (ab_off == 16 && banjo::renut_gpu::Gpr32On())
      key.reserved |= 2u;
  }
  // banjo-nx (2026-10-20, in-run A/B target 17): banjo_deko3d_skin_once's off half, for the vertex shaders it applies to
  // only (the other pipelines stay the same keys).
  if (vs && IsSkinOnceVs(vs->hash) && banjo::renut_gpu::SkinOnceOn() &&
      banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 17)
    key.reserved |= 4u;
  return key;
}

// ---------------------------------------------------------------------------
// Memexport. N&B fills vertex buffers (and other data) from vertex shaders that
// write to memory: `mad eA, index, const0100, stream` then eM# (see memExport in
// shader_common.h). A draw whose shaders do gets a window in the export buffer
// per stream constant it uses, seeded from guest memory so unwritten bytes keep
// their value; after the draw the windows are copied to the readback buffer,
// and before anything reads that memory (a draw that reads it, a kickoff or
// present) the list is submitted and waited, and the
// results are written to guest memory, where every other path reads them from.
// Waiting at every draw instead cost a GPU round trip per export draw.

struct MemExportStream {
  u32 base = 0;  // guest physical byte address
  u32 size = 0;  // bytes
  u32 format = 0;      // color format (MemExportElementSize)
  u32 endian = 0;      // Endian128
  u32 num_format = 0;  // SurfaceNumberFormat
};

// Bytes per element of a memexport format: the game's bits per pixel for a
// format of single texels in whole dwords, which is what the shaders write
// (memExport in shader_common.h); 0 for any other.
u32 MemExportElementSize(u32 format) {
  const GpuTextureFormat f = GpuTextureFormat(format & 0x3F);
  u32 block_width, block_height;
  FormatBlockSize(f, block_width, block_height);
  const u32 bpp = FormatBitsPerPixel(f);
  return block_width == 1 && block_height == 1 && bpp % 32 == 0 && bpp <= 128 && bpp != 96
             ? bpp / 8
             : 0;
}

constexpr u32 kMaxMemExportStreams = 4;
constexpr u32 kMaxMemExportBytes = 64u << 20;

// The streams the shader's stream constants describe, laid out as N&B's own
// export passes build them (sub_822E6618 before D3DDevice_EndExport): x the
// GPU address in dwords (bits 0-29) tagged 01 in bits 30-31, z the endian in
// bits 0-2, the format in 8-13 and the number format in 16-18, w the element
// count in bits 0-22 (y, z and w carry 0x4B high bits to stay floats).
void GatherMemExportStreams(const ShaderCacheEntry *entry, const FloatConstants &constants,
                            MemExportStream *streams, u32 &count) {
  if (!entry)
    return;
  // banjo-nx (2026-10-19, render-thread proposal 4): whether the shader exports at all, once per entry (static): the
  // 99 % of shaders without exports no longer read their eight cold memExportConstants words at every draw.
  struct ExportSummary {
    const ShaderCacheEntry *entry = nullptr;
    bool any = false;
  };
  static ExportSummary summaries[512];
  const uintptr_t a = reinterpret_cast<uintptr_t>(entry);
  ExportSummary &summary = summaries[((a >> 3) ^ (a >> 12)) & 511];
  if (summary.entry != entry) {
    u32 any = 0;
    for (u32 word = 0; word < 8; ++word)
      any |= entry->memExportConstants[word];
    summary.any = any != 0;
    summary.entry = entry;
  }
  if (!summary.any)
    return;
  // Only the registers the shader exports through (most draws have none).
  for (u32 word = 0; word < 8; ++word) {
    for (u32 bits = entry->memExportConstants[word]; bits; bits &= bits - 1) {
      const u32 r = word * 32 + u32(std::countr_zero(bits));
      const auto *words = reinterpret_cast<const be_u32 *>(constants.Register(r));
      const u32 x = words[0], z = words[2], w = words[3];
      const u32 element = MemExportElementSize((z >> 8) & 0x3F);
      const u32 elements = w & 0x7FFFFF;
      if ((x >> 30) != 1 || !element || !elements) {
        static uint32_t logged = 0;
        if ((x >> 30) == 1 && LogLimited(logged, 8)) {
          RGPU_WARN("Memexport stream in c{} not exported: {:08X} {:08X} {:08X} {:08X} (format "
                    "{}, {} elements)",
                    r, x, u32(words[1]), z, w, (z >> 8) & 0x3F, elements);
        }
        continue;
      }
      MemExportStream stream;
      stream.base = (x & 0x3FFFFFFF) * 4;
      stream.size = std::min(elements * element, kMaxMemExportBytes);
      stream.format = (z >> 8) & 0x3F;
      stream.endian = z & 7;
      stream.num_format = (z >> 16) & 7;
      if (stream.base >= 0x20000000u)
        continue;
      stream.size = std::min(stream.size, 0x20000000u - stream.base);
      bool known = false;
      for (u32 i = 0; i < count; ++i)
        known |= streams[i].base == stream.base && streams[i].size == stream.size;
      if (!known && count < kMaxMemExportStreams)
        streams[count++] = stream;
    }
  }
}

struct PendingExport {
  u32 base = 0;
  u32 size = 0;
  // Its window in the slot's export buffer, and its copy in the slot's
  // readback buffer (the same offset in both).
  uint64_t offset = 0;
};
// The exports a slot's lists made. They reach guest memory once the slot's
// fence is waited (detail::CompleteMemExportsLocked), a frame later; until
// then their windows stay in the slot's export buffer, where the next frame's
// draws seed from them on the GPU (SeedFromPendingExportsLocked).
struct SlotExports {
  std::vector<PendingExport> pending;
  uint64_t used = 0;
};
SlotExports g_exports[kFramesInFlight];

// The 64 KiB granules of physical memory any pending export writes: most
// ranges a draw reads touch none, and skip the lists (PendingExportSlots runs
// for every draw's streams and textures while exports are pending, which is
// always).
constexpr u32 kExportGranuleShift = 16;
constexpr u32 kExportGranules = 0x20000000u >> kExportGranuleShift;
uint64_t g_export_granules[kExportGranules / 64];
// banjo-nx (2026-10-28, cache-miss audit 2): bit i set when any granule of the bitmap's words 2i and 2i + 1 (8 MB) is:
// a range whose words' summary bits are clear touches none, without reading the 1 KB bitmap (its loads were about 0.8 %
// of the render thread in the c4 console run). Kept with the bitmap by the two functions below.
uint64_t g_export_summary = 0;
// banjo-nx (2026-10-23, batch 5): raised at every change of the pending exports (both places go through the two
// functions below), so a texture's answer in DrawNeedsExportFlush can be kept while it stays the same.
uint64_t g_export_generation = 1;

void MarkExportGranules(u32 base, u32 size) {
  ++g_export_generation;
  if (!size)
    return;
  const u32 first = (base & 0x1FFFFFFFu) >> kExportGranuleShift;
  const u32 last = std::min((((base & 0x1FFFFFFFu) + size - 1) >> kExportGranuleShift),
                            kExportGranules - 1);
  for (u32 g = first; g <= last; ++g)
    g_export_granules[g / 64] |= uint64_t(1) << (g % 64);
  for (u32 w = first / 128; w <= last / 128; ++w)
    g_export_summary |= uint64_t(1) << w;
}

// sasrt-nx (2026-11-08, sasrt_gpu_convert_memo_keep): pending exports rebuilt (a slot's completed: its windows used
// again from offset 0) and windows written again in place: what clears the conversion memo with the key.
uint64_t g_sasrt_export_rebuilds = 0, g_sasrt_window_rewrites = 0;
void RebuildExportGranules() {
  ++g_sasrt_export_rebuilds;
  ++g_export_generation;
  std::memset(g_export_granules, 0, sizeof(g_export_granules));
  g_export_summary = 0;
  for (const SlotExports &slot : g_exports) {
    for (const PendingExport &p : slot.pending)
      MarkExportGranules(p.base, p.size);
  }
}

// A word of the bitmap at a time: the bits of the first and last words that
// are in the range, the words between whole.
bool ExportGranulesTouched(u32 base, u32 size) {
  const u32 first = (base & 0x1FFFFFFFu) >> kExportGranuleShift;
  const u32 last = std::min((((base & 0x1FFFFFFFu) + size - 1) >> kExportGranuleShift),
                            kExportGranules - 1);
  if (last < first)
    return false;
  // The summary's bits of the range's 8 MB pieces (bits first/128 to last/128).
  const uint64_t pieces = (~uint64_t(0) << (first / 128)) & (~uint64_t(0) >> (63 - last / 128));
  if (!(g_export_summary & pieces))
    return false;
  const u32 first_word = first / 64, last_word = last / 64;
  const uint64_t first_mask = ~uint64_t(0) << (first % 64);
  const uint64_t last_mask = ~uint64_t(0) >> (63 - last % 64);
  if (first_word == last_word)
    return (g_export_granules[first_word] & first_mask & last_mask) != 0;
  if (g_export_granules[first_word] & first_mask)
    return true;
  for (u32 w = first_word + 1; w < last_word; ++w) {
    if (g_export_granules[w])
      return true;
  }
  return (g_export_granules[last_word] & last_mask) != 0;
}

// banjo-nx (2026-10-23, batch 6): the pending exports of all slots, counted where they are added (FinishMemExportLocked)
// and where a slot's are emptied (CompleteMemExportsLocked): one test at each of a draw's calls, not a walk of the slots.
u32 g_pending_export_count = 0;

bool AnyPendingExports() {
  return g_pending_export_count != 0;
}

// A GPU physical address, or a guest VA in the physical windows, as a physical
// address (other VAs come out as unrelated addresses, which at worst flush the
// exports early).
u32 PhysicalAddress(u32 address) {
  return address >= 0x20000000u ? d3d::CpuToGpuAddress(address) & 0x1FFFFFFFu : address;
}

// The slots (a mask) with a pending export that will write memory overlapping
// [address, address + size).
u32 PendingExportSlots(u32 address, u32 size) {
  if (!size)
    return 0;
  const u32 base = PhysicalAddress(address);
  if (!ExportGranulesTouched(base, size))
    return 0;
  u32 slots = 0;
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    for (const PendingExport &p : g_exports[slot].pending) {
      if (base < p.base + p.size && p.base < base + size) {
        slots |= 1u << slot;
        break;
      }
    }
  }
  return slots;
}

// banjo-nx (2026-10-23, batch 5; file scope since batch 6): DrawNeedsExportFlush's answer per texture fetch slot, kept
// while the slot's six fetch words and the pending exports are the same. DrawStateLocked prefetches a texture's extent
// only when this will not answer it.
struct TextureExportAnswer {
  u32 words[6];
  uint64_t generation = 0;
  u32 slots = 0;
};
// banjo-nx (2026-10-26, the record-time draw path's round 3): by the fetch words (512 entries), not by the slot: draws
// in a row mostly bind other textures to the same slots, so the slot's answer was another texture's and the extent was
// worked out again at nearly every draw while an export was pending (always, in the city): now once per texture and
// change of the pending exports.
constexpr u32 kTextureExportAnswers = 512;
TextureExportAnswer g_texture_export_answers[kTextureExportAnswers];
inline TextureExportAnswer &TextureExportAnswerFor(const TextureFetch &fetch) {
  uint64_t h = 0;
  for (u32 i = 0; i < 6; ++i)
    h = (h ^ fetch.dword[i]) * 0x9E3779B97F4A7C15ull;
  return g_texture_export_answers[u32(h >> 55) & (kTextureExportAnswers - 1)];
}

// banjo-nx (2026-10-23, batch 6): PendingExportSlots' answers kept per caller slot (a fetch constant's stream check,
// its fetch-by-address check, its copy, the index buffer) while the range and the pending exports (g_export_generation,
// raised at every change of them) are the same: the same answer.
struct ExportSlotsAnswer {
  u32 address = 0, size = 0;
  uint64_t generation = 0;
  u32 slots = 0;
};
// kExportAnswerFetch, kExportAnswerCopy, kExportAnswerIndex: defined with WholeGeometryLocked's declarations.
ExportSlotsAnswer g_export_answers[kExportAnswerIndex + 1];

// banjo-nx (2026-10-26, the record-time draw path's round 3): behind the caller's slot, the answers by the range (512
// entries): draws in a row mostly read other buffers through the same fetch constants, so the slot's answer was another
// buffer's at most draws (the console profile: about 7 % of the render thread under the geometry's export checks).
ExportSlotsAnswer g_export_range_answers[512];

u32 PendingExportSlotsMemo(u32 which, u32 address, u32 size) {
  // banjo-nx (2026-10-27, render thread night): PendingExportSlots' own first test, first: a range no pending export's
  // granule touches answers 0 from the 1 KB bitmap, without the answer tables' lines (the console profile's misses:
  // 2 % of the render thread on the memo's two tests). The same answer the memo would give.
  if (!size || !ExportGranulesTouched(PhysicalAddress(address), size))
    return 0;
  ExportSlotsAnswer &a = g_export_answers[which];
  if (a.generation == g_export_generation && a.address == address && a.size == size)
    return a.slots;
  const uint64_t h = (uint64_t(address) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(size) * 0xC4CEB9FE1A85EC53ull);
  ExportSlotsAnswer &r = g_export_range_answers[u32(h >> 55) & 511u];
  if (!(r.generation == g_export_generation && r.address == address && r.size == size))
    r = {address, size, g_export_generation, PendingExportSlots(address, size)};
  a = r;
  return a.slots;
}

// The pending export window holding the newest data of all of [base, base +
// size) (physical): the newest pending export overlapping it, if it covers it
// whole. Its slot and the offset of 'base' in that slot's export buffer.
bool FindExportWindow(u32 base, u32 size, u32 &slot, uint64_t &offset) {
  const auto &s = State();
  // Oldest slot first, and a slot's exports in the order they were drawn.
  const PendingExport *newest = nullptr;
  u32 newest_slot = 0;
  for (u32 k = 1; k <= kFramesInFlight; ++k) {
    const u32 at = (s.frame + k) % kFramesInFlight;
    for (const PendingExport &p : g_exports[at].pending) {
      if (base < p.base + p.size && p.base < base + size) {
        newest = &p;
        newest_slot = at;
      }
    }
  }
  if (!newest || base < newest->base || base + size > newest->base + newest->size)
    return false;
  slot = newest_slot;
  offset = newest->offset + (base - newest->base);
  return true;
}

// Whether UploadVertexFetchData copies the buffer of fetch constant
// 'fetch_index' (read by vfetchMemory / vfetchBound) from a pending export's
// window on the GPU, which it does when the window holds all of it.
bool FetchFromExportWindow(const GuestDevice &device, const DrawCall &call, u32 fetch_index,
                           u32 &slot, uint64_t &offset) {
  if (call.up_vertices && fetch_index == d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(0))
    return false;
  const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
  const u32 size = std::min(device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu, kMaxFetchCopyBytes);
  return base && size && PendingExportSlotsMemo(kExportAnswerCopy + ((fetch_index - 64) & 31), base, size) &&
         FindExportWindow(base, size, slot, offset);
}

// A vertex stream built on the GPU from the export window that holds its
// newest data (ConvertExportedStreamsLocked).
struct ExportedStream {
  u32 slot = 0;          // whose export buffer holds the window
  uint64_t offset = 0;   // of the stream's first byte there
  u32 stride = 0;        // guest
  u32 host_stride = 0;
  u32 vertex_count = 0;  // in the stream's buffer
  u32 element_count = 0;
  u32 elements[2 * kStreamConvertTotalElements] = {};  // StreamConvertPush::elements, 8 elements a dispatch
};

// Whether a draw can read vertex stream 'stream' from a pending export's
// window instead of guest memory: the window holds the newest data of all of
// the stream's buffer (FindExportWindow), and stream_convert_cs can build its
// host layout (word-aligned elements, few enough of them). Shader reads
// through vfetchBound get the window's bytes too (UploadVertexFetchData).
bool FindExportedStream(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                        const u32 *shader_strides, const VertexDeclarationRecord &declaration,
                        u32 stream, ExportedStream &out) {
  const auto &s = State();
  if (!s.stream_convert_pipeline || stream >= kMaxVertexStreams || (stream == 0 && call.up_vertices))
    return false;
  const HostVertexStream &hs = declaration.layout.streams[stream];
  if (!hs.used)
    return false;
  const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
  const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
  const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
  if (!base || !size || !PendingExportSlotsMemo(kExportAnswerFetch + ((fetch_index - 64) & 31), base, size))
    return false;
  const StreamSource src = GetStreamSource(device, call, vs, shader_strides, stream);
  if (!src.stride || src.stride % 4 || src.stride / 4 > kStreamConvertMaxStrideWords ||
      hs.host_stride % 4 || hs.elements.size() > kStreamConvertTotalElements) {
    return false;
  }
  u32 elements[2 * kStreamConvertTotalElements] = {};
  for (size_t i = 0; i < hs.elements.size(); ++i) {
    const HostVertexElement &e = hs.elements[i];
    if (e.guest_offset % 4 || e.guest_offset + 4 > src.stride)
      return false;
    const bool expand = e.conversion == ElementConversion::ExpandFloat;
    // As ConvertVertices: swapping the halves too is the endian mode ^ 3.
    const u32 endian = !expand && e.swap_halves ? src.endian ^ 3 : src.endian;
    u32 type = e.type.format | (e.type.is_signed ? 1u << 8 : 0u) | (e.type.is_integer ? 1u << 9 : 0u);
    for (u32 c = 0; c < 4; ++c)
      type |= u32(e.type.swizzle[c] & 7) << (10 + 3 * c);
    elements[2 * i] = (e.guest_offset / 4) | (e.host_offset / 4) << 8 | endian << 16 |
                      (expand ? 1u : 0u) << 18 | (e.host_size / 4 & 7) << 19;
    elements[2 * i + 1] = type;
  }
  if (!FindExportWindow(base, size, out.slot, out.offset))
    return false;
  out.stride = src.stride;
  out.host_stride = hs.host_stride;
  out.vertex_count = size / src.stride;
  out.element_count = u32(hs.elements.size());
  std::memcpy(out.elements, elements, sizeof(elements));
  return true;
}

// Why a draw needs the pending exports in guest memory first (see
// DrawNeedsExportFlush).
enum class ExportRead : u32 {
  None,
  Stream,    // a declared vertex stream
  Fetch,     // a vertex fetch by address (vfetchMemory)
  Index,     // the index buffer
  Texture,
  Capacity,  // its exports would not fit the readback buffer beside theirs
};

// Whether pending exports must reach guest memory before a draw, and whose
// ('slots', a mask): it reads memory they write (its vertex streams, bound or
// read by address, indices or textures), or its exports would not fit this
// slot's buffers beside theirs (growing them would lose their windows). Any
// other draw can be recorded while they are still on the GPU, and so can one
// whose streams are built from the export windows (FindExportedStream) or
// whose own exports are seeded from them (SeedFromPendingExportsLocked).
ExportRead DrawNeedsExportFlush(const GuestDevice &device, const DrawCall &call,
                                const ShaderRecord &vs, const ShaderRecord *ps,
                                const VertexDeclarationRecord &declaration,
                                const u32 *shader_strides, const MemExportStream *exports, u32 count,
                                u32 &slots, const TextureFetch *draw_fetches, bool *untouched = nullptr) {
  const u32 current = 1u << State().frame;
  // banjo-nx (2026-10-26, API-level stage 4): 'untouched' (or null) receives whether none of the draw's reads met a
  // pending export window at all (an answer that then holds while none of them changes).
  if (untouched)
    *untouched = true;
  auto fetch_slots = [&](u32 fetch_index) {
    const u32 w0 = device.VertexFetchWord(fetch_index, 0);
    const u32 w1 = device.VertexFetchWord(fetch_index, 1);
    return PendingExportSlotsMemo(kExportAnswerFetch + ((fetch_index - 64) & 31), w0 & ~3u, w1 & 0x3FFFFFCu);
  };
  auto found = [&](ExportRead read, u32 found_slots) {
    slots = found_slots;
    return read;
  };
  // banjo-nx (2026-10-23, batch 6): the declaration's streams in the order of their first element, as the walk over its
  // elements found them (VertexDeclarationRecord::stream_order).
  for (u32 k = 0; k < declaration.stream_count; ++k) {
    const u32 stream = declaration.stream_order[k];
    // A Begin/DrawVerticesUP draw's stream 0 is its own vertices, whatever
    // buffer the fetch constant still names: nothing of it is read from
    // memory.
    if (stream == 0 && call.up_vertices)
      continue;
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    if (const u32 found_slots = fetch_slots(fetch_index)) {
      if (untouched)
        *untouched = false;
      // A stream the host layout reads is built from the export window; one it
      // does not (no host input) is read only by vfetchBound, whose copy the
      // window supplies (UploadVertexFetchData).
      ExportedStream exported;
      u32 slot;
      uint64_t offset;
      const bool on_gpu =
          declaration.layout.streams[stream].used
              ? FindExportedStream(device, call, vs, shader_strides, declaration, stream, exported)
              : FetchFromExportWindow(device, call, fetch_index, slot, offset);
      if (!on_gpu)
        return found(ExportRead::Stream, found_slots);
    }
  }
  if (vs.entry) {
    for (u32 bits = vs.entry->vertexFetchMemoryMask; bits; bits &= bits - 1) {
      const u32 fetch_index = 64 + u32(std::countr_zero(bits));
      if (const u32 found_slots = fetch_slots(fetch_index)) {
        if (untouched)
          *untouched = false;
        u32 slot;
        uint64_t offset;
        if (!FetchFromExportWindow(device, call, fetch_index, slot, offset))
          return found(ExportRead::Fetch, found_slots);
      }
    }
  }
  // A tessellated draw's "indices" are only adaptive edge factors, which set
  // its level (PrepareTessellation): the ones in guest memory, from a frame
  // before, set it as well. N&B's water computes them with memexport before
  // each draw, and waiting for them was two GPU round trips a frame.
  if (call.indexed && !call.up_indices && !call.tessellation) {
    const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
    if (ib) {
      if (const u32 found_slots = PendingExportSlotsMemo(kExportAnswerIndex, ib->Address, ib->Size))
        return found(ExportRead::Index, found_slots);
    }
  }
  const u32 texture_mask =
      (vs.entry ? vs.entry->textureFetchMask : 0) | (ps && ps->entry ? ps->entry->textureFetchMask : 0);
  // banjo-nx (2026-10-23, batch 5): each fetch slot's last answer, kept while the slot's six fetch words and the
  // pending exports (g_export_generation) are the same: the extent lookup (a 96 KB memo, past the A57's L1) and the
  // overlap test ran for every texture of every draw while exports were pending (4 % of the render thread at high
  // clocks). The same answer: it depends on nothing else.
  for (u32 bits = texture_mask; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    const TextureFetch &fetch = draw_fetches[r];  // banjo-nx (2026-10-23, batch 6): converted once for the draw
    TextureExportAnswer &a = TextureExportAnswerFor(fetch);
    u32 found_slots = 0;
    if (a.generation == g_export_generation && std::memcmp(a.words, fetch.dword, sizeof(a.words)) == 0) {
      found_slots = a.slots;
    } else {
      u32 pages[2], sizes[2];
      TextureGuestExtent(fetch, pages, sizes);
      for (u32 i = 0; i < 2 && !found_slots; ++i) {
        if (pages[i])
          found_slots = PendingExportSlots(pages[i] << 12, sizes[i]);
      }
      std::memcpy(a.words, fetch.dword, sizeof(a.words));
      a.generation = g_export_generation;
      a.slots = found_slots;
    }
    if (found_slots)
      return found(ExportRead::Texture, found_slots);
  }
  // banjo-nx (2026-10-19, render-thread proposal 4): the draw's exports, gathered once (DrawStateLocked).
  uint64_t bytes = 0;
  for (u32 i = 0; i < count; ++i)
    bytes += (exports[i].size + 255u) & ~255u;
  const auto &s = State();
  const uint64_t used = g_exports[s.frame].used;
  if (bytes && used &&
      used + bytes > std::min(s.memexport_capacity[s.frame], s.memexport_readback_capacity[s.frame])) {
    return found(ExportRead::Capacity, current);
  }
  return ExportRead::None;
}

// Recreates 'buffer' with at least 'size' bytes when it is smaller.
bool EnsureBuffer(std::unique_ptr<plume::RenderBuffer> &buffer, uint64_t &capacity, uint64_t size,
                  const plume::RenderBufferDesc &desc_template, const char *name) {
  if (buffer && capacity >= size)
    return true;
  uint64_t grown = std::max<uint64_t>(capacity * 2, 4u << 20);
  while (grown < size)
    grown *= 2;
  plume::RenderBufferDesc desc = desc_template;
  desc.size = grown;
  if (buffer)
    Video::RetireLocked(std::move(buffer));
  buffer = State().device->createBuffer(desc);
  capacity = buffer ? grown : 0;
  if (buffer)
    buffer->setName(name);
  return buffer != nullptr;
}

// The current slot's export buffer, in its unordered-access state (created on
// first use).
plume::RenderBuffer *MemExportBufferSlowLocked(VideoState &s, uint64_t size) {
  auto &buffer = s.memexport_buffer[s.frame];
  const bool had = buffer && s.memexport_capacity[s.frame] >= size;
  // sasrt-nx (2026-11-09, sasrt_rt_cached_export_seeds; port 54): CPU-cached on deko3d (the key as latched: a buffer keeps
  // its memory type, so the A/B half does not decide it). The windows start on 256-byte boundaries (PrepareMemExportLocked),
  // so a cache line belongs to one window, and the CPU never reads the buffer.
  if (!sasrt::gpu_round::g_latched_ready.load(std::memory_order_acquire)) [[unlikely]]
    sasrt::gpu_round::LatchNow();
  const bool cached = IsDeko3D() && sasrt::gpu_round::g_latched.on[sasrt::gpu_round::kRtCachedExportSeeds];
  if (!EnsureBuffer(buffer, s.memexport_capacity[s.frame], size,
                    cached ? plume::RenderBufferDesc::UploadBuffer(0, StorageBufferFlags() | AddressableBufferFlags() |
                                                                          plume::RenderBufferFlag::HOST_CACHED)
                           : plume::RenderBufferDesc::DefaultBuffer(
                                 0, StorageBufferFlags() | AddressableBufferFlags()),
                    "renut memexport buffer")) {
    return nullptr;
  }
  if (!had) {
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                             plume::RenderBufferBarrier(buffer.get(),
                                                        plume::RenderBufferAccess::WRITE));
  }
  return buffer.get();
}
// banjo-nx (2026-10-27, render thread night): the slot's buffer when it is big enough (EnsureBuffer's first test), inline:
// the slow form built a buffer description at every draw before finding nothing to do.
inline plume::RenderBuffer *MemExportBufferLocked(VideoState &s, uint64_t size = 256) {
  auto &buffer = s.memexport_buffer[s.frame];
  if (buffer && s.memexport_capacity[s.frame] >= size) [[likely]]
    return buffer.get();
  return MemExportBufferSlowLocked(s, size);
}

// A part of a draw's export window whose seed comes from a pending export's
// window instead of guest memory.
struct SeedPiece {
  u32 slot = 0;
  uint64_t source = 0;  // in the slot's export buffer
  uint64_t dest = 0;    // in the current slot's export buffer
  u32 size = 0;
};

// Guest memory lacks what the pending exports will write there, the older
// slots' and this slot's own earlier draws': copies their windows over the
// parts of the draw's windows they overlap, each byte from the newest export
// of it (the copies of one batch are unordered, so no two overlap). Waiting
// for this slot's instead cost a GPU round trip per such draw, the whole
// frame's GPU work so far. A slot's own windows pass through its scratch
// buffer, as a buffer cannot be copied into itself. The copies wait for the
// guest seed through the buffer's unordered-access state in between.
bool SeedFromPendingExportsLocked(VideoState &s, plume::RenderBuffer *buffer,
                                  const MemExportStream *streams, u32 count, const u32 *offsets) {
  static std::vector<SeedPiece> pieces;
  static std::vector<std::pair<u32, u32>> covered;  // [begin, end), newer exports
  static std::vector<std::pair<u32, u32>> parts;
  pieces.clear();
  for (u32 i = 0; i < count; ++i) {
    const u32 stream_begin = streams[i].base, stream_end = streams[i].base + streams[i].size;
    covered.clear();
    // Newest first: this slot's latest draw, back to the oldest slot's first.
    for (u32 k = kFramesInFlight; k >= 1; --k) {
      const u32 slot = (s.frame + k) % kFramesInFlight;
      if (!s.memexport_buffer[slot])
        continue;
      const auto &pending = g_exports[slot].pending;
      for (auto p = pending.rbegin(); p != pending.rend(); ++p) {
        const u32 begin = std::max(p->base, stream_begin);
        const u32 end = std::min(p->base + p->size, stream_end);
        if (begin >= end)
          continue;
        // What of [begin, end) no newer export covers.
        parts.assign(1, {begin, end});
        for (const auto &[cb, ce] : covered) {
          for (size_t j = 0; j < parts.size();) {
            const auto [pb, pe] = parts[j];
            if (ce <= pb || pe <= cb) {
              ++j;
              continue;
            }
            parts.erase(parts.begin() + j);
            if (pb < cb)
              parts.insert(parts.begin() + j++, {pb, cb});
            if (ce < pe)
              parts.insert(parts.begin() + j++, {ce, pe});
          }
        }
        for (const auto &[pb, pe] : parts) {
          pieces.push_back({slot, p->offset + (pb - p->base),
                            offsets[i] + uint64_t(pb - stream_begin), pe - pb});
        }
        covered.push_back({begin, end});
      }
    }
  }
  if (pieces.empty())
    return true;

  // This slot's own pieces go to the scratch buffer first.
  uint64_t own_bytes = 0;
  for (const SeedPiece &piece : pieces) {
    if (piece.slot == s.frame)
      own_bytes += piece.size;
  }
  plume::RenderBuffer *scratch = nullptr;
  uint64_t scratch_offset = 0;
  if (own_bytes) {
    uint64_t &used = s.memexport_scratch_used[s.frame];
    if (!s.memexport_scratch[s.frame] || used + own_bytes > s.memexport_scratch_capacity[s.frame]) {
      // Copies recorded before still use the old one, retired with the slot.
      if (!EnsureBuffer(s.memexport_scratch[s.frame], s.memexport_scratch_capacity[s.frame],
                        used + own_bytes, plume::RenderBufferDesc::DefaultBuffer(0),
                        "renut memexport scratch")) {
        return false;
      }
      used = 0;
    }
    scratch = s.memexport_scratch[s.frame].get();
    scratch_offset = used;
    used += own_bytes;
  }

  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier barriers[kFramesInFlight + 1];
  u32 barrier_count = 0;
  if (scratch) {
    barriers[0] = plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::READ);
    barriers[1] = plume::RenderBufferBarrier(scratch, plume::RenderBufferAccess::WRITE);
    cmd->barriers(plume::RenderBarrierStage::COPY, barriers, 2);
    uint64_t at = scratch_offset;
    for (const SeedPiece &piece : pieces) {
      if (piece.slot != s.frame)
        continue;
      cmd->copyBufferRegion(plume::RenderBufferReference(scratch, at),
                            plume::RenderBufferReference(buffer, piece.source), piece.size);
      at += piece.size;
    }
    barriers[barrier_count++] = plume::RenderBufferBarrier(scratch, plume::RenderBufferAccess::READ);
  }
  barriers[barrier_count++] = plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE);
  for (u32 k = 1; k < kFramesInFlight; ++k) {
    const u32 slot = (s.frame + k) % kFramesInFlight;
    if (s.memexport_buffer[slot]) {
      barriers[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::READ);
    }
  }
  cmd->barriers(plume::RenderBarrierStage::COPY, barriers, barrier_count);
  uint64_t at = scratch_offset;
  for (const SeedPiece &piece : pieces) {
    if (piece.slot == s.frame) {
      cmd->copyBufferRegion(plume::RenderBufferReference(buffer, piece.dest),
                            plume::RenderBufferReference(scratch, at), piece.size);
      at += piece.size;
    } else {
      cmd->copyBufferRegion(plume::RenderBufferReference(buffer, piece.dest),
                            plume::RenderBufferReference(s.memexport_buffer[piece.slot].get(),
                                                         piece.source),
                            piece.size);
    }
  }
  // Back to unordered access, as the other export buffers were.
  barrier_count = 0;
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (s.memexport_buffer[slot]) {
      barriers[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::WRITE);
    }
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, barrier_count);
  return true;
}

// Seeds the draw's export windows from guest memory (and the older slots'
// pending exports) and fills its stream table. The windows follow the slot's
// earlier ones in its export buffer, which keeps them for later draws' seeds;
// returns their offsets.
uint64_t g_seed_bytes = 0;
uint64_t g_exports_in_place = 0;  // the 10 s line's
REXCVAR_DEFINE_BOOL(banjo_renut_export_in_place, true, "Banjo/Renut",
                    "A memexport draw whose stream is the newest window this slot has over that memory writes into that "
                    "window, with no seed and no readback of its own: the GPU writes one pool in order, as the console does. "
                    "Halo 3's particle pool is 573 KB, and about 100 draws a frame seeded and read back all of it");
// This slot's window for this stream when it is the newest over that memory: draws write it in place. Null: a new window.
const PendingExport *InPlaceWindow(const SlotExports &slot, const MemExportStream &stream) {
  for (auto it = slot.pending.rbegin(); it != slot.pending.rend(); ++it) {
    if (stream.base < it->base + it->size && it->base < stream.base + stream.size)
      return it->base == stream.base && it->size == stream.size ? &*it : nullptr;
  }
  return nullptr;
}
// sasrt-nx (2026-11-08, sasrt_gpu_convert_lead_wait): the command list of the last memexport draw and its buffer
// barriers then (plume::Deko3DBufferWaits): a buffer barrier requested since in the same list waits for that draw and
// drops the data caches before any later work. (Moved here for sasrt_gpu_seed_cpu_direct.)
// sasrt-nx (2026-11-08): an EXPERIMENT for the 33 ms goal, NOT pixel-identical, off by default: the stream
// conversion's waits (before: the export draw's writes; after: the draw's vertex fetch) left out, to measure what the
// export chains' waits cost the shadow pass. Skinned meshes may show garbage with it.
REXCVAR_DEFINE_BOOL(sasrt_gpu_exp_no_chain_waits, false, "SASRT/GPU",
                    "EXPERIMENT, NOT pixel-identical (2026-11-08): the stream conversion's two waits left out (skinned "
                    "meshes may show garbage): measures what the export chains' waits cost");
bool SasrtExpNoChainWaits() {
  static const bool on = REXCVAR_GET(sasrt_gpu_exp_no_chain_waits);
  return on;
}

struct SasrtExportWrite {
  const plume::RenderCommandList *cmd = nullptr;
  uint64_t waits = 0;
};
SasrtExportWrite g_sasrt_export_write;

// sasrt-nx (2026-11-08, sasrt_gpu_seed_cpu_direct; the third GPU round): the seeds written into the windows by the CPU
// (the export buffer is plume's deko3d memory, CPU-uncached and mapped). True when every seed went this way; false:
// nothing written, the dispatches' way then.
bool SasrtSeedExportsCpuLocked(plume::RenderBuffer *buffer, const MemExportStream *fresh, const u32 *fresh_offsets,
                               const u32 *indices, u32 count) {
#if defined(PLUME_DEKO3D)
  if (!IsDeko3D() || !sasrt::gpu_round::On(sasrt::gpu_round::kSeedCpu))
    return false;
  u8 *const window = static_cast<u8 *>(buffer->map());
  if (!window)
    return false;
  const u8 *guests[kMaxMemExportStreams];
  for (u32 k = 0; k < count; ++k) {
    guests[k] = GuestPhysicalPtr<const u8>(fresh[indices[k]].base);
    if (!guests[k])
      return false;
  }
  for (u32 k = 0; k < count; ++k) {
    const MemExportStream &f = fresh[indices[k]];
    std::memcpy(window + fresh_offsets[indices[k]], guests[k], f.size);
    g_seed_bytes += f.size;  // banjo-nx (2026-10-26): the 10 s line's
    // sasrt-nx (2026-11-09, sasrt_rt_cached_export_seeds; port 54): the seed's own lines cleaned when the buffer is
    // CPU-cached (plume's unmap cleans the written range then; on uncached memory it does nothing, as the whole-buffer
    // unmap here before did).
    const plume::RenderRange written(fresh_offsets[indices[k]], uint64_t(fresh_offsets[indices[k]]) + f.size);
    buffer->unmap(0, &written);
  }
  sasrt::gpu_round::Count(sasrt::gpu_round::kSeedCpu, count);
  return true;
#else
  (void)buffer, (void)fresh, (void)fresh_offsets, (void)indices, (void)count;
  return false;
#endif
}

// sasrt-nx (2026-11-08, sasrt_gpu_fetch_copy_compute): PrepareMemExportLocked's fresh seeds (guest memory through the
// upload ring into the slot's export buffer) as fetch_copy_cs dispatches: the barrier before them, the copies. False
// (nothing recorded) when a seed cannot go this way (no set, a misaligned piece): the copy engine's path then.
// sasrt-nx (2026-11-08, sasrt_gpu_seed_cpu_direct): *dispatched says whether a dispatch was recorded.
bool SasrtSeedExportsComputeLocked(VideoState &s, plume::RenderBuffer *buffer, const MemExportStream *fresh,
                                   u32 fresh_count, const u32 *fresh_offsets, bool in_place, bool *dispatched) {
  *dispatched = false;
  struct Seed {
    UploadAllocation alloc;
    u32 index;
  };
  Seed seeds[kMaxMemExportStreams];
  u32 seed_count = 0;
  for (u32 i = 0; i < fresh_count; ++i) {
    u32 older_slot;
    uint64_t older_offset;
    if (in_place && FindExportWindow(fresh[i].base, fresh[i].size, older_slot, older_offset))
      continue;
    if ((fresh[i].size & 3u) || (fresh_offsets[i] & 3u))
      return false;
    seeds[seed_count++] = {UploadAllocation{}, i};
  }
  // sasrt-nx (2026-11-08, sasrt_gpu_seed_cpu_direct): by the CPU into the windows, when it can.
  if (seed_count) {
    u32 indices[kMaxMemExportStreams];
    for (u32 k = 0; k < seed_count; ++k)
      indices[k] = seeds[k].index;
    if (SasrtSeedExportsCpuLocked(buffer, fresh, fresh_offsets, indices, seed_count))
      return true;
  }
  // The uploads first (all in the same chunk, or the copy engine's way: one set binds one chunk).
  const plume::RenderBuffer *chunk = nullptr;
  for (u32 k = 0; k < seed_count; ++k) {
    const MemExportStream &f = fresh[seeds[k].index];
    UploadAllocation seed = Video::UploadsLocked().Allocate(f.size, 256);
    const u8 *guest = GuestPhysicalPtr<const u8>(f.base);
    if (!seed || !guest || (chunk && seed.ref.ref != chunk) || (seed.ref.offset & 3u))
      return false;  // (an allocation made here and not used is the ring's: it is reset with the slot)
    chunk = seed.ref.ref;
    std::memcpy(seed.cpu, guest, f.size);
    seeds[k].alloc = seed;
  }
  plume::RenderBuffer *const fallback = buffer;
  const plume::RenderBuffer *buffers[kFetchCopyBuffers] = {
      s.memexport_buffer[0] ? s.memexport_buffer[0].get() : fallback,
      kFramesInFlight > 1 && s.memexport_buffer[1] ? s.memexport_buffer[1].get() : fallback,
      chunk ? chunk : fallback, buffer};
  std::unique_ptr<plume::RenderDescriptorSet> &set = s.seed_copy_set[s.frame];
  if (!set || std::memcmp(buffers, s.seed_copy_set_buffers[s.frame], sizeof(buffers)) != 0) {
    if (set)
      Video::RetireLocked(std::move(set));
    set = detail::CreateFetchCopySet(s);
    if (!set)
      return false;
    for (u32 i = 0; i < kFetchCopyBuffers; ++i)
      set->setBuffer(i, const_cast<plume::RenderBuffer *>(buffers[i]));
    std::memcpy(s.seed_copy_set_buffers[s.frame], buffers, sizeof(buffers));
  }
  plume::RenderCommandList *cmd = s.command_list;
  // sasrt-nx (2026-11-08, sasrt_gpu_seed_no_lead_wait; the second GPU round): the windows are fresh (from the slot's
  // used end on, 256-byte aligned): nothing recorded before read or wrote them, so the seeds need not wait for it. The
  // barrier after them (PrepareMemExportLocked) orders them and everything before with the export draw.
  if (!seed_count) {
  } else if (sasrt::gpu_round::On(sasrt::gpu_round::kSeedLead)) {
    sasrt::gpu_round::Count(sasrt::gpu_round::kSeedLead);
  } else {
    cmd->barriers(plume::RenderBarrierStage::COMPUTE,
                  plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  }
  if (seed_count) {
    cmd->setComputePipelineLayout(s.fetch_copy_layout.get());
    cmd->setPipeline(s.fetch_copy_pipeline.get());
    cmd->setComputeDescriptorSet(set.get(), 0);
    for (u32 k = 0; k < seed_count; ++k) {
      const MemExportStream &f = fresh[seeds[k].index];
      FetchCopyPush push{};
      push.source = 2;
      push.source_offset = u32(seeds[k].alloc.ref.offset);
      push.dest_offset = fresh_offsets[seeds[k].index];
      push.words = f.size / 4u;
      cmd->setComputePushConstants(0, &push, 0, sizeof(push));
      cmd->dispatch((push.words + 63u) / 64u, 1, 1);
      g_seed_bytes += f.size;  // banjo-nx (2026-10-26): the 10 s line's
    }
    // The compute pipeline replaced the graphics one (as ConvertExportedStreamsLocked).
    g_bound.pipeline = nullptr;
    g_bound.framebuffer = nullptr;
    sasrt::gpu_round::Count(sasrt::gpu_round::kFetchCopyCompute, 1, 1);
    *dispatched = true;
  }
  return true;
}

bool PrepareMemExportLocked(VideoState &s, const MemExportStream *streams, u32 count,
                            SharedConstants &sc, u32 *offsets) {
  static const bool in_place = REXCVAR_GET(banjo_renut_export_in_place) && REXCVAR_GET(banjo_renut_batched_readbacks);
  const uint64_t used = g_exports[s.frame].used;
  uint64_t total = used;
  MemExportStream fresh[kMaxMemExportStreams];
  u32 fresh_offsets[kMaxMemExportStreams];
  u32 fresh_count = 0;
  for (u32 i = 0; i < count; ++i) {
    const PendingExport *window = in_place ? InPlaceWindow(g_exports[s.frame], streams[i]) : nullptr;
    if (window) {
      offsets[i] = window->offset;
      ++g_exports_in_place;
      ++g_sasrt_window_rewrites;  // sasrt-nx (2026-11-08, sasrt_gpu_convert_memo_keep)
      continue;
    }
    offsets[i] = u32(total);
    fresh[fresh_count] = streams[i];
    fresh_offsets[fresh_count++] = offsets[i];
    total += (streams[i].size + 255u) & ~255u;
  }
  plume::RenderBuffer *buffer = MemExportBufferLocked(s, total);
  // banjo-nx (2026-10-23, batch 6): CPU-cached (plume's deko3d buffer drops the read range's lines at map).
  if (!buffer || !EnsureBuffer(s.memexport_readback[s.frame], s.memexport_readback_capacity[s.frame],
                               total, plume::RenderBufferDesc::ReadbackBuffer(0, plume::RenderBufferFlag::HOST_CACHED),
                               "renut memexport readback")) {
    return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  for (u32 i = 0; i < count; ++i) {
    sc.memexport_streams[i][0] = streams[i].base;
    sc.memexport_streams[i][1] = streams[i].size;
    sc.memexport_streams[i][2] = offsets[i];
  }
  if (!fresh_count) {  // halo3-nx: all in place: only ordered with the draws and conversions before
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
    return true;
  }
  // sasrt-nx (2026-11-08, sasrt_gpu_fetch_copy_compute; the GPU round): on deko3d the seeds are copied by fetch_copy_cs on
  // the 3D engine: a wait for the 3D engine before (the draws and conversions that read or wrote the buffer) instead of a
  // full barrier, and the export draw that follows no longer races the copy engine (the barrier below waits for the 3D
  // engine only, and the copy engine is not it). The same bytes from the same upload in the same places.
  bool dispatched = false;
  if (IsDeko3D() && s.fetch_copy_pipeline && sasrt::gpu_round::On(sasrt::gpu_round::kFetchCopyCompute) &&
      SasrtSeedExportsComputeLocked(s, buffer, fresh, fresh_count, fresh_offsets, in_place, &dispatched)) {
#if defined(PLUME_DEKO3D)
    // sasrt-nx (2026-11-08, sasrt_gpu_seed_cpu_direct; the third GPU round): no dispatch recorded (the seeds written by
    // the CPU, or none): the export draw writes fresh windows only, which nothing recorded before reads or writes, so it
    // waits for nothing; SeedFromPendingExportsLocked's copies wait with their own barriers. Kept when a memexport draw
    // of this list has had no buffer wait since (the export draw may read that draw's windows by address).
    if (!dispatched && sasrt::gpu_round::On(sasrt::gpu_round::kSeedCpu) &&
        !(g_sasrt_export_write.cmd == cmd && plume::Deko3DBufferWaits(cmd) <= g_sasrt_export_write.waits)) {
      sasrt::gpu_round::Count(sasrt::gpu_round::kSeedCpu, 1, 1);
      return SeedFromPendingExportsLocked(s, buffer, fresh, fresh_count, fresh_offsets);
    }
#endif
    // sasrt-nx (2026-11-08, sasrt_gpu_seed_wait_only; the second GPU round): what follows the seeds only writes the
    // windows (the export draw; SeedFromPendingExportsLocked's copies wait with their own full barrier): a wait for the 3D
    // engine orders them, and the texture, data and constant caches, which nothing here reads the windows through, are
    // kept. Every later reader of a window has its own barrier that drops them (the conversions', the readback's).
    if (sasrt::gpu_round::On(sasrt::gpu_round::kSeedWaitOnly)) {
      cmd->barriers(plume::RenderBarrierStage::COMPUTE, static_cast<const plume::RenderBufferBarrier *>(nullptr), 0u);
      sasrt::gpu_round::Count(sasrt::gpu_round::kSeedWaitOnly);
    } else {
      cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                    plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
    }
    return SeedFromPendingExportsLocked(s, buffer, fresh, fresh_count, fresh_offsets);
  }
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  for (u32 i = 0; i < fresh_count; ++i) {
    // halo3-nx: a window an older slot holds whole seeds all of this one on the GPU (SeedFromPendingExportsLocked): guest memory is not read.
    u32 older_slot;
    uint64_t older_offset;
    if (in_place && FindExportWindow(fresh[i].base, fresh[i].size, older_slot, older_offset))
      continue;
    UploadAllocation seed = Video::UploadsLocked().Allocate(fresh[i].size, 256);
    const u8 *guest = GuestPhysicalPtr<const u8>(fresh[i].base);
    if (!seed || !guest)
      return false;
    std::memcpy(seed.cpu, guest, fresh[i].size);
    cmd->copyBufferRegion(plume::RenderBufferReference(buffer, fresh_offsets[i]), seed.ref,
                          fresh[i].size);
    g_seed_bytes += fresh[i].size;  // banjo-nx (2026-10-26): the 10 s line's
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  return SeedFromPendingExportsLocked(s, buffer, fresh, fresh_count, fresh_offsets);
}

// banjo-nx (2026-10-26, GPU, banjo_renut_batched_readbacks): the readback copies FinishMemExportLocked left for the
// end of the list (FlushExportReadbacksLocked, from FinishRecordingLocked before every submit that ends a slot's
// recording or waits for it). Each keeps the export buffer it was written to (a buffer replaced since stays alive with
// the slot's retired objects); the copy goes to the slot's readback buffer of the flush, the one
// CompleteMemExportsLocked reads.
struct DeferredReadback {
  plume::RenderBuffer *buffer;
  u32 offset, size;
};
std::vector<DeferredReadback> g_deferred_readbacks;
uint64_t g_memexport_draws = 0, g_readback_groups = 0;  // the 10 s line's

// After the draw: copies its windows to the slot's readback buffer, pending
// until the slot's fence is waited. banjo-nx (2026-10-26): whether it recorded the copies (false: left for the list's
// end, banjo_renut_batched_readbacks). Every later reader of a window waits for the draw with its own barrier: the
// stream conversion's buffer barrier and the copies' (Primitives or Full: deko3d's Primitives completes all earlier
// 3D work, fragments included), and the CPU reads after the slot's fence.
// sasrt-nx (2026-11-08, sasrt_gpu_convert_lead_wait): g_sasrt_export_write (before SasrtSeedExportsComputeLocked).

bool FinishMemExportLocked(VideoState &s, const MemExportStream *streams, u32 count,
                           const u32 *offsets) {
  plume::RenderCommandList *cmd = s.command_list;
#if defined(PLUME_DEKO3D)
  if (IsDeko3D())
    g_sasrt_export_write = {cmd, plume::Deko3DBufferWaits(cmd)};
#endif
  if (SasrtOrderEvery() > 0 && count) [[unlikely]]  // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order)
    SasrtOrderToken(count > 1 ? fmt::format("E{:x}/{}", offsets[0] / 256, count) : fmt::format("E{:x}", offsets[0] / 256));
  plume::RenderBuffer *buffer = s.memexport_buffer[s.frame].get();
  plume::RenderBuffer *readback = s.memexport_readback[s.frame].get();
  SlotExports &slot = g_exports[s.frame];
  ++g_memexport_draws;
  static const bool batched = REXCVAR_GET(banjo_renut_batched_readbacks);
  if (batched && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 24) {  // A/B target 24
    for (u32 i = 0; i < count; ++i) {
      // halo3-nx (banjo_renut_export_in_place): a window written again is read back once, after all of this list's writes.
      bool deferred = false;
      for (const DeferredReadback &r : g_deferred_readbacks)
        deferred = deferred || (r.buffer == buffer && r.offset == offsets[i] && r.size == streams[i].size);
      if (!deferred)
        g_deferred_readbacks.push_back({buffer, offsets[i], streams[i].size});
      bool known = false;
      for (const PendingExport &p : slot.pending)
        known = known || (p.base == streams[i].base && p.size == streams[i].size && p.offset == offsets[i]);
      MarkExportGranules(streams[i].base, streams[i].size);  // (its generation: what the window holds changed)
      if (known)
        continue;
      slot.pending.push_back({streams[i].base, streams[i].size, offsets[i]});
      ++g_pending_export_count;
      slot.used = offsets[i] + ((streams[i].size + 255u) & ~255u);
    }
    return false;
  }
  ++g_readback_groups;
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::READ));
  for (u32 i = 0; i < count; ++i) {
    cmd->copyBufferRegion(plume::RenderBufferReference(readback, offsets[i]),
                          plume::RenderBufferReference(buffer, offsets[i]), streams[i].size);
    slot.pending.push_back({streams[i].base, streams[i].size, offsets[i]});
    ++g_pending_export_count;
    MarkExportGranules(streams[i].base, streams[i].size);
    slot.used = offsets[i] + ((streams[i].size + 255u) & ~255u);
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::WRITE));
  return true;
}

void FlushExportReadbacksLocked() {
  if (g_deferred_readbacks.empty())
    return;
  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBuffer *readback = s.memexport_readback[s.frame].get();
  if (!cmd || !readback) {
    g_deferred_readbacks.clear();
    return;
  }
  ++g_readback_groups;
#if defined(PLUME_DEKO3D)
  // sasrt-nx (2026-11-08, sasrt_gpu_readback_compute; the GPU round): on deko3d the copies to the readback buffer are
  // fetch_copy_cs dispatches on the 3D engine when every source is one of the slots' export buffers: a wait for the 3D
  // engine before (the exports' writes) instead of a full barrier, then a wait and the L2's dirty lines to memory for the
  // CPU (Deko3DFlushL2). The copy engine's copies had no flush of their own; its full barrier waited for everything.
  if (IsDeko3D() && s.fetch_copy_pipeline && sasrt::gpu_round::On(sasrt::gpu_round::kReadbackCompute)) {
    plume::RenderBuffer *e0 = s.memexport_buffer[0].get(), *e1 = kFramesInFlight > 1 ? s.memexport_buffer[1].get() : nullptr;
    bool ok = true;
    for (const DeferredReadback &r : g_deferred_readbacks)
      ok = ok && (r.buffer == e0 || r.buffer == e1) && r.buffer && !((r.offset | r.size) & 3u);
    if (ok) {
      plume::RenderBuffer *const fallback = readback;
      const plume::RenderBuffer *buffers[kFetchCopyBuffers] = {e0 ? e0 : fallback, e1 ? e1 : fallback, fallback, readback};
      std::unique_ptr<plume::RenderDescriptorSet> &set = s.readback_copy_set[s.frame];
      if (!set || std::memcmp(buffers, s.readback_copy_set_buffers[s.frame], sizeof(buffers)) != 0) {
        if (set)
          Video::RetireLocked(std::move(set));
        set = detail::CreateFetchCopySet(s);
        if (set) {
          for (u32 i = 0; i < kFetchCopyBuffers; ++i)
            set->setBuffer(i, const_cast<plume::RenderBuffer *>(buffers[i]));
          std::memcpy(s.readback_copy_set_buffers[s.frame], buffers, sizeof(buffers));
        }
      }
      if (set) {
        plume::RenderBufferBarrier reads[kFramesInFlight + 1];
        u32 n = 0;
        reads[n++] = plume::RenderBufferBarrier(readback, plume::RenderBufferAccess::WRITE);
        if (e0)
          reads[n++] = plume::RenderBufferBarrier(e0, plume::RenderBufferAccess::READ);
        if (e1)
          reads[n++] = plume::RenderBufferBarrier(e1, plume::RenderBufferAccess::READ);
        // sasrt-nx (2026-11-08, sasrt_gpu_readback_barriers_trim; the second GPU round): the copies load the windows as
        // storage buffers and store to the readback buffer the CPU reads after the slot's fence: a wait that drops the
        // global-data cache alone.
        const bool trim = sasrt::gpu_round::On(sasrt::gpu_round::kReadbackTrim);
        if (trim) {
          plume::Deko3DBufferWait(cmd, plume::Deko3DGlobalDataInvalidate, true);
          sasrt::gpu_round::Count(sasrt::gpu_round::kReadbackTrim);
        } else {
          cmd->barriers(plume::RenderBarrierStage::COMPUTE, reads, n);
        }
        cmd->setComputePipelineLayout(s.fetch_copy_layout.get());
        cmd->setPipeline(s.fetch_copy_pipeline.get());
        cmd->setComputeDescriptorSet(set.get(), 0);
        // sasrt-nx (2026-11-08, sasrt_gpu_readback_spans; the second GPU round): windows that follow one another in
        // the same export buffer (each starts where the one before ends, 256-byte aligned) copied by one dispatch, the
        // alignment padding between them with them (bytes nothing reads: the CPU takes the windows alone). The race's
        // 44 a frame were 44 dispatches.
        const bool spans = sasrt::gpu_round::On(sasrt::gpu_round::kReadbackSpans);
        FetchCopyPush push{};
        u32 span_end = 0;  // bytes, the open span's end (push.words counts it)
        bool open_span = false;
        auto flush_span = [&]() {
          if (!open_span)
            return;
          cmd->setComputePushConstants(0, &push, 0, sizeof(push));
          cmd->dispatch((push.words + 63u) / 64u, 1, 1);
          open_span = false;
        };
        for (const DeferredReadback &r : g_deferred_readbacks) {
          const u32 source = r.buffer == e0 ? 0u : 1u;
          const u32 aligned_end = (span_end + 255u) & ~255u;
          if (spans && open_span && push.source == source && u32(r.offset) == aligned_end &&
              (u32(r.offset) + r.size - push.source_offset) / 4u <= 65535u * 64u) {
            push.words = (u32(r.offset) + r.size - push.source_offset) / 4u;
            span_end = u32(r.offset) + r.size;
            sasrt::gpu_round::Count(sasrt::gpu_round::kReadbackSpans);
            continue;
          }
          flush_span();
          push = FetchCopyPush{};
          push.source = source;
          push.source_offset = u32(r.offset);
          push.dest_offset = u32(r.offset);
          push.words = u32(r.size / 4u);
          span_end = u32(r.offset) + r.size;
          open_span = true;
        }
        flush_span();
        // sasrt-nx (2026-11-08, sasrt_gpu_readback_fence_flush; the second GPU round): the CPU reads the readback
        // buffer after waiting for the slot's fence, which plume signals with deko3d's cache flush (the L2 written out
        // first): no flush of its own here.
        if (sasrt::gpu_round::On(sasrt::gpu_round::kFenceFlush))
          sasrt::gpu_round::Count(sasrt::gpu_round::kFenceFlush);
        else
          plume::Deko3DFlushL2(cmd);
        // sasrt-nx (2026-11-08, sasrt_gpu_readback_barriers_trim): the list ends next (FinishRecordingLocked); the slot's
        // windows are written again only after its fence was waited (or the CPU waited for this list): no wait after.
        if (!trim) {
          for (u32 i = 1; i < n; ++i)
            reads[i].accessBits = plume::RenderBufferAccess::WRITE;
          cmd->barriers(plume::RenderBarrierStage::GRAPHICS, reads + 1, n - 1);
        }
        sasrt::gpu_round::Count(sasrt::gpu_round::kReadbackCompute);
        g_deferred_readbacks.clear();
        g_bound.pipeline = nullptr;
        g_bound.framebuffer = nullptr;
        return;
      }
    }
  }
#endif
  plume::RenderBufferBarrier barriers[4];
  u32 barrier_count = 0;
  for (const DeferredReadback &r : g_deferred_readbacks) {
    bool listed = false;
    for (u32 i = 0; i < barrier_count; ++i)
      listed |= barriers[i].buffer == r.buffer;
    if (!listed && barrier_count < 4)
      barriers[barrier_count++] = plume::RenderBufferBarrier(r.buffer, plume::RenderBufferAccess::READ);
  }
  cmd->barriers(plume::RenderBarrierStage::COPY, barriers, barrier_count);
  for (const DeferredReadback &r : g_deferred_readbacks) {
    cmd->copyBufferRegion(plume::RenderBufferReference(readback, r.offset),
                          plume::RenderBufferReference(r.buffer, r.offset), r.size);
  }
  for (u32 i = 0; i < barrier_count; ++i)
    barriers[i].accessBits = plume::RenderBufferAccess::WRITE;
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, barrier_count);
  g_deferred_readbacks.clear();
  g_bound.framebuffer = nullptr;  // the copies end any render pass
}

// Brings the pending exports of 'slots' to guest memory, with those of every
// older slot (so a later export of the same memory lands last): waits for the
// older slots' fences, and submits the open list and waits for it when the
// current slot is one of them.
void WaitForExportsLocked(u32 slots) {
  auto &s = State();
  u32 last = 0;  // in age order: 1 is the oldest slot, kFramesInFlight the current one
  for (u32 k = 1; k <= kFramesInFlight; ++k) {
    if (slots & (1u << ((s.frame + k) % kFramesInFlight)))
      last = k;
  }
  for (u32 k = 1; k <= last; ++k) {
    const u32 slot = (s.frame + k) % kFramesInFlight;
    if (g_exports[slot].pending.empty())
      continue;
    if (slot == s.frame)
      detail::SubmitAndWaitLocked(s);
    else
      detail::WaitSlotLocked(s, slot);
    detail::CompleteMemExportsLocked(slot);
  }
}

}  // namespace
// Port 170 (from fnxf_xiii, its note of 2026-11-16; banjo_fence_waits_exports): a D3D fence tells the game its GPU work
// is done; the GPU's memexport results (fnxf_xiii: the game's copy kernel moving its physical heap's blocks) must be in
// guest memory by then, or the game writes the freed memory and the late results land over it (fnxf_xiii's damaged
// loading screens). On the render thread, before a fence completes (backend/render_thread.cpp QueueFenceCompletion):
// every pending export brought to guest memory (submit and wait when they are this frame's). A GPU sync on every fence
// while exports are pending, so off by default.
void SettleExportsForFence() {
  static const bool on = REXCVAR_GET(banjo_fence_waits_exports);
  if (!on)
    return;
  std::lock_guard lock(State().mutex);
  u32 slots = 0;
  for (u32 slot = 0; slot < kFramesInFlight; ++slot)
    if (!g_exports[slot].pending.empty())
      slots |= 1u << slot;
  if (slots)
    WaitForExportsLocked(slots);
}
namespace {

// banjo-nx (2026-10-26, GPU, banjo_renut_convert_reuse): the conversions made in this frame's slot, by everything
// stream_convert_cs reads (the export slot, its offset, the vertices, both strides and the elements). The export
// buffers change only with a new export, which raises g_export_generation; the slot's stream buffer only grows within
// a frame (DrainSlot empties it at the frame's start: 'used' going back, or a new buffer, clears the memo). A hit's
// output was made visible to the vertex fetch by its own dispatch's barrier.
struct ConvertMemo {
  struct Entry {
    u32 slot, offset, count, stride, host_stride, element_count;
    u32 elements[2 * kStreamConvertTotalElements];
    uint64_t dest_offset;
    uint64_t generation;  // sasrt-nx (2026-11-08): g_export_generation when made (the count of sasrt_gpu_convert_memo_keep)
  };
  // sasrt-nx (2026-11-08, sasrt_gpu_convert_memo_keep): 64 (a race frame converts about 88 streams), 32 before.
  static constexpr u32 kEntries = 64;
  Entry entries[kEntries];
  u32 count = 0, next = 0;
  uint64_t generation = 0, used_high = 0;
  // sasrt-nx (2026-11-08, sasrt_gpu_convert_memo_keep): what the memo was made under, with the key on.
  uint64_t sasrt_rebuilds = 0, sasrt_rewrites = 0;
  const plume::RenderBuffer *sasrt_export_buffers[2] = {};
  u32 frame = ~0u;
  const plume::RenderBuffer *dest = nullptr;
  uint64_t reused = 0, dispatched = 0;  // the 10 s line's
};
ConvertMemo g_convert_memo;

// Builds the host vertices of the draw's streams whose newest data is in a
// pending export's window (FindExportedStream; DrawNeedsExportFlush did not
// wait for those) with stream_convert_cs, into the slot's stream buffer.
// Records dispatches: before the framebuffer is bound.
bool ConvertExportedStreamsLocked(VideoState &s, const GuestDevice &device, const DrawCall &call,
                                  const ShaderRecord &vs, const u32 *shader_strides,
                                  const VertexDeclarationRecord &declaration, const Geometry &g,
                                  GpuStreams &out) {
  static const bool no_stream_convert = REXCVAR_GET(banjo_renut_diag_no_stream_convert);  // banjo-nx (2026-10-23)
  if (!s.stream_convert_pipeline || !AnyPendingExports() || no_stream_convert)
    return true;
  struct Job {
    u32 stream;
    ExportedStream src;
    u32 first, count;
  };
  // banjo-nx (2026-10-19, render-thread proposal 4): storage only (ExportedStream's member initialisers zeroed all 16
  // jobs, about 1.8 KB, at every draw while exports were pending); a job is made for each stream that has one.
  alignas(Job) unsigned char job_storage[sizeof(Job) * kMaxVertexStreams];
  Job *jobs = reinterpret_cast<Job *>(job_storage);
  u32 job_count = 0;
  uint64_t total = 0;
  static const bool reuse_on = REXCVAR_GET(banjo_renut_convert_reuse);
  const bool reuse = reuse_on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 23;  // A/B target 23
  ConvertMemo &memo = g_convert_memo;
  // sasrt-nx (2026-11-08, sasrt_gpu_convert_memo_keep; the second GPU round): with the key, a new export does not clear
  // the memo (its window is fresh: a key no entry has); a slot's exports completing, a window written again in place,
  // or an export buffer replaced does.
  const bool keep = reuse && sasrt::gpu_round::On(sasrt::gpu_round::kConvertMemoKeep);
  const plume::RenderBuffer *export_buffers[2] = {s.memexport_buffer[0].get(),
                                                  kFramesInFlight > 1 ? s.memexport_buffer[1].get() : nullptr};
  const bool memo_stale = keep ? memo.sasrt_rebuilds != g_sasrt_export_rebuilds ||
                                     memo.sasrt_rewrites != g_sasrt_window_rewrites ||
                                     memo.sasrt_export_buffers[0] != export_buffers[0] ||
                                     memo.sasrt_export_buffers[1] != export_buffers[1]
                               : memo.generation != g_export_generation;
  if (reuse && (memo_stale || memo.frame != s.frame ||
                memo.dest != s.exported_streams[s.frame].get() ||
                s.exported_streams_used[s.frame] < memo.used_high)) {
    memo.count = memo.next = 0;
    memo.sasrt_rebuilds = g_sasrt_export_rebuilds;
    memo.sasrt_rewrites = g_sasrt_window_rewrites;
    memo.sasrt_export_buffers[0] = export_buffers[0];
    memo.sasrt_export_buffers[1] = export_buffers[1];
    memo.generation = g_export_generation;
    memo.frame = s.frame;
    memo.dest = s.exported_streams[s.frame].get();
    memo.used_high = 0;
  }
  // banjo-nx (2026-10-23, batch 5): the used streams' bits (used_mask, bit s = streams[s].used), not all 16 tested.
  for (u32 left = declaration.layout.used_mask; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream);
    const u32 base = device.VertexFetchWord(fetch_index, 0) & ~3u;
    const u32 size = device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu;
    if (!PendingExportSlotsMemo(kExportAnswerFetch + ((fetch_index - 64) & 31), base, size))  // banjo-nx (batch 6)
      continue;
    Job &job = *new (&jobs[job_count]) Job{stream};  // made in place, kept by the ++job_count below
    if (!FindExportedStream(device, call, vs, shader_strides, declaration, stream, job.src))
      continue;
    // The vertices BindStreams would convert.
    job.first = g.first_vertex;
    if (job.first >= job.src.vertex_count)
      continue;
    job.count = std::min(g.vertex_range, job.src.vertex_count - job.first);
    if (!job.count)
      continue;
    if (reuse) {
      const u32 offset = u32(job.src.offset + uint64_t(job.first) * job.src.stride);
      const ConvertMemo::Entry *hit = nullptr;
      for (u32 i = 0; i < memo.count; ++i) {
        const ConvertMemo::Entry &e = memo.entries[i];
        if (e.offset == offset && e.count == job.count && e.slot == job.src.slot && e.stride == job.src.stride &&
            e.host_stride == job.src.host_stride && e.element_count == job.src.element_count &&
            std::memcmp(e.elements, job.src.elements, sizeof(e.elements)) == 0) {
          hit = &e;
          break;
        }
      }
      if (hit) {
        out.ref[stream] = plume::RenderBufferReference(memo.dest, hit->dest_offset);
        out.bytes[stream] = job.count * job.src.host_stride;
        ++memo.reused;
        if (SasrtOrderEvery() > 0) [[unlikely]]  // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order)
          SasrtOrderToken(fmt::format("H{:x}", offset / 256));
        if (hit->generation != g_export_generation)  // sasrt-nx (2026-11-08): banjo-nx's memo had forgotten it
          sasrt::gpu_round::Count(sasrt::gpu_round::kConvertMemoKeep);
        continue;
      }
    }
    total += (uint64_t(job.count) * job.src.host_stride + 255u) & ~uint64_t(255);
    ++job_count;
  }
  if (!job_count)
    return true;

  // A buffer too small is replaced; the draws before still read the old one,
  // retired with the slot.
  uint64_t &used = s.exported_streams_used[s.frame];
  if (!s.exported_streams[s.frame] || used + total > s.exported_streams_capacity[s.frame]) {
    if (!EnsureBuffer(s.exported_streams[s.frame], s.exported_streams_capacity[s.frame],
                      used + total,
                      plume::RenderBufferDesc::DefaultBuffer(
                          0, StorageBufferFlags() | plume::RenderBufferFlag::VERTEX),
                      "renut exported streams")) {
      return false;
    }
    used = 0;
    memo.count = memo.next = 0;
    memo.used_high = 0;
  }
  plume::RenderBuffer *dest = s.exported_streams[s.frame].get();
  const plume::RenderBuffer *buffers[kStreamConvertBuffers];
  for (u32 slot = 0; slot < kFramesInFlight; ++slot)
    buffers[slot] = s.memexport_buffer[slot] ? s.memexport_buffer[slot].get() : dest;
  buffers[kFramesInFlight] = dest;
  if (!s.stream_convert_set ||
      std::memcmp(buffers, s.stream_convert_set_buffers, sizeof(buffers)) != 0) {
    // Dispatches recorded before still use the old set.
    Video::RetireLocked(std::move(s.stream_convert_set));
    s.stream_convert_set = detail::CreateStreamConvertSet(s);
    if (!s.stream_convert_set)
      return false;
    for (u32 i = 0; i < kStreamConvertBuffers; ++i)
      s.stream_convert_set->setBuffer(i, buffers[i]);
    std::memcpy(s.stream_convert_set_buffers, buffers, sizeof(buffers));
  }

  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier barriers[kFramesInFlight + 1];
  u32 barrier_count = 0;
  barriers[barrier_count++] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::WRITE);
  for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
    if (s.memexport_buffer[slot]) {
      barriers[barrier_count++] =
          plume::RenderBufferBarrier(s.memexport_buffer[slot].get(), plume::RenderBufferAccess::WRITE);
    }
  }
  static const uint32_t kProfileLabel = gpu_profile::Label("exported stream conversion");
  gpu_profile::Mark(s, kProfileLabel);
  // sasrt-nx (2026-11-08, sasrt_gpu_convert_lead_wait; the second GPU round): no memexport draw since the list's last
  // buffer barrier: what the conversion reads is written and its caches dropped, and the part of the stream buffer it
  // writes is fresh (past 'used'): no wait first. The barrier after it stays.
  bool lead = true;
#if defined(PLUME_DEKO3D)
  if (IsDeko3D() && sasrt::gpu_round::On(sasrt::gpu_round::kConvertLead) && g_sasrt_export_write.cmd == cmd &&
      plume::Deko3DBufferWaits(cmd) > g_sasrt_export_write.waits) {
    lead = false;
    sasrt::gpu_round::Count(sasrt::gpu_round::kConvertLead);
  }
#endif
  if (lead && SasrtExpNoChainWaits())  // sasrt-nx (2026-11-08): an EXPERIMENT (not identical): no wait
    lead = false;
  if (lead) {
#if defined(PLUME_DEKO3D)
    // sasrt-nx (2026-11-08, sasrt_gpu_convert_lead_data_only; the second GPU round): the conversion reads the export
    // windows by storage-buffer loads (the global-data cache) and its push constants from fresh transient memory
    // (Deko3DOptions::freshComputeConstants): the wait drops that cache alone, not the texture and constant caches.
    if (IsDeko3D() && sasrt::gpu_round::On(sasrt::gpu_round::kConvertLeadData)) {
      plume::Deko3DBufferWait(cmd, plume::Deko3DGlobalDataInvalidate, true);
      sasrt::gpu_round::Count(sasrt::gpu_round::kConvertLeadData);
    } else
#endif
      cmd->barriers(plume::RenderBarrierStage::COMPUTE, barriers, barrier_count);
  }
  if (SasrtOrderEvery() > 0) [[unlikely]] {  // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order)
    const Targets *t = g_handover_targets;
    std::string token = fmt::format("C{}x{}{}", t ? t->width : 0, t ? t->height : 0, lead ? "L" : "");
    for (u32 i = 0; i < job_count; ++i)
      token += fmt::format(",{:x}:{}", u32(jobs[i].src.offset + uint64_t(jobs[i].first) * jobs[i].src.stride) / 256,
                           jobs[i].count);
    SasrtOrderToken(token);
  }
  cmd->setComputePipelineLayout(s.stream_convert_layout.get());
  cmd->setPipeline(s.stream_convert_pipeline.get());
  cmd->setComputeDescriptorSet(s.stream_convert_set.get(), 0);
  for (u32 i = 0; i < job_count; ++i) {
    const Job &job = jobs[i];
    const u32 bytes = job.count * job.src.host_stride;
    StreamConvertPush push{};
    push.source = job.src.slot;
    push.source_offset = u32(job.src.offset + uint64_t(job.first) * job.src.stride);
    push.dest_offset = u32(used);
    push.vertices = job.count;
    push.guest_stride_words = job.src.stride / 4;
    push.host_stride_words = job.src.host_stride / 4;
    for (u32 part = 0; part < job.src.element_count; part += kStreamConvertMaxElements) {  // halo3-nx: kStreamConvertTotalElements
      push.element_count = std::min(job.src.element_count - part, kStreamConvertMaxElements);
      std::memcpy(push.elements, job.src.elements + 2 * part, sizeof(push.elements));
      cmd->setComputePushConstants(0, &push, 0, sizeof(push));  // banjo-nx: the size (the record worker copies it)
      cmd->dispatch((job.count * push.element_count + 63) / 64, 1, 1);
    }
    out.ref[job.stream] = plume::RenderBufferReference(dest, used);
    out.bytes[job.stream] = bytes;
    if (reuse) {
      ConvertMemo::Entry &e = memo.entries[memo.next];
      e = {job.src.slot, push.source_offset, job.count, job.src.stride, job.src.host_stride, job.src.element_count,
           {}, used, g_export_generation};
      std::memcpy(e.elements, job.src.elements, sizeof(e.elements));
      memo.next = (memo.next + 1) % ConvertMemo::kEntries;
      memo.count = std::min(memo.count + 1, ConvertMemo::kEntries);
    }
    ++memo.dispatched;
    used += (uint64_t(bytes) + 255u) & ~uint64_t(255);
  }
  memo.dest = dest;
  memo.used_high = used;
  barriers[0] = plume::RenderBufferBarrier(dest, plume::RenderBufferAccess::READ);
#if defined(PLUME_DEKO3D)
  // sasrt-nx (2026-11-08, sasrt_gpu_convert_tail_vertex_only; the second GPU round): the converted stream's reader is
  // the draw's vertex fetch: the wait drops the texture cache (as every buffer barrier did; the fetch is given it), not
  // the shaders' global-data and constant caches, which no reader of the stream buffer goes through.
  if (SasrtExpNoChainWaits()) {  // sasrt-nx (2026-11-08): an EXPERIMENT (not identical): no wait
  } else
  if (IsDeko3D() && sasrt::gpu_round::On(sasrt::gpu_round::kConvertTail)) {
    // sasrt-nx (2026-11-08, sasrt_gpu_convert_tail_no_texture_drop; the third GPU round): the converted streams were
    // written past 'used' (fresh in the frame: nothing read them since the last queue flush, which dropped the texture
    // cache): no cache holds their lines, so the wait keeps the texture cache too.
    const bool keep_texture = sasrt::gpu_round::On(sasrt::gpu_round::kConvertTailNoImage);
    plume::Deko3DBufferWait(cmd, keep_texture ? 0u : uint32_t(DkInvalidateFlags_Image), false);
    sasrt::gpu_round::Count(sasrt::gpu_round::kConvertTail);
    if (keep_texture)
      sasrt::gpu_round::Count(sasrt::gpu_round::kConvertTailNoImage);
  } else
#endif
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, barrier_count);
  // The compute pipeline replaced the graphics one (D3D12), and the barriers
  // end any render pass.
  g_bound.pipeline = nullptr;
  g_bound.framebuffer = nullptr;
  return true;
}

// Stands in for a null vertex declaration: the vertex shader fetches its data
// by address (vfetchMemory), and any input it declares reads the zero stream.
VertexDeclarationRecord &NoDeclaration() {
  static VertexDeclarationRecord record = [] {
    VertexDeclarationRecord r;
    r.layout = BuildHostVertexLayout(nullptr, 0);
    r.layout_hash = 0x4E4F44454300ull;  // any value no FNV hash of a real layout hits
    return r;
  }();
  return record;
}

// katamari-nx (2026-10-30, from peggle2-nx's fix of 2026-10-20): the device's own vertex declaration. Renut's
// CreateDevice points the device's declaration at the block embedded in the device (+0x2F98 on XDK 2.0.5632; N&B's
// update +0x2FA8), and the XDK's SetFVF rebuilds that block from an FVF; it is no record of ours. The first PC run drew
// with it ("Draw dropped: vertex declaration ABE92F98 is not one the renderer knows (words 00000005 00000001)").
// Its D3DVERTEXELEMENT9s (12 bytes each, ended by D3DDECL_END) are at +52 of the declaration, inside the device
// snapshot, so a draw reads them from its own snapshot; at most 15 before the device's next field (+0x3088). One record
// per distinct element list; each new list is logged once with its elements, to check the layout.
constexpr u32 kDeviceDeclarationOffset = 0x2F98;
constexpr u32 kDeclarationElementsOffset = 52;
constexpr u32 kDeviceDeclarationMaxElements = 15;

const VertexDeclarationRecord *DeviceDeclarationLocked(const GuestDevice &device) {
  static std::unordered_map<uint64_t, std::unique_ptr<VertexDeclarationRecord>> records;
  static uint64_t last_hash = 0;
  static const VertexDeclarationRecord *last = nullptr;
  const u32 first = kDeviceDeclarationOffset + kDeclarationElementsOffset;
  u32 count = 0;
  while (count < kDeviceDeclarationMaxElements && device.U32(first + count * 12 + 4) != kDeclTypeUnused)
    ++count;
  if (!count || count == kDeviceDeclarationMaxElements)
    return nullptr;
  const uint64_t hash = XXH3_64bits(device.Bytes(first), count * 12);
  if (last && hash == last_hash)
    return last;
  auto &record = records[hash];
  if (!record) {
    record = std::make_unique<VertexDeclarationRecord>();
    const auto *elements = reinterpret_cast<const GuestVertexElement *>(device.Bytes(first));
    record->elements.assign(elements, elements + count);
    BuildDeclarationLayout(*record);
    std::string words;
    for (u32 i = 0; i < count; ++i)
      words += fmt::format(" {:08X}:{:08X}:{:08X}", device.U32(first + i * 12), device.U32(first + i * 12 + 4),
                           device.U32(first + i * 12 + 8));
    RGPU_INFO("[katamari] device declaration {:016X}: {} elements{}", hash, count, words);
  }
  last_hash = hash;
  last = record.get();
  return last;
}

} // namespace

// banjo-nx (2026-10-27, the user's shadow-caster culling): render/draw.h.
uint64_t g_shadow_culled = 0, g_shadow_culled_indices = 0;
u32 ShadowCullIndices() {
  static const u32 limit = u32(std::max<int32_t>(0, REXCVAR_GET(banjo_renut_shadow_cull_indices)));
  return limit;
}
bool ShadowCasterCulledLocked(const u8 *device_state, const DrawCall &call) {
  const u32 limit = ShadowCullIndices();
  if (call.count > limit || call.tessellation || call.up_vertices || call.up_indices)
    return false;
  if (banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 44)
    return false;  // A/B 44's off half: every shadow drawn (the same session, culled and not, 30 s each)
  // Meshes only: triangle lists, fans and strips, quad lists (no points, lines or rectangle lists).
  if (!(call.primitive == kTriangleList || call.primitive == kTriangleFan || call.primitive == kTriangleStrip ||
        call.primitive == kQuadList))
    return false;
  const GuestDevice d(device_state);
  const d3d::Device &dev = d.dev();
  const u32 dc = u32(dev.m_ControlPacket.DepthControl);
  if ((dc & 1) || !(dc & 2) || !(dc & 4))
    return false;  // a stencil test, or no depth test or no depth write: not a shadow caster's draw
  const u32 mask = u32(dev.m_ValuesPacket.ColorMask);
  for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i) {
    if (d.U32(d3d::kDeviceOffsetRenderTargets + i * 4) && ((mask >> (4 * i)) & 0xFu))
      return false;  // writes colour
  }
  const u32 ds = d.U32(d3d::kDeviceOffsetDepthStencil);
  if (!ds)
    return false;
  // The depth target's shape, by its address while no target was built or retired (the surface epoch).
  static u32 last_ds = 0;
  static uint64_t last_epoch = 0;
  static bool last_shadow = false;
  if (ds != last_ds || last_epoch != SurfaceEpochLocked()) {
    const HostSurface *s = GetHostSurfaceLocked(ds);
    last_shadow = s && s->depth && s->width == s->height && s->width >= 256;
    last_ds = ds;
    last_epoch = SurfaceEpochLocked();
  }
  if (!last_shadow)
    return false;
  ++g_shadow_culled;
  g_shadow_culled_indices += call.count;
  return true;
}

// banjo-nx (2026-10-27, Phase A): render/draw.h.
const DrawResources *g_draw_resources = nullptr;
bool ResourceObjectsOn() {
  static const bool on = REXCVAR_GET(banjo_renut_resource_objects);
  return on;
}
// The recording threads' lookup: open addressing by the address and the header words, at most 16 probes. A thread that
// finds a slot empty claims it (kSlotBusy), makes the object, writes the key into the slot and the object, then
// publishes the slot (release); a thread that sees it published (acquire) sees its key whole. A slot being written is
// passed over: the key may then get a second object further on, which only costs an object (each is answered alone).
bool LookupTablesOn() {
  static const bool on = REXCVAR_GET(banjo_renut_record_lookups);
  return on;
}
uint64_t TextureObjectHash(u32 va, const u32 header[6]) {
  uint64_t h = uint64_t(va) * 0x9E3779B97F4A7C15ull;
  for (u32 i = 0; i < 6; ++i)
    h = (h ^ header[i]) * 0xFF51AFD7ED558CCDull;
  h ^= h >> 31;
  __builtin_prefetch(&g_texture_slots[h & (kTextureObjectSlots - 1)]);
  return h;
}
TextureObject *FindTextureObjectAt(uint64_t h, u32 va, const u32 header[6]) {
  for (u32 probe = 0; probe < 16; ++probe) {
    TextureSlot &slot = g_texture_slots[(h + probe) & (kTextureObjectSlots - 1)];
    u32 id = slot.id.load(std::memory_order_acquire);
    if (id == 0) {
      if (slot.id.compare_exchange_strong(id, kSlotBusy, std::memory_order_acquire, std::memory_order_acquire)) {
        const u32 n = g_texture_objects_made.fetch_add(1, std::memory_order_relaxed);
        if (n >= kTextureObjects) {
          g_texture_objects_made.store(kTextureObjects, std::memory_order_relaxed);
          slot.id.store(0, std::memory_order_release);
          return nullptr;  // full: the draws take the path without objects
        }
        TextureObject *made = &g_texture_objects[n];
        made->va = va;
        std::memcpy(made->header, header, sizeof(made->header));
        slot.va = va;
        std::memcpy(slot.header, header, sizeof(slot.header));
        slot.id.store(n + 1, std::memory_order_release);
        return made;
      }
      // (another thread claimed or published it first: 'id' holds what it put there)
    }
    if (id == kSlotBusy)
      continue;
    if (slot.va == va && SameWords<6>(slot.header, header))
      return &g_texture_objects[id - 1];
  }
  return nullptr;
}
TextureObject *FindTextureObject(u32 va, const u32 header[6]) {
  return FindTextureObjectAt(TextureObjectHash(va, header), va, header);
}

// banjo-nx (2026-10-27, geometry objects): render/draw.h. Open addressing as FindTextureObject's, at most 16 probes.
namespace {
template <u32 kWords>
uint64_t GeometryHash(GeometrySlot<kWords> *table, u32 slots, const u32 *key, uint64_t seed) {
  uint64_t h = seed;
  for (u32 i = 0; i < kWords; ++i)
    h = (h ^ key[i]) * 0xFF51AFD7ED558CCDull;
  h ^= h >> 31;
  __builtin_prefetch(&table[h & (slots - 1)]);
  return h;
}
template <u32 kWords>
GeometryObject *FindGeometryObject(GeometrySlot<kWords> *table, u32 slots, const u32 *key, uint64_t h) {
  for (u32 probe = 0; probe < 16; ++probe) {
    GeometrySlot<kWords> &slot = table[(h + probe) & (slots - 1)];
    u32 id = slot.id.load(std::memory_order_acquire);
    if (id == 0) {
      if (slot.id.compare_exchange_strong(id, kSlotBusy, std::memory_order_acquire, std::memory_order_acquire)) {
        const u32 n = g_geometry_objects_made.fetch_add(1, std::memory_order_relaxed);
        if (n >= kGeometryObjects) {
          g_geometry_objects_made.store(kGeometryObjects, std::memory_order_relaxed);
          slot.id.store(0, std::memory_order_release);
          return nullptr;  // full: the draws take the path without objects
        }
        GeometryObject *made = &g_geometry_objects[n];
        std::memcpy(made->key, key, kWords * 4);
        std::memcpy(slot.key, key, kWords * 4);
        slot.id.store(n + 1, std::memory_order_release);
        return made;
      }
    }
    if (id == kSlotBusy)
      continue;
    if (SameWords<kWords>(slot.key, key))
      return &g_geometry_objects[id - 1];
  }
  return nullptr;
}
}  // namespace
uint64_t StreamObjectHash(const u32 key[6]) {
  return GeometryHash<6>(g_stream_slots, kStreamObjectSlots, key, 0x9E3779B97F4A7C15ull);
}
GeometryObject *FindStreamObjectAt(uint64_t hash, const u32 key[6]) {
  return FindGeometryObject<6>(g_stream_slots, kStreamObjectSlots, key, hash);
}
uint64_t IndexObjectHash(const u32 key[3]) {
  return GeometryHash<3>(g_index_slots, kIndexObjectSlots, key, 0xC2B2AE3D27D4EB4Full);
}
GeometryObject *FindIndexObjectAt(uint64_t hash, const u32 key[3]) {
  return FindGeometryObject<3>(g_index_slots, kIndexObjectSlots, key, hash);
}
GeometryObject *FindStreamObject(const u32 key[6]) { return FindStreamObjectAt(StreamObjectHash(key), key); }
GeometryObject *FindIndexObject(const u32 key[3]) { return FindIndexObjectAt(IndexObjectHash(key), key); }

// banjo-nx (2026-10-28, ARM audit): render/draw.h (Replay's stream).
// banjo-nx (2026-10-28, trimmed after the d4 console profile: the helpers' own cost was about 2 ms a frame, many
// prefetches an op filling the core's miss buffers): an index buffer's object has one answer (two lines, not three).
void PrefetchGeometryObject(const GeometryObject *o, bool index) {
  const char *p = reinterpret_cast<const char *>(o);
  __builtin_prefetch(p);       // the key (the export checks) and is32
  __builtin_prefetch(p + 64);  // answer 0
  if (!index)
    __builtin_prefetch(p + 128);  // answer 1
}
void PrefetchTextureObject(const TextureObject *o) {
  const char *p = reinterpret_cast<const char *>(o);
  __builtin_prefetch(p + 64);
  __builtin_prefetch(p + 128);
}
void PrefetchGeometryCopies(const GeometryObject *o, bool index) {
  const uint64_t gen = g_geometry_gen;
  const void *done = nullptr;
  bool cards = false;
  for (u32 k = 0; k < (index ? 1u : 2u); ++k) {
    const GeometryAnswer &a = o->answer[k];
    if (a.entry_gen != gen || !a.copy || a.copy == done)
      continue;  // (none, or the other answer's copy: fetched already)
    done = a.copy;
    if (index) {
      const auto *c = static_cast<const CachedIndices *>(a.copy);
      __builtin_prefetch(&c->copy);
      __builtin_prefetch(&c->proven_flush);
    } else {
      const auto *c = static_cast<const CachedStream *>(a.copy);
      __builtin_prefetch(&c->copy);
      __builtin_prefetch(&c->proven_flush);
    }
    // (2026-10-28, cache-miss audit 2) the cards and serials its interval proof reads (PagesClean's loads were about
    // 2 % of the render thread in the c4 console run): once an object (its answers' copies are of the same guest
    // buffer), and only for a copy not proven in this interval already (trimmed after d4: most are).
    if (!cards && a.proven != u32(g_flush_serial)) {
      PrefetchCards(a.guest, a.bytes);
      cards = true;
    }
  }
}
bool GeometryObjectsRecorded() {
  static const bool on = REXCVAR_GET(banjo_renut_geometry_objects);
  return on;
}

// banjo-nx (2026-10-27, banjo_renut_record_extents): render/draw.h (outside the anonymous namespace: command_buffers.cpp
// reads them).
const DrawSlotExtents *g_draw_slot_extents = nullptr;
bool RecordExtentsOn() {
  static const bool on = REXCVAR_GET(banjo_renut_record_extents);
  return on;
}

namespace detail {

void FinishRecordingLocked(VideoState &) {
  FlushVertexFillsLocked();
  FlushTextureFillsLocked();
  FlushExportReadbacksLocked();  // banjo-nx (2026-10-26, banjo_renut_batched_readbacks)
}

} // namespace detail

// dishonored-nx (2026-10-30; port 101): whether the guest bytes at host pointer 'p' are committed and readable, from
// the guest heaps' page tables. A fetch constant's range can reach past what the game committed (a stream's last vertex
// at the end of a heap allocation, or a buffer freed between the draw and the kick): on the Xbox 360 the GPU reads
// whatever is there, here the read faulted the render thread in ConvertVertices (dishonored-nx: guest 0x9192B000, its
// rex_crash.log of 2026-10-29). Host memory (a draw's copied user-pointer vertices) is always readable.
static bool GuestBytesReadable(const u8 *p, size_t len) {
  if (!p || !len)
    return true;
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && defined(__aarch64__))
  return true;  // heaps with host offsets there: a guest pointer is not a plain sum
#else
  rex::memory::Memory *memory = REX_KERNEL_MEMORY();
  const u8 *pbase = detail::GuestPhysicalBase();
  const u8 *vbase = detail::GuestVirtualBase();
  rex::memory::BaseHeap *heap = nullptr;
  uint64_t address = 0;
  if (p >= pbase && uint64_t(p - pbase) < 0x20000000ull) {
    address = uint64_t(p - pbase);
    heap = memory->GetPhysicalHeap();
  } else if (p >= vbase && uint64_t(p - vbase) < 0x100000000ull) {
    address = uint64_t(p - vbase);
    heap = memory->LookupHeap(uint32_t(address));
    // dishonored-nx (2026-11-01, the black objects): a view of physical memory (0xA0000000 64 KB pages, 0xC0000000
    // 16 MB, 0xE0000000 4 KB) keeps a page table of its own allocations only; memory the game allocated through
    // another view, or as physical memory, is committed in the physical heap and read "no access" there (Unreal's
    // vertex buffers in the 0xC0000000 view came out all zeros: black models). The physical heap answers for every view.
    if (heap && heap->heap_type() == rex::memory::HeapType::kGuestPhysical) {
      const uint32_t physical = memory->GetPhysicalAddress(uint32_t(address));
      if (physical != UINT32_MAX) {
        address = physical;
        heap = memory->GetPhysicalHeap();
      }
    }
  } else {
    return true;
  }
  if (!heap || address + len - 1 > 0xFFFFFFFFull)
    return false;
  return uint32_t(heap->QueryRangeAccess(uint32_t(address), uint32_t(address + len - 1))) &
         uint32_t(rex::memory::PageAccess::kReadOnly);
#endif
}

// dishonored-nx (2026-10-31; port 101): how many bytes from 'p' on are readable (whole 4 KB pages, as far as the first
// page that is not). A stream whose range ran a little past its allocation keeps its real vertices; only the part past
// the commit is zero (dishonored-nx: zeroing it all made black models at a distance).
static size_t GuestReadablePrefix(const u8 *p, size_t len) {
  if (!banjo::renut_gpu::VertexReadableCheckOn() || GuestBytesReadable(p, len))
    return len;
  constexpr size_t kPage = 4096;
  size_t done = 0;
  const size_t head = kPage - size_t(reinterpret_cast<uintptr_t>(p) & (kPage - 1));
  size_t step = std::min(len, head);
  while (done < len) {
    if (!GuestBytesReadable(p + done, step))
      break;
    done += step;
    step = std::min(kPage, len - done);
  }
  return done;
}

void FlushVertexFillsLocked() {
  ++g_flush_serial;  // banjo-nx: banjo_renut_exact_streams' intervals
  banjo::renut_gpu::CountKick();
  static uint32_t unreadable_logged = 0;  // dishonored-nx (2026-10-30; port 101)
  for (const PendingStreamFill &f : g_stream_fills) {
    // dishonored-nx (2026-10-30/31; port 101): a range with an uncommitted page: the vertices before the first such page
    // convert, the rest are zeros, and the stream is not cached.
    const size_t bytes = size_t(f.count) * f.stride;
    const size_t readable = GuestReadablePrefix(f.guest, bytes);
    if (readable < bytes) [[unlikely]] {
      const u32 good = f.stride ? u32(readable / f.stride) : 0u;
      if (unreadable_logged < 16) {
        ++unreadable_logged;
        const u8 *pbase = detail::GuestPhysicalBase();
        const u8 *vbase = detail::GuestVirtualBase();
        const bool physical = f.guest >= pbase && uint64_t(f.guest - pbase) < 0x20000000ull;
        const bool virt = !physical && f.guest >= vbase && uint64_t(f.guest - vbase) < 0x100000000ull;
        RGPU_WARN("[streams] vertex stream {} bytes (guest {} {:08X}) not committed in guest memory past {} bytes: {} of "
                  "{} vertices converted, the rest zeros (dishonored_renut_vertex_readable_check)",
                  bytes, physical ? "physical" : (virt ? "virtual" : "?"),
                  physical ? uint32_t(f.guest - pbase) : (virt ? uint32_t(f.guest - vbase) : 0u), readable, good,
                  f.count);
      }
      if (good)
        ConvertVertices(g_fill_elements.data() + f.first_element, f.element_count, f.host_stride, f.guest, f.stride,
                        good, f.endian, f.host);
      std::memset(f.host + size_t(good) * f.host_stride, 0, size_t(f.count - good) * f.host_stride);
      continue;
    }
    if (::renut::gpu::host_batch::CensusOn())  // blur-nx (2026-11-18, the census's #12): the range fills at the kick
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kConvertRangeBytes, uint64_t(f.count) * f.host_stride);
    ConvertVertices(g_fill_elements.data() + f.first_element, f.element_count, f.host_stride, f.guest, f.stride,
                    f.count, f.endian, f.host);
    static const bool redeem = REXCVAR_GET(banjo_renut_stream_redeem);  // port 145 (tw2de_nx's), on by default
    if (f.cacheable)
      TrackConvertedStream(f);
    else if (redeem && f.stride)
      RedeemQuietStream(f);
  }
  g_stream_fills.clear();
  g_fill_elements.clear();
  // sasrt-nx (2026-11-12, batch C/D, item D2; port 93): the ones only this list holds go to the pool (64 at most, up
  // to 1 MB each)
  for (auto &p : g_held_retired)
    if (p && p.use_count() == 1 && g_held_pool.size() < 64 && p->capacity() <= (1u << 20))
      g_held_pool.push_back(std::move(p));
  g_held_retired.clear();  // sasrt-nx (2026-10-21, sasrt_renut_exports_held): no fill points into them now
  ++g_queued_generation;  // banjo-nx (2026-10-19): the table emptied
  for (const PendingCopy &c : g_copy_fills) {
    // dishonored-nx (2026-10-30/31; port 101): the fetch data copies the same way: the readable part, the rest zeros.
    const size_t readable = GuestReadablePrefix(c.src, c.size);
    if (readable)
      std::memcpy(c.dst, c.src, readable);
    if (readable < c.size) [[unlikely]]
      std::memset(c.dst + readable, 0, c.size - readable);
    g_fetch_fill_bytes += c.size;  // banjo-nx (2026-10-27): the 10 s line's
  }
  g_copy_fills.clear();
  ++g_fetch_copy_generation;
}

// sasrt-nx (2026-11-08, sasrt_gpu_exp_shadow_d32f): stencil used on a depth target made without a stencil plane.
// renut-ultimate (port 75): in this base only renut_depth_float_no_stencil makes such a target (sasrt-nx's shadow-map
// rules are not here), so the warning names it.
void SasrtWarnStencilOnD32f(const HostSurface *d, bool clear = false, bool first = false) {
  // sasrt-nx (2026-11-11, port 75; its formats batch, the diagnostic: renut_diag_formats; renut_depth_float_no_stencil's
  // question): the first stencil use of each target build, by a draw or a clear ('first': its stencil_used was still
  // false; its audit's finding 6: no lookup on every stencil draw).
  if (first && d && d->depth && banjo::renut_gpu::DiagFormatsOn()) {
    static uint32_t logged = 0;
    if (logged++ < 48)
      RGPU_INFO("[formats] stencil {} on depth target {:08X} {}x{} (guest format {}, host format {})",
                clear ? "cleared" : "used by a draw", d->guest_va, d->width, d->height,
                GpuTextureFormatName(d->format_word), int(d->format));
  }
  if (d && d->depth && d->format == plume::RenderFormat::D32_FLOAT) {
    static uint32_t logged = 0;
    if (logged++ < 16)
      RGPU_WARN("[sasrt] stencil on a depth target without stencil: {:08X} {}x{} (renut_depth_float_no_stencil)",
                d->guest_va, d->width, d->height);
  }
}

void SetTilesLocked(const ClearRect *rects, u32 count) {
  g_tiles.assign(rects, rects + (rects ? count : 0));
}

u32 SetReplayPredicationLocked(u32 mask) {
  const u32 previous = g_replay_predication;
  g_replay_predication = mask;
  return previous;
}

// banjo-nx (banjo_gpu_depth_only, package 28; Unleashed / Marathon's depth-only pipelines): a draw that writes no
// colour, into a depth target, without the alpha test or alpha to coverage, keeps nothing of its pixel shader unless the
// shader kills pixels, writes depth or exports to memory (ShaderCacheEntry::shaderFlags, which the translator checks
// against the shader's own text, and memExportConstants): it is drawn without it. Depth and stencil come from the
// rasterizer either way (no SV_Depth), so the targets get the same values; the shader's constants and textures are not
// set up, and its draws share one pipeline per vertex shader and state, whose vertex outputs NVK drops (linked stages).
bool DrawsWithoutPixelShader(const GuestDevice &device, const Targets &t, const ShaderRecord &ps) {
  if (!t.depth || !ps.entry || !banjo::renut_gpu::DepthOnlyOn())
    return false;
  // banjo-nx (2026-10-20, from sa2-nx): the colour mask first (SA2: every draw writes colour; N&B's shadow draws do not: one word instead of the
  // shader's export constants either way); all the checks only read, so their order does not change the answer.
  const d3d::Device &dev = device.dev();
  const u32 color_mask = u32(dev.m_ValuesPacket.ColorMask) & 0xFFFF;  // MakeKey's; 4 bits per target
  if (t.color_count && (color_mask & ((1u << (4 * t.color_count)) - 1u)))
    return false;
  const ShaderCacheEntry &e = *ps.entry;
  if (e.shaderFlags & (kShaderFlagPixelKill | kShaderFlagPixelDepth))
    return false;
  for (const u32 m : e.memExportConstants) {
    if (m)
      return false;
  }
  const u32 color_control = dev.m_ControlPacket.ColorControl;
  if ((color_control & 8) && (color_control & 7) != 7)  // the alpha test, as sc.alpha_test_func
    return false;
  return ((color_control >> 4) & 1) == 0;  // alpha to coverage, as MakeKey's
}

// banjo-nx (2026-10-18, matching RenderDoc captures of Ryujinx to the game's shaders): banjo_renut_diag_draw_log_s
// seconds after the first draw, every draw of the next two frames: its shaders, counts, target and state, in order.
void DrawLog(const DrawCall &call, const Geometry &geometry, const ShaderRecord *vs, const ShaderRecord *ps,
             const Targets &targets, const PipelineKey &key, const SharedConstants &sc, u32 export_count) {
  static const int32_t at = REXCVAR_GET(banjo_renut_diag_draw_log_s);
  if (at <= 0)
    return;
  static const auto start = std::chrono::steady_clock::now();
  static uint64_t first_frame = 0;
  static uint32_t ordinal = 0;
  static uint64_t ordinal_frame = 0;
  const uint64_t frame = TextureFrameLocked();
  if (!first_frame) {
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds(at))
      return;
    first_frame = frame + 1;  // from the next whole frame
  }
  if (frame < first_frame || frame > first_frame + 1)
    return;
  if (frame != ordinal_frame) {
    ordinal_frame = frame;
    ordinal = 0;
  }
  const HostSurface *c0 = targets.color_count ? targets.colors[0] : nullptr;
  RGPU_INFO("[drawlog] frame {} draw {}: vs {:016X} ps {:016X} prim {} count {} -> {} {} inst {}{} target {}x{} fmt {} "
            "x{} depth {} dc {:08X} blend {:08X} mask {:X} a2c {} vfetch {} bytes, exports {}",
            frame, ordinal++, vs ? vs->hash : 0, ps ? ps->hash : 0, call.primitive, call.count,
            geometry.indexed ? "indices" : "vertices", geometry.indexed ? geometry.index_count : geometry.vertex_count,
            geometry.instance_count, call.tessellation ? " tess" : "", c0 ? c0->width : 0, c0 ? c0->height : 0,
            c0 ? int(c0->format) : -1, targets.color_count, targets.depth ? int(targets.depth->format) : -1,
            key.depth_control, key.blend_control[0], key.color_mask, key.alpha_to_coverage, sc.vertex_fetch_bytes,
            export_count);
}

#if defined(PLUME_DEKO3D)
// ---------------------------------------------------------------------------
// banjo-nx (2026-10-20, banjo_deko3d_skin_once): the fur skinned once a frame
// ---------------------------------------------------------------------------
// The fur family draws each body part once a shell (12 shells and more a frame, shell by shell), and each draw's vertex
// shader skins the same vertices again: 12 bone rows loaded and blended a vertex, about half its work. With the key on
// the listed vertex shaders (tools/skinonce_shaders.txt) have constant 913 (tools/deko3d_glsl_opt.py skinonce): the
// draw's shared word v[84].z (pad_1352[0]) 1 skins and also writes each vertex's three skinned rows to the storage
// buffer at binding 13 (gl_VertexID * 3), 2 reads them there instead. A part's first draw of the frame writes; a later
// draw reads only when everything the skinning reads is the same: the vertex shader, the bound vertex data (GPU
// addresses: the upload ring never hands out a range twice in a frame), the index buffer and draw range, the fetch
// window (the bone data's GPU address) and the vertex-fetch part of SharedConstants (kSkinShared: bone offsets, fetch
// slots, bools). The skinning reads nothing else: the pass refuses shaders whose skinning reads the float constants,
// textures, the instance or another shared word, so draws with other pixel shaders on the same mesh share rows too.
// The rows are stored and loaded as the bits the writer computed, which are the bits the reader would have computed.
// A barrier (vertex work done, caches invalidated) comes before the first read after writes.
// The shared constants the skinning may read (the pass refuses any other word): the booleans (v[32-33]), the fetch
// data, its slots and size (v[35-60]), tessellation and the bound fetches (v[65-81]) and flt_max (v[83].w). The rest
// (the pixel shader's textures, the viewport) may differ between draws that share rows.
constexpr struct {
  u32 offset, size;
} kSkinShared[] = {{512, 32}, {560, 416}, {1040, 272}, {1340, 4}};
constexpr u32 kSkinSharedBytes = 32 + 416 + 272 + 4;
static_assert(offsetof(SharedConstants, booleans) == 512 && offsetof(SharedConstants, vertex_fetch_data) == 560 &&
              offsetof(SharedConstants, memexport_streams) == 976 && offsetof(SharedConstants, tessellation) == 1040 &&
              offsetof(SharedConstants, point_state) == 1312 && offsetof(SharedConstants, flt_max) == 1340);

void SkinSharedKey(const SharedConstants &sc, u8 *out) {
  // Where the fetch copies sit in the upload ring (the data's address, its size, the slots' offsets) is left out: each
  // kick copies the bone data again to another offset. The key compares the copies' guest bytes instead (FurSkinLocked);
  // the slots' endian bits stay.
  SharedConstants k = sc;
  k.vertex_fetch_data = 0;
  k.vertex_fetch_bytes = 0;
  for (u32 &slot : k.vertex_fetch_slots)
    slot &= 3u;
  const u8 *b = reinterpret_cast<const u8 *>(&k);
  for (const auto &r : kSkinShared) {
    std::memcpy(out, b + r.offset, r.size);
    out += r.size;
  }
}

struct SkinEntry {
  uint64_t frame = UINT64_MAX;
  const ShaderRecord *vs = nullptr;
  uint64_t streams = 0, fetch_window = 0, index_address = 0, region = 0;
  u32 index_size = 0, index_format = 0, index_count = 0, vertex_count = 0, vertex_range = 0, first_vertex = 0;
  bool indexed = false;
  u8 shared[kSkinSharedBytes] = {};
  u32 copy_count = 0;
  FetchRecordCopy copies[32];
  std::vector<u8> bytes;  // the copies' guest bytes when the rows were written
};
constexpr u32 kSkinMaxFetchBytes = 64u << 10;
SkinEntry g_skin_entries[64];
std::unique_ptr<plume::RenderBuffer> g_skin_buffer[kFramesInFlight];
uint64_t g_skin_capacity[kFramesInFlight] = {};
uint64_t g_skin_used[kFramesInFlight] = {};
uint64_t g_skin_frame[kFramesInFlight] = {};
bool g_skin_writes_unseen = false;  // rows written since the last barrier

// The draw's skin mode (0 as before, 1 write, 2 read), its region bound at slot 13.
u32 FurSkinLocked(VideoState &s, plume::RenderCommandList *cmd, const ShaderRecord *vs, const Geometry &g,
                  const SharedConstants &sc, uint64_t fetch_window) {
  const uint64_t frame = TextureFrameLocked();
  const u32 slot = s.frame;
  const u32 other = IsFurVs(vs->hash) ? 0u : 4u;  // the report's counts: the fur, the other skinned draws
  const uint64_t streams_hash = BoundStreamsHash();  // banjo-nx (2026-10-23): made here, not at each bind
  if (g_skin_frame[slot] != frame + 1) {  // + 1: the zero-initialised slots start unused
    g_skin_frame[slot] = frame + 1;
    g_skin_used[slot] = 0;
  }
  const uint64_t index_address =
      g.indexed && g.index_view.buffer.ref ? g.index_view.buffer.ref->getDeviceAddress() + g.index_view.buffer.offset : 0;
  const u32 index_size = g.indexed ? g.index_view.size : 0;
  const u32 index_format = g.indexed ? u32(g.index_view.format) : 0;
  const u32 vertices = g.indexed ? g.vertex_range : g.vertex_count;
  if (!vertices || (g.indexed && !index_address)) {
    banjo::renut_gpu::CountSkinOnce(other + 2);
    return 0;
  }
  u8 shared[kSkinSharedBytes];
  SkinSharedKey(sc, shared);
  const FetchRecord &fr = g_fetch_record;
  u32 fetch_bytes = 0;
  for (u32 i = 0; i < fr.count; ++i)
    fetch_bytes += fr.copies[i].src ? fr.copies[i].size : 0;
  if (fr.unusable || fetch_bytes > kSkinMaxFetchBytes) {
    banjo::renut_gpu::CountSkinOnce(other + 2);
    return 0;
  }
  // The copies match an entry's: the same sources, sizes and endian modes, and the same bytes there now as when the
  // entry's rows were written.
  auto same_fetch = [&](const SkinEntry &e) {
    if (e.copy_count != fr.count)
      return false;
    size_t at = 0;
    for (u32 i = 0; i < fr.count; ++i) {
      if (!(e.copies[i] == fr.copies[i]))
        return false;
      if (fr.copies[i].src && fr.copies[i].size) {
        if (at + fr.copies[i].size > e.bytes.size() ||
            std::memcmp(e.bytes.data() + at, fr.copies[i].src, fr.copies[i].size) != 0)
          return false;
        at += fr.copies[i].size;
      }
    }
    return true;
  };
  SkinEntry *hit = nullptr, *free_entry = nullptr;
  for (SkinEntry &e : g_skin_entries) {
    if (e.frame != frame) {
      if (!free_entry)
        free_entry = &e;
      continue;
    }
    if (e.vs == vs && e.streams == streams_hash && e.index_address == index_address && e.index_size == index_size && e.index_format == index_format &&
        e.index_count == g.index_count && e.vertex_count == g.vertex_count && e.vertex_range == g.vertex_range &&
        e.first_vertex == g.first_vertex && e.indexed == g.indexed &&
        std::memcmp(e.shared, shared, sizeof(shared)) == 0 && same_fetch(e)) {
      hit = &e;
      break;
    }
  }
  const u32 bytes = vertices * 48u;
  if (hit) {
    if (g_skin_writes_unseen) {
      const plume::RenderBufferBarrier barrier(g_skin_buffer[slot].get(), plume::RenderBufferAccess::READ);
      cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &barrier, 1);
      g_skin_writes_unseen = false;
    }
    plume::Deko3DBindStorage(cmd, 13, hit->region, bytes);
    banjo::renut_gpu::CountSkinOnce(other + 1);
    return 2;
  }
  // Diagnostic: a miss where the frame has an entry of the same shader and draw size (the same part, it would seem):
  // which of the key's fields differed (the "[renut] skin once" line's near misses).
  for (const SkinEntry &e : g_skin_entries) {
    if (e.frame != frame || e.vs != vs || e.index_count != g.index_count || e.vertex_count != g.vertex_count ||
        e.vertex_range != g.vertex_range || e.indexed != g.indexed)
      continue;
    u32 fields = 0;
    fields |= e.streams != streams_hash ? 1u : 0u;
    fields |= !same_fetch(e) ? 2u : 0u;
    fields |= e.index_address != index_address || e.index_size != index_size || e.index_format != index_format ? 4u : 0u;
    fields |= e.first_vertex != g.first_vertex ? 8u : 0u;
    u32 first_shared = UINT32_MAX;
    for (u32 i = 0; i < kSkinSharedBytes; ++i) {
      if (e.shared[i] != shared[i]) {
        first_shared = i;
        break;
      }
    }
    fields |= first_shared != UINT32_MAX ? 16u : 0u;
    banjo::renut_gpu::CountSkinMiss(fields);
    static uint32_t logged = 0;
    if ((fields & 16u) && logged++ < 8) {
      // the key byte back to its SharedConstants offset
      u32 at = first_shared, offset = 0;
      for (const auto &r : kSkinShared) {
        if (at < r.size) {
          offset = r.offset + at;
          break;
        }
        at -= r.size;
      }
      RGPU_INFO("[skin] vs {:016X}: a part's shared constants differ from its first draw's at byte {} (other fields "
                "{:X})", vs->hash, offset, fields & 15u);
    }
    break;
  }
  if (!free_entry) {
    banjo::renut_gpu::CountSkinOnce(other + 2);
    return 0;
  }
  const uint64_t need = (uint64_t(bytes) + 255u) & ~uint64_t(255);
  if (!g_skin_buffer[slot] || g_skin_used[slot] + need > g_skin_capacity[slot]) {
    // A bigger buffer: the regions written before stay in the old one, retired with the slot.
    if (!EnsureBuffer(g_skin_buffer[slot], g_skin_capacity[slot], g_skin_used[slot] + need,
                      plume::RenderBufferDesc::DefaultBuffer(0, StorageBufferFlags() | AddressableBufferFlags()),
                      "renut skin cache")) {
      banjo::renut_gpu::CountSkinOnce(other + 3);
      return 0;
    }
    g_skin_used[slot] = 0;
  }
  const uint64_t region = g_skin_buffer[slot]->getDeviceAddress() + g_skin_used[slot];
  g_skin_used[slot] += need;
  SkinEntry &e = *free_entry;
  e.frame = frame;
  e.vs = vs;
  e.streams = streams_hash;
  e.fetch_window = fetch_window;
  e.index_address = index_address;
  e.index_size = index_size;
  e.index_format = index_format;
  e.index_count = g.index_count;
  e.vertex_count = g.vertex_count;
  e.vertex_range = g.vertex_range;
  e.first_vertex = g.first_vertex;
  e.indexed = g.indexed;
  e.region = region;
  std::memcpy(e.shared, shared, sizeof(shared));
  e.copy_count = fr.count;
  std::copy(fr.copies, fr.copies + fr.count, e.copies);
  e.bytes.clear();
  for (u32 i = 0; i < fr.count; ++i)
    if (fr.copies[i].src && fr.copies[i].size)
      e.bytes.insert(e.bytes.end(), fr.copies[i].src, fr.copies[i].src + fr.copies[i].size);
  plume::Deko3DBindStorage(cmd, 13, region, bytes);
  g_skin_writes_unseen = true;
  banjo::renut_gpu::CountSkinOnce(other + 0);
  return 1;
}
#endif  // PLUME_DEKO3D

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): the previous chain draw's targets and viewport (DrawStateLocked).
struct TargetsReuse {
  uint64_t seq = 0, epoch = 0;
  u32 va[d3d::kMaxRenderTargets + 1] = {};       // the device's render target words and its depth word
  // each one's Size, SurfaceInfo, Format and ColorInfo (GetHostSurfaceLocked's; port 161: ColorInfo, the EDRAM base)
  u32 desc[d3d::kMaxRenderTargets + 1][4] = {};
  Targets targets;
};
TargetsReuse g_targets_reuse;
struct ViewportReuse {
  uint64_t seq = 0;

  u32 width = 0, height = 0, guest_width = 0, guest_height = 0, scale = 0, predication = 0;
  size_t tiles = 0;
  ViewportState viewport;
};
ViewportReuse g_viewport_reuse;

// The descriptor words GetHostSurfaceLocked reads for a surface word of the device (0 0 0 0 for none). Port 161 (from
// fnxf_xiii, 2026-11-11): ColorInfo too, as the surface memo: a target pointed at another EDRAM base is another target.
void SurfaceDescWords(u32 va, u32 out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0;
  if (!va)
    return;
  if (const auto *desc = GuestPtr<const d3d::SurfaceDesc>(va + sizeof(d3d::Resource))) {
    out[0] = desc->Size;
    out[1] = desc->SurfaceInfo;
    out[2] = desc->Format;
    out[3] = desc->ColorInfo;
  }
}

// banjo-nx (2026-10-23, batch 6b, banjo_renut_diag_repeat_census): frames 0, 1 and 2 of every 32
// (TextureFrameLocked), draw by draw; frames 1 and 2 compare with the frame before (N-1) and frame 2 also with the one
// before that (N-2: a game that alternates two buffers or targets repeats every other frame). For each draw: its state
// key (a hash of the device bytes a draw reads: the fetch constants and the tail but for the range no draw reads,
// 10880-11775, with its call), its constants key (its shaders' float constants, the groups they read), whether its
// guest data (streams, indices, fetch copies; not Draw*UP data) is unwritten since the compared frame began (the store
// cards), and its full key (state and constants) found among that frame's draws on the same target. Shadow maps (a
// depth target alone) are counted apart: a map all of whose draws repeat completely, as many as before, would come out
// the same (depth alone does not depend on the draw order).
struct CensusCounts {
  std::atomic<uint64_t> draws{0}, state{0}, constants{0}, data{0}, full{0}, maps{0}, maps_same{0};
};
struct RepeatCensusTotals {
  std::atomic<uint64_t> pairs{0};
  CensusCounts c[2][2];  // [shadow?][N-1, N-2]
};
RepeatCensusTotals g_census_totals;

struct CensusTarget {
  bool depth_only = false;
  u32 draws = 0;
  std::unordered_map<uint64_t, u32> fulls;  // full key -> draws
  u32 matched[2] = {};                      // draws matched against N-1, N-2
};
struct CensusFrame {
  uint64_t frame = UINT64_MAX;
  uint64_t serial_begin = 0;
  std::unordered_set<uint64_t> states, constants;
  std::unordered_map<const void *, CensusTarget> targets;
};
CensusFrame g_census[3];  // [0] this frame, [1] N-1, [2] N-2 (valid when frame numbers follow)
// Per compared frame and target: its full keys not matched yet (consumed as this frame's draws match them).
std::unordered_map<const void *, std::unordered_map<uint64_t, u32>> g_census_left[2];

constexpr u32 kCensusFetchBegin = u32(offsetof(d3d::Device, m_Constants));
constexpr u32 kCensusFloatBegin = kCensusFetchBegin + u32(offsetof(d3d::Constants, VertexShaderF));
constexpr u32 kCensusTailBegin = kCensusFetchBegin + u32(offsetof(d3d::Constants, VertexShaderB));
static_assert(kCensusTailBegin < 10880 && 11776 < d3d::kDeviceSnapshotSize);  // katamari-nx: a chunk boundary (backend/queued.cpp)

bool CensusCompared(u32 k) {  // g_census[k + 1] is frame N-(k+1)
  return g_census[0].frame != UINT64_MAX && g_census[k + 1].frame != UINT64_MAX &&
         g_census[k + 1].frame + k + 1 == g_census[0].frame;
}

void CensusFinishFrame() {
  if (g_census[0].frame == UINT64_MAX)
    return;
  bool any = false;
  for (u32 k = 0; k < 2; ++k) {
    if (!CensusCompared(k))
      continue;
    any = true;
    for (const auto &[target, t] : g_census[0].targets) {
      CensusCounts &c = g_census_totals.c[t.depth_only ? 1 : 0][k];
      c.maps.fetch_add(1, std::memory_order_relaxed);
      const auto before = g_census[k + 1].targets.find(target);
      if (before != g_census[k + 1].targets.end() && t.matched[k] == t.draws && before->second.draws == t.draws)
        c.maps_same.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (any)
    g_census_totals.pairs.fetch_add(1, std::memory_order_relaxed);
  g_census[2] = std::move(g_census[1]);
  g_census[1] = std::move(g_census[0]);
  g_census[0] = CensusFrame{};
  g_census_left[0].clear();
  g_census_left[1].clear();
}

void UnleashedCensusDraw(const GuestDevice &device, const DrawCall &call, u32 texture_mask, u32 stream_mask,
                         const StreamKey *streams, u32 stream_count, const IndexKey *index);  // banjo-nx (2026-10-26)
void CensusDraw(const u8 *device_state, const DrawCall &call, const Targets &targets, const ShaderRecord *vs,
                const ShaderRecord *ps, const FloatConstants &vs_constants, const FloatConstants &ps_constants,
                const StreamKey *streams, u32 stream_count, const IndexKey *index) {
  static const bool on = REXCVAR_GET(banjo_renut_diag_repeat_census);
  if (!on)
    return;
  const uint64_t frame = TextureFrameLocked();
  if (frame % 32 > 2) {
    if (g_census[0].frame != UINT64_MAX)
      CensusFinishFrame();
    return;
  }
  if (g_census[0].frame != frame) {
    CensusFinishFrame();
    g_census[0].frame = frame;
    g_census[0].serial_begin = g_flush_serial;
  }
  CensusFrame &cur = g_census[0];
  // The state key.
  uint64_t state = XXH3_64bits(device_state + kCensusFetchBegin, kCensusFloatBegin - kCensusFetchBegin);
  state = XXH3_64bits_withSeed(device_state + kCensusTailBegin, 10880 - kCensusTailBegin, state);
  state = XXH3_64bits_withSeed(device_state + 11776, d3d::kDeviceSnapshotSize - 11776, state);  // katamari-nx: a chunk boundary (backend/queued.cpp)
  const u32 call_words[6] = {call.primitive, call.start_index, call.start_vertex, call.count, u32(call.base_vertex),
                             u32(call.indexed) | u32(call.tessellation) << 1 | u32(call.up_vertices != nullptr) << 2 |
                                 u32(call.up_indices != nullptr) << 3};
  state = XXH3_64bits_withSeed(call_words, sizeof(call_words), state);
  // The constants key.
  uint64_t constants = 0x5BD1E995u;
  const ShaderRecord *records[2] = {vs, ps};
  const FloatConstants *stage_constants[2] = {&vs_constants, &ps_constants};
  for (u32 stage = 0; stage < 2; ++stage) {
    if (!records[stage])
      continue;
    for (uint64_t groups = DrawSummaryOf(*records[stage]).groups; groups; groups &= groups - 1) {
      const u32 group = 63 - u32(std::countr_zero(groups));
      constants = XXH3_64bits_withSeed(stage_constants[stage]->Register(group * 4), 64, constants);
    }
  }
  const uint64_t full = (state ^ (constants * 0x9E3779B97F4A7C15ull)) | 1;
  cur.states.insert(state);
  cur.constants.insert(constants);
  const bool depth_only = targets.color_count == 0 && targets.depth;
  const void *target = depth_only ? static_cast<const void *>(targets.depth)
                                  : static_cast<const void *>(targets.color_count ? targets.colors[0] : nullptr);
  CensusTarget &t = cur.targets[target];
  t.depth_only = depth_only;
  ++t.draws;
  ++t.fulls[full];
  for (u32 k = 0; k < 2; ++k) {
    if (!CensusCompared(k))
      continue;
    const CensusFrame &before = g_census[k + 1];
    CensusCounts &c = g_census_totals.c[depth_only ? 1 : 0][k];
    c.draws.fetch_add(1, std::memory_order_relaxed);
    const bool same_state = before.states.count(state) != 0;
    const bool same_constants = before.constants.count(constants) != 0;
    // The guest data: unwritten since the compared frame began.
    bool clean = !call.up_vertices && !call.up_indices && !g_fetch_record.unusable;
    for (u32 i = 0; clean && i < stream_count; ++i)
      clean = PagesClean(streams[i].guest, size_t(streams[i].count) * streams[i].stride, before.serial_begin);
    if (clean && index)
      clean = PagesClean(index->src, size_t(index->count) * (index->is32 ? 4 : 2), before.serial_begin);
    for (u32 i = 0; clean && i < g_fetch_record.count; ++i)
      if (g_fetch_record.copies[i].src && g_fetch_record.copies[i].size)
        clean = PagesClean(g_fetch_record.copies[i].src, g_fetch_record.copies[i].size, before.serial_begin);
    if (same_state)
      c.state.fetch_add(1, std::memory_order_relaxed);
    if (same_constants)
      c.constants.fetch_add(1, std::memory_order_relaxed);
    if (clean)
      c.data.fetch_add(1, std::memory_order_relaxed);
    if (!clean || !same_state || !same_constants)
      continue;
    auto left_it = g_census_left[k].find(target);
    if (left_it == g_census_left[k].end()) {
      auto &left = g_census_left[k][target];
      if (const auto tb = before.targets.find(target); tb != before.targets.end())
        left = tb->second.fulls;
      left_it = g_census_left[k].find(target);
    }
    if (auto f = left_it->second.find(full); f != left_it->second.end() && f->second) {
      --f->second;
      ++t.matched[k];
      c.full.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

bool SameTargets(const Targets &a, const Targets &b) {
  if (a.color_count != b.color_count || a.depth != b.depth || a.width != b.width || a.height != b.height ||
      a.scale.code != b.scale.code || a.guest_width != b.guest_width || a.guest_height != b.guest_height)
    return false;
  for (u32 i = 0; i < a.color_count; ++i)
    if (a.colors[i] != b.colors[i])
      return false;
  return true;
}

// banjo-nx (2026-10-26, the record-time draw path, stage 1: docs/record-time-draw-path.md): the lean executor.
// DrawStateLocked derives a draw from the whole rebuilt device, about forty steps whatever changed; FastDrawLocked
// starts from what the draw before left (g_fast, stored by DrawStateLocked or by itself) and redoes only what the
// recording's field bits (ChainHint::fields, from the setters' marks) and the render thread's epochs say changed:
// shaders, textures, bools and alpha, viewport, the depth-only answer, the pipeline key and pipeline. Per draw it does
// what depends on the call: the geometry (whole buffers only), the bound fetches and fetch data, the float constants
// and the pushed shared constants, the binds and the draw. Whatever it does not handle goes to DrawStateLocked before
// anything is recorded (returns 0): new targets or declaration, the predication, a chain break, Draw*UP and
// Begin*Vertices, points, tessellation, memexport (its own or a read of a pending window), the fur's skinning and
// shells, the composite, range geometry, the diagnostics. banjo_renut_fast_draw_verify derives each kept part again
// and logs a difference (and uses the derived value).
namespace {
// banjo-nx (2026-10-28, total CPU): what a fast draw reads of its pipeline key after the key is chosen (the point
// sprite push, the targets' write counts), kept beside the choice: those fields are on the key's third and fourth lines
// (a miss each at most pipeline changes, the c4 profile's hottest lines of FastDrawLocked with the entry's own check).
struct KeyHot {
  u32 point_sprite = 0, color_count = 0, color_mask = 0, depth_control = 0, stencil_ref_mask = 0;
  static KeyHot Of(const PipelineKey &k) {
    return {k.point_sprite, k.color_count, k.color_mask, k.depth_control, k.stencil_ref_mask};
  }
};
struct FastState {
  bool valid = false;
  uint64_t seq = 0;
  ShaderRecord *vs = nullptr, *ps = nullptr;  // ps: the device's, before the depth-only answer
  plume::RenderShader *host_vs = nullptr, *host_ps = nullptr;
  const VertexDeclarationRecord *declaration = nullptr;
  const u32 *shader_strides = nullptr;
  bool declaration_from_va = false;  // the device's declaration (not the shader's own), kept while kFieldDecl is not set
  Targets targets;
  uint64_t surface_epoch = 0;
  uint64_t sasrt_write_seq = 0;  // sasrt-nx (2026-10-29): g_surface_write_seq after the last draw (the EDRAM depth restore)
  ViewportState viewport;
  size_t viewport_tiles = 0;
  u32 viewport_predication = 0;
  bool depth_only = false;
  u32 alpha_test_func = 7;
  bool textures_valid = false;
  u32 texture_mask = 0;
  uint64_t texture_frame = 0, texture_epoch = 0, depth_epoch = 0;
  u32 texture_va[32] = {};
  u32 texture_header[32][6] = {};
  const TextureObject *texture_obj[32] = {};  // banjo-nx (2026-10-27, Phase A): the slots' objects at the last fill
  BindEffects fx[32];
  PipelineKey key{};
  // banjo-nx (2026-10-27, render thread night): the draw's key where it is (a pipeline cache entry's, or 'key' for one
  // made here: the full path's, a verify's): not copied out of the cache at every key change (about 200 bytes), and
  // whether its blend controls read the blend constant (DrawBlendConstant's loop only when they do).
  const PipelineKey *kp = nullptr;
  KeyHot kp_hot;  // banjo-nx (2026-10-28): KeyHot::Of(*kp)
  bool blend_used = true;
  uint64_t key_rs_epoch = 0;  // the render-state epoch and the rest (FastDrawLocked's misc) the key was made for
  u32 key_misc = ~0u;
  plume::RenderPipeline *pipeline = nullptr;
  uint64_t pipeline_lookups = 0, prebuild_epoch = 0;
  uint64_t export_generation = 0;
  bool export_none = false;
  uint32_t profile_label = UINT32_MAX;
  bool profile_label_shaders = false;  // the label depends on the pixel shader (banjo_renut_diag_gpu_shaders)
  // Round 4: the layout epoch the bound slots' transitions were last applied at, and their counters' shares; the
  // framebuffer this state bound and the epochs it was bound at (BindTargets skipped while they hold).
  uint64_t layout_epoch = 0;
  u32 fx_float_sampled = 0, fx_words_viewed = 0;
  const plume::RenderFramebuffer *bound_framebuffer = nullptr;
  uint64_t bind_layout_epoch = 0, framebuffer_epoch = 0;
  // Round 4: the pending exports' answer per kind of read (the draw read none of their windows), kept by
  // export_generation while the kind's inputs do not change.
  bool export_streams_ok = false, export_fetch_ok = false, export_index_ok = false, export_textures_ok = false;
  // Round 4 (stages 1-2's ARM pass): whether the kept shaders are ones the fast path draws (no exports, composite,
  // skin-once or fur), checked when they change; and the texture mask of the kept shaders and depth-only answer.
  bool shaders_fast = false;
  u32 draw_texture_mask = 0;
  // The words MakeKey reads of the device's render state and bools (FastRenderState), and the pipeline cache's tag for
  // them with the targets' part of the key: banjo-nx (2026-10-27, setter rewrite) the number RenderStateIdLocked gives
  // that content (the same words and targets, the same number), so a state the draws come back to finds its pipelines
  // again; a number raised at every change before (A/B 28): the city's draws go back and forth between a few states,
  // and 32 % of the fast draws made their key again (about 1,300 MakeKey and pipeline map lookups a frame).
  bool rs_valid = false;
  u32 rs[20] = {};
  uint64_t rs_epoch = 1;
  // banjo-nx (2026-10-27, render thread night, A/B 36): the fetch data the draw before bound (UploadVertexFetchData's
  // copies in the upload chunk, its slots and address in the shared constants), kept for a draw with the same vertex
  // shader, fetch constants and fetch slots in the same chunk, kick and command list: the same words, nothing to do.
  bool fetch_kept = false;
  u32 fetch_kept_mask = 0;
  const void *fetch_kept_chunk = nullptr;
  uint64_t fetch_kept_flush = 0, fetch_kept_resets = 0, fetch_kept_list = 0, fetch_kept_export = 0;
  // banjo-nx (2026-10-27, banjo_renut_fetch_cache): the kept data was the fetch cache's pool (proven for the kick: kept
  // for the same kick whatever the upload chunk is now).
  bool fetch_kept_pool = false;
};

// banjo-nx (2026-10-27): the render-state numbers by content (FastState::rs_epoch). Never two contents for one number:
// a number is given once (g_rs_next_id) and an entry another content takes is a new number for it when it comes back.
struct RenderStateIdEntry {
  u32 rs[20];
  u32 targets[8];  // MakeKey's inputs from the targets: count, formats, depth format, samples, depth words
  uint64_t id;
};
RenderStateIdEntry g_rs_ids[256];
uint64_t g_rs_next_id = 1;
uint64_t g_rs_id_hits = 0, g_rs_id_new = 0;  // the 10 s line's
uint64_t RenderStateIdLocked(const u32 rs[20], const Targets &t) {
  u32 sig[8] = {};
  sig[0] = t.color_count | (t.depth || t.depth_dropped ? 0x100u : 0u);
  for (u32 i = 0; i < t.color_count && i < 4; ++i)
    sig[1 + i] = u32(t.colors[i]->format);
  sig[5] = t.depth ? u32(t.depth->format) : u32(plume::RenderFormat::UNKNOWN);
  const HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  sig[6] = first ? u32(first->samples) : 0u;
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (u32 i = 0; i < 20; ++i)
    h = (h ^ rs[i]) * 0xFF51AFD7ED558CCDull;
  for (u32 i = 0; i < 7; ++i)
    h = (h ^ sig[i]) * 0xC4CEB9FE1A85EC53ull;
  RenderStateIdEntry &e = g_rs_ids[u32(h >> 56)];
  if (e.id && SameBytes<sizeof(e.rs)>(e.rs, rs) && SameBytes<sizeof(sig)>(e.targets, sig)) {
    ++g_rs_id_hits;
    return e.id;
  }
  ++g_rs_id_new;
  std::memcpy(e.rs, rs, sizeof(e.rs));
  std::memcpy(e.targets, sig, sizeof(sig));
  e.id = ++g_rs_next_id;
  return e.id;
}
FastState g_fast;

// banjo-nx (2026-10-28, host packets, banjo_renut_host_packets; the user's mandate): a replayed draw's derived state kept
// by its packet key (ChainHint::packet_key: the device words these answers read, guest/command_buffers.cpp
// kPacketWords, made by the recording threads as they record, with the call's primitive kind), so a later draw with the
// same words takes its answers instead of deriving them again:
// the shaders' records, host shaders and cache entries, the declaration and strides (no shader or declaration lookup and
// no record line read for them), the targets, the depth-only answer and texture mask, the bools and alpha test, the
// viewport, and the render-state words and their number. Each is derived from those words alone (the executor's own
// dependency sets, which its verify checks); what it reads besides is checked: the records and host shaders (g_packet_epoch, raised
// whenever a record or a host shader goes), the surfaces (the targets and what reads them), the tiles and the replay's
// predication (the viewport). banjo_renut_fast_draw_verify compares the outcome with the whole way's, as for every fast
// draw. A/B 65's off half: no packet taken (they are still made).
uint64_t g_packet_epoch = 1;
uint64_t g_packet_hits = 0, g_packet_misses = 0, g_packet_stale = 0, g_packet_made = 0;  // the 10 s line's
struct alignas(64) HostPacket {
  // Line 0: the check and the shaders.
  uint64_t key = 0, epoch = 0;
  ShaderRecord *vs = nullptr, *ps = nullptr;  // ps: the device's (before the depth-only answer)
  plume::RenderShader *host_vs = nullptr, *host_ps = nullptr;
  const VertexDeclarationRecord *declaration = nullptr;
  const u32 *shader_strides = nullptr;
  // Line 1: the answers' scalars.
  const ShaderCacheEntry *vs_entry = nullptr, *ps_entry = nullptr;
  uint64_t surface_epoch = 0, rs_epoch = 0;
  u32 viewport_tiles = 0, viewport_predication = 0;
  u32 texture_mask = 0, alpha_test_func = 7;
  float alpha_threshold = 0.0f;
  bool declaration_from_va = false, depth_only = false;
  // Then the rest.
  u32 booleans[8] = {};
  u32 rs[20] = {};
  Targets targets;
  ViewportState viewport;
};
constexpr u32 kHostPacketBits = 13;  // 8,192 packets, direct-mapped (a city frame draws ~4,800 states)
HostPacket *g_host_packets = nullptr;
inline bool HostPacketsOn() {
  static const bool on = REXCVAR_GET(banjo_renut_host_packets);
  return on;
}
inline u32 HostPacketIndex(uint64_t key) { return u32((key * 0x9E3779B97F4A7C15ull) >> (64 - kHostPacketBits)); }
// The slot of a key (the table made at first use).
inline HostPacket &HostPacketSlot(uint64_t key) {
  if (!g_host_packets) [[unlikely]]
    g_host_packets = new HostPacket[size_t(1) << kHostPacketBits];
  return g_host_packets[HostPacketIndex(key)];
}

// banjo-nx (2026-10-26, the record-time draw path): MakeKey's device inputs besides the shaders, the declaration's
// layout, the targets, the primitive, the instancing and the alpha test: the four blend controls, the colour mask, the
// depth control, both stencil reference/mask words, the mode control, the colour control's alpha-to-coverage bit, the
// bools (sc.booleans, the same words the key's bool specialisation reads).
inline void FastRenderState(const d3d::Device &dev, const u32 *booleans, u32 out[20]) {
  const d3d::ControlPacket &cp = dev.m_ControlPacket;
  out[0] = cp.BlendControl0;
  out[1] = cp.BlendControl1;
  out[2] = cp.BlendControl2;
  out[3] = cp.BlendControl3;
  out[4] = u32(dev.m_ValuesPacket.ColorMask);
  out[5] = u32(cp.DepthControl);
  out[6] = u32(dev.m_ValuesPacket.StencilRefMask);
  out[7] = u32(dev.m_ValuesPacket.StencilRefMaskBF);
  out[8] = u32(cp.ModeControl);
  out[9] = u32(cp.ColorControl);
  std::memcpy(out + 10, booleans, 8 * sizeof(u32));
  out[18] = out[19] = 0;
}

// The pipeline cache: (render-state epoch, shader entries, layout, primitive, instancing, alpha test, A/B half) to the
// key MakeKey made and the pipeline GetPipelineLocked gave, while no prebuilt pipeline came in since.
// banjo-nx (2026-10-28, total CPU): the pipeline first, with the words its check compares (line 0; it was after the key,
// on line 3, which the entry's prefetch did not take), then the key's draw-time fields (line 1, KeyHot), then the key.
struct FastPipeline {
  plume::RenderPipeline *pipeline = nullptr;
  uint64_t rs_epoch = 0, layout_hash = 0, prebuild_epoch = 0, generation = 0;
  const ShaderCacheEntry *vs = nullptr, *ps = nullptr;
  u32 misc = 0;
  bool blend_used = true;  // banjo-nx (2026-10-27): BlendConstantUsed(key)
  bool provisional = true;  // banjo-nx (2026-10-28): the answer holds only for its prebuild epoch (LastPipelineProvisional)
  uint8_t stamp = 0;  // blur-nx (2026-11-18, #16): g_fast_pipeline_stamp at its last use (the older way is replaced)
  KeyHot hot;
  PipelineKey key{};
};
static_assert(offsetof(FastPipeline, hot) == 64, "FastPipeline: line 0 the check, line 1 the key's draw-time fields");
// banjo-nx (2026-10-27): 1,024 entries (512 before): with numbers by content the frame's pipelines are found again
// from one frame to the next, and they are more than 512.
// banjo-nx (2026-10-28, per-draw night): 4,096 (1,024 kept evicting a city frame's ~3,200 pipelines: 7.5 % of the fast
// draws made their key again in d4; 1.3 MB, kept at that for the heap's sake).
constexpr u32 kFastPipelines = 4096;
FastPipeline g_fast_pipelines[kFastPipelines];
inline u32 FastPipelineIndex(uint64_t rs_epoch, const ShaderCacheEntry *vs, const ShaderCacheEntry *ps,
                             uint64_t layout_hash, u32 misc) {
  uint64_t h = rs_epoch * 0x9E3779B97F4A7C15ull;
  h ^= (uint64_t(reinterpret_cast<uintptr_t>(vs)) >> 4) * 0xFF51AFD7ED558CCDull;
  h ^= (uint64_t(reinterpret_cast<uintptr_t>(ps)) >> 4) * 0xC4CEB9FE1A85EC53ull;
  h ^= layout_hash + misc;
  return u32(h ^ (h >> 29)) & (kFastPipelines - 1);
}
// banjo-nx (2026-10-27, banjo_renut_diag_draw_kinds): the last draw's kind, fields and path, set by FastDrawLocked and
// StoreFastStateLocked for NoteDrawKind.
struct DrawKindStats {
  uint64_t draws = 0, ticks = 0, fast = 0, runs = 0, chain_breaks = 0, nothing = 0;
  uint64_t fields[18] = {};
};
DrawKindStats g_kind_stats[3];
uint64_t g_merge_same[3] = {}, g_merge_contiguous[3] = {};  // banjo-nx (2026-10-27): NoteDrawMerge's, by kind
u32 g_last_draw_kind = 2, g_last_draw_fields = 0, g_prev_draw_kind = 3;
bool g_last_draw_fast = false;
inline u32 DrawKindOf(const Targets &t, const ShaderRecord *ps) {
  return ps ? 2u : (t.color_count == 0 && t.depth ? 0u : 1u);
}
uint64_t g_fast_pipeline_generation = 1;  // raised by InvalidateFastDrawLocked
uint8_t g_fast_pipeline_stamp = 0;  // blur-nx (2026-11-18, #16): one more at each use of an entry
uint64_t g_fast_record_drops = 0;  // banjo-nx (2026-10-28): InvalidateFastDrawForRecordLocked's calls
uint64_t g_fast_pipeline_hits = 0, g_fast_pipeline_misses = 0;  // the 10 s line's
// banjo-nx (2026-10-28, per-draw night): the misses by what the slot held: [0] nothing, [1] another pipeline's answer
// (a conflict: the table is direct-mapped), [2] this one's from a past prebuild epoch (provisional), [3] this one's
// from before an invalidation (generation).
uint64_t g_fast_pipeline_miss_why[4] = {};
uint64_t g_fast_draws = 0, g_full_draws = 0, g_fast_why[16] = {};  // the 10 s line's

// The reasons a draw went to DrawStateLocked, by the first met (the 10 s line).
enum FastWhy : u32 {
  kFastNoState, kFastBreak, kFastCall, kFastFields, kFastSurfaces, kFastDiag, kFastShaders, kFastDecl, kFastExports,
  kFastSpecial, kFastGeometry, kFastExportRead, kFastPushed, kFastDropDepth, kFastWhyCount
};
inline int FastMiss(u32 why) {
  ++g_fast_why[why];
  return 0;
}

}  // namespace

// banjo-nx (2026-10-26, the record-time draw path's round 3: the next draw's lookups ahead): the texture slots the
// last draw bound (most next draws bind the same ones), and a slot's memo entry fetched for the given texture and
// fetch constant (render thread).
u32 FastTextureMaskGuess() { return g_fast.valid ? g_fast.texture_mask : 0u; }
void PrefetchTextureMemoLocked(u32 va, const d3d::TextureFetchConstant &fetch) {
  const TextureSlotMemo &m = TextureMemoSlot(va, TextureFetch::FromGuest(fetch));
  __builtin_prefetch(&m);
  __builtin_prefetch(reinterpret_cast<const char *>(&m) + 64);
}

// banjo-nx (2026-10-28, host packets): draw.h.
void PrefetchHostPacket(uint64_t key) {
  if (!key || !g_host_packets || !HostPacketsOn())
    return;
  const char *p = reinterpret_cast<const char *>(&g_host_packets[HostPacketIndex(key)]);
  __builtin_prefetch(p);
  __builtin_prefetch(p + 64);
}
void InvalidateHostPacketsLocked() { ++g_packet_epoch; }

// banjo-nx (2026-10-28, banjo_renut_lazy_resolve_mips; from sa2-nx's sa2_renut_lazy_resolve_mips, credit sa2-nx): a scaled
// colour resolve destination's host levels (docked: 3D at 1.25x, the HUD at 1.5x) were made after every resolve into it
// (UpdateResolveMipsLocked: a scratch draw a level and copies, barriers around them), sampled or not. Now when a draw
// first samples it, or before its level 0 is written another way. A/B 66's off half: after every resolve, as before.
uint64_t g_lazy_mips_deferred = 0, g_lazy_mips_made = 0;  // the 10 s line's
bool LazyResolveMipsOn() {
  static const bool on = REXCVAR_GET(banjo_renut_lazy_resolve_mips);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 66;
}
// banjo-nx (2026-10-28, banjo_renut_clear_repeat; from sa2-nx's sa2_renut_clear_repeat, credit sa2-nx): ClearTargets.
// A/B 67's off half sends every clear (what the targets hold is still followed).
uint64_t g_clear_repeats = 0, g_clears_whole = 0;  // the 10 s line's
bool ClearRepeatOn() {
  static const bool on = REXCVAR_GET(banjo_renut_clear_repeat);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 67;
}
// The levels the resolves since the last ones left, from level 0 as it is now: the same as at those resolves (only
// resolves, each widening the region, wrote level 0 since; anything else that writes it flushes first). Outside the
// resolves' rectangles level 0 is unchanged since the levels there were made, so those come out as they were.
void FlushResolveMipsLocked(HostTexture &t) {
  if (!t.mips_pending)
    return;
  t.mips_pending = false;
  ++g_lazy_mips_made;
  UpdateResolveMipsLocked(&t, t.mips_rect);
  // The draw path binds its own targets, layout, pipeline and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
}

// sasrt-nx (2026-10-20, sasrt_renut_deko3d_clear_new; surfaces.cpp): a new target was cleared with its own framebuffer.
void ForgetBoundTargetsLocked() {
  g_bound.framebuffer = nullptr;
  g_bound.viewport_set = false;
}

void InvalidateFastDrawLocked() {
  g_fast.valid = false;
  ++g_fast_pipeline_generation;
  ++g_packet_epoch;  // banjo-nx (2026-10-28, host packets)
  for (LastStreamSource &l : g_last_stream_sources)  // banjo-nx (2026-10-27): they name the shader record
    l.valid = false;
}
// banjo-nx (2026-10-28, per-draw night, render/draw.h). The fast state names records by its shader pointers, its
// declaration (a vertex shader's bound or fetch declaration is inside that shader's record's ownership) and its
// strides (inside the vertex shader's record); the pipeline cache's entries name cache entries and pipelines only.
// Every record a frame destroyed cleared both (N&B makes and drops shader objects with its meshes): the next draws took
// the full path and made their pipeline keys again.
void InvalidateFastDrawForRecordLocked(const void *record) {
  if (g_fast.vs == record || g_fast.ps == record || g_fast.declaration == record)
    g_fast.valid = false;
  for (LastStreamSource &l : g_last_stream_sources)
    l.valid = false;
  ++g_fast_record_drops;
  ++g_packet_epoch;  // banjo-nx (2026-10-28, host packets): a packet may name the record (a few a 10 s in the city)
}

namespace {

// banjo-nx (round 4, ARM): DrawNeedsExportFlush's test as the executor needs it (any read of a pending export's window
// sends the draw to DrawStateLocked), by kind, each only when asked: the streams' fetch constants, the vertex shader's
// fetches by address, the index buffer (its guest header read only then), the textures. True: a read meets a window.
// banjo-nx (2026-10-27): banjo_renut_diag_shadow_census's table (the first eight depth targets met in 10 s).
struct ShadowCensusRow {
  const HostSurface *depth = nullptr;
  u32 width = 0, height = 0, colors = 0;
  uint64_t draws = 0, count[5] = {}, indices = 0;  // count: <= 36, <= 300, <= 3000, <= 30000, more (indices or vertices)
};
ShadowCensusRow g_shadow_census[8];
uint64_t g_shadow_census_other = 0;
inline void ShadowCensusNote(const Targets &t, const DrawCall &call) {
  ShadowCensusRow *row = nullptr;
  for (ShadowCensusRow &r : g_shadow_census) {
    if (r.depth == t.depth || !r.depth) {
      row = &r;
      break;
    }
  }
  if (!row) {
    ++g_shadow_census_other;
    return;
  }
  if (!row->depth) {
    row->depth = t.depth;
    row->width = t.depth->width;
    row->height = t.depth->height;
    row->colors = t.color_count;
  }
  ++row->draws;
  row->indices += call.count;
  ++row->count[call.count <= 36 ? 0 : call.count <= 300 ? 1 : call.count <= 3000 ? 2 : call.count <= 30000 ? 3 : 4];
}
std::string ShadowCensusReport() {
  std::string out;
  for (ShadowCensusRow &r : g_shadow_census) {
    if (!r.depth)
      break;
    out += fmt::format("{}{}x{} ({} colour) {} draws, {} indices (by count: <=36 {}, <=300 {}, <=3000 {}, <=30000 {}, more {})",
                       out.empty() ? "" : "; ", r.width, r.height, r.colors, r.draws, r.indices, r.count[0], r.count[1],
                       r.count[2], r.count[3], r.count[4]);
    r = {};
  }
  if (out.empty())
    return {};
  out = "depth-only draws by depth target (banjo_renut_diag_shadow_census): " + out +
        fmt::format("; other targets {}", g_shadow_census_other);
  g_shadow_census_other = 0;
  return out;
}

uint64_t g_extent_answers = 0, g_extent_fallbacks = 0, g_extent_verified = 0, g_extent_differ = 0;
std::string RecordExtentsReport() {
  if (!g_extent_answers && !g_extent_fallbacks)
    return {};
  std::string out = fmt::format("record-time extents (banjo_renut_record_extents, A/B 43): {} texture export checks from "
                                "the recording, {} by the answers table; verify {} checked, {} differ",
                                g_extent_answers, g_extent_fallbacks, g_extent_verified, g_extent_differ);
  g_extent_answers = g_extent_fallbacks = g_extent_verified = g_extent_differ = 0;
  return out;
}

bool FastExportReads(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                     const VertexDeclarationRecord &declaration, const TextureFetch *fetches, u32 texture_mask,
                     bool streams, bool fetch, bool index, bool textures) {
  const auto fetch_slots = [&device](u32 fetch_index) {
    return PendingExportSlotsMemo(kExportAnswerFetch + ((fetch_index - 64) & 31),
                                  device.VertexFetchWord(fetch_index, 0) & ~3u,
                                  device.VertexFetchWord(fetch_index, 1) & 0x3FFFFFCu);
  };
  if (streams) {
    for (u32 k = 0; k < declaration.stream_count; ++k) {
      const u32 stream = declaration.stream_order[k];
      if (stream == 0 && call.up_vertices)
        continue;
      if (fetch_slots(d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(stream)))
        return true;
    }
  }
  if (fetch && vs.entry) {
    for (u32 bits = vs.entry->vertexFetchMemoryMask; bits; bits &= bits - 1)
      if (fetch_slots(64 + u32(std::countr_zero(bits))))
        return true;
  }
  if (index && call.indexed && !call.up_indices && !call.tessellation) {
    // banjo-nx (2026-10-27, Phase A): the recorded header words when there are.
    if (g_draw_resources && g_draw_resources->ib_valid) {
      if (PendingExportSlotsMemo(kExportAnswerIndex, g_draw_resources->ib[1], g_draw_resources->ib[2]))
        return true;
    } else {
      const auto *ib = GuestPtr<const d3d::IndexBuffer>(device.U32(d3d::kDeviceOffsetIndexBuffer));
      if (ib && PendingExportSlotsMemo(kExportAnswerIndex, ib->Address, ib->Size))
        return true;
    }
  }
  if (textures) {
    // banjo-nx (2026-10-27): the slots' extents as the recording worked them out (g_draw_slot_extents), against the
    // export bitmap: a range no pending export's granule touches has no pending export (PendingExportSlotsMemo's own
    // first test), else PendingExportSlots. The answers table below for the others.
    static const bool extents_verify = REXCVAR_GET(banjo_renut_record_extents_verify);
    const DrawSlotExtents *const ext =
        g_draw_slot_extents && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 43 ? g_draw_slot_extents
                                                                                                : nullptr;
    for (u32 bits = texture_mask; bits; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      if (ext && r < d3d::kTextureFetchSlots && ((ext->mask >> r) & 1)) {
        const SlotExtent &e = ext->slot[r];
        if (extents_verify) [[unlikely]] {
          u32 pages[2], sizes[2];
          TextureGuestExtent(fetches[r], pages, sizes);
          ++g_extent_verified;
          if (std::memcmp(pages, e.pages, sizeof(pages)) != 0 || std::memcmp(sizes, e.sizes, sizeof(sizes)) != 0) {
            ++g_extent_differ;
            static uint32_t logged = 0;
            if (logged++ < 16)
              RGPU_WARN("banjo_renut_record_extents_verify: slot {}: recorded extent {:05X}+{:X} / {:05X}+{:X}, the "
                        "draw's {:05X}+{:X} / {:05X}+{:X}", r, e.pages[0], e.sizes[0], e.pages[1], e.sizes[1], pages[0],
                        sizes[0], pages[1], sizes[1]);
          }
        }
        ++g_extent_answers;
        u32 found = 0;
        for (u32 i = 0; i < 2 && !found; ++i) {
          if (e.pages[i] && e.sizes[i] && ExportGranulesTouched(e.pages[i] << 12, e.sizes[i]))
            found = PendingExportSlots(e.pages[i] << 12, e.sizes[i]);
        }
        if (found)
          return true;
        continue;
      }
      ++g_extent_fallbacks;
      const TextureFetch &tf = fetches[r];
      TextureExportAnswer &a = TextureExportAnswerFor(tf);
      if (!(a.generation == g_export_generation && SameBytes<sizeof(a.words)>(a.words, tf.dword))) {
        u32 pages[2], sizes[2];
        TextureGuestExtent(tf, pages, sizes);
        u32 found = 0;
        for (u32 i = 0; i < 2 && !found; ++i) {
          if (pages[i])
            found = PendingExportSlots(pages[i] << 12, sizes[i]);
        }
        std::memcpy(a.words, tf.dword, sizeof(a.words));
        a.generation = g_export_generation;
        a.slots = found;
      }
      if (a.slots)
        return true;
    }
  }
  return false;
}

// Port 166 (from fnxf_xiii, its notes of 2026-10-21, 10-22 and 11-06; banjo_deko3d_sample_mask): PA_SC_AA_MASK
// (device 0x2A00), 4 bits per pixel of a 2x2 quad (sample i of pixel p is bit 4p + i): a sample is written when any
// pixel's bit for it is set (the D3D masks repeat it for the four). Before, no draw's mask reached the GPU (every
// sample written). deko3d's mask is a command, not pipeline state (plume Deko3DSetSampleMask, recorded when it
// changes): the pipeline key and its lists stay as they are. Only on a multisampled target; host sample j takes guest
// sample j's bit (the base only ever lowers a target's count).
void SetSampleMaskLocked(plume::RenderCommandList *cmd, const GuestDevice &device, const Targets &t) {
#if defined(PLUME_DEKO3D)
  static const bool on = REXCVAR_GET(banjo_deko3d_sample_mask);
  if (!on || !cmd)
    return;
  const HostSurface *first = t.color_count ? t.colors[0] : t.depth;
  const u32 samples = first ? u32(first->samples) : 1u;
  u32 mask = 0xFFFFu;
  if (samples > 1) {
    const u32 aa = device.U32(0x2A00) & 0xFFFFu;
    const u32 all = (1u << samples) - 1u;
    mask = (aa | aa >> 4 | aa >> 8 | aa >> 12) & all;
    if (mask == all)
      mask = 0xFFFFu;
  }
  plume::Deko3DSetSampleMask(cmd, mask);
#else
  (void)cmd;
  (void)device;
  (void)t;
#endif
}

int FastDrawLocked(const u8 *device_state, const DrawCall &call, const ReplayConstants *replay) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kFastDraw);  // blur-nx (2026-11-17)
  // It keeps what the persistent shared constants, the pushed constants and the chain's hints make possible.
  // banjo-nx (round 4, ARM): the switches read once, one guard a draw.
  struct Config {
    bool on, draw_log, census, verify;
  };
  static const Config cfg = [] {
    Config c;
    c.on = REXCVAR_GET(banjo_renut_fast_draw) && REXCVAR_GET(banjo_renut_persistent_shared) &&
           REXCVAR_GET(banjo_renut_fetch_data_unread) && REXCVAR_GET(banjo_renut_chain_reuse);
    c.draw_log = !REXCVAR_GET(banjo_renut_lean_diagnostics) || REXCVAR_GET(banjo_renut_diag_draw_log_s) > 0;
    c.census = REXCVAR_GET(banjo_renut_diag_fast_census) || UnleashedCensusOn();
    c.verify = REXCVAR_GET(banjo_renut_fast_draw_verify);
    return c;
  }();
  FastState &f = g_fast;
  g_skip_fx = f.fx;  // banjo-nx (2026-10-28): banjo_renut_texture_slot_skip's array
  // banjo-nx (2026-10-27, render thread night): the A/B half read once (it changes between frames, on this thread).
  const int ab = banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed);
  if (!cfg.on || !IsDeko3D() || ab == 25)
    return 0;
  if (!f.valid)
    return FastMiss(kFastNoState);
  const ChainHint &chain = g_chain_hint;
  if (!chain.seq || chain.seq != f.seq + 1)
    return FastMiss(kFastBreak);
  // (Stage 2: Draw*UP, Begin*Vertices, range geometry and rectangle lists are the executor's too.)
  // bangaio-nx (2026-10-29, banjo_renut_point_fast_draw / A/B 69): point lists too (their point state set below).
  const bool point = call.primitive == kPointList;
  if (call.tessellation || (point && !PointFastDrawOn()))
    return FastMiss(kFastCall);
  const u32 fields = chain.fields;
  // banjo-nx (2026-10-28, per-draw night): the predication is one of the viewport's inputs below (the viewport is all
  // that reads it); A/B 61's off half sends the draw the full way as before.
  if ((fields & kFieldPredication) && ab == 61)
    return FastMiss(kFastFields);
  // The targets gathered again (stage 2) when their words changed, a target was built, retired or rebound since
  // (the surface epoch), or the draw before's depth target was left out (DropSmallerDepth, which reads the depth
  // control) and the depth control may have changed.
  const bool targets_changed = (fields & kFieldTargets) || f.surface_epoch != SurfaceEpochLocked() ||
                               (f.targets.depth_dropped && (fields & kFieldViewPackets));
  if (cfg.draw_log || cfg.census || banjo::renut_gpu::ResolveTraceOn() || g_dump_fetch)
    return FastMiss(kFastDiag);
  const bool verify = cfg.verify;
  auto &s = State();
  const GuestDevice device(device_state);
  if (!device.valid())
    return FastMiss(kFastNoState);

  // banjo-nx (2026-10-28, host packets): this state's packet, when one holds (its first lines came an op ago). The slot
  // gets this draw's answers at the end when it has none for this state, or stale ones. A/B 65's off half neither takes
  // nor makes any (the whole cost against the whole gain; the emulator's x86 caches hide the misses packets remove).
  HostPacket *pk_slot = nullptr, *pk = nullptr;
  if (HostPacketsOn() && chain.packet_key && ab != 65) {
    pk_slot = &HostPacketSlot(chain.packet_key);
    if (pk_slot->key == chain.packet_key && pk_slot->epoch == g_packet_epoch) {
      ++g_packet_hits;
      pk = pk_slot;
    } else {
      ++(pk_slot->key == chain.packet_key ? g_packet_stale : g_packet_misses);
    }
  }
  // The answers that read the targets hold while the surfaces are as they were.
  const bool pk_targets = pk && pk->surface_epoch == SurfaceEpochLocked();

  // The shaders and the declaration.
  ShaderRecord *vs = f.vs, *device_ps = f.ps;
  plume::RenderShader *host_vs = f.host_vs, *host_ps = f.host_ps;
  const VertexDeclarationRecord *declaration = f.declaration;
  const bool shaders_changed = (fields & kFieldShaders) != 0;
  if (shaders_changed && pk) {
    // banjo-nx (2026-10-28, host packets): the records, host shaders and declaration of this state, without a lookup or
    // a record line read; the lines of the records the draw reads further down (their draw summaries) asked for now.
    vs = pk->vs;
    device_ps = pk->ps;
    host_vs = pk->host_vs;
    host_ps = pk->host_ps;
    for (const ShaderRecord *r : {vs, device_ps}) {
      if (const char *p = reinterpret_cast<const char *>(r)) {
        __builtin_prefetch(p);
        __builtin_prefetch(p + 64);
        __builtin_prefetch(p + 128);
        __builtin_prefetch(p + 192);
      }
    }
  } else if (shaders_changed) {
    vs = FindShaderLocked(device.U32(d3d::kDeviceOffsetVertexShader), RecordKind::VertexShader);
    device_ps = FindShaderLocked(device.U32(d3d::kDeviceOffsetPixelShader), RecordKind::PixelShader);
    if (!vs)
      return FastMiss(kFastShaders);
    host_vs = HostShaderOf(vs);
    host_ps = device_ps ? HostShaderOf(device_ps) : nullptr;
    if (!host_vs || (device_ps && !host_ps))
      return FastMiss(kFastShaders);
    // banjo-nx (2026-10-28, cache-miss audit 2): what a shader change reads further down whatever the fields say: the
    // device snapshot's bools and alpha test lines (written by the replay only when they change), and the bound
    // declaration's first three lines (its hash, stream order and used mask; its first streams).
    {
      const d3d::Device &snap = device.dev();
      __builtin_prefetch(&snap.m_Constants.VertexShaderB[0]);
      __builtin_prefetch(&snap.m_ControlPacket);
      __builtin_prefetch(&snap.m_ValuesPacket);
      if (const VertexDeclarationRecord *bd = vs->bound_declaration.get()) {
        const char *p = reinterpret_cast<const char *>(bd);
        __builtin_prefetch(p);
        __builtin_prefetch(p + 64);
        __builtin_prefetch(p + 128);
      }
    }
  }
  // The declaration as DrawStateLocked finds it: the device's, else the one D3DVertexShader_Bind bound to the shader,
  // else the shader's own fetches', else none; again when the shaders or the device's word changed.
  const u32 *shader_strides = f.shader_strides;  // null for a device declaration
  bool declaration_from_va = f.declaration_from_va;
  if ((shaders_changed || (fields & kFieldDecl)) && pk) {
    declaration = pk->declaration;  // banjo-nx (2026-10-28, host packets)
    shader_strides = pk->shader_strides;
    declaration_from_va = pk->declaration_from_va;
  } else if (shaders_changed || (fields & kFieldDecl)) {
    const u32 declaration_va = device.U32(d3d::kDeviceOffsetVertexDeclaration);
    shader_strides = nullptr;
    declaration_from_va = declaration_va != 0;
    if (declaration_va) {
      if (declaration_va != g_last_declaration_va) {
        g_last_declaration = FindRecord<VertexDeclarationRecord>(declaration_va, RecordKind::VertexDeclaration);
        g_last_declaration_va = g_last_declaration ? declaration_va : 0;
      }
      declaration = g_last_declaration_va == declaration_va ? g_last_declaration : nullptr;
      if (!declaration && declaration_va == Video::GuestDevice() + kDeviceDeclarationOffset)
        declaration = DeviceDeclarationLocked(device);  // katamari-nx
    } else if (vs->bound_declaration) {
      declaration = vs->bound_declaration.get();
      shader_strides = vs->bound_strides;
    } else if (const VertexDeclarationRecord *fetch = ShaderFetchDeclarationLocked(*vs)) {
      declaration = fetch;
      shader_strides = vs->fetch_strides;
    } else {
      declaration = &NoDeclaration();
    }
    if (!declaration)
      return FastMiss(kFastDecl);
  }
  // The shaders' own cases, looked at when they change (round 4: the kept shaders' answer otherwise).
  // banjo-nx (2026-10-28, host packets): a packet is made only by a draw whose shaders passed them.
  if (shaders_changed && !pk) {
    if (DrawSummaryOf(*vs).exports || (device_ps && DrawSummaryOf(*device_ps).exports))
      return FastMiss(kFastExports);
    if ((device_ps && DrawSummaryOf(*device_ps).composite) || DrawSummaryOf(*vs).skin_once)
      return FastMiss(kFastSpecial);
    // renut-ultimate (2026-11-16, port 145; banjo_renut_fetch_filters, off by default): shaders whose fetches give
    // their own texture filters stay DrawStateLocked's, which applies them (FillTextures' filters).
    if (FetchFiltersOn() && (!vs->fetch_filters.Empty() || (device_ps && !device_ps->fetch_filters.Empty())))
      [[unlikely]] return FastMiss(kFastSpecial);
    // banjo-nx (2026-10-27, constants): the new shaders' literal runs, which the constants' push reads (a heap object a
    // shader, cold at a shader change), fetched while the declaration, geometry and textures are worked out.
    if (const void *lr = DrawSummaryOf(*vs).literal_runs)
      __builtin_prefetch(lr);
    if (device_ps) {
      if (const void *lr = DrawSummaryOf(*device_ps).literal_runs)
        __builtin_prefetch(lr);
    }
    if (device_ps) {
      const uint64_t h = device_ps->hash;
      if (h == 0x2F36770769087F99ull || h == 0xC92BA80672FB9774ull || h == 0xB3C10F3D80EF61E2ull ||
          h == 0xD40EBF0336F89333ull)
        return FastMiss(kFastSpecial);  // the fur family: shells and skinning stay DrawStateLocked's
    }
  } else if (!f.shaders_fast) {
    return FastMiss(kFastSpecial);
  }
  // banjo-nx (2026-10-27, shadow sort, A/B 41): whether the shaders' contents changed, not only their objects. N&B makes
  // a shader object per mesh (another address at 69-72 % of the draws), and what follows reads the shaders through
  // their cache entries only (the depth-only answer, the texture mask, the texture slots, the export reads, the fetch
  // data's mask, the bools' answer, the pipeline key); the declaration, which may come with the object, is compared
  // where it is read.
  // banjo-nx (2026-10-28, host packets): the entries as the packet holds them (no record read for them).
  const ShaderCacheEntry *const vs_entry_now = pk ? pk->vs_entry : vs->entry;
  const ShaderCacheEntry *const ps_entry_now = pk ? pk->ps_entry : (device_ps ? device_ps->entry : nullptr);
  const bool entries_changed =
      shaders_changed && (ab == 41 || !f.vs || vs_entry_now != f.vs->entry ||
                          ps_entry_now != (f.ps ? f.ps->entry : nullptr));
  // banjo-nx (round 4, ARM): the shader entries' lines (the texture masks, the constant summaries, the key's inputs),
  // fetched now and read after the geometry's work.
  if (shaders_changed) {
    if (vs_entry_now) {
      __builtin_prefetch(vs_entry_now);
      __builtin_prefetch(reinterpret_cast<const char *>(vs_entry_now) + 64);
    }
    if (ps_entry_now) {
      __builtin_prefetch(ps_entry_now);
      __builtin_prefetch(reinterpret_cast<const char *>(ps_entry_now) + 64);
    }
  }
  // The geometry first: whole-buffer copies, or (stage 2) the ranges the draw reads, as DrawStateLocked converts them
  // (PrepareGeometry; Draw*UP's own vertices and indices among them). Nothing is recorded for either.
  Geometry geometry;
  CachedStream *whole_copies[kMaxVertexStreams] = {};
  const bool whole = WholeGeometryLocked(device, call, *vs, shader_strides, declaration->layout,
                                         declaration->layout_hash, geometry, whole_copies);
  if (!whole && !PrepareGeometry(device, call, geometry))
    return FastMiss(kFastGeometry);
  // banjo-nx (2026-10-27, render thread night, A/B 30): a whole-buffer draw whose vertex shader never reads the vertex
  // index draws with its base vertex (the hardware adds it to each index) from copies bound at their start, rather than
  // from copies bound at the base and a vertex index offset in the shared constants: the same vertices, without a
  // vertex buffer bind and a shared constant line pushed at every draw (the bindings stay while the copies do).
#if defined(PLUME_DEKO3D)
  bool reads_vertex_id = true;
  if (whole) {
    // banjo-nx (2026-10-27): the answer kept in the record's summary (the b35 profile's hottest line of the fast path).
    DrawSummaryOf(*vs);
    int8_t &known = vs->draw_summary.reads_vertex_id;
    if (known < 0)
      known = plume::Deko3DShaderReadsVertexId(host_vs) ? 1 : 0;
    reads_vertex_id = known != 0;
  }
  const bool base_draw = whole && !reads_vertex_id && ab != 30;
#else
  const bool base_draw = false;
#endif
  // banjo-nx (2026-10-27, render thread night): the open list tested here, the call only to open one (it was a call to
  // video.cpp at every draw); the same for the list's draw count below.
  if (!s.list_open && !Video::EnsureCommandListLocked())
    return -1;
  SyncBound(s);
  // The targets: the draw before's, or gathered again as DrawStateLocked gathers them (with the smaller depth target
  // left out for a draw that does not use it).
  Targets gathered;
  if (targets_changed && pk_targets) {
    gathered = pk->targets;  // banjo-nx (2026-10-28, host packets): the same state and surfaces, the same targets
  } else if (targets_changed) {
    if (!GatherTargets(device, gathered))
      return FastMiss(kFastFields);
    if (gathered.depth && gathered.color_count && gathered.depth->scale != gathered.scale)
      DropSmallerDepth(device, gathered, vs->hash, device_ps ? device_ps->hash : 0);
  }
  const Targets &targets = targets_changed ? gathered : f.targets;
  // sasrt-nx (2026-10-29): the EDRAM depth restore (GatherTargets does it for gathered targets) for kept ones too, when another
  // writer (a clear, a resolve, a draw of DrawStateLocked) moved the write sequence since.
  if (!targets_changed && targets.depth && g_surface_write_seq != f.sasrt_write_seq && banjo::renut_gpu::DepthRestoreOn()) {
    if (HostSurface *color = DepthRestoreSourceLocked(targets.depth))
      RestoreDepthFromColorLocked(targets.depth, color);
  }
  HandOverBeforeDrawLocked(targets, u32(device.dev().m_ControlPacket.DepthControl),
                           u32(device.dev().m_ValuesPacket.StencilRefMask));
  // banjo-nx (2026-10-28, cache-miss audit 2): the targets' write counters the draw bumps at its end (a miss there).
  for (u32 i = 0; i < targets.color_count; ++i)
    __builtin_prefetch(&targets.colors[i]->writes);
  if (targets.depth)
    __builtin_prefetch(&targets.depth->writes);

  // The depth-only answer (DrawsWithoutPixelShader reads the shader, the targets and the colour mask and control).
  bool depth_only = f.depth_only;
  if ((entries_changed || (fields & kFieldViewPackets) || targets_changed) && pk_targets)
    depth_only = pk->depth_only;  // banjo-nx (2026-10-28, host packets): the same shader, targets and state
  else if (entries_changed || (fields & kFieldViewPackets) || targets_changed)
    depth_only = device_ps && DrawsWithoutPixelShader(device, targets, *device_ps);
  ShaderRecord *ps = depth_only ? nullptr : device_ps;
  plume::RenderShader *draw_host_ps = depth_only ? nullptr : host_ps;
  {
    static const bool shadow_census = REXCVAR_GET(banjo_renut_diag_shadow_census);  // banjo-nx (2026-10-27)
    if (shadow_census && (depth_only || !device_ps) && targets.depth) [[unlikely]]
      ShadowCensusNote(targets, call);
  }
  // Round 4: the shaders' masks read only when the shaders or the depth-only answer changed.
  // banjo-nx (2026-10-28, host packets): the packet's mask when its depth-only answer is this one (the same entries).
  const u32 texture_mask =
      !(entries_changed || depth_only != f.depth_only) ? f.draw_texture_mask
      : pk && depth_only == pk->depth_only
          ? pk->texture_mask
          : (vs->entry ? vs->entry->textureFetchMask : 0) | (ps && ps->entry ? ps->entry->textureFetchMask : 0);
  g_fetch_3d_mask = Fetch3DMaskOf(vs, ps);  // kameo-nx (2026-11-03)

  // The texture fetches the draw binds, converted when something they come from changed (storage only: the mask's
  // slots are written).
  union FastFetches {
    FastFetches() {}
    TextureFetch f[32];
  };
  FastFetches fetch_storage;
  TextureFetch *const fetches = fetch_storage.f;
  // (A texture slot from 26 on is read through the vertex fetch constants' words.)
  const bool textures_inputs = (fields & (kFieldFetch | kFieldTexWords)) ||
                               ((fields & kFieldVertexFetch) && (texture_mask >> d3d::kTextureFetchSlots)) ||
                               entries_changed || depth_only != f.depth_only ||
                               targets_changed ||
                               !f.textures_valid || f.texture_mask != texture_mask ||
                               f.texture_frame != TextureFrameLocked() || f.texture_epoch != g_texture_epoch ||
                               f.depth_epoch != g_depth_epoch;
  const d3d::Device &dev = device.dev();
  const bool exports_pending = AnyPendingExports();
  // Round 4: which kinds of read the export test looks at again (each kind's answer kept by the export generation
  // while its inputs are the same).
  const bool export_generation_same = f.export_generation == g_export_generation;
  const bool check_streams = exports_pending && (!export_generation_same || !f.export_streams_ok || entries_changed ||
                                                 declaration != f.declaration ||
                                                 (fields & (kFieldFetch | kFieldVertexFetch | kFieldStreams | kFieldDecl)));
  const bool check_fetch = exports_pending && (!export_generation_same || !f.export_fetch_ok || entries_changed ||
                                               (fields & (kFieldFetch | kFieldVertexFetch)));
  const bool check_index = exports_pending && call.indexed && (!export_generation_same || !f.export_index_ok ||
                                                               (fields & kFieldIndex));
  const bool check_textures = exports_pending && (!export_generation_same || !f.export_textures_ok || textures_inputs);
  if (textures_inputs || check_textures || verify) {
    for (u32 bits = texture_mask; bits; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      fetches[r] = DeviceTextureFetch(dev, r);
      // banjo-nx (round 4, ARM): the header and the memo entry FillTextures reads for the slot, fetched now.
      // banjo-nx (2026-10-27, Phase A): the slot's object's answer lines instead, when the recording gave it.
      if (textures_inputs && r < d3d::kTextureFetchSlots && g_draw_resources && ((g_draw_resources->mask >> r) & 1)) {
        __builtin_prefetch(reinterpret_cast<const char *>(g_draw_resources->slot[r]) + 64);
        __builtin_prefetch(reinterpret_cast<const char *>(g_draw_resources->slot[r]) + 128);
      } else if (textures_inputs && r < d3d::kTextureFetchSlots) {
        const u32 va = SlotTextureVa(device, r);
        if (const auto *header = va ? GuestPtr<const d3d::BaseTexture>(va) : nullptr)
          __builtin_prefetch(&header->Format);
        const TextureSlotMemo &m = TextureMemoSlot(va, fetches[r]);
        __builtin_prefetch(&m);
        __builtin_prefetch(reinterpret_cast<const char *>(&m) + 64);
      }
    }
  }
  // banjo-nx (2026-10-27, Phase A): the slots' objects as the recording gave them, every slot of the mask (else the
  // headers, below).
  const u32 tex_slots_mask = texture_mask & ((1u << d3d::kTextureFetchSlots) - 1u);
  const bool tex_objects = memo_on_objects(g_draw_resources) && (g_draw_resources->mask & tex_slots_mask) == tex_slots_mask;
  if (!textures_inputs && !tex_objects) {
    for (u32 bits = tex_slots_mask; bits; bits &= bits - 1) {
      const u32 va = f.texture_va[std::countr_zero(bits)];
      if (const auto *header = va ? GuestPtr<const d3d::BaseTexture>(va) : nullptr)
        __builtin_prefetch(&header->Format);
    }
  }
  // banjo-nx (round 4, ARM): the pipeline cache's entry this draw most likely reads (its tag with the draw before's
  // alpha test and render-state epoch), fetched now.
  if (entries_changed || (fields & (kFieldBools | kFieldDest | kFieldViewPackets | kFieldDecl)) || targets_changed) {
    const ShaderCacheEntry *const guess_ps = depth_only ? nullptr : (device_ps ? device_ps->entry : nullptr);
    const u32 guess_misc = call.primitive | u32(geometry.per_instance) << 8 | f.alpha_test_func << 9 |
                           u32(ab) << 16;
    const FastPipeline &c = g_fast_pipelines[FastPipelineIndex(f.rs_epoch, vs->entry, guess_ps,
                                                               declaration->layout_hash, guess_misc)];
    __builtin_prefetch(&c);
    __builtin_prefetch(reinterpret_cast<const char *>(&c) + 64);
  }
  // Pending memexport results: the draw may read none of their windows (DrawNeedsExportFlush's "none, and no read met
  // a window"), each kind of read kept while its inputs and the pending exports are the same (round 4).
  if ((check_streams || check_fetch || check_index || check_textures) &&
      FastExportReads(device, call, *vs, *declaration, fetches, texture_mask, check_streams, check_fetch, check_index,
                      check_textures))
    return FastMiss(kFastExportRead);
#if defined(PLUME_DEKO3D)
  if (!banjo::renut_gpu::PushConstantsOn() || !EnsurePushedBuffers())
    return FastMiss(kFastPushed);
#else
  return FastMiss(kFastPushed);
#endif
  // From here the draw is the fast path's: the state below is this draw's.
  ++g_trace_draws;
  if (::renut::gpu::host_batch::CensusOn()) {  // blur-nx (2026-11-18, the census's #19): the draws with no field changed
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kFastDraws);
    if (!fields)
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kFastDrawsNoFields);
  }
  SharedConstants &sc = DrawSharedLocked();
  // What the draw before left for its next draw to put back (DrawStateLocked's own steps): the skin mode, the point
  // sprite state, the memexport windows.
  if (g_draw_sc_track.skin) {
    SetShared(sc, sc.pad_1352[0], 0.0f);
    g_draw_sc_track.skin = false;
  }
  if (point) {
    // bangaio-nx (2026-10-29): the point size state as DrawStateLocked sets it (its line marked only when it changes).
    float point_state[4];
    PointStateOf(dev, point_state);
    SetShared(sc, sc.point_state, point_state);
    g_draw_sc_track.point = true;
  } else if (g_draw_sc_track.point) {
    static constexpr float kZero4[4] = {};
    SetShared(sc, sc.point_state, kZero4);
    g_draw_sc_track.point = false;
  }
  SetUserClipPlane(sc, device.dev(), vs->hash);  // rr6-nx / vivapinata-nx: plane 0 or zeros, every draw
  if (g_draw_sc_track.memexport) {
    std::memset(sc.memexport_streams, 0, sizeof(sc.memexport_streams));
    MarkShared(sc, sc.memexport_streams, sizeof(sc.memexport_streams));
    g_draw_sc_track.memexport = false;
  }

  // Textures.
  // banjo-nx (2026-10-27, render thread night, A/B 31): a draw that samples nothing (the shadow maps' depth-only draws)
  // leaves the texture words in the block as the draw before set them: its shaders read none of them, and the next
  // draw that samples finds its slots' last answers still in the block (FillTextures put the defaults back in every
  // slot the last call set and pushed them, and the next draw set and pushed them again, at every change of kind).
  const bool skip_textures = textures_inputs && texture_mask == 0 &&
                             ab != 31;
  if (skip_textures) {
    f.textures_valid = true;
    f.texture_mask = 0;
    f.texture_frame = TextureFrameLocked();
    f.texture_epoch = g_texture_epoch;
    f.depth_epoch = g_depth_epoch;
    f.fx_float_sampled = f.fx_words_viewed = 0;
    f.layout_epoch = g_layout_epoch;
  } else if (textures_inputs) {
    bool all_memo = false;
    g_handover_targets = &targets;
    FillTextures(device, texture_mask, sc, f.fx, &all_memo, fetches, nullptr);
    g_handover_targets = nullptr;
    f.textures_valid = all_memo;
    for (u32 bits = texture_mask & ((1u << d3d::kTextureFetchSlots) - 1u); bits && all_memo; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      const u32 va = SlotTextureVa(device, r);
      f.texture_va[r] = va;
      f.texture_obj[r] = tex_objects ? g_draw_resources->slot[r] : nullptr;  // banjo-nx (2026-10-27, Phase A)
      if (tex_objects)
        continue;  // (the objects stand for the headers: below)
      const auto *header = SlotHeader(va);  // sasrt-nx (2026-10-29)
      if (header)
        std::memcpy(f.texture_header[r], &header->Format, sizeof(f.texture_header[r]));
      else
        f.textures_valid = false;
    }
    f.texture_mask = texture_mask;
    f.texture_frame = TextureFrameLocked();
    f.texture_epoch = g_texture_epoch;
    f.depth_epoch = g_depth_epoch;
    f.fx_float_sampled = f.fx_words_viewed = 0;
    for (u32 bits = texture_mask & ((1u << d3d::kTextureFetchSlots) - 1u); bits; bits &= bits - 1) {
      const BindEffects &fx = f.fx[std::countr_zero(bits)];
      f.fx_float_sampled += fx.float_sampled;
      f.fx_words_viewed += fx.words_viewed;
    }
    f.layout_epoch = g_layout_epoch;
  } else {
    // The headers (guest memory, which no device field covers) the same, or the slots filled again.
    // banjo-nx (2026-10-27, Phase A): with the recording's objects, the same objects (a header the game rewrites takes a
    // new object when it binds it again; verify counts headers changed in between).
    bool same = true;
    for (u32 bits = tex_slots_mask; bits && same; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      if (f.texture_obj[r]) {  // filled through the objects: the same objects, else filled again
        same = tex_objects && f.texture_obj[r] == g_draw_resources->slot[r];
        continue;
      }
      const auto *header = SlotHeader(f.texture_va[r]);  // sasrt-nx (2026-10-29)
      same = header && SameBytes<24>(f.texture_header[r], &header->Format);
    }
    if (!same) {
      for (u32 bits = texture_mask; bits; bits &= bits - 1) {
        const u32 r = u32(std::countr_zero(bits));
        fetches[r] = DeviceTextureFetch(dev, r);
      }
      bool all_memo = false;
      g_handover_targets = &targets;
      FillTextures(device, texture_mask, sc, f.fx, &all_memo, fetches, nullptr);
      g_handover_targets = nullptr;
      f.textures_valid = false;  // headers taken again at the next change
    } else {
      if (f.layout_epoch == g_layout_epoch) {
        // Round 4: no layout changed since the slots' transitions were applied: they hold (the counters as then).
        for (u32 i = 0; i < f.fx_float_sampled; ++i)
          banjo::renut_gpu::CountDepthFloatSampled();
        for (u32 i = 0; i < f.fx_words_viewed; ++i)
          banjo::renut_gpu::CountDepthWords(true);
      } else {
        for (u32 bits = texture_mask & ((1u << d3d::kTextureFetchSlots) - 1u); bits; bits &= bits - 1) {
          const BindEffects &fx = f.fx[std::countr_zero(bits)];
          for (HostTexture *tex : fx.transition)
            if (tex && tex->layout != plume::RenderTextureLayout::SHADER_READ)
              TransitionTextureLocked(tex, plume::RenderTextureLayout::SHADER_READ);
          if (fx.handover_surface)
            TransitionSurfaceLocked(fx.handover_surface, plume::RenderTextureLayout::SHADER_READ);
          if (fx.handover_image)
            TransitionDepthImageLocked(*fx.handover_image, plume::RenderTextureLayout::SHADER_READ);
          if (fx.float_sampled)
            banjo::renut_gpu::CountDepthFloatSampled();
          if (fx.words_viewed)
            banjo::renut_gpu::CountDepthWords(true);
        }
        f.layout_epoch = g_layout_epoch;
      }
      if (verify) {
        for (u32 bits = texture_mask; bits; bits &= bits - 1) {
          const u32 r = u32(std::countr_zero(bits));
          fetches[r] = DeviceTextureFetch(dev, r);
        }
        SharedConstants made{};
        g_handover_targets = &targets;
        FillTextures(device, texture_mask, made, nullptr, nullptr, fetches, nullptr);
        g_handover_targets = nullptr;
        bool differ = false;
        for (u32 bits = texture_mask; bits; bits &= bits - 1) {
          const u32 r = u32(std::countr_zero(bits));
          differ |= made.texture_indices[0][r] != sc.texture_indices[0][r] ||
                    made.texture_indices[1][r] != sc.texture_indices[1][r] ||
                    made.texture_indices[2][r] != sc.texture_indices[2][r] ||
                    made.sampler_indices[r] != sc.sampler_indices[r];
        }
        if (differ) {
          static uint32_t logged = 0;
          if (logged++ < 8)
            RGPU_WARN("banjo_renut_fast_draw_verify: the kept texture words differ from those filled (VS {:016X})",
                      vs->hash);
          for (u32 bits = texture_mask; bits; bits &= bits - 1) {
            const u32 r = u32(std::countr_zero(bits));
            for (u32 d = 0; d < 3; ++d)
              SetShared(sc, sc.texture_indices[d][r], made.texture_indices[d][r]);
            SetShared(sc, sc.sampler_indices[r], made.sampler_indices[r]);
          }
        }
      }
    }
  }

  // The bools and the alpha test (the alpha-to-mask test depends on the pixel shader drawn and the first target).
  u32 alpha_test_func = f.alpha_test_func;
  if (((fields & (kFieldBools | kFieldViewPackets)) || entries_changed || depth_only != f.depth_only ||
       targets_changed) &&
      pk_targets && !verify) {
    // banjo-nx (2026-10-28, host packets): the same state, pixel shader drawn and first target: the same words.
    SetShared(sc, sc.booleans, pk->booleans);
    SetLoopConstants(sc, dev);  // halo3-nx: the packet does not hold them
    SetShared(sc, sc.alpha_test_func, pk->alpha_test_func);
    SetShared(sc, sc.alpha_threshold, pk->alpha_threshold);
    alpha_test_func = pk->alpha_test_func;
  } else if ((fields & (kFieldBools | kFieldViewPackets)) || entries_changed || depth_only != f.depth_only ||
             targets_changed || verify) {
    const u8 *bools = reinterpret_cast<const u8 *>(&dev.m_Constants.VertexShaderB[0]);
    u32 booleans[8];
    for (u32 i = 0; i < 8; ++i) {
      u32 word;
      std::memcpy(&word, bools + 4 * i, 4);
      booleans[i] = __builtin_bswap32(word);
    }
    const u32 color_control = dev.m_ControlPacket.ColorControl;
    u32 func = (color_control & 8) ? (color_control & 7) : 7;
    float threshold = dev.m_ValuesPacket.AlphaRef;
    if (((color_control >> 4) & 1) && banjo::renut_gpu::AlphaToMaskTestOn()) {
      const HostSurface *first = targets.color_count ? targets.colors[0] : targets.depth;
      if (ps && first && first->samples == plume::RenderSampleCount::COUNT_1)
        AlphaToMaskTestSingleSample(func, threshold);  // blur-nx (2026-11-13): with the game's own test too
    }
    if (verify && !((fields & (kFieldBools | kFieldViewPackets)) || entries_changed || depth_only != f.depth_only ||
                    targets_changed) &&
        (func != sc.alpha_test_func || threshold != sc.alpha_threshold ||
         std::memcmp(booleans, sc.booleans, sizeof(booleans)) != 0)) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_WARN("banjo_renut_fast_draw_verify: the kept bools or alpha test differ from the device's");
    }
    SetShared(sc, sc.booleans, booleans);
    SetLoopConstants(sc, dev);  // halo3-nx
    SetShared(sc, sc.alpha_test_func, func);
    SetShared(sc, sc.alpha_threshold, threshold);
    alpha_test_func = func;
  }

  // The viewport.
  const bool viewport_inputs = (fields & (kFieldViewPackets | kField29C0 | kFieldPredication)) ||
                               f.viewport_tiles != g_tiles.size() ||
                               f.viewport_predication != g_replay_predication || targets_changed;
  if (viewport_inputs || verify) {
    // banjo-nx (2026-10-28, host packets): the packet's, for the same state, targets, tiles and predication.
    const bool from_packet = pk_targets && !verify && pk->viewport_tiles == u32(g_tiles.size()) &&
                             pk->viewport_predication == g_replay_predication;
    const ViewportState viewport = from_packet ? pk->viewport : ComputeViewport(device, targets);
    if (!viewport_inputs && std::memcmp(&viewport, &f.viewport, sizeof(viewport)) != 0) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_WARN("banjo_renut_fast_draw_verify: the kept viewport differs from the one computed");
    }
    f.viewport = viewport;
    f.viewport_tiles = g_tiles.size();
    f.viewport_predication = g_replay_predication;
    SetShared(sc, sc.screen_space, u32(viewport.screen_space ? 1 : 0));
    SetShared(sc, sc.screen_scale, viewport.screen_scale);
    SetShared(sc, sc.screen_offset, viewport.screen_offset);
    SetShared(sc, sc.depth_remap, viewport.depth_remap);  // sasrt-nx (2026-10-29): g_DepthRemap, as DrawStateLocked
    SetShared(sc, sc.viewport_size, viewport.guest_size);
    SetShared(sc, sc.pixel_origin, viewport.pixel_origin);
    if (targets_changed) {  // with the targets (DrawStateLocked sets them at every draw)
      SetShared(sc, sc.resolution_scale, targets.scale.Factor());
      SetShared(sc, sc.pad_1352[1], std::bit_cast<float>(InternalHeightsWord()));
    }
  }

  // What depends on the call: the geometry's vertex index offset, the bound fetches and the fetch data.
  if (whole)
    ++g_whole_draws;
  else
    ++g_range_draws;
  const u32 tessellation[4] = {0, 0, 0, whole ? (base_draw ? 0u : geometry.whole_base) : geometry.first_vertex};
  SetShared(sc, sc.tessellation, tessellation);
  g_base_draws += base_draw;  // banjo-nx (2026-10-27): the 10 s line's
  // Round 4: the bound fetch entries and the fetch data's words marked only where they changed (marking them all sent
  // their lines to the GPU again at every draw): the last draw's entries kept, zeroed, described again, compared.
  constexpr u32 kBoundEntries = sizeof(SharedConstants::bound_fetch) / sizeof(SharedConstants::bound_fetch[0]);
  static_assert(kBoundEntries <= 32);
  const u32 old_entries = g_draw_sc_track.bound_entries;
  u32 old_bound[kBoundEntries][sizeof(sc.bound_fetch[0]) / 4];
  for (u32 bits = old_entries; bits; bits &= bits - 1) {
    const u32 i = u32(std::countr_zero(bits));
    std::memcpy(old_bound[i], sc.bound_fetch[i], sizeof(old_bound[i]));
    std::memset(sc.bound_fetch[i], 0, sizeof(sc.bound_fetch[0]));
  }
  u32 bound_entries = 0;
  const u32 fetch_mask = (vs->entry ? vs->entry->vertexFetchMemoryMask : 0) |
                         DescribeBoundFetches(device, call, *vs, *declaration, shader_strides, sc, &bound_entries);
  g_draw_sc_track.bound_entries = bound_entries;
  for (u32 bits = old_entries | bound_entries; bits; bits &= bits - 1) {
    const u32 i = u32(std::countr_zero(bits));
    static constexpr u32 kZeroEntry[sizeof(sc.bound_fetch[0]) / 4] = {};
    const void *was = ((old_entries >> i) & 1) ? static_cast<const void *>(old_bound[i]) : kZeroEntry;
    if (!SameBytes<sizeof(sc.bound_fetch[0])>(was, sc.bound_fetch[i]))
      MarkShared(sc, sc.bound_fetch[i], sizeof(sc.bound_fetch[0]));
  }
  for (u32 bits = g_draw_sc_track.fetch_slots & ~fetch_mask; bits; bits &= bits - 1)
    SetShared(sc, sc.vertex_fetch_slots[64 + std::countr_zero(bits)], 0u);
  g_draw_sc_track.fetch_slots = fetch_mask;
  plume::RenderCommandList *cmd = s.command_list;
  const bool keep_fetch = f.fetch_kept && fetch_mask && fetch_mask == f.fetch_kept_mask && !entries_changed &&
                          !(fields & (kFieldFetch | kFieldVertexFetch | kFieldStreams | kFieldDecl)) &&
                          !call.up_vertices &&
                          (f.fetch_kept_pool || f.fetch_kept_chunk == Video::UploadsLocked().CurrentBuffer()) &&
                          f.fetch_kept_flush == g_flush_serial && f.fetch_kept_resets == UploadRing::Resets() &&
                          f.fetch_kept_list == s.list_generation && f.fetch_kept_export == g_export_generation &&
                          ab != 36;
  if (keep_fetch) {
    ++g_fetch_kept_draws;  // banjo-nx (2026-10-27): the words and the binding the draw before left
  } else if (fetch_mask == 0) {
    f.fetch_kept = false;
    g_fetch_record.count = 0;
    g_fetch_record.unusable = call.up_vertices != nullptr;
    SetShared(sc, sc.vertex_fetch_data, uint64_t(0));
    SetShared(sc, sc.vertex_fetch_bytes, 0u);
  } else {
    // Round 4: the words UploadVertexFetchData writes, kept to mark only those it changed.
    u32 old_slots[32];
    for (u32 bits = fetch_mask; bits; bits &= bits - 1) {
      const u32 i = u32(std::countr_zero(bits));
      old_slots[i] = sc.vertex_fetch_slots[64 + i];
    }
    const uint64_t old_data = sc.vertex_fetch_data;
    const u32 old_bytes = sc.vertex_fetch_bytes;
    const UploadAllocation fetch_alloc = UploadVertexFetchData(device, call, fetch_mask, sc);
    if (!fetch_alloc)
      return -1;
    f.fetch_kept = !g_fetch_record.unusable;  // not through an export window or with Draw*UP data
    f.fetch_kept_mask = fetch_mask;
    f.fetch_kept_chunk = fetch_alloc.ref.ref;
    f.fetch_kept_pool = g_fetch_from_pool;
    f.fetch_kept_flush = g_flush_serial;
    f.fetch_kept_resets = UploadRing::Resets();
    f.fetch_kept_list = s.list_generation;
    f.fetch_kept_export = g_export_generation;
    for (u32 bits = fetch_mask; bits; bits &= bits - 1) {
      const u32 i = u32(std::countr_zero(bits));
      if (sc.vertex_fetch_slots[64 + i] != old_slots[i])
        MarkShared(sc, &sc.vertex_fetch_slots[64 + i], 4);
    }
    if (sc.vertex_fetch_bytes != old_bytes)
      MarkShared(sc, &sc.vertex_fetch_bytes, sizeof(sc.vertex_fetch_bytes));
#if defined(PLUME_DEKO3D)
    if (sc.vertex_fetch_data) {
      const uint64_t address = sc.vertex_fetch_data;
      const uint64_t base = address & ~uint64_t(0xFF);
      plume::Deko3DBindFetchData(cmd, base, u32(address - base) + sc.vertex_fetch_bytes);
      sc.vertex_fetch_data = old_data;  // the word as it was, so that SetShared marks its line only if it changes
      SetShared(sc, sc.vertex_fetch_data, address - base);
    } else if (old_data != 0) {
      MarkShared(sc, &sc.vertex_fetch_data, sizeof(sc.vertex_fetch_data));
    }
#else
    if (sc.vertex_fetch_data != old_data)
      MarkShared(sc, &sc.vertex_fetch_data, sizeof(sc.vertex_fetch_data));
#endif
  }

  // The float constants (the groups the recording says changed), then the shared constants' marked lines.
  static constexpr u32 kNoRegisters[8] = {};
  static constexpr u32 kAllRegisters[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  const u32 *vs_used = vs->entry ? vs->entry->floatConstants : kAllRegisters;
  const u32 *ps_used = !ps ? kNoRegisters : ps->entry ? ps->entry->floatConstants : kAllRegisters;
  // banjo-nx (2026-10-27, constants): the literal runs' arrays (the runs object came with the shader change's prefetch).
  if (shaders_changed) {
    for (const ShaderRecord *r : {vs, ps}) {
      if (const auto *lr = r ? static_cast<const LiteralRuns *>(DrawSummaryOf(*r).literal_runs) : nullptr) {
        // banjo-nx (2026-10-28): one block now: its first lines, with no load of the object first.
        const char *b = reinterpret_cast<const char *>(lr);
        __builtin_prefetch(b);
        __builtin_prefetch(b + 64);
        __builtin_prefetch(b + 128);
      }
    }
  }
  const FloatConstants vs_constants = DrawFloatConstants(device_state, replay, false);
  const FloatConstants ps_constants = DrawFloatConstants(device_state, replay, true);
#if defined(PLUME_DEKO3D)
  g_diag_draw_no_ps = !ps;  // banjo-nx (2026-10-28, diagnostic)
  PushFloatConstants(cmd, vs_constants, vs_used, g_bound.pushed_vs, vs->entry, g_pushed_buffers.address,
                     DrawSummaryOf(*vs));
  // banjo-nx (2026-10-27, render thread night, A/B 31): no pixel shader, no pixel constants to push. The replay's pixel
  // dirty groups are left set, so the next draw with a pixel shader compares every group written since its last push
  // (PushFloatConstants' tracked case), as if this draw were not there.
  if (ps || ab == 31)
    PushFloatConstants(cmd, ps_constants, ps_used, g_bound.pushed_ps, ps ? ps->entry : nullptr,
                       g_pushed_buffers.address + kPushedFloatStride, ps ? DrawSummaryOf(*ps) : SummaryOf(kNoRegisters));
  g_bound.vs_constants.replay = 0;
  g_bound.ps_constants.replay = 0;
  banjo::renut_gpu::CountPushedConstants(1, 2);
  plume::RenderBuffer *memexport = MemExportBufferLocked(s);
  if (!memexport)
    return -1;
  // banjo-nx (round 4): its address asked only when the buffer is another (the console profile: a miss a draw).
  static const plume::RenderBuffer *memexport_known = nullptr;
  static uint64_t memexport_address = 0;
  if (memexport != memexport_known) {
    memexport_known = memexport;
    memexport_address = memexport->getDeviceAddress();
  }
  SetShared(sc, sc.memexport_address, memexport_address);
  plume::Deko3DBindMemExport(cmd, memexport);
  PushSharedConstants(cmd, sc);
  banjo::renut_gpu::CountSharedConstants(false);
#endif

  // The pipeline key and the pipeline: the draw before's when nothing MakeKey reads changed; else from the pipeline
  // cache (FastPipeline) by the render state's epoch and the rest of the key's inputs; else made and looked up.
  const int ab_off = ab;
  bool rs_new = targets_changed;  // the key reads the targets' formats and samples
  if ((fields & (kFieldBools | kFieldDest | kFieldViewPackets)) || !f.rs_valid) {
    u32 rs[20];
    if (pk)
      std::memcpy(rs, pk->rs, sizeof(rs));  // banjo-nx (2026-10-28, host packets): the same state's words
    else
      FastRenderState(dev, sc.booleans, rs);
    if (!f.rs_valid || !SameBytes<80>(rs, f.rs)) {
      std::memcpy(f.rs, rs, sizeof(rs));
      f.rs_valid = true;
      rs_new = true;
    }
  }
  // banjo-nx (2026-10-27): the content's number (A/B 28: a new number at every change, as before).
  // banjo-nx (2026-10-28, host packets): the packet's number for the same words and targets (a number is never given to
  // two contents, so one the table has given this content again since still names it alone).
  if (rs_new)
    f.rs_epoch = ab_off == 28                                     ? g_rs_next_id++ + (uint64_t(1) << 62)
                 : pk_targets && SameBytes<80>(f.rs, pk->rs) ? pk->rs_epoch
                                                                  : RenderStateIdLocked(f.rs, targets);
  const uint64_t prebuild_epoch = PrebuildEpochLocked();
  const u32 misc = call.primitive | u32(geometry.per_instance) << 8 | alpha_test_func << 9 | u32(ab_off) << 16;
  const FetchSpec fetch_spec = FetchSpecWords(sc, *vs, geometry.per_instance, ab_off);  // banjo-nx (banjo_gpu_fetch_spec)
  const ShaderCacheEntry *const ps_entry = ps ? ps->entry : nullptr;
  // banjo-nx (2026-10-27, shadow sort): the key reads the shaders through their cache entries and the declaration
  // through its layout hash (MakeKey, BoolSpecMask), so another shader object of the same contents (N&B makes one per
  // mesh: a new address at 69 % of the shadow draws) keeps the key and the pipeline; compared with the key's own.
  const u32 plain_fetches = SasrtPlainFetchMask(sc, ps_entry, vs->entry);  // sasrt-nx (2026-11-08); vivapinata-nx: vs
  const u32 clip_bit = SasrtClipBit(sc);  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
  const SasrtSpecWords spec_words = SasrtSpecWordsOf(sc, vs->entry, ps_entry, &vs_constants, &targets);  // sasrt-nx (third round)
  const bool key_inputs = f.key_rs_epoch != f.rs_epoch || !f.pipeline || !f.kp || depth_only != f.depth_only ||
                          std::memcmp(f.kp->sasrt_words, spec_words.w, sizeof(spec_words.w)) != 0 ||  // sasrt-nx
                          misc != f.key_misc || f.prebuild_epoch != prebuild_epoch ||
                          f.kp->fetch_pad != (f.kp->pixel_shader ? plain_fetches : 0u) ||
                          (f.kp->reserved & 8u) != clip_bit ||  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
                          ((shaders_changed || (fields & kFieldDecl)) &&
                           (vs->entry != f.kp->vertex_shader || ps_entry != f.kp->pixel_shader ||
                            declaration->layout_hash != f.kp->layout_hash || ab_off == 41)) ||  // A/B 41: as before
                          FetchSpecDiffers(*f.kp, fetch_spec);  // banjo_gpu_fetch_spec; vivapinata-nx (2026-11-12): every draw
  if (key_inputs) {
    // blur-nx (2026-11-18, the Renut host batch's #16, blur_renut_fast_pipeline_two_way / A/B 235): the entry and its
    // neighbour. An entry answers when it was made from this draw's inputs (same_inputs: all that MakeKey reads and the
    // words set after it) in this generation and prebuild epoch (or not provisional); a miss fills the way with this
    // draw's inputs from an earlier epoch, else an empty one, else the one used longer ago. The race had 7,652
    // conflicts a 10 s (27 a frame), each a MakeKey and GetPipelineLocked.
    const u32 fp_index = FastPipelineIndex(f.rs_epoch, vs->entry, ps_entry, declaration->layout_hash, misc);
    const auto same_inputs = [&](const FastPipeline &e) {
      return e.pipeline && e.rs_epoch == f.rs_epoch && e.vs == vs->entry && e.ps == ps_entry &&
             e.layout_hash == declaration->layout_hash && e.misc == misc &&
             e.key.fetch_pad == (e.key.pixel_shader ? plain_fetches : 0u) &&  // sasrt-nx
             (e.key.reserved & 8u) == clip_bit &&  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
             std::memcmp(e.key.sasrt_words, spec_words.w, sizeof(spec_words.w)) == 0 &&  // sasrt-nx (the third round)
             !FetchSpecDiffers(e.key, fetch_spec);  // banjo_gpu_fetch_spec; vivapinata-nx (2026-11-12): every draw
    };
    // banjo-nx (2026-10-28, per-draw night, A/B 63): the prebuild epoch only for a provisional answer.
    const auto answers = [&](const FastPipeline &e) {
      return same_inputs(e) && (e.prebuild_epoch == prebuild_epoch || (!e.provisional && ab_off != 63)) &&
             e.generation == g_fast_pipeline_generation;
    };
    FastPipeline *cp = &g_fast_pipelines[fp_index];
    bool hit = answers(*cp);
    if (!hit && ::renut::gpu::host_batch::On(::renut::gpu::host_batch::kPipelineTwoWay)) {
      FastPipeline &other = g_fast_pipelines[fp_index ^ 1u];
      if (answers(other)) {
        cp = &other;
        hit = true;
      } else if (!same_inputs(*cp) &&
                 (same_inputs(other) || !other.pipeline ||
                  (cp->pipeline && uint8_t(g_fast_pipeline_stamp - other.stamp) > uint8_t(g_fast_pipeline_stamp - cp->stamp)))) {
        cp = &other;
      }
    }
    FastPipeline &c = *cp;
    c.stamp = ++g_fast_pipeline_stamp;
    if (hit) {
      ++g_fast_pipeline_hits;
      f.kp = &c.key;
      f.kp_hot = c.hot;
      f.blend_used = c.blend_used;
      f.pipeline = c.pipeline;
    } else {
      ++g_fast_pipeline_misses;
      ++g_fast_pipeline_miss_why[!c.pipeline                                                     ? 0
                                 : c.rs_epoch != f.rs_epoch || c.vs != vs->entry || c.ps != ps_entry ||
                                           c.layout_hash != declaration->layout_hash || c.misc != misc
                                     ? 1
                                 : c.generation == g_fast_pipeline_generation ? 2
                                                                              : 3];
      // blur-nx (2026-11-18, the Renut host batch's #18, blur_renut_provisional_key_kept / A/B 237): an entry made from
      // this draw's inputs in this generation (a provisional answer of an earlier prebuild epoch) holds the key MakeKey
      // would make again: looked up again with it.
      const bool keep_key = c.generation == g_fast_pipeline_generation && same_inputs(c) &&
                            ::renut::gpu::host_batch::On(::renut::gpu::host_batch::kProvisionalKey);
      if (!keep_key) {
        c.key = MakeKey(device, vs, ps, declaration, targets, call.primitive, geometry.per_instance, sc.booleans,
                        sc.alpha_test_func, fetch_spec);
        c.key.fetch_pad = c.key.pixel_shader ? plain_fetches : 0u;  // sasrt-nx (2026-11-08, sasrt_gpu_plain_fetches)
        c.key.reserved = (c.key.reserved & ~8u) | clip_bit;  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
        std::memcpy(c.key.sasrt_words, spec_words.w, sizeof(spec_words.w));  // sasrt-nx (the third round)
        c.blend_used = BlendConstantUsed(c.key);
        c.hot = KeyHot::Of(c.key);
      }
      f.kp = &c.key;
      f.kp_hot = c.hot;
      f.blend_used = c.blend_used;
      f.pipeline = GetPipelineLocked(c.key, {host_vs, draw_host_ps, &declaration->layout});
      c.provisional = LastPipelineProvisionalLocked();  // banjo-nx (2026-10-28)
      c.rs_epoch = f.rs_epoch;
      c.vs = vs->entry;
      c.ps = ps_entry;
      c.layout_hash = declaration->layout_hash;
      c.misc = misc;
      c.prebuild_epoch = prebuild_epoch;
      c.generation = g_fast_pipeline_generation;
      c.pipeline = f.pipeline;
    }
    f.key_rs_epoch = f.rs_epoch;
    f.key_misc = misc;
    f.prebuild_epoch = prebuild_epoch;
  }
  if (verify) {
    PipelineKey key = MakeKey(device, vs, ps, declaration, targets, call.primitive, geometry.per_instance,
                              sc.booleans, sc.alpha_test_func, fetch_spec);
    key.fetch_pad = key.pixel_shader ? plain_fetches : 0u;  // sasrt-nx (2026-11-08, sasrt_gpu_plain_fetches)
    key.reserved = (key.reserved & ~8u) | clip_bit;  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
    std::memcpy(key.sasrt_words, spec_words.w, sizeof(spec_words.w));  // sasrt-nx (2026-11-08, the third round)
    plume::RenderPipeline *made = GetPipelineLocked(key, {host_vs, draw_host_ps, &declaration->layout});
    if (std::memcmp(&key, f.kp, sizeof(key)) != 0 || made != f.pipeline) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_WARN("banjo_renut_fast_draw_verify: the kept or cached pipeline {} differs from the one made (VS {:016X})",
                  std::memcmp(&key, f.kp, sizeof(key)) != 0 ? "key" : "", vs->hash);
      f.key = key;
      f.kp = &f.key;
      f.kp_hot = KeyHot::Of(key);
      f.blend_used = true;
      f.pipeline = made;
    }
  }
  plume::RenderPipeline *pipeline = f.pipeline;
  if (!pipeline)
    return -1;
#if defined(PLUME_DEKO3D)
  // banjo-nx (2026-10-27, deko3d recording): a pipeline to bind: its draw-time lines, while the targets, the refresh
  // and the viewport go first (setPipeline, bindGraphicsBuffers and bindTextures read them).
  if (g_bound.pipeline != pipeline)
    plume::Deko3DPrefetchPipeline(pipeline);
#endif
  // Round 4: the targets bound again only when something BindTargets reads could have changed: other targets, another
  // framebuffer bound since (copies, resolves and new lists unbind it), a layout change (a target's transitions), a
  // framebuffer retired, a depth target handed over, or the GPU profile's per-target marks.
  // banjo-nx (2026-10-28, banjo_deko3d_depth_handover_readonly): a handed-over depth target bound again only for a draw
  // that writes its depth or stencil (BindTargets then draws the resolve first).
  const u32 fast_dc = f.kp_hot.depth_control;
  const bool fast_depth_writes = ((fast_dc & 2) && (fast_dc & 4)) ||
                                 ((fast_dc & 1) && ((f.kp_hot.stencil_ref_mask >> 16) & 0xFF) != 0);
  // Port 179 (from fnxf_xiii; banjo_renut_edram_carry, off): before the bind test, so every draw marks its targets this
  // frame; a carry draws with its own framebuffer and resets g_bound, so the targets are bound again below.
  if (EdramCarryOn()) [[unlikely]]
    CarryEdramBeforeDrawLocked(targets);
  if (targets_changed || !g_bound.framebuffer || g_bound.framebuffer != f.bound_framebuffer ||
      f.bind_layout_epoch != g_layout_epoch || f.framebuffer_epoch != FramebufferEpochLocked() ||
      (targets.depth && targets.depth->handed && (fast_depth_writes || !banjo::renut_gpu::DepthHandOverReadOnlyOn())) ||
      // kameo-nx (2026-11-03, banjo_deko3d_depth_read_layout): a draw that writes a depth target left in the read layout.
      (fast_depth_writes && targets.depth && targets.depth->layout != plume::RenderTextureLayout::DEPTH_WRITE) ||
      (gpu_profile::Timed() && !banjo::renut_gpu::DiagGpuShadersOn())) {
    // blur-nx (2026-11-11, port 76; its key 220: blur_renut_edram_inherit): what other views wrote over a target this
    // draw keeps in part.
    InheritEdramBeforeDrawLocked(targets, f.kp_hot.color_mask, f.kp ? f.kp->blend_control : nullptr);
    if (!BindTargets(targets, fast_depth_writes))
      return -1;
    f.bound_framebuffer = g_bound.framebuffer;
    f.bind_layout_epoch = g_layout_epoch;
    f.framebuffer_epoch = FramebufferEpochLocked();
  }
#if defined(PLUME_DEKO3D)
  if (plume::Deko3DRefreshPipeline(pipeline))
    g_bound.pipeline = nullptr;
  if (g_bound.pipeline != pipeline)
    plume::Deko3DPrefetchPipelineShaders(pipeline);  // banjo-nx (2026-10-27): while the viewport and layout go first
#endif
  SetViewport(cmd, f.viewport.viewport, f.viewport.scissor, targets.color_count);
  if (!g_bound.layout) {
    cmd->setGraphicsPipelineLayout(s.draw_layout.get());
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 1);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 2);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 3);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 4);
    g_bound.layout = true;
    g_bound.pipeline = nullptr;
    g_bound.pushed_ubos = false;  // banjo-nx (2026-10-27)
    std::fill(std::begin(g_bound.root_set), std::end(g_bound.root_set), false);
    std::fill(std::begin(g_bound.constant_sets), std::end(g_bound.constant_sets), nullptr);
  }
  if (g_bound.pipeline != pipeline) {
    if (!g_bound.pipeline)
      g_bound.depth_bias_set = false;  // rr6-nx: after an internal pass, whose pipeline set its own bias
    {
      ::renut::gpu::census::Scope pipeline_scope(::renut::gpu::census::kSetPipeline);  // blur-nx (2026-11-17)
      cmd->setPipeline(pipeline);
    }
    g_bound.pipeline = pipeline;
  }
  SetDrawDepthBias(cmd, dev);  // rr6-nx (2026-11-03, rr6_depth_bias)
  float blend_constant[4];
  if (f.blend_used && DrawBlendConstant(dev, *f.kp, blend_constant) &&
      !(g_bound.blend_constant_set && SameBytes<sizeof(blend_constant)>(blend_constant, g_bound.blend_constant))) {
    if (SetBlendConstant(cmd, blend_constant)) {
      std::memcpy(g_bound.blend_constant, blend_constant, sizeof(blend_constant));
      g_bound.blend_constant_set = true;
    }
  }
#if defined(PLUME_DEKO3D)
  if (f.kp_hot.point_sprite || !banjo::renut_gpu::PushPointOnly()) {
    const DrawPushConstants push{0, 0, 0};
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  }
  // banjo-nx (2026-10-27, render thread night): once while they stay (plume found them bound at every draw after the
  // first: three calls a draw).
  if (!g_bound.pushed_ubos) {
    plume::Deko3DBindUniform(cmd, 0, g_pushed_buffers.address, kFloatConstantsUboBytes);
    plume::Deko3DBindUniform(cmd, 1, g_pushed_buffers.address + kPushedFloatStride, kFloatConstantsUboBytes);
    plume::Deko3DBindUniform(cmd, 2, g_pushed_buffers.address + kPushedSharedOffset, u32(sizeof(SharedConstants)));
    std::fill(std::begin(g_bound.constant_sets), std::end(g_bound.constant_sets), nullptr);
    g_bound.pushed_ubos = true;
  }
  if (!g_bound.shared_words_set) {
    plume::Deko3DSetSharedWords(cmd, &g_bound.pushed_shared, u32(sizeof(SharedConstants)));
    g_bound.shared_words_set = true;
  }
#endif
  if (whole ? !BindWholeStreams(declaration->layout, whole_copies, base_draw ? 0u : geometry.whole_base)
            : !BindStreams(device, call, *vs, shader_strides, declaration->layout, declaration->layout_hash, geometry,
                           GpuStreams{}, nullptr))
    return -1;
  if (targets_changed || (key_inputs && banjo::renut_gpu::DiagGpuShadersOn()) || SasrtProfileVsOn())
    f.profile_label = UINT32_MAX;  // the label reads the targets (and with the shaders' breakdown the shader and key)
  if (gpu_profile::Timed()) {
    const bool by_shader = banjo::renut_gpu::DiagGpuShadersOn();
    if (f.profile_label == UINT32_MAX)
      f.profile_label = by_shader ? ShaderProfileLabel(targets, ps, *f.kp, vs) : TargetsProfileLabel(targets);
    if (by_shader)
      gpu_profile::Mark(s, f.profile_label);
    gpu_profile::CountDraw(s, f.profile_label);
  }
  for (u32 i = 0; i < targets.color_count && i < f.kp_hot.color_count; ++i)
    if ((f.kp_hot.color_mask >> (4 * i)) & 0xF) {
      ++targets.colors[i]->writes;
      targets.colors[i]->write_seq = ++g_surface_write_seq;  // sasrt-nx: the EDRAM depth restore
      targets.colors[i]->blur_draw_seq = g_surface_write_seq;  // blur-nx (2026-11-10, port 76; its key 216)
      g_blur_color_write_last = g_surface_write_seq;  // blur-nx (2026-11-11, port 76; its key 220)
    }
  if (targets.depth) {
    const u32 dc = f.kp_hot.depth_control;
    if (((dc & 2) && (dc & 4)) || ((dc & 1) && ((f.kp_hot.stencil_ref_mask >> 16) & 0xFF) != 0)) {
      ++targets.depth->writes;
      targets.depth->write_seq = ++g_surface_write_seq;  // sasrt-nx: the EDRAM depth restore
    }
    if (dc & 1) {  // sasrt-nx's SasrtWarnStencilOnD32f inside it (rr6-nx, 2026-11-09)
      const bool first_stencil = !targets.depth->stencil_used;  // sasrt-nx (2026-11-11, port 75; renut_diag_formats)
      targets.depth->stencil_used = true;
      SasrtWarnStencilOnD32f(targets.depth, false, first_stencil);
    }
  }
  ++s.list_draws;  // Video::NoteDrawLocked
  if (vs->entry && vs->entry->textureFetchMask)
    VertexTextureBarrierLocked();  // port 161 (banjo_deko3d_vertex_texture_barrier)
  SetSampleMaskLocked(cmd, device, targets);  // port 166 (banjo_deko3d_sample_mask)
  if (geometry.indexed) {
    // banjo-nx (2026-10-27, render thread night, A/B 38): a whole index copy is a piece of a pool chunk: the chunk bound at
    // its start and the copy's place given as the first index, so the draws of every copy in one chunk keep one binding
    // (each draw's index buffer is another, 99 % of the draws: an index buffer bind each). The same indices read.
    plume::RenderIndexBufferView index_view = geometry.index_view;
    u32 first_index = whole ? geometry.first_index : 0;
    if (whole && ab != 38) {
      // banjo-nx (2026-10-28, ARM audit): shifts (the size is 2 or 4; the division was a udiv at every such draw).
      const u32 index_shift = index_view.format == plume::RenderFormat::R32_UINT ? 2u : 1u;
      const uint64_t index_offset = index_view.buffer.offset;
      if ((index_offset & ((uint64_t(1) << index_shift) - 1)) == 0 && (index_offset >> index_shift) < (uint64_t(1) << 31)) {
        first_index += u32(index_offset >> index_shift);
        index_view = plume::RenderIndexBufferView(plume::RenderBufferReference(index_view.buffer.ref, 0), UINT32_MAX,
                                                  index_view.format);
      }
    }
    SetIndexBuffer(cmd, index_view);
    ::renut::gpu::census::Scope draw_scope(::renut::gpu::census::kHostDraw);  // blur-nx (2026-11-17)
    cmd->drawIndexedInstanced(geometry.index_count, geometry.instance_count, first_index,
                              base_draw ? int32_t(geometry.whole_base) : 0, 0);
  } else {
    ::renut::gpu::census::Scope draw_scope(::renut::gpu::census::kHostDraw);  // blur-nx (2026-11-17)
    cmd->drawInstanced(geometry.vertex_count, geometry.instance_count, base_draw ? geometry.whole_base : 0u, 0);
  }
  f.seq = chain.seq;
  if (targets_changed) {
    f.targets = targets;
    f.surface_epoch = SurfaceEpochLocked();
  }
  f.vs = vs;
  f.ps = device_ps;
  f.host_vs = host_vs;
  f.host_ps = host_ps;
  f.declaration = declaration;
  f.shader_strides = shader_strides;
  f.declaration_from_va = declaration_from_va;
  f.depth_only = depth_only;
  f.alpha_test_func = alpha_test_func;
  f.shaders_fast = true;  // they passed the shaders' cases (now or when they came)
  f.draw_texture_mask = texture_mask;
  // banjo-nx (2026-10-28, host packets): this state's answers into its slot when it holds none, or ones made with other
  // surfaces, tiles or predication (taken whole or not at all, so a packet is one draw's answers). A packet taken whole
  // and still current is left as it is: nothing written at a steady state's draws.
  if (pk_slot) {
    const bool current = pk_slot->key == chain.packet_key && pk_slot->epoch == g_packet_epoch &&
                         pk_slot->surface_epoch == SurfaceEpochLocked() &&
                         pk_slot->viewport_tiles == u32(g_tiles.size()) &&
                         pk_slot->viewport_predication == g_replay_predication;
    if (!current) {
      HostPacket &e = *pk_slot;
      e.key = chain.packet_key;
      e.epoch = g_packet_epoch;
      e.vs = vs;
      e.ps = device_ps;
      e.host_vs = host_vs;
      e.host_ps = host_ps;
      e.declaration = declaration;
      e.shader_strides = shader_strides;
      e.vs_entry = vs->entry;
      e.ps_entry = device_ps ? device_ps->entry : nullptr;
      e.surface_epoch = SurfaceEpochLocked();
      e.rs_epoch = f.rs_epoch;
      e.viewport_tiles = u32(g_tiles.size());
      e.viewport_predication = g_replay_predication;
      e.texture_mask = texture_mask;
      e.alpha_test_func = alpha_test_func;
      e.alpha_threshold = sc.alpha_threshold;
      e.declaration_from_va = declaration_from_va;
      e.depth_only = depth_only;
      std::memcpy(e.booleans, sc.booleans, sizeof(e.booleans));
      std::memcpy(e.rs, f.rs, sizeof(e.rs));
      e.targets = targets;
      e.viewport = f.viewport;
      ++g_packet_made;
    }
  }
  if (exports_pending) {
    f.export_generation = g_export_generation;
    f.export_none = true;
    f.export_streams_ok = !call.up_vertices;  // stream 0 was not checked for a Draw*UP draw
    f.export_fetch_ok = true;
    f.export_index_ok = call.indexed || (f.export_index_ok && export_generation_same && !(fields & kFieldIndex));
    f.export_textures_ok = true;
  }
  f.sasrt_write_seq = g_surface_write_seq;  // sasrt-nx (2026-10-29): the EDRAM depth restore
  ++g_fast_draws;
  ++g_sasrt_order_draws;  // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order)
  g_last_draw_kind = DrawKindOf(targets, ps);  // banjo-nx (2026-10-27): the census's
  g_last_draw_fields = fields;
  g_last_draw_fast = true;
  return 1;
}

// After a draw of DrawStateLocked's own: what the next draw's FastDrawLocked starts from, when it can.
// (DrawStateLocked's own texture words and transitions are not taken: the next fast draw fills its slots.)
void StoreFastStateLocked(const DrawCall &call, ShaderRecord *vs, ShaderRecord *device_ps, bool depth_only,
                          const VertexDeclarationRecord *declaration, const u32 *shader_strides,
                          bool declaration_from_va, const Targets &targets, const ViewportState &viewport,
                          const PipelineKey &key, plume::RenderPipeline *pipeline, u32 texture_mask,
                          const TextureFetch *fetches, u32 alpha_test_func, bool per_instance, bool usable) {
  FastState &f = g_fast;
  ++g_full_draws;
  ++g_sasrt_order_draws;  // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order)
  f.fetch_kept = false;  // banjo-nx (2026-10-27): DrawStateLocked bound its own
  g_last_draw_kind = DrawKindOf(targets, depth_only ? nullptr : device_ps);  // banjo-nx (2026-10-27): the census's
  g_last_draw_fields = g_chain_hint.fields;
  g_last_draw_fast = false;
  f.valid = usable;
  if (!usable)
    return;
  f.seq = g_chain_hint.seq;
  f.vs = vs;
  f.ps = device_ps;
  f.host_vs = GetHostShader(vs);
  f.host_ps = device_ps ? GetHostShader(device_ps) : nullptr;
  f.declaration = declaration;
  f.shader_strides = shader_strides;
  f.declaration_from_va = declaration_from_va;
  {
    bool fast = !DrawSummaryOf(*vs).exports && !(device_ps && DrawSummaryOf(*device_ps).exports) &&
                !(device_ps && DrawSummaryOf(*device_ps).composite) && !IsSkinOnceVs(vs->hash);
    if (fast && device_ps) {
      const uint64_t h = device_ps->hash;
      fast = !(h == 0x2F36770769087F99ull || h == 0xC92BA80672FB9774ull || h == 0xB3C10F3D80EF61E2ull ||
               h == 0xD40EBF0336F89333ull);
    }
    f.shaders_fast = fast;
    f.draw_texture_mask = texture_mask;  // the draw's own (after its depth-only answer)
  }
  f.targets = targets;
  f.surface_epoch = SurfaceEpochLocked();
  f.sasrt_write_seq = g_surface_write_seq;  // sasrt-nx (2026-10-29): the EDRAM depth restore (FastDrawLocked)
  f.viewport = viewport;
  f.viewport_tiles = g_tiles.size();
  f.viewport_predication = g_replay_predication;
  f.depth_only = depth_only;
  f.alpha_test_func = alpha_test_func;
  f.textures_valid = false;  // the next draw fills its slots (the words and transitions are DrawStateLocked's)
  f.texture_mask = texture_mask;
  (void)fetches;
  f.key = key;
  f.kp = &f.key;  // banjo-nx (2026-10-27)
  f.kp_hot = KeyHot::Of(key);  // banjo-nx (2026-10-28)
  f.blend_used = true;
  f.rs_valid = false;  // taken again at the next fast draw (a new epoch, unless the words are the same)
  f.key_rs_epoch = 0;  // the next fast draw takes its key from the cache or makes it
  f.key_misc = call.primitive | u32(per_instance) << 8 | alpha_test_func << 9 |
               u32(banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed)) << 16;
  f.pipeline = pipeline;
  f.pipeline_lookups = PipelineLookupsLocked();
  f.prebuild_epoch = PrebuildEpochLocked();
  f.export_none = false;
  f.export_streams_ok = f.export_fetch_ok = f.export_index_ok = f.export_textures_ok = false;
  f.bound_framebuffer = nullptr;  // BindTargets again at the next fast draw
  f.layout_epoch = 0;
  f.profile_label = UINT32_MAX;
}

}  // namespace

// banjo-nx (2026-10-27, banjo_renut_diag_draw_kinds).
bool DrawKindsCensusOn() {
  static const bool on = REXCVAR_GET(banjo_renut_diag_draw_kinds);
  return on;
}
void NoteDrawKind(uint64_t ticks) {
  DrawKindStats &k = g_kind_stats[g_last_draw_kind];
  ++k.draws;
  k.ticks += ticks;
  k.fast += g_last_draw_fast;
  if (g_last_draw_kind != g_prev_draw_kind)
    ++k.runs;
  g_prev_draw_kind = g_last_draw_kind;
  const u32 f = g_last_draw_fields;
  if (f == kFieldsAll) {
    ++k.chain_breaks;
  } else if (!f) {
    ++k.nothing;
  } else {
    for (u32 bits = f; bits; bits &= bits - 1)
      ++k.fields[std::countr_zero(bits)];
  }
}
bool MergeDrawsOn() {  // the key only: Replay latches it, and tests A/B 33 at each draw
  static const bool on = REXCVAR_GET(banjo_renut_merge_draws);
  return on;
}
bool MergeableShadersLocked(const u8 *device_state) {
  const GuestDevice device(device_state);
  if (!device.valid())
    return false;
  ShaderRecord *vs = FindShaderLocked(device.U32(d3d::kDeviceOffsetVertexShader), RecordKind::VertexShader);
  if (!vs)
    return false;
  const auto &v = DrawSummaryOf(*vs);
  if (v.exports || v.skin_once || IsFurVs(vs->hash))
    return false;
  if (ShaderRecord *ps = FindShaderLocked(device.U32(d3d::kDeviceOffsetPixelShader), RecordKind::PixelShader)) {
    const auto &p = DrawSummaryOf(*ps);
    const uint64_t h = ps->hash;
    if (p.exports || p.composite || h == 0x2F36770769087F99ull || h == 0xC92BA80672FB9774ull ||
        h == 0xB3C10F3D80EF61E2ull || h == 0xD40EBF0336F89333ull)
      return false;
  }
  return true;
}
bool ShaderSortableByHash(uint64_t hash, const ShaderCacheEntry *entry, bool pixel) {
  if (!entry)
    return false;
  for (u32 word = 0; word < 8; ++word)
    if (entry->memExportConstants[word])
      return false;
  if (!pixel)
    return !IsSkinOnceVs(hash) && !IsFurVs(hash);
  return !(CompositePs(hash) || hash == 0x2F36770769087F99ull || hash == 0xC92BA80672FB9774ull ||
           hash == 0xB3C10F3D80EF61E2ull || hash == 0xD40EBF0336F89333ull);
}
bool SortableShadersLocked(u32 vs_va, u32 ps_va) {
  ShaderRecord *vs = FindShaderLocked(vs_va, RecordKind::VertexShader);
  if (!vs)
    return false;
  const auto &v = DrawSummaryOf(*vs);
  if (v.exports || v.skin_once || IsFurVs(vs->hash))
    return false;
  if (!ps_va)
    return true;
  ShaderRecord *ps = FindShaderLocked(ps_va, RecordKind::PixelShader);
  if (!ps)
    return false;
  const auto &p = DrawSummaryOf(*ps);
  const uint64_t h = ps->hash;
  return !(p.exports || p.composite || h == 0x2F36770769087F99ull || h == 0xC92BA80672FB9774ull ||
           h == 0xB3C10F3D80EF61E2ull || h == 0xD40EBF0336F89333ull);
}
uint64_t g_merged_draws = 0, g_merged_ops = 0;  // the 10 s line's (WholeBuffersReport)
void CountMergedDraws(uint64_t draws, uint64_t ops) {
  g_merged_draws += draws;
  g_merged_ops += ops;
}
void NoteDrawMerge(bool same_state, bool contiguous) {
  g_merge_same[g_prev_draw_kind < 3 ? g_prev_draw_kind : 2] += same_state;
  g_merge_contiguous[g_prev_draw_kind < 3 ? g_prev_draw_kind : 2] += contiguous;
}
// halo3-nx (2026-11-03): draws that never reached the GPU, by why. These exits were silent: three of four draws of Halo 3's
// first mission left here and nothing said so.
enum DropReason : uint32_t {
  kDropShaderRecord, kDropHostShader, kDropTargets, kDropGeometry, kDropStreams, kDropAllocation, kDropPipeline, kDropCount
};
uint64_t g_dropped_draws[kDropCount] = {};
std::string DroppedDrawsReport() {
  uint64_t all = 0;
  for (const uint64_t n : g_dropped_draws)
    all += n;
  if (!all)
    return {};
  std::string out = fmt::format("DRAWS NOT DRAWN: {} (no shader record {}, no host shader {}, no targets {}, geometry {}, "
                                "exported streams {}, allocation {}, pipeline or framebuffer {})",
                                all, g_dropped_draws[kDropShaderRecord], g_dropped_draws[kDropHostShader],
                                g_dropped_draws[kDropTargets], g_dropped_draws[kDropGeometry],
                                g_dropped_draws[kDropStreams], g_dropped_draws[kDropAllocation],
                                g_dropped_draws[kDropPipeline]);
  std::fill(std::begin(g_dropped_draws), std::end(g_dropped_draws), uint64_t(0));
  return out;
}

std::string DrawKindsReport() {
  uint64_t all = 0;
  for (const DrawKindStats &k : g_kind_stats)
    all += k.draws;
  if (!all)
    return {};
#if defined(__aarch64__)
  uint64_t freq;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  const double per_us = double(freq) / 1e6;
#else
  const double per_us = 3000.0;  // the PC's TSC, roughly
#endif
  static constexpr const char *kKinds[3] = {"shadow (depth target, no pixel shader)", "other without a pixel shader",
                                            "colour"};
  static constexpr const char *kNames[18] = {"fetch", "bools", "dest", "view packets", "misc", "29C0", "unread",
                                             "private", "decl", "tail", "index", "targets", "streams", "tex words",
                                             "scissor", "shaders", "predication", "vertex fetch"};
  std::string out = "draws by kind (banjo_renut_diag_draw_kinds):";
  for (u32 i = 0; i < 3; ++i) {
    DrawKindStats &k = g_kind_stats[i];
    if (!k.draws)
      continue;
    const double d = double(k.draws);
    out += fmt::format(" {}: {} ({:.1f} %), {:.2f} us a draw, {:.1f} % fast, runs of {:.1f}; none changed {:.1f} %, "
                       "chain breaks {:.1f} %; the same state and constants as the draw before {:.1f} %, and its next "
                       "indices {:.1f} % (one draw with it); changed:",
                       kKinds[i], k.draws, 100.0 * d / double(all), double(k.ticks) / per_us / d,
                       100.0 * double(k.fast) / d, k.runs ? d / double(k.runs) : 0.0, 100.0 * double(k.nothing) / d,
                       100.0 * double(k.chain_breaks) / d, 100.0 * double(g_merge_same[i]) / d,
                       100.0 * double(g_merge_contiguous[i]) / d);
    g_merge_same[i] = g_merge_contiguous[i] = 0;
    for (u32 b = 0; b < 18; ++b)
      if (k.fields[b])
        out += fmt::format(" {} {:.0f} %", kNames[b], 100.0 * double(k.fields[b]) / d);
    out += ';';
    k = {};
  }
  return out;
}

// banjo-nx (2026-10-27): FillTextures' answers (file scope: the counters are the anonymous namespace's).
std::string TextureAnswersReport() { return TextureAnswersReportLocal(); }

// banjo-nx (2026-10-26): the 10 s line.
// banjo-nx (2026-10-28, banjo_renut_diag_terrain): see the key. Render thread only (DrawStateLocked: tessellated draws
// never take the fast path).
struct TerrainDiag {
  uint64_t frame = UINT64_MAX;
  std::vector<uint64_t> seen;  // this frame's input keys
  uint64_t last[6] = {};
  uint64_t frames = 0, draws = 0, repeats = 0, vertices = 0, max_draws = 0, frame_draws = 0;
  uint64_t differ[6] = {};  // against the terrain draw before: geometry, VS constants, VS textures, fetch data, shared
                            // words, vertex shader
  // Within a frame, against the frame's first terrain draw: its component hashes, its shared words v[32-33] and
  // v[65-81] (76 words) and its VS constant registers' hashes; how many later draws, which components, words and
  // registers differed.
  uint64_t first[6] = {};
  u32 first_words[76] = {};
  uint64_t first_regs[256] = {};
  uint64_t later = 0, later_differ[6] = {};
  uint32_t word_differ[76] = {};
  uint32_t reg_differ[256] = {};
};
TerrainDiag g_terrain_diag;
bool TerrainDiagOn() {
  static const bool on = REXCVAR_GET(banjo_renut_diag_terrain);
  return on;
}
inline bool IsTerrainVs(uint64_t hash) { return hash == 0xF09B139EE9188255ull || hash == 0x8EDDB32357201CD0ull; }
[[gnu::cold, gnu::noinline]] void NoteTerrainDraw(const ShaderRecord &vs, const DrawCall &call, const Geometry &g,
                                                  const FloatConstants &vsc, const TextureFetch *fetches,
                                                  const SharedConstants &sc) {
  using banjo::renut_gpu::ContentHash64;
  TerrainDiag &d = g_terrain_diag;
  if (d.frame != g_stream_cache_frame) {
    if (d.frame != UINT64_MAX) {
      ++d.frames;
      d.max_draws = std::max(d.max_draws, d.frame_draws);
    }
    d.frame = g_stream_cache_frame;
    d.seen.clear();
    d.frame_draws = 0;
  }
  const auto mix = [](uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull; };
  uint64_t c[6] = {};
  const u32 geo[10] = {u32(call.tessellation), u32(call.indexed), call.count, call.start_index, call.start_vertex,
                       u32(call.base_vertex), g.instance_count, g.vertex_count, g.index_count, g.first_index};
  c[0] = ContentHash64(geo, sizeof(geo));
  static constexpr u32 kAll[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  const u32 *used = vs.entry ? vs.entry->floatConstants : kAll;
  uint64_t regs[256] = {};
  for (u32 w = 0; w < 8; ++w)
    for (u32 bits = used[w]; bits; bits &= bits - 1) {
      const u32 r = w * 32 + u32(std::countr_zero(bits));
      regs[r] = ContentHash64(vsc.Register(r), 16);
      c[1] = mix(c[1], regs[r]);
    }
  for (u32 bits = vs.entry ? vs.entry->textureFetchMask : 0u; bits; bits &= bits - 1)
    c[2] = mix(c[2], ContentHash64(fetches[std::countr_zero(bits)].dword, sizeof(fetches[0].dword)));
  for (u32 i = 0; i < g_fetch_record.count && i < 32; ++i) {
    const FetchRecordCopy &f = g_fetch_record.copies[i];
    const u32 head[3] = {f.index, f.size, f.endian};
    c[3] = mix(c[3], ContentHash64(head, sizeof(head)));
    if (f.src && f.size)
      c[3] = mix(c[3], ContentHash64(f.src, f.size));
  }
  const u8 *s = reinterpret_cast<const u8 *>(&sc);
  c[4] = mix(ContentHash64(s + 32 * 16, 2 * 16), ContentHash64(s + 65 * 16, 17 * 16));  // v[32-33], v[65-81]
  c[5] = vs.hash;
  uint64_t key = 0;
  for (uint64_t v : c)
    key = mix(key, v);
  ++d.draws;
  ++d.frame_draws;
  d.vertices += uint64_t(g.instance_count) * (g.index_count ? g.index_count : g.vertex_count);
  if (std::find(d.seen.begin(), d.seen.end(), key) != d.seen.end())
    ++d.repeats;
  else
    d.seen.push_back(key);
  if (d.draws > 1)
    for (u32 i = 0; i < 6; ++i)
      d.differ[i] += c[i] != d.last[i];
  std::copy(std::begin(c), std::end(c), d.last);
  u32 words[76];
  std::memcpy(words, s + 32 * 16, 2 * 16);
  std::memcpy(words + 8, s + 65 * 16, 17 * 16);
  if (d.frame_draws == 1) {
    std::copy(std::begin(c), std::end(c), d.first);
    std::memcpy(d.first_words, words, sizeof(words));
    std::copy(std::begin(regs), std::end(regs), d.first_regs);
  } else {
    ++d.later;
    for (u32 i = 0; i < 6; ++i)
      d.later_differ[i] += c[i] != d.first[i];
    for (u32 i = 0; i < 76; ++i)
      d.word_differ[i] += words[i] != d.first_words[i];
    for (u32 r = 0; r < 256; ++r)
      d.reg_differ[r] += regs[r] != d.first_regs[r];
  }
}

std::string FastDrawReport() {
  if (!g_fast_draws && !g_full_draws)
    return {};
  static constexpr const char *kWhy[kFastWhyCount] = {"no state", "chain break", "call kind", "targets/predication",
                                                      "surfaces", "diagnostics", "shaders", "declaration",
                                                      "own exports", "fur/skin/composite", "range geometry",
                                                      "export read", "no pushed constants", "smaller depth"};
  std::string out = fmt::format("fast draws (banjo_renut_fast_draw): {} fast, {} full ({:.1f} % fast); pipeline "
                                "cache {} hits, {} made ({} empty, {} conflicts, {} provisional, {} invalidated; render "
                                "states: {} found by content, {} new); to the full path because:",
                                g_fast_draws, g_full_draws,
                                100.0 * double(g_fast_draws) / double(g_fast_draws + g_full_draws),
                                g_fast_pipeline_hits, g_fast_pipeline_misses, g_fast_pipeline_miss_why[0],
                                g_fast_pipeline_miss_why[1], g_fast_pipeline_miss_why[2], g_fast_pipeline_miss_why[3],
                                g_rs_id_hits, g_rs_id_new);
  g_fast_pipeline_hits = g_fast_pipeline_misses = 0;
  std::fill(std::begin(g_fast_pipeline_miss_why), std::end(g_fast_pipeline_miss_why), 0);
#if defined(PLUME_DEKO3D)  // banjo-nx (2026-10-29): the pushes are deko3d's (the PC build)
  if (DiagPushOn()) {  // banjo-nx (2026-10-28, diagnostic)
    out += " [push diag: shared lines";
    for (u32 l = 0; l < 22; ++l)
      out += fmt::format(" {}", g_diag_push_lines[l]);
    out += fmt::format("; float KB: VS {:.1f}, PS {:.1f}, VS of draws without a PS {:.1f} (their read groups {:.1f} KB)]",
                       double(g_diag_push_float[0]) / 1024.0, double(g_diag_push_float[1]) / 1024.0,
                       double(g_diag_push_float[2]) / 1024.0, double(g_diag_push_float[3]) / 1024.0);
    std::fill(std::begin(g_diag_push_lines), std::end(g_diag_push_lines), 0);
    std::fill(std::begin(g_diag_push_float), std::end(g_diag_push_float), 0);
  }
#endif
  g_rs_id_hits = g_rs_id_new = 0;
  for (u32 i = 0; i < kFastWhyCount; ++i) {
    if (g_fast_why[i])
      out += fmt::format(" {} {},", kWhy[i], g_fast_why[i]);
    g_fast_why[i] = 0;
  }
  g_fast_draws = g_full_draws = 0;
  if (g_fast_record_drops)  // banjo-nx (2026-10-28)
    out += fmt::format(" records dropped {} (the fast state kept unless it held them),", std::exchange(g_fast_record_drops, 0));
  if (TerrainDiagOn() && g_terrain_diag.draws) {  // banjo-nx (2026-10-28, banjo_renut_diag_terrain)
    TerrainDiag &d = g_terrain_diag;
    out += fmt::format(" terrain (banjo_renut_diag_terrain): {} draws in {} frames (at most {} a frame), {:.0f} vertices "
                       "a draw, {} with the vertex inputs of an earlier draw of their frame; against the terrain draw "
                       "before, differing: geometry {}, VS constants {}, VS textures {}, fetch data {}, shared words {}, "
                       "vertex shader {},",
                       d.draws, d.frames, d.max_draws, double(d.vertices) / double(d.draws), d.repeats, d.differ[0],
                       d.differ[1], d.differ[2], d.differ[3], d.differ[4], d.differ[5]);
    // Within a frame, against its first terrain draw: the components, then the shared words (v[i].c) and the VS
    // constant registers (cN) that differed, the most often first.
    out += fmt::format(" within a frame, {} later draws against the frame's first: geometry {}, VS constants {}, VS "
                       "textures {}, fetch data {}, shared words {}, vertex shader {}; shared words differing:",
                       d.later, d.later_differ[0], d.later_differ[1], d.later_differ[2], d.later_differ[3],
                       d.later_differ[4], d.later_differ[5]);
    static const char kComp[] = "xyzw";
    for (u32 n = 0; n < 8; ++n) {
      u32 best = 0;
      for (u32 i = 1; i < 76; ++i)
        best = d.word_differ[i] > d.word_differ[best] ? i : best;
      if (!d.word_differ[best])
        break;
      const u32 vec = best < 8 ? 32 + best / 4 : 65 + (best - 8) / 4;
      out += fmt::format(" v[{}].{} {}", vec, kComp[best % 4], d.word_differ[best]);
      d.word_differ[best] = 0;
    }
    out += "; VS constants differing:";
    for (u32 n = 0; n < 8; ++n) {
      u32 best = 0;
      for (u32 r = 1; r < 256; ++r)
        best = d.reg_differ[r] > d.reg_differ[best] ? r : best;
      if (!d.reg_differ[best])
        break;
      out += fmt::format(" c{} {}", best, d.reg_differ[best]);
      d.reg_differ[best] = 0;
    }
    out += ",";
    d.draws = d.repeats = d.vertices = d.frames = d.max_draws = d.later = 0;
    std::fill(std::begin(d.differ), std::end(d.differ), 0);
    std::fill(std::begin(d.later_differ), std::end(d.later_differ), 0);
    std::fill(std::begin(d.word_differ), std::end(d.word_differ), 0);
    std::fill(std::begin(d.reg_differ), std::end(d.reg_differ), 0);
  }
  if (g_clears_whole)  // banjo-nx (2026-10-28, banjo_renut_clear_repeat)
    out += fmt::format(" whole clears {}, of a target holding that clear already (not sent) {},",
                       std::exchange(g_clears_whole, 0), std::exchange(g_clear_repeats, 0));
  if (g_lazy_mips_deferred || g_lazy_mips_made)  // banjo-nx (2026-10-28, banjo_renut_lazy_resolve_mips)
    out += fmt::format(" resolve host levels: {} left for their first read, {} made at it (the rest never read),",
                       std::exchange(g_lazy_mips_deferred, 0), std::exchange(g_lazy_mips_made, 0));
#if defined(PLUME_DEKO3D)
  if (g_const_compared) {  // banjo-nx (2026-10-28)
    const uint64_t compared = std::exchange(g_const_compared, 0), same = std::exchange(g_const_same, 0);
    out += fmt::format(" written constant groups compared with the pushed {}, the same {:.1f} %,", compared,
                       100.0 * double(same) / double(compared));
  }
#endif
  if (HostPacketsOn())  // banjo-nx (2026-10-28, host packets)
    out += fmt::format(" host packets (banjo_renut_host_packets): {} found, {} not, {} stale, {} made,",
                       std::exchange(g_packet_hits, 0), std::exchange(g_packet_misses, 0),
                       std::exchange(g_packet_stale, 0), std::exchange(g_packet_made, 0));
  return out;
}

// banjo-nx (2026-10-26, banjo_renut_diag_fast_census): see the cvar. Diagnostics only.
struct FastCensus {
  uint64_t draws = 0, breaks = 0, replayed = 0;
  uint64_t bits[18] = {};
  uint64_t tess = 0, up = 0, points = 0, exports = 0, pending = 0, skin = 0, fur = 0;
  uint64_t vs_groups = 0, ps_groups = 0, no_change = 0;
  std::unordered_map<u32, uint64_t> combos;
  uint64_t last_seq = 0;
} g_fast_census;
// blur-nx (2026-11-10, port 76; the Ryujinx crashes of its emu2 / emu3: FastCensusDraw read a pointer made of log text):
// with a queued back end (deko3d) the draws reach FastCensusDraw on the render thread while FastCensusReport, on the
// game's frame thread (StatsGameFrame), walks and replaces the same map. Both now hold this. Diagnostics only (the census
// key, banjo_renut_diag_fast_census).
std::mutex g_fast_census_mutex;

void FastCensusDraw(const DrawCall &call, const ShaderRecord &vs, const ShaderRecord *ps, u32 export_count,
                    const ReplayConstants *replay) {
  std::lock_guard census_lock(g_fast_census_mutex);
  FastCensus &c = g_fast_census;
  ++c.draws;
  const ChainHint &h = g_chain_hint;
  const bool chained = h.seq && h.seq == c.last_seq + 1;
  c.last_seq = h.seq;
  c.breaks += !chained;
  c.replayed += replay != nullptr;
  const u32 fields = chained ? h.fields : u32(kFieldsAll);
  for (u32 b = 0; b < 18; ++b)
    c.bits[b] += (fields >> b) & 1;
  c.no_change += chained && !(fields & ~u32(kFieldUnread | kFieldPrivate | kFieldTail));
  if (c.combos.size() < 4096 || c.combos.contains(fields))
    ++c.combos[fields];
  c.tess += call.tessellation;
  c.up += call.up_vertices != nullptr || call.up_indices != nullptr;
  c.points += call.primitive == kPointList;
  c.exports += export_count != 0;
  c.pending += AnyPendingExports();
  c.skin += IsSkinOnceVs(vs.hash);
  if (ps) {
    const uint64_t hh = ps->hash;
    c.fur += hh == 0x2F36770769087F99ull || hh == 0xC92BA80672FB9774ull || hh == 0xB3C10F3D80EF61E2ull ||
             hh == 0xD40EBF0336F89333ull;
  }
  if (replay && replay->vs_dirty)
    c.vs_groups += uint64_t(std::popcount(*replay->vs_dirty));
  if (replay && replay->ps_dirty)
    c.ps_groups += uint64_t(std::popcount(*replay->ps_dirty));
}

// kameo-nx (2026-11-04, the user's console run: a crash loading the big battle level 004CFC24, out of memory in seconds).
// The ring is only recycled when its frame ends, and a level's first frames can draw many thousands of models at 2-4 KB
// of bone constants each, in one frame. So a long frame is ended on the GPU here, as a frame with nowhere to present ends
// (SubmitAndAdvance): the wait for the next slot holds this thread back while the GPU catches up, and that slot's ring is
// free. Commands still run in order, on the same targets. What the fast path kept from the draw before can name the ring, so
// it starts again; the bound state goes with the list's generation.
uint64_t g_frame_splits = 0;
[[gnu::cold, gnu::noinline]] void SplitLongFrameLocked() {
  auto &s = State();
  if (!s.list_open)
    return;
  detail::SubmitAndAdvance(s, nullptr, nullptr);
  if (!Video::EnsureCommandListLocked())
    return;
  SyncBound(s);
  InvalidateFastDrawLocked();
  ++g_frame_splits;
  if (g_frame_splits <= 4 || g_frame_splits % 256 == 0)
    RGPU_INFO("[renut] long frame split (banjo_renut_split_frame_mb): {} so far", g_frame_splits);
}

// sasrt-nx (2026-11-08, sasrt_gpu_skip_inert_draws; the GPU round, sasrt_gpu_round.h): whether a draw can write nothing
// at all: no colour channel in the colour mask (whatever is bound), no depth write (tested and written), no stencil
// operation, and neither shader exports to memory. The cheapest words first: nearly every draw writes depth or colour.
// A shader the renderer does not know yet answers "not inert" (the draw path handles it as before).
// sasrt-nx (2026-11-08, sasrt_gpu_export_raster_discard): allow_vs_exports: a vertex shader's memory exports allowed (the
// draw's render state writes nothing; its exports still happen).
[[gnu::noinline]] bool SasrtInertDrawLocked(const u8 *device_state, bool allow_vs_exports = false) {
  const GuestDevice device(device_state);
  if (!device.valid())
    return false;
  const d3d::Device &dev = device.dev();
  const d3d::ControlPacket &cp = dev.m_ControlPacket;
  const u32 dc = u32(cp.DepthControl);
  if ((dc & 6) == 6)  // Z_ENABLE and Z_WRITE_ENABLE
    return false;
  if (u32(dev.m_ValuesPacket.ColorMask) & 0xFFFF)
    return false;
  if ((dc & 1) && (((dc >> 11) & 0x1FF) != 0 || ((dc >> 20) & 0x1FF) != 0))  // STENCIL_ENABLE with an operation
    return false;
  const auto exports = [](const ShaderRecord *r) {
    if (!r->entry)
      return true;
    for (const u32 m : r->entry->memExportConstants)
      if (m)
        return true;
    return false;
  };
  const ShaderRecord *vs = FindShaderLocked(device.U32(d3d::kDeviceOffsetVertexShader), RecordKind::VertexShader);
  if (!vs || (exports(vs) && !allow_vs_exports))
    return false;
  if (const u32 ps_va = device.U32(d3d::kDeviceOffsetPixelShader)) {
    const ShaderRecord *ps = FindShaderLocked(ps_va, RecordKind::PixelShader);
    if (!ps || exports(ps))
      return false;
  }
  return true;
}

// sasrt-nx (2026-11-08, sasrt_gpu_diag_export_dupes; the user's "big cuts"): memexport draws whose inputs are those of an
// earlier one in the same game frame: the same vertex shader, draw, export streams, the float registers the shader reads,
// its bool and loop constants and every vertex fetch constant (the vertex data itself is not compared here). Counted per
// 10 s ([sasrt] export repeats); nothing is changed.
REXCVAR_DEFINE_BOOL(sasrt_gpu_diag_export_dupes, false, "SASRT/Diagnostics",
                    "Diagnostic (2026-11-08): count the memexport draws whose inputs (vertex shader, draw, export streams, "
                    "the float registers it reads, bool and loop constants, vertex fetch constants) repeat an earlier "
                    "one's in the same game frame: the skinning exports the game may make once a pass");
struct SasrtExportRepeat {
  uint64_t sig = 0, frame = ~0ull;
  u32 base = 0, size = 0;
};
SasrtExportRepeat g_sasrt_export_repeat[512];
uint64_t g_sasrt_export_counts[4];  // exports, repeats, repeats of the newest window over their memory, distinct vs
// sasrt-nx (2026-11-08, the third round): the memexport draws by primitive type (Xenos 0-15) and indexed, and their
// vertex counts (what a vertex shader runs over)
uint64_t g_sasrt_export_prims[16][2], g_sasrt_export_vertices;
std::chrono::steady_clock::time_point g_sasrt_export_report;

// sasrt-nx (2026-11-08, the third round): with_base false: the windows' sizes only, and the float registers that hold
// the export addresses (the entry's memExportConstants) left out: the same skinning into another window.
uint64_t SasrtExportSignature(const GuestDevice &device, const DrawCall &call, const ShaderRecord &vs,
                              const FloatConstants &vsc, const u32 *vs_used, const MemExportStream *exports,
                              u32 export_count, bool with_base = true) {
  using banjo::renut_gpu::ContentHash64;
  struct Head {
    uint64_t vs;
    u32 primitive, indexed, start_vertex, count, start_index, up;
    i32 base_vertex;
  } head{vs.hash, call.primitive, call.indexed, call.start_vertex, call.count, call.start_index,
         call.up_vertices != nullptr, call.base_vertex};
  uint64_t h = ContentHash64(&head, sizeof(head));
  if (with_base) {
    h = ContentHash64(exports, sizeof(MemExportStream) * export_count, h);
  } else {
    for (u32 i = 0; i < export_count; ++i)
      h = ContentHash64(&exports[i].size, sizeof(exports[i].size), h);
  }
  for (u32 w = 0; w < 8; ++w)
    for (u32 bits = (vs_used ? vs_used[w] : 0xFFFFFFFFu) & (with_base || !vs.entry ? ~0u : ~vs.entry->memExportConstants[w]);
         bits; bits &= bits - 1) {
      const u32 r = w * 32 + u32(std::countr_zero(bits));
      h = ContentHash64(vsc.Register(r), 16, h ^ r);
    }
  const d3d::Constants &k = device.dev().m_Constants;
  h = ContentHash64(&k.VertexShaderB, sizeof(k.VertexShaderB), h);
  h = ContentHash64(&k.VertexShaderI, sizeof(k.VertexShaderI), h);
  h = ContentHash64(&k.VertexFetch, sizeof(k.VertexFetch), h);
  return h;
}

// sasrt-nx's game-frame count lives in its src/sasrt_capture.cpp (bumped at each Swap). Here it only tags the export
// repeat diagnostic (sasrt_gpu_diag_export_dupes, off by default) and is never bumped: with it on, repeats are counted
// across frames, not within one.
std::atomic<uint64_t> g_sasrt_game_frames{0};
SasrtExportRepeat g_sasrt_export_moved[512];  // sasrt-nx (2026-11-08): signatures without the windows' addresses
void SasrtNoteExport(uint64_t sig, uint64_t moved_sig, const MemExportStream *exports, u32 export_count) {
  const uint64_t frame = g_sasrt_game_frames.load(std::memory_order_relaxed);
  ++g_sasrt_export_counts[0];
  {
    SasrtExportRepeat &m = g_sasrt_export_moved[moved_sig % 512];
    if (m.sig == moved_sig && m.frame == frame)
      ++g_sasrt_export_counts[3];
    else
      m = {moved_sig, frame, exports[0].base, exports[0].size};
  }
  SasrtExportRepeat &e = g_sasrt_export_repeat[sig % 512];
  if (e.sig == sig && e.frame == frame) {
    ++g_sasrt_export_counts[1];
    // the earlier window still the newest over that memory (what a skipped draw would leave its readers)
    const PendingExport *w = InPlaceWindow(g_exports[State().frame], exports[0]);
    if (export_count == 1 && w && w->base == e.base && w->size == e.size)
      ++g_sasrt_export_counts[2];
  } else {
    e = {sig, frame, exports[0].base, exports[0].size};
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_sasrt_export_report > std::chrono::seconds(10)) {
    if (g_sasrt_export_report.time_since_epoch().count())
      RGPU_INFO("[sasrt] export repeats (sasrt_gpu_diag_export_dupes), 10 s: {} memexport draws, {} repeat an earlier "
                "one of the same frame ({} with its window still the newest over that memory); {} repeat one into "
                "another window (the same shader, draw, sizes and inputs but the export addresses; vertex data not "
                "compared)",
                g_sasrt_export_counts[0], g_sasrt_export_counts[1], g_sasrt_export_counts[2], g_sasrt_export_counts[3]);
    if (g_sasrt_export_report.time_since_epoch().count()) {
      std::string kinds;
      for (u32 p = 0; p < 16; ++p)
        for (u32 i = 0; i < 2; ++i)
          if (g_sasrt_export_prims[p][i])
            kinds += fmt::format(" primitive {}{} {}", p, i ? " indexed" : "", g_sasrt_export_prims[p][i]);
      RGPU_INFO("[sasrt] export draws by kind, 10 s:{}; {} vertices (indices) drawn", kinds, g_sasrt_export_vertices);
    }
    std::memset(g_sasrt_export_prims, 0, sizeof(g_sasrt_export_prims));
    g_sasrt_export_vertices = 0;
    g_sasrt_export_report = now;
    g_sasrt_export_counts[0] = g_sasrt_export_counts[1] = g_sasrt_export_counts[2] = g_sasrt_export_counts[3] = 0;
  }
}

// sasrt-nx (2026-11-11, port 82; its GPU list's row 4; UNTESTED), sasrt_gpu_skip_cull_both: a triangle draw culling
// both faces, built as FRONT_AND_BACK (banjo_renut_cull_both, pipeline.cpp), rasterises nothing; left out when neither
// shader exports memory (an export draw's vertex shader still runs). Points have no facing and lines are not culled:
// only triangle kinds. renut-ultimate (port 82): not rectangle lists or tessellated draws either, which sasrt-nx's form
// took: MakeKey clears their cull bits (they are drawn unculled), so leaving them out would drop what is drawn now.
// Direct3D 12 has no such cull mode and draws both faces: there these draws now draw nothing, as on Vulkan, deko3d and
// the console. Counted in the 10 s "[renut] untested GPU rows" line.
[[gnu::noinline]] bool SasrtCullBothLocked(const u8 *device_state, const DrawCall &call) {
  if (call.tessellation || (call.primitive != kTriangleList && call.primitive != kTriangleFan &&
                            call.primitive != kTriangleStrip && call.primitive != kQuadList))
    return false;
  const GuestDevice device(device_state);
  if (!device.valid() || (u32(device.dev().m_ControlPacket.ModeControl) & 3u) != 3u)
    return false;
  const auto exports = [](const ShaderRecord *r) {
    if (!r->entry)
      return true;
    for (const u32 m : r->entry->memExportConstants)
      if (m)
        return true;
    return false;
  };
  const ShaderRecord *vs = FindShaderLocked(device.U32(d3d::kDeviceOffsetVertexShader), RecordKind::VertexShader);
  if (!vs || exports(vs))
    return false;
  if (const u32 ps_va = device.U32(d3d::kDeviceOffsetPixelShader)) {
    const ShaderRecord *ps = FindShaderLocked(ps_va, RecordKind::PixelShader);
    if (!ps || exports(ps))
      return false;
  }
  std::atomic<uint64_t> &skipped = sasrt::gpu_round::g_census_cull_both_skipped;  // a load and a store (one writer)
  skipped.store(skipped.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  return true;
}

// blur-nx (2026-11-17, the Renut audit's #24): DrawStateLocked's keys, read at the render thread's first draw
// (DrawStateLocked runs on the render thread only: a plain flag, no guard's acquire load per key per draw).
namespace {
struct DrawStateKeys {
  bool cull_both = false;
  bool keep_fetches = false;
  bool fetch_verify = false;
  bool fast_census = false;
  bool export_verify = false;
  bool targets_verify = false;
  bool persistent_sc = false;
  bool persistent_verify = false;
  bool partial_clear = false;
  bool whole_verify = false;
  bool fetch_none_skip = false;
  bool fetch_data_unread = false;
  bool export_dupes = false;
  bool fetch_slots_skip = false;
  bool export_twice = false;
};
DrawStateKeys g_ds_keys;
bool g_ds_keys_ready = false;
[[gnu::noinline, gnu::cold]] void ReadDrawStateKeys() {
  g_ds_keys.cull_both = sasrt::gpu_round::SkipCullBothOn();
  g_ds_keys.keep_fetches = REXCVAR_GET(banjo_renut_keep_fetches);
  g_ds_keys.fetch_verify = REXCVAR_GET(banjo_renut_chain_reuse_verify);
  g_ds_keys.fast_census = REXCVAR_GET(banjo_renut_diag_fast_census);
  g_ds_keys.export_verify = REXCVAR_GET(banjo_renut_chain_reuse_verify);
  g_ds_keys.targets_verify = REXCVAR_GET(banjo_renut_chain_reuse) && REXCVAR_GET(banjo_renut_chain_reuse_verify);
  g_ds_keys.persistent_sc = REXCVAR_GET(banjo_renut_persistent_shared);
  g_ds_keys.persistent_verify = g_ds_keys.persistent_sc && REXCVAR_GET(banjo_renut_persistent_shared_verify);
  g_ds_keys.partial_clear = REXCVAR_GET(banjo_renut_shared_partial_clear);
  g_ds_keys.whole_verify = REXCVAR_GET(banjo_renut_whole_buffers_verify);
  g_ds_keys.fetch_none_skip = REXCVAR_GET(banjo_renut_fetch_data_unread);
  g_ds_keys.fetch_data_unread = REXCVAR_GET(banjo_renut_fetch_data_unread);
  g_ds_keys.export_dupes = REXCVAR_GET(sasrt_gpu_diag_export_dupes);
  g_ds_keys.fetch_slots_skip = REXCVAR_GET(banjo_renut_fetch_slots_skip);
  g_ds_keys.export_twice = REXCVAR_GET(sasrt_gpu_exp_export_twice);
  g_ds_keys_ready = true;
}
}  // namespace

bool DrawStateLocked(const u8 *device_state, const DrawCall &call, const ReplayConstants *replay) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kDrawState);  // blur-nx (2026-11-17)
  if (!g_ds_keys_ready) [[unlikely]]  // blur-nx (2026-11-17, audit #24)
    ReadDrawStateKeys();
  {  // sasrt-nx (2026-11-11, port 82; its GPU list's row 4; UNTESTED), sasrt_gpu_skip_cull_both: before everything else
    const bool cull_both = g_ds_keys.cull_both;
    if (cull_both && SasrtCullBothLocked(device_state, call)) [[unlikely]]
      return true;
  }
  // sasrt-nx (2026-11-08, sasrt_gpu_skip_inert_draws): before the fast path and the targets (SasrtInertDrawLocked). The
  // chain's next draw then finds no kept state of this one (its sequence number is not the last kept + 1): the full path.
  if (sasrt::gpu_round::On(sasrt::gpu_round::kInertDraws)) {
    const GuestDevice inert_device(device_state);
    const u32 dc = inert_device.valid() ? u32(inert_device.dev().m_ControlPacket.DepthControl) : 6u;
    if ((dc & 6) != 6 && SasrtInertDrawLocked(device_state)) [[unlikely]] {
      sasrt::gpu_round::Count(sasrt::gpu_round::kInertDraws);
      return true;
    }
  }
  static const size_t split_chunks = [] {
    const int mb = banjo::renut_gpu::SplitFrameMb();
    return mb > 0 && IsDeko3D() ? std::max<size_t>(size_t(mb) / 16, 1) : ~size_t(0);
  }();
  // sasrt-nx (2026-11-09 night, sasrt_memory_governor; port 58): at 48 MB (3 chunks) while memory is low (SASRT's
  // load frames grew the rings to 160-240 MB in its docked crashes); a frame split is submitted, the same pixels.
  if (const size_t chunk = Video::UploadsLocked().CurrentChunk();
      chunk >= split_chunks || (chunk >= 3 && split_chunks != ~size_t(0) && banjo::renut_gpu::MemoryLow())) [[unlikely]]
    SplitLongFrameLocked();  // kameo-nx (2026-11-04)
  // banjo-nx (2026-10-26, the record-time draw path): the lean executor first; 0: this draw is DrawStateLocked's.
  if (const int fast = FastDrawLocked(device_state, call, replay))
    return fast > 0;
  ++g_trace_draws;
  auto &s = State();
  const GuestDevice device(device_state);
  if (!device.valid())
    return false;

  const u32 vs_va = device.U32(d3d::kDeviceOffsetVertexShader);
  auto *vs = FindShaderLocked(vs_va, RecordKind::VertexShader);
  const u32 declaration_va = device.U32(d3d::kDeviceOffsetVertexDeclaration);
  auto *ps = FindShaderLocked(device.U32(d3d::kDeviceOffsetPixelShader), RecordKind::PixelShader);
  // Without a declaration set, the shader fetches as D3DVertexShader_Bind
  // bound it, or else as its own vfetches describe (bundle meshes).
  const VertexDeclarationRecord *declaration = nullptr;
  const u32 *shader_strides = nullptr;  // see GetStreamSource
  if (declaration_va) {
    if (declaration_va != g_last_declaration_va) {
      g_last_declaration =
          FindRecord<VertexDeclarationRecord>(declaration_va, RecordKind::VertexDeclaration);
      g_last_declaration_va = g_last_declaration ? declaration_va : 0;
    }
    declaration = g_last_declaration_va == declaration_va ? g_last_declaration : nullptr;
    if (!declaration && declaration_va == Video::GuestDevice() + kDeviceDeclarationOffset)
      declaration = DeviceDeclarationLocked(device);  // katamari-nx
  } else if (vs && vs->bound_declaration) {
    declaration = vs->bound_declaration.get();
    shader_strides = vs->bound_strides;
  } else if (const VertexDeclarationRecord *fetch = vs ? ShaderFetchDeclarationLocked(*vs) : nullptr) {
    declaration = fetch;
    shader_strides = vs->fetch_strides;
  } else {
    declaration = &NoDeclaration();
  }
  // banjo-nx (2026-10-19, render-thread proposal 16): the records this draw reads next, fetched while the code between
  // runs (the shader entries' first lines, the declaration): first touches were most of the draw's sampled loads.
  if (vs && vs->entry) {
    __builtin_prefetch(vs->entry);
    __builtin_prefetch(reinterpret_cast<const char *>(vs->entry) + 64);
  }
  if (ps && ps->entry) {
    __builtin_prefetch(ps->entry);
    __builtin_prefetch(reinterpret_cast<const char *>(ps->entry) + 64);
  }
  if (declaration)
    __builtin_prefetch(declaration);
  if (!vs || !declaration) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 16)) {
      const u32 va = vs ? declaration_va : vs_va;
      const auto *words = GuestPtr<const be_u32>(va);
      ++g_dropped_draws[kDropShaderRecord];
      RGPU_WARN("Draw dropped: {} {:08X} is not one the renderer knows (words {:08X} {:08X})",
                vs ? "vertex declaration" : "vertex shader", va, words ? u32(words[0]) : 0,
                words ? u32(words[1]) : 0);
    }
    return false;
  }
  // banjo-nx (2026-10-23, render-thread round): what this draw reads in guest memory and in the memos keyed by its
  // state, fetched now, all at once: the bound textures' headers and memo entries (two lines each), the texture extents'
  // memo lines (read for every texture while memexport results are pending), the vertex and index buffers' headers and
  // both stages' constant summaries. Reached one after the other as the draw went, these were most of the render
  // thread's stalls in the city: 5,000 draws a frame, whose records do not stay in the A57's 2 MB L2 from one frame to
  // the next. Prefetches only: nothing is read for them or changed.
  // banjo-nx (2026-10-23, batch 6): the bound slots' fetch constants, converted once for the draw (the prefetches below,
  // DrawNeedsExportFlush, FillTextures). Storage only: the set slots are written.
  // banjo-nx (2026-10-26, Unleashed-style step 1): kept from the draw before (static storage) while the recording says
  // no fetch constant changed since (ChainHint::fields): a slot is converted again only then, or when the draw before
  // did not convert it; its header and memo lines are prefetched only then too (the draw before read them).
  union DrawFetches {
    DrawFetches() {}
    TextureFetch f[32];
  };
  static DrawFetches draw_fetches;
  static u32 fetches_valid = 0;
  static uint64_t fetches_seq = 0;
  {
    const ChainHint &fetch_hint = g_chain_hint;
    const bool keep_fetches = g_ds_keys.keep_fetches;
    if (!keep_fetches || !(fetch_hint.seq && fetch_hint.seq == fetches_seq + 1 && !(fetch_hint.fields & (kFieldFetch | kFieldVertexFetch))))
      fetches_valid = 0;
    const bool texture_words_changed = !keep_fetches || !fetch_hint.seq || (fetch_hint.fields & kFieldTexWords);
    fetches_seq = fetch_hint.seq;
    const ShaderCacheEntry *vs_entry = vs->entry;
    const ShaderCacheEntry *ps_entry = ps ? ps->entry : nullptr;
    __builtin_prefetch(reinterpret_cast<const char *>(vs) + 64);  // the records' draw summaries (DrawSummaryOf)
    if (ps)
      __builtin_prefetch(reinterpret_cast<const char *>(ps) + 64);
    const u32 prefetch_textures =
        (vs_entry ? vs_entry->textureFetchMask : 0) | (ps_entry ? ps_entry->textureFetchMask : 0);
    const bool exports_pending = AnyPendingExports();
    const d3d::Device &pdev = device.dev();
    for (u32 bits = prefetch_textures; bits; bits &= bits - 1) {
      const u32 r = u32(std::countr_zero(bits));
      const u32 va = SlotTextureVa(device, r);
      const bool fresh = !((fetches_valid >> r) & 1);
      if (fresh) {
        draw_fetches.f[r] = DeviceTextureFetch(pdev, r);
        fetches_valid |= 1u << r;
      }
      const bool fetch_verify = g_ds_keys.fetch_verify;
      if (fetch_verify && !fresh) {
        const TextureFetch made = DeviceTextureFetch(pdev, r);
        if (std::memcmp(made.dword, draw_fetches.f[r].dword, sizeof(made.dword)) != 0) {
          static uint32_t logged = 0;
          if (logged++ < 8)
            RGPU_WARN("banjo_renut_chain_reuse_verify: a kept fetch constant (slot {}) differs from the device's", r);
          draw_fetches.f[r] = made;
        }
      }
      const TextureFetch &fetch = draw_fetches.f[r];
      if (va && (fresh || texture_words_changed)) {
        if (const auto *header = GuestPtr<const d3d::BaseTexture>(va)) {
          __builtin_prefetch(&header->Format);
          __builtin_prefetch(reinterpret_cast<const char *>(&header->Format) + sizeof(header->Format) - 1);
        }
        const TextureSlotMemo &m = TextureMemoSlot(va, fetch);
        __builtin_prefetch(&m);
        __builtin_prefetch(reinterpret_cast<const char *>(&m) + 64);
      }
      // banjo-nx (2026-10-23, batch 6): only when DrawNeedsExportFlush's answer for the slot is not kept already.
      if (exports_pending) {
        const TextureExportAnswer &a = TextureExportAnswerFor(fetch);
        if (a.generation != g_export_generation || std::memcmp(a.words, fetch.dword, sizeof(a.words)) != 0)
          PrefetchTextureGuestExtent(fetch);
      }
    }
    for (u32 left = declaration->layout.used_mask; left; left &= left - 1) {
      const u32 vb = device.U32(d3d::kDeviceOffsetStreamSources + u32(std::countr_zero(left)) * 4);
      if (const auto *p = vb ? GuestPtr<const u8>(vb) : nullptr) {
        __builtin_prefetch(p);
        __builtin_prefetch(p + sizeof(d3d::VertexBuffer) - 1);
      }
    }
    if (call.indexed && !call.up_indices) {
      const u32 ib = device.U32(d3d::kDeviceOffsetIndexBuffer);
      if (const auto *p = ib ? GuestPtr<const u8>(ib) : nullptr) {
        __builtin_prefetch(p);
        __builtin_prefetch(p + sizeof(d3d::IndexBuffer) - 1);
      }
    }
    if (vs_entry)
      __builtin_prefetch(&SummarySlot(vs_entry->floatConstants));
    if (ps_entry)
      __builtin_prefetch(&SummarySlot(ps_entry->floatConstants));
  }

  // Earlier memexport results reach guest memory before this draw reads them
  // (WaitForExportsLocked may submit the list: before anything is recorded).
  const FloatConstants vs_constants = DrawFloatConstants(device_state, replay, false);
  const FloatConstants ps_constants = DrawFloatConstants(device_state, replay, true);
  // banjo-nx (2026-10-19, render-thread proposal 4): the draw's memexport streams, gathered once here (they were
  // gathered for each pass of the loop below and again for the draw: the same constants each time).
  MemExportStream exports[kMaxMemExportStreams];
  u32 export_count = 0;
  // banjo-nx (2026-10-23): only shaders that export (DrawSummaryOf), without the entry's memo lookup per draw.
  if (DrawSummaryOf(*vs).exports)
    GatherMemExportStreams(vs->entry, vs_constants, exports, export_count);
  if (export_count && vs->export_only && SasrtExpNoExports()) [[unlikely]]
    return true;  // sasrt-nx (2026-11-08): an EXPERIMENT, not identical (sasrt_gpu_exp_no_exports)
  if (ps && DrawSummaryOf(*ps).exports)
    GatherMemExportStreams(ps->entry, ps_constants, exports, export_count);
  const bool fast_census = g_ds_keys.fast_census;
  if (fast_census)
    FastCensusDraw(call, *vs, ps, export_count, replay);
  // Waiting for an older slot's results leaves the current slot's, which the
  // draw may need as well: another pass checks (none are left after it).
  // banjo-nx (2026-10-26, API-level stage 4): the previous draw's "none, and no read met a window" again when nothing it
  // depends on changed: the same pending exports (g_export_generation), no fetch constant, texture word, stream, index
  // buffer, shader or declaration changed (ChainHint::fields), the same kind of call, and no exports of its own.
  struct ExportNone {
    uint64_t seq = 0, generation = 0;
    u32 call_kind = 0;
  };
  static ExportNone export_none;
  const ChainHint &export_hint = g_chain_hint;
  const u32 call_kind = u32(call.indexed) | u32(call.tessellation) << 1 | u32(call.up_vertices != nullptr) << 2 |
                        u32(call.up_indices != nullptr) << 3;
  const bool export_none_held =
      ChainReuseOn() && export_hint.seq && export_hint.seq == export_none.seq + 1 && !export_count &&
      export_none.generation == g_export_generation && export_none.call_kind == call_kind &&
      !(export_hint.fields &
        (kFieldFetch | kFieldVertexFetch | kFieldTexWords | kFieldStreams | kFieldIndex | kFieldShaders | kFieldDecl));
  export_none.seq = 0;
  if (export_none_held) {
    export_none.seq = export_hint.seq;
    const bool export_verify = g_ds_keys.export_verify;
    if (export_verify && AnyPendingExports()) {
      u32 slots = 0;
      if (DrawNeedsExportFlush(device, call, *vs, ps, *declaration, shader_strides, exports, export_count, slots,
                               draw_fetches.f) != ExportRead::None) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("banjo_renut_chain_reuse_verify: a kept \"no export read\" differs from the check's answer");
      }
    }
  }
  while (!export_none_held && AnyPendingExports()) {
    u32 slots = 0;
    bool untouched = false;
    const ExportRead read = DrawNeedsExportFlush(device, call, *vs, ps, *declaration,
                                                 shader_strides, exports, export_count, slots, draw_fetches.f,
                                                 &untouched);
    if (read == ExportRead::None) {
      if (untouched && !export_count)
        export_none = {export_hint.seq, g_export_generation, call_kind};
      break;
    }
    static uint32_t wait_logged = 0;
    if (LogLimited(wait_logged)) {
      static constexpr const char *kReadNames[] = {"none",    "vertex stream", "vertex fetch", "indices",
                                                   "texture", "capacity"};
      RGPU_WARN("Draw waits for memexport results ({}; VS {:016X}, PS {:016X}{})",
                kReadNames[u32(read)], vs->hash, ps ? ps->hash : 0,
                (slots & (1u << s.frame)) ? ", this frame's: GPU round trip" : "");
    }
    WaitForExportsLocked(slots);
  }
  if (!Video::EnsureCommandListLocked())
    return false;
  SyncBound(s);

  plume::RenderShader *host_vs = GetHostShader(vs);
  plume::RenderShader *host_ps = ps ? GetHostShader(ps) : nullptr;
  if ((!host_vs || (ps && !host_ps)) && ++g_dropped_draws[kDropHostShader])
    return false;  // missing from the shader cache (captured for the next build)

  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): the previous chain draw's targets when the device's target words (bytes
  // 12440-12459 in N&B, 12816-12835 in SA2) did not change, no target was built, retired or rebound since (the surface epoch), and the targets'
  // descriptor words (which GetHostSurfaceLocked reads in guest memory) are the same.
  const bool targets_reuse_on = ChainReuseOn();  // banjo-nx (2026-10-26): with its A/B
  const bool targets_verify = g_ds_keys.targets_verify;
  const ChainHint &chain = g_chain_hint;
  Targets targets;
  bool targets_reused = false;
  // banjo-nx (2026-10-26): the five target words themselves compared, not their chunk (which holds the index buffer
  // and stream words too, changed by nearly every draw: the first emulator check reused 6-10 % of the targets).
  const auto target_words_same = [&device] {
    u32 now[d3d::kMaxRenderTargets + 1];
    for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i)
      now[i] = device.U32(d3d::kDeviceOffsetRenderTargets + i * 4);
    now[d3d::kMaxRenderTargets] = device.U32(d3d::kDeviceOffsetDepthStencil);
    return std::memcmp(now, g_targets_reuse.va, sizeof(now)) == 0;
  };
  // banjo-nx (2026-10-26, Unleashed-style step 1): the words compared only when the recording said they changed.
  if (targets_reuse_on && chain.seq && chain.seq == g_targets_reuse.seq + 1 &&
      (!(chain.fields & kFieldTargets) || target_words_same()) && g_targets_reuse.epoch == SurfaceEpochLocked()) {
    targets_reused = true;
    for (u32 i = 0; targets_reused && i <= d3d::kMaxRenderTargets; ++i) {
      u32 words[4];
      SurfaceDescWords(g_targets_reuse.va[i], words);
      targets_reused = std::memcmp(words, g_targets_reuse.desc[i], sizeof(words)) == 0;
    }
  }
  if (targets_reuse_on)
    CountReuse(0, targets_reused);  // banjo-nx (2026-10-23)
  if (targets_reused) {
    targets = g_targets_reuse.targets;
    if (targets_verify) {
      Targets made;
      if (!GatherTargets(device, made) || !SameTargets(made, targets)) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("banjo_renut_chain_reuse_verify: the reused targets differ from those gathered");
        targets = made;
      }
    }
  } else {
    if (!GatherTargets(device, targets)) {
      ++g_dropped_draws[kDropTargets];
      static uint32_t no_targets_logged = 0;
      if (LogLimited(no_targets_logged, 8))
        RGPU_WARN("Draw dropped: no render target or depth surface (RT0 {:08X}, depth {:08X}, {}, VS {:016X})",
                  device.U32(d3d::kDeviceOffsetRenderTargets), device.U32(d3d::kDeviceOffsetDepthStencil),
                  replay ? "replayed" : "immediate", vs->hash);
      g_targets_reuse.seq = 0;
      return false;
    }
  }
  if (targets_reuse_on) {
    g_targets_reuse.seq = chain.seq;
    g_targets_reuse.epoch = SurfaceEpochLocked();
    if (!targets_reused) {
      for (u32 i = 0; i <= d3d::kMaxRenderTargets; ++i) {
        g_targets_reuse.va[i] = device.U32(i < d3d::kMaxRenderTargets ? d3d::kDeviceOffsetRenderTargets + i * 4
                                                                       : d3d::kDeviceOffsetDepthStencil);
        SurfaceDescWords(g_targets_reuse.va[i], g_targets_reuse.desc[i]);
      }
      g_targets_reuse.targets = targets;
    }
  }
  // banjo-nx (2026-10-21, internal resolution): after the composite the HUD draws into the UI target (the display's
  // resolution) with the scene's depth target still bound, at the internal resolution: smaller. A deko3d framebuffer is
  // as large as its smallest attachment, so it is left out when the draw neither tests nor writes depth or stencil
  // (RB_DEPTHCONTROL bits 0-1; with the Z test off the GPU writes no depth either): the emulator's draw logs of the HUD
  // show 00708734, 00700040 and 00700060 only.
  if (targets.depth && targets.color_count && targets.depth->scale != targets.scale)
    DropSmallerDepth(device, targets, vs->hash, ps ? ps->hash : 0);
  // banjo-nx (2026-10-20, NOT exact, banjo_renut_fur_shells = N > 1, the user's call): the fur family's meshes are drawn
  // once a shell, shell by shell (each shell draws every part of the body: RenderDoc of Ryujinx shows the same four
  // parts repeating); of each part's draws in a frame (the same shaders and index range) every Nth is drawn, the first
  // always (tools/nullps_shaders.txt: Banjo's fur and the dialogue portrait's).
  // banjo_renut_portrait_fur_shells: the same for draws into a target of 128 x 128 or less (the dialogue portrait,
  // 96 x 96: 21 shells there cost ~2.5 ms of the heaviest frames, mostly vertex work).
  // banjo_renut_fur_shells_percent (2026-10-21, < 100): that share of each part's shells, spread evenly (the nth draw is
  // drawn when ceil((n + 1) p / 100) > ceil(n p / 100): 75 draws three, leaves one, ...), the first always (with
  // Banjo's 21 shells the outermost is drawn too).
  const bool portrait = targets.width <= 128 && targets.height <= 128;
  // banjo_renut_portrait_fur_shells_percent (2026-10-21): the portrait's own share when below 100.
  const uint32_t percent = portrait && banjo::renut_gpu::PortraitFurShellPercent() < 100
                               ? banjo::renut_gpu::PortraitFurShellPercent()
                               : banjo::renut_gpu::FurShellPercent();
  if (const uint32_t every = portrait ? banjo::renut_gpu::PortraitFurShellEvery() : banjo::renut_gpu::FurShellEvery();
      (every > 1 || percent < 100) && ps) {
    const uint64_t h = ps->hash;
    if (h == 0x2F36770769087F99ull || h == 0xC92BA80672FB9774ull || h == 0xB3C10F3D80EF61E2ull ||
        h == 0xD40EBF0336F89333ull) {
      struct ShellPart {
        uint64_t frame = 0;
        const void *vs = nullptr, *ps = nullptr;
        u32 start_index = 0, start_vertex = 0, count = 0;
        i32 base_vertex = 0;
        bool indexed = false;
        bool portrait = false;
        u32 n = 0;
      };
      static ShellPart parts[64];
      const uint64_t frame = TextureFrameLocked();
      ShellPart *part = nullptr, *free_part = nullptr;
      for (ShellPart &p : parts) {
        if (p.frame != frame) {
          if (!free_part)
            free_part = &p;
          continue;
        }
        if (p.vs == vs && p.ps == ps && p.start_index == call.start_index && p.start_vertex == call.start_vertex &&
            p.count == call.count && p.base_vertex == call.base_vertex && p.indexed == call.indexed &&
            p.portrait == portrait) {
          part = &p;
          break;
        }
      }
      bool draw = true;
      if (part) {
        const u32 n = ++part->n;
        draw = percent < 100 ? (n + 1) * percent / 100 + ((n + 1) * percent % 100 != 0) >
                                   n * percent / 100 + (n * percent % 100 != 0)
                             : n % every == 0;
      } else if (free_part) {
        *free_part = {frame, vs, ps, call.start_index, call.start_vertex, call.count, call.base_vertex, call.indexed,
                      portrait, 0};
      }
      banjo::renut_gpu::CountFurShell(draw);
      if (!draw)
        return true;
    }
  }

  // banjo-nx (banjo_gpu_depth_only): before anything of the pixel shader is set up.
  ShaderRecord *const store_device_ps = ps;  // banjo-nx (2026-10-26): for StoreFastStateLocked
  bool store_depth_only = false;
  if (ps && DrawsWithoutPixelShader(device, targets, *ps)) {
    ps = nullptr;
    host_ps = nullptr;
    store_depth_only = true;
    banjo::renut_gpu::CountDepthOnly();
  }
  // sasrt-nx (2026-11-08, sasrt_gpu_export_no_pixel_shader; the third GPU round): an export-only draw is drawn with the
  // rasterizer off and an empty scissor (sasrt_gpu_export_raster_discard, below): its pixel shader never runs, so it is
  // built without it (its vertex shader then writes no output for it). Not when the pixel shader exports itself.
  if (ps && vs->export_only && IsDeko3D() && banjo::renut_gpu::ExportOnlyDrawsOn() &&
      sasrt::gpu_round::On(sasrt::gpu_round::kExportDiscard) && sasrt::gpu_round::On(sasrt::gpu_round::kExportNoPs) &&
      !DrawSummaryOf(*ps).exports) {
    ps = nullptr;
    host_ps = nullptr;
    sasrt::gpu_round::Count(sasrt::gpu_round::kExportNoPs);
  }

  // banjo-nx (2026-10-23, render-thread round): the index memo's slot for this draw (the index buffer's header came with
  // the prefetches above); after FillTextures its entry and the range's first cards, which PrepareGeometry reads.
  IndexKey prefetch_index{};
  // banjo-nx (2026-10-26, API-level stage 4): not when the draw before was drawn whole and neither the index buffer nor
  // a stream, fetch constant or declaration changed since: this one most likely is too (its range key goes unused).
  static uint64_t whole_seq = 0;
  const ChainHint &index_hint = g_chain_hint;
  const bool likely_whole = index_hint.seq && index_hint.seq == whole_seq + 1 &&
                            !(index_hint.fields & (kFieldIndex | kFieldStreams | kFieldFetch | kFieldVertexFetch | kFieldDecl));
  const bool prefetch_indexed = !likely_whole && DrawIndexKey(device, call, prefetch_index);
  if (prefetch_indexed)
    g_index_memo.PrefetchSlot(prefetch_index);

  // Everything that records copies or barriers comes before the framebuffer.
  // banjo-nx (2026-10-26, Unleashed-style step 1, banjo_renut_persistent_shared): the block kept from the draw before
  // (g_draw_sc), or a new one.
  const bool persistent_sc = g_ds_keys.persistent_sc;
  const bool persistent_verify = g_ds_keys.persistent_verify;
  SharedConstants local_sc;
  SharedConstants &sc = persistent_sc ? DrawSharedLocked() : local_sc;
  if (!persistent_sc) {
    // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_shared_partial_clear): FillTextures writes all 128 texture and 32 sampler words (its
    // first loop, or the chain's copy of a previous draw's), so only what follows them is zeroed (848 of 1,360 bytes).
    static_assert(offsetof(SharedConstants, booleans) == sizeof(sc.texture_indices) + sizeof(sc.sampler_indices));
    const bool partial_clear = g_ds_keys.partial_clear;
    if (partial_clear)
      std::memset(&sc.booleans, 0, sizeof(sc) - offsetof(SharedConstants, booleans));
    else
      std::memset(&sc, 0, sizeof(sc));
    sc.flt_max = std::numeric_limits<float>::max();  // banjo-nx: 0x7F7FFFFF, the clamps' bound (shader_common.h)
  } else if (g_draw_sc_track.skin) {
    SetShared(sc, sc.pad_1352[0], 0.0f);  // the last draw's skin mode
    g_draw_sc_track.skin = false;
  }
  const u32 texture_mask =
      (vs->entry ? vs->entry->textureFetchMask : 0) | (ps && ps->entry ? ps->entry->textureFetchMask : 0);
  g_fetch_3d_mask = Fetch3DMaskOf(vs, ps);  // kameo-nx (2026-11-03)
  HandOverBeforeDrawLocked(targets, u32(device.dev().m_ControlPacket.DepthControl),
                           u32(device.dev().m_ValuesPacket.StencilRefMask));  // kameo-nx (2026-11-03)
  g_handover_targets = &targets;  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover)
  if (SasrtOrderEvery() > 0 && g_sasrt_order.on &&
      (targets.width != g_sasrt_order.tw || targets.height != g_sasrt_order.th)) [[unlikely]] {
    g_sasrt_order.tw = targets.width;  // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order)
    g_sasrt_order.th = targets.height;
    SasrtOrderToken(fmt::format("T{}x{}{}", targets.width, targets.height, targets.color_count ? "" : "z"));
  }
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): the previous chain draw's texture and sampler words when FillTextures'
  // memo would answer every bound slot as it did then: the device's fetch constants and texture words unchanged
  // (bytes 1152-1775, and the texture words: 12544-12647 in N&B, 12920-13023 in SA2), the same slots, frame, texture and depth epochs and depth target, and each bound
  // texture's header words the same. The memo hit's work is done again for each slot (its transitions and counters).
  struct TexturesReuse {
    uint64_t seq = 0, frame = 0, epoch = 0, depth_epoch = 0;
    u32 mask = 0;
    const HostSurface *depth = nullptr;
    u32 header[32][6] = {};
    u32 fetch[32][6] = {};  // banjo-nx (2026-10-26): the bound slots' fetch constants and texture words
    u32 va[32] = {};
    BindEffects fx[32];
    u32 indices[3][32] = {};
    u32 samplers[32] = {};
  };
  static TexturesReuse treuse;
  static u32 treuse_fetch_3d = 0;  // kameo-nx (2026-11-03): the same device words bind otherwise for another shader
  // renut-ultimate (2026-11-16, port 145; banjo_renut_fetch_filters, off by default): the shaders' own texture filters,
  // and which shaders gave the words the chain would reuse (the same device words sample otherwise for them).
  uint64_t filters_key = 0;
  const FetchFilterOverrides *const draw_filters = DrawFetchFiltersLocked(vs, ps, filters_key);
  static uint64_t treuse_filters_key = 0;
  // banjo-nx (2026-10-23, batch 6): FillTextures' words the same as its previous call's, and the fill count before it
  // (see the shared constants' push below).
  bool texture_words_same = false;
  uint64_t texture_fill_before = 0;
  bool textures_reused = targets_reuse_on && chain.seq && chain.seq == treuse.seq + 1 && treuse.mask == texture_mask &&
                         treuse.frame == TextureFrameLocked() && treuse.epoch == g_texture_epoch &&
                         treuse.depth_epoch == g_depth_epoch && treuse.depth == targets.depth &&
                         treuse_fetch_3d == g_fetch_3d_mask && treuse_filters_key == filters_key;
  // banjo-nx (2026-10-26): each bound slot's fetch constant and texture word compared (FillTextures reads those of the
  // mask's slots only; the others' words are its defaults), not every fetch constant's and texture word's chunk.
  // banjo-nx (2026-10-26, Unleashed-style step 1): the fetch constants and texture words compared only when the
  // recording said some changed; the headers (guest memory, which no device field covers) always.
  const bool texture_fields_same = !(chain.fields & (kFieldFetch | kFieldVertexFetch | kFieldTexWords));
  for (u32 bits = textures_reused ? texture_mask & ((1u << d3d::kTextureFetchSlots) - 1u) : 0u; bits;
       bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    const u32 va = SlotTextureVa(device, r);
    const auto *header = SlotHeader(va);  // sasrt-nx (2026-10-29)
    textures_reused = header &&
                      (texture_fields_same ||
                       (va == treuse.va[r] &&
                        std::memcmp(treuse.fetch[r], draw_fetches.f[r].dword, sizeof(treuse.fetch[r])) == 0)) &&
                      std::memcmp(treuse.header[r], &header->Format, sizeof(treuse.header[r])) == 0;
    if (!textures_reused)
      break;
  }
  if (targets_reuse_on)
    CountReuse(1, textures_reused);  // banjo-nx (2026-10-23)
  if (textures_reused) {
    // banjo-nx (2026-10-26, banjo_renut_persistent_shared): the kept block holds the previous draw's words already.
    for (u32 r = 0; r < 32 && !persistent_sc; ++r) {
      sc.texture_indices[0][r] = treuse.indices[0][r];
      sc.texture_indices[1][r] = treuse.indices[1][r];
      sc.texture_indices[2][r] = treuse.indices[2][r];
      sc.sampler_indices[r] = treuse.samplers[r];
    }
    for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
      if (!(texture_mask & (1u << r)))
        continue;
      const BindEffects &fx = treuse.fx[r];
      for (HostTexture *tex : fx.transition)
        if (tex && tex->layout != plume::RenderTextureLayout::SHADER_READ)  // banjo-nx (2026-10-23): as FillTextures
          TransitionTextureLocked(tex, plume::RenderTextureLayout::SHADER_READ);
      if (fx.handover_surface)
        TransitionSurfaceLocked(fx.handover_surface, plume::RenderTextureLayout::SHADER_READ);
      if (fx.handover_image)
        TransitionDepthImageLocked(*fx.handover_image, plume::RenderTextureLayout::SHADER_READ);
      if (fx.float_sampled)
        banjo::renut_gpu::CountDepthFloatSampled();
      if (fx.words_viewed)
        banjo::renut_gpu::CountDepthWords(true);
    }
    if (targets_verify) {
      SharedConstants made{};
      FillTextures(device, texture_mask, made, nullptr, nullptr, nullptr, nullptr, draw_filters);
      if (std::memcmp(made.texture_indices, sc.texture_indices, sizeof(sc.texture_indices)) != 0 ||
          std::memcmp(made.sampler_indices, sc.sampler_indices, sizeof(sc.sampler_indices)) != 0) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("banjo_renut_chain_reuse_verify: the reused texture words differ from those filled");
        std::memcpy(sc.texture_indices, made.texture_indices, sizeof(sc.texture_indices));
        std::memcpy(sc.sampler_indices, made.sampler_indices, sizeof(sc.sampler_indices));
        MarkShared(sc, sc.texture_indices, sizeof(sc.texture_indices) + sizeof(sc.sampler_indices));
        for (SlotLast &l : g_slot_last) {
          l.valid = false;  // the words are the made ones now
          l.obj = nullptr;  // banjo-nx (2026-10-27, Phase A)
          l.obj_fx = nullptr;  // banjo-nx (2026-10-28)
        }
      }
    }
  } else {
    bool all_memo = false;
    texture_fill_before = g_texture_fills;
    FillTextures(device, texture_mask, sc, targets_reuse_on ? treuse.fx : nullptr,
                 targets_reuse_on ? &all_memo : nullptr, draw_fetches.f, &texture_words_same, draw_filters);
    if (targets_reuse_on)
      treuse.seq = all_memo ? chain.seq : 0;
    treuse_filters_key = filters_key;  // port 145
    // banjo-nx (2026-10-26): the words copied only for a draw the next one may take them from (a hint, every slot
    // through the memo): the copies (headers, 512 bytes of words) were made at every draw.
    if (targets_reuse_on && all_memo && chain.seq) {
      // Kept for the next draw only when every slot is one the memo keys (a header), as it then answers them all.
      treuse.mask = texture_mask;
      treuse_fetch_3d = g_fetch_3d_mask;
      treuse.frame = TextureFrameLocked();
      treuse.epoch = g_texture_epoch;
      treuse.depth_epoch = g_depth_epoch;
      treuse.depth = targets.depth;
      for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
        if (!(texture_mask & (1u << r)))
          continue;
        const u32 va = SlotTextureVa(device, r);
        const auto *header = SlotHeader(va);  // sasrt-nx (2026-10-29)
        if (header)
          std::memcpy(treuse.header[r], &header->Format, sizeof(treuse.header[r]));
        treuse.va[r] = va;  // banjo-nx (2026-10-26)
        std::memcpy(treuse.fetch[r], draw_fetches.f[r].dword, sizeof(treuse.fetch[r]));
      }
      for (u32 r = 0; r < 32; ++r) {
        treuse.indices[0][r] = sc.texture_indices[0][r];
        treuse.indices[1][r] = sc.texture_indices[1][r];
        treuse.indices[2][r] = sc.texture_indices[2][r];
        treuse.samplers[r] = sc.sampler_indices[r];
      }
    }
  }
  if (textures_reused && targets_reuse_on)
    treuse.seq = chain.seq;
  g_handover_targets = nullptr;
  if (prefetch_indexed) {  // banjo-nx (2026-10-23): see prefetch_index
    g_index_memo.PrefetchEntry(prefetch_index);
    PrefetchCards(prefetch_index.src, size_t(prefetch_index.count) * (prefetch_index.is32 ? 4 : 2));
  }

  const d3d::Device &dev = device.dev();
  const u8 *bools = reinterpret_cast<const u8 *>(&dev.m_Constants.VertexShaderB[0]);
  // banjo-nx (2026-10-19, render-thread proposal 22): SwapEndian(2, ...) (8in32) inline: eight byte swaps, vectorised.
  u32 booleans[8];
  for (u32 i = 0; i < 8; ++i) {
    u32 word;
    std::memcpy(&word, bools + 4 * i, 4);
    booleans[i] = __builtin_bswap32(word);
  }
  SetShared(sc, sc.booleans, booleans);  // banjo-nx (2026-10-26): its line marked only when they changed
  SetLoopConstants(sc, dev);  // halo3-nx
  const u32 color_control = dev.m_ControlPacket.ColorControl;
  u32 alpha_test_func = (color_control & 8) ? (color_control & 7) : 7;
  float alpha_threshold = dev.m_ValuesPacket.AlphaRef;
  // banjo-nx (2026-10-26): this draw's function (the kept block's sc.alpha_test_func is the draw before's: read here
  // since API-level stage 1, every other alpha-to-coverage draw lost its test, the console's title-screen grass cards).
  if (((color_control >> 4) & 1) && banjo::renut_gpu::AlphaToMaskTestOn()) {
    // banjo-nx (banjo_renut_alpha_to_mask_test, 2026-10-18): alpha to coverage on a single-sample target is an alpha
    // test at 0.5 (MakeKey leaves the pipeline's alpha to coverage off for it). NVIDIA GPUs dither alpha to coverage
    // without samples: the checker-patterned grass, fur and foliage edges of the console and Ryujinx runs.
    // blur-nx (2026-11-13): also when the game has its own alpha test (AlphaToMaskTestSingleSample).
    const HostSurface *first = targets.color_count ? targets.colors[0] : targets.depth;
    if (ps && first && first->samples == plume::RenderSampleCount::COUNT_1)
      AlphaToMaskTestSingleSample(alpha_test_func, alpha_threshold);
  }
  SetShared(sc, sc.alpha_test_func, alpha_test_func);  // banjo-nx (2026-10-26)
  SetShared(sc, sc.alpha_threshold, alpha_threshold);
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): the previous chain draw's viewport when nothing ComputeViewport reads
  // changed: the window, values and control packets (bytes 10432-10599), the word at 0x29C0, the predication, the
  // targets' sizes and scale, the tiles and the replay's predication.
  ViewportState viewport;
  const bool viewport_reused =
      targets_reuse_on && chain.seq && chain.seq == g_viewport_reuse.seq + 1 &&
      // banjo-nx (2026-10-26, Unleashed-style step 1): what ComputeViewport reads of the device, by field (the
      // packets' 168 bytes were compared, a C library call at every draw: 3 % of the render thread on the console).
      !(chain.fields & (kFieldViewPackets | kField29C0 | kFieldPredication)) &&
      g_viewport_reuse.width == targets.width && g_viewport_reuse.height == targets.height &&
      g_viewport_reuse.guest_width == targets.guest_width && g_viewport_reuse.guest_height == targets.guest_height &&
      g_viewport_reuse.scale == targets.scale.code && g_viewport_reuse.tiles == g_tiles.size() &&
      g_viewport_reuse.predication == g_replay_predication;
  if (targets_reuse_on)
    CountReuse(2, viewport_reused);  // banjo-nx (2026-10-23)
  if (viewport_reused) {
    viewport = g_viewport_reuse.viewport;
    if (targets_verify) {
      const ViewportState made = ComputeViewport(device, targets);
      if (std::memcmp(&made, &viewport, sizeof(made)) != 0) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("banjo_renut_chain_reuse_verify: the reused viewport differs from the one computed");
        viewport = made;
      }
    }
  } else {
    viewport = ComputeViewport(device, targets);
  }
  if (targets_reuse_on) {
    g_viewport_reuse.seq = chain.seq;
    g_viewport_reuse.width = targets.width;
    g_viewport_reuse.height = targets.height;
    g_viewport_reuse.guest_width = targets.guest_width;
    g_viewport_reuse.guest_height = targets.guest_height;
    g_viewport_reuse.scale = targets.scale.code;
    g_viewport_reuse.tiles = g_tiles.size();
    g_viewport_reuse.predication = g_replay_predication;
    g_viewport_reuse.viewport = viewport;

  }
  // banjo-nx (2026-10-26, API-level stage 1): each set only when it changes (its line marked then).
  SetShared(sc, sc.screen_space, u32(viewport.screen_space ? 1 : 0));
  SetShared(sc, sc.screen_scale, viewport.screen_scale);
  SetShared(sc, sc.screen_offset, viewport.screen_offset);
  SetShared(sc, sc.depth_remap, viewport.depth_remap);  // sasrt-nx: g_DepthRemap
  SetShared(sc, sc.viewport_size, viewport.guest_size);
  SetShared(sc, sc.resolution_scale, targets.scale.Factor());
  SetShared(sc, sc.pad_1352[1], std::bit_cast<float>(InternalHeightsWord()));  // banjo-nx (2026-10-21): the shaders' codes 4 and 6
  SetShared(sc, sc.pixel_origin, viewport.pixel_origin);  // Renut upstream 574b488
  if (call.primitive == kPointList) {
    float point_state[4];
    PointStateOf(dev, point_state);  // bangaio-nx (2026-10-29): shared with FastDrawLocked
    SetShared(sc, sc.point_state, point_state);
  } else if (persistent_sc && g_draw_sc_track.point) {
    static constexpr float kZero4[4] = {};
    SetShared(sc, sc.point_state, kZero4);  // banjo-nx (2026-10-26): the last draw's
  }
  if (persistent_sc)
    g_draw_sc_track.point = call.primitive == kPointList;
  SetUserClipPlane(sc, dev, vs ? vs->hash : 0);  // rr6-nx / vivapinata-nx: plane 0 or zeros, every draw

  Geometry geometry;
  // banjo-nx (2026-10-26): as a new block's, then the tessellated draw's words (into a copy, set below).
  u32 tessellation[4] = {};
  // banjo-nx (2026-10-26, API-level stage 2): from whole-buffer copies when every stream and the indices have one.
  CachedStream *whole_copies[kMaxVertexStreams] = {};
  const bool whole = WholeGeometryLocked(device, call, *vs, shader_strides, declaration->layout,
                                         declaration->layout_hash, geometry, whole_copies);
  whole_seq = whole ? index_hint.seq : 0;  // banjo-nx (2026-10-26, stage 4): see likely_whole
  if (whole) {
    ++g_whole_draws;
    const bool whole_verify = g_ds_keys.whole_verify;
    if (whole_verify)
      VerifyWholeGeometry(device, call, *vs, shader_strides, declaration->layout, geometry, whole_copies);
  } else {
    ++g_range_draws;
    if (call.tessellation ? !PrepareTessellation(device, call, geometry, tessellation)
                          : !PrepareGeometry(device, call, geometry)) {
      ++g_dropped_draws[kDropGeometry];
      return false;
    }
  }
  // The host draws the vertex range in use from vertex 0 (indices rebased to
  // it in PrepareGeometry); the guest's r0.x is the vertex index itself.
  if (!call.tessellation)
    tessellation[3] = whole ? geometry.whole_base : geometry.first_vertex;  // banjo-nx: see Geometry::whole_base
  SetShared(sc, sc.tessellation, tessellation);
  // banjo-nx (2026-10-23, render-thread round): the stream memo's slots for the streams BindStreams will look up (their
  // vertex buffers' headers came with the prefetches above); before the constants are pushed, their entries and cards.
  StreamKey prefetch_streams[kMaxVertexStreams];
  StreamSource stream_sources[kMaxVertexStreams];  // banjo-nx (2026-10-23, batch 6): BindStreams takes them
  const u32 prefetch_stream_count =
      whole ? 0u
            : DrawStreamKeys(device, call, *vs, shader_strides, declaration->layout, declaration->layout_hash, geometry,
                             prefetch_streams, stream_sources);
  for (u32 i = 0; i < prefetch_stream_count; ++i)
    g_stream_memo.PrefetchSlot(prefetch_streams[i]);
  GpuStreams gpu_streams;
  if (!whole && !ConvertExportedStreamsLocked(s, device, call, *vs, shader_strides, *declaration, geometry,
                                              gpu_streams)) {
    ++g_dropped_draws[kDropStreams];
    return false;
  }

  // banjo-nx (2026-10-26, banjo_renut_persistent_shared): the entries and fetch slots the last draw set, put back.
  if (persistent_sc) {
    for (u32 bits = g_draw_sc_track.bound_entries; bits; bits &= bits - 1) {
      std::memset(sc.bound_fetch[std::countr_zero(bits)], 0, sizeof(sc.bound_fetch[0]));
      MarkShared(sc, sc.bound_fetch[std::countr_zero(bits)], sizeof(sc.bound_fetch[0]));
    }
    g_draw_sc_track.bound_entries = 0;
  }
  u32 bound_entries = 0;
  u32 fetch_mask = (vs->entry ? vs->entry->vertexFetchMemoryMask : 0) |
                   DescribeBoundFetches(device, call, *vs, *declaration, shader_strides, sc, &bound_entries);
  if (call.tessellation && g_cp_fetch.src) {  // vivapinata-nx (2026-11-03): a tessellated list's indices, a fetch of their own
    fetch_mask = ControlPointFetchMask(fetch_mask, tessellation);
    SetShared(sc, sc.tessellation, tessellation);
  }
  if (persistent_sc) {
    g_draw_sc_track.bound_entries = bound_entries;
    for (u32 bits = bound_entries; bits; bits &= bits - 1)
      MarkShared(sc, sc.bound_fetch[std::countr_zero(bits)], sizeof(sc.bound_fetch[0]));
    for (u32 bits = g_draw_sc_track.fetch_slots & ~fetch_mask; bits; bits &= bits - 1)
      SetShared(sc, sc.vertex_fetch_slots[64 + std::countr_zero(bits)], 0u);
    g_draw_sc_track.fetch_slots = fetch_mask;
  }
  {
    static const uint64_t want = [] {
      const std::string v = REXCVAR_GET(banjo_renut_diag_stream_dump_vs);
      return v.empty() ? 0ull : uint64_t(std::strtoull(v.c_str(), nullptr, 16));
    }();
    g_dump_fetch = want && vs->hash == want;
  }
  // banjo-nx (2026-10-26, Unleashed-style step 2): a draw that fetches nothing by address on deko3d (fetch_mask 0, its
  // data address and size left 0 below) makes no allocation: it took 16 bytes at a 256-byte alignment of the upload
  // ring at every such draw (most draws: about 1.3 MB of ring a frame) and wrote the fetch record nothing reads then.
  const bool fetch_none_skip = g_ds_keys.fetch_none_skip;
  const bool fetch_none = IsDeko3D() && fetch_mask == 0 && fetch_none_skip && !g_dump_fetch;
  UploadAllocation fetch_alloc;
  if (fetch_none) {
    g_fetch_record.count = 0;
    g_fetch_record.unusable = call.up_vertices != nullptr;
  } else {
    fetch_alloc = UploadVertexFetchData(device, call, fetch_mask, sc);
    // banjo-nx (2026-10-26): what it set: the mask's slots and the data's address and size.
    for (u32 bits = fetch_mask; bits; bits &= bits - 1)
      MarkShared(sc, &sc.vertex_fetch_slots[64 + std::countr_zero(bits)], 4);
    MarkShared(sc, &sc.vertex_fetch_data, sizeof(sc.vertex_fetch_data));
    MarkShared(sc, &sc.vertex_fetch_bytes, sizeof(sc.vertex_fetch_bytes));
  }
  if (g_dump_fetch) {
    static uint32_t logged = 0;
    if (logged++ < 24)
      RGPU_INFO("[fetchdump] draw: mask {:08X} fetch data {:X} bytes {} prim {} count {} tess {}", fetch_mask,
                sc.vertex_fetch_data, sc.vertex_fetch_bytes, call.primitive, call.count, call.tessellation);
  }
  g_dump_fetch = false;
  uint64_t fetch_window = 0;  // banjo-nx (2026-10-20, banjo_deko3d_skin_once): the binding's GPU address
#if defined(PLUME_DEKO3D)
  // banjo-nx (deko3d renderer): the vertex shaders read the fetch data through a storage binding of its range alone,
  // indexed by the address's low word (shader_common.h, g_DkAddressWindow): the binding starts at the address rounded
  // down to 256 bytes, and the word becomes the offset from there. One binding over every buffer (runs 14-15) cost
  // Ryujinx ~12 s a frame: it brings a storage binding's whole range up to date with guest memory at each draw.
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_fetch_data_unread): a draw whose shaders fetch nothing by address (fetch_mask 0:
  // the vertex shader's memory fetches and its bound fetches) never reads the fetch data's address or size: they stay
  // 0 instead of this draw's new allocation, so the shared constants' line with them does not change at every draw,
  // and no binding is made.
  const bool fetch_data_unread = g_ds_keys.fetch_data_unread;
  if (IsDeko3D() && fetch_mask == 0 && fetch_data_unread) {
    SetShared(sc, sc.vertex_fetch_data, uint64_t(0));
    SetShared(sc, sc.vertex_fetch_bytes, 0u);
  } else if (IsDeko3D() && sc.vertex_fetch_data) {
    const uint64_t address = sc.vertex_fetch_data;
    const uint64_t base = address & ~uint64_t(0xFF);
    plume::Deko3DBindFetchData(s.command_list, base, u32(address - base) + sc.vertex_fetch_bytes);
    SetShared(sc, sc.vertex_fetch_data, address - base);
    fetch_window = base;
    // banjo-nx (2026-10-20): the 16-byte multiples tools/deko3d_glsl_opt.py's aw4 pass relies on.
    if (DiagCountsOn()) {  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_lean_diagnostics)
      u32 bits = u32(sc.vertex_fetch_data) | sc.vertex_fetch_bytes;
      for (u32 m = fetch_mask; m; m &= m - 1)
        bits |= sc.vertex_fetch_slots[64 + std::countr_zero(m)] & ~3u;
      banjo::renut_gpu::CountFetchAlignment((bits & 15u) == 0);
    }
  }
#endif
  static constexpr u32 kAllRegisters[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  static constexpr u32 kNoRegisters[8] = {};
  const u32 *vs_used = vs->entry ? vs->entry->floatConstants : kAllRegisters;
  const u32 *ps_used = !ps ? kNoRegisters : ps->entry ? ps->entry->floatConstants : kAllRegisters;
  // banjo-nx (2026-10-19, banjo_deko3d_push_constants): pushed, except for draws point_sprite_gs expands (it reads
  // SharedConstants through its address).
  // bangaio-nx (2026-10-29, banjo_renut_point_fast_draw / A/B 69): those too: on deko3d point_sprite_gs reads
  // SharedConstants from UBO binding 2 (its BANJO_DEKO3D branch, the "banjo-dk ubo=0x4" of its header), the binding
  // the pushed shared buffer is bound at; the address read is the Vulkan shader's.
#if defined(PLUME_DEKO3D)
  const bool pushed = IsDeko3D() && banjo::renut_gpu::PushConstantsOn() &&
                      !(call.primitive == kPointList && s.point_sprite_gs && !PointFastDrawOn()) &&
                      EnsurePushedBuffers();
#else
  constexpr bool pushed = false;
#endif
  UploadAllocation vs_alloc, ps_alloc;
  if (pushed) {
#if defined(PLUME_DEKO3D)
    plume::RenderCommandList *cmd = s.command_list;
    g_diag_draw_no_ps = !ps;  // banjo-nx (2026-10-28, diagnostic)
    // banjo-nx (2026-10-23): the shaders' summaries from their records (DrawSummaryOf), not a memo per draw.
    PushFloatConstants(cmd, vs_constants, vs_used, g_bound.pushed_vs, vs->entry, g_pushed_buffers.address,
                       DrawSummaryOf(*vs));
    PushFloatConstants(cmd, ps_constants, ps_used, g_bound.pushed_ps, ps ? ps->entry : nullptr,
                       g_pushed_buffers.address + kPushedFloatStride, ps ? DrawSummaryOf(*ps) : SummaryOf(kNoRegisters));
    // The replay's dirty masks were cleared for the pushed models: the ring uploads' tracking of them no longer holds.
    g_bound.vs_constants.replay = 0;
    g_bound.ps_constants.replay = 0;
    banjo::renut_gpu::CountPushedConstants(1, 2);
#endif
  } else {
    vs_alloc = UploadFloatConstants(vs_constants, vs_used, g_bound.vs_constants, vs->entry);
    ps_alloc = UploadFloatConstants(ps_constants, ps_used, g_bound.ps_constants, ps ? ps->entry : nullptr);
    // banjo-nx (2026-10-19): and the other way round: the uploads cleared the dirty masks the pushed models track.
    g_bound.pushed_vs.replay = 0;
    g_bound.pushed_ps.replay = 0;
  }
  for (u32 i = 0; i < prefetch_stream_count; ++i) {  // banjo-nx (2026-10-23): see prefetch_streams
    g_stream_memo.PrefetchEntry(prefetch_streams[i]);
    PrefetchCards(prefetch_streams[i].guest, size_t(prefetch_streams[i].count) * prefetch_streams[i].stride);
  }
  u32 export_offsets[kMaxMemExportStreams] = {};
  if (persistent_sc) {  // banjo-nx (2026-10-26): the last draw's windows
    if (g_draw_sc_track.memexport) {
      std::memset(sc.memexport_streams, 0, sizeof(sc.memexport_streams));
      MarkShared(sc, sc.memexport_streams, sizeof(sc.memexport_streams));
    }
    g_draw_sc_track.memexport = export_count != 0;
  }
  if (export_count)
    MarkShared(sc, sc.memexport_streams, sizeof(sc.memexport_streams));  // banjo-nx (2026-10-26): set below
  if (export_count) {  // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_dupes)
    const bool export_dupes = g_ds_keys.export_dupes;
    if (export_dupes) [[unlikely]] {
      ++g_sasrt_export_prims[call.primitive & 15][call.indexed ? 1 : 0];
      g_sasrt_export_vertices += call.count;
    }
    if (export_dupes) [[unlikely]]
      SasrtNoteExport(SasrtExportSignature(GuestDevice(device_state), call, *vs, vs_constants, vs_used, exports,
                                           export_count),
                      SasrtExportSignature(GuestDevice(device_state), call, *vs, vs_constants, vs_used, exports,
                                           export_count, false),
                      exports, export_count);
  }
  if (export_count &&!PrepareMemExportLocked(s, exports, export_count, sc, export_offsets)) {
    export_count = 0;
    std::memset(sc.memexport_streams, 0, sizeof(sc.memexport_streams));
  }
  plume::RenderBuffer *memexport = MemExportBufferLocked(s);
  if (memexport && UsesVulkanBindings())  // banjo-nx: and deko3d
    SetShared(sc, sc.memexport_address, memexport->getDeviceAddress());
  else
    SetShared(sc, sc.memexport_address, uint64_t(0));  // banjo-nx (2026-10-26): as a new block's
  if (persistent_verify)
    VerifyDrawShared(sc, texture_mask, fetch_mask, bound_entries, export_count != 0, call.primitive == kPointList,
                     call.tessellation, false);
#if defined(PLUME_DEKO3D)
  // banjo-nx (deko3d renderer): the deko3d shaders write memexport through a binding of the buffer itself.
  if (memexport && IsDeko3D())
    plume::Deko3DBindMemExport(s.command_list, memexport);
#endif
  UploadAllocation sc_alloc;
  if (pushed) {
#if defined(PLUME_DEKO3D)
    // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): a chain draw whose texture words are the previous draw's, when that
    // draw pushed last (no push since, the same command list's buffer): its first 512 bytes are held already.
    static uint64_t texture_push_seq = 0, texture_push_count = 0;
    const bool texture_lines_held = textures_reused && !targets_verify && chain.seq &&
                                    texture_push_seq + 1 == chain.seq && texture_push_count == g_shared_pushes &&
                                    g_bound.pushed_shared_valid;
    // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_fetch_slots_skip): no fetches in this draw and none in the last push: both have
    // the fetch slots zero (sc is zeroed there; only UploadVertexFetchData writes them, for the fetch mask's bits).
    const bool fetch_slots_skip = g_ds_keys.fetch_slots_skip;
    const bool fetch_slots_equal = fetch_slots_skip && fetch_mask == 0 && g_held_fetch_slots_zero &&
                                   g_bound.pushed_shared_valid;
    // banjo-nx (2026-10-23, batch 6): the texture and sampler lines also when FillTextures wrote the words its previous
    // call wrote and that call's draw pushed last: the buffer holds them (every push writes each line that differs).
    static uint64_t held_texture_fill = 0;  // g_texture_fills after the fill whose words the buffer holds; 0: unknown
    bool texture_words_held = texture_words_same && texture_fill_before != 0 &&
                              held_texture_fill == texture_fill_before && g_bound.pushed_shared_valid;
    if (texture_words_held && banjo::renut_gpu::PushConstantsVerifyOn() &&
        std::memcmp(&g_bound.pushed_shared, &sc, offsetof(SharedConstants, booleans)) != 0) {
      static uint32_t logged = 0;
      if (logged++ < 16)
        RGPU_WARN("[pushed] the texture words held are not this draw's (banjo_deko3d_push_constants_verify)");
      texture_words_held = false;
    }
    PushSharedConstants(s.command_list, sc,
                        (texture_lines_held || texture_words_held ? kTextureLines : 0u) |
                            (fetch_slots_equal ? kFetchSlotLines : 0u));
    held_texture_fill = textures_reused ? 0 : g_texture_fills;
    g_held_fetch_slots_zero = fetch_mask == 0;  // the buffer holds this draw's now
    texture_push_seq = chain.seq;
    texture_push_count = g_shared_pushes;
#endif
    // The 10 s line's draw count is the uploads plus the reuses: a pushed draw counts as an upload (package 35 logged
    // about 2,800 draws where there were 250,000).
    banjo::renut_gpu::CountSharedConstants(false);
  } else if (g_bound.shared_valid && banjo::renut_gpu::SharedDedupeOn() &&
      std::memcmp(&g_bound.shared_bytes, &sc, sizeof(sc)) == 0) {
    sc_alloc = g_bound.shared_alloc;  // banjo-nx: the same bytes are in this list's last upload
    banjo::renut_gpu::CountSharedConstants(true);
  } else {
    sc_alloc = Video::UploadsLocked().Upload(&sc, sizeof(sc), 256);
    if (sc_alloc) {
      g_bound.shared_alloc = sc_alloc;
      std::memcpy(&g_bound.shared_bytes, &sc, sizeof(sc));
      g_bound.shared_valid = true;
    }
    banjo::renut_gpu::CountSharedConstants(false);
  }
  if (((!fetch_alloc && !fetch_none) || (!pushed && (!vs_alloc || !ps_alloc || !sc_alloc)) || !memexport) &&
      ++g_dropped_draws[kDropAllocation])
    return false;

  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): a chain draw takes the previous draw's key when nothing MakeKey reads
  // changed: the device's bool constants and render state packets (bytes 10112-10599: VertexShaderB / PixelShaderB,
  // the clip planes, the destination, window, values and control packets), the shaders' cache entries, the
  // declaration's layout, the primitive, the instancing, the alpha test, the targets (their surfaces and the surface
  // epoch, which every target built, retired or rebound bumps) and the in-run A/B half.
  struct KeyReuse {
    uint64_t seq = 0;
    const ShaderCacheEntry *vs_entry = nullptr, *ps_entry = nullptr;
    uint64_t layout_hash = 0, surface_epoch = 0;
    u32 primitive = 0, alpha_test_func = 0, color_count = 0;
    bool per_instance = false;
    int ab_off = 0;
    HostSurface *colors[d3d::kMaxRenderTargets] = {};
    HostSurface *depth = nullptr;
    PipelineKey key{};
  };
  static KeyReuse reuse;
  const bool reuse_on = targets_reuse_on;  // banjo-nx (2026-10-26): ChainReuseOn, read once a draw
  const bool reuse_verify = targets_verify;
  const ChainHint &hint = chain;
  const ShaderCacheEntry *const ps_entry = ps ? ps->entry : nullptr;
  const int ab_off = banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed);
  const uint64_t surface_epoch = SurfaceEpochLocked();
  bool reused = reuse_on && hint.seq && hint.seq == reuse.seq + 1 &&
                !(hint.fields & (kFieldBools | kFieldDest | kFieldViewPackets)) &&  // banjo-nx (2026-10-26): 10112-10599
                reuse.vs_entry == vs->entry && reuse.ps_entry == ps_entry &&
                reuse.layout_hash == declaration->layout_hash && reuse.primitive == call.primitive &&
                reuse.per_instance == geometry.per_instance && reuse.alpha_test_func == sc.alpha_test_func &&
                reuse.color_count == targets.color_count && reuse.depth == targets.depth &&
                reuse.surface_epoch == surface_epoch && reuse.ab_off == ab_off;
  for (u32 i = 0; reused && i < targets.color_count; ++i)
    reused = reuse.colors[i] == targets.colors[i];
  const FetchSpec fetch_spec = FetchSpecWords(sc, *vs, geometry.per_instance, ab_off);  // banjo-nx (banjo_gpu_fetch_spec)
  if (reused && FetchSpecDiffers(reuse.key, fetch_spec))  // vivapinata-nx (2026-11-12): every draw (the shader round)
    reused = false;
  // sasrt-nx (2026-11-08, sasrt_gpu_export_raster_discard; decided here since the fourth round): an export draw recorded
  // with the rasterizer off. The export-only draws this tree can tell: vivapinata-nx's export-mode shaders, or a memexport
  // draw whose render state writes nothing (SasrtInertDrawLocked with the vertex shader's exports allowed).
  const bool sasrt_export_discard =
      IsDeko3D() && sasrt::gpu_round::On(sasrt::gpu_round::kExportDiscard) &&
      ((vs->export_only && banjo::renut_gpu::ExportOnlyDrawsOn()) || (export_count && SasrtInertDrawLocked(device_state, true)));
  // sasrt-nx (2026-11-09, sasrt_gpu_export_no_point_gs; the fourth GPU round): such a draw of a point list without Renut's
  // point-sprite geometry shader. It made a quad of every point for a rasterizer that is off: SASRT's skinning exports
  // (point lists, ~47,000 points a frame) cost ~120 ns a point whatever their vertex shader did (console: an empty vertex
  // shader, its position moved, its stores flag cleared, other topologies: the same). The vertex shader runs once for
  // each point either way: the same exports; no pixel either way.
  const bool sasrt_no_point_gs = sasrt_export_discard && call.primitive == kPointList &&
                                 sasrt::gpu_round::On(sasrt::gpu_round::kExportNoPointGs);
  PipelineKey key;
  if (reuse_on)
    CountReuse(3, reused);  // banjo-nx (2026-10-23)
  if (reused) {
    key = reuse.key;
    if (reuse_verify) {
      PipelineKey made = MakeKey(device, vs, ps, declaration, targets, call.primitive, geometry.per_instance,
                                 sc.booleans, sc.alpha_test_func, fetch_spec);
      made.fetch_pad = key.fetch_pad;  // sasrt-nx (2026-11-08): set below for both (sasrt_gpu_plain_fetches)
      made.reserved = (made.reserved & ~8u) | (key.reserved & 8u);  // sasrt-nx (2026-11-08): sasrt_gpu_clip_spec, below
      std::memcpy(made.sasrt_words, key.sasrt_words, sizeof(made.sasrt_words));  // sasrt-nx: set below for both
      if (std::memcmp(&made, &key, sizeof(key)) != 0) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("banjo_renut_chain_reuse_verify: the reused pipeline key differs from the one made (VS {:016X})",
                    vs->hash);
        key = made;
      }
    }
  } else {
    key = MakeKey(device, vs, ps, declaration, targets, call.primitive, geometry.per_instance, sc.booleans,
                  sc.alpha_test_func, fetch_spec);
  }
  key.fetch_pad = key.pixel_shader ? SasrtPlainFetchMask(sc, ps_entry, vs->entry) : 0u;  // sasrt-nx; vivapinata-nx: vs
  if (call.primitive == kPointList && State().point_sprite_gs) {  // sasrt-nx (2026-11-09, sasrt_gpu_export_no_point_gs)
    key.point_sprite = sasrt_no_point_gs ? 0 : 1;  // (a reused key holds the decision of the draw it came from)
    if (sasrt_no_point_gs)
      sasrt::gpu_round::Count(sasrt::gpu_round::kExportNoPointGs);
  }
  key.reserved = (key.reserved & ~8u) | SasrtClipBit(sc);  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
  {  // sasrt-nx (2026-11-08, the third round: SasrtSpecWordsOf)
    const SasrtSpecWords words = SasrtSpecWordsOf(sc, vs->entry, ps_entry, &vs_constants, &targets);
    std::memcpy(key.sasrt_words, words.w, sizeof(words.w));
  }
  if (reuse_on) {
    reuse.seq = hint.seq;
    reuse.vs_entry = vs->entry;
    reuse.ps_entry = ps_entry;
    reuse.layout_hash = declaration->layout_hash;
    reuse.surface_epoch = surface_epoch;
    reuse.primitive = call.primitive;
    reuse.alpha_test_func = sc.alpha_test_func;
    reuse.color_count = targets.color_count;
    reuse.per_instance = geometry.per_instance;
    reuse.ab_off = ab_off;
    for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i)
      reuse.colors[i] = i < targets.color_count ? targets.colors[i] : nullptr;
    reuse.depth = targets.depth;
    if (!reused)
      reuse.key = key;
  }
  if (DiagCountsOn()) {  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_lean_diagnostics)
    // banjo-nx (package 28, a census): draws that write no colour, nothing at all, or part of a target's channels.
    const u32 used_mask = key.color_count ? key.color_mask & ((1u << (4 * key.color_count)) - 1u) : 0u;
    const bool no_colour = used_mask == 0;
    const u32 dc = key.depth_control;
    const bool depth = key.depth_format != plume::RenderFormat::UNKNOWN;
    const bool depth_writes = depth && (dc & 2) && (dc & 4);
    const u32 stencil_write_mask = (key.stencil_ref_mask >> 16) & 0xFF;
    const bool stencil_ops = ((dc >> 11) & 0x1FF) != 0 || ((dc >> 20) & 0x1FF) != 0;
    const bool stencil_writes = depth && (dc & 1) && stencil_write_mask && stencil_ops;
    bool partial = false;
    for (u32 i = 0; i < key.color_count; ++i) {
      const u32 m = (key.color_mask >> (4 * i)) & 0xF;
      partial |= m != 0 && m != 0xF;
    }
    const bool exports = (vs->entry && vs->entry->memExportConstants[0]) || (ps && ps->entry && ps->entry->memExportConstants[0]);
    {  // blur-nx (2026-11-13, #47's census): shaders indexing their float constants (all bits set), else the reads
      const auto all_set = [](const uint32_t *m) {
        for (int w = 0; w < 8; ++w)
          if (m[w] != ~0u) return false;
        return true;
      };
      const bool vs_all = vs->entry && all_set(vs->entry->floatConstants);
      const bool ps_all = ps && ps->entry && all_set(ps->entry->floatConstants);
      uint32_t regs = 0;
      if (vs->entry && !vs_all)
        for (int w = 0; w < 8; ++w) regs += uint32_t(__builtin_popcount(vs->entry->floatConstants[w]));
      if (ps && ps->entry && !ps_all)
        for (int w = 0; w < 8; ++w) regs += uint32_t(__builtin_popcount(ps->entry->floatConstants[w]));
      banjo::renut_gpu::CountConstantUse(vs_all, ps_all, regs);
    }
    banjo::renut_gpu::CountDrawKind(no_colour && depth, no_colour && depth && sc.alpha_test_func == 7 &&
                                    !key.alpha_to_coverage && ps != nullptr,
                                    no_colour && !depth_writes && !stencil_writes && !exports, partial);
  }
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): the previous draw's pipeline for its key again when no lookup was made
  // since and no prebuilt pipeline came in (PipelineLookupsLocked): GetPipelineLocked would answer the same.
  struct PipelineReuse {
    uint64_t seq = 0, lookups = 0, epoch = 0;
    plume::RenderPipeline *pipeline = nullptr;
    // port 58 (2026-11-10; RR6's diagnosis, SASRT's octopus): the words the key takes from each draw after the base key
    // was reused (sasrt_words, fetch_pad, the clip bit in reserved, point_sprite). The pipeline was handed back whenever
    // the base key was reused, so with sasrt_gpu_fetch_endian_spec a skinning draw whose bone fetch had another byte
    // order than the draw before ran the draw before's specialised pipeline: SASRT's Ocean View octopus came apart under
    // a heavy GPU (more draws in a row without a lookup between them). Now the reuse needs them the same too (every
    // sasrt_words item goes through here). Console check: SASRT's run octo_fix38, the octopus right in all ten shots.
    uint32_t sasrt_words[20] = {};
    uint32_t fetch_pad = 0, reserved = 0, point_sprite = 0;
  };
  static PipelineReuse preuse;
  plume::RenderPipeline *pipeline = nullptr;
  // banjo-nx (2026-10-20): the prebuild epoch read before the lookup, not after it: a worker that finished a specialised
  // pipeline between this draw's lookup (the generic one, its job not done yet) and the epoch read left the next draw
  // reusing the generic pipeline while a lookup would give the specialised one (emulator run 120: one
  // banjo_renut_chain_reuse_verify difference in 7 minutes; the same pixels, the generic pipeline reads the bools).
  const uint64_t prebuild_epoch = PrebuildEpochLocked();
  const bool same_draw_words = preuse.fetch_pad == key.fetch_pad && preuse.reserved == key.reserved &&
                               preuse.point_sprite == key.point_sprite &&
                               std::memcmp(preuse.sasrt_words, key.sasrt_words, sizeof(key.sasrt_words)) == 0;
  if (reused && preuse.pipeline && preuse.seq + 1 == hint.seq && preuse.lookups == PipelineLookupsLocked() &&
      preuse.epoch == prebuild_epoch && same_draw_words) {
    pipeline = preuse.pipeline;
    CountReuse(4, true);  // banjo-nx (2026-10-23)
    if (reuse_verify) {
      plume::RenderPipeline *made = GetPipelineLocked(key, {host_vs, host_ps, &declaration->layout});
      if (made != pipeline) {
        static uint32_t logged = 0;
        if (logged++ < 8)
          RGPU_WARN("banjo_renut_chain_reuse_verify: the reused pipeline differs from the one looked up");
        pipeline = made;
      }
    }
  } else {
    if (reuse_on)
      CountReuse(4, false);  // banjo-nx (2026-10-23)
    pipeline = GetPipelineLocked(key, {host_vs, host_ps, &declaration->layout});
  }
  if (reuse_on) {
    preuse.seq = hint.seq;
    preuse.lookups = PipelineLookupsLocked();
    preuse.epoch = prebuild_epoch;
    preuse.pipeline = pipeline;
    std::memcpy(preuse.sasrt_words, key.sasrt_words, sizeof(key.sasrt_words));  // port 58
    preuse.fetch_pad = key.fetch_pad;
    preuse.reserved = key.reserved;
    preuse.point_sprite = key.point_sprite;
  }
  // banjo-nx (2026-10-23, render-thread round): the pipeline's lines setPipeline and the refresh read, fetched while
  // the targets are bound.
  __builtin_prefetch(pipeline);
  __builtin_prefetch(reinterpret_cast<const char *>(pipeline) + 64);
  // banjo-nx (2026-10-28, banjo_deko3d_depth_handover_readonly): whether this draw writes depth or stencil.
  const bool draw_depth_writes = ((key.depth_control & 2) && (key.depth_control & 4)) ||
                                 ((key.depth_control & 1) && ((key.stencil_ref_mask >> 16) & 0xFF) != 0);
  // Port 179 (from fnxf_xiii; banjo_renut_edram_carry, off): a target first drawn this frame without a clear.
  if (pipeline && EdramCarryOn()) [[unlikely]]
    CarryEdramBeforeDrawLocked(targets);
  // blur-nx (2026-11-11, port 76; its key 220: blur_renut_edram_inherit): what other views wrote over a target this draw
  // keeps in part.
  if (pipeline)
    InheritEdramBeforeDrawLocked(targets, key.color_mask, key.blend_control);
  if ((!pipeline || !BindTargets(targets, draw_depth_writes)) && ++g_dropped_draws[kDropPipeline])
    return false;
#if defined(PLUME_DEKO3D)
  // banjo-nx (2026-10-21, banjo_deko3d_async_variants): a pipeline drawing a shader's generic variant while its own was
  // compiled in the background takes it once done (one atomic load otherwise), bound again when a stage changed.
  if (IsDeko3D() && plume::Deko3DRefreshPipeline(pipeline))
    g_bound.pipeline = nullptr;
#endif
  plume::RenderCommandList *cmd = s.command_list;
  // vivapinata-nx (2026-11-03, the white bird): a multipass vertex shader only exports to memory: the Xenos kills its primitives. It was
  // rasterized here, its pixel shader's literal white and its depth left under the coloured pass that reads the exported vertices:
  // distant creatures black and white with hard polygon edges. An empty scissor: the same pipeline, so no new shader variant. (Such a
  // draw exports, so it never takes FastDrawLocked.)
  // sasrt-nx (2026-11-08, sasrt_gpu_export_raster_discard): this tree sets export_only only since that item's port of
  // vivapinata-nx's export-mode test (d3d/hooks/shader.cpp ContainerIsExportOnly), so both of its effects follow the item's
  // key: off, such draws are rasterized as before the port.
  const bool export_only_draw = vs->export_only && banjo::renut_gpu::ExportOnlyDrawsOn() &&
                                sasrt::gpu_round::On(sasrt::gpu_round::kExportDiscard);
  SetViewport(cmd, viewport.viewport, export_only_draw ? plume::RenderRect(0, 0, 0, 0) : viewport.scissor,
              targets.color_count);
#if defined(PLUME_DEKO3D)
  // sasrt-nx (2026-11-08, sasrt_gpu_export_raster_discard; the second GPU round): and the rasterizer off for it: its
  // vertex shader runs and exports as before, and its primitives are dropped before clipping and setup instead of at the
  // empty scissor. No pixel either way.
  // The export draws this tree can tell (vs->export_only is vivapinata-nx's analysis, not made here): a memexport draw
  // whose render state writes nothing (SasrtInertDrawLocked with the vertex shader's exports allowed: no colour channel, no
  // depth write, no stencil operation, no pixel shader exports).
  if (sasrt_export_discard) {  // (export_only_draw implies it)
    plume::Deko3DDiscardNextDraw(cmd);
    sasrt::gpu_round::Count(sasrt::gpu_round::kExportDiscard);
    // sasrt-nx (2026-11-08, sasrt_gpu_export_list_topology; the fourth GPU round): a point list drawn this way (the
    // skinning exports) as a triangle list (count a multiple of 3) or a line list (even): with the rasterizer off only
    // the vertex shader runs, once for each vertex of a list either way, so the same exports; the GPU packs a point
    // list's vertices into vertex warps worse (SASRT: ~150 ns a skinned vertex).
    if (call.primitive == 1 && sasrt::gpu_round::On(sasrt::gpu_round::kExportListTopology)) {
      const u32 n = geometry.indexed ? geometry.index_count : geometry.vertex_count;
      if (n >= 3 && n % 3 == 0) {
        plume::Deko3DSetNextPrimitive(cmd, DkPrimitive_Triangles);
        sasrt::gpu_round::Count(sasrt::gpu_round::kExportListTopology);
      } else if (n >= 2 && n % 2 == 0) {
        plume::Deko3DSetNextPrimitive(cmd, DkPrimitive_Lines);
        sasrt::gpu_round::Count(sasrt::gpu_round::kExportListTopology);
      }
    }
  }
#endif

  if (!g_bound.layout) {
    cmd->setGraphicsPipelineLayout(s.draw_layout.get());
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 1);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 2);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 3);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 4);
    g_bound.layout = true;
    g_bound.pipeline = nullptr;
    g_bound.pushed_ubos = false;  // banjo-nx (2026-10-27)
    std::fill(std::begin(g_bound.root_set), std::end(g_bound.root_set), false);
    std::fill(std::begin(g_bound.constant_sets), std::end(g_bound.constant_sets), nullptr);
  }
  if (g_bound.pipeline != pipeline) {
    // banjo-nx: NVK's prefetch hint (the Switch); the record worker gives it itself, on the core that binds.
    // banjo-nx (2026-10-23, batch 6): not on deko3d (two calls a bind that did nothing there).
    if (!IsDeko3D() && !banjo::renut_gpu::record::IsProxy(cmd))
      banjo::renut_gpu::BeforePipelineBind(pipeline);
    if (!g_bound.pipeline)
      g_bound.depth_bias_set = false;  // rr6-nx: after an internal pass, whose pipeline set its own bias
    {
      ::renut::gpu::census::Scope pipeline_scope(::renut::gpu::census::kSetPipeline);  // blur-nx (2026-11-17)
      cmd->setPipeline(pipeline);
    }
    g_bound.pipeline = pipeline;
    // Vulkan: a pipeline without the constant as dynamic state overwrites it.
    if (IsVulkan())
      g_bound.blend_constant_set = false;
  }
  SetDrawDepthBias(cmd, dev);  // rr6-nx (2026-11-03, rr6_depth_bias)
  float blend_constant[4];
  if (DrawBlendConstant(dev, key, blend_constant) &&
      !(g_bound.blend_constant_set &&
        std::memcmp(blend_constant, g_bound.blend_constant, sizeof(blend_constant)) == 0)) {
    if (SetBlendConstant(cmd, blend_constant)) {
      std::memcpy(g_bound.blend_constant, blend_constant, sizeof(blend_constant));
      g_bound.blend_constant_set = true;
    } else {
      static bool logged = false;
      if (!logged) {
        logged = true;
        RGPU_WARN("Draws blend with the blend constant, which this backend cannot set");
      }
    }
  }
  if (UsesVulkanBindings()) {  // banjo-nx: and deko3d
    // banjo-nx (banjo_renut_push_point_only): with the constants in uniform buffers only point_sprite_gs reads the
    // push constant, so the other draws do not push it (NVK: a root-table upload a draw less).
    if (key.point_sprite || !banjo::renut_gpu::PushPointOnly()) {
      const DrawPushConstants push{vs_alloc.gpu_address, ps_alloc.gpu_address, sc_alloc.gpu_address};
      cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    }
    // banjo-nx: what the guest shaders read.
    if (pushed) {
#if defined(PLUME_DEKO3D)
      // banjo-nx (2026-10-19, banjo_deko3d_push_constants): the three fixed buffers (bindings 0, 1, 2), and bound
      // textures read from the renderer's copy of the shared constants.
      plume::Deko3DBindUniform(cmd, 0, g_pushed_buffers.address, kFloatConstantsUboBytes);
      plume::Deko3DBindUniform(cmd, 1, g_pushed_buffers.address + kPushedFloatStride, kFloatConstantsUboBytes);
      plume::Deko3DBindUniform(cmd, 2, g_pushed_buffers.address + kPushedSharedOffset, u32(sizeof(SharedConstants)));
      std::fill(std::begin(g_bound.constant_sets), std::end(g_bound.constant_sets), nullptr);  // rebind after
      if (!g_bound.shared_words_set) {
        plume::Deko3DSetSharedWords(cmd, &g_bound.pushed_shared, u32(sizeof(SharedConstants)));
        g_bound.shared_words_set = true;
      }
#endif
    } else {
#if defined(PLUME_DEKO3D)
      if (g_bound.shared_words_set) {
        plume::Deko3DSetSharedWords(cmd, nullptr, 0);
        g_bound.shared_words_set = false;
      }
#endif
      BindConstantSet(cmd, kVertexConstantsSet, vs_alloc.float_constants_set, vs_alloc.ref.offset);
      BindConstantSet(cmd, kPixelConstantsSet, ps_alloc.float_constants_set, ps_alloc.ref.offset);
      BindConstantSet(cmd, kSharedConstantsSet, sc_alloc.shared_constants_set, sc_alloc.ref.offset);
    }
  } else {
    SetRootDescriptor(cmd, vs_alloc.ref, kDrawRootVertexConstants);
    SetRootDescriptor(cmd, ps_alloc.ref, kDrawRootPixelConstants);
    SetRootDescriptor(cmd, sc_alloc.ref, kDrawRootSharedConstants);
    SetRootDescriptor(cmd, fetch_alloc.ref, kDrawRootVertexFetchData);
    SetRootDescriptor(cmd, plume::RenderBufferReference(memexport), kDrawRootMemExport);
  }

  if (whole ? !BindWholeStreams(declaration->layout, whole_copies, geometry.whole_base)
            : !BindStreams(device, call, *vs, shader_strides, declaration->layout, declaration->layout_hash, geometry,
                           gpu_streams, stream_sources)) {
    return false;
  }
#if defined(PLUME_DEKO3D)
  // banjo-nx (2026-10-20, banjo_deko3d_skin_once, FurSkinLocked): the fur's vertex shaders skin once a frame. The
  // pipeline has constant 913 under the same condition (pipeline.cpp); the mode word goes to the GPU with the pushed
  // shared constants (one 64-byte line again).
  if (pushed && !export_count && fetch_window && IsSkinOnceVs(vs->hash) && banjo::renut_gpu::SkinOnceOn() &&
      !(key.reserved & 4u) && !call.tessellation && geometry.instance_count == 1 && !geometry.per_instance) {
    if (const u32 mode = FurSkinLocked(s, cmd, vs, geometry, sc, fetch_window)) {
      SetShared(sc, sc.pad_1352[0], std::bit_cast<float>(mode));
      g_draw_sc_track.skin = persistent_sc;  // banjo-nx (2026-10-26): put back at the next draw
      PushSharedConstants(cmd, sc);
    }
  }
#endif
  if (TerrainDiagOn() && IsTerrainVs(vs->hash)) [[unlikely]]  // banjo-nx (2026-10-28, banjo_renut_diag_terrain)
    NoteTerrainDraw(*vs, call, geometry, vs_constants, draw_fetches.f, sc);
  if (gpu_profile::Timed()) {
    // banjo-nx (package 28b): the GPU profile's draws a frame, and with banjo_renut_diag_gpu_shaders its label.
    // banjo-nx (2026-10-19, render-thread proposal 7): only in the frames the profile times.
    if (export_count && SasrtProfileVsOn()) {  // sasrt-nx (2026-11-08, sasrt_gpu_diag_profile_vs)
      g_sasrt_label_export_count = std::max(1u, geometry.indexed ? geometry.index_count : geometry.vertex_count);
      g_sasrt_label_export_prim = call.primitive;
      g_sasrt_label_export_inst = geometry.instance_count;
    }
    const uint32_t label = banjo::renut_gpu::DiagGpuShadersOn() ? ShaderProfileLabel(targets, ps, key, vs)
                                                                : TargetsProfileLabel(targets);
    g_sasrt_label_export_count = 0;
    if (banjo::renut_gpu::DiagGpuShadersOn())
      gpu_profile::Mark(s, label);
    gpu_profile::CountDraw(s, label);
  }
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_lean_diagnostics): the draw log's check once, not a call per draw.
  static const bool draw_log = !REXCVAR_GET(banjo_renut_lean_diagnostics) ||
                               REXCVAR_GET(banjo_renut_diag_draw_log_s) > 0;
  if (draw_log)
    DrawLog(call, geometry, vs, ps, targets, key, sc, export_count);
  // banjo-nx (2026-10-19, banjo_renut_resolve_skip): the targets this draw can write.
  for (u32 i = 0; i < targets.color_count && i < key.color_count; ++i)
    if ((key.color_mask >> (4 * i)) & 0xF) {
      ++targets.colors[i]->writes;
      targets.colors[i]->write_seq = ++g_surface_write_seq;  // sasrt-nx: the EDRAM depth restore
      targets.colors[i]->blur_draw_seq = g_surface_write_seq;  // blur-nx (2026-11-10, port 76; its key 216)
      g_blur_color_write_last = g_surface_write_seq;  // blur-nx (2026-11-11, port 76; its key 220)
    }
  if (targets.depth) {
    const u32 dc = key.depth_control;
    const bool depth_writes = (dc & 2) && (dc & 4);
    const bool stencil_writes = (dc & 1) && ((key.stencil_ref_mask >> 16) & 0xFF) != 0;
    if (depth_writes || stencil_writes) {
      ++targets.depth->writes;
      targets.depth->write_seq = ++g_surface_write_seq;  // sasrt-nx: the EDRAM depth restore
    }
    if (dc & 1) {  // sasrt-nx's SasrtWarnStencilOnD32f inside it (rr6-nx, 2026-11-09)
      const bool first_stencil = !targets.depth->stencil_used;  // sasrt-nx (2026-11-11, port 75; renut_diag_formats)
      targets.depth->stencil_used = true;
      SasrtWarnStencilOnD32f(targets.depth, false, first_stencil);  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover)
    }
  }
  CensusDraw(device_state, call, targets, vs, ps, vs_constants, ps_constants, prefetch_streams, prefetch_stream_count,
             prefetch_indexed ? &prefetch_index : nullptr);  // banjo-nx (2026-10-23, batch 6b): diagnostics only
  if (UnleashedCensusOn())  // banjo-nx (2026-10-26): diagnostics only
    UnleashedCensusDraw(device, call, texture_mask, declaration->layout.used_mask, prefetch_streams,
                        prefetch_stream_count, prefetch_indexed ? &prefetch_index : nullptr);
  Video::NoteDrawLocked();  // banjo-nx (2026-10-19, banjo_deko3d_kick_submit): the open list's draws
  if (vs->entry && vs->entry->textureFetchMask)
    VertexTextureBarrierLocked();  // port 161 (banjo_deko3d_vertex_texture_barrier)
  SetSampleMaskLocked(cmd, device, targets);  // port 166 (banjo_deko3d_sample_mask)
  // sasrt-nx (2026-11-08, sasrt_gpu_exp_export_twice): a memexport draw drawn twice (the second writes the same bytes).
  const bool export_twice = g_ds_keys.export_twice;
  for (int pass = 0; pass < (export_count && export_twice ? 2 : 1); ++pass) {
  if (geometry.indexed) {
    SetIndexBuffer(cmd, geometry.index_view);
    // banjo-nx: instanced for a tessellation grid (banjo_gpu_tess_grid); 1 for every other indexed draw.
    // banjo-nx (2026-10-26, stage 2): a whole draw from the game's first index, with its base vertex.
    cmd->drawIndexedInstanced(geometry.index_count, geometry.instance_count, geometry.whole ? geometry.first_index : 0,
                              0, 0);
  } else {
    cmd->drawInstanced(geometry.vertex_count, geometry.instance_count, 0, 0);
  }
  }
  if (export_count && FinishMemExportLocked(s, exports, export_count, export_offsets))
    g_bound.framebuffer = nullptr;  // the copies end any render pass
  // banjo-nx (2026-10-21, banjo_renut_smaa): SMAA right after the frame's final composite, before the HUD is drawn over
  // it (render/smaa.cpp); then (internal resolution) the image scaled to the display's resolution for the HUD
  // (render/ui_scale.cpp). The passes leave their own targets, pipeline, layout and viewport bound.
  // banjo-nx (2026-10-23, batch 6): the record's answer (DrawSummaryOf), not the list's scan at every draw.
  // sasrt-nx (2026-11-08, port 52): a composite with more targets too (Sonic Transformed's 2414CA963766CE4E writes two):
  // SMAA and the HUD scale on its first.
  if (ps && targets.color_count >= 1 && !export_count && DrawSummaryOf(*ps).composite) {
    // sasrt-nx (2026-11-08, sasrt_gpu_handover_with_smaa): a texture still sharing the target's image gets its resolve
    // drawn before SMAA writes the image (the composite draw settled them already; this keeps SMAA safe on its own).
    if (targets.colors[0]->handed && SmaaAfterPs(ps->hash))
      DepthHandOverBeforeBindLocked(targets.colors[0], nullptr, 0);
    const bool smaa = SmaaAfterPs(ps->hash) && SmaaRunLocked(targets.colors[0]);
    const bool scaled = UiScaleAfterCompositeLocked(targets.colors[0]);
    if (smaa || scaled) {
      g_bound.framebuffer = nullptr;
      g_bound.pipeline = nullptr;
      g_bound.layout = false;
      g_bound.viewport_set = false;
    }
  }
  // banjo-nx (2026-10-26, the record-time draw path): what the next draw's FastDrawLocked starts from; not after a draw
  // that leaves something the next one undoes (the skin mode, points, memexport) or that the fast path never draws.
  // (2026-10-26, v2: also after Draw*UP, tessellated, range, point, memexport and skinned draws: the fast path puts
  // back what they leave in the block, and binds its own streams, index buffer and fetch data.)
  // banjo-nx (2026-10-28, per-draw night, banjo_renut_point_fast_state / A/B 59): a point-sprite draw's ring uploads
  // too (the only draws not pushed when pushing is on).
  const bool point_ring = !pushed && call.primitive == kPointList && s.point_sprite_gs && IsDeko3D() &&
                          banjo::renut_gpu::PushConstantsOn() && PointFastStateOn();
  const bool fast_usable = (pushed || point_ring) && persistent_sc && !(ps && DrawSummaryOf(*ps).composite);
  StoreFastStateLocked(call, vs, store_device_ps, store_depth_only, declaration, shader_strides, declaration_va != 0,
                       targets, viewport, key, pipeline, texture_mask, nullptr, sc.alpha_test_func,
                       geometry.per_instance, fast_usable);
  return true;
}

namespace detail {

void ReleaseCacheCopiesLocked(uint32_t slot) {
  g_copy_pool.ReleaseSlot(slot);
}

// sasrt-nx (2026-10-30, sasrt_renut_resolve_readback): Renut's resolves stay on the GPU, but a game's CPU may read one: Sonic
// Transformed's average scene luminance, resolved into a 1x1 k_32_FLOAT texture and locked by the tone mapper. With nothing
// written there it read 0, so every emissive surface stored its colour at 1/20. Such a resolve's texel is copied to the slot's
// small readback buffer after the resolve and written to its guest memory (big-endian, as the CPU's lfs reads it) when the
// slot's fence is waited, a frame or two later, with the slot's memexport results: the game locks a texture of its ring a
// frame behind.
struct ResolveReadback {
  u32 guest_pa;
  u32 offset;
};
constexpr u32 kReadbackStride = 512, kReadbackCount = 16;  // D3D12: 512-byte placement, 256-byte rows
std::vector<ResolveReadback> g_resolve_readbacks[kFramesInFlight];
plume::RenderBuffer *g_resolve_readback_buffer[kFramesInFlight] = {};  // kept until exit
uint64_t g_resolve_readbacks_done = 0;

bool ReadbackDestination(const HostTexture *dest) {
  return banjo::renut_gpu::ResolveReadbackOn() && dest && dest->width == 1 && dest->height == 1 &&
         dest->host_format == plume::RenderFormat::R32_FLOAT;
}

void QueueResolveReadbackLocked(VideoState &s, HostTexture *dest, uint32_t slice, uint32_t z, u32 level) {
  if (!ReadbackDestination(dest) || !dest->texture || slice || z || level || !dest->base_page)
    return;
  std::vector<ResolveReadback> &list = g_resolve_readbacks[s.frame];
  if (list.size() >= kReadbackCount)
    return;
  plume::RenderBuffer *&buffer = g_resolve_readback_buffer[s.frame];
  if (!buffer)
    buffer = s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(kReadbackStride * kReadbackCount)).release();
  if (!buffer)
    return;
  const u32 offset = u32(list.size()) * kReadbackStride;
#if defined(PLUME_DEKO3D)
  // sasrt-nx (2026-11-08, sasrt_gpu_texel_readback_compute; the GPU round): the texel by texel_readback_cs on the 3D
  // engine, from the texture as the resolve left it (in the read layout): a wait for the 3D engine and the L2 written back
  // for the CPU (Deko3DFlushL2) instead of the copy layout's full barriers on both sides of a copy-engine copy. The
  // same 32 bits land at the same place.
  if (IsDeko3D() && s.texel_readback_pipeline && dest->slot != UINT32_MAX && dest->texture &&
      sasrt::gpu_round::On(sasrt::gpu_round::kTexelReadback)) {
    std::unique_ptr<plume::RenderDescriptorSet> &set = s.texel_readback_set[s.frame];
    if (!set || s.texel_readback_set_buffer[s.frame] != buffer) {
      if (set)
        Video::RetireLocked(std::move(set));
      set = detail::CreateTargetReadbackSet(s);
      if (set) {
        set->setBuffer(0, buffer);
        s.texel_readback_set_buffer[s.frame] = buffer;
      }
    }
    if (set) {
      plume::RenderCommandList *cmd = s.command_list;
      TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);
      cmd->barriers(plume::RenderBarrierStage::COMPUTE,
                    plume::RenderTextureBarrier(dest->texture.get(), plume::RenderTextureLayout::SHADER_READ));
      cmd->setComputePipelineLayout(s.target_readback_layout.get());
      cmd->setPipeline(s.texel_readback_pipeline.get());
      cmd->setComputeDescriptorSet(s.texture_set.get(), 0);
      cmd->setComputeDescriptorSet(set.get(), 1);
      u32 push[8] = {dest->slot, offset, 0, 0, 0, 0, 0, 0};  // TexelReadbackPush (texel_readback_cs.hlsl)
      static_assert(sizeof(push) == sizeof(TargetReadbackPush));
      cmd->setComputePushConstants(0, push, 0, sizeof(push));
      cmd->dispatch(1, 1, 1);
      // sasrt-nx (2026-11-08, sasrt_gpu_readback_fence_flush): the slot's fence flushes the L2 before the CPU reads.
      if (sasrt::gpu_round::On(sasrt::gpu_round::kFenceFlush))
        sasrt::gpu_round::Count(sasrt::gpu_round::kFenceFlush);
      else
        plume::Deko3DFlushL2(cmd);
      list.push_back({dest->base_page << 12, offset});
      sasrt::gpu_round::Count(sasrt::gpu_round::kTexelReadback);
      g_bound.pipeline = nullptr;
      g_bound.framebuffer = nullptr;
      return;
    }
  }
#endif
  TransitionTextureLocked(dest, plume::RenderTextureLayout::COPY_SOURCE);
  const plume::RenderBox box(0, 0, 1, 1);
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(buffer, plume::RenderFormat::R32_FLOAT, 1, 1, 1, 64, offset),
      plume::RenderTextureCopyLocation::Subresource(dest->texture.get()), 0, 0, 0, &box);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);
  list.push_back({dest->base_page << 12, offset});
  g_bound.framebuffer = nullptr;  // the copy ends any render pass
}

void CompleteResolveReadbacksLocked(uint32_t slot) {
  std::vector<ResolveReadback> &list = g_resolve_readbacks[slot];
  if (list.empty())
    return;
  plume::RenderBuffer *buffer = g_resolve_readback_buffer[slot];
  const plume::RenderRange range(0, uint64_t(kReadbackStride) * kReadbackCount);
  if (const auto *data = buffer ? static_cast<const u8 *>(buffer->map(0, &range)) : nullptr) {
    for (const ResolveReadback &r : list) {
      u32 bits;
      std::memcpy(&bits, data + r.offset, 4);
      if (u8 *guest = GuestPhysicalPtr<u8>(r.guest_pa)) {
        const u32 big = __builtin_bswap32(bits);
        std::memcpy(guest, &big, 4);
      }
      if (g_resolve_readbacks_done++ < 4 || (g_resolve_readbacks_done & 0x3FF) == 0) {
        float v;
        std::memcpy(&v, &bits, 4);
        RGPU_INFO("[renut] resolve readback {}: {} to guest {:08X} (sasrt_renut_resolve_readback)", g_resolve_readbacks_done, v,
                  r.guest_pa);
      }
    }
    buffer->unmap();
  }
  list.clear();
}

void CompleteMemExportsLocked(uint32_t slot) {
  CompleteResolveReadbacksLocked(slot);  // sasrt-nx (2026-10-30): before the exports' early return
  SlotExports &exports = g_exports[slot];
  if (exports.pending.empty())
    return;
  auto &s = State();
  plume::RenderBuffer *readback = s.memexport_readback[slot].get();
  // banjo-nx (2026-10-23, batch 6): the slot's windows only (the readback buffer is CPU-cached: map drops the lines read).
  const plume::RenderRange read_range(0, exports.used);
  if (const auto *data = readback ? static_cast<const u8 *>(readback->map(0, &read_range)) : nullptr) {
    for (const PendingExport &p : exports.pending) {
      if (banjo::renut_gpu::ExportsHeldOn())  // sasrt-nx (2026-10-21, sasrt_renut_exports_held)
        HoldExport(p.base & 0x1FFFFFFFu, data + p.offset, p.size, g_stream_cache_frame);
      if (u8 *guest = GuestPhysicalPtr<u8>(p.base)) {
        std::memcpy(guest, data + p.offset, p.size);
        rex::memory::MarkStoreCardsAt(StoreCards(), 0xA0000000u + (p.base & 0x1FFFFFFFu), p.size);  // store cards
      }
    }
    readback->unmap();
  }
  g_pending_export_count -= u32(exports.pending.size());
  exports.pending.clear();
  exports.used = 0;
  RebuildExportGranules();
  // banjo-nx (2026-10-17): guest memory changed under the draws: cached copies are proven again
  // (banjo_renut_exact_streams), as after a kick.
  ++g_flush_serial;
}

} // namespace detail

// banjo-nx (2026-10-26, banjo_renut_diag_unleashed_census): see draw.h.
bool UnleashedCensusOn() {
  static const bool on = REXCVAR_GET(banjo_renut_diag_unleashed_census);
  return on;
}
namespace {
struct HookResourceShard {
  std::mutex mutex;
  std::unordered_map<u32, u8> kinds;
};
HookResourceShard g_hook_resources[16];
HookResourceShard &HookShard(u32 header) { return g_hook_resources[(header >> 6) & 15]; }
u8 HookResourceKind(u32 header) {
  if (!header)
    return 0;
  HookResourceShard &s = HookShard(header);
  std::lock_guard lock(s.mutex);
  const auto it = s.kinds.find(header);
  return it == s.kinds.end() ? 0 : it->second;
}
struct UnleashedCensusCounts {
  // [0] all, [1] header from a Create hook / D3D_SetTextureHeader, [2] data written since the frame before (a hook's
  // resource), [3] data written since the frame before (another's)
  std::atomic<uint64_t> streams[4], indices[4], textures[4];
  std::atomic<uint64_t> draws{0}, frames{0};
  std::atomic<uint64_t> locks[5], lock_bytes{0};  // by the resource's type (1 vertex, 2 index, 3 texture, 4 other), [0] of a hook's
  std::atomic<uint64_t> runs{0}, run_draws{0}, again_frame{0}, again_frame_draws{0}, again_later{0}, again_later_draws{0};
};
UnleashedCensusCounts g_ucensus;
std::mutex g_run_census_mutex;
std::unordered_map<uint64_t, uint64_t> g_run_last_swap;  // recording id -> the swap it last ran in
uint64_t g_ucensus_serial[2] = {0, 0};  // g_flush_serial at the start of this census frame, and of the frame before
uint64_t g_ucensus_frame = UINT64_MAX;
}  // namespace

void NoteHookResource(u32 header, u8 kind) {
  HookResourceShard &s = HookShard(header);
  std::lock_guard lock(s.mutex);
  s.kinds[header] = kind;
}
void ForgetHookResource(u32 header) {
  HookResourceShard &s = HookShard(header);
  std::lock_guard lock(s.mutex);
  s.kinds.erase(header);
}
void NoteResourceLock(u32 resource, u32 bytes) {
  const auto *r = GuestPtr<const be_u32>(resource);
  const u32 type = r ? u32(r[0]) & 0xF : 0;
  g_ucensus.locks[type >= 1 && type <= 3 ? type : 4].fetch_add(1, std::memory_order_relaxed);
  if (HookResourceKind(resource))
    g_ucensus.locks[0].fetch_add(1, std::memory_order_relaxed);
  g_ucensus.lock_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void NoteCommandBufferRun(const void *recording, u32 draws, uint64_t swap) {
  const uint64_t id = uint64_t(reinterpret_cast<uintptr_t>(recording));
  g_ucensus.runs.fetch_add(1, std::memory_order_relaxed);
  g_ucensus.run_draws.fetch_add(draws, std::memory_order_relaxed);
  std::lock_guard lock(g_run_census_mutex);
  auto [it, fresh] = g_run_last_swap.try_emplace(id, swap);
  if (!fresh) {
    if (it->second == swap) {
      g_ucensus.again_frame.fetch_add(1, std::memory_order_relaxed);
      g_ucensus.again_frame_draws.fetch_add(draws, std::memory_order_relaxed);
    } else {
      g_ucensus.again_later.fetch_add(1, std::memory_order_relaxed);
      g_ucensus.again_later_draws.fetch_add(draws, std::memory_order_relaxed);
    }
    it->second = swap;
  }
  if (g_run_last_swap.size() > 65536)
    g_run_last_swap.clear();  // ids only grow: the census forgets now and then
}

// On the render thread, a draw of a census frame.
void UnleashedCensusDraw(const GuestDevice &device, const DrawCall &call, u32 texture_mask, u32 stream_mask,
                         const StreamKey *streams, u32 stream_count, const IndexKey *index) {
  const uint64_t frame = TextureFrameLocked();
  if (frame != g_ucensus_frame) {
    g_ucensus_frame = frame;
    if (frame % 8 == 0) {
      g_ucensus_serial[1] = g_ucensus_serial[0];
      g_ucensus_serial[0] = g_flush_serial;
      g_ucensus.frames.fetch_add(1, std::memory_order_relaxed);
    } else if (frame % 8 == 7) {
      g_ucensus_serial[0] = g_flush_serial;  // the frame before the next census frame begins
    }
  }
  if (frame % 8 != 0 || !g_ucensus_serial[1])
    return;
  const uint64_t since = g_ucensus_serial[1];
  g_ucensus.draws.fetch_add(1, std::memory_order_relaxed);
  auto count = [](std::atomic<uint64_t> (&c)[4], bool hook, bool written) {
    c[0].fetch_add(1, std::memory_order_relaxed);
    if (hook)
      c[1].fetch_add(1, std::memory_order_relaxed);
    if (written)
      c[hook ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
  };
  // Streams: each used stream's vertex buffer header, and the range the draw reads.
  for (u32 left = stream_mask; left; left &= left - 1) {
    const u32 stream = u32(std::countr_zero(left));
    const u32 header = device.U32(d3d::kDeviceOffsetStreamSources + stream * 4);
    if (!header)
      continue;
    const StreamKey *key = nullptr;
    for (u32 i = 0; i < stream_count; ++i)
      if (streams[i].stream == stream)
        key = &streams[i];
    const bool written = key && !PagesClean(key->guest, size_t(key->count) * key->stride, since);
    count(g_ucensus.streams, HookResourceKind(header) == 1, written);
  }
  if (call.indexed && !call.up_indices) {
    const u32 header = device.U32(d3d::kDeviceOffsetIndexBuffer);
    const bool written = index && !PagesClean(index->src, size_t(index->count) * (index->is32 ? 4 : 2), since);
    count(g_ucensus.indices, HookResourceKind(header) == 2, written);
  }
  for (u32 bits = texture_mask & ((1u << d3d::kTextureFetchSlots) - 1u); bits; bits &= bits - 1) {
    const u32 header = SlotTextureVa(device, u32(std::countr_zero(bits)));
    if (!header)
      continue;
    const u8 kind = HookResourceKind(header);
    count(g_ucensus.textures, kind == 3 || kind == 4, false);
  }
}

// banjo-nx (2026-10-26, banjo_renut_diag_fast_census): the 10 s line.
std::string FastCensusReport() {
  std::lock_guard census_lock(g_fast_census_mutex);  // blur-nx (2026-11-10, port 76): see g_fast_census_mutex
  FastCensus &c = g_fast_census;
  if (!c.draws)
    return {};
  static constexpr const char *kNames[18] = {"fetch", "bools", "dest", "view packets", "misc", "29C0", "unread",
                                             "private", "decl", "tail", "index", "targets", "streams", "tex words",
                                             "scissor", "shaders", "predication", "vertex fetch"};
  const double n = double(c.draws);
  std::string out = fmt::format("fast-path census (banjo_renut_diag_fast_census): {} draws, {:.1f} % chain breaks, "
                                "{:.1f} % replayed, {:.1f} % no state change; fields:",
                                c.draws, 100.0 * c.breaks / n, 100.0 * c.replayed / n, 100.0 * c.no_change / n);
  for (u32 b = 0; b < 18; ++b)
    out += fmt::format(" {} {:.1f} %{}", kNames[b], 100.0 * c.bits[b] / n, b + 1 < 18 ? "," : ";");
  out += fmt::format(" cases: tessellated {:.2f} %, UP {:.2f} %, points {:.2f} %, own exports {:.2f} %, exports "
                     "pending {:.1f} %, skin once {:.2f} %, fur {:.2f} %; float groups a replayed draw: VS {:.1f}, PS "
                     "{:.1f}; top combos:",
                     100.0 * c.tess / n, 100.0 * c.up / n, 100.0 * c.points / n, 100.0 * c.exports / n,
                     100.0 * c.pending / n, 100.0 * c.skin / n, 100.0 * c.fur / n,
                     c.replayed ? double(c.vs_groups) / double(c.replayed) : 0.0,
                     c.replayed ? double(c.ps_groups) / double(c.replayed) : 0.0);
  std::vector<std::pair<uint64_t, u32>> top;
  for (const auto &[k, v] : c.combos)
    top.emplace_back(v, k);
  std::partial_sort(top.begin(), top.begin() + std::min<size_t>(10, top.size()), top.end(), std::greater<>());
  for (size_t i = 0; i < top.size() && i < 10; ++i) {
    out += fmt::format(" [{:.1f} %:", 100.0 * double(top[i].first) / n);
    for (u32 b = 0; b < 18; ++b)
      if ((top[i].second >> b) & 1)
        out += fmt::format(" {}", kNames[b]);
    out += "]";
  }
  c = FastCensus{};
  return out;
}

std::string UnleashedCensusReport() {
  if (!UnleashedCensusOn())
    return {};
  UnleashedCensusCounts &c = g_ucensus;
  auto take = [](std::atomic<uint64_t> &a) { return a.exchange(0, std::memory_order_relaxed); };
  uint64_t s[4], ix[4], t[4], l[5];
  for (u32 i = 0; i < 4; ++i) {
    s[i] = take(c.streams[i]);
    ix[i] = take(c.indices[i]);
    t[i] = take(c.textures[i]);
  }
  for (u32 i = 0; i < 5; ++i)
    l[i] = take(c.locks[i]);
  const uint64_t draws = take(c.draws), frames = take(c.frames), lock_bytes = take(c.lock_bytes);
  const uint64_t runs = take(c.runs), run_draws = take(c.run_draws), af = take(c.again_frame),
                 afd = take(c.again_frame_draws), al = take(c.again_later), ald = take(c.again_later_draws);
  auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
  return fmt::format(
      "Unleashed census (banjo_renut_diag_unleashed_census, {} census frames, {} draws): streams {} ({:.1f} % of a "
      "Create hook's buffer; written since the frame before: {} of those, {} of the others); indexed draws {} ({:.1f} % "
      "a Create hook's; written: {} / {}); textures bound {} ({:.1f} % from a Create hook or D3D_SetTextureHeader); "
      "Locks {} vertex, {} index, {} texture, {} other ({} of a hook's resource, {:.1f} MB); RunCommandBuffer {} "
      "recordings run ({} draws): {} run again in the same frame ({} draws), {} in a later frame ({} draws)",
      frames, draws, s[0], pct(s[1], s[0]), s[2], s[3], ix[0], pct(ix[1], ix[0]), ix[2], ix[3], t[0], pct(t[1], t[0]),
      l[1], l[2], l[3], l[4], l[0], double(lock_bytes) / double(1u << 20), runs, run_draws, af, afd, al, ald);
}

// banjo-nx (2026-10-26, banjo_renut_convert_reuse): the 10 s line.
std::string ConvertReuseReport() {
  ConvertMemo &m = g_convert_memo;
  if (!m.reused && !m.dispatched && !g_memexport_draws)
    return {};
  const uint64_t reused = m.reused, dispatched = m.dispatched, draws = g_memexport_draws, groups = g_readback_groups;
  const double seed_kb = double(g_seed_bytes) / 1024.0;
  const uint64_t in_place = g_exports_in_place;
  m.reused = m.dispatched = 0;
  g_memexport_draws = g_readback_groups = g_seed_bytes = g_exports_in_place = 0;
  return fmt::format("memexport GPU work: {} memexport draws ({:.0f} KB of seeds from guest memory, {} streams written in place, "
                     "banjo_renut_export_in_place), {} readback copy "
                     "groups (banjo_renut_batched_readbacks); exported stream conversions {} dispatched, {} used again "
                     "(banjo_renut_convert_reuse)",
                     draws, seed_kb, in_place, groups, dispatched, reused);
}

// bangaio-nx (2026-10-29): WholeBuffersReport's "kind" split (CountKindWhy) and the ranges drawn without an entry
// (RangeEntry).
std::string KindWhyReport() {
  uint64_t k[kKindWhyCount];
  for (u32 i = 0; i < kKindWhyCount; ++i) {
    k[i] = g_kind_why[i];
    g_kind_why[i] = 0;
  }
  const uint64_t points = g_kind_up_points, first = g_first_sightings, quads = g_quad_static_draws;
  const uint64_t forgotten = g_ranges_forgotten;
  g_kind_up_points = g_first_sightings = g_quad_static_draws = g_ranges_forgotten = 0;
  return fmt::format("\n\"kind\" was: tessellation {}, immediate vertices {} ({} point lists), immediate indices {}, no "
                     "count {}, triangle fans {}, quad lists {}, skinned once {}, off or not deko3d {}; ranges drawn "
                     "at their first sighting, no entry made {}, range entries with no copy dropped (new at every "
                     "kick, or unused 120 frames) {} "
                     "(banjo_renut_stream_second_sighting, A/B 71); quad lists drawn with the index buffer made once "
                     "{} (banjo_renut_quad_indices, A/B 72)",
                     k[kKindTess], k[kKindUpVertices], points, k[kKindUpIndices], k[kKindNoCount], k[kKindFan],
                     k[kKindQuads], k[kKindSkin], k[kKindOff], first, forgotten, quads);
}

std::string WholeBuffersReport() {
  const uint64_t whole = g_whole_draws, range = g_range_draws, base_draws = g_base_draws;
  g_whole_draws = g_range_draws = g_base_draws = 0;
  const uint64_t merged_draws = g_merged_draws, merged_ops = g_merged_ops;
  g_merged_draws = g_merged_ops = 0;
  const double fetch_fill_mb = double(g_fetch_fill_bytes) / 1048576.0;
  g_fetch_fill_bytes = 0;
  const uint64_t literals_resident = g_literals_resident;
  g_literals_resident = 0;
  const uint64_t fetch_kept = g_fetch_kept_draws;
  g_fetch_kept_draws = 0;
  // banjo-nx (2026-10-27, #8): the mesh objects' answers.
  const uint64_t mesh_hits = g_mesh_hits, mesh_reproved = g_mesh_reproved, mesh_kept = g_mesh_kept,
                 mesh_stale = g_mesh_stale, mesh_misses = g_mesh_misses;
  g_mesh_hits = g_mesh_reproved = g_mesh_kept = g_mesh_stale = g_mesh_misses = 0;
  if (!whole && !range)
    return {};
  uint64_t w[kWhyCount];
  for (u32 i = 0; i < kWhyCount; ++i) {
    w[i] = g_whole_why[i];
    g_whole_why[i] = 0;
  }
  const double made_mb = double(g_whole_bytes) / 1048576.0, dropped_mb = double(g_replaced_bytes) / 1048576.0;
  g_whole_bytes = g_replaced_bytes = 0;
  const std::string extents = RecordExtentsReport();  // banjo-nx (2026-10-27)
  // banjo-nx (2026-10-27, Phase A): the texture objects' answers.
  const std::string objects = fmt::format(
      "resource objects (banjo_renut_resource_objects, A/B 45): {} texture slots answered by their object ({} with the "
      "frame's content check), {} answers stored, {} objects made of {}; index buffer headers from the recording {}; "
      "verify: {} checked, {} answers differ, {} headers changed since recorded ({} of them bind to other words now), "
      "{} index / vertex buffer headers differ",
      g_tex_obj, g_tex_obj_refresh, g_tex_obj_stored, g_texture_objects_made.load(std::memory_order_relaxed),
      kTextureObjects, g_ib_recorded, g_tex_obj_verified, g_tex_obj_differ, g_tex_obj_header_differ,
      g_tex_obj_rewritten_differ, g_ib_verify_differ);
  g_tex_obj = g_tex_obj_refresh = g_tex_obj_stored = g_tex_obj_verified = g_tex_obj_differ = g_tex_obj_header_differ = 0;
  g_tex_obj_rewritten_differ = 0;
  g_ib_recorded = g_ib_verify_differ = 0;
  // banjo-nx (2026-10-27, banjo_renut_fetch_cache): the fetch cache's answers.
  const std::string fetch_cache = fmt::format(
      "fetch cache (banjo_renut_fetch_cache, A/B 47): {} draws bound kept fetch data ({:.1f} MB not copied), {:.1f} MB "
      "copied into it, {} copies dropped, {:.1f} MB held; verify: {} differ",
      g_fetch_pool_draws, double(g_fetch_pool_bytes_used) / 1048576.0, double(g_fetch_pool_made) / 1048576.0,
      g_fetch_pool_dropped, double(g_fetch_pool_held_bytes) / 1048576.0, g_fetch_pool_differ);
  g_fetch_pool_draws = g_fetch_pool_bytes_used = g_fetch_pool_made = g_fetch_pool_dropped = g_fetch_pool_differ = 0;
  // banjo-nx (2026-10-27, geometry objects): their answers.
  const std::string geometry = fmt::format(
      "geometry objects (banjo_renut_geometry_objects, A/B 48): {} draws answered by their buffers' objects, {} not "
      "(an object without this answer), {} answers stored, {} objects made of {}",
      g_geo_hits, g_geo_misses, g_geo_answers, g_geometry_objects_made.load(std::memory_order_relaxed),
      kGeometryObjects);
  g_geo_hits = g_geo_misses = g_geo_answers = 0;
  std::string shadows = ShadowCensusReport();
  if (g_shadow_culled) {  // banjo-nx (2026-10-27)
    shadows += fmt::format("{}shadow casters skipped (banjo_renut_shadow_cull_indices {}): {} draws, {} indices",
                           shadows.empty() ? "" : "\n", ShadowCullIndices(), g_shadow_culled, g_shadow_culled_indices);
    g_shadow_culled = g_shadow_culled_indices = 0;
  }
  return objects + "\n" + fetch_cache + "\n" + geometry + "\n" + (extents.empty() ? std::string() : extents + "\n") +
         (shadows.empty() ? std::string() : shadows + "\n") +
         fmt::format("mesh objects (banjo_renut_mesh_objects, A/B 42): {} draws answered ({} after proving their "
                     "copies for a new interval), {} objects made, {} of an older generation, {} other keys or none; "
                     "generation {}\n", mesh_hits, mesh_reproved, mesh_kept, mesh_stale, mesh_misses, g_mesh_gen) +
         fmt::format("whole buffers (banjo_renut_whole_buffers): {} draws from whole-buffer copies, {} from ranges "
                     "({:.1f} % whole; {} drawn with their base vertex, A/B 30; {} draws took {} draws after them, A/B 33); fetch copies made {:.1f} MB, {} draws kept the fetch data of the draw before (A/B 36); shaders' literals found still pushed {} (A/B 34); cache {:.1f} MB, whole copies made {:.1f} MB, range copies they replaced "
                     "dropped {:.1f} MB; ranges because: kind {}, stream source {}, stream in an export window {}, "
                     "stream dynamic {}, stream written lately {}, stream cache full {}, stream copy dropped {}, "
                     "index range {}, index in an export window {}, index dynamic {}, index written lately {}, index "
                     "cache full {}, index copy dropped {}, no memory {}",
                     whole, range, 100.0 * double(whole) / double(whole + range), base_draws, merged_draws, merged_ops, fetch_fill_mb, fetch_kept, literals_resident,
                     double(g_stream_cache_bytes) / 1048576.0, made_mb, dropped_mb, w[0], w[1], w[2], w[3], w[4], w[5],
                     w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13]) +
         KindWhyReport();
}

std::string RepeatCensusReport() {
  RepeatCensusTotals &t = g_census_totals;
  const uint64_t pairs = t.pairs.exchange(0, std::memory_order_relaxed);
  std::string out;
  for (u32 shadow = 0; shadow < 2; ++shadow) {
    std::string part;
    for (u32 k = 0; k < 2; ++k) {
      CensusCounts &c = t.c[shadow][k];
      const uint64_t draws = c.draws.exchange(0, std::memory_order_relaxed);
      const uint64_t state = c.state.exchange(0, std::memory_order_relaxed);
      const uint64_t constants = c.constants.exchange(0, std::memory_order_relaxed);
      const uint64_t data = c.data.exchange(0, std::memory_order_relaxed);
      const uint64_t full = c.full.exchange(0, std::memory_order_relaxed);
      const uint64_t maps = c.maps.exchange(0, std::memory_order_relaxed);
      const uint64_t maps_same = c.maps_same.exchange(0, std::memory_order_relaxed);
      if (!draws)
        continue;
      auto pct = [draws](uint64_t a) { return 100.0 * double(a) / double(draws); };
      part += fmt::format("{}vs N-{}: {} draws, state {:.1f} %, constants {:.1f} %, data unwritten {:.1f} %, all of it "
                          "{:.1f} %, {} of {} {} all the same",
                          part.empty() ? "" : "; ", k + 1, draws, pct(state), pct(constants), pct(data), pct(full),
                          maps_same, maps, shadow ? "maps" : "targets");
    }
    if (!part.empty())
      out += fmt::format("{}{}: {}", out.empty() ? "" : " | ", shadow ? "shadow maps (depth targets alone)" : "other targets",
                         part);
  }
  if (out.empty())
    return {};
  return fmt::format("repeat census (banjo_renut_diag_repeat_census, {} census frames): {}", pairs, out);
}

void ForgetVertexDeclarationLocked(u32 declaration_va) {
  if (declaration_va == g_last_declaration_va) {
    g_last_declaration_va = 0;
    g_last_declaration = nullptr;
  }
}

// banjo-nx (2026-10-26): drops the range copies a whole copy made kWholeReplaceFrames ago or more holds (g_whole_spans),
// those no draw used in the last kWholeReplaceFrames frames. One pass over the caches, only in a frame with such spans.
void DropReplacedRanges() {
  if (g_whole_spans.empty() || g_stream_cache_frame - g_whole_spans.front().frame < kWholeReplaceFrames)
    return;
  // kameo-nx (2026-11-04): every frame while past the limit (WholeCopyFits): 16 frames are 5 s at the 3 fps it is for.
  const bool over_limit = g_stream_cache_bytes > banjo::renut_gpu::StreamCacheMaxBytes(kStreamCacheMaxBytes);
  if (!over_limit && g_stream_cache_frame % kWholeReplacePeriod &&
      banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 54)
    return;  // banjo-nx (2026-10-28): kWholeReplacePeriod (A/B 54: every frame, as before)
  const auto ready = std::find_if(g_whole_spans.begin(), g_whole_spans.end(), [](const WholeSpan &w) {
    return g_stream_cache_frame - w.frame < kWholeReplaceFrames;
  });
  std::vector<WholeSpan> spans(g_whole_spans.begin(), ready);
  g_whole_spans.erase(g_whole_spans.begin(), ready);
  std::sort(spans.begin(), spans.end(), [](const WholeSpan &a, const WholeSpan &b) { return a.lo < b.lo; });
  // The spans whose range holds [lo, hi) with the same layout, stream and stride (a whole copy itself excluded).
  const auto held = [&spans](const u8 *lo, const u8 *hi, uint64_t layout_hash, u32 stream, u32 stride) {
    auto it = std::upper_bound(spans.begin(), spans.end(), lo, [](const u8 *v, const WholeSpan &w) { return v < w.lo; });
    for (int steps = 0; it != spans.begin() && steps < 16; ++steps) {
      const WholeSpan &w = *--it;
      if (hi <= w.hi && w.layout_hash == layout_hash && w.stream == stream && w.stride == stride &&
          !(lo == w.lo && hi == w.hi))
        return true;
    }
    return false;
  };
  uint64_t dropped = 0;
  const uint64_t frame = g_stream_cache_frame;
  bool whole_gone = false;  // banjo-nx (2026-10-28): a whole entry among them (index ones never are: excluded below)
  const size_t streams = std::erase_if(g_stream_cache, [&](auto &entry) {
    const StreamKey &k = entry.first;
    if (frame - entry.second.last_used < kWholeReplaceFrames ||
        !held(k.guest, k.guest + size_t(k.count) * k.stride, k.layout_hash, k.stream, k.stride))
      return false;
    dropped += entry.second.bytes;
    whole_gone |= entry.second.whole;
    DropCachedCopy(entry.second);
    return true;
  });
  const size_t indices = std::erase_if(g_index_cache, [&](auto &entry) {
    const IndexKey &k = entry.first;
    const u32 size = k.is32 ? 4u : 2u;
    if (k.primitive == kWholeIndexPrimitive || frame - entry.second.last_used < kWholeReplaceFrames ||
        !held(k.src, k.src + size_t(k.count) * size, 0, 0x100u | k.endian, size))
      return false;
    dropped += entry.second.bytes;
    DropCachedIndices(entry.second);
    return true;
  });
  if (streams || indices) {
    g_stream_memo.Clear();  // they may hold the entries dropped
    g_index_memo.Clear();
    // banjo-nx (2026-10-28, per-draw night): every answer voided only when a whole entry went (answers, mesh objects
    // and the last pointers hold whole entries alone).
    if (whole_gone || banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 60)
      ClearWholeLast();
  }
  g_replaced_bytes += dropped;
}

// bangaio-nx (2026-10-29, v5): the range entries TrackConvertedStream doomed since the last frame: erased, unless one
// made a copy, became a whole buffer's or saw the same bytes again since. Only those slots of the memo are forgotten
// (the answers, mesh objects and last pointers hold whole entries alone), and each range is marked as changing in the
// sightings table, so it gets no new entry while the mark holds. No draw holds an entry here (the frame's start).
void ForgetChangingRanges() {
  for (const StreamKey &key : g_doomed_ranges) {
    const auto it = g_stream_cache.find(key);
    if (it == g_stream_cache.end())
      continue;
    const CachedStream &c = it->second;
    if (c.copy || c.whole || c.changes < kRangeChanges)
      continue;
    const size_t hash = StreamKeyHash{}(key);
    g_stream_memo.Forget(hash, &*it);
    g_range_sightings.MarkChanging(hash);
    g_stream_cache.erase(it);
    ++g_ranges_forgotten;
  }
  g_doomed_ranges.clear();
}

// bangaio-nx (2026-10-29, v6): the range entries made kRangeAgeFrames ago or more, each looked at once. One with a copy
// (or a whole buffer's) is the idle pass's; one still counting stable kicks and drawn lately is looked at again later;
// any other is erased, its memo slot forgotten and its range marked as changing, as ForgetChangingRanges does. Such
// an entry holds no copy, only its count toward one: a range drawn with the same bytes at kStableFlushes kicks has its
// copy within these frames. The work is the entries made, not the map's size (no walk).
void AgeRanges() {
  size_t head = g_range_age_head;
  const size_t end = g_range_ages.size();
  while (head < end && g_range_ages[head].frame + kRangeAgeFrames <= g_stream_cache_frame) {
    const StreamKey key = g_range_ages[head++].key;
    const auto it = g_stream_cache.find(key);
    if (it == g_stream_cache.end())
      continue;
    const CachedStream &c = it->second;
    if (c.copy || c.whole)
      continue;
    if (c.stable && g_stream_cache_frame - c.last_used < kRangeAgeFrames) {
      g_range_ages.push_back({key, g_stream_cache_frame});
      continue;
    }
    const size_t hash = StreamKeyHash{}(key);
    g_stream_memo.Forget(hash, &*it);
    g_range_sightings.MarkChanging(hash);
    g_stream_cache.erase(it);
    ++g_ranges_forgotten;
  }
  // The looked-at front goes once it is half the vector (amortised: no reallocation a frame).
  if (head >= 4096 && head * 2 >= g_range_ages.size()) {
    g_range_ages.erase(g_range_ages.begin(), g_range_ages.begin() + std::ptrdiff_t(head));
    head = 0;
  }
  g_range_age_head = head;
}

void AdvanceStreamCacheFrameLocked() {
  banjo::renut_gpu::ResolveTraceFrameStart();
  SweepUnusedTargetsLocked();  // port 145 (tw2de_nx's, banjo_renut_keep_unused_targets): nothing kept when off
  ForgetChangingRanges();  // bangaio-nx (2026-10-29, v5)
  if (SecondSightingOn())
    AgeRanges();  // bangaio-nx (2026-10-29, v6)
  // banjo-nx (2026-10-19): banjo_renut_store_cards_verify's diagnostics, every 10 s.
  if (banjo::renut_gpu::StoreCardsVerifyOn()) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(10)) {
      last = now;
      uint64_t host[3] = {};
#if !defined(__SWITCH__)
      for (int i = 0; i < 3; ++i)
        host[i] = RexStoreCardsHostBytes(i);
#endif
      // Written cards in each view's part of the table (0: written): A, C, E (512 MB each) and 7F (16 MB).
      uint32_t zero[4] = {};
      if (const u8 *cards = StoreCards()) {
        for (u32 i = 0; i < 0x20000u; ++i) {
          zero[0] += cards[0xA0000u + i] == 0;
          zero[1] += cards[0xC0000u + i] == 0;
          zero[2] += cards[0xE0000u + i] == 0;
        }
        for (u32 i = 0; i < 0x1000u; ++i)
          zero[3] += cards[0x7F000u + i] == 0;
      }
      std::string samples = fmt::format(" (written cards now: A {} C {} E {} 7F {} of 131072/131072/131072/4096)",
                                        zero[0], zero[1], zero[2], zero[3]);
      for (uint32_t i = 0; i < g_card_sample_count; ++i)
        samples += fmt::format(" {:05X}{}{}{}{}", g_card_samples[i] & 0xFFFFFFF, (g_card_samples[i] >> 28) & 1 ? "a" : "",
                               (g_card_samples[i] >> 29) & 1 ? "c" : "", (g_card_samples[i] >> 30) & 1 ? "e" : "",
                               (g_card_samples[i] >> 31) & 1 ? "f" : "");
      RGPU_INFO("[cards] 10 s: {} pages read, {} found written (view A {}, C {}, E {}, 7F {}); {} copies written only "
                "through the page before; {} hashed after a written page with their bytes unchanged; host writers "
                "marked {:.1f} MB of the physical views and {:.1f} MB elsewhere in {} calls; {} checks of memory outside the "
                "physical views; first pages:{}",
                g_card_diag[0], g_card_diag[1], g_card_diag[2], g_card_diag[3], g_card_diag[4], g_card_diag[5],
                g_card_diag[6], g_card_diag[7], double(host[0]) / double(1u << 20), double(host[1]) / double(1u << 20),
                host[2], g_card_diag[8], samples);
      std::fill(std::begin(g_card_diag), std::end(g_card_diag), 0);
      g_card_sample_count = 0;
    }
  }
  g_trace_draws = 0;
  // Copies whose guest bytes changed (ContentVerifier), unless made again
  // since they were checked.
  static std::vector<decltype(g_stream_verifier)::Check> stale;
  g_stream_verifier.Flush();
  g_stream_verifier.TakeStale(stale);
  for (const auto &check : stale) {
    auto it = g_stream_cache.find(check.key);
    if (it == g_stream_cache.end())
      continue;
    CachedStream &c = it->second;
    if (!c.copy || c.hash != check.hash)
      continue;
    DropCachedCopy(c);
    c.stable = 0;
    c.checked_frame = UINT64_MAX;
    c.dynamic = ++c.drops >= kMaxCacheDrops;
    banjo::renut_gpu::CountLateStale(1);  // banjo-nx: drawn from its old copy until now
  }
  stale.clear();

  static std::vector<decltype(g_index_verifier)::Check> stale_indices;
  g_index_verifier.Flush();
  g_index_verifier.TakeStale(stale_indices);
  for (const auto &check : stale_indices) {
    auto it = g_index_cache.find(check.key);
    if (it == g_index_cache.end())
      continue;
    CachedIndices &c = it->second;
    if (!c.copy || c.hash != check.hash)
      continue;
    DropCachedIndices(c);
    c.stable = 0;
    c.checked_frame = UINT64_MAX;  // hashed again by TrackIndices
    c.dynamic = ++c.drops >= kMaxCacheDrops;
    banjo::renut_gpu::CountLateStale(1);  // banjo-nx: drawn from its old copy until now
  }
  stale_indices.clear();

  DropReplacedRanges();
  // sasrt-nx (2026-10-21, sasrt_renut_exports_held): kept exports four frames old go (a game exporting its skinned meshes every
  // frame, in two buffers by turns).
  for (auto it = g_held_exports.begin(); it != g_held_exports.end();) {
    if (g_stream_cache_frame - it->second.frame > 4) {
      g_held_retired.push_back(std::move(it->second.data));
      it = g_held_exports.erase(it);
    } else {
      ++it;
    }
  }
  if (g_held_reads && banjo::renut_gpu::ExportsHeldOn() && g_stream_cache_frame % 300 == 0) {
    RGPU_INFO("[renut] vertex streams read from kept export results (sasrt_renut_exports_held): {} in 300 frames; {} kept",
              g_held_reads, g_held_exports.size());
    g_held_reads = 0;
  }
  if (++g_stream_cache_frame % 60)
    return;
  // banjo-nx: banjo_renut_memory_policy's idle time (Renut's kStreamCacheIdleFrames without it), or under pressure
  // (copies refused at the cap since the last pass, banjo_renut_stream_cache_pressure) kPressureIdleFrames.
  uint64_t idle_frames = banjo::renut_gpu::StreamCacheIdleFrames(kStreamCacheIdleFrames);
  const uint64_t refused = g_stream_cache_refused;
  g_stream_cache_refused = 0;
  // banjo-nx (2026-10-28, the heap): also when the heap is short (its room below 256 MB, as the prebuilt pipelines'
  // rule): copies made again when needed are better than a heap that runs out (the d4 crash).
  bool heap_short = false;
#if defined(__SWITCH__)
  {
    const char *top = static_cast<const char *>(sbrk(0));
    const uint64_t room = (top && fake_heap_end > top) ? uint64_t(fake_heap_end - top) : 0;
    heap_short = room < (uint64_t(256) << 20);
  }
#endif
  const bool pressure =
      (refused || heap_short) && banjo::renut_gpu::StreamCachePressureOn() && idle_frames > kPressureIdleFrames;
  const uint64_t bytes_before = g_stream_cache_bytes;
  if (pressure)
    idle_frames = kPressureIdleFrames;
  // sasrt-nx (2026-11-09, sasrt_memory_governor; port 58): critical: the copies no draw read for 60 frames go (made
  // again from guest memory when drawn)
  if (banjo::renut_gpu::MemoryCritical() && banjo::renut_gpu::StreamCachePressureOn() && idle_frames > 60)
    idle_frames = 60;
  bool whole_gone = false;  // banjo-nx (2026-10-28): a whole entry among them (see DropReplacedRanges)
  // bangaio-nx (2026-10-29, v5, banjo_renut_stream_second_sighting): a range entry with no copy holds only its count of
  // stable kicks: it goes after kRangeIdleFrames unused, not idle_frames (20 s), and only its memo slot is forgotten.
  // Bangai-O's sprite ranges move: most are drawn in a frame or two and never again, and the map kept every one for
  // 20 s (108,892 entries in the v4 console storm, walked here and every 16 frames by DropReplacedRanges).
  const bool short_idle = SecondSightingOn();
  size_t ranges_gone = 0;
  const size_t streams_gone =
      std::erase_if(g_stream_cache, [idle_frames, short_idle, &whole_gone, &ranges_gone](auto &entry) {
        const CachedStream &c = entry.second;
        const uint64_t idle = g_stream_cache_frame - c.last_used;
        if (idle < idle_frames) {
          if (!short_idle || idle < kRangeIdleFrames || c.copy || c.whole)
            return false;
          g_stream_memo.Forget(StreamKeyHash{}(entry.first), &entry);
          ++ranges_gone;
          return true;
        }
        whole_gone |= c.whole;
        DropCachedCopy(entry.second);
        return true;
      });
  g_ranges_forgotten += ranges_gone;
  // sasrt-nx (2026-11-10, sasrt_stream_cache_drain; port 58; SASRT's run mem14: 104-146 MB of copies in a 288 MB pool, a
  // chunk going back only once every copy in it has gone, and the copies spread over all of them). With the pool past
  // 1.5 times its copies and 64 MB, its sparsest chunk is drained: new copies go elsewhere (CopyPool::Allocate takes it
  // last) and its copies no draw used in the last frame are dropped at each pass (their entries stay: a stream is
  // converted into the upload ring meanwhile and copied again when it holds, as after any drop); empty, the chunk goes
  // back (ReleaseSlot). Whole-buffer copies (objects hold them) stay; a drain not done in 10 passes is given up. The
  // same pixels. SASRT: one chunk back in its first run, the pool refilled (copies spread with small holes): weak.
  {
    static const bool drain_on = REXCVAR_GET(sasrt_stream_cache_drain);
    CopyPool &pool = g_copy_pool;
    if (drain_on && pool.draining_ == UINT32_MAX &&
        pool.Bytes() > g_stream_cache_bytes + g_stream_cache_bytes / 2 + (uint64_t(64) << 20)) {
      pool.draining_ = pool.SparsestChunk();
      pool.drain_passes_ = 0;
    }
    if (pool.draining_ != UINT32_MAX) {
      uint64_t dropped = 0;
      for (auto &[skey, c] : g_stream_cache) {
        if (c.copy && c.copy.chunk == pool.draining_ && !c.whole && g_stream_cache_frame - c.last_used >= 1) {
          dropped += c.bytes;
          DropCachedCopy(c);
        }
      }
      for (auto &[ikey, c] : g_index_cache) {
        if (c.copy && c.copy.chunk == pool.draining_ && ikey.primitive != kWholeIndexPrimitive &&
            g_stream_cache_frame - c.last_used >= 1) {
          dropped += c.bytes;
          DropCachedIndices(c);
        }
      }
      static uint32_t drain_logged = 0;
      if (dropped && drain_logged++ < 16)
        RGPU_INFO("[renut] stream cache: chunk {} drained ({:.1f} MB of copies dropped; {:.0f} MB of copies in a {:.0f} MB "
                  "pool; {} chunks given back so far by draining, sasrt_stream_cache_drain)", pool.draining_,
                  double(dropped) / 1048576.0, double(g_stream_cache_bytes) / 1048576.0,
                  double(pool.Bytes()) / 1048576.0, pool.drained_);
      if (++pool.drain_passes_ >= 10)
        pool.draining_ = UINT32_MAX;  // whole copies or ones used every frame keep it: given up
    }
  }
  const size_t indices_gone = std::erase_if(g_index_cache, [idle_frames, &whole_gone](auto &entry) {
    if (g_stream_cache_frame - entry.second.last_used < idle_frames)
      return false;
    whole_gone |= entry.first.primitive == kWholeIndexPrimitive;
    DropCachedIndices(entry.second);
    return true;
  });
  // The memos, the last-copy pointers and every mesh and geometry object's answer name entries: cleared when entries
  // went. banjo-nx (2026-10-28, per-draw night): only then (they were cleared at every pass, before it: the frame after
  // each pass, a frame in sixty, took every draw's geometry the full way). Nothing reads them in between.
  if (streams_gone > ranges_gone || indices_gone) {  // bangaio-nx: the short-idle ones forgot their own slots
    g_stream_memo.Clear();
    g_index_memo.Clear();
    if (whole_gone || banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 60)  // banjo-nx (2026-10-28)
      ClearWholeLast();
  }
  DropIdleFetchCopies(idle_frames);  // banjo-nx (2026-10-27, banjo_renut_fetch_cache)
  banjo::renut_gpu::CountStreamCachePressure(refused, pressure ? bytes_before - g_stream_cache_bytes : 0);
}

StreamCacheMemory StreamCacheMemoryLocked() {
  StreamCacheMemory usage;
  usage.pool_bytes = g_copy_pool.Bytes();
  usage.cached_bytes = g_stream_cache_bytes;
  usage.streams = g_stream_cache.size();
  usage.index_ranges = g_index_cache.size();
  return usage;
}

namespace {

// Clears on explicit targets (framebuffer bound as for a draw). A color clear
// reaches the targets over the same EDRAM in other host formats too
// (ColorAliasesLocked): on the console the clear writes the memory, whatever
// format a surface reads it as. N&B's paint shop clears its glow buffer
// through the 8-bit view of that EDRAM, then adds the glow particles through a
// 16-bit float view; the float target kept the frames before, and the glow
// piled up into drips.
void ClearTargets(const Targets &t, u32 flags, const plume::RenderRect &rect, const float color[4],
                  float depth, u32 stencil) {
  if (banjo::renut_gpu::ResolveTraceOn()) {
    RGPU_INFO("[trace] clear {:X} rect ({},{})-({},{}) of {}x{}", flags, rect.left, rect.top, rect.right, rect.bottom,
              t.width, t.height);
  }
  if (rect.right <= rect.left || rect.bottom <= rect.top)
    return;
  // banjo-nx (2026-10-21, internal resolution): colour and depth targets of different scales (the HUD's UI target with
  // the scene's depth, the post-processing buffers at 1x with it) cleared one at a time, each at its own scale.
  if (t.depth && t.color_count && t.depth->scale != t.scale) {
    Targets colors = t;
    colors.depth = nullptr;
    if (flags & 0xF)
      ClearTargets(colors, flags & 0xF, rect, color, depth, stencil);
    if (flags & 0x30) {
      Targets d;
      d.depth = t.depth;
      d.width = t.depth->width;
      d.height = t.depth->height;
      d.scale = t.depth->scale;
      d.guest_width = d3d::SurfaceWidth(t.depth->size_word);
      d.guest_height = d3d::SurfaceHeight(t.depth->size_word);
      const plume::RenderRect depth_rect(
          std::min(rect.left, int32_t(d.width)), std::min(rect.top, int32_t(d.height)),
          std::min(rect.right, int32_t(d.width)), std::min(rect.bottom, int32_t(d.height)));
      ClearTargets(d, flags & 0x30, depth_rect, color, depth, stencil);
    }
    return;
  }
  // renut-ultimate (2026-11-13, sasrt-nx's sasrt_gpu_diag_resolve_split): a depth clear's GPU time under its own label
  // (SASRT's cascades' and scene depth's clears are full-bandwidth writes on deko3d whatever the image's state).
  if (t.depth && (flags & 0x30) && SasrtResolveSplitOn()) [[unlikely]] {
    static std::unordered_map<uint64_t, uint32_t> labels;
    const uint64_t k = uint64_t(t.depth->width) | uint64_t(t.depth->height) << 16 | uint64_t(flags & 0x30) << 32;
    uint32_t &label = labels[k];
    if (!label)
      label = gpu_profile::Label(fmt::format("clears | depth {}x{}{}", t.depth->width, t.depth->height,
                                             (flags & 0x30) == 0x30 ? " and stencil" : "")) + 1;
    gpu_profile::Mark(State(), label - 1);
  }
  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover): before the target is bound for the clear.
  if (t.depth && t.depth->handed)
    DepthHandOverBeforeBindLocked(t.depth, &rect, flags);
  // kameo-nx (2026-11-03, banjo_deko3d_color_handover): a colour target this clear fills: bit 0 of 'clear_flags' is its own.
  for (uint32_t i = 0; i < t.color_count; ++i) {
    if (t.colors[i]->handed) [[unlikely]]
      DepthHandOverBeforeBindLocked(t.colors[i], &rect, (flags >> i) & 1u);
  }
  if (t.depth && (flags & 0x20)) {  // sasrt-nx's SasrtWarnStencilOnD32f inside it (rr6-nx, 2026-11-09)
    const bool first_stencil = !t.depth->stencil_used;  // sasrt-nx (2026-11-11, port 75; renut_diag_formats)
    t.depth->stencil_used = true;
    SasrtWarnStencilOnD32f(t.depth, true, first_stencil);  // a clear
  }
  if (!BindTargets(t))
    return;
  plume::RenderCommandList *cmd = State().command_list;
  const plume::RenderRect host_rect = ScaleRect(rect, t.scale);
  const plume::RenderColor clear_color(color[0], color[1], color[2], color[3]);
  // banjo-nx (2026-10-19, render-thread proposal 20): scratch vectors kept between clears (render thread only).
  static std::vector<HostSurface *> aliases;
  static std::vector<HostSurface *> found;
  aliases.clear();
  // banjo-nx (2026-10-28, banjo_renut_clear_repeat; from sa2-nx's sa2_renut_clear_repeat, credit sa2-nx): a whole clear
  // of a target that holds exactly that clear already (its last whole clear had the same values, bit for bit, and neither
  // a write nor another image since: writes and generation as that clear left them) is not sent; everything else about
  // it is as before (its writes counted, the targets bound, the aliases cleared). What writes a target's image: draws
  // and clears (writes), SMAA and the UI scale (writes), a depth hand-over or a rebuild (generation: every image has its
  // own, g_next_generation).
  const bool repeat_on = ClearRepeatOn();
  const auto whole = [&](const HostSurface *s) {
    return rect.left <= 0 && rect.top <= 0 && rect.right >= int32_t(s->width) && rect.bottom >= int32_t(s->height);
  };
  for (uint32_t i = 0; i < t.color_count; ++i) {
    if (!(flags & (1u << i)))
      continue;
    HostSurface *c = t.colors[i];
    const bool is_whole = whole(c);
    const bool repeat = repeat_on && is_whole && c->clear_known && c->clear_generation == c->generation &&
                        c->clear_writes == c->writes && std::memcmp(c->clear_color, color, sizeof(c->clear_color)) == 0;
    ++c->writes;  // banjo-nx (banjo_renut_resolve_skip)
    NoteSiblingClear(*c, host_rect);  // vivapinata-nx (2026-11-11, vivapinata_resolve_from_sibling)
    c->write_seq = ++g_surface_write_seq;  // sasrt-nx: the EDRAM depth restore
    g_blur_color_write_last = c->write_seq;  // blur-nx (2026-11-11, port 76; its key 220)
    if (EdramCarryOn()) [[unlikely]]
      NoteEdramCarryClearLocked(c);  // port 179 (from fnxf_xiii; banjo_renut_edram_carry)
    g_clears_whole += is_whole;
    if (repeat)
      ++g_clear_repeats;
    else
      cmd->clearColor(i, clear_color, &host_rect, 1);
    c->clear_known = is_whole;
    c->clear_generation = c->generation;
    c->clear_writes = c->writes;
    std::memcpy(c->clear_color, color, sizeof(c->clear_color));
    ColorAliasesLocked(t.colors[i], found);
    for (HostSurface *alias : found) {
      if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end() &&
          std::find(t.colors, t.colors + t.color_count, alias) == t.colors + t.color_count)
        aliases.push_back(alias);
    }
    // vivapinata-nx (2026-11-04, the white fog at the cuts): noted, and done only where a resolve reads it (ResolveStateLocked).
    if (banjo::renut_gpu::EdramClearOverlapOn()) {
      ColorEdramWipedLocked(c, rect, found);
      for (HostSurface *wiped : found) {
        wiped->edram_wiped = true;
        wiped->blur_wiped_seq = g_surface_write_seq;  // blur-nx (2026-11-10, port 76; its key 218)
        wiped->edram_wiped_generation = wiped->generation;
        wiped->edram_wiped_writes = wiped->writes;
        std::memcpy(wiped->edram_wiped_color, color, sizeof(wiped->edram_wiped_color));
      }
    }
  }
  if (t.depth && (flags & 0x30)) {
    HostSurface *d = t.depth;
    const bool zc = (flags & 0x10) != 0, sc = (flags & 0x20) != 0;
    const bool is_whole = whole(d);
    const bool same = is_whole && d->clear_generation == d->generation && d->clear_writes == d->writes;
    // Only when every plane cleared (depth and/or stencil) is known to hold that value.
    const bool repeat = repeat_on && same &&
                        (!zc || (d->clear_depth_known && std::memcmp(&d->clear_depth, &depth, sizeof(depth)) == 0)) &&
                        (!sc || (d->clear_stencil_known && d->clear_stencil == (stencil & 0xFF)));
    ++d->writes;  // banjo-nx (banjo_renut_resolve_skip)
    d->write_seq = ++g_surface_write_seq;  // sasrt-nx: the EDRAM depth restore
    g_clears_whole += is_whole;
    if (repeat)
      ++g_clear_repeats;
    else
      cmd->clearDepthStencil(zc, sc, depth, stencil & 0xFF, &host_rect, 1);
    // banjo-nx (banjo_renut_clear_repeat): what the target holds now; a plane this clear left alone keeps what was known
    // of it only while the image and its writes are the ones that knowledge was of.
    if (!is_whole) {
      d->clear_depth_known = d->clear_stencil_known = false;
    } else {
      if (!same)
        d->clear_depth_known = d->clear_stencil_known = false;
      if (zc) {
        d->clear_depth_known = true;
        d->clear_depth = depth;
      }
      if (sc) {
        d->clear_stencil_known = true;
        d->clear_stencil = stencil & 0xFF;
      }
    }
    d->clear_generation = d->generation;
    d->clear_writes = d->writes;
  }
  for (HostSurface *alias : aliases) {
    Targets a;
    a.colors[a.color_count++] = alias;
    a.width = alias->width;
    a.height = alias->height;
    a.scale = alias->scale;
    const plume::RenderRect alias_rect(
        std::min(rect.left, int32_t(alias->width)), std::min(rect.top, int32_t(alias->height)),
        std::min(rect.right, int32_t(alias->width)), std::min(rect.bottom, int32_t(alias->height)));
    if (alias_rect.right <= alias_rect.left || alias_rect.bottom <= alias_rect.top || !BindTargets(a))
      continue;
    const plume::RenderRect alias_host = ScaleRect(alias_rect, a.scale);
    ++alias->writes;  // banjo-nx (banjo_renut_resolve_skip)
    NoteSiblingClear(*alias, alias_host);  // vivapinata-nx (2026-11-11, vivapinata_resolve_from_sibling)
    alias->clear_known = false;  // banjo-nx (banjo_renut_clear_repeat): not followed through aliases
    cmd->clearColor(0, clear_color, &alias_host, 1);
    if (EdramCarryOn()) [[unlikely]]
      alias->carry_cleared_frame = TextureFrameLocked();  // port 179: cleared through another view: nothing carried in
  }
}

// Depth resolve (shaders/host/depth_resolve.hlsli): 'src' of the depth target
// to ('dest_x', 'dest_y') of the destination and of its guest words, in guest
// pixels (the destination is of the surface's scale).
bool ResolveDepth(HostSurface *surface, HostTexture *dest, const ClearRect &src, i32 dest_x,
                  i32 dest_y, bool words = true) {
  const RenderScale scale = surface->scale;
  auto &s = State();
  const uint32_t variant = surface->samples != plume::RenderSampleCount::COUNT_1 ? 1 : 0;
  const uint32_t source_slot = SurfaceSampleSlotLocked(surface);
  if (!s.depth_resolve_float_pipeline[variant])
    words = true;  // banjo-nx (banjo_renut_depth_words_lazy): no depth-only pipeline
  if (source_slot == kInvalidDescriptor || !dest->depth_framebuffer ||
      !s.depth_resolve_pipeline[variant]) {
    return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  static const uint32_t kProfileLabel = gpu_profile::Label("depth resolves");
  gpu_profile::Mark(s, kProfileLabel);
  const bool split = SasrtResolveSplitOn();  // renut-ultimate (2026-11-13, sasrt_gpu_diag_resolve_split)
  if (split) [[unlikely]]
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 0));
  TransitionSurfaceLocked(surface, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::COLOR_WRITE);
  TransitionTextureLocked(dest->words.get(), plume::RenderTextureLayout::COLOR_WRITE);
  if (split) [[unlikely]] {
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 1));
    gpu_profile::CountDraw(s, SasrtResolvePartLabel(surface, 1));  // its resolves a frame
  }
  cmd->setFramebuffer(dest->depth_framebuffer.get());
  // Pixel coordinates are destination texels.
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest->alloc_width),
                                          float(dest->alloc_height)));
  cmd->setScissors(ScaleRect(plume::RenderRect(dest_x, dest_y, dest_x + (src.x2 - src.x1),
                                               dest_y + (src.y2 - src.y1)),
                             surface->scale));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  // sasrt-nx (2026-11-08, sasrt_gpu_depth_resolve_float_ps; the second GPU round): the depth-only resolve with the
  // shader that computes the float alone (the words target is write-masked in both).
  if (!words && variant == 0 && s.sasrt_depth_float_pipeline &&
      sasrt::gpu_round::On(sasrt::gpu_round::kDepthFloatPs)) {
    cmd->setPipeline(s.sasrt_depth_float_pipeline.get());
    sasrt::gpu_round::Count(sasrt::gpu_round::kDepthFloatPs);
  } else {
    cmd->setPipeline(words ? s.depth_resolve_pipeline[variant].get() : s.depth_resolve_float_pipeline[variant].get());
  }
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 2);  // banjo-nx: the stencil view, read as uint
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.translate[0] = float(scale.Coord(src.x1) - scale.Coord(dest_x));
  push.translate[1] = float(scale.Coord(src.y1) - scale.Coord(dest_y));
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  // banjo-nx (2026-10-18, banjo_renut_depth_words_stencil): the stencil in the words' low byte (depth_resolve.hlsli),
  // which N&B's depth of field reads to keep the characters sharp. The resolve reads no sampler: the index is the
  // stencil view's slot.
  if (banjo::renut_gpu::DepthWordsStencilOn() && words) {
    if (const uint32_t stencil_slot = SurfaceStencilSlotLocked(surface); stencil_slot != kInvalidDescriptor) {
      push.sampler_index = stencil_slot;
      push.param1 = 1.0f;
    }
  }
  // D3DFMT_D24FS8 (GPU texture format 24_8_FLOAT) stores 20e4 float depth.
  push.param0 = GpuTextureFormat(surface->format_word & 0x3F) == GpuTextureFormat::Fmt24_8_Float
                    ? 1.0f
                    : 0.0f;
#if defined(PLUME_DEKO3D)
  // katamari-nx (2026-10-30): the multisampled variant's deko3d shader reads the depth target (binding 0) and its
  // stencil view (binding 1) as BOUND textures (tools/deko3d_glsl.py). Bindless, the multisampled fetch was one Ryujinx's
  // shader translator cannot follow ("Failed to find handle source for bindless access of type texture2DMS"): it read
  // 0, and Beautiful Katamari's depth of field, which reads this resolve of its 4x depth, blurred the whole scene.
  if (variant == 1 && IsDeko3D()) {
    const uint32_t slots[2] = {source_slot, push.param1 != 0.0f ? push.sampler_index : source_slot};
    plume::Deko3DBindFragmentTextures(cmd, 0, slots, 2);
  }
#endif
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  if (split) [[unlikely]]
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 2));
  TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);
  if (split) [[unlikely]]  // the move to shader read apart from the clear and what follows
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 3));

  // The draw path binds its own layout, pipeline, targets and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
  return true;
}

// ---------------------------------------------------------------------------
// Depth hand-over (banjo-nx, 2026-10-19, banjo_deko3d_depth_handover)
// ---------------------------------------------------------------------------
// A depth resolve of a whole single-sampled depth target into a destination of its size, without the words
// (banjo_renut_depth_words_lazy), is not drawn: the destination's content stays in the target's image (handed_from,
// with the generation and writes it had), and a bind of the destination samples that image through a depth view with
// the destination's component mapping. The float the resolve writes is the depth it loads (depth_resolve.hlsli), so the
// shaders read the same values. Before the target is bound again (a draw or a clear: BindTargets), or goes, the
// resolve is drawn after all (MaterialiseLocked), from the image still unchanged; so is it for a draw that has the
// target as its own depth target. A clear of the target's whole depth (and its stencil, or of a target whose stencil
// nothing uses) instead gives the image to the destination (held) and the destination's previous one, or a new one,
// to the target, which the clear then fills. The shadow maps are resolved once a frame, sampled by the scene and
// cleared whole at the start of the next frame's shadow pass: they are never copied.

// kameo-nx (2026-11-03, banjo_deko3d_color_handover): the same for a colour resolve (HostTexture::handed_color): whole,
// single-sampled, the destination of the target's size and format, one level. A texel the destination would sample is the
// texel of the target's image, through a view of the same format and the destination's mapping, because DrawResolveCopy
// writes every texel back with its bits (its formats only). The resolve is drawn after all before a draw writes the target,
// before anything other than a whole clear binds it, and before it goes. A whole clear of the target gives the image to the
// destination instead: the render-to-texture loop's resolve-and-clear and the frame's last resolve are never copied.
// blur-nx (2026-11-10, port 76; its keys 212 and 219): 'convert', a destination ResolveTarget kept in another format;
// 'color_scale', the resolve's exponent bias as 2^bias.
bool DrawResolveCopy(HostSurface *surface, HostTexture *dest, const plume::RenderRect &rect, uint32_t host_x,
                     uint32_t host_y, bool convert = false, float color_scale = 1.0f);
// blur-nx (2026-11-10, port 76; its key 212: blur_renut_resolve_keep_dest): colour formats a converting draw writes and
// reads exactly (textures.cpp has the same list for ResolveTarget; R10G10B10A2_UNORM: port 75's 2_10_10_10).
static bool ConvertibleResolveFormat(plume::RenderFormat f) {
  switch (f) {
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R10G10B10A2_UNORM:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R16_FLOAT:
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
    return true;
  default:
    return false;
  }
}
bool ResolveCopyPossible(const HostSurface *surface, const HostTexture *dest);

std::vector<HostTexture *> g_handed;  // destinations whose content is a target's current image

void CountHanded(const HostTexture &d, int what) {
  if (d.handed_color)
    banjo::renut_gpu::CountColorHandOver(what);
  else
    banjo::renut_gpu::CountHandOver(what);
}

// The resolve left out, drawn from 'source' (the target, or the taken image as a target).
bool DrawHandedResolveLocked(HostSurface *source, HostTexture &d, const ClearRect &src, bool words) {
  if (!d.handed_color)
    return ResolveDepth(source, &d, src, d.last_dest_x, d.last_dest_y, words);
  if (!DrawResolveCopy(source, &d, plume::RenderRect(0, 0, int32_t(d.alloc_width), int32_t(d.alloc_height)), 0, 0))
    return false;
  TransitionTextureLocked(&d, plume::RenderTextureLayout::SHADER_READ);
  return true;
}

void UnhandLocked(HostTexture &d) {
  if (!d.handed_from)
    return;
  BumpTextureBindings(d);  // banjo-nx (2026-10-27): its bindings' answer changes
  if (d.handed_from->handed)
    --d.handed_from->handed;
  d.handed_from = nullptr;
  std::erase(g_handed, &d);
}

// Draws the resolve left out into the destination's own targets, from where its content is.
bool MaterialiseLocked(HostTexture &d, bool words) {
  ++g_texture_epoch;
  BumpTextureBindings(d);  // banjo-nx (2026-10-27)
  const ClearRect src{d.last_src[0], d.last_src[1], d.last_src[2], d.last_src[3]};
  if (HostSurface *source = d.handed_from) {
    const bool same = source->texture && source->generation == d.handed_generation &&
                      source->writes == d.handed_writes;
    UnhandLocked(d);
    if (same && DrawHandedResolveLocked(source, d, src, words)) {
      d.words_current = words;
      CountHanded(d, 3);
      CountHanded(d, g_materialise_cause);  // banjo-nx (2026-10-28, diagnostic)
      return true;
    }
    CountHanded(d, 5);
    return false;
  }
  if (!d.held || !d.held_current)
    return false;
  d.held_current = false;
  // The taken image as the target it was taken from, for ResolveDepth.
  HostSurface image;
  image.width = d.width;
  image.height = d.height;
  image.scale = d.scale;
  image.depth = !d.handed_color;
  image.format = d.held->format;
  image.samples = d.held->samples;
  image.format_word = d.held->format_word;
  SwapDepthImageLocked(image, *d.held);
  const bool ok = DrawHandedResolveLocked(&image, d, src, words);
  SwapDepthImageLocked(image, *d.held);
  if (ok) {
    d.words_current = words;
    CountHanded(d, 3);
    CountHanded(d, 4);
  } else {
    CountHanded(d, 5);
  }
  return ok;
}

// blur-nx (2026-11-10, port 76; its key 212: blur_renut_resolve_keep_dest): how many pixels a single-sampled colour
// resolve of 'surface' writes (its rect within the surface), for ResolveTarget; 0 for a multisampled source (resolved by
// the API, which wants one format) or an empty rect.
uint32_t ResolveCoverPixels(const HostSurface *surface, const ClearRect *source_rect) {
  if (surface->samples != plume::RenderSampleCount::COUNT_1)
    return 0;
  int32_t w = int32_t(surface->width), h = int32_t(surface->height);
  if (source_rect) {
    const int32_t x1 = std::clamp(source_rect->x1, 0, w), y1 = std::clamp(source_rect->y1, 0, h);
    const int32_t x2 = std::clamp(source_rect->x2, x1, w), y2 = std::clamp(source_rect->y2, y1, h);
    w = x2 - x1;
    h = y2 - y1;
  }
  return w > 0 && h > 0 ? uint32_t(w) * uint32_t(h) : 0;
}

// blur-nx (2026-11-17, G1: the colour hand-over in Blur): ResolveTarget draws this resolve from the view's sample grid
// (blur_renut_sample_grid_resolve, port 76's key 216; the same test as there): the console's resolve averages the samples
// of a 4x (2x) view, which are the 2x2 (1x2) blocks of the single-sampled target of twice its pitch, drawn after the view.
// The view's own image is not what that resolve reads, so the resolve is never left in it. Blur's 640x360 half-size scene,
// the motion blur's source, is such a view over its 1280x720 post target: left in the view's image it was black, and with
// the colour hand-over on (sasrt_gpu_handover_with_smaa, or SMAA off) the race darkened with speed and the mirror went
// black in Ryujinx, and the console flickered at the mirror and the car's back.
// The grid's target that resolve reads, or null (a level-0, slice-0 resolve, as ResolveTarget's grid path).
const HostSurface *SampleGridSourceLocked(const HostSurface *s, const HostTexture *d) {
  if (s->depth || !banjo::renut_gpu::SampleGridResolveOn() || d->host_format != s->format ||
      ((u32(s->info_word) >> 16) & 3) == 0)
    return nullptr;
  const HostSurface *grid = SampleGridAliasLocked(s);
  return grid && grid->blur_draw_seq > s->blur_draw_seq ? grid : nullptr;
}
bool SampleGridResolveWantedLocked(const HostSurface *s, const HostTexture *d) {
  return SampleGridSourceLocked(s, d) != nullptr;
}

// A resolve into 'd' left in the target's image when it can be (true). Otherwise (false, the resolve is drawn) a
// destination still holding other content gets it first when the resolve does not cover it.
bool HandOverResolveLocked(HostSurface *s, HostTexture *d, const ClearRect &src, i32 dest_x, i32 dest_y,
                           bool words) {
  const bool whole = src.x1 == 0 && src.y1 == 0 && src.x2 == int32_t(s->width) && src.y2 == int32_t(s->height) &&
                     dest_x == 0 && dest_y == 0;
  const bool kind_on = s->depth ? banjo::renut_gpu::DepthHandOverOn() && !words
                                : banjo::renut_gpu::ColorHandOverOn() &&
                                      (!banjo::renut_gpu::SmaaOn() ||  // sasrt-nx (2026-11-08, sasrt_gpu_handover_with_smaa)
                                       sasrt::gpu_round::On(sasrt::gpu_round::kHandoverSmaa)) &&
                                      ResolveCopyPossible(s, d) &&
                                      !(s->sasrt_fixed16 && banjo::renut_gpu::Fixed16ClampOn()) &&  // its resolve clamps
                                      !detail::ReadbackDestination(d);  // the CPU reads it (sasrt_renut_resolve_readback)
  bool can = IsDeko3D() && kind_on && whole && s->texture &&
             s->samples == plume::RenderSampleCount::COUNT_1 && d->width == s->width &&
             d->height == s->height && d->scale == s->scale && d->alloc_width == s->scale.Size(s->width) &&
             d->alloc_height == s->scale.Size(s->height) && d->dimension == TextureDimension::Tex2D &&
             !d->stacked && d->extra_levels == 0 && d->mip_levels == 1;
  // blur-nx (2026-11-17): a resolve drawn from the view's sample grid is not left in the view's image (above). Looked at
  // only for a resolve that could be left there otherwise (the hand-over on: G1 with SMAA, or SMAA off).
  if (can && !s->depth && SampleGridResolveWantedLocked(s, d)) [[unlikely]] {
    can = false;
    static u32 logged = 0;
    if (LogLimited(logged, 4))
      RGPU_INFO("[renut] colour hand-over: the resolve of a {}x{} {}x view is drawn from its sample grid, not left in the "
                "view's image (blur_renut_sample_grid_resolve)", s->width, s->height,
                ((u32(s->info_word) >> 16) & 3) == 2 ? 4 : 2);
  }
  // kameo-nx (2026-11-03, diagnostic): why a colour resolve was drawn (the colour hand-over's 10 s line).
  if (!can && !s->depth) {
    const bool same_size = d->width == s->width && d->height == s->height && d->scale == s->scale;
    banjo::renut_gpu::CountColorNotLeft(!whole ? (same_size ? 1 : 0) : !same_size ? 2
                                        : d->extra_levels != 0 || d->mip_levels != 1 ? 3
                                                                                     : 4);
  }
  if (!can) {
    if (d->handed_from || d->held_current) {
      const bool covers = dest_x == 0 && dest_y == 0 && src.x2 - src.x1 >= int32_t(d->width) &&
                          src.y2 - src.y1 >= int32_t(d->height);
      if (covers) {
        UnhandLocked(*d);
        d->held_current = false;
      } else {
        MaterialiseLocked(*d, false);
      }
    }
    return false;
  }
  UnhandLocked(*d);  // what it held is overwritten
  d->held_current = false;
  d->handed_from = s;
  d->handed_generation = s->generation;
  d->handed_writes = s->writes;
  d->handed_color = !s->depth;
  ++s->handed;
  g_handed.push_back(d);
  ++g_texture_epoch;
  BumpTextureBindings(*d);  // banjo-nx (2026-10-27)
  CountHanded(*d, 0);
  return true;
}

// The destination takes the target's image; the target takes the destination's previous one (or a new one).
bool TakeImageLocked(HostTexture &d, HostSurface &s) {
  if (d.held && (d.held->host_width != s.scale.Size(s.width) || d.held->host_height != s.scale.Size(s.height) ||
                 d.held->format != s.format || d.held->samples != s.samples)) {
    RetireDepthImageLocked(*d.held);
    d.held.reset();
  }
  if (!d.held) {
    auto image = std::make_shared<DepthImage>();
    if (!MakeDepthImageLocked(s, *image))
      return false;
    d.held = std::move(image);
  }
  UnhandLocked(d);
  SwapDepthImageLocked(s, *d.held);
  d.held_current = true;
  ++g_texture_epoch;
  BumpTextureBindings(d);  // banjo-nx (2026-10-27)
  CountHanded(d, 2);
  return true;
}

void DepthHandOverBeforeBindLocked(HostSurface *surface, const plume::RenderRect *clear_rect, u32 clear_flags) {
  if (!surface || !surface->handed)
    return;
  // A colour target: bit 0 of the flags says this clear fills it (ClearTargets); the clear writes all its channels.
  const bool fills = surface->depth ? (clear_flags & 0x10) && ((clear_flags & 0x20) || !surface->stencil_used)
                                    : (clear_flags & 1u) != 0;
  bool take = clear_rect && fills &&
              clear_rect->left <= 0 && clear_rect->top <= 0 && clear_rect->right >= int32_t(surface->width) &&
              clear_rect->bottom >= int32_t(surface->height);
  std::vector<HostTexture *> holders;
  for (HostTexture *d : g_handed)
    if (d->handed_from == surface)
      holders.push_back(d);
  // The last resolve's destination takes the image; any other is drawn.
  for (size_t i = holders.size(); i-- > 0;) {
    HostTexture &d = *holders[i];
    if (take && surface->generation == d.handed_generation && surface->writes == d.handed_writes &&
        TakeImageLocked(d, *surface)) {
      take = false;
      continue;
    }
    MaterialiseLocked(d, false);
  }
}

// banjo-nx (2026-10-18, banjo_deko3d_draw_resolves): a single-sampled colour resolve drawn into the destination
// (shaders/host/resolve_copy_ps.hlsl) rather than copied. deko3d copies images on the copy engine, which plume_deko3d
// fences with a full barrier before and after (the 3D engine idle for the whole copy); a draw is ordered by the usual
// fragment barriers and overlaps the work around it. Same format both sides, 2D level 0, slice 0: every texel is written
// back bit for bit. 'rect' is the source rect and (host_x, host_y) its place in the destination, in host pixels.
// sasrt-nx (2026-10-20, sasrt_renut_fixed16_clamp): a resolve of a 16-bit fixed-point target, drawn with the console's -32..32
// (shaders/host/resolve_clamp_ps.hlsl); as DrawResolveCopy otherwise, on every back end.
bool ResolveFixed16Locked(HostSurface *surface, HostTexture *dest, const plume::RenderRect &rect, uint32_t host_x,
                          uint32_t host_y) {
  auto &s = State();
  if (!dest->texture || !dest->render_target || dest->host_format != surface->format ||
      dest->dimension != TextureDimension::Tex2D || dest->stacked || !s.present_vs)
    return false;
  static std::unique_ptr<plume::RenderShader> ps;
  static bool ps_tried = false;
  if (!ps_tried) {
    ps_tried = true;
    ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(resolve_clamp_ps), "resolve_clamp_ps");
  }
  if (!ps)
    return false;
  static std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
  auto &pipeline = pipelines[uint32_t(surface->format)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = surface->format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return false;
    pipeline->setName("sasrt fixed-point resolve");
  }
  const uint32_t source_slot = SurfaceSampleSlotLocked(surface);
  if (source_slot == kInvalidDescriptor)
    return false;
  if (!dest->copy_framebuffer) {
    const plume::RenderTexture *targets[] = {dest->texture.get()};
    dest->copy_framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
    if (!dest->copy_framebuffer)
      return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  const bool split = SasrtResolveSplitOn();  // renut-ultimate (2026-11-13, sasrt_gpu_diag_resolve_split)
  if (split) [[unlikely]]
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 0));
  TransitionSurfaceLocked(surface, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::COLOR_WRITE);
  if (split) [[unlikely]] {
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 1));
    gpu_profile::CountDraw(s, SasrtResolvePartLabel(surface, 1));
  }
  cmd->setFramebuffer(dest->copy_framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest->alloc_width), float(dest->alloc_height)));
  cmd->setScissors(plume::RenderRect(int32_t(host_x), int32_t(host_y), int32_t(host_x) + (rect.right - rect.left),
                                     int32_t(host_y) + (rect.bottom - rect.top)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.translate[0] = float(rect.left - int32_t(host_x));
  push.translate[1] = float(rect.top - int32_t(host_y));
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  if (split) [[unlikely]]
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 2));
  // The draw path binds its own layout, pipeline, targets and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
  return true;
}

// sasrt-nx (2026-10-20): the EDRAM depth restore (see DepthRestoreSourceLocked): the depth of 'depth' from the words 'color'
// holds over the same EDRAM, its stencil 0 (a game's restore copies a depth whose stencil it cleared).
bool RestoreDepthFromColorLocked(HostSurface *depth, HostSurface *color) {
  auto &s = State();
  // renut-ultimate: a resolve handed over in this target's image is drawn out first; the restore writes the image.
  if (depth->handed)
    DepthHandOverBeforeBindLocked(depth, nullptr, 0);
  static std::unique_ptr<plume::RenderShader> ps;
  static bool ps_tried = false;
  if (!ps_tried) {
    ps_tried = true;
    ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(depth_restore_ps), "depth_restore_ps");
  }
  struct Entry {
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    std::unique_ptr<plume::RenderPipeline> pipeline;
  };
  static Entry pipelines[4];
  plume::RenderPipeline *pipeline = nullptr;
  for (Entry &e : pipelines) {
    if (e.pipeline && e.format == depth->format) {
      pipeline = e.pipeline.get();
      break;
    }
    if (!e.pipeline && ps && s.present_vs) {
      plume::RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = s.utility_layout.get();
      desc.vertexShader = s.present_vs.get();
      desc.pixelShader = ps.get();
      desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      desc.cullMode = plume::RenderCullMode::NONE;
      desc.renderTargetCount = 0;
      desc.depthTargetFormat = depth->format;
      desc.depthEnabled = true;
      desc.depthWriteEnabled = true;
      desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
      e.pipeline = s.device->createGraphicsPipeline(desc);
      e.format = depth->format;
      if (e.pipeline)
        e.pipeline->setName("sasrt depth restore");
      pipeline = e.pipeline.get();
      break;
    }
  }
  const uint32_t slot = SurfaceSampleSlotLocked(color);
  plume::RenderFramebuffer *framebuffer = GetFramebufferLocked(nullptr, 0, depth);
  {
    static uint32_t logged = 0;
    if (logged++ < 8)
      RGPU_INFO("[renut] EDRAM depth restore (sasrt_renut_depth_restore): depth {:08X} {}x{} gen {} from colour {:08X} {}x{} gen {} ({})",
                depth->guest_va, depth->width, depth->height, depth->generation, color->guest_va, color->width,
                color->height, color->generation, pipeline && slot != kInvalidDescriptor && framebuffer ? "drawn" : "not possible");
  }
  depth->restored_from_seq = color->write_seq;
  if (!pipeline || slot == kInvalidDescriptor || !framebuffer)
    return false;
  plume::RenderCommandList *cmd = s.command_list;
  TransitionSurfaceLocked(color, plume::RenderTextureLayout::SHADER_READ);
  TransitionSurfaceLocked(depth, plume::RenderTextureLayout::DEPTH_WRITE);
  cmd->setFramebuffer(framebuffer);
  const uint32_t w = depth->scale.Size(std::min(depth->width, color->width));
  const uint32_t h = depth->scale.Size(std::min(depth->height, color->height));
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(w), int32_t(h)));
  {  // sasrt-nx (2026-11-09, sasrt_gpu_depth_restore_whole_clear, item 50; port 52): depth with the stencil when every
     // pixel's depth is drawn (a stencil-only clear keeps the depth, so it cannot be a fast clear)
    const bool whole = sasrt::gpu_round::On(sasrt::gpu_round::kRestoreClear) && w >= depth->scale.Size(depth->width) &&
                       h >= depth->scale.Size(depth->height);
    cmd->clearDepthStencil(whole, true, 0.0f, 0, nullptr, 0);
    if (whole)
      sasrt::gpu_round::Count(sasrt::gpu_round::kRestoreClear);
  }
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline);
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.texture_index = slot;
  push.sampler_index = kPointClampSamplerSlot;
  push.param0 = GpuTextureFormat(depth->format_word & 0x3F) == GpuTextureFormat::Fmt24_8_Float ? 1.0f : 0.0f;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  ++depth->writes;
  depth->write_seq = ++g_surface_write_seq;
  // The draw path binds its own layout, pipeline, targets and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
  return true;
}

bool ResolveCopyPossible(const HostSurface *surface, const HostTexture *dest) {
  if (!State().resolve_copy_ps || !dest->texture || !dest->render_target || dest->host_format != surface->format ||
      dest->dimension != TextureDimension::Tex2D || dest->stacked)
    return false;
  switch (surface->format) {  // formats a texel load gives back exactly when written as a colour
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  // sasrt-nx (2026-11-11, port 75; its formats batch, rows 6, 8, 10, 74, 93 and 7, 66, 73, 95, 96; its audit's
  // finding 1)
  case plume::RenderFormat::R10G10B10A2_UNORM:
  case plume::RenderFormat::R11G11B10_FLOAT:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R16_UNORM:
  case plume::RenderFormat::R16_FLOAT:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
  case plume::RenderFormat::R32G32B32A32_FLOAT:
    return true;
  default:
    return false;
  }
}

// blur-nx (2026-11-10, port 76; its key 216: blur_renut_sample_grid_resolve): the resolve of 'rect' of a multisampled view
// into dest at (host_x, host_y), from the single-sampled target of its sample grid (sample_grid_resolve_ps: each view
// pixel the average of its samples_x x samples_y block there), times color_scale (its key 219). The caller has checked
// the formats.
bool DrawSampleGridResolveLocked(HostSurface *grid, HostTexture *dest, const plume::RenderRect &rect, uint32_t host_x,
                                 uint32_t host_y, uint32_t samples_x, uint32_t samples_y, float color_scale) {
  auto &s = State();
  if (!s.sample_grid_resolve_ps || !dest->texture || !dest->render_target || dest->dimension != TextureDimension::Tex2D ||
      dest->stacked)
    return false;
  const uint32_t source_slot = SurfaceSampleSlotLocked(grid);
  if (source_slot == kInvalidDescriptor)
    return false;
  auto &pipeline = s.sample_grid_pipelines[uint32_t(dest->host_format)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = s.sample_grid_resolve_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = dest->host_format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return false;
    pipeline->setName("renut sample grid resolve");
  }
  if (!dest->copy_framebuffer) {
    const plume::RenderTexture *targets[] = {dest->texture.get()};
    dest->copy_framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
    if (!dest->copy_framebuffer)
      return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  TransitionSurfaceLocked(grid, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(dest, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(dest->copy_framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest->alloc_width), float(dest->alloc_height)));
  cmd->setScissors(plume::RenderRect(int32_t(host_x), int32_t(host_y), int32_t(host_x) + (rect.right - rect.left),
                                     int32_t(host_y) + (rect.bottom - rect.top)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = color_scale;  // the resolve's colour scale (blur_renut_resolve_exp_bias)
  push.scale[1] = 1.0f;
  push.translate[0] = float(rect.left - int32_t(host_x));
  push.translate[1] = float(rect.top - int32_t(host_y));
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  push.param0 = float(samples_x);
  push.param1 = float(samples_y);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
  return true;
}

// blur-nx (2026-11-10, port 76; its keys 212 and 219): the copy draw itself, from a bindless slot (a target's sample view
// or a staging image): dest's region at (host_x, host_y) from the source texels at rect's corner, times color_scale
// (resolve_copy_ps). The caller has made the source readable. The pipeline is the destination format's (the source's
// when not converting).
// renut-ultimate (2026-11-13, sasrt-nx's sasrt_gpu_diag_resolve_split): 'split_surface', DrawResolveCopy's source with
// the split diagnostic on (its parts 1 and 2 are marked here, part 0 there); null for none (the staging sources).
bool DrawCopyFromSlotLocked(uint32_t source_slot, HostTexture *dest, const plume::RenderRect &rect, uint32_t host_x,
                            uint32_t host_y, float color_scale, const HostSurface *split_surface = nullptr) {
  auto &s = State();
  if (source_slot == kInvalidDescriptor || !s.resolve_copy_ps || !dest->texture || !dest->render_target)
    return false;
  auto &pipeline = s.resolve_copy_pipelines[uint32_t(dest->host_format)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = s.resolve_copy_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = dest->host_format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return false;
    pipeline->setName("renut resolve copy");
  }
  if (!dest->copy_framebuffer) {
    const plume::RenderTexture *targets[] = {dest->texture.get()};
    dest->copy_framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
    if (!dest->copy_framebuffer)
      return false;
  }
  plume::RenderCommandList *cmd = s.command_list;
  const HostSurface *const surface = split_surface;
  const bool split = surface != nullptr;  // renut-ultimate (2026-11-13, sasrt_gpu_diag_resolve_split)
  TransitionTextureLocked(dest, plume::RenderTextureLayout::COLOR_WRITE);
  if (split) [[unlikely]] {
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 1));
    gpu_profile::CountDraw(s, SasrtResolvePartLabel(surface, 1));  // its resolves a frame
  }
  cmd->setFramebuffer(dest->copy_framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest->alloc_width), float(dest->alloc_height)));
  cmd->setScissors(plume::RenderRect(int32_t(host_x), int32_t(host_y), int32_t(host_x) + (rect.right - rect.left),
                                     int32_t(host_y) + (rect.bottom - rect.top)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = color_scale;  // resolve_copy_ps: the colour scale (1: a plain copy)
  push.scale[1] = 1.0f;
  push.translate[0] = float(rect.left - int32_t(host_x));
  push.translate[1] = float(rect.top - int32_t(host_y));
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  if (split) [[unlikely]]
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 2));
  banjo::renut_gpu::CountDrawnResolve();
  // The draw path binds its own layout, pipeline, targets and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
  return true;
}

bool DrawResolveCopy(HostSurface *surface, HostTexture *dest, const plume::RenderRect &rect, uint32_t host_x,
                     uint32_t host_y, bool convert, float color_scale) {
  auto &s = State();
  // blur-nx (2026-11-10, port 76; its key 212: blur_renut_resolve_keep_dest): 'convert', a destination kept in another
  // format (ResolveTarget): the texel load gives the source's values, the target's format stores them.
  if (convert ? !(s.resolve_copy_ps && dest->texture && dest->render_target &&
                  dest->dimension == TextureDimension::Tex2D && !dest->stacked &&
                  surface->samples == plume::RenderSampleCount::COUNT_1 && ConvertibleResolveFormat(surface->format) &&
                  ConvertibleResolveFormat(dest->host_format))
              : !ResolveCopyPossible(surface, dest))
    return false;
  const uint32_t source_slot = SurfaceSampleSlotLocked(surface);
  if (source_slot == kInvalidDescriptor)
    return false;
  const bool split = SasrtResolveSplitOn();  // renut-ultimate (2026-11-13, sasrt_gpu_diag_resolve_split)
  if (split) [[unlikely]]
    gpu_profile::Mark(s, SasrtResolvePartLabel(surface, 0));
  TransitionSurfaceLocked(surface, plume::RenderTextureLayout::SHADER_READ);
  return DrawCopyFromSlotLocked(source_slot, dest, rect, host_x, host_y, color_scale, split ? surface : nullptr);
}

// halo3-nx (2026-11-03): a resolve about to read 'surface', when another format's target over the same EDRAM view was drawn to
// after it. On the console both are the same memory; here each host format has its own target (surfaces.cpp, TargetKey). Halo
// 3's last pass draws to a 2_10_10_10_FLOAT view and resolves through a 2_10_10_10 one, which was the black frame: the resolve
// read a target nothing had drawn to. The floats are encoded into it as the console stores them. Other pairs are reported once.
void SyncColorViewLocked(HostSurface *surface) {
  static std::vector<HostSurface *> aliases;
  ColorAliasesLocked(surface, aliases);
  HostSurface *latest = nullptr;
  for (HostSurface *alias : aliases) {
    if (alias->bound_serial > surface->bound_serial && (!latest || alias->bound_serial > latest->bound_serial))
      latest = alias;
  }
  if (!latest || (surface->synced_view == latest && surface->synced_writes == latest->writes))
    return;
  constexpr u32 kFormat2_10_10_10_Float_Edram = 0x3F, kFormat2_10_10_10_As_16 = 0x36;
  auto &s = State();
  if ((latest->format_word & 0x3F) != kFormat2_10_10_10_Float_Edram || (surface->format_word & 0x3F) != kFormat2_10_10_10_As_16 ||
      latest->samples != plume::RenderSampleCount::COUNT_1 || surface->samples != plume::RenderSampleCount::COUNT_1 ||
      !s.view_fp10_encode_ps) {
    static uint32_t logged = 0;
    if (LogLimited(logged, 8))
      RGPU_WARN("Resolve through a view of format {:02X} of EDRAM last drawn as {:02X}: its pixels are not carried over",
                surface->format_word & 0x3F, latest->format_word & 0x3F);
    return;
  }
  // renut-ultimate: a resolve handed over in this target's image is drawn out first; the encode writes the image.
  if (surface->handed)
    DepthHandOverBeforeBindLocked(surface, nullptr, 0);
  const uint32_t source_slot = SurfaceSampleSlotLocked(latest);
  if (source_slot == kInvalidDescriptor)
    return;
  auto &pipeline = s.view_fp10_encode_pipelines[uint32_t(surface->format)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = s.view_fp10_encode_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = surface->format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return;
    pipeline->setName("renut view fp10 encode");
  }
  HostSurface *const colors[] = {surface};
  plume::RenderFramebuffer *framebuffer = GetFramebufferLocked(colors, 1, nullptr);
  if (!framebuffer)
    return;
  const int32_t width = int32_t(surface->scale.Size(std::min(surface->width, latest->width)));
  const int32_t height = int32_t(surface->scale.Size(std::min(surface->height, latest->height)));
  plume::RenderCommandList *cmd = s.command_list;
  TransitionSurfaceLocked(latest, plume::RenderTextureLayout::SHADER_READ);
  TransitionSurfaceLocked(surface, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(framebuffer);
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(surface->scale.Size(surface->width)),
                                          float(surface->scale.Size(surface->height))));
  cmd->setScissors(plume::RenderRect(0, 0, width, height));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  // RB_COLOR_INFO bits 20-25: the signed exponent the console applies when it writes the colour.
  push.param0 = std::ldexp(1.0f, int32_t(latest->color_info_word << 6) >> 26);
  // blur-nx (2026-11-17, blur_renut_target_bias_rgb): with the colour scaled by the target's bias as it is drawn
  // (shader_common.h banjoTrim, constant 927), the float target holds the biased value, as the console's EDRAM does: the
  // words are made from it as it is. 2^bias here as well scaled it twice (Halo 3's last pass has a bias).
  if (banjo::renut_gpu::TargetBiasRgbOn())
    push.param0 = 1.0f;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  surface->synced_view = latest;
  surface->synced_writes = latest->writes;
  ++surface->writes;  // banjo-nx (banjo_renut_resolve_skip): it holds other pixels now
  surface->write_seq = ++g_surface_write_seq;  // sasrt-nx (port 29): the EDRAM depth restore's order
  g_blur_color_write_last = surface->write_seq;  // blur-nx (2026-11-11, port 76; its key 220)
  // The draw path binds its own layout, pipeline, targets and viewport again.
  g_bound.framebuffer = nullptr;
  g_bound.pipeline = nullptr;
  g_bound.layout = false;
  g_bound.viewport_set = false;
}

plume::RenderRect ClampRect(const ClearRect *rect, uint32_t width, uint32_t height) {
  if (!rect)
    return plume::RenderRect(0, 0, int32_t(width), int32_t(height));
  const int32_t x1 = std::clamp(rect->x1, 0, int32_t(width));
  const int32_t y1 = std::clamp(rect->y1, 0, int32_t(height));
  const int32_t x2 = std::clamp(rect->x2, x1, int32_t(width));
  const int32_t y2 = std::clamp(rect->y2, y1, int32_t(height));
  return plume::RenderRect(x1, y1, x2, y2);
}

} // namespace

void ReadClearViewport(const u8 *device_state, float viewport[4]) {  // blur-nx (2026-11-13, #55)
  const GuestDevice device(device_state);
  for (u32 i = 0; i < 4; ++i)
    viewport[i] = device.F32(kClearViewportOffset + i * 4);
}

void ClearStateLocked(const u8 *device_state, u32 flags, const ClearRect *rect,
                      const float color[4], float depth, u32 stencil) {
  if (!device_state) {  // (as before: a null device clears nothing; the command list and SyncBound first)
    ClearTargetsLocked(nullptr, nullptr, flags, rect, color, depth, stencil);
    return;
  }
  u32 targets[kResolveTargetWords];
  float viewport[4];
  ReadResolveTargets(device_state, targets);
  ReadClearViewport(device_state, viewport);
  ClearTargetsLocked(targets, viewport, flags, rect, color, depth, stencil);
}

void ClearTargetsLocked(const u32 *target_words, const float *viewport, u32 flags, const ClearRect *rect,
                        const float color[4], float depth, u32 stencil) {
  auto &s = State();
  if (!Video::EnsureCommandListLocked())
    return;
  SyncBound(s);
  if (!target_words)  // blur-nx (2026-11-13, #55): was a null device state
    return;
  Targets targets;
  if (!GatherTargetsWords(target_words, targets))
    return;

  // D3D clamps clears to the viewport (CDevice viewport floats at 0x3170; blur-nx: 0x3218, kClearViewportOffset).
  ClearRect r = rect ? *rect : ClearRect{0, 0, int32_t(targets.width), int32_t(targets.height)};
  const float vx = viewport[0], vy = viewport[1];
  const float vw = viewport[2], vh = viewport[3];
  if (vw > 0.0f && vh > 0.0f) {
    int32_t vx2 = int32_t(vx + vw), vy2 = int32_t(vy + vh);
    // A target grown for tiling: D3D bounds the viewport by the tile-sized
    // surface, which covers the whole target across the tiles (as for the
    // scissor in ComputeViewport).
    if (targets.width > targets.guest_width && vx2 >= int32_t(targets.guest_width))
      vx2 = int32_t(targets.width);
    if (targets.height > targets.guest_height && vy2 >= int32_t(targets.guest_height))
      vy2 = int32_t(targets.height);
    r.x1 = std::max(r.x1, int32_t(vx));
    r.y1 = std::max(r.y1, int32_t(vy));
    r.x2 = std::min(r.x2, vx2);
    r.y2 = std::min(r.y2, vy2);
  }
  const plume::RenderRect clear_rect = ClampRect(&r, targets.width, targets.height);
  if (clear_rect.right <= clear_rect.left || clear_rect.bottom <= clear_rect.top)
    return;
  ClearTargets(targets, flags, clear_rect, color, depth, stencil);
}

// banjo-nx (2026-10-19, banjo_renut_depth_words_lazy): a destination's first resolves write the words either way.
constexpr uint32_t kDepthWordsProbe = 300;

// renut-ultimate (2026-11-13, sasrt-nx's resolve project R1; logging only, off by default): per depth resolve destination
// (guest address and size, the first 16), its resolves asked with and without the words, the reads of its words
// (EnsureDepthWordsLocked: a fetch of the words), and whether a read set words_wanted this run. The 10 s line tells which
// destinations' words nothing reads (banjo_renut_depth_words_lazy, sasrt_gpu_words_probe_kept), sasrt-nx's base for R2.
// Render thread; the line is the stats thread's: a mutex.
REXCVAR_DEFINE_BOOL(sasrt_gpu_diag_depth_words, false, "SASRT/Diagnostics",
                    "Diagnostic (sasrt-nx's resolve project R1), logging only, every 10 s: per depth resolve destination, "
                    "its resolves with and without the depth words, how often the words were read, and whether a read "
                    "first asked for them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace {
struct DepthWordsCensus {
  u32 va = 0, w = 0, h = 0;
  uint64_t with_words = 0, without = 0, reads = 0;
  bool wanted_by_read = false;  // a read set words_wanted (this run)
};
std::mutex g_depth_words_census_mutex;
std::vector<DepthWordsCensus> g_depth_words_census;
bool DepthWordsCensusOn() {
  static const bool on = REXCVAR_GET(sasrt_gpu_diag_depth_words);
  return on;
}
DepthWordsCensus *DepthWordsCensusOf(const HostTexture &d) {  // under the mutex
  for (auto &c : g_depth_words_census)
    if (c.va == d.guest_va && c.w == d.width && c.h == d.height)
      return &c;
  if (g_depth_words_census.size() >= 16)
    return nullptr;
  g_depth_words_census.push_back({d.guest_va, d.width, d.height});
  return &g_depth_words_census.back();
}
void DepthWordsCensusResolve(const HostTexture &d, bool words) {
  if (!DepthWordsCensusOn()) [[likely]]
    return;
  std::lock_guard lock(g_depth_words_census_mutex);
  if (DepthWordsCensus *c = DepthWordsCensusOf(d))
    ++(words ? c->with_words : c->without);
}
void DepthWordsCensusRead(const HostTexture &d) {
  if (!DepthWordsCensusOn()) [[likely]]
    return;
  std::lock_guard lock(g_depth_words_census_mutex);
  if (DepthWordsCensus *c = DepthWordsCensusOf(d)) {
    ++c->reads;
    if (!d.words_wanted)
      c->wanted_by_read = true;
  }
}
}  // namespace

std::string DepthWordsCensusReport() {
  if (!DepthWordsCensusOn())
    return {};
  std::lock_guard lock(g_depth_words_census_mutex);
  std::string out = "[renut] depth resolve words by destination (sasrt_gpu_diag_depth_words):";
  if (g_depth_words_census.empty())
    out += " none";
  for (auto &c : g_depth_words_census) {
    out += fmt::format(" {:08X} {}x{}: {} with words, {} without, read {}, first wanted by {};", c.va, c.w, c.h,
                       c.with_words, c.without, c.reads, c.wanted_by_read ? "a read" : "nothing (this run)");
    c.with_words = c.without = c.reads = 0;
  }
  return out;
}

void EnsureDepthWordsLocked(HostTexture &dest) {
  DepthResolveReadLocked(dest, 2);  // vivapinata-nx (2026-11-12, vivapinata_depth_resolve_skip_unread)
  DepthWordsCensusRead(dest);  // renut-ultimate (2026-11-13, sasrt-nx's R1)
  if (!banjo::renut_gpu::DepthWordsLazyOn())
    return;
  dest.words_wanted = true;
  if (dest.words_current)
    return;
  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover): left in a target's image: drawn now, with the words.
  if (dest.handed_from || dest.held_current) {
    banjo::renut_gpu::CountLazyWords(MaterialiseLocked(dest, true) ? 1 : 2);
    return;
  }
  HostSurface *surface = SurfaceByGenerationLocked(dest.last_source_generation);
  if (surface && surface->writes == dest.last_source_writes) {
    const ClearRect src{dest.last_src[0], dest.last_src[1], dest.last_src[2], dest.last_src[3]};
    if (ResolveDepth(surface, &dest, src, dest.last_dest_x, dest.last_dest_y, true)) {
      dest.words_current = true;
      banjo::renut_gpu::CountLazyWords(1);
      return;
    }
  }
  // The source has changed since: these words are older ones. From now on this destination writes them each time.
  banjo::renut_gpu::CountLazyWords(2);
}

// vivapinata-nx (2026-11-12, vivapinata_depth_resolve_skip_unread; the owner's call: "skip them but make sure they turn
// back on if anything else reads them"). The destinations read so far, by guest address: a destination made again at
// one of them is never skipped either.
std::unordered_set<u32> g_depth_resolve_read_va;

void DepthResolveReadLocked(HostTexture &d, int path) {
  if (d.depth_read) [[likely]]
    return;
  d.depth_read = true;
  {  // which destinations are read, and how (the first read of each)
    static uint32_t logged_reads = 0;
    if (logged_reads++ < 48)
      RGPU_INFO("[renut] depth resolve destination {:08X} ({}x{}) read for the first time ({}, after {} resolves, frame "
                "{}){}", d.guest_va, d.width, d.height, path == 0 ? "sampled at its address" : path == 1 ? "sampled "
                "through another header" : "its words", d.resolves, TextureFrameLocked(), d.depth_skipped ? ", its last resolve "
                "left out" : "");
  }
  g_depth_resolve_read_va.insert(d.guest_va);
  if (!d.depth_skipped)
    return;
  // Its last resolve was left out: drawn now, from the source if that is unchanged since (the same pixels), else from
  // the target's newer content (this once not what the game resolved). Every resolve into it is drawn from now on.
  d.depth_skipped = false;
  HostSurface *surface = SurfaceByGenerationLocked(d.last_source_generation);
  const bool unchanged = surface && surface->writes == d.last_source_writes;
  bool drawn = false;
  if (surface) {
    const ClearRect src{d.last_src[0], d.last_src[1], d.last_src[2], d.last_src[3]};
    drawn = ResolveDepth(surface, &d, src, d.last_dest_x, d.last_dest_y, true);
    if (drawn)
      d.words_current = true;
  }
  if (!(drawn && unchanged))
    d.resolve_signature = 0;
  banjo::renut_gpu::CountDepthResolveSkip(drawn && unchanged ? 1 : 2);
  static uint32_t logged = 0;
  if (logged++ < 16)
    RGPU_WARN("[renut] vivapinata_depth_resolve_skip_unread: depth resolve destination {:08X} ({}x{}) read; its "
              "resolves are drawn from now on; the one left out {}",
              d.guest_va, d.width, d.height,
              drawn && unchanged ? "drawn now from its unchanged source (the same pixels)"
                                 : drawn ? "drawn now from the target's newer content (NOT the same pixels, this once)"
                                         : "could not be drawn (its source is gone; NOT the same pixels, this once)");
}

// banjo-nx (2026-10-19, banjo_deko3d_depth_handover): see "Depth hand-over".
uint32_t DepthHandOverSlotLocked(HostTexture &d, const plume::RenderComponentMapping &mapping, BindEffects *fx) {
  if (HostSurface *source = d.handed_from) {
    if (!source->texture || source->generation != d.handed_generation || source->writes != d.handed_writes) {
      UnhandLocked(d);
      CountHanded(d, 5);
      return UINT32_MAX;
    }
    // Not the image of a target of the draw (a depth target it only tests is allowed: HandOverSourceIsDrawTarget): its
    // resolve is drawn after all.
    if (HandOverSourceIsDrawTarget(source)) {
      g_materialise_cause = 8;  // banjo-nx (2026-10-28, diagnostic)
      MaterialiseLocked(d, false);
      g_materialise_cause = 9;
      return UINT32_MAX;
    }
    const uint32_t slot =
        DepthImageViewSlotLocked(source->texture.get(), source->format, source->handover_views, mapping);
    if (slot == kInvalidDescriptor) {
      MaterialiseLocked(d, false);
      return UINT32_MAX;
    }
    TransitionSurfaceLocked(source, plume::RenderTextureLayout::SHADER_READ);
    if (fx)
      fx->handover_surface = source;
    CountHanded(d, 1);
    return slot;
  }
  if (!d.held || !d.held_current)
    return UINT32_MAX;
  DepthImage &image = *d.held;
  const uint32_t slot = DepthImageViewSlotLocked(image.texture.get(), image.format, image.views, mapping);
  if (slot == kInvalidDescriptor) {
    MaterialiseLocked(d, false);
    return UINT32_MAX;
  }
  TransitionDepthImageLocked(image, plume::RenderTextureLayout::SHADER_READ);
  if (fx)
    fx->handover_image = &image;
  CountHanded(d, 1);
  return slot;
}

void DepthHandOverForgetLocked(HostTexture &d) {
  UnhandLocked(d);
  d.held_current = false;
  if (d.held) {
    RetireDepthImageLocked(*d.held);
    d.held.reset();
  }
}

void DepthHandOverBeforeRetireLocked(HostSurface *surface) {
  if (!surface || !surface->handed)
    return;
  const bool list = Video::EnsureCommandListLocked();
  std::vector<HostTexture *> holders;
  for (HostTexture *d : g_handed)
    if (d->handed_from == surface)
      holders.push_back(d);
  for (HostTexture *d : holders) {
    if (list) {
      MaterialiseLocked(*d, false);
    } else {
      UnhandLocked(*d);
      CountHanded(*d, 5);
    }
  }
}

// banjo-nx (2026-10-19, banjo_renut_resolve_skip): what a resolve wrote into its destination: the source surface (its
// identity, rebuilds and writes), the rectangle and the place. Never 0.
static uint64_t ResolveSignature(const HostSurface *surface, const ClearRect &src, i32 dest_x, i32 dest_y,
                                 u32 level, u32 slice) {
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&h](uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0x100000001b3ull;
  };
  mix(uint64_t(uintptr_t(surface)));
  mix(surface->generation);
  mix(surface->writes);
  mix(surface->format_word);
  mix(uint64_t(uint32_t(src.x1)) | uint64_t(uint32_t(src.y1)) << 32);
  mix(uint64_t(uint32_t(src.x2)) | uint64_t(uint32_t(src.y2)) << 32);
  mix(uint64_t(uint32_t(dest_x)) | uint64_t(uint32_t(dest_y)) << 32);
  mix(uint64_t(level) | uint64_t(slice) << 32);
  return h ? h : 1;
}

// blur-nx (2026-11-13, M-35): draw.h kResolveTargetWords.
static_assert(d3d::kDeviceOffsetDepthStencil == d3d::kDeviceOffsetRenderTargets + 16);
static_assert(d3d::kDeviceOffsetRenderTargets + kResolveTargetWords * 4 <= d3d::kDeviceSnapshotSize);

void ReadResolveTargets(const u8 *device_state, u32 targets[kResolveTargetWords]) {
  const GuestDevice device(device_state);
  for (u32 i = 0; i < kResolveTargetWords; ++i)
    targets[i] = device.U32(d3d::kDeviceOffsetRenderTargets + i * 4);
}

void ResolveStateLocked(const u8 *device_state, u32 flags, const ClearRect *source_rect,
                        u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                        i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                        const float clear_color[4], float clear_depth, u32 clear_stencil) {
  u32 targets[kResolveTargetWords];
  if (device_state)
    ReadResolveTargets(device_state, targets);
  ResolveTargetsLocked(device_state ? targets : nullptr, flags, source_rect, dest_texture_va, dest_format, dest_x,
                       dest_y, dest_level, dest_slice, clear_color, clear_depth, clear_stencil);
}

// blur-nx (2026-11-18, the census's #6): a resolve's time, and how much of it finding its destination.
struct HbResolveScope {
    bool on = false;
    uint64_t t0 = 0, dest = 0;
    template <typename F>
    auto Time(F f) {
      if (!on)
        return f();
      const uint64_t t = ::renut::gpu::host_batch::Ticks();
      auto r = f();
      dest += ::renut::gpu::host_batch::Ticks() - t;
      return r;
    }
    ~HbResolveScope() {
      if (!on)
        return;
      const uint64_t total = ::renut::gpu::host_batch::Ticks() - t0;
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kResolveCalls);
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kResolveTicksDest, dest);
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kResolveTicksWork, total > dest ? total - dest : 0);
    }
};
void ResolveTargetsLocked(const u32 *targets, u32 flags, const ClearRect *source_rect,
                          u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                          i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                          const float clear_color[4], float clear_depth, u32 clear_stencil) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kResolveSetup);  // blur-nx (2026-11-17)
  HbResolveScope hb_rs;  // blur-nx (2026-11-18, the census's #6)
  hb_rs.on = ::renut::gpu::host_batch::CensusOn();
  hb_rs.t0 = hb_rs.on ? ::renut::gpu::host_batch::Ticks() : 0;
  auto &s = State();
  static const uint32_t kProfileLabel = gpu_profile::Label("resolves");
  gpu_profile::Mark(s, kProfileLabel);
  // Pending memexport results stay on the GPU: a resolve writes host textures
  // only, and a clear reads no memory.
  if (!Video::EnsureCommandListLocked())
    return;
  SyncBound(s);
  if (!targets)  // blur-nx (2026-11-13, M-35): was a null device state
    return;

  const u32 source = flags & 7;
  // blur-nx (2026-11-13, M-35): word 4 is the depth-stencil surface (kDeviceOffsetDepthStencil), as before.
  const u32 source_va = targets[source];
  HostSurface *surface = GetHostSurfaceLocked(source_va);
  // sasrt-nx (2026-11-09, sasrt_gpu_diag_profile_vs; the fourth round): with the shaders' breakdown, each resolve's own
  // label: its source's size and format, colour or depth, and whether it clears after.
  if (surface && banjo::renut_gpu::DiagGpuShadersOn() && SasrtProfileVsOn() && gpu_profile::Timed()) [[unlikely]] {
    static std::unordered_map<uint64_t, uint32_t> labels;
    const uint64_t k = uint64_t(surface->width) | uint64_t(surface->height) << 14 | uint64_t(int(surface->format)) << 28 |
                       uint64_t(surface->depth) << 40 | uint64_t((flags >> 8) & 3) << 41;
    uint32_t &label = labels[k];
    if (!label)
      label = gpu_profile::Label(fmt::format("resolves | {} {}x{} format {}{}", surface->depth ? "depth" : "colour",
                                             surface->width, surface->height, int(surface->format),
                                             (flags & 0x300) ? " and clear" : "")) + 1;
    gpu_profile::Mark(s, label - 1);
  }
  // vivapinata-nx (2026-11-04, the white fog at the cuts): this target's EDRAM was wiped by a clear through another layout and
  // nothing has written here since (ClearTargets): the resolve reads that clear. Viva Pinata blanks its 1280x720 mask this way at a
  // fade (the scene's tile clears, then a resolve of the mask's target into the mask's texture).
  if (surface && surface->edram_wiped) {
    bool still = surface->edram_wiped_generation == surface->generation && surface->edram_wiped_writes == surface->writes;
    surface->edram_wiped = false;
    // blur-nx (2026-11-10, port 76; its key 218: blur_renut_wipe_overwritten): written over through another view since
    // the wipe: on the console that write is in the EDRAM the resolve reads.
    if (still && banjo::renut_gpu::WipeOverwrittenOn() && OverlapWrittenSinceLocked(surface, surface->blur_wiped_seq)) {
      still = false;
      static u32 logged = 0;
      if (LogLimited(logged, 4))
        RGPU_INFO("[renut] resolve of {}x{}: its EDRAM wipe was drawn over through another view since; not applied "
                  "(blur_renut_wipe_overwritten)", surface->width, surface->height);
    }
    if (still) {
      Targets wiped;
      wiped.colors[wiped.color_count++] = surface;
      wiped.width = surface->width;
      wiped.height = surface->height;
      wiped.scale = surface->scale;
      ClearTargets(wiped, 1, plume::RenderRect(0, 0, int32_t(surface->width), int32_t(surface->height)),
                   surface->edram_wiped_color, 0.0f, 0);
      static u32 logged = 0;
      if (LogLimited(logged, 4))
        RGPU_INFO("[renut] resolve of {}x{} after a clear through another layout over its EDRAM: it reads that clear",
                  surface->width, surface->height);
    }
  }
  if (banjo::renut_gpu::ResolveTraceOn() && surface) {
    const ClearRect r = source_rect ? *source_rect : ClearRect{0, 0, -1, -1};
    RGPU_INFO("[trace] resolve {} {:08X} ({}x{} fmt {} samples {}) -> texture {:08X} at ({},{}) level {} slice {}, "
              "rect ({},{})-({},{}), flags {:X} ({}{})",
              surface->depth ? "depth" : "colour", source_va, surface->width, surface->height, u32(surface->format),
              u32(surface->samples), dest_texture_va, dest_x, dest_y, dest_level, dest_slice, r.x1, r.y1, r.x2, r.y2,
              flags, (flags & 0x100) ? "clear colour after " : "", (flags & 0x200) ? "clear depth after" : "");
  }
  plume::RenderCommandList *cmd = s.command_list;
  static uint32_t depth_logged = 0;

  // The source rect within the target and the destination level.
  auto fit = [&](const HostTexture *dest, ClearRect &src) {
    const uint32_t level_w = std::max(1u, dest->width >> dest_level);
    const uint32_t level_h = std::max(1u, dest->height >> dest_level);
    src = source_rect ? *source_rect : ClearRect{0, 0, int32_t(level_w), int32_t(level_h)};
    src.x1 = std::clamp(src.x1, 0, int32_t(surface->width));
    src.y1 = std::clamp(src.y1, 0, int32_t(surface->height));
    src.x2 = std::clamp(src.x2, src.x1, int32_t(surface->width));
    src.y2 = std::clamp(src.y2, src.y1, int32_t(surface->height));
    dest_x = std::clamp(dest_x, 0, int32_t(level_w));
    dest_y = std::clamp(dest_y, 0, int32_t(level_h));
    src.x2 = std::min(src.x2, src.x1 + int32_t(level_w) - dest_x);
    src.y2 = std::min(src.y2, src.y1 + int32_t(level_h) - dest_y);
    return src.x2 > src.x1 && src.y2 > src.y1;
  };

  if (surface && dest_texture_va) {
    if (surface->depth) {
      HostTexture *dest =
          dest_level == 0 && dest_slice == 0
              ? hb_rs.Time([&] { return GetDepthResolveTextureLocked(dest_texture_va, surface->scale, dest_format); })
              : nullptr;
      ClearRect src;
      if (!dest) {
        if (LogLimited(depth_logged, 2)) {
          RGPU_WARN("Depth resolve into {:08X} (level {}, slice {}) not done", dest_texture_va,
                    dest_level, dest_slice);
        }
      } else if (fit(dest, src)) {
        if (SasrtOrderEvery() > 0 && g_sasrt_order.on) [[unlikely]]  // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order)
          SasrtOrderToken(fmt::format("Z{}x{}f{}[{},{},{},{}]>{}x{}f{}@{},{}", surface->width, surface->height,
                                      int(surface->format), src.x1, src.y1, src.x2, src.y2, dest->width, dest->height,
                                      int(dest->host_format), dest_x, dest_y));
        // banjo-nx (2026-10-19, banjo_renut_resolve_skip): the same resolve of an unchanged source is in dest already.
        const uint64_t sig = ResolveSignature(surface, src, dest_x, dest_y, dest_level, dest_slice);
        // banjo-nx (2026-10-19, banjo_renut_depth_words_lazy): the words only where something reads them (or while
        // probing a new destination).
        const bool lazy = banjo::renut_gpu::DepthWordsLazyOn();
        const bool words = !lazy || dest->words_wanted || dest->words_probe < kDepthWordsProbe;
        DepthWordsCensusResolve(*dest, words);  // renut-ultimate (2026-11-13, sasrt-nx's R1)
        // vivapinata-nx (2026-11-12, vivapinata_depth_resolve_skip_unread; NOT exact in principle, the owner's call): a
        // resolve of a multisampled depth target into a destination nothing has read yet (neither its float nor its
        // words, nor a destination at its address before) is left out; DepthResolveReadLocked draws it when one is
        // read, and every later one. Viva's garden resolves its 2-sample scene depth twice a frame into two
        // destinations nothing samples (1.56 ms a frame in run c23), each followed by a draw writing the target, so
        // they cannot wait. Not a 1x1 destination (sasrt_renut_resolve_readback), not one handed over.
        if (!dest->depth_read && g_depth_resolve_read_va.contains(dest->guest_va))
          dest->depth_read = true;
        const bool skip_unread = !words && !dest->depth_read && surface->samples != plume::RenderSampleCount::COUNT_1 &&
                                 !(dest->width == 1 && dest->height == 1) && !dest->handed_from &&
                                 !dest->held_current && banjo::renut_gpu::DepthResolveSkipUnreadOn();
        if (banjo::renut_gpu::ResolveSkipOn() && dest->resolve_signature == sig) {
          banjo::renut_gpu::CountSkippedResolve(true);
        } else if (skip_unread || HandOverResolveLocked(surface, dest, src, dest_x, dest_y, words) ||
                   ResolveDepth(surface, dest, src, dest_x, dest_y, words)) {
          dest->depth_skipped = skip_unread;  // vivapinata-nx (2026-11-12)
          if (skip_unread)
            banjo::renut_gpu::CountDepthResolveSkip(0);
          dest->resolve_signature = sig;
          dest->words_current = words;
          if (!dest->words_wanted && dest->words_probe < kDepthWordsProbe)
            ++dest->words_probe;
          if (!words)
            banjo::renut_gpu::CountLazyWords(0);
          dest->last_source_generation = surface->generation;
          dest->last_source_writes = surface->writes;
          dest->last_src[0] = src.x1;
          dest->last_src[1] = src.y1;
          dest->last_src[2] = src.x2;
          dest->last_src[3] = src.y2;
          dest->last_dest_x = dest_x;
          dest->last_dest_y = dest_y;
        } else {
          dest->resolve_signature = 0;
        }
      }
    } else if (HostTexture *dest = hb_rs.Time([&] {
                 return GetResolveTextureLocked(dest_texture_va, surface->format, surface->scale, dest_format,
                                                ResolveCoverPixels(surface, source_rect));  // port 76 (212)
               })) {
      SyncColorViewLocked(surface);  // halo3-nx (2026-11-03)
      ClearRect src;
      // A volume's slices are depth; stacked textures and cubes have layers.
      const bool volume = dest->dimension == TextureDimension::Tex3D && !dest->stacked;
      const uint32_t slice = volume ? 0 : dest_slice;
      const uint32_t z = volume ? dest_slice : 0;
      const bool fits = fit(dest, src);
      // sasrt-nx (2026-11-08, sasrt_gpu_diag_export_order): R<target w>x<h>f<host format>[<rect>]><texture w>x<h>f<host
      // format, guest format>@<x>,<y> (l: a level or slice; x: ResolveCopyPossible says no; s: stacked or not 2D)
      if (SasrtOrderEvery() > 0 && g_sasrt_order.on && fits) [[unlikely]]
        SasrtOrderToken(fmt::format("R{}x{}f{}[{},{},{},{}]>{}x{}f{},{}@{},{}{}{}{}", surface->width, surface->height,
                                    int(surface->format), src.x1, src.y1, src.x2, src.y2, dest->width, dest->height,
                                    int(dest->host_format), int(dest->guest_format), dest_x, dest_y,
                                    dest_level || dest_slice ? "l" : "", ResolveCopyPossible(surface, dest) ? "" : "x",
                                    dest->stacked || dest->dimension != TextureDimension::Tex2D ? "s" : ""));
      // blur-nx (2026-11-10, port 76; its key 219: blur_renut_resolve_exp_bias): the colour's exponent bias, the Resolve
      // flags' top six bits, signed (D3DRESOLVE_EXPONENTBIAS; the game's D3D moves them to RB_COPY_DEST_INFO's
      // copy_dest_exp_bias).
      const int exp_bias = banjo::renut_gpu::ResolveExpBiasOn() ? int(int32_t(flags) >> 26) : 0;
      const float exp_scale = std::ldexp(1.0f, exp_bias);
      uint64_t sig = fits ? ResolveSignature(surface, src, dest_x, dest_y, dest_level, dest_slice) : 0;
      if (sig && exp_bias)
        sig ^= (uint64_t(uint32_t(exp_bias) & 63u) + 1u) * 0x9E3779B97F4A7C15ull;  // a biased resolve is another one
      // blur-nx (2026-11-17, the menu backdrop): a resolve the sample grid draws (below) reads the grid's target, not the
      // view, which the game never draws into (its writes and image never change). Its signature takes the grid target's
      // image, writes and last draw too: with the view's alone, banjo_renut_resolve_skip kept the first resolve for good.
      // Blur's 640x360 half-size scene behind the menus (a view at a fixed address) stayed the sky of the frame it was
      // first made in, and its depth-of-field composite blended the city's buildings and trees away.
      if (sig && slice == 0 && z == 0 && dest_level == 0) {
        if (const HostSurface *grid = SampleGridSourceLocked(surface, dest)) {
          sig ^= (uint64_t(grid->generation) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(grid->writes) * 0x165667B19E3779F9ull) ^
                 (uint64_t(grid->blur_draw_seq) * 0x27D4EB2F165667C5ull);
          if (!sig)
            sig = 1;
        }
      }
      if (fits && banjo::renut_gpu::ResolveSkipOn() && dest->resolve_signature == sig) {
        // banjo-nx (2026-10-19, banjo_renut_resolve_skip): the same resolve of an unchanged source is in dest already.
        banjo::renut_gpu::CountSkippedResolve(false);
      } else if (fits && slice == 0 && z == 0 && dest_level == 0 && exp_bias == 0 &&
                 HandOverResolveLocked(surface, dest, src, dest_x, dest_y, false)) {
        // kameo-nx (2026-11-03, banjo_deko3d_color_handover): left in the target's image; the target stays bound
        // (banjo_deko3d_resolve_keeps_target): nothing was drawn and no target changed.
        dest->resolve_signature = sig;
        if (!banjo::renut_gpu::ResolveKeepsTargetOn())
          g_bound.framebuffer = nullptr;
      } else if (fits) {
        // (Not left in the image: a destination holding other content has it back or no longer needs it, as the depth
        // path's HandOverResolveLocked does; a level or slice of a stacked destination never holds any.)
        if ((dest->handed_from || dest->held_current) && (slice != 0 || z != 0 || dest_level != 0))
          MaterialiseLocked(*dest, false);
        const uint64_t prev_sig = dest->resolve_signature;  // vivapinata-nx (2026-11-12, A/B 87): what dest held before
        dest->resolve_signature = sig;
        g_bound.framebuffer = nullptr;
        // The destination is of the surface's scale.
        plume::RenderRect rect =
            ScaleRect(plume::RenderRect(src.x1, src.y1, src.x2, src.y2), surface->scale);
        const uint32_t host_x = uint32_t(surface->scale.Coord(dest_x));
        const uint32_t host_y = uint32_t(surface->scale.Coord(dest_y));
        // At a fractional scale the source rect, rounded at its own edges, can
        // pass the destination's host size by a texel.
        rect.right = std::min(rect.right, rect.left + int32_t(dest->alloc_width - host_x));
        rect.bottom = std::min(rect.bottom, rect.top + int32_t(dest->alloc_height - host_y));
        const plume::RenderRect dest_rect(int32_t(host_x), int32_t(host_y),
                                          int32_t(host_x) + (rect.right - rect.left),
                                          int32_t(host_y) + (rect.bottom - rect.top));
        const bool multisampled = surface->samples != plume::RenderSampleCount::COUNT_1;
        // blur-nx (2026-11-10, port 76; its key 216: blur_renut_sample_grid_resolve): a 4x view over a single-sampled
        // target of twice its pitch (2x: the same pitch), drawn after the view was (its own clears aside): the console's
        // resolve averages each pixel's samples, which are that target's 2x2 (1x2) blocks. Blur's 640x360 half-size scene
        // comes from its 1280x720 post target this way (the reference's capture: the motion blur's source is that image;
        // here it was black).
        // blur-nx (2026-11-13, MSAA lowered): decided by the console's sample count (the header's): with msaa_level 1 or
        // 2 the 4x view is single-sampled or 2x here, but its samples are still the grid target's 2x2 blocks on the
        // console (Blur's half-size scene was black or wrong without this).
        const u32 guest_msaa = (u32(surface->info_word) >> 16) & 3;  // 0: 1x, 1: 2x, 2: 4x
        HostSurface *grid_alias = nullptr;
        if (guest_msaa != 0 && slice == 0 && z == 0 && dest_level == 0 && banjo::renut_gpu::SampleGridResolveOn() &&
            dest->host_format == surface->format) {
          grid_alias = SampleGridAliasLocked(surface);
          if (grid_alias && grid_alias->blur_draw_seq <= surface->blur_draw_seq)
            grid_alias = nullptr;
        }
        const uint32_t grid_x = guest_msaa == 2 ? 2u : 1u;
        // blur-nx (2026-11-10, port 76; its key 219: blur_renut_resolve_exp_bias): with a bias, a resolve is drawn (a copy
        // cannot scale): a single-sampled source straight from its view, a multisampled one through a staging image of its
        // own format (resolved whole by the API).
        uint32_t biased_slot = kInvalidDescriptor;
        if (exp_bias != 0 && !grid_alias && slice == 0 && z == 0 && dest_level == 0) {
          if (!multisampled) {
            biased_slot = SurfaceSampleSlotLocked(surface);
            if (biased_slot != kInvalidDescriptor)
              TransitionSurfaceLocked(surface, plume::RenderTextureLayout::SHADER_READ);
          } else if (surface->texture) {
            const uint32_t sw = surface->scale.Size(surface->width), sh = surface->scale.Size(surface->height);
            if (HostTexture *stage = ResolveSourceStagingLocked(surface->format, sw, sh); stage && stage->texture) {
              TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
              TransitionTextureLocked(stage, plume::RenderTextureLayout::RESOLVE_DEST);
              cmd->resolveTexture(stage->texture.get(), surface->texture.get());
              TransitionTextureLocked(stage, plume::RenderTextureLayout::SHADER_READ);
              biased_slot = ResolveSourceStagingSlotLocked(surface->format, sw, sh);
            }
          }
        }
        if (exp_bias != 0) {
          static uint32_t logged = 0;
          if (logged++ < 4)
            RGPU_INFO("[renut] resolve of {}x{} with exponent bias {} (times {}; blur_renut_resolve_exp_bias){}",
                      surface->width, surface->height, exp_bias, exp_scale,
                      grid_alias || biased_slot != kInvalidDescriptor ? "" : ": not drawable here, copied without it");
        }
        if (grid_alias && DrawSampleGridResolveLocked(grid_alias, dest, rect, host_x, host_y, grid_x, 2u, exp_scale)) {
          static uint32_t logged = 0;
          if (logged++ < 4)
            RGPU_INFO("[renut] resolve of a {}x{} {}x view drawn from the {}x{} target of its sample grid "
                      "(blur_renut_sample_grid_resolve)", surface->width, surface->height, grid_x * 2,
                      grid_alias->width, grid_alias->height);
        } else if (biased_slot != kInvalidDescriptor &&
                   DrawCopyFromSlotLocked(biased_slot, dest, rect, host_x, host_y, exp_scale)) {
          // blur-nx (2026-11-10, port 76; its key 219): drawn with the bias (into the destination's own format)
        } else if (dest->host_format != surface->format) {
          // blur-nx (2026-11-10, port 76; its key 212: blur_renut_resolve_keep_dest): a destination kept in another format
          // (ResolveTarget): its part written by a draw that converts.
          if (!(slice == 0 && z == 0 && dest_level == 0 && DrawResolveCopy(surface, dest, rect, host_x, host_y, true))) {
            static uint32_t logged = 0;
            if (LogLimited(logged, 8))
              RGPU_WARN("[renut] resolve into {:08X} kept in format {} (source format {}) not drawn (level {}, slice "
                        "{})", dest_texture_va, u32(dest->host_format), u32(surface->format), dest_level, dest_slice);
            dest->resolve_signature = 0;
          }
        } else if (multisampled && ((dest->extra_levels && IsVulkan()) ||
                                    ((slice != 0 || z != 0) && banjo::renut_gpu::MsSliceResolveOn()))) {
          // blur-nx (2026-11-11, port 76; its key 221: blur_renut_ms_slice_resolve): also a slice or depth other than 0 (a
          // cube face, an array layer): plume's region resolve writes slice 0 only, so the staging image and a copy into
          // the slice.
          // A destination with host levels takes it through a single-level
          // target (ResolveStagingLocked): plume's Vulkan resolve writes every
          // level of its destination (D3D12's writes level 0 only, and
          // resolves in place below). Without one (device lost) nothing of
          // the resolve lands.
          if (HostTexture *staging = ResolveStagingLocked(dest)) {
            TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
            TransitionTextureLocked(staging, plume::RenderTextureLayout::RESOLVE_DEST);
            cmd->resolveTextureRegion(staging->texture.get(), host_x, host_y,
                                      surface->texture.get(), &rect);
            TransitionTextureLocked(staging, plume::RenderTextureLayout::COPY_SOURCE);
            TransitionTextureLocked(dest, plume::RenderTextureLayout::COPY_DEST);
            const plume::RenderBox box(dest_rect.left, dest_rect.top, dest_rect.right,
                                       dest_rect.bottom);
            cmd->copyTextureRegion(
                plume::RenderTextureCopyLocation::Subresource(dest->texture.get(), 0, slice),
                plume::RenderTextureCopyLocation::Subresource(staging->texture.get()), host_x,
                host_y, z, &box);
          }
        } else if (multisampled) {
          // vivapinata-nx (2026-11-11, vivapinata_resolve_from_sibling / A/B 87): the last resolve of this target to its own
          // place (recorded below) and the target unchanged since but for colour clears outside this rect. Into the same
          // destination, which still holds that resolve (its signature) at the same place, over a rect inside it: those
          // texels are this resolve's already, so nothing is resolved. Into another destination of the same format and
          // scale: that one's texels are copied (one sample a texel, through an identity view: no swizzle, no red-blue
          // swap) instead of resolved again. Viva Pinata resolves its scene whole into BCCA0550, then into BCCA05D0, then
          // per tile into BCCA0550 again, each tile cleared after (EndTiling).
          bool copied = false;
          if (ResolveFromSiblingOn() && slice == 0 && z == 0 && dest_level == 0 && exp_bias == 0) {
            const HostSurface::MsSibling &sib = surface->ms_sibling;
            // Every write since was a colour clear (rect_clears), and none of them reached this rect.
            int why = !sib.dest_va                                                         ? 2
                      : surface->writes - sib.writes != surface->rect_clears - sib.clears ? 3
                                                                                           : 0;
            for (uint32_t i = 0; !why && i < sib.cleared_count; ++i) {
              const int32_t *c = sib.cleared[i];
              if (!(c[2] <= rect.left || c[0] >= rect.right || c[3] <= rect.top || c[1] >= rect.bottom))
                why = 4;
            }
            if (!why && (sib.generation != surface->generation || rect.left < sib.left || rect.top < sib.top ||
                         rect.right > sib.right || rect.bottom > sib.bottom))
              why = 5;
            if (!why && sib.dest_va == dest_texture_va) {
              if (prev_sig == sib.dest_sig && rect.left == int32_t(host_x) && rect.top == int32_t(host_y) &&
                  !dest->handed_from && !dest->held_current) {
                dest->resolve_signature = prev_sig;  // it still holds that resolve, of which this one is a part
                copied = true;
                why = 1;
              } else {
                why = 6;
              }
            } else if (!why) {
              HostTexture *from = FindHostTextureLocked(sib.dest_va);
              // 7: the other destination's reason, for the diagnostic line (vivapinata-nx, 2026-11-12)
              static uint64_t refused[8] = {};
              const int r = !from || from == dest || !from->texture        ? 0
                            : !from->gpu_written                           ? 1
                            : from->resolve_signature != sib.dest_sig      ? 2
                            : from->host_format != dest->host_format       ? 3
                            : !(from->scale == surface->scale)             ? 4
                            : from->handed_from || from->held_current      ? 5
                            : from->dimension != TextureDimension::Tex2D || from->stacked ? 6
                                                                           : 7;
              if (r < 7 && (++refused[r] & (refused[r] - 1)) == 0)  // the 1st, 2nd, 4th, 8th ... of each
                RGPU_INFO("[renut] resolve from another destination {:08X} into {:08X} refused: reason {} ({} times; "
                          "0 none, 1 not GPU-written, 2 changed since, 3 format {} / {}, 4 scale, 5 handed, 6 not 2D)",
                          sib.dest_va, dest_texture_va, r, refused[r], from ? int(from->host_format) : -1,
                          int(dest->host_format));
              why = 7;
              if (r == 7) {
                const uint32_t slot = RawViewSlotLocked(*from);  // x, y, z, w, no red-blue swap; else none
                TransitionTextureLocked(from, plume::RenderTextureLayout::SHADER_READ);
                copied = slot != kInvalidDescriptor && DrawCopyFromSlotLocked(slot, dest, rect, host_x, host_y, 1.0f);
                if (copied)
                  why = 0;
              }
            }
            CountResolveFromSibling(why);
            if (banjo::renut_gpu::ResolveTraceOn()) [[unlikely]]
              RGPU_INFO("[trace]   from the last one: {} {} (record {:08X} rect ({},{})-({},{}); this ({},{})-({},{}) at "
                        "({},{}))",
                        why == 0 ? "copied" : why == 1 ? "not resolved" : "resolved", why, sib.dest_va, sib.left, sib.top,
                        sib.right, sib.bottom, rect.left, rect.top, rect.right, rect.bottom, host_x, host_y);
          }
          if (!copied) {
            TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
            TransitionTextureLocked(dest, plume::RenderTextureLayout::RESOLVE_DEST);
            cmd->resolveTextureRegion(dest->texture.get(), host_x, host_y, surface->texture.get(),
                                      &rect);
            // The destination's texels at the target's coordinates (a resolve to its own place): a sibling for later.
            surface->ms_sibling = {};
            if (rect.left == int32_t(host_x) && rect.top == int32_t(host_y)) {
              HostSurface::MsSibling &sib = surface->ms_sibling;
              sib.dest_va = dest_texture_va;
              sib.dest_sig = sig;
              sib.generation = surface->generation;
              sib.writes = surface->writes;
              sib.clears = surface->rect_clears;
              sib.left = rect.left;
              sib.top = rect.top;
              sib.right = rect.right;
              sib.bottom = rect.bottom;
            }
          }
        } else if (surface->sasrt_fixed16 && slice == 0 && z == 0 && banjo::renut_gpu::Fixed16ClampOn() &&
                   ResolveFixed16Locked(surface, dest, rect, host_x, host_y)) {
          // sasrt-nx (2026-10-20): drawn with the fixed-point range.
        } else if (IsDeko3D() && banjo::renut_gpu::DrawResolvesOn() && slice == 0 && z == 0 &&
                   DrawResolveCopy(surface, dest, rect, host_x, host_y)) {
          // banjo-nx (2026-10-18): drawn, not copied (deko3d).
        } else {
          TransitionSurfaceLocked(surface, plume::RenderTextureLayout::COPY_SOURCE);
          TransitionTextureLocked(dest, plume::RenderTextureLayout::COPY_DEST);
          const plume::RenderBox box(rect.left, rect.top, rect.right, rect.bottom);
          cmd->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(dest->texture.get(), 0, slice),
              plume::RenderTextureCopyLocation::Subresource(surface->texture.get()), host_x,
              host_y, z, &box);
        }
        TransitionTextureLocked(dest, plume::RenderTextureLayout::SHADER_READ);
        detail::QueueResolveReadbackLocked(s, dest, slice, z, dest_level);  // sasrt-nx (2026-10-30)
        if (dest->extra_levels && LazyResolveMipsOn()) {
          // banjo-nx (2026-10-28, banjo_renut_lazy_resolve_mips; from sa2-nx): made where they are read
          // (FlushResolveMipsLocked), or before level 0 is written another way. Every way a draw takes a texture's words
          // without BindTextureLocked holds by an epoch: the lean executor's kept slots by g_texture_epoch, the texture
          // objects, the slots' last answers and the memo by the texture's binding buckets (sa2-nx bumped only the
          // first; Banjo's objects check the buckets). The first pending region bumps both.
          const plume::RenderRect &p = dest->mips_rect;
          if (dest->mips_pending) {
            dest->mips_rect = plume::RenderRect(std::min(p.left, dest_rect.left), std::min(p.top, dest_rect.top),
                                                std::max(p.right, dest_rect.right), std::max(p.bottom, dest_rect.bottom));
          } else {
            dest->mips_rect = dest_rect;
            dest->mips_pending = true;
            ++g_texture_epoch;
            BumpTextureBindings(*dest);
            ++g_lazy_mips_deferred;
          }
        } else if (dest->extra_levels) {
          UpdateResolveMipsLocked(dest, dest_rect);
          // The draw path binds its own layout, pipeline and viewport again.
          g_bound.pipeline = nullptr;
          g_bound.layout = false;
          g_bound.viewport_set = false;
        }
      }
    }
  }

  // Resolve-and-clear.
  if ((flags & 0x300) && surface) {
    if (banjo::renut_gpu::DiagGpuShadersOn() && SasrtProfileVsOn() && gpu_profile::Timed()) [[unlikely]] {
      static const uint32_t kClearLabel = gpu_profile::Label("resolves | the clear after");  // sasrt-nx (2026-11-09)
      gpu_profile::Mark(s, kClearLabel);
    }
    Targets t;
    if (surface->depth)
      t.depth = surface;
    else
      t.colors[t.color_count++] = surface;
    t.width = surface->width;
    t.height = surface->height;
    t.scale = surface->scale;
    const plume::RenderRect rect = ClampRect(source_rect, t.width, t.height);
    static const float kBlack[4] = {0, 0, 0, 0};
    const u32 clear_flags = ((flags & 0x100) ? 1u : 0u) | ((flags & 0x200) ? 0x30u : 0u);
    ClearTargets(t, clear_flags, rect, clear_color ? clear_color : kBlack, clear_depth,
                 clear_stencil);
    // kameo-nx (2026-11-04, banjo_renut_resolve_clears_both): the flags name what to clear whichever surface was resolved,
    // and the other one is the device's. Kameo's imposter renderer resolves each model's colour with 0x300 so the next starts
    // clean; with only the colour cleared each model was depth-tested against the last one's depth. Noted by Kameo:
    // RePowered (MaxDeadBear, BiRabittoh; MIT).
    const u32 other_flags = surface->depth ? clear_flags & 1u : clear_flags & 0x30u;
    if (banjo::renut_gpu::ResolveClearsBothOn() && other_flags) {
      HostSurface *other = GetHostSurfaceLocked(targets[surface->depth ? 0 : 4]);  // M-35: render target 0 or depth
      if (other && other != surface) {
        Targets o;
        if (other->depth)
          o.depth = other;
        else
          o.colors[o.color_count++] = other;
        o.width = other->width;
        o.height = other->height;
        o.scale = other->scale;
        ClearTargets(o, other_flags, ClampRect(source_rect, o.width, o.height), clear_color ? clear_color : kBlack,
                     clear_depth, clear_stencil);
      }
    }
  }
}

bool ReadRenderTargetLocked(const u8 *device_state, u32 source, u32 &width, u32 &height,
                            std::vector<u32> &words) {
  auto &s = State();
  if (!s.target_readback_pipeline || !Video::EnsureCommandListLocked())
    return false;
  SyncBound(s);
  const GuestDevice device(device_state);
  if (!device.valid() || source >= d3d::kMaxRenderTargets)
    return false;
  HostSurface *surface =
      GetHostSurfaceLocked(device.U32(d3d::kDeviceOffsetRenderTargets + source * 4));
  if (!surface || surface->depth || !surface->texture)
    return false;
  width = std::min(width, surface->width);
  height = std::min(height, surface->height);
  if (!width || !height)
    return false;
  static const uint32_t kProfileLabel = gpu_profile::Label("render target readback");
  gpu_profile::Mark(s, kProfileLabel);

  // A single-sampled copy of the rect, which the shader reads by its slot.
  const RenderScale scale = surface->scale;
  const plume::RenderRect rect(0, 0, int32_t(scale.Size(width)), int32_t(scale.Size(height)));
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      uint32_t(rect.right), uint32_t(rect.bottom), 1, surface->format);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  std::unique_ptr<plume::RenderTexture> copy = s.device->createTexture(desc);
  if (!TextureCreated(copy.get()))
    return false;
  std::unique_ptr<plume::RenderTextureView> view =
      copy->createTextureView(plume::RenderTextureViewDesc::Texture2D(surface->format));
  const uint64_t bytes = uint64_t(width) * height * 4;
  std::unique_ptr<plume::RenderBuffer> output = s.device->createBuffer(
      plume::RenderBufferDesc::DefaultBuffer(bytes, StorageBufferFlags()));
  std::unique_ptr<plume::RenderBuffer> readback =
      s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));
  std::unique_ptr<plume::RenderDescriptorSet> set = detail::CreateTargetReadbackSet(s);
  const uint32_t slot = view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
  if (!view || !output || !readback || !set || slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(slot, copy.get(), view.get());
  set->setBuffer(0, output.get());

  plume::RenderCommandList *cmd = s.command_list;
  g_bound.framebuffer = nullptr;
  if (surface->samples != plume::RenderSampleCount::COUNT_1) {
    TransitionSurfaceLocked(surface, plume::RenderTextureLayout::RESOLVE_SOURCE);
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(copy.get(), plume::RenderTextureLayout::RESOLVE_DEST));
    cmd->resolveTextureRegion(copy.get(), 0, 0, surface->texture.get(), &rect);
  } else {
    TransitionSurfaceLocked(surface, plume::RenderTextureLayout::COPY_SOURCE);
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(copy.get(), plume::RenderTextureLayout::COPY_DEST));
    const plume::RenderBox box(rect.left, rect.top, rect.right, rect.bottom);
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(copy.get()),
                           plume::RenderTextureCopyLocation::Subresource(surface->texture.get()),
                           0, 0, 0, &box);
  }
  const plume::RenderTextureBarrier read(copy.get(), plume::RenderTextureLayout::SHADER_READ);
  const plume::RenderBufferBarrier write(output.get(), plume::RenderBufferAccess::WRITE);
  cmd->barriers(plume::RenderBarrierStage::COMPUTE, &write, 1, &read, 1);
  cmd->setComputePipelineLayout(s.target_readback_layout.get());
  cmd->setPipeline(s.target_readback_pipeline.get());
  cmd->setComputeDescriptorSet(s.texture_set.get(), 0);
  cmd->setComputeDescriptorSet(set.get(), 1);
  TargetReadbackPush push{};
  push.texture_index = slot;
  push.width = width;
  push.height = height;
  push.scale_numerator = RenderScale::kNumerators[scale.code];
  push.scale_denominator = RenderScale::kDenominators[scale.code];
  cmd->setComputePushConstants(0, &push, 0, sizeof(push));  // banjo-nx: the size (the record worker copies it)
  cmd->dispatch((width + 7) / 8, (height + 7) / 8, 1);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderBufferBarrier(output.get(), plume::RenderBufferAccess::READ));
  cmd->copyBufferRegion(plume::RenderBufferReference(readback.get()),
                        plume::RenderBufferReference(output.get()), bytes);
  // The compute pipeline replaced the graphics one (D3D12), and the barriers
  // ended any render pass.
  g_bound.pipeline = nullptr;
  g_bound.layout = false;

  detail::SubmitAndWaitLocked(s);
  bool read_back = false;
  if (const auto *data = static_cast<const u32 *>(readback->map())) {
    words.assign(data, data + size_t(width) * height);
    readback->unmap();
    read_back = true;
  }
  Video::FreeTextureSlotLocked(slot);
  Video::RetireLocked(std::move(view));
  Video::RetireLocked(std::move(copy));
  Video::RetireLocked(std::move(set));
  Video::RetireLocked(std::move(output));
  Video::RetireLocked(std::move(readback));
  return read_back;
}

bool FrontBufferWrittenLocked(u32 texture_va) {
  const HostTexture *texture = FindHostTextureLocked(texture_va);
  return texture && texture->texture && texture->gpu_written;
}

// Port 169 (from fnxf_xiii, banjo_renut_scaler_crop): the scaler's source rect, the part of the front buffer the video
// hardware scans out and stretches to the screen. A game that renders below 720p into a corner of its front buffer
// was shown whole (its frame in a corner of the window). Only a rect from the corner, inside the texture.
PresentImage FrontBufferImageLocked(u32 texture_va, const i32 scaler[4]) {
  PresentImage image = FrontBufferImageLocked(texture_va);
  const HostTexture *texture = image.texture ? FindHostTextureLocked(texture_va) : nullptr;
  if (!texture)
    return image;
  const i32 x1 = scaler[0], y1 = scaler[1], x2 = scaler[2], y2 = scaler[3];
  if (x1 == 0 && y1 == 0 && x2 > 0 && y2 > 0 && u32(x2) <= texture->width && u32(y2) <= texture->height) {
    if (u32(x2) != texture->width || u32(y2) != texture->height) {
      image.source_width = std::max<uint32_t>(texture->scale.Size(u32(x2)), 1u);
      image.source_height = std::max<uint32_t>(texture->scale.Size(u32(y2)), 1u);
    }
  } else if (x2 > x1 && (x1 || y1)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("Swap: scaler source ({},{})-({},{}) not from the corner of the {}x{} front buffer: shown whole", x1, y1,
                x2, y2, texture->width, texture->height);
    }
  }
  return image;
}

PresentImage FrontBufferImageLocked(u32 texture_va) {
  PresentImage image;
  HostTexture *texture = FindHostTextureLocked(texture_va);
  if (!texture || !texture->texture || texture->slot == UINT32_MAX)
    return image;
  image.texture = texture->texture.get();
  image.layout = &texture->layout;
  image.descriptor = texture->slot;
  image.width = texture->alloc_width;
  image.height = texture->alloc_height;
  image.source_width = texture->scale.Size(texture->width);
  image.source_height = texture->scale.Size(texture->height);
  // vivapinata-nx (2026-11-12, vivapinata_present_lut): a hand-over image is the target's, of the same format.
  image.unorm8 = texture->host_format == plume::RenderFormat::R8G8B8A8_UNORM ||
                 texture->host_format == plume::RenderFormat::B8G8R8A8_UNORM;
  // kameo-nx (2026-11-03, banjo_deko3d_scanout_handover): the frame's last resolve left in the target's image is scanned out
  // from there (or from the image the destination took): no copy of a whole frame to read it once. With the key off, or
  // with no room for a view, it is drawn first and scanned out from the destination as before.
  if ((texture->handed_from || texture->held_current) && Video::EnsureCommandListLocked()) {
    HostSurface *const source = texture->handed_from;
    DepthImage *const held = source ? nullptr : texture->held.get();
    const uint32_t slot = banjo::renut_gpu::ScanOutHandOverOn()
                              ? DepthHandOverSlotLocked(*texture, texture->view_desc.componentMapping, nullptr)
                              : UINT32_MAX;
    if (slot == UINT32_MAX) {
      if (texture->handed_from || texture->held_current)
        MaterialiseLocked(*texture, false);
    } else if (source) {
      image.texture = source->texture.get();
      image.layout = &source->layout;
      image.descriptor = slot;
    } else {
      image.texture = held->texture.get();
      image.layout = &held->layout;
      image.descriptor = slot;
    }
  }
  return image;
}

// banjo-nx (2026-10-19, render-thread proposal 1): see draw.h.
bool GuestRangeCardsClean(const u8 *data, size_t bytes, uint64_t proven) {
  return PagesClean(data, bytes, proven);
}

uint64_t CardsInterval() {
  return g_flush_serial;
}

} // namespace renut::gpu
