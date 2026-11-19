# banjo-nx's changes to Renut's files

- `banjo.patch`: Renut's renderer (`src/renut_engine/gpu` of a reNut checkout), as imported into
  `app/src/renut_engine/gpu` by `tools/renut_gpu_import.py`, which applies it after its rewrite. Saved with
  `python tools/renut_gpu_import.py --renut <reNut> --save-patch`; checked with `--check-into <empty folder>`.
- `xenosrecomp.patch`: Renut's XenosRecomp (`thirdparty/reblue-XenosRecomp/XenosRecomp` of the checkout) against the
  copy vendored in `app/thirdparty/reblue-XenosRecomp` (constants through uniform buffers, select() for the predicate
  updates and kills, clamp bounds from SharedConstants, predicated blocks without a branch; package 27: moves as
  their operand, bools as specialization constants, bone rows in one load, clamped fetches in predicated blocks, literals
  from the constant bank, blend skip, the tessellation grid, early alpha's override and dump). For reference: the
  vendored copy is the one built. Regenerate with
  `git diff --no-index reNut/thirdparty/reblue-XenosRecomp/XenosRecomp banjo-nx/app/thirdparty/reblue-XenosRecomp/XenosRecomp`.

What each change is and why: renut-nx's [docs/renut-renderer-port.md](https://github.com/ChanseyIsTheBest/renut-nx/blob/master/docs/renut-renderer-port.md). These are banjo-nx's patches against reNut; the base's renderer has moved on since (docs/ultimate-fixes.md lists what changed).
