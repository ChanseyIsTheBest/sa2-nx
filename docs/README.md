# The base's documentation

The pages in this folder: where to start, what each page is, and how they are kept. The [repository's README](../README.md)
is the summary of the base itself.

The base builds Beautiful Katamari as checked in, so the names and numbers in these pages are Katamari's unless a page
says otherwise. A new port's differ wherever they name the game: its manifest, its shader finder, its tomls.

## Where to start

| To | Read |
| --- | --- |
| Start a new port from the base | [porting-a-360-game-prompt.md](porting-a-360-game-prompt.md), the order of work and its rules as a prompt for a new session, then [building.md](building.md) |
| Build the base | [building.md](building.md): the game files, code generation, the shaders, the Switch builds, PGO, the installer, tests |
| See what the base changed from katamari-nx 1.0.1 | [changelog.md](changelog.md), a line per port, then the port's full row in [ultimate-fixes.md](ultimate-fixes.md) |
| Find out why a switch is off, or turn one on for a game | [off-by-default.md](off-by-default.md) |
| Check that the Switch build's one flush mode (CG-13, on) suits a game | `tools/cg13_census.ps1` on the PC build: [building.md](building.md) section 6, [off-by-default.md](off-by-default.md) section 2 |
| Fix a picture bug | A RenderDoc capture first: [porting-a-360-game-prompt.md](porting-a-360-game-prompt.md), appendix B, with Katamari's depth-of-field blur in [katamari-notes.md](katamari-notes.md) as the worked example |
| Run a build on the console | [porting-a-360-game-prompt.md](porting-a-360-game-prompt.md), appendix A (sys-botbase, sys-ftpd-light, `tools/switch_remote.py`); [building.md](building.md) section 6 on measuring |
| Take the base's later ports into a port | [building.md](building.md) section 7 (`tools/ult_port_sync.py`), then each port's row in [ultimate-fixes.md](ultimate-fixes.md) |
| Make the game's CPU side faster | [hand-and-machine-optimising-guide.md](hand-and-machine-optimising-guide.md) (sasrt-nx's, with the later tools), then [optimising-functions.md](optimising-functions.md) (the method in full) and [sasrt-batch-cc2.md](sasrt-batch-cc2.md) (register liveness, stack forwarding, the calling-convention traps) |
| Fix a crash the code generation caused | [codegen-crash-fixes.md](codegen-crash-fixes.md): the kinds of bug, and the scans that find more of each |
| Port a game built with a later XDK | [xdk-notes.md](xdk-notes.md) |
| See a whole port, step by step | [katamari-notes.md](katamari-notes.md) |

## Every page

Three kinds of page: a **guide** applies to any port and is kept current; a **reference** grows with every port; a
**record** is one port's notes as they were written, with that game's dates and numbers, and a note at its top says
what in it still applies.

| Page | Kind | What is in it |
| --- | --- | --- |
| [building.md](building.md) | Guide | The tools, the game files, code generation and the shader translator, the shaders, the Switch builds (LTO, PGO, the shader package), the installer, tests (PC verify, Ryujinx, the guard, the difftests, the console, RenderDoc), taking the base's later ports |
| [porting-a-360-game-prompt.md](porting-a-360-game-prompt.md) | Guide | A fill-in prompt for a new port's session: the placeholders, the rules, the order of work. Appendix A: the console over the network. Appendix B: RenderDoc |
| [ultimate-fixes.md](ultimate-fixes.md) | Reference | Every port by number: its source and commit, the evidence, what was run here, its switch. Also the test runs of 2026-11-07/08, the hooks that need a game's own function address, what sasrt-nx reported, what was left out and why, and what is not done yet |
| [changelog.md](changelog.md) | Reference | One line per port from port 44 on (ports 1-43 are the first round, in the README's "What's in it") |
| [off-by-default.md](off-by-default.md) | Reference | The switches that are off and why: fixes that break another game's picture, faster forms that are not exact, speed-ups the console measured as slower or as no gain, trials |
| [xdk-notes.md](xdk-notes.md) | Reference | What ports at a later XDK met: hooks, the device layout |
| [hand-and-machine-optimising-guide.md](hand-and-machine-optimising-guide.md) | Guide | sasrt-nx's guide (2026-11-11): measuring first, machine copies, hand natives, the order that paid off, a checklist for one function |
| [optimising-functions.md](optimising-functions.md) | Guide | The hot-function method written for Katamari: the machine track, the hand track, console profiles, checking, measuring, PGO |
| [sasrt-batch-cc2.md](sasrt-batch-cc2.md) | Record | sasrt-nx's handoff of port 121: whole-program register liveness for the machine copies, stack forwarding, the trap and calling-convention rules, its results |
| [katamari-notes.md](katamari-notes.md) | Record | katamari-nx's notes, 2026-10-30 to 11-05: code generation, Renut at Katamari's addresses, the shaders, the hot functions, Ryujinx, every console test, 1.0.0 and 1.0.1 |
| [codegen-crash-fixes.md](codegen-crash-fixes.md) | Record | Katamari's code-generation crashes (a function never found, a function table read as a jump table, a split function, a vcall thunk, a DLC level) and the scans after them |

## Elsewhere

| Where | What is in it |
| --- | --- |
| [app/README.md](../app/README.md) | The app's layout (the game layer, Renut's renderer, the shader translator, plume) and reNut's credits |
| [sdk/README.md](../sdk/README.md) | The ReXGlue SDK fork and its Horizon (Switch) layer |
| [tools/difftest/README.md](../tools/difftest/README.md) | The shader difftests: a pass checked bit for bit on random inputs |
| [tools/renut_patches/README.md](../tools/renut_patches/README.md) | banjo-nx's changes to Renut's files, as patches |
| [shaders/README.md](../shaders/README.md) | masseffect-nx's old shader tools, not used by Renut's build |
| [renut-nx's docs](https://github.com/ChanseyIsTheBest/renut-nx/tree/master/docs) | The method docs the base was built on: the renderer guide, the enhancements reference, deko3d, PGO and LTO |
| [katamari-nx](https://github.com/ChanseyIsTheBest/katamari-nx) | Katamari's console run breakdowns and progress log |

## Keeping these pages

- A port of a fix gets its row in [ultimate-fixes.md](ultimate-fixes.md), its line in [changelog.md](changelog.md), and,
  when its switch is off, a row in [off-by-default.md](off-by-default.md) (its "Keeping this page" has the rule).
- A guide changes with the code it describes, in the same commit.
- A record stays as its port wrote it. When it goes stale, say so in its top note; do not rewrite it.
- Dates are written out (2026-11-13), not "today" or "last week".
- No personal details: no names, e-mail addresses, home network addresses or user-profile folders. The console's
  address is `<CONSOLE_IP>` in the guide and `out/switch_host.txt` on your own PC (`out/` is not committed).
