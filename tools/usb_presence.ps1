# Poll for the dongle's presence on USB and log every appear/disappear event.
# Original (upstream) firmware has no MAP_REG_DIAG RPC, so presence is the signal.
# Usage: powershell -NoProfile -File usb_presence.ps1 [-Seconds 180]
param([int]$Seconds = 180)

$log = "$PSScriptRoot\presence.log"
$deadline = (Get-Date).AddSeconds($Seconds)

function Snapshot {
    $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
         Where-Object { $_.InstanceId -match 'VID_054C' -and $_.InstanceId -like 'USB\*' }
    $ids = @($d | ForEach-Object { $_.InstanceId } | Sort-Object)
    # 只看接收器（0DF2/0CE6 的复合设备与 MI_00 音频口），虚拟手柄 REV_0100 不算
    $real = @($ids | Where-Object { $_ -notmatch 'REV_0100' })
    return ($real -join ' | ')
}

"=== presence watch start $(Get-Date -Format 'HH:mm:ss') (${Seconds}s) ===" |
    Tee-Object -FilePath $log
$last = $null
$upSince = $null
while ((Get-Date) -lt $deadline) {
    $now = Snapshot
    if ($now -eq '') { $now = '<none>' }
    if ($now -ne $last) {
        if ($last -eq '<none>' -or $last -eq $null) {
            "PRESENT  $(Get-Date -Format 'HH:mm:ss.fff')  $now" | Tee-Object -FilePath $log -Append
            $upSince = Get-Date
        } else {
            $dur = if ($upSince) { [math]::Round(((Get-Date) - $upSince).TotalSeconds, 1) } else { '?' }
            "GONE     $(Get-Date -Format 'HH:mm:ss.fff')  (was up ${dur}s)  $last" | Tee-Object -FilePath $log -Append
        }
        $last = $now
    }
    Start-Sleep -Milliseconds 700
}
if ($last -and $last -ne '<none>' -and $upSince) {
    $dur = [math]::Round(((Get-Date) - $upSince).TotalSeconds, 1)
    "=== end: still up for ${dur}s ===" | Tee-Object -FilePath $log -Append
} else {
    "=== end: not present ===" | Tee-Object -FilePath $log -Append
}
