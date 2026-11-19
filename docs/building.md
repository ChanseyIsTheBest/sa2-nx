# Building the base

As checked in, the base builds Beautiful Katamari, so the file names and counts below are Katamari's; a new port's
differ where they name the game (its own manifest, shader finder, tomls). You need your own copy of the game: the code generator and the shader tools read it. Nothing made from it is committed
(`.gitignore`): `app/assets`, `app/generated`, `out/`, `app/out/`.

## Tools

Windows; LLVM clang and the Visual Studio Build Tools (CMake and Ninja) for the PC build (`tools/pc_env.bat` names the
paths); devkitA64, libnx and deko3d from [devkitPro](https://devkitpro.org) for the Switch; Python 3 with `xxhash` and
`Pillow`. The deko3d host tools (`out/deko3d_tools`: SPIRV-Cross and the PC uam) come from `tools/deko3d_build_tools.sh`.

## 1. The game

Extract the disc (`tools/extract_iso.py`) and make `app/assets` point at the folder that holds `default.xex`.

## 2. The PC build: code generation and the shader translator

```bat
tools\build_host.bat
tools\build_pc.bat
```

The PC build runs the code generator (`app/renut_manifest.toml`), the post-codegen passes (`tools/post_codegen.py`) and
Renut's XenosRecomp over `out/renut_shader_dump`. Run it with `tools/run_pc.ps1`.

**Code generation again.** The PC build regenerates by itself when the generator (`sdk/out/win-amd64/rexglue.exe`)
relinks. A change to a post-codegen tool, to a header the generated code includes, or a port that says "take it with a
code generation" needs it by hand: `powershell -ExecutionPolicy Bypass -File tools/codegen.ps1`, then
`python tools/post_codegen.py` in full (never one of its steps alone on a finished tree: tools/localize_native.py
refuses copies there), then the builds. Its log names what each step did; `reg_liveness` and `localize_native` print
what the machine copies leave out.

**A new game's calling convention.** Port 121's register liveness (tools/reg_liveness.py) relies on the 360 convention
(v1 is the first vector argument and result; v0 and v32-v63 are scratch; a function reached through a pointer returns
to a caller that reads only results). Check it on each game before trusting it: `python tools/cc_entry_census.py` must
end with "none", and every read `python tools/cc_exit_census.py` lists must be value-independent. If a game breaks it,
generate with `SASRT_REG_LIVENESS_EXIT_ABI=0` (and `SASRT_REG_LIVENESS_HOOKS_ABI=0` for a vector hit).

## 3. The shaders

```bash
python tools/renut_shader_dump.py --image out/renut_image.bin --out out/renut_shader_dump
python tools/katamari_files.py --image out/renut_image.bin --game <game folder> --shaders out/katamari_file_shaders --known out/renut_shader_dump
```

The first takes the 111 containers stored in `default.xex`, the second finds the 84 in the models of `d02.dat`-`d05.dat`
(copy them into `out/renut_shader_dump`). Then:

```bat
powershell -File tools\switch_shaders.ps1
python tools\renut_release_shaders.py
```

`switch_shaders.ps1` translates all 195 to GLSL and makes the everyday build's files (`app/out/renut_switch`, every
shader built into the NRO). A port whose notes say "take it with a shader package made again" needs this step (and the
Switch build after it). A new package changes the shaders' text, so the device compiles their variants once: before a
console measurement, make the DKSH cache from a pipeline list (`python tools/deko3d_variant_cache.py`, its help) so a
first run does not compile while it draws. `renut_release_shaders.py` makes the release build's (`app/out/renut_switch_release`: the
shader table without SPIR-V, and `sd/deko3d_glsl.bin` to put next to the NRO for tests). `BANJO_CENTROID=0` (or
`switch_shaders.ps1 -NoCentroid`) translates without centroid sampling.

## 4. The Switch build

The everyday build (`app/out/switch`, every shader built in, no LTO):

```bat
tools\build_switch.bat
```

The Switch build runs all guest float code in the host's flush mode (CG-13, CMake `BLUR_FPCR_ONE_MODE`, ON on the
Switch since port 133, OFF on the PC): no `msr fpcr` between the game's VMX and scalar code. Not exact for scalar
denormals: check each game with `tools/cg13_census.ps1` on the PC build (section 6, off-by-default.md section 2). A
folder configured before port 133 keeps its cached OFF: `cmake -DBLUR_FPCR_ONE_MODE=ON app\out\switch` (and
app\out\switch-guard) once, then build; every generated file compiles again. A PGO profile is trained with the
setting the build ships with.

