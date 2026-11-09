# build_and_flash.ps1 -- build THIS checkout's firmware, sanity-check the artifact,
# re-generate the WEB FLASH PACKAGE (manifest.json + bins for the flash page), and
# optionally flash over serial.
# ASCII only: Windows PowerShell 5.1 reads BOM-less .ps1 as GBK and would mangle CJK.
#
# Normal use: double-click build_and_flash.bat next to this file.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File build_and_flash.ps1
#   ... -SkipBuild        (artifact already fresh: only pack)
#   ... -SkipPack         (do not touch the web package)
#   ... -SerialFlash      (also flash over UART; needs the dongle in ISP/BOOT)
#   ... -OnlyFlash        (skip build+pack, just flash what is there)
#
# IMPORTANT (learned the hard way 2026-10-17): pass the SDK path with FORWARD slashes.
# The SDK's project.build builds $(CMAKE) as "$(BL_SDK_BASE)/tools/cmake/bin/cmake.exe"
# and the `cmake_cache` recipe contains quotes ("MinGW Makefiles"), so make runs it
# through sh; with backslashes in BL_SDK_BASE sh eats them (F:\B -> F:B) and you get
#   /bin/sh: F:BL616bouffalo_sdk/tools/cmake/bin/cmake.exe: No such file or directory
# ...while the "simple" commands (cmake -E touch ...) still work, which makes it look
# like a random failure. Keep these two variables forward-slashed.
#
# Why the pack step: the flash page (webgui/js/page-flash.js) reads
# webgui/firmware/manifest.json + the bins next to it. Those are generated FROM the
# build output by webflash/make_fw_pack.py, so a fresh build is NOT visible on that
# page until the package is re-generated. tools/test_fw_pack.py then checks
# manifest <-> files <-> embedded version string <-> partition table.

param(
    [string]$SdkBase    = 'F:/BL616/bouffalo_sdk',
    [string]$Toolchain  = 'F:/BL616/toolchain_gcc_t-head_windows',
    [string]$FwDir      = 'F:\DongleRE\firmware',
    [int]   $TimeoutSec = 600,
    [switch]$SkipBuild,
    [switch]$SkipPack,
    [switch]$SerialFlash,
    [switch]$OnlyFlash
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

$root  = Split-Path -Parent $PSScriptRoot            # F:\DongleRE
$want  = 'R3.25'                                     # must match firmware/src/version.h
$bat   = Join-Path $FwDir 'build_windows.bat'
$bin   = Join-Path $FwDir 'build\build_out\ds5donglere_bl618_bl616.bin'
$pack  = Join-Path $root 'webflash\make_fw_pack.py'
$check = Join-Path $PSScriptRoot 'test_fw_pack.py'
$fwps  = Join-Path $PSScriptRoot 'flash_wait.ps1'
$man   = Join-Path $root 'webgui\firmware\manifest.json'
$log   = Join-Path $FwDir 'build_and_flash_build.log'

function Say($m)  { Write-Host "[..] $m" }
function Warn($m) { Write-Host "[!]  $m" -ForegroundColor Yellow }
function Tail($file, $n) {
    if (Test-Path $file) {
        Write-Host "----- last $n lines of $file -----" -ForegroundColor DarkGray
        Get-Content $file -Tail $n -ErrorAction SilentlyContinue | Write-Host
        Write-Host '----- end of log -----' -ForegroundColor DarkGray
    }
}
function Fail($m) { Write-Host "[X]  $m" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- python
$py = $null
foreach ($c in @('python', 'D:\Python\python3.11.4\python.exe')) {
    $g = Get-Command $c -ErrorAction SilentlyContinue
    if ($g) { $py = $g.Source; break }
}
$pyOk = $false
if ($py) {
    & $py -c "import sys; sys.exit(0)" 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { $pyOk = $true }
}
if ($pyOk) { Say "python   : $py" }
else { Warn "no working python found; the web package step will be skipped" }

if ($OnlyFlash) { $SkipBuild = $true; $SkipPack = $true; $SerialFlash = $true }

# ---------------------------------------------------------------- 1. build
if (-not $SkipBuild) {
    Say "checkout : $FwDir"
    if (-not (Test-Path $bat)) { Fail "build_windows.bat not found under $FwDir" }
    Say "SDK      : $SdkBase   (forward slashes on purpose)"
    if (-not (Test-Path ($SdkBase + '/project.build'))) { Fail "SDK not found: $SdkBase" }
    Say "toolchain: $Toolchain"
    if (-not (Test-Path ($Toolchain + '/bin/riscv64-unknown-elf-gcc.exe'))) { Fail "toolchain not found: $Toolchain" }

    # a working cmake is the one thing that bit us: check it can start at all
    $cm = $SdkBase + '/tools/cmake/bin/cmake.exe'
    if (Test-Path $cm) {
        $cv = (& $cm --version 2>&1 | Select-Object -First 1)
        if ($LASTEXITCODE -ne 0) { Warn "cmake.exe did not answer --version ($cv) - build will probably fail" }
        else { Say "cmake    : $cv" }
    } else { Warn "cmake.exe not found at $cm" }

    $isp = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -like '*VID_349B&PID_6160*' } | Select-Object -First 1
    if ($isp) { Say "ISP device found (BOOT bootrom) - good" }
    else { Warn "no ISP device seen right now (only needed for -SerialFlash; the web page connects itself)" }

    Say "building ... (cold tree = full build, a few minutes; do not close this window)"
    $env:BL_SDK_BASE         = $SdkBase          # FORWARD slashes (see header)
    $env:TOOLCHAIN_PATH      = $Toolchain
    $env:MSYS_NO_PATHCONV    = '1'
    $env:MSYS2_ARG_CONV_EXCL = '*'
    if (Test-Path $log) { Remove-Item $log -Force -ErrorAction SilentlyContinue }
    & cmd.exe /C ('"' + $bat + '" build') 2>&1 | Tee-Object -FilePath $log | Write-Host
    Say "build log: $log   (UTF-16, opens fine in Notepad)"
}

# ---------------------------------------------------------------- 2. artifact gate
if (-not (Test-Path $bin)) {
    Tail $log 25
    Fail "no artifact: $bin  (the build failed - see the log tail above)"
}
$it = Get-Item $bin
$age = [int]((Get-Date) - $it.LastWriteTime).TotalMinutes
Say ("artifact : {0} bytes, written {1} ({2} min ago)" -f $it.Length, $it.LastWriteTime, $age)
$txt = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($bin))
if ($txt -notmatch [regex]::Escape($want)) {
    Tail $log 25
    Fail "artifact does NOT contain '$want' -> stale build (it is $age min old). Refusing to pack or flash. See the log tail."
}
Say "artifact contains '$want' - ok"

