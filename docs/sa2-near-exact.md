# Sonic Adventure 2 on the ultimate base: the not-exact forms and their censuses

The base's switches that change results in rare cases (off-by-default.md section 2), each with SA2's census, its sites,
and a verdict. A form is on only when its census is done and written here; one that counts stays off until the user
decides. The PC build counts what each form would change while it runs the exact forms (`banjo_diag_runtime_census`,
the "[census]" lines every 10 s).

## The calling convention (before the machine copies' liveness is trusted)

Port 121's register liveness relies on the 360 convention. Run on SA2's generated main module on 2026-11-19:

- `tools/cc_entry_census.py`: 31,003 functions; 13,393 reached through pointers; **no pointer-reached function reads v0
  or v32-v63 on entry** (the line that must say none). The pointer-reached functions read v1 (5), v96 (3), v12 and v11
  (1 each) on entry: v1 is the first vector argument, v96 a non-volatile VMX128 register, v11 / v12 arguments. 115
  functions read a vector register on entry at all, mostly v64-v127 (the save helpers storing the non-volatiles).
- `tools/cc_exit_census.py`: 21,365 indirect call sites. The first run listed 54 with a register of the exit bound
  (r0, r5-r12, f0, f5-f13, v0, v5-v13, v32-v63) read after the call. 52 of them are MSVC's `subfe rD,rX,rX` (the
  carry as 0 / -1: ~rX + rX + CA = CA - 1, whatever rX holds), a value-independent form the tool did not know; with it
  taught (`subfe` with both operands the same register reads no value, sa2-nx-ult's change to the tool) **2 sites are
  left, both `twi 31,r0,22`**, the debug trap: TO = 31 traps whatever r0 holds, and `ppc_trap` only logs (Katamari's
  census had the same kind). So the exit bound holds for SA2: `SASRT_REG_LIVENESS_EXIT_ABI` stays on.
  `tools/cc_exit_census.py --all` (new) lists every site with its instructions.
- The stage DLL: its 160 functions get no machine copies and no whole-program liveness (tools/post_codegen_modules.py),
  so the convention is not relied on there.

The setjmp / longjmp pair (libjpeg's error path: setjmp 0x82908F10, longjmp 0x82908AE0): to check with the machine
copies (the hot-function step).

## CG-13 (`BLUR_FPCR_ONE_MODE`): SA2's census (2026-11-19)

`tools/cg13_census.ps1` on the PC build (the mode switches there; the census counts each stretch of game code between
a switch off and the next switch on that met a denormal operand or gave an underflow: the scalar results the Switch
build, one flush mode for everything, would change). Three routes of 240 s each, on copies of the save:

| Run | Route | Windows of 10 s | With a denormal |
|---|---|---|---|
| cg13_city | the stage map, City Escape played (sticks) | 23 | 1: the first (start-up) |
| cg13_story | the Dark story: its opening cutscene, then Iron Gate | 23 | 1: the first (start-up) |
| cg13_chao | the stage map, the Chao World lobby (sticks) | 23 | 1: the first (start-up) |

About 17,000-18,000 flush-mode switches a second in SA2's game code (each an `msr fpcr` on the console that the
Switch build leaves out). Gameplay, the cutscene, the menus and the lobby: 0 in every window.

The start-up window, the same in all three runs: the base's own deliberate self-check (`banjo::FormsSelfCheck`) and
one game site, 506 stretches in `sub_82832B78` (the switch back on at its `vsubfp128`, renut_recomp.71.cpp). That
function takes 16 entries of four floats 16 bytes apart, compares each one's fourth (alpha) with a constant and marks
the block when all fall below it, then fits colours with `fmuls` / `fmadds` / `fcfid`: a 4x4 block compressor
(DXT/BCn) in the library code at 0x828..., reached through a pointer (`sub_82816B40` has no direct caller). It runs
in the first seconds only. With CG-13 the Switch build gives zero for those denormal operands inside its colour fit;
what that changes in its output (at most an endpoint of a block, if anything) and where the texture is shown is not
known yet. The emulator comparison against 1.0.6 (which had no CG-13) is the check; until it is done, CG-13 stays as
the base has it (on in the Switch build).

## The forms

Filled in by step 6 of the plan (the CG-13 census, BLUR_NEAR_EXACT's switches, BLUR_NX_R1_ASSUME, BLUR_LOCALIZE_ABI_TRUST,
the stack-frame elisions and their uninitialised stack word, BANJO_GENERATED_NO_TRAPPING_MATH).

| Form | Census | Sites | Verdict | In the build |
|---|---|---|---|---|
| CG-13 (`BLUR_FPCR_ONE_MODE`) | | | | ON on the Switch (the base's default) until its census says otherwise |
| `BLUR_NEAR_EXACT` #1, #2, #5, #17, #19, #41, #42 | the PC census of the three CG-13 runs (city, story, chao; `[census] near-exact (cumulative)`, the counters compiled into the main module, not the stage DLL) | #1 signalling NaNs stored 0, #2 NaNs converted to integers 0, #17 divisions by zero or INT_MIN / -1 0, #41 compares with a NaN 0, #5 XER[SO] copies 0, #3 / #4 FPSCR writes and reads 0. #42: SA2 has 24 `vmaxfp128` / `vminfp128` sites; none ran on these routes (0 of 0). #19: the only traps are the 2 `twi 31,r0,22` debug traps of the exit census, which already only log | nothing on these routes would change; #42 not covered | off (the base's default); a decision for the user, with a console A/B for the gain |
| `BLUR_NX_R1_ASSUME` | | | | off |
| `BLUR_LOCALIZE_ABI_TRUST` | | | | off |
| `BANJO_GEN_NV_SAVES_ELIDE` | | | | off (the tinted characters of 2026-10-12) |
| `BANJO_GEN_LR_MOVES_ELIDE`, `BANJO_GEN_BACK_CHAIN_ELIDE`, `SASRT_HOT_LR_ELIDE` | | | | off |
| `BANJO_GENERATED_NO_TRAPPING_MATH` | the fp_env_files check (2026-11-19): `tools/fp_env_files.py` lists the generated files that write the guest's FPSCR (`fpscr.storeFromGuest`, the rounding mode: with CG-13 the only `msr fpcr` left in game code), which must keep `-ftrapping-math` (a float operation moved across the write would round in the other mode) | SA2: 15 files, one write each; on the three census routes 0 FPSCR writes ran (#3). **But the base's CMake does not read `fp_env_files.txt`:** the option puts `-fno-trapping-math` on every file of `renut_recomp` and the DLL, those 15 included (blur-nx's own `BLUR_NO_TRAPPING_MATH` leaves them out) | not safe as the base wires it | off; to turn on, the 15 files must keep `-ftrapping-math` first (a handoff), and a console A/B must show a gain (sa2-nx measured at most 0.5 % of one file) |
