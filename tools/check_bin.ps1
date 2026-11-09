$p = 'E:\BL616_Project\DongleRE\firmware\firmware\lctech616\ds5donglere-lctech616.bin'
$a = [System.IO.File]::ReadAllBytes($p)
Start-Sleep -Milliseconds 300
$b = [System.IO.File]::ReadAllBytes($p)
$t = [System.Text.Encoding]::ASCII.GetString($b)
Write-Output ('size=' + $b.Length)
Write-Output ('version=' + [regex]::Match($t, '(LCT616|M0S|BL618)-DongleRE R\d+(\.\d+)?( HS)?( \w+)?').Value)
Write-Output ('sha256=' + (Get-FileHash $p -Algorithm SHA256).Hash)
