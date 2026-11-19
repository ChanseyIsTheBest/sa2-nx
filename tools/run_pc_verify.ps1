# katamari-nx: a PC verify run on Renut: every CPU enhancement checks itself on every use (banjo_cpu_verify), the guard on
# every call of every hook, the transform chain's own check on every call (docs/optimising-functions.md, "Checking"). For
# a new game, move the save out of out/pc_user first (it changes the menu path; run_pc.ps1's --user_data_root comes first
# and wins over a second one).
#   powershell -File tools/run_pc_verify.ps1 [-Seconds 300] [-Name verify1] [-Every 15] [-Script "20:START,24:A"] [-Sticks 240]
param([int]$Seconds = 300, [string]$Name = "verify1", [int]$Every = 15, [string]$Script = "", [int]$Sticks = 0)
$root = Split-Path -Parent $PSScriptRoot
# renut-ultimate (2026-11-11, port 78; sasrt-nx's idea 27): the guard runs the copies' logged forms (CMake
# SASRT_HOT_GUARD_FORMS, ON by default on the PC). A PC build configured without them would check no native copy.
$cache = Join-Path $root "app\out\pc\CMakeCache.txt"
if ((Test-Path $cache) -and (Select-String -Path $cache -Pattern '^SASRT_HOT_GUARD_FORMS:BOOL=(OFF|0|FALSE|NO|N)$' -Quiet)) {
    Write-Output ("app\out\pc has no guard forms (SASRT_HOT_GUARD_FORMS OFF): this run would check no native copy. " +
                  "Configure it with -DSASRT_HOT_GUARD_FORMS=ON (cmake -DSASRT_HOT_GUARD_FORMS=ON app\out\pc), then " +
                  "tools\build_pc.bat")
    exit 2
}
# renut-ultimate (2026-11-11, port 81; sasrt-nx's machine copies in place): with the copies in place (CMake
# SASRT_HOT_IN_PLACE, OFF by default on the PC) there is no hook, so no guard.
if ((Test-Path $cache) -and (Select-String -Path $cache -Pattern '^SASRT_HOT_IN_PLACE:BOOL=(ON|1|TRUE|YES|Y)$' -Quiet)) {
    Write-Output ("app\out\pc has the copies in place (SASRT_HOT_IN_PLACE ON): no hook, so this run would check no " +
                  "native copy. Configure it with -DSASRT_HOT_IN_PLACE=OFF (cmake -DSASRT_HOT_IN_PLACE=OFF app\out\pc), " +
                  "then tools\build_pc.bat")
    exit 2
}
$extra = "--banjo_cpu_verify=true --banjo_hot_guard_calls=4000 --banjo_hot_guard_period=1 " +
         "--katamari_hand_transform_chain_verify=true --katamari_hand_sphere_box=true"
# renut-ultimate (port 70, 2026-11-10): an empty --input_script= stopped the run at the command line ("1 required TEXT
# missing"), so the documented command without -Script never started.
if ($Script) { $extra += " --input_script=$Script" }
if ($Sticks -gt 0) { $extra += " --input_script_sticks=$Sticks" }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\run_pc.ps1") -Seconds $Seconds -Name $Name -Every $Every -Visible -Extra $extra
