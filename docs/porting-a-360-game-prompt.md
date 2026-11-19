# Prompt: port an Xbox 360 game to the Switch

A fill-in prompt for starting a new port in a coding-agent session. Replace each placeholder below, then paste the whole
block into a new session.

It starts from this repository, the ultimate base
([renut_nx_ultimate_base](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base)): katamari-nx with the universal
fixes and optimisations of the other Renut ports folded in (Beautiful Katamari is its example game). The method docs it
names are renut-nx's ([banjo-nx](https://github.com/ChanseyIsTheBest/renut-nx)'s `docs/`), which keeps the same prompt. Its web installer template is
[renut_nx_ultimate_installer](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_installer). To start from another
port instead, change the base's name and the pages of step 1 of the reading list.

| Placeholder | What it is | Example (the Pac-Man port) |
|---|---|---|
| `<GAME>` | The game's name | Pac-Man Championship Edition DX |
| `<SHORT>` | A short name for the port's folder, notes and Ryujinx data | `pacman` |
| `<TITLE_ID>` | The game's title ID | `58410A8C` |
| `<STOP_TIME>` | When the session stops | 6:00 am (AEST) |
| `<GAME_FILES>` | The game's package or files | `D:\pacman\PAC MAN CE DX\58410A8C\000D0000\<package>` |
| `<UPDATE_AND_DLC>` | Its title update and DLC packages, or "none" | `D:\pacman\PAC MAN CE DX (1)\PAC MAN CE DX\58410A8C\00000002\AllinOnePack` |
| `<WORK_DIR>` | The folder this port's files go in | `D:\pacman` |
| `<BASE_DIR>` | Your copy of renut_nx_ultimate_base | `D:\renut-ultimate\base` |
| `<INSTALLER_DIR>` | Your copy of renut_nx_ultimate_installer | `D:\renut-ultimate\installer` |
| `<RENUT_DIR>` | Your copy of renut-nx (the method docs) | `D:\Banjo kazooie port\banjo-nx` |
| `<RYU_DATA>` | The folder of Ryujinx data folders | `D:\New folder\ryu-data` |
| `<SD_FOLDER>` | The one folder on the console's SD card this port may touch | `sdmc:/switch/pacman-nx` |
| `<CONSOLE_IP>` | The console's address on your network (it runs sys-botbase and sys-ftpd-light: Appendix A) | `192.168.0.20` |
| `<RENDERDOC_DIR>` | RenderDoc 1.46, the portable 64-bit build (Appendix B) | `D:\tools\RenderDoc_1.46_64` |

