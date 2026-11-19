# Hand- and machine-optimising guest functions: a guide for 360 ports on the base

Written 2026-11-11 by the sasrt-nx (Sonic & All-Stars Racing Transformed) session at the user's request, for the ultimate
base (RR6). It distils what SASRT built and measured on the console: the tools named are sasrt-nx's (most came from
katamari-nx, banjo-nx and sa2-nx and are in the base under the same names). Numbers are SASRT's own unless said.

> **In the base** (added with port 81, 2026-11-11). The guide is sasrt-nx's, kept as it wrote it; where the base's names
> differ:
> - `build_switch.ps1 -Final` is `tools/build_switch.bat` (everyday) and `tools/build_switch_release.bat` (the released
>   NRO): on the Switch both build without guard forms and with the copies in place by default (ports 78 and 81).
>   `-GuardForms` is `tools/build_switch_guard.bat` (app/out/switch-guard: hooked, with the forms) or `GUARD_FORMS=ON`;
>   `-InPlace` is `IN_PLACE=ON` / `OFF` of build_switch.bat.
> - `pgo_cycle.ps1 -InPlace` (out\nx-inplace, pgo-inplace/) is the three steps in [building.md](building.md): an
>   instrumented folder, `python tools/pgo_merge.py <folder with the streams> pgo-inplace`, then
>   build_switch_release.bat (CMake `BANJO_PGO_DIR_IN_PLACE`). `pgo/` holds the hooked form's profile.
> - `tools/profile_breakdown.py` came to the base with port 83; the map is the build folder's own
>   (`app/out/switch/renut.map`, not `out\nx\sasrt.map`). `tools/switch_hot_pcs.py` is in the base. `perfab.py` is
>   sasrt-nx's and is not.
> - `stacks_profile.flag` is the SDK's and works the same way here; the log names are the base game's, not `sasrt_NNN.log`.
> - The base's hand natives' keys are `banjo_hand_*`, not `sasrt_hand_*`. `SASRT_LOCALIZE_LIVE_OUT`,
>   `SASRT_LIVE_OUT_LISTS_NAMED`, `SASRT_HOT_LR_ELIDE` (OFF by default in the base, port 84), `sasrt_test_vmx_fuzz` and
>   the in-run A/B `banjo_renut_ab_target = 70` keep their names (A/B 70 does nothing in an in-place build).
> - Beautiful Katamari's results for in-place copies are in [ultimate-fixes.md](ultimate-fixes.md), row 81.
> - Section 4, step 1's OBJECT libraries are in the base's LTO and PGO since port 89
>   (`BANJO_SWITCH_SDK_OBJECT_OPT`).

There are three tiers, cheapest first:

| Tier | What | Who writes it | Where it lives |
|---|---|---|---|
| Codegen passes | text rewrites over the whole generated tree (flags, byte swaps, locals, constants, float modes, ...) | a pass, once, for every game | `tools/*.py` run by `tools/post_codegen.py` |
| **Machine copies** | a hot function's text re-made with guest registers as C++ locals and its callees inlined | `tools/localize_native.py`, from a list | `app/generated/hot/n_<ADDR>.h`, compiled in `banjo_hot_shard_*.cpp` |
| **Hand natives** | a person (or agent) rewrites a hot function's hot part by hand | by hand, per function | `app/src/cpu/hot/banjo_hand_natives.h` + `hand.txt` |

**Set expectations first.** On SASRT the biggest single CPU win was PGO + LTO (render thread 30.9 -> 24.6 ms, game
thread 29.8 -> 24.6 ms, +9.2 % fps). Hooked machine copies were about neutral; copies *in place* gave -2 % process CPU
and -4 % on the game thread; a good hand native takes 7-26 % off *its own function*, which is rarely measurable on the
whole frame. Most of the A57's time in a 360 game goes to arithmetic the exact 360 semantics require, and no rewrite can
remove that. Do PGO + LTO first, copies in place second, hand natives last and only for the top few functions of the
thread that limits the frame.

---

## 1. Measure before touching anything

1. **Which limit are you at?** In SASRT's log, the `[renut] 10.0 s: ... GPU x ms a frame` lines and the profile's
   per-thread CPU say whether the GPU or a CPU thread holds the frame. With dynamic resolution on, a GPU saving shows up
   as a higher average 3D height, not as fps; a CPU saving on a thread that is not critical shows nowhere.
