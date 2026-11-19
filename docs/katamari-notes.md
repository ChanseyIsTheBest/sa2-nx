# Beautiful Katamari port: notes

> In the base: katamari-nx's notes, kept as the worked example of a port on this renderer (the base builds
> Katamari as checked in). Its console run breakdowns and progress log stay in
> [katamari-nx](https://github.com/ChanseyIsTheBest/katamari-nx)'s docs; what the base changed since is in
> [ultimate-fixes.md](ultimate-fixes.md) and [changelog.md](changelog.md).

This file records the port: what each step found, with its evidence. The project starts from banjo-nx (Banjo-Kazooie:
Nuts & Bolts, `D:\Banjo kazooie port\banjo-nx`, read-only): its SDK fork, app, tools, renderer and post-codegen
passes. banjo-nx's docs are the reference for everything taken from it; comments that say "banjo-nx", "sa2-nx",
"peggle2-nx" or "sasrt-nx" record findings from those ports.

The user's request (2026-10-30): port Beautiful Katamari to the PC and the Switch with ReXGlue, banjo-nx as the base,
the Switch the main goal; work uninterrupted until 20:00 AEST.

## Decisions (2026-10-30, made without asking, as the request said)

| What | Decision | Why |
|---|---|---|
| Game files | `D:\beautifulkatamari\Beautiful Katamari [NTSCU]\Beautiful Katamari [NTSCU].iso`, extracted to `D:\beautifulkatamari\game` | The request named `D:/kingdomkeflings`, but that folder holds A Kingdom for Keflings and another session's `keflings-nx` port (left alone). The Katamari ISO is in this session's working folder. |
| Port folder | `D:\beautifulkatamari\katamari-nx` (git, local commits only) | next to the game, like `D:\kingdomkeflings\keflings-nx` |
| Ryujinx data folder | `D:\New folder\ryu-data\katamari` | the port's own, as the request asks |
| `app/assets` | a junction to `D:\beautifulkatamari\game` | the manifest reads `assets/default.xex`; no second 2.7 GB copy on a D: drive with 7-20 GB free |
| Internal project name | `renut` (as banjo-nx) | banjo-nx's CMake, post-codegen passes and renderer glue name `renut_*` files and targets (the passes take a port's own project name too since port 186); the NRO, its title and the SD folder are katamari-nx |
| First boot | the SDK's emulated GPU (`gpu_plugin = "xenos"`), Renut compiled out | separates CPU / kernel problems from renderer problems (peggle2-nx's order of work); Renut's hooks are at N&B's addresses and must be moved first |
| Renut's hooks | compiled but not registered until moved (`KATAMARI_RENUT_HOOKS`, `banjo_renut_select.h`) | an N&B address is an unrelated function in this game |
| Banjo-only sources | not built: cheats / mid-asm hooks (`hooks.cpp`), title update, DLC, `banjo_me_hooks.cpp`, XAudio natives, hand natives, hot copies, shader harvest | each is written for N&B's addresses; `src/katamari_keys.cpp` defines the keys the kept files read (off) |
| Host shader tools | glslangValidator, SPIRV-Cross, uam copied from banjo-nx's `out/` | built from the same sources this tree carries (`tools/deko3d_build_tools.sh`) |
| Shipped `renut.toml` | 720 lines handheld, 1080 docked (the user's choice, 2026-10-31; rc4 and rc5 shipped 720 docked by mistake, fixed in rc6), no dynamic resolution, the game's MSAA (`disable_msaa = false`), shadow scale 1x, post-processing at the game's precision, no extra frame cap | the user decides what changes the picture; the Xbox 360's own 720 lines docked, banjo-nx's 648/864 lines, dynamic resolution and MSAA off are `configs/*.toml` (`tools/make_katamari_tomls.py`) |
| Four-core forwarder | program id `05D6EA1333487100` (banjo-nx's is `...7000`), NPDM `tools/forwarder/katamari_main.npdm` | its own id so the two NPDMs never collide; the user makes the forwarder; since rc6 off (the user: three cores): `banjo_forwarder_program_id = ""`, the package's NPDM only with `package_switch.ps1 -FourCore` |
| PGO + LTO | **deferred by the user** (14:55): the package is a normal `-O3` build (`app/out/switch`); PGO + LTO only for the final build, after more optimisation and the user's console tests, when the user says so | the user's message. The instrumented build (`app/out/switch-pgo-gen`, `-DBANJO_PGO=generate`) was stopped at 991 of 1044 steps and left as it is; `tools/build_switch.bat` takes `BUILD_DIR` and `CMAKE_EXTRA` for it (banjo-nx's docs/pgo-lto.md) |

## The game (2026-10-30)

- ISO: XGD2, 1,392 files, 9 directories, 2.68 GB (`tools/extract_iso.py`, from peggle2-nx).
- `default.xex`: title **4E4D07DC**, media `2C479157`, version 0.0.0.4, built 2007-09-02; base `0x82000000`, entry
  `0x824C1A28`, image `0x011F0000` (18.8 MB); AES + LZX. Code range (generated `REX_CODE_BASE / SIZE`)
  `0x82210000-0x82A05350`.
- XDK: every static library **2.0.5632** (XAPILIB, D3D9, D3DX9, XGRAPHC, XBOXKRNL, XNET, XAUD, XMEDIA, XONLINE, XHV,
  LIBCMT). Older than N&B's 7645 and Feeding Frenzy 2's 6534; between Feeding Frenzy 1's 2571 and FF2's 6534 in the
  table of the "XDK versions of our Xbox 360 ports" doc.
- Data: `d02.dat`-`d05.dat` archives (2.2 GB), WMV movies (`data/movie`, Japanese and US), `.nps` sound streams
  (`data/sound/bgm`, `data/sound/stream`), `alchemy.ini`, `$SystemUpdate`. No other code module in the data.

## Code generation (2026-10-30)

- Config: `app/renut_config.toml`, banjo-nx's codegen keys that need no per-game audit (CR / CTR / XER / reservation
  as locals, skip_msr, dform split, the fcti / fp-mode / copy / div / rotate forms, dcbt prefetch). The register
  diet (non_volatile_as_local, elide_lr_stores, non_argument_as_local) waits for this game's audits.
- **First run:** three "target not in any function", each a function reached only by a tail jump from a thunk
  (`app/config/renut_funcs.toml`, with the instructions): `0x825387E8`, `0x8266DEA0`, `0x82408488`.
- **Second run:** three "Unresolved conditional branch" to `li r3,0; blr` tails the scanner left outside their
  function: `0x822AC5E8` declared as its own function (the branch becomes a conditional tail call, same result);
  `0x82685520` (`cmplwi cr6,r4,0; bne cr6,0x82685530`, then the tail) declared over the whole body to `0x826855C8`
  (the generator writes no fall-through into a next function and refuses overlapping declarations: with the stub
  ending at the tail its r4 == 0 path returned without r3 = 0).
- **Pointer sweep** (`tools/pointer_fixpoint.py`, from peggle2-nx, adapted to this tree): pass 1, 18,307 gaps, 262
  candidates, **111 accepted** (vtables, callback tables, `lis/addi` callbacks; `app/overrides.toml`), rejected 127
  tail branches into function bodies, 15 without code, 4 beyond their gap, 4 without a terminator, 1 fragment; pass 2
  no new candidates (fixpoint).
  - **Seven of those were wrong** and are removed (`app/overrides.toml`, with the reason): their "built at" address was
    0x14 bytes before them, the `lis/addi` that builds a jump table's base right after its `bctr`; they were case code
    of their parent (the codegen then reported "Jump target ... unresolved at bctr" for 0x8230DCD8, 0x8240305C,
    0x8255BCD0, 0x82707584). A port taking this tool should drop "built at" candidates 0x14 after their build.
- **First boot stop:** "Call to invalid or unregistered function at guest address 0x82689770": a 10-instruction table
  accessor in a gap between two registered ones, reached through a pointer built elsewhere. **Gap sweep**
  (`tools/gap_sweep.py`, new): candidates are the first word of every gap after its padding and the word after each
  `blr` / `bctr` inside a gap, not within 0x800 bytes after a jump-table `bctr` (an indexed load feeding `mtctr`:
  the first try without that rule took 2-instruction switch cases for functions and split their parents); the
  checks are the pointer sweep's. **85 functions**, fixpoint after one pass; the codegen reports no unresolved branch.
- **Jump tables** (`tools/switch_check.py`, from peggle2-nx): 768 switches; truncated 0, wrong targets 0, missed 0;
  23 odd shapes generated as indirect jumps (to look at if one of them stops a run).
- **Second boot stop:** "exception C000001D" (the generated `__builtin_trap` of an out-of-range switch) in
  `sub_8222E438`: a dispatch through a table of four function starts (`0x82A196A8`) indexed by the word at `0x82B095E0`.
  The generator had made it a switch on the loaded *address* (r11) with cases 0-3. Fixed with a `[[switch_tables]]`
  entry on r10 (the index already times 4: the four targets at 0, 4, 8 and 12), `app/overrides.toml`. The other six
  "no bound in the code" tables of `switch_check.py` switch on their index registers (checked in the generated code).
- **setjmp / longjmp** by shape: setjmp `0x827EB5D0` (saves f14-f31 then r13-r31 into the buffer at r3, after a hook
  word check), longjmp `0x827EB3B0` (about 20 callers in 0x828A-0x829A, image decoders' error exits).
- 269 generated files.

## Renut's renderer at Katamari's addresses (2026-10-30)

banjo-nx's Renut is written for N&B's **title update 3** (XDK 2.0.7978): its hooks are at the update's addresses and
its device offsets are the update's (some differ from the base game's: IndexBuffer 12436 in the base, 12444 in the
update). Every comparison below is against banjo-nx's own generated tree (`D:\Banjo kazooie port\banjo-nx\app\generated`)
and image dump, never the base game.

### The hooks (`tools/renut_hook_remap_katamari.py`, `out/renut_map/remap_report.txt`)

322 of Renut's 354 hooks are at Beautiful Katamari's functions; the other 32 compile but are registered nowhere
(`BANJO_RENUT_UNHOOKED_*`, `banjo_renut_select.h`):

- **222 state functions from the state tables** (`tools/renut_state_tables.py`): N&B's records {getter, setter,
  default} at `0x82D8A948` (101 render states) and `0x82D8AE28` (20 sampler states); Katamari's are at **`0x82AD8E48`**
  and **`0x82AD9308`**, found by N&B's defaults at stride 12, every record paired by its default (same count, same
  order).
