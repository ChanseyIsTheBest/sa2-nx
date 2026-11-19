# sa2-nx-ult (2026-11-20): the Switch package of the rebased build (the rebase prompt's step 9), in this port's own folder:
#   out\packages\<date>_<time>_<name>\
#     COPY_TO_SD_switch_sa2-nx\        sa2.nro (the NRO this build made), sa2.toml (the player's file, app\sa2_switch.toml:
#                                      1.0.6's; a player who has one keeps theirs), configs\ (sa2_play.toml: the same file;
#                                      sa2_ab.toml: with logs and the CPU sampler on), deko3d_shaders.bin when one was made
#                                      (the prebuilt DKSH cache, tools\deko3d_variant_cache.py), and deko3d_glsl.bin only
#                                      for a release NRO (-GlslPackage: the everyday NRO has its GLSL package built in)
#     sa2-nx.elf                       the unstripped ELF of this NRO (console profiles map to it)
#     source_<commit>.tgz              the source snapshot (git archive of HEAD: no game data, no generated code)
#     PACKAGE_NOTES.txt                -Notes, when given
# No game data goes in: the card keeps 1.0.6's game_root.nxpack / .nxidx and dlc\ (the rebased NRO reads them).
#   powershell -File tools\sa2_package_switch.ps1 -Name <name> [-Build app\out\switch] [-Notes <file>] [-GlslPackage <bin>]
param([string]$Name = "package", [string]$Build = "app\out\switch", [string]$Notes = "", [string]$GlslPackage = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$free = (Get-PSDrive D).Free / 1GB
if ($free -lt 2) { throw ("D: has {0:N1} GB free: not packaging (the prompt: stop and say so)" -f $free) }
$b = Join-Path $root $Build
$nro = Join-Path $b "renut.nro"
$elf = Join-Path $b "renut"
if (-not (Test-Path $nro)) { throw "no NRO at $nro" }
$stamp = Get-Date -Format "yyyy-MM-dd_HHmm"
$out = Join-Path $root "out\packages\${stamp}_$Name"
$sd = Join-Path $out "COPY_TO_SD_switch_sa2-nx"
New-Item -ItemType Directory -Force $sd, (Join-Path $sd "configs") | Out-Null
Copy-Item $nro (Join-Path $sd "sa2.nro")
$player = Join-Path $root "app\sa2_switch.toml"
Copy-Item $player (Join-Path $sd "sa2.toml")
Copy-Item $player (Join-Path $sd "configs\sa2_play.toml")
Copy-Item (Join-Path $root "dist\switch\sa2-nx\sa2_ab_keys.toml") (Join-Path $sd "configs\sa2_ab.toml")
# sa2_ab.toml: the test keys (above the first table), then the player's file.
Add-Content -Path (Join-Path $sd "configs\sa2_ab.toml") -Value (Get-Content $player -Raw) -NoNewline
$cache = Join-Path $root "out\deko3d_shaders\deko3d_shaders.bin"
if (Test-Path $cache) { Copy-Item $cache $sd }
if ($GlslPackage) { Copy-Item $GlslPackage (Join-Path $sd "deko3d_glsl.bin") }
if (Test-Path $elf) { Copy-Item $elf (Join-Path $out "sa2-nx.elf") }
if ($Notes) { Copy-Item $Notes (Join-Path $out "PACKAGE_NOTES.txt") }
Push-Location $root
try {
    $sha = (& git rev-parse --short=12 HEAD).Trim()
    & git archive --format=tar.gz -o (Join-Path $out "source_$sha.tgz") HEAD
} finally { Pop-Location }
$md5 = (Get-FileHash -Algorithm MD5 (Join-Path $sd "sa2.nro")).Hash.ToLower()
Write-Output "package: $out"
Write-Output ("NRO {0:N1} MB, md5 {1}; commit {2}" -f ((Get-Item (Join-Path $sd "sa2.nro")).Length / 1MB), $md5, $sha)
Get-ChildItem -Recurse $out -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring($out.Length + 1) } }, Length | Format-Table -AutoSize
