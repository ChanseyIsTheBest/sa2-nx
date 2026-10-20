# Renut CPU work in sa2-nx, for banjo-nx to port (2026-10-20)

Everything SA2 changed in Renut's CPU paths since taking it from banjo-nx (commit `c319b29` plus banjo-nx's working
changes up to 2026-10-20 07:00), plus the plume and SDK change that drops the Vulkan driver from a deko3d-only build.
Each item says what was slow (measured on the console), what changed, where the code is, and what to adapt for N&B.

- **SA2's tree:** `D:\sonic adventure 2\sa2-nx` (same machine; read it directly). Every change carries a
  `// sa2-nx (2026-10-20 ...)` comment, so `grep -rn "sa2-nx (2026-10-20" app/src/renut_engine` lists them all.
- **Full diffs against your base:** `patches/renut-banjo-nx-c319b29.patch` (all of `app/src/renut_engine` against
  your `c319b29`: it also holds SA2's retargeting, so take the hunks below, not the whole file) and
  `patches/plume-banjo-nx-c319b29.patch` (plume, 4 files).
- **Keys:** SA2 names them `sa2_renut_*`; rename to `banjo_*` as you like. Each is on by default and can be switched
  off for an A/B. The `_verify` keys do the work both ways and count differences.
- **Verification in SA2:** PC lockstep frames with every key on against every key off (bit-identical, 468 of 468;
  the later items 447 of 447 and 467 of 467), the verify keys (0 differences over millions of draws), Ryujinx runs, and
  four console runs (below).

## What it did on the console (SA2, City Escape, 1020 MHz CPU, handheld)

Fitted from Renut's 10 s lines (thread time against draws a frame):

| Package | Render thread at 1,400 draws | Game thread at 1,400 draws | Items |
|---|---|---|---|
| 1 (Renut as taken) | 17.6 ms (11.7 us a draw) | 19.2 ms (10.4 us a draw) | - |
| 3 | 12.5 ms | 17.6 ms | deltas, constants by the chain, pool, fast paths, chain reuse, done signal, store cards |
| 4 | 11.5 ms | 17.3 ms | narrow compare, shared constants, prefetch, core layout fixed, smaller ones |
| 6 | 11.1 ms (7.2 us a draw) | 15.5 ms (7.2 us a draw) | NEON compare, wake batch, core 2 preferred, card loop fix, no Vulkan |

Content hashing went from 16 % of the render thread to 0 (store cards, which are yours), `PushFloatConstants` from 15 %
to about 5 %. GPU time stayed 5-6 ms at 460.8 MHz and 7-8.6 ms at 307.2 MHz.

## Game thread (the hooks' side)

1. **Device deltas instead of a whole device copy per draw** (`sa2_renut_snapshot_deltas`, `backend/queued.cpp`:
   `CaptureDelta`, `DeviceForDraw`, `DeltaChain`, `SnapshotDeltasNewFrame`, `ForgetSnapshotDeltas`).
   *Was:* every immediate draw (`QueueDraw`) copied the whole 13,088-byte device: `memmove` was 19 % of SA2's game
   thread. *Now:* the game thread keeps a shadow of the device and sends only the 64-byte chunks that differ from the
   previous draw, plus the float-constant groups `m_Pending[0..1]` marks dirty (8 KB of the device is constants). The
   render thread rebuilds the device in its own copy (stays in its cache). The chain is made and queued under one lock
   so the render thread applies deltas in order; each frame's first draw is a whole copy (`Rebase`), and Begin*Vertices
   draws (queued later than made) stay whole copies outside the chain. About 590 bytes a draw instead of 13,088.
   Verify: `sa2_renut_snapshot_deltas_verify` keeps the whole copy too and compares the rebuilt device byte for byte.
   *For N&B:* the layout constants come from `offsetof(d3d::Device, ...)` (`kDeltaVertexF`, `kDeltaPixelF`,
   `kDeltaTailBegin`), so they follow your device struct; check first how many of your draws are immediate
   (`QueueDraw`) rather than recorded command buffers, since only those use it. `ClearPending` hands the groups a whole
   capture consumed to the chain's next delta; `command_buffers.cpp`'s `NewOp` calls `ForgetSnapshotDeltas`, and
   `Swap` calls `SnapshotDeltasNewFrame`.
2. **Constants by the chain** (`sa2_renut_snapshot_deltas_constants`): the chain's draws go to `DrawStateLocked` as a
   `ReplayConstants` replay (id per whole copy, dirty groups since the upload last consumed them), so
   `PushFloatConstants` / `UploadFloatConstants` compare only changed groups, as they already did for recorded buffers.
3. **Narrow compare** (`sa2_renut_snapshot_deltas_narrow`): compare only the 40 of 77 chunks the render thread
   reads. *For N&B:* the unread ranges are SA2's (`kDeltaUnreadEnd` 0x480 = before the fetch constants;
   `kDeltaPrivateUnreadBegin/End` 11008-12223 = creation thread, fences, swap flags). Re-derive them for your device
   from what `DrawStateLocked` and its callees read, and check with the verify key.
