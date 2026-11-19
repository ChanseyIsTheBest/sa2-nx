# katamari-nx: runs the code generator on app/renut_manifest.toml (the image dump to out/renut_image.bin, as
# tools/pc_env.bat sets it for the PC build). The post-codegen passes are NOT run here: the PC build runs them after
# its own codegen step (app/CMakeLists.txt), and tools/post_codegen.py can be run by hand.
#   powershell -File tools/codegen.ps1 [-Rexglue <path>] [-Log <file>]
param([string]$Rexglue = "", [string]$Log = "")
$root = Split-Path -Parent $PSScriptRoot
if (-not $Rexglue) { $Rexglue = Join-Path $root "sdk\out\win-amd64\rexglue.exe" }
if (-not $Log) { $Log = Join-Path $root "out\codegen.log" }
$env:REXGLUE_DUMP_IMAGE = Join-Path $root "out\renut_image.bin"
Push-Location (Join-Path $root "app")
try {
    & $Rexglue codegen renut_manifest.toml *> $Log
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
Get-Content $Log -Tail 4
exit $code
