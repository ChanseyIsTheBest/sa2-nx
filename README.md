<h1 align=center>renut_nx_ultimate_base</h1>

<p align=center>The Renut renderer for Nintendo Switch ports of Xbox 360 games, with every universal fix and optimisation of the ports so far in one place.</p>

Each Switch port of an Xbox 360 game here (Banjo-Kazooie: Nuts & Bolts, Kameo, Halo 3, Viva Piñata, Ridge Racer 6, Sonic &
All-Stars Racing Transformed, Blur, Pac-Man CE DX, Bangai-O HD and the others) carries its own copy of
[reNut](https://github.com/masterspike52/reNut)'s Direct3D-level renderer on deko3d, and each copy fixed things the others
never met. This repository puts them together: [katamari-nx](https://github.com/ChanseyIsTheBest/katamari-nx) 1.0.1 with
the universal fixes and optimisations of the other ports applied to it, port by port, each with its source, its tests and
its switch. A new port starts from here instead of from one game's copy.

> [!NOTE]
> As checked in, this builds **Beautiful Katamari**: the game layer (code generation manifest, hooks, keys, hand-written
> functions) is katamari-nx's. The renderer, the shader translator, plume, the SDK and the tools are the shared part. You
> need your own copy of a game to build anything; this repository holds no game files and no code generated from a game.

## What's in it

Every fix, with its source port and commit, the evidence for it, how it was tested here and its switch, is in
[docs/ultimate-fixes.md](docs/ultimate-fixes.md); [docs/changelog.md](docs/changelog.md) has one line per port. In short:

<div align="center">

| Area | What the base has | From |
| --- | --- | --- |
| Clipping, geometry, depth | the user clip enable at every command list, the game's clip plane for every vertex shader, both cull bits, vertex kill only when every vertex is killed; strip restarts, export-only shaders, finite vertex floats, missing inputs; the game's depth bias, compressed depth decompressed before a read, the EDRAM depth restore; `invariant gl_Position` so depth-equal passes cover the same pixels | rr6-nx, kameo-nx, vivapinata-nx, keflings-nx, halo3-nx, sasrt-nx, dishonored-nx |
| Textures, targets, resolves | the fetch constant over SetTexture's pointer, fetch signs, exponents and integer formats, volumes, DXT3A; Kameo's hand-overs, gamma targets, 16-bit fixed targets, one EDRAM view through another, resolves that keep their target, occlusion queries; the 7e3 clamp; zero-level textures watched until their data comes | rr6-nx, kameo-nx, halo3-nx, sasrt-nx, vivapinata-nx, blur-nx, dishonored-nx |
| Command buffers, memexport | replays inheriting the running device, recorded shader binds, memexport in place, tiles run once for every predicated tile | halo3-nx, sasrt-nx, vivapinata-nx |
| Shader translator and passes | each fetch's own filters and LOD bias, the cube op, tessellation, indexed literals, raw vertex fetch; the fetch spec, plain fetches, the shader rounds (GPU items with in-run A/B targets), early alpha kill, the vertex fetch types | vivapinata-nx, kameo-nx, sasrt-nx, banjo-nx, feedingfrenzy-nx, dishonored-nx |
| Renderer speed | small captures, the stream cache, NEON delta masks, the texture memo, specialised pipelines, resolves skipped or copied, the render queue as a lock-free ring, dynamic resolution, SMAA | bangaio-nx, kameo-nx, sasrt-nx, vivapinata-nx, sa2-nx, dishonored-nx |
| Generated code and machine copies | the post-codegen passes (flush-mode flow, single floats, vector forms, dcbz as DC ZVA, stack forwarding); machine copies of hot functions in place, with whole-program register liveness and a guard that compares them with the original | banjo-nx, sa2-nx, sasrt-nx, blur-nx, katamari-nx |
| Memory, threads, audio, SDK | a memory governor, guest thread stacks pooled (8 MB each), the SDK in the Switch's LTO and PGO, event waits on an address, lock-free XMA kicks (off), the XMA frame cache, rumble, guest sign-in for players 2-4, the time asleep not reaching the game, multi-disc games, LZ4-packed game files | sasrt-nx, blur-nx, feedingfrenzy2-nx, mk9-nx, ds3-nx, LostOdysseyRecomp |

</div>

Each fix keeps its source port's switch name. Nearly all are on by default; [docs/off-by-default.md](docs/off-by-default.md)
lists the ones that are not and why (a fix one game needs that breaks another's picture, a faster form that is not exact,
a speed-up the console measured as slower, a trial). Any of them can be set from a game's toml.

## Defaults a new port settles

- **SMAA is on** (`banjo_renut_smaa`) but runs only right after the draw whose pixel shader is listed in
  `banjo_renut_smaa_after_ps`: until a game lists its own final composite (the last full-screen draw before the HUD), the
  10 s log line says "SMAA ... 0 frames". `banjo_renut_diag_draw_log_s` logs a frame's draws to find it.
- **MSAA is off** (`msaa_level` 1). A game's own sample count changes its picture: the port's decision.
- **The keys of off-by-default.md section 1** fix one game's picture and break another's: a game turns one on only after
  its own comparison shows the same fault.
- **The machine copies' register liveness** relies on the 360 calling convention: check it on the game with
  `tools/cc_entry_census.py` and `tools/cc_exit_census.py` ([docs/building.md](docs/building.md)).
- **Guest thread stacks are 8 MB** (`switch_guest_stack_mb`); a game with deep recursion sets 16.
- **Translator and pass changes** take effect when the game's shader package is made again; before a console
  measurement, build its DKSH cache (`tools/deko3d_variant_cache.py`) so a first run does not compile while it draws.

## State

> [!WARNING]
> **The base itself (Beautiful Katamari) has not run on a real console.** Ridge Racer 6 has, on the base
> (2026-11-08: [docs/ultimate-fixes.md](docs/ultimate-fixes.md), "Ridge Racer 6 on the console"), and most later
> ports carry their source port's console measurements.

- Every port builds for the Switch (the everyday NRO, a guard NRO and an LTO + PGO NRO) and for the PC.
- Each was tested here with Beautiful Katamari: the PC verify (the CPU verify and the guard on every machine copy:
  0 differences), Ryujinx runs, the shader difftests for the shader passes, as its row in ultimate-fixes.md says. Menus,
  the hub and a new game through to the practice room run with no errors logged, and the Switch build's picture matches
  the PC's.
- Katamari does not record command buffers, use memexport, draw 7e3 targets or bind textures by fetch constant alone, so
  the fixes for those are only build-tested here; their source ports tested them.
- The PGO profile in `pgo/` is katamari-nx 1.0.0's (the hooked form, no SDK counts); `pgo-inplace/` (the in-place copies'
  profile) is empty: a port trains its own.

## Starting a port from it

[docs/porting-a-360-game-prompt.md](docs/porting-a-360-game-prompt.md) is the whole order of work, as a prompt for a
coding-agent session. In short:

1. Swap the game layer: `app/renut_manifest.toml` (the code generation config), the game's keys and fixes
   (`app/src/katamari_*`, `app/src/cpu/hot`), its tomls (`app/katamari_switch*.toml`, `tools/make_katamari_tomls.py`,
   `dist/switch/katamari-nx`) and its icon.
2. Bind the D3D hooks to your game's own functions. They are bound to Katamari's addresses (XDK 2.0.5632); a game on
   another XDK needs its own device layout ([docs/xdk-notes.md](docs/xdk-notes.md) has what ports at XDK 2.0.11164 met).
   Some hook bodies wait for a game's own function ([docs/ultimate-fixes.md](docs/ultimate-fixes.md), "Needs the game's
   own function address").
3. Build as [docs/building.md](docs/building.md) describes: the PC build first (it runs the code generator and the
   post-codegen steps, and builds the shader translator), then the shaders, then the NRO; then the tests.
4. Settle the defaults above, then make its web installer from
   [renut_nx_ultimate_installer](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_installer).

## Build

Windows, LLVM clang and the VS Build Tools for the PC build; devkitA64, libnx and deko3d from
[devkitPro](https://devkitpro.org) for the Switch; Python; and your own copy of the game. The whole process is in
[docs/building.md](docs/building.md).

<div align="center">

| Folder | Contents |
| --- | --- |
| `app/` | The game layer and the renderer (`app/src/renut_engine/gpu`), the shader translator (`app/thirdparty/reblue-XenosRecomp`), plume, the CMake project ([app/README.md](app/README.md)) |
| `sdk/` | ReXGlue SDK with the Horizon layer and the code generator changes |
| `tools/` | Code generation steps, build and test scripts, shader tools, the difftests (`tools/difftest`) |
| `pgo/` | katamari-nx's PGO profile (counts of which code ran, no game code or data) |
| `dist/` | The settings of the test builds |
| `docs/` | The documentation (below; [docs/README.md](docs/README.md) is its index) |
| `shaders/` | masseffect-nx's old shader tools, not used by Renut's build ([shaders/README.md](shaders/README.md)) |

</div>

## Documentation

| Page | What it is |
| --- | --- |
| [docs/README.md](docs/README.md) | The index: where to start for each task, and what each page is |
| [docs/building.md](docs/building.md) | Building, code generation, the shaders, the Switch builds, PGO, the installer, tests |
| [docs/porting-a-360-game-prompt.md](docs/porting-a-360-game-prompt.md) | Starting a new port from the base: the order of work and the rules |
| [docs/ultimate-fixes.md](docs/ultimate-fixes.md) | Every fix the base took, with its source, evidence, tests and switch |
| [docs/changelog.md](docs/changelog.md) | One line per port |
| [docs/off-by-default.md](docs/off-by-default.md) | The switches that are off by default, and why |
| [docs/optimising-functions.md](docs/optimising-functions.md) | The hot-function method: machine copies and hand-written natives (Katamari's numbers) |
| [docs/hand-and-machine-optimising-guide.md](docs/hand-and-machine-optimising-guide.md) | sasrt-nx's guide to the same, with its later tools |
| [docs/sasrt-batch-cc2.md](docs/sasrt-batch-cc2.md) | The machine copies' register liveness and stack forwarding, and the calling-convention traps |
| [docs/xdk-notes.md](docs/xdk-notes.md) | What ports at later XDKs met |
| [docs/katamari-notes.md](docs/katamari-notes.md), [docs/codegen-crash-fixes.md](docs/codegen-crash-fixes.md) | katamari-nx's notes and crash fixes: the worked example |
| [renut-nx's docs](https://github.com/ChanseyIsTheBest/renut-nx/tree/master/docs) | The method docs the base was built on: the renderer guide, the enhancements reference, deko3d, PGO and LTO |

## Credits

- **ChanseyIsTheBest** — the ports and this base.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)** — the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **reNut and [masterspike52](https://github.com/masterspike52/reNut)** — reNut's Direct3D-level renderer (reused with its
  author's permission); reNut's own contributors are credited in [app/README.md](app/README.md).
- **[StevensND](https://github.com/StevensND/nfsmw-nx)** — nfsmw-nx: the Switch layer of the SDK and much of the method.
- **[nebadasSwifty](https://github.com/nebadasSwifty/masseffect-nx)** — masseffect-nx.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)** — XenosRecomp, the Xenos shader translator.
- **[plume](https://github.com/renderbag/plume)** (from UnleashedRecomp), **[deko3d](https://github.com/devkitPro/deko3d)**
  and **[uam](https://github.com/devkitPro/uam)** by fincs, with SwitchWakerHD's uam patches.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)** — devkitA64 and libnx.
- **[KhronosGroup](https://github.com/KhronosGroup/SPIRV-Cross)** — SPIRV-Cross.
- The ports whose fixes are here: [renut-nx](https://github.com/ChanseyIsTheBest/renut-nx),
  [katamari-nx](https://github.com/ChanseyIsTheBest/katamari-nx), [kameo-nx](https://github.com/ChanseyIsTheBest/kameo-nx),
  [bangaio-nx](https://github.com/ChanseyIsTheBest/bangaio-nx), [pacman-nx](https://github.com/ChanseyIsTheBest/pacman-nx),
  [keflings-nx](https://github.com/ChanseyIsTheBest/keflings-nx), [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx),
  [futari-nx](https://github.com/ChanseyIsTheBest/futari-nx),
  [feedingfrenzy-nx](https://github.com/ChanseyIsTheBest/feedingfrenzy-nx), the Halo 3, Viva Piñata, Ridge Racer 6, Sonic &
  All-Stars Racing Transformed, Blur and Feeding Frenzy 2 ports, [dishonored-nx](https://github.com/cznn999/dishonored-nx)
  (cznn999), mk9-nx (Thorhax), ds3-nx (VasilkovND) and LostOdysseyRecomp.
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

No affiliation with Microsoft, Nintendo or any game's publisher. This repository contains no game files and no code
generated from a game. Users must provide their own legally obtained copy, and everything made from it (`app/generated/`,
the shaders) is made on their own computer. Running homebrew requires custom firmware, which violates Nintendo's ToS and can
get a console banned — your call.

Source code is provided under the GPL-3.0 License (see [LICENSE](LICENSE)), like the ports it builds on. The SDK keeps its
BSD-3-Clause license (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
