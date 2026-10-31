param(
  [string]$Label = "pacing",
  [string]$Settings = "SettingsUser.fg_unlocked.json",
  [int]$WarmSeconds = 40,
  [int]$CaptureSeconds = 20
)
$ErrorActionPreference = 'Stop'
# PresentMon writes its privilege warning to stderr, and PowerShell 5.1 turns native
# stderr into a terminating NativeCommandError under Stop. That killed an earlier
# capture after the whole load-and-settle cycle had already run.
$G   = 'I:\SteamLibrary\steamapps\common\Skyrim Special Edition'
$SCR = 'F:\claudetmp\claude\J--hdresreach\5898b011-4161-48ee-8d41-d13ca41c0ac9\scratchpad'

Get-Process SkyrimSE, skse64_loader -EA SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 4
Copy-Item "$SCR\$Settings" "$G\Data\SKSE\Plugins\CommunityShaders\SettingsUser.json" -Force
foreach ($n in @('DXVK_CONFIG','CS_D3D12','DXVK_VKPROF')) { Remove-Item "Env:$n" -EA SilentlyContinue }

Start-Process -FilePath "$G\skse64_loader.exe" -WorkingDirectory $G | Out-Null

function Tool($n, $a) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$n" -Method Post `
        -Body ($a | ConvertTo-Json -Depth 6) -ContentType 'application/json' -TimeoutSec 30
}
$up = $false
for ($i = 0; $i -lt 150; $i++) { Start-Sleep -Seconds 2
    if ((Test-NetConnection 127.0.0.1 -Port 8920 -WarningAction SilentlyContinue).TcpTestSucceeded) { $up = $true; break } }
if (-not $up) { Write-Host "PACING $Label : devbench never came up"; exit 1 }

Start-Sleep -Seconds 10
try { $null = Tool 'game' @{ action='load'; name='csbench2' } } catch {}
$loaded = $false
for ($i = 0; $i -lt 150; $i++) { Start-Sleep -Seconds 4
    if (-not (Get-Process SkyrimSE -EA SilentlyContinue)) { Write-Host "PACING $Label : DIED"; exit 1 }
    try { $m = Tool 'menu' @{action='list'}; if ($m.messageBoxOpen) { $null = Tool 'menu' @{action='accept';index=1} }
          if ((Tool 'inspect' @{kind='state'}).playerLoaded) { $loaded = $true; break } } catch {} }
if (-not $loaded) { Write-Host "PACING $Label : never loaded"; exit 1 }

$null = Tool 'console' @{ command='set timescale to 0' }
$null = Tool 'camera'  @{ action='freecam'; on=$true }; Start-Sleep -Seconds 3
$null = Tool 'camera'  @{ action='drive'; x=19486.96; y=-7458.07; z=-3500; pitch=0; yaw=2.0354063510894775 }
Start-Sleep -Seconds $WarmSeconds

$pid_ = (Get-Process SkyrimSE).Id
$csv = "$SCR\pm_$Label.csv"
Remove-Item $csv -EA SilentlyContinue
$ErrorActionPreference = 'Continue'
& "$SCR\PresentMon-2.5.1-x64.exe" --process_id $pid_ --output_file $csv --timed $CaptureSeconds --terminate_after_timed --stop_existing_session 2>&1 |
    Select-String -Pattern 'Started|Stopped' | ForEach-Object { $_.Line }
Write-Host "PACING $Label : csv=$csv rows=$((Get-Content $csv -EA SilentlyContinue | Measure-Object -Line).Lines)"
