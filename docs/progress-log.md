# Progress log: sa2-nx on the ultimate base

Dated entries, newest last. The plan is docs/ultimate-rebase-plan.md; the findings are in docs/sa2-ultimate-notes.md
and docs/sa2-near-exact.md.

## 2026-11-19

- Read the base's docs (README, building, changelog, off-by-default, the porting prompt, katamari-notes, xdk-notes,
  codegen-crash-fixes, the optimising guides, sasrt-batch-cc2) and sa2-nx's (sa2-notes, building, renut-renderer,
  cpu-rounds, renut-cpu-work-for-banjo).
- The file comparison and the inventory: docs/ultimate-rebase-plan.md (a86a7c54).
- The worktree `D:\sonic adventure 2\sa2-nx-ult`, branch `ultimate` from main (baf523b). The base's tree taken as its
  tree object (e2099f33): its blobs byte for byte; the worktree reads files as stored (`core.autocrlf false` in its
  own worktree config), since the base stores most text with CRLF and has no .gitattributes.
- Katamari's layer out (36bba673). `assets\game_root`: a junction to the main checkout's.
- The base's code generator built from this tree (`out\host`, `sdk\out\win-amd64\rexglue.exe`; `out\host_build.bat`,
  not committed). The base's `tools\build_host.bat` names banjo-nx's host folder, so it was not used.
- The base's tree has no FFmpeg configure-made files (FFmpeg's .gitignore hides them): taken from the base's working
  tree (cb3a6f56). The base's session took the same nine as port 190 (07647474), byte for byte.
- SA2's code generation (both modules, 31,285 + 160 functions, as 1.0.6), the base's post-codegen on the main module
  (about 50 minutes beside another session's build), the module steps on the DLL (tools/post_codegen_modules.py).
- Renut at SA2's Direct3D: the layout from 1.0.6 by name (tools/sa2_renut_layout.py), SA2's hook blocks, the Swap's
  pacing and gamma, no ResetAllState; SA2's game layer and app wiring; the settings check (tools/sa2_effective_keys.py:
  451 keys paired, 7 differ, none that draws or plays differently).
- The plan re-sorted into the three lists (the user's word through the base's session: a full rebase).
- SA2's lockstep recorder and the frame comparison tools.
