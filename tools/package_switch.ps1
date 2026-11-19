# katamari-nx: the Switch package (the request's step 7), as banjo-nx's builds/ folders:
#   builds/<date>_<time>_<name>/
#     COPY_TO_SD_switch_katamari-nx/   katamari-nx.nro, katamari.toml (dist's renut.toml; named renut.toml in a
#                                      KATAMARI_BUILTIN_CONFIG=OFF build), configs/ (renut.toml, renut_play.toml,
#                                      renut_ab.toml and the rest), deko3d_shaders.bin (the prebuilt DKSH cache),
#                                      game_root/ (your game files)
#     COPY_TO_SD_ROOT/atmosphere/contents/05D6EA1333487100/exefs/main.npdm   (the four-core forwarder NPDM; only with
#                                      -FourCore since rc6: the user runs on three cores)
#     katamari-nx.elf                  the unstripped ELF of this NRO (profiles map to it)
#     source_<commit>.tgz              the source snapshot (git archive of the commit; no game data, no generated code)
#     README.txt, PACKAGE_NOTES.txt
# Nothing of the game's data goes in by default (the NRO holds the recompiled code; the DKSH cache is compiled shader
# code). katamari-nx (2026-10-31, the user's request, for their own SD card): -GameRoot copies the extracted disc into
# game_root and -Dlc the DLC packages into dlc (installed at the first start). Never into git, never uploaded.
#   powershell -File tools/package_switch.ps1 -Name pgo_lto [-Build app\out\switch-pgo-use] [-Notes <file>]
#       [-GameRoot D:\beautifulkatamari\game] [-Dlc D:\beautifulkatamari\dlc]
param([string]$Name = "package", [string]$Build = "app\out\switch", [string]$Notes = "", [string]$GameRoot = "",
      [string]$Dlc = "", [switch]$FourCore)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$free = (Get-PSDrive D).Free / 1GB
if ($free -lt 2) { throw ("D: has {0:N1} GB free: not packaging (the request: stop and say so)" -f $free) }
$b = Join-Path $root $Build
$nro = Join-Path $b "renut.nro"
$elf = Join-Path $b "renut"
if (-not (Test-Path $nro)) { throw "no NRO at $nro" }
$stamp = Get-Date -Format "yyyy-MM-dd_HHmm"
$out = Join-Path (Split-Path -Parent $root) "builds\${stamp}_$Name"
$sd = Join-Path $out "COPY_TO_SD_switch_katamari-nx"
New-Item -ItemType Directory -Force $sd, (Join-Path $sd "configs"), (Join-Path $sd "game_root") | Out-Null
Copy-Item $nro (Join-Path $sd "katamari-nx.nro")
$dist = Join-Path $root "dist\switch\katamari-nx"
# The one toml the NRO reads (src/renut_app.h, OnConfigurePaths): under any other name it does nothing on the console.
$builtin = Select-String -Path (Join-Path $b "CMakeCache.txt") -Pattern "^KATAMARI_BUILTIN_CONFIG:BOOL=ON" -Quiet
Copy-Item (Join-Path $dist "renut.toml") (Join-Path $sd $(if ($builtin) { "katamari.toml" } else { "renut.toml" }))
Copy-Item (Join-Path $dist "renut.toml"), (Join-Path $dist "renut_play.toml"), (Join-Path $dist "renut_ab.toml") (Join-Path $sd "configs")
Copy-Item (Join-Path $dist "configs\*.toml") (Join-Path $sd "configs")
$cache = Join-Path $root "out\deko3d_shaders\deko3d_shaders.bin"
if (Test-Path $cache) { Copy-Item $cache $sd }
if ($GameRoot) {
    $need = ((Get-ChildItem $GameRoot -Recurse -File | Measure-Object -Sum Length).Sum + 1GB) / 1GB
    if ((Get-PSDrive D).Free / 1GB -lt $need) { throw ("D: has too little space for the game files ({0:N1} GB needed)" -f $need) }
    & robocopy $GameRoot (Join-Path $sd "game_root") /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy of the game files failed ($LASTEXITCODE)" }
} else {
    Set-Content -Path (Join-Path $sd "game_root\PUT_YOUR_GAME_FILES_HERE.txt") -Encoding ascii -Value `
        "Extract your own Beautiful Katamari disc here: default.xex, the .dat files, data/ and the rest (README.txt)."
}
New-Item -ItemType Directory -Force (Join-Path $sd "dlc") | Out-Null
if ($Dlc) {
    Copy-Item (Join-Path $Dlc "*") (Join-Path $sd "dlc") -Force
} else {
    Set-Content -Path (Join-Path $sd "dlc\PUT_THE_DLC_PACKAGES_HERE.txt") -Encoding ascii -Value `
        "Optional: Beautiful Katamari's DLC packages (Xbox 360 LIVE files, content type 2) here; installed at the first start."
}
if ($FourCore) {
    $npdm = Join-Path $out "COPY_TO_SD_ROOT\atmosphere\contents\05D6EA1333487100\exefs"
    New-Item -ItemType Directory -Force $npdm | Out-Null
    Copy-Item (Join-Path $root "tools\forwarder\katamari_main.npdm") (Join-Path $npdm "main.npdm")
}
if (Test-Path $elf) { Copy-Item $elf (Join-Path $out "katamari-nx.elf") }
Copy-Item (Join-Path $root "dist\switch\README.txt") $out
if ($Notes) { Copy-Item $Notes (Join-Path $out "PACKAGE_NOTES.txt") }
Push-Location $root
try {
    $sha = (& git rev-parse --short=12 HEAD).Trim()
    & git archive --format=tar.gz -o (Join-Path $out "source_$sha.tgz") HEAD
} finally { Pop-Location }
$md5 = (Get-FileHash -Algorithm MD5 (Join-Path $sd "katamari-nx.nro")).Hash.ToLower()
Write-Output "package: $out"
Write-Output ("NRO {0:N1} MB, md5 {1}; commit {2}" -f ((Get-Item (Join-Path $sd "katamari-nx.nro")).Length / 1MB), $md5, $sha)
Get-ChildItem -Recurse $out | Where-Object { -not $_.PSIsContainer } | Select-Object FullName, Length | Format-Table -AutoSize
