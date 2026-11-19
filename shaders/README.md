# shaders (masseffect-nx's shader tools, not part of Renut's build)

This folder came with the structure the base grew from (banjo-nx, which took it from masseffect-nx's Mass Effect port):
its own XenosRecomp copy, the tools that made masseffect-nx's SPIR-V shader package (`masseffect_shaders.mesp`), and
their WebAssembly recipes. **Renut's renderer does not use it.** Nothing in the base's build compiles it; only
`tools/wrap_discovered.py` still calls one of its scripts (`tools/wrap_raw_shader.py`).

What the base uses instead:

- the shader translator: `app/thirdparty/reblue-XenosRecomp` (built by the PC build);
- the Switch shader package: `tools/switch_shaders.ps1` (GLSL from the translator's SPIR-V, the passes of
  `tools/deko3d_glsl_opt.py`, `app/out/renut_switch`), and `tools/deko3d_variant_cache.py` for the DKSH cache;
- the release's package, made in the player's browser: the web installer,
  [renut_nx_ultimate_installer](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_installer).

[docs/building.md](../docs/building.md) has the steps. `README.upstream.md` is XenosRecomp's own README, and
`LICENSE.md` its license.
