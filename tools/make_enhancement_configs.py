#!/usr/bin/env python3
"""banjo-nx: writes the A/B test configs from the shipped renut.toml (docs/enhancements.md).

Each variant is the base config with a few keys changed, so two logs compare directly:
  banjo_cpu_on.toml      every enhancement as shipped (profiled, like every config but verify)
  banjo_cpu_off.toml     CPU master switch off (each CPU enhancement back to the original path)
  banjo_noncpu_off.toml  non-CPU master switch off (shader prewarm, Mesa cache, per-stage cache)
  banjo_all_off.toml     both master switches off
  banjo_profile.toml     as cpu_on, plus the app's own sampler (banjo_profiler.log)
  banjo_cpu_verify.toml  every CPU enhancement on (banjo_direct_state too) AND checked against its original path on every use; the log's
                         "[verify] ... differences: N" line must say 0. Profiling OFF. Slower: a test session only
  banjo_direct_ab.toml   as cpu_on plus banjo_direct_state, which turns itself off and on every 4 s: the log's
                         "ring thread A/B" line gives the ring's time with and without it from one run
  banjo_memo_ab.toml     the same for banjo_ring_edram_memo
  banjo_upload_uncached.toml  as cpu_on but the upload buffer in uncached memory (the A/B of upload_memory = 1)
  banjo_logwriter_off.toml    as cpu_on but the file log written by the logging thread itself (the A/B of
                              log_file_writer: compare the hitch frames around the ring's 10 s reports)
  banjo_cpu_rounds_off.toml  the three run-time keys of sa2-nx's CPU rounds off (the rest of them is in the NRO)
  banjo_tiles_once_ab.toml         as cpu_on, the single-pass scene (banjo_gpu_tiles_once) off and on every 4 s: the
                                   "ring thread A/B" line gives the ring's time per Swap for both from one run
  banjo_vertex_persistent_ab.toml  the same for the persistent vertex buffer (banjo_ring_vertex_persistent)
  banjo_emulation_items_off.toml   the 2026-10-13 evening items off (EDRAM proofs, tiles once, persistent vertices):
                                   the renderer as in the 2026-10-13_1411 build, with the texture fixes kept
  banjo_shader_load_memo_ab.toml   in-run A/B of banjo_ring_shader_load_memo (the "ring thread A/B" line)
  banjo_zcull_greater.toml         console trial: the driver's ZCULL in the direction of Banjo's reversed-depth scene
                                   (whole run; compare the "scene" figure of the "GPU per Swap" lines of two runs)
  banjo_shaders25_ab.toml          package 25's shader round (targets 49-54) switched together every 4 s; each key has its
                                   own config too (banjo_clamp_cbuf_ab, banjo_sign_mask_skip_ab, banjo_vs_outputs_
                                   unwritten_ab, banjo_bone_padding_ab, banjo_robustness_off_ab, banjo_edram_checks_cpu_ab)
  banjo_shaders25_off.toml         package 25's six shader keys off for a whole run
  banjo_deferred_off.toml          deferred recording off for a whole run (on by default since package 25, its
                                   worker on core 0: package 24b's A/B gave -12 % a draw on the executor)
  banjo_gpu_test.toml              banjo_gpu_zcull_greater + banjo_diag_gpu_groups = 2 in one run (the two GPU
                                   questions of 2026-10-14: does ZCULL in the reversed direction cut the scene's time,
                                   and which shaders the scene's time goes to)
  banjo_all_tests.toml             GPU groups + the Clear markers' A/B (GPU and ring) in one run (2026-10-14)
  banjo_api_ops_off.toml           the API layer's operations off (Clear, Resolve, dead draws) (2026-10-14)
  banjo_dead_draws_ab.toml         banjo_gpu_skip_dead_draws switched every 4 s (2026-10-14)
  banjo_early_alpha_ab.toml        banjo_gpu_early_alpha switched every 4 s, with the per-shader GPU groups: the GPU A/B
                                   line gives both halves' GPU time (2026-10-15)
  banjo_index_persistent_ab.toml   banjo_ring_index_persistent switched every 4 s (the "ring thread A/B" line)
  banjo_texture_signs_ab.toml      banjo_gpu_texture_signs_per_slot switched every 4 s, with the per-shader GPU groups
  banjo_clear_merge_ab.toml        banjo_gpu_edram4_clear_merge switched every 4 s, with the GPU groups
  banjo_vfetch_block_ab.toml       banjo_gpu_vfetch_block switched every 4 s, with the per-shader GPU groups
  banjo_branchless_ab.toml         banjo_gpu_branchless_select switched every 4 s, with the per-shader GPU groups
  banjo_bool_spec_ab.toml          banjo_gpu_bool_spec switched every 4 s, with the per-shader GPU groups
  banjo_stencil_fetch_3d_ab.toml   banjo_gpu_stencil_fetch_3d switched every 4 s, with the GPU groups
  banjo_lean_barriers_ab.toml      banjo_gpu_edram4_lean_barriers switched every 4 s, with the GPU groups
  banjo_resolve_clear_merge_ab.toml banjo_gpu_resolve_clear_merge switched every 4 s, with the GPU groups
  banjo_pred_select_ab.toml        banjo_gpu_pred_select (vertex shaders' predicated blocks flattened) switched every 4 s,
                                   with the per-shader GPU groups (2026-10-16)
  banjo_pred_select_ps_ab.toml     banjo_gpu_pred_select_ps (the same for pixel shaders) switched every 4 s, with the
                                   per-shader GPU groups (2026-10-16)
  banjo_tess_grid_ab.toml          banjo_gpu_tess_grid (the water's tessellated patches drawn as an indexed grid)
                                   switched every 4 s, with the per-shader GPU groups (2026-10-16)
  banjo_shaders21_ab.toml          the whole shader round of 2026-10-16 (targets 29, 37, 38, 39) switched together
                                   every 4 s, with the per-shader GPU groups
  banjo_ring_core_ab.toml          the ring thread pinned to core 1 (banjo_ring_core) against the whole core mask, every
                                   4 s (the "ring thread A/B" and "GPU A/B" lines give both halves)
  banjo_ring_decoder_ab.toml       banjo_ring_decoder (the ring split: a decoder thread and the ring as executor)
                                   against one item at a time, every 4 s (2026-10-16)
  banjo_vs_hash_given_ab.toml      banjo_ring_vs_hash_given (the vertex input key takes the program hash of its load)
                                   switched every 4 s (2026-10-16)
  banjo_d3d_records_ab.toml        banjo_d3d_records (records of every Direct3D call's packets: the decoder parses no
                                   PM4 for them) switched every 4 s (2026-10-16, step 4)
  banjo_d3d_records_native_ab.toml banjo_d3d_records_native (the native draw path writes its records itself) switched
                                   every 4 s (step 5)
  banjo_d3d_host_submit_ab.toml    banjo_d3d_host_submit (the kicks through a host queue: the primary ring not
                                   parsed) switched every 4 s (step 6)
  banjo_d3d_records_off.toml       steps 4-6 off for a whole run (the decoder parses the PM4, as in package 23)
  banjo_ring_decoder_off.toml      the ring split off for a whole run (the ring decodes and executes, as before)
  banjo_ring_decoder_core0.toml    the decoder thread on core 0 (main thread) instead of core 2 (deferred worker)
  banjo_stencil_fetch_3d_probe.toml  every EDRAM stencil import through the 3D engine's masked passes (no copy
                                   engine; exact either way): what the late stencil fetch costs there
  banjo_shaders_old.toml           the shader-size work of 2026-10-15 off for a whole run (per-slot signs, bone
                                   fetches, branch-free selects, unread vertex outputs, merged clears): compare with a
                                   run of renut.toml
  banjo_fragments.toml             measurement: fragments shaded per pass category, every frame (the Mfrag figures of
                                   the "GPU per Swap" lines), with the GPU time by draw state and pixel shader
  banjo_diagnostics.toml           every GPU measurement at once: one serialized frame every 10 s measured per draw (GPU
                                   time, fragments and ns per fragment per pixel shader), GPU groups, the driver's
                                   shader statistics, Vulkan commands / barriers / render passes per Swap with their
                                   call sites, the app's CPU sampler
  banjo_pmull_off.toml             banjo_ring_pmull_fingerprint off (the CRC32 pair hashes, as in the sixteenth package):
                                   compare RangeFingerprint's share of the ring thread in rex_profile.log (2026-10-15)
  banjo_d3d_native_off.toml        the native Direct3D draw path off (Direct3D's own draw functions run): compare the
                                   drawing threads' share in rex_profile.log with banjo_cpu_on.toml's
  banjo_deferred_ab.toml           the same trial as an in-run A/B: deferred recording off and on every 4 s; the
                                   "ring thread A/B" line gives the ring's time per Swap for both from one run
  banjo_deferred_ab_core2.toml     the same with the worker on core 2 (with the ring split's decoder) instead of 0
  banjo_transfers_off.toml         the 2026-10-14 EDRAM items off (banjo_gpu_dead_raster, banjo_gpu_rect_split,
                                   banjo_gpu_proof_depth_only, banjo_gpu_edram4_split_tiles): compare the
                                   "imports/exports" figures of the "GPU per Swap" lines with cpu_on's
  banjo_gpu_groups.toml            measurement: GPU time inside every pass by draw state and pixel shader
                                   (banjo_diag_gpu_groups = 2; the "GPU time by pass" lines get 48 entries)
  banjo_hot_functions_off.toml     every native copy of a hot game function off (the banjo_cpu_native_* keys of
                                   app/src/cpu/hot/localized.txt): the recompiled originals run, as before 2026-10-14.
                                   Compare the game threads' share in rex_profile.log with banjo_cpu_on.toml's
  banjo_hand_natives_off.toml      the hand-written natives of memcpy, memset and the four Direct3D writers off (the
                                   generated copies of those functions run instead): the A/B of 2026-10-14's natives
  banjo_renderer_renut.toml        package 26: Renut's renderer (banjo_renderer = "renut", docs/renut-renderer-port.md),
                                   with its GPU time per frame (renut_gpu_profile) and the frame rate every 10 s
                                   (since package 28 the same as renut.toml, which boots Renut's renderer)
  banjo_renut_vulkan.toml          package 29: renut.toml's settings with Renut's renderer on Vulkan (NVK), as up to
                                   package 28b
  banjo_renut_deko3d.toml          package 29: the same on the deko3d back end (docs/deko3d-renderer.md)
  banjo_renut_a2c_dither.toml      package 31: alpha to coverage drawn as before (the hardware's dither with MSAA off:
                                   the checker-patterned grass and fur edges), banjo_renut_alpha_to_mask_test off
  banjo_renut_words_no_stencil.toml  package 31: the depth resolve's words without the stencil (as before), 
                                   banjo_renut_depth_words_stencil off
  banjo_renut_half_pixel_off.toml  package 32: everything drawn half a pixel up-left as before (the halo the depth of
                                   field made around Kazooie), banjo_renut_half_pixel_offset off
  banjo_deko3d_copied_resolves.toml  package 33: colour resolves copied on the copy engine as before (each copy idles
                                   the whole GPU), banjo_deko3d_draw_resolves off
  banjo_deko3d_words_copied.toml   package 33: the depth resolve's words copied for the depth of field every frame as
                                   before, banjo_gpu_depth_words_view off
  banjo_deko3d_depth_uncompressed.toml  package 33: depth targets without hardware compression as before,
                                   banjo_deko3d_depth_compression off
  banjo_deko3d_zcull_off.toml      package 33: no Zcull (hierarchical depth culling) as before, banjo_deko3d_zcull off
  banjo_deko3d_gpu_before.toml     package 33: all four of the above off at once (package 32's GPU work)
  banjo_deko3d_uam_prmt_off.toml   package 33: uam without its new PRMT byte swaps (the shifts and masks as before), with
                                   a shader cache of its own (deko3d_shaders_f446.bin): if characters are missing or
                                   broken with renut.toml, this tells whether PRMT is why
  banjo_deko3d_uam_gvn_off.toml    package 34: uam without its value numbering (what a shader computes again is computed
                                   again), banjo_deko3d_uam_gvn off; a shader cache of its own, compiled as it goes
  banjo_deko3d_uam_uniform_branches_off.toml  package 34: uam keeps the SSY / SYNC pair around branches on constants
                                   (as before), banjo_deko3d_uam_uniform_branches off; a shader cache of its own
  banjo_renut_words_always.toml    package 34: every depth resolve writes its words again (as before),
                                   banjo_renut_depth_words_lazy off: the GPU A/B of the depth resolves
  banjo_renut_resolve_skip_off.toml  package 34: repeated resolves of an unchanged source done again,
                                   banjo_renut_resolve_skip off
  banjo_deko3d_barriers_before.toml  package 35: plume's barriers one per call with every invalidation (as before),
                                   banjo_deko3d_barrier_merge and banjo_deko3d_barrier_lean off
  banjo_deko3d_depth_handover_off.toml  package 35: every depth resolve drawn when the game asks (as before; the shadow
                                   maps copied each frame), banjo_deko3d_depth_handover off
  banjo_deko3d_depth24_off.toml    package 35: D24S8 depth targets (the shadow maps) as 32-bit float again (as before),
                                   banjo_deko3d_depth24 off
  banjo_deko3d_push_constants_off.toml  package 35: the float constants and SharedConstants uploaded whole into the
                                   ring for every draw again (as before), banjo_deko3d_push_constants off: the render
                                   thread's A/B of the pushed constants (and the GPU's, which processes the updates)
  banjo_deko3d_kick_submit_off.toml  package 35: the frame's GPU work submitted only at the present (as before),
                                   banjo_deko3d_kick_submit off: the input latency A/B of the kick submits
  banjo_late_input_off.toml        package 35: the controller read at the start of the frame as before,
                                   banjo_late_input off: the input latency A/B of the late input
  banjo_deko3d_ring_cleaner_off.toml  package 35: the upload ring cleaned by the render thread at the submit (as
                                   before), banjo_deko3d_ring_cleaner off
  banjo_deko3d_txq_off.toml        package 36: texture size queries in the shaders again (banjo_deko3d_txq_constants
                                   off): the GPU A/B of the texture size buffer (A5E2, the fur shells)
  banjo_deko3d_txq_verify.toml     package 36, a test: queries and sizes both, magenta pixels where they differ
                                   (the variants compile at run time; not for timing)
  banjo_deko3d_early_z_off.toml    package 36: discarding pixel shaders of draws without depth or stencil writes
                                   tested after the shader again (banjo_deko3d_early_z off): the GPU A/B
  banjo_deko3d_gpr32_on.toml       package 36: the fur family compiled with 32 GPRs at most (banjo_deko3d_gpr32 on):
                                   the GPU A/B of occupancy against spills
  banjo_renut_stall_present_on.toml  package 36: the last frame re-presented while the game stalls, as before
                                   (banjo_renut_stall_present on): the A/B of the colour flash fix
  banjo_renut_diag_fur_vertex_only.toml  package 36b, a measurement: the fur family's pixel shaders discard at
                                   once (banjo_renut_diag_null_ps); the GPU profile then shows the fur draws'
                                   vertex cost (the fur vanishes; its shaders compile on the console)
  banjo_renut_game_gamma_on.toml   package 36b: the game's gamma ramp applied to the output, as a 360 shows it
                                   (banjo_renut_game_gamma on; darker midtones): to look at, the user's call
  banjo_deko3d_light_branch_on.toml  package 36c: the heaviest lit shaders' point lights in branches on their reach
                                   (banjo_deko3d_light_branch on): the GPU A/B
  banjo_renut_device_swizzle_off.toml  package 36c: textures sampled through their header's swizzle, as before
                                   (banjo_renut_device_swizzle off): the A/B of the second colour flash fix
  banjo_deko3d_light_branch_ab.toml  package 36c: banjo_deko3d_light_branch switched off and on every 4 s in one
                                   run (in-run A/B target 15): the "[renut A/B]" line's GPU times
  banjo_deko3d_gpr32_ab.toml       package 36c: the same for banjo_deko3d_gpr32 (target 16)
  banjo_renut_fur_shells_2.toml    package 36c, NOT exact: every 2nd fur shell drawn (banjo_renut_fur_shells 2): the
                                   look and the GPU time of thinner fur, the user's call
  banjo_deko3d_uam_wide_rows_off.toml  package 36d: the bone rows as four 32-bit loads, as before (the A/B of the
                                   128-bit rows; its cache deko3d_shaders_f447.bin is in the package)
  banjo_deko3d_latency_trim_on.toml  package 36e: input latency, the next frame recorded just in time where the GPU
                                   is the limit (banjo_deko3d_latency_trim): its [latency] lines against run (a)'s
  banjo_gpu_ab_pack.toml           package 36q: banjo_deko3d_light_branch and banjo_deko3d_gpr32 both on, each switched
                                   off in turn (banjo_renut_ab_targets "15,16"): one "[renut A/B]" line each, GPU ms
                                   on and off (cache 56 has both together)
  banjo_renut_sa2_cpu_off.toml     package 36o: every item of sa2-nx's Renut CPU work off (the whole A/B: render and game
                                   thread ms a frame against renut_auto.toml's)
  banjo_renut_chain_reuse_replays_off.toml  package 36o: the replayed draws make their key, targets, viewport, textures
                                   again (banjo-nx's extension of sa2-nx's chain reuse)
  banjo_renut_core_layout_on.toml  package 36q: sa2-nx's core layout on (off by default: it hung the first Swap in
                                   Ryujinx 4 times in 12 runs)
  banjo_renut_cpu_verify.toml      package 36o: the deltas and the reuse checked draw by draw ("verify: ... differ")
  banjo_renut_portrait_fur_shells_3.toml  package 36e, NOT exact: the dialogue portrait's fur with 7 of 21 shells
  banjo_deko3d_skin_once_on.toml   package 36g: the fur's meshes skinned once a frame, the other shells reading the
                                   rows (banjo_deko3d_skin_once on): the same pixels, the GPU A/B
  banjo_deko3d_skin_once_ab.toml   package 36g: the same switched off and on every 4 s in one run (in-run A/B
                                   target 17): the "[renut A/B]" line's GPU times
  banjo_deko3d_skin_once_off.toml  package 36g: the fur skinned for every shell, as before (banjo_deko3d_skin_once
                                   off; on by default since 36g)
  banjo_deko3d_eager_kicks_on.toml  package 36i: input latency at the frame cap: the frame goes to the GPU in more,
                                   earlier parts (banjo_deko3d_eager_kicks): its [latency] "input -> GPU done"
  banjo_deko3d_eager_kicks_ab.toml  package 36i: the same switched off and on every 4 s (in-run A/B target 18): the
                                   "[renut A/B]" line's fps and render thread time with and without it
  banjo_deko3d_eager_kicks_off.toml  package 36i: the frame to the GPU as before (200-draw lists at the next kick;
                                   banjo_deko3d_eager_kicks off, on by default since 36i at the user's request)
  banjo_renut_fur_shells_all.toml  package 36x: every fur shell drawn (banjo_renut_fur_shells_percent 100; 75 by default
                                   since 36x, the user's call)
  banjo_renut_portrait_full.toml   package 36y: the dialogue portrait as the game draws it (every fur shell, its 4x MSAA;
                                   36y's tomls: 50 % shells and 2x MSAA)
  banjo_deko3d_async_variants_off.toml  package 36z3: a draw whose shader variant is not in deko3d_shaders.bin waits
                                   for its compile again (36z3: drawn with the generic variant meanwhile)
  banjo_renut_host_mips_on.toml    package 36z2: Renut's extra host mip level for resolve destinations above 1x again
                                   (docked: the 3D image at 1.2x, the HUD at 1.5x; about 4 ms a docked frame in 36z;
                                   off in renut.toml and every other config, your call)
  banjo_renut_native_720p.toml     package 36z: the game's own 1280x720 everywhere (renut.toml: the 3D image at 648p
                                   handheld and 900p docked, the HUD at 720p / 1080p; NOT exact)
  banjo_renut_smaa_high.toml       package 36u: SMAA's high preset (banjo_renut_smaa_quality 2)
  banjo_renut_smaa_ab.toml         package 36u: SMAA switched off and on every 4 s (in-run A/B target 19): its GPU time
  banjo_renut_smaa_compare.toml    package 36u: every 15 s the same frame before and after SMAA as
                                   capturas/smaa_NNN_off.tga / _on.tga (banjo_renut_diag_smaa_compare_s)
  banjo_display_sync_on.toml       package 35b: the frame limiter on the display's refresh (banjo_display_sync on;
                                   off by default: the emulator stalled with it), for a console try
  banjo_display_sync_off.toml      package 36i: the frame limiter on its own 33.3 ms timer again (banjo_display_sync
                                   off; on by default since 36i for the user's console try, the emulator hangs with it)
  banjo_renut_store_cards_off.toml  package 34: cached vertex and index copies hashed whole after every kick again (as
                                   before; the generated code still marks the cards), banjo_renut_store_cards off: the
                                   render thread's A/B of the store cards
  banjo_renut_cpu_memos_off.toml   package 34: the render thread's per-draw lookups (targets, texture bindings, the
                                   recording threads' buffers) made in full every time, banjo_renut_cpu_memos off
  banjo_renut_pipeline_memo_off.toml  package 34: the pipeline lookup hashes the draw's state every time,
                                   banjo_renut_pipeline_memo off
  banjo_deko3d_early_hoist_off.toml  package 34: the fur-type shaders' early alpha test fetches its textures again (as
                                   before; 2 registers fewer), banjo_deko3d_early_hoist off: the GPU A/B of the top shader
  banjo_deko3d_alpha_spec_off.toml  package 33: the alpha test read at run time by every pixel shader (no early depth
                                   test for any draw), banjo_deko3d_alpha_spec off
  banjo_deko3d_vs_outputs_off.toml  package 33: every vertex shader writes all its outputs again (shadow maps too),
                                   banjo_deko3d_vs_outputs off
  banjo_deko3d_tess_spec_off.toml  package 33: the tessellation code kept in every vertex shader, banjo_deko3d_tess_spec off
  banjo_renderer_native.toml       package 28: renut.toml with banjo-nx's renderer (banjo_renderer = "native"); the
                                   configs of that renderer's own keys (NATIVE_ONLY below) boot it too
  banjo_renut_tuning_off.toml      Renut's renderer without banjo-nx's tuning of it (pipeline cache, precise barriers,
                                   cached uploads, PMULL hashes, thread placement)
  banjo_renut_upload_uncached.toml / _barriers_full / _xxh3 / _unpinned: one of those parts off, a whole run each
  banjo_renut_cpu_off.toml         Renut's renderer with the CPU master switch off
  banjo_renut_msaa.toml            package 28b: Renut's renderer with the game's 2x MSAA (disable_msaa = false; renut.toml
                                   has it off since package 28b)
  banjo_renut_post_high.toml       Renut's renderer with its own default 16-bit post-processing buffers (the others use
                                   the console's 8-bit, renut_post_process_precision = "Game")
  banjo_renut_barriers_ab.toml / _pin_ab / _dedupe_ab / _push_ab / _pool_ab: Renut's renderer with its in-run A/B
                                   ("[renut A/B]" lines) of the precise barriers / the render thread's pin / the
                                   SharedConstants reuse / the push constant skip / the capture pool
  banjo_renut_exact_ab.toml        package 27: in-run A/B target 6 (banjo_renut_exact_streams: what the exact rule
                                   for cached vertex streams and index ranges costs)
  banjo_renut_exact_off.toml       package 27: Renut's own 4-frame background check of those copies (the "[renut]"
                                   line's "found stale late" counts the copies it would have drawn from stale)
  banjo_renut_memory_policy_off.toml  package 27: Renut's own memory limits (sized for a PC) on the console
  banjo_renut_eviction_test.toml   package 27, a TEST: texture eviction forced every 32 frames (a reserve larger than the
                                   budget) and released slots poisoned magenta: any magenta on screen is a bug
  banjo_renut_boolspec_ab.toml / _blendskip_ab / _tessgrid_ab: package 27, Renut's renderer's in-run A/B targets 7, 8
                                   and 9 (banjo_gpu_bool_spec, banjo_gpu_blend_skip, banjo_gpu_tess_grid)
  banjo_renut_depthonly_ab.toml / _trim_ab / _words_ab: package 28, Renut's renderer's in-run A/B targets 11, 12 and 13
                                   (banjo_gpu_depth_only, banjo_gpu_output_trim, banjo_gpu_depth_words_view)
  banjo_renut_28b_test.toml        package 28b: its three test runs in one (renut.toml, the GPU profile by pixel
                                   shader, the record worker's A/B every 4 s)
  banjo_renut_gpu_shaders.toml     package 28b, a measurement: the GPU profile split by pixel shader (the main
                                   1280x720 pass in ms, with draws a frame; the timestamps cost a little)
  banjo_renut_record_ab.toml       package 28b: in-run A/B target 14 (banjo_renut_record_worker: the command recording
                                   on a worker thread against the render thread alone)
  banjo_renut_trim_poison_test.toml  package 28, a TEST: the channels banjo_gpu_output_trim leaves out are 60000 instead
                                   of 0; the picture must be the same as with renut.toml (any change is a bug)
  banjo_pacing_game.toml           the game's own frame pacing instead of renut.toml's target_refreshRate = 0 +
                                   frame_cap = "30" (package 26)
  banjo_literals_cbuf_ab.toml      package 26: in-run A/B target 56 (literals from the constant registers)
  banjo_stencil_clear_partial_ab.toml  package 26: in-run A/B target 57 (partial stencil fetch before a Clear)
  banjo_import9_steps.toml         package 26 diagnostic: the 9-pass EDRAM import timed per step
  banjo_cpu_native_bulk_off.toml   2026-10-22: the bulk localized copies (localized_bulk.txt) all off: the A/B of the
                                   round (the hand-picked copies stay on)
  banjo_hot_guard_on.toml          2026-10-22: the native copies' guard back on (64 first calls per thread, then 1 in
                                   4096); renut.toml calls every copy directly

To use one on the console, copy it over the toml the NRO reads (katamari.toml next to it; renut.toml with
KATAMARI_BUILTIN_CONFIG off).

Usage: python tools/make_enhancement_configs.py <base renut.toml> <output folder>
"""
import os
import re
import sys

