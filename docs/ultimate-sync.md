# sa2-nx and the ultimate base: where the branch stands

| | |
|---|---|
| The base | renut_nx_ultimate_base, branch `ultimate` (D:\renut-ultimate\base) |
| The base commit this branch is at | **2f590c31ef155f50862b3de225611292e81690c9** (2026-11-20 06:24, "docs: port 218's follow-up"; ports 1-222); first taken at 73b7b7315be117d4c4a84f298dfa411e681c9b8a (2026-11-19 10:15, "docs: port 189"; ports 1-189) |
| How it was taken | the base's whole tree as one commit (e2099f33, "The ultimate base at 73b7b731"), byte for byte, then Katamari's layer out and SA2's layer on top (docs/ultimate-rebase-plan.md) |

## Taking the base's later ports

The base's later ports come in with its sync tool (docs/building.md section 7), from the commit above:

```
python tools/ult_port_sync.py --base D:\renut-ultimate\base --port "D:\sonic adventure 2\sa2-nx-ult" --range 2f590c31..<base commit> --dry-run
```

then without `--dry-run`, the conflicts by hand, a build and the tests. After a sync, change the commit in the table
above to the last base commit taken.

What the sync must leave to a hand port in this tree (SA2 keeps its own form of these files):

- Renut's device layout and hook bindings at SA2's addresses (`app/src/renut_engine/gpu/d3d/d3d.h`, `d3d_native.h`,
  `d3d/hooks/*.cpp`): a base change there names Katamari's offsets and addresses.
- `app/CMakeLists.txt`: SA2's sources, the stage DLL's target and its static link on the Switch, the NRO's name.
- The tomls and SA2's keys (`app/sa2_*.toml`, `app/src/sa2_keys.cpp`).

## History

| Date | Base commit | What came in |
|---|---|---|
| 2026-11-19 | 73b7b731 | the first rebase: ports 1-189 |
| 2026-11-20 | 2f590c31 | ports 191-222 with `tools/ult_port_sync.py --range 73b7b731..2f590c31 --skip 190` (port 190 is sa2-nx's own FFmpeg files, already here): 73 files merged cleanly, 4 new, 1 conflict (`app/CMakeLists.txt`: SA2's sources and name with port 193's build stamp, `RENUT_GAME_NAME` "Sonic Adventure 2"), the hand port of `d3d/hooks/shader.cpp` (ports 196, 207: three-way merged into SA2's form without a conflict; the GetFunction hooks stay unbound, their key off, and SA2's containers are 0x11) |

Ports 191-222 and SA2 (2026-11-20):

- **The code generation:** ports 199 (stack forwarding ends at every branch and call) and 200 (`inline_memcalls.py`)
  change two post-codegen passes, so the tree is generated again in full. Port 191's `pointer_fixpoint.py` fix is in a
  porting audit tool, not the build.
- **Keys that reach SA2:** port 202 turns `sasrt_renut_depth_restore` off (no "EDRAM depth restore" line in any of
  SA2's 110 logs); port 211's `banjo_renut_callbacks_at_waits` (on) runs Renut's GPU callbacks at game threads' waits
  too; port 217's `xam_input_clear_disconnected` (on, as the console); port 220 signs in a pad that connects after the
  game opened its sign-in screen as a guest (SA2 imports `XamShowSigninUI`; no log shows it opened: a console check in
  the two-player menus); port 203's shared save descriptors (`filesystem_shared_handles`, on) on the Switch; port 195's
  message boxes at twice the size; port 206's shortcuts on release.
- **A port-213 fix:** its new device field (the device's own vertex declaration, which SetFVF rewrites in place) came
  with Katamari's offset 0x2F98 in `draw.h`; its static_assert stopped the build: SA2's 0x3118 (4ea0837b).
- **The code generated again** (the generator's record moved aside, `app/renut_config.toml` touched): 24 of 726 files
  changed. Port 199: 46 stack forwards in 35 functions before, 43 in 26 after (the forwards past a call gone, a few new
  after a branch). Port 200: nothing in SA2. Port 191's `tools/built_ptr_scan.py`: 0 candidates in both modules.
- **The PC checks (2026-11-20):** 1.0.6's 750 attract frames all identical (`out/lockstep/sync_attract1`, game frames
  5800-9400 hash-only, aligned by content), no DIFFERENCE or error line, 1.09 million stack forwards compared with the
  real load: 0 differences. A verify run (5 minutes, every self-check on every use): 0 differences in every check (1.16
  billion flush-mode checks, 125 million folded loads, 109 million r1 checks, 7.5 million stack forwards, 61.8 million
  guarded calls of the machine copies with 5 "differed once and not when repeated", as every run before the sync: 4-10).
  Its scripted presses were missed while the save loaded (the guarded run is slow; known), so it showed the title and
  the intro; a plain run on the same route played City Escape (`out/pc_runs/sync_route`, all ten presses taken).
- **The Switch shader package** made again with the merged translator (ports 194, 207, 209): all four files
  byte-identical to the package before.
