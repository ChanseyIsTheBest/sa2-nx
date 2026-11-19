# Code-generation crash fixes (Beautiful Katamari)

> In the base: katamari-nx's record, kept as the worked example. The classes of bug (functions the analysis never
> found, tables read as jump tables, split functions, vcall thunks) and the scans after them apply to any game; the
> addresses are Katamari's.

Every crash this port has had that came from how the Xbox 360 code was translated, from the first boot
(2026-10-30) to the supermarket DLC crash of rc6 (2026-11-01): what happened, why, what fixed it, how it was found,
and what was done to find the rest of its kind. Two related translation bugs that broke the picture or the sound
without crashing are listed at the end, because they came from the same machinery.

## Summary

| # | When / where | Symptom | Class | Fix |
|---|---|---|---|---|
| 1 | 2026-10-30, third boot | "Call to invalid or unregistered function" at 0x82689770 | function not found by the analysis | pointer sweep (111) and gap sweep (85 functions) registered in `app/overrides.toml` |
| 2 | 2026-10-30, fourth boot | exception C000001D (illegal instruction) in sub_8222E438 | call through a table of functions read as a jump table | `[[switch_tables]]` entry on the real index register |
| 3 | 2026-10-30 evening, first build with the register diet | crash at boot in sub_824CF3F8 | register-diet option broke a split function | `share_registers` on 44 split pieces |
| 4 | rc3 on the console, 2026-10-31 | crash leaving the collection menu ("unregistered function" 0x823700C8) | function not found by the analysis | registered; the pointer sweep's window widened from 8 to 32 instructions |
| 5 | rc6 on the console, 2026-11-01 | crash entering the supermarket DLC level (`brk #1000` in sub_822AC470) | call through a table of functions read as a jump table | `indirect_calls` (new in the generator) |
| 6 | found by the scan of 2026-11-01, never hit yet | would crash the same way in sub_822DF3B0 | same as 5 | same as 5 |

## 1. A function the analysis never found (first boot stop)

**Symptom.** The third boot stopped with `Call to invalid or unregistered function at guest address 0x82689770`.

**Cause.** The code generator finds functions by following calls and branches from the entry point. A function
that is only reached through a pointer (a vtable entry, a callback stored in data, an address built with
`lis`/`addi` and called later) is invisible to that walk. It stays as unclaimed bytes between two known functions,
is never translated, and the first call to it at run time has nowhere to go. 0x82689770 is a 10-instruction table
accessor sitting between two registered functions.

**Fix.** Two sweeps of the image, each accepting a candidate only if it decodes as a complete function (ends in
`blr`/`bctr` or a tail branch, no padding or invalid words, reads no register that is undefined at a function
entry):

- `tools/pointer_fixpoint.py` (pointers stored in data or built in code): 111 functions. Seven of its first picks were
  wrong and were removed: the `lis`/`addi` it took for a function address was building a jump table's base, so the
  "functions" were case code inside their parent (the code generator reported "Jump target ... unresolved at bctr").
- `tools/gap_sweep.py` (the first word of every gap and the word after each `blr`/`bctr` inside one, outside the 2 KB
  after a jump-table `bctr`): 85 functions.

Both are in `app/overrides.toml` with the evidence for each.

## 2. A table of functions read as a jump table (second boot stop)

**Symptom.** `exception C000001D` (illegal instruction) in sub_8222E438 on the next boot.

**Cause.** The generator turns each `bctr` (jump through the count register) that reads a jump table into a C++
`switch`, with a trap (`__builtin_trap`) as the default case. sub_8222E438 calls through a table of five function
starts at 0x82A196A8, indexed by the word at 0x82B095E0
(`lwz r10; rlwinm r10,r10,2,0,29; lwzx r11,r10,r11; mtctr r11; bctr`). The generator recognised a table but made the
switch test r11, the *loaded function address*, with cases 0-3. An address is never 0-3, so every call fell to the
trap.

**Fix.** A `[[switch_tables]]` entry in `app/overrides.toml` that switches on r10 (the index already multiplied by
4), with the targets at 0, 4, 8, 12 and 16. The first version had four entries and trapped at index 4: the table is a
state machine whose fifth entry means "done".

## 3. The register diet broke a split function (first diet build)

**Symptom.** The first build with `non_volatile_as_local` crashed at boot in sub_824CF3F8.

