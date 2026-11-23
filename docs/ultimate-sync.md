# sa2-nx and the ultimate base: where the branch stands

| | |
|---|---|
| The base | renut_nx_ultimate_base, branch `ultimate` (D:\renut-ultimate\base) |
| The base commit this branch is at | **9da84f9b87ab91c25aa0e9510188d3c605e08f6e** (2026-11-23 11:16, "docs: port 263 (sasrt_gpu_seed_cpu_direct off)"; ports 1-263); before it f70d05a4 (2026-11-20, ports 1-235; branch `ultimate-at-f70d05a4`); first taken at 73b7b7315be117d4c4a84f298dfa411e681c9b8a (2026-11-19 10:15, "docs: port 189"; ports 1-189) |
| How it was taken | the base's whole tree as one commit (e2099f33, "The ultimate base at 73b7b731"), byte for byte, then Katamari's layer out and SA2's layer on top (docs/ultimate-rebase-plan.md) |

## Taking the base's later ports

The base's later ports come in with its sync tool (docs/building.md section 7), from the commit above:

```
python tools/ult_port_sync.py --base D:\renut-ultimate\base --port "D:\sonic adventure 2\sa2-nx-ult" --range 9da84f9b..<base commit> --dry-run
```

then without `--dry-run`, the conflicts by hand, a build and the tests. After a sync, change the commit in the table
above to the last base commit taken.

What the sync must leave to a hand port in this tree (SA2 keeps its own form of these files):

- Renut's device layout and hook bindings at SA2's addresses (`app/src/renut_engine/gpu/d3d/d3d.h`, `d3d_native.h`,
  `d3d/hooks/*.cpp`): a base change there names Katamari's offsets and addresses. The tool merges `render/draw.h`
  and `draw.cpp` (shared files), but their device offsets are SA2's too: a new constant there may be Katamari's (port
  213's 0x2F98, SA2's 0x3118; its static_assert caught it).
- `app/CMakeLists.txt`: SA2's sources, the stage DLL's target and its static link on the Switch, the NRO's name.
- The tomls and SA2's keys (`app/sa2_*.toml`, `app/src/sa2_keys.cpp`).

## History