```text
Port the Xbox 360 game <GAME> to PC and the Nintendo Switch with ReXGlue, starting from the ultimate base
(renut_nx_ultimate_base). Our primary objective is the Switch port; the PC port is only an intermediary step.
Work uninterrupted until <STOP_TIME> without stopping to ask me anything: make sensible decisions, write them down, and
keep going. Do the work in this main session (no subagents).

Inputs
- Game files (title ID <TITLE_ID>): <GAME_FILES>
- Title update and DLC: <UPDATE_AND_DLC>
  Extract packages (LIVE, PIRS or CON) with the base's tools/stfs_extract.py into <WORK_DIR>\game, <WORK_DIR>\update and
  <WORK_DIR>\dlc, outside the repo. Game data is never committed, uploaded or distributed.
- New port folder: <WORK_DIR>\<SHORT>-nx (create it as a new git repo from a copy of the base's files, not its history;
  commit locally as you go, with my GitHub noreply address; never push unless I ask).
- Base, read-only (do not modify it): <BASE_DIR>. Web installer template, read-only: <INSTALLER_DIR>.
- Method docs, read-only (the base was built on renut-nx and does not carry them): <RENUT_DIR>\docs
- Ryujinx data folder for this port: <RYU_DATA>\<SHORT> (make it from <RYU_DATA>\katamari's system folders, keys and
  firmware, not its sdcard).

The console and RenderDoc (<BASE_DIR>\docs\porting-a-360-game-prompt.md, appendices A and B, has the details)
- The console at <CONSOLE_IP> runs sys-botbase (TCP 6000: button presses, screenshots, the running title ID) and
  sys-ftpd-light (TCP 5000: the SD card). Drive it only through <BASE_DIR>\tools\switch_remote.py with --root
  <SD_FOLDER>: it refuses any other path on the card and has no delete command. Every console run needs my word first,
  and is arranged with the other port sessions that share the console. Before pressing A on the Home menu, read the
  selected tile's name on a screenshot; after the launch, check the running title ID; on a wrong title, stop pressing.
- RenderDoc (<RENDERDOC_DIR>) for every picture bug: capture the frame (the PC build with banjo_renderdoc_game_s, or
  Ryujinx's Vulkan back end with tools\run_ryujinx_rdc.ps1), read it with the tools\rdc_*.py scripts inside qrenderdoc
  (tools\rdc_run.ps1), and find the draw and the cause in the capture before changing code. Compare a Ryujinx capture
  with a PC capture of the same moment: the emulator translates the shaders itself.

Read first, in this order
1. <BASE_DIR>\README.md and docs\building.md (the build, the code generation, PGO), then docs\ultimate-fixes.md (one row
   per fix the base took from another port, with how it was tested) and docs\off-by-default.md (what is off and why: the
   keys that fix one game's picture and break another's, the forms that are not exact, the console's measurements).
2. <BASE_DIR>\docs\katamari-notes.md and docs\codegen-crash-fixes.md: the example port's findings and the generator's
   known crashes. docs\xdk-notes.md for the XDK.
3. <BASE_DIR>\docs\optimising-functions.md and docs\hand-and-machine-optimising-guide.md, all of both: the hot-function
   method. Their numbers are other games'; their rules and tools apply to this port. Then docs\sasrt-batch-cc2.md (the
   machine copies' register liveness, and the calling-convention traps).
4. <RENUT_DIR>\docs\porting-start-here.md: the reading order, what is current, the method rules, the build and test
   essentials of this PC.
5. <RENUT_DIR>\docs\porting-guide-renderer.md, all of it. Section 6b lists the items measured SLOWER ON THE CONSOLE:
   the base already has them off; keep them off.
6. <RENUT_DIR>\docs\enhancements-reference.md: the table at the top, then the keys this game uses.
7. <RENUT_DIR>\docs\deko3d-renderer.md, record-time-draw-path.md, codegen-enhancements.md, pgo-lto.md,
   deko3d-shader-optimisations.md, shader.md, controls-and-keyboard.md, and title-update-3.md if the game has a title
   update or DLC.
8. https://github.com/ChanseyIsTheBest/peggle2-nx/blob/main/docs/porting-a-360-game-prompt.md: the first ReXGlue port's
   order of work. It is older. Where docs differ, the ultimate base's win, then renut-nx's.
9. <RENUT_DIR>\docs\xdk-versions.md: the XDK build each of our ported games was made with.

The work, in order
1. Code generation and the PC port. Set up the project from the base's structure (CMake, the SDK, tools, the
   post-codegen passes). Take out what belongs to Katamari first: its function addresses and hooks
   (app/src/katamari_keys.cpp and the hook files), app/src/cpu/hot's lists, hand.txt and natives,
   app/indirect_promotions.txt, pgo/ and pgo-inplace/, its icon, its tomls and their generator
   (tools/make_katamari_tomls.py, app/katamari_switch*.toml, dist/switch/katamari-nx) and its DLC pack list (stale
   addresses also block copies: see "Stale mentions" in optimising-functions.md). Run code generation for <GAME>
   (tools/codegen.ps1, then python tools/post_codegen.py in full), and get it booting to gameplay on the PC with Renut's
   renderer (the Vulkan back end). Fix what stops it (imports, kernel calls, file system, input). Record every
   game-specific fix in docs/<SHORT>-notes.md.
   Then CG-13's census, as soon as the PC build reaches gameplay: the Switch build runs all of the game's float code in
   the host's flush mode (BLUR_FPCR_ONE_MODE, on by default there: no `msr fpcr` between its VMX and scalar code, a
   large CPU saving), which turns a scalar denormal into zero where the Xenon keeps it. Run
   `tools/cg13_census.ps1 -Seconds 300` with this game's input script, in gameplay and in its most physics- and
   audio-heavy scenes (the PC build keeps the option off so the census can count; building.md section 6). 0 in every
   window: keep it on. Any window with a count: the script lists the sites; look at what each computes (a value
   decaying to zero is harmless; a division or normalisation of it, a guard against zero, an input-based replay's
   simulation is not), write them down in the notes, and if one matters tell me before building the Switch folder with
   -DBLUR_FPCR_ONE_MODE=OFF. Run it again after a large change to the code generation.
2. Check the calling convention before trusting the machine copies' liveness: run tools/cc_entry_census.py and
   tools/cc_exit_census.py on the generated tree. The entry census must say none; every read the exit census lists must be
   value-independent (look at each). If not, generate with SASRT_REG_LIVENESS_EXIT_ABI=0 (and HOOKS_ABI=0 for a vector
   hit) and write down why.
3. The native deko3d renderer for the Switch. The base already has Renut on plume's deko3d back end
   (app/src/renut_engine/gpu, app/thirdparty/plume, the uam shader pipeline, tools/switch_shaders.ps1 for the shader
   package, tools/deko3d_variant_cache.py for the DKSH cache). Get it drawing this game, with NVK not linked (deko3d
   only). Nothing Katamari decided carries over untested: the game-specific decisions of porting-guide-renderer.md
   section 2 ("What your game decides") each get a census first, including where this game keeps its shaders. The
   base's defaults to settle for this game:
   - SMAA is on (banjo_renut_smaa) but runs only right after the draw whose pixel shader is in
     banjo_renut_smaa_after_ps; until this game's final composite (the last full-screen draw before the HUD) is listed
     there, the 10 s line says "SMAA ... 0 frames". Find it with banjo_renut_diag_draw_log_s in gameplay, not a menu.
   - MSAA is off (msaa_level 1). The game's own count is a decision for me (it changes the picture).
   - The keys of off-by-default.md section 1 fix one game's picture: turn one on in this game's toml only after its
     own A/B shows the same fault.
4. Test in Ryujinx. Build the NRO and run it with tools/run_ryujinx.ps1 and this port's own data folder, to the title
   screen and an attract loop or first gameplay, with captures every 15 s and the verify keys on. Fix every visual
   difference against the PC build, each found in a RenderDoc capture first (Appendix B). A new shader package means the
   device compiles its variants on the first run: make the DKSH cache again before judging hitches.
   On the console, when I say: the run order of Appendix A (upload with switch_remote.py put, the launch checked by its
   tile's name and its title ID, the game's input_script for the presses, screenshots with shot, the logs fetched with
   get, my own setup put back after).
5. The enhancements. The base has renut-nx's and the other ports' CPU, GPU and shader enhancements on by default except
   those off-by-default.md lists; each still gets checked on this game (its key and in-run A/B target, in the emulator
   with the verify keys: 0 differences). The section 4 trials (vivapinata_shader_noearly's A/B 151 and the like) are per
   game: measure them on the console before turning one on. The ones that change the picture become configs for me.
   Audio (blur-nx's stereo check, port 135): race on the PC with audio_channel_census = true; the 10 s "[audio]
   channels with sound" line shows which of the six channels the game fills. Again with audio_speaker_config = 1
   (analog stereo): if the centre, LFE and rear channels drop to 0, the game mixes stereo itself when told, which
   saves its mixer work (blur-nx: ~+8 % fps from its mixer doing 2 channels, not 6); measure it on the console and
   match the old loudness with audio_stereo_frame_gain (compare audio_dump_s dumps, folded). A game that ignores it
   needs patching per game (blur-nx's tools/stereo_mix.py), worth it only when audio is near the top of the CPU
   table. audio_switch_pump_pairs (off) trades +5.3 ms of latency for CPU: my choice.
   Three keys are decided from this port's own log (blur-nx, 2026-11-18; port 177), in gameplay windows only (not
   menus or loads), with no A/B: a 0 is enough. The base's defaults stay; set them in the port's own toml:
   - `ultimate_renut_replay_inherit_same` (true): the line "[renut] replays with the inherited words as recorded
     (ultimate_renut_replay_inherit_same): N of M". N is 0: false.
   - `sasrt_gpu_skip_inert_draws` (true): the line "[sasrt gpu] GPU round ..., in the last 10 s:
     sasrt_gpu_skip_inert_draws (on) N". N is 0 in every gameplay window: false.
   - `banjo_renut_tags_check` (32): the line "[renut] recording from marks (banjo_renut_tags): ..., N whole checks,
     M chunks changed without a mark". M is 0 over whole gameplay sessions: 256.
6. Hot functions, by the base's optimising-functions.md. Its two tracks replace any fixed top-5 or top-100 target.
   - Machine-optimise as many functions as possible, at every opportunity: every function any session samples (Ryujinx
     and the PC's profiler now, the console when I send a log), with tools/localize_native.py and
     tools/switch_hot_bulk.py --merge, then further eligible functions in batches, watching NRO size, free heap and build
     time. Profile in Ryujinx as well as on the PC, because we are optimising for ARM.
   - Hand-write a function only from console profile data, past the guide's gate, with the evidence recorded in hand.txt.
     A PC or Ryujinx profile never qualifies, however clear it looks, so expect no hand-written natives until I send a
     console profile.
   Verify each against the generated code before it is on (tools/run_pc_verify.ps1: 0 differences, with the counts showing
   it was reached; a guard build with banjo_hot_poison_dead on shows a wrong liveness claim the guard cannot see).
7. PGO + LTO for the Switch build only when I say, in the guide's order: the last batch of machine copies, any
   hand-written natives, the instrumented build, my console session, then the optimised build (building.md). Nothing in
   the tree changes between the training race and the final build; a fix found after training means a new training run.
   That includes BLUR_FPCR_ONE_MODE: the profile is trained with the setting the build ships with.
8. A package for my console by <STOP_TIME>:
   - the NRO
   - renut.toml with the shipped settings
   - renut_play.toml with the diagnostics off
   - renut_ab.toml with 30 s halves and a do-nothing control target
   - the ELF and a source snapshot, so a console profile maps to source lines
   - notes on what to test, and how to turn on a console profile
   And, if the port boots to gameplay, its web installer page: a copy of <INSTALLER_DIR> with this game's game.json and
   tools/make_release.py's release folder (the player's own game files make the package in their browser).
9. Fixes that are not this game's own (an SDK bug, a renderer fault any game could hit, a faster exact form): write a
   handoff for the ultimate base (a patch against <BASE_DIR> and a note: what, why, how it was tested, its key and
   default) in <WORK_DIR>\out, and name it in the final report.

Rules
- Pixel-identical by default. Anything that changes the picture is a separate config and my decision.
- The console decides performance. The PC and the emulator are proxies: judge on the A57 at 1020 MHz. Never compare load
  times or frame rates between emulator runs.
- A console A/B is the same NRO both ways (an in-run A/B or ABBA order) with a control from the same session: the
  console's speed drifts a few percent between sessions and after a reboot. Read fps from Renut's wall-clock 10 s lines;
  the profiler's per-second counters can read high when core 3 is busy.
- Remove work, do not move it to another thread or core.
- The console and the PC are shared with other port sessions. Arrange console time with them, never take it mid-run.
  On the console's SD card touch only <SD_FOLDER>, and delete nothing there.
- One Ryujinx run at a time, and only when none runs and nothing is compiling (a run starved by another session's build
  is void: run it again). Stop only your own run's process, never Ryujinx by name. Check free commit memory (8 GB or
  more) before a run. Restore the emulator SD's toml after each run.
- Never run two builds, never build during an emulator run, never edit sources while a build compiles. Run
  memory-heavy tools (a difftest of a large shader with an export window grew to 10 GB) one at a time and watch memory.
- Check disk space before code generation, builds and packages. If the disk is full, stop and tell me; do not delete
  anything outside this port's own folders.
- No downloads, no system or firewall setting changes, no git push without my say. Before anything is published,
  check it for personal details (names, email addresses, home or user paths, home-network addresses): commits use my
  GitHub noreply address.
- Bash heredocs mangle backslashes: write files with the editor tools.
- Credit Renut's renderer (its author permits reuse in my ports), the ultimate base and the ports whose fixes it
  carries (its README, THIRD_PARTY_NOTICES.md and docs/ultimate-fixes.md name them), renut-nx, sa2-nx, NetherSX2_nx,
  battd_nx and the bejeweled3-nx / zuma ports. GPL-3.0 where the sources are.
- Keep docs/<SHORT>-notes.md and a progress log current as you go, so the final summary is accurate.

At <STOP_TIME> stop, and report:
- what works on the PC, in the emulator, and in the package
- what was measured, and how (same-session control or not)
- what is next
- the fixes handed off for the ultimate base
- anything that needs my decision
```

