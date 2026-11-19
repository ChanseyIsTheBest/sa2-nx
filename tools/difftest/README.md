# Differential tests of GLSL passes (2026-10-20)

How the exact shader passes of tools/deko3d_glsl_opt.py are checked (docs/deko3d-shader-optimisations.md 2.37-2.40):
the shader before and after a pass, with the same specialization defines, compiled by uam, decompiled by Ryujinx's
shader translator, turned into Python and run on random inputs; the outputs (and discards) must be equal bit for bit.

- `shtool/`: decompiles a Maxwell program (SPH + code, as in a DKSH) to GLSL with Ryujinx's translator (the disassembly
  as comments). Build once, against the fork's Ryujinx build:
  `DOTNET_ROOT="D:\New folder\dotnet10" "D:/New folder/dotnet10/dotnet.exe" build tools/difftest/shtool/shtool.csproj -c Release -o tools/difftest/shtool/out`
  (the csproj references `D:/New folder/Ryubing-src/ryubing/build/Ryujinx.Graphics.Shader.dll` and friends).
- `difftest_ps.py`: the transpiler (shtool's GLSL -> Python) and a suite of small synthetic pixel shaders for uam flag
  A/B tests (`python difftest_ps.py <flags A> <flags B>`).
- `difftest_game.py`: whole game shaders. `python difftest_game.py <before.glsl> <after.glsl> <trials> [N=value ...]`.
  Texture fetches are a fixed function of the binding and the coordinates' bits (the same in both programs), sizes a
  fixed function of the binding, derivatives 0, the special functions Python's (the same in both), every instruction's
  result rounded to float32, constant buffer words a function of the trial's seed and the word.
  Pitfalls found on the way (both gave false mismatches): Python computes in doubles (round every assignment to
  float32), and constants drawn in read order differ between two programs that read them in another order (key them).
  Loops in the decompile (`do`) are not supported: test the specialised variants. Check a deliberately broken copy too
  (a wrong arm, a changed constant) so the test is known to catch differences.
  Options (renut-ultimate, ports 94 and 113, from vivapinata-nx): `--inputs LO:HI` (the inputs' range, -2:2 by default),
  `--cbmask C:I[-J]:comps:MASK` (constant words masked, e.g. a scale code kept at 0), `--cbset C:I:comp:VALUE` (a word
  pinned), `--texalias B=A` (binding B reads binding A's texels) and `--texcopy B:y=x` (a component copies another), for
  paths that assume two inputs equal. Each run works in a temp folder of its own, removed when it ends.
- `difftest_vs.py` (renut-ultimate, port 94, with vivapinata-nx): the same for vertex shaders. Every gl_Position,
  out_attrN and clip distance is compared, and so are local memory, storage-buffer stores (memory export) and every
  texture fetch's coordinates (as a multiset: the GPU compiler orders fetches its own way). An output counts as live
  when it took two or more finite values over the trials; a run that compared nothing live says NOTHING LIVE. A
  memory-export shader needs an export window and register to store anything (`--cbset 4:61:x:0 --cbset 4:61:y:0x7FFFFFFF
  --cbset 4:61:z:0 --cbset 2:0:x:0x00001000 --cbset 2:0:y:0x4B000000 --cbset 2:0:z:0x00002600 --cbset 2:0:w:0x4B7FFFFF`
  for Beautiful Katamari's); a large one with the window set can grow to 10 GB: run those one at a time.
  `python difftest_vs.py <before.glsl> <after.glsl> [trials] [N=value ...] [--cbmask ...] [--cbset ...]`.
- A rare mismatch (one trial in twenty, a value a texel or an ulp off) may be the GPU compiler fusing a multiply-add in
  one program and not the other: set `difftest_ps.FMA` to the unfused form and run again; if it goes, it was fusion,
  not the pass.
- `batch_gs.py`: guardsel on a list of shaders, each with up to two of its logged variants' constants
  (`python batch_gs.py <hash,hash,...> [trials]`; paths inside: the base GLSL folder and the emulator's variant log).
- `difftest_vs_skin.py` (2026-10-20): the skinonce pass (constant 913) on a fur vertex shader. The shader before the
  pass (A), after it in the write mode (W, 913 = 1, stores the rows) and the read mode (R, loads them), each compiled
  with flags 1471 and decompiled; A and W run on the same random bone words, R on W's stores; every output
  (gl_Position, varyings) must be equal bit for bit in all three, and W must store 12 words.
  `python difftest_vs_skin.py <before.glsl> <after.glsl> [trials] [N=value ...]` (the variant's 904 / 905). Integers are
  typed 32-bit from the decompile's declarations (bitfieldExtract signed for int temps). The read program's skin
  buffer is found by trying each storage buffer (a shader that fetches its vertex data from memory has the address
  window as well). Results: all four fur shaders 150 / 150; controls (a row read from the wrong slot, a stored row
  1 ulp off) caught.
- `uam_ops.py`: compiles one GLSL stage with uam and prints its instruction / GPR counts and Maxwell opcodes
  (`python uam_ops.py <glsl> frag|vert [N=value ...] [--seq]`).

Paths are this machine's (uam at out/deko3d_tools/uam-build, the GLSL under out/deko3d_shaders). Game data: the shaders
are the game's; nothing here is to be committed with them.

Limits found by other ports (2026-10-20): shaders with the txq pass (constant 908) read texture sizes from the size
buffer in the after-program and from textureSize() in the before-program; the harness gives the two different values
(KeyedCB words vs TSIZE), so test them with 908=0. A division whose operand is not a plain name is not wrapped in DIV;
a division by zero there ends the trial as 'zerodiv' (a mismatch only if the other program does not get there).