- **60 by field signature** (`tools/renut_sigmatch.py`: the multiset of instructions with registers removed, D-form
  displacements and immediates kept, imports by name; weighted Jaccard), score >= 0.75 and 0.05 ahead of the next.
  Examples: DrawVertices `sub_824D9D08` 0.90, DrawIndexedVertices `sub_824DA0F0` 0.87, Clear `sub_824F3370` 0.84,
  Resolve `sub_824F12B8` 0.78, SetVertexShader / SetPixelShader `sub_824D4930` / `sub_824D4630`, the lock and
  surface-description functions at 1.00.
- **40 by hand** (`tools/renut_hook_map_katamari.txt`, each with its evidence): CreateDevice `sub_824D0598` (allocates
  the 0x5700 device, calls the device init `sub_824E18D0`), Present `sub_824E9428` (calls
  SynchronizeToPresentationInterval, Resolve, KickOff, Swap), Swap `sub_824E8E90` (the only VdSwap caller),
  GetDeviceCaps `sub_824E6410` (copies 304 bytes from `0x820FC1F8`), SetGammaRamp / SetPWLGamma `sub_824DE218` /
  `sub_824DE2F8` (called in N&B's order by InitializePresentationParameters), the surface-level getters read in the
  image (GetCubeMapSurface `sub_824D2160` keeps the face argument), and the ring and fence functions.
- **Not hooked**: BeginExport / EndExport (XDK 2.0.5632 has no memexport), the command-buffer functions, 
  DrawIndexedVerticesUP, the one-instruction thunks, D3D_IsFusion, a few ambiguous ones. The XDK is linked
  statically, so a function with no counterpart is usually not in the image at all (the command-buffer recorder and
  runner have none: BeginCommandBuffer's best candidate is BeginTiling at 0.27). Two the game does use, checked by
  hand: EndIndexedVertices is `sub_824D9CE8` (`Ring = VerticesRingEnd`, then a kick unless Flags has 0x80): on
  Renut's devices, which carry the no-hardware flag 0x80, the game's code does exactly what Renut's hook does, so it
  stays unhooked; BeginIndexedVertices `sub_824D97B8` (0.85 against N&B's `sub_8263EDA0`, called once from
  `0x8271F7C8`) is not hooked in banjo-nx either.
- **Tiling: XDK 2.0.5632 keeps the full public forms** (N&B's update dropped the unused arguments).
  BeginTiling is **`sub_824EFBC8`** (it stores the count at TileCount and the targets at TilingSurfaces, where the
  zones below put N&B's fields; the game calls it at `0x8271E62C`): BeginTiling(Flags r4, Count r5, pTileRects r6,
  pClearColor r7, ClearZ f1, ClearStencil r9). It clears with ClearZ / ClearStencil and keeps them for EndTiling,
  stores Flags at +0x337C, sets Flags3 0x20 (and 0x10 with Flags bit 2) when Flags has bit 1, and skips its first
  clear when Flags has bit 0 or Flags3 has 0x20. EndTiling `sub_824F0150` (ResolveFlags r4, pResolveRects r5,
  pDestTexture r6, pClearColor r7, ClearZ f1, ClearStencil r9, pParameters r10) resolves every tile but the last
  with ResolveFlags | 0x100 (a colour target bound) | 0x200 (a depth surface bound) and BeginTiling's clear values,
  the last with the caller's values. Renut's two hooks became `BeginTilingImpl` / `EndTilingImpl` with those
  arguments; raw hooks read them from the registers (`d3d/hooks/draw.cpp`).
- The profiler's frame boundary is at Present (N&B's was its `appMainDraw` hook, not built here).

### The device layout (`tools/renut_field_diff.py`, `renut_field_pairs.py`, `renut_device_offsets_katamari.py`)

The displacements of 288 paired functions, N&B's update against Katamari:

| N&B update offsets | Katamari | Evidence |
|---|---|---|
| 0 .. 10943 | the same | ring, state-call tables, fetch constants, the register packets, fences, flags (158 pairs identical) |
| 10944 .. 11827 | -12 | 11012 -> 11000; VertexDeclaration 11824 -> 11812 (SetVertexDeclaration) |
| 11828 .. ~13470 | -16 | 11832 -> 11816 (XenonGpu_Initialize), the render-state block 11856.. -> 11840.., IndexBuffer / RenderTargets / DepthStencil / streams / textures / predication / tiling, VerticesRingEnd 13444 -> 13428, InKickOff 13232 -> 13216 |
| ~13490 .. 16800 | -24 | CommandBuffer 13516 -> 13492, PresentInterval 13596 -> 13572, the presentation copy 13544 -> 13520, auto surfaces 14824.. -> 14800.., ring fields 14836.. -> 14812.., critical sections 14944 / 14972 -> 14920 / 14948 (RtlInitializeCriticalSection calls), SwapCount 16560 -> 16536, 16708 -> 16684, SideBuffer 16728 -> 16704 |
| 21532 | 21508 | Swap |
| 21544 .. 21592 | -28 | display mode 21544 / 48 / 52 / 56 -> 21516 / 20 / 24 / 28 (InitializePresentationParameters, by source), the -1 words 21560 / 64 -> 21532 / 36 and perf frequency / period 21588 / 92 -> 21560 / 64 (the device init) |
| 21624 .. 21632 | -32 | Swap |
| 23348 .. 23956 | -1708 | the 41 -1 words 23380 -> 21672 (`li r11,41; addi r10,r31,23380` / `addi r11,r3,21672; li r10,41`), gamma 23952 / 56 -> 22244 / 48 |
| 23976 .. 23980 | -1716 | BehaviorFlags 23980 -> 22264 (InitializeApiState, XenonGpu_Initialize) |
| device size 0x5E00 | 0x5700 | Katamari allocates and clears 0x5700 (`li r3,22272`, `sub_824D0240`); N&B allocates 0x5E80, clears 0x5E00 |

165 lines of Renut rewritten (`out/renut_map/offsets_report.txt`); every literal fell in a zone. Three of them were
not fields but 64-byte chunk boundaries of the replay's delta compare and census (`backend/queued.cpp`
`kDeltaPrivateUnreadEnd`, two lines of `render/draw.cpp`: 11776), caught by queued.cpp's static assertions: put
back to 11776, still the last boundary before Katamari's vertex declaration (+11812).

### Guest data (`tools/renut_data_refs.py`)

| What | N&B update | Katamari | Evidence |
|---|---|---|---|
| XDK build qword (device +10880) | `0x82DA3478` | `0x82AD93F8` | holds 0x0002000016004000 (2.0.5632.0) |
| VdGlobalDevice import record | `0x820007F8` | `0x82000750` | both resolve to the same kernel variable (0xFFCAE000); D3D_LockResource, D3DResource_Destroy |
| D3DCAPS9 | `0x82161B30` | `0x820FC1F8` | GetDeviceCaps's 304-byte copy |
| full viewport | `0x82001744` | `0x820FAEB4` | EndTiling, D3D_SetSurfaceInfo |
| XPhysicalAlloc's no-large-pages flag | `0x82DA3778` | `0x8315DFE0` | Katamari's XPhysicalAlloc at `0x824C1C30` (same instructions) |
| format table | `0x82005200` | `0x820FA9D0` | seven D3D functions agree |
| HiZ owner | `0x82E108A0` | `0x8315E830` | CreateSurface, D3DResource_Destroy |
| XMemFree | `sub_822089F0` | `sub_824CAB98` | the hardware-device init frees with `lis r4,0xB1800000; li r3,0; bl` |
| D3D_IsFusion's word, the two export skip words | `0x82DC67D4`, `0x82E1A9E4/8` | none | not in XDK 2.0.5632: 0, and the reads guarded |

### First runs on the PC (2026-10-30, Renut on Vulkan)

- `renut2` (minimised, so no swapchain: no pacing, about 2,000 frames a second): 150 s, the generated-code
  self-checks 0 differences, XMA audio decoding, 16 draws dropped on the device's own declaration (fixed:
  `DeviceDeclarationLocked`, from peggle2-nx, at +0x2F98) and five run-time shaders captured, two of which pass the
  fog interpolator Renut's XenosRecomp did not declare (fixed: `{ Fog, 0 }` appended to its interpolators).
- `renut3` (visible window): 60 frames a second by vsync, render thread 0.3-0.5 ms and GPU 1.4-3.2 ms a frame, no
  draw dropped, no shader missing: the boot screen (the Prince on white), the save menu (its entries and the button
  prompts), the new-game confirmation dialog, all drawn as expected.

### What the game decides (porting-guide-renderer.md section 2; census of the menus, gameplay to follow)

| Question | Beautiful Katamari (menus) | N&B |
|---|---|---|
| Where draws come from | direct calls: 0 command-buffer recordings run; 83-84 % DrawVerticesUP | 98.5 % recorded command buffers |
| Where resources come from | 100 % of vertex streams from a Create hook; 100 % of bound textures from a Create hook or SetTextureHeader; no locks | 5 % / 26 % through creation calls |
| How state is set | through the hooked setters (87-88 % of draws change nearly every field group: UI draws) | 99.9 % hookable setters |
| Memexport | none (no export functions in XDK 2.0.5632) | nearly every draw |

Gameplay (emulator run `emu2`, the practice room with the scripted sticks rolling the katamari; one 10-s report):
180,228 draws, all immediate (0 replayed from recorded command buffers), 100 % on the fast draw path, 78.8 % drawn
from whole-buffer copies (with their base vertex), the texture slots answered 99.6 % by the slot's last or the memo,
mesh objects answering 17,734 draws, no depth-only draws, no memexport; about 450 draws a frame. Same picture as the
menus: direct calls, resources from Create hooks.

So the record-time items built for N&B's recorded buffers (resource / geometry objects at record time, the replay
stream, record lookups: A/B 45-50) have little or nothing to act on in the menus; the per-draw fast path, pipelines
by content, pushed constants and whole-buffer copies apply as they are.

### Shaders

