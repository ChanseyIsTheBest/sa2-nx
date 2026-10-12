# Enhancements

Performance changes that keep the rendered image and the game's behaviour the same. Every one has its own key in
`sa2.toml` and belongs to a group whose master key must also be on (`sa2_cpu_enhancements`, `sa2_noncpu_enhancements`).
The framework is `app/src/cpu/sa2_enhancements.{h,cpp}`.

What the log shows:

- `[enh] sa2-nx build <date> <time> (<codegen flavour>); ...`: the first enhancement line. It names the build and lists
  every key as `on`, `off` or `on-but-master-off`.
- `[enh] uses in the last 10 s: ...`: how often each active enhancement ran. An enhancement that is switched on but
  never appears here did not run.
- `[verify] checks done: native replacements N | differences (...): D`: printed when checks run. D must stay 0.

Premade configurations (`python tools/make_enhancement_configs.py`, written to `out/nx/configs`):

| File | Change from `sa2.toml` |
|---|---|
| `sa2_cpu_off.toml` | the CPU group off |
| `sa2_noncpu_off.toml` | the non-CPU group off |
| `sa2_all_off.toml` | both groups off: the original paths everywhere |
| `sa2_cpu_verify.toml` | `sa2_cpu_verify = true`: every native call and every renderer fast path is checked against the original path (slow) |
| `sa2_profile.toml` | GPU time per pass (`sa2_gpu_marks_categories`) |
| `sa2_cpu_on.toml` | the defaults with the profiler on (the reference side of every A/B below) |
| `sa2_test_deferred_off.toml` | deferred Vulkan recording off (on by default since round 5), profiler on |
| `sa2_test_direct_state_off.toml` | the direct renderer's state records off: the ring parses PM4 as before round 5, profiler on |
| `sa2_test_main_priority_on.toml` | the drawing game thread at 0x3A (round 2), profiler on |
| `sa2_test_copies_off.toml` | the 53 unguarded localized copies off (on by default since round 4), profiler on |
| `sa2_test_upload_uncached.toml` | the upload buffer in uncached memory as before round 3, profiler on |
| `sa2_test_render_scale_off.toml` | the render resolution off: 1280x720 docked too, as before (2026-10-07), profiler on |
| `sa2_test_msaa2.toml`, `sa2_test_msaa4.toml` | `sa2_msaa = 2` / `4`: MSAA of the game's render targets (2026-10-07), profiler on |
| `sa2_test_vsync_relaxed_off.toml` | `sa2_vsync_relaxed = false`: the old fixed vsync (round 13), profiler on |
| `sa2_test_round13_off.toml` | round 13's hand-written vertex loaders, njCalcPoint and njMultiMatrix off, profiler on |
| `sa2_test_vs_fetch_reorder_off.toml` | `sa2_gpu_vs_fetch_reorder = false`: vertex shaders Direct3D reordered are dropped again unless the package has them (2026-10-07), profiler on |

