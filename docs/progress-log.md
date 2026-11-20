# Progress log: sa2-nx on the ultimate base

Dated entries, newest last. The plan is docs/ultimate-rebase-plan.md; the findings are in docs/sa2-ultimate-notes.md
and docs/sa2-near-exact.md.

## 2026-11-19

- Read the base's docs (README, building, changelog, off-by-default, the porting prompt, katamari-notes, xdk-notes,
  codegen-crash-fixes, the optimising guides, sasrt-batch-cc2) and sa2-nx's (sa2-notes, building, renut-renderer,
  cpu-rounds, renut-cpu-work-for-banjo).
- The file comparison and the inventory: docs/ultimate-rebase-plan.md (a86a7c54).
- The worktree `D:\sonic adventure 2\sa2-nx-ult`, branch `ultimate` from main (baf523b). The base's tree taken as its
  tree object (e2099f33): its blobs byte for byte; the worktree reads files as stored (`core.autocrlf false` in its
  own worktree config), since the base stores most text with CRLF and has no .gitattributes.
- Katamari's layer out (36bba673). `assets\game_root`: a junction to the main checkout's.
- The base's code generator built from this tree (`out\host`, `sdk\out\win-amd64\rexglue.exe`; `out\host_build.bat`,
  not committed). The base's `tools\build_host.bat` names banjo-nx's host folder, so it was not used.
- The base's tree has no FFmpeg configure-made files (FFmpeg's .gitignore hides them): taken from the base's working
  tree (cb3a6f56). The base's session took the same nine as port 190 (07647474), byte for byte.
- SA2's code generation (both modules, 31,285 + 160 functions, as 1.0.6), the base's post-codegen on the main module
  (about 50 minutes beside another session's build), the module steps on the DLL (tools/post_codegen_modules.py).
- Renut at SA2's Direct3D: the layout from 1.0.6 by name (tools/sa2_renut_layout.py), SA2's hook blocks, the Swap's
  pacing and gamma, no ResetAllState; SA2's game layer and app wiring; the settings check (tools/sa2_effective_keys.py:
  451 keys paired, 7 differ, none that draws or plays differently).
- The plan re-sorted into the three lists (the user's word through the base's session: a full rebase).
- SA2's lockstep recorder and the frame comparison tools.
- The PC build boots to the title with the Battle DLC. Its first lockstep run against 1.0.6's PC build: every frame
  different. Two causes, both found: the runs' labels drift apart at loading screens (pairing by content now:
  `sa2_compare_frames.py --align`, hash-only recordings), and the gamma ramp's rounding (one level at 9 of 256 levels;
  `sa2_renut_gamma_entries`, sa2-ultimate-notes.md). With both: 400 of 400 and 750 of 750 of 1.0.6's frames identical
  (title, loading, 2P demo).
