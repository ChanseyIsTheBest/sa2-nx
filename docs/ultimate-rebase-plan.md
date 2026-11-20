# Rebasing sa2-nx on the ultimate base: the inventory and the method

Written 2026-11-19, before any code moved; sorted into the three lists below the same day (the user's word through the
base's session: a full rebase, grafting only what SA2 needs). The rebase starts from sa2-nx 1.0.6 (main, baf523b) and
the ultimate base (D:\renut-ultimate\base, branch `ultimate`, commit 73b7b731, ports 1-189). The base's working tree had
979 files that differ from that commit only in line endings, so the commit is what is taken. The base's later ports
(191-234, to 764410eb) came in on 2026-11-20 with its sync tool: docs/ultimate-sync.md.

## The method: a full rebase

- The branch starts as the base's whole tree (e2099f33, "The ultimate base at 73b7b731"; its blobs byte for byte). That
  is the project from now on. sa2-nx's structure, its SDK and renderer copies, and its versions of the files the base
  also has are not kept.
- Katamari's layer goes (36bba673).
- From sa2-nx, only what SA2 needs and the base does not have is grafted, a commit per piece, each with its line in
  list (a) or (c) below. Where the base has its own form of something sa2-nx also did, the base's form wins (list
  (b)). Where a shared file needs SA2's lines, the base's file is taken and only those lines go back.
- Nothing is grafted because it was there: a piece SA2 runs without, or one the base does better, stays behind
  (list (d)), with the reason.
- `docs/ultimate-sync.md` records the base commit the branch is at: later base ports come in with
  `tools/ult_port_sync.py --range 73b7b731..<base commit>`.

## What the file comparison says

`git ls-tree` of both commits, blob by blob (`out/tree_compare.tsv` in the worktree): 1,880 files the same, 2,172 in
both but different (nearly all of sa2-nx's SDK and renderer: the base carries a later form of each), 489 only in sa2-nx
(220 of them 1.0.4's pgo/*.gcda), 19,584 only in the base (18,873 of them sdk/thirdparty, which sa2-nx fetched instead
of tracking). banjo-nx took sa2-nx's CPU rounds and Renut work (`docs/renut-cpu-work-for-banjo.md`) before the base began
(banjo-nx 894d3256, 2026-10-29), and the base took sa2-nx's later queue and core-layout work through dishonored-nx (ports
103 and 105). The base mentions sa2-nx in 136 files.

## Decisions