2. **Stack samples on the console.** Put an empty `stacks_profile.flag` in `/switch/<port>/logs/rex/` (the toml key
   `switch_profile_stacks` does not work; only the flag does). The SDK's sampler then writes `rex_profile_NNN.log`
   (numbered one below the session log, `sasrt_NNN.log`). No screenshots during a measured race: the bot's pixelPeek
   stalls the game ~250 ms.
3. **Break the profile down** with the link map of the same build:
   `python tools/profile_breakdown.py PROFILE --map out\nx\sasrt.map --from-report N --to-report M --top 12` gives, per
   thread, its CPU and the split hand / machine / generated / renderer / runtime, then each thread's hottest functions.
   (With LTO the tool cannot tell renderer from runtime by object file.) If "generated" is near 0 %, every hot function is
   already a copy or a native.
4. **Look inside one function** before deciding anything about it:
   `python tools/switch_hot_pcs.py PROFILE MAP ELF "n_8286C230" 12 8 64` prints the hottest instruction windows with
   disassembly. This is what tells you whether a rewrite can remove the cost (a stack round trip, a byte-swap chain, an
   `msr fpcr`, a byte-at-a-time store loop) or whether it is arithmetic you must keep.
5. **Compare runs properly**: `perfab.py`-style per-thread CPU from the profile (exact `svcGetInfo` ticks), the same
   track, the same clocks (check the profile's `clocks:` line), the same toml. Run-to-run noise on process CPU is about
   +-2 %, on small threads +-5-8 %: one race pair proves only bigger differences. The in-run A/B (`banjo_renut_ab_target =
   70` sends every copy's hook to the original in alternate 4 s halves) removes scene noise for hooked copies.
   A port's own A/B targets take numbers 256-511 (port 188): the base's are 1-255, and it never takes a
   number from 256 up, so a sync cannot meet a port's own.

---

## 2. Machine copies

### 2.1 What the transform does

`tools/localize_native.py` takes a root function and the closure of its callees (up to `closure=` lines, members up to
`member=` lines; hot copies take twice: idea 33) and writes one C++ function in which:
- guest registers are C++ locals (`l_r3`, `l_v12`, ...), not `ctx` fields: the compiler keeps them in machine registers,
  which it cannot do with `ctx` (any guest store may alias it);
- the closure's members are inlined, registers staying local across those calls;
- at a real call (a callee outside the closure) the registers that callee can reach are written to `ctx` before and read
  back after; at the return, the registers a caller can read (LiveOut, idea 37) are written back;
- extras ride along: merged `stvlx/stvrx` pairs and `stvewx` quads (idea 57 / port 74), link-register move elision
  (idea 47), pointer-chase prefetches (52), store-card runs (39), the fast VMX forms (port 72), the lane pass.

Functions it refuses (it says why): atomics (`lwarx/stwcx`), the global lock, `setjmp`, host calls inside.

### 2.2 Choosing what to copy

- `tools/switch_hot_bulk.py PROFILE MAP [PROFILE MAP ...] --write app/src/cpu/hot/localized_bulk.txt --merge` makes
  the bulk list from console profiles: every function still running as generated code with `--min-samples` samples,
  leaving out tiny leaves (`--tiny 12`: the generator inlines them already) and, for the hooked form,
  **`--min-instructions 40 --small-keep-samples 50`** (idea 30): a small function behind a hook is slower than the
  generated one, because its callers can no longer inline it.
- `app/src/cpu/hot/localized.txt` is the hand-picked list (each its own hook in `banjo_hot_guest.cpp`); line options:
  `off`, `key=`, `closure=`, `member=`, `inline=`.
- Better than a profile per scene: the PGO counts you already collect record every function's execution count; a list
  from them covers scenes the stack sampler never saw. Do NOT copy everything: cold code is barely exercised by races or
  the guard, so a bug there reaches players, for no measurable gain (the user's call on SASRT).

### 2.3 Hooked or in place

| | Hooked (`SASRT_HOT_IN_PLACE` OFF) | **In place** (ON) |
|---|---|---|
| the generated function's symbol | a hook: state byte, guard, key, then the copy | the copy itself |
| copy entry | `[[gnu::noinline]]` | inlinable (`BANJO_HOT_NATIVE_SPEC`) |
| the original | kept (the guard's reference, the key's off path) | dropped by LTO when nothing else names it |
| per-copy keys, in-run A/B 70 | yes | no (build switch; `off` in the list keeps the original) |
| guard | yes (with guard forms) | no: guard runs use a hooked build of the same list |
| SASRT race (each with its own PGO + LTO) | baseline | process CPU -2.0 %, game thread -4.1 %, NRO -4.1 MB |

**Ship in place; guard with hooked.** The copies' code is identical in both forms, so a guard run of the hooked build
verifies what the in-place build runs. Each form needs its own PGO profile (`pgo_cycle.ps1 -InPlace` uses
`out\nx-inplace` and `pgo-inplace/`). Hook dispatch itself is cheap (Blur measured the stubs at 0.4 % of CPU): what the
hook costs is the inlining it blocks and the duplicated code.

### 2.4 Build switches and lessons (each was measured or caused a bug)

- **`-ffp-contract=off` and `-fno-strict-aliasing` on every target that compiles copies, and on the LTO link.** GCC's
  default fuses separate guest `fmul` + `fadd` into an FMA; the LTRANS stage fuses too unless the link repeats the flag.
- **Guard forms only in guard/verify builds** (idea 27, `SASRT_HOT_GUARD_FORMS`, OFF with `-Final`): about two thirds of
  the copies' code. Shipping builds call every copy directly.
- **Never compile the shards with `-Os`** (idea 58, reverted): GCC then left `CRRegister::compare`, the flush-mode
  switches and the conversions out of line (24,000 calls from 700 copies), which showed on the game thread. PGO already
  optimises cold blocks for size.
- **A hook whose copy is off is the worst case**: the hook stays, the inlining is gone. Remove the line instead.
- **Any change to the list, the generator or a post-codegen step changes generated files: run a PGO cycle.** A stale
  profile is dropped silently for every changed function (`-Wno-coverage-mismatch`); SASRT once lost 9 % fps that way.

### 2.5 Verifying copies (the guard)

- Turn it on with `banjo_hot_guard_calls` (first N calls of each thread compared) and `banjo_hot_guard_period` (then 1 in
  P); the dense check is 4000 / 64. **Run it on the emulator (Ryujinx) or the PC, not the console**: it runs both forms of
  every checked call and the console crawls (the user's rule). It needs a build with guard forms (non-`-Final`).
- Read the summary line: `N calls compared, M not comparable (stopped in both forms with the same stores before the
  stop)` (a real call ends the comparison; the stores up to it were equal), `K differed once and not when repeated`
  (another thread changed the inputs: harmless), `differences: D`. A `DIFFERENCE` line that repeats 3 passes in a row is
  real, and the copy switches itself off for the rest of the run.
- **The guard is blind to anything both forms share**, so those need their own checks:
  - helpers both forms call: the lane helper (`vec_lane.h`) and the VMX float macros have start-up self-checks and a
    console fuzz (`sasrt_test_vmx_fuzz`);
  - **registers LiveOut calls dead are not compared**: a wrong LiveOut (a caller does read the register) is invisible to
    the guard and shows up later as a game bug. If you cannot validate LiveOut for a game, ship with
    `SASRT_LOCALIZE_LIVE_OUT=0` and `SASRT_LIVE_OUT_LISTS_NAMED=1`;
  - forms built in different translation units: Viva found 8 differences that came from idea 47 (LR elision) eliding the
    save in one form and not the other. Keep a root's two forms and every member they reach in the same TU and under the
    same defines; when in doubt, build with `SASRT_HOT_LR_ELIDE=OFF`.
- A guard difference in a volatile register (r0, r11, r12, ...) after the return is usually a dead register the
  conservative LiveOut compares; confirm by checking that no caller reads it before writing it.
- memcpy-like copies "differ" when another thread writes the source between the two runs: the guard repeats up to 3
  passes and rolls back; a buffer that is rewritten continuously can still show one.

---

## 3. Hand natives

### 3.1 The gate (katamari-nx's)

Write one only when all five hold: (1) a console profile shows the function; (2) it is in the thread that limits the
frame; (3) at least 2 % of that thread; (4) it already runs as a machine copy; (5) **the samples show a cost a rewrite
removes**. Condition 5 decides: read `switch_hot_pcs.py` first.

Costs a rewrite removes (SASRT's natives, each from its samples):
| Function | What the samples showed | What the native does | Result on its thread |
|---|---|---|---|
| water step `8286C230` | ~70 of 207 samples in four misaligned `stvewx` (vperm, stack spill, lane load, swap, store) a step | each group as one 16-byte store | 31.4 % -> 29.1 % of WaterThread |
| water table `82872180` | 22 of 143 samples on two `msr fpcr` and the branch after; the rest stack round trips | no mode switch, no stack round trip | 21.0 % -> 15.5 % |
| audio resampler `82720840` | 20 of 64 samples in the `stvlx/stvrx` byte helpers | one 16-byte store | (similar) |
| bounding sphere fit `8279A888` | 17 of 57 samples on two `msr fpcr` a point | keeps the vector mode; `detail::Sqrts` / `Fadds` exact | |
| stack cookie check `82C92CC8` | (not hot itself) | lets 73 copies avoid a real call at their end | |

Costs a rewrite does NOT remove (SASRT declined these): plain vector multiply/add chains (the copy already keeps the
registers local; the rest is the arithmetic), single-precision multiply-adds whose double rounding is the exact
semantics, functions that are atomics (`lwarx/stwcx`), and a cost right after a recursive call. Record each decline and
its reason in the docs, so nobody repeats it.

### 3.2 Writing one

1. **Start from the machine copy's own statements** (`app/generated/hot/n_<ADDR>.h`, the `f_sub_*` with registers in
   locals) and rewrite only the hot part. SASRT's natives were made by small generator scripts from the copy's text
   (named in each native's comment), so everything outside the change is the copy by construction.
2. **Keep the 360's semantics exactly**: the same rounding (single vs double), the flush-to-zero / non-IEEE mode the
   guest code runs in (`detail::Sqrts`, `detail::Fadds` compute in the right mode, with a cold path for denormal
   operands), big-endian memory, the same stores in the same bytes (store cards included).
3. **Use the same macros as the generated code for VMX floating point** (`REX_VMADDFP`, `REX_VNMSUBFP`,
   `REX_VRSQRTEFP`, `REX_VREFP`, `REX_VLOGEFP`). SASRT's natives predated the fast VMX forms and kept the old
   expressions; the dense guard then found two 1-ulp differences (an audio byte 0xfe vs 0xfd, a water value -1.19e-7 vs
   +1.19e-7). Fix: run `tools/vmx_float_macros.py`'s `rewrite()` over the natives' header whenever the forms change.
4. **Write back every register the original writes back**, and list them in `hand.txt` as `exact=`: the guard then
   compares all of them. `reads=` names what a native in front of a real call reads.
5. Wire it: a line in `app/src/cpu/hot/hand.txt` (`<address> [id=] [exact=...] [reads=...]  # evidence`), the native in
   `banjo_hand_natives.h` (`banjo::hand::n_<id>`), and the function in `localized.txt` so it has its own hook;
   `localize_native.py` puts the native in front of the copy wherever the copy is used. Each native gets a key
   (`sasrt_hand_<name>`) so it can be turned off alone.
6. Verify with the dense guard on the emulator (the native against the original statements), then race on the console
   with stack samples and look at the function's own share before and after.

---

## 4. The order that paid off on SASRT

1. PGO + LTO over the game, the renderer, plume, the SDK runtime and FFmpeg (and, per Blur's finding, the SDK's OBJECT
   libraries too: rexaudio, rexcore, rexfilesystem, rexinput, rexui, with `-fno-lto` on the files that define `--wrap`
   functions).
2. Codegen passes: they apply to every function and every game.
3. Machine copies of the profile's (or the PGO counts') hot set, **in place**, ideas 27 and 30 for any hooked build,
   guard-verified on the emulator.
4. Hand natives for the top few functions of the critical thread, through the gate.
5. After each step: PGO cycle, guard on the emulator, one console race with stack samples, the numbers recorded with the
   noise level stated.

## 5. Checklist for one function

- [ ] Critical thread? Its share of that thread? (`profile_breakdown.py`)
- [ ] Hot instructions read (`switch_hot_pcs.py`): which cost, removable or arithmetic?
- [ ] Already a copy? If not, copy it first (and measure that alone).
- [ ] Native written from the copy's text, 360 semantics kept, the VMX macros used, every written register in `exact=`.
- [ ] Key added; `hand.txt` line with the evidence; decline reasons recorded for the others.
- [ ] Post-codegen run, PGO cycle, guard on the emulator (0 repeating differences), console race with stack samples.
- [ ] Before/after numbers with the noise stated; a handoff to the base if it is universal.
