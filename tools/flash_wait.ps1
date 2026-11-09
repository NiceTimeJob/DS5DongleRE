# Wait for the BL616 ISP bootrom, flash with --reset, and check whether the app came up.
# ASCII only (Windows PowerShell 5.1 reads BOM-less .ps1 as GBK and mangles CJK).
#
# IMPORTANT (measured 2026-10-11, 4 attempts in a row): on this board the BOOT pin is sampled
# ONLY at power-on. A software reset (BLFlashCommand --reset, or the ISP protocol's reset
# command) always drops back into the ROM's ISP mode while BOOT is asserted, and releasing
# BOOT afterwards does NOT help. So the workflow is fixed:
#     hold BOOT while plugging in  ->  flash  ->  UNPLUG AND REPLUG WITHOUT BOOT
# The retry loop below is kept only for the case where the user power-cycles on their own
# during the wait; it is not a substitute for the replug.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File flash_wait.ps1 [-TimeoutSec 1200] [-BootRetries 1]
param(
    [string]$Dir = 'F:\BL616\DongleRE\firmware',
    [string]$Cfg = 'flash_prog_cfg.ini',
    [int]$TimeoutSec = 1200,
    [int]$BootRetries = 1
)

$flash = 'F:\BL616\bouffalo_sdk\tools\bflb_tools\bouffalo_flash_cube\BLFlashCommand.exe'
$py = 'D:\Python\python3.11.4\python.exe'
# diag.py / verify_fw.py 与本脚本同目录（DongleRE\tools）
$scratch = $PSScriptRoot

function Find-IspCom {
    $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
         Where-Object { $_.InstanceId -like '*VID_349B&PID_6160*' } |
         Select-Object -First 1
    if (-not $d) { return $null }
    $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_FriendlyName' -ErrorAction SilentlyContinue
    if ($p -and $p.Data -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

function Test-AppUp {
    Push-Location $scratch
    & $py -X utf8 -c "import sys, diag; sys.exit(0 if diag.open_dev(retries=1) else 1)" *> $null
    $rc = $LASTEXITCODE
    Pop-Location
    return ($rc -eq 0)
}

function Invoke-Flash([string]$com) {
    Push-Location $Dir
    $out = (& $flash --interface=uart --baudrate=2000000 --port=$com --chipname=bl616 `
                     --cpu_id= --config=$Cfg --reset 2>&1 | Out-String)
    Pop-Location
    if ($out -match 'All programming completed successfully') { return $true }
    Write-Output $out
    return $false
}

Write-Output "=== waiting for ISP (max $TimeoutSec s) - hold BOOT while plugging in ==="
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$com = $null
while ((Get-Date) -lt $deadline) {
    $com = Find-IspCom
    if ($com) { break }
    Start-Sleep -Milliseconds 200
}
if (-not $com) { Write-Output "!!! timeout: no BL616 bootrom found"; exit 2 }

Write-Output "=== ISP on $com - flashing (with --reset) ==="
if (-not (Invoke-Flash $com)) { Write-Output "!!! flash reported failure"; exit 3 }
Write-Output "=== flash + SHA verify OK ==="
Write-Output "=== NOTE: unplug and replug WITHOUT BOOT to run the new firmware (BOOT is only"
Write-Output "===       sampled at power-on; a soft reset keeps landing in ISP on this board) ==="

for ($i = 1; $i -le $BootRetries; $i++) {
    Start-Sleep -Seconds 6
    if (Test-AppUp) {
        Write-Output "=== app is running (attempt $i) - done ==="
        Push-Location $scratch
        & $py -X utf8 verify_fw.py 5
        Pop-Location
        exit 0
    }
    Write-Output "=== app not up after attempt $i - re-sending reset (boots only if BOOT was released) ==="
    if (-not (Find-IspCom)) {
        Write-Output "=== not in ISP right now - waiting 5 s and checking the app once more ==="
        Start-Sleep -Seconds 5
        if (Test-AppUp) {
            Write-Output "=== app is running - done ==="
            Push-Location $scratch
            & $py -X utf8 verify_fw.py 5
            Pop-Location
            exit 0
        }
        Write-Output "!!! neither ISP nor a running app - check the USB connection"; exit 4
    }
    if (-not (Invoke-Flash $com)) { Write-Output "!!! re-flash failed"; exit 3 }
}

Write-Output "=== still in ISP: UNPLUG and REPLUG WITHOUT BOOT ==="
exit 1
