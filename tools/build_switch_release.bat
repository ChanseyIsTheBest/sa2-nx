@echo off
rem (port 81: setlocal, so GUARD_FORMS and IN_PLACE set here do not stay in the console for the next build)
setlocal
rem katamari-nx: the released NRO, in a build folder of its own (docs/building.md): LTO, the PGO profile in pgo/, no game
rem shader inside (it reads deko3d_glsl.bin beside itself). tools\renut_release_shaders.py makes the files it builds with.
rem Output: app\out\switch-pgo-use\renut.nro
set "BUILD_DIR=%~dp0..\app\out\switch-pgo-use"
set "CMAKE_EXTRA=-DBANJO_PGO=use -DBANJO_SWITCH_LTO=ON -DBANJO_RENUT_RELEASE_SHADERS=ON"
rem renut-ultimate (2026-11-11, port 78; sasrt-nx's idea 27, its -Final): the released NRO has no guard forms
rem (SASRT_HOT_GUARD_FORMS OFF, held so in the folder's cache), unless GUARD_FORMS=ON is set (sasrt-nx's -GuardForms).
if "%GUARD_FORMS%"=="" set "GUARD_FORMS=OFF"
rem renut-ultimate (2026-11-11, port 81; sasrt-nx's machine copies in place): the released NRO has the copies in place
rem (SASRT_HOT_IN_PLACE ON, held so) and reads that form's profile, pgo-inplace\ (CMake BANJO_PGO_DIR_IN_PLACE; the
rem configure stops while it has none). IN_PLACE=OFF, or GUARD_FORMS=ON, builds the hooked form with pgo\.
if "%IN_PLACE%"=="" if /i not "%GUARD_FORMS%"=="ON" set "IN_PLACE=ON"
if "%JOBS%"=="" set "JOBS=10"
call "%~dp0build_switch.bat" %*
