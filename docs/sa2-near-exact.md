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

## The forms

Filled in by step 6 of the plan (the CG-13 census, BLUR_NEAR_EXACT's switches, BLUR_NX_R1_ASSUME, BLUR_LOCALIZE_ABI_TRUST,
the stack-frame elisions and their uninitialised stack word, BANJO_GENERATED_NO_TRAPPING_MATH).

| Form | Census | Sites | Verdict | In the build |
|---|---|---|---|---|
| CG-13 (`BLUR_FPCR_ONE_MODE`) | | | | ON on the Switch (the base's default) until its census says otherwise |
| `BLUR_NEAR_EXACT` #1, #2, #5, #17, #19, #41, #42 | | | | off |
| `BLUR_NX_R1_ASSUME` | | | | off |
| `BLUR_LOCALIZE_ABI_TRUST` | | | | off |
| `BANJO_GEN_NV_SAVES_ELIDE` | | | | off (the tinted characters of 2026-10-12) |
| `BANJO_GEN_LR_MOVES_ELIDE`, `BANJO_GEN_BACK_CHAIN_ELIDE`, `SASRT_HOT_LR_ELIDE` | | | | off |
| `BANJO_GENERATED_NO_TRAPPING_MATH` | | | | off |