The image holds 118 shader containers (92 vertex, 26 pixel; 111 unique by XXH3): `tools/renut_shader_dump.py` (from
peggle2-nx) seeded `out/renut_shader_dump` with them. Shaders the game makes at run time from its data are captured
by Renut on the PC (its capture folder) and added by the next build.

The Switch's inputs (`tools/switch_shaders.ps1`, banjo-nx's steps): Renut's XenosRecomp with the early-alpha pass and
the deko3d SPIR-V output, SPIRV-Cross to GLSL, banjo-nx's 27 GLSL passes, uam. First run on the 111: all compile
(86 vertex shaders, median 31 KB of DKSH, largest 154 KB; 25 pixel shaders, median 1 KB), the package 6 MB of GLSL
-> 518 KB (`app/out/renut_switch/deko3d_glsl.bin`), the Switch copy of the cache 23 KB + 527 KB of SPIR-V. banjo-nx's
hash-keyed passes (skin-once, light-branch, null-ps, gpr32 lists) name N&B's shaders and change none of these.

To do once Renut runs (from sasrt-nx, 2026-10-30): Renut writes no resolve result back to guest memory, so game code
that reads a GPU result on the CPU (auto-exposure, glare visibility) gets 0. sasrt-nx's fix is
`sasrt_renut_resolve_readback` ("D:\sonic transformed\sasrt-nx\tools\renut_patches\sasrt_edits.py"). Check whether
Katamari locks resolve destinations on the CPU (log Renut's `LockSurface` on resolve destinations).
**Answered (2026-10-30)**: `[katamari] texture lock` (resource.cpp) logs every READ-ONLY lock of a texture or
surface; in four emulator runs from boot to the practice room (one of 25 minutes) there were none (the only texture
locks are the uploads at load time, flags 0). Beautiful Katamari does not read GPU results on the CPU on that route:
sasrt-nx's resolve readback is not needed so far. plume's four
deko3d fixes of the same message (Zcull invalidate braces, MSAA sample positions, alpha-to-coverage dither,
primitive restart) are already in this tree's plume (copied from banjo-nx).

## Hot functions (2026-10-30, from the PC profile)

Profile: `out/pc_runs/renut4/pc_profile.log` (banjo-nx's PC sampler, 1000 Hz, seven 60-s windows of a visible run:
boot, save menu, new game, the King's intro, the controls tutorial). Ranked by
`<scratchpad>/hot_list.py` (each thread's top 30 per window summed; shares are of every busy sample of every thread).
In gameplay (windows 4-7) the main thread's top guest functions are a 4x4 matrix copy (5.2 % of all busy samples), a
VMX 4x4 matrix multiply (2.8 %), a bounding-volume visibility test (2.3 %), a scene-graph walk (2.2 %); the audio
thread's top is a mixer loop (0.9 %).

Hand-written natives (`app/src/cpu/hot/hand.txt`, code at the end of `banjo_hand_natives.h`), chosen where the
function is a pure data transform that can be written EXACTLY from the generated code (the guard compares them with
the generated original in verify mode):

| Function | What | Native |
|---|---|---|
| sub_82759C98 (rank 1) | 4x4 float matrix copy, 16 lfs / stfs | 64 bytes moved as they are; f0 as left; declines a signalling NaN (the generated widening quiets it) and a forward overlap |
| sub_827598C0 (rank 2) | 4x4 matrix multiply in place (vmsum4fp128 x 16) | the same simde dot products on locals; rows loaded / stored once; every register the original writes is written |
| sub_82759780 (rank 11) | the same multiply with three operands | the same template (first operand at r5) |
| sub_82759D20 (rank 20) | translation matrix from three floats | 16 words at once; 1.0 / 0.0 read from the image's constants |
| sub_827E7A40 | C runtime memset | banjo-nx's (sa2-nx's) native: the routine is the same 40 instructions |

Keys: `katamari_hand_matrix_copy`, `katamari_hand_matrix_multiply`, `katamari_hand_matrix_translation`,
`banjo_hand_memset`. Ranks 3-5 (the visibility test, two scene-graph walks with virtual calls) are left to the
machine-optimised copies: their value is in the register hand-over and the inlined callees, which the copy tool does.

Machine-optimised: `app/src/cpu/hot/localized.txt` lists the 5 hooks of the natives and the next 100 functions of the
profile (tools/localize_native.py: registers in locals, write-backs of changed registers only, small callees inlined,
direct dispatch). The bulk list stays empty: it is made from console profiles (tools/switch_hot_bulk.py).

## The Switch build in Ryujinx (2026-10-30)

First NRO (`app/out/switch/renut.nro`, 47.9 MB; deko3d only, NVK not linked), the user's patched Ryubing with this
port's own data folder (`D:\New folder\ryu-data\katamari`: banjo's system folders, keys and firmware; its own SD card),
`tools/run_ryujinx.ps1`, configs/renut_verify.toml plus the scripted pad and a capture every 15 s.

Run `emu1` (300 s, every Renut verify key on): boot, save probe, the story intro (sky and subtitles), the King's
throne scene, the first tutorial dialogue: every capture drawn as on the PC (compared with the PC run's captures at
the same scenes: the same layout, colours, the depth-of-field blur on the Prince, the dialogue box). 52-60 game frames
a second in the emulator after loading (the first 10 s 36 fps while the first shaders compile), render thread
1.6-1.9 ms a frame. Verify counters: store cards 0 changed under clean cards, fetch cache 0 differ, XMA frame cache
0 differ. Six `banjo_renut_fast_draw_verify` lines, all with the same pipeline KEY and a different pipeline object:
the kept pipeline was a provisional one (a stage still on its generic variant while uam compiles the specialised one
in the background; banjo-nx's docs/deko3d-renderer.md: the same pixels by construction). Fix for the package: the
prebuilt DKSH cache below, so nothing is provisional.

Found and fixed:
- 13 shaders the game builds at run time were not in the cache (draws with them were skipped until the next build):
  captured on the SD card, added to `out/renut_shader_dump` (now 129) and to the PC build's capture folder; the Switch
  shader package rebuilt (`tools/switch_shaders.ps1`: all 129 compile).
- The DKSH cache `deko3d_shaders.bin` (`tools/deko3d_variant_cache.py --generic --list <SD pipeline_list.bin>
  --runtime-log <emu1 log> --merge <SD cache>`): 206 pipelines, 181 stage variants, 3.1 MB, 0 failed. Shipped next to
  the NRO (it is compiled shader code made by this port's tools; it is not committed).

Saves: the game probes `SAVE:` before creating its save (the open fails, then XamContentCreate with mode 4 creates
it), as on the PC: not an error.

### Verified on the PC (2026-10-30, compare mode)

`out/pc_runs/hot1` and `hot2` (visible, the scripted pad into the tutorial; `banjo_hot_guard_calls = 4000`,
`banjo_hot_guard_period = 64`: each thread's first 4000 calls of every copy and then one in 64 run both the copy and
the generated original and compare every register, the flush mode and every byte written):

- hot1: 4,688,275 calls compared; 2 differences, both the flush mode (`fpscr.csr`) after the matrix copy and the
  translation-matrix natives: the post-codegen passes had made those originals' lfs / stfs pairs mode-free
  (`SingleToDoublePure` / `QuietSingleBits`), my first natives still switched the mode off. Fixed (they use the same
  pure widening and leave the mode alone).
- hot2 (after the fix): 93 copies reached, all 93 on, 3,927,559 calls compared, **0 differences** (one call differed
  once and not when repeated: inputs changed by another thread between the passes, which the guard does not count).

Three functions of the profile cannot be copies (an atomic compare-and-swap: state shared with other threads):
826A13D8, 82210630, 826E9E38; the next ones of the profile took their places.

### Gameplay comparison: Switch vs PC (2026-10-30; the blur FIXED in the evening, below)

Run `emu2` (Switch build, sticks wandering from 240 s) reached the practice room. Compared with PC runs of the same
script (`out/pc_runs/sticks2` on D3D12, `out/pc_runs/vk_sticks` on Vulkan):

- **Colours**: the Switch matches the PC on **Vulkan** (the brighter floor and walls); the PC on **D3D12** is darker
  and more saturated. The PC reference of this port is Renut on Vulkan (the request's step 1), so the Switch is right
  here; the D3D12 difference is a PC-only matter (D3D12 is not the reference back end; noted, not chased).
- **Blur (open)**: on the Switch the whole 3D scene (room, katamari, Prince) is soft, as if the game's depth of field
  had no focus; on both PC back ends gameplay is sharp. In the dialogue scenes both blur the background alike (the
  game's own depth of field). The game resolves its 4x-multisampled depth buffer in four 320-pixel tiles into a texture
  its depth-of-field pass reads (the trace: `resolve depth ... samples 4 -> texture ... at (320k, 0)`); Renut makes a
  float target of it (ResolveDepth, the multisampled variant). banjo-nx ships `disable_msaa = true`, so Renut's
  multisampled depth resolve on deko3d was never used there. Suspects: sampling a hardware-COMPRESSED multisampled
  depth target (`banjo_deko3d_depth_compression`, on), or the multisampled path itself. Two diagnostic emulator runs
  are prepared (`out/emu_nomsaa.toml`: MSAA off; `out/emu_nocomp.toml`: MSAA on, depth compression off); both were
  held by the free-commit-memory rule (other sessions' processes left 8-11 GB).

Diagnostics of the blur (2026-10-30 afternoon; each a 330-400 s emulator run to the practice room, same script):

| Run | Change | Blur |
|---|---|---|
| emu4_nocomp | `banjo_deko3d_depth_compression = false` | still there |
| emu3_nomsaa | `disable_msaa = true` | still there, but the key does nothing in this port (the run still resolved 4-sample depth): it acts through banjo-nx's mid-asm hooks at N&B's render-target creation, not built here. `configs/renut_nomsaa.toml` dropped from the package |
| emu5_nopass | the shader package without any of the 27 GLSL passes (all 129 compile) | still there |
| emu7_gpuoff | `banjo_noncpu_enhancements = false` (every renderer enhancement off) | still there |

So no banjo-nx optimisation causes it. The multisampled depth resolve's deko3d GLSL is right
(`texelFetch(sampler2DMS(...), coord, 0)`), and the same HLSL is right on the PC's Vulkan path. Left: plume's deko3d
handling of multisampled depth images, or the emulator's (Ryujinx's) sampling of them; the console decides
(PACKAGE_NOTES: "look at the practice room"). `emu9_opengl_long` (Ryujinx's OpenGL back end, 900 s to reach the
room): just as soft. So it is not one emulator back end's; it is this port's deko3d path or Ryujinx's GPU emulation
that both back ends share. Not separable without the console. If the console is sharp, nothing to fix in the port;
if it is soft too, next: compare the depth-of-field draws' bound textures between the PC and the Switch (a per-draw
trace of the post-processing), starting with the multisampled depth resolve's float target.

### The emulator A/B (2026-10-30)

`emu6_ab` (25 min, renut_ab.toml's 9 targets): the do-nothing control (target 1) differed by 2.4 fps / 0.9 ms of
render thread between its halves, as much as most targets; only the fast draw path (25) stood out (render thread
8.6 vs 9.8 ms). **The user (16:5x): the emulator is not for performance A/Bs**, only for the picture; the A/B is the
console's (renut_ab.toml in the package).

## Codegen options: the register diet (2026-10-30 evening, the user's request)

Every codegen option banjo-nx uses is now on. The three this morning left out needed this game's audits, done on a
codegen WITHOUT them (`out/audit/gen`, 55 s) and checked on one WITH them (`out/audit/gen2`):

- `non_volatile_as_local` (r14-r31, f14-f31, v14-v31 / v64-v127 as C++ locals; the `__savegprlr` / `__restgprlr`
  helpers and their guest-stack traffic go): `tools/read_before_write.py --noreturn=827EB3B0` must run on code
  generated WITH the option (it reads registers spelled as locals; my first run, on code without it, saw nothing, and
  the first diet build crashed at boot: sub_824CF3F8, a split tail of sub_824CEC10, computed its `__finally` funclet's
  frame from a zeroed local r31). On `out/audit/gen2`: 51 functions. 44 split pieces read non-volatiles their parent
  set up: `share_registers` (33 in config/renut_funcs.toml, 11 on their pointer-sweep entries in overrides.toml);
  setjmp and its four callers save CR / XER / registers into the jump buffer (the codegen's setjmp handling: unmarked,
  as banjo-nx); sub_82627520 / sub_82627B58 read v69 / v72, which nothing in the game sets: `non_volatile_as_local_keep
  = ["v69", "v72"]` (they read what is left there, as on the console). After the marks: only the five setjmp lines.
- `elide_lr_stores`: the link register is read by prologue saves, setjmp / longjmp (handled at their call sites) and
  five C runtime exception helpers that only save and restore it; the runtime never runs the guest's exception dispatch.
- `non_argument_as_local` (r0, r2, r11, r12, f0, v32-v63 as locals): `tools/non_argument_audit.py`: ENTRY 6,595 (218
  save / restore helpers, kept by name; 6,357 MSVC unwind funclets, `addi r31,r12,-N`; two functions taking r11; a
  vector codec family saving / restoring v32-v63 through sub_829E70A8 / sub_829E73F0), RET 14, HANDOVER 0. All 6,379
  ENTRY / RET functions are in `non_argument_as_local_keep`.
- With the options: `non_argument_audit.py --with-option`: 29,265 functions localize them, HANDOVER 0;
  `read_before_write.py --volatile`: 0.

Why it matters on ARM: in the Ryujinx profile of rc1 the save / restore helpers were ~20 % of the game's main thread.

## Hot functions on ARM (2026-10-30 evening)

Profile: a 15-minute Ryujinx session of the package rc1 (`out/emu_runs/emu10_profile/rex_profile.log`, the SDK's
1 kHz stack sampler on the ARM code; under Ryujinx it writes `sdmc:/switch/rex_profile.log`, one folder up, because
the emulator passes another program path), mapped with its ELF by `tools/switch_hot_bulk.py`. The main thread: host
memset 12 %, the register save / restore helpers ~20 %, localized copies 41 %, generated code 16 %. The ARM ranking
differs from the PC's: memcpy (sub_827E6F50) is the second-hottest guest function (1,371 samples), the vector memcpy
sub_824BFE50 third (1,230); the PC's matrix compare and visibility test barely run.

- Machine-optimised: `localized_bulk.txt` = every generated function with 2+ ARM samples: 218 (31 tiny leaves and 11
  forms the copy tool refuses left out), 217 after the string hash became a hand-written native; with `localized.txt`'s
  107, 324 functions run as localized copies.
- Hand-written natives added (`hand.txt`): memcpy (banjo-nx's native: the same contract, `std r3,-8(r1)` first and
  `ld r3,-8(r1)` at its four returns), the string hash sub_8266AF60 (Jenkins' one-at-a-time), the 4-float parameter set
  sub_8271FAD0, the word copy sub_827E73A8 (also inlined into the vector memcpy's copy), the identity matrix
  sub_827595D0; plus the matrix comparison sub_82759A00 (hot on the PC). The vector memcpy itself stays a machine copy:
  it builds a frame and calls two copy routines whose stack temporaries a native would have to reproduce byte for byte.
- The natives follow the register-diet codegen: each writes back exactly the registers its processed original writes
  back (r11, f0, v29-v31 are locals now: the first natives no longer write them).

### The depth-of-field blur: found with RenderDoc and fixed (2026-10-30 evening)

The user: the emulator should be accurate; use RenderDoc. `tools/run_ryujinx_rdc.ps1` runs Ryujinx with RenderDoc
1.46's Vulkan layer (an explicit layer: `VK_LAYER_PATH` + `VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture`; the enable
variable alone left `rdc_trigger.py` with no target) and takes captures by time. In the capture of the practice room
(`out/emu_runs/rdc_emu2`, `tools/rdc_passes.py`, `tools/rdc_bindings.py`):

- the scene's 4x depth target (D32S8 on the host, Renut's D24FS8: 20e4 float depth, "greater" test) held 0.04-0.56;
- the depth resolve's four tile draws into the R32F float target wrote 0.0 everywhere, and their pixel shader had no
  texture bound at all, while every game draw showed its bindless texture;
- Ryujinx's log: "Shader translator: Failed to find handle source for bindless access of type texture2DMS". Its
  BindlessElimination follows a texture operation's handle back to a constant buffer; for the multisampled bindless
  fetch (TLD.MS: handle and sample index from the same register sequence) it cannot, and it replaces the fetch with 0.
  The bindless 2D fetches of the other host shaders are followed.

So the depth of field read depth 0 everywhere and blurred the scene. The fix (commit 32330443): host shaders'
multisampled textures are BOUND textures on deko3d (`tools/deko3d_glsl.py`: `dk_ms_b<k>`; uam then emits the
multisampled TLDS.LZ form, which Ryujinx decodes), `plume::Deko3DBindFragmentTextures` binds them by descriptor slot,
and Renut's ResolveDepth binds the depth target and its stencil view for the multisampled variant. After it: no
warning, and the practice room is sharp in Ryujinx (`out/emu_runs/dof_fix1`), as on the PC's Vulkan build, the far
background softened as the game intends. banjo-nx (MSAA off) does not hit it; the fix was sent to its session.

## DLC (2026-10-31)

The user's 16 DLC packs (`Beautiful_Katamari.rar`: Xbox 360 `LIVE` packages, title 4E4D07DC, content type 2, in the
usual `content/0000000000000000/4E4D07DC/00000002/` layout; extracted to `D:\beautifulkatamari\dlc`, flat). The SDK's
content manager lists marketplace content from `<user data>/renut/0000000000000000/4E4D07DC/00000002/` as unpacked
folders; reNut upstream's installer (`renut_engine/dlc_install.cpp`, game-agnostic: the running title's type-2 packages
only; banjo-nx uses it) unpacks the packages in a `dlc` folder next to the NRO at the first start
(`ContentManager::InstallContent`: the STFS package mounted and extracted, the display name kept). Wired back into
`renut_app.h` (OnPostSetup, OnConfigurePaths, the PC path wizard). Ryujinx run `dlc1`: 16 installed, the game's
enumeration lists 16 and opens each one (`XamContentCreate ... type 00000002 ... open`), no error. What each pack adds in
game was not walked through with the scripted pad.

## The first console test, rc3 (2026-10-31): movies, crash, a wider scan, profile

The user's logs (in the rc2 folder, but written by rc3: the DLC installs are in them) and a phone video of the
movies. Symbolised against `builds/2026-10-31_0030_rc3_dlc_full/katamari-nx.elf`; the breakdown is
[katamari-nx's docs/console-rc3-breakdown.md](https://github.com/ChanseyIsTheBest/katamari-nx/blob/main/docs/console-rc3-breakdown.md) (ReverseNX docked mode with handheld clocks: 1080p on the 460 MHz GPU, gameplay
GPU-bound at 24-40 ms a frame).

**Movies decoded into green blocks (fixed).** Ryujinx shows the same (run `movie1`, captures every 3 s: the opening
movie broken by 20 s in), so it is not the hardware. Bisected in the emulator: CPU enhancements off (`movie_cpuoff`):
still broken; renderer enhancements off (`movie_noncpuoff`): still broken; rc1's NRO (`movie_rc1`): clean. Between rc1
and rc2 the register diet came in. The movies' codec keeps constants in VMX128 registers across its calls:
`sub_829E70A8` saves v32-v127 into a buffer and loads nine constant vectors into v64-v72 from a table, its callers
(`sub_829E3628` ... `sub_829E6CF0`, `sub_825EADF8`, `sub_82A01D88`; reached through pointers) run the decoder, and
`sub_829E73F0` restores v32-v127. With `non_volatile_as_local` those nine loads went into the helper's own locals and
died with it; only v69 and v72 (already in `non_volatile_as_local_keep`) reached the decoder. Fix: the keep list is
`["v64-v127"]` (`app/renut_config.toml`): only about 40 functions name a register above v31, so it costs nothing that
shows, and their saves / restores of v64-v127 are real again, as on the Xbox 360. (A checker of locals read before
they are written, `local_rbw.py` in the session scratchpad, found the helper's 62 reads of unwritten v-locals; the
audits of 2026-10-30 had kept v69 / v72 for the same codec without seeing the other seven.)

**Crash leaving the collection menu (fixed).** `[FATAL] Call to invalid or unregistered function at guest address
0x823700C8`: a 4-instruction vcall thunk (`lwz r12,0(r3); lwz r11,36(r12); mtctr r11; bctr`) next to three that were
registered. Its address is built 9 instructions after its `lis` (0x8237068C / 0x823706B0), past the 8-instruction
window of `tools/pointer_fixpoint.py` (now 32), and the gap sweep skipped it in a jump table's 2 KB shadow. Registered
in `app/overrides.toml`.

**Could there be more?** Three wider scans of the image, all for addresses that can be called through a pointer but
are not function starts: (1) every `lis` / `addi` pair inside a function with no instruction window; (2) every aligned
data word into the code; (3) every vcall-thunk shape with any registers. Only 0x823700C8 is new. The rest are switch
tables (absolute case addresses after a `bctr`), the C++ exception tables' (address, state) pairs and `__except`
continuations (reached only by the exception dispatcher, which this runtime does not run), and tail or epilogue code
inside registered functions. A call through a pointer computed some other way can still only be found at run time
(`tools/runtime_gap_resolver.py`).

**The buzzing in the movies (the same bug).** The game's mixer output (`audio_dump_s`, the six channels before the
host driver) over the opening movie, 6 s before it to 23 s in, in Ryujinx: rc3 turns into one steady 345 Hz tone 10 s
into the movie (the strongest spectral line 85,000-99,000 times the band's median, every second to the end of the
dump); with the codec fix the same seconds are ordinary audio (no line above 140 times the median, as rc1 over the
same movie). The movies' sound goes through the same codec helpers (`sub_825EADF8`'s family). No dropouts, clicks or
clipping in any dump. `out/audio_cmp/`, `audio_stats.py` in the session scratchpad.

**Machine copies from the console profile.** `tools/switch_hot_bulk.py` on the rc3 console profile (with rc3's ELF)
and the rc1 Ryujinx profile, every function with one sample or more: 832 bulk copies (217 before; tiny leaves the
generator inlines and the forms the copy tool refuses left out), 827 after seven became natives. The copy tool's own
register handling is the write-back the user asked about: registers in locals, written back at a return only when the
function changed them, handed over at a call only when the callee can reach them. Ryujinx run `rc4pre_verify` with
the guard on every call (`banjo_hot_guard_calls = 4000`, `banjo_hot_guard_period = 64`): 2,146,377 calls compared, 662
of the 832 bulk copies reached, 0 differences.

## Seven more natives (2026-10-31, the user's request: "hand optimise the top 7 functions of your choice")

From the rc3 console profile. The hottest functions overall are not candidates: `sub_82277E70` (the main thread's
14 %) is mostly the matrix natives it inlines, `sub_8221BC58` / `sub_822683D0` / `sub_82317F48` call other guest
functions, `sub_826707B0` / `sub_826A13D8` are interlocked reference counts (the guard's roll-back would be seen by
other threads), XThread0080's `sub_82539A88` / `sub_82536F90` / `sub_82541A50` are the game's audio engine, and
`sub_8221BF20` (the collision ray cast, 4.5 %) round-trips dozens of temporaries through the stack that a native would
have to leave as they end. The seven are the hottest pure-data leaves, each written from its generated statements in
`app/src/cpu/hot/banjo_hand_natives.h` (the end of the file), banjo-nx's rule for scratch (volatile registers the
original leaves behind are not reproduced; hand.txt names those that are):

| Function | What | What the native removes |
|---|---|---|
| `sub_82316A78` | job threads' sphere test against a box | the four or five flush-mode switches a call (scalar steps through the mode-free helpers, vector steps checked for denormals; at most one switch) |
| `sub_8275AA88` | N points by a 4x4 matrix, divided by w | four loop variants reduced to one formula a point; matrix read once; the in-place path declined |
| `sub_8275A828` | N vectors by a 3x3 matrix | the matrix read again for every vector |
| `sub_8276E3D8` | integration step of N bodies | the array's address read again after every store |
| `sub_827596B0` | 4x4 matrix product r3 = r4 x r5 | the context traffic of 76 statements |
| `sub_8270FE40` | four vectors, a scale and a flag into an object | per-word conversions and cards |
| `sub_824E97F0` | sine | the context and stack traffic (its flush-mode switches stay: the polynomial's smallest terms are denormals for ordinary angles) |

The host's flush-mode switch is an `msr fpcr` with no memory operand; each native orders its arithmetic around the
switch it makes (a compiler barrier after it with the inputs read from memory, or the values pinned to registers).

**Checked (Ryujinx, the build with the natives; the guard on every call, `banjo_hot_guard_calls = 4000`,
`banjo_hot_guard_period = 64`).** Run `rc4_verify` (380 s, the scripted pad into the practice room): 3,594,019 calls
compared, 0 differences. Run `rc4_long_verify` (12 min, the sticks rolling in the practice room from 240 s):
7,980,028 calls compared, 731 of the 827 bulk copies reached, 0 differences, no FATAL. Six of the seven natives
compared with the generated original through their hooks (`sub_827596B0` 8,932 calls compared of about 320,000,
`sub_8275AA88` 5,540 of 102,560, `sub_8270FE40` 4,282, `sub_8275A828` 4,128, `sub_8276E3D8` 4,047, `sub_824E97F0`
4,015): 0 differences. `sub_82316A78` (the sphere test) was not reached: the job threads' collision code
(`sub_8221BF20`, `sub_8221BC58`, `sub_82317F48`, it) runs in the game's levels, which the scripted run does not reach.
So its key, `katamari_hand_sphere_box`, ships **off** in `renut.toml` and is on in `configs/renut_verify.toml` (a level
played with that config checks it; `[hot] ... differences: 0`).

## The second console test, rc4 (2026-10-31 12:55)

The user: every movie fixed; a beeping tone still heard and some sound effects in gameplay do not play. Breakdown:
[katamari-nx's docs/console-rc4-breakdown.md](https://github.com/ChanseyIsTheBest/katamari-nx/blob/main/docs/console-rc4-breakdown.md) (handheld, 720p: gameplay CPU-bound at 28-44 fps, the GPU 15-17 ms a frame; the main
thread 89 %, the draw thread 83 %).

**Three cores.** Both console runs (rc3 and rc4) logged "Too few processor cores" and "forwarder NPDM not written:
running as 0100080021954000, not the forwarder 05D6EA1333487100": the user's forwarder has its own program id, so the
package's four-core NPDM (made for 05D6EA1333487100) never applied. With 241 % of CPU demand on 300 %, the game's audio
engine (XThread18C0, 25 %) and the XMA decoder (34 %) share three cores with the main and draw threads, and the
decoder's data arrived up to 18 ms after a kick. `banjo_forwarder_program_id = "0100080021954000"` in every toml
(`tools/make_katamari_tomls.py`): the NRO writes that forwarder's four-core NPDM on the first launch
(`sdk/src/core/threading_switch.cpp`, RexSwitchEnsureForwarderNpdm), the second launch has CPU 0-3.

**XMA frame cache off.** In that run it answered 8-36 % of the frames and re-decoded 10,000-23,000 frames per 10 s in
bursts "to catch the codec up": 0-8 % less decoding in all, and the bursts line up with the worst kick-to-data delays.
Exact either way; `banjo_xma_frame_cache = false` in every toml. Ryujinx run `shipped_check` (the shipped toml): the
opening movie's sound ordinary, no steady line.

The host driver (`sdk/src/audio/switch/switch_audio_system.cpp`) plays silence for a client with nothing queued, so the
beep is not the driver repeating a block: it comes from the game's side when a voice's decoded data is late (the same
tone as rc3's movies, whose XMA feed was broken).

**The constant tone and the missing sound effects (fixed, 2026-10-31).** The user: a constant, unchanging tone from
the main menu on, gone only during loading screens; some gameplay sound effects never play. Reproduced in Ryujinx
on the main menu (rc1 too: it was never the register diet): the game's mix turns into one line at 344.5 Hz
(44,100 / 128) at a constant level; with `scribble_heap` (new memory filled with 0xCD) it comes at once, at 689 Hz.
The per-context XMA dumps: three 44.1 kHz mono contexts decoded their first frame over and over and handed the game
one 128-sample subframe again and again. `audio_xma_loop_trace` (a new diagnostic key, off by default) gave their
fields: loop start 0, loop end 0, loop count 255, on buffers of 1, 2 and 8 packets: looping sounds whose loop is the
whole buffer (XAudio's "loop length 0 is the entire buffer"). The decoder (`sdk/src/audio/xma_context.cpp`, from
Xenia's newer XMA code) clamped the loop end up to bit 32, the first frame of every buffer, so it took the first
frame for the loop end and replayed one subframe of it forever. A first fix that let them end stalled the game's
audio engine on the menus (it waits on voices it believes are still looping), which confirmed the reading. The fix:
a loop end below bit 32 is no loop end; with a loop count and no loop end, past the buffer's last frame (nothing valid
in the other buffer) the read position goes back to the loop start (`XmaContext::LoopWholeBuffer`). Ryujinx after
the fix: the main menu with clean and with scribbled memory, no repeated block (40 s each), the audio goes on; in
gameplay ten contexts loop their whole buffer (the game's looping effects), no repeated block; the opening movie's
sound ordinary.

## rc6 (2026-11-01): three cores, the guest clock, the one-shot effects

The user, after rc5: most effects loop endlessly; "let's just focus on 3 core performance", then the looping.

**Where the three cores go (rc5 console run, handheld, 39 fps in a level).** Main thread 80 % of a core, the draw
thread 72 %, the game's audio engine 27 %, the XMA decoder 18 % ([katamari-nx's docs/console-rc5-breakdown.md](https://github.com/ChanseyIsTheBest/katamari-nx/blob/main/docs/console-rc5-breakdown.md)). What was looked at:

| Where | Finding | Done |
|---|---|---|
| Draw thread, host | the game's QueryPerformanceCounter (`sub_824C0698`, one `mftb`) about 300,000 times a second in a level, almost all from the draw thread through the elapsed-time helper `sub_82678260` (a vtable slot: counter++, QPC, minus a start time, times a scale); every call went through `UpdateGuestClock`: a mutex try-lock, two stores and an add to the counter table all cores share. With the mutex and the counter: about 8 % of the draw thread | `clock_lock_free` (on): the guest count is a fixed base plus the scaled host delta (one acquire load, no store), the base moving only when the ratio changes (twice, at start); calls counted 64 at a time per thread. The summed per-call deltas of the locked path dropped each call's remainder (up to 47/48 of a tick at 125/48): at 300,000 calls a second the guest clock ran about 0.3 % slow; this count does not drift |
| Main thread | 91.6 % machine copies; the two hottest (`sub_82277E70` 18 %, `sub_822683D0` 14 %) have flat profiles (the hottest 64 bytes 7 %), their samples on loads of the matrices they read (cache misses); `sub_827A5B00` (6 %) walks an object tree through vtables | nothing to remove without changing what the game computes |
| Draw thread, guest | `sub_826A13D8` / `sub_826707B0` (5 %): reference counts, one `lwarx`/`stwcx.` loop each, already a single CAS; their cost is the shared lines | none |
| Draw thread, renderer | `CaptureDelta` 4.5 %: 39 chunks (2.5 KB) compared a draw, 21,000 draws a second; the copies the render thread needs | none |
| Audio engine | its top function's time is the VMX resampler's sample gather (loads), already vector code | none this round |
| XMA decoder | 17 % of it is FFmpeg's per-frame allocations (a buffer reference a packet, pooled frame buffers behind a mutex) | needs FFmpeg's internals: later |
| Elapsed-time helper | a hand native would save about 2 % of the draw thread, but the guard cannot compare a function that reads the clock | not done |

**The one-shot effects (fixed).** rc5 looped every sound with loop count 255 and no loop end (`LoopWholeBuffer`),
from a Ryujinx run whose main menu went quiet when they ended. New keys: `audio_xma_whole_buffer_loop` (rc5's loop;
off since rc6), `audio_xma_output_done_when_drained` (Xenia's rule: a drained output with no input left clears
`output_buffer_valid`; off, never needed), and the trace (`audio_xma_loop_trace`) now logs a sound's input ending,
the game re-submitting input or re-validating output, clears and releases. Runs `menu6_end`, `game6_end` (practice
room) and `menu6_rc6` (the new defaults): every such sound that reached the end of its input was released by the game
60-70 ms later (3 on the menus, 16 in the practice room, 9 with the defaults): one-shots; the game's streams (count
0) are re-submitted by the game as before. The practice room's recording follows the King's lines (sound while a
line types, quiet between), where rc5's had sound throughout. `menu6_lockclock` (the same with `clock_lock_free =
false`): the same channels sound at the same seconds, so the clock change does not move the audio.

**Also in rc6.** Docked renders at 1080 lines again (the user's choice of 2026-10-31; rc4 and rc5 shipped 720 docked
by mistake: their log says "Internal heights: handheld 1280x720, docked 1280x720"); `configs/renut_720_docked.toml`
replaces `renut_1080_docked.toml`. No four-core NPDM: `banjo_forwarder_program_id = ""`; rc5 wrote one for the
user's forwarder, which they delete to be back on three cores.

## rc7 (2026-11-01): the supermarket DLC crash, a scan for its kind

The user's rc6 run: entering the supermarket DLC level stopped the game, and nothing in the supermarket was drawn.
Both are written up in `docs/codegen-crash-fixes.md` (with every earlier codegen crash); in short:

- **Crash:** `brk #1000` in sub_822AC470, a call through a table of function pointers whose index register the load
  overwrites; the generator had made a two-case switch on the table's base register, so every call trapped. The
  generator's `indirect_calls` list (loaded, never used) now reaches both of its jump-table detectors;
  `app/overrides.toml` lists 0x822AC494 and 0x822DF3C4 (sub_822DF3B0, the same bug with a masked index, not hit yet).
- **Scan:** `tools/switch_check.py` had been blind since the register diet (it read 157 of 770 switches) and is fixed
  (local-register switches, loop-exit labels, indirect calls in their save blocks, rewritten index registers, masked
  indices, tables of function starts in data). Over all 1,219 `bctr` of the image only those two were wrong.
- **Nothing drawn:** 24 shaders the game builds from its compressed data were not in the shader package; the console
  has no translator, so their draws were skipped. They are saved in `sdmc:/switch/katamari_nx/shader_capture/` on the
  user's card (their folder is `katamari_nx`); the data files cannot be scanned for them (2 containers in `d02.dat`,
  the rest compressed). The next package adds them once the user sends the folder.
- **Generated-code drift:** a fresh codegen and post-codegen today differ from rc6's tree in four functions besides the
  fix and its two inlining callers (sub_824EB898, sub_82535930, sub_8258EB68, sub_829626B8; condition-bit logic lowered
  differently). The same pipeline without the two `indirect_calls` entries gives the same four, today's three runs agree
  with each other, and no generator source, post-codegen tool or config changed since rc4; which input made rc6's tree
  is not known. The guarded gameplay run of the rc7 build (4.54 M calls compared, 0 differences, no FATAL) found
  nothing wrong.
- Generator changes need `rexglue codegen --ignore-stamp`: its stamp covers the config and the image, not the
  generator.

## rc8 (2026-11-01): the 24 captured shaders; where Beautiful Katamari's shaders come from

The user's rc6 console run saved the 24 shaders it was missing (`sdmc:/switch/katamari_nx/shader_capture/`) and sent
them. All 24 are valid containers whose names are their XXH3 (the hashes the log reported). Added to
`out/renut_shader_dump` (153); the PC table regenerated (`renut_shader_cache_gen` of `app/out/pc`, after removing
the generated `shader_cache.cpp`: its dependencies are the dump's files at configure time, so new captures do not
rerun it), then `tools/switch_shaders.ps1`: 153 shaders, 153 compiled with uam (112 vertex, 41 pixel).

**Corrected in rc9 (below): the shaders are precompiled after all**, stored whole in the compressed models next to
their keys; the scan of the game's files found nothing only because it read them compressed. The paragraph as written
in rc8:

**Why a scan of the game's files cannot find them (unlike the other ports' games).** The executable links the whole
D3DX HLSL compiler (its preprocessor and parser messages, the intrinsic table) and the game compiles its shaders at
run time: each material carries a feature key such as `MT0_00000000_OP9_GR9_NM9_RL9_TA0_LT1_SK0_VC0_VFn1c0t0_00000000`
(material type, normal map, lighting, skinning, vertex colour, vertex format, ...), and when a stage loads the game
compiles the HLSL library `npLib` (2006, uncompressed in `d02.dat`) with `#define`s from the key. The keys are inside
the models, which are compressed. A static path would need (1) the decompressor, (2) every key, and (3) the game's
own compiler output byte for byte (Renut looks shaders up by the hash of the compiled container).

**The data layout found so far.**
- The executable holds the game's file system: 31 category descriptors at 0x82AD5078 (20 bytes: name, record table,
  alternate table, record count, state), e.g. Map (51 stage maps, `d02`), MonoMdl (572 object models, `d04`), Idv
  (7,001, `d03`), Item (2,968, `d05`), Ouji, Effect, Xml, Sound, ... Records are 16 bytes: archive name pointer
  (`d02.dat` ... `d05.dat`), flags in the top 3 bits and the uncompressed size in the low 29, offset, stored size.
  Flags 0xC0 / 0xE0 are compressed, 0x20 / 0x00 stored.
