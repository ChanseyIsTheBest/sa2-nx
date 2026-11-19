# sasrt-nx's batch CC2: the calling-convention stages and stack forwarding (port 121)

renut-ultimate's note (2026-11-12, port 121): sasrt-nx's handoff below, as it sent it. In the base:
`tools/reg_liveness.py` follows the base's `leaf_dead_stores.py` (the named guest functions through
`tools/dead_ctx_defs.py`, the hand natives of `app/src/cpu/hot/hand.txt`, the mid-asm hook blocks with this file's
192-bit masks), and its `POINTER_REASONS` are `data word` and `lis pair` only (the base's parser never reads the copy
lists or `app/indirect_promotions.txt` as naming a function: ports 74 and 108). `tools/stack_forward.py` runs last
before the copies, after `fma_single.py`. `REX_STACK_FWD_VERIFY` is on in the hooked builds (the PC, guard builds),
reported by `app/src/cpu/banjo_codegen_checks.cpp`. `banjo_hot_poison_dead` is in `app/src/cpu/banjo_hot_guest.*`.
Idea 1 (fewer flush-mode switches) is not taken: sasrt-nx found it slower. Its census scripts became
`tools/cc_entry_census.py` and `tools/cc_exit_census.py`: **run both on a game before relying on this** (the
convention is the game's compiler's). Beautiful Katamari: no pointer-reached function reads v0 or v32-v63 on entry;
6 of 12,610 indirect call sites read r0 after the call, all through the debug trap `twi 31,r0,22` (ppc_trap only logs).

# Batch CC2 (sasrt-nx, 2026-11-12): calling-convention stages 1b-1d, the trap fix, stack forwarding (CONFIRMED: -2.5 % CPU)

Items for the ultimate base, all exact (no pixel, result or timing change). Status: built and guarded / measured as
filled in below; until then this is a draft.

**THE TRAP (found 2026-11-12 16:30, before any CC2 build ran): the 360 code passes the first vector argument and
returns vector results in v1, not v2.** Census over SASRT's tree: 173 functions read v1 on entry (v2: 57, v3: 17),
and of the 45,146 pointer-reached functions 100 read v1, 40 v2, 8 v3, none v0 or v32-v63; v1 is also the vector most
read right after a call. The scratch set is v0 + v32-v63 only. A liveness pass that treats v1 as scratch drops v1
hand-overs to pointer calls and v1 write-backs of vector-returning virtual functions, and the guard cannot see it (its
skip set comes from the same LiveOut). Census tools: sasrt-nx scratch vec_abi_census.py, abi_after_call_census.py
(copies in this folder).

## 1. Whole-program register liveness for the machine copies (tools/reg_liveness.py)

