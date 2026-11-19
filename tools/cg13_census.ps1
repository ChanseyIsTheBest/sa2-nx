# renut-ultimate (2026-11-13, port 133): CG-13's census in one run. The Switch build runs all guest float code in the
# host's flush mode (BLUR_FPCR_ONE_MODE, ON there since port 133), which the Xenon's scalar FPU does not: a scalar value
# below FLT_MIN / DBL_MIN becomes zero. The PC build keeps the mode switching (the option is OFF there) and, with
# banjo_diag_runtime_census, counts every stretch of game code between a switch off and the next switch on that met a
# denormal operand or gave an underflow: the scalar results the Switch build changes. This runs app/out/pc/renut.exe
# with the census on, adds up its "[census] CG-13" lines and, when any window counted one, lists the sites by function
# and generated source line (tools/census_sites.py --denormal). Play the parts of the game that matter: gameplay, not
# only menus (-Script / -Extra take the game's input script, or -Visible to play it yourself).
#
#   powershell -ExecutionPolicy Bypass -File tools\cg13_census.ps1 [-Seconds 300] [-Name cg13_census]
#       [-Script "20:START,24:A"] [-Sticks 240] [-Extra "--more=keys"] [-Visible] [-Top 40]
#   ... -Log out\pc_runs\<run>\<run>.log   (no run: the verdict of an earlier census run's log)
param([int]$Seconds = 180, [string]$Name = "cg13_census", [string]$Script = "20:START,24:A", [int]$Sticks = 240,
      [string]$Extra = "", [switch]$Visible, [int]$Top = 40, [string]$Log = "")
$root = Split-Path -Parent $PSScriptRoot
if ($Log -ne "") {
  $log = $Log
} else {
  $cache = Join-Path $root "app\out\pc\CMakeCache.txt"
  if (-not (Test-Path $cache)) { Write-Error "no PC build (app\out\pc): tools\build_pc.bat first"; exit 1 }
  if (Select-String -Path $cache -Pattern '^BLUR_FPCR_ONE_MODE:BOOL=ON$' -Quiet) {
    Write-Error ("the PC build has BLUR_FPCR_ONE_MODE ON: the mode never switches, so the census sees nothing. " +
                 "Configure it OFF (cmake -DBLUR_FPCR_ONE_MODE=OFF app\out\pc), build, and run this again")
    exit 1
  }
  $census = "--banjo_diag_runtime_census=true"
  if ($Script -ne "") { $census += " --input_script=$Script --input_script_sticks=$Sticks" }
  if ($Extra -ne "") { $census += " $Extra" }
  $run = @{ Seconds = $Seconds; Name = $Name; Extra = $census }
  if ($Visible) { $run.Visible = $true }
  & (Join-Path $PSScriptRoot "run_pc.ps1") @run
  $log = Join-Path $root "out\pc_runs\$Name\$Name.log"
}
if (-not (Test-Path $log)) { Write-Error "no log at $log"; exit 1 }
$log = (Resolve-Path $log).Path
$cg13 = [regex]'\[census\] CG-13 \(one flush mode\): per second (\d+) any-mode cold paths and (\d+) of (\d+) FPU-mode regions'
$switches = [regex]'\[census\] guest code per second: (\d+) flush-mode switches'
$windows = 0; $hit = 0; $maxRegions = 0; $maxCold = 0; $regions = 0.0; $sw = 0.0; $swn = 0
foreach ($line in [System.IO.File]::ReadLines($log)) {
  $m = $cg13.Match($line)
  if ($m.Success) {
    $windows++
    $cold = [int64]$m.Groups[1].Value; $den = [int64]$m.Groups[2].Value
    $regions += [double]$m.Groups[3].Value
    if ($cold -gt 0 -or $den -gt 0) { $hit++ }
    if ($den -gt $maxRegions) { $maxRegions = $den }
    if ($cold -gt $maxCold) { $maxCold = $cold }
    continue
  }
  $m = $switches.Match($line)
  if ($m.Success) { $sw += [double]$m.Groups[1].Value; $swn++ }
}
if ($windows -eq 0) {
  Write-Output "No '[census] CG-13' line in $log (a run shorter than 10 s, or a build without the census)."
  exit 1
}
$avgRegions = [math]::Round($regions / $windows)
$avgSwitches = if ($swn -gt 0) { [math]::Round($sw / $swn) } else { 0 }
$runName = [System.IO.Path]::GetFileNameWithoutExtension($log)
Write-Output (("CG-13 census, {0}: {1} windows of 10 s; about {2} flush-mode switches a second in the game code (each " +
               "an msr fpcr on the console, which the Switch build leaves out) and {3} FPU-mode regions a second.") -f
              $runName, $windows, $avgSwitches, $avgRegions)
if ($hit -eq 0) {
  Write-Output ("Verdict: 0 regions with a denormal and 0 any-mode cold paths in every window: the one flush mode " +
                "changes no scalar result in what this run played. Keep BLUR_FPCR_ONE_MODE on for the Switch build.")
  exit 0
}
Write-Output (("Verdict: {0} of {1} windows met denormals (at most {2} regions and {3} any-mode cold paths a second). The " +
              "sites follow; banjo::FormsSelfCheck's are its own deliberate ones at start-up. Look at what each site " +
              "computes: a value that decays to zero anyway is harmless; a division, a normalisation, a guard against " +
              "zero or a replay's simulation is not. If one matters, build the Switch folder with " +
              "-DBLUR_FPCR_ONE_MODE=OFF (docs/building.md) and retrain the PGO profile.") -f
              $hit, $windows, $maxRegions, $maxCold)
& python -B (Join-Path $PSScriptRoot "census_sites.py") $log --denormal --top $Top
exit 2
