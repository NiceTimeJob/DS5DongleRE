# deploy_public.ps1 -- publish webgui to https://donglere.megu17.xyz (Cloudflare Worker).
# ASCII only (Windows PowerShell 5.1 reads BOM-less .ps1 as GBK and mangles CJK).
#
# RULE (user, 2026-10-17): NEVER deploy on your own initiative. The public site may only be
# updated after the user has tested the local build and explicitly said "you can deploy".
# This script enforces that with a typed confirmation, so it cannot happen by accident.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File F:\DongleRE\tools\deploy_public.ps1
#   ... -DryRun     (show what would change, do not touch anything)

param(
    [switch]$DryRun,
    [string]$Staging = 'F:\BL616\.scratch\pages_deploy',
    [string]$Root    = 'F:\DongleRE\webgui',
    [string]$Url     = 'https://donglere.megu17.xyz'
)

$ErrorActionPreference = 'Continue'

function Say($m)  { Write-Host "[..] $m" }
function Warn($m) { Write-Host "[!]  $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[X]  $m" -ForegroundColor Red; exit 1 }

# ---- 1. local build number (source of truth = webgui/js/app.js) ----
$appJs = Join-Path $Root 'js\app.js'
if (-not (Test-Path $appJs)) { Fail "not found: $appJs" }
$localBuild = ([regex]::Match((Get-Content $appJs -Raw), "const BUILD = '([^']+)'")).Groups[1].Value
if (-not $localBuild) { Fail "cannot read BUILD from $appJs" }
Say "local  : $localBuild  ($Root)"

# ---- 2. what is live right now (best effort: the box sometimes truncates downloads) ----
$liveBuild = '?'
for ($i = 1; $i -le 3; $i++) {
    try {
        $t = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 20 "$Url/js/app.js?x=$i").Content
        $m = [regex]::Match($t, "const BUILD = '([^']+)'")
        if ($m.Success) { $liveBuild = $m.Groups[1].Value; break }
    } catch { Start-Sleep -Seconds 2 }
}
Say "live   : $liveBuild  ($Url)"

# ---- 3. list-only diff of staging vs webgui ----
$excl = 'serve.bat','serve.py','serve_launch.py','serve_launch.log','smoke_err.txt','smoke_out.html',
        '_gmfake.js','_smoke.html','_shot.html','_logtest.html','_gmtest.html','_lstest.html','wrangler.jsonc'
$args = @($Root, $Staging, '/MIR', '/L', '/NJH', '/NDL', '/NP', '/XF') + $excl + @('/XD', '.wrangler')
$diff = (& robocopy @args 2>&1 | Out-String)
$filesLine = ([regex]::Match($diff, '(?m)^\s*Files\s*:.*$')).Value.Trim()
$bytesLine = ([regex]::Match($diff, '(?m)^\s*Bytes\s*:.*$')).Value.Trim()
Say "dry run (webgui -> staging): $filesLine"
Say "                              $bytesLine"
if ($filesLine -match '^\s*Files\s*:\s+\d+\s+0\s') {
    Warn 'staging is already identical to webgui - nothing to publish'
}

if ($DryRun) { Say 'DryRun: nothing was deployed.'; exit 0 }

if ($liveBuild -eq $localBuild) {
    Warn "live already reports $localBuild - deploy anyway?"
}

# ---- 4. hard gate: the operator must type the word ----
Write-Host ''
Write-Host 'RULE: only deploy after the user tested the local build and said it is OK.' -ForegroundColor Cyan
$ans = Read-Host 'Type exactly  deploy  to publish, anything else aborts'
if ($ans -ne 'deploy') { Warn 'aborted (nothing was deployed)'; exit 2 }

# ---- 5. sync + deploy ----
Say 'syncing staging ...'
$rc = @($Root, $Staging, '/MIR', '/NJH', '/NDL', '/NFL', '/NP', '/XF') + $excl + @('/XD', '.wrangler')
& robocopy @rc | Out-Null
if ($LASTEXITCODE -ge 8) { Fail "robocopy failed ($LASTEXITCODE)" }

Say 'deploying (wrangler) ...'
$env:NODE_OPTIONS = '--dns-result-order=ipv4first'
$env:XDG_CONFIG_HOME = Join-Path $env:APPDATA 'xdg.config'
Push-Location $Staging
try {
    for ($i = 1; $i -le 3; $i++) {
        $out = npx -y wrangler deploy 2>&1 | Out-String
        if ($out -match 'Version ID') {
            ($out -split "`n" | Select-String -Pattern 'Success|Version ID|custom domain' |
                ForEach-Object { $_.Line.Trim() })
            break
        }
        Warn "attempt $i failed (auth/network hiccup) - retrying in 6 s"
        Start-Sleep -Seconds 6
        if ($i -eq 3) { Fail 'deploy failed 3 times' }
    }
} finally { Pop-Location }

Write-Host ''
Write-Host 'NEXT' -ForegroundColor Cyan
Write-Host "  1) open $Url and press Ctrl+F5, confirm the version shows $localBuild"
Write-Host '  2) check the flash page still lists the right firmware package'
exit 0