4. **NEON line compare** (`sa2_renut_neon_compare`, new header `gpu/sa2_line_compare.h`: `Line64Differs`).
   *Was:* GCC left the eight-64-bit-word compare scalar (about 30 instructions a line; the chunk compare was 6.1 % of
   the game thread). *Now:* 4 x 128-bit loads a side, XOR, OR, one `vmaxvq_u32`. Used by the delta chunks and by the
   render thread's shared-constant lines (item 13).
5. **Capture pool in batches** (`sa2_renut_capture_batches`, `CapturePool::Take/Give` in `backend/queued.cpp`): the
   pool's lock was taken by the game thread every draw and by the render thread every return (its cache line bounced
   between cores). Now 32 at a time. **No thread-local storage** on that path: on the Switch every `thread_local`
   access is a function call; the lists are owned by role instead (`chain_taken_` under the chain's lock,
   `render_given_` on the render thread).
6. **Hooks' fast paths** (`sa2_renut_callbacks_fast`, `guest/callbacks.cpp/.h`, `backend/queued.cpp`,
   `guest/command_buffers.cpp`): every hook first ran `RunReadyCallbacks` (2 locks), `FlushDeferredVertices` and
   `FlushRecordedVertices` (1 lock each) to find nothing to do. Atomic counts kept with their queues
   (`g_waiting`, `g_deferred_count`, `g_pending_count`) let them return at once; the on/off flag is read inline
   (`HookFastPathsOn()` in `callbacks.h`) instead of a call per hook.
7. **Render thread woken in batches** (`sa2_renut_wake_batch` = 16, `backend/render_thread.cpp`: `Push(..., draw)`,
   `WakeBatch`, `WakeForWaitLocked`). Once the render thread is faster than the game thread it sleeps, and every draw
   woke it (a kernel call on both threads). Now a draw wakes it only when 16 are queued; any other item (frames,
   fences, resources), every wait for it (`Drain`, the frame wait, `WaitForFenceProgress`, which call
   `WakeForWaitLocked` first) and a 2 ms timed wait still wake it at once, so nothing can strand.
   This complements your `banjo_renut_queue_notify_idle` (notify only when it sleeps).
8. **Next item prefetched** (`sa2_renut_prefetch_next`, `render_thread.cpp`: `Item::prefetch`, `PrefetchItem`,
   `QueuePrefetched`): the next draw's capture and device bytes (written on the game thread's core) are prefetched
   while the render thread does the current one.

## Render thread

9. **Done signal only with a waiter** (`sa2_renut_done_notify_waiters`): `g_done_cv.notify_all()` after every batch
   was a kernel call with nobody waiting. Waiters count themselves under `g_mutex` (`g_done_waiters`); the signal goes
   only when there is one (the other direction of your queue_notify_idle).
