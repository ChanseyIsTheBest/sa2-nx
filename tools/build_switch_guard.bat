@echo off
rem renut-ultimate (2026-11-11, port 78; sasrt-nx's idea 27, its build_switch.ps1 -GuardForms): a Switch NRO WITH the
rem localized copies' guard forms (SASRT_HOT_GUARD_FORMS ON), in a folder of its own. The everyday and release NROs
rem have none (the option's Switch default): there banjo_cpu_verify and banjo_hot_guard_* check no native copy (the
rem log says so). This one is for a guard or verify run on Ryujinx or the console, e.g.
rem   powershell -File tools\run_ryujinx.ps1 -Build switch-guard -Toml TOML
rem   with a TOML that sets banjo_hot_guard_calls = 4000 and banjo_hot_guard_period = 64 (the dense guard).
rem The everyday build's options otherwise (no LTO, no PGO). Output: app\out\switch-guard\renut.nro
setlocal
set "BUILD_DIR=%~dp0..\app\out\switch-guard"
rem renut-ultimate (2026-11-11, port 81): and the copies hooked (SASRT_HOT_IN_PLACE OFF): the guard runs in the hooks.
rem The copies' code is the one the everyday and release NROs have in place, so this build checks theirs too.
set "CMAKE_EXTRA=-DSASRT_HOT_GUARD_FORMS=ON -DSASRT_HOT_IN_PLACE=OFF %CMAKE_EXTRA%"
set "GUARD_FORMS=ON"
set "IN_PLACE=OFF"
call "%~dp0build_switch.bat" %*
