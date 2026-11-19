# app

The game layer and the renderer of the ultimate base. As checked in it builds Beautiful Katamari; a new port swaps the
game layer (see the repository's README, "Starting a port from it", and
[docs/porting-a-360-game-prompt.md](../docs/porting-a-360-game-prompt.md)).

| Path | What it is |
| --- | --- |
| `renut_manifest.toml` | The code generator's config for the game |
| `src/renut_engine/gpu` | Renut, reNut's Direct3D-level renderer, on plume's Vulkan (PC) and deko3d (Switch) back ends |
| `src/cpu` | The machine copies' hooks and guard, the hand-written natives, the generated-code self-checks |
| `src/katamari_*`, `katamari_switch*.toml` | Beautiful Katamari's own keys, fixes and settings (the parts a new port replaces) |
| `thirdparty/reblue-XenosRecomp` | The Xenos shader translator (Renut's fork, with this base's changes) |
| `thirdparty/plume` | plume, the render back ends |
| `generated/` | The code generated from your own copy of the game (never committed) |

## reNut

Renut started as [reNut](https://github.com/masterspike52/reNut), the Banjo-Kazooie: Nuts & Bolts recompilation by
masterspike52, made with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk); its renderer is reused here with its
author's permission. reNut's own credits:

- the [ReXGlue team](https://github.com/rexglue/rexglue-sdk), for the ReXGlue SDK, and the ReXGlue SDK Discord, for the
  information they shared;
- SolarCookies, for the mid-asm hooks, the CRT functions and the reNut Launcher;
- ValcomDrifty, for the reNut logo;
- OlieGamerTV, for the information and documentation on Nuts & Bolts.

reNut's questions go to its Discord: https://discord.gg/D5Bz2ZsHdY. For playing Nuts & Bolts itself (its PC
releases, launchers and settings), see reNut's repository.