## Appendix A: the console over the network (sys-botbase and sys-ftpd-light)

**Setup, once, by the console's owner.** The console runs Atmosphère. Install two sysmodules from their releases:
**sys-botbase** (masagrator's fork; it listens on TCP 6000 for controller presses, screenshots, the running title ID and
memory reads) and **sys-ftpd-light** (an FTP server on TCP 5000, anonymous login, for the SD card). Each release unpacks
into `/atmosphere/contents/<its program id>/` with a `boot2.flag`, so it starts at boot; reboot once. Give the console a
fixed address on the router. On the PC, write that address into `out/switch_host.txt` and the port's folder (for example
`/switch/pacman-nx`) into `out/switch_sd_root.txt` (`out/` is not committed), or pass `--host` and `--root`.

**`tools/switch_remote.py`** (the base's; rr6-nx's of 2026-11-04 as sasrt-nx grew it). In Git Bash, set
`MSYS_NO_PATHCONV=1` for commands that name a card path, or Git Bash rewrites `/switch/...`.

| Command | What it does |
| --- | --- |
| `shot [OUT.jpg]` | A screenshot of what the console shows (sys-botbase's `pixelPeek`) |
| `click A [B ...]` | Button taps, one after another (A B X Y L R ZL ZR PLUS MINUS HOME CAPTURE DUP DDOWN DLEFT DRIGHT LSTICK RSTICK) |
| `hold BTN [ms]`, `chord A B ... [ms]`, `stick L\|R X Y [ms]` | A button held, several held together, a stick moved (X and Y in -1..1) |
| `title`, `version`, `heap` | The running title ID, sys-botbase's version, the game's heap base |
| `ls [PATH]`, `get REMOTE [LOCAL]`, `put LOCAL REMOTE` | The port's folder on the card: list, download (a folder: every file in it), upload |
| `album-ls`, `album-get` | The Album, read only (screenshots the owner took) |
| `raw "CMD ..."` | One sys-botbase command as it is, from a short list that cannot write memory or reach the card |

Every card path is checked against the port's folder and refused outside it; nothing on the card is ever deleted.

**A console run, in order.**

1. The owner's word, and no other session using the console (the sessions here agree a time and say "done" after).
2. `put` the NRO and the run's toml into the port's folder (the toml can carry `input_script` / `input_script_sticks`:
   the presses from inside the game, at exact times from its start, more reliable than presses over the network).
3. `shot` on the Home menu, and read the selected tile's name: forwarder icons look alike. Then `click A` once, and after
   a few seconds `title`: it must be this port's title ID (read it once with `title` while the game runs). On any other
   title, stop: no more presses.
4. While it runs: `shot` for the evidence. The first sys-botbase press after a launch can be lost (rr6-nx: release, then
   press); after the console has slept, `raw detachController` once, or the presses are ignored.
5. At the end: close the game (`click HOME`, then `X`, then `A`), `get logs` (the profiler's lines are there), and put
   back the owner's own NRO and settings if the run replaced them.

sys-ftpd-light runs out of sockets after a few transfers on one connection (451 on every PASV for a while):
switch_remote.py opens a connection per file and retries. rr6-nx's and sasrt-nx's `tools/console_run.py` automate a
whole run for their games (upload, the checked launch, timed presses, logs, a contact sheet of the shots): start from
one for a new game. Measure on the console only with the same NRO both ways (an in-run A/B target or ABBA order) and a
control from the same session; read the fps from Renut's wall-clock 10 s log lines.

## Appendix B: RenderDoc

**Setup.** RenderDoc 1.46, the portable 64-bit build, at `<RENDERDOC_DIR>`; set the `RENDERDOC_DIR` environment
variable to that folder (`tools/run_ryujinx_rdc.ps1` and `tools/rdc_run.ps1` read it; without it they use banjo-nx's
copy). `tools/run_ryujinx_rdc.ps1` names Katamari's emulator and data folders, as `tools/run_ryujinx.ps1` names the
base's: point both at this port's.

**Capturing.**

- **The PC build** (Renut on Vulkan): start `renut.exe` with RenderDoc's Vulkan layer loaded, from qrenderdoc's *Launch
  Application*, or with `VK_LAYER_PATH=<RENDERDOC_DIR>` and `VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture` set, and give
  it `banjo_renderdoc_game_s = N` (the next three game frames, N seconds after the first game frame) or
  `banjo_renderdoc_capture_s = N` (one frame, N seconds after the renderer starts). The captures go to `capturas/rdc`
  beside the exe.
- **The Switch build in Ryujinx** (deko3d, translated to Vulkan by the emulator):
  `powershell -File tools/run_ryujinx_rdc.ps1 -Seconds 420 -Name rdc1 -Toml <toml> -Times "330,345"` runs the emulator
  with RenderDoc's layer and takes captures at those seconds (`tools/rdc_trigger.py` inside qrenderdoc), into
  `out/emu_runs/<Name>`; the same rules as `tools/run_ryujinx.ps1` (one emulator at a time, free memory). The emulator's
  own shader translation is in that picture: compare with a PC capture of the same moment before blaming the port.

**Reading a capture.** `powershell -File tools/rdc_run.ps1 -Script rdc_passes.py -In <capture.rdc> -Out <folder>` runs one
of the `tools/rdc_*.py` scripts inside qrenderdoc and prints its report; each script's header names its `RDC_*` settings
(`RDC_EVENTS`, `RDC_PIXELS`, `RDC_TARGET`...).

| Script | What it gives |
| --- | --- |
| `rdc_passes.py` | Every render pass's colour target as a PNG, and passes.txt: start here, to see where the picture goes wrong |
| `rdc_last.py` | The frame as it was shown |
| `rdc_draws.py` | The draws of a range: their shaders (Renut labels each draw with its vertex and pixel shader), targets, textures, blend |
| `rdc_event.py`, `rdc_draw_detail.py` | Chosen draws: depth, stencil and rasterizer state, the vertex outputs (NaN, w <= 0), a pixel history |
| `rdc_pixel.py`, `rdc_history.py` | Which draws wrote a pixel, and what each wrote |
| `rdc_bindings.py` | What a draw can read, every descriptor (also for the emulator's translated shaders, where RenderDoc shows none) |
| `rdc_state.py`, `rdc_stencil.py` | Fixed-function state; stencil contents |
| `rdc_save.py`, `rdc_pick.py`, `rdc_mips.py`, `rdc_channel.py` | Textures saved as PNG, pixel values, every mip level's range, one channel |
| `rdc_buffer.py`, `rdc_mesh.py` | Buffer contents; a draw's vertices |

A worked example: [katamari-notes.md](katamari-notes.md), "The depth-of-field blur: found with RenderDoc and fixed"
(a multisampled depth fetch the emulator replaced with 0, found from the capture's bindings and the emulator's log).
renut-nx's `docs/deko3d-renderer.md` section 12 has more.