# ---------------------------------------------------------------- 3. web package
if (-not $SkipPack) {
    if (-not $pyOk) {
        Warn "SKIPPED the web package (no python). The flash page would keep showing the old version."
    } else {
        if (-not (Test-Path $pack)) { Fail "packer not found: $pack" }
        Say "packing web firmware package (webflash/firmware + webgui/firmware) ..."
        $env:PYTHONUTF8 = '1'
        & $py -X utf8 $pack --src $FwDir 2>&1 | Tee-Object -FilePath (Join-Path $root 'webflash\_pack.log') | Write-Host
        if (-not (Test-Path $man)) { Fail "pack step produced no manifest: $man" }
        $mj = Get-Content $man -Raw -Encoding UTF8 | ConvertFrom-Json
        Say ("manifest : version='{0}'  date={1}  notes={2}" -f $mj.version, $mj.date, $mj.notes.Count)
        if ($mj.version -notmatch [regex]::Escape($want)) {
            Fail "manifest version '$($mj.version)' does not contain '$want' - the page would still show the old one."
        }
        if (Test-Path $check) {
            Say "validating package (test_fw_pack.py) ..."
            & $py -X utf8 $check 2>&1 | Write-Host
            if ($LASTEXITCODE -ne 0) { Warn "test_fw_pack.py reported failures (see above) - read them before flashing" }
        }
    }
}

# ---------------------------------------------------------------- 4. optional serial flash
if ($SerialFlash) {
    Say "flashing over UART (erase=1) ..."
    if (-not (Test-Path $fwps)) { Fail "flash_wait.ps1 not found: $fwps" }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fwps -Dir $FwDir -TimeoutSec $TimeoutSec -BootRetries 1
    if ($LASTEXITCODE -ne 0) { Warn "flash_wait.ps1 exited $LASTEXITCODE" }
}

Write-Host ''
Write-Host 'NEXT' -ForegroundColor Cyan
if ($SerialFlash) {
    Write-Host '  1) UNPLUG the dongle, then plug it back in WITHOUT pressing BOOT.'
    Write-Host '  2) python -X utf8 F:\DongleRE\tools\verify_fw.py 60'
    Write-Host '     expect BL616-DongleRE R3.25 / 0x0B = 66 bytes / CONFIG_VERSION 15'
} else {
    Write-Host '  1) Flash page: press Ctrl+F5 (the package is now re-generated).'
    Write-Host '     It should show today s date, version R3.25 and the R3.25 change notes.'
    Write-Host '  2) Dongle: hold BOOT while plugging in USB, then Connect serial -> Start flashing.'
    Write-Host '  3) After it finishes: unplug, replug WITHOUT BOOT.'
    Write-Host '  4) python -X utf8 F:\DongleRE\tools\verify_fw.py 60'
    Write-Host '     expect BL616-DongleRE R3.25 / 0x0B = 66 bytes / CONFIG_VERSION 15'
}
Write-Host '  5) Config page: Ctrl+F5 (web build r107). Device config was reset (CFG_VER 14 -> 15):'
Write-Host '     pick your saved profile from the game-config dropdown, then press Save.'
exit 0
