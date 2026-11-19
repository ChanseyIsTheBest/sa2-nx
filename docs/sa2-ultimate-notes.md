# Sonic Adventure 2 on the ultimate base: notes

What the rebase of sa2-nx (1.0.6) on the ultimate base found and decided, with its evidence. The plan and the
inventory are in [ultimate-rebase-plan.md](ultimate-rebase-plan.md); the not-exact forms in
[sa2-near-exact.md](sa2-near-exact.md); dated entries in [progress-log.md](progress-log.md). SA2's own records from
1.0.0-1.0.6 (sa2-notes.md, renut-renderer.md, cpu-rounds.md, ...) are in this folder as they were written.

## Settings: a 1.0.6 player's sa2.toml

The player's file is 1.0.6's (`app/sa2_switch.toml`, written next to the NRO when it is missing), so a player who has
one keeps it. Each key of it, and what the rebased build does with it:

| 1.0.6 key | The base's key | What the rebased build does with the line |
|---|---|---|
| `language` (auto, english, japanese, french, german, spanish, italian) | the SDK's `language` (blur-nx's: System, English, ... by name, case-insensitive) | reads it: the same names work; "auto" is not one of them, so it falls to "System" (the console's language), what 1.0.6's "auto" did. A console language the game lacks gives the game's own fallback (English), as 1.0.6's did |
| `sa2_renut_msaa` (0, 2, 4, 8) | none: the base's `msaa_level` sets the samples of the game's own MSAA targets, another thing | reads it; 0 (the default) is what this build draws; any other value is ignored with a warning line in the log (`src/sa2_keys.cpp`). A decision for the user: port 1.0.6's MSAA of single-sample targets to the base, or leave it out |
| `sa2_vsync_relaxed` | SA2's own (`gpu/sa2_renut_pacing.cpp`) | reads it, as 1.0.6 (active when the base's non-CPU master is on, as 1.0.6's with its master) |
| `sa2_renut_double_buffer` | `pacman_renut_double_buffer` (pacman-nx's form of sa2-nx's key) | reads it: true sets the base's key (`src/sa2_keys.cpp`), unless a toml sets the base's key itself |
| `input_xbox_layout`, `input_joycon_sideways` | the same keys (the base's input system) | reads them |
| `sa2_analog_fix`, `sa2_deadzone`, `sa2_action_buttons`, `sa2_camera_invert`, `sa2_hold_toggle` | SA2's own (`src/sa2_controls.cpp`, 1.0.5's OpenSA2 options) | reads them, as 1.0.6 |
| `[input_p1]`, `[input_p2]` (a = "B, ZR", ...) | the same tables (the SDK's input maps, `input_pN_*`) | reads them |

The compiled-in settings (`app/sa2_switch_defaults.toml`) carry 1.0.6's decisions under the base's names.
`tools/sa2_effective_keys.py` compares the two builds' settings key by key (each key's default as written in its
REXCVAR_DEFINE, then the compiled-in and player tomls). On 2026-11-19: 451 keys paired by name or by sa2-nx's renaming,
7 differ, none that changes the picture or the game: `log_file_writer` (false: 1.0.6's `log_to_file = false`), the
guard's call count (no guard in the Switch NRO), `banjo_renut_inline_line_compare` (the base's form: off since the mask
compare replaced it), the MSAA keys' two scales (both off), the test video's frame count, and two keys of the old PM4
renderer. The 1.0.6 keys with no pair are the left-behind renderers' (`sa2_native_*`, `sa2_api_*`, `sa2_cpu_ring_*`),
1.0.6's own copies' keys (`sa2_cpu_native_*`: the base's copies are remade in the hot-function step) and its read-ahead
(`sa2_io_*`); an old toml's lines for them load and do nothing.

Where SA2's compiled-in settings differ from the base's defaults, and why:

| Key | SA2 | The base | Why |
|---|---|---|---|
| `banjo_renderer`, `renut_gpu_backend`, `license_mask` | renut, deko3d, 1 | native, d3d12, 0 | the game's renderer and the full game, as 1.0.6 |
| `renut_post_process_precision` | Game | High | 1.0.6's: the game's 8-bit post buffers (the base's High changes the picture) |
| `banjo_renut_render_height_docked`, `banjo_renut_display_height_docked` | 1080 | 720 | 1.0.6 (1.0.3.1): 1920x1080 docked |
| `banjo_renut_game_gamma` | true | false | SA2's display gamma ramp at scan-out (1.0.6's `sa2_renut_gamma_ramp`); the ramp at SA2's +15408 |
| `msaa_level` | 4 | 1 | the game's own sample counts (the base's key only lowers a count). SA2's render targets are single-sample; its only 4x surfaces are Direct3D's fast-clear aliases, which 1.0.6 drew at 4x. The base's 1 would draw them at 1x: a config for the frame comparison, not the default |
| `banjo_renut_smaa` | false | true | 1.0.6 drew no anti-aliasing; SMAA would do nothing until SA2's composite shader is listed (`banjo_renut_smaa_after_ps`): the user's picture decision |
| `banjo_renut_core_layout`, `banjo_renut_deferred_chain`, `banjo_display_sync`, `banjo_deko3d_eager_kicks` | true | false | on in 1.0.6 (each measured or run on SA2's console); off in the base for other games' reasons (off-by-default.md section 4) |
| `banjo_deko3d_alpha_spec` | false | true | off in 1.0.6: in Ryujinx every textured SA2 surface came out one flat colour with it (renut-renderer.md); to check on the console first |
| `switch_thread_stack_pool` | 64 | 4 | 1.0.6's fix for the heap's growth at stage loads (renut-renderer.md, "The heap in long sessions") |
| `banjo_renut_tags` | false | true | the base's tags need marks from its native state setters, which SA2 does not use (its own Direct3D sets the state); without them a delta could miss a changed chunk. Off, the deltas compare their chunks (exact). SA2's own marks (1.0.4's `sa2_renut_setter_marks`) are not in the base's form yet |
| `input_rumble` | false | true | 1.0.6 had no rumble; the base's (port 59) is the game's own vibration: a decision for the user |

## Two code modules

SA2 has two: `default.xex` and the stage DLL `gd/DLL/xbox 360/Data_DLL.xex` (160 functions; 31 MB, nearly all stage
data), a `[[modules]]` entry of the manifest. The base's generator writes the main module into `app/generated` (flat:
the base's passes and CMake expect that) and the DLL into `app/generated/data_dll` with its own `renut_*` files and
target (`renut_Data_DLL`, a DLL on the PC, partially linked into the NRO on the Switch). The first generation: 31,285
functions in the main module and 160 in the DLL, as 1.0.6's.

The passes: the base's whole `tools/post_codegen.py` on the main module; on each module folder
`tools/post_codegen_modules.py` (new, called at the end of post_codegen.py), the steps a file needs to be right or that
its pch's macros assume (pch_no_volatile with loop_barriers, cold traps, direct calls, guest_cas, guest_fences, the
flush-mode flow without the image, vec_byteswap, the NEON shuffles, float_loads, frame_traffic's marks, loop and leaf
locals, the store cards). Not the whole-program analyses (leaf_dead_stores, dead_ctx_defs, reg_liveness, single_floats,
ro_constants, the lr / r1 proofs, inline_leaves) nor the copies: they read one module's call graph and one image, and
the modules reach each other only through pointers (sa2-nx's audit: the DLL's data holds no pointer into default.xex's
code, and no indirect call of the game leaves the main image). This is 1.0.6's split.
