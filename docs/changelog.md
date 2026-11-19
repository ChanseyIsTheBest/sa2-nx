# Changelog

One entry per port of the ultimate base, by number. Each port's full row (its source port and commit, the evidence,
how it was tested here, its switch) is in [ultimate-fixes.md](ultimate-fixes.md); the switches that are off by
default are in [off-by-default.md](off-by-default.md).

Ports 1-43 are the first round: the universal fixes of the other ports' renderers, sorted by area in the README's
"What's in it" table and listed in ultimate-fixes.md. From port 44 on:

- On the console the base first cost Ridge Racer 6 4.1 ms of GPU a frame against its own build, at the same frame rate
  but with no headroom left. Port 44 gives 3.2 ms of it back (its game passes at its own build's times), and port 45 (the
  depth decompress only for a read) 0.5 ms more. 0.4 ms, in the game-gamma ramp's present, is still open.
- Port 46 (sasrt-nx, from a console crash) puts the SDK's input system under one lock: a game that reads its
  controllers from more than one thread could free the device list twice.
- Ports 47 and 48 (rr6-nx) take the locks out of Renut's object lookups and shorten the guest clock's read. On the
  console they took Ridge Racer 6's drawing thread from its limit (42-48 fps while racing) to 60 fps with time to spare.
- Port 49 brings sasrt-nx's three GPU rounds (45 items, each with its own key and A/B target). In Ridge Racer 6 they
  take 0.4-0.9 ms of GPU a frame (13.6 -> 13.2 racing); in sasrt-nx's game 68 -> 47 ms. The optimiser passes act once a
  game's shader package and DKSH cache are made again.
- Port 50 is sasrt-nx's fourth GPU round: memexport draws drawn with the rasterizer off no longer go through the
  point-sprite geometry shader, and their export windows are read from the constant buffer. sasrt-nx's game went from
  46.6 to 38.8 ms of GPU a frame; games without memexport draws (Ridge Racer 6) are unchanged.
- Port 51 is sasrt-nx's fifth GPU round (its pixel-identical items): a texture fetch's sign, gamma and exponent
  conversions become constants of the draw, so each pixel shader keeps only the conversions its textures use (sasrt-nx:
  -3.9 ms of GPU a frame; Ridge Racer 6, whose fetches are nearly all plain, flat). It also fixes the DKSH cache tool,
  which read the pipeline list 4 bytes off since the first round.
- Port 52 is what was left of sasrt-nx's changes for any game: dynamic resolution that steps down only when the GPU, not
  the CPU, holds the frame (and any range on the Switch), SMAA after a composite with more than one target, the depth
  restore's whole clear, and a 1 s grace before a controller HID reports absent is disconnected for the game.
- Port 53 makes the CPU time a frame the game thread's own work: the frame cap's sleep and the display pacing's wait
  count as waits, so the FPS overlay's CPU figure and port 52's dynamic resolution see the game's real load.
- Port 54 is sasrt-nx's render-thread round: the EDRAM depth restore's source found from each depth target's own few
  candidates instead of two scans of every target a draw, memory-export seeds written into CPU-cached memory, two
  per-draw counters inlined, and A/B lines that give draws a frame and microseconds a draw.
- Port 55 is feedingfrenzy2-nx's SDK fixes: controllers 2 to 4 sign in as guests when a game opens the sign-in screen,
  players 3 and 4 get their own button maps, the leaderboard calls fail safely, the exit no longer crashes, freed memory
  reads as zeros when it is used again, and five opt-in VMX accuracy keys for the code generator.
- Port 56 is mk9-nx's fixes and ds3-nx's polling back-off: faster physical allocations at loads, fallbacks for a rare
  boot hang, audio that keeps going when the output stalls, a fix for huge stray triangles, one-pass shader compiles
  while a draw waits, the first controller to press Start as player 1, and polling threads that sleep instead of
  spinning a core.
- Port 57 is ds3-nx's SDK fixes: the time a console spends asleep or at HOME no longer reaches the game as one huge
  frame, package reads are one at a time, XMA audio no longer replays sound it has played, modules are found by file
  name when a game remaps its paths, and multi-disc games can take the disc swap over.
