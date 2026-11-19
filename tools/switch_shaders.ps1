# katamari-nx: the Switch build's shader inputs from the PC build (banjo-nx's steps, docs/title-update-3.md "Shaders the
# update brings" and docs/deko3d-port-faq.md), on every shader of out/renut_shader_dump (the image's containers and the
# PC runs' captures, which the PC build adds there):
#   1. Renut's XenosRecomp (the PC build's, app/out/pc/xenos_recomp): the early-alpha dump (BANJO_HLSL_DUMP_ONLY),
#      tools/renut_early_alpha.py, then the translation with BANJO_HLSL_OVERRIDE and BANJO_DEKO3D_SPIRV_DIR (SPIR-V per
#      shader);
#   2. tools/deko3d_shaders.py (SPIRV-Cross -> GLSL for uam), tools/deko3d_glsl_opt.py (every pass), then
#      tools/deko3d_shader_package.py -> app/out/renut_switch/deko3d_glsl*.{bin,cpp};
#   3. tools/renut_switch_shader_cache.py --pc app/out/pc -> app/out/renut_switch/shader_cache_switch.cpp.
#   powershell -File tools/switch_shaders.ps1
# katamari-nx (2026-11-02): XenosRecomp samples inputs at the centroid under MSAA as each shader's header asks
# (BANJO_CENTROID, on unless 0). -NoCentroid translates with BANJO_CENTROID=0 (rc9's shaders) into its own folders
# (out/deko3d_shaders_nocentroid, app/out/renut_switch_nocentroid: a GLSL package the runtime loads from the SD,
# /switch/katamari_nx/deko3d_glsl.bin), step 3 left out (the NRO's own shader cache stays the default set's).
param([switch]$NoCentroid)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = "C:\Python314\python.exe"
$exe = Join-Path $root "app\out\pc\xenos_recomp\build\XenosRecomp\XenosRecomp.exe"
$common = Join-Path $root "app\thirdparty\reblue-XenosRecomp\XenosRecomp\shader_common.h"
$dump = Join-Path $root "out\renut_shader_dump"
$work = Join-Path $root $(if ($NoCentroid) { "out\deko3d_shaders_nocentroid" } else { "out\deko3d_shaders" })
$pkg = $(if ($NoCentroid) { "app\out\renut_switch_nocentroid" } else { "app\out\renut_switch" })
if ($NoCentroid) { $env:BANJO_CENTROID = "0" } else { Remove-Item Env:\BANJO_CENTROID -ErrorAction SilentlyContinue }
$early = Join-Path $work "early_alpha_hlsl"
$spirv = Join-Path $work "spirv"
foreach ($d in @($early, $spirv)) { if (Test-Path $d) { Remove-Item -Recurse -Force $d }; New-Item -ItemType Directory -Force $d | Out-Null }
if (-not (Test-Path $exe)) { throw "no ${exe}: build the PC app with Renut first" }
# renut-ultimate (port 69, 2026-11-10, blur-nx): "Continue" from here. XenosRecomp writes informational lines to stderr
# ("fetch lod ..."), which Windows PowerShell makes errors when the output is captured, and "Stop" ended the script at
# the first one (Blur). Every native call below checks $LASTEXITCODE; the folders above were made under "Stop".
$ErrorActionPreference = "Continue"
$env:BANJO_HLSL_DUMP_ONLY = "1"
& $exe $dump (Join-Path $work "early_alpha_pass1.cpp") $common $early
if ($LASTEXITCODE -ne 0) { throw "XenosRecomp (early-alpha dump) failed" }
Remove-Item Env:\BANJO_HLSL_DUMP_ONLY
& $py (Join-Path $root "tools\renut_early_alpha.py") $early
if ($LASTEXITCODE -ne 0) { throw "renut_early_alpha.py failed" }
$env:BANJO_HLSL_OVERRIDE = $early
$env:BANJO_DEKO3D_SPIRV_DIR = $spirv
& $exe $dump (Join-Path $work "shader_cache_deko3d.cpp") $common
if ($LASTEXITCODE -ne 0) { throw "XenosRecomp (deko3d SPIR-V) failed" }
Remove-Item Env:\BANJO_HLSL_OVERRIDE; Remove-Item Env:\BANJO_DEKO3D_SPIRV_DIR
Push-Location $root
try {
    & $py tools\deko3d_shaders.py --spirv $spirv --out $work
    if ($LASTEXITCODE -ne 0) { throw "deko3d_shaders.py failed" }
    & $py tools\deko3d_glsl_opt.py --in (Join-Path $work "glsl") --out (Join-Path $work "glsl_opt")
    if ($LASTEXITCODE -ne 0) { throw "deko3d_glsl_opt.py failed" }
    & $py tools\deko3d_shader_package.py --glsl (Join-Path $work "glsl_opt") --out $pkg
    if ($LASTEXITCODE -ne 0) { throw "deko3d_shader_package.py failed" }
    if (-not $NoCentroid) {
        & $py tools\renut_switch_shader_cache.py --pc app\out\pc
        if ($LASTEXITCODE -ne 0) { throw "renut_switch_shader_cache.py failed" }
    }
} finally { Pop-Location; Remove-Item Env:\BANJO_CENTROID -ErrorAction SilentlyContinue }
Get-ChildItem (Join-Path $root $pkg) | Select-Object Name, Length