| Date | Base commit | What came in |
|---|---|---|
| 2026-11-19 | 73b7b731 | the first rebase: ports 1-189 |
| 2026-11-20 | 2f590c31 | ports 191-222 with `tools/ult_port_sync.py --range 73b7b731..2f590c31 --skip 190` (port 190 is sa2-nx's own FFmpeg files, already here): 73 files merged cleanly, 4 new, 1 conflict (`app/CMakeLists.txt`: SA2's sources and name with port 193's build stamp, `RENUT_GAME_NAME` "Sonic Adventure 2"), the hand port of `d3d/hooks/shader.cpp` (ports 196, 207: three-way merged into SA2's form without a conflict; the GetFunction hooks stay unbound, their key off, and SA2's containers are 0x11) |
| 2026-11-20 | 764410eb | ports 223-234 (banjo-nx's, from its own rebase) with `--range 2f590c31..764410eb`: 28 files merged cleanly, 8 new tools, 2 conflicts (`app/CMakeLists.txt`: port 231's no-trapping-math condition in SA2's loop over both modules; `tools/deko3d_variant_cache.py`: SA2's `--uam` beside port 232's `--round-guess`); `tools/run_pc.ps1`'s argument quoting now port 225's (the same line sa2-nx-ult had added) |
| 2026-11-20 | f70d05a4 | port 234's follow-up (banjo-nx's b830247a: a draw's shader compile no longer waits behind a prebuild's blocking compile during the compiler's rest, up to 60 s; `Deko3DHurryCompiles`) and port 235 (three analysis tools) with `--range 764410eb..f70d05a4`: 4 files merged cleanly, 2 new. SA2 compiles shaders at run time on the console (its DKSH cache covers the attract sequence only), so the packages of 11:30 / 11:59 were rebuilt with it. Checks: the PC's 750 attract frames identical, no DIFFERENCE line; the emulator's demo block for block as before (265 of 2,259); packages `2026-11-20_1210_rebase_test4` and the instrumented one of the same tree |
| 2026-11-23 | 9da84f9b | ports 236-263 (banjo-nx's render-thread rounds and command stage, sasrt-nx's rounds 2-4 and handoff base_1122, rr6-nx's dynamic resolution switches, the Cave ports' GPU boost fix) in five steps (`--range f70d05a4..b73aea69`, then `..7754b5a6`, `..5b67c75b`, `..818f3b09` and `..9da84f9b`; the dry run's conflicts in 236-258 were knock-on ones: each step merged cleanly), every other file merged cleanly; ports 252 and 254 change only Katamari's tomls and tools; port 259 (the smaller game files in the SDK: the same rr6-nx / culdcept-nx files this tree had taken on 2026-11-21) with 10 conflicts, files both sides had added: the base's `opus_decoder.*` and `host_path_index.cpp`, SA2's `sfd_hevc.*` (the base's plus the pack device's readers), the base's `host_path_entry.*`, `host_path_device.cpp` and `src/filesystem/CMakeLists.txt` with SA2's NAME.adxo and pack device sources, the base's nxvideo library (`RENUT_NXVIDEO`, in place of SA2's `SA2_NXVIDEO` copy of the same object) |

Ports 236-263 and SA2 (2026-11-23):

- **Keys:** about 50 new or changed defaults; all but one are render-thread and deko3d speed items the base reports
  as pixel-identical (port 251's command stage, 4-core mode only; port 260's 13 `sasrt_rt*` items; port 256's
  compile threads on core 2; port 250's copy-run barriers and port 263's `sasrt_gpu_seed_cpu_direct` back off). Port
  255 turned docked dynamic resolution on (972p-1080p); 1.0.6 renders a fixed 1080p docked, so
  `app/sa2_switch_defaults.toml` sets `banjo_renut_dynamic_resolution_docked = false` (1088110f; the player can turn
  it on in sa2.toml).
- **The checks (2026-11-23):** the PC: 1.0.6's 750 attract frames all identical (`out/lockstep/sync263_attract1`), no
  DIFFERENCE or error line, 1.10 million stack forwards: 0 differences. A verify run (4 minutes,
  `out/pc_runs/sync263_verify`): 0 differences in every check (1.20 billion flush-mode checks, 121 million folded
  loads, 89.6 million r1 checks, 7.4 million stack forwards, 49.1 million guarded calls of 269 machine copies and 208
  bulk copies, 10 "differed once and not when repeated"). The emulator (`out\emu_runs\emu_sync263_attract`, the
  Switch build of 1088110f's tree): Ryujinx ran at about half the usual speed (two other ports compiling: the
  recording took 8 minutes, 4.5 before), so the two-player demo began 2,028 frames later and only 1,472 of its
  2,259 frames fell in the window; those blocks are 1.0.6's as in every run before (3, 22, 3, 26, 28, 4, 38, then 11
  of the cut block). Run again with a 6,400-frame window (`out\emu_tomls\ult_attract_wide.toml`,
  `emu_sync263_attract_wide`, build 0c71fcbc; the demo again about 1,965 frames late): the whole demo, 261 of 2,259
  frames identical, block for block as in every run before (the last block varies between runs: 45-49). Packaged:
  `out\packages\2026-11-23_sync263` (NRO, ELF, notes).

Ports 223-234 and SA2 (2026-11-20):

- **The code generation:** port 224's `r1_preserve.py` and `lr_preserve.py` had put banjo-nx master's tools folder
  first on `sys.path`, so on this PC their proofs ran with banjo-nx's older `leaf_dead_stores.py` (SA2's generations
  of 2026-11-19/20 too); now this tree's. `reg_liveness.py` reads `app/config/scratch_vectors_read_on_entry.txt` (SA2
  has none: as before); `indirect_calls.py` keys a named function's sites by its own address (SA2's config names no
  function: its 243-site list keeps its keys). So the code is generated again.
- **Keys and switches:** port 231's `BANJO_GENERATED_NO_TRAPPING_MATH` form (off in SA2: nothing changes); port 230
  writes rex_profile.log on the Switch only with logging or the sampler on (SA2's release settings have neither, nor
  have the emulator's attract tomls, whose runs are measured by their frame hashes; `ult_log`, `ult_profile` and the
  console's `sa2_ab.toml` turn logging on, so their profiles stay); port 226's `banjo_hot_bulk_lines` (off); port 227's game-menu hook (SA2 sets
  none); ports 228, 229, 232, 234 change deko3d's shader handling (GLSL unpacked when needed, a draw's pipelines ahead of
  the background list, the cache tool's `--round-guess`, the compile share count).
