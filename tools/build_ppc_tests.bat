@echo off
rem renut-ultimate (2026-11-16, port 147; tw2de_nx's build_ppc_tests.bat of 2026-11-02): the PowerPC instruction
rem tests as a Switch homebrew, out\ppc_tests\renut_ppc_tests.nro (tools\ppc_tests\build_ppc_tests.py: Xenia's tests,
rem fetched with git from xenia-canary at a fixed commit, and the base's own, tools\ppc_tests\own). The code generator
rem is the one tools\build_pc.bat builds (sdk\out\win-amd64\rexglue.exe), so the tests check the generator you have
rem now, with app\renut_manifest.toml's codegen keys and exact_fp on. Tools: DEVKITPRO set, git, cmake, ninja, python
rem and a C compiler (clang, gcc or cc) on PATH (tools\pc_env.bat). Start the NRO from the homebrew menu (or an
rem emulator); the failures are in renut_ppc_tests.txt next to it.
rem   tools\build_ppc_tests.bat [--jobs 8] [--xenia <a checkout's src\xenia\cpu\ppc\testing>]
setlocal
call "%~dp0pc_env.bat" || exit /b 1
python "%~dp0ppc_tests\build_ppc_tests.py" %* || (echo build_ppc_tests failed: the reason is above. & exit /b 1)
echo build_ppc_tests finished: out\ppc_tests\renut_ppc_tests.nro
