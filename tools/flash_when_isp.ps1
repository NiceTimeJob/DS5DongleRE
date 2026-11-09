# Wait for the BL616 ISP bootrom COM port, then flash immediately.
# ASCII only: Windows PowerShell 5.1 reads BOM-less .ps1 as GBK and mangles CJK.
# Usage: powershell -NoProfile -File flash_when_isp.ps1 [timeout_seconds]
param([int]$TimeoutSec = 300)

$proj  = 'F:\BL616\DongleRE\firmware'
$flash = 'F:\BL616\bouffalo_sdk\tools\bflb_tools\bouffalo_flash_cube\BLFlashCommand.exe'
$log   = "$PSScriptRoot\flash_isp.log"
$deadline = (Get-Date).AddSeconds($TimeoutSec)

function Find-IspCom {
    $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
         Where-Object { $_.InstanceId -like '*VID_349B&PID_6160*' } |
         Select-Object -First 1
    if (-not $d) { return $null }
    $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_FriendlyName' -ErrorAction SilentlyContinue
    if ($p -and $p.Data -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

"=== waiting for ISP (max $TimeoutSec s) at $(Get-Date -Format 'HH:mm:ss') ===" | Tee-Object -FilePath $log
$com = $null
while ((Get-Date) -lt $deadline) {
    $com = Find-IspCom
    if ($com) { break }
    Start-Sleep -Milliseconds 200
}

if (-not $com) {
    "!!! timeout: no BL616 bootrom within $TimeoutSec s (hold BOOT while plugging USB)" | Tee-Object -FilePath $log -Append
    exit 2
}

"=== ISP found on $com at $(Get-Date -Format 'HH:mm:ss') - flashing ===" | Tee-Object -FilePath $log -Append
Push-Location $proj
& $flash --interface=uart --baudrate=2000000 --port=$com --chipname=bl616 --cpu_id= --config=flash_prog_cfg.ini 2>&1 |
    Tee-Object -FilePath $log -Append
$rc = $LASTEXITCODE
Pop-Location
"=== BLFlashCommand exit=$rc ===" | Tee-Object -FilePath $log -Append
exit $rc
