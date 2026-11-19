# Third-party notices

This port is built on the work of other projects. Their licenses apply to their code and to the parts of this repository
derived from it. New code written for this port is copyright (c) 2026 ChanseyIsTheBest and is under the license of the
folder it is in (see below).

## Projects this port is derived from

| Project | Used for | License |
|---|---|---|
| [renut-nx](https://github.com/ChanseyIsTheBest/renut-nx) (banjo-nx: Banjo-Kazooie: Nuts & Bolts) by ChanseyIsTheBest | The tree this port started from: the SDK fork, the app, the tools, the renderer's adaptation, plume's deko3d back end, the shader passes | GPL-3.0 |
| [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx), [futari-nx](https://github.com/ChanseyIsTheBest/futari-nx), Peggle 2's and Sonic & All-Stars Racing Transformed's ports by ChanseyIsTheBest | CPU rounds, built-in settings, release shader files, fixes | GPL-3.0 |
| [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by NebadasSwifty | The structure of `app/`, `app/src/me` | GPL-3.0 |
| [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND | The Horizon (Switch) layer of the SDK, the profiling and PGO method, scripts and the toolchain file | GPL-3.0; the SDK parts BSD-3-Clause |
| [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) by madelrandel-blip | The recompilation project the ports above started from | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | `sdk/` | BSD-3-Clause (`sdk/LICENSE`) |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev and contributors, with the changes of the ports above and this one | `shaders/`, `app/thirdparty/reblue-XenosRecomp` | MIT |

The Horizon (Switch) layer added to `sdk/` is under the SDK's BSD-3-Clause license, and the changes in `shaders/` and
`app/thirdparty/reblue-XenosRecomp` under XenosRecomp's MIT license, so they can be reused by other ports. The rest of
this repository (`app/`, `tools/`, `docs/`) is GPL-3.0 (`LICENSE`).

## The deko3d renderer (app/src/renut_engine, app/thirdparty)

- **reNut** (github.com/masterspike52/reNut, by reNut and masterspike52): the Direct3D-level renderer in
  `app/src/renut_engine/gpu`, through banjo-nx's adaptation. reNut has no licence file; its author permits the user's
  ports to reuse it.
- **plume** (`app/thirdparty/plume`): MIT; its deko3d back end (`plume_deko3d*`) is banjo-nx's, under the same licence.
- **uam** (`app/thirdparty/uam`): zlib (uam), MIT (the Mesa files it imports), MPL-2.0 (SwitchWakerHD's patches).
- **reblue-XenosRecomp** (`app/thirdparty/reblue-XenosRecomp`): MIT, with its own third-party folder (zstd, xxHash,
  smol-v, DXC) under their licences.
- **Dear ImGui** (`app/thirdparty/imgui`, the PC build's overlay): MIT.
- **The game menu's fonts** (Dear ImGui's `misc/fonts`, embedded in the build by `app/CMakeLists.txt`): ProggyTiny by
  Tristan Grimmer (free, its licence: MIT-style); Roboto by Christian Robertson (Apache License 2.0); Karla by Jonathan
  Pinhorn (SIL Open Font License 1.1); Cousine by Steve Matteson (Apache License 2.0). The default ("System") is the
  console's own shared font, read at run time, not shipped.
- **deko3d** (devkitPro): zlib, linked from devkitPro's portlibs.

## Libraries

Libraries used by the SDK, the NRO and the code generator, in `sdk/thirdparty`, each with its own license file:

| Library | License |
|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil: the XMA, WMA and WMV decoders) | LGPL-2.1-or-later |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie (LZX decompression) | LGPL-2.1 |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT, Apache-2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | MIT |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 |
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause |
| [libnx](https://github.com/switchbrew/libnx) and the devkitA64 runtime (newlib, libstdc++) | ISC; newlib and GCC runtime licenses |
| zlib and Zstandard, from devkitPro's Switch portlibs | zlib License; BSD-3-Clause |

Only for builds on a computer or for the tests, never in the Switch NRO: SDL3 (zlib), the Vulkan loader (Apache-2.0),
Catch2 (BSL-1.0).

### FFmpeg and libmspack (LGPL)

Everything needed to rebuild and relink the NRO with a modified version of either library is in this repository and in
[docs/building.md](docs/building.md).

## The installer page

The installer page is its own repository, [katamari-nx-installer](https://github.com/ChanseyIsTheBest/katamari-nx-installer).
Its WebAssembly shader translator is built from `app/thirdparty/reblue-XenosRecomp` and `tools/renut_hlsl_frontend.cpp` of
this repository (with [Emscripten](https://emscripten.org): MIT and the University of Illinois/NCSA license); its other
components are listed in its `NOTICES.md`.

## Trademarks

Beautiful Katamari is a trademark of Bandai Namco Entertainment; the icon (`tools/switch/icon/katamari_icon.jpg`,
`app/icon/app.ico`) is its cover artwork, © Bandai Namco. Nintendo Switch is a trademark of Nintendo. Xbox 360 is a
trademark of Microsoft. This project is not affiliated with or endorsed by any of them.
