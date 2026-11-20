# Sonic Adventure 2 rebased on the ultimate base: the report

sa2-nx 1.0.6 (main, baf523b8, untouched) rebased on renut_nx_ultimate_base 73b7b731 (ports 1-189) on 2026-11-19, then
brought up to the base's 764410eb (ports 1-234) on 2026-11-20 with the base's sync tool. Branch `ultimate` (worktree
`D:\sonic adventure 2\sa2-nx-ult`, local only, not pushed); the state tested before the two syncs is the branch
`ultimate-at-73b7b731`. The details and the evidence are in [sa2-ultimate-notes.md](sa2-ultimate-notes.md), the
not-exact forms in [sa2-near-exact.md](sa2-near-exact.md), the inventory in
[ultimate-rebase-plan.md](ultimate-rebase-plan.md), the syncs in [ultimate-sync.md](ultimate-sync.md), the dated steps
in [progress-log.md](progress-log.md).

## What works, against 1.0.6

| Where | What was checked | Result |
|---|---|---|
| PC (Renut on Vulkan) | lockstep frames against 1.0.6's PC build, paired by content: the title, its fade, loading, the two-player attract demo | **identical**: 400 of 400 consecutive frames, 750 of 750 sampled over game frames 6000-9000 (after the gamma-rounding fix); 750 of 750 again after each sync |
| PC | the game read from 1.0.6's own `game_root.nxpack` | 750 of 750 identical |
| PC, verify mode | City Escape played, the Dark story's cutscene and Iron Gate, the Chao World lobby, the attract demos; with and without the machine copies; after the first sync the title, menus, save load and intro | 0 differences in every self-check; the copies' guard: 0 differences in about 480 million compared calls |
| PC, plain run after the syncs | the save, the stage map, City Escape | played (all scripted presses taken) |
| Emulator (Ryujinx, deko3d) | the attract sequence against 1.0.6's released NRO | the title identical; the two-player demo plays the same moments, player 2's run no longer drifts since the fused multiply-add is off; a small fixed difference remains (below); after the first sync: as before (289 of 2,259 demo frames identical against 260, the same pattern); after the second: as before (263, the same pattern) |
| Emulator | the Switch build with the 447 copies in place | block for block as without them |
| Emulator | the instrumented PGO build of the current tree | boots, runs the attract sequence, writes its stream (5.1 MB in 7 minutes), no error line |
| Console | the packages in `out\packages`: `2026-11-20_1130_rebase_test3` (the current tree, ports 1-234; the everyday form) and `2026-11-20_1159_pgo_generate2` (the instrumented PGO build of the same tree; it replaces `_0534_pgo_generate`, built before the syncs) | **not run** (your word needed) |

**The remaining difference (open):** with every not-exact float form off, the two-player demo in the emulator matches
1.0.6 in the same stretches every run and differs in the same others: a few grind sparks placed elsewhere, a sliver
at the edge of a board. Ruled out: the shaders (1.0.6's own GLSL package gives the same), the base's renderer and CPU
run-time enhancements, its fences and compare-and-swap barriers, 1.0.6's native copies, the clock, a vblank
interrupt, the zero-initialised locals, the compile definitions, the code generation's float forms, Renut's store
cards, the machine copies, and stack forwarding (the one form the PC and the Switch builds run differently on purpose:
the PC compares every forward with the real load, 0 differences in every run). The PC shows none of it. It breaks
nothing. Both NROs run in Ryujinx as ARM code that its JIT turns into x86, so an ARM floating-point edge case the JIT
does not reproduce exactly could show for one build's code only; the console run tells whether it is there at all.
Otherwise the next step is a comparison of guest memory of the two NROs at a frame that differs.

## The base's later ports (2026-11-20)

The base took 45 more ports during the night. Both batches came in with `tools/ult_port_sync.py`
([ultimate-sync.md](ultimate-sync.md)): ports 191-222 (73 files merged cleanly, one conflict, the hand port of the
shader hooks) and 223-234 (two conflicts). One fix of SA2's own: port 213's new device field had Katamari's offset in
`render/draw.h` (its static_assert stopped the build): SA2's. The code generation changed in 24 files (port 199's
stack forwarding: 46 forwards before, 43 after); port 224's fix of the r1 / link-register proofs' imports changed
nothing in SA2's code. The Switch shader package is byte-identical. What reaches SA2 at run time, for the console
check: Renut's GPU callbacks also run at a game thread's waits (port 211), a pad that connects after the game opened
its sign-in screen is signed in as a guest (port 220; SA2 imports the sign-in screen), shared save descriptors on
Horizon (port 203), the message boxes twice the size (port 195), the shortcuts on release (port 206), and logs and
crash reports start with the build stamp (port 193: "Sonic Adventure 2 switch build <commit>").

## The inventory's outcome

- **(a) SA2's layer, grafted:** the code generation config of both modules; Renut at SA2's Direct3D 2.0.21076
  (layout by name, SA2's hook blocks, the Swap's pacing and gamma, no ResetAllState); the window scissor's shadow;
  the screen width; the stage DLL linked into the NRO; the OpenSA2 controls; SA2's keys and 1.0.6's renamed player
  keys; the tomls; the icon and NRO identity; the lockstep recorder and comparison (now by content); the asset pack.
