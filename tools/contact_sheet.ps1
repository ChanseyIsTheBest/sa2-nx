# Tiles PNG captures into one contact sheet (to compare consecutive frames for flicker).
# Usage: powershell -File tools\contact_sheet.ps1 -Folder capturas -Out sheet.png [-Columns 4] [-Scale 0.4]
param([string]$Folder, [string]$Out, [int]$Columns = 4, [double]$Scale = 0.4, [string]$Filter = "*.png")
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem -Path $Folder -Filter $Filter | Sort-Object Name
if (-not $files) { Write-Output "no images"; exit 1 }
$first = [System.Drawing.Image]::FromFile($files[0].FullName)
$w = [int]($first.Width * $Scale); $h = [int]($first.Height * $Scale); $first.Dispose()
$rows = [math]::Ceiling($files.Count / $Columns)
$sheet = New-Object System.Drawing.Bitmap ($w * $Columns), ($h * $rows)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
for ($i = 0; $i -lt $files.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile($files[$i].FullName)
  $g.DrawImage($img, ($i % $Columns) * $w, [math]::Floor($i / $Columns) * $h, $w, $h)
  $img.Dispose()
}
$sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $Out ($($sheet.Width)x$($sheet.Height), $($files.Count) frames)"
