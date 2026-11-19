# Rebasing sa2-nx on the ultimate base: the inventory and the method

Written 2026-11-19, before any code moved. The rebase starts from sa2-nx 1.0.6 (main, baf523b) and the ultimate base
(D:\renut-ultimate\base, branch `ultimate`, commit 73b7b731, ports 1-189). The base's working tree had 979 files that
differ from that commit only in line endings, so the commit is what is taken.

## What the file comparison says

`git ls-tree` of both commits, blob by blob (`out/tree_compare.tsv` in the worktree; made by a scratch script):

| | Files |
|---|---|
| The same in both | 1,880 (most of XenosRecomp, uam, plume's unchanged files, some tools) |
| In both, different | 2,172 (nearly all of sa2-nx's SDK and renderer: the base carries a later form of each) |
| Only in sa2-nx | 489 (220 of them pgo/*.gcda: 1.0.4's profile) |
| Only in the base | 19,584 (18,873 of them sdk/thirdparty, which sa2-nx fetches with tools/fetch_thirdparty.py instead of tracking) |

The lineage explains the "different" column. sa2-nx's SDK is rexglue 0.10.0 with `patches/rexglue-sdk-v0.10.0.patch`,
and its renderer is Renut from banjo-nx c319b29 with `patches/renut-banjo-nx-c319b29.patch`. banjo-nx took sa2-nx's CPU
rounds and Renut work (`docs/renut-cpu-work-for-banjo.md`) before the base began (banjo-nx 894d3256, 2026-10-29), and the
base took sa2-nx's later queue and core-layout work through dishonored-nx (ports 103 and 105). The base mentions sa2-nx in
136 files. So for these files the base's form wins, and only SA2's own lines go back on top.

## Method

1. The worktree takes the base's tree in one commit ("the ultimate base at 73b7b731"). This plan stays in it.
2. Katamari's game layer goes in one commit: `app/src/katamari_*`, the Katamari hooks and lists, `pgo/` and
   `pgo-inplace/` (Katamari's profile), its tomls and `tools/make_katamari_tomls.py`, `dist/switch/katamari-nx`, its DLC
   pack list, its shader-file tools and records, its icon.
3. SA2's layer comes back piece by piece, a commit each, in the order of the table below. Where a shared file differs,
   the base's form is kept and SA2's own lines are put back (building.md section 7). Each piece names its source.
4. `docs/ultimate-sync.md` records the base commit the branch is at, so later base ports come in with
   `tools/ult_port_sync.py --range 73b7b731..<base commit>`.

### Decisions

| What | Decision | Why |
|---|---|---|
| The project's internal name | `renut`, as the base and Katamari (generated `renut_recomp.N.cpp`, `renut_pch.h`, the CMake target `renut`) | Every base CMake rule, pass and script names `renut_*` first; port 186 lets the passes take another name, but CMake does not. Fewest changes to base files means cleaner `ult_port_sync.py` merges later. SA2's own sources that name `sa2_recomp` / `sa2_pch.h` are changed to the base's names |
| What the player sees | `switch/sa2-nx/sa2.nro`, `sa2.toml` next to it, the same title, author, icon and NACP as 1.0.6 | A player's install must keep working (the prompt) |
| The game files | `assets/game_root`: a directory junction to the main checkout's (2.4 GB, never copied). The manifest reads `../assets/game_root/default.xex` as 1.0.6 did | The prompt; the base's own `app/assets` is Katamari's convention and is not used |
| The two code modules | the main module into `app/generated` (flat, as the base's passes expect), the stage DLL into `app/generated/data_dll` | Step 2 of the work: decided after the first code generation, written in `docs/sa2-ultimate-notes.md` |
| The game files on the card | SA2's `game_root.nxpack` + `.nxidx` (sa2-nx's pack device), not the base's `.lz4b` files (port 61) | A 1.0.6 player's card has the pack, and sa2-nx's installer makes it; the `.lz4b` form would need every player to repack. The base's `.lz4b` reader stays in the build for any loose `NAME.lz4b` |
| The renderers | Renut only (the base's), on Vulkan on the PC and deko3d on the Switch | 1.0.6 ships Renut only. sa2-nx's earlier renderers (the native PM4 renderer `app/src/me/native`, the API renderer `sa2_api_*` / `sa2_d3d_api.*`) stay behind: they were the road to Renut, and their keys do nothing in 1.0.6 on the Switch |
| Renut's state setters | SA2's own code, as 1.0.6 (the base's `d3d/hooks/state.cpp` not bound for SA2) | 1.0.6 draws from the device shadows SA2's Direct3D writes; the base's native setters were written from N&B's and Katamari's disassembly (XDK 7645 / 5632) and SA2's 21076 setters differ (SetTexture's mip-address translation). Binding them is a separate step with its own A/B, not part of a pixel-identical rebase |
| The PC reference | the 1.0.6 NRO in Ryujinx and the rebased PC build (Renut on Vulkan) | The prompt: every comparison is against 1.0.6 |

## The inventory

"Base" names the base's form; "comes over" means SA2's form is put on top of the base's tree.

### Already in the base: the base's form is kept, nothing of SA2's comes over

| sa2-nx piece | The base's form |
|---|---|
| CPU rounds 1-3 of 2026-10-12 and banjo-nx's codegen round (generator options, post-codegen passes, SDK tests) | banjo-nx's copies in the base: `tools/post_codegen.py` and its passes (`pch_no_volatile`, `loop_barriers`, `indirect_calls`, `pch_cold_traps`, `direct_calls`, `guest_cas`, `flush_mode_flow`, `vec_byteswap`, `pch_neon_shuffles`, `ro_constants`, `ro_constants_cfg`, `float_loads`, `frame_traffic`, `loop_locals`, `leaf_locals`, `fp_sign_ops`, `leaf_dead_stores`, `single_floats`, `vperm_one_source`, `inline_memcalls`, `vec_unaligned_stores`, `dead_ctx_defs`, `flush_hoist`, `lr_preserve`, `vsel_neon`, `inline_leaves`), the generator's `fcti_inline`, `fp_copy_pairs`, `div_inline`, `rotate_word_shifts`, `fp_copy_forwarding`, `dcbt_prefetch`, the setjmp locals fix |
| The flush-mode flow | `tools/flush_mode_flow.py` (and CG-13, port 123/133, on the Switch) |
| frame_traffic.py's three elisions (`SA2_GEN_*_ELIDE`, off) | `BANJO_GEN_LR_MOVES_ELIDE`, `BANJO_GEN_NV_SAVES_ELIDE`, `BANJO_GEN_BACK_CHAIN_ELIDE` (off; off-by-default.md section 2 cites sa2-nx) |
| The localized native copies (`app/generated/hot`, `tools/localize_native.py`, the bulk lists, the guard) | the base's machine copies: in place on the Switch (port 81), whole-program register liveness (port 121), the guard and `banjo_hot_poison_dead` |
| `SA2_GAME_NO_MATH_ERRNO` / `NO_TRAPPING_MATH` | the base's flags (`BANJO_GENERATED_NO_TRAPPING_MATH` off; fp_env_files.py keeps -ftrapping-math on the FPSCR writers) |
| The SDK's console tuning: critical sections from the processor block, inline `kernel_state()`, `XObject` counts, `TranslateVirtual`, signal skips, the lock words, the core layout (`RexSetGuestThreadCoreLayout`) | the base's SDK (and port 103: the layout applied at each thread's own wait) |
| Renut's game- and render-thread work (device deltas, constants by the chain, narrow and NEON compares, capture batches, fast paths, wake batch, prefetch, done signal, batch clock, chain reuse, shared constants, stream mask, texture defaults, lean diagnostics, clear repeat, lazy resolve mips) | the same items under `banjo_renut_*` names (banjo-nx took them; renut-cpu-work-for-banjo.md) |
| The render queue as a lock-free ring (`sa2_renut_lockfree_queue`) | port 105, `dishonored_renut_lockfree_queue` |
| Double buffering, the display queue drain (`sa2_renut_double_buffer`, `sa2_renut_queue_drain`) | `pacman_renut_double_buffer`, `pacman_renut_queue_drain` |
| The colour hand-over (`sa2_renut_colour_handover`) | `banjo_deko3d_color_handover` |
| The setter marks and dirty tags (`sa2_renut_setter_marks`, `_check`, `marks_copy`) | `banjo_renut_tags`, `banjo_renut_tags_check` (to confirm when the hooks are bound: SA2's marks come from its setter hooks, step "Renut at SA2's addresses") |
| The display gamma ramp at scan-out (`sa2_renut_gamma_ramp`) | port 91, `banjo_renut_game_gamma` (a table at scan-out): needs SA2's ramp offset (+15408) |
| MSAA (`sa2_renut_msaa`) | `msaa_level` (port 118) |
| The render resolution by the console's mode (`sa2_renut_dock_resolution`, `sa2_render_height_*`) | `banjo_renut_render_height_docked` / `_handheld`, `banjo_renut_display_height_docked` |
| No Vulkan driver in a deko3d-only NRO | `REXGLUE_SWITCH_NVK` OFF (banjo-nx's form of sa2-nx's `REXGLUE_SWITCH_NO_NVK`) |
| Game memory and the NRO's code on 2 MB blocks (1.0.6) | kameo-nx's `sdk/src/ui/switch_code_blocks.cpp` and `RexGmSetWindowBlocks`: the same code; the base's reads `katamari.toml`, which SA2's tree changes to `sa2.toml` (a handoff: the file name from the build) |
| `tools/cg13_census.ps1`, `tools/stfs_extract.py`, `tools/switch_check.py`, `tools/pointer_fixpoint.py`, `tools/census_*.py`, `tools/arm_cost_census.py`, `tools/loop_body_census.py`, `tools/generated_code_census.py` | the base's copies |
| The PGO build with the inliner's limit 64 and growth 12 % for game code | to compare with the base's link settings in step 8 |
| The Joy-Con sideways stick fix, input remapping, the Xbox layout | the base's input system (`input_joycon_sideways_stick`, `input_xbox_layout`, `[input_pN]`) |

### Comes over: SA2's own layer

| Piece | Where in 1.0.6 | Notes |
|---|---|---|
| Code generation config | `app/sa2_manifest.toml` (as `app/renut_manifest.toml`), `overrides.toml` (both modules), `overrides_data_dll.toml`, `perf_overrides.toml` (as the base's `renut_config.toml` keys), `codegen_flavor.toml`, `indirect_promotions.txt`, `hot_functions.ld` | each entry keeps its evidence; the base's config keys are compared one by one with SA2's |
| The game layer | `app/src/sa2_capture.cpp/.h`, `sa2_controls.cpp` (OpenSA2's options, 1.0.5), `sa2_crash_log.cpp`, `sa2_d3d_trace.cpp`, `sa2_debug_hooks.cpp`, `sa2_dlc.cpp/.h` (the Battle DLC), `sa2_language.cpp/.h`, `sa2_pgo.cpp/.h` (against the base's `banjo_pgo.cpp`), `sa2_static_modules.cpp` (the stage DLL linked into the NRO), `sa2_app.h` (against `renut_app.h`), `sa2_builtin_config.h.in` | against the base's app (`renut_app.h`, `main.cpp`, `banjo_pgo.cpp`, the game menu) |
| Renut at SA2's addresses and Direct3D 2.0.21076 | `d3d/d3d.h`, `d3d_native.h` (the device layout; renut-renderer.md has every offset), the "sa2-nx" hook blocks of `d3d/hooks/{device,draw,shader,resource}.cpp`, `banjo_renut_select.cpp`, `guest/sa2_screen.cpp`, `sa2_renut_pacing.cpp/.h` (the 60 Hz frame pacing: SA2's game speed follows its frames), `sa2_renut_yield.h`, `sa2_line_compare.h` (if the base's NEON compare is another), `sa2_setter_marks*.h`, `sa2_compat/` (only what the base lacks), the window-scissor hook of `src/me/sa2_me_hooks.cpp` (sub_8254FCE8), `src/me/sa2_setter_marks.cpp`, `sa2_render_mode.cpp/.h` | from the base's Katamari layout commit (838c8c41: every constant it moved carries "katamari-nx: old->new") to SA2's values; base fields added since (the viewport, port 138) get SA2's offsets read from SA2's code |
| `sa2_renut_direct_resolve_mips` (on in 1.0.6) | `render/*` | not in the base: comes over if it fits the base's resolve path, else noted (a handoff) |
| The game files as a pack | `sdk/src/filesystem/devices/pack_device.cpp`, `pack_storage_switch.cpp`, `include/rex/filesystem/devices/pack_device.h`, the keys `asset_pack`, `asset_pack_build`, `tools/build_asset_pack.py` | see Decisions |
| The lockstep test path | `vblank_per_swap*` (SDK, `xboxkrnl_video.cpp`), `sa2_capture.cpp`, `tools/compare_raw.py`, `tools/compare_video.py` | the frame comparison is this port's gate (the uninitialised stack word was found only by it) |
| SA2's CPU layer | `app/src/cpu/hot/` (hand.txt, localized.txt, localized_bulk.txt, the hand natives `n_*.h`), `sa2_hot_*`, `sa2_enhancements.*`, `sa2_codegen_checks.cpp`, `sa2_forms_selfcheck.cpp`, `sa2_gpu_wait.cpp`, `sa2_gpu_boost.cpp` | against the base's `banjo_hot_guest.*`, `banjo_hand_natives.h`, `banjo_codegen_checks.cpp`; hand natives only where the base's machine copies do not cover them (step 7) |
| Tools the base does not have | `tools/localize_writebacks.py`, `tools/hand_natives/` (1.0.4's natives and their generators), `tools/pgo_function_counts.py`, `tools/compare_raw.py`, `compare_video.py`, `tools/build_asset_pack.py`, `tools/nro_set_version.py`, `tools/check_switch_build.py`, `tools/sa2_dump_shaders.py`, `tools/switch_profile_report.py` | each checked against the base's equivalent first (the base's `localize_native.py` may already keep only changed write-backs) |
| The tomls | `sa2.toml` (PC), `sa2_switch.toml` (the player's), `sa2_switch_defaults.toml` (compiled in) | the 1.0.6 player keys and their base names: `docs/sa2-ultimate-notes.md` |
| The icon, the NACP, the folder | `app/icon.jpg`, `app/CMakeLists.txt`'s NAME / AUTHOR / VERSION, `switch/sa2-nx` | |
| Docs | `docs/sa2-notes.md`, `renut-renderer.md`, `cpu-rounds.md`, `enhancements.md`, `switch-testing.md`, `porting-guide.md`, `deko3d-renderer-guide.md`, the rest | SA2's records, kept as they are (a top note says what is stale); the base's guides stay as they are |
| Credits | README, THIRD_PARTY_NOTICES.md (OpenSA2 GPL-3.0-only, kameo-nx, reNut, Renut, banjo-nx, the base and its ports) | |

### Left behind, and why

| Piece | Why |
|---|---|
| `app/src/me/native/` (72 files), `me_native_*`, `sa2_api_*`, `sa2_d3d_api.*`, the `sa2_native_*` / `sa2_api_*` / most `sa2_cpu_ring_*` keys | sa2-nx's earlier renderers (masseffect-nx's PM4 renderer and the API renderer): 1.0.6 runs Renut only on the Switch, and the base has Renut on the PC too. Their keys in an old toml are ignored |
| `patches/` (sa2-nx's patches against rexglue 0.10.0, banjo-nx's Renut, plume, FFmpeg, XenosRecomp) | the base carries the whole SDK and renderer, with its own patches (`tools/renut_patches`) |
| `tools/win/*` (sa2-nx's PowerShell wrappers), `tools/fetch_thirdparty.py` | the base's `tools/*.bat` / `*.ps1` and its tracked `sdk/thirdparty` replace them |
| `mesa/` | the base links no Vulkan driver on the Switch by default |
| `pgo/` (1.0.4's profile) | it fits sa2-nx's code, not the rebased code: step 8 trains a new one |
| sa2-nx's I/O read-ahead keys (`sa2_io_*`) | the base's host file reads (tw2de_nx's 4 MB pieces, port 146); on the Switch SA2's files come from the pack |

### The base's Katamari layer, which goes

`app/src/katamari_keys.cpp` and `katamari_builtin_config.h.in` (with the keys SA2's layer must define in its own file:
`frame_cap` and the others `game_menu.cpp` reads), `app/src/cpu/hot`'s Katamari lists, natives and hand.txt,
`app/src/cpu/katamari_*`, `app/katamari_switch*.toml`, `tools/make_katamari_tomls.py`, `dist/switch/katamari-nx`,
`pgo/` and `pgo-inplace/`, `app/overrides.toml`, `app/renut_config.toml`'s game keys, `app/config/renut_funcs.toml` and
`renut_hooks.toml`, `app/indirect_promotions.txt`, `tools/katamari_*.py` and their records, Katamari's icon
(`tools/switch/icon/katamari_icon.jpg`), Renut's Katamari hook bindings (replaced by SA2's).

## The order of work

1. This plan; the base's tree; Katamari's layer out.
2. Code generation for SA2's two modules with the base's generator and passes (`docs/sa2-ultimate-notes.md` records how).
3. The PC build to gameplay; SA2's layer piece by piece; `tools/run_pc_verify.ps1`.
4. The calling-convention censuses; the setjmp / longjmp check.
5. The Switch build, Ryujinx against 1.0.6, the keys of off-by-default.md section 1 and 1.0.6's own fixes.
6. The not-exact forms and their censuses (`docs/sa2-near-exact.md`).
7. Hot functions; 8. PGO + LTO ready; 9. the console package; 10. handoffs.

A progress log is kept in `docs/progress-log.md` (dated entries).