- Compressed files are "CPDB" blocks: magic, 1 (little endian), 0, uncompressed size (big endian), then a
  bit-packed stream (2 KB aligned, about 3-5x). Not zlib / raw deflate / gzip at any offset 0-255, not LZX (libmspack
  `lzxd`, windows 2^15-2^21, plain or 16-bit-swapped words, offsets 16-24). `tools/lzx_tool.c` (from worldkeflings-nx)
  is the LZX test tool.
- The loaders take a descriptor's record (`table + index * 16`) and the string at 0x82006034 (`game:\`) and call
  0x82423688 / 0x82423C68 (queue: 0x82426588, 0x82426308, 0x82425xxx), which go on into Alchemy's resource code
  (0x824A6xxx-0x824B3xxx). The decompressor itself is not identified yet; Ryujinx's GDB stub has software breakpoints
  only (no watchpoints), and the CPU sampler's loading windows were dominated by waits.
- The game state at `*(0x82B0B568)`: entering the practice room sets +964 to 16 and +966 to 1
  (`katamari_diag_game_state`, a new diagnostic, off: `src/cpu/katamari_stage_diag.cpp`, a libnx thread on the Switch:
  std::thread throws "Function not implemented" there). A stage warp would start from these two fields.

Ryujinx frame rates of this round are not comparable: a `node` process on the PC used about nine cores during the
runs (rc7's own NRO also fell to 14-37 fps in them). rc8 changes only the shader package and the off-by-default
diagnostic.

## rc9 (2026-11-01): every shader from the game's files; a switch for the unlock-only DLC

**CPDB, solved** (`tools/cpdb.py`, read from the generated code). The loader sub_824A77D0 reads the 16-byte header
and picks the decoder by its byte 4: 0 stored (the bytes after the header), 1 plain LZSS (sub_824B35D8), 2 Huffman
LZSS (sub_824B3500 with sub_824B3688 / sub_824B3458). Header bytes 12-15 are the uncompressed size (big endian). The
bit stream is 64-bit big-endian words, each read from its least significant bit (sub_824B3340).
- Plain LZSS: a flag bit; 0 = an 8-bit literal; 1 = a 4-bit length (+3) and a 12-bit distance.
- Huffman: blocks of a 16-bit entry count, a tree written depth first (1 = node: the 1-bit subtree, then the 0-bit
  one; 0 = leaf, 9-bit symbol), then the entries (after a symbol of 256 or more, a 12-bit distance). Symbols below
  256 are literals, the rest copy symbol - 253 bytes.
- Copies go byte by byte from `out - distance`, so they may overlap themselves.
- The record flag 0x20 is not the coding. The first guess read it that way, and every Huffman-flagged map then
  failed; they are all LZSS.
- Every file of the 31 categories decodes to its record's size: 5.9 GB once the alternate tables' repeats are
  skipped (`tools/katamari_files.py`, about 15 minutes).
- Two exceptions:
  - The three stored fonts are shorter than their records, whose size is then the buffer's.
  - Test[4] says coding 1 but is Huffman: an older packer, and the game never loads the Test category. It is the
    compressed copy of Test[5]; the decoder falls back to the other coding when a stream does not fit its header.

**The shaders are precompiled after all.** The models store Alchemy's igVertexShaderInfo / igPixelShaderInfo with
the compiled containers whole, at any byte offset. The material key strings (28 distinct, e.g.
`MT1_10000000_OP9_..._VFn1c0t1_00000000`) are only their names. `find_shader_headers(data, aligned=False)`: three of
the user's captures were stored only at offsets that are not a multiple of 4.
- 84 distinct containers: 42 already in the package, 42 new (Idv 28, Map 8, Test 4, Core 1, Item 1), all valid.
  **All 24 of the user's console captures are among them**, so the DLC stages need no console capture.
- The HLSL library (npLib, 88 sources with their `#define` headers) is in Test[5] only.
- Compiler version strings in the data: 2.0.4314, 4929, 5426 and 5632. One shader built by two compilers differs
  only in that string, so it hashes differently; both builds are stored.
- The 71 shaders of the package with 2.0.5526 are the executable's video decoder's.
- The DLC packages (16, about 320 KB each) hold unlock data only: no CPDB blocks, no containers. The DLC stages'
  models are on the disc.
- Package: `out/renut_shader_dump` 153 -> 195. The DKSH cache, which had not been remade since 2026-10-30 (207
  variants), was rebuilt with every shader's generic variant: `tools/deko3d_variant_cache.py --generic --list <emulator
  SD pipeline_list.bin> --merge <emulator SD deko3d_shaders.bin>`, 337 variants, 3.4 MB, 0 failed. Without it, the
  console would compile the new shaders the first time a stage draws them.

**The user's DLC switch.** `katamari_dlc_unlock_packs` (app, `renut_engine/dlc_install.cpp`). **False by default,
the user's choice** (first written true, then "could you have those DLC packs off by default"). False keeps the nine
packs that only unlock presents and cousins the game unlocks itself (Accessory Sets A-D, Character Sets A-D, Special
Accessory) from the game. It does this by adding their display names to the SDK's new `dlc_hidden` list, which
`ContentManager::ListContent` leaves out of the game's marketplace content list (`[dlc] '...' hidden` in the log).
- The packs stay installed, so true brings them back.
- The seven stage packs always load. Mechanical Katamari is one of them (its description has you build Antlia from
  mechanical objects), though the user's list of six left it out.
- What a save already received from a hidden pack is not taken back.
- Checked in Ryujinx (run `rc9_default`, a toml without the line, so the built-in default decides): the game's DLC
  list 32 s into the run had the nine `[dlc] ... hidden` lines and 7 entries, the seven stage packs; 60 fps after it,
  no errors, no shader captured.

## rc10 (2026-11-01): the rc9 console run, three hand natives, 560 more copies, the presents menu's seam

**The rc9 console run** ([katamari-nx's docs/console-rc9-breakdown.md](https://github.com/ChanseyIsTheBest/katamari-nx/blob/main/docs/console-rc9-breakdown.md), handheld, three cores). The slowest place so far is the
stage with the music M71_KDXSE: 19-30 fps, the main thread at 101 % of a core (the two others half idle), 46 % of it in
sub_82277E70 (objects' world transforms) and sub_822683D0 (the visibility manager's walk). The `rex_crash.log` that
came with the logs was rc6's (the supermarket crash, still on the SD card).

**Timers after a sleep.** A repeating timer that is late added one interval and fired again
(`sdk/src/core/timer_queue.cpp`), so after the console slept it replayed every interval it missed: 66-88 % of a core
for minutes after a 6.5-hour sleep. Now a timer more than a second behind goes on from the present; less late than
that (a busy moment) it still catches up as before.

**Hand-written natives** (the user: "optimise these two sub_82277E70 and sub_822683D0 by hand"; the end of
`app/src/cpu/hot/banjo_hand_natives.h`, `hand.txt`, keys `katamari_hand_transform_chain`, `katamari_hand_box_test`,
`katamari_hand_translate_multiply`):
- `sub_82277E70`: after its two real calls (both make indirect calls, so no guard can repeat it), the matrix chain
  (constant copy, three or four products, one or two 3x3 rotations, two translate calls, four copies) on registers:
  the same dot products in the same order as the multiply native (`MatMulRegs`), the rotation native's and the
  generated code's formulas, each in the flush mode the original uses, every byte the original leaves (matrices,
  stack temporaries, frames, cards) and every register. Its own check against the existing natives in the
  original's order: every call with `katamari_hand_transform_chain_verify` or `banjo_cpu_verify`, else the first
  2000 calls and one in 256. On the A57 the original stored each matrix (byte-swapped) and the next native loaded it
  back at once: failed store forwarding, plus fourteen vector registers written to the context per product.
- `sub_82265C08`: the visibility manager's box test, sub_822683D0's hot loop: the eight corners without each one's
  store, 4-byte store and reload through the stack, the loop's invariant merges once, and the next object of the
  walk (the link at +1500, read only when it is on the corners' own page) prefetched while this one is tested.
- `sub_8223A970` (translate, then multiply; 57 call sites): copy, translation and product on registers.
- Not five: the rest of the main thread's hot list (sub_822470D0, sub_827A5B00, sub_8227A280, sub_822772C0,
  sub_822889D8) are dispatchers whose time is cache misses on object fields, which hand code does not remove; the
  two prefetches aim at those misses.

**More machine copies:** every function still in generated code with at least one sample in the rc4, rc5 and rc9
console profiles (`tools/switch_hot_bulk.py --min-samples 1`): 560 more, 1,387 in the bulk list (tiny leaves, refused
forms and app-mentioned addresses left out).

**Checked on the PC (Renut):**
- Run `verify_newgame`: a new game into the practice room, the sticks rolling from 240 s, `banjo_cpu_verify` (every
  CPU enhancement on every use). sub_82265C08 4,314,903 calls compared with the original, sub_8223A970 4,591,798,
  the copies 342.6 M calls (1,219 of 1,387 bulk copies reached): 0 differences (8 differed once and not when
  repeated: another thread's inputs). The user's save moved out of `out/pc_user` first (it changes the menu path).
- Run `verify_chain`: the guard in its normal mode (so sub_82277E70's hook answers with its copy, the native in
  front) and the transform chain's check on every call: 28,049,408 calls compared with the reference path, 0
  differences.

**The emulated GPU again (the user: get it working, to double check the presents menu).** `banjo_renderer =
"emulated"` (PC only) leaves the game's Direct3D to the SDK's emulated Xbox 360 GPU: Renut's hooks fall back to the
game's own functions, the app loads the GPU plugin. Two fixes on the way: the app's ImGui dialogs (Renut's overlay
context only) are not created then, and the paths resolve without the wizard. Runs also need
`--banjo_noncpu_enhancements=false`, `--gpu_backend=vulkan --vulkan_device=1` (the RTX 3060; device 0 is the AMD
integrated GPU) and the DLC installed (the user's save needs it). It draws the whole game.
- The presents menu's yellow bar (a 1280 x 4 opaque quad through the selected row) is the game's: the emulated GPU
  draws it in the same rows as Renut (half-covered rows 200 and 204, full 201-203). With Renut's half-pixel offset
  off the bar lands a pixel off, so the offset is right.
- The thin dark line across the big circle is Renut's: the emulated GPU has none. Cause: under MSAA the game samples
  its interpolators at the centroid of the covered samples (logged with `gpu_log_interpolator_sampling` in the
  emulated GPU: SQ_CONTEXT_MISC.sc_sample_cntl 0 for most shaders, 2 with SQ_INTERPOLATOR_CNTL's sampling pattern
  0004 for a few), Renut at the centre: the circle's two halves meet on a row of pixel centres, where the texture
  coordinate is on the border of the circle's image. A shader's header holds both registers (fieldC = its
  SQ_CONTEXT_MISC, field18 = its SQ_INTERPOLATOR_CNTL); XenosRecomp (RENUT_RECOMP) now declares each input
  `centroid` by Xenia's rule. Only MSAA draws change (a single-sample pixel's centroid is its centre). All 195
  shaders compile with uam; the DKSH cache remade (273 variants).

## rc10b (2026-11-01/02): rc10's black screen; centroid checked

**rc10 was black** on the console and in Ryujinx alike (the logos, the title, the menus; the opening movie drew).
plume_deko3d learns which varyings a pixel shader reads from its `layout(location = N) in` lines (the vertex
shader's constant 905, `Deko3DOptions::vsOutputs`, drops every output nothing reads). rc10's `layout(location = N)
centroid in` did not match: those inputs were left out of the mask, the vertex shaders stopped writing them, and the
pixel shaders read nothing. The same parse in `tools/deko3d_variant_cache.py` (the DKSH cache's 905 masks) and in
`tools/deko3d_glsl_opt.py` (pszero's input list) now take an interpolation qualifier first. rc10b's NRO f70c972a256f,
cache 15f8c4e588ca (273 variants, 0 failed).

**Centroid, checked where it can be.** Ryujinx decodes the IPA instruction's sample mode (bits 52-53) and never
applies it: its captures keep the presents menu's line with and without centroid (row 371 identical). uam's output is
standard: the circle's shader (ps 835AAC3F43888B52, TEXCOORD0 and COLOR0 centroid, FOG0 centre) differs from the
centre-sampled one only in the centroid bit of its six IPA instructions. The PC's Renut (Vulkan, RTX 3060) with the
centroid shaders: the presents menu complete, rows 368-374 uniform at x = 470, 520, 760 and 820 (the line gone). On
the console (the user, rc10b): the presents menu without the line, three runs out of four. The first visit of one run
drew nothing there for 40 s (the game at 60 fps, the circle's shader drawn every frame: the user saw the game locked);
the same screen fine on the next start, and twice more after the user deleted everything and copied the files again.
Not reproduced in Ryujinx (no pipeline list: the first visit's generic variants and background reads, the same
counters as the console's log); the user's call: not followed further.

**`BANJO_CENTROID`** (XenosRecomp, on unless `0`; the user: "please leave centroid on by default").
`tools/switch_shaders.ps1 -NoCentroid` builds rc9's centre-sampled set into `out/deko3d_shaders_nocentroid` and
`app/out/renut_switch_nocentroid` (a GLSL package the runtime loads from `/switch/katamari_nx/deko3d_glsl.bin`).
With it off the package and the SPIR-V cache are rc9's byte for byte; on, rc10b's.

## 1.0.0, the PGO-instrumented build (2026-11-02)

The user: "make this the icon", "the version number 1.0.0 and the author ChanseyIsTheBest", "make this build
LTO+PGO instrumented for collecting PGO data", "turn off all logging besides crash logging".
- NRO metadata (`app/CMakeLists.txt`): version 1.0.0, author ChanseyIsTheBest; the icon `tools/switch/icon/
  katamari_icon.jpg` (the user's image, the cover art, 1024 x 1024 WebP cropped and scaled to a 256 x 256 JPEG): not
  in git (`.gitignore`); without it the NRO has the default icon.
- Instrumented build: `app/out/switch-pgo-gen`, `-DBANJO_PGO=generate -DBANJO_SWITCH_LTO=ON` (scratchpad
  `build_pgo_gen.bat`): 22 minutes from scratch, NRO 100 MB, 219 objects in the `gcov_info` section (all in one LTRANS
  partition). LTO is not needed to instrument (GCC counts before its interprocedural passes); the user asked for it,
  and it makes the instrumented NRO faster to play. The streams go to `pgo/banjo_pgo_<N>.stream` next to the NRO
  (`src/banjo_pgo.cpp`): every 2 minutes, at HOME, at exit. Merge them on this PC (`tools/pgo_merge.py <folder>`:
  the streams name its profile folder), then the optimised build: `-DBANJO_PGO=use -DBANJO_SWITCH_LTO=ON` in a folder
  of its own.
- Only crash logging: `configs/renut_pgo.toml` (`tools/make_katamari_tomls.py`, PGO): `log_level = "off"`, the
  samplers, the stall watchdog and the renderer's diagnostics off. With every level off the SDK makes no log file
  (`sdk/src/core/logging.cpp`), and `deko3d.log` is not opened (`banjo_renut_deko3d.cpp`). Left: `rex_crash.log`
  (the crash handler) and `rex_stderr.log` (the crash hooks: abort, std::terminate, Mesa).

## 1.0.0, the final build and the installer (2026-11-02)

**Settings (the user: only these in the toml, the rest hardcoded).** `app/katamari_switch_defaults.toml` is compiled in
and applied first (`BuiltInConfig`, sa2-nx's design); `app/katamari_switch.toml` is written next to the NRO as
`katamari.toml` when it is missing (`DefaultConfigFile`). It is not `renut.toml`: the full copy a test build left on the
SD card would override every fixed setting. A key added to `katamari.toml` still overrides a fixed one, which the test
runs use. The player's: `msaa_level`, `frame_cap`, `user_language`, `input_xbox_layout`, `input_joycon_sideways`,
`input_joycon_sideways_stick`, `katamari_dlc_unlock_packs` (false: the user asked for it back in the toml), `[input_p1]`, `[input_p2]`.

**The package tools (2026-11-03, found by kameo-nx).** `dist/switch/README.txt`, the tomls' headers,
`tools/package_switch.ps1` and `tools/run_ryujinx_rdc.ps1` still named `renut.toml` after this change. A built-in build
ignores that file: a tester swapping one in changes nothing and gets no log. They now name `katamari.toml`, and the
package script takes the name from the build's `CMakeCache.txt`. 1.0.0 was not affected: its package carries no toml and
its README names `katamari.toml`. Key by key, the built-in settings differ from `renut.toml` and `renut_play.toml` in 9
keys only: logs, profiler and diagnostics, all off in the built-in settings.

**Language.** `XGetLanguage` answered English whatever `user_language` said; it is the game's only language query. Ryujinx
with 2: Japanese title, save menu and hub labels, `opening_jp.wmv`. French (4) and Spanish (5): English text and
`opening_us.wmv`, so the game has English and Japanese.

**MSAA level.** `msaa_level` lowers the samples of every target the game makes with MSAA (`render/surfaces.cpp`, the
portrait key's mechanism). Ryujinx at 4, 2 and 1: the hub and the presents menu whole, 60 fps. At 1 the presents menu's
row 371 is uniform too (single-sample, no centroid needed there).

**Frame cap.** `frame_cap` was defined and did nothing: `renut_engine/frameHooks.cpp`'s limiter is not built here (its
hooks are N&B's). `renutFrameLimit` is in `katamari_keys.cpp`, called at the end of `D3DDevice_Swap_hook`: the game never
calls Present (the first attempt, in the Present hook, never ran; the profiler's frame boundary there does not run
either). With "30", Ryujinx holds 29.9-30.0 fps, 300 images in 600 refreshes, and the logos and menus keep their timing.
A stage has not been checked under the cap.

**PGO.** One console session of the instrumented NRO: 219 `.gcda`, largest arc count 1.3e9, kept in `pgo/`. Release folder
`app/out/switch-pgo-use` (`tools/build_switch_release.bat`): about 25 minutes from scratch.

**Shaders outside the NRO.** All 195 are in the player's files: 111 in `default.xex`, 84 in `d02.dat`-`d05.dat`
(`tools/katamari_shader_files.py`: 57 records, the largest 1.9 MB, 116 ms to decode in JavaScript). The release NRO has the
shader table without SPIR-V (`tools/renut_release_shaders.py`) and reads a version 1 `deko3d_glsl.bin` beside it into
memory (`banjo_renut_deko3d.cpp` Open); banjo-nx's version 2 path, with the table in the package, is still there.

**The installer** ([katamari-nx-installer](https://github.com/ChanseyIsTheBest/katamari-nx-installer), from
futari-nx-installer's). What changed from futari's page: `lib/cpdb.js`; the containers by the manifest's records; the
translator rebuilt from this tree (centroid sampling; `tools/renut_hlsl_frontend.cpp`); `scalecodes` and the qualifier in
`PSIN_RE` in `lib/deko3d_glsl.js`; Futari's named constants out of `lib/early_alpha.js` (with them the page made early
alpha copies of 7 pixel shaders the native tools leave alone). Result, `test/deko3d_node.mjs`: 123 of 195 identical to the
native chain's, all 195 once SPIR-V's value numbers are renamed (the page's newer DXC). Package 6.9 MB, 40 s in Node. The
zip of the whole game and the 16 DLC packages: 2.9 GB, 39 s. Ryujinx from a clean folder made of that zip: the first start
writes `katamari.toml` and no log, installs the 16 packs; with the user's save the hub and the presents menu draw at 60 fps,
49 shaders compiled at first use, 0 failed, 1.6 s in all that a draw waited for (on the PC; slower on the console).

**The public repository.** A history audit (every blob outside third-party code) found one file of generated code:
`app/src/renut_engine/fixes/mullhwucrash.cpp`, N&B's translated PowerPC, never built here. The public history is a rewrite
without it, with the noreply author; the local repository keeps its own history (the file removed by a commit).

## 1.0.1: 2 MB blocks and the boot's cvars (2026-11-05)

The user, through the Kameo session: "uptick their versions by 0.0.1 and upload it github with these two new
fixes/improvements", "on release notes just put -Increased performance by approximately 5%", "keep the install/update steps
in the release notes". Then, here: "Build and upload and publish straight away".

**Not measured on this game.** The 5 % is the user's wording. Kameo measured about 4.3 % on its own game, standing still in
one GPU-heavy spot. **This NRO went up with no emulator or console run before release**, at the user's request. The console
is the only real test of the code move: its log should say `[code blocks] text ... now on a copy`, and `left where it is`
means it skipped itself.

**Game memory** (`sdk/src/core/guest_memory_switch.cpp`, from kameo-nx `1904dad3`, which took it from the Torchlight port's
findings). `RexGmInit` passed 2 MB as the second argument of `virtmemFindAslr`, which is a guard and not an alignment. So the
window was never on a 2 MB boundary and all the game's memory was on 4 KB pages. It now asks for 2 MB more and rounds the
base up. `banjo_window_blocks = false` in `katamari.toml` puts it back.

**The NRO's code** (`sdk/src/ui/switch_code_blocks.cpp`, kameo-nx's at `be9ee495`, with this port's toml name). The text and
read-only data move to a copy that lines up with their addresses modulo 2 MB. `banjo_code_blocks = false` in `katamari.toml`
turns it off. It costs their size again in memory: about 59 MB at 1.0.0's sizes (text 50.4 MB, read-only data 8.5 MB). The
lowest free heap in the console logs at hand is 1,736 MB (rc9).

Kameo's traps, checked for this port. Every orderly exit is `__nx_applet_exit_mode = 1` and `__libnx_exit`
(`windowed_app_main_switch.cpp`, `rex_app.cpp`); `abort()`, `exit()` and uncaught exceptions end in `diagAbortWithResult`
(`switch_crash_hooks.c`, `runtime_switch.cpp`), so nothing returns to hbloader. The other priority 101 constructors start no
thread, and the first thread of a constructor is the profiler's, at priority 102. The emulator doesn't run the move (it has
no loader, so only a self-test of the swap runs). The path that puts the old copy back has never run anywhere.

`banjo_tlb_test_s` (`banjo_renut_tuning.cpp`) measures the TLB's cost on the console, in the log. Kameo's corrected version of
it hasn't run on a console either.

**The boot's cvars** (`sdk/src/core/cvar.cpp`, from pacman-nx `72c28ced`). `cvar::Init` gave CLI11 every cvar, and CLI11's cost
is quadratic in their number: 2,467 in the 1.0.0 ELF. It now registers only the ones the command line names. Old and new
compared on 145 command lines on the PC (a test outside the repository): 0 differences, and a copy broken on purpose showed
them. `Init` on the PC went from 346 ms to 0.05 ms at that count. Scaled from Pac-Man's 90 s or so at about 6,500 cvars, that is
about 12 s at every console start: an estimate, not measured. The console's log can't show it, because it opens after `Init`.

**The SDK's version** (`sdk/cmake/rex_version.cmake`, from kameo-nx `be9ee495`). It read this repository's `v1.0.0` tag as the
SDK's version and stopped the configure, "floor version (0.10) is behind tag version (1.0)", in any clone with tags. The version
now always comes from the floor.

**PGO.** `pgo/` is still 1.0.0's profile. New functions are at the end of their files, and the SDK's core has no profile. A
function changed since then keeps none (`-Wno-coverage-mismatch`).
