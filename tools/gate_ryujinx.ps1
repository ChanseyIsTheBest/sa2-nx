# katamari-nx: waits until an emulator run is allowed (the request's rules: no Ryujinx running, free commit memory at
# least -MinFree GB; the user, 2026-10-30 16:12: 8 GB is enough for Katamari, a light game), then runs
# tools/run_ryujinx.ps1 once with the same floor. Gives up after -WaitMinutes.
#   powershell -File tools/gate_ryujinx.ps1 -Name emu4 -Toml out/emu_x.toml [-Seconds 400] [-MinFree 8]
param([string]$Name, [string]$Toml, [int]$Seconds = 400, [double]$MinFree = 8, [int]$WaitMinutes = 90)
$runner = Join-Path $PSScriptRoot "run_ryujinx.ps1"
$deadline = (Get-Date).AddMinutes($WaitMinutes)
while ((Get-Date) -lt $deadline) {
    $os = Get-CimInstance Win32_OperatingSystem
    $free = $os.FreeVirtualMemory / 1MB
    if ($free -ge $MinFree -and -not (Get-Process Ryujinx -ErrorAction SilentlyContinue)) {
        Write-Output ("{0}: starting at {1} with {2:N1} GB free commit" -f $Name, (Get-Date -Format HH:mm:ss), $free)
        & powershell -NoProfile -File $runner -Seconds $Seconds -Name $Name -MinCommitGB $MinFree -Toml $Toml
        exit $LASTEXITCODE
    }
    Start-Sleep -Seconds 15
}
Write-Output ("{0}: free commit memory never reached {1} GB in {2} min" -f $Name, $MinFree, $WaitMinutes)
exit 5