- `tools/run_pc.ps1`: paths with spaces are quoted (Windows PowerShell's Start-Process split them).
- Verify mode with gameplay (six runs on copies of the save): City Escape, the Dark story's opening cutscene and Iron
  Gate, the Chao World lobby; 0 differences in every self-check (sa2-ultimate-notes.md, "Gameplay on the PC").
- The base's Katamari hand natives (`banjo_hot_guest.cpp`, `katamari_hand_*` keys) are compiled in but reach nothing:
  SA2's hot lists are empty, so no hook names their addresses. The start-up line still prints their keys ON; they go
  in the hot-function step.
- D: was down to 1.7 GB (other sessions' builds): the lockstep frames (.rgba) LZX-compressed in place (1.6-2.1 : 1),
  nothing deleted; 2.7 GB free after.
- 1.0.6's asset pack grafted (`pack_device`, `pack_storage_switch`, the `asset_pack` keys, `tools/build_asset_pack.py`):
  1.0.6's installer puts only `game_root.nxpack` + `.nxidx` on the card, so the rebased NRO needs it to find the game.
  PC check: the build mounted 1.0.6's own pack (the emulator's `sa2` folder, read-only: 3,709 files, 2.4 GB, in 7 ms)
  and drew 750 of 750 of 1.0.6's frames identically.
- The 2 MB code blocks' off switch reads `sa2.toml` (the base read `katamari.toml`), as 1.0.6.
- `katamari_dlc_unlock_packs = true` in SA2's settings: the base's DLC installer otherwise hides Beautiful Katamari's
  nine unlock packs by name (none of them SA2's); nothing hidden, as 1.0.6.
- The Switch build's inputs: uam built from this tree into `out\deko3d_tools_ult` (the base's PC uam is from
  2026-11-07, before its uam ports 56, 162 and 163; the base's `tools\deko3d_build_tools.sh` would write into the
  base through this worktree's `out\deko3d_tools` link, so it was not run) and given to the configure as
  `BANJO_DEKO3D_UAM` (`out\switch_build.bat`, not committed); `tools\switch_shaders.ps1`: SA2's 103 shaders into
  `app\out\renut_switch` (the GLSL package, 210 KB, and the Switch shader cache).
- The Switch configure stopped at the stage DLL's target: the generator's `dll_targets_cmake.inja` made it depend on
  `renut_codegen`, which the cross build does not have. Guarded as the main template guards its own (the template, and
  the generated file by hand so the tree need not be generated again now; the next PC build generates it again, since
  the generator changes).
- The first Switch build: `app\out\switch\renut.nro`, 55.7 MB (the everyday form: the GLSL package built in, no LTO).
  A trap on the way: `tools\build_switch.bat` expands `CMAKE_EXTRA` unquoted, so a path with a space came through as
  "D:/sonic" (a stray log file of the first PC boot happened to exist there, so the configure's check passed and the
  build tried to run it as uam). `out\switch_build.bat` quotes it; the stray file is in `out\to_delete_stray_20261119`.
- Censuses on the PC (sa2-near-exact.md): CG-13 on three routes, the near-exact counters, the fp_env_files check; the
  three keys from SA2's logs and the stereo check (sa2-ultimate-notes.md).
- `tools\sa2_emu_ab.ps1`: the emulator A/B of the rebased NRO and 1.0.6's in the same SD folder, each with its own files.
- `tools\deko3d_variant_cache.py --uam`.
- The emulator against 1.0.6 (sa2-ultimate-notes.md, "The picture against 1.0.6 in the emulator"): the title
  identical; the two-player demo drifted (player 2's run). Six Switch builds told the forms apart: the fused
  multiply-add (`SASRT_VMX_FUSED_MADD`) makes it drift, CG-13 and the estimates do not. The user: the offending one off
  in SA2's build, on in the base: `SASRT_VMX_FUSED_MADD` OFF in `app/CMakeLists.txt`. A smaller fixed difference
  (grind sparks, an edge of a board) remains with every form off; its cause is open.
- The excess files of the day (18.6 GB: five variant Switch builds, the recordings' frames, run copies of the save)
  went to `out\to_delete_20261119`, which the user deleted.

## 2026-11-20

- The hooks compiled into SA2's build, checked one by one: every N&B / Katamari address hook is behind
  `KATAMARI_BANJO_ADDRESS_HOOKS` (undefined, so off) except reNut's `single_tile` hook of `sub_821F99C0` (a
  pass-through with its key off, but on an SA2 function, which it kept from being copied or called directly): now
  behind the same switch (`renut_engine/hooks.cpp`).
- The code generator only orders after `rexglue.exe` (an order-only input of the codegen step): a new generator
  does not re-run the code generation; touching an input of `codegen.d` (`app/renut_config.toml`) does.
- The R1_ASSUME census: the code generated again with `BLUR_NX_R1_ASSUME=1` (and the new `dll_targets` template), the PC
  build, then a verify run.
- SA2's hot lists for the base's tool (in `out\hot_prep` until the next generation): 1.0.6's `localized.txt` (87) and
  `localized_bulk.txt` (290 after the 12 that were on both), and `hand.txt` with 1.0.6's four hand-written natives the
  base carries under Banjo's addresses (memcpy, memset, the two shader-constant writers; their stack writes and
  offsets checked against SA2's originals).
- `tools\sa2_package_switch.ps1` (the console package in `out\packages`) and `dist\switch\sa2-nx\sa2_ab_keys.toml`.
- R1_ASSUME's census: +3,427 functions assumed, 0 differences in 1.69 billion checked calls (sa2-near-exact.md); off.
- An emulator profile of the attract sequence (Ryujinx starts an NRO without argv, so the SDK's rex logs, the CPU
  sampler's profile among them, are in `sdmc:/switch/` itself: `tools\sa2_emu_ab.ps1` collects them).
- The machine copies: 89 + 358 bulk, verified on the PC (sa2-ultimate-notes.md, "The machine copies"); five of 1.0.6's
  listed functions are hooked here and came out.
- setjmp / longjmp against the lists: no copy on the path (sa2-near-exact.md).
- `-fipa-pta` on for SA2's LTO builds, as 1.0.6 (the base has it off without a recorded reason); the inliner limits
  are already sa2-nx's (64, 12).
- `tools\sa2_stale_frame_reads.py`: 189 candidate loads (149 functions) for the read of a frame word never written;
  static, so candidates (the first one's use is on a path its earlier check rules out).
- The Switch build with the copies in place: in the emulator block for block as without them. The console package
  `rebase_test1` and the instrumented `pgo_generate` package (it boots in the emulator and writes its stream).
- The installer's needs for SA2 (`out\handoffs\installer-needs.md`): the asset pack and the update zip.
- The elision census: 1.0.6's tint is back with `BANJO_GEN_NV_SAVES_ELIDE` on this base; `SA2_DIAG_NV_ELIDE_FILES`
  bisected the 129 files (file 116), then its 74 functions: SA2's DXT1 decoder `sub_82833568`, whose real
  `std r31,-208(r1)` (read back by `lfd`) `tools\frame_traffic.py` took for a save. Fixed (5b78e9b1); with fresh
  code (frame_traffic has no restore: the generator's record moved aside) all three elisions together give 750 of 750
  of 1.0.6's frames. 1.0.6's "uninitialised stack word" was the tool. The fresh generation was also the determinism
  check: 677 of 726 files identical to the previous run, the 49 others the fixed marks.
- `sasrt_gpu_skip_inert_draws = false` in SA2's settings (0 in every gameplay window of the PC's and emulator's logs).
- `tools/frame_traffic.py` back to the base's LF line endings (2d9d4ad7): the worktree had checked out 436 of the base's
  LF files with CRLF before `core.autocrlf false`; the fix's commit had stored that one file whole with CRLF. The 436
  were put back as stored (their content was the same). The fix as a patch for the base:
  `out\handoffs\frame-traffic-data-store.patch` (applies to the base's committed file).
- The base had moved on to port 222 (2f590c31) during the night: ports 191-222 taken with `tools/ult_port_sync.py`
  (02450663; docs/ultimate-sync.md): 73 files merged cleanly, 4 new, one conflict (`app/CMakeLists.txt`), the hand port
  of `d3d/hooks/shader.cpp`. The tested state before it is the branch `ultimate-at-73b7b731`. Port 213's new device
  field (the device's own vertex declaration) had Katamari's offset 0x2F98 in `draw.h`; its static_assert in
  `draw.cpp` stopped the build: SA2's 0x3118 (draw.cpp's `kDeviceDeclarationOffset`, 1.0.6's layout).
- Stack forwarding ruled out for the emulator's remaining demo difference (sa2-ultimate-notes.md): the PC checks every
  forward against the real load (0 differences in every log), the Switch runs the same forwards.
- The code generated again after the sync (24 of 726 files changed: port 199's stack forwards, 46 -> 43) and the PC
  build: 1.0.6's 750 attract frames identical; a verify run with 0 differences in every self-check; City Escape played
  on the plain route; port 191's built-address scan 0 in both modules; the Switch shader package byte-identical
  (docs/ultimate-sync.md). Its Switch build in Ryujinx: the two-player demo as before the sync (289 of 2,259 frames
  identical to 1.0.6 against 260, the same pattern, no drift).
- The base had moved on again (ports 223-234, banjo-nx's, by 09:24): taken (d4ab878c; two conflicts by hand). Port
  224 fixed the r1 / lr proofs' imports (they took banjo-nx master's older helper on this PC); the code generated
  again from scratch is byte-identical to the previous generation. The PC: 1.0.6's 750 attract frames identical; a
  verify run with 0 differences in every self-check. The Switch build in Ryujinx: the two-player demo block for block
  as before the syncs. The console package `rebase_test3` (`out\packages\2026-11-20_1130_rebase_test3`), then the
  instrumented PGO build of the same tree: it boots in Ryujinx and writes its stream; packaged as
  `out\packages\2026-11-20_1159_pgo_generate2` (the 05:34 one was built before the syncs).
- `tools\sa2_emu_ab.ps1` keeps the SD's own toml on disk during a run (`sa2.toml.before_emu_ab`) and the next run puts
  it back if a run never finished: the PGO boot check's output was piped into `Select-Object -First 3`, which killed
  the script after its third line (the GLSL package line came first this time), before it collected the results and
  restored the SD's toml. Both done by hand (the SD's resting toml is 1.0.6's player toml, `out\emu_tomls\player_106.toml`).
- Port 234's follow-up (a draw's shader compile no longer waits up to 60 s behind the compiler's rest) and port 235
  (tools), 4e0bde24: the PC's 750 attract frames identical, the emulator's demo block for block as before (265; the
  script's toml handling worked: the SD's toml back, no copy left). The packages made again with it:
  `2026-11-20_1210_rebase_test4` and the instrumented one; `_1130_rebase_test3` and `_1159_pgo_generate2` (without
  the follow-up), `_0453_rebase_test1` and `_0534_pgo_generate` (before the syncs) moved to `out\to_delete_20261120`
  with today's other superseded files (4.9 GB: the diagnostic Switch build, the elision PC build, the run copies of
  the save, the generation diffs).