The profiler (`switch_profile_stacks` and `sa2_gpu_marks_categories`) is on in `sa2.toml` and in every
configuration except `sa2_cpu_verify.toml` (2026-10-06, the user's choice: every test run brings its profile).

How to measure: compare the same spot with one configuration changed. The frame rate is capped at 60, so compare the
game thread's CPU % in `rex_profile.log` and the `[enh] uses` sleep time, not only FPS.

## CPU group

### `sa2_cpu_gpu_wait_sleep` (+ `sa2_gpu_wait_max_us`, default 1000)

Direct3D's waits for the GPU loop on `sub_8254F400`. That routine waits 32 × db16cyc, checks the GPU read pointer and
the hang timer, and returns "keep waiting". The hook sleeps on the ring thread's progress counter before an iteration
in which nothing can have changed:

- the same wait record as the previous iteration;
- the read pointer unchanged;
- no ring progress, fence write or interrupt since that iteration returned.

The `[enh] uses` line reports the time asleep.

- **Why exact:** the game reads the same memory and takes the same decisions, only later. The progress counter is read
  after the original iteration returns, so progress the caller could have missed always prevents the next sleep.
- **Risk:** a wait for something that neither the ring nor an interrupt reports would lose up to max_us per iteration.
  None was seen: PC stayed at 600 swaps every 10 s.
- **Measured (PC, attract mode):** the game thread used 1.01 cores before and 0.55 after, at the same 60 FPS. It now
  sleeps 54 to 95 % of the time instead of spinning.

### `sa2_cpu_ring_notify_lockfree`

The ring thread's progress notification takes the mutex only when somebody is sleeping on it, using a sequentially
consistent handshake (masseffect-nx's `sa2_notify_no_lock`).

- **Why exact:** the wake-ups are the same, and the counter is only an event count.
- **Risk:** a lost wake-up, which would cost at most `sa2_gpu_wait_max_us`.

### `sa2_cpu_lazy_trace_text`

The EDRAM transfer-trace text is formatted when a trace line is logged, not for every draw slot until 60 traces exist.
On the PC profile this was about 4 % of the ring thread.

- **Why exact:** the log text is the same.
- **Risk:** none.

### Native replacements: `sa2_cpu_native_*`

Implementation: `app/src/cpu/sa2_hot_guest.cpp` and `app/src/cpu/hot/`, from masseffect-nx's guarded hot hooks. Each
native function engages only if the loaded game code hashes to the code it was written from
(`tools/guest_code_hash.py`).

Every native is self-checked. On the first 256 calls of each thread, then 1 call in 4096, it runs on a copy of the
registers. The guard then rolls memory back, runs the recompiled original, and compares registers, written memory and
the FPU mode. A difference logs `[hot] DIFFERENCE` and switches that native off for the rest of the run. With
`sa2_cpu_verify` every call is checked.

SA2 was compiled with interprocedural register allocation. Callers keep values in volatile registers across calls to
functions known not to touch them; `tools/call_site_liveness.py` shows, for example, that callers of njCalcPoint read
r4 to r10 after the call. So a native never writes a register the original leaves alone.

| Key | Guest function | How |
|---|---|---|
| `sa2_cpu_native_memcpy` | sub_827F05C0, CRT memcpy | host memcpy/memmove; declines `dst > src` overlaps (the original smears) |
| `sa2_cpu_native_memset` | sub_827EFEA0, CRT memset | host memset |
| `sa2_cpu_native_memmove` | sub_827EF8B0, CRT memmove | host memmove; declines the signed/unsigned order corner |
| `sa2_cpu_native_calc_point` | sub_82146ED8, njCalcPoint | localized copy |
| `sa2_cpu_native_multi_matrix` | sub_82146F60, njMultiMatrix | localized copy |
| `sa2_cpu_native_translate` | sub_82147AC0, njTranslate | localized copy |
| `sa2_cpu_native_push_matrix` | sub_82147DA8, njPushMatrix | localized copy |
| `sa2_cpu_native_rotate_x/y/z` | sub_82147B38 / C08 / CD8, njRotateX/Y/Z | localized copy (calls the game's sin/cos) |
| `sa2_cpu_native_map_lower_bound` | sub_8213EC28, std::map lower bound | localized copy |

A "localized copy" (`tools/localize_native.py`, regenerated by `tools\codegen.ps1` into `app/generated/hot/`) is the
recompiled function's own C++ with two changes:

- the guest registers are C++ locals, written back before each call and at every return;
- guest memory is accessed without `volatile`.

The operations are the same, so the results are the same.

How they were chosen: the PC profile of the game thread with the GPU wait asleep, counting busy samples only.

| Function | Share of busy samples |
|---|---|
| memcpy | 6.1 % |
| njCalcPoint | 4.2 % |
| map lower bound | 3.0 % |
| njMultiMatrix | 1.9 % |

njTranslate, njRotate* and njPushMatrix are called from 1,000 to 2,700 places each.

Verified on PC: 115 million checked calls in attract mode with 0 differences. The guard caught two localizer bugs
before that run.

Measured (PC): the game thread's busy share went from 35.6 % to 31.5 % of samples.

| Function | Change in samples |
|---|---|
| memcpy (host side) | −35 % |
| njCalcPoint | −25 % |
| njMultiMatrix | −24 % |
| map lower bound | −10 % |

- **Risk:** an input the guard never saw that behaves differently. The CRT natives decline the cases they do not
  reproduce, and the localized copies cannot differ.

### The ring thread: masseffect-nx's measured set (CPU group)

These are renderer cvars that masseffect-nx measured on its console: its ring thread went from 28.0 to 25.7 ms, and
deferred recording alone took it from 62 to 40 %. SA2 had them all off. Each fast path checks itself against the old
path for its first uses.

The keys:

- `sa2_native_flat_identity`, `fast_pair`, `raw_microcode`, `pm4_fast`, `compare_fast`, `fast_copy`,
  `indices_cache` and `pipelines_prewarm` (all true);
- `fingerprint_mode` = 1;
- `dedupe_max_fingerprint` = 16384.

They are listed in `sa2_enhancements.cpp` with on and off values, so the CPU master switches them too. A key missing
from an older `sa2.toml` is turned on anyway (futari-nx's console ran with half its set off because of an old toml).

Checks so far:

- **PC lockstep capture with all of them:** every gameplay frame identical.
- **On the console** (log `sa2_001 (2)`, before this set): the ring thread was the busiest thread at stock clocks, 60
  to 82 % of a core, at about 30,000 draws a second.

`sa2_native_deferred_recording` stays off (explicit only). On the PC it gave frames of 100 to 160 ms; futari-nx
saw the same with `sa2_native_output_no_wait`.

### The 15 next-hottest game functions as localized copies

Keys: `sa2_cpu_native_sub_<addr>`, for sub_8213A4D8, 82139DA0, 8214CF10, 8214E130, 82555CC8, 821BC520, 82549178,
82960CE0, 82141420, 82555B70, 82960928, 8295F9C0, 82552408, 82960808 and 8214D6D8.

They call other functions, so the guard cannot run both versions: the callees' side effects would happen twice. They
run unguarded, exact by construction, and the localizer was proven by the guard on the first 8 copies.

Checks:

- **PC lockstep capture:** every gameplay frame identical, with each copy called millions of times.
- **PC speed:** no gain; per-function time is equal or slightly higher. x86 reads volatile memory cheaply, and the
  copies pay to sync registers around calls.
- **Console speed:** unknown. On the console, GCC's code for volatile guest memory is costlier.

**Off by default until round 4.** Before round 3, a copy wrote back and reloaded every localized register at each
call, so copies that make real calls ran slower: sasrt-nx's PC A/B of 36 copies measured +13-18 % CPU per frame, up to
twice the samples per function. Leaves and functions that only call the save/restore helpers got faster. Round 3's
smart sync (below) syncs only the registers a call can reach, and round 4 turned every copy on after the PC checks.
`configs/sa2_test_copies_off.toml` turns them all off for the console A/B.

How the console finds the next hot functions:

1. Turn the sampler on with `switch_profile_stacks = true` (the A/B and profile configs do).
2. Every sampled pc now goes into `rex_profile.log` with its count.
3. `tools/arm_hot_functions.py` ranks a PC profile with the Switch ELF's per-function AArch64 code: FPCR mode
   switches, byte swaps and calls.

Floating-point mode switches are cheap in SA2. Of 46,307 mode-switch points in the generated code, only 196 enter the
vector unit's flush mode. The rest are conditional checks that write FPCR only when that mode is on, so a codegen
"fewer mode switches" pass is not worth it here.

### Round 2: what the console profile showed (2026-10-06)

The cpu_on/cpu_off/verify console runs used the optimised NRO at 1020 MHz:

| Run (gameplay) | FPS | CPU used |
|---|---|---|
| cpu_on | 29-36 | 131-167 % |
| cpu_off | 21-26 | 209-213 % |

The verify run checked 20.5 million native calls with 0 differences. At 2397 MHz cpu_on holds 61 FPS.

The game's speed is tied to its frame rate: below 60 it falls to 30 or 20, so the target is a locked 60 at stock
clocks. `tools/switch_profile_report.py <rex_profile.log> --elf out/nx/ab/optimised/sa2.elf --max-fps 40` ranks the
slow gameplay intervals:

- **GPU ring thread:** 61-83 % of a core, about 25 ms per frame, about 20 µs per draw at ~1,300 draws per frame. It
  sleeps the rest of the frame in the vblank WAIT_REG_MEM.
  - NVK: ~30 %. `vk_dynamic_graphics_state_copy` alone is 11 %, 2.2 µs per pipeline bind changing ~1 bit.
  - memcpy: 9 %.
  - Our renderer: ~35 %.
  - fmt string formatting: ~4 %. malloc/free: ~3 %.
- **Main thread:** about 15 ms per frame of its own work. About 60 % of its samples wait in the D3D GPU wait
  (`sub_8254F400` from the command-buffer allocator), i.e. for ring-buffer space. The ring is the bottleneck.

The C6 "substages" stopwatch covers only ~6.5 µs of each draw's ~20; the rest is PM4 decoding, shader identity and
EDRAM tracking outside it.

Rules for this round: CPU work only (GPU changes only if they lower CPU load), no driver changes, every GPU-side
change pixel-identical, and only the optimised NRO is delivered.

#### Ring thread and main thread, our own C++ (on by default)

| Key | What | Why exact |
|---|---|---|
| `sa2_cpu_hook_counter_shared` | The native hooks count calls in one relaxed counter per hook instead of a `thread_local` (on the Switch every TLS access is a call to `__aarch64_read_tp`, on every hooked call) | Only which calls the guard samples changes |
| `sa2_cpu_note_shader_fast` | `NoteShaderObject` (main thread, twice per draw): the 32-line startup diagnostic stops taking its mutex once full; finished objects are cached in one shared atomic table, not a `thread_local` one | "Finished" only becomes true (nothing erases the object sets), so a hit returns where the locked path would. Main-thread profile: NoteShaderObject + NoteDrawCall + mutexLock + read_tp ~7 % |
| `sa2_cpu_ring_extent_key` | The bounded-draw extent diagnostic dedupes on its five values (XXH3) instead of an fmt string key; SA2 never fills its 32 slots, so it formatted on every bounded draw | Same log lines (6.8 % of the PC ring's running time) |
| `sa2_cpu_ring_estimator_reason` | The draw-extent estimator keeps its reason as a pointer to the literal, not a `std::string` (a heap allocation per call over 15 characters, the success reason included) | Every reason is a literal |
| `sa2_cpu_ring_rect_list_in_place` | `ProveRectangleList` sets the two register words right before each estimate and restores them right after, instead of copying the 80 KB register file | The estimator sees the same words |
| `sa2_cpu_ring_identity_accept_fast` | `AcceptVertexShaderIdentity` skips the mismatch hash-set lookup when the flat memo already says "matches" | The mismatch memo only holds failed pairs; checked while the self-check counter runs |
| `sa2_cpu_ring_entry_hash` | The vertex-input cache key reuses the ring's XXH3 of the loaded VS (`current_vs_hash_`) instead of re-hashing the program on every VS change | Same function over the same words; first 2,048 uses (all in verify mode) recompute and compare |
| `sa2_cpu_ring_redirect_key` | `RedirectClearDepthEDRAM4`'s decline reasons are counted by (literal, depth info) and formatted for the 10 s report | Same report line |
| `sa2_cpu_ring_target_memo` | `GetTarget` / `GetDepth` (every slot of every draw; 1.2 % + 2.2 % of the ring) answered from a 4-slot memo of their inputs | The maps never erase and their nodes never move, so the same inputs give the same image |

`sa2::enh::VerifyMode()` is now read once at `Resolve()` instead of by name per call.

#### Game functions: 13 guarded natives (on by default)

They are the hottest guardable functions of the console profile: leaves, or functions calling only the save/restore
helpers, memcpy and another native. Functions with `bctrl` (indirect calls) cannot be guarded and were skipped:
`8214B520`, `829621A8`, `8213E040`, `82141240`, `82141860`, `82915B18`, `8213D1F8`, `82139930`, `82960CE0`.

| Key | Function | How |
|---|---|---|
| `sa2_cpu_native_d3d_constants_a` / `_b` | `sub_8255D990` / `sub_8255D8B8`: Direct3D's shader float-constant writers (1.3 % + 1.2 %) | Hand-written: one copy of the 16-byte vectors plus the dirty-mask OR (`hot/n_d3d_constants.h`). Overlapping or 4 GB-wrapping ranges are declined; no register is written |
| `sa2_cpu_native_key_search` | `sub_8214BBD8`: keyframe binary search | Localized copy |
| `sa2_cpu_native_records_equal` | `sub_82963658`: compares two 24-byte record arrays | Localized copy |
| `sa2_cpu_native_vmx_matrix` | `sub_82140A90`: VMX 4x4 matrix routine | Localized copy |
| `sa2_cpu_native_matrix_op` | `sub_82140C98` (2.9 %): calls memcpy and sub_82140A90 | Localized copy, `kCalls` |
| `sa2_cpu_native_cos` / `_sin` | `sub_8214A208` / `sub_8214A050`: njCos / njSin | Localized copies |
| `sa2_cpu_native_matrix_4x4` / `_3x4` | `sub_82146E48` / `sub_82146DD8`: matrix transposes | Localized copies |
| `sa2_cpu_native_store_slot` | `sub_82141700`: one-of-seven word store | Localized copy |
| `sa2_cpu_native_records_copy` | `sub_82963590`: 24-byte record copy | Localized copy |
| `sa2_cpu_native_visibility` | `sub_821BA660`: visibility test (njCalcPoint by the current matrix, frustum planes) | Localized copy, `kCalls` |

After adding hooks, `tools/post_codegen.py` must run again: `direct_calls.py --undo` and redo (1,531 calls went back
through hooks), then the localized copies are regenerated.

#### Console A/B configs (off by default)

- **`configs/sa2_test_deferred_on.toml`:** `sa2_native_deferred_recording`, `sa2_deferred_native_fast`
  and `sa2_deferred_native_update`. The ring thread queues its vkCmd* calls and a worker records them: same
  commands, same order, so the image is identical. masseffect-nx measured its ring going from 62 to 40 % of a core.
  The 100-160 ms PC frames were Windows rounding the queue's 5-20 µs `sleep_for` up to the timer period. The PC
  build now yields there (`ShortPause`); the Switch path is unchanged.
- **`configs/sa2_test_main_priority_on.toml`:** `sa2_cpu_main_thread_priority`, from nfsmw-nx. The thread that
  draws runs at 0x3A (`sa2_main_thread_priority`), one step above the other game threads. Only 0x3B is time-sliced
  on Horizon (10 ms slices).
  - **Risk:** if that thread spin-waits on work of a 0x3B thread on the same core, the other thread waits for a core
    change. Off until the A/B shows it helps and nothing hangs.

#### Checks on PC (2026-10-06)

- **Verify mode, defaults:** 82,992,218 native calls and renderer self-checks compared, 0 differences. All 13 new
  natives engaged except the two called rarely in the attract loop.
- **Lockstep captures pinned to game frames:** `sa2_video_frame` waits for a game frame, and `<capture>.frames`
  labels every recorded image with its game frame. Runs: all CPU enhancements off (baseline), defaults, and defaults
  plus deferred recording, each 3,000 frames from game frame 6000 (title, loading, City Escape 2P demo).
  - Defaults vs baseline: 2,938 of 2,968 game frames identical. Deferred vs baseline: 2,935 of 2,963.
  - The rest is the loading screen's spinning icon (x 344-364, y 308-332, real-time animated while loading) and one
    fade frame: timing, not rendering.
  - The game-frame counter can be offset by a flip or two between runs (boot and loading flips), so frames are
    matched exactly within +-3 labels.
- **Defaults capture vs the original reference** (`nat_g`, the classic build): 105 of 105 gameplay frames identical.
- **Guard false positives found and fixed:** in normal runs, push_matrix, rotate_x/y and sub_82140C98 logged
  `[hot] DIFFERENCE` in the stack below their own frames. That is their callees' dead scratch: the CRT memcpy saves
  r3 at -8(r1), the native one writes nothing there, and it differs whenever only one of a check's two nested memcpy
  calls is itself checked.
  - The ranges were 128 for a 112-byte frame, 176 for 160 and 320 for 288; they are now exactly each function's own
    frame.
  - Verify mode cannot show this (every nested call is checked). The round-1 ranges had the same flaw; earlier runs
    did not happen to hit it.

#### From nfsmw-nx, not done in this round

- **`non_volatile_as_local` with `share_registers` marks:** drops the `__savegprlr`/`__restgprlr` calls (14 % of
  NFSMW's main thread; ~4 % of SA2's).
- **LTO, PGO** and hot-function ordering.
- **Direct3D register dump as one marker packet:** large.
- **IM_LOAD without memcmp:** shader-constructor write counters.
- **The ring's 10 s report written by another thread:** a 30 ms SD stall every 10 s.
- **Guarding natives that make calls:** a recorder of their outgoing calls.

### Round 3 (2026-10-06)

Console results of the round-2 NRO (cpu_on and verify runs, 1020 MHz): performance up, verify 21.6 million checks with
0 differences.

The cpu_on run logged one real guard DIFFERENCE: sub_82140A90, a 4x4 inverse of an all-zero matrix.
- **Cause:** -NaN natively and +NaN in the original. With FPCR.DN off, fmul/fadd of two NaNs return the first
  operand's NaN, and GCC may swap the operands of commutative operations, so two compilations of the same C++ can
  disagree on a NaN's sign.
- **Fix:** the guard's `kAllNaN` / `kCallsNaN` compare (any NaN equals any NaN, in float registers and float memory
  words), used only by the float natives sub_82140A90 and sub_82140C98.

Still CPU-bound: the ring thread ~25 ms per frame at 1020 MHz (NVK state copy 12.5 %, PrepareDrawEDRAM4 10 %, memcpy
9 %, descriptor binds 7 %), the main thread ~16 ms.

#### Build: the user's compiler set (UnleashedRecomp-NX's, without PGO, at -O3)

Every Switch NRO is now built this way; these are build options of `app/CMakeLists.txt`, all ON.

- **`SA2_SWITCH_LTO`:** link-time optimisation of the recompiled game (sa2_recomp) and the app (renderer, natives):
  `-flto -fno-fat-lto-objects`, linked with `-flto=2 -flto-partition=balanced`.
  - The SDK libraries and the stage module stay out: the module is partially linked with `ld -r` + objcopy, which
    needs machine code.
  - The link repeats `-ffp-contract=off -fno-strict-aliasing`.
  - xxHash is built with `XXH_FORCE_MEMORY_ACCESS=0` (nfsmw-nx's LTO pitfall: its default reads violate strict
    aliasing once inlined into a key builder).
  - The LTO link takes about 17 minutes on the build PC: GCC on Windows runs its 512 LTRANS partitions serially. The
    NRO grows from 55.2 to 57.8 MB (cross-file inlining).
- **`SA2_SWITCH_IPA_PTA`:** `-fipa-pta` at compile and link (UnleashedRecomp-NX).
- **`SA2_SWITCH_HOT_ORDER`:** the 1,500 functions with the most console samples first in .text
  (`app/hot_functions.ld`, `tools/hot_order.py`, from nfsmw-nx's orden_funciones.ld). It starts with
  `KEEP (*(.crt0))`: nfsmw-nx's NRO did not boot when the ordering moved the start-up code off address 0.
- **`tools/check_switch_build.py`** runs on every NRO before packaging:
  - `_start` at 0;
  - the optimised flavour string;
  - the SDK driver's Mesa string;
  - branch-to-self loops in recompiled code only at the 15 known guest halts;
  - no vector `fmla`/`fmls` in recompiled functions (the generated code has none, so any would be contraction that
    leaked past `-ffp-contract=off`). The round-2 non-LTO build: 0 vector, 10,173 scalar (the guest's own `std::fma`).

#### Ring and main thread, our own C++

| Key | What | Why exact |
|---|---|---|
| `sa2_cpu_note_shader_fast` (extended) | SA2 binds fewer than 32 distinct shader objects, so the trace set never filled and every NoteShaderObject call still took the mutex for an insert. Console: ~2.4 % of the main thread in that insert plus ~3 % in lock/unlock. A 64-slot lock-free table of keys already in the set now skips the lock | A key in the set can never be logged again |
| `sa2_native_upload_memory = 1` (kExternal, CPU group) | The upload buffer (every draw's vertices and indices) in cached memory, published before each submit. The console measured 5,251 MB/s of CPU writes there against 2,506 MB/s in the uncached type the SDK picks; the shared constants already use the cached type this way | Same bytes reach the GPU; `configs/sa2_test_upload_uncached.toml` is the A/B |
| `sa2_cpu_ring_bias_seen` | Draws into a render target with a colour exponent bias did an unordered_set insert each, only to log the first sighting; a 4-slot table of logged target infos skips it | Same log lines |

#### Localized copies: smart sync, and 20 more functions

- **`tools/localize_native.py`, smart sync:**
  - **Before:** a copy wrote back every localized register before each call and reloaded all of them after it,
    vector registers included. That made copies with calls slower than the originals (sasrt-nx: +13-18 % CPU).
  - **Now:** a call writes back and reloads only the localized registers its callee can reach: those its body
    mentions, plus everything its callees mention, transitively. Every generated function is indexed
    (`out/localize_index.json`) and the reach comes from strongly connected components of the call graph.
  - **Full sync is kept for:** indirect calls, traps, imports and host functions, and functions overridden by an app
    hook other than our natives.
  - **Examples:** sub_82140C98's four calls now sync 7, 11, 22 and 7 of its 28 registers; push_matrix and the
    rotations no longer sync fully anywhere.
  - **Fixed on the way:** the old pattern only treated `sub_`, the save/restore helpers, indirect calls and traps as
    calls, so a call to an import would not have been synced. Any `name(ctx, base);` is a call now.
- **The guarded natives that make calls** (push_matrix, rotations, sub_82140C98, sub_821BA660) use smart sync too, so
  verify mode checks the mechanism on every call.
- **20 more functions as copies** (keys `sa2_cpu_native_sub_<addr>`):
  - addresses: 8214B6C8, 8214B520, 8213D1F8, 829601B0, 82832B78, 8213D7F0, 8214ACA8, 828326F8, 82833E20,
    828243A0, 821BA890, 82833938, 82834458, 82816B40, 82139930, 82834538, 821472E0, 82565758, 8213E040, 82833568;
  - they are the next hottest main-thread functions of the round-2 console profile;
  - they make calls (some indirect) and write through computed pointers, so they run unguarded, exact by
    construction, like the 15 of round 1.
  - All the unguarded copies share one default, `SA2_COPIES_DEFAULT` (on since round 4);
    `configs/sa2_test_copies_off.toml` turns them all off.
- **`tools/direct_calls.py --redo`:** undo plus redo in one pass, writing only the files that change. A new hook
  rewrote all 138 generated files, and so recompiled them all; now only the files that call the new hook.

### Round 4 (2026-10-06): 20 more game-thread functions, copies on by default

**PC checks of round 3's copies (all 35 on, smart sync), lockstep captures pinned at game frame 6,000:**

- verify mode: 186,449,222 checks, 0 differences; every one of the 35 copies ran (up to 270,000 calls per 10 s);
- normal run: every frame of the 3,000 that the baseline's window holds is pixel-identical to the baseline with the
  CPU group off (2,983 of 3,000; the other 17 come before the baseline's first frame). The copies run was 4-6 game
  frames behind the baseline: loading screens take a different number of frames when the game thread runs at another
  speed. The old comparison only looked at offsets of up to 3 frames, so it reported every frame as different.
  `anyoffset.py` (the session's scratch tools) hashes every frame and matches each against all frames of the other run.

So `SA2_COPIES_DEFAULT` is now `true`: the 53 unguarded copies (15 + 20 + 18) ship on, and
`configs/sa2_test_copies_off.toml` is the console A/B.

**Round 4's 20** (keys `sa2_cpu_native_sub_<addr>`): the main-thread functions of the round-2 console profile (cpu_on,
1020 MHz, under 58 fps) with the most samples of their own that had no native yet. A copy speeds up the function's own
code, not its callees, so they are ranked by self samples. Together they hold 89 of the main thread's 945 samples
(9.4 %).

| Address | Kind | Self samples | What |
|---|---|---|---|
| 82565C60 | copy, from the renderer's hook | 16 | D3DDevice_DrawVertices: the draw's PM4 packets (the largest function without a native; 7.5 % with what it calls) |
| 82556A60 | copy, from the renderer's hook | 6 | D3D draw-time shader load (FlushState part) |
| 82555E18 | copy | 6 | D3D: copies a block of state into the command buffer |
| 8251D2A0 | copy | 6 | float routine with indirect calls |
| 82141050 | copy | 6 | calls sub_82141C40 and one indirect call |
| 821390C8 | copy | 5 | wrapper of sub_82139120 |
| 82832298 | guarded leaf (kAllNaN) | 4 | rescales 16 records of four floats by a limit over the fourth |
| 8214B320 | guarded leaf (kAll) | 4 | fills one 17-byte record of a table at 0x83853480 from three lookups indexed by r3 |
| 8217EB38 | copy | 4 | calls sub_8217FA70 and one indirect call |
| 82141240 | copy | 4 | float routine with six indirect calls |
| 825632F8 | copy | 3 | D3D: VMX and float state into PM4 packets |
| 8251D1A8 | copy | 3 | object transform: njTranslate, njRotateX/Y/Z and sub_8251D2A0 |
| 82556718, 8254DED0, 8254D0E0 | copies | 2 each | Direct3D internals (memcpy and D3D helpers) |
| 8217F048, 8214EAF0 | copies | 2 each | the 0x8217E family; a float routine calling njCalcPoint twice |
| 82227C98 | copy | 4 | calls the 0x82227 family, sub_82222B60/82222A08 and sub_82195B08 |
| 8213D5C8 | copy | 4 | converts a float and dispatches through one indirect call |
| 82139C90 | copy | 4 | three memcpy calls |

- **Why exact:** the copies are the recompiled functions' own operations (`tools/localize_native.py`); the two leaves
  are also guarded: the guard runs both versions and compares every register (NaN-tolerant for the float one) and the
  memory they write (sub_8214B320's record address is computed with the same loads the function makes).
- **The two Direct3D functions the renderer also hooks** (DrawVertices and its shader load) cannot have a second
  override. `SA2_COPY_FOR_HOOK` defines `sa2::hot::Copy_<addr>` (declared in `cpu/sa2_hot_copies.h`), and the
  renderer's hooks in `src/me/sa2_me_hooks.cpp` call it where they called `__imp__sub_<addr>`: the copy when the key
  is on and the loaded code matches, the original otherwise.
- **Localizer fix:** `native_hooks()` now counts `SA2_HOT_HOOK_COPY` as a native (a copy leaves exactly the registers
  its original leaves), so a copy calling another copy syncs only what the callee can reach instead of everything.
  `SA2_COPY_FOR_HOOK` stays out on purpose: the renderer's hooks read argument registers themselves.
- **Localizer fix 2:** a copy now declares every direct callee, kernel imports included (`__imp__KeBugCheck`,
  `__imp__RtlEnterCriticalSection`, ...). Round 3 already synced around any `name(ctx, base);` call, but only declared
  `sub_` functions and the save/restore helpers, so the first copies that call an import (sub_82556A60, sub_82545970)
  did not compile.
- **Lesson: hook descriptions must not name other functions' addresses.** `tools/direct_calls.py` and
  `tools/localize_native.py` treat every 82xxxxxx address in the app and SDK sources as hooked. Round 4's first
  descriptions ("calls sub_82222B60 ...") named nine callees, which took the direct call away from 264 call sites and
  made the copies sync everything around them. `tools/direct_calls.py`'s output and the copies' "synced per call" lists
  show it; the other 29 such mentions in the sources have at most 16 call sites, on the GPU wait path.
  - Fixed in the tool (2026-10-07, after futari-nx found the same in its own): in C/C++ files `tools/direct_calls.py`
    now reads only the code. It ignores comments, and `__imp__sub_X` references, which call the original and hook
    nothing. The native Direct3D layer's comments and its calls of originals had kept 198 calls weak in 67 files,
    the D3D space function's 123 among them.
  - Checked: every hook-macro use in the raw sources is still counted (0 dropped).
  - The .toml and .txt files are still read whole. `app/overrides.toml` names 967 functions (boundary entries) but
    only 43 call sites; it stays counted, to be safe.
- **Not done, and why:** the save/restore helpers (`__savegprlr_*` / `__restgprlr_*`, 5.9 % of the main thread) are
  already optimal in the generated code (`ldp` from the context, `rev`, `stp` to the guest stack: about 25
  instructions for nine registers), so a native version gains nothing. The ring thread's remaining costs are spread
  thin (PrepareDrawEDRAM4 9 % already has its fast paths; the per-packet vector and clock calls are under 1 %); its
  real levers are deferred recording (`configs/sa2_test_deferred_on.toml`, still untested on the console) and an
  API-level renderer.
- **Left out:** sub_8254F400 (the GPU wait loop: `sa2_cpu_gpu_wait_sleep` hooks it already; its samples are the
  wait), sub_821B4D08 (a bare double-precision multiply: the localizer refuses it, contraction could differ).
- **PC checks of the round-4 build** (all 53 copies and both leaves on by default, lockstep, pinned at frame 6,000):
  - verify mode: 176,674,950 checks, 0 differences;
  - shipped defaults: 2,988 of 3,000 frames pixel-identical to the baseline with the CPU group off, at an offset
    of 0 or 1 frame; the other 12 come before the baseline's first frame.
- **Switch build** (LTO, 8 parallel LTRANS jobs, about 5 minutes; `tools/check_switch_build.py`: all checks passed,
  0 vector FMAs, 10,273 scalar, 15 known halts). ELF: `out/nx/elf/sa2_round4_20261006_2218.elf`.
- **Risk:** a copy that differs only on inputs the PC run did not reach (unguarded: verify mode cannot see it). The
  A/B config turns them all off.

### Round 5 (2026-10-06): the direct renderer, step 1, and the ring's back end

The goal (the user's): the ring thread at 8-10 ms a frame on the console at 1020 MHz (round 4: about 22 ms, 19.5 µs
a draw), by removing what the ring emulates instead of tuning it. Round 4's console profile of the ring: the EDRAM
emulation about 19 %, PM4 parsing about 21 %, Vulkan recording about 15 %, vertex/constant fingerprints and dedupe
about 14 %, shader identity about 6 %.

#### `sa2_cpu_direct_state`: no PM4 for the draw state (the front end)

- **What:** the hooks of the three Draw* functions (DrawVertices sub_82565C60, DrawIndexedVertices sub_82566078,
  DrawVerticesUP sub_82565C18, in `src/me/sa2_me_hooks.cpp`) call `BeginDrawDirect` before the original and
  `EndDrawDirect` right after it. `EndDrawDirect` decodes the packets the call wrote before its draw packet into a
  host record:
  - register runs in host byte order;
  - type-1 pairs;
  - the type-3 packets the ring handles anyway, as they were (IM_LOAD, IM_LOAD_IMMEDIATE, SET_CONSTANT,
    LOAD_ALU_CONSTANT, INVALIDATE_STATE, WAIT_REG_MEM, REG_RMW, EVENT_WRITE).

  It then writes one NOP packet over the region (header, `'SA2D'`, record number). The old packets stay inside the
  NOP as payload; the draw packet and every address Direct3D keeps stay where they were. The records live in an 8 MB
  ring of host memory.
- **On the ring:** `Type3`'s NOP case finds the record and applies it:
  - register runs through the same register writes (`WriteConstantRunHost` has `WriteConstantRun`'s effect, compared
    and copied a part at a time);
  - single-register runs and type-1 pairs through `WriteRegister`;
  - type-3 packets through `Type3`, with their original words and address.
- **Why exact:** the same register writes in the same order at the same place in the stream. The decoder walks the
  words exactly as `Packet()` does (zero words, type-2 fillers and NOPs skipped).
- **Left as PM4:** a draw during which the game submitted (wrote CP_RB_WPTR) or the command buffer changed segment
  (device+52/+56), an unknown or predicated packet, and a NOP in the region's first three words (inline data another
  packet may point at). A submission that races with a marker being written waits for it (`g_marking`: either the draw
  thread sees the submission and leaves its packets, or the submission publishes the marker whole).
- **Self-checks:**
  - the first 4,096 records (every record in verify mode) are re-encoded and compared word for word with what Direct3D
    wrote;
  - the ring checks that every marker finds its record, in order. If one does not, it logs an error and the game
    thread makes no more records.
- **Not handled:** a command buffer range the GPU runs twice, such as predicated tiling replays. SA2 has none (the
  report's "tile replays" stays 0); Banjo does, and needs records kept until the last pass.
- **Numbers (PC):**
  - 99 % of the draws converted; the other 1 % had a submission during the call;
  - the PM4 packets the ring parses per draw went from about 21 to about 4. The constant flush's alignment fillers
    alone were about 11 packets a draw, each a trip through `Packet()`;
  - verify mode: 1,924,356 records, every one re-encoded identically, 0 lost, 0 left as PM4 for an unknown packet.
- **Risk:** Direct3D reading back or patching a state packet after the call returns (none of SA2's draw functions
  does; the lockstep captures would show it).
- **Key:** `sa2_cpu_direct_state` (CPU group). Off: `configs/sa2_test_direct_state_off.toml`.
- **Report:** `[native] direct state (cumulative): ...`: draws converted, records applied, lost (must stay 0), and why
  the others were left as PM4.

#### `sa2_cpu_ring_edram_memo`: the EDRAM preparation of a draw like the previous one

- **What:** a draw whose 14 render-target registers equal the previous draw's reuses that draw's EDRAM preparation,
  but only when the full path would do no work: same images, tiles owned in the same epoch, nothing pending, content
  still valid, the draw's area inside the area already prepared, the same stencil sources.
- **Why exact:** in exactly those conditions the full path (`PrepareDrawEDRAM4`) changes nothing. The memo is dropped
  whenever a redirect path handles a draw.
- **Self-check:** in verify mode every memo hit also runs the full path and compares its outputs and its sync, stencil
  and epoch counters (DIFFERENCE: the memo is off for the session).
- **Numbers (PC):** 97 % of the draws from the memo; verify mode: 155,419 hits checked, 0 differences.
- **Key:** `sa2_cpu_ring_edram_memo`.

#### `sa2_cpu_ring_compare_uploaded`: one copy of each upload, not two

- **What:** the constants dedupe and the shared block's change test compare with the block already uploaded
  (CPU-cached memory) instead of a second copy kept for that purpose.
- **Why exact:** it compares the same bytes. In verify mode the shadow copy is still kept and compared too.
- **Key:** `sa2_cpu_ring_compare_uploaded`.

#### Deferred Vulkan recording, on by default

`sa2_native_deferred_recording`, `sa2_deferred_native_fast` and `sa2_deferred_native_update` are
now on (they follow the CPU master like masseffect-nx's measured set). The ring queues its vkCmd* calls and a worker
records them: the same commands in the same order. masseffect-nx measured its console ring thread going from 62 to
40 % of a core. Off for the A/B: `configs/sa2_test_deferred_off.toml`.

#### The ring's own timer

`[native] ring thread: X ms of work per Swap (Y µs per draw, N draws per Swap), Z ms per Swap waiting for the game
(WAIT_REG_MEM)`, every 10 s. "Work" is the ring's parse loop less its WAIT_REG_MEM waits; it includes the time the
ring waits for the host GPU. On the PC, most of the ring's profile samples are those waits: SA2's command stream
waits on memory the game writes, and the PC ring catches up with the game.

### Round 6 (2026-10-07): what round 5's console profile showed, and the next emulation removed

Round 5 on the console (logs5, 1020 MHz, the heaviest scene at about 1,650 draws a frame and 30 FPS):

| Thread | Round 4 | Round 5 |
|---|---|---|
| Ring | 22-24 ms a frame, 19.5 µs a draw | 17.6-18.8 ms a frame, 11.4 µs a draw (-38 % a draw) |
| Main (game) | 14-15 ms a frame (a lighter scene, ~1,300 draws) | 19-21 ms a frame |
| Deferred-recording worker | - | 8.5 ms a frame (pinned to core 0) |

The game runs at exactly 60 or 30, so both main and ring must stay under 16.7 ms in the heaviest scenes.

- **The main thread** is now the bottleneck there. The renderer's own hooks are about 9 % of it (`EndDrawDirect` 4.9 %,
  `NoteDrawCall` 2.5 %, `NoteShaderObject` 1.7 %), and the guest Direct3D library about 14 %.
- **The ring's largest costs:**

  | Cost | Share of the ring |
  |---|---|
  | `DrawsVulkanImpl::Draw` (own time) | 29 % |
  | Applying direct records | 7.7 % |
  | Shader identity | about 6.4 % |
  | XXH3 and sampled fingerprints | about 6 % |
  | `PairByAddress` | 4.6 % |
  | Deferred queueing | about 4 % |

**The census** (`sa2_direct_state_census`, a diagnostic) counts what each direct record changes. On PC:

| What a draw changes | Share of records |
|---|---|
| Vertex fetch constants | 97 % |
| Texture fetch constants | 76 % |
| Shader constants | 33 % |
| A shader load | 11 % |
| Render state (0x2000-0x23FF) | about 2 % |

The 0x2100 group (the VGT index range) is written by every draw but never changes. So for about two thirds of the
draws, the only change since the previous draw is which vertex and texture data to use.

#### `sa2_cpu_direct_pair`: the Draw* record inside the direct record

- **What:** `NoteDrawCall` holds the call's record back. `EndDrawDirect` puts it in the call's direct record (op
  `kPair`), and the ring uses it for the draw packet right after the marker, under the table's own rule: at most 0x2000
  below the packet, same primitive type.
- **The other paths:**
  - A draw that keeps its packets pushes the record to the queue, under `g_marking`.
  - A submission during the call pushes it before it publishes the write pointer (`FlushPendingPairs` in `WriteMmio`).
  - A submission from a thread that never drew turns the key off, with a warning: Direct3D's device is used by one
    thread.
- **It also fixes a real problem.** The address table holds 8 records per 8 KB bucket, but the game writes about 13
  draws per 8 KB. When the game runs ahead of the ring, the oldest records are evicted before the ring reaches them,
  and those draws fall back to ring-side shader identification. In verify mode 40 % of the carried records were no
  longer in the table (the old report's "unpaired" count was mostly these). Every one the table still held was
  identical to the carried one.
- **Checks (PC):** verify mode 133.4 M checks, 0 differences; 1,077,541 records compared with the table, all equal.
  The lockstep capture matched the baseline; the only unmatched frames were the title screen's wall-clock animation.
- **Round 6 together** (pairing, identity memo, shader-pair skip, references, bound-plan memo, write-back):
  - verify mode 169.6 M checks, 0 differences;
  - exact lockstep capture: 2,971 of 3,000 frames identical to the reference, and all 29 others outside its window.
- **Key:** `sa2_cpu_direct_pair`.

#### `sa2_cpu_note_shader_pair`: two loads instead of four calls

- **What:** `NoteShaderObject` runs four times per draw: twice from the draw hook and twice from the shader-load hook.
  When both objects are already "finished" (NoteShaderObject's own lock-free table, never erased), the calls return at
  once anyway. Now the hooks check the table and skip both calls.
- **Why exact:** a finished object's call returns before doing anything.
- **Key:** `sa2_cpu_note_shader_pair`.

#### `sa2_cpu_ring_identity_memo`: the identity checks once per change, not per draw

- **What:** a directly paired draw reuses the previous draw's pairing and final VS/PS choice when all of these equal
  that draw's:
  - the two shader objects;
  - the object-table version (bumped on every publish);
  - the loaded VS and PS and their load generations;
  - the fast-pair and flat-identity switches.

  A pair the table rule refuses goes to the replay and queue paths, so it is never memoised.
- **Why exact:** the identity functions read only those inputs (their audit counters aside).
- **Self-check:** in verify mode the memo is never used. Every draw with the stored key must reach the stored outcome
  through the full checks (DIFFERENCE otherwise).
- **Numbers (PC):** about 86 % of draws hit.
- **Key:** `sa2_cpu_ring_identity_memo`.

#### `sa2_cpu_direct_state_refs`: the record holds the skeleton, not the values

- **What:** a packet that starts at word 2 or later is recorded as register, count and offset (`kRunRef`,
  `kRunSingleRef`, `kType1Ref`, `kType3Ref`). The ring reads its values from the command buffer, which stays intact
  under the marker NOP until the ring has passed it. Packets in the marker's three words are still copied.
- **Why:** the game thread no longer byte-swaps and copies about 100 words per draw into a cold 8 MB ring, which costs
  cache-line fills on the A57. The ring reads the same bytes it read before round 5, without the parsing.
- **Why exact:** `Packet()`'s own type-0 handling (`WriteConstantRunRaw` or the per-word path) on the same words.
- **Numbers (PC):** 54.7 record words per draw instead of about 110. Verify mode: every record re-encoded identically
  (`SameAsWritten` checks references by register, count and offset).
- **Key:** `sa2_cpu_direct_state_refs`.

#### `sa2_cpu_ring_bound_plan_memo`

- **What:** `PlanEdramBoundRanges` is a pure function of a small request (views, pitch, MSAA, masks). The draw path
  reuses the previous plan when the request is field-for-field the same. Verify mode recomputes and compares.
- **Key:** `sa2_cpu_ring_bound_plan_memo`.

#### `sa2_cpu_vertices_base_zero_r0`: one vertex-buffer binding per upload buffer, not one per draw

- **What:** `sa2_native_vertices_base_zero` binds the upload buffer at 0 and shifts the draw (firstVertex, or
  vertexOffset = first_vertex - vmin). It skipped every vertex shader that reads r0, which is all of SA2's ("0 of N
  draws" in the zero-base report). So each draw recorded a vkCmdBindVertexBuffers, an NVK macro of about a
  microsecond on the A57 (the deferred worker's ~3.3 calls a draw cost ~8.5 ms a frame).
- **The change:** for an r0-reading shader the vertex index base in the shared block becomes vmin - first_vertex
  instead of vmin.
- **Why exact:** XenosRecomp initialises r0 as float(SV_VertexID + g_VertexIndexBase) in unsigned arithmetic, and
  SV_VertexID includes the base vertex (DXC without -fvk-support-nonzero-base-vertex). The shift and the compensation
  cancel for every vertex, indexed or not, so the shader sees the same r0. CheckBaseZero's self-check of the
  offsets is unchanged.
- **Where:** the zero-base decision moved ahead of the shared block, where g_VertexIndexBase is written. Mass
  Effect's postponed sky keeps its own binding, because it is drawn again later.
- **Numbers (PC, gameplay):** 100 % of draws take the zero-base path; vkCmdBindVertexBuffers went from 1.00 to 0.00
  per draw.
- **Lockstep capture:** 2,996 of 3,000 frames identical to the deferred-off reference. The other 4 lie outside the
  reference's window.
- **Key:** `sa2_cpu_vertices_base_zero_r0`.

#### Fingerprint mode 2 on the Switch: the hardware CRC32 pair

- **What:** `sa2_native_fingerprint_mode` = 2 (`sa2_crc_fingerprint.h`, masseffect-nx's): two lanes of
  the A57's CRC32C and CRC32 instructions instead of XXH3's NEON loop. It covers the vertex dedupe, the index cache
  and the texture recheck fingerprints. The Switch build has `-march=armv8-a+crc+crypto`; the PC build falls back to
  XXH3 (mode 1) by itself.
- **Checked on PC with a software shim** of the four instructions (scratch `crc_test`):
  - 48,856 single-bit flips all change the fingerprint;
  - zero buffers of every length 0-256 all differ;
  - 200,000 random 520-byte buffers, no collision;
  - 496 two-word changes, no collision;
  - the software CRCs match the standard check values.
- **Key:** `sa2_native_fingerprint_mode` (CPU master: 2 on, 0 off).

#### Deferred recording, checked again in exact lockstep

Futari-nx found deferred recording not pixel-identical in its game: a menu fade differed on four frames,
differently in each run. SA2 rechecked with the exact lockstep captures below:

- deferred off vs the CPU-off baseline: 2,994 of 3,000 frames identical, at one constant offset;
- deferred on vs deferred off: 2,519 of 2,519 frames held by both captures identical. That includes a 47-frame fade
  from black (swaps 6383-6430).

The unmatched frames in both lie before the other capture's first frame. Deferred recording stays on in SA2.

#### The deferred-recording worker's core (an A/B, default unchanged)

The round-5 console profile shows the worker pinned to core 0 (libnx core id -2: the process's default core), at the
ring thread's priority, above the game threads'. It preempts whatever game thread shares core 0, and the game thread
is now the bottleneck in the heaviest scenes. `sa2_native_deferred_worker_core` sets it:

- -2: as before;
- 0-2: only that core;
- -1: any of cores 0-2, ideal core 2 (the SDK's own `svcSetThreadCoreMask` call).

`configs/sa2_test_worker_core2.toml` and `configs/sa2_test_worker_core_any.toml` are the console A/B. Scheduling
only: the image cannot change.

#### Line-level console profiles

The Switch ELF has line tables for the app's own code (`-g1 -gdwarf-4`, the app target only; the NRO is made from
the stripped ELF). The linker leaves the line sequences of discarded sections at address 0, where they overlap the
hot functions placed first in .text, so addr2line and llvm-symbolizer answer with the wrong file there. Look addresses
up in an `llvm-dwarfdump --debug-line` dump instead, skipping the sequences at 0 (`tools/nx_line_map.py`).

#### Lockstep captures that survive a loaded PC

With the user's five ports building at once, the recorder skipped up to 1,402 flips and a comparison became
meaningless. With `sa2_video_lockstep` (on), the ring thread waits after each presented swap until the recorder has
taken that frame (`SetPresentHook` / `OnPresent`, at most 1 s). Frames are labelled by presented-swap number, which is
the same in every lockstep run. Holding the game at its flip instead does not work: Direct3D submits a frame's swap
lazily, with the next frame's work.

### Round 7 (2026-10-07): the codegen "diet", float moves without FPCR writes, and two correctness fixes

Round 7 is mostly the code generator: it speeds up all the game code at once instead of one function at a time.

#### `non_volatile_as_local` + `elide_lr_stores` (build-time, in the optimised flavour)

- **What:** two more generator options in `app/perf_overrides.toml`, the ones the other ports measured.
  - SASRT: `ctx.r14-r31` uses fell from 669,839 to 1,586, save/restore helper calls from 29,172 to 0, and `ctx.lr`
    stores from 158,515 to 0.
  - masseffect-nx: -10 % main thread with the same set.
  - r14-r31, f14-f31 and v14-v31/v64-v127 become C++ locals of each recompiled function. The compiler keeps them in
    host registers, and the `__savegprlr_N` / `__restgprlr_N` helper calls disappear (5.9 % of SA2's console main
    thread in round 4).
  - `ctx.lr = <return address>` is no longer stored before every call.
- **Checks before turning them on:**
  - `tools/read_before_write.py --noreturn=827F2E48` (sub_827F2E48 is the C++ throw: the code after it is a catch
    funclet the discovery merged in, which accounted for 12 apparent "r14 read before written" cases):
    - one real case, sub_82546D74, a fragment of sub_82546560 reached through a pointer table that reads the
      parent's r31: marked `share_registers` in `app/overrides.toml`;
    - the tool's known false alarms (`xor rX,rX,rX` in sub_8280A620 and sub_8280A9B0, `subfe rX,rX,rX` in
      sub_828AC628);
    - the guest setjmp (sub_82908F10), as before;
    - in the stage DLL, sub_88014440, a loop body registered as a function that nothing calls (its parent runs the
      loop inline).
  - **Trap:** the flavour file is also included by the stage DLL's module. A `[functions]` entry there makes the DLL
    generate a stub at that address ("function 0x82546D74 not in any code region ... generating stub"), so the mark
    lives in the main module's `overrides.toml`. Without non_volatile_as_local the mark changes nothing.
  - **`ctx.lr` readers:** only the d3d-trace diagnostic (`sa2_d3d_trace`, off) and the console profiler's caller
    attribution (`switch_perf.cpp`). With the stores elided, those show an older return address, so caller lines in
    console profiles from this build onwards are not reliable. A PIC `bl` to a local label keeps its store.
  - **setjmp/longjmp** (libjpeg's error path, corrupt JPEG data only): after a longjmp ReXGlue restores `ctx`, but the
    setjmp caller's r14-r31 locals keep their values from the failing call instead of setjmp's. This is the same class
    of risk as the CR locals already shipped.
  - The localized copies are regenerated from the new code by `tools/post_codegen.py`. They now keep r14-r31 as
    locals and call no save/restore helper.
  - **`tools/check_switch_build.py`:** GCC now inlines the known guest halts of sub_828DB988 and sub_8293FA08 into
    their callers (sub_828D71E8, sub_8293FE28), so a `b .` appears there too. The check now accepts a self-loop in a
    function whose generated body calls a known-halt function, and names it.
- **PC checks (round 7a: these two options plus the round-6 renderer, texture sampling pinned to the old 8):**
  - verify mode: 285,195,336 checks, 0 differences;
  - exact lockstep capture: every one of the 3,000 frames (2,846 distinct) has a pixel-identical frame in the round-6
    captures. The new build presents fewer frames during loading, so its window starts 1,790 swaps earlier in the
    boot sequence; the intro frames match the round-6 runs whose windows reach that far.

#### `fp_moves_modeless`: float moves without flush-mode switches (sa2-nx's own generator option)

- **Why:** on the A57 every host flush-mode switch is an `msr fpcr` that costs as much as dozens of instructions.
  - In the round-5 console profile, the VMX-heavy game thread `XThreadF43C2C0` (14 % of a core in the heaviest scene)
    had 20-26 % of all its samples on the instruction right after an `msr fpcr`.
  - In its hottest function, sub_8292BDC0, the share was 65 %.
  - The generator switched to IEEE mode for every scalar float instruction, including the ones that only move bits,
    and back to flush mode for the next VMX instruction.
- **What:**
  - lfd/stfd and their indexed and update forms, stfiwx, fmr, fabs, fnabs and fneg only move bits (the sign ops are
    integer operations in the generated code): no switch at all.
  - In VMX code (flush on), the lfs/stfs forms convert through `rex::ppc::SingleToDoubleAnyMode` /
    `DoubleToSingleAnyMode` (`sdk/include/rex/ppc/diet_ops.h`). These give the flush-off result in either mode, so
    the code no longer switches off and on again.
  - Elsewhere, lfs/stfs switch to IEEE mode as before.
- **Why exact:** the flush mode can change a single <-> double conversion in only two cases:
  - a denormal single input (lfs);
  - 0 < |d| < 2^-126 for stfs: a result in the single denormal range, or a denormal double input.

  Those cases take a cold path that clears the mode for that one conversion. Volatile asm barriers keep the
  conversion between the two mode writes. Every other input converts identically in both modes, on AArch64
  (tininess before rounding) and x86 (after).
  - **Unit test (PC, scratch `fpm`):** about 1 million single bit patterns and 4 million doubles (including the
    boundary range and denormal doubles), with flush both off and on. 0 differences from the IEEE conversion.
  - **Control:** the plain conversion with flush on differs 984,644 times, so the test exercises the mode.
- **Effect on the generated code:**
  - flush-mode switches fell from 46,307 to 40,339;
  - 104 lfs/stfs now convert in VMX code without switching;
  - sub_8292BDC0 keeps one conditional switch at its entry, where it had one around every scalar load and store
    between its VMX operations.

#### `sa2_native_dedupe_sync_epochs`: the dedupe's guest synchronization, finally advanced

- **The bug** (Banjo's and SASRT's finding, present in every port on the masseffect-nx renderer):
  - The vertex dedupe and the index cache forget what they recorded when `g_synchronizations_ring` changes.
  - Their design relies on that: the game may rewrite a referenced range only after a guest synchronization.
  - But nothing ever advanced the counter, so ranges above `sa2_native_dedupe_max_fingerprint` (16 KB) were
    matched by address alone for a whole upload buffer.
- **Fix:** the ring advances the counter in stream order at every packet that writes something the game can observe:
  EVENT_WRITE_SHD fences, MEM_WRITE, REG_TO_MEM, COND_WRITE memory writes and INTERRUPT. Only after one of those may
  the game rewrite memory that an earlier draw read.
- **Cost:** more copies, never a stale one. `configs/sa2_test_sync_epochs_off.toml` is the console A/B for what it
  costs.
- **Key:** `sa2_native_dedupe_sync_epochs` (non-CPU group: it fixes a correctness hole).

#### Texture fingerprint sampling off by default

- **Why:** `sa2_native_fingerprints_sampling` (masseffect-nx's, default 8) rechecks stable textures with a
  sample and computes both fingerprints for the first 3,000 rechecks, switching itself off at the first
  disagreement. In 56 of 58 SA2 PC runs the disagreement came after those 3,000 (at 3,044-3,123: texture 1EE4E000,
  512x512 k_8_8_8_8 with mips, whose changes the sample does not see). So the sample-only rechecks before it may
  already have kept a stale texture for up to 8 rechecks.
- **What it saved:** on the console (round 5) about 1.7 MB/s not read, well under 0.1 % of a core with the CRC32
  fingerprints.
- **Now:** 0, the full fingerprint every time (SA2's copy of the cvar's default, and in `sa2_switch.toml`). 8 restores
  masseffect-nx's behaviour.

#### Round 7 checked together (PC, 2026-10-07)

All of round 7 together: the diet, `fp_moves_modeless`, the sync epochs and texture sampling off.

- **Verify mode:** 166,030,875 checks, 0 differences.
- **Exact lockstep captures:**
  - shipped defaults: all 3,000 frames (2,962 distinct) have a pixel-identical frame in the earlier captures (round
    6's and round 7a's), compared with the scratch tool `cmp/pool.py`;
  - the verify run's capture: 3,000 of 3,000 as well.
- **Dedupe:** over the capture run, the share of lookups that reuse an upload fell from 42.1 % (round 7a) to 18.9 %.
  The sync epoch advances about 23 times a frame, so there are more copies, but never a stale one.
- **Switch build:** `tools/check_switch_build.py` passed every check.
  - `msr fpcr` instructions in the whole text: 35,922 -> 31,726;
  - in sub_8292BDC0: 6 -> 1.
- **Files:** ELF `out/nx/elf/sa2_round7b_20261007_0546.elf`, packaged at 05:46. The round-7a NRO (the diet only) is
  kept in `out/nx/r7a/` as a fallback.
- **Known limitation, from futari-nx:** with non_volatile_as_local, the generator syncs a caller's locals into `ctx`
  only around direct calls to a `share_registers` function.
  - SA2's one marked function, sub_82546D74, is also named in an exception scope table in `.rdata` (pointer at
    0x820F1678, among other scope addresses of sub_82546560).
  - So if a guest exception were ever dispatched through that table, the funclet would see a stale r31.
  - The console logs no guest SEH dispatch ("SEH 0" in every profile line). The risk class is the same as setjmp's.

#### The console profile, read again

- **The shared constant block** (`sa2_diag_shared_words`, a diagnostic): 99.6 % of draws change it.
  - Word 136, `g_VertexIndexBase`, changes on every draw.
  - Texture slot 0, sampler 0 and its 1/size change on about 65 %.
  - Holding the index base for vertex shaders that do not read r0 would save nothing: by `AluReadsR0`, every SA2
    vertex shader reads r0.
- **Per-thread budget at 31 FPS, 1020 MHz** (round 5, heaviest scene; ms of CPU per frame):

  | Thread | ms per frame |
  |---|---|
  | Main | 19.7 |
  | Ring | 18.1 |
  | Deferred worker | 8.6 |
  | XThreadF43C2C0 | 4.4 |
  | Others | about 5 |
  | **Total** | **about 56** |

  At 60 FPS the three cores give about 50 ms per frame, and main and ring must each stay under 16.7 ms. So the total
  matters as well as the two big threads.

### Step 2 (2026-10-07): `sa2_cpu_direct_flush`, Direct3D's state flush writes the draw's record

- **What:** inside a converted draw call, the three flush helpers of the Draw* functions write register runs straight
  into the call's direct record, and leave a 3-word placeholder NOP in the command buffer. The helpers are the shader
  constants (sub_82555CC8), the register groups (sub_82555930) and the fetch constants (sub_82555B70).
  - EndDrawDirect takes the ops at the placeholder's place.
  - When it makes no record, each placeholder becomes a step-1 marker with a record of its own. The ring is
    unchanged.
  - Full description: docs/direct-renderer.md, "Step 2".
- **Why:** the game thread no longer encodes those packets, and EndDrawDirect no longer decodes them. On the console,
  that pair was about a third of the D3D draw path (EndDrawDirect 5.3 % of the game thread, the flush helpers 3-4 %).
  The register values are still copied once, into the record instead of the command buffer.
- **Why exact:** the native path follows the original's fast path exactly, with the same runs, values and end packet.
  It runs only when every run of the original would have taken that fast path; the original runs otherwise.
  - The placeholder sits where the first packet would have started, so the stream order is the same.
  - Concurrency: the drawing thread holds `g_marking` from its first placeholder to the end of the call. Its own
    submissions inside the call publish the placeholders first.
- **Self-check:** the first 4,096 flushes (all in verify mode) also run the original on a scratch copy of the device
  (its own scratch command buffer) and compare word for word, plus the copy's dirty masks. The real device is never
  touched. A difference turns the key off for the session.
- **Checked (PC):**
  - Verify mode: 8.26 million flushes, each compared with the original: 0 differences (222 million checks in all).
  - Every path ran: 13,028 placeholders published alone, 6,580 of the game's own submissions inside a call, 3,799
    slow-path and 3,701 "submitted first" calls left to the original.
  - 0 "no room" and 0 "ring full", at about 316 record words per draw (55 before).
  - Lockstep: 2,998 of 2,998 overlapping frames identical to the GPU round's reference capture.
- **Risk:** a submission from a second drawing thread (the key then turns itself off), or a Direct3D version whose
  helpers differ (the self-check reports it).
- **A/B:** `sa2_test_direct_flush_off.toml`.

### The native Direct3D layer (2026-10-07): `sa2_api_draws`, `sa2_api_clears`

The first steps of removing the guest Direct3D, PM4 and EDRAM (the user's order of 2026-10-07; the design, the method
and the guide for the other ports are in docs/api-renderer.md).

- **What:** Direct3D entry points become native C++ that keeps the Direct3D device structure exactly as the game sees
  it and writes the renderer's records of operations instead of PM4:
  - `sa2_api_draws`: DrawVertices (sub_82565C60) with its draw-time state flush, shader load, vertex shader binding
    and patching, and the draw; BeginVertices / DrawVerticesUP (sub_82565758 / sub_82565C18); the window-scissor
    writer sub_8254FCE8 that SetViewport, SetScissorRect and the surface setters call.
  - `sa2_api_clears`: D3DDevice_Clear (sub_82563AB0 and the five functions under it): its state, its own clear
    shaders, the rect vertices, the fast path (the color buffer bound as depth) and the 4x-MSAA alias interior.
  - Each call's ops go to the ring's direct-record queue, named by a 3-word marker NOP while other Direct3D functions
    still write PM4. Calls in states SA2 never reaches (predicated tiling) run the original; the `[api]` line counts
    them (0 in SA2).
- **Why:** on the console the main thread is the bottleneck, and the recompiled Direct3D plus the hooks that decoded
  its packets were 20-25 % of it (round 8). The game thread no longer encodes the packets and the ring no longer
  decodes them for these calls.
- **Why exact:** transliterated instruction by instruction from the listings (helpers that mirror PowerPC's rotate
  and mask instructions; float conversions as the recompiled code does them).
- **Self-check (verify mode):** every call first runs the original on a copy of the device with a scratch command
  buffer, then the native code runs for real. The ops are compared with the original's packets in a canonical form,
  along with the device bytes, the shader regions the original writes and the inline vertex data.
- **Checked (PC):**
  - Verify mode: draws 2,150,579 records and Clear 9,649 calls (1,098,494 records), each compared with the original:
    0 differences.
  - Lockstep, full resolution (every pixel hashed, not the half-resolution video): draws 2,982 of 2,982 frames, and
    draws + Clear 2,998 of 2,998 frames, identical to the shipped defaults.
  - Production run: 714,556 native records in 90 s, no fallback.
- **A/B:** `sa2_test_api_off.toml` (both off).
- **ARM optimisation (round 10, 2026-10-07, from round 9's console profile):** the first version was correct on the
  console (verify mode: 322,267 records compared, 0 differences) but took about 18 % of the drawing thread in
  CPU-bound stretches at 1020 MHz, where the round-8 path took about 12 %. The causes and fixes are written up in
  docs/api-renderer.md, "ARM first", for every port:
  - a `uint32_t` op count that aliased the op words (about 11 instructions per word), now a pointer cursor;
  - word-at-a-time byte swaps, now NEON `rev32` copies;
  - `thread_local` reads, which are function calls on the Switch, one of them inside a per-word loop;
  - a 2.7 KB `PPCContext` copy per guest-helper call;
  - three copies of the inline vertex shader, now one memcpy in and one out;
  - records built and then copied, now written in place in the ring;
  - the publish's store-release waiting on write misses, now with the ring prefetched for writing.

  Checked on PC after each step: verify mode 196,163,069 checks with 3,290,604 records compared, then 203,892,646
  checks with 3,358,506 records compared, 0 differences both times; lockstep 2,998 / 2,998 and 2,999 / 2,999 frames
  identical at full resolution. The `[api] drawing-thread cost` line now reports the layer's own time (every 16th
  call timed): the console figure to compare with round 9's 18 %.
- **Round 10 on the console:** 1.0-1.5 µs per call in heavy scenes, and about 8-9 % of the drawing thread's samples
  at 1020 MHz (from about 18 %).
- **Round 11:**
  - `sa2_api_ring_zero_ahead` (key, default on): the ring of records is zeroed in the cache ahead of its head with
    DC ZVA (no DRAM read before the record's stores; each zeroed block is checked against the free space in verify
    mode).
  - The shader pair memo: the last VS/PS pair found finished skips NoteShaderObject's shared-table loads.
  - PC: verify mode 206,987,536 checks, 0 differences; lockstep 2,998 / 2,998 frames identical; 0.20 µs per call on
    the PC (0.27-0.30 in round 10).

### `non_argument_as_local` (2026-10-07, after round 8; build-time, in the optimised flavour)

- **What:** the last register class the optimised flavour still kept in `PPCContext`. r0, r2, r11, r12, f0 and
  v32-v63 (the volatile registers that carry no argument in the Xbox 360 ABI) become C++ locals of each recompiled
  function, as UnleashedRecomp's XenonRecomp config does. MSVC uses r11, r12 and f0 as scratch everywhere, so outside
  the localized copies every one of their uses was a load or a store of `ctx`:

  | `ctx` uses in the generated code | before | after |
  |---|---|---|
  | r11 | 632,469 | 9,464 |
  | r12 | 65,368 | 7,741 |
  | f0 | 87,592 | 10 |
  | r0 / r2 | 1,398 / 1 | 8 / 0 |
  | v32-v63 | 9,708 | 0 |

  What is left is in the functions that keep these registers in `ctx` (below) and in the hand-overs around calls to
  them. The PC executable shrank by 1.7 MB (40,393,728 -> 38,659,584 bytes).
- **Why it was left out:** "callees take arguments in r0, r11 and r12". With the option a value can no longer cross a
  call in these registers, so every place where one does had to be found. `tools/non_argument_audit.py` (whole program,
  on code generated without the option) lists them:
  - **Arguments read on entry (ENTRY), all in r12:**
    - the SDK's `__save*` / `__rest*` helpers (218 in the game, 18 in the stage DLL). Their calls are elided under
      `non_volatile_as_local`; their bodies now keep these registers in `ctx` anyway (generator rule, by name);
    - the MSVC EH/SEH funclets (535 + 8), `share_registers` already (the generator marks them from the EH tables).
      Their owners call them inline (`addi r12,r31,N; bl funclet`) through `emit_call_sharing_registers`, which hands
      r12 over with the other localized registers and takes the results back;
    - the stack probe sub_827F5144 (frame size in r12; `li r12,-N; bl; stwux r1,r1,r12` in its 3 callers).
  - **Values passed through an indirect call:** the C++ EH dispatch, sub_82801D90 (`_CallSettingFrame`: r12 = frame,
    then `blrl` to the funclet), and the continuation jumps sub_827EFE34 / sub_88010BE4 (stage DLL) (`mr r12,r5;
    bctr`). Guest exceptions are never dispatched in this port (RtlUnwind and `__C_specific_handler` are stubs), so
    this path does not run; it is kept exact anyway.
  - **Values returned in these registers (RET):** one. sub_82805400's `__finally` funclet sub_828054C8 reloads r11
    for its owner, and the owner's copy-back after the funclet call takes it. sub_8275B300's `subfe r10,r11,r11` after
    a call does not depend on r11. Every other read after a call is a value the callee never writes (MSVC's
    interprocedural register allocation: 46 functions keep f0 across a call, 3 keep r12 across the stack probe), and a
    C++ local survives the call the same way.
  - **Three audio resamplers** (sub_828A0DB0 / sub_828A2A60 / sub_828A3220: 32-, 16- and 8-bit samples, linear
    interpolation) read v57-v63 before writing them. They are the unaligned-load shift registers of a
    software-pipelined loop: each `vsldoi128 vX,vX,vNew,4` (or `,2`) pushes the old bytes out. `tools/vec_shiftout.py`
    follows every byte from the entry values and proves none reaches a store (sub_828A2A60's inner loop runs exactly
    twice: 2 x 4 shifts of 2 bytes). Zero-initialized locals give the same results.
  - **Nothing else:** no hook, native or SDK runtime code reads or writes `ctx.r0/r2/r11/r12/f0/v32-v63`. The
    localized copies are made from the generated code and keep the same locals. No mid-asm hooks. The 6 setjmp callers
    write these registers again before reading them after `ppc_setjmp`.
- **The generator (sa2-nx's own option `non_argument_as_local_keep`):** functions listed there keep r0/r2/r11/r12/f0/
  v32-v63 in `ctx` and localize everything else; so do the save/restore helpers, by name. A function that localizes
  them hands its values over around a call to one of them (`emit_call_handing_non_arguments`: copied in before the
  call, taken back after a `bl`, `ctx` restored as for `share_registers`). The list in `app/perf_overrides.toml`:
  sub_827F5144 (stack probe), sub_82801D90 (`_CallSettingFrame`), sub_827EFE34 and sub_88010BE4 (continuation
  jumps). With the option off the generator's output is byte-identical to before.
- **Checks on the generated code (with the option, both modules):** `tools/read_before_write.py --volatile` reports
  only the three resamplers; `tools/non_argument_audit.py --with-option` finds no missing hand-over (every call to a
  function that keeps the registers in `ctx` copies in what it reads on entry), no value returned without a
  copy-back, and no localized register read before written except the resamplers. (Checked the other way too: with
  one hand-over or one copy-back deleted from the generated code, the audit reports it.)
- **Who keeps them in `ctx`** (the game / the stage DLL): 27,624 / 64 functions localize them; 566 / 12
  `share_registers` functions keep everything in `ctx` as before; the 236 / 36 save/restore helpers and the 3 / 1
  `non_argument_as_local_keep` functions keep these registers in `ctx` and localize the rest; 2,574 / 14 touch none.
- **Checked (2026-10-07, built in an isolated copy of the tree, both builds from the same sources):**
  - verify mode: 182,776,283 checks, 0 differences, no DIFFERENCE/MISMATCH line;
  - exact lockstep capture against the same tree built without the option (shipped defaults, 3,000 frames from game
    frame 6000): 2,991 of 3,000 frames have a pixel-identical frame in the reference; the other 9 lie past the end of
    the reference's window. The attract demo is in sync: 2,594 consecutive frames at one constant offset of the
    presented-swap labels (2,585 of them with content found only once in the reference), as are the 343 frames before
    the loading screen; only the loading screen in between presents a different number of swaps, as between any two
    runs (the round-8 reference against this tree's own build without the option shows the same pattern);
  - the verify run's capture: every frame inside the round-8 verify capture's window is identical (929 + 1,383 frames
    in two in-sync stretches; the frames before them are the intro, before that capture's window);
  - Switch: `tools/check_switch_build.py` passed every check. NRO 54,683,845 -> 53,315,781 bytes. In the recompiled
    game functions of the ELF (against the round-8 ELF): 6,048,784 -> 5,708,335 instructions (-5.6 %) and 2,330,442 ->
    2,124,479 loads/stores (-8.8 %); the rest of the program is unchanged;
  - PC CPU: not measurable. Over the same game frames (6000-9000, 50 s at 60 fps, three alternating runs each) the
    process used 70.3 / 72.7 / 78.0 CPU-seconds without the option and 72.8 / 73.0 / 69.8 with it, the game thread
    12.4 / 14.1 / 10.8 and 14.3 / 13.4 / 11.6: the run-to-run spread (about 10 %) is larger than any effect. The PC
    ring thread is busy 96 % of the time either way. The console decides.

### `sa2_cpu_verify`

This is a check mode, not an enhancement. It checks every native call and sets `sa2_native_verify_n` and
`sa2_native_fast_copy_verify` to "every use". The renderer's DIFFERENCE/MISMATCH reports count into the
`[verify]` line.

## Non-CPU group

### `sa2_gpu_boost`

In handheld mode it asks for the official 0x92220008 configuration (GPU 460.8 MHz, memory 1331.2 MHz) through the
SDK's `switch_apm.cpp`. That code checks the real clocks for 1.5 s and reverts if the memory clock moved. Docked, it
requests nothing.

- **Why exact:** it changes a clock, not a setting.
- **Risk:** more power draw in handheld mode.

### Renderer GPU optimisations

The keys are `sa2_native_edram4_clear_color0`, `sa2_native_constants_dedupe`, `sa2_native_pass_narrow_end` and
`sa2_native_zcull_1x_depth`; see `docs/gpu-optimisation-candidates.md`. They are plain cvars of the renderer, and the
framework sets them back to the previous path when `sa2_noncpu_enhancements = false`.

`sa2_native_zcull_1x_depth` stays off by default. Unleashed lost the GPU after 15 to 20 minutes with depth-clear and
ZCULL changes, so it needs a 30-minute play test before it can be turned on.

### GPU round (2026-10-07): what the Switch GPU spends on resolves and blending

On the console, resolve copies took up to 2.5 ms of a 9 ms GPU frame (the scene 3.6-5.9 ms). SA2 resolves 8 times
a frame: two 512x512 offscreen renders, each to its texture (1E6A0000, 1E1AA000), a duplicate to 1E8A0000 and its
depth to 1E7A0000; then the frame to 1EFB0000 and to 1F6F8000, which is what the Swap presents. The PC's
per-category GPU timestamps and the resolve log (`sa2_diag_resolves_from`, `sa2_diag_alias_frames`) show it.

#### `sa2_gpu_resolve_draw`: resolves drawn on the 3D engine

- **What:** a same-format RGBA8 color resolve is one fragment pass (`shaders/sa2_resolve_copy.frag`: texelFetch of
  source texel (x, y) + delta) instead of `vkCmdCopyImage`.
- **Why:** NVK records `vkCmdCopyImage` on the copy engine (NV90B5), with a wait for idle at each engine switch.
  SA2 switched about 15 times a frame.
- **Why exact:** UNORM8 values round-trip exactly through float on NVIDIA/AMD/NVK, as in the EDRAM color
  conversions. The render pass's dependencies order it as the copy's barriers did.
- **Checked (PC, lockstep):** 2,999 of 2,999 overlapping frames identical; about 6,000 resolves drawn per 10 s, 0
  declined.

#### `sa2_gpu_blend_skip`: transparent pixels discarded before the blend (UnleashedRecomp-NX's idea)

- **What:** a shader specialization (`SPEC_CONSTANT_BLEND_SKIP_ALPHA` / `_ZERO`, XenosRecomp) discards a pixel whose
  blend would leave the target as it was. That saves the blend's read-modify-write.
- **Why exact:** only on an RGBA8 UNORM target without exponent bias (the blend clamps to [0, 1], so a source of 0 or
  less contributes exactly 0, and destination x 1 is exact). Also required: no alpha to coverage, no depth write, no
  stencil write of any kind, op ADD, and a destination factor that is exactly 1 once the source is 0. A NaN output is
  never discarded. SA2's occlusion queries are answered on the CPU, so no host query counts these pixels.
- **Checked (PC, lockstep):** 2,997 of 2,997 overlapping frames identical; about 38,000-50,000 specialised draws per
  10 s.

#### `sa2_gpu_resolve_alias`: the duplicate resolves are not copied

- **What:** a color resolve that repeats the previous one (same render target image and rectangle) into another
  texture is not copied. That texture becomes an alias of the first, which holds the same bytes.
  - It gets them only when something reads it (a draw, the Swap, a restore), or before the first texture changes:
    a copy, or an exchange of the two images when the first one is overwritten whole.
  - When a resolve covers it first, nothing is copied at all.
- **SA2:** both 1E8A0000 duplicates are dropped every frame (two 512x512 copies saved). 1F6F8000 is the presented
  image, so its alias is copied at every Swap: no saving there, but no cost either.
- **Why exact:** the previous resolve copied the same rectangle of the same render target image, in the command right
  before this one. Nothing in between may change the target:
  - no draw (`Drawn()`), no EDRAM import, no restore, no clear in either command;
  - and EDRAM mode 4's tile ownership version (`edram4_version_`) unchanged.

  The source texture still has the revision it left. Every write of a resolved texture calls `BeforeResolvedWrite`
  first, and every read materializes the alias first (`ResolvedTexture`, `ResolvedTextureForFetch`, `Present`,
  `RestoreContent`). A new alias invalidates the draws' cached bindings of its image.
- **The bug the lockstep capture found:** at the demo's scene change, the game clears its frame between the two
  resolves (the fade).
  - Direct3D's Clear is a draw that mode 4 redirects into an EDRAM clear (`RedirectClearDepthEDRAM4`, which also clears
    the color alias tiles), and `Drawn()` does not count it.
  - The first version made an alias there and presented the frame from before the clear: one frame, half of it an old
    city view instead of black.
  - The EDRAM version check fixed it. The alias is therefore enabled in mode 4 only, where every write of a render
    target moves that version.
- **GUARD:** an alias whose source no longer holds its bytes (image or revision changed) is reported as DIFFERENCE
  and the key turns off. This is checked at every materialization and every drop.
- **Test of the copy on read:** `sa2_diag_present_alias` makes the Swap present the alias, so it is copied every
  frame. Its frames must be identical.
- **Checked (PC, lockstep):** 2,998 of 2,998 overlapping frames identical, the scene change included. Per 10 s: about
  2,500 aliases made, 1,900-2,500 dropped unread, about 600 copied at the Swap.

#### Planned and not done, with the reason

- **Narrower barriers / one barrier per resolve group:** on NVK every barrier with a source stage waits for idle
  whatever its masks, and the consecutive resolves already run on an idle pipe. Removing the duplicate resolves
  (above) removes their barriers too.
- **Alpha-test early-out:** SA2's pixel shaders are a few instructions long. A discard placed before an
  implicit-derivative texture sample is not exact either.
- **Trimming unused vertex-shader outputs:** the Switch driver already does it. The NFSMW/Unleashed Mesa
  (`NVK_LINK_VARYINGS`, on by default) drops the outputs a linked pipeline's fragment shader never reads, and SA2's
  pipelines are monolithic. PC drivers do the same.
- **Skipping an identity gamma ramp:** SA2's ramp is not the identity ([64] = 193, [128] = 462 in 10 bits), so the
  pass must run.

### `sa2_native_linear_fetch_not_resolved`: a linear fetch is never a resolve (Wild Canyon fix)

- **The bug:** the round-7 console log had 138 draws rejected with cause 109 at Wild Canyon, after the Knuckles event.
  An agent reproduced it on PC through the story route (Stage Select does not reproduce it):
  - The draw is the 8x8 centre of the title card's "1st Mission" box: an ordinary linear CPU texture.
  - The CPU loads it where a 512x512 character shadow map had been resolved (and then freed).
  - The renderer's `resolved_` map still held the shadow map under that address. The fetch was taken for it, failed
    the crop contract and was dropped. The box lost its red fill for about 2.3 s.
- **The fix:** Xenos resolves write tiled layouts only, so a linear fetch (fetch word 0, bit 31 clear) skips the
  resolved-texture lookup and takes the ordinary texture path from guest memory, as the 8 pieces around it always
  did. This is a fidelity fix: it changes only the draws that were rejected.
- **Risk:** a game that resolves and then reads the same memory with a linear fetch would see guest memory, which is
  what the hardware shows too. Of the 60 distinct textures the agent traced, all 56 linear ones were CPU textures and
  all 4 tiled ones were resolves.

### GPU round 2 (2026-10-07): per-draw constant traffic and per-vertex/pixel tests, from round 8's console log

Six keys (all `kNonCpu`, default on; `REX_SA2_GPU_<KEY>=false` turns one off). Four need the shader package of this
build: `sa2_gpu_push_once` works with any package, and the others only take effect with the new one, whose
translator emits the new variants.

- **The evidence (round 8, 1020 MHz):** 1,241,607 root-table uploads per 10 s for 614,144 draws, about 2.0 per draw.
  On Maxwell each one is a constant-buffer load into the root table. In addition, 99.4 % of draws changed the shared
  constant block, 28-34 % of them only through the vertex index base (PC diagnostic `sa2_diag_shared_words`).
- **`sa2_gpu_push_once`:** the three push-constant addresses are recorded once per recorded state (pass start, new
  command buffer, after a utility pass) instead of on every draw whose constant offsets moved.
  - Exact because every pipeline specialized with constants in set 4 never reads them: the push-constant branch of
    `shader_common.h` is dead after specialization. This was checked in all 44 translated shaders.
  - Verify mode reports any skipped push for a pipeline without set-4 constants.
  - Expected on the console: about 1 root upload per draw instead of 2, about 13 command words per draw instead of 21.
- **`sa2_gpu_index_base_held`:** the translator proves that 19 of SA2's 22 vertex shaders never read the vertex
  index r0 starts with. Their only read is the Xenos zero idiom `-|r0| > 0`, false for any value, NaN included
  (`vertexIndexMayBeRead` in `shader_recompiler.cpp`).
  - With `SPEC_CONSTANT_VERTEX_INDEX_UNUSED` their r0 starts at 0, and the renderer holds `g_VertexIndexBase` at 0
    (not for rect lists). Consecutive draws with the same textures then keep the same shared block, its offset and
    the set-4 binding.
  - PC: set-4 binds 1.00 -> 0.70-0.76 per draw in play. This was the one key with a PC GPU gain of its own (-4.9 %).
- **`sa2_gpu_structured_cf`:** for shaders with forward-only control flow, the translator also emits a nested
  if/else form under `SPEC_CONSTANT_STRUCTURED_CF`.
  - It does so only after an exhaustive proof: for every combination of jump and conditional-return outcomes (up to
    20 decisions), both forms run the same CF instructions in the same order.
  - It replaces the `if (skipTo <= pc)` test per CF instruction, which NIR cannot fold after two jumps. SA2's
    lighting vertex shaders had 72 such tests each.
  - Coverage: 19 of the 44 shaders, every one that used the skipTo form.
  - Static count (spirv-opt): vertex shaders -267 conditional branches and -3,062 instructions; pixel shaders -68
    and -583.
- **`sa2_gpu_texcoords_unswapped`, `sa2_gpu_no_texture_signs`, `sa2_gpu_identity_remap`:** three per-pipeline facts
  the renderer already guarantees, passed as specialization bits so the shaders stop testing them per vertex or per
  pixel:
  - `g_SwappedTexcoords` is always 0;
  - no texture of the draw has a signed, biased or gamma component;
  - every vertex input keeps its component order (`g_InputRemap` 0xFFF).

  Each bit replaces exactly the early return its test would take. Each key has a verify-mode check on the shared
  block it relies on.
- **Static count with all six:** vertex shaders -44 % instructions, pixel shaders -64 %.
- **Checked (PC, NVIDIA, D24S8 depth like the console; lockstep at full resolution):**
  - Each key alone, against all six off: 2,992/2,992, 2,990/2,990, 2,991/2,991, 2,999/2,999, 3,000/3,000 and
    2,999/2,999 frames identical.
  - All six on: 2,997/2,997 frames identical.
  - Verify mode: 194,877,739 checks, 0 differences.
  - PC GPU time: scene -15.6 % (clock-corrected).
- **Console:** watch the "C6 NVK per draw", "C6 set 4" and "GPU per Swap" lines. After a new shader package the new
  pipeline variants compile once on the ring (68-159 ms each) until the prewarm list has them.
- **Dropped:** `sa2_gpu_fetch_offset_zero` (identical, but NVK already removes the `texCoord + 0 * invSize` it
  targeted).
- **A/B:** `sa2_test_gpu_round2_off.toml` (same package, all six off).
- **Next candidates (estimated console saving per Swap):**
  - merge consecutive draws with identical state (0.5-1.5 ms);
  - present straight from the alias source instead of copying into 1F6F8000 at every Swap (about 0.4 ms);
  - skip the unread 512x512 resolve to 1E866000 (1,423 copies per 10 s, never read; about 0.28 ms);
  - ZCULL on the scene depth (`sa2_native_zcull_1x_depth`, needs a 30-minute play test).

### `log_file_writer`: the file log from a thread of its own (2026-10-07, from futari-nx; SDK cvar, no master key)

- **What:** the logging thread only formats the line and appends it to a memory buffer: a copy under the sink's mutex,
  never any I/O. A "Log writer" thread (priority 0x3B on the Switch, the guests' band) writes the whole batch with one
  `fwrite` (64 KB `setvbuf`) once a second, or sooner at 64 KB or on a warning.
  - It never blocks the caller: past `log_file_writer_buffer_kb` (4 MB) lines are dropped and counted, and the count
    is logged once the writer catches up.
  - `FlushLogging` (crash handlers, exit) still writes everything before it returns.
  - A Switch fatal error appends the lines not yet written to `rex_crash.log`.
  - Rotation works like spdlog's.
  - Code: `sdk/src/core/logging.cpp` (`WriterFileSink`) and `sdk/src/core/exception_handler_switch.cpp`.
- **Why:** newlib's FILE buffer is 1 KB, so every long log line was an SD write of its own, made by whichever thread
  logged it. With music streaming from the same SD card, one write took 50-150 ms.
  - Futari found its worst console stutters (a 1,022 ms frame) inside the ring thread's 10-second report.
  - SA2's round-8 console profile shows the same: in 19 of its 54 stutter intervals (frames over 45 ms) the ring
    thread is inside `fsdev_write` (spdlog -> fwrite -> `__sflush_r`), and the game thread waits for the ring in
    sub_8254F400.
- **Image and game:** unchanged (logging only).
- **A/B:** `sa2_test_log_writer_off.toml` (the synchronous rotating file sink as before).

### Round 9 build (2026-10-07): the combination, checked together

The console build of 2026-10-07 13:26 includes everything above that is new today: `non_argument_as_local`, the native
Direct3D layer, GPU round 2 with its shader package, and `log_file_writer`, all on. Its ELF is
`out/nx/elf/sa2_round9_20261007_1326.elf`.

- **Shipped defaults (lockstep, full resolution) against the reference from before these changes:** every frame
  inside the reference's window is identical at one constant label offset. The one exception is a 25-frame looping
  loading-screen animation: the same frames, but its phase depends on loading time, as it does between any two runs
  of one build.
- **Verify mode:** 204,071,230 checks, 0 differences, no DIFFERENCE / MISMATCH line. Its capture matches the
  reference over the 1,982 frames where the two windows overlap, with the same exception.
- **The new keys all ran, with no fallback:**
  - native records: 9,956,167, including 66,305 clears;
  - GPU round 2: each of the six keys 8.7-9.3 million uses per run.
- **Switch:** `tools/check_switch_build.py` passed every check. NRO 53,463,237 bytes.

### Render resolution and MSAA (2026-10-07): `sa2_gpu_render_scale`, `sa2_gpu_msaa`, `sa2_gpu_prewarm_current_keys`

The user's request: 720p handheld and 1080p docked, set in the settings file, changing live when the Switch is docked
or undocked (simulated on the PC), plus an MSAA option, everything rendering correctly. Unlike the enhancements above
this changes the image on purpose, so it has plain settings as well as keys:

| Setting (`sa2.toml`) | Default | What it does |
|---|---|---|
| `sa2_render_height_handheld` | 720 | the height drawn in handheld mode (the PC's starting mode); 720 = the game's 1280x720, exactly as before |
| `sa2_render_height_docked` | 1080 | the height drawn docked: 1080 = 1920x1080 |
| `sa2_msaa` | 0 | 0 = off, as before; 2 or 4 = 2x / 4x MSAA of the game's single-sample render targets (read at start) |
| `sa2_render_docked` | false | PC only: start in the simulated docked mode |
| `bind_sa2_docked` | F9 | PC only: switch between docked and handheld while the game runs |
| `sa2_render_test_toggle_at`, `_every` | 0 | PC tests: toggle the simulated mode after that presented swap, then every N swaps |

Heights are multiples of 180 from 360 to 2160 (others go to the nearest one): the scale is q / 4 with q = height /
180, and every EDRAM tile size is then a whole number of host pixels (below). The width follows at 16:9.

#### `sa2_gpu_render_scale`: the render resolution

- **What:** every render target, depth target and resolved texture (the 1280x720 scene, the 512x512 offscreen
  renders, the front buffer the Swap presents) is a VkImage q / 4 times the guest size in each axis; the presenter
  gets the larger guest output and scales it to the display as before.
  - The renderer keeps every guest size in guest units (`ImageNative::width` / `height`), so the guest logic does not
    change: EDRAM tiles and their ownership, resolve rectangles, the texture behind an address and its size, the 1/size
    of the texture constants (a texel offset stays a guest texel). Only the Vulkan commands use host pixels, converted
    in one place (`me_resolution_scale.h`): extents, render areas, copies, blits, clears, dispatches, push constants.
  - A guest pixel edge e is host edge ceil(e * q / 4 - 1/2), the first host pixel whose centre is at or after it:
    neighbouring rectangles (split-screen halves, scissors, tiles) never overlap and never leave a gap.
  - The EDRAM becomes a "virtual EDRAM" q / 4 times larger: an 80x16-sample tile is 20q x 4q host samples, so every
    view of the same tiles (1x, Direct3D's 4x Clear aliases, depth and color) keeps its exact relation to the others.
    The transfer shaders (`shaders/me_edram_*`) take the tile size as a push constant instead of 80 / 16 / 40.
  - Draws: the viewport is multiplied by q / 4; scissors go through the edge rule; the D3D half-pixel offset is one
    GUEST half pixel; guest lines are q / 4 pixels wide (`wideLines`, enabled by the SDK when the device has it), so
    they keep their thickness relative to the image. Points stay one pixel: the game's point sprites (the sparks of
    the boards and shoes, VS n2 / PS n39, up to about 120 per draw in the demo; `PA_SU_POINT_SIZE` 0x00180018, 3x3
    pixels on the Xbox 360) have always been 1-pixel points in this renderer, which has no point-sprite expansion
    and whose shaders write no point size. At 1080p they are therefore two thirds of their 720p size relative to the
    picture. Point-sprite expansion would fix both, but it changes the 720p image: a separate change with its own key.
  - A guest read-back of a resolved texture (none in SA2) is filtered down to the guest size first.
- **Live switching:** at the end of each Swap the renderer asks for the wanted scale (`sa2_render_mode.cpp`); when it
  changed, `RescaleAll` drains the GPU, records the deferred front copies and resolve aliases at the old scale, makes a
  new VkImage at the new scale for every render target and resolved texture (keeping the `ImageNative`, so every
  pointer, EDRAM tile owner and memo stays valid), scales the content into it (color linear, depth nearest), drains
  again and frees the old images. An image that cannot be scaled starts cleared instead (color 0, depth at the far
  plane), which SA2 does to its targets at the start of every frame anyway: the scene's ZCULL depth (no copies into
  it), depth when the driver cannot blit it, and multisampled images (`sa2_msaa`). One longer frame (19-29 ms on the
  PC for 14-18 images), only when the mode changes.
  - **Switch:** the mode is libnx's cached operation mode, which the SDK's applet loop keeps current (the
    OperationModeChanged message), with Reverse-NX's mode obeyed through `switch_saltynx::BaseMode`, the same rule as
    the window. The window and the swapchain follow the display as before (1920x1080 docked, 1280x720 handheld); the
    guest output follows the render height a frame later.
  - **PC:** `sa2_render_docked` and the F9 key (`bind_sa2_docked`) simulate it; `sa2_render_test_toggle_at` /
    `_every` toggle it at presented swaps for unattended tests.
- **Why exact at 720p:** q = 4 makes every conversion the identity (the same integers and floats go into the same
  commands), the transfer shaders get 80 and 16, the half-pixel offset multiplies by exactly 1, no line pipeline
  changes, and no rescale ever runs.
- **Off:** `sa2_gpu_render_scale = false` (or the non-CPU master off) draws 1280x720 in both modes, as before.

#### `sa2_gpu_msaa`: MSAA

- **What the game does:** SA2 draws its scene and its offscreen renders single-sample. Its only multisampled surfaces
  are Direct3D's fast-Clear aliases: a 4x view of the same EDRAM tiles drawn with a quarter of the pixels. The
  renderer keeps those as before (one host sample per pixel), and mode 4 redirects nearly all of those clears into the
  1x owners. So "the game's MSAA" has nothing to anti-alias, and `sa2_msaa` adds anti-aliasing the Xbox 360 version
  does not have.
- **What:** with `sa2_msaa = 2` or `4`, the game's single-sample RGBA8 color targets and its depth targets get that
  many host samples (`sa2_gpu_msaa` on, and the device supporting the count for color, depth/stencil and sampled
  depth). Passes and pipelines carry the sample count (pipeline key `fill`, 0 as before without MSAA).
  - Resolves: color with `vkCmdResolveImage` (the samples averaged: the anti-aliasing); depth with a fragment pass
    writing sample 0 of each pixel (`shaders/sa2_depth_resolve_ms.frag`), the guest's only sample.
  - EDRAM: redirected clears clear every sample; a transfer from a single-sample view (a 4x Clear alias) into a
    multisampled target is a fragment pass of the same sample count (each pixel's samples get its value); a transfer
    out of a multisampled target is declined and skipped like any unsupported transfer (none seen in SA2).
  - 0 (default): no multisampled image exists, no new path runs.
- **Values:** 0 = off (as before, default); 2 = 2x; 4 = 4x. Alpha-tested edges (foliage) stay as they are: the
  renderer turns the game's alpha-to-mask into an alpha test, with or without MSAA.

#### `sa2_gpu_prewarm_current_keys`: the pipeline list prewarms current keys

- **What:** the prewarm list (`cache/sa2_native_pipelines.bin`: a record of every pipeline the ring created,
  compiled again by the prewarm thread at each start) skips the records whose key no draw of this run can produce:
  another `sa2_msaa` (pipeline key `fill` bits 0-7), a line pipeline of a render scale neither console mode uses
  (bits 8-15), or the other state of `sa2_gpu_texcoords_unswapped` (specialization bit 5, which GPU round 2 put on
  every pipeline). They are skipped like the records of another dynamic-state mode: kept in the list, not compiled.
- **Why:** those records were compiled at every start for nothing, while the keys really used still compiled on first
  use. Bumping the list's version instead would have discarded the whole list, current records included, although
  the default settings keep every key as it was.
- **Round 12's version dropped them** from the list. The console's first round-12 session (`sa2_msaa = 2`) dropped all
  241 records, and the next one, MSAA off again, found 2: every pipeline was created on first use (mostly from the
  Vulkan cache, 0.1 ms each, but the list was gone). Since 2026-10-07 evening they stay in the list, so switching a
  setting back (the MSAA A/B configs) finds its records again.
- **Why exact:** the prewarm only compiles ahead of time; the ring creates the same pipelines with or without the list.
- **Off:** `sa2_gpu_prewarm_current_keys = false` prewarms every record, as before.

#### Checked (PC)

Windows 10, the AMD Radeon integrated GPU (the Vulkan device the runs use); lockstep captures of the attract demo
(title, loading, City Escape, the 2P race in split screen) and `work/res` tools in the sa2-nx-gpu tree.

- **720p defaults are today's frames:** 3,000 lockstep frames from game frame 6000, this build (merged with main of
  2026-10-07 16:13) against the build without the change, compared at full resolution: 2,998 of 2,998 overlapping
  frames identical (`rs_base` / `rs_def2`, one label offset). Two more default runs sampled one frame in 60 over the
  same stretch (`sa2_video_lockstep_every`, full resolution): 50 of 50 and 49 of 49 identical.
- **Live switching:** the 3,000 frames with the simulated mode toggled at presented swaps 6600, 7200, 7800 and 8400:
  every 720p frame is identical to the reference, the first one after each switch back included (1,718 frames); the
  1080p stretches show the same scenes. Each switch recreated 14-18 images in 19-29 ms. Rapid switching
  (every 10 swaps, 28 and 37 switches, with and without 4x MSAA): no black, stale or torn frame (frame-to-frame
  differences only spike at the demo's own camera cuts).
- **1080p:** the 3,000 frames docked: every frame shows the 720p one's scene (mean difference 3.2 / 255 after a 3x3
  blur, from sharper edges and texture detail, plus the wall-clock "Press START"). Full-resolution frames: HUD, ring
  counter, "Press START" / "START!" text, lens flare, the split-screen seam, the loading screens and the title screen
  (with a switch every 500 swaps) are right. The menus and the pause screen were not reached: the scripted pad input
  of the unattended PC runs did not get past the title, so they are on the console list.
- **Other heights:** 360, 540, 900, 1440 and 2160 (q = 2, 3, 5, 8, 12: odd q tests the edge rule): the same picture,
  no seam between the split-screen halves, the HUD in place.
- **MSAA:** 2x and 4x at 720p and 1080p, 3,000 frames at 720p 4x: smoother geometry edges (characters, the road
  lines; mean difference 0.30 / 255 from 720p, all at edges), the rest unchanged.
- **Every run:** 0 rejected draws, 0 failed draws or copies, 0 skipped EDRAM transfers (no transfer out of a
  multisampled target happened in any of them).
- **GPU time per frame** (busy GPU time per Swap on this iGPU, not the Switch; 170 s runs without lockstep, so about
  10 % apart run to run; the heaviest 40 s of the demo / a lighter 50 s): 720p 3.0 / 2.0 ms; 1080p 7.6 / 4.5 ms; 720p
  2x 3.7 / 2.7 ms; 720p 4x 6.0 / 4.3 ms; 1080p 2x 7.2 / 5.3 ms; 1080p 4x 11.4 / 8.9 ms. So 1080p costs about 2.3-2.5
  times 720p's GPU time (2.25 times the pixels; resolves and copies scale too) and 4x MSAA about 2 times.
- **Memory of the scaled images** (render targets, depth targets, resolved textures; `[res] images` lines): 27-41 MB
  at 720p, 60-90 MB at 1080p, 43 / 96 MB with 2x, 71 / 159 MB with 4x (720p / 1080p).
- **CPU:** at 720p every conversion returns at its first test (q = 4); otherwise a few integer operations per pass,
  scissor and copy. The mode is read once per Swap (libnx's cached value on the Switch).
- **Pipelines:** at the default settings every key is the one it was; `sa2_gpu_prewarm_current_keys` dropped 613 of
  909 records of this PC's list (records of earlier MSAA runs and from before GPU round 2) on the first start.
- **Console:** MSAA works on the Switch (the user's test, 2026-10-07).
- **Limitations:** the game's point sprites stay one pixel (above); `sa2_msaa`
  needs a restart; at a mode change the multisampled images start cleared (one frame).

### Round 13 (2026-10-07): relaxed vsync and the 2P race's vertex work

From the round-12 console log (`logs 11 cpu on`), 2P race at 1020 MHz: 31-37 fps. Per frame the game thread needed
~17.7 ms and the GPU ring thread ~17 ms (the split screen draws the scene twice: 1,060-1,650 draws), each just over a
refresh, and the fixed vsync then held every late frame to the next vblank: 30 fps, the game at half speed. About 70 %
of the game thread is the game's own drawing code (Ninja chunk models, matrices, Direct3D), not gameplay. The same log
showed no CPU regression against round 10 at equal clocks (the console changed clocks during both sessions: 1224, 1326,
1683 and 1785 MHz stretches; compare `clocks:` in `rex_profile.log`).

#### `sa2_vsync_relaxed` (non-CPU group, on): no fall from 60 straight to 30

- **How the game waits:** after each swap the ring meets a WAIT_REG_MEM on a flag in memory (PC: physical 1FCA3004,
  `== 0`). Direct3D queued a flip entry with a due vblank number (the current one + 1); the game's vblank interrupt
  callback (`sub_8254D420` -> `sub_82557FC0`) counts the vblank, takes the due entries and clears the flag. With the
  emulated display's fixed 1/60 s vblanks, a frame that took 17-33 ms waited for the next one: 30 fps.
- **What:** the vblank comes when the flip waits for it, like a variable-refresh display: the ring asks for it when
  the flip wait is not met, and gets it at once, but never sooner than 1/60 s after the previous one, so 60 stays the
  cap (on time, the vblanks stay on the 1/60 s grid). Nobody asking for 50 ms (loading, frames under 20 fps): the
  regular ticks until the next request.
- **Which wait is the flip** is learned at start: the memory a wait polls that keeps ending right after a vblank that
  came during it (8 times; logged as `[native] sa2_vsync_relaxed: the flip wait polls 1FCA3006`). Until then the
  regular ticks run.
- **What failed first** (PC, with a simulated slow ring): a vblank at the game's VdSwap call (13-24 fps: used up
  before the ring reached its wait); at the ring's XE_SWAP (13 fps: before the flip entry was due); for any unmet wait
  (29 fps: a mid-frame wait on the game thread took the vblank, then the flip waited a full refresh); a second request
  from the same flip wait while the first vblank's callback was still releasing it (the next flip waited a full
  refresh); vblank times taken from the thread's wake-ups (57.5 fps instead of 60: the wake-up latency added to every
  frame).
- **Measured (PC, `diag_vdswap_delay_us` / `sa2_diag_ring_swap_delay_us` simulate a slower game thread / ring):**

| Simulated load | fixed vsync | relaxed vsync |
|---|---|---|
| none | 60 fps | 60 fps (600-601 Swaps per 10 s) |
| ring 18 ms per frame | 30 fps (ring waits 15 ms per Swap) | 54.6 fps (waits 0.01 ms) |
| ring 12 ms per frame | 60 fps | 60 fps (600 Swaps per 10 s) |
| ring 18 ms per frame (19 with the real work) | 30 fps | 52.6-54.6 fps |
| game thread 20 ms per frame | 45-49 fps | the same (the game thread was the limit, not the vsync) |
| both 15 ms, the demo's 2P race | 34-42 fps | 35-44 fps (the game thread is the limit there) |

- **The game's speed:** it still runs per frame. At 50 fps it runs at 83 % speed (at 30: 50 %). Nothing is decoupled.
- **Off:** `sa2_vsync_relaxed = false` (`configs/sa2_test_vsync_relaxed_off.toml`): the fixed 1/60 s vblanks.
- The lockstep pixel tests use `vblank_per_swap` (one vblank per swap, the same rule without the fallback), so they
  are unaffected.

#### Hand-written vertex loaders, njCalcPoint and njMultiMatrix (CPU group, on)

- `sa2_cpu_native_vertex_normals` (`sub_8214CF10`) and `sa2_cpu_native_vertex_points` (`sub_8214D0F8`), in
  `src/cpu/hot/n_vertex_chunks.h`: the Ninja chunk-model vertex loaders, with njCalcPoint inline and the matrix read
  once per call. Chunk types 41 (vertex + normal), 42 (+ a float converted with fctidz), 44 (skinned: the transformed
  vertex scaled by its weight and stored or added into a record: the work of `sub_8214EAF0` / `sub_8214ED18`) and 35
  (+ a diffuse word: `sub_8214E9B8`). Declined (the original runs): skinned chunks without the transform flag (the
  originals then scale values left on their stack), writes that could overlap the matrix, the flags word, the weight
  constant or the chunk list. In the 2P race their njCalcPoint calls were the game thread's largest single cost
  (8.5 %), each through the hook and a 17-register write-back and reload.
- `sa2_cpu_native_calc_point` (`sub_82146ED8`) and `sa2_cpu_native_multi_matrix` (`sub_82146F60`), in
  `src/cpu/hot/n_ninja_matrix.h`: hand-written instead of the localized copies, writing only the result.
  `tools/call_site_liveness.py` over every direct call site (1,800 and 529): callers read only r3-r10 after them, never
  a float register, so the copies' write-back of 13 float registers per call was dead. njMultiMatrix declines partial
  overlaps of the output with an input.
- **Exact:** the same floating-point operations in the same order (doubles, every fmuls / fmadds / fadds rounded to
  float, std::fma for the fused ones). Guarded (verify mode checks every call) with the new compare mode `kReturnInt`:
  r1, r3, the non-volatile registers, the FPSCR and the written memory (f1 is scratch for these integer functions).
- **Found by the guard on the PC:** the weight constant's address worked out by hand from `lis r11,-32241` was wrong
  (0x820AABD0 instead of 0x820EABD0): verify mode switched the native off at its first skinned chunk, with the address
  of the differing record.
- **Off:** `configs/sa2_test_round13_off.toml` (the four keys; njCalcPoint and njMultiMatrix then run the original).
- **Checked (PC):** verify mode 198.7 M checks, 0 differences (njCalcPoint and njMultiMatrix over 1,048,576 checks
  each, both loaders over 4,096, no list declined); lockstep 2,998 of 2,998 frames identical to `r2f_def` at full
  resolution (`r2f_r13`). ELF `out/nx/elf/sa2_round13_20261007_2114.elf`.
- **A false alarm, fixed afterwards (2026-10-07 night):** `sa2_cpu_native_visibility` (`sub_821BA660`) is a
  localized copy that calls njCalcPoint. Its guard compared with `kCalls`, which includes f1. The hand-written
  njCalcPoint leaves f1 alone, while its original leaves a scratch value there, so f1 at the copy's return depended
  on whether the nested njCalcPoint call was itself a checked one. A difference on f1 alone, with r3, the non-volatile
  registers and the memory equal, switched the copy off for the session (console log "logs11 all on"; once on PC).
  Its 8 call sites read r3, r8 and r9 after it, never f1 (`tools/call_site_liveness.py`), so it is compared with
  `kReturnInt` now.

#### The pipeline list keeps other settings' records

Round 12's `sa2_gpu_prewarm_current_keys` dropped other settings' records from the list. One console session with
`sa2_msaa = 2` emptied the list's 241 records, so the next one with MSAA off created every pipeline on first use again.
They are kept now and only not prewarmed (above, "`sa2_gpu_prewarm_current_keys`"); PC: an MSAA 2 run, then an MSAA-off
run prewarmed its 70 records again (0 new pipelines).

### Vertex shaders Direct3D reorders (2026-10-07): `sa2_gpu_vs_fetch_reorder` (non-CPU group, on)

- **The bug:** the Chao garden's stats window was not drawn (the user's report, 2026-10-07). Direct3D's vertex fetch
  patcher (`sub_82555E18`) rewrites a vertex shader's fetches for the vertex declaration it is drawn with, and for
  some declarations it also moves them to each other's slots: the fetch that writes r10 sits where the container has
  r1's, and so on. The renderer identified a loaded shader by holding every fetch at its own slot, so such a load
  matched no package entry and its draws were dropped. Until now each order was fixed on its own: a dump of the load,
  a container repaired offline (`shaders/tools/repair_vertex_variant_declarations.cpp`) and a new package. That made 9
  orders (7 from console logs, then the stats window's and the garden's on PC).
- **Which shaders can be affected:** only the 4 of the package's 15 vertex shaders with more than one declared fetch.
  Of the other 11, one has a single fetch and 10 have none (Direct3D's own). The 4 are n0 (the main model shader, 4
  fetches), n5 (3), n8 (4) and n21 (4), with 23 + 5 + 23 + 23 = 74 possible orders. 9 had been seen and repaired.
- **What:** when no package entry matches a vertex load in its own order, the renderer runs the repair tool's proof at
  run time (`app/src/me/me_vertex_fetch_reorder.h`) against the package entries of the same length. The load is the
  entry's shader with its fetches permuted when all of these hold:
  - every word outside the fetch slots is identical;
  - the loaded program executes exactly the declared slots as vertex fetches, once each, in unconditional clauses;
  - each fetch has exactly one slot with its destination register, write mask, source and predicate (a bijection), and
    the draw path's own selection picks that slot;
  - each moved fetch stays inside one unconditional fetch-only run between equivalent full-fetch bases.

  The entry it holds for draws the load, chosen once per load. If several qualify, a container of the game's own comes
  first, then the one with the fewest swizzle components to remap. The register-linked wrap of a dump comes last: it
  computes the same but uploads all 256 constant registers. The draw path already read each element's buffer, offset
  and format from the slot the loaded program has it in.
- **Why it is exact:** the package's shader for that entry runs the same ALU code. It differs from the loaded program
  only in the order of independent fetch writes, each to its own register, done before any ALU instruction reads them.
  Destination swizzles the declaration changed (D3DCOLOR order, 0 or 1 for elements the declaration lacks) go through
  the per-draw input remap the renderer already had. That remap is a select chain, so its results are exact.
- **What stays as it was:** a load that any entry matches in order keeps that entry, including the 9 repaired
  containers, which stay in the package. The proof runs only for loads that used to be dropped, and only the entry it
  chose is accepted, and only for that load. The draw-time guard proves it again on every new load, like the ordered
  check.
- **Risk:** a Direct3D rewrite that is more than a reordering is refused, as before: the draw is dropped and the
  shader-miss dump keeps it. That covers a changed ALU word, a fetch in a conditional clause, and a fetch moved across
  an ALU instruction or to another stream's base.
- **Self-check:** the first 64 programs it identifies, and every one in verify mode, are also searched by brute force.
  Every placement of the declared fetches is tried against the ordered predicate, and only the proven placement may
  pass. A difference turns it off for the session.
- **Tests:** `shaders/tests/test_vertex_fetch_reorder.cpp` (host build, see its header).
  - On the 9 repaired containers, the run-time proof maps every fetch to the slot the offline tool wrote, and the
    brute force agrees.
  - A changed ALU word, a changed fetch destination and the unmoved order are refused.
  - `--orders` places each of the 4 shaders' fetches every possible way, as Direct3D would load them: all 74 orders are
    proven, with the right mapping.
- **Off:** `sa2_gpu_vs_fetch_reorder = false` (`configs/sa2_test_vs_fetch_reorder_off.toml`). Such draws are then
  dropped unless the package has a container repaired for that order.

### Version 1.0.0 (2026-10-08): the player's config, language, per-player controls, PGO

These changes concern the release itself, not performance.

- **Built-in settings:** `app/sa2_switch_defaults.toml` is compiled into the Switch NRO (CMake `configure_file` into
  `sa2_builtin_config.h`; `Sa2App::BuiltInConfig`). The SDK applies it before `sa2.toml` (`rex::cvar::LoadConfigString`
  in `rex_app.cpp`). The SD card's `sa2.toml` (`app/sa2_switch.toml`) lists only `language`, `sa2_msaa`,
  `sa2_vsync_relaxed`, `input_xbox_layout`, `input_joycon_sideways` and the `[input_p1]` / `[input_p2]` maps. Every
  other key keeps the value it had in the round-13 `sa2.toml`. Exceptions: logging off (`log_level = "off"`, and the
  new SDK cvar `log_to_file = false`: no `logs/` folder), profiling off (`switch_profile_stacks`,
  `sa2_gpu_marks_categories`) and no shader-miss dumps (`sa2_shader_misses = ""`).
- **Language** (`app/src/sa2_language.cpp`): the SDK's `XGetLanguage` (and `XamGetLanguage`, a stub before) answered
  English whatever `user_language` said. Both return `user_language` now, and `language` sets it at start.
  - `auto` maps the Switch's language through `setGetSystemLanguage` / `setMakeLanguage` (the PC uses its Windows
    display language).
  - SA2 has English, Japanese, French, German, Spanish and Italian; any other language gives English.
  - PC: `language = french` gave the French title screen and file menu.
- **Per-player controls:** the input system tells each driver which guest player each device is
  (`InputDriver::OnDevicesAssigned`, called after the ordinals change). The Switch driver keeps one button table per
  player: `input_p1_*` for player 1 (and 3-4), `input_p2_*` for player 2. These replace `input_map_*`.
- **PGO:** `tools\win\build_switch.ps1 -Pgo generate` builds the game code and the app with `-fprofile-generate
  -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info`.
  - `app/src/sa2_pgo.cpp` streams every object's counts into `pgo/sa2_pgo_<N>.stream`
    (`__gcov_info_to_gcda` / `__gcov_filename_to_gcfn`): every 2 minutes, on focus loss and at exit.
  - `tools\win\pgo_merge.ps1` merges the sessions into `.gcda` files next to the objects (`gcov-tool
    merge-stream`). `-Pgo use` then builds with `-fprofile-use -fprofile-correction -fprofile-partial-training`.
    `-fprofile-correction` is needed: without atomic counter updates, the game threads that ran the same code at once
    lost a few increments. The first use build stopped on blocks whose counts did not add up ("profile data is not
    flow-consistent", `sa2_recomp.1.cpp` and `.5.cpp`).
  - Value profiling is off: its shared lists would need atomic updates across the game's threads.
  - `__gcov_info_to_gcda` writes every object summary as zero (runs 0, sum_max 0), and GCC's hot and never-executed
    thresholds come from it. `tools/pgo_summary.py` (run by `pgo_merge.ps1`) writes runs = sessions and sum_max =
    the largest arc count, as libgcov's own dump would. The first session: 171 objects, largest count 2,515,567,889.
  - **Console result** (the player's test of the released build, 2026-10-08): about 15 % faster than without PGO.
    Single player barely dips below 60 FPS any more; the two-player race does better but often runs around 50.
  - Since then the profile is in the repository (`pgo/`), named by each object's path in the build folder
    (`-fprofile-use=<pgo> -fprofile-prefix-path=<build folder>`), and `pgo_merge.ps1` merges new sessions there. An
    NRO built from `pgo/` is identical to the released one except its build stamps.
- **Logging off, completely:** with `log_to_file = false` nothing is created under `logs/`.
  - stderr goes to `logs/rex/rex_stderr.log` only once the configuration says so (`RexSwitchRedirectStderr`, after
    the config is read; it used to be opened by a startup constructor).
  - The profiler's 10-second `rex_profile.log` report follows the new `switch_profile_report` (off in the built-in
    settings); its FPS overlay keeps running.
  - The folders are created only when something is written; a crash report creates them itself.
- **Default `sa2.toml`:** when there is none next to the NRO, the SDK writes the application's default
  (`ReXApp::DefaultConfigFile`: the player file `app/sa2_switch.toml`, embedded like the built-in settings) before
  reading it.
- **Names:** the renderer and its settings no longer say Mass Effect.
  - The folder `app/src/me/masseffect` is now `app/src/me/native`, and the `masseffect_*` files are `sa2_*`.
  - The namespace `masseffect::` is `sa2::`, cvars `masseffect_*` are `sa2_*`, environment variables `MASSEFFECT_*`
    are `SA2_*`, and the cvar category "Mass Effect" is "SA2 renderer".
  - `app/hot_functions.ld` follows the new mangled names. The pipeline cache migrates once from its old name.
  - Credits to masseffect-nx and notes about what was found in Mass Effect stay as they were, as do the shader
    translator's own names (its output must not change).

### The CPU round of 2026-10-12 (sasrt-nx's): measured first, ported, checked on the PC

sasrt-nx found 1.4 to 2.1 million host float-mode switches a second in a race (each an `msr fpcr` on the A57) and
built thirteen changes around that and around the lock words, most of them in the generator, the post-codegen tools
and the SDK. The user asked for the same in SA2: measure first, port what sasrt-nx had built and verified, try to
verify what it had not, stop before any NRO. Everything below is on the branch `cpu-round-2026-10-12`; **no NRO was
built from it and nothing here ran on the console.**

#### Step 0: the census, on SA2

`sa2_diag_runtime_census = true` (PC builds only; the SDK's `rex/sys_counters.h` counts nothing of it on the Switch)
logs, with every `[enh] uses` report, the real flush-mode writes, the code addresses they come from, and the calls of
the runtime's lock and wait paths. `tools/census_summary.py <log> --capture` turns the lines into per-Swap figures
over the span of a lockstep capture (the same game frames in every run); `tools/census_sites.py <log>` names the
functions.

The released code (the run `sa2_292`, lockstep, 3,000 frames from game frame 6000: title, loading, the City Escape
2-player demo), against what sasrt-nx measured in a race:

| per second, PC | SA2 | sasrt-nx |
|---|---|---|
| flush-mode switches | 80,400 (1,368 per Swap) | 1,400,000-2,100,000 (60,800 per Swap) |
| critical sections entered | 25,700 (437 per Swap) | 85,000 |
| events set / semaphores released | 1,850 / 750 | 12,400 / 6,650 |
| waits | 1,800 | 8,900 |
| handle lookups under the global lock | 7,400 | 31,000 |

So SA2 switches the mode about 45 times less often per frame, and about 60 % of what it does switch comes from
five localized copies: a vector matrix multiply (`n_82140A90`) calling scalar matrix copies (`n_82146E48`,
`n_82146DD8`), and `n_82833568` / `n_82832B78`. SA2's float code is scalar almost everywhere: the generated code
has 40,146 "switch off" checks against 136 "switch on" ones.

#### What was ported, and how each was checked

| Change (sasrt-nx's status) | In SA2 | Switch | Checked by |
|---|---|---|---|
| `fp_compares_modeless` (built) | on | `app/perf_overrides.toml` | unit test, lockstep, verify mode |
| `fp_mode_lookahead` (built; its "mode-free" refinement PC only) | on, **without** the bet past a branch (below) | `fp_mode_lookahead`, `fp_mode_lookahead_past_branch = false` | unit test, lockstep, verify mode, the any-mode census |
| `rotate_word_narrow` (PC only) | ported, **left off** | `app/perf_overrides.toml` | exact (unit test, 2.57 million comparisons; lockstep and verify mode passed with it on), but no gain: with the Switch compiler three recompiled files came out 0.07-0.12 % larger (GCC folds the 64-bit form of an `srwi` into one `lsr`, the narrow one into `ror` + `and`) |
| `tools/guest_cas.py`: `stwcx.` without the trailing `dmb` (built) | 592 sites | `SA2_SWITCH_GUEST_CAS`; run time: `sa2_cpu_guest_cas_no_barrier` | PC runs the rewritten expression; Switch compiler: `ldaxr` / `stlxr`, the `dmb` only behind the key |
| The SDK's lock words inline, `__atomic` (atomic_posix.cpp built; inline in `atomic.h` not compiled for ARM) | on | the same run-time key | Switch compiler (`tools/arm_forms_check.py`); every use read (critical sections, spin locks, lists, bit map) |
| `tools/flush_mode_flow.py` (built) | 15,477 of 40,168 checks proven | `SA2_SWITCH_FLUSH_FLOW` | every PC run tests each proof it executes |
| `thread_signal_skip_idle` (built) | on | the SDK key (`thread_signal_skip_idle`) | protocol test, no lost wake-up; Switch compiler |
| Hook call counters in the thread's context (built) | on, for the copies' use counters too | `sa2_cpu_hook_counter_ctx` | lockstep, verify mode, a run with the key off |
| `-fomit-frame-pointer` on the game code (built) | on | `SA2_SWITCH_OMIT_FRAME_POINTER` | Switch compiler (a flag: nothing to run on the PC) |
| `tools/vec_byteswap.py` (PC only) | 6,357 reversals, 1,293 table shuffles | `SA2_SWITCH_VEC_BYTESWAP` | unit test (4 million vectors); Switch compiler |
| `tools/pch_neon_shuffles.py` (PC only, no test) | on | `SA2_SWITCH_NEON_SHUFFLES` | **new** unit test, 10.2 million comparisons over all 256 immediates; Switch compiler |
| NEON forms of vperm, vctsxs, vctuxs (not compiled for ARM) | on | `SA2_SWITCH_NEON_VMX_HELPERS` | unit tests (19 million comparisons; all 2^32 inputs for the small helpers); Switch compiler: one `tbl`, `fcvtzs`, `fcvtzu` |
| `sasrt_cpu_index_no_fingerprint`, `sasrt_cpu_ring_draw_trims` | **not ported** | - | SA2 made no indexed draw in 5.9 million draws of the census run: neither has anything to act on |
| `sasrt_api_thread_stats` | **not ported** | - | SA2 has one drawing thread: the shared words it would move are written by that thread alone |

The "Switch" options are CMake options of the Switch build (`app/CMakeLists.txt`), all on; each OFF defines the
macro that puts the old form back. `configs/sa2_test_cpu_round12_off.toml` (`tools/make_enhancement_configs.py`)
turns the three run-time keys off.

#### What SA2 does differently: no any-mode conversions on a bet

sasrt-nx's `fp_mode_lookahead` gives an `lfs` / `stfs` / `frsp` / `fcmpu` its any-mode form (a denormal test, about
four instructions, instead of a mode check per function) in three cases: a vector instruction comes next; VMX mode
is known to be current; or nothing that needs a mode follows before the next branch, in any function with vector
float code and in small "mode-free" functions. The third is a bet on a switch that may come later.

Counted on the PC (`REX_DIET_CENSUS_ON`: plain counters in the any-mode helpers of `rex/ppc/diet_ops.h`, compiled
into PC builds of the game code only), over the capture span:

| generator rule | any-mode sites | executed per Swap | flush-mode switches per Swap |
|---|---|---|---|
| the released code (`fp_moves_modeless` only) | 104 | not counted | 1,368 |
| sasrt-nx's rule as shipped | 8,949 | 93,656 conversions, 404 compares | 1,359 |
| without the bet (`fp_mode_lookahead_past_branch = false`) | 205 | 29,813 conversions | 1,346 |

With the bet SA2 ran 94,000 tests a frame, each about four instructions, to save nine switches a frame in play
(about 180 a frame over the whole run, in the menus): a loss by any plausible cost of an `msr fpcr`. Without it the
any-mode forms stay where they replace a switch for certain. `fp_mode_lookahead_past_branch` is a new generator
option (default true: sasrt-nx's behaviour; `sdk/include/rex/codegen/config.h`). Over the first 150 s of the run
(menus included) the switches per Swap are 928 in the released code, 744 with sasrt-nx's rule and 921 without
the bet.

What is left in SA2 is real alternation: the matrix multiply in vector code calls matrix copies that load and store
sixteen floats through the float registers. A copy that needs no mode (one test per load / store pair, or none) is
candidate 3 of the next round's list.

#### Why each is exact

sasrt-nx's arguments, read again against SA2's code:

- **Any-mode helpers:** each returns, in either mode, the bits the plain expression gives with the mode off
  (`sdk/tests/fp_any_mode_test.cpp`). The generator only chooses between two exact forms, so the lists it decides by
  cannot produce a wrong result (they were still compared with SA2's builders: they match).
- **Compare-and-swap without the barrier:** `lwarx` / `stwcx.` order nothing on PowerPC; the game's own `lwsync` /
  `sync` / `eieio` are translated separately. The SDK's lock words (critical sections, spin locks, interlocked
  lists, the bit map) are each read and written only through these atomics, and acquire/release exclusives are what
  every C++ mutex on AArch64 is built from.
- **Flush-mode proofs:** a forward must-dataflow per function with callee summaries; anything that hands `ctx` to
  other code resets it. SA2's addition: `--redo`. A proof depends on which callees are called directly, and
  `tools/direct_calls.py --redo` turns the calls of a newly hooked function back into hooked calls; proofs from
  before would be stale. `tools/post_codegen.py` therefore undoes and proves again after `direct_calls.py`, writing
  only files that change.
- **Skipped notifies:** a waiter is counted under the condition's mutex before the wait releases it, and the
  signaller reads the count together with its state change. The interrupt path, mutants, timers and thread handles
  notify as before.
- **Byte reversal, shuffles, NEON helpers (and the narrow rotates):** identities, each with a test that compares
  the two forms.

#### Checks

Unit tests (`tools\win\run_sdk_tests.ps1`, PC, clang; `sdk/tests/`):

| test | result |
|---|---|
| `fp_any_mode_test` | 600,081,920 comparisons, 0 differences; negative control 448,806 differences |
| `rotate_word_test` | 2,568,192 comparisons, 0 differences (and the wrapping masks do need the old form) |
| `signal_skip_idle_test` | 19.7 million signals notified, 3.4 million skipped, no lost wake-up |
| `vec_byteswap_test` | 4,004,081 vectors, 0 differences |
| `vmx_helpers_neon_test` | 19,006,688 comparisons, 0 differences |
| `vmx_small_helpers_test` | all 2^32 inputs of vcfux, 6 million pairs of the others, 0 differences |
| `neon_shuffles_test` (new) | 10,240,512 comparisons, 0 differences |

PC runs of the final build (lockstep, 3,000 frames from game frame 6000):

- **Frames:** all 2,997 frames in common with the baseline run (`r2f_crcensus0`: the released code with the counters) have a pixel-identical frame there: 2,944 at the same position, and the 53 frames of the loading screen one animation step apart (its ring and its Omochao run on the loading thread's timing). Against the previous build of this branch: 3,000 of 3,000 identical.
- **Verify mode** (`sa2_301`): 208,122,035 native replacement checks, 0 differences; the native Direct3D layer compared 3,519,467 records with the original, 0 differences; no `DIFFERENCE` or `MISMATCH` line.
- **Flush-mode proofs:** every PC run tests each proof it executes (`REX_FLUSH_KNOWN_*` keeps its test off the
  Switch and reports to `RexFlushFlowMismatch`); the `[verify] flush-mode flow` line counts them. 508,240,316 in the census run and 1,077,244,266 in the verify run, none wrong.
- **Run-time keys off** (`sa2_cpu_hook_counter_ctx`, `sa2_cpu_guest_cas_no_barrier`): 2,999 of 2,999 frames identical to the default run of the same build (the build before the last regeneration); the log shows the old counters in use.

With the Switch compiler, without building the NRO (devkitA64, the Switch build's own flags from `out/nx`):

- `tools/arm_forms_check.py`: the `__atomic` lock words are `ldaxr` / `stlxr` with their only `dmb` behind the
  key's byte (the legacy forms end in an unconditional `dmb`); vperm is one two-register `tbl` (8 instructions
  against 12), vctsxs `fcvtzs` (2 against 14), vctuxs `fcvtzu` (2 against 29).
- `tools/switch_compile_check.py`: every changed file the PC does not build compiles (the POSIX threading, the
  atomics, the profiler, the kernel's lock paths), and so do the app's hook file with all the regenerated copies,
  the stage module and game files with `-fomit-frame-pointer`.
- `tools/one_file_ab.py` (one recompiled file, each switch alone, instructions / loads / stores):

| `sa2_recomp.16.cpp` | instructions | loads | stores | `msr fpcr` sites |
|---|---|---|---|---|
| every switch off | 42,138 | 7,617 | 7,738 | 211 |
| flush-mode proofs | 41,713 | 7,584 | 7,714 | 173 |
| no frame pointer | 41,654 | 7,483 | 7,574 | 211 |
| NEON VMX helpers | 41,990 | 7,624 | 7,757 | 211 |
| vector byte reversal | 42,120 | 7,614 | 7,740 | 211 |
| compare-and-swap (adds the key's test) | 42,160 | 7,626 | 7,738 | 211 |
| all on | 41,026 (-2.6 %) | 7,438 (-2.4 %) | 7,549 (-2.4 %) | 173 |

`sa2_recomp.64.cpp`, every switch off against all on: 40,050 -> 38,764 instructions (-3.2 %), loads -4.1 %, stores -3.0 %, `msr fpcr` sites 258 -> 196, `tbl` 81 -> 25.

Static, in the game code: flush-mode checks compiled on the Switch 40,282 -> 24,691 (15,477 proven), sites that
always switch 57 -> 39, 592 compare-and-swap sites, 7,650 vector reversals and table shuffles in the new form.

#### The profile

The committed profile (`pgo/`) was recorded with the released code. `tools/pgo_fit.py` compiles regenerated files
against it with the mismatch warnings on: of 2,075 functions in nine files, 267 (12.9 %) no longer match (their control flow changed) and 6 are new. GCC still uses the counts of a function whose lines only
moved; a function whose control flow changed is compiled without its profile. The flush-mode proofs and the
generator options account for most of it; `-fomit-frame-pointer` and the SDK changes cost nothing. The PGO build
gave 15 % on the console, so a build from this branch should have a new instrumented session before it replaces the
release.

#### Not measurable on the PC

The `dmb` removal (both forms are `lock cmpxchg` on x86), the skipped wake syscalls (the PC does not build that
file), the frame pointer and the NEON forms (Switch code), and what a mode write costs on the A57. The static
counts above and the per-frame counts of the census are what the PC can give: 437 critical sections, 44 signals and
1,368 mode switches a frame are small numbers next to sasrt-nx's, so the expected gain in SA2 is small too: a
few per cent of the game code's instructions (frame pointer, proven checks), not a new frame rate.

#### A fix found on the way: the SDK read the port's release tag as its own

`sdk/cmake/rex_version.cmake` takes the SDK's version from `git describe` and stops when the tag it finds is ahead
of the SDK's floor. With the repository's `v1.0.0` tag every configure of a clone failed ("floor version (0.10) is
behind tag version (1.0)"). The tags are now asked for only when the SDK is the root of its repository; inside a
port the version is the floor, the string the released build has. Committed on `main` on its own.

#### Tools of this round

`tools/census_summary.py`, `tools/census_sites.py`, `tools/arm_forms_check.py`, `tools/switch_compile_check.py`,
`tools/one_file_ab.py`, `tools/pgo_fit.py`, `tools/arm_cost_census.py`, `tools/arm_got_census.py`,
`tools/generated_code_census.py`, `tools/make_sdk_patch.py`, `tools\win\run_sdk_tests.ps1`, and the four
post-codegen steps (`guest_cas.py`, `flush_mode_flow.py`, `vec_byteswap.py`, `pch_neon_shuffles.py`).

### The second CPU round of 2026-10-12: the fifteen candidates built, more found, everything audited

The user's requests after the round above: build the fifteen candidates listed under "Candidates" below, find and
build more that fit any ReXGlue port, then audit all of it for what could backfire on the A57. Everything here was
verified on the PC build only. Nothing has run on the console, no NRO was built, and the committed profile no longer
fits this code (see "PGO" at the end).

**Result in one paragraph.** With the Switch compiler, three recompiled files come out 2.0 to 3.1 % smaller than after
the first round (2.7 to 3.3 % with the errno flag), with 2.5 to 7.7 % fewer loads. About 21,000 indirect calls a frame
were counted; 78 % of them now go straight to their target behind a compare. Two steps that looked free were not:
one changed the picture (found by the frame comparison, not by any self-check) and one cost more than it saved
(found by the census). Both are switched off.

#### The fifteen candidates

| # | Candidate | State | What was measured |
|---|---|---|---|
| 1 | `-fno-math-errno` on the game code | **on** (`SA2_GAME_NO_MATH_ERRNO`) | 0.2 to 0.6 % fewer instructions on top of the rest. `-fno-trapping-math` was split off and is **off** (audit, below) |
| 2 | Loop barriers only where a guest load could be hoisted | **on** (`tools/loop_barriers.py`) | 6,281 of 20,249 loop headers lose their barrier: the loop reads no guest memory and calls nothing, or every way round it passes an indirect call |
| 3 | Registers as locals in leaf functions with loops | **on** (`tools/leaf_locals.py`) | 1,005 functions, 8,526 registers (1,333 only read, not written back). One file: 6.5 % fewer loads |
| 4 | A float copied through a register without a mode | generator option `fp_copy_pairs`, **on where certain**; `fp_copy_only_functions` **off** | Exact for all 2^32 singles (unit test). Where vector mode is current: 71 statements, 1.6 million executions a second, the any-mode conversions drop from 1.32 million to 126,000 a second. In every copy-only function: a loss, see the audit |
| 5 | The cost of one `msr fpcr` on the A57 | built into the profiler's report header ("FPCR on this CPU") | Compiles with the Switch compiler; runs only on the console |
| 6 | Indirect calls with one usual target called directly | **on** (`tools/indirect_calls.py`, `app/indirect_promotions.txt`) | 20,624 sites tagged. PC census of the attract demo: 1.2 to 1.36 million indirect calls a second (about 21,000 a frame), 82 % from sites with one target. 222 sites promoted; calls through the table fall to 283,000 a second. 72.5 million promoted calls checked in one run, 0 wrong |
| 7 | Flush-mode proofs into a function from its direct callers | **on** (`tools/flush_mode_flow.py --image`) | 1,185 functions are entered in a known mode; proven checks 15,477 -> 16,139 of 40,168 |
| 8 | NEON forms for the small VMX helpers | **on** (`rex/ppc/intrinsics.h`) | Ten helpers: 2 to 5 instructions instead of 4 to 41, one of them was 2 already (`tools/arm_forms_check.py`); 0 differences against the x86 forms |
| 9 | `fctidz` as `fcvtzs` and a NaN select | **on** (generator option `fcti_inline`) | 843 sites |
| 10 | `fctid` / `fctiw` round halves to even on ARM | **on** (same option) | 10 sites. A correction of the Switch build: simde's portable conversion rounded halves away from zero. 240 million results against an integer model in the four rounding modes, 0 differences |
| 11 | No address loads from the GOT in the libraries | **not built** | Measured: a forced include with `#pragma GCC visibility push(hidden)` takes one kernel file from 893 to 371 GOT loads. It changes how every library symbol links, which only a linked and run NRO can validate |
| 12 | Critical sections without the thread lookup | **on** (SDK key `kernel_critsec_pcr_owner`) | 28,000 enters a second in SA2. The owner comes from the guest context (r13 -> KPCR -> current thread); 8.3 million calls compared with the lookup in one run, 0 wrong. In the Switch object the enter path has no call left |
| 13 | One signature table of the C runtime for all ports | tool built (`tools/crt_signatures.py`, `tools/crt_signatures.json`) | SA2's `memcpy`, `memmove` and `memset` are found at one address each in another game's image (Banjo-Kazooie) |
| 14 | LTO and PGO for the SDK's libraries | **not built** | Confirmed: the SDK is compiled without `-flto` (177 of the Switch build's 1,253 compile commands have it: the game code and the app). A link-time change: needs an NRO build |
| 15 | Hot words of the shared constant block as push constants | **not built** | It changes the translated shaders, so the shader library the installer builds; a decision for the user, not a CPU step |

#### More that was found while building them

| Step | State | What was measured |
|---|---|---|
| Loads from the image's read-only data as constants (`tools/ro_constants.py`) | **on** | 14,297 float, 792 double and 1,380 integer loads folded (the address is built from `lis` / `addi` constants and lies in `.rdata`). The PC build still loads and compares: 59.7 million in one run, 0 wrong |
| A single-precision load straight into a float register (`tools/float_loads.py`) | **on** | 72,124 loads: `ldr s; rev32; fcvt` instead of `ldr w; rev; fmov; fcvt` (the general-to-float move is the slow step on the A57) |
| The trap helper out of line and cold (`tools/pch_cold_traps.py`) | **on** | 1,219 trap sites (mostly the guards in front of divisions) no longer carry logging code inline. A layout change: -0.4 / -0.8 / +0.25 % instructions |
| `dcbt` as the host's prefetch (generator option `dcbt_prefetch`) | built, **off** | 217 sites, no measurement on the console |
| No link-register moves in prologues (`tools/frame_traffic.py`) | marked, **off** | 9,880 `mflr` with their saves, 10,682 `mtlr`. Passed the frame comparison, but see the audit |
| No prologue saves of non-volatile locals (same tool) | marked, **off: NOT exact** | 14,979 saves. Changed the picture, see the audit |
| No back-chain stores (same tool) | marked, **off** | 22,597 stores |
| The stack pointer as a C++ local | measured and **dropped** | 1.3 to 1.6 % MORE instructions: a local that lives across a call needs a callee-saved host register |
| Pointer translation without a heap lookup (SDK, off Windows) | **on** | `Memory::TranslateVirtual` called `LookupHeap` for an offset that is 0 on every platform but Windows and macOS on ARM. 318 call sites; 5,700 a second in SA2 |
| Object reference counts inline (SDK) | **on** | `XObject::Retain` / `Release`: 577 call sites; 14,700 a second in SA2 |

Checked and nothing to gain: the Switch compiler already expands atomics inline (no `-moutline-atomics` helpers), and
no indirect call of SA2 leaves the main image (0 % go through the cross-module resolver).

#### The audit: what could backfire, and what was changed because of it

1. **Removing the dead saves of non-volatile registers changes the picture. Off, and opt-in in the tool.** With
   `non_volatile_as_local` these saves write zeros (the callee's fresh locals) into the frame. The game reads a stack
   word it never initialised: zero is harmless, stale data is not. In the two-player demo Sonic's and Shadow's red
   parts came out purple and white road lines blue (`out/pc/captures/broken_r2/`, the user spotted the tint).
   Every self-check passed; only the lockstep frame comparison showed it. The released build depends on those zeros
   too. The reader has not been found yet.
2. **The link-register half is off as well.** It passed the same comparison, but it changes stale frame contents in
   9,880 functions in the same way, and one demo scene is not enough after item 1.
3. **`-fno-trapping-math` is off.** It lets the compiler run a float operation earlier than the code does (out of a
   branch or a loop). The recompiled code changes the host's flush mode in between, which the compiler does not know
   a float operation depends on. It bought at most 0.5 % of one file. sasrt-nx and masseffect-nx ship it.
4. **The float copy forms in every copy-only function are off.** They removed 24 % of the mode switches (92,300 ->
   70,000 a second) but ran 5.25 million times a second to do it: 235 extra tests per switch saved. SA2's matrix
   copies run about 1,500 times a frame and only about one call in six comes from vector code. The same bet lost in the
   first round (`fp_mode_lookahead_past_branch`). The number that would decide it is candidate 5's.
5. **Promoted calls and loop barriers.** A loop needs no barrier when every way round it passes an indirect call. A
   promoted call is no longer one: the compiler can inline its target. So `tools/indirect_calls.py` now runs before
   `tools/loop_barriers.py`, and 23 loops got their barrier back.
6. **Promoted calls otherwise.** The guard compares the target, so a site that goes elsewhere is still correct and
   pays one compare. The list comes from one scene (the attract demo); a census of more scenes would make it better,
   not safer. `SA2_GEN_PROMOTED_CALLS=OFF` makes every guard false.
7. **Read-only constants** assume nothing writes the image's `.rdata`. No source of the app does (it only reads
   constants there), and every PC run compares each folded load with memory. A future patch that writes there would
   show up as a `DIFFERENCE` line on the PC, not on the console.
8. **Leaf locals** load every register at entry and store the changed ones at each return. That pays when the loop
   runs at least once; a leaf that usually skips its loop does a little more than before. Only leaves that still
   have a barrier are taken.
9. **The NEON `vcfux`** rounds in the current mode like the x86 form, but the two differ in rare cases when the
   guest has set a mode other than round-to-nearest. The 360 always rounds vector conversions to nearest, so neither
   was exact there.
10. **Entry modes, critical section owners, pointer translation**: each has its check. The first two are compared
    on every execution in the PC build; the third aborts at start-up if a heap's offset disagrees with the constant.
11. **The post-codegen sequence is idempotent**: a second run on a processed tree changes no file (compared by
    hash). It was not at first: the barrier step decided differently on text the later steps had rewritten.

#### Verification (PC, final build)

- **Unit tests** (`tools\win\run_sdk_tests.ps1`): 9 of 9. New: `sdk/tests/fcti_copy_test.cpp` (17.4 billion checks,
  0 differences; its negative control finds 16,777,214) and `sdk/tests/float_loads_test.cpp` (the vector form of a
  float load against the integer form, all 2^32 patterns).
- **Lockstep capture** against the first round's reference (`r2f_crcensus0`): 2,942 of 2,995 frames identical in
  place; the other 53 are the known loading-screen frames. The same with the run-time keys off.
- **Verify mode**: 208 million native checks, 0 differences; 1,938 of 1,991 frames identical plus the same 53.
- **Self-checks of one run**: 59.7 million folded constants, 72.5 million promoted calls, 8.3 million critical
  section owners, 514 million flush-mode proofs; 0 wrong.
- **Switch compiler**: every changed SDK and app source and four generated files compile
  (`tools/switch_compile_check.py`); `tools/arm_forms_check.py`: all claims hold.

#### Switches

| Kind | Name | Default |
|---|---|---|
| Generator (`app/perf_overrides.toml`) | `fcti_inline`, `fp_copy_pairs` | set |
| Generator | `fp_copy_only_functions`, `dcbt_prefetch` | not set |
| Build (`SA2_GEN_*`, every platform) | `RO_CONSTANTS`, `FLOAT_LOADS`, `LEAF_LOCALS`, `COLD_TRAPS`, `PROMOTED_CALLS` | ON |
| Build | `SA2_GEN_LR_MOVES_ELIDE`, `SA2_GEN_NV_SAVES_ELIDE`, `SA2_GEN_BACK_CHAIN_ELIDE` | OFF (not exact) |
| Build | `SA2_GAME_NO_MATH_ERRNO` / `SA2_GAME_NO_TRAPPING_MATH` | ON / OFF |
| Run time (SDK) | `kernel_critsec_pcr_owner` | true |
| Run time (PC diagnostic) | `sa2_diag_indirect_census = "<file>"` | empty |

The promotion list is made with `python tools/indirect_promotions.py --census <file> --gen app/generated/default`.

#### PGO

`tools/pgo_fit.py` on three files: 29.7 % of the functions lose the committed profile (12.9 % after the first round).
A release build from this code needs a new instrumented session first; without one it may be slower than v1.0.0.

### Fifteen more candidates, mostly in the SDK (2026-10-12; found, two built)

The user's next request: look at the SDK itself. "Sites" are call sites in the 138 Switch objects of the SDK's
kernel, system and core sources (`objdump -r`). "A second" is the PC census of SA2's attract demo at 58 frames a
second (`sa2_diag_runtime_census`, new line "runtime overhead"). In SA2 all of these are small: its game thread makes
a few hundred kernel calls a frame. They weigh more in a port with more kernel traffic (sasrt-nx enters 85,000
critical sections a second, three times SA2).

| # | Candidate | Sites | A second in SA2 | State |
|---|---|---|---|---|
| 1 | Every log statement calls `rex::GetLoggerRaw` out of line before it tests its level, written or not. An inline level gate in front | 3,012 (plus 245 category accessors) | 1,360 | not built |
| 2 | `Memory::TranslateVirtual` looked a heap up for an offset that is 0 off Windows | 318 | 5,700 | **built** |
| 3 | `XObject::Retain` / `Release` out of line | 577 | 14,700 | **built** |
| 4 | `rex::system::kernel_state()`: an out-of-line call that returns one pointer | 378 | 12,000 | not built (a DLL data boundary on Windows) |
| 5 | A setting read (`REXCVAR_GET`) is a call that returns a reference to a function-local static | 319 over 63 settings | not counted | built for the critical sections only |
| 6 | Handle lookups take the global recursive mutex and retain the object | 72 | 7,400 (16 to 28 find the lock taken) | not built (a concurrency change) |
| 7 | Waits and events look their host object up from the guest dispatcher header | - | 3,200 | not built |
| 8 | The guest clock behind a mutex | - | 2,700 | not built |
| 9 | Thread-local reads are calls on the Switch (`__aarch64_read_tp`); kernel exports ask for the current thread and thread state that way. Hand them the guest context instead, as the critical sections now get | 51 + 48 + 23 | not counted | built for the critical sections only |
| 10 | Every kernel export goes through a `noinline` shim and, when it returns a value, a compiler barrier that spills everything | 1,363 exports | about 1,100 a frame | not built |
| 11 | The profiler's counters are out-of-line calls on the Switch even with the report off (one per handle lookup, per clock read) | 18 | not counted | not built |
| 12 | Sites with two or three usual targets: a chain of guards. 18 % of the calls came from sites with several targets (the largest, `sub_8214B520`, the render-state table, has more than four and stays indirect) | - | 283,000 indirect calls left | not built |
| 13 | Signed division is written `(d && !(n == INT32_MIN && d == -1)) ? n / d : 0`; AArch64's `sdiv` already answers 0 for a zero divisor, so one of the two tests can go | 3,643 in the game code | not counted | not built |
| 14 | A host call into guest code zero-fills a whole `PPCContext` first (its 128 vector registers alone are 2 KB) | - | 117 | not built |
| 15 | Each log statement expands to a formatted call inside a try / catch in the function that logs | 2,271 handler sites | - | not built (code layout) |

### The third CPU round of 2026-10-12: searched, built and audited; six items kept, three dropped

The user's request: search, build and audit another fifteen improvements that fit any ReXGlue port. Fifteen that hold
up were not there to find in one pass. What follows is everything that was tried: six items kept, three measuring
tools, three experiments dropped with their numbers, and the audit. PC-verified only, like the rounds above.

#### Kept

| Item | Where | What was measured |
|---|---|---|
| Guest registers as C++ locals for the length of a call-free loop | `tools/loop_locals.py` (`SA2_GEN_LOOP_LOCALS`) | 5,767 loop regions in 3,044 functions, 25,967 registers (5,283 only read). In `sub_8286ADC8`'s loop the seven loads and stores of ctx per iteration are gone and the body is 33 instructions instead of about 45. The barrier stays. Whole files: instructions unchanged, 0.8 to 1.6 % fewer loads |
| Promoted calls with two or three targets | `tools/indirect_calls.py`, `tools/indirect_promotions.py` | 243 sites (21 with several targets): the guards catch 84.9 % of the census's indirect calls, up from 80.6 %. 74.3 million promoted calls checked in one run, 0 wrong |
| The integer divisions as the instruction | generator option `div_inline`, `rex::ppc::divw` ... | 1,978 sites. With the Switch compiler: `divwu` 2 instructions instead of 4, `divw` 6 instead of 9, no branch; a constant divisor still becomes a multiplication |
| Vector loads from read-only data as constants | `tools/ro_constants.py` (`REX_RO_V128`) | 94 `lvx` sites; they ran 22 million times in one PC run (about 1,300 a frame). A literal instead of a guest load, REV64 and EXT |
| `kernel_state()` inline off Windows | `rex/system/kernel_state.h` | 378 call sites in the runtime, 12,000 calls a second in SA2. The PC build keeps the function (the pointer is in the runtime DLL), so this one is compile-checked only |
| Every start checks the forms only one processor has | `app/src/cpu/sa2_forms_selfcheck.cpp` | 248 results (the conversions to integer, the divisions, the float copies, twelve vector helpers, the two saturating conversions) against plain C++ references; one `[enh] instruction forms self-check` line. On the PC it proves the expected values; on the console it is the first time the AArch64 forms run. Not a speed item: the idea is banjo-nx's |

#### Measuring tools that came out of it

- `tools/loop_body_census.py`: instructions, loads and stores inside loops of an AArch64 object (a file's size cannot
  judge a change inside loops).
- A census of calls per kernel export (`rex/hook.h` counts every `REX_EXPORT` in PC builds; `[census] kernel exports
  by calls`). SA2's attract demo: `RtlLeaveCriticalSection` 28,000 a second, then nothing above 900 a second
  (`NtWaitForSingleObjectEx`, `NtSetEvent`, `KeSetEvent`, `KeTlsGetValue` ...). Beyond the critical sections the
  kernel side has nothing left to win in this game.
- CPU cycles per thread over a lockstep capture (`[capture] CPU over the N recorded frames`). Two identical runs
  differed by 13 % on the game thread, so it shows proportions (the renderer's command thread costs about three times
  the game thread on the PC), not a step's gain.

#### Tried and dropped

All three wanted the same thing as the first item: a loop barrier that does not force every ctx register through
memory on each iteration.

| Experiment | Result with the Switch compiler | Why it was dropped |
|---|---|---|
| The loop's own loads as volatile reads instead of a barrier | +1.2 / +2.3 / +3.0 % instructions, +2.0 to +5.9 % loads | A volatile load cannot take its value from a store of the same iteration, and the game stores an integer to the stack and loads it back as a float all the time |
| The guest-memory base pointer made opaque at the loop header | +3.4 / +4.5 / +3.3 % instructions, +5.8 to +7.9 % loads | The compiler then no longer believes that guest stores leave ctx alone and reloads ctx after every one of them |
| An opaque zero added to the base pointer at the loop header | +1.3 / +3.5 / +1.1 % instructions; in one loop 7 loads and stores of ctx per iteration became 1 | It works as intended in the loop looked at, but the files grow, the gain cannot be shown without the console, and it changes the one mechanism that keeps waiting loops from hanging. The first item reaches the same loops and leaves that mechanism alone |

#### The audit

1. **Loop locals load every register the loop mentions on the way in.** A loop that usually runs once loads a few
   registers it then does not use. Half of the regions carry at most 4 registers, 90 % at most 8, none more than
   22, so that cost is small and bounded; from the second iteration on the step only saves. Every `goto` keeps its
   plain shape (jumps across a region's border go through stubs), which the other tools depend on: without that a
   second run of the post-codegen sequence lost flush-mode proofs and added barriers.
2. **The divisions use inline assembly**, which hides them from the compiler: it cannot merge two equal divisions
   any more. A divisor known at compile time keeps the C expression for that reason.
3. **`kernel_state()` inline has not run anywhere yet**: the PC keeps the old form. It reads the same pointer.
4. **A second or third guard** costs one compare more on the calls that match none (under 0.5 % of a promoted
   site's calls by the rule that selects them).
5. **Vector constants** rest on the same assumption as the scalar ones (nothing writes the image's `.rdata`); the PC
   build compares each one with memory every time it runs.
6. **The forms self-check** can only raise a false alarm, never hide a fault; its expected values are proven by
   the PC run of the same table.

#### Verification (PC, the round's final build)

- Lockstep capture against `r2f_crcensus0`: 2,926 of 2,979 frames identical in place, the other 53 the known
  loading frames; the same in verify mode (1,943 of 1,996) and with the run-time keys off (2,930 of 2,983).
- Verify mode: 208.8 million native checks, 0 differences. Self-checks of one run: 81.6 million folded constants,
  74.3 million promoted calls, 7.8 million critical section owners, 0 wrong. Forms self-check: 248 of 248.
- The post-codegen sequence run twice leaves the tree unchanged (compared by hash).
- Switch compiler: the changed and new sources and four generated files compile; `tools/arm_forms_check.py` holds
  (now also the divisions); no `fcvtas` in the generated objects (the old rounding, as banjo-nx checks it).

Of the SDK list above, this round built items 4 (`kernel_state()`), 12 (several targets per site) and 13 (the
divisions).

### Link-time optimisation of a recompiled program: what the release's link did and did not do (2026-10-12)

The user's question: can LTO be improved for ReXGlue ports in general? Measured on the released v1.0.0 ELF (LTO and
PGO, `out/nx/elf/sa2_v1.0.0_pgo_use_20261008_0105.elf`) and with the Switch compiler (GCC 16.1). Nothing here has
changed a build yet; the last part says why that is the right order.

The LTO unit is the game code and the app: 177 of the Switch build's 1,253 compile commands. The SDK, the
third-party libraries, the stage module (127 functions, 55 KB) and the driver stay outside.

| Question | Measured | Conclusion |
|---|---|---|
| Does the inliner take the small functions? | 110,826 direct call sites are left between game functions. 2.0 % of them call a function of 32 instructions or fewer, 14.1 % one of 64 or fewer, 46 % one of 128 or fewer | Yes. What stays out of line is medium and large |
| What about functions called from one place? | 8,579 of the 30,766 game functions have one direct call statement in the generated code and no address anywhere in the image (`tools/address_taken_census.py`). In the ELF, 7,326 functions with one call site left hold 28.5 % of the game code's instructions | In an ordinary program GCC inlines a function that is called once and drops its body (`-finline-functions-called-once`). It needs the function to be unreachable otherwise, and the function table takes the address of every function, so the rule never applies. These functions meet the limit for any other function instead (`max-inline-insns-auto` = 30) |
| Are functions split into a hot and a cold part? | No: 0 `.cold` parts in the release. `-freorder-blocks-and-partition` is off by default on AArch64. Tried on two game files: without a profile 2 cold parts (248 bytes); with the committed profile 4 parts in file 16 (328 bytes of 179 KB), and file 64 does not assemble (`invalid operands (.text.unlikely.X and .text.X sections) for '-'`: a jump table whose targets were split off) | Dropped: it breaks with this compiler and moves next to nothing |
| Do the interprocedural passes specialise functions? | 437 `.constprop`, 1,083 `.isra` and 218 `.part` clones in the program, 24 of them of game functions | Not in the game code: every game function has the same two parameters and all of its state in memory, so there is nothing to specialise |
| What crosses the border of the LTO unit? | From game code: the critical sections (2,220 call sites into one-instruction thunks), the cold fallback of an indirect call (22,866 sites), and in the release the logging inside every trap site (round 2 moved that out of line) | In SA2 the kernel is rarely called (the third round's census: only the critical sections are frequent), so putting the SDK into the unit would win little here. A port with more kernel traffic should measure it before deciding |
| How is the link partitioned? | 512 LTRANS partitions (GCC 16's default). `-fipa-ra` only helps a call whose callee was compiled earlier in the same partition, and `-fipa-pta` runs per partition | A link-time experiment (fewer, larger partitions); game functions push 2.99 callee-saved registers on average |

#### The order of work

GCC inserts the profile's counters before its interprocedural inliner runs, and matches a function with its profile
by the function's own control flow. So:

- what is decided at the link can still change after a profile is recorded, without losing it: the inliner's
  limits, the partitions, which functions the table lists;
- what must be final before the console session is the source (every round above) and the set of instrumented
  objects.

The instrumented build of this branch therefore keeps the release's LTO settings, and the items below wait for the
new profile, whose counts can then judge them (`-fdump-ipa-inline` at the link lists every decision with the
call site's count).

#### Candidates for the build that uses the new profile

1. **A higher `max-inline-insns-auto` for the game code** (a compile option of the game files: GCC keeps its
   parameters per function). With a profile the inliner takes the hottest call sites first and leaves cold ones
   alone, so this reaches the hot medium-sized functions the limit of 30 keeps out, including the hot ones that are
   called once. Semantics cannot change; the cost is code size, bounded by `inline-unit-growth`.
2. **Fewer, larger partitions**, for `-fipa-ra`.
3. **A function table without the functions nobody takes the address of.** The largest lever (the called-once rule
   and smaller code), and the only one that can break the game: a function reached by an address the analysis
   missed ends in the invalid-function trap. The loaded image has no relocation table to make the list exact (its
   `.reloc` range holds other data), so this needs a conservative address scan and a PC census proving that every
   indirect target of long runs is on the list. Not started.

GCC 16.1 also has `-flto-incremental=<folder>` (with `-flto-partition=cache`): unchanged partitions are reused
from one link to the next, which would shorten the A/B builds. Not for a release build.

Tools: `tools/lto_census.py` (what a link left in an ELF) and `tools/address_taken_census.py`.

## Build-time: the codegen flavour (A/B NRO pair)

These options cannot be switched at run time:

- `tools\codegen.ps1 -Flavor classic`: the generator's plain output.
- `tools\codegen.ps1 -Flavor optimised`: copies `app/perf_overrides.toml` into `app/codegen_flavor.toml` (included by
  the manifest) and runs the post-codegen steps.

Both flavours regenerate the localized natives (`app/src/cpu/hot/localized.txt` → `app/generated/hot/`) and write
`app/generated/codegen_flavor.txt`. The build puts that file's name in the first `[enh]` line. Build each flavour as
its own NRO.

The optimised flavour adds these codegen options:

- `cr_as_local`, `ctr_as_local`, `xer_as_local`, `reserved_as_local`
- `skip_msr`
- `dform_disp_split`
- `fctiwz_inline`

It then runs two post-codegen steps:

- `tools/pch_no_volatile.py`: guest memory accessed without `volatile`;
- `tools/direct_calls.py`: about 91,000 calls between unhooked functions become direct.

These are masseffect-nx's measured options. Since round 7 they also include `non_volatile_as_local`,
`elide_lr_stores` and sa2-nx's own `fp_moves_modeless` (see Round 7), and since 2026-10-07 `non_argument_as_local` with
sa2-nx's `non_argument_as_local_keep` list (see "`non_argument_as_local`").

On the branch `cpu-round-2026-10-12` (see "The CPU round of 2026-10-12") the flavour also sets sasrt-nx's
`fp_compares_modeless` and `fp_mode_lookahead` (with sa2-nx's `fp_mode_lookahead_past_branch = false`), and
`tools/post_codegen.py` runs four more steps after the direct calls, on both generated trees: `tools/guest_cas.py`,
`tools/flush_mode_flow.py --redo`, `tools/vec_byteswap.py` and `tools/pch_neon_shuffles.py`.

`tools/read_before_write.py` on the optimised code finds only the guest setjmp (`sub_82908F10`). It reads cr1–cr7 to
store them in the jmp_buf, as Mass Effect's does. The six setjmp callers (libjpeg, sub_82812A10 and others) are not
marked `share_registers`. After a longjmp, ReXGlue restores `ctx` but not the CR/XER/CTR locals. Those callers
recompute CR0 right after setjmp and set the others again before use. A longjmp only happens on libjpeg's error path,
which needs a corrupt JPEG.

Checked on PC with the optimised flavour, its post-codegen steps (91,279 direct calls, non-volatile memory) and all
CPU enhancements:

- **Verify mode:** 128 million native calls checked, 0 differences.
- **Lockstep capture:** a `vblank_per_swap` capture of the City Escape 2-player attract demo compared with the classic
  build's capture (`nat_g`).
  - All 105 gameplay frame pairs are pixel-identical outside the blinking "Press START" text, which runs on
    wall-clock time.
  - The simulation stayed in sync up to the end of the 3,000-frame capture.
- **Speed:** not measurable on this PC while other builds were running. The console A/B decides.

Pitfall (fixed): the PC build re-runs the generator by itself whenever an input changes (the manifest, a toml, the
xex, the generator binary). That rewrites `app/generated` without the post-codegen steps. The first optimised PC
tests and the first 59.7 MB optimised NRO had the codegen options only, for that reason.

`tools/post_codegen.py` now holds the steps. `tools\codegen.ps1` runs it, and `app/CMakeLists.txt` appends it to the
build's own codegen command. To check after a build, count `__imp__sub_X(ctx, base);` in `app/generated/default`.

Loop barriers (required with non-volatile memory). GCC -O3 on the Switch hoists the load of a guest polling loop
out of the loop and turns the loop into a branch to itself. clang on the PC does not, so PC runs looked fine. futari-nx
found this on its console.

`tools/loop_barriers.py` (run by `tools/post_codegen.py`) puts `REX_LOOP_BARRIER()`, a compiler memory barrier, at every
loop header of the generated code: 20,331 headers in SA2. Check a Switch ELF with `tools/find_self_loops.py out/nx/sa2`.

| SA2 optimised build | Branch-to-self loops in recompiled code |
|---|---|
| Without barriers (the first NRO of that flavour; do not use it) | 40, including sub_8255A760 in Direct3D |
| With barriers | 15 |

All 15 left are genuine guest `b .` halts present in the original PowerPC code.

## Candidates, measured and not done yet

The masseffect-nx renderer that Futari also carries has these ring-thread costs. Measured on SA2's PC ring thread
(attract loop, busy samples, 2026-10-06):

| Candidate | SA2 PC cost | Note |
|---|---|---|
| In-place `RepackWord1011` on the uncached upload buffer | 0 % | Futari: 91 %. SA2 draws no k_10_11_11 repacks here; recheck in stages. |
| `ProveRectangleList` copying the whole register file per rectangle list | 0 % | Done in round 2 (`sa2_cpu_ring_rect_list_in_place`). |
| String formatting on the ring: RedirectClearDepthEDRAM4's `no()` decline counter, the bounded-draw-extent key, the clear-color plan | about 3 % together | Round 2 did the first two (`sa2_cpu_ring_redirect_key`, `sa2_cpu_ring_extent_key`). |

On the console the ring thread used only 7 to 15 % of a core in the attract loop. These wait until console logs from
gameplay show the ring thread is busy.
| Null vertex shader one-point draws through the 4x depth alias (sasrt-nx: XXH3 fingerprint AFCC8377CE2A398F, a whole shadow map moved to the 4x image and back per memexport draw) | not seen | No SA2 log (PC or console) names that fingerprint; recheck in stages. This changes EDRAM/depth ownership, so it needs a long play test. |

### Fifteen CPU candidates for any ReXGlue port (2026-10-12; as found. Built since: see the second round)

The user's request after the round above. Each has what was counted for it in SA2, on the PC or with the Switch
compiler; none has run on the console. "Sites" are places in the generated code (`tools/generated_code_census.py` counts them for any port), "per
Swap" comes from the census run. Order: what the numbers promise against the work.

| # | Candidate | Where | What was counted | Exactness / check |
|---|---|---|---|---|
| 1 | `-fno-math-errno -fno-trapping-math` on the game code. sasrt-nx and masseffect-nx ship both; sa2-nx and nfsmw-nx do not | build flag | 1,223 `sqrt` calls in 667 game functions of the released ELF (a compare and a branch to the library after every `fsqrt`). Three files with the Switch compiler: -0.3 / -0.6 / -1.4 % instructions, -1.9 % at best with both | Values unchanged: nothing reads `errno` or the host's float flags. Lockstep and verify mode |
| 2 | Loop barriers only where a guest load could be hoisted | `tools/loop_barriers.py` | 20,249 barriers. 1,653 loops read no guest memory at all, and 4,232 contain a call the compiler cannot see through (redundant where every path back to the loop header passes one): up to 29 % protect nothing. A barrier clobbers all memory, so every register still in `ctx` is stored and reloaded on each iteration | The barrier's own argument, per loop; `tools/find_self_loops.py` on the ELF as today |
| 3 | Argument registers (r3-r10, f1-f13, v0-v13) as locals in functions with loops | generator | The 9,012 functions with a barrier hold 56 % of the 1,503,601 uses of those registers; 4,881 functions are leaves. The localized copies do this by hand for about 50 functions. masseffect-nx measured full arguments-in-registers as no gain on top of locals, but it has no loop barriers | Exact by construction, like `tools/localize_native.py`; lockstep |
| 4 | A float load and store of the same register as a copy that needs no mode | generator | 40 % of SA2's remaining mode switches: a vector matrix multiply calling scalar matrix copies (`n_82140A90` 21.7 %, `n_82146E48` 13.2 %, `n_82146DD8` 5.6 %). 2,399 such adjacent pairs in the code | One denormal test per pair gives the released bits; with the test dropped a signalling NaN is copied unchanged, as on the 360. Needs its own unit test |
| 5 | The cost of one `msr fpcr` on the A57, measured at start and logged | profiler | sasrt-nx's lookahead threshold and candidate 4 both hang on this number, which nobody has | A timing line, no behaviour |
| 6 | Indirect calls with one usual target called directly | post-codegen, from a census | 21,365 indirect call sites, 16 % of all call sites (range check, table load, null check, `blr`). How often each runs and where to is not counted yet: the flush census's site table can count it. GCC's own value profiles would do the same in the PGO session; they are off there because their updates are not atomic | The guard compares the target; the slow path is today's code |
| 7 | Flush-mode proofs across calls into a function (its entry mode from its direct callers) | `tools/flush_mode_flow.py` | 24,691 checks left in 7,684 functions; at least one per function is "unknown at entry". 55,097 proven checks run per Swap already; the unproven ones are not counted yet | The PC self-check tests every proof it executes |
| 8 | NEON forms for the small VMX helpers | `rex/ppc/intrinsics.h` | With the Switch compiler: vcfux 10 instructions (`ucvtf` is one), `simde_mm_sllv_epi8` 40, `simde_mm_cvttps_epi32` 7, `simde_mm_dp_ps` 4-8. SA2 uses them 33, 2, 4 and 34 times; vector-heavy games more | sasrt-nx's `vmx_small_helpers_test.cpp` already reports 0 differences for five of them |
| 9 | `fctidz` as `fcvtzs` and a NaN select, like `fctiwz_inline` | generator | 843 sites in SA2, each an `isnan` test, a range compare and a select around the conversion | The same argument as `fctiwz_inline`; a unit test over the edge values |
| 10 | `fctid` / `fctiw` on ARM round halves away from zero (**a difference from the PC build and the 360, not a speed item**) | generator / simde | simde's non-x86 `simde_mm_cvtsd_si64` is C `round()`; x86 and PowerPC round to even. 10 sites in SA2 | `frinti` + `fcvtzs`; a unit test of the ties |
| 11 | No address loads from the GOT in the libraries outside the LTO link | build (hidden visibility on declarations) | 38,528 loads of an address from the GOT in the released ELF: 3,555 in game functions, 34,973 in the SDK, the renderer and the driver; 5,685 slots | Addressing only |
| 12 | Critical sections without the thread lookup | SDK | `RtlEnterCriticalSection` calls `XThread::GetCurrentThread()` (out of line, a thread-pointer call) for the owner id that `ctx.r13` already gives, as the spin locks do. 437 enters per Swap in SA2, 85,000 a second in sasrt-nx | The same id from another place; an assert in the PC build |
| 13 | One signature table of the C runtime and math functions for all ports | tools | Every port finds `memcpy`, `memset`, `sin`... by hand. SA2's `memcpy` hook takes about 400,000 calls a second, `sin` and `cos` 19,000 each | The existing guard (native against original) |
| 14 | LTO and PGO for the SDK's runtime libraries too | build | A kernel call is `__imp__X` -> `HostToGuestFunction<>` -> `X_entry` -> its helpers, each a real call. 2,140 import call sites, about 1,100 counted runtime calls per Swap in SA2 | No source change |
| 15 | The four hot words of the shared constant block as push constants (the renderer all masseffect-nx-based ports share) | renderer | The shared block changes on 90 % of draws, mostly in words 0, 48, 90 and 91; 0.92 binds of set 4 per draw at about 1.3 µs each on the console: about 1.3 ms per frame of the ring thread in play | Pixel-identical: lockstep capture |

For SA2 itself the first three and 6 are the ones with weight: its game thread is drawing code in loops, and the
kernel traffic (12, 14) and the mode switches (4, 5, 7) are small here. For sasrt-nx the order is the other way
round.

## Build time

The generated `sa2_register.cpp` (one function of SetFunction calls, run once at startup) is compiled with `-O0` on
the Switch. GCC `-O3` spent 771-927 s on it, the critical path of every build. It now takes about 30 s, and a full
NRO build takes 8 minutes instead of 16-20 (from sasrt-nx).
