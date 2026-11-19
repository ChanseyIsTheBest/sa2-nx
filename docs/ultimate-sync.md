# sa2-nx and the ultimate base: where the branch stands

| | |
|---|---|
| The base | renut_nx_ultimate_base, branch `ultimate` (D:\renut-ultimate\base) |
| The base commit this branch is at | **73b7b7315be117d4c4a84f298dfa411e681c9b8a** (2026-11-19 10:15, "docs: port 189"; ports 1-189) |
| How it was taken | the base's whole tree as one commit (e2099f33, "The ultimate base at 73b7b731"), byte for byte, then Katamari's layer out and SA2's layer on top (docs/ultimate-rebase-plan.md) |

## Taking the base's later ports

The base's later ports come in with its sync tool (docs/building.md section 7), from the commit above:

```
python tools/ult_port_sync.py --base D:\renut-ultimate\base --port "D:\sonic adventure 2\sa2-nx-ult" --range 73b7b731..<base commit> --dry-run
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
