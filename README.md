<p align=center><img src="app/icon.jpg" alt="Sonic Adventure 2" width="256" height="256"></p>

<h1 align=center>Sonic Adventure 2: Nintendo Switch port</h1>

<p align=center><b>sa2-nx 1.0.6</b> · <a href="https://chanseyisthebest.github.io/sa2-nx-installer/">web installer</a> · <a href="https://github.com/ChanseyIsTheBest/sa2-nx/releases/tag/v1.0.6">release</a></p>

An unofficial native port of the Xbox 360 (Xbox Live Arcade) version of **Sonic Adventure 2** (2012), made by static
recompilation with [ReXGlue](https://github.com/rexglue/rexglue-sdk) and drawn natively with deko3d. No emulator.

This repository contains no game files. You need your own copy of the game: its Xbox Live Arcade content package from
an Xbox 360 (`Content/0000000000000000/58411202/000D0000/525E7BB1E72A2DB99AB79281BA24938048198FAD58`); the **Battle**
DLC (`58411202/00000002/SonicAdventure2Battle`) is optional.

## Playing it on the Switch

1. Open the web installer: **https://chanseyisthebest.github.io/sa2-nx-installer/**
2. Give it the package file (`525E7BB1E72A2DB99AB79281BA24938048198FAD58`) or the whole `58411202` folder from your
   Xbox 360 (with the Battle DLC if you have it) and press **Create sa2-nx.zip**. It all runs in your browser.
3. Extract `sa2-nx.zip` into `sdmc:/switch/`.
4. Start it with a game override (hold **R** while starting an installed title) or a forwarder.

To update, open the installer again, press **Create sa2-nx-update.zip** and extract it over your folder (your
`sa2.toml` and the game files stay).

Features:

- 60 FPS at the console's stock clocks in single player; the two-player race is heavier and can dip.
- Native Switch renderer (Renut's, on deko3d): 1280x720 handheld, 1920x1080 docked. Anti-aliasing off by default
  (`sa2_renut_msaa`: `2`, `4` or `8`).
- Relaxed vsync on by default (a missed frame shows at 50-59 FPS instead of 30); optional double buffering for less
  input lag (`sa2_renut_double_buffer`).
- Two players: every controller is its own player; single Joy-Cons held sideways; **L + R + ZR** opens the
  controller screen.
- Face buttons by position as on the Xbox 360 pad (Switch B = Xbox A, A = B, Y = X, X = Y); `input_xbox_layout = false`
  for by letter. Per-player remapping in `sa2.toml` (`[input_p1]`, `[input_p2]`).
- Control options from OpenSA2: stick fix and round deadzone (on by default), action buttons, camera invert and hold
  toggle (off by default).
- Language follows the Switch's (`language` in `sa2.toml`).
- Status Monitor shows the frame rate and resolution if SaltyNX is installed.

All options are in `sa2.toml` next to the NRO. Delete it and the game writes the default one again.

Warnings:

- Album applet mode does not give the game enough memory. A forwarder must be set to a **39-bit address space**.
- The first time you play after installing or updating it can stutter for a moment while shaders compile; later
  sessions start smoothly.
- The game runs per frame, so a lower frame rate also slows the game down.
- Made for stock clocks. A CPU overclock raises the frame rate in heavy scenes, at your own risk: an unstable memory
  overclock can corrupt the NAND or the SD card, so back up first. Guides:
  [How to get 60 FPS](https://rentry.org/howtoget60fps), [Mariko](https://rentry.co/mariko),
  [Erista](https://rentry.co/erista), [How to test stability](https://rentry.co/howtoteststability).

## Building

Windows, LLVM clang 20 and the VS 2022 Build Tools for the PC build; devkitA64 and libnx from
[devkitPro](https://devkitpro.org) for the Switch; Python; and your own copy of the game for the code generator. Steps
in [docs/building.md](docs/building.md).

## Documentation

- [docs/porting-guide.md](docs/porting-guide.md): porting an Xbox 360 game to the Switch, start here
- [docs/deko3d-renderer-guide.md](docs/deko3d-renderer-guide.md): making a deko3d renderer for a port
- [docs/renut-renderer.md](docs/renut-renderer.md): Renut's renderer on SA2
- [docs/building.md](docs/building.md): building the PC test build, the NRO, the shaders and the SD folder
- [docs/enhancements.md](docs/enhancements.md) and [docs/cpu-rounds.md](docs/cpu-rounds.md): every optimisation
- [docs/api-renderer.md](docs/api-renderer.md), [docs/direct-renderer.md](docs/direct-renderer.md),
  [docs/gpu-optimisation-candidates.md](docs/gpu-optimisation-candidates.md): the earlier renderers
- [docs/switch-testing.md](docs/switch-testing.md): testing on the console
- [docs/sa2-notes.md](docs/sa2-notes.md): notes on the game's code

## Credits and license

- **SEGA and Sonic Team**: Sonic Adventure 2. This is an unofficial fan-made port with no affiliation.
- **ChanseyIsTheBest**: this port.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)**: the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)**: nfsmw-nx (the SDK's Switch layer, the NVK build, much of the
  method) and [nfsmw-nx-installer](https://github.com/StevensND/nfsmw-nx-installer), which the installer page is built on.
- **[nebadasSwifty](https://github.com/nebadasSwifty/masseffect-nx)**: masseffect-nx, the native renderer this port's
  grew from.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)**: XenosRecomp; shaders also go through DXC and SPIRV-Cross.
- **[kittycache](https://github.com/kittycache9/OpenSA2)**: OpenSA2, the control options (`app/src/sa2_controls.cpp`),
  with the SA2 Mod Loader code "Press B to Somersault" by Hyper and MainMemory.
- **[masterspike52](https://github.com/masterspike52/reNut)**: reNut, the Direct3D-level renderer, used with the
  author's permission as [banjo-nx](https://github.com/ChanseyIsTheBest) carries it;
  **[renderbag](https://github.com/renderbag/plume)**: plume; **[fincs](https://github.com/devkitPro/uam)**: uam.
- **[danfromtico](https://github.com/danfromtico/mesa-switch)** and **[NaGaa95](https://github.com/NaGaa95/mesa-switch)**:
  mesa-switch (Mesa, NVK and NAK on Horizon; the driver up to 1.0.2.1).
- **[fincs](https://github.com/devkitPro/deko3d)**: deko3d.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)**: devkitA64 and libnx.
- The sibling ports (Banjo-Kazooie: Nuts & Bolts, Sonic & All-Stars Racing Transformed, Mushihimesama Futari,
  Peggle 2), which share their fixes: banjo-nx's Renut renderer and deko3d back end, and peggle2-nx-installer's deko3d
  shader chain in the browser.
- The other libraries in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

No affiliation with SEGA, Microsoft or Nintendo. "Sonic Adventure 2" and "Sonic the Hedgehog" are trademarks of SEGA.
This repository contains no assets or code from the original game; everything made from your copy (`assets/`,
`app/generated/`, the shader package) is made on your own computer. Running homebrew needs custom firmware, which
violates Nintendo's ToS and can get a console banned.

GPL-3.0 (see [LICENSE](LICENSE)); the SDK keeps its BSD-3-Clause license (see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
