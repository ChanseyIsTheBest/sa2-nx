# katamari-nx: one PC test run of app/out/pc/renut.exe: minimised, muted, its own log, PNG captures every N seconds
# (nfsmw_captura_cada_s), stopped after -Seconds. Stops only the process it started.
#   powershell -File tools/run_pc.ps1 -Seconds 120 -Name boot1 [-Every 15] [-Extra "--gpu_plugin=xenos --foo=1"]
# The log is written unbuffered (log_file_writer = false): the run is stopped by force, which would drop the buffer.
# -Visible: the window is shown (not minimised): Renut presents (and so captures and paces by vsync) only with a
# swapchain, which a minimised window does not get.
# -Game (blur-nx, 2026-11-18): another game folder (a web installer's game_root, with its .lz4b files, for example) in
# place of Katamari's (passing --game_data_root in -Extra too would give the key twice).
param([int]$Seconds = 90, [string]$Name = "run", [int]$Every = 15, [string]$Extra = "", [switch]$Visible,
      [string]$Game = "D:/beautifulkatamari/game")
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "app\out\pc"
$exe = Join-Path $dir "renut.exe"
$logs = Join-Path $root "out\pc_runs\$Name"
New-Item -ItemType Directory -Force $logs | Out-Null
$log = Join-Path $logs "$Name.log"
Remove-Item $log -ErrorAction SilentlyContinue
$min = if ($Visible) { "false" } else { "true" }
$args = @("--log_file=$log", "--audio_mute=true", "--window_start_minimized=$min", "--fullscreen=false",
          "--game_data_root=$Game", "--user_data_root=$root/out/pc_user",
          "--nfsmw_captura_cada_s=$Every", "--nfsmw_captura_max=60", "--log_file_writer=false") +
        @($Extra -split ' ' | Where-Object { $_ })
$style = if ($Visible) { "Normal" } else { "Minimized" }
$p = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $dir -PassThru -WindowStyle $style `
     -RedirectStandardError (Join-Path $logs "stderr.txt") -RedirectStandardOutput (Join-Path $logs "stdout.txt")
Write-Output "started pid $($p.Id)"
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output "stopped pid $($p.Id) after $Seconds s" }
else { Write-Output "exited by itself with code $($p.ExitCode)" }
Start-Sleep -Seconds 1
# The captures land next to the executable (nfsmw_nativo_captura.cpp); move this run's into its folder.
Get-ChildItem $dir -Filter "*.png" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $p.StartTime } |
    Move-Item -Destination $logs -Force -ErrorAction SilentlyContinue
Get-ChildItem (Join-Path $dir "capturas") -Filter "*.png" -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt $p.StartTime } | Move-Item -Destination $logs -Force -ErrorAction SilentlyContinue
if (Test-Path $log) { Get-Content $log -Tail 25 }