10. **One clock read per batch** (`sa2_renut_batch_clock`) instead of per item (~1,800 items a frame).
11. **The chain's reuse** (`sa2_renut_chain_reuse`, `+_verify`; `render/draw.h`: `ChainHint`, `ChainChanged`;
    `render/draw.cpp`): the delta tells the render thread which device chunks changed since the previous draw, so a
    draw takes the previous draw's derived state when nothing it is made from changed: the pipeline key (`MakeKey`),
    the targets (`GatherTargets`, guarded by the surface epoch and each target's descriptor words), the viewport
    (`ComputeViewport`), the texture and sampler words (`FillTextures`: every slot's memo hit, same frame and epochs,
    each texture's header words; the hit's transitions replayed), the pipeline lookup (`PipelineLookupsLocked`,
    `PrebuildEpochLocked` in `render/pipeline.cpp`) and the framebuffer lookup (`SurfaceEpochLocked`,
    `FramebufferEpochLocked` in `render/surfaces.cpp`). Needs item 1.
12. **`PushFloatConstants` fast path:** in a tracked replay with none of the shader's groups dirty and every register
    it reads held, nothing to push: 8 word tests instead of the group loop. The prune loop is skipped when no group is
    unsure.
13. **Shared constants (1,360 bytes a draw):** lines compared inline instead of 21 `memcmp` calls
    (`sa2_renut_inline_line_compare`, then NEON from item 4); the texture lines (0-7) and the vertex-fetch slot lines
    (9-14) not compared when known equal (`sa2_renut_fetch_slots_skip`; `PushSharedConstants(cmd, sc, equal_lines)`);
    only the part after the texture words zeroed (`sa2_renut_shared_partial_clear`); a draw that fetches nothing by
    address keeps the fetch address 0 instead of a new upload each draw (`sa2_renut_fetch_data_unread`), so that line
    stops changing every draw.
14. **Smaller ones:** vertex streams walked by a mask (`sa2_renut_stream_mask`, `HostVertexLayout::used_mask` in
    `render/vertex_format.h`); default texture words as one block copy (`sa2_renut_texture_defaults_copy`); the per-draw
    diagnostic counts only while their 10 s lines are logged (`sa2_renut_lean_diagnostics`); the depth-only check's
    cheapest tests first (`DrawsWithoutPixelShader`).
15. **Next target (not done):** `PushSharedConstants` is still about 10 % of SA2's render thread, half of it comparing
    its 21 lines one at a time with a branch each; one pass building a 21-bit mask of differing lines, then pushing the
    runs, should halve it.

## Switch scheduling (SDK)

16. **Core layout** (`sa2_renut_core_layout`; SDK `system/xthread.cpp`: `RexSetGuestThreadCoreLayout`;
    `core/threading_switch.cpp/.h`: `RexSwitchSetThreadMaskOf`). On fewer than 6 cores guest thread affinity is
    ignored, so the game's drawing thread had mask 0x7 and time-sliced (10 ms slices at priority 0x3B) with other game
    threads, or landed under the render thread on core 1. At the first Swap: guest main thread pinned to core 0, every
    other guest thread (and later ones) on cores 1-2 preferring core 2. Two traps found on the console: the SDK's
    Switch thread registry was never filled, so only the calling thread could be re-masked (every XThread now registers
    itself at start: `RexSwitchRegisterCurrentThread`), and a plain mask makes a thread prefer its lowest core (core 1,
    the render thread's), so the preferred core is set explicitly.

## Store cards (your item, two follow-ups)

17. **Memexport results mark their cards** (`render/draw.cpp`, `MarkStoreCardsAt` after a memexport completes), so a
    later proof never trusts bytes the GPU wrote.
18. **Check your small-range card marking.** SA2's natives marked cards with the same page loop as your SDK's
    `MarkStoreCardsAt` (`sdk/include/rex/system/xmemory.h`, `for (page ...) cards[page] = 0`). GCC turns that loop into
    a `memset` call: njCalcPoint paid 3 calls a call, njMultiMatrix 12, about 20,000 a frame (`memset` 0.6 % of SA2's
    game thread). For ranges up to 4 KB mark the first and last page with two byte stores (SA2:
    `app/src/cpu/sa2_hot_common.h`, `Card`). If you change it in the SDK header, the generated code includes it
    (through `rex/ppc/function.h`), so that is a full rebuild.

## No Vulkan driver in a deko3d-only build (plume patch + SDK)

19. Only four Mesa symbols are linked by name: `vkGetInstanceProcAddr`, `vkDestroyInstance` (SDK
    `ui/vulkan/vulkan_instance.cpp`) and `vk_icdGetInstanceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion`
    (plume's Vulkan back end). Answering them with "no driver" drops the whole driver: SA2's NRO went from 58.2 MB to
    43.8 MB.
    - **plume** (`patches/plume-banjo-nx-c319b29.patch`, `app/thirdparty/plume/CMakeLists.txt`): option
      `PLUME_SWITCH_NO_NVK` skips the driver archive, its `-u` symbols and extras, and links only `zstd z nx`. The
      Vulkan back end still compiles and cannot start.
    - **SDK** (`sdk/cmake/rexglue_switch.cmake`): option `REXGLUE_SWITCH_NO_NVK`; when on, `rexglue_switch_nvk` links
      `zstd z nx stdc++ m` without `libvulkan.a`, and `rexglue_switch_startup` takes
      `sdk/src/ui/switch_no_vulkan.c` instead of `switch_elf_stubs.c`. That file defines the four entry points as
      failing stubs and **`posix_memalign`** (newlib declares it but does not define it; FFmpeg's `av_malloc` had been
      getting it from Mesa's archive, the only link error).
    - **App** (`app/cmake/sa2_renut_gpu.cmake`): passes `PLUME_SWITCH_NO_NVK = REXGLUE_SWITCH_NO_NVK`, and adds
      `SA2_SWITCH_NO_NVK=1` to two sources only (so nothing else recompiles): `banjo_renut_select.cpp` (`LatchActive`
      returns true) and `backend/backend.cpp` (`ResolveBackend` returns deko3d). Only for a build whose every renderer
      is deko3d: the native/ring renderer and the SDK's Vulkan presenter cannot start in it.
    - Checked: a scan of every object's undefined symbols against `libvulkan.a`'s definitions found only those four
      (plus libc/libgcc copies); the NRO boots, draws and runs the attract demo in Ryujinx.

## Order to port, by what it paid in SA2

1. Items 1-4 (deltas, constants by the chain, narrow, NEON): the game thread's biggest cost.
2. Items 9-13 (render thread): the pushed constants and the chain's reuse.
3. Items 5-8 (pool, fast paths, wake batch, prefetch).
4. Item 18 (check your card loop) and item 16 (core layout) if your threads have the same masks.
5. Item 19 when you want a deko3d-only NRO.
