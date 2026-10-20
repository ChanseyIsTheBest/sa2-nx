# Renut's Direct3D-level renderer on SA2 (sa2_renderer = "renut")

Started 2026-10-20. The renderer of banjo-nx's deko3d route (`D:\Banjo kazooie port\banjo-nx\docs\deko3d-native-renderer-kit.md`):
Renut's Direct3D-level renderer (reNut by masterspike52; its author permits the user's ports to reuse it; credited in
the README and THIRD_PARTY_NOTICES) as banjo-nx carries it, retargeted to SA2's Direct3D. It replaces ReXGlue's Xenos
GPU emulation: **no PM4 ring is written for the GPU or parsed, and there is no EDRAM model**. SA2's Direct3D calls
reach the renderer at the API level: the draws read the device's state (the shadows SA2's own Direct3D keeps), the
textures, streams and targets are read from guest memory and their headers, and a render thread replays the frame on
plume (Vulkan or D3D12 on the PC, banjo-nx's deko3d back end on the Switch).

The emulated GPU stays the default (`sa2_renderer = "xenos"`) until this renderer is verified frame by frame against
it. `sa2_renderer = "renut"` chooses it at boot (environment: `REX_SA2_RENDERER=renut`). The build links it with the
CMake option `SA2_RENUT_RENDERER` (on).

## Where it lives

| Part | Where |
|---|---|
| The renderer (Renut's, as banjo-nx carries it) | `app/src/renut_engine/gpu` (`backend`, `render`, `guest`, `overlay`, `d3d`) |
| banjo-nx's tuning around it (keys `banjo_*`, reports, checks) | `app/src/renut_engine/gpu/banjo_renut_*.cpp`, `display_sync.cpp`, `perf/` |
| SA2's layout and hooks | `d3d/d3d.h`, `d3d/d3d_native.h` (offsets), `d3d/hooks/{device,draw,shader,resource}.cpp` (the "sa2-nx" hook blocks at the end of each) |
| What banjo-nx's SDK / app has and SA2's does not | `app/src/renut_engine/sa2_compat` (enhancement switches mapped to SA2's masters, the frame latency probe, store cards off) |
| The boot choice | `banjo_renut_select.cpp` (`sa2_renderer`), `sa2_renut_yield.h` (sa2-nx's own hooks hand over) |
| App wiring | `app/src/sa2_app.h` (no SDK graphics system, the renderer's window, overlay drawer and capture) |
| Build | `app/cmake/sa2_renut_gpu.cmake` (from banjo-nx's `banjo_renut_gpu.cmake`), `src/renut_engine/gpu/gpu.cmake` (Renut's) |
| Third-party code | `app/thirdparty/plume` (with banjo-nx's deko3d back end), `reblue-XenosRecomp` (the translator), `uam` (the deko3d shader compiler), `imgui` |
| Shader input | `tools/sa2_dump_shaders.py` -> `out/renut_shader_dump` (103 containers: the executable's and `Shader/xbox360/obj`) |

## SA2's Direct3D against N&B's

SA2 links Direct3D 2.0.21076 (N&B 2.0.7645). The code is the same; the private part of the device moved. Every offset
in `d3d.h` / `d3d_native.h` / `device.cpp` names the SA2 function it was read from, with N&B's value in brackets:

- public device, constants, packets, resource / surface / texture headers: N&B's byte for byte;
- N&B's 0x2A88-0x2ABF block (fences, flags): +128 (fence completion 11024, next fence 11036, flags 11068-11072);
- the vertex declaration: 12240 (N&B 0x2E2C);
- the bound objects 0x3094-0x3400: +376 (index buffer 12812, targets 12816, depth 12832, streams 12836, strides 12904,
  textures 12920, pixel / vertex shader 13068 / 13072);
- 0x3400 on: +400-412 (the inline vertices' ring end 13844, command buffer 13920, the automatic depth / front / back
  buffers 15228 / 15232 / 15236, present fence 15344, swap count 16968, side buffer 17136, behaviour flags 24392);
- the device is 0x6080 bytes (N&B 0x5E00); the shader objects have N&B's layout (containers at +0x368 / +0x28).

Sonic & All-Stars Racing Transformed (21119) has the same layout (its session matched 634 of the 640 functions).

## The hooks (SA2 address, N&B's)

| Function | SA2 | N&B | What the hook does |
|---|---|---|---|
| Direct3D_CreateDevice | 8254FC10 | 8264DF28 | SA2's own no-hardware path, then the title device's setup without the GPU (below) |
| CDevice::KickOff / KickOffSegment | 8254EBB8 / 8254E830 | 82210AB0 / 822106C8 | the ring sink rewinds; the batch's fence completes when the render thread has read it |
| BeginRingBig, ring segment allocator | 8254EDF8, 8254DA90 | 821F7C18, - | the sink |
| RingBufferDeviceAllocate, AllocateInlineData | 8254D4B8, 8254D9A8 | 8222D3E0, 8222D240 | scratch handed out in a circle |
| SetRingBufferParameters | 8254EF40 | 82657508 | nothing (would replace the sink) |
| CDevice::BlockOnFence, CBlocker::Check, IsFencePending | 8254D6E8, 8254F400, 8254ECF8 | 8228F558, 8264DCD0, 8228F650 | fences complete on the render thread |
| InsertCallback | 8254EEE8 | 821FB688 | the callback runs when the render thread reaches it |
| SynchronizeToPresentationInterval | 825589D8 | 8228A748 | the swap is pending (its packets are the GPU's) |
| Swap | 82558E98 | 82287590 | the frame goes to the render thread; swap count and fence tags as SA2's |
| DrawVertices, DrawIndexedVertices, DrawVerticesUP, BeginVertices | 82565C60, 82566078, 82565C18, 82565758 | 8222C7A0, 826567C8, 8222E520, 8222CC50 | Renut's draws |
| ClearF | 82563978 | 822811A0 | the host clear, then SA2's ClearF for the device work |
| Resolve | 825656F8 | 8222D6C8 | the host resolve, then SA2's Resolve for the device work |
| XGRegisterVertexShader / PixelShader | 8255E658 / 8255DBF8 | 8264E7B8 / 8264E670 | SA2's, then the host shader by the container's hash |
| CreateVertexDeclaration | 8255E198 | 8264EA90 | SA2's, then the host declaration by object |
| D3DResource_Destroy | 8254A200 | 822084F0 | host objects released in order, then SA2's |

What stays SA2's own code: the state setters (they write the shadows the draws read), the resources (creation, locks,
references), Present (its Resolve, Swap and KickOff are hooked), and the device work of Clear and Resolve. N&B's hooks
did that work natively from N&B's disassembly (`d3d_native_state.cpp`, `d3d/hooks/state.cpp`); on SA2 those are not
used (`state.cpp` is not built), and their packets go into the ring sink.

Creation: SA2's Direct3D_CreateDevice runs with device type 2 (no hardware: SA2's own device block, XenonGpu_Initialize
and InitializeApiState), then the title device gets what SA2's hardware path (sub_8255CDA0) gives it apart from the
GPU: its critical sections, VdGlobalDevice, a fence word, the timer frequency, SA2's InitializePresentationParameters
(the automatic back buffer, and SA2's automatic front buffer, a texture the Present resolves into and Swap shows) and
InitializeApiState again, in the hardware path's order.

## What else it took

- **Post-codegen direct calls.** `tools/direct_calls.py` turns calls to functions it does not see hooked into
  `__imp__sub_X` (inlinable). Renut's new hooks were bypassed (the first run dropped every draw: "vertex shader not one
  the renderer knows"); the tool now reads `src/renut_engine` for its `BANJO_RENUT_HOOK*(sub_X)` macros only (the tree
  names N&B's addresses in constants and in files the build leaves out), and post-codegen was run again.
- **Frame pacing** (`gpu/sa2_renut_pacing.cpp`, key `sa2_renut_vblank_pacing`, on). On the console the vertical blank
  paces SA2 (after each Swap the GPU waits for the flip), and SA2's game speed follows its frame rate. Without a ring
  or a vblank interrupt the game ran uncapped (300-1,900 frames a second on the PC). Each Swap is now released at the
  game's own presentation interval (the device's copy of `D3DPRESENT_PARAMETERS::PresentationInterval`, +14000: ONE =
  60 Hz), on a fixed 60 Hz grid; a late frame goes at once with `sa2_vsync_relaxed` (the player's default) or at the
  next refresh without it.
- **What banjo-nx's SDK and app have** (`src/renut_engine/sa2_compat`): its enhancement switches (mapped to SA2's
  masters), its profiler window (none here), the frame latency probe (futari-nx's, app-side here), its store cards (SA2's
  generated code marks none: off, the renderer hashes), and fourteen `banjo_*` / `masseffect_*` keys its own renderer
  defines (same defaults).
- **SA2-specific defaults:** `renut_post_process_precision = "Game"` (8-bit post buffers as on the console; banjo-nx's
  default "High" changes the picture). `guest/sa2_screen.cpp`: the game's screen is 1280 wide (N&B's widescreen code
  is not built).
- **Captures** (`sa2_video_s`): from Renut's own frame (`renut::gpu::CaptureFrame`); the Swap hook counts the game
  frames the recorder waits on (the flip hook needs the vblank interrupt).
- **Lockstep captures** (`sa2_video_lockstep`, `sa2_video_every = 1`): the Swap hook asks for the capture of the frame
  it is about to queue (`RequestFrameCapture`, split from banjo-nx's `CaptureFrame`), waits for that frame's image and
  hands it to the recorder with its game frame (`sa2::capture::RenutLockstepFrame`), which labels it. Every frame is
  taken, none late: two Renut runs gave 583 of 600 frames byte-identical (the rest: the boot's timing).
- **The display gamma ramp** (key `sa2_renut_gamma_ramp`, on; `backend/present.cpp`, `shaders/host/present_ps.hlsl`).
  SA2's Direct3D sets the ramp once at boot through the display's curve (`VdGetCurrentDisplayGamma`: [64] = 193,
  [128] = 462 of 1023, not the identity) and keeps it in the device (`sub_825527D8`: +15408, 768 big-endian words;
  N&B's +0x3A9C), for the Swap to write into the display controller's table. Renut presented without it, as banjo-nx's
  FAQ says for its ports, and every frame came out 12-16 levels lighter than the emulated GPU's and the native
  renderer's (both apply it: DC_LUT registers). The Swap hook now hands the device's ramp to the renderer (a compare
  a frame; uploaded when it changes, a 256 x 1 float texture in a bindless slot) and the present shader looks it up as
  the native renderer's output pass does: each channel's 8-bit value picks its entry, the 10-bit value / 1023. The
  slot travels as the raw bits of the push constants' `param1` (0 = none) and is used as it is: Ryujinx follows a
  bindless handle only back to a constant buffer load, and the first version's computed handle (`uint(param1) - 1`)
  read zero there, a black picture (banjo-nx found the same with its LUT). Also in the present shader: `pow(x, 1)` is
  skipped (it could move a value by an ulp).

## Shaders for the Switch

    python tools/sa2_dump_shaders.py                      # out/renut_shader_dump (needs out/default_image.bin)
    cmake --build out/pc                                  # the translator and Renut's shader cache (PC)
    BANJO_DEKO3D_SPIRV_DIR=out/deko3d_shaders/spirv BANJO_HLSL_OVERRIDE=out/pc/renut_gpu/early_alpha_hlsl         out/pc/xenos_recomp/build/XenosRecomp/XenosRecomp.exe out/renut_shader_dump out/deko3d_shaders/spirv_pass.cpp         app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h
    python tools/deko3d_shaders.py --spirv out/deko3d_shaders/spirv --out out/deko3d_shaders/glsl --no-uam
    python tools/deko3d_glsl_opt.py --in out/deko3d_shaders/glsl/glsl --out out/deko3d_shaders/glsl_opt --check
    python tools/deko3d_shader_package.py --glsl out/deko3d_shaders/glsl_opt --out app/out/renut_switch
    python tools/renut_switch_shader_cache.py --pc out/pc

All 103 shaders compile with uam after banjo-nx's passes (none kept as they were). The PC tools (SPIRV-Cross and uam
for the PC) are banjo-nx's builds (`tools/deko3d_build_tools.sh`), in `out/deko3d_tools`. The Switch build then links
the GLSL package (191 KB) and the SPIR-V cache; uam compiles each variant on the console's background core on first
use, unless the DKSH cache next to the NRO (`deko3d_shaders.bin`) has it. That cache is made from the pipeline list a
run writes (Ryujinx's, so far):

    python tools/deko3d_variant_cache.py --list out/deko3d_shaders/pipeline_list_ryujinx.bin \
        --glsl out/deko3d_shaders/glsl_opt --out out/deko3d_shaders/deko3d_shaders.bin

(100 variants; with it the emulator compiled none in a run.)

## Ryujinx (the emulator tests)

banjo-nx's Ryubing build runs deko3d NROs (`D:\New folder\Ryubing-src`; each port has its own data folder: SA2's is
`D:\New folder\ryu-data\sa2`, started with `-r`, stopped by process id). The NRO is `sdcard\switch\sa2-nx\sa2.nro` with
the game as a junction to `assets\game_root` and the asset pack hard-linked (`asset_pack_build = false` in the test
`sa2.toml`, which also sets `sa2_renderer = "renut"`, `renut_gpu_backend = "deko3d"` and logging). An emulator starts
homebrew without a loader, which needed banjo-nx's three SDK changes, all inert on a console (hbloader passes argv and
the process handle, and its heap override is used as before):

- `sdk/src/core/filesystem_posix.cpp`: with no argv[0], the NRO's folder is its usual place on the SD
  (`REX_SWITCH_FALLBACK_EXE`, defined by `app/CMakeLists.txt` as `/switch/sa2-nx/sa2.nro`). Without it the first run
  read no `sa2.toml` and wrote its files to the SD's root.
- `sdk/src/core/guest_memory_switch.cpp`: the current-process pseudo-handle when the loader's is missing (the guest
  memory commit failed with 0xE401, invalid handle), the views mapped at commit (the emulator delivers no data abort
  for the lazy mapping) and no page protection.
- `sdk/src/ui/runtime_switch.cpp`: the heap is all the available memory without a loader (1 GB ran out in Ryujinx).

`patches/rexglue-sdk-v0.10.0.patch` has them.

Scripted input in the emulator (`input_script` with `headless = true`, `docs/switch-testing.md`; without headless
SA2 waits for a message box after opening the save): 2026-10-20 07:30-07:55, the file select, the menus, the stage map,
City Escape played, Chao World (the lobby, Chao Garden) and the Dark story's first event, on deko3d with the shaders
of banjo-nx's passes, all drawn right.

## banjo-nx's work after its commit c319b29, taken 2026-10-20

At the user's request ("use the shader optimisations from banjo + render thread optimisations"), banjo-nx's changes
since the commit SA2's copy came from, as banjo-nx listed them (emulator-checked there, not yet on its console):

- **Shader passes** (`tools/deko3d_glsl_opt.py`, banjo-nx's file as is): `deadcomp` (a component overwritten before
  any read is 0.0) and `guardsel` (a select whose other arm nothing reads is its chosen arm): 258 of SA2's 4,994
  selects gone. `earlyz` (constant 909: a pixel shader that discards tests depth first when its draw writes neither
  depth nor stencil; `render/pipeline.cpp` sets it, also for stencil tests that cannot write): declared in 93 of SA2's
  pixel shaders. `txq` (constant 908: `textureSize` from plume's size buffer instead of a texture query): 5 shaders.
  `aw4`, `bonesel`, `lightbranch`, `gpr32`, `nullps`: nothing in SA2's shaders matches (aw4, bonesel) or their
  shader lists are N&B's (the others; console A/Bs, off by default). All 103 shaders compile with uam.
- **uam** (banjo-nx's four changed files and its rebuilt PC tool): flag 1024 (128-bit loads of binding 15, aw4's;
  other shaders byte-identical), a per-compile GPR cap (gpr32); the DKSH cache tag is `bnx4` now, so the prebuilt
  cache (`deko3d_shaders.bin`) is rebuilt with the new tool.
- **Render thread / runtime** (`renut_engine`): the present-stall fix (`banjo_renut_stall_present`, off), the stale
  texture header rule (above), early-Z keys, the texture size buffer, the aw4 fetch alignment count ("[renut] fetch
  data: N of M draws ... off 16 bytes"), in-run A/B targets. Left out: `banjo_renut_game_gamma` (N&B's ramp at
  N&B's +0x3A9C; SA2's `sa2_renut_gamma_ramp` does it for SA2), and `banjo_renut_fur_shells` stays off (N&B's shader
  hashes, not exact).
- **Later the same morning** (banjo-nx's tree at 07:00, three-way merged): the pipeline list and the DKSH cache synced
  to the card (`fsync`; before, a game closed with HOME or an emulator stop lost the records, and the same shader
  variants were compiled again at every start; found by peggle2-nx), stale list records normalised to the current
  keys, uam's `checkSplitLoad` fix (a split 128-bit load lost its address; flag 1024's), plume logging each run-time
  compile, and the latency trim (`banjo_deko3d_latency_trim`, off: where the GPU is the limit, the render thread
  waits after a present until the GPU has all but the frame's last part, so the game reads its pad later; it stops
  for 60 frames when the GPU runs dry). In the emulator with it on: no hang, it held back 4-6 frames in 10 s and
  backed off (the emulator is render-thread bound). The frame latency probe's "input -> GPU done" with it.
- **Verified:** banjo-nx's differential test (`tools/difftest` there, a local copy in `out/difftest` with two
  helpers SA2's shaders need: `isnan`, `TextureSizeUnscale`): every changed pixel shader, before and after, with the
  default constants and with 909 = 1, compiled by uam, decompiled by Ryujinx's translator and run on random inputs,
  is bit for bit the same (60 trials each; one shader 20, past which a random input hits a division the harness does
  not model; the 5 txq shaders with 908 = 0, since the harness feeds the size buffer random words). The vertex
  shaders (5 changed: the same two transforms) by frames in the emulator. PC (D3D12, which the passes do not touch):
  the title screen in lockstep, 491 of 491 frame pairs byte-identical (the others fall outside the overlap).

## Found on the way (deko3d)

- **The window scissor.** Renut takes the window offset and scissor (registers 0x2080-0x2082) from the device's window
  packet (+0x28C0); SA2's Direct3D writes them only into the command buffer (sub_8254FCE8, which SetViewport,
  SetScissorRect, the target setters, Clear and Resolve call). sa2-nx's hook there (`src/me/sa2_me_hooks.cpp`) writes
  the packet when Renut's renderer is chosen, then runs the original.
- **`banjo_deko3d_alpha_spec` is off for SA2.** With it, every textured surface in Ryujinx came out one flat colour, as
  if sampled at its smallest mip (Sonic's quills beige, Shadow's limbs orange, the roads grey). Bisected with a test
  key that reads the GLSL package from the SD (`sa2_deko3d_glsl_package_file`; `tools/deko3d_glsl_opt.py --passes`
  makes the packages): the unoptimised package is right; of the 18 passes, `alpha` alone reproduces it, and the full
  optimised package is right with the key off (the generic variant reads the function at run time). uam's intermediate
  code for the two variants reads the same (the texture fetch first, the discard gone), so the difference is in the
  Maxwell code or in how the emulator runs it. banjo-nx's own captures in the same emulator are right with it on, so
  the cause is in SA2's shaders' variants; ruled out so far: the field the pass replaces (`v[34].w` is SA2's alpha
  test function too), stale DKSH entries (the cache key is the variant's whole source) and the early hoist
  (`banjo_deko3d_early_hoist` off: the same flat colours). uam's DKSH for a SA2 pixel shader with constant 903 = 8
  (always) against the generic one: the program headers differ in one bit, KillsPixels (off: the depth test runs
  before the shader), the input map is the same, the code is the generic one without the alpha test. So the shaders
  are as intended, and what changes is the early depth test, which the emulator runs differently for SA2's shaders
  than for N&B's. To check on the console (`banjo_deko3d_alpha_spec = true` at the top of sa2.toml) before turning
  it back on.
- **The display gamma ramp:** SA2's is not the identity, so Renut's frames were lighter than the console's; now applied
  at scan-out (above, "What else it took").
- **banjo-nx's colour-flash fix, taken 2026-10-20** (`banjo_renut_device_swizzle`, on; `render/textures.cpp`
  `BindTextureLocked`): Renut read a texture's description from its header in guest memory at render time, which a
  game may have rewritten since SetTexture (banjo-nx: a bloom level read as 1,1,0,0 turned a frame yellow). A header
  that describes the same data as the device's fetch constant but differs in what SetTexture copies unchanged (dwords
  0, 3, 5: signs, pitch, tiling; swizzle, number format, exponent adjust; dimension, mips) is stale, and the device's
  copy, what the GPU reads, is used; dword 4's mip levels differ legitimately (the sampler's mip range) and do not
  count. The 10 s report's "[flash] header vs device" and "[flash] device swizzle" lines count them. Note for SA2:
  SetTexture rewrites dword 5's mip address through a translation (`d3d_native_state.cpp`), so the dword-5 count
  says whether SA2's headers and fetch constants differ there routinely.
- **A depth target smaller than its colour target** (diagnostic, 2026-10-20): Renut sizes the draw area and scissor
  from the first colour target; a smaller depth target bound with it is undefined on D3D12 and a "3D WIDTH ZT
  Violation" GPU exception on Maxwell (this PC's GPU reset at 04:45 with exactly that, from some process). SA2 never
  bound such a pair in 10 minutes of the title, intro and attract demos; the first eight would be logged ("Draw binds
  a depth target smaller than its colour target").
- **The heap in long sessions** (Ryujinx, 9 minutes of attract demos, Renut's "Memory: heap" lines): the memory in use
  flat (~1.25 GB) but the heap taken growing 1.27 -> 2.68 GB, in steps at each stage load: fragmentation from the
  SDK's 16 MB guest thread stacks (55 at once at a load, 4 kept for reuse). `switch_thread_stack_pool` = 64 (SDK, both
  renderers; `docs/porting-guide.md` 7.4): flat after the first load.
- **A host trap, not the renderer:** at 04:11 the PC ran out of commit memory (several emulator instances; C: nearly
  full, so the page file could not grow). The game's `CreateThread` failed (C0000017) and it stayed on a loading screen
  for good, and an emulator start died ("the paging file is too small"). A test run then proves nothing.

## The console's first run, and the render thread (2026-10-20)

The user's first console run (the 08:36 NRO, `b410aad4`, 1020 MHz CPU, GPU 460.8 MHz, handheld; logs and
`rex_profile.log` kept in `out/renut_test/logs`, the symbolised per-thread profiles next to them):

| Scene | Draws a frame | fps | Render thread | GPU | Game thread (Swap to Swap) |
|---|---|---|---|---|---|
| Menus, title | 60-70 | 60 | 2.5-3.2 ms | 2.1-2.5 ms | 1.5-1.6 ms |
| Light play, attract | 250-650 | 60 | 4.2-8.8 ms | 3.5-5.5 ms | 7-11.6 ms |
| City Escape | 1,100-1,800 | 41-56 | 15-19.6 ms | 5.0-6.1 ms | 15.7-23.7 ms |

The render thread measured about 9 us a draw plus 2.6 ms a frame (19.1 ms at 1,788 draws, 7.3 ms at 510): banjo-nx's
figure held. In City Escape the game thread and the render thread were each about 100 % of a core; the GPU at most
6.1 ms (the user's idea of the 307.2 MHz GPU clock fits: about 9 ms or less). Where the time went, in the windows under
58 fps (pc samples):

- Game thread: 47.5 % game code, 8.4 % SA2's natives, **19.3 % `memmove`: Renut's whole-device copy for every draw**
  (13,088 bytes, `QueueDraw`), 7 % locks, 4.3 % Renut's capture and queueing.
- Render thread: 24.3 % draw setup (`DrawStateLocked` 17 %), 16.4 % content hashing (`PmullHash`: the vertex and index
  copies hashed again after every kick, about 600 checks and 270 KB a frame, and every bound texture's sample hash),
  14.9 % `PushFloatConstants`, 13.1 % vertex streams, 10.1 % textures and samplers, 6.2 % queue and locks, 4.1 % plume's
  deko3d calls, 2.2 % pipelines.

What followed (each keyed, verified on PC frame by frame):

1. **`sa2_renut_snapshot_deltas`** (`backend/queued.cpp`, on): SA2 records no command buffers, so every draw was a
   whole copy of the device. A draw now keeps the 64-byte chunks of the device that differ from the previous draw's
   (compared on the game thread with a shadow) and the float constant groups the game marked dirty (`m_Pending`, as the
   recorded command buffers keep them); the render thread rebuilds the device from them in a copy of its own, which
   stays in its cache. The chain is made and queued under one lock (the render thread applies it in queue order); each
   frame's first draw keeps the whole device again, and the Begin*Vertices draws (queued later than made) stay whole
   copies outside the chain. 590 bytes a draw on average instead of 13,088 (the 10 s "[renut] device snapshots" line).
   `sa2_renut_snapshot_deltas_verify` keeps the whole copy as well and compares every rebuilt device byte for byte:
   0 of about 740,000 draws differed (title, attract demos); the title's lockstep frames with and without the deltas:
   431 of 431 identical.
2. **`sa2_renut_snapshot_deltas_constants`** (on): the chain's draws go to `DrawStateLocked` as a replay of the constant
   uploads' tracking (`ReplayConstants`: an id per whole copy, the groups written since the upload last consumed them),
   so `PushFloatConstants` (deko3d) and `UploadFloatConstants` (PC) compare only the groups that changed, as for a
   recorded buffer's draws; before, an immediate draw compared every group its shaders read. Lockstep: 369 of 369
   frames identical, verify 0 differing.
3. Line tables (`-g1`) for the renderer's sources on the Switch (`cmake/sa2_renut_gpu.cmake`; GCC's code does not change
   with -g), so the next console profile resolves to lines.
4. **Store cards** (banjo-nx's CPU proposal 1, the 16 % of hashing): every guest store outside the stack also zeroes
   its 4 KB page's card (`tools/store_cards.py`, run last by `tools/post_codegen.py` on both trees and the localized
   copies; 179,700 stores marked, 105,700 stack stores left plain; `SA2_GEN_STORE_CARDS`, OFF = `REX_STORE_CARDS_OFF`),
   and so does every other writer of guest memory: the SDK's CRT memory hooks, `PhysicalHeap::TriggerCallbacks` (file
   reads into physical memory, decommits, releases), SA2's natives (`sa2::hot::Card`: memcpy / memmove / memset, the
   D3D constants copy, the Ninja matrix stores, the vertex chunks), Renut's memexport results. The cards are the first
   MB of the guest window (`Memory::Initialize`, read-write). `src/cpu/sa2_hot_guest.cpp` tells Renut the generated
   code marks them (`g_banjo_store_cards`), and `banjo_renut_store_cards` then proves the cached vertex, index and
   texture copies by them instead of hashing. PC, City Escape played with `banjo_renut_store_cards_verify` (every
   proven copy hashed as well): 412,760 copies proven in 10 s, 484 hashed (written pages), 0 changed under clean cards;
   textures 102,703 proven, 0 changed.
5. `sa2_renut_capture_batches`: the capture pool's lock was taken by the game thread for every draw and by the render
   thread for every return (two cores, the lock's cache line back and forth): 32 at a time now.
6. `sa2_renut_callbacks_fast`: every hook first ran `RunReadyCallbacks` (two locks) and `FlushDeferredVertices` (two
   more) to find nothing waiting; atomic counts kept with the queues now let them return at once.
7. `sa2_renut_core_layout` (the Switch, at the first Swap): the guest main thread (SA2's game and drawing thread, a
   whole core in City Escape) pinned to core 0, every other guest thread, now and later, on cores 1-2 preferring 2
   (the render thread is pinned to core 1): before, every guest thread could run on any core, and at their priority
   (0x3B) Horizon time-slices the threads of a core in 10 ms slices (`RexSetGuestThreadCoreLayout` in the SDK's
   `system/xthread.cpp`).
8. `sa2_renut_lean_diagnostics`: the per-draw counts only the 10 s lines read (the draw kinds, the fetch alignment
   census) only while those lines are logged; the draw and stream dumps' checks once instead of a call per draw.
9. The chain's hint (`ChainHint` in `render/draw.h`: the device chunks that changed since the chain's previous draw)
   lets a draw take the previous draw's derived state when nothing it is made from changed (`sa2_renut_chain_reuse`,
   with `sa2_renut_chain_reuse_verify` making it anyway and comparing): the pipeline key (`MakeKey`), the targets
   (`GatherTargets`, with the surface epoch and each target's descriptor words), the viewport (`ComputeViewport`), and
   the texture and sampler words (`FillTextures`: every bound slot's memo hit, the same frame and epochs, each
   texture's header words; the hit's transitions done again).
10. `PushFloatConstants`: within a tracked replay (the chain's draws) with none of the shader's groups dirty and every
    register it reads held, nothing to push: a few word tests instead of the loop over its groups.
11. `sa2_renut_inline_line_compare`: the pushed shared constants' 64-byte lines compared inline instead of 21 memcmp
    calls a draw (the render thread's memcmp of the profile).
12. `sa2_renut_done_notify_waiters`: the render thread signalled "work done" after every batch, a kernel call on the
    Switch even with nobody waiting; now only when a thread waits (the waits count themselves under the queue's lock).
13. `sa2_renut_batch_clock`: one clock read per batch of render-thread work instead of per item.

Verified on the PC together (2026-10-20 10:20): the title's lockstep frames with every key above on and every one off,
469 of 469 identical (and 371 of 371 against the run before any of them); City Escape played with every verify key
on: the deltas 0 of about 2.8 million draws different, the cards 0 changed, the chain reuse never different from what
was made again. The deko3d-only parts (the pushed constants, the line compare, the core layout) were checked in the
emulator with `banjo_deko3d_push_constants_verify` (below, Status).

## The next console runs (2026-10-20, packages 3-5)

The user's console runs, City Escape (1020 MHz, handheld), fitted at 1,400 draws a frame (logs and profiles in
`out/renut_test_pkg3` and `out/renut_test_pkg4`; each NRO's ELF in `out/nx/elf`):

| Package | Render thread | Game thread | What it added |
|---|---|---|---|
| 1 (`b410aad4`) | 17.6 ms | 19.2 ms | the renderer as above |
| 3 (`5b589620`) | 12.5 ms | 17.6 ms | items 1-13 |
| 4 (`c55e68a7`) | 11.5 ms | 17.3 ms | items 14-22 |

Content hashing went to 0 % with the store cards, the constants to 4.6 %. The game thread is the limit now: in
package 4, Renut's `QueueDraw` was 10.8 % of it (6.1 % the device chunk compare, which GCC had left scalar), the
locks 3.1 % (most of it the render queue's: the render thread, faster now, slept and was woken for nearly every
draw), `memmove` 2.8 % (not the device copies: 0 Begin*Vertices draws are whole copies, the 10 s line says so now).

14. `sa2_renut_snapshot_deltas_narrow`: the device chunks compared only where the render thread reads them (40 of
    77): not the dirty masks, ring pointers and call tables before the fetch constants, nor the private words
    11008-12223 (the hooks use those on the live device).
15. The hooks' fast-path flag read inline (`HookFastPathsOn`, `guest/callbacks.h`) instead of a call per hook.
16. `sa2_renut_core_layout` fixed: in package 3 only the main thread moved, since the SDK's Switch thread registry was
    never filled; every guest thread registers itself now (`RexSwitchRegisterCurrentThread` in `XThread`'s start), and
    the other game threads are re-masked through it (`RexSwitchSetThreadMaskOf`) to cores 1-2 preferring core 2 (a
    mask alone made them prefer core 1, the render thread's).
17. `sa2_renut_chain_reuse`, more: the pipeline lookup (with the pipeline list's lookups and the prebuild epoch) and
    the framebuffer lookup (surface and framebuffer epochs) skipped when the key and targets are the previous draw's.
18. The shared constants (1,360 bytes a draw): the texture lines and the vertex fetch slot lines not compared when
    they are known equal (`sa2_renut_fetch_slots_skip`), only the part after the texture words zeroed
    (`sa2_renut_shared_partial_clear`), and a draw that fetches nothing by address keeps the fetch address at 0
    (`sa2_renut_fetch_data_unread`), so that line stops changing every draw.
19. `sa2_renut_prefetch_next`: the next queued draw's capture and device bytes fetched into the render thread's cache
    while it does the current one.
20. `sa2_renut_stream_mask` (the vertex streams walked by the layout's used mask) and
    `sa2_renut_texture_defaults_copy` (the default texture words as one copy).
21. The capture pool without thread-local storage on its hot path (a call per use on the Switch).
22. The depth-only check's cheapest tests first; `PushFloatConstants`' no-op prune loop skipped.
23. `sa2_renut_neon_compare` (`sa2_line_compare.h`): the device chunks and the shared constants' lines compared 64
    bytes at a time with NEON (four 128-bit loads a side, XOR, OR, one horizontal max) instead of eight 64-bit words.
24. `sa2_renut_wake_batch` (16): a sleeping render thread is woken by the game's draws only once 16 are queued (every
    wake is a kernel call on both threads); every other item (frames, fences, resources), every wait for the render
    thread (`Drain`, the frame wait, fence progress) and a 2 ms timer still wake it at once. 1: every draw, as before.
25. No Vulkan driver in the Switch test build (`REXGLUE_SWITCH_NO_NVK`, the user's request): only four of Mesa's
    symbols were linked by name (`vkGetInstanceProcAddr` and `vkDestroyInstance` in the SDK's
    `ui/vulkan/vulkan_instance.cpp`, the ICD pair plume's Vulkan back end starts from); `sdk/src/ui/switch_no_vulkan.c`
    answers them with "no driver" (and supplies `posix_memalign`, which FFmpeg took from the driver's archive), plume's
    `PLUME_SWITCH_NO_NVK` drops its copy of the driver, and that build runs Renut on deko3d whatever `sa2_renderer` and
    `renut_gpu_backend` say. The NRO went from 58.2 MB to 43.8 MB. The release build keeps the driver (its renderers
    use Vulkan).

26. Found comparing the generated code with the hand-written natives (2026-10-20 12:50): since item 4, every guest
    store a hand-written native made marked its card through a call of the C library's `memset` (GCC turned
    `Card()`'s page loop in `src/cpu/sa2_hot_common.h` into one): 3 calls per njCalcPoint, 12 per njMultiMatrix, 1-2
    per constant write and memcpy, about 20,000 a frame in City Escape (`memset` 0.6 % of the game thread in package 4).
    A range of up to 4 KB now marks its first and last page with two byte stores. PC: `sa2_cpu_verify` no difference,
    lockstep 467 of 467 frames identical (package 6).

27. **Banjo's input latency set** (asked by banjo-nx's session 2026-10-20 13:20; the user's call: on by default),
    three-way merged from banjo-nx's working tree (`render_thread.cpp`, `video.cpp/.h`, `video_internal.h`,
    `present.cpp`, `display_sync.cpp`, `banjo_renut_tuning.cpp/.h`; base: `out/banjo_sync/merge0535`; tools:
    scratchpad `merge_banjo_latency.py`, `resolve_banjo_latency.py`):
    - **Late input, now really on.** `banjo_late_input` was on but did nothing in SA2: SA2's SDK never called the
      delay or the latency probe at the controller reads (banjo-nx's input system does). The SDK has a read hook now
      (`rex/input/read_hook.h`, called at the start of `InputSystem::GetState`), which Renut sets in
      `InstallLateInput` to `BeforeInputRead` + `NoteInputRead`. The "[latency]" line now has its input timings.
    - **`banjo_late_input_gpu`**: taken, then removed entirely the same day (the user's request through banjo-nx):
      on banjo-nx's console it found false slack where frames just hold their rate (its slack counts the delay it
      already applied, and the GPU queue's waits are seen late) and cut those scenes from 30 to 24 fps, for at most
      11 ms less lag; at a frame cap with spare GPU it measures nothing. Plain late input stays.
    - **`banjo_deko3d_eager_kicks`** (on in SA2): kick lists of 100 draws, up to 6 a frame, each submitted at its own
      kick. `banjo_deko3d_latency_trim` stays off (it starved the GPU on banjo-nx's console with eager kicks).
    - **`banjo_display_sync`** (on in SA2): the display's vsync thread, with banjo-nx's own `__nx_win_init` /
      `__nx_win_exit` / `nwindowGetDefault` (libnx's default window, the same calls, so the display can give its vsync
      event; a second `viOpenDefaultDisplay` fails on the console). SA2's pacing (`sa2_renut_pacing.cpp` `PaceSwap`)
      takes each release from `display_sync::NextRelease`: a display refresh plus the measured phase
      (`banjo_display_phase_auto`, at most `banjo_display_phase_max_us`), a late frame at once; fixed vsync and an
      unknown refresh (the PC, the first frames) keep SA2's own 60 Hz grid.
28. **`sa2_renut_double_buffer`** (off): a 2-image swapchain instead of 3 (`video.cpp`): a finished frame is shown at
    the next refresh instead of queueing behind one already waiting; off because a frame that misses a refresh then
    waits a whole one (split screen runs close to the budget). PC: 2 images, 475 of 475 lockstep frames identical.
30. The pipeline reuse's race (found by banjo-nx's `sa2_renut_chain_reuse_verify` run, 2026-10-20): a prebuild worker
    finishing a specialised pipeline between a draw's lookup and the later read of `PrebuildEpochLocked()` put its bump
    into the stored epoch, so the next draw reused the generic pipeline (same pixels: it reads the bools at run time).
    The epoch is now read once, before the test and the lookup. PC: verify 0 differences, 449 of 449 frames identical.
31. **`sa2_renut_deferred_chain`** (on; futari-nx's, passed on by peggle2-nx at the user's request, 2026-10-20):
    Begin*Vertices draws join the delta chain instead of staying whole copies. `QueueDeferredDraw` makes the delta under
    the chain's lock and `FlushDeferredVertices` queues it under the chain's lock too (chain first, then the deferred
    map); every hook flushes its device's waiting draw first, so the chain stays in order. A draw without scratch stays
    a whole capture outside the chain. SA2 hooks `D3DDevice_BeginVertices` but the title and attract demos never call
    it (the 10 s line's "Begin*Vertices draws as whole copies" was 0 before too): PC verify 0 of about 640,000 draws
    different.
32. **The core layout applied by each thread itself** (`sa2_renut_core_layout`, after banjo-nx's Ryujinx runs: 4 of 10
    froze at the first Swap with the layout on, none with it off). `svcSetThreadCoreMask` on another thread that is
    running elsewhere waits for it to leave its core; with the game's loader threads busy that wait could hang the
    caller. `RexSetGuestThreadCoreLayout` (SDK `system/xthread.cpp`) now only records the layout and a generation:
    the calling thread (the main thread, at its first Swap) pins itself at once, every other guest thread applies its
    own mask at its next wait (`ApplyGuestCoreLayoutAtWait`, called from `XObject::Wait`, `SignalAndWait`,
    `WaitMultiple` and `XThread::Delay`), and threads made later at their start, as before. No thread sets another's.
33. **`sa2_renut_msaa`** (2 by default: the user's call for the final build, 2026-10-20): SA2 makes every render target
    single-sample (all "x1 samples" in the logs), so Renut's multisampling (banjo-nx's, for N&B's own multisampled
    targets: the colour and depth resolves of multisampled surfaces, their readbacks, alpha to coverage) never ran in
    SA2. `BuildTarget` (`render/surfaces.cpp`) now gives the game's single-sample targets 2 (or 4 or 8: the key takes 0, 2, 4, 8) host samples, as the
    native renderer's `sa2_msaa` did; the game's resolves downsample them; its 4x surfaces (Direct3D's fast Clear
    aliases) stay as they are. PC, lockstep through the 2-player City Escape demo, 196 frame pairs on and off: channel
    means equal, 1.3 % of the pixels apart by more than 8 levels (the edges), the edges visibly smoother; no errors. 4x and
    8x the same way (1.8-2 % of the pixels, the edges; plume's deko3d back end maps 8 to DkMsMode_8x).
29. plume's Switch `VulkanInterface` returns without an instance when volk finds no `vkCreateInstance` (the no-NVK
    build's stubs), instead of calling `vkEnumerateInstanceExtensionProperties` through null (from banjo-nx, passed
    on by Peggle 2).

Verified on the PC (2026-10-20 12:15): the title's lockstep frames with the wake batching on and off, 447 of 447
identical, the delta verify key 0 of about 860,000 draws different; package 4's keys all on against all off, 468 of
468 identical.

## Status

- 2026-10-20 00:00, PC (D3D12, RTX 3060): boots to the title screen and plays the intro cutscene (the bridge, Shadow,
  Eggman, Sonic and Shadow before the moon) and the attract demo through Renut's renderer; the title screen matches the
  emulated GPU's in composition, colours and brightness (by eye: the two runs are not in lockstep). Render thread
  1-7 ms a frame, GPU 1-4 ms. With the pacing (high-resolution timer): 59.6-60.0 fps on the PC, also while loaded.
- 2026-10-20 01:50, Ryujinx (deko3d, the NRO from `out/nx-renut`): boots to the title screen, the menus' loading
  screens and the attract demo (City Escape in one and two players, Sonic and Shadow, the cutscenes) for 4-5 minutes
  without a crash; 103 shaders in the package, uam compiling variants on the background core (34 in a run, 0 failed);
  27-60 game fps in the emulator (not a console measurement). The pictures are right with `banjo_deko3d_alpha_spec`
  off (above).
- 2026-10-20 02:25, Ryujinx: a 9-minute run (the attract demo looping: City Escape, Pyramid Cave in two players, back
  to the ESRB screen) without a crash, 51-58 game fps in the emulator.
- 2026-10-20 02:40, PC, lockstep against sa2-nx's native renderer (`sa2_renderer_native`), with the gamma ramp: the
  title screen's 589 frame pairs differ by 0.066 levels on average (the most on any pixel: 5, on text edges; no pixel
  off by more than 8), channel means equal (71.2/57.8/59.1 against 71.3/57.8/59.2). Before the ramp: 12.8 levels on
  average, every pixel lighter. The intro cutscene's frames match the same way where both runs show the same moment
  (0.015-0.1 levels); its title texts and flares run on a clock of their own, so lockstep runs do not pair all of
  them (the camera and the models do).
- 2026-10-20 03:10, Ryujinx (deko3d), the ramp: the picture with the ramp equals the ramp applied to the picture
  without it (the SEGA logo, same moment of two runs: 0.16 levels apart on average, at most 1).
- 2026-10-20 02:50, PC: Chao World through Renut's renderer (the route of `chao_explore.ps1` on the copy of the save:
  the loading screen, Chao Lobby, the transporter, Chao Garden), 59-60 fps. Against the native renderer on the same
  route (inputs on a timer, so frames paired by content): 0.006-0.2 levels apart on average; the few pixels further
  off are things caught at another moment (the loading ring's spin, the garden's moving objects).
- 2026-10-20 03:25, PC: 10 minutes through Renut's renderer from boot, a frame every 3 s: the logos, the intro
  cutscenes, the title screen and every attract demo the game cycled through (City Escape, Pyramid Cave, the
  space-colony and Meteor Herd battles, the desert and jungle stages in two players): all drawn, 50-60 fps (the dips
  are loads), no crash.
- 2026-10-20 03:35, PC, play with scripted input (on the copy of the save, restored from the user's original before
  each run): the stage map, City Escape's mission card, loading and play; the Dark story's first event (Prison Island,
  with its Sofdec explosion and smoke overlays) on both renderers: the same pictures (the movie's frames, the
  subtitles, the scenes), the frames apart only where the event's real-time effects caught another moment.
- Console estimate (banjo-nx's console numbers for the same renderer, package 35 at 1020 MHz: about 9-12 us of render
  thread per draw plus about 5 ms a frame): the attract demos' 290-490 draws a frame would take about 8-11 ms of the
  render thread's 16.7, but City Escape played draws 1,100-1,400 times a frame (PC: render thread 2.3-2.7 ms, game
  thread 2.9-3.4 ms from Swap to Swap, GPU about 1 ms; 260 KB of vertex streams hashed a frame), about 16-19 ms: at or
  over a 60 fps frame. The GPU's share is not known for SA2. The 10 s "[renut] game thread" line (game thread from
  Swap to Swap, the pacing's sleep, late frames; `sa2_renut_pacing.cpp`) and the "[renut] 10.0 s" line (render thread,
  GPU, draws) measure it on the console.
- Threads on the Switch: the render thread on core 1 (`banjo_ring_core`; the native renderer's ring thread does not
  run), uam and the upload cleaner on core 2. Renut's record worker (`banjo_renut_record_worker`) is Vulkan only:
  `record::Wrap` hands deko3d the real command list, so the worker and its core key do nothing on the Switch
  (banjo-nx, 2026-10-20; the test package's core A/B was dropped).
- Docked: 1280x720 in both modes on deko3d (banjo-nx's shader passes `scale1`/`fragscale` assume the 1x scale; the
  console scales the picture to the TV), where the native renderer draws 1080p docked (`sa2_render_height_docked`).
- 2026-10-20, version 1.0.3 (the release): Renut's renderer on deko3d is the Switch build's only renderer
  (`sa2_renderer = "renut"`, `renut_gpu_backend = "deko3d"`, both in the hidden defaults). Built with
  `tools/win/build_switch.ps1 -Pgo use -NoNvk -ReleaseShaders` from `out/nx-renut-pgo`, with the PGO profile of a
  console session of this renderer (`pgo/`, 218 files): no Vulkan driver linked, no translated game GLSL in the NRO
  (the shader table with its hashes, masks and constants stays; the GLSL comes from `deko3d_glsl.bin`, which the
  installer page makes from the user's own copy, 97 of 103 shaders byte for byte the native chain's, 6 with other
  temporary numbers). `sa2.toml` shows only 1.0.2.1's settings plus `sa2_renut_double_buffer` and
  `sa2_renut_msaa` (2 by default). Ryujinx with the installer's `deko3d_glsl.bin`: 103 shaders loaded, uam compiling
  variants, the attract demo drawn right, no errors. The console run is the user's.
- Not done yet: a DKSH cache from the console's own pipeline list, and Chao Garden and the rest of the game beyond
  the attract demo on the console.
