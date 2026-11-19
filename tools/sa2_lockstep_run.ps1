# sa2-nx-ult: one lockstep recording of the PC build (SA2's frame comparison, tools/sa2_compare_frames.py), of this
# tree's build (renut.exe) or of sa2-nx 1.0.6's PC build (sa2.exe, the reference): muted, minimised, a fresh user data
# folder (the Battle DLC installed from dlc/ next to the executable), Renut on Vulkan, game frames From to From +
# Frames x Every recorded (every Every-th kept; each kept frame's full-resolution hash in captures/video_1s.hash).
# Stops only the process it started; the recording is moved to out\lockstep\<Name>.
#   powershell -File tools\sa2_lockstep_run.ps1 -Name ult_attract [-Exe ref] [-From 6000] [-Frames 750] [-Every 4]
#       [-Seconds 240] [-Script "26:START,..."] [-Extra "--key=value ..."] [-HashOnly]
# -HashOnly (this tree's build only: sa2_video_hash_only): the hashes and labels without the frames (no .rgba), so a
# wide window costs no disk. Each run numbers its frames from its own start and the title and loading screens take a
# different number of them, so two runs are compared by content (tools/sa2_compare_frames.py --align).
param([string]$Name = "lockstep", [string]$Exe = "ult", [int]$From = 6000, [int]$Frames = 750, [int]$Every = 4,
      [int]$Seconds = 0, [string]$Script = "", [string]$Extra = "", [switch]$HashOnly)
$root = Split-Path -Parent $PSScriptRoot
if ($Exe -eq "ult") {
    $exePath = Join-Path $root "app\out\pc\renut.exe"
} elseif ($Exe -eq "ref") {
    $exePath = Join-Path (Split-Path -Parent $root) "sa2-nx\out\pc\sa2.exe"
} else {
    $exePath = $Exe
}
if (-not (Test-Path $exePath)) { Write-Output "no executable at $exePath"; exit 2 }
$dir = Split-Path $exePath
$out = Join-Path $root "out\lockstep\$Name"
$user = Join-Path $root "out\lockstep_user\$Name"
foreach ($d in @($out, $user)) { if (Test-Path $d) { Remove-Item -Recurse -Force $d }; New-Item -ItemType Directory -Force $d | Out-Null }
if ($Seconds -le 0) { $Seconds = [int](($From + $Frames * $Every) / 60.0) + 90 }
$capDir = Join-Path $dir "captures"
Get-ChildItem $capDir -Filter "video_1s.*" -ErrorAction SilentlyContinue | Remove-Item -Force
$env:REX_AUDIO_MUTE = "true"
$env:REX_WINDOW_START_MINIMIZED = "true"
$env:REX_USER_DATA_ROOT = $user
$env:REX_LOG_FILE = (Join-Path $out "$Name.log")
$env:REX_SA2_VIDEO_S = "1"
$env:REX_SA2_VIDEO_FRAME = "$From"
$env:REX_SA2_VIDEO_FRAMES = "$Frames"
$env:REX_SA2_VIDEO_LOCKSTEP_EVERY = "$Every"
$env:REX_SA2_VIDEO_EVERY = "1"
$env:REX_SA2_VIDEO_LOCKSTEP = "true"
$env:REX_RENUT_GPU_BACKEND = "vulkan"
if ($HashOnly) { $env:REX_SA2_VIDEO_HASH_ONLY = "true" } else { Remove-Item Env:\REX_SA2_VIDEO_HASH_ONLY -ErrorAction SilentlyContinue }
if ($Exe -eq "ref") { $env:REX_SA2_RENDERER = "renut" } else { Remove-Item Env:\REX_SA2_RENDERER -ErrorAction SilentlyContinue }
if ($Script) { $env:REX_INPUT_SCRIPT = $Script; $env:REX_HEADLESS = "true" } else { Remove-Item Env:\REX_INPUT_SCRIPT, Env:\REX_HEADLESS -ErrorAction SilentlyContinue }
$argList = @($Extra -split ' ' | Where-Object { $_ } | ForEach-Object { if ($_ -match ' ') { '"' + $_ + '"' } else { $_ } })
$started = Get-Date
$p = if ($argList.Count) { Start-Process -FilePath $exePath -ArgumentList $argList -WorkingDirectory $dir -PassThru -WindowStyle Minimized }
     else { Start-Process -FilePath $exePath -WorkingDirectory $dir -PassThru -WindowStyle Minimized }
Write-Output "started $Exe pid $($p.Id) for up to $Seconds s"
$txt = Join-Path $capDir "video_1s.txt"
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $txt) -and (Get-Item $txt).LastWriteTime -gt $started) { Start-Sleep -Seconds 2; break }
    Start-Sleep -Seconds 3
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(20000) | Out-Null; Write-Output "stopped pid $($p.Id)" }
foreach ($v in 'REX_AUDIO_MUTE','REX_WINDOW_START_MINIMIZED','REX_USER_DATA_ROOT','REX_LOG_FILE','REX_SA2_VIDEO_S','REX_SA2_VIDEO_FRAME',
              'REX_SA2_VIDEO_FRAMES','REX_SA2_VIDEO_LOCKSTEP_EVERY','REX_SA2_VIDEO_EVERY','REX_SA2_VIDEO_LOCKSTEP','REX_RENUT_GPU_BACKEND',
              'REX_SA2_RENDERER','REX_INPUT_SCRIPT','REX_HEADLESS','REX_SA2_VIDEO_HASH_ONLY') { Remove-Item "Env:\$v" -ErrorAction SilentlyContinue }
Get-ChildItem $capDir -Filter "video_1s.*" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $started } |
    Move-Item -Destination $out -Force
Get-ChildItem $out | Select-Object Name, Length
$log = Join-Path $out "$Name.log"
if (Test-Path $log) { Select-String -Path $log -Pattern '\[capture\]' | Select-Object -Last 4 | ForEach-Object { $_.Line } }
