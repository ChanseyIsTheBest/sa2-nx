# katamari-nx: one emulator run with RenderDoc captures of Ryujinx's Vulkan back end (banjo-nx's method,
# docs/deko3d-renderer.md section 12): RenderDoc 1.46's Vulkan layer enabled in Ryujinx's environment only
# (VK_LAYER_PATH = the RenderDoc folder, ENABLE_VULKAN_RENDERDOC_CAPTURE = 1), and qrenderdoc running
# tools/rdc_trigger.py, which connects to the target and takes captures -Times seconds after connecting.
# The same rules as tools/run_ryujinx.ps1: no other Ryujinx running, free commit memory at least -MinCommitGB,
# only the processes it started are stopped.
#   powershell -File tools/run_ryujinx_rdc.ps1 -Seconds 420 -Name rdc1 -Toml out/x.toml -Times "330,345"
param([int]$Seconds = 420, [string]$Name = "rdc", [string]$Toml = "", [string]$Times = "330", [double]$MinCommitGB = 8)
$root = Split-Path -Parent $PSScriptRoot
# renut-ultimate (2026-11-13): the RenderDoc folder from RENDERDOC_DIR, else banjo-nx's copy.
$rdcDir = if ($env:RENDERDOC_DIR) { $env:RENDERDOC_DIR } else { "D:\Banjo kazooie port\tools\RenderDoc_1.46_64" }
$ryu = "D:\New folder\Ryubing-src\ryubing\build\Ryujinx.exe"
$data = "D:\New folder\ryu-data\katamari"
$sd = Join-Path $data "sdcard\switch\katamari-nx"
$nro = Join-Path $sd "katamari-nx.nro"
$out = Join-Path $root "out\emu_runs\$Name"
if (Get-Process -Name Ryujinx -ErrorAction SilentlyContinue) { Write-Output "a Ryujinx is running (another port's?): not starting"; exit 2 }
$os = Get-CimInstance Win32_OperatingSystem
$freeCommit = $os.FreeVirtualMemory / 1MB
if ($freeCommit -lt $MinCommitGB) { Write-Output ("free commit memory {0:N1} GB < {1} GB: not starting" -f $freeCommit, $MinCommitGB); exit 3 }
if ($Toml) { Copy-Item $Toml (Join-Path $sd "katamari.toml") -Force }
New-Item -ItemType Directory -Force $out | Out-Null
$logs = Join-Path $sd "logs"
if (Test-Path $logs) { Remove-Item -Recurse -Force $logs }
$env:DOTNET_ROOT = "D:\New folder\dotnet10"
# An explicit layer through VK_LAYER_PATH: it is enabled by VK_INSTANCE_LAYERS (ENABLE_VULKAN_RENDERDOC_CAPTURE alone only
# acts on an implicit, registered layer: the first run's trigger found no target).
$env:VK_LAYER_PATH = $rdcDir
$env:VK_INSTANCE_LAYERS = "VK_LAYER_RENDERDOC_Capture"
$env:ENABLE_VULKAN_RENDERDOC_CAPTURE = "1"
$p = Start-Process -FilePath $ryu -ArgumentList @("-r", "`"$data`"", "`"$nro`"") -PassThru -WindowStyle Minimized `
     -RedirectStandardOutput (Join-Path $out "ryujinx_stdout.txt") -RedirectStandardError (Join-Path $out "ryujinx_stderr.txt")
Remove-Item Env:\VK_LAYER_PATH, Env:\VK_INSTANCE_LAYERS, Env:\ENABLE_VULKAN_RENDERDOC_CAPTURE
$env:RDC_OUT = $out
$env:RDC_TIMES = $Times
$q = Start-Process -FilePath (Join-Path $rdcDir "qrenderdoc.exe") -ArgumentList @("--python", "`"$(Join-Path $root 'tools\rdc_trigger.py')`"") `
     -PassThru -WindowStyle Minimized
Write-Output ("started Ryujinx pid {0} and qrenderdoc pid {1} (free commit {2:N1} GB)" -f $p.Id, $q.Id, $freeCommit)
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output "stopped Ryujinx pid $($p.Id) after $Seconds s" }
Start-Sleep -Seconds 3
if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force; Write-Output "stopped qrenderdoc pid $($q.Id)" }
if (Test-Path $logs) { Copy-Item -Recurse $logs (Join-Path $out "logs") -Force }
$caps = Join-Path $sd "capturas"
if (Test-Path $caps) { Get-ChildItem $caps -Filter *.png | Move-Item -Destination $out -Force -ErrorAction SilentlyContinue }
Get-Content (Join-Path $out "trigger.txt") -ErrorAction SilentlyContinue
