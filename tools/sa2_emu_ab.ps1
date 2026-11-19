# sa2-nx-ult (2026-11-19): one emulator run of the rebased NRO or of sa2-nx 1.0.6's released NRO in the sa2-ult data
# folder, for the A/B of the picture (tools/sa2_compare_frames.py --align). Both NROs take /switch/sa2-nx/ as their
# folder (Ryujinx starts an NRO without argv: each falls back to /switch/sa2-nx/sa2.nro), so each run gets its own
# files there and puts them back after:
#   - the NRO (sa2.nro): app\out\switch\renut.nro ("ult"), app\out\switch-<variant>\renut.nro ("ult-<variant>": a
#     build of this tree in a folder of its own, e.g. ult-exact) or app\out\ref106\renut.nro ("ref106": 1.0.6,
#     bba67b6a);
#   - deko3d_glsl.bin: none for "ult" (its GLSL package is built in; one beside it would be read first), 1.0.6's
#     release package for "ref106" (sa2-nx app\out\renut_switch_release\sd\deko3d_glsl.bin, what its installer makes);
#   - deko3d_shaders.bin (the compiled variants, written as it plays) and the user folder (sa2\): each build's own,
#     kept beside as <name>.<build> between runs;
#   - sa2.toml: the -Toml given (test keys above the first table), the SD's own put back after.
# Refuses to start while any Ryujinx runs, with under -MinCommitGB of free commit memory, or while a build compiles;
# stops only the process it started, when the recording's .txt appears (or after -Seconds). The recording
# (captures\video_1s.*) and the logs go to out\emu_runs\<Name>.
#   powershell -File tools\sa2_emu_ab.ps1 -Build ult|ult-<variant>|ref106 -Name <run> -Toml out\emu_tomls\<toml> [-Seconds 900]
param([Parameter(Mandatory = $true)][ValidatePattern("^(ult|ult-[a-z0-9]+|ref106)$")][string]$Build,
      [string]$Name = "emu_ab",
      [Parameter(Mandatory = $true)][string]$Toml, [int]$Seconds = 900, [double]$MinCommitGB = 12,
      [string]$Data = "D:\New folder\ryu-data\sa2-ult")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$ryu = "D:\New folder\Ryubing-src\ryubing\build\Ryujinx.exe"
$sd = Join-Path $Data "sdcard\switch\sa2-nx"
$out = Join-Path $root "out\emu_runs\$Name"
if (Get-Process -Name Ryujinx -ErrorAction SilentlyContinue) { Write-Output "a Ryujinx is running: not starting"; exit 2 }
# This port's builds only (other sessions build their own trees all day).
$building = Get-CimInstance Win32_Process -Filter "Name='ninja.exe' or Name='cmake.exe'" |
    Where-Object { $_.CommandLine -like "*sa2-nx-ult*" }
if ($building) { Write-Output "a build of this tree is running: not starting"; exit 2 }
$os = Get-CimInstance Win32_OperatingSystem
$freeCommit = $os.FreeVirtualMemory / 1MB
if ($freeCommit -lt $MinCommitGB) { Write-Output ("free commit memory {0:N1} GB < {1} GB: not starting" -f $freeCommit, $MinCommitGB); exit 3 }
$nroSource = if ($Build -eq "ult") { Join-Path $root "app\out\switch\renut.nro" }
              elseif ($Build -like "ult-*") { Join-Path $root ("app\out\switch-" + $Build.Substring(4) + "\renut.nro") }
              else { Join-Path $root "app\out\ref106\renut.nro" }
if (-not (Test-Path $nroSource)) { Write-Output "no NRO at $nroSource"; exit 4 }
if (-not (Test-Path $Toml)) { Write-Output "no toml at $Toml"; exit 4 }
$glslRef = "D:\sonic adventure 2\sa2-nx\app\out\renut_switch_release\sd\deko3d_glsl.bin"
New-Item -ItemType Directory -Force $out | Out-Null

# This build's files in place.
Copy-Item $nroSource (Join-Path $sd "sa2.nro") -Force
$glsl = Join-Path $sd "deko3d_glsl.bin"
if (Test-Path $glsl) { Remove-Item $glsl -Force }  # a copy of a reference file (the release package), never the only one
if ($Build -eq "ref106") { Copy-Item $glslRef $glsl -Force }
foreach ($item in @("deko3d_shaders.bin", "sa2")) {
    $live = Join-Path $sd $item
    $mine = Join-Path $sd "$item.$Build"
    if (Test-Path $live) { Write-Output "$live is there from a run that did not finish: moved to $item.unknown"; Move-Item $live (Join-Path $sd "$item.unknown") -Force }
    if (Test-Path $mine) { Move-Item $mine $live }
}
$sdToml = Join-Path $sd "sa2.toml"
$sdTomlSaved = if (Test-Path $sdToml) { [System.IO.File]::ReadAllBytes($sdToml) } else { $null }
Copy-Item $Toml $sdToml -Force
$captures = Join-Path $sd "captures"
$logs = Join-Path $sd "logs"
foreach ($d in @($captures, $logs)) {
    if (Test-Path $d) { Move-Item $d (Join-Path $out ("before_" + (Split-Path $d -Leaf))) -Force }
}

$env:DOTNET_ROOT = "D:\New folder\dotnet10"
$started = Get-Date
$p = Start-Process -FilePath $ryu -ArgumentList @("-r", "`"$Data`"", "`"$(Join-Path $sd 'sa2.nro')`"") -PassThru `
     -WindowStyle Minimized -RedirectStandardOutput (Join-Path $out "ryujinx_stdout.txt") `
     -RedirectStandardError (Join-Path $out "ryujinx_stderr.txt")
Write-Output ("started Ryujinx pid {0} ({1}, free commit {2:N1} GB)" -f $p.Id, $Build, $freeCommit)
$txt = Join-Path $captures "video_1s.txt"
$deadline = $started.AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $txt) -and (Get-Item $txt).LastWriteTime -gt $started) { Start-Sleep -Seconds 3; break }
    Start-Sleep -Seconds 5
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(30000) | Out-Null; Write-Output ("stopped pid {0} after {1:N0} s" -f $p.Id, ((Get-Date) - $started).TotalSeconds) }
else { Write-Output "Ryujinx exited by itself (code $($p.ExitCode))" }
Start-Sleep -Seconds 2

# The run's results out, this build's files back beside, the SD's toml back.
if (Test-Path $captures) { Move-Item $captures (Join-Path $out "captures") -Force }
if (Test-Path $logs) { Move-Item $logs (Join-Path $out "logs") -Force }
foreach ($item in @("deko3d_shaders.bin", "sa2")) {
    $live = Join-Path $sd $item
    if (Test-Path $live) { Move-Item $live (Join-Path $sd "$item.$Build") -Force }
}
if (Test-Path $glsl) { Remove-Item $glsl -Force }
if ($sdTomlSaved) { [System.IO.File]::WriteAllBytes($sdToml, $sdTomlSaved) } else { Remove-Item $sdToml -ErrorAction SilentlyContinue }
Get-ChildItem $out -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring($out.Length + 1) } }, Length
