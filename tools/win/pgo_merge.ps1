# Profile-guided optimisation, step 2 of 3 (2026-10-08). Step 1 is the instrumented NRO (build_switch.ps1 with
# -Pgo generate), played on the console: it writes pgo/sa2_pgo_<N>.stream next to the NRO (one per session, see
# app/src/sa2_pgo.cpp). Copy that pgo folder to the PC and run this: every session's counts are added into .gcda files
# next to the objects of out/nx (gcov-tool merge-stream; the paths are inside the streams), then their summary is
# filled in (tools/pgo_summary.py: the stream API writes it as zero). Step 3: build_switch.ps1 -Pgo use.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\pgo_merge.ps1 <folder with the .stream files>
#
# merge-stream ADDS to .gcda files already there: a second run with the same streams would count them twice, so it
# stops when out\nx already has .gcda files (move them aside first).
param([Parameter(Mandatory = $true, Position = 0)][string]$Streams)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $root 'out\nx'
$tool = 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcov-tool.exe'
if (-not (Test-Path $tool)) { throw "gcov-tool not found: $tool" }
$files = Get-ChildItem -Path $Streams -Filter 'sa2_pgo_*.stream' -File | Sort-Object Name
if (-not $files) { throw "no sa2_pgo_*.stream in $Streams" }
if (Get-ChildItem -Path $build -Filter *.gcda -Recurse -File | Select-Object -First 1) {
    throw "out\nx already has .gcda files: move them aside first (merge-stream would add to them)"
}
foreach ($f in $files) {
    "merging $($f.Name) ($([math]::Round($f.Length / 1MB, 1)) MB)"
    & $tool merge-stream $f.FullName
    if ($LASTEXITCODE) { throw "gcov-tool merge-stream failed on $($f.FullName)" }
}
& python (Join-Path $root 'tools\pgo_summary.py') $build $files.Count
if ($LASTEXITCODE) { throw 'pgo_summary.py failed' }
"$($files.Count) session(s) merged under out\nx"
