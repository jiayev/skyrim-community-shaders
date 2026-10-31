<#
.SYNOPSIS
    Continuous screen captures of a game run, for finding DCLF's corrupt frames (tools/dclf-corrupt-frames.py).

.DESCRIPTION
    Start it alongside the game (a run with CS_DCLF_FRAME_TRACE=1). It waits until CommunityShaders.log shows gameplay
    ('DLSS evaluate', at most 200 seconds), then captures the primary screen as fast as it can for -Seconds, each capture a
    960x540 JPEG named by its local time to the millisecond (HH-mm-ss.fff.jpg), the clock the log's timestamps use. A
    corrupt frame shows as a single-capture outlier, which dclf-corrupt-frames.py finds and maps to the frame trace's lines.
    The game window must stay in front: the capture is of the screen.

.PARAMETER Out
    Directory the captures are written to, created when missing.

.PARAMETER Seconds
    How long to capture once gameplay starts (default 70).

.EXAMPLE
    powershell -File tools/dclf-capture-burst.ps1 -Out C:\runs\burst -Seconds 60
#>
param([Parameter(Mandatory)] [string]$Out, [int]$Seconds = 70)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$log = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition\SKSE\CommunityShaders.log'
New-Item -ItemType Directory -Force $Out | Out-Null
$deadline = (Get-Date).AddSeconds(200)
Start-Sleep -Seconds 15
while ((Get-Date) -lt $deadline) {
    if ((Test-Path $log) -and (Get-Content $log -Raw -ErrorAction SilentlyContinue).Contains('DLSS evaluate')) { break }
    Start-Sleep -Milliseconds 500
}
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$params = New-Object System.Drawing.Imaging.EncoderParameters 1
$params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 70L
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$small = New-Object System.Drawing.Bitmap 960, 540
$sg = [System.Drawing.Graphics]::FromImage($small)
$sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::Bilinear
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    $t = Get-Date
    $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $sg.DrawImage($bitmap, 0, 0, 960, 540)
    $small.Save((Join-Path $Out ($t.ToString('HH-mm-ss.fff') + '.jpg')), $codec, $params)
}
