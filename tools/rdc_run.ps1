# katamari-nx: runs one of the tools/rdc_*.py capture scripts inside qrenderdoc (RenderDoc 1.46, banjo-nx's copy) and
# waits for it: RDC_IN / RDC_OUT / RDC_FIRST / RDC_LAST as the scripts read them.
#   powershell -File tools/rdc_run.ps1 -Script rdc_passes.py -In out/emu_runs/rdc_emu2/capture_01_frameN.rdc -Out out/rdc/emu
param([string]$Script, [string]$In, [string]$Out, [int]$First = 0, [int]$Last = 1000000000, [int]$TimeoutS = 900)
$root = Split-Path -Parent $PSScriptRoot
# renut-ultimate (2026-11-13): the RenderDoc folder from RENDERDOC_DIR, else banjo-nx's copy.
$rdcDir = if ($env:RENDERDOC_DIR) { $env:RENDERDOC_DIR } else { "D:\Banjo kazooie port\tools\RenderDoc_1.46_64" }
$q = Join-Path $rdcDir "qrenderdoc.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RDC_IN = (Resolve-Path $In).Path
$env:RDC_OUT = (Resolve-Path $Out).Path
$env:RDC_FIRST = "$First"
$env:RDC_LAST = "$Last"
$p = Start-Process -FilePath $q -ArgumentList @("--python", (Join-Path $root "tools\$Script")) -PassThru -WindowStyle Minimized
if (-not $p.WaitForExit($TimeoutS * 1000)) { Stop-Process -Id $p.Id -Force; Write-Output "timed out after $TimeoutS s" }
Get-ChildItem $env:RDC_OUT -Filter *.txt | ForEach-Object { Write-Output "== $($_.Name)"; Get-Content $_.FullName -Tail 60 }