- **(b) left behind, the base's form kept:** the CPU rounds, the flush-mode flow, the elisions (off), the copies and
  the guard, the console tuning, Renut's render-thread work, double buffering, the colour hand-over, the gamma ramp
  (with SA2's offset and rounding key), the render resolution, the 2 MB blocks, the DLC installer, the language, PGO
  plumbing, the input system.
- **(c) general work the base lacks:** second-module support (patch v2), FFmpeg's files (now base port 190), the
  generator's module-target guard, the asset pack, the gamma entries, the frame_traffic.py fix; handoffs in
  `out\handoffs`.
- **(d) left behind:** sa2-nx's earlier renderers and their tracing, its Windows crash log, `patches/`, the old
  Windows tools, Mesa, 1.0.4's profile, its read-ahead.

## The two modules

`default.xex` into `app/generated` (31,285 functions) and the stage DLL `Data_DLL.xex` into `app/generated/data_dll`
(160), as 1.0.6. The DLL has its own target, gets the module steps of post-codegen
(`tools/post_codegen_modules.py`), no machine copies and no whole-program liveness, and is partially linked into the
NRO on the Switch and registered statically (`rex/system/static_modules.h`).

## The censuses

| Census | Result |
|---|---|
| Calling convention, entry | no pointer-reached function reads v0 or v32-v63 on entry |
| Calling convention, exit | 54 sites, 52 the value-independent `subfe rD,rX,rX`, 2 debug traps: the exit bound holds |
| setjmp / longjmp | no copy on a path between a setjmp caller and a longjmp |
| Built code addresses (port 191's scan) | 0 candidates in both modules: no callback the game builds in code is missing from the function table |
| CG-13 (`BLUR_FPCR_ONE_MODE`) | gameplay, cutscenes, the lobby: 0; the start-up's texture-block compressor and the two-player demo meet denormals, but a build with CG-13 alone plays the demo as the exact build: **on** |
| Port 72's VMX forms | the fused multiply-add makes the demo's replay drift: **off for SA2** (your decision); the estimates do not: **on** (fewer frames identical; your call) |
| `BLUR_NEAR_EXACT` | every counter 0 on three routes; #42's sites never ran: **off** |
| `BLUR_NX_R1_ASSUME` | +3,427 functions assumed; 0 differences in 1.69 billion checked calls: **off** pending a console A/B |
| `BANJO_GENERATED_NO_TRAPPING_MATH` | the base's CMake would put it on the 15 FPSCR-writing files (port 231 now keeps them out): **off** |
| `BLUR_LOCALIZE_ABI_TRUST`, `SASRT_HOT_LR_ELIDE` | off (the base's default); their censuses need a guard build with them on |
| The stack-frame elisions | **1.0.6's tint found and fixed:** not a stale stack word the game reads but a real data store `tools/frame_traffic.py` took for a save (SA2's DXT1 decoder `sub_82833568`, frameless: an integer-to-float conversion through the stack); found by bisecting the generated files, then the functions. With the fixed tool all three elisions together: 750 of 750 of 1.0.6's frames identical. Off pending a console A/B |
| The stereo check | SA2 ignores the speaker setting: the default stays |
| The three keys from the log | `sasrt_gpu_skip_inert_draws`: 0 in every gameplay window of the PC's and the emulator's logs: **false** in SA2's settings; `ultimate_renut_replay_inherit_same` and `banjo_renut_tags_check`: nothing to decide (SA2 runs without the base's tags) |

## What was measured

Frame identity and self-check counts only: every number above is a count, not a timing. No performance was measured
anywhere (the console decides; PC and emulator timings are not evidence).

## Next steps

1. A console run of the newest package (your word): boot, menus, stages, Chao Garden, the two-player modes (connect
   the second controller after the menus too: port 220), saving; send the logs of the `sa2_ab.toml` run (its CPU
   profile feeds the next batch of copies).
2. PGO: the instrumented package of the current tree (`out\packages\2026-11-20_1159_pgo_generate2`); a console
   training session (its notes say what to play),
   `tools\pgo_merge.py <pgo folder> pgo-inplace`, then `tools\build_switch_release.bat`.
3. The installer (`out\handoffs\installer-needs.md`): the asset pack and the update zip, as 1.0.6's installer has.
4. The remaining two-player-demo difference in the emulator (grind sparks, a board's edge), if the console shows it.
5. Later base ports (port 235 was committed at 11:12, after this branch's last sync):
   `tools/ult_port_sync.py --range 764410eb..<base commit>`, then the checks of [ultimate-sync.md](ultimate-sync.md).

## Decisions for you

- The VMX estimates: on (speed) or off (more frames identical to 1.0.6).
- `BLUR_NX_R1_ASSUME`, `BLUR_NEAR_EXACT`, and the three stack-frame elisions (`BANJO_GEN_NV_SAVES_ELIDE`,
  `BANJO_GEN_LR_MOVES_ELIDE`, `BANJO_GEN_BACK_CHAIN_ELIDE`): censuses clean; on only after a console A/B shows a gain.
- `sa2_renut_msaa` (1.0.6's MSAA of single-sample targets): not in the base; port it or leave it out.
- `input_rumble`: off as 1.0.6, or the base's (the game's own vibration).
- The base's SMAA: off as 1.0.6.
- When to run the package on the console, and when to train PGO.
- The handoffs for the base (`out\handoffs\README.md`: `second-module-support-v2.patch` and
  `frame-traffic-data-store.patch` both apply to the base's head at 11:12, e047863f): whether to send them.