| What | Decision | Why |
|---|---|---|
| The project's internal name | `renut`, as the base (generated `renut_recomp.N.cpp`, `renut_pch.h`, the CMake targets `renut`, `renut_recomp`, `renut_Data_DLL`) | every base CMake rule, pass and script names `renut_*`; the fewest changes to base files keep later syncs clean |
| The app's name | `"sa2"` (`renut_app.h`'s Create) | the SDK names the save folder (the user folder / "sa2") and the PC's settings file after it: a 1.0.6 player's saves and sa2.toml are found as before |
| What the player sees | `switch/sa2-nx/sa2.nro`, `sa2.toml` next to it, 1.0.6's name, author and icon; NACP version "1.0.6-ult" until a release is decided | a player's install keeps working |
| The game files | `assets/game_root` and `app/assets`: directory junctions to the main checkout's `assets/game_root` | the prompt; `app/assets` is the base's own convention, which its passes read (`ro_constants --xex`) |
| The two code modules | the main module into `app/generated` (flat, as the base's passes expect), the stage DLL into `app/generated/data_dll` | docs/sa2-ultimate-notes.md, "Two code modules" |
| The renderer | Renut only (the base's), Vulkan on the PC, deko3d on the Switch; SA2's own Direct3D keeps the state setters (the base's `state.cpp` not built) | 1.0.6 ships Renut only and leaves the setters to SA2's Direct3D (21076; the base's native setters were written at 7645 / 5632) |
| The PC reference | the 1.0.6 NRO in Ryujinx and the rebased PC build | the prompt |

## (a) SA2's layer: grafted

| Piece | From 1.0.6 | In the rebase | Why it is needed |
|---|---|---|---|
| Code generation config | `sa2_manifest.toml`, `overrides.toml`, `overrides_data_dll.toml`, `perf_overrides.toml` / `codegen_flavor.toml`, `indirect_promotions.txt` | `app/renut_manifest.toml`, `renut_config.toml` (SA2's keys), `config/codegen_keys_data_dll.toml`, `overrides.toml`, `overrides_data_dll.toml`, `indirect_promotions.txt`; `config/renut_funcs.toml` and `renut_hooks.toml` emptied | SA2's functions, switch tables, setjmp / longjmp, its two modules; each entry keeps its evidence |
| Renut's device layout at Direct3D 2.0.21076 | `d3d/d3d.h`, `d3d_native.h`, the hook files' constants | the base's files with SA2's values (`tools/sa2_renut_layout.py`, every value from 1.0.6 by name; the viewport, port 138's field, read from SA2's GetViewport at +13024; SA2's gamma ramp at +15408; the delta capture's private chunk bounds 11008-12224) | the base's are Katamari's (5632) |
| Renut's hook bindings | the "sa2-nx: SA2's hooks" blocks of `d3d/hooks/{device,draw,resource,shader}.cpp` | Katamari's bindings out, SA2's blocks in (SA2's own device creation, ring segment allocator, SynchronizeToPresentationInterval); the hooks 1.0.6 shared with its own Direct3D hooks are Renut's own | Katamari's addresses are other functions in SA2 |
| The Swap hook's SA2 lines | `device.cpp` | the base's Swap with SA2's frame pacing first, SA2's gamma ramp offset, no `ResetAllState` (SA2's setters keep the dirty marks the delta chain reads) | SA2's game speed follows its frames; the base's Swap is Katamari's |
| Frame pacing | `gpu/sa2_renut_pacing.cpp/.h` (`sa2_renut_vblank_pacing`, `sa2_vsync_relaxed`) | as it was, `sa2_vsync_relaxed` defined there with the base's non-CPU master | the base has no game-interval pacing (its frame cap is a fixed rate) |
| The window scissor's shadow | `src/me/sa2_me_hooks.cpp`'s hook of `sub_8254FCE8` | `src/sa2_renut_hooks.cpp` | Renut reads the window packet SA2's Direct3D never writes |
| The screen width | `gpu/guest/sa2_screen.cpp` | as it was (the base's `widescreen.cpp` and `photo_capture.cpp` not built, as in 1.0.6) | N&B's / Katamari's game code otherwise |
| The stage DLL in the NRO | `src/sa2_static_modules.cpp`, the CMake partial link | as it was, under the base's project name (`renut_Data_DLL`) | an NRO cannot load a DLL |
| OpenSA2's control options (1.0.5) | `src/sa2_controls.cpp` | as it was, on the base's memory helpers | the player's options |
| The keys other base files read, and 1.0.6's renamed player keys | (Katamari's `katamari_keys.cpp` in the base) | `src/sa2_keys.cpp`: the base's frame cap and the not-built keys; `sa2_renut_double_buffer` read and handed to `pacman_renut_double_buffer`; `sa2_renut_msaa` read and ignored with a log line (docs/sa2-ultimate-notes.md, "Settings") | a 1.0.6 sa2.toml keeps working |
| The tomls | `sa2_switch.toml`, `sa2_switch_defaults.toml` | the player's as it was; the compiled-in one with 1.0.6's decisions under the base's names (`tools/sa2_effective_keys.py` checks them key by key) | the shipped settings |
| The icon and the NRO's identity | `app/icon.jpg`, CMake NAME / AUTHOR | `tools/switch/icon/sa2_icon.jpg`, the CMake call | the homebrew menu entry |
| The game layer's app wiring | `sa2_app.h` | the base's `renut_app.h` with SA2's lines (the app name, the compiled-in settings, the game files' search with the pack) | |
| The lockstep frame comparison | `src/sa2_capture.cpp/.h`, the SDK's `vblank_per_swap`, `tools/compare_raw.py`, `compare_video.py` | grafted: the recorder (Renut's path only, with `sa2_video_hash_only`), `tools/sa2_compare_frames.py` (with `--align`: runs are paired by content), `sa2_lockstep_run.ps1`; NOT `vblank_per_swap` (Renut's Swap never calls VdSwap, so it pins nothing; sa2-ultimate-notes.md) | this port's gate: the uninitialised stack word was found only by it, and the base has no lockstep capture |
| The game files as a pack | SDK `pack_device.*`, `pack_storage_switch.cpp`, the keys `asset_pack`, `asset_pack_build`, `tools/build_asset_pack.py` | grafted (ae99203c); PC: 1.0.6's own pack read, 750 of 750 frames identical | a 1.0.6 player's card holds game_root.nxpack and sa2-nx's installer makes it; the base's `.lz4b` (port 61) would need every player to repack |
| SA2's console fixes in its scenes | Chao Garden's resolves, the long-session texture fixes (1.0.2), the 4x MSAA alias clear | each checked against the base's form in step 5 | 1.0.6 draws them right |
| SA2's hot functions | `app/src/cpu/hot` lists, 1.0.4's hand natives (`tools/hand_natives`) | lists emptied (Katamari's); the copies remade by the base's tools in step 7; a hand native only where the base's copies do not cover it | |
| Docs | sa2-nx's `docs/` (records) | to graft as records, with a top note | |

## (b) General work the base already has: left behind, the base's form kept

| sa2-nx's piece | The base's form |
|---|---|
| The CPU rounds of 2026-10-12 and banjo-nx's codegen round (generator options and post-codegen passes) | banjo-nx's copies in the base, `tools/post_codegen.py` and its passes; the generator's `fcti_inline`, `fp_copy_pairs`, `div_inline`, `rotate_word_shifts`, `fp_copy_forwarding`, `dcbt_prefetch`, the setjmp locals fix |
| The flush-mode flow | `tools/flush_mode_flow.py` (and CG-13, ports 123 / 133) |
| frame_traffic.py's three elisions | `BANJO_GEN_*_ELIDE` (off; off-by-default.md section 2) |
| The localized copies, the bulk lists, the guard | the base's machine copies: in place (port 81), register liveness (port 121), the guard, `banjo_hot_poison_dead` |
| `tools/localize_writebacks.py` (changed-only write-backs) | port 121's LiveOut (`live_out_keep`) and `leaf_dead_stores.py` keep only what callers read; whether a write-back the copy does not change is still stored is checked in step 7 before anything is grafted |
| `SA2_GAME_NO_MATH_ERRNO` / `NO_TRAPPING_MATH` | `BANJO_GENERATED_MATH_FLAGS`, `BANJO_GENERATED_NO_TRAPPING_MATH` (off; fp_env_files.py) |
| The SDK's console tuning (critical sections, `kernel_state()`, `XObject` counts, `TranslateVirtual`, signal skips, lock words, the core layout) | the base's SDK; port 103 |
| Renut's game- and render-thread work of 2026-10-20 to 10-28 | the same items as `banjo_renut_*` |
| The render queue as a lock-free ring | port 105, `dishonored_renut_lockfree_queue` |
| Double buffering and the queue drain | `pacman_renut_double_buffer`, `pacman_renut_queue_drain` |
| The colour hand-over | `banjo_deko3d_color_handover` |
| The gamma ramp at scan-out | port 91, `banjo_renut_game_gamma` (with SA2's offset, list (a)) |
| The render resolution by the console's mode | `banjo_renut_render_height_*`, `banjo_renut_display_height_docked` |
| No Vulkan driver in a deko3d NRO | `REXGLUE_SWITCH_NVK` OFF |
| 2 MB blocks (1.0.6) | kameo-nx's `switch_code_blocks.cpp` and `RexGmSetWindowBlocks`, the same code (it reads `katamari.toml`: SA2's line, `sa2.toml`, goes back) |
| The DLC installer (`sa2_dlc.cpp`) | reNut's `renut_engine/dlc_install.cpp` (the running title's packages from `dlc/`; key `renut_dlc_folder`) |
| The language (`sa2_language.cpp`, `language = auto/english/...`) | the SDK's `language` key (blur-nx's): the same names, case-insensitive; "auto" falls to the console's language as before |
| The PGO plumbing (`sa2_pgo.cpp`, `pgo_merge.ps1`) | `banjo_pgo.cpp`, `tools/pgo_merge.py` (step 8) |
| The Joy-Con sideways stick fix, the input maps, the Xbox layout | the base's input system (`input_joycon_sideways_stick`, `[input_pN]`, `input_xbox_layout`) |
| `tools/cg13_census.ps1`, `stfs_extract.py`, `switch_check.py`, `pointer_fixpoint.py`, `census_*.py`, `arm_cost_census.py`, `loop_body_census.py`, `generated_code_census.py` | the base's copies |
| The setter marks and dirty tags (`sa2_renut_setter_marks`) | the base has its own form (`banjo_renut_tags`), driven by the native setters; SA2's tag table (`sa2_setter_marks_tags.h`) is SA2's layer and comes over in that form only if a measurement asks for it; until then `banjo_renut_tags` is off in SA2's settings (the deltas compare their chunks: exact) |

## (c) General work the base lacks

| Piece | Grafted or left | Handoff |
|---|---|---|
| Guest DLL modules linked into an executable (`rex/system/static_modules.h`, `KernelState::LoadUserModule`) | grafted (the Switch needs it for SA2's stage DLL) | yes: a second module in any port |
| The post-codegen steps of a second module (`tools/post_codegen_modules.py`, run on every module folder) | grafted | yes |
| FFmpeg's nine configure-made files the base's tree lacks (cb3a6f56) | grafted | yes: a fresh checkout of the base does not build without them (now the base's port 190) |
| The generator's module targets without a codegen target (`dll_targets_cmake.inja`: the Switch cross build has none) | fixed (fd930520) | yes, with the second module |
| 1.0.6's gamma ramp as entries (`sa2_renut_gamma_entries`: the GPU rounds the entry, as 1.0.6's float ramp; the base rounds it itself) | SA2 key, on | yes: a finding for ports compared against a float-ramp build |
| The asset pack (list (a)) | grafted | yes: the base's own answer is the installer's .lz4b files |
| `sa2_renut_direct_resolve_mips` (resolve mip levels drawn straight into the texture's levels, deko3d; on in 1.0.6) | to decide in step 5 against the base's resolve path | if grafted |
| `tools/pgo_function_counts.py` (a PGO profile's executed instructions per function) | candidate for step 7 (the base's `switch_hot_bulk.py` takes console profiles; the PGO counts cover more scenes) | if grafted |
| `tools/hand_natives/` | candidate for step 7 only where the base's copies do not cover a function | |
| `tools/nro_set_version.py`, `tools/check_switch_build.py` | candidates for step 9 | |

## (d) Left behind, and why

| Piece | Why |
|---|---|
| `app/src/me/native/` (72 files), `me_native_*`, `sa2_api_*`, `sa2_d3d_api.*`, `sa2_d3d_trace.cpp`, `sa2_debug_hooks.cpp` (an empty template), the `sa2_native_*` / `sa2_api_*` / `sa2_cpu_ring_*` keys | sa2-nx's earlier renderers and their tracing: 1.0.6 runs Renut only on the Switch; their keys in an old toml are ignored |
| `sa2_crash_log.cpp` (Windows host exception log) | PC only; the base has port 154's crash-report tools |
| `patches/` | the base carries the whole SDK and renderer with its own patches (`tools/renut_patches`) |
| `tools/win/*`, `tools/fetch_thirdparty.py` | the base's `tools/*.bat` / `*.ps1` and its tracked `sdk/thirdparty` |
| `mesa/` | no Vulkan driver on the Switch by default |
| `pgo/` (1.0.4's profile) | it fits sa2-nx's code: step 8 trains a new one |
| `sa2_io_*` (sa2-nx's read-ahead) | the base's host file reads (port 146); on the Switch SA2's files come from the pack |

## The base's Katamari layer, which went (36bba673)

`app/src/katamari_keys.cpp` (replaced by `sa2_keys.cpp`), `katamari_builtin_config.h.in`, `app/src/cpu/katamari_*`,
`app/katamari_switch*.toml`, `tools/make_katamari_tomls.py`, `dist/switch/katamari-nx`, `pgo/` (`pgo-inplace/` was
empty), `tools/katamari_*.py`, the forwarder's Katamari files, the icon; and with SA2's pieces: the hot lists and hand.txt,
`app/overrides.toml`, `config/renut_funcs.toml`, `config/renut_hooks.toml`, `app/indirect_promotions.txt`, Renut's
Katamari bindings. Katamari's hand natives in `banjo_hand_natives.h` stay compiled (unused) until step 7.

## The order of work

1. This plan; the base's tree; Katamari's layer out.
2. Code generation for SA2's two modules (`docs/sa2-ultimate-notes.md`, "Two code modules").
3. The PC build to gameplay; SA2's layer piece by piece; `tools/run_pc_verify.ps1`.
4. The calling-convention censuses; the setjmp / longjmp check.
5. The Switch build, Ryujinx against 1.0.6, the keys of off-by-default.md section 1 and 1.0.6's own fixes.
6. The not-exact forms and their censuses (`docs/sa2-near-exact.md`).
7. Hot functions; 8. PGO + LTO ready; 9. the console package; 10. handoffs.

The progress log is `docs/progress-log.md`.
