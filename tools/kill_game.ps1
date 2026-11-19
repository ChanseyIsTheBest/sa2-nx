# Stops only the renut.exe processes started from a given folder (default: this repo's PC build).
param([string]$PathLike = "*banjo-nx\app\out\pc-nx\*")
Get-Process renut -ErrorAction SilentlyContinue | Where-Object { $_.Path -like $PathLike } | ForEach-Object { Stop-Process -Id $_.Id -Force }