# Profiling is ON in every config (and in the shipped renut.toml) and OFF only in the verify config, at the user's
# request: the CPU sampler of the SDK (switch_profile_stacks -> logs/rex/rex_profile.log, every pc with its count) and
# the GPU time per pass category (masseffect_gpu_marks_categories -> the "GPU per Swap" lines). Both are plain cvars,
# outside the enhancement master switches, and are written explicitly here so a config stays profiled even if the
# default changes. banjo_profile.toml adds the app's own sampler (banjo_cpu_profiler -> banjo_profiler.log).
PROFILING = {"switch_profile_stacks": "true", "masseffect_gpu_marks_categories": "true"}
NO_PROFILING = {"switch_profile_stacks": "false", "masseffect_gpu_marks_categories": "false", "banjo_cpu_profiler": "false"}
# Package 26: Renut's renderer (banjo_renderer, read at boot), with its GPU time per frame logged every 10 s, and its
# post-processing buffers in the console's 8-bit format ("Game"; Renut's default "High" makes the bloom, depth of field
# and SSAO buffers 16-bit: twice their bandwidth, a different image). Package 29: its back end (renut_gpu_backend:
# "vulkan" on NVK, or "deko3d") is renut.toml's; banjo_renut_vulkan.toml / banjo_renut_deko3d.toml pick one.
RENUT = {"banjo_renderer": '"renut"', "renut_gpu_profile": "10.0", "renut_post_process_precision": '"Game"'}



