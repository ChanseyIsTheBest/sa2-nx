# Optimising game functions

> In the base: written for Beautiful Katamari; the two tracks and their tools apply to any port. Read it with
> [hand-and-machine-optimising-guide.md](hand-and-machine-optimising-guide.md) (sasrt-nx's, the later tools) and
> [sasrt-batch-cc2.md](sasrt-batch-cc2.md) (the copies' register liveness).

How this port makes Beautiful Katamari's own functions faster on the Switch. Written 2026-11-02, on the model of
[bejeweled3-nx's document](https://github.com/ChanseyIsTheBest/bejeweled3-nx/blob/main/docs/optimising-functions.md), with
a different policy (the user's) and this port's tools and numbers.

> [!IMPORTANT]
> **Two tracks, two different standards of evidence.**
>
> 1. **Hand-written functions come only from console profile data.** Nothing is written by hand because it looked hot on
>    the PC or in Ryujinx, or because it looks like it could be faster.
> 2. **Machine optimise as many functions as possible, at every opportunity.** A machine copy is mechanical and checkable,
>    so it needs no such evidence. Every session that shows a function running, on any machine, is a reason to copy it.

Everything below follows from that split. A hand-written function costs review, a place in the source for good, and a
risk a tool does not carry, so it has to earn its place with a measurement from the machine that matters. A machine copy
costs little, so the default is yes.

## Contents

- [How this differs from bejeweled3-nx](#how-this-differs-from-bejeweled3-nx)
- [The rule both tracks keep](#the-rule-both-tracks-keep)
- [The machine track](#the-machine-track)
- [The hand track](#the-hand-track)
- [Where the hand-written functions stand](#where-the-hand-written-functions-stand)
- [Getting a console profile](#getting-a-console-profile)
- [Checking](#checking)
- [Measuring](#measuring)
- [PGO and the order of work](#pgo-and-the-order-of-work)
- [Workflow](#workflow)

## How this differs from bejeweled3-nx

| | bejeweled3-nx | This port |
|---|---|---|
| Machine copies | Every function with at least 3 console samples | Every function any session sampled once, then every other eligible function, in batches (below) |
| Sources for machine copies | Console profiles | Console, Ryujinx and PC samples, and no samples at all once those are used up |
| Hand-written functions | A batch of the hottest, then the ones whose profile shows a removable cost | One at a time, only past the gate below, with the console evidence recorded next to the function |
| After writing one | Judged by the next profile | The same, and a function the next console profile does not support is switched off or removed |

The rest of the method is bejeweled3-nx's and banjo-nx's: localized copies, changed-only write-backs, inlined callees, direct
dispatch and verify mode all come from there.

## The rule both tracks keep

An optimised function does exactly what the recompiled one does: the same guest memory stores, the same registers left
behind, the same calls in the same order, the same float rounding and flush mode. Faster is only worth having when it is the
same game.

| Track | Exact because |
|---|---|
| Machine copy | Made mechanically from the generated function's own statements (`tools/localize_native.py`) |
| Hand-written | Written against the generated code, then compared with it call by call ([Checking](#checking)); it declines (returns false, the copy runs) on any input it does not handle exactly |

## The machine track

### What a copy is

`tools/localize_native.py` turns a generated function into a C++ copy (`app/generated/hot/`, 1,500 headers): registers in
locals rather than the shared context, write-backs of only the registers that changed, small callees inlined, and direct
dispatch from its hook. `tools/post_codegen.py` remakes the listed copies after every code generation, so they stay in step
with the generated code. Each has a key, `banjo_cpu_native_sub_<ADDR>`, for comparing with it off.

Since port 81 (sasrt-nx's machine copies in place, CMake `SASRT_HOT_IN_PLACE`) the Switch's everyday and released NROs have
no hooks: the generated function's symbol is its copy, which the compiler and LTO may inline into its callers. There the
keys do nothing (the log names any that is set against the list): comparing a copy with it off, the guard and
`banjo_hot_count` need a hooked build (`tools/build_switch_guard.bat`, `IN_PLACE=OFF`, or the PC). A copy that has to
stay off in a game is marked `off` in its list.

The lists:

- `app/src/cpu/hot/localized.txt` (112): the first batches, chosen by hand from early profiles;
- `app/src/cpu/hot/localized_bulk.txt` (1,387): made by `tools/switch_hot_bulk.py`;
- `app/src/cpu/hot/hand.txt` (21): functions with a hand-written native in front of their copy.

### Every opportunity

| When | Do this |
|---|---|
| A console log comes back with samples | Remake the bulk list with `--min-samples 1` |
| Any Ryujinx session with sampling on (a new stage, a menu nobody profiled) | The same: add the pair to the command |
| Any PC profile (`tools/pc_hot_functions.py`) | Add what it names to `localized_bulk.txt`. `switch_hot_bulk.py` reads ARM profiles only, so this is by hand |
| Before an instrumented PGO build | A last batch, because a function copied after the profile has no counts ([PGO](#pgo-and-the-order-of-work)) |
| The copy tool refuses something the profile shows | Teach the tool, if the form can be copied exactly |
| No new profile at all | Take the next batch of eligible functions (below) |

The command takes the new profile and the ELF of the build that wrote it:

```bash
python tools/switch_hot_bulk.py PROFILE ELF --min-samples 1 --merge --write app/src/cpu/hot/localized_bulk.txt
```

**Always use `--merge`.** A profile lists only what its build still ran as generated code, so a function that build had already
copied is not in its rows. Without `--merge`, `--write` replaces the list, and remaking it from the profiles on disk would have
dropped 802 of the 1,387 functions.

**Small functions stay out** (port 78, sasrt-nx's idea 30): fewer than 40 guest instructions, unless 50 samples or more
(`--min-instructions`, `--small-keep-samples`). Behind its hook a small copy's callers no longer inline it, and every call
hands the registers over: in sasrt-nx's console races its copies under 40 instructions cost +188 % against the generated
code. `--merge` takes the list's own such lines out as well (their size and samples are on the line), so the next remake of
Katamari's list drops 624 of its 1,387 lines (628 are under 40 instructions, 4 of them with 50 samples or more). A pruned
function comes back when a later profile, of a build that ran it as generated code, has 50 samples of it.
`--min-instructions 0` keeps the old selection.

The list is complete for now: merging every profile on disk (rc1 in Ryujinx; rc3, rc4, rc5 and rc9 on the console) adds 0 functions
(2026-11-02). New functions need a new session.

### How far it has got

Of the 37,763 generated functions, 1,499 are on a list (2026-11-02). In the last sampled console profile (rc9, 63,348
samples), before rc10 added 560 copies:

| Where the samples were | Share |
|---|---|
| Copies and hand-written natives | 62.8 % |
| Host code (renderer, runtime, C library, audio) | 25.5 % |
| Generated code that has a copy since rc10 | 6.1 % |
| Generated code still without one | 5.5 % |

That last 5.5 %, by the rules `switch_hot_bulk.py` applies:

| Share of rc9's samples | Functions | Why it has no copy |
|---|---|---|
| 3.18 % | 81 | Refused: atomics (reservations). The largest is the interlocked reference count `sub_826A13D8` (1.27 %) |
| 1.67 % | 130 | Tiny leaves (12 instructions or fewer, no calls). The largest are `sub_826707B0` (0.46 %) and `sub_824C0698` (0.35 %) |
| 0.63 % | 29 | An app or SDK source mentions the address |
| 0.06 % | 4 | Refused: other forms |

Across all generated functions, by the same rules (2026-11-02):

| Functions | What |
|---|---|
| 1,499 | On a list |
| 24,238 | Eligible, never copied |
| 6,533 | An app or SDK source mentions the address |
| 4,035 | Tiny leaves |
| 1,422 | Refused: atomics (reservations) |
| 36 | Refused: other forms |

### What to do next

1. **The rest of the eligible functions: 24,238, in batches.** This is where "as many as possible" now lies, because every
   function any profile sampled is already done. They need no samples to qualify, so play nothing special for them. It is not
   free: rc9 to rc10 added 560 copies and the NRO grew from 53.6 to 59.8 MB, with other changes in that figure too, so roughly
   11 KB a copy, and something like 270 MB of code for all of them, plus the compile time. So: batches, watching NRO size, free
   heap and build time, and stopping where one of them hurts. Not tried yet.
2. **Callees of what is already copied: 7 functions, 145 statements.** Following direct calls from every listed function finds
   only these, because the lists are nearly closed under direct calls already. Trivial, and not done yet.
3. **Stale mentions: up to 6,533 functions.** `Hooks` reads every source under `app/src`, including banjo-nx files this port does
   not build, which name addresses from another game. An address that only such a file mentions blocks a copy of an unrelated
   Katamari function. How many of the 6,533 that accounts for has not been counted, and `sub_824DB5C8` and its neighbours
   (0.63 % of rc9) may well be real hooks. Count them first.
4. **Atomics: 1,422 functions, 3.18 % of the samples.** The largest gain left in what actually runs. Interlocked operations cannot
   be repeated (another thread would see them), so the guard cannot check such a copy, and any tool support needs its own way of
   showing it is exact. Harder than the rest.

### What stays out

- **Tiny leaves.** The generator already inlines them into their callers, and a hook can make them slower
  (bejeweled3-nx measured one going from 0.15 % to 0.85 % of its main thread when hooked).
- **Anything an app or SDK source mentions** (`Hooks` in the tool): two hooks on one function do not link.
- **What the tool refuses**, until it is taught otherwise. `fits()` names the reason; `--survey` shows it for any name.

A copy is not always faster: bejeweled3-nx and banjo-nx both saw copies come out as long as the original where the generator
had already kept the registers in locals. This port has not measured that function by function, since it has few
before-and-after console profiles. `tools/hot_arm_compare.py` compares the ARM code without a console. "As many as possible"
means copies are the default, not that each one is assumed to help.

## The hand track

### The gate

A function gets a hand-written native only when all of these hold:

1. **A console profile shows it.** Real hardware at stock clocks, sampling on, read with that build's ELF. A Ryujinx or PC
   profile does not count, however clear it looks.
2. **It is in the way.** It is in the thread that limits the frame rate, in a scene below target. A busy function on a thread
   with time to spare gains nothing.
3. **It is big enough.** At least 2 % of that thread's samples in that scene. This floor is this document's choice, not a
   measurement, and it is not a reason on its own.
4. **It already runs as a machine copy.** The profile must be measuring the copy, because that is what the native has to
   beat. Generated code that is hot needs a copy first, which may be enough.
5. **The samples show a cost a rewrite removes** ([next section](#reading-the-samples)). This is the condition most
   candidates fail.

Why the console and nothing else: conditions 2 and 5 are questions about the A57. Only samples of the ARM code on the console
show what limits the frame rate there and what a rewrite would remove. The PC does not run the ARM code, and Ryujinx runs it
at costs that are not the A57's. [What the data showed](#what-the-data-showed) is what choosing from those sources did here.

A user asking for a function by name, from console data, satisfies 1 to 4. Condition 5 still decides what gets written, and
the answer can be fewer functions than were asked for: rc10's request for five ended in three natives, with the reasons
recorded.

### What the data showed

Eleven of the 21 hand-written functions were chosen from a PC or Ryujinx profile, before this rule. The matrix comparison
`sub_82759A00`, chosen from the PC, has been followed through every source:

| Source | Of all samples | Of its thread |
|---|---|---|
| Its line in `hand.txt` | "PC hot" | |
| PC profile | 0.09 % of busy samples, rank 49 | 0.8-0.9 % where it reached a thread's table, rank 27-30 |
| Console rc4, rc5, rc9, by its own symbols | 0.00, 0.00, 0.00 % | 0.00, 0.00, 0.00 % |
| Console rc4, rc5, rc9, inside its inlined copies | 0.33, 0.31, 0.37 % | 1.15, 1.01, 1.27 % |

**It was never hot.** The PC's own numbers are far below the 2 % floor, and the native came from the word in its line. On the
console it stays under the floor, on a thread that in rc9 is draw submission and not the main thread that limits the slow
stages. It fails conditions 2 and 3, so this gate would have stopped it.

**The PC's number was not the error.** It and the console give the same order of size. But nothing could show that until a
console profile existed, and the PC's figure did not stop the native being written.

**The 0.00 % was false.** The native is inlined four times into the copy of `sub_82746B70` and once into the native of
`sub_8271F9C0`, so its samples count as theirs: 186 and 48 of rc9's. Read by symbol, it looks like a native that never runs and
a PC profile that was badly wrong, and neither is so. A share by symbol says nothing about a native until its callers have
been checked. `addr2line -i` found none of these samples. They were found by what the code contains: 16 `fsub` with the sign
masks between them, and the tolerance `0x350637BD`.

A ranking also does not carry from one source to another. The same functions, by containing function:

| Function | Console rc9 | PC | Ryujinx rc1 |
|---|---|---|---|
| `sub_82277E70` | 1 | 7 | 4 |
| `sub_822683D0` | 2 | 8 | 1 |
| `sub_82536F90` | 3 | 5 | 35 |
| `sub_827A5B00` | 6 | 4 | 34 |
| `sub_82656DF8` | 7 | 6 | no samples |
| `sub_82759C98` | 105 | 1 | 28 |
| `sub_827598C0` | 274 | 2 | 40 |
| `sub_827E6F50` | 361 | 23 | 2 |

These are not all differences of hardware, and no table can separate the causes. The sessions played different scenes, and
the builds differ in what is copied, hand-written and inlined. `sub_82277E70` calls the matrix copy and the multiply directly,
so the PC's ranks 1, 2 and 7 are partly the same work as the console's rank 1. A ranking from elsewhere mixes all of that with
the hardware, which is why it cannot answer what the gate asks: what limits this scene on the A57, and whether a rewrite
removes it.

### Reading the samples

Rank the functions, then look inside the candidate:

```bash
python tools/switch_hot_symbols.py PROFILE --elf ELF --thread "Main XThread" --top 40 --group
python tools/switch_hot_pcs.py PROFILE ELF "n_82277E70" "Main XThread" 0
python tools/switch_hot_lines.py PROFILE --elf ELF --function "n_82277E70"
python tools/switch_hot_stacks.py PROFILE --elf ELF --thread "Main XThread" --inclusive
```

`switch_hot_pcs.py` prints the sampled instructions with their disassembly. A sample usually lands on the instruction after
the slow one. What was found here, and what became of it:

| What the samples show | Example | Verdict |
|---|---|---|
| A load right after a store of the same data (failed store forwarding) | `sub_82277E70`: each matrix stored byte-swapped, the next step loading it back at once | Removable: keep it in registers |
| Stack stores and reloads around a computation | `sub_82265C08`, the box test in `sub_822683D0`: each corner through the stack | Removable |
| Flush-mode switches around arithmetic | `sub_82316A78`: four or five a call | Removable where the arithmetic can be shown not to need them |
| The same input read again and again | `sub_8275A828`: the matrix read for every vector | Removable |
| Context traffic with no work behind it | `sub_827596B0`: 76 statements of it | Removable |
| Loads of object fields spread over a dispatcher | `sub_822470D0`, `sub_827A5B00`, `sub_8227A280`, `sub_822772C0`, `sub_822889D8` | Cache misses. Hand code does not remove them, so not written |
| Interlocked operations | `sub_826707B0`, `sub_826A13D8` | Not hand-written here: other threads would see the guard's roll-back, so it cannot be checked |
| Time spent in callees | `sub_8221BC58`, `sub_82317F48` at the time they were considered | The callees are the candidates, not this function |
| A flat profile | | Leave it |

### Writing one

The code goes at the end of `app/src/cpu/hot/banjo_hand_natives.h`, in `namespace n_<id>`, as a `Native(ctx, base, s)` template
in front of the copy. Returning false declines, and the copy runs. Add its line to `hand.txt`, a key in
`app/src/cpu/banjo_hot_guest.cpp` (`katamari_hand_*`), and write `exact=` with the registers it leaves as the original does.

Keep the flush mode as the original has it: vector work runs with flush on, scalar and rotation work with it off, and
`katamari::Pin`, `PinW` and `Fence` keep arithmetic ordered around a mode switch. Decline on signalling NaNs where the generated
widening would quiet them, and on overlaps the generated code handles differently.

**Every new line in `hand.txt` records its evidence** in its comment: the profile (build), the scene, the thread, its share, and
what the samples showed. For example:

```text
82277E70 exact=...   # rc9 console, M71_KDXSE stage, main thread 25.1 %: matrices stored and reloaded (store forwarding)
```

If that comment cannot be written, the function does not pass the gate.

### After writing it

Check it ([Checking](#checking)), ship it, and read the next console profile of the same scene. If the time has not moved,
measured against functions that did not change ([Measuring](#measuring)), switch the key off and remove the code at the next
tidy-up. A native is kept because a console profile supports it, not because it passed its checks: passing them only shows it
is correct.

## Where the hand-written functions stand

Written before this policy, so not all of them pass it. Shares are of all samples in the console profile, by the function's
own symbol.

> [!NOTE]
> A native inlined into a caller's copy is counted as the caller, so a low share here does **not** prove a native is useless:
> `sub_82759A00` shows 0.00 here and really has 0.31-0.37 % ([What the data showed](#what-the-data-showed)). Before removing
> one, find its code inside its callers with `switch_hot_pcs.py`, or compare a console run with its key off.

| Function | What | Chosen from | rc4 | rc5 | rc9 |
|---|---|---|---|---|---|
| `sub_82277E70` | Objects' transform chain | **Console rc9** (25.1 % of main in M71_KDXSE) | 5.42 | 5.94 | 7.76 |
| `sub_82265C08` | Box test inside `sub_822683D0` (21.3 % of main) | **Console rc9** | 0.00 | 0.00 | 0.00 |
| `sub_8223A970` | Translate, then multiply | **Console rc9** call sites; weakest of the three | 0.02 | 0.02 | 0.00 |
| `sub_827596B0` | 4x4 matrix product | **Console rc3** | 0.04 | 0.00 | 0.06 |
| `sub_8275AA88` | N points by a matrix, with divide | **Console rc3** | 0.07 | 0.06 | 0.09 |
| `sub_8275A828` | N vectors by a 3x3 matrix | **Console rc3** | 0.00 | 0.00 | 0.03 |
| `sub_8270FE40` | Four vectors into an object | **Console rc3** | 0.00 | 0.00 | 0.00 |
| `sub_8276E3D8` | Integration step of N bodies | **Console rc3** | 0.00 | 0.00 | 0.00 |
| `sub_824E97F0` | Sine | **Console rc3** | 0.00 | 0.00 | 0.00 |
| `sub_82316A78` | Sphere test against a box. Key off in the release: the checks never reached it | **Console rc3** (1.3 %) | 0.00 | 0.00 | 0.00 |
| `sub_827E6F50` | memcpy | Ryujinx rc1 | 0.00 | 0.01 | 0.02 |
| `sub_827595D0` | Identity matrix | Ryujinx rc1 | 0.00 | 0.01 | 0.04 |
| `sub_8266AF60` | String hash | Ryujinx rc1 | 0.00 | 0.00 | 0.00 |
| `sub_8271FAD0` | 4-float parameter set | Ryujinx rc1 | 0.00 | 0.00 | 0.00 |
| `sub_827E73A8` | Word copy | Ryujinx rc1 | 0.00 | 0.00 | 0.00 |
| `sub_82759C98` | 4x4 matrix copy | PC | 0.06 | 0.04 | 0.11 |
| `sub_827598C0` | Matrix multiply in place | PC | 0.01 | 0.01 | 0.03 |
| `sub_82759780` | Matrix multiply, three operands | PC | 0.00 | 0.01 | 0.02 |
| `sub_82759D20` | Translation matrix | PC | 0.01 | 0.00 | 0.01 |
| `sub_827E7A40` | memset | PC | 0.01 | 0.01 | 0.01 |
| `sub_82759A00` | Matrix comparison | PC | 0.00 | 0.00 | 0.00 |

Eleven of the 21 were not chosen from console data. They are correct, so they stay for now, but each needs console evidence at
its next review, and the shares above are not that evidence. `sub_82759A00` has had that review: it would not pass the gate
today, but it is in use, so whether it goes is for a console comparison with its key off.

**The three rc10 natives have never been measured on the console.** The rows above for `sub_82277E70` and `sub_8223A970` are the
machine copies, before their natives existed. The rc10 console log has no samples (`Stack sampling: disabled`), and the PGO
session had sampling off. The next sampled console run of the M71_KDXSE stage is what judges them.

## Getting a console profile

Add one line to `katamari.toml` next to the NRO, and play the slow scene:

```toml
switch_profile_stacks = true
```

- The report is `logs/rex/rex_profile.log`, one every 10 s: every thread's CPU and every sampled pc. Bring the file back.
- Keep the ELF of the exact NRO. `builds/<package>/katamari-nx.elf` is kept for this. A profile read with another build's ELF is
  wrong without looking wrong: the log in the rc2 folder was written by rc3, and reading it with rc2's ELF reported 702 functions
  that needed copying. With rc3's ELF, it reported none.
- Name the scene and the time, as the notes do. Skip load time with `--from-report N`.
- Sampling pauses each busy thread 1,000 times a second, so read shares from these runs, not the best frame rate.
- Under Ryujinx the report goes to `sdmc:/switch/rex_profile.log`, one folder up from the game, because the emulator passes another
  program path. It is easy to miss when checking `logs/`.

## Checking

The PC and Ryujinx have no vote on what gets written, but they are where correctness is checked.

- **Every call, on the PC:** `powershell -File tools/run_pc_verify.ps1 -Seconds 300 -Name verify1`, with `banjo_cpu_verify` on and
  the guard on every call. A hook that can be repeated runs the original too, and registers and stored bytes are compared. It logs
  `[hot] DIFFERENCE` and switches the hook off on a mismatch.
- **Hooks with real or indirect calls** cannot be repeated, and count as "not comparable". Such a native needs its own check against
  a reference, as `sub_82277E70` has (`katamari_hand_transform_chain_verify`, the `Check` in its namespace). With `banjo_cpu_verify`
  such hooks run the original, so its native needs a run in the guard's normal mode as well.
- **It must actually be reached.** A check that never ran proves nothing. Look at the "compared" counts for that function. The
  sphere test ships off for this reason: the scripted runs do not reach the job threads' collision code.
- **Ryujinx:** the same guard through a test `katamari.toml` (`banjo_hot_guard_calls = 4000`, `banjo_hot_guard_period = 64`) and
  `tools/run_ryujinx.ps1 -Build switch-guard -Toml <file>`, on the guard build (`tools/build_switch_guard.bat`). Since port 78
  the everyday and released NROs have no guard forms (`SASRT_HOT_GUARD_FORMS`, OFF on the Switch): there the guard keys check
  no copy, and the log says so once ("this build has no guard forms"). Since port 81 they have no hooks at all (the copies
  in place); the guard build is hooked and compiles the same copies' code, so its clean run vouches for theirs.
- **The console:** no `[hot] DIFFERENCE` in a logged run (a guard run: the guard build's NRO).

rc10's result, as the standard to meet: on the PC, 28,049,408 calls of `sub_82277E70` compared with its reference, 4,314,903 of
`sub_82265C08` and 4,591,798 of `sub_8223A970` with the originals, and 342.6 million calls of the copies (1,219 of the 1,387 reached):
0 differences.

Machine copies get the same run in bulk, as batches, not one by one.

## Measuring

Whole-run frame rates depend on the scene: rc9 ranged from 19 to 60 fps with one build. To judge a change:

- Same scene, same clocks, on the console, sampling on in both runs.
- Compare the thread that limits the frame, using the share of the function and the thread's CPU.
- Use functions whose code did not change as the yardstick: if they got cheaper too, the scene did, not the code.
- `tools/switch_hot_symbols.py --group` splits a thread into game code, renderer, C library and so on. In an NRO with
  the copies in place (port 81) a copy's samples are under its generated name, `sub_X` (game code there), and where LTO
  inlined it, in its callers'; `tools/switch_hot_bulk.py` tells the copies apart by their records (`g_in_place_*`), and
  `tools/profile_breakdown.py` by their object (a hot shard) in a build without LTO.
  [katamari-nx's docs/console-rc9-breakdown.md](https://github.com/ChanseyIsTheBest/katamari-nx/blob/main/docs/console-rc9-breakdown.md) is the model for the report.
- Call counts: `banjo_hot_count` on the PC. They depend on the path taken, not the processor, so they are usable for working out
  cost a call, with the time still coming from the console.

The numbers to beat, rc9 in the M71_KDXSE stage: 19-30 fps (mean 27.4), main thread 101 % of a core, of which hand-written 0.7 %,
machine copies 93.4 % and unoptimised 3.8 %; `sub_82277E70` 25.1 % and `sub_822683D0` 21.3 %. That main thread was already almost
all machine copies, which is why the hand track starts there. 1.0.0 has not been measured against this.

## PGO and the order of work

A function changed after the instrumented build keeps no profile: GCC drops its counts and guesses. So the order is:

1. The last batch of machine copies.
2. Any hand-written natives, checked.
3. The instrumented build (`-DBANJO_PGO=generate`), then a console session.
4. The optimised build (`tools/build_switch_release.bat`).

Each form of the copies has its own profile (port 81): the everyday and released NROs have them in place, so the
session is an in-place instrumented build's and its profile goes to `pgo-inplace/` (docs/building.md, section 4).

A sampling run is a different thing from the PGO session: PGO counts show which paths ran and tune the compiler, but they do not
show time. A PGO profile is not hand-track evidence.

## Workflow

1. **Get a console profile** of the slow scene, sampling on, ELF kept.
2. **Machine, always:** merge the new profile into the bulk list, then take the next batch of eligible functions.
3. **Hand, only through the gate:** rank, look inside, write down the evidence. Most candidates stop here, and that is a fine result.
4. **Build and check:** PC verify on every call, with the counts showing each new function was reached.
5. **Ship, and read the next console profile** of the same scene. Keep what the numbers support.
6. **Before a release:** the PGO steps above.
7. Back to 1.