**Cause.** `non_volatile_as_local` keeps the callee-saved registers (r14-r31, f14-f31, v14-v31, v64-v127) in C++
locals instead of the shared context, which removes a lot of memory traffic on the Switch. That is only correct if
every function sets those registers before reading them. sub_824CF3F8 is a split-off tail of sub_824CEC10 (its
`__finally` handler) and reads r31 that its parent set up. As a separate function with its own locals, it read a
zero and computed its stack frame from it.

**Fix.** `tools/read_before_write.py` run on code generated *with* the option (the first audit ran on code without
it and saw nothing) listed 44 such split pieces. They are marked `share_registers` (33 in `config/renut_funcs.toml`,
11 in `app/overrides.toml`), so they use their parent's registers. Two functions that read v69 / v72, which nothing in
the game sets, went into `non_volatile_as_local_keep`.

## 4. A vcall thunk the sweeps missed (collection menu, rc3)

**Symptom.** On the console, leaving the collection menu stopped with
`[FATAL] Call to invalid or unregistered function at guest address 0x823700C8`.

**Cause.** Same class as 1. 0x823700C8 is a 4-instruction virtual-call thunk
(`lwz r12,0(r3); lwz r11,36(r12); mtctr r11; bctr`) next to three thunks that were registered. Its address is built
9 instructions after its `lis`, one past the pointer sweep's 8-instruction window, and the gap sweep skipped it
because it lies within 2 KB after a jump table.

**Fix.** Registered in `app/overrides.toml`; the pointer sweep's window is now 32 instructions. Three wider scans
followed (every `lis`/`addi` pair in a function with no window limit, every aligned data word pointing into code,
every vcall-thunk shape with any registers). None found another missing function. What a scan cannot see is a
pointer the game computes some other way; `tools/runtime_gap_resolver.py` handles those when a run hits one.

## 5. The supermarket DLC level (rc6)

**Symptom.** Entering the supermarket DLC level: the game stopped. `rex_crash.log`: `brk #0x3e8` at image+0x2914,
which the rc6 ELF names as the translated sub_822AC470.

**Cause.** Same class as 2, a variant the earlier fix did not cover. sub_822AC470 calls through a table of function
pointers:

    lis r11 / lwz r11,-19096(r11) / lhz r11,968(r11) / extsh r11,r11    index from a game object
    rlwinm r11,r11,2,0,29                                               index * 4, in r11
    lwzx r11,r11,r10                                                    r10 = table 0x82A3AE08: the load overwrites r11
    mtctr r11 / bctr

The load writes the function address into the register that held the index, so at the `bctr` no register holds the
index any more. The generator still made a two-case switch (0x822AC4F8 and 0x822AC5A8, the right targets) but tested
r10, the table's base address, so every call trapped. The game only calls this function in that level. A
`[[switch_tables]]` entry cannot fix it: there is no register to switch on. The only correct translation is an
indirect call through the function pointer.

**Fix.** The generator already loaded an `indirect_calls` list from the config but never used it. It is now wired
into both of its jump-table detectors (`FunctionScanner::detect_jump_table`, used when code is emitted, and
`detectJumpTable`, used during block discovery): a `bctr` listed there is never read as a table and becomes
`REX_CALL_INDIRECT_FUNC`. `app/overrides.toml`: `indirect_calls = [0x822AC494, 0x822DF3C4]`. (The first rebuild only
covered the emission detector and changed nothing; the block-discovery detector was the one that made this switch.
The code generator also skips regeneration when its inputs match the last run, so a generator change needs
`rexglue codegen --ignore-stamp`.)

**Checked.** In the rc7 build both functions are `REX_CALL_INDIRECT_FUNC` through the loaded pointer and every target in
both tables is a registered function. The emulator's scripted run cannot reach the supermarket, so the level itself
is to be checked on the console. A guarded gameplay run in Ryujinx (every machine copy compared with the generated
code: 4.54 million calls, 0 differences) and `switch_check.py` on the rc7 code (no finding) found nothing else.

## 6. The same bug, not hit yet (sub_822DF3B0)

Found by the scan below. sub_822DF3B0 masks and scales its index in one instruction (`rlwinm r10,r3,2,27,29`: index
0-7 times 4), loads from an 8-entry table of handlers at 0x82A3B848 and jumps. The generator made a one-case switch
on r11, the loaded pointer, so any call would trap. Fixed the same way (`indirect_calls`). All eight table entries
are registered functions.

