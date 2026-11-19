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