The everyday and released NROs carry only the running form of each machine copy (port 78, sasrt-nx's idea 27: CMake
`SASRT_HOT_GUARD_FORMS`, OFF by default on the Switch and ON on the PC): every copy is called directly, and
`banjo_cpu_verify` / `banjo_hot_guard_*` check none of them (the log says so). For a guard or verify run on Ryujinx or
the console, the guard build (`app/out/switch-guard`, otherwise the everyday build's options):

```bat
tools\build_switch_guard.bat
```

then `tools/run_ryujinx.ps1 -Build switch-guard -Toml <file>`. `GUARD_FORMS=ON` (or `OFF`) sets the option in any
folder `tools/build_switch.bat` builds; `tools/build_switch_release.bat` holds its folder at OFF unless `GUARD_FORMS=ON`
(sasrt-nx's `-Final` and `-GuardForms`).

Since port 81 (sasrt-nx's machine copies in place: CMake `SASRT_HOT_IN_PLACE`, ON on the Switch wherever the guard forms
are OFF, OFF on the PC and in `app/out/switch-guard`) the everyday and released NROs have no hooks in front of the copies:
the generated function's symbol is its copy, which the compiler and LTO may inline into its callers, and the generated
original is dropped. The copies' keys (`banjo_cpu_native_sub_*`, `banjo_cpu_native_bulk`), the guard and verify keys,
`banjo_hot_count` and the in-run A/B's target 70 do nothing there, and the log names any that is set; a copy marked `off`
in its list stays generated code. The hand-written natives keep their keys. `IN_PLACE=OFF` (or `ON`) sets the option in
any folder `tools/build_switch.bat` builds (`GUARD_FORMS=ON` sets it OFF); the guard build is hooked and compiles the
same copies' code, so a clean guard run checks the in-place NROs' copies too. After this port, run
`python tools/post_codegen.py` once (or a code generation): it writes the copies' headers with the in-place entry
(not tools/localize_native.py alone: run on the finished tree, after the store cards and other late steps, it refuses
copies) (an in-place build of older headers says so at compile time; its copies stay calls).

The released NRO, in folders of their own (`BUILD_DIR` and `CMAKE_EXTRA` of `tools/build_switch.bat`). The two forms of the
copies compile to different code, so each has its own profile (port 81): `pgo/` for the hooked form (CMake
`BANJO_PGO_DIR`), `pgo-inplace/` for the in-place one (`BANJO_PGO_DIR_IN_PLACE`). A folder writes and reads the one of
its form, so with the defaults these steps make and use `pgo-inplace/`; with `IN_PLACE=OFF` on both folders, `pgo/`:

1. **Instrumented:** `-DBANJO_PGO=generate -DBANJO_SWITCH_LTO=ON`. Play it: it writes `pgo/banjo_pgo_<N>.stream` next to
   the NRO every 2 minutes, at HOME and at exit.
2. **Merge** on the PC that built it (the streams name its folders): `python tools/pgo_merge.py <folder with the
   streams> pgo-inplace` fills `pgo-inplace/` (the second argument `pgo`, or none, for a hooked session; it stops if
   the folder already has `.gcda` files: move them aside first).
3. **Optimised:** `-DBANJO_PGO=use -DBANJO_SWITCH_LTO=ON -DBANJO_RENUT_RELEASE_SHADERS=ON` (`tools/build_switch_release.bat`,
   in place; its configure stops while `pgo-inplace/` has no profile, and `IN_PLACE=OFF` builds the hooked form with
   `pgo/`). It carries no game shader and reads `deko3d_glsl.bin` beside itself. `pgo/` in this repository is the
   profile of the releases before port 81 (the hooked form). A function changed since the instrumented build keeps no
   profile. So train again after a change to the draw path's own functions: port 179 added calls inside
   FastDrawLocked and DrawStateLocked, and Blur's NRO with the profile trained before it drew at 14.75 us a draw
   (render thread) until it was trained again (11.56 us; blur-nx, 2026-11-18).

Two more PGO options were tried and are not used (blur-nx, 2026-11-12, its CPU batch 3; port 111):

- **Value profiles** (`-fprofile-values`): devkitA64's libgcov is not usable with them as it stands. Its indirect-call
  profiler reads `tpidr_el0` (these builds use `-mtp=soft`: the training NRO crashed in 8 s), its top-n writer frees
  lists it keeps across dumps, its top-n profiler callocs from the game threads, and the writer walks lists other
  threads are still growing. With all four worked around (blur-nx's app/src/banjo_pgo.cpp: a TLS profiler of its own, a
  static node pool, the threads held during a dump) the training NRO still corrupted memory at its first dump (about
  2:15 in, on the console and in Ryujinx, 4 runs of 4), while the same tree trained without value profiles ran clean.
- **`-fipa-reorder-for-locality`** (GCC 16): 24.56 fps against 24.77 in Blur with the same toml, the NRO 2.3 MB larger
  (it clones callees); and with it GCC 16.1's LTO dropped a top-level asm (the `.incbin` that defines the SPIR-V
  cache: an undefined reference at the link), so the generated `.incbin` files would have to be built without LTO.

Keep the temporary folder's path free of spaces (GCC's parallel LTO link writes a makefile with it unquoted). Each build
folder keeps its own SDK libraries (`<folder>/rexglue-sdk/out`, renut-ultimate's `REXGLUE_OUTPUT_IN_BUILD`), so the
stages no longer replace each other's, and the SDK runtime and FFmpeg get the folder's LTO and PGO options
(`BANJO_SWITCH_SDK_OPT`), and since port 89 the SDK's OBJECT libraries too (rexaudio, rexcore,
rexfilesystem, rexinput, rexui; `BANJO_SWITCH_SDK_OBJECT_OPT`), but for four files kept out of LTO:
threading_switch.cpp and switch_perf.cpp (they define `--wrap`'s `__wrap_` functions),
exception_handler_switch.cpp and fiber_switch.cpp (top-level asm naming C symbols, which LTO cannot
see). A new SDK file with either needs the same (`_banjo_sdk_no_lto` in app/CMakeLists.txt), or the LTO
link fails. Their counts need an instrumented run of this build: the profile in `pgo/` has none.

The fixed settings are `app/katamari_switch_defaults.toml`, compiled in; the player's are `app/katamari_switch.toml`,
written next to the NRO as `katamari.toml` when it is missing. A key added to `katamari.toml` overrides a fixed one (the
test runs use that: `tools/run_ryujinx.ps1 -Toml <file>`).

## 5. The installer

[renut_nx_ultimate_installer](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_installer) is the web installer
for any port of this base (2026-11-10): a port forks it, writes its `game.json` and fills `release/` with
`tools/make_release.py` from its tree. It makes `deko3d_glsl.bin` (version 2, with the shader table: the NRO is built
with `-DBANJO_RENUT_RELEASE_SHADERS=ON` and carries no game shader) and the DKSH cache from the player's files, with this
base's translator and passes, and packs the archives `game.json` names as `.lz4b` (port 61). Its `wasm/renut_hlsl` and
`wasm/uam` are built from this tree: rebuild them after a change to the translator, its front end or uam, and run its
`tools/sync_from_base.py` after a change to the Python passes. Its README has the steps; Katamari is its example.

The older page, [katamari-nx-installer](https://github.com/ChanseyIsTheBest/katamari-nx-installer) makes the same `deko3d_glsl.bin` in the
browser. For a new release:

```bash
python tools/katamari_shader_files.py --image out/renut_image.bin --game <game folder> --wanted out/renut_shader_dump --out out/katamari_shader_files.json
```

then its `test/release_node.mjs` (its README). `wasm/renut_hlsl` must be rebuilt after a change to the translator
(`app/thirdparty/reblue-XenosRecomp`, `tools/renut_hlsl_frontend.cpp`): its `wasm/src/deko3d/build_wasm_deko3d.sh`.

## 6. Tests

- **PC:** `tools/run_pc.ps1 -Seconds N -Name X [-Extra "--key=value ..."]` runs the PC build with captures;
  `tools/run_pc_verify.ps1 -Seconds 180 -Name X -Script "20:START,24:A"` runs it with the CPU verify and the guard on
  every machine copy (the log's `[verify]` and `[hot]` lines: 0 differences). The PC build is hooked, so it also checks
  each forwarded stack load against the real one (port 121).
- **Ryujinx:** `tools/run_ryujinx.ps1 -Seconds N -Name X -Toml <file> -Build <folder under app/out>`, with a data folder
  of its own. One run at a time, when no other Ryujinx runs and nothing is compiling (a starved run says nothing);
  `-MinCommitGB` sets the free memory it waits for. It copies the toml over the emulator SD's: put it back after.
  Frame rates and load times in the emulator are not measurements.
- **Guard on the Switch side:** `tools/build_switch_guard.bat`, then a Ryujinx run with `banjo_cpu_verify` and the
  `banjo_hot_guard_*` keys. The guard cannot see a wrong "dead register" claim (its skip set comes from the same
  analysis): a run with `banjo_hot_poison_dead = true` and the guard off fills those registers with garbage after each
  copy call, so a wrong claim shows as a crash, NaNs or a wrong picture.
- **Shader passes:** tools/difftest (its README): each changed shader against its old form, with a deliberately broken
  copy as a control.
- **CG-13's census** (port 133): `tools/cg13_census.ps1 [-Seconds 300] [-Script <input script>]` runs the PC build
  with `banjo_diag_runtime_census` and adds up its "[census] CG-13" lines: how many stretches of game code met a
  scalar denormal, which the Switch build's one flush mode turns into zero. 0 in every window (Katamari): keep it.
  Otherwise it lists the sites by function and generated line (tools/census_sites.py --denormal) to judge. The PC
  build must have `BLUR_FPCR_ONE_MODE` OFF (its default; the script checks). Run it in gameplay, not only menus.
- **The console** decides performance: the same NRO both ways (an in-run A/B target, or ABBA order) with a control
  from the same session, fps from Renut's wall-clock 10 s lines. With a new shader package, measure after warm-up runs:
  the DKSH cache built from pipeline lists lacks the specialised variants the console compiles at run time, and the
  first races compile them (sasrt-nx: 95 compiles, +8 ms of process CPU in that race, gone after two races).
  Over the network, `tools/switch_remote.py` (sys-botbase and sys-ftpd-light; only the port's own folder on the SD
  card, nothing deleted): [porting-a-360-game-prompt.md](porting-a-360-game-prompt.md), appendix A.
- **Picture bugs:** a RenderDoc capture first, of the PC build or of Ryujinx (`tools/run_ryujinx_rdc.ps1`, then the
  `tools/rdc_*.py` scripts through `tools/rdc_run.ps1`; `RENDERDOC_DIR` names the RenderDoc folder): the porting
  guide's appendix B.

## 7. Taking the base's later ports into a port

`tools/ult_port_sync.py` (sasrt-nx's, 2026-11-13) brings the base's ports into a game's port one base commit at a time:
for each file a port commit changes, a three-way merge of the port's file with the base's file before and after that
commit, so exactly that port's change comes in and the port's own work stays. Files the commit adds are copied; files the
port keeps in its own form (the per-XDK files, `--hand`), files under another name and deleted files are listed for a
hand port; the docs, Katamari's files and dist/ are skipped. Conflicts are left in place as diff3 blocks.

```
python tools/ult_port_sync.py --base <BASE_DIR> --port <PORT_DIR> --range <last port taken>..<base commit> --dry-run
```

`--range` takes the base's "Port N: ..." commits, oldest first (`--skip N` leaves out a port that is a copy of the port's
own work); a PORTS.json of `[[port, commit], ...]` does the same by hand. Its log is `<PORT_DIR>/out/ult_port_sync.txt`.
sasrt-nx took ports 45-129 with it: 130 files, 136 conflict blocks resolved by hand. Build and test after, as for any
port: the merge knows the text, not the program.
Port 134 (the XMA frame cache v2) rewrites most of `sdk/src/audio/xma_context.cpp`: a port that never took the base's
first frame cache (ports 111 and 120) or Katamari's loop code will not merge it (sasrt-nx's file: 1,270 lines against
the base's 1,913). Take the base's XMA files whole (`xma_context.cpp`, `xma_decoder.cpp`, `rex/audio/xma/context.h`)
and put the port's own lines back.