- **The checks (2026-11-20):** the code generated again from scratch: all 726 files byte-identical to those of ports
  191-222 (the r1 / lr proofs with this tree's helpers prove what banjo-nx's older one did; `indirect_calls.py` found
  all 244 listed sites). The PC: 1.0.6's 750 attract frames all identical (`out/lockstep/sync234_attract1`), no
  DIFFERENCE or error line, 1.09 million stack forwards compared: 0 differences. A verify run (4 minutes: the title,
  the intro and the attract demos, every self-check on every use; `out/pc_runs/sync234_verify`): 0 differences in
  every check (1.18 billion flush-mode checks, 138 million folded loads, 134 million r1 checks, 7.5 million stack
  forwards, 90.5 million guarded calls of 272 machine copies with 7 "differed once and not when repeated"). The
  emulator (11:26, `out\emu_runs\emu_sync234_attract`, the Switch build of 817afc76): 1.0.6's two-player demo 263 of
  2,259 frames identical, block for block as before both syncs (260) and after the first (289): only the first
  block, which varies between runs, and the last differ. Packaged: `out\packages\2026-11-20_1130_rebase_test3`.

Ports 191-222 and SA2 (2026-11-20):

- **The code generation:** ports 199 (stack forwarding ends at every branch and call) and 200 (`inline_memcalls.py`)
  change two post-codegen passes, so the tree is generated again in full. Port 191's `pointer_fixpoint.py` fix is in a
  porting audit tool, not the build.
- **Keys that reach SA2:** port 202 turns `sasrt_renut_depth_restore` off (no "EDRAM depth restore" line in any of
  SA2's 110 logs); port 211's `banjo_renut_callbacks_at_waits` (on) runs Renut's GPU callbacks at game threads' waits
  too; port 217's `xam_input_clear_disconnected` (on, as the console); port 220 signs in a pad that connects after the
  game opened its sign-in screen as a guest (SA2 imports `XamShowSigninUI`; no log shows it opened: a console check in
  the two-player menus); port 203's shared save descriptors (`filesystem_shared_handles`, on) on the Switch; port 195's
  message boxes at twice the size; port 206's shortcuts on release.
- **A port-213 fix:** its new device field (the device's own vertex declaration, which SetFVF rewrites in place) came
  with Katamari's offset 0x2F98 in `draw.h`; its static_assert stopped the build: SA2's 0x3118 (4ea0837b).
- **The code generated again** (the generator's record moved aside, `app/renut_config.toml` touched): 24 of 726 files
  changed. Port 199: 46 stack forwards in 35 functions before, 43 in 26 after (the forwards past a call gone, a few new
  after a branch). Port 200: nothing in SA2. Port 191's `tools/built_ptr_scan.py`: 0 candidates in both modules.
- **The PC checks (2026-11-20):** 1.0.6's 750 attract frames all identical (`out/lockstep/sync_attract1`, game frames
  5800-9400 hash-only, aligned by content), no DIFFERENCE or error line, 1.09 million stack forwards compared with the
  real load: 0 differences. A verify run (5 minutes, every self-check on every use): 0 differences in every check (1.16
  billion flush-mode checks, 125 million folded loads, 109 million r1 checks, 7.5 million stack forwards, 61.8 million
  guarded calls of the machine copies with 5 "differed once and not when repeated", as every run before the sync: 4-10).
  Its scripted presses were missed while the save loaded (the guarded run is slow; known), so it showed the title and
  the intro; a plain run on the same route played City Escape (`out/pc_runs/sync_route`, all ten presses taken).
- **The Switch shader package** made again with the merged translator (ports 194, 207, 209): all four files
  byte-identical to the package before.
- **The emulator (Ryujinx, 10:35):** the Switch build of ports 191-222 (`app\out\switch`, 4ea0837b) through the
  attract sequence (`out\emu_runs\emu_sync222_attract`): 1.0.6's two-player demo 289 of 2,259 frames identical against
  the pre-sync build's 260 (`emu_ult_head1_attract`), the same pattern block by block (the first 400 frames vary
  between runs), the late blocks as high: no drift. sasrt's Ryujinx ran beside it from 10:40 (frame content is
  compared, not timing).