- Port 58 is SASRT's memory round: a governor that watches the Switch's free memory and gives caches, idle render
  targets, kept thread stacks and GPU memory back before it runs out (SASRT's docked races went from crashing to
  1,080-1,220 MB of CPU-side heap in use, from 1,526), and the fix for a pipeline shortcut that could draw with the
  previous draw's specialised pipeline (SASRT's broken octopus).
- Port 59 turns controller rumble on by default (the game's own vibration, HD rumble on the Switch's controllers;
  `input_rumble = false` turns it off) and brings two small fixes from LostOdysseyRecomp: texture descriptors made once
  per view, and multiple-object waits without heap allocations.
- Port 60 takes port 56's first-press player swap back out (`input_first_press_is_player1`): the players follow the order
  the controllers connected in again. Ridge Racer 6's Multi Battle found no second controller with it.
- Port 61 lets a game file sit on the card packed as `NAME.lz4b` (LZ4 in 256 KB blocks), read as `NAME`. The ultimate
  installer packs the files a port names; Katamari's archives go from 2.1 GB to 0.9 GB.
- Port 62 stops a release NRO with the web installer's shader package beside it from logging a false "no GLSL package"
  error.
- Port 63 keeps port 04 (a slot after SetTexture(NULL)) to fetches of the same data, so a resolve's texture is not remade
  for other data (Viva Pinata: 64 "[flash] ... dropped" lines in a 200 s run, now none).
- Port 64 is sasrt-nx's GPU hang fix: with dynamic resolution, a texture over a depth resolve is made again when its size
  no longer matches, and image copies and resolves never write past an image.
- Port 65 makes a thread blocked on an event sleep on an address on the Switch, so a woken thread no longer holds the
  event's mutex before it runs (SASRT's main thread spent 5.6 % of its time waiting there; now 0 %).
- Port 66 fixes the PC build's intermittent crash at start in the FPS overlay (the app's dialogs drew before Renut's overlay
  had begun its first frame).
- Ports 67-69 are blur-nx's fixes: the replay inheritance no longer reads and writes past its bit array (67), XenosRecomp
  translates vertex shaders with two bindings (68), and a new port's first builds and runs no longer stop at the path
  wizard, a missing generated/hot, switch_shaders.ps1's stderr or a stale PCH (69).
- Port 70 lets machine copies take in functions with guest atomics (sasrt-nx): each atomic stays as it is, and the guard's
  comparing forms stop before it. A game gets more copies when it remakes its bulk list; retrain its PGO profile after.
- Ports 71-74 are sasrt-nx's machine-copy round, every optimisation on: the SDK's small helpers forced inline and
  setFromMask in NEON (71); VMX's estimates and multiply-add in their faster AArch64 forms, not bit-identical but no
  further from the 360 than before (72); NEON idioms of the generated code (73); and ten copy-generator ideas (74).
  The copies' code changes: retrain a game's PGO profile after them.
- Port 75 is sasrt-nx's renderer formats batch: 2_10_10_10 in the console's own 10:10:10:2 layout, EDRAM formats 10
  and 12 no longer RGBA8, 32-bit float textures point-sampled, and a log of the formats a session makes.
- Port 76 is blur-nx's renderer fixes (keys 211-221): EXPAND / FLOAT resolve aliases, partial resolves that keep
  their destination, occlusion queries answered (a game hooks its own GetData), stack-held surface headers, the
  resolve exponent bias, the EDRAM inherit and more; Viva Pinata's fade mask to recheck (key 218).
- Port 77 is the rule for keys that break a game's picture: off in the base by default, on in the toml of the game
  that needs them (Blur's 218 and Halo 3's replay inherits so far). docs/off-by-default.md lists every switch that
  is off by default, why, and what is known of its speed.
- Port 78 builds the machine copies' guard forms only into guard / verify builds (Katamari's Switch NRO 11 MB
  smaller; tools/build_switch_guard.bat for a Switch guard build), and makes tools/switch_hot_bulk.py leave small
  copies out of a remade bulk list.