## The scan of 2026-11-01: are there more?

`tools/switch_check.py` compares every `bctr` in the game's code with what the generator wrote for it. It had been
written to catch exactly this class, but it had gone blind without anyone noticing:

- Since the register diet (the evening of 2026-10-30), the generator writes most switches on locals
  (`switch (r11.u32)`, `l_r10`, `ll_r9`); the checker only matched `switch (ctx.rN.u32)` and saw 157 of the 770.
- Switch cases that leave or enter a loop go through synthetic labels (`loc_F0000001:` ... `goto loc_X;`); the
  checker took those for addresses and reported false "wrong targets".
- Indirect calls are now emitted inside a block that saves registers; the checker looked only 3 lines ahead and
  missed them.
- A table jump whose index register is overwritten by the load (case 5) went to "odd shapes, review by hand"
  without checking whether the generator had made a switch there. The run of 2026-10-30 noted its 23 odd shapes as
  indirect jumps without checking each one.
- The masked-index form of case 6 was not recognised as a table at all.

All five are fixed. The checker now also reports a switch at a rewritten index register as a crash risk, takes the
mask as the table's bound, and accepts an indirect call through a table of function starts in data. Result on the
rc6 generated code, against all 1,219 `bctr` instructions of the image:

| Check | Result |
|---|---|
| Switches parsed | 768 of 770 (the other two are inline copies of sub_823A4050, a small leaf copied into two other files; its own switch is parsed and right) |
| Truncated tables (missing cases) | 0 |
| Cases jumping to the wrong place | 0 |
| Tables emitted as plain indirect calls | 0 |
| Switch on a register that does not hold the index | 2: sub_822AC470 (5), sub_822DF3B0 (6) |
| Tables whose size the code does not state, checked by hand | 8: three are indirect calls through tables of registered functions (54, 22 and 11 entries); sub_8222E438 is fix 2; sub_8236E778 (12 cases), sub_827F1A80 and sub_827F79B8 (8 each) are bounded right before the jump; sub_8282CE58's second table is bounded to 15 by an earlier check and has 17 cases |
| Other jump shapes (vtable calls and similar) | 19, all emitted as indirect calls |

Other ways the translated code can stop: no `REX_FATAL` (unresolved jump), no jump to null and no other
`__builtin_trap` in the generated code. The 929 `ppc_trap` sites are the game's own `tw`/`twi` checks, which trap on
the Xbox 360 as well.

## What a scan cannot catch

- A function reached only through a pointer computed in an unusual way (class 1 and 4). The three scans of
  2026-10-31 cover stored pointers, `lis`/`addi` pairs and thunk shapes; anything else shows up at run time as
  "unregistered function" with the address, and is fixed by registering it.
- A register-diet mistake in code paths that `read_before_write.py` cannot see (class 3). The guard
  (`banjo_hot_guard_calls`) compares the machine-optimised copies with the generated code; it cannot check the
  generated code itself.

## Related translation bugs that did not crash

- **Movies decoded into green blocks, and a steady 345 Hz tone over them (rc3).** The register diet again: the
  movie decoder loads nine constant vectors into v64-v72 in one helper and uses them in others. With
  `non_volatile_as_local` the helper's loads went into its own locals and the decoder read zeros. Fix:
  `non_volatile_as_local_keep = ["v64-v127"]` (`app/renut_config.toml`). Found by bisecting in Ryujinx (rc1 clean,
  rc2 broken).
- **Missing shaders (rc6, the supermarket drew nothing).** Not code generation, but the same session: the renderer
  has no on-console shader translator, so a draw whose shader is not in the package's cache is skipped. 24 shaders the
  game builds from its (compressed) data at run time were missing in that run; the console saved each one to
  `shader_capture/` on the SD card, and the next package adds them.

## Safeguards inherited from banjo-nx

- `-mearly-ra=none` for every file compiled for the console: GCC's AArch64 early register allocation pass dropped
  table lookups of the NEON byte-permute helper in another port and broke large textures on the console only.
- The hand-written natives order their arithmetic around flush-mode switches with explicit compiler barriers
  (`katamari::Fence`, `Pin`), because the host's `msr fpcr` carries no memory operand the compiler would respect.
