# Making a deko3d renderer for a ReXGlue Switch port

This guide covers how sa2-nx went from a Vulkan renderer on NVK (up to 1.0.2.1) to Renut's Direct3D-level renderer on
deko3d, the Switch's own graphics API (1.0.3). It is written for the next port that wants the same, in the order of
work that kept the picture checkable at every step. It assumes the port already boots and plays on the PC
([porting-guide.md](porting-guide.md), sections 1-4).

The route is banjo-nx's (Banjo-Kazooie: Nuts & Bolts), which built the deko3d back end and the shader pipeline and
wrote them up for the sibling ports in `docs/deko3d-native-renderer-kit.md`. peggle2-nx made the shaders in the
installer page, and futari-nx added the frame latency probe and the deferred chain. This guide adds what SA2 needed on
top: a second XDK layout, a game whose speed follows its frame rate, and a release with no game shaders inside the
NRO. The full record, with every measurement, is [renut-renderer.md](renut-renderer.md).

## Contents

0. [What you end up with](#0-what-you-end-up-with)
1. [What to take, and the credits](#1-what-to-take-and-the-credits)
2. [Before you start: the census and the device layout](#2-before-you-start-the-census-and-the-device-layout)
3. [Step 1: the renderer on the PC](#3-step-1-the-renderer-on-the-pc)
4. [Step 2: shaders for deko3d](#4-step-2-shaders-for-deko3d)
5. [Step 3: deko3d, first in the emulator](#5-step-3-deko3d-first-in-the-emulator)
6. [Step 4: drop the Vulkan driver](#6-step-4-drop-the-vulkan-driver)
7. [Step 5: the console, and the render thread](#7-step-5-the-console-and-the-render-thread)
8. [Step 6: pacing and input latency](#8-step-6-pacing-and-input-latency)
9. [Step 7: picture options](#9-step-7-picture-options)
10. [Step 8: shipping without game shaders](#10-step-8-shipping-without-game-shaders)
11. [Proving every step](#11-proving-every-step)
12. [Traps](#12-traps)
13. [Checklist](#13-checklist)

---

## 0. What you end up with

```text
game's Direct3D calls (D3DDevice_*, Swap, KickOff, fences)
   --hooks-->  Renut's renderer (app/src/renut_engine/gpu): reads the device's own state, textures, streams, targets
   --queue-->  one render thread replays each draw on plume
   --plume-->  deko3d (the Switch)   |   Vulkan or D3D12 (the PC)
shaders: game containers --Renut's XenosRecomp--> HLSL --DXC--> SPIR-V --SPIRV-Cross--> GLSL --passes--> package
         --uam on the console, once per variant--> DKSH (kept in deko3d_shaders.bin)
```

There is no PM4 ring written for the GPU or parsed, no Xenos register file, no EDRAM model and no Vulkan driver. The
game's Direct3D runs as it is for everything that is not a GPU command (state setters, resources, locks), so the
device memory the game reads inline stays correct.

**Why deko3d instead of NVK.** deko3d writes the GPU's command words in user space with no validation and no
descriptor management. banjo-nx measured NVK's per-call costs on the A57 at about 3 µs a pipeline bind, 2.4-2.9 µs a
draw and 1.3-1.6 µs a descriptor or uniform buffer update. deko3d does not have this driver work. Dropping the driver
also took the NRO from 58.2 MB to 43.8 MB.

**What it gave SA2** (City Escape, 1020 MHz, about 1,400 draws a frame, the user's console):

| Build | Render thread | Game thread (Swap to Swap) |
|---|---|---|
| Renut on deko3d, first console run | 17.6 ms | 19.2 ms |
| After the render-thread work (section 7) | 11.1 ms | 15.5 ms |

A 60 FPS frame is 16.7 ms. The GPU needed 5-6 ms a frame at 460.8 MHz and 7-9 ms even at 307 MHz. The PGO profile of
a console session of the new renderer came after that.

## 1. What to take, and the credits

| Part | Where (sa2-nx) | Licence | How to take it |
|---|---|---|---|
| Renut's renderer | `app/src/renut_engine` | reNut by masterspike52 has no licence file. Its author permits reuse by the user's ports. banjo-nx's and sa2-nx's parts are GPL-3.0 | Retarget the hooks and the device offsets (sections 2-3). Credit reNut and masterspike52 |
| plume, with banjo-nx's deko3d back end | `app/thirdparty/plume` (`plume_deko3d*.{h,cpp}`) | MIT | As it is. `patches/plume-banjo-nx-c319b29.patch` holds SA2's few changes |
| uam, the deko3d shader compiler | `app/thirdparty/uam` | zlib (uam), MIT (Mesa's files), MPL-2.0 (SwitchWakerHD's patches) | As it is. Built for the Switch and as a PC tool |
| Renut's translator | `app/thirdparty/reblue-XenosRecomp` | MIT | As it is |
| The shader tools | `tools/deko3d_*.py`, `tools/renut_early_alpha.py`, `tools/early_alpha.py`, `tools/sa2_dump_shaders.py` | banjo-nx's, GPL-3.0 | The dump script is game-specific, the rest as they are |
| The browser chain | sa2-nx-installer `lib/deko3d*.js`, `wasm/renut_hlsl.*`, `wasm/spirv_cross.*` | GPL-3.0; SPIRV-Cross Apache-2.0 | From peggle2-nx-installer. Only the container finder is game-specific |
| SDK changes | `sdk/` (`patches/rexglue-sdk-v0.10.0.patch`) | BSD-3-Clause | The no-NVK switch, the input read hook, the core layout, the emulator start |

Every change to third-party code goes into `patches/` as a diff against the upstream it came from (`patches/README.md`),
and every project gets a line in `THIRD_PARTY_NOTICES.md` and in the release's `LICENSES.txt`.

## 2. Before you start: the census and the device layout

1. **Find your Direct3D version.** Every game links its own copy of the XDK's Direct3D. SA2 links 2.0.21076, N&B
   2.0.7645. The code is the same, but the device's private part moved.
2. **Find the functions by address.** Renut hooks about twenty Direct3D functions: device creation, the ring
   (`KickOff`, `KickOffSegment`, `BeginRingBig`, the segment and inline allocators), fences (`BlockOnFence`,
   `CBlocker::Check`, `IsFencePending`), `InsertCallback`, `SynchronizeToPresentationInterval`, `Swap`, the four draws
   (`DrawVertices`, `DrawIndexedVertices`, `DrawVerticesUP`, `BeginVertices`), `ClearF`, `Resolve`, shader
   registration (`XGRegisterVertexShader` / `PixelShader`), `CreateVertexDeclaration` and `D3DResource_Destroy`. Match
   them against N&B's (renut-renderer.md, "The hooks", lists SA2's and N&B's side by side) with
   `tools/d3d_match.py` and confirm each by its call layout.
3. **Map the device layout.** Read every offset Renut uses from your game's code, and write in the source which function
   it came from and N&B's value. SA2's against N&B's:
   - the public device, constants, packets and resource headers are byte for byte the same;
   - N&B's 0x2A88-0x2ABF block (fences, flags) is 128 bytes further on;
   - the bound objects (index buffer, targets, depth, streams, textures, shaders) are 376 bytes further on;
   - from 0x3400 on, 400-412 bytes further on;
   - the device is 0x6080 bytes, against N&B's 0x5E00.
   A wrong offset is not a crash but a wrong picture, so name the source of every one. Sonic & All-Stars Racing
   Transformed (21119) has SA2's layout.

## 3. Step 1: the renderer on the PC

Run Renut on plume's Vulkan or D3D12 back end first, with your current renderer still built in as the reference.

- **A boot key chooses the renderer** (`sa2_renderer = "xenos"` or `"renut"`; `banjo_renut_select.cpp`). Each hook
  is a dispatcher, so both renderers can run the same build and be compared.
- **Device creation without a GPU.** SA2's `Direct3D_CreateDevice` runs with device type 2 (no hardware). The hook
  then gives the title device everything the hardware path would have given it apart from the GPU: critical
  sections, `VdGlobalDevice`, a fence word, the timer frequency, the presentation parameters (the automatic back and
  front buffers) and `InitializeApiState`, in the hardware path's order.
- **The ring becomes a sink.** `KickOff` rewinds it. A batch's fence completes when the render thread has read the
  batch, and `InsertCallback` runs when the render thread reaches it. The game's own state setters keep writing the
  shadows the draws read, and Present, Clear and Resolve keep their device work: the hooks do the host part, then run
  the original.
- **Shaders by container hash.** The registration hooks run the original, then find the host shader by the hash of the
  container. Vertex declarations are found by object.
- **Direct calls.** The post-codegen step that turns calls into plain C++ calls (`tools/direct_calls.py`) must know
  the new hooks, or the hooked functions are called around them. SA2's first run drew nothing ("vertex shader not one
  the renderer knows"). The tool now reads `src/renut_engine` for `BANJO_RENUT_HOOK*(sub_X)`.
- **Frame pacing, if the game runs per frame** (`gpu/sa2_renut_pacing.cpp`). Without a ring and a vblank interrupt
  nothing paced SA2, and it ran at 300-1,900 FPS on the PC. Each Swap is now released at the game's own presentation
  interval, read from the device, on a 60 Hz grid. A late frame goes at once with relaxed vsync. Never decouple the game
  from its frame rate: the game's speed must keep following it.
- **What your game draws differently from N&B:**
  - **The window scissor.** Renut reads the window offset and scissor from the device's window packet. SA2's Direct3D
    writes them only into the command buffer, so a hook on that writer fills the packet.
  - **The display gamma ramp.** SA2 sets a non-identity ramp at boot and Swap writes it to the display. Without it,
    every frame was 12-16 levels lighter. The Swap hook hands the device's ramp to the present shader.
  - **Post-process precision.** `renut_post_process_precision = "Game"` keeps 8-bit post buffers, as on the console.
  - **The screen width.** N&B's widescreen code is not built. `guest/sa2_screen.cpp` gives SA2's 1280.
- **Captures.** Lockstep captures through Renut's own frame (`RequestFrameCapture` from the Swap hook) let two runs be
  compared frame by frame (section 11). Check boot, menus, gameplay, cutscenes and the 2-player modes against the
  reference renderer before going further.

## 4. Step 2: shaders for deko3d

uam takes GLSL, not SPIR-V. The chain is native first, as PC tools:

```sh
python tools/sa2_dump_shaders.py                   # the game's containers: the executable's and Shader/xbox360/obj
cmake --build out/pc                               # Renut's translator and its PC shader cache
BANJO_DEKO3D_SPIRV_DIR=out/deko3d_shaders/spirv XenosRecomp out/renut_shader_dump out/deko3d_shaders/spirv_pass.cpp shader_common.h
python tools/deko3d_shaders.py --spirv out/deko3d_shaders/spirv --out out/deko3d_shaders/glsl --no-uam
python tools/deko3d_glsl_opt.py --in out/deko3d_shaders/glsl/glsl --out out/deko3d_shaders/glsl_opt --check
python tools/deko3d_shader_package.py --glsl out/deko3d_shaders/glsl_opt --out app/out/renut_switch
python tools/renut_switch_shader_cache.py --pc out/pc
```

(renut-renderer.md, "Shaders for the Switch", has the exact command lines.)

- **Find every container.** SA2 has 103: some compiled into `default.xex` (Direct3D's own and the game's), the rest in
  its files (`Shader/xbox360/obj/*.wpu` and `*.wvu`). Use reNut's container check (`tools/sa2_dump_shaders.py`). Your
  installer will need the same rules (section 10).
- **Bindings.** Renut's shaders read textures and samplers bindlessly. uam has no descriptor indexing, so
  `deko3d_shaders.py` rewrites each access to a fixed binding and writes the map on the first line
  (`// banjo-dk ubo=... tex=...`). Float constants are three uniform buffers (vertex 0, pixel 1, shared 2), and fetch
  data is storage binding 15.
- **Specialization constants pick variants.** Renut's bool constants (ids 1000-1255), the alpha test function (903),
  vertex outputs (905), early hoist (907), the texture size buffer (908) and early depth (909) are defined at the top
  of the same GLSL. Each variant is one DKSH, keyed by the FNV-1a 64 of its text.
- **The passes** (`deko3d_glsl_opt.py`, banjo-nx's: `docs/deko3d-shader-optimisations.md` there gives each one's
  measured reason). `--check` compiles every output with the PC uam, and a shader that fails keeps its input text. All
  103 of SA2's shaders compile with them. Check each pass on your game in the emulator before trusting it: SA2 turns
  `banjo_deko3d_alpha_spec` off (see Traps).
- **The DKSH cache.** The console compiles a variant on the background core the first time a draw needs it, and keeps
  it in `deko3d_shaders.bin` beside the NRO. A run also writes its pipeline list. `tools/deko3d_variant_cache.py`
  builds the cache from such a list on the PC. The list and the cache must be synced to the card (`fsync`), or a game
  closed with HOME loses them and compiles the same variants at every start.
- **The `.incbin` package is not dependency-tracked.** Touch the `.cpp` that includes it after rebuilding it.

## 5. Step 3: deko3d, first in the emulator

- **The back end.** plume's deko3d back end (`CreateDeko3DInterface()`), with GLSL as the shader format, and Renut's
  `renut_gpu_backend = "deko3d"`. uam is linked into the NRO.
- **Ryujinx before the console.** banjo-nx's Ryujinx build runs deko3d NROs and showed every first-light bug, and
  RenderDoc works on it. Give each port its own data folder, and stop only your own instance (by process id).
- **An emulator starts homebrew without a loader.** Three SDK changes, all inert on a console:
  - with no `argv[0]`, the NRO's folder is its usual place on the SD (`REX_SWITCH_FALLBACK_EXE`);
  - the current-process pseudo-handle when the loader's is missing, the guest memory views mapped at commit and no
    page protection (the emulator delivers no data abort for lazy mapping);
  - the heap is all the available memory when there is no loader.
- **Scripted input** (`input_script` with `headless = true`) plays menus and stages in the emulator. The emulator's
  timings are not console timings, so use it for pictures and hangs only.

## 6. Step 4: drop the Vulkan driver

Once deko3d draws everything, the driver is dead weight. Only four of Mesa's symbols were linked by name:
`vkGetInstanceProcAddr` and `vkDestroyInstance` (the SDK's Vulkan instance code) and the ICD pair plume's Vulkan
back end starts from.

- `REXGLUE_SWITCH_NO_NVK` (SDK, `cmake/rexglue_switch.cmake`): `src/ui/switch_no_vulkan.c` answers those four with "no
  driver", and supplies `posix_memalign` (FFmpeg had been taking it from the driver's archive: the first link failed
  without it).
- `PLUME_SWITCH_NO_NVK`: plume drops its copy of the driver. Its Switch `VulkanInterface` returns no instance when
  volk finds no `vkCreateInstance`, instead of calling through null.
- That build runs Renut on deko3d whatever the settings say (`banjo_renut_select.cpp`).
- `tools/win/build_switch.ps1 -NoNvk` builds it. The build check's Mesa string test only applies to a build with the
  driver.

Result: no driver changes, nothing to recompile in Mesa, and an NRO 14 MB smaller.

## 7. Step 5: the console, and the render thread

Measure on the console with the SDK's profiler (stack samples, per-thread CPU time) and Renut's 10-second lines
(render thread, GPU, draws, device snapshots). The first SA2 run showed both threads at a whole core in City Escape:

- **Game thread:** 19.3 % was `memmove`, Renut's 13 KB copy of the whole device for every draw.
- **Render thread:** 24.3 % draw setup, 16.4 % content hashing, 14.9 % constant pushes, 13.1 % vertex streams.

What paid off, in order. Each item has its own key, is verified frame by frame, and has a verify mode where it can
diverge:

| Change | Key | What it does |
|---|---|---|
| Device deltas | `sa2_renut_snapshot_deltas` | A draw keeps only the 64-byte chunks of the device that changed since the previous draw, plus the dirty constant groups. The render thread rebuilds the device in its own copy. 590 bytes a draw instead of 13,088. Only the chunks the render thread reads are compared (`_narrow`) |
| Constants by the chain | `sa2_renut_snapshot_deltas_constants` | The constant uploads compare only the groups that changed |
| Store cards | `banjo_renut_store_cards` | Every guest store marks its 4 KB page (`tools/store_cards.py` on the generated code, and every other writer: the CRT hooks, file reads, hand-written natives). Cached vertex, index and texture copies are proven by the cards instead of hashed. Hashing went from 16 % to 0 |
| Chain reuse | `sa2_renut_chain_reuse` | A draw takes the previous draw's pipeline key, targets, viewport, textures, pipeline and framebuffer when nothing they are made from changed |
| NEON compares | `sa2_renut_neon_compare` | Device chunks and constant lines compared 64 bytes at a time |
| Batched wakes | `sa2_renut_wake_batch` | The sleeping render thread is woken once 16 draws are queued, not for every draw. Every wait for it still wakes it at once |
| Capture batches, fast callbacks, one clock read a batch, waiter-only done signals | `sa2_renut_capture_batches`, `_callbacks_fast`, `_batch_clock`, `_done_notify_waiters` | Fewer locks, kernel calls and clock reads per draw |
| Prefetch | `sa2_renut_prefetch_next` | The next draw's bytes are fetched into the cache while the current one is done |
| Deferred chain | `sa2_renut_deferred_chain` | Begin*Vertices draws join the delta chain (futari-nx) |
| Core layout | `sa2_renut_core_layout` | The game's main thread on core 0, the render thread on core 1, the other game threads on cores 1-2 preferring 2 |

Lessons that cost time:

- **Each thread sets its own core mask.** `svcSetThreadCoreMask` on another thread that is running elsewhere waits for
  it to leave its core, and with busy loader threads the caller could hang (4 of 10 emulator runs froze). The layout
  is now only recorded. Each guest thread applies it to itself at its next wait (`ApplyGuestCoreLayoutAtWait`).
- **Register every guest thread** with the SDK's Switch thread registry, or only the main thread moves.
- **Check the hand-written natives after adding store cards.** GCC turned `Card()`'s page loop into a `memset` call,
  about 20,000 calls a frame, until it was rewritten as two byte stores.
- **Memo keys must hold everything the result reads.** A missing field gives a wrong picture, not a slow one. A
  prebuild worker bumping an epoch between a lookup and its read made the reuse take the generic pipeline: read the
  epoch once, before the test.
- **`thread_local` is a function call on the Switch.** Keep it off hot paths.

## 8. Step 6: pacing and input latency

- **Late input** (`banjo_late_input`): the first controller read after a Swap waits by the slack the game thread had,
  so the game reads the pad as late as it can. SA2's SDK needed a read hook first (`rex/input/read_hook.h`, called at
  the start of `InputSystem::GetState`), or the key did nothing.
- **Display sync** (`banjo_display_sync`): the release of each frame follows the display's vsync event plus a
  measured phase. SA2's pacing takes its release time from it, and keeps its own 60 Hz grid when the refresh is not
  known yet.
- **Eager kicks** (`banjo_deko3d_eager_kicks`): the frame goes to the GPU in pieces of 100 draws, up to 6 a frame.
- **Double buffering** (`sa2_renut_double_buffer`, off): two swapchain images instead of three show a finished frame
  sooner, but a frame that misses a refresh waits a whole one. It is a player option, off by default, because
  2-player runs close to the budget.
- **Left off:** the latency trim (it starved the GPU with eager kicks on banjo-nx's console) and late input from the
  GPU queue (it measured false slack and cut frame rates; removed).

## 9. Step 7: picture options

- **MSAA** (`sa2_renut_msaa`: 0, 2, 4 or 8; 2 by default in 1.0.3, off from 1.0.3.1: on the console 2x cost a little
  too much CPU). SA2 makes every render target single-sample, so Renut's
  multisampling never ran. `BuildTarget` (`render/surfaces.cpp`) now gives the game's single-sample targets 2, 4 or 8
  host samples. The game's own resolves downsample them, and its genuinely multisampled surfaces stay as they are.
- **Resolution: 1920x1080 docked, 1280x720 handheld** (1.0.3.1, `sa2_renut_dock_resolution`). Three parts:
  - **The render scale follows the console's mode.** `LatchResolutionScaleLocked` (`render/surfaces.cpp`) reads the
    mode every frame (libnx's cached operation mode, Reverse-NX obeyed; the SDK's applet loop keeps it current) and
    takes the scale from the heights set for it (`sa2_render_height_docked` 1080, `_handheld` 720). banjo-nx's deko3d
    path forced 1x there; that is gone. The change applies at the next frame, and the targets are made again at the
    new scale.
  - **The swapchain takes the display's size.** On deko3d the window and swapchain were fixed at 1280x720 (libnx's
    default window, and display sync's own), so a 1080p frame would only be scaled down again. plume's
    `Deko3DSetSwapChainSize` asks for 1920x1080 docked: `needsResize` reports it, and `resize` destroys the old
    swapchain first, because `nwindowSetDimensions` is refused while buffers are registered with the window.
  - **Shaders without `scale1` and `fragscale`.** Those two passes fold the texture scale code to 1x. Without them,
    5 of SA2's 103 shaders change (the ones that read a texture's size for texel offsets: `(x >> 26u) & 7u`, then a
    divide by the scale). At 720p they divide whole-number sizes by 1.0, so they compute the same values. `fragscale`
    changes none of SA2's shaders. The installer page's pass list is in the build's manifest entry, so only the list
    changes. A package made with `scale1` (1.0.3's) is found at start, and the game then stays at 1280x720
    (`deko3d::PackageScales`).
  The shadow maps follow the render resolution (1.5x at 1080p), as on the PC.

## 10. Step 8: shipping without game shaders

The GLSL is translated from the game's own shaders, so it is game data and cannot ship in the NRO. 1.0.3 does what
peggle2-nx did:

- **The NRO keeps the shader table and drops the GLSL** (`SA2_RENUT_RELEASE_SHADERS`, `tools/renut_release_shaders.py`).
  The table has the hashes, fetch masks, vertex input formats and literal constants, which are numbers. The SPIR-V and
  the GLSL go.
- **The GLSL comes from `deko3d_glsl.bin` beside the NRO** (`banjo_renut_deko3d.cpp`). The format is a 32-byte
  `BDKG` header, a table of 56-byte entries sorted by hash (offset, sizes, stage, the bool constant mask), and one
  zstd frame per shader. Without a dictionary, the page writes raw blocks, so it needs no compressor. A missing file
  or shader is logged by name, telling the player the installer makes it.
- **The installer page makes the package** from the player's own copy (`lib/deko3d.js` of sa2-nx-installer):
  - it finds the containers with the dump script's rules (here: the decompressed `default.xex` and
    `Shader/xbox360/obj`);
  - Renut's translator (`wasm/renut_hlsl`) turns them into HLSL, with `#define BANJO_DEKO3D 1` and
    `#define RENUT_RECOMP 1` first;
  - DXC turns that into SPIR-V, and the vertex shaders' Y flip is taken back out (deko3d takes Direct3D's clip space
    as it is);
  - SPIRV-Cross (`wasm/spirv_cross`) turns it into GLSL;
  - the conversion and the build's passes run (`lib/deko3d_glsl.js`);
  - the package is written (`lib/deko3d_package.js`).
- **Pin the result.** The build's manifest entry holds the pass list, every shader's hash, every container's SHA-256
  and the package's SHA-256. The page refuses a package that differs. The browser's DXC (v2025.1) is not the PC's
  (1.8): 97 of SA2's 103 shaders came out byte for byte the native chain's, and the other 6 differ only in the numbers
  of SPIR-V temporaries. Use the pass list the native packages were tested with (1.0.3.1: the default list without
  `scale1` and `fragscale`). A shorter list gave 5 of 103 identical.
- **Test the NRO with the page's own package.** SA2's 1.0.3 was run with the page's `deko3d_glsl.bin`: 103 shaders
  loaded, uam compiling variants, the attract demo drawn right, no errors. Then `test/release_node.mjs --deko3d
  ... --no-library --write` fills in the manifest.
- **Build the release** with `tools/win/build_switch.ps1 -Pgo use -NoNvk -ReleaseShaders`, and check that no game GLSL
  strings are left in the NRO.
- **Settings.** The renderer choice, the back end and the reports go into the compiled-in defaults
  (`app/sa2_switch_defaults.toml`). The player's `sa2.toml` shows only what they may want to change (here also
  `sa2_renut_msaa` and `sa2_renut_double_buffer`).
- **Tell the players** that an update now needs the page (it makes `deko3d_glsl.bin`), that the first session can
  stutter while uam compiles, and that the old shader library and `cache/` folder can be deleted.

## 11. Proving every step

- **Lockstep frames** (sa2-nx's captures, one vblank per presented frame): the same frames from two runs, compared by
  hash. Each render-thread change above was checked with its key on and off (for example 469 of 469 frames identical
  with all of items 1-13 on against all off).
- **Verify keys** where a change can diverge: the deltas rebuild every device and compare it byte for byte (0 of
  about 2.8 million draws different), the store cards hash every proven copy as well (0 changed under clean cards),
  the chain reuse makes everything again and compares, and the pushed constants are checked in the emulator.
- **Shader passes by differential test** (banjo-nx's `tools/difftest`): every changed pixel shader, before and after,
  compiled by uam and run on random inputs, bit for bit the same.
- **Against the old renderer:** SA2's title screen in lockstep against the native renderer, with the gamma ramp: 0.066
  levels apart on average.
- **Look at the frames** before calling something a difference: wall-clock animations shift between runs.

## 12. Traps

- **Direct calls bypass new hooks** until the post-codegen tool knows them (section 3).
- **The gamma ramp:** a game with a non-identity ramp looks washed out without it.
- **`banjo_deko3d_alpha_spec` gave flat colours on SA2 in Ryujinx** (every textured surface as if at its smallest
  mip). It was bisected to the `alpha` pass alone, with the KillsPixels bit changing the early depth test. It is off
  for SA2 until a console run says otherwise. Bisect suspect passes with packages made by `--passes`.
- **Ryujinx follows a bindless handle only back to a constant buffer load.** A computed handle read zero, and the
  picture went black. Pass slots as raw bits.
- **The heap fragments around stage loads** with 16 MB guest thread stacks: keep them in a pool
  (`switch_thread_stack_pool`, 64).
- **A depth target smaller than its colour target** is a GPU exception on Maxwell. Log it if your game ever binds one.
- **Setting another thread's core mask can hang** (section 7).
- **The deko3d swapchain stays 1280x720 unless asked.** Rendering 1080p docked without resizing it gives nothing: the
  frame is scaled back down. Resize it with the window, after the old swapchain has let go of its buffers (section 9).
- **A second `viOpenDefaultDisplay` fails on the console.** Display sync must use libnx's default window.
- **The emulator and the PC are not timing evidence.** Only console profiles decide what stays.
- **Don't run several emulator instances at once on a full disk.** When the page file cannot grow, `CreateThread`
  fails and the run proves nothing.

## 13. Checklist

1. Census your Direct3D, map the device layout with a source for every offset.
2. Renut on plume Vulkan/D3D12 on the PC, behind a boot key, against your current renderer frame by frame.
3. Frame pacing if the game runs per frame. Gamma ramp, scissor, precision, screen width.
4. Dump every container. Translator, SPIRV-Cross, passes (`--check`), package. All shaders compile with uam.
5. plume's deko3d back end in Ryujinx, with the three no-loader SDK changes.
6. Drop NVK (`REXGLUE_SWITCH_NO_NVK`, `PLUME_SWITCH_NO_NVK`).
7. Console profile. Deltas, store cards, chain reuse, wake batching, core layout, each keyed and verified.
8. Late input with the SDK's read hook, display sync, eager kicks. Double buffer as an option.
9. MSAA on single-sample targets if you want it. 1080p docked: the scale from the console's mode, the swapchain at
   the display's size, the shaders without `scale1` and `fragscale`.
10. Release shaders: the table in the NRO, the GLSL from the installer page, pinned by SHA-256, tested with the page's
    own package. PGO from a console session of this renderer. Patches, notices and licences updated.