New tool, run by tools/post_codegen.py right before `localize_native.py --list --bulk`; writes out/reg_liveness.json
(stamped with the tree's file sizes and times; the copy tool ignores it for another tree).

- UE / LiveOut over the guest asm comments with the vector registers (192-bit masks: r0-31, f0-31, v0-127), reusing
  leaf_dead_stores.py's parser (copied with asserted replacements, so a change there is noticed).
- Stage 1: per direct call site, the registers the callee does not read before writing them and nothing reads after
  the call: localize_native.py leaves their stores out of a real call's hand-over.
- 1b: calls through app / SDK hooks and kernel imports read no scratch vector (v0, v32-v63) on entry.
- 1c: the exit side. A function whose every reference is an image pointer (data word, lis pair; plus the generator's
  promotion list) returns to a caller that follows the convention: v0, v5-v13, v32-v63, r0, r5-r12, f0, f5-f13
  are dead at its exit (r3/r4, f1-f4, v1-v4 kept). Functions named in app sources, the entry point, functions called by
  named (unparsed) functions, and functions with no reference at all keep LiveOut = everything. Constant-target
  bctr / bctrl (lis + addi / ori -> mtctr) count as unknown direct calls of their target.
- Copies reached through image pointers count as known at their direct (hooked) calls: leaf_dead_stores.py treated
  every taken copy as unknown.
- 1d: indirect calls and promoted calls' fallbacks leave out the scratch vectors dead after them.
- localize_native.py: `live_out_keep()` drops a general / float write-back when either LiveOut (leaf_dead_stores.py's
  or reg_liveness's) finds it dead; the guard's skip set uses the same predicate, so the guard stays exact.

**Port trap:** leaf_dead_stores.address_taken scans every app/*.txt and app/src file for function addresses. A stale
copy list (sasrt-nx: app/src/hot/localized.txt, unused since 2026-11-08) or app/indirect_promotions.txt "names" the
hottest copies, which made their calls unknown and their LiveOut everything. reg_liveness treats those two as
pointer-only reasons (POINTER_REASONS); check each port's tree for its own stale lists.

Switches: SASRT_REG_LIVENESS_HOOKS_ABI=0, SASRT_REG_LIVENESS_EXIT_ABI=0 (older behaviour), SASRT_LOCALIZE_CALL_LIVENESS=0.

**Checking LiveOut itself: banjo_hot_poison_dead** (hooked builds, default off; app/src/cpu/banjo_hot_guest.{h,cpp},
BANJO_HOT_POISON / PoisonDead). The guard compares a copy with its original except the registers the skip set says
are dead after the return, and that skip set comes from the same LiveOut, so a wrong LiveOut is invisible to it.
With the key on, each unguarded copy run overwrites its skip-set registers with garbage (0xBAD0... in the general
registers, NaN patterns in the float and vector ones): a wrong "dead" claim shows as a crash, NaNs or a wrong race.

## 2. Idea 2: vector loads of stack slots forwarded from their four stores (tools/stack_forward.py)

The 360 builds vectors from scalars through the stack (stw / stfs x4, then lvx); the A57 cannot forward four narrow
stores into one wide load. The stores stay; each stored value is also kept in a local, and the load becomes
REX_STACK_FWD4: when the address is exactly r1 + A (straight-line block, no r1 write, call, label, jump or foreign
store in between) the vector is built from the four values, else the original load. 510 sites in SASRT.
REX_STACK_FWD_OFF: the original loads. REX_STACK_FWD_VERIFY (on in hooked builds via app/CMakeLists.txt): the real
load, compared with the forwarded words, reported by the generated-code self-checks line; the guard cannot see this
pass (the original statements forward the same way).

## 3. Idea 1: fewer flush-mode switches

**Not in the confirmed set (both options off in SASRT's shipping config; see Results).** Kept as generator options for a
loop-aware retry; do not port as a speed-up.


- Generator option `fp_mode_lookahead_run` (default 2; SASRT 4): runs of up to N scalar single operations between
  vector code take their any-mode forms instead of two FPCR writes (console: a toggling FPCR write 44 ns).
- Generator option `fp_fused_single_any_mode` (default false; SASRT true): fmadds / fmsubs / fnmadds / fnmsubs get
  exact any-mode forms (rex::ppc::FmaddSingleAnyMode etc., diet_ops.h) instead of forcing FPU mode.
  sdk/tests/fp_fused_any_mode_test.cpp (run_sdk_tests.ps1, needs -mfma): 160.6 million comparisons in both modes,
  0 differences, negative control positive.
- Profile header: FPSR read / clear costs (sdk/src/ui/switch_perf.cpp): read 16.6 ns, write of zero 10.8 ns.

## 3b. Traps read only their operands (tools/reg_liveness.py defuse, TRAP_USE)

leaf_dead_stores.py's defuse says a tw / td trap reads every register; ppc_trap (the generated pch) only logs and
continues (service traps 20 / 26 read r3 / r4 for their string). Every function with a divide-by-zero check therefore
had UE = everything. reg_liveness's own defuse now returns the operands + r3 / r4 (SASRT_REG_LIVENESS_TRAP_ALL=1 restores
ALL). In leaf_dead_stores.py itself the same fix only adds 37 dead leaf write-backs of 36,014 in SASRT (the vector side,
which lds does not track, is where it pays), so it was left as is there.

## Where the code is (sasrt-nx, D:\sonic transformed\sasrt-nx)

tools/reg_liveness.py (all of section 1 + 3b), tools/localize_native.py (call_text's per-site / indirect filters,
live_out_keep's AND of both LiveOuts), tools/post_codegen.py (the reg_liveness and stack_forward steps),
tools/stack_forward.py (+ REX_STACK_FWD_VERIFY), app/src/sasrt_codegen_checks.cpp (the self-check reporter, PC and
Switch), app/CMakeLists.txt (REX_STACK_FWD_VERIFY in hooked builds), app/src/cpu/banjo_hot_guest.{h,cpp}
(banjo_hot_poison_dead), the census scripts in this folder and tools/census_1112/.

## 4. Tool fix: tools/ctx_traffic.py

x0 holds ctx only until its first write or call; the first version counted every x0 access. SASRT's ctx share went
from 17.1 % to 13.7 % of guest samples (7.9 % of process CPU).

## Results

**Guards (hooked build of the CC2 tree, before the promoted-target fix):**
- Ryujinx dense guard (4000 / 64, fast-draw verify, 900 s, `race_ryujinx_slower.txt`: A presses until 646 s, because
  this run's start was ~2x slower for every thread, untouched hand-native code included): race reached, 1,156 copies,
  3.3 M calls compared; REX_STACK_FWD_VERIFY 14.2 M forwarded loads checked, 0 differences. Differences: the known v0
  NaN-sign family (82591260, 8276ACB8, 8276AE60, 82B10EE0, 827570B8; CC1 had the same family) and one real one:
- sub_826A1D38: r8 copy 0x400, original 0 (also found by a sparse guard race on the console, 64 / 4096, 1,214 copies).
  Cause: **a promoted indirect call with several targets** (`if (REX_PROMOTED_IS(a)) {...} else if
  (REX_PROMOTED_IS(b)) {...}`): leaf_dead_stores' parser keeps only the first `sub_X(ctx, base)` of the line as the
  node's call, so the second target (823FE090, pointer-reached) never got this caller's after-set, the exit bound made
  its r8 dead, and its member copy dropped the r8 write-back that flows to the root's exit. Fixed: every target of a
  promoted line is a call site (NodeX.extra; UE, LiveOut sites, the unknown-call rule). 31 multi-target sites in SASRT.
- banjo_hot_poison_dead: the first version (a 192-bit loop and a shared counter per copy call) slowed Ryujinx ~4x; it
  now visits set bits only. Its first run (menus only) found no problem.

**Re-guard after the fix** (Ryujinx dense, 900 s, race reached): 1,138 copies, 2.45 M calls compared, differences only
the known v0 NaN-sign family (r8 of sub_826A1D38 gone); 7.2 M stack forwards verified, 0 differences.

**First console A/B (not valid, kept for the record):** the CC2 build that was measured took its PGO profile from a
training race of the tree BEFORE the promoted-target fix; the fix changed LiveOut for 40 targets and, through LiveOut
propagation, 285 write-backs over many copies, which then lost their profile (-Wno-coverage-mismatch) and GCC's
inlining moved (functions with 0 samples in CC1 had 25-75 in CC2 and others the reverse). And the console was ~2.5 %
slower after a reboot (abCC1 re-run abCC1r: 82.11 ms against 80.07 in the afternoon). Same-session numbers: CC1r
82.11 ms / 28.16 fps, CC2 84.09 ms / 27.92 fps. Flush-switch samples 2.18 % -> 1.77 %; ctx loads/stores 13.7 -> 13.8 %
of guest samples. Lesson: never change the tree between the training race and the final build; a fix found after
training means a new training race.

**Confirmed (cycle A, 2026-11-12 23:00): CC2 without idea 1, plus the trap fix** (reg_liveness: a tw / td trap reads
its operands and r3 / r4, not every register - ppc_trap only logs; indirect sites with skippable scratch vectors 12,076
-> 21,404). Own training race (pgotrain_A, LAP 1), final PGO+LTO build, measured right after a CC1 control in the same
session (handheld 1020 / 460.8 / 1600, Ocean View, 1,284 vs 1,275 draws a frame):

| | CC1 control (abCC1r2) | cycle A (abA) | change |
|---|---|---|---|
| process CPU ms a frame | 82.94 | 80.86 | **-2.5 %** |
| Main XThread | 22.71 | 21.96 | -3.3 % |
| SlRenderer | 14.81 | 14.69 | -0.8 % |
| render thread (core 1) | 19.09 | 18.55 | -2.8 % |
| fps (GPU-bound) | 27.98 | 28.01 | flat |

Guard of cycle A: Ryujinx dense, race reached, 1,155 copies, 3.2 M calls compared, only the known v0 NaN-sign family;
13.4 M stack forwards verified, 0 differences.

**Idea 1 is NOT part of the confirmed set** (fp_mode_lookahead_run back to 2, fp_fused_single_any_mode off): it cut
flush-switch samples 2.18 -> 1.77 % but the water step's loop got slower (more any-mode checks and spills, same FPCR
writes). The fused forms stay in the generator (exact, off) for a loop-aware retry.