- Port 79 keeps emptied render-target memory blocks for the next targets (sasrt-nx: a dynamic-resolution step
  no longer makes ~33 blocks in one frame; its worst frames per window 108 -> 59 ms).
- Port 80 lets a submit clean the upload ring's range in progress itself instead of waiting for the cleaner thread
  (sasrt-nx: no more 280-350 ms render-thread waits at its race start).
- Port 81 puts the machine copies in place of their generated functions on the Switch (sasrt-nx: no hooks, the copies
  inlinable, its game thread 4 % faster and the NRO 4 MB smaller); their keys, the guard and A/B 70 need a hooked
  build (tools/build_switch_guard.bat, IN_PLACE=OFF, the PC), and the in-place form has its own PGO profile
  (pgo-inplace/). sasrt-nx's guide to hand and machine optimising is
  [docs/hand-and-machine-optimising-guide.md](hand-and-machine-optimising-guide.md).
- Port 82 adds sasrt-nx's GPU rows 1, 2 and 4, marked untested (exact by construction; a 10 s log line counts
  what each one did; Katamari hits row 1, with the same frames on and off). Port 83 adds
  tools/profile_breakdown.py (a console stack profile per thread, against the build's map).
- Port 84 turns the bulk copies' link-register elision off by default (vivapinata-nx's guard found it
  differing between a copy's two forms).
- Port 85 takes blur-nx's vector-lane reads: an `stvewx` / `stvehx` lane picked by its address is read from
  the register instead of through the stack (the A57 cannot forward that load); a start-up self-check
  and an SDK test check every lane. input_script takes LT and RT.
- Port 86 turns Blur's EDRAM inherit (key 220) off by default: on the console it drew garbage tiles over Viva
  Pinata's title (blur-nx turns it on).
- Port 87 replays a command buffer a game runs once per predicated tile once for every tile
  (vivapinata-nx's tiles once: -3.74 ms of GPU a frame in its garden on the console).
- Port 88 skips or copies a multisampled resolve of a target unchanged since its last resolve
  (vivapinata-nx: two of its four 2D-engine blits a frame gone, -0.14 ms of GPU).
- Port 89 puts the SDK's OBJECT libraries (audio, core, file system, input, UI) in the Switch LTO and
  PGO too, four files kept out (sasrt-nx: process CPU -0.7 %, render thread -1.9 %).
- Port 90 takes blur-nx's CPU batches 1 and 2: vector conversions, shifts, prefetches and zeroing in
  fewer instructions, grouped card checks, NoteSetWords and the XMA sweep faster (blur-nx's console:
  16.6 -> 18.3 fps). Two of them change the code generator: run a code generation
  (`tools/codegen.ps1`) and `python tools/post_codegen.py` after it.
- Port 91 applies the game's gamma ramp at scan-out from a table (two fetches a pixel instead of three;
  vivapinata-nx: -0.60 ms a frame), port 92 simplifies the fetches through sampler index + k
  (tools/deko3d_glsl_opt.py's samplerk: a shader package made again takes it).
- Port 93 takes sasrt-nx's exact items from its batches C and D the base has the code for: the critical
  section's leave as a raw export, replay and capture prefetches, the render scale's factor and
  coordinates without divisions, pooled held exports.
- Port 94 takes vivapinata-nx's shader round: the fetch folding on vertex shaders too, a fetch's texel step at
  1x without its scale chain, the vertex fetch types for every draw (vivapinata-nx: -8.13 ms of GPU a
  frame with port 92). Take it with a shader package made again (tools/switch_shaders.ps1).
- Port 95 (from vivapinata-nx) makes Renut's early alpha test optional per game (`vivapinata_shader_noearly`, off;
  A/B 151 on the console says which shaders gain, tools/noearly_keep.txt keeps the others' test). Take it with a shader package made again (tools/switch_shaders.ps1).
- Port 96 rounds the single-precision fused multiply-adds (fmadds and kin) once, as the Xenon does, instead of
  a double fused multiply-add rounded again to single; chains stay in float registers (blur-nx: CPU 190 -> 184
  ms a frame). On by default (CMake BLUR_FMADDS_SINGLE); it needs the code generated again (tools/codegen.ps1,
  then tools/post_codegen.py).
- Port 97 turns pacman-nx's vblank display sync off by default (`pacman_vblank_display_sync`): it fixed
  pacman-nx's input lag at full speed but cost blur-nx 15-18 % of its frame rate at 18-21 fps with no latency
  gain. A game at the refresh rate that wants it turns it on in its toml (docs/off-by-default.md).
- Port 98 fixes the specialised pipelines in an in-run A/B's off half (A/B 15-17): each draw made a job it
  never matched (vivapinata-nx: 153 thousand jobs in 13 minutes, the off halves at 1.5-10 fps).
- Port 99 (from vivapinata-nx) moves a pixel shader's final alpha test up to right after what it needs, so a
  discarded pixel skips the colour arithmetic (`vivapinata_shader_earlykill`, on; the same pixels). Take it with a shader package made again (tools/switch_shaders.ps1).
- Port 100 (from dishonored-nx) declares `invariant gl_Position` in every vertex shader, so depth-equal passes
  drawn with different vertex shaders cover the same pixels (Unreal Engine 3's bright silhouette edges). Take it with a shader package made again (tools/switch_shaders.ps1).
- Port 101 (from dishonored-nx) converts only the committed part of a vertex stream or fetch copy and zeros the
  rest, where a range past the game's allocation faulted the render thread (Switch).
- Port 102 (from dishonored-nx) lets XenosRecomp translate a vertex shader that exports to an interpolator its
  table does not list (an assert or a crash before). Take it with a shader package made again (tools/switch_shaders.ps1).
- Port 103 (from dishonored-nx) applies the guest core layout from each thread at its own next wait instead of
  re-masking running threads, the cause of the first-Swap hangs that keep `banjo_renut_core_layout` off.
- Port 104 (from dishonored-nx) watches a tiled texture uploaded with an all-zero level until the game streams
  its data in, then uploads it again (Dishonored's black walls and heads at a distance).
- Port 105 (from dishonored-nx, sa2-nx's) feeds Renut's render thread from a lock-free ring instead of a vector
  under a mutex (`dishonored_renut_lockfree_queue`; sa2-nx: the queue's locks 0.83 -> 0.18 ms of the game thread).
- Port 106 (from dishonored-nx) clamps a pixel shader's output into a 7e3 target to the 7e3 range, as the Xenos
  stores it (`dishonored_renut_clamp_7e3`; constant 990, sasrt_words[7] bits 28-31). Take it with a shader package made again (tools/switch_shaders.ps1).
- Port 107 (from blur-nx's CPU batch 3) reads dcbz's block check once, rounds frsp with one range test and
  uses the plain single-precision forms where both operands are proven single. Take it with a code generation.
- Port 108 (from blur-nx) stops leaf_dead_stores counting functions named only in the promotion and draft lists
  as address-taken, so their copies write back only what callers read.
- Port 109 (from blur-nx) adds `switch_thread_priorities` and `switch_thread_cores`: per-thread host priorities
  and core masks by thread name, for A/Bs (empty by default).
- Port 110 (from blur-nx) makes a mutant released before its native object exists fail as not owned instead
  of crashing.
- Port 111 records blur-nx's batch 3 notes: the XMA frame cache size per game, and the two PGO options it
  tried and does not use (docs/building.md).
- Port 112 makes each guest thread's host stack 8 MB by default (16 before; `switch_guest_stack_mb`: a game
  with a deep recursion sets 16 in its toml).
- Port 113 (from vivapinata-nx) makes difftest_game.py read `--cbset`, adds `--texalias` / `--texcopy`, and
  removes each run's temp folder when it ends.
- Port 114 (from vivapinata-nx) adds `renut_gpu_profile_counters`: pixels shaded and Zcull's words beside each
  GPU profile line (deko3d, off).
- Port 115 (from vivapinata-nx) can leave out multisampled depth resolves nothing reads until something does
  (`vivapinata_depth_resolve_skip_unread`, off: not exact in principle).
- Port 116 marks Katamari's `disable_msaa` as not read (`msaa_level` sets the samples).
- Port 117 takes a home-network address out of `log_network`'s help text.
- Port 118 makes MSAA off (`msaa_level` 1) and SMAA on (`banjo_renut_smaa`) the base's defaults; a game lists its
  final composite shader in `banjo_renut_smaa_after_ps` for SMAA to run.
- Port 119 (sasrt-nx's, from vivapinata-nx) lets dynamic resolution step up when the next height has 1 ms of room,
  one step at a time, with a grid of heights per mode.
- Port 120 (from blur-nx) adds `audio_xma_kick_without_lock` (off): an XMA kick no longer waits for a decode in
  progress (Blur's console +5.9 % fps); the XMA frame cache can go up to 512 MB.
- Port 121 (sasrt-nx's batch CC2) gives the machine copies whole-program register liveness with the vector
  registers and forwards stack stores into vector loads (sasrt-nx: -2.5 % CPU). Check a game's convention with
  tools/cc_entry_census.py and cc_exit_census.py, and take it with a code generation.
- Port 122 (sasrt-nx's) makes tools/pgo_merge.py refuse a folder of streams from more than one build.
- Port 123 (from blur-nx) adds CG-13, all guest float code in one flush mode (`BLUR_FPCR_ONE_MODE`, OFF: not
  bit-exact for scalar denormals; Blur's console +4.6 % fps), and its census.
- Port 124 (from blur-nx) counts every promoted call target as address-taken in leaf_dead_stores.
- Port 125 (from blur-nx) turns the user clip slot off for pipelines that write no clip distance (Renut's host
  passes on deko3d; not Blur's black band, which is its race intro: port 154).
- Port 126 adds tools/switch_remote.py (sys-botbase and sys-ftpd-light, the port's own SD folder only, no delete)
  and the porting guide's appendices on the console and RenderDoc.
- Port 127 (from blur-nx) keeps the console's EDRAM tricks working when `msaa_level` lowers a target's samples
  (port 118's default of 1): the sample-grid resolve, the EDRAM inherit, alpha to coverage with the game's own
  alpha test, and the depth restore's candidates go by the console's sample count.
- Port 128 (from blur-nx) lets SMAA run on RGBA16F composite targets (Blur's float composite).
- Port 129 (sasrt-nx's finding) makes port 45's deferred depth decompress drop the bound targets' serial and mark
  its texture used, as the other decompresses do (a later bind of the same targets could be skipped).
- Port 130 (sasrt-nx's R2) keeps a resolve destination's depth words probe when a dynamic-resolution step makes
  it again, so words nothing reads stay unwritten.
- Port 131 (sasrt-nx's) adds two resolve diagnostics, off by default (`sasrt_gpu_diag_depth_words`,
  `sasrt_gpu_diag_resolve_split`), and times the exclusive pairs in the profile header.
- Port 132 adds `tools/ult_port_sync.py` (sasrt-nx's), which takes the base's later ports into a game's port
  (docs/building.md section 7), and sasrt-nx's notes on what base ports cost it.
- Port 133 turns CG-13 (`BLUR_FPCR_ONE_MODE`) on in the Switch build at the user's word, and adds
  `tools/cg13_census.ps1`, the PC check of each game, to the porting prompt.
- Port 134 (from blur-nx) is the XMA frame cache v2: pair-keyed with clock eviction, one frame of catch-up,
  and the codec opened only for a sound that needs it.
- Port 135 (from blur-nx) adds the paired audio pump (off), the speaker config and channel census keys, and the
  stereo frame gain.
- Port 136 (from blur-nx) adds the any-mode float loads' second pass (vector forms under CG-13), the fused
  known-single forms, prefetch rules for the copies and the hand natives' lighter card marks.
- Port 137 (from blur-nx) makes the texture-extent memo two ways a set and names the devices that draw.
- Port 138 (from blur-nx) narrows the Begin*Vertices captures to the constants their shaders read, keeps the
  deferred chain ordered across devices, and queues resolves and clears with only their own state.
- Port 139 (from blur-nx) takes handle lookups off the global lock, starts the timestamp timer only for a game
  that imports it, and adds the direct stereo path and finer XMA and profiler counters.
- Port 140 (from blur-nx) adds the near-exact switches (off) with their censuses, store cards by the copies'
  rule, and leaves out FPSCR writes that change nothing.
- Port 141 (from blur-nx) makes the guard take a NaN that differs only in its sign as the same.
- Port 142 removes `kernel_timestamp_timer_on_import` (port 139's M-38) at the user's word: the timestamp
  timer runs from start-up again in every game.
- Port 143 (from tw2de_nx) drops Xenia's 100 ms wait on saves and dialogs (with the event reset it needs) and
  throttles XMP polling; whole 64-bit arguments, the disc swap's event and the scatter read are off.
- Port 144 (from tw2de_nx) yields at the Xenon's spin hints and unpacks NORMSHORT2 with NEON; its exact FPU and
  VMX forms, the 360's estimate tables, exact_fp and the missing instructions are off by default.
- Port 145 (from tw2de_nx) redeems quiet vertex streams, keeps unused render targets, uploads only the written
  rows of dynamic textures, uses the GPU's 16-bit formats and presents from a helper thread.
- Port 146 (from tw2de_nx) releases mutants without their mutex when nobody waits and reads files in 4 MB
  pieces, host buffers directly.
- Port 147 (from tw2de_nx) adds the PowerPC instruction test NRO (Xenia's hardware-recorded tests) and the
  console's time zone (off).
- Port 148 (from blur-nx) takes Renut's recorded binds, its per-draw lookups and its record lock off the
  frame jobs' path, scans only the changed chunks for set words and adds the Renut audit's CPU batch
  (Blur's console: +3.5, +0.5 and +0.4 fps).
- Port 149 (from blur-nx) never leaves a sample-grid resolve in the view's image (the colour hand-over).
- Port 150 (from blur-nx) caches each indirect call site's target, loads doubles in the vector unit, drops the
  barriers of counted loops and gives the guest compare-and-swap and fences the PowerPC's orderings
  (acquire in a game with isync); the ABI-trust copies are off.
- Port 151 (from blur-nx) gives small C++ allocations per-thread free lists on the Switch (32 MB arena).
- Port 152 (from blur-nx) skips the APC lock on an empty queue and adds the startup-notification option,
  Sphaira's forwarder ids, the guard's keep list and the audio pump's frames (off).
- Port 153 (from blur-nx) lists the blocked guest waits in the stall watchdog's dumps and adds the profiler's
  interval, call-count switch and audout counters, and deko3d's crash breadcrumbs.
- Port 154 (from blur-nx) adds its census, signature, crash-report and RenderDoc tools, and corrects ports
  125 and 145.
- Port 155 (from blur-nx) scales a target's colour by its exponent bias as well as its alpha, and keeps a
  sample-grid resolve from being skipped as unchanged (Blur's menu backdrop). Take it with a shader package
  made again (tools/switch_shaders.ps1).
- Port 156 (from blur-nx) counts the game frames at Swap, so the captures' video works in a game that
  swaps without Present.
- Port 157 (from blur-nx) sends a GPU submission that nvservices refuses again in halves instead of
  aborting (Blur's map-load crashes, a Timeout on a full GPFIFO ring).
- Port 158 (from fnxf_xiii) builds the function registration file at -O0 in the Switch build (1,171 s of a full build down to 37 s).
- Port 159 (from fnxf_xiii) adds kernel and SDK fixes from fnxf_xiii, off by default (timer due times, profile settings, HOME close,
  PC keystrokes), and cleans SaltySD's page.
- Port 160 (from fnxf_xiii) makes the store cards evaluate their address once (less code, fewer loads).
- Port 161 (from fnxf_xiii) holds back barriers between buffer copies, runs the frame bookkeeping after the present, writes deko3d's
  log on its own thread and fixes the surface memo (the EDRAM base).
- Port 162 (from fnxf_xiii) speeds up uam's compiles and frees what each compile leaked.
- Port 163 (from fnxf_xiii) adds uam's register pressure passes, off by default (banjo_deko3d_uam_pressure).
- Port 164 (from fnxf_xiii) sends a frame to the GPU in pieces sized by the frame before, and takes scratch memory out of two host shaders.
- Port 165 (from fnxf_xiii) adds banjo_deko3d_swap_interval (1-4 refreshes an image).
- Port 166 (from fnxf_xiii) adds the MSAA sample mask and absolute wait timeouts, off by default.
- Port 167 (from fnxf_xiii) adds guest callers to the stall dumps, a GPU fault report and better gap tools (find_gaps.py,
  runtime_gap_resolver.py, code_pointer_scan.py).
- Port 168 (from fnxf_xiii) keeps a replaced texture a while for its content's return (no upload again).
- Port 169 (from fnxf_xiii) adds the scaler's source crop at present, off by default.
- Port 170 (from fnxf_xiii) lets a D3D fence wait for pending memexport results, off by default.
- Port 171 (from fnxf_xiii) untiles large texture levels on two helper threads with the render thread.
- Port 172 (from fnxf_xiii) lets the guest's vblanks follow the display's refreshes, off by default.
- Port 173 (from rehatched-nx) adds hook bodies for later XDKs (GetDeclaration, Bind and gamma with Flags), not bound.
- Port 174 (from rehatched-nx) can keep every bound surface's header as bound (banjo_renut_all_surface_headers, off).
- Port 175 (from rehatched-nx) translates a shader export to a register with no name as a plain register write.
- Port 176 (from rehatched-nx) lets input_script split at ';' and spaces, and adds the XDK 2.0.9407 notes.
- Port 177 (from blur-nx) adds Blur's Renut host batch (render thread ~16 % faster per draw on its console),
  1 % lows in the 10 s report, the image heap's spare chunk, the UI-upscale band fix and the game menu.
- Port 178 (from blur-nx) wires the game menu into the base's app (A+B+X+Y+L+R opens it, + closes it).
- Port 179 (from fnxf_xiii) adds the EDRAM carry, off by default (banjo_renut_edram_carry): a colour target drawn
  without a clear in a frame starts from the picture last written over its EDRAM base.
- Port 180 adds menu_background_opacity: the game menu's black background in percent, a row under Menu colour and
  Menu font.
- Port 181 turns port 168's texture park off by default: on Blur's console it drew whole races with ghosting,
  colour fringes and grain.
- Port 181 corrected (Blur, the same day): the texture park did not break Blur's picture (that was slow
  instrumented builds); it stays off because it took back no texture in Blur or Katamari.
- Port 182 (from blur-nx) sends the game menu's combo once a press across pads (Ryujinx with three pads opened
  and closed the menu at once).
- Port 183 (from blur-nx): with logging off, stderr stays in memory (a crash report carries it) and nothing is
  written in logs/; with logging on, rex_stderr.log appears at its first write.
- Port 184 (from blur-nx): the forms self-check expects what the near-exact switches choose (#1, #17).
- Port 185 (from blur-nx): tools/run_pc.ps1 -Game and tools/switch_remote.py mv (refuses to replace a file).
- Port 186 (sasrt-nx's sync): the post-codegen passes find the generated files by the port's project name
  (<project>_recomp.N.cpp, _pch.h, _init.cpp), not renut_ only; near_exact.py stopped and fp_env_files.py missed
  the FPSCR writers in a port with another name.
- Port 187 (sasrt-nx's sync): port 144's builders in rex::codegen::extra_insn, so a port's own build_ldbrx (and
  the rest) still links.
- Port 188 (sasrt-nx's sync): A/B targets 256-511 are a port's own; the base's stay 1-255.
- Port 189 (from sasrt-nx): the game menu leaves out a settings row whose key the build does not define (a port
  without Katamari's frame_cap needs no edit there).
