# Tests for tools/umc-wsl.ps1 (Windows PowerShell 5.1, no Pester).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-umc-wsl.ps1
#
# Fake usbipd.cmd, wsl.cmd and winget.cmd sit first on PATH; they log their
# arguments and print canned output from a scratch folder. Nothing elevated,
# no real USB. Runs in CI's Windows job. Exit code 0 when every check passes.

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot 'umc-wsl.ps1'
$script:Failed = 0
function Ok([string]$What) { Write-Host ('UMC-WSL-PS ok    ' + $What) }
function Bad([string]$What) { Write-Host ('UMC-WSL-PS FAIL  ' + $What); $script:Failed = 1 }
function Expect([bool]$Cond, [string]$What) { if ($Cond) { Ok $What } else { Bad $What } }

$work = Join-Path ([IO.Path]::GetTempPath()) ('sonora-umc-wsl-test-' + [guid]::NewGuid().ToString('N'))
$bin = Join-Path $work 'bin'
New-Item -ItemType Directory -Path $bin | Out-Null
$log = Join-Path $work 'calls.log'

$usbipdBody = "@echo off`r`necho usbipd %*>>`"$log`"`r`nif `"%1`"==`"list`" type `"$work\list.txt`"`r`nexit /b 0"
Set-Content -Encoding ASCII (Join-Path $bin 'usbipd.cmd') $usbipdBody
Set-Content -Encoding ASCII (Join-Path $bin 'winget.cmd') "@echo off`r`necho winget %*>>`"$log`"`r`nexit /b 0"
# wsl: picks the canned file by what is asked.
$wslBody = @(
    '@echo off',
    "echo wsl %*>>`"$log`"",
    "echo %* | findstr /c:`"/proc/asound/cards`" >nul && ( type `"$work\cards.txt`" & exit /b 0 )",
    "echo %* | findstr /c:`"dump-hw-params`" >nul && ( type `"$work\hw.txt`" & exit /b 0 )",
    "echo %* | findstr /c:`"arecord -l`" >nul && ( type `"$work\cards.txt`" & exit /b 0 )",
    "echo %* | findstr /c:`"jack_lsp`" >nul && ( type `"$work\jack.txt`" & exit /b 0 )",
    'exit /b 0'
)
Set-Content -Encoding ASCII (Join-Path $bin 'wsl.cmd') $wslBody

function Set-Fake([string]$State, [bool]$Card) {
    $row = '2-3          1397:0503  UMC1820  ' + $State
    Set-Content -Encoding ASCII (Join-Path $work 'list.txt') @('Connected:', 'BUSID  VID:PID    DEVICE    STATE', $row)
    if ($Card) {
        Set-Content -Encoding ASCII (Join-Path $work 'cards.txt') ' 1 [UMC1820       ]: USB-Audio - UMC1820'
    } else {
        Set-Content -Encoding ASCII (Join-Path $work 'cards.txt') '--- no soundcards ---'
    }
    Set-Content -Encoding ASCII (Join-Path $work 'hw.txt') @('HW Params of device "hw:CARD=UMC1820":', 'CHANNELS: [2 18]', 'RATE: [44100 192000]')
    Set-Content -Encoding ASCII (Join-Path $work 'jack.txt') @('system:capture_1', 'system:capture_2', 'system:playback_1')
    Remove-Item $log -ErrorAction SilentlyContinue
}

function Run-Script([string]$Admin, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    $oldPath = $env:PATH
    $env:PATH = $bin + ';' + $oldPath
    $env:SONORA_UMC_ASSUME_ADMIN = $Admin
    try {
        $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -NonInteractive -File $script @Arguments 2>&1 | Out-String
        $code = $LASTEXITCODE
    } finally { $env:PATH = $oldPath; Remove-Item Env:SONORA_UMC_ASSUME_ADMIN }
    $calls = ''
    if (Test-Path $log) { $calls = Get-Content $log -Raw }
    return [pscustomobject]@{ Code = $code; Text = $out; Calls = $calls }
}

try {
    Set-Fake 'Attached' $true
    $r = Run-Script '0' @('check')
    Expect ($r.Code -eq 0 -and $r.Text -match "lists 'UMC1820'" -and $r.Text -match 'state:\s+attached') 'check: attached, card listed, exit 0'
    Set-Fake 'Not shared' $false
    $r = Run-Script '0' @('check')
    Expect ($r.Code -eq 1 -and $r.Text -match 'not bound' -and $r.Text -match 'no UMC1820 card') 'check: not bound, no card, exit 1'

    # usbipd missing (the owner's machine today)
    Set-Fake 'Attached' $true
    Remove-Item (Join-Path $bin 'usbipd.cmd') -Force
    $r = Run-Script '0' @('install')
    Expect ($r.Code -eq 2 -and $r.Text -match 'winget install --exact --id dorssel.usbipd-win' -and $r.Calls -notmatch 'winget') 'install: non-admin prints the command, exit 2, no winget call'
    $r = Run-Script '0' @('check')
    Expect ($r.Code -eq 1 -and $r.Text -match 'NOT installed') 'check: usbipd missing reported'
    $r = Run-Script '1' @('install')
    Expect ($r.Code -eq 0 -and $r.Calls -match 'winget install --exact --id dorssel.usbipd-win') 'install: admin runs winget'
    Set-Content -Encoding ASCII (Join-Path $bin 'usbipd.cmd') $usbipdBody

    Set-Fake 'Not shared' $true
    $r = Run-Script '0' @('attach')
    Expect ($r.Code -eq 2 -and $r.Text -match 'usbipd bind --hardware-id 1397:0503' -and $r.Calls -notmatch 'usbipd bind') 'attach: non-admin, unbound: prints bind command, exit 2'
    $r = Run-Script '1' @('attach')
    Expect ($r.Code -eq 0 -and $r.Calls -match 'usbipd bind --hardware-id 1397:0503' -and $r.Calls -match 'usbipd attach --wsl Debian --hardware-id 1397:0503' -and $r.Calls -notmatch 'auto-attach') 'attach: admin binds then plain attach'
    Set-Fake 'Shared' $true
    $r = Run-Script '0' @('attach')
    Expect ($r.Code -eq 0 -and $r.Calls -notmatch 'usbipd bind' -and $r.Calls -match 'usbipd attach') 'attach: already bound, non-admin attaches'

    Set-Fake 'Attached' $true
    $r = Run-Script '0' @('verify')
    Expect ($r.Code -eq 0 -and $r.Text -match 'capture channels \(max\): 18' -and $r.Text -match 'capture: 2' -and $r.Calls -match 'modprobe snd-usb-audio' -and $r.Calls -match 'hw:CARD=UMC1820') 'verify: 18 channels, 2 capture ports, modprobe'
    Set-Fake 'Shared' $false
    $r = Run-Script '0' @('verify')
    Expect ($r.Code -eq 1 -and $r.Text -match 'FAIL: no UMC1820') 'verify: no card fails'

    Set-Fake 'Attached' $true
    $r = Run-Script '0' @('detach')
    Expect ($r.Code -eq 0 -and $r.Calls -match 'usbipd detach --hardware-id 1397:0503' -and $r.Text -match 'Windows has') 'detach: non-admin detaches'
    $r = Run-Script '0' @('detach', '-Unbind')
    Expect ($r.Code -eq 2 -and $r.Text -match 'usbipd unbind --hardware-id 1397:0503') 'detach -Unbind non-admin: prints unbind command, exit 2'
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($script:Failed) { Write-Host 'UMC-WSL-PS FAILED'; exit 1 }
Write-Host 'UMC-WSL-PS all passed'
exit 0