def hot_function_keys():
    """The keys of the native copies of hot game functions: one per line of app/src/cpu/hot/localized.txt, named as
    tools/localize_native.py names them."""
    tools = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, tools)
    import localize_native
    return [localize_native.key_of(root, opts) for root, opts, _ in localize_native.list_entries()]


VARIANTS = {
    "banjo_cpu_on.toml": dict(PROFILING),
    # log_file_writer is read when logging starts, before the master switches: set with the CPU master by hand.
    "banjo_cpu_off.toml": dict(PROFILING, banjo_cpu_enhancements="false", log_file_writer="false"),
    "banjo_noncpu_off.toml": dict(PROFILING, banjo_noncpu_enhancements="false"),
    "banjo_all_off.toml": dict(PROFILING, banjo_cpu_enhancements="false", banjo_noncpu_enhancements="false",
                               log_file_writer="false"),
    "banjo_profile.toml": dict(PROFILING, banjo_cpu_profiler="true"),
    "banjo_cpu_verify.toml": dict(NO_PROFILING, banjo_cpu_verify="true", banjo_direct_state="true"),
    # 2026-10-22 (bejeweled3-nx's method): the bulk localized copies all off (the A/B of the round), and the copies'
    # guard back on (renut.toml has none: direct dispatch).
    "banjo_cpu_native_bulk_off.toml": dict(PROFILING, banjo_cpu_native_bulk="false"),
    "banjo_hot_guard_on.toml": dict(PROFILING, banjo_hot_guard_calls="64", banjo_hot_guard_period="4096"),
    "banjo_direct_ab.toml": dict(PROFILING, banjo_direct_state="true", banjo_direct_state_ab_s="4", banjo_ab_target="1"),
    "banjo_memo_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="2"),
    "banjo_upload_uncached.toml": dict(PROFILING, masseffect_native_upload_memory="0"),
    "banjo_logwriter_off.toml": dict(PROFILING, log_file_writer="false"),
    # The run-time keys of sa2-nx's CPU rounds (2026-10-13) off: the barrier after every compare-and-swap again, the
    # thread lookup in every critical section call, a notify for every event and semaphore signal. The rest of the
    # rounds is in the NRO (generated code), not switchable at run time.
    "banjo_cpu_rounds_off.toml": dict(PROFILING, banjo_cpu_guest_cas_no_barrier="false",
                                      kernel_critsec_pcr_owner="false", thread_signal_skip_idle="false"),
    # The items of 2026-10-13 evening ("Removing emulation the game does not rely on"), in-run A/B and all off.
    "banjo_tiles_once_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="9"),
    "banjo_vertex_persistent_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="11"),
    # After the second console run: one in-run A/B and the two console trials.
    "banjo_shader_load_memo_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="12"),
    "banjo_zcull_greater.toml": dict(PROFILING, banjo_gpu_zcull_greater="true"),
    "banjo_deferred_off.toml": dict(PROFILING, masseffect_native_deferred_recording="false",
                                    masseffect_deferred_native_fast="false",
                                    masseffect_deferred_native_update="false"),
    "banjo_emulation_items_off.toml": dict(PROFILING, banjo_gpu_proof_tess_leftover="false",
                                           banjo_gpu_proof_triangle="false", banjo_gpu_edram4_color_consumer="false",
                                           banjo_gpu_edram4_alias_by_shape="false", banjo_gpu_edram4_copy_batch="false",
                                           banjo_gpu_tiles_once="false", banjo_ring_vertex_persistent="false"),
    # Render thread / GPU round of 2026-10-14: the two EDRAM items off, and the GPU time split by draw group.
    "banjo_transfers_off.toml": dict(PROFILING, banjo_gpu_dead_raster="false", banjo_gpu_rect_split="false",
                                     banjo_gpu_proof_depth_only="false", banjo_gpu_edram4_split_tiles="false",
                                     banjo_d3d_api_clear="false"),
    # One console run that gives both GPU answers (the user's request, 2026-10-14): ZCULL in the reversed-depth
    # direction (a change, read once by the driver at start: compare its "scene" time with an earlier run) and the GPU
    # time split by draw state and pixel shader (measurement only).
    "banjo_gpu_test.toml": dict(PROFILING, banjo_gpu_zcull_greater="true", banjo_diag_gpu_groups="2"),
    # Everything to test in one console run (the user's request, 2026-10-14). Second form, after that run
    # (banjo_direct_state lost its A/B again; ZCULL greater showed nothing): GPU time split by draw group (the
    # marks are written when the work ends, masseffect_native_precise_marks, on by default), and the ring's use of
    # the Clear markers (banjo_d3d_api_clear) switched every 4 s: the "GPU A/B" line gives both halves' GPU time.
    "banjo_all_tests.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                 banjo_ab_target="20"),
    # The API layer's operations off (Clear and Resolve markers unused, dead draws drawn): the A/B of the sixteenth
    # package's GPU work against the fifteenth's renderer.
    "banjo_api_ops_off.toml": dict(PROFILING, banjo_d3d_api_clear="false", banjo_d3d_api_resolve="false",
                                   banjo_gpu_skip_dead_draws="false"),
    # The dead-draw skip switched every 4 s (GPU A/B line: the console's GPU time with and without those draws).
    "banjo_dead_draws_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="22"),
    # The early alpha test (2026-10-15) switched every 4 s; the GPU groups give the shaders' share.
    "banjo_early_alpha_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                      banjo_ab_target="26"),
    # The persistent indices (2026-10-15) switched every 4 s: the console's index substage did not move with it on
    # (3.13 -> 3.12 us a draw against the sixteenth package), so the A/B decides whether it stays.
    "banjo_index_persistent_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="25"),
    # The shader-size work (2026-10-15), one key at a time switched every 4 s (both variants of a pipeline get compiled:
    # the first switches hitch), and all of it off for a whole run.
    "banjo_texture_signs_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                        banjo_ab_target="27", banjo_gpu_texture_signs_per_slot="true"),
    "banjo_clear_merge_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="1", banjo_direct_state_ab_s="4",
                                      banjo_ab_target="28"),
    "banjo_vfetch_block_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                       banjo_ab_target="29"),
    "banjo_branchless_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                     banjo_ab_target="30"),
    "banjo_bool_spec_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                    banjo_ab_target="31", banjo_gpu_bool_spec="true"),  # off by default since 2026-10-16
    # EDRAM removal steps of 2026-10-16 (docs/edram-removal-plan.md, "Status").
    "banjo_stencil_fetch_3d_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="1", banjo_direct_state_ab_s="4",
                                           banjo_ab_target="32"),
    "banjo_lean_barriers_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="1", banjo_direct_state_ab_s="4",
                                        banjo_ab_target="33"),
    "banjo_resolve_clear_merge_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="1", banjo_direct_state_ab_s="4",
                                              banjo_ab_target="36"),
    # Translator round of 2026-10-16 (docs/shader.md): predicated blocks without a branch, per stage.
    "banjo_pred_select_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                      banjo_ab_target="37"),
    "banjo_pred_select_ps_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                         banjo_ab_target="38"),
    "banjo_tess_grid_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                    banjo_ab_target="39"),
    # Package 25's shader round (docs/shader-translator-ideas.md items 3, 5, 16, 15, 4, 61): each key, then all together.
    "banjo_clamp_cbuf_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4", banjo_ab_target="49"),
    "banjo_sign_mask_skip_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4", banjo_ab_target="50"),
    "banjo_vs_outputs_unwritten_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4", banjo_ab_target="51"),
    "banjo_bone_padding_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4", banjo_ab_target="52"),
    "banjo_robustness_off_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4", banjo_ab_target="53"),
    "banjo_edram_checks_cpu_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4", banjo_ab_target="54"),
    "banjo_shaders25_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4", banjo_ab_target="55"),
    "banjo_shaders25_off.toml": dict(PROFILING, banjo_gpu_clamp_cbuf="false", banjo_gpu_sign_mask_skip="false",
                                     banjo_gpu_vs_outputs_unwritten="false", banjo_gpu_bone_padding="false",
                                     banjo_gpu_robustness_off="false", banjo_gpu_edram_checks_cpu="false"),
    "banjo_shaders21_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                    banjo_ab_target="40"),
    # 2026-10-16: the ring thread pinned to core 1 against the whole core mask (the ring report line says which).
    "banjo_ring_core_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="41"),
    # (Target 42, the ring prover, was removed on 2026-10-16 after its console runs: docs/d3d-full-replacement.md.)
    # 2026-10-16: the ring split (banjo_ring_split.h) against its serialized off half.
    "banjo_ring_decoder_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="43"),
    "banjo_vs_hash_given_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="44"),
    # 2026-10-16, steps 4-6 of docs/d3d-full-replacement.md: each switched every 4 s against the step before it.
    "banjo_d3d_records_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="45"),
    "banjo_d3d_records_native_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="46"),
    "banjo_d3d_host_submit_ab.toml": dict(PROFILING, banjo_direct_state_ab_s="4", banjo_ab_target="47"),
    # Steps 4-6 off for a whole run (the decoder parses the PM4 again, as in package 23).
    "banjo_d3d_records_off.toml": dict(PROFILING, banjo_d3d_records="false", banjo_d3d_host_submit="false"),
    # The EDRAM plan's zero-code probe (2026-10-15): every stencil import, the late fetch (96 tiles a frame, 1.3 ms of
    # edram_import9 at stock) included, through the 3D masked passes instead of the copy engine. Same bits either way.
    "banjo_stencil_fetch_3d_probe.toml": dict(PROFILING, masseffect_native_edram4_stencil_copy_min_tiles="2048",
                                              banjo_diag_gpu_groups="1"),
    "banjo_shaders_old.toml": dict(PROFILING, banjo_gpu_texture_signs_per_slot="false", banjo_gpu_vfetch_block="false",
                                   banjo_gpu_branchless_select="false", masseffect_native_vs_pruned_outputs="false",
                                   banjo_gpu_edram4_clear_merge="false", banjo_gpu_bool_spec="false",
                                   banjo_gpu_pred_select="false", banjo_gpu_pred_select_ps="false",
                                   banjo_gpu_tess_grid="false"),
    # Measurement (2026-10-15): fragments shaded per pass and, one frame every 5 s, per draw and pixel shader (GPU time,
    # fragments, ns per fragment; the measured frame is serialized and slower). Why the scene's shaders cost what they do.
    # (masseffect_native_stats_pipeline: per pass, every frame. The renderer opens no per-pass query while
    # masseffect_native_stats_per_draw_s is set, so the per-draw sampling is in banjo_diagnostics.toml instead.)
    "banjo_fragments.toml": dict(PROFILING, masseffect_native_stats_pipeline="true", banjo_diag_gpu_groups="2"),
    # Every GPU measurement at once (2026-10-15): fragments per pass and per shader (one serialized frame every 10 s),
    # GPU time by draw state and pixel shader, the driver's statistics of every shader (registers, max warps per SM,
    # instructions, cycles, spills: one extra compile per shader on the ring), Vulkan commands, barriers and render
    # passes per Swap, and the app's own CPU sampler. Slower than play: for one measuring run.
    "banjo_diagnostics.toml": dict(PROFILING, banjo_cpu_profiler="true", banjo_diag_gpu_groups="2",
                                   masseffect_native_stats_per_draw_s="10",
                                   vulkan_pipeline_executable_info="true", banjo_diag_shader_stats='"all"',
                                   # (2026-10-16) NVK's NIR and NAK assembly of the heaviest shaders of package 20e's
                                   # console run (level and cutscene), into shader_asm/: where their instructions go
                                   banjo_diag_shader_asm='"538,449,37,201,557,767,167,159,252,401,292,112,116,658,173"',
                                   banjo_diag_vk_calls="true"),
    # The PMULL fingerprint (2026-10-15) off: no in-run A/B (fingerprints are compared across draws).
    "banjo_pmull_off.toml": dict(PROFILING, banjo_ring_pmull_fingerprint="false"),
    # 2026-10-16, the replacement's steps 2-3 off for a whole run (if one misbehaves on the console), and the decoder
    # on core 0 (with the main thread) instead of core 2 (with deferred recording's worker).
    "banjo_ring_decoder_off.toml": dict(PROFILING, banjo_ring_decoder="false"),
    "banjo_ring_decoder_core0.toml": dict(PROFILING, banjo_ring_decoder_core="0"),
    # PC diagnostic of banjo_d3d_api_clear: the padding it takes filled with a loud value, the use switched every 2 s
    # (tools/ab_flicker.py on a recording: a difference that follows the switch would be a read of the padding).
    "banjo_api_clear_poison.toml": dict(PROFILING, banjo_diag_clear_padding="1", banjo_direct_state_ab_s="2",
                                        banjo_ab_target="20"),
    # The native Direct3D draw path (2026-10-14) off: Direct3D's recompiled draw functions run.
    "banjo_d3d_native_off.toml": dict(PROFILING, banjo_d3d_native_draw_indexed="false",
                                      banjo_d3d_native_draw_vertices="false"),
    # Deferred recording switched in the run (banjo_ab_target 16; the key itself has to be on for the switch to exist).
    "banjo_deferred_ab.toml": dict(PROFILING, masseffect_native_deferred_recording="true",
                                   masseffect_deferred_native_fast="true", masseffect_deferred_native_update="true",
                                   banjo_direct_state_ab_s="4", banjo_ab_target="16"),
    # The same with the worker on core 2 (with the ring split's decoder) instead of package 25's default, core 0.
    "banjo_deferred_ab_core2.toml": dict(PROFILING, masseffect_native_deferred_recording="true",
                                         masseffect_deferred_native_fast="true", masseffect_deferred_native_update="true",
                                         banjo_deferred_core="2", banjo_direct_state_ab_s="4", banjo_ab_target="16"),
    "banjo_gpu_groups.toml": dict(PROFILING, banjo_diag_gpu_groups="2"),
    # The native copies of hot game functions (2026-10-14, from sa2-nx's mechanism): all of them off.
    "banjo_hot_functions_off.toml": dict(PROFILING, **{key: "false" for key in hot_function_keys()}),
    # The hand-written natives (2026-10-14) off: their functions' generated copies run.
    "banjo_hand_natives_off.toml": dict(PROFILING, banjo_hand_memcpy="false", banjo_hand_memset="false",
                                        banjo_hand_constants="false", banjo_hand_state_flush="false"),
    # Package 26 (2026-10-16): literal constants from the constant registers (A/B 56), the partial stencil fetch before
    # a stencil-replacing Clear (A/B 57), and the 9-pass import timed per step (diagnostic).
    "banjo_literals_cbuf_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="2", banjo_direct_state_ab_s="4",
                                        banjo_ab_target="56"),
    "banjo_stencil_clear_partial_ab.toml": dict(PROFILING, banjo_diag_gpu_groups="1", banjo_direct_state_ab_s="4",
                                                banjo_ab_target="57"),
    "banjo_import9_steps.toml": dict(PROFILING, banjo_diag_gpu_groups="1", banjo_diag_import9_steps="true"),
    # The game's own frame pacing (its present interval: every frame rounded to 33 or 50 ms) instead of package 26's
    # target_refreshRate = 0 + frame_cap = "30" in renut.toml (the user's "it likes 20 if it cannot hold 30").
    "banjo_pacing_game.toml": dict(PROFILING, target_refreshRate="-1", frame_cap='"Off"'),
    # Renut's renderer's own in-run A/B (banjo_renut_ab_target): precise barriers; the render thread's pin.
    "banjo_renut_barriers_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="1", banjo_renut_ab_s="4"),
    "banjo_renut_pin_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="2", banjo_renut_ab_s="4"),
    "banjo_renut_dedupe_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="3", banjo_renut_ab_s="4"),
    "banjo_renut_push_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="4", banjo_renut_ab_s="4"),
    "banjo_renut_pool_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="5", banjo_renut_ab_s="4"),
    # Package 27: banjo_renut_exact_streams' cost (in-run A/B target 6), and a whole run without it (Renut's 4-frame
    # background check: the "[renut]" line's "found stale late" counts the copies drawn from stale until then).
    "banjo_renut_exact_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="6", banjo_renut_ab_s="4"),
    "banjo_renut_exact_off.toml": dict(PROFILING, **RENUT, banjo_renut_exact_streams="false"),
    # Package 27: Renut's own memory limits (banjo_renut_memory_policy off: sized for a PC's video memory).
    "banjo_renut_memory_policy_off.toml": dict(PROFILING, **RENUT, banjo_renut_memory_policy="false"),
    # Package 27, a test: the eviction rules forced every 32 frames (a reserve larger than any budget) and released
    # descriptor slots pointed at magenta: any magenta is a draw sampling a released texture (sa2-nx's poison test).
    # Package 27: banjo-nx's translator items as Renut's renderer runs them, each with its in-run A/B.
    "banjo_renut_boolspec_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="7", banjo_renut_ab_s="4"),
    "banjo_renut_blendskip_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="8", banjo_renut_ab_s="4",
                                         banjo_renut_blend_skip="true"),
    "banjo_renut_tessgrid_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="9", banjo_renut_ab_s="4"),
    # Package 27c: the stream checks hashed by a worker thread at each kick (banjo_renut_check_worker), in-run A/B.
    "banjo_renut_depthonly_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="11", banjo_renut_ab_s="4"),
    "banjo_renut_trim_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="12", banjo_renut_ab_s="4"),
    "banjo_renut_words_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="13", banjo_renut_ab_s="4"),
    "banjo_renut_record_ab.toml": dict(PROFILING, **RENUT, banjo_renut_ab_target="14", banjo_renut_ab_s="4"),
    "banjo_renut_gpu_shaders.toml": dict(PROFILING, **RENUT, banjo_renut_diag_gpu_shaders="true"),
    # Package 28b: the three runs of its README in one (the user's request): renut.toml's settings, the GPU profile
    # split by pixel shader, and the record worker's in-run A/B (both halves carry the profile's timestamps alike).
    "banjo_renut_28b_test.toml": dict(PROFILING, **RENUT, banjo_renut_diag_gpu_shaders="true",
                                      banjo_renut_ab_target="14", banjo_renut_ab_s="4"),
    "banjo_renut_trim_poison_test.toml": dict(PROFILING, **RENUT, banjo_renut_diag_trim_poison="true"),
    "banjo_renut_eviction_test.toml": dict(PROFILING, **RENUT, banjo_renut_memory_policy="true",
                                           banjo_renut_textures_reserve_mb="16384",
                                           banjo_renut_diag_poison_slots="true"),
    # Package 26 (2026-10-17): Renut's renderer (docs/renut-renderer-port.md), chosen at boot. Its GPU time per frame
    # every 10 s (renut_gpu_profile), the frame rate every 10 s ("[frames]" lines, both renderers).
    "banjo_renderer_renut.toml": dict(PROFILING, **RENUT),
    # Package 29 (docs/deko3d-renderer.md): Renut's renderer on each back end, the rest renut.toml's: the same place
    # with both shows what the deko3d back end draws differently (the colours) and what it costs.
    "banjo_renut_vulkan.toml": dict(PROFILING, **RENUT, renut_gpu_backend='"vulkan"'),
    "banjo_renut_deko3d.toml": dict(PROFILING, **RENUT, renut_gpu_backend='"deko3d"'),
    # Package 31 (2026-10-18): the visual fixes' own switches off, a whole run each (to compare).
    "banjo_renut_a2c_dither.toml": dict(PROFILING, **RENUT, banjo_renut_alpha_to_mask_test="false"),
    "banjo_renut_words_no_stencil.toml": dict(PROFILING, **RENUT, banjo_renut_depth_words_stencil="false"),
    "banjo_renut_half_pixel_off.toml": dict(PROFILING, **RENUT, banjo_renut_half_pixel_offset="false"),
    # Package 33 (2026-10-18): the deko3d GPU savings off, one at a time and all at once (with the per-shader GPU profile
    # that renut.toml has, to compare the passes).
    "banjo_deko3d_copied_resolves.toml": dict(PROFILING, **RENUT, banjo_deko3d_draw_resolves="false"),
    "banjo_deko3d_words_copied.toml": dict(PROFILING, **RENUT, banjo_gpu_depth_words_view="false"),
    "banjo_deko3d_depth_uncompressed.toml": dict(PROFILING, **RENUT, banjo_deko3d_depth_compression="false"),
    "banjo_deko3d_zcull_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_zcull="false"),
    "banjo_deko3d_gpu_before.toml": dict(PROFILING, **RENUT, banjo_deko3d_draw_resolves="false",
                                         banjo_gpu_depth_words_view="false", banjo_deko3d_depth_compression="false",
                                         banjo_deko3d_zcull="false"),
    # Package 33: the shader work's run-time parts off, one at a time.
    "banjo_deko3d_uam_prmt_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_uam_prmt="false"),
    "banjo_deko3d_alpha_spec_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_alpha_spec="false"),
    "banjo_deko3d_vs_outputs_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_vs_outputs="false"),
    "banjo_deko3d_tess_spec_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_tess_spec="false"),
    # Package 34: uam's value numbering and its uniform branches off (each with a DKSH cache of its own).
    "banjo_deko3d_uam_gvn_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_uam_gvn="false"),
    "banjo_deko3d_uam_uniform_branches_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_uam_uniform_branches="false"),
    "banjo_deko3d_early_hoist_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_early_hoist="false"),
    "banjo_renut_words_always.toml": dict(PROFILING, **RENUT, banjo_renut_depth_words_lazy="false"),
    "banjo_renut_resolve_skip_off.toml": dict(PROFILING, **RENUT, banjo_renut_resolve_skip="false"),
    # Package 35 (GPU, no shader changes): barriers as before; depth resolves always drawn.
    "banjo_deko3d_barriers_before.toml": dict(PROFILING, **RENUT, banjo_deko3d_barrier_merge="false",
                                              banjo_deko3d_barrier_lean="false"),
    "banjo_deko3d_depth_handover_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_depth_handover="false"),
    "banjo_deko3d_depth24_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_depth24="false"),
    # Package 35 (render thread, input latency).
    "banjo_deko3d_push_constants_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_push_constants="false"),
    "banjo_deko3d_kick_submit_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_kick_submit="false"),
    "banjo_late_input_off.toml": dict(PROFILING, **RENUT, banjo_late_input="false"),
    "banjo_deko3d_ring_cleaner_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_ring_cleaner="false"),
    # Package 35b (latency batch 3).
    "banjo_display_sync_on.toml": dict(PROFILING, **RENUT, banjo_display_sync="true"),
    # 2026-10-20: on by default since 36i (the user's console try); this is the frame limiter's own timer again.
    "banjo_display_sync_off.toml": dict(PROFILING, **RENUT, banjo_display_sync="false"),
    # Package 36 (2026-10-19 night, GPU and the colour flash).
    "banjo_deko3d_txq_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_txq_constants="false"),
    "banjo_deko3d_txq_verify.toml": dict(PROFILING, **RENUT, banjo_deko3d_txq_verify="true"),
    "banjo_deko3d_early_z_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_early_z="false"),
    "banjo_deko3d_gpr32_on.toml": dict(PROFILING, **RENUT, banjo_deko3d_gpr32="true"),
    "banjo_renut_stall_present_on.toml": dict(PROFILING, **RENUT, banjo_renut_stall_present="true"),
    "banjo_deko3d_light_branch_on.toml": dict(PROFILING, **RENUT, banjo_deko3d_light_branch="true"),
    "banjo_renut_game_gamma_on.toml": dict(PROFILING, **RENUT, banjo_renut_game_gamma="true"),
    "banjo_renut_device_swizzle_off.toml": dict(PROFILING, **RENUT, banjo_renut_device_swizzle="false"),
    # 2026-10-20: the two GPU A/Bs in one run each (in-run targets 15 / 16): the "[renut A/B]" line's GPU times of the
    # same scenes with and without the constant.
    "banjo_deko3d_light_branch_ab.toml": dict(PROFILING, **RENUT, banjo_deko3d_light_branch="true",
                                              banjo_renut_ab_target="15", banjo_renut_ab_s="4"),
    "banjo_deko3d_gpr32_ab.toml": dict(PROFILING, **RENUT, banjo_deko3d_gpr32="true", banjo_renut_ab_target="16",
                                       banjo_renut_ab_s="4"),
    # 2026-10-20, NOT exact (the user's call): every 2nd fur shell drawn (11 of Banjo's 21).
    "banjo_renut_fur_shells_2.toml": dict(PROFILING, **RENUT, banjo_renut_fur_shells="2"),
    # 2026-10-20 (package 36d): the bone rows as four 32-bit loads again (its own cache, deko3d_shaders_f447.bin).
    "banjo_deko3d_uam_wide_rows_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_uam_wide_rows="false"),
    # 2026-10-20 (package 36e): input latency without vsync; the portrait's fur (NOT exact).
    "banjo_deko3d_latency_trim_on.toml": dict(PROFILING, **RENUT, banjo_deko3d_latency_trim="true"),
    # (banjo_late_input_gpu's configs, 36l-36r, went with the key in package 36s.)
    # 2026-10-20 (package 36o): sa2-nx's Renut CPU work, ported (docs/renut-cpu-work-from-sa2.md). Every item off (the
    # whole A/B), the replayed draws' chain reuse off (banjo-nx's extension), the core layout off, both verify keys on.
    "banjo_renut_sa2_cpu_off.toml": dict(PROFILING, **RENUT, banjo_renut_snapshot_deltas="false", banjo_renut_capture_batches="false", banjo_renut_neon_compare="false", banjo_renut_callbacks_fast="false", banjo_renut_wake_batch="1", banjo_renut_prefetch_next="false", banjo_renut_done_notify_waiters="false", banjo_renut_batch_clock="false", banjo_renut_chain_reuse="false", banjo_renut_inline_line_compare="false", banjo_renut_stream_mask="false", banjo_renut_texture_defaults_copy="false", banjo_renut_fetch_slots_skip="false", banjo_renut_fetch_data_unread="false", banjo_renut_shared_partial_clear="false", banjo_renut_lean_diagnostics="false", banjo_renut_deferred_chain="false"),
    # 36r: off by default (the console's 36q profile: the reuse checks cost the render thread more than they saved).
    "banjo_renut_chain_reuse_on.toml": dict(PROFILING, **RENUT, banjo_renut_chain_reuse="true",
                                            banjo_renut_chain_reuse_replays="true"),
    "banjo_renut_inline_compare_on.toml": dict(PROFILING, **RENUT, banjo_renut_inline_line_compare="true"),
    # 2026-10-20 evening (package 36q): the GPU A/B pack, light branch (target 15) and gpr32 (target 16) in one run:
    # both keys on, the in-run A/B rotating over the two (banjo_renut_ab_targets), 4 s halves.
    "banjo_gpu_ab_pack.toml": dict(PROFILING, **RENUT, banjo_deko3d_light_branch="true", banjo_deko3d_gpr32="true",
                                   banjo_renut_ab_targets='"15,16"', banjo_renut_ab_s="4"),
    # 36q: off by default (a hang at the first Swap in Ryujinx, 4 of 12 runs); the on config to try it.
    "banjo_renut_core_layout_on.toml": dict(PROFILING, **RENUT, banjo_renut_core_layout="true"),
    "banjo_renut_cpu_verify.toml": dict(PROFILING, **RENUT, banjo_renut_snapshot_deltas_verify="true",
                                         banjo_renut_chain_reuse_verify="true"),
    "banjo_renut_portrait_fur_shells_3.toml": dict(PROFILING, **RENUT, banjo_renut_portrait_fur_shells="3"),
    # 2026-10-20 (package 36g): the fur skinned once a frame (exact), on and as an in-run A/B (target 17).
    "banjo_deko3d_skin_once_on.toml": dict(PROFILING, **RENUT, banjo_deko3d_skin_once="true"),
    "banjo_deko3d_skin_once_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_skin_once="false"),
    # 2026-10-20 (input latency at the frame cap): the frame to the GPU in more, earlier parts; on, and in-run A/B 18.
    "banjo_deko3d_eager_kicks_on.toml": dict(PROFILING, **RENUT, banjo_deko3d_eager_kicks="true"),
    "banjo_deko3d_eager_kicks_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_eager_kicks="false"),
    "banjo_deko3d_eager_kicks_ab.toml": dict(PROFILING, **RENUT, banjo_deko3d_eager_kicks="true",
                                             banjo_renut_ab_target="18", banjo_renut_ab_s="4"),
    # 2026-10-21 (package 36u): SMAA on the final composite (render/smaa.cpp), on in every config (the user's call): the
    # high preset, in-run A/B 19, comparison pictures.
    "banjo_renut_smaa_high.toml": dict(PROFILING, **RENUT, banjo_renut_smaa="true", banjo_renut_smaa_quality="2"),
    # 2026-10-21 (package 36x): every fur shell again (75 % in renut.toml, the user's call; NOT exact).
    "banjo_renut_fur_shells_all.toml": dict(PROFILING, **RENUT, banjo_renut_fur_shells_percent="100"),
    # 2026-10-21 (package 36y): the dialogue portrait as the game draws it (50 % shells and 2x MSAA in renut.toml).
    "banjo_renut_portrait_full.toml": dict(PROFILING, **RENUT, banjo_renut_portrait_fur_shells_percent="100",
                                           banjo_renut_portrait_msaa_samples="0"),
    # 2026-10-21 (package 36z): the game's own 720p (648p handheld, 900p docked with the HUD at 1080p in renut.toml).
    # 2026-10-21 (package 36z3): draws wait for their shader compiles again (as before 36z3).
    "banjo_deko3d_async_variants_off.toml": dict(PROFILING, **RENUT, banjo_deko3d_async_variants="false"),
    # 2026-10-21 (package 36z2): Renut's host mip levels for targets above 1x again (off in renut.toml, the user's call).
    "banjo_renut_host_mips_on.toml": dict(PROFILING, **RENUT, banjo_renut_host_mips="true"),
    "banjo_renut_native_720p.toml": dict(PROFILING, **RENUT, banjo_renut_render_height_handheld="720",
                                         banjo_renut_render_height_docked="720",
                                         banjo_renut_display_height_docked="720"),
    "banjo_renut_smaa_ab.toml": dict(PROFILING, **RENUT, banjo_renut_smaa="true", banjo_renut_ab_target="19",
                                     banjo_renut_ab_s="4"),
    "banjo_renut_smaa_compare.toml": dict(PROFILING, **RENUT, banjo_renut_smaa="true",
                                          banjo_renut_diag_smaa_compare_s="15"),
    "banjo_deko3d_skin_once_ab.toml": dict(PROFILING, **RENUT, banjo_deko3d_skin_once="true",
                                           banjo_renut_ab_target="17", banjo_renut_ab_s="4"),
    "banjo_renut_diag_fur_vertex_only.toml": dict(PROFILING, **RENUT, banjo_renut_diag_gpu_shaders="true",
                                                  banjo_renut_diag_null_ps="true"),
    # Package 34 (CPU): the render thread's new work off, one at a time.
    "banjo_renut_store_cards_off.toml": dict(PROFILING, **RENUT, banjo_renut_store_cards="false"),
    "banjo_renut_cpu_memos_off.toml": dict(PROFILING, **RENUT, banjo_renut_cpu_memos="false"),
    "banjo_renut_pipeline_memo_off.toml": dict(PROFILING, **RENUT, banjo_renut_pipeline_memo="false"),
    # Package 28: renut.toml boots Renut's renderer; this is it with banjo-nx's renderer.
    "banjo_renderer_native.toml": dict(PROFILING, banjo_renderer='"native"', disable_msaa="false"),
    # Renut's renderer with banjo-nx's tuning of it off (the four keys; the threads unpinned).
    "banjo_renut_tuning_off.toml": dict(PROFILING, **RENUT, banjo_renut_pipeline_cache="false",
                                        banjo_renut_precise_barriers="false", banjo_renut_upload_cached="false",
                                        banjo_renut_pmull_hash="false", banjo_renut_push_point_only="false",
                                        banjo_renut_capture_pool="false", banjo_renut_neon_swap="false",
                                        banjo_renut_shared_dedupe="false", banjo_renut_queue_notify_idle="false",
                                        banjo_renut_linked_stages="false",
                                        banjo_ring_core="-1",
                                        banjo_background_compile_core="-1"),
    # One part at a time, for a whole run each.
    "banjo_renut_upload_uncached.toml": dict(PROFILING, **RENUT, banjo_renut_upload_cached="false"),
    # Renut's renderer makes real multisampled targets for the game's 2x MSAA scene; banjo-nx's renderer draws it at one
    # sample per pixel on the console. The game-level disable_msaa gives Renut's renderer that same one-sample image.
    "banjo_renut_msaa.toml": dict(PROFILING, **RENUT, disable_msaa="false"),
    # Renut's own default for the post-processing buffers (16-bit), against the console's 8-bit of every other config.
    "banjo_renut_post_high.toml": dict(PROFILING, **dict(RENUT, renut_post_process_precision='"High"')),
    "banjo_renut_barriers_full.toml": dict(PROFILING, **RENUT, banjo_renut_precise_barriers="false"),
    "banjo_renut_xxh3.toml": dict(PROFILING, **RENUT, banjo_renut_pmull_hash="false"),
    "banjo_renut_unpinned.toml": dict(PROFILING, **RENUT, banjo_ring_core="-1", banjo_background_compile_core="-1"),
    # Renut's renderer with the CPU master switch off (the game code's enhancements off too: the CPU A/B of package 26
    # under Renut's renderer).
    "banjo_renut_cpu_off.toml": dict(PROFILING, **RENUT, banjo_cpu_enhancements="false", log_file_writer="false"),
}


def apply(text, changes):
    # 2026-10-21: renut.toml ends with the per-player input sections ([input_p1], [input_p2]); a key renut.toml does
    # not have goes before the first section (in TOML every line after a [section] header belongs to it), and only
    # the top-level part is searched for the keys.
    first = re.search(r"^\[", text, re.M)
    top, sections = (text[:first.start()], text[first.start():]) if first else (text, "")
    for key, value in changes.items():
        line = "%s = %s" % (key, value)
        pattern = re.compile(r"^%s\s*=.*$" % re.escape(key), re.M)
        if pattern.search(top):
            top = pattern.sub(line, top)
        else:
            top = top.rstrip("\n") + "\n" + line + "\n" + ("\n" if sections else "")
    return top + sections


# Package 28: configs of banjo-nx's renderer's own keys (its A/B targets, tests and diagnostics), which boot that
# renderer now that renut.toml boots Renut's. The rest (master switches, CPU, codegen, profiling, pacing) keep
# renut.toml's renderer.
NATIVE_ONLY = {
    "banjo_direct_ab.toml", "banjo_memo_ab.toml", "banjo_upload_uncached.toml", "banjo_tiles_once_ab.toml",
    "banjo_vertex_persistent_ab.toml", "banjo_shader_load_memo_ab.toml", "banjo_zcull_greater.toml",
    "banjo_deferred_off.toml", "banjo_emulation_items_off.toml", "banjo_transfers_off.toml", "banjo_gpu_test.toml",
    "banjo_all_tests.toml", "banjo_api_ops_off.toml", "banjo_dead_draws_ab.toml", "banjo_early_alpha_ab.toml",
    "banjo_index_persistent_ab.toml", "banjo_texture_signs_ab.toml", "banjo_clear_merge_ab.toml",
    "banjo_vfetch_block_ab.toml", "banjo_branchless_ab.toml", "banjo_bool_spec_ab.toml",
    "banjo_stencil_fetch_3d_ab.toml", "banjo_lean_barriers_ab.toml", "banjo_resolve_clear_merge_ab.toml",
    "banjo_pred_select_ab.toml", "banjo_pred_select_ps_ab.toml", "banjo_tess_grid_ab.toml", "banjo_clamp_cbuf_ab.toml",
    "banjo_sign_mask_skip_ab.toml", "banjo_vs_outputs_unwritten_ab.toml", "banjo_bone_padding_ab.toml",
    "banjo_robustness_off_ab.toml", "banjo_edram_checks_cpu_ab.toml", "banjo_shaders25_ab.toml",
    "banjo_shaders25_off.toml", "banjo_shaders21_ab.toml", "banjo_ring_core_ab.toml", "banjo_ring_decoder_ab.toml",
    "banjo_vs_hash_given_ab.toml", "banjo_d3d_records_ab.toml", "banjo_d3d_records_native_ab.toml",
    "banjo_d3d_host_submit_ab.toml", "banjo_d3d_records_off.toml", "banjo_stencil_fetch_3d_probe.toml",
    "banjo_shaders_old.toml", "banjo_fragments.toml", "banjo_diagnostics.toml", "banjo_ring_decoder_off.toml",
    "banjo_ring_decoder_core0.toml", "banjo_api_clear_poison.toml", "banjo_d3d_native_off.toml",
    "banjo_deferred_ab.toml", "banjo_deferred_ab_core2.toml", "banjo_gpu_groups.toml", "banjo_literals_cbuf_ab.toml",
    "banjo_stencil_clear_partial_ab.toml", "banjo_import9_steps.toml",
}


def main():
    base, out = sys.argv[1], sys.argv[2]
    unknown = NATIVE_ONLY - set(VARIANTS)
    assert not unknown, unknown
    text = open(base, encoding="utf-8").read()
    os.makedirs(out, exist_ok=True)
    # Old configs go first: a stale test config with outdated keys must not stay in the package.
    for name in os.listdir(out):
        if name.endswith(".toml"):
            os.remove(os.path.join(out, name))
    for name, changes in VARIANTS.items():
        # Package 28: renut.toml boots Renut's renderer; the configs of banjo-nx's renderer's own keys boot that one.
        if name in NATIVE_ONLY and "banjo_renderer" not in changes:
            changes = dict(changes, banjo_renderer='"native"', disable_msaa="false")
        header = "# %s: generated by tools/make_enhancement_configs.py from renut.toml; changed keys: %s\n" % (
            name, ", ".join("%s = %s" % kv for kv in changes.items()) or "none")
        open(os.path.join(out, name), "w", encoding="utf-8").write(header + apply(text, changes))
        print(os.path.join(out, name))


if __name__ == "__main__":
    main()
