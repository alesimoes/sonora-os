# Make the Debian WSL2 distro see the Behringer UMC1820 (Windows PowerShell 5.1).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\umc-wsl.ps1 check
#   powershell ... -File tools\umc-wsl.ps1 install    (elevated)
#   powershell ... -File tools\umc-wsl.ps1 attach     (bind once elevated, attach not)
#   powershell ... -File tools\umc-wsl.ps1 verify
#   powershell ... -File tools\umc-wsl.ps1 detach
#
# Owner steps, in order:
#   1. check      Non-elevated, read-only. Says what is missing.
#   2. install    usbipd-win is not part of Windows. Needs an elevated
#                 PowerShell; run from a normal one it prints the exact command
#                 to paste into an elevated one and exits 2. Open a new shell
#                 afterwards so PATH has usbipd.
#   3. attach     `usbipd bind --hardware-id 1397:0503` (elevated, once; it
#                 persists across reboots) then `usbipd attach --wsl Debian
#                 --hardware-id 1397:0503` (plain, no --auto-attach: repeat it
#                 after every WSL restart or replug). From a normal shell, when
#                 the bind is still missing, it prints the bind command for an
#                 elevated shell and exits 2.
#   4. verify     Inside WSL: modprobe snd-usb-audio, arecord -l,
#                 /proc/asound/cards, capture channels from arecord
#                 --dump-hw-params, the ports pw-jack's jack_lsp shows.
#   5. detach     `usbipd detach`: Windows gets the interface back. -Unbind
#                 (elevated) also removes the share.
#
# CAVEATS, honestly:
#  * While attached, WINDOWS LOSES THE DEVICE: no sound from the UMC1820 in
#    Windows apps and its TUSBAUDIO driver is idle until `detach` (or unplug).
#  * USB/IP in WSL2 carries the audio isochronous traffic over a virtual
#    network link, with extra and variable latency. What this proves is
#    FUNCTIONAL: the card enumerates, the channels are there, the ports
#    exist. Latency, xrun counts and buffer sizes measured this way are NOT
#    valid as the bare-metal figures the OS items require (brain/research,
#    drivers.usb.*); those need the real machine booted from the stick.
#  * The WSL kernel needs snd-usb-audio.ko; the stock 6.6.x
#    microsoft-standard-WSL2 kernel ships it under /lib/modules.
#
# Exit codes: 0 done / all good, 1 error or something missing, 2 refused
# because an elevated shell is needed (the command to run is printed).
# SONORA_UMC_ASSUME_ADMIN=0|1 overrides the elevation test (used by
# tools/test-umc-wsl.ps1 only).

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0)]
    [ValidateSet('check', 'install', 'attach', 'verify', 'detach')]
    [string]$Command = 'check',
    [string]$Distro = 'Debian',
    [string]$HardwareId = '1397:0503',
    [switch]$Unbind
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

function Say([string]$Text) { Write-Host $Text }

function Test-Admin {
    if ($env:SONORA_UMC_ASSUME_ADMIN) { return ($env:SONORA_UMC_ASSUME_ADMIN -eq '1') }
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Have([string]$Name) { return ($null -ne (Get-Command $Name -ErrorAction SilentlyContinue)) }

# Native commands: under Stop, 5.1 turns a stderr line into an error record.
function Run-Native([string]$Exe, [string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    return [pscustomobject]@{ Code = $code; Lines = @($out) }
}

function Need-Elevation([string]$Why, [string[]]$Commands) {
    Say "Needs an elevated PowerShell ($Why). Open one (Start, type powershell, Run as administrator) and run:"
    foreach ($c in $Commands) { Say "    $c" }
    exit 2
}

# `usbipd list` rows: BUSID  VID:PID  DEVICE...  STATE (Not shared|Shared|Attached).
function Get-Device {
    $r = Run-Native 'usbipd' @('list')
    if ($r.Code -ne 0) { throw ('usbipd list failed: ' + ($r.Lines -join ' ')) }
    foreach ($l in $r.Lines) {
        if ($l -match ('^\s*(\S+)\s+' + [regex]::Escape($HardwareId) + '\s+(.*?)\s{2,}(Not shared|Shared|Attached)\b')) {
            return [pscustomobject]@{ BusId = $Matches[1]; Name = $Matches[2]; State = $Matches[3] }
        }
    }
    return $null
}

function Wsl-Run([string[]]$Arguments, [switch]$Root) {
    $a = @('-d', $Distro)
    if ($Root) { $a += @('-u', 'root') }
    $a += '--'
    return Run-Native 'wsl' ($a + $Arguments)
}

function Get-Cards {
    $r = Wsl-Run @('cat', '/proc/asound/cards')
    if ($r.Code -ne 0) { return @() }
    return @($r.Lines | Where-Object { $_ -match '^\s*\d+\s+\[' })
}

function Find-UmcCard([string[]]$Cards) {
    foreach ($c in $Cards) {
        if ($c -match '^\s*\d+\s+\[(\S+)\s*\]:.*(UMC|1820|BEHRINGER)') { return $Matches[1] }
    }
    return $null
}

function Do-Check {
    $bad = 0
    if (Have 'usbipd') { Say 'usbipd:   installed' } else {
        Say 'usbipd:   NOT installed (run: tools\umc-wsl.ps1 install)'; $bad = 1
    }
    if (-not (Have 'wsl')) { Say 'wsl:      NOT found'; exit 1 }
    if ($bad) { Say 'device:   unknown without usbipd'; Say 'card:     not checked'; exit 1 }
    $d = Get-Device
    if ($null -eq $d) {
        Say "device:   $HardwareId not present (plug the UMC1820 into this laptop; is it powered?)"; $bad = 1
    } else {
        Say ("device:   present, bus {0}, '{1}'" -f $d.BusId, $d.Name)
        switch ($d.State) {
            'Not shared' { Say 'state:    not bound (attach, elevated once, binds it)'; $bad = 1 }
            'Shared'     { Say 'state:    bound, not attached to WSL'; $bad = 1 }
            'Attached'   { Say 'state:    attached' }
        }
    }
    $card = Find-UmcCard (Get-Cards)
    if ($card) { Say "card:     $Distro lists '$card'" } else { Say "card:     $Distro lists no UMC1820 card"; $bad = 1 }
    if ($bad) { exit 1 }
    exit 0
}

function Do-Install {
    if (Have 'usbipd') { Say 'usbipd is already installed.'; exit 0 }
    $cmd = 'winget install --exact --id dorssel.usbipd-win'
    if (-not (Test-Admin)) { Need-Elevation 'installing usbipd-win' @($cmd) }
    if (-not (Have 'winget')) { Say 'winget not found; install usbipd-win from https://github.com/dorssel/usbipd-win/releases'; exit 1 }
    $r = Run-Native 'winget' @('install', '--exact', '--id', 'dorssel.usbipd-win')
    $r.Lines | ForEach-Object { Say $_ }
    if ($r.Code -ne 0) { exit 1 }
    Say 'Installed. Open a new shell so PATH has usbipd, then: tools\umc-wsl.ps1 attach'
    exit 0
}

function Do-Attach {
    if (-not (Have 'usbipd')) { Say 'usbipd is not installed: run tools\umc-wsl.ps1 install'; exit 1 }
    $d = Get-Device
    if ($null -eq $d) { Say "$HardwareId is not present; plug in the UMC1820."; exit 1 }
    if ($d.State -eq 'Attached') { Say 'Already attached.'; exit 0 }
    if ($d.State -eq 'Not shared') {
        $bind = "usbipd bind --hardware-id $HardwareId"
        if (-not (Test-Admin)) { Need-Elevation 'one-time bind' @($bind) }
        $r = Run-Native 'usbipd' @('bind', '--hardware-id', $HardwareId)
        $r.Lines | ForEach-Object { Say $_ }
        if ($r.Code -ne 0) { exit 1 }
    }
    Say 'Attaching; Windows loses the device until detach.'
    $r = Run-Native 'usbipd' @('attach', '--wsl', $Distro, '--hardware-id', $HardwareId)
    $r.Lines | ForEach-Object { Say $_ }
    if ($r.Code -ne 0) { exit 1 }
    Say 'Attached. Next: tools\umc-wsl.ps1 verify'
    exit 0
}

function Do-Verify {
    Wsl-Run @('modprobe', 'snd-usb-audio') -Root | Out-Null
    Say '--- arecord -l'
    $r = Wsl-Run @('arecord', '-l')
    $r.Lines | ForEach-Object { Say $_ }
    Say '--- /proc/asound/cards'
    $cards = Get-Cards
    $cards | ForEach-Object { Say $_ }
    $card = Find-UmcCard $cards
    if (-not $card) {
        Say 'FAIL: no UMC1820 card. Attached? (tools\umc-wsl.ps1 check)'; exit 1
    }
    Say "--- capture channels of hw:CARD=$card"
    $r = Wsl-Run @('arecord', '-D', "hw:CARD=$card", '--dump-hw-params', '-d', '1', '/dev/null')
    $ch = $null
    foreach ($l in $r.Lines) {
        if ($l -match '^CHANNELS:\s*\[?\s*(\d+)(?:\s+(\d+))?') {
            if ($Matches[2]) { $ch = [int]$Matches[2] } else { $ch = [int]$Matches[1] }
        }
    }
    if ($null -eq $ch) { Say 'FAIL: arecord gave no CHANNELS line'; $r.Lines | ForEach-Object { Say $_ }; exit 1 }
    Say "capture channels (max): $ch"
    Say '--- pw-jack jack_lsp'
    $r = Wsl-Run @('pw-jack', 'jack_lsp')
    if ($r.Code -ne 0) {
        Say 'jack_lsp failed (is PipeWire running? tools/dev-wsl.sh audio)'
        $r.Lines | ForEach-Object { Say $_ }
    } else {
        $ports = @($r.Lines)
        $cap = @($ports | Where-Object { $_ -match 'capture' })
        Say ("jack ports: {0}, capture: {1}" -f $ports.Count, $cap.Count)
    }
    Say 'NOTE: functional result only; USB/IP latency is not a bare-metal figure.'
    exit 0
}

function Do-Detach {
    if (-not (Have 'usbipd')) { Say 'usbipd is not installed.'; exit 1 }
    $r = Run-Native 'usbipd' @('detach', '--hardware-id', $HardwareId)
    $r.Lines | ForEach-Object { Say $_ }
    if ($r.Code -ne 0) { exit 1 }
    if ($Unbind) {
        $u = "usbipd unbind --hardware-id $HardwareId"
        if (-not (Test-Admin)) { Need-Elevation 'unbind' @($u) }
        $r = Run-Native 'usbipd' @('unbind', '--hardware-id', $HardwareId)
        $r.Lines | ForEach-Object { Say $_ }
        if ($r.Code -ne 0) { exit 1 }
    }
    Say 'Detached: Windows has the UMC1820 again.'
    exit 0
}

switch ($Command) {
    'check'   { Do-Check }
    'install' { Do-Install }
    'attach'  { Do-Attach }
    'verify'  { Do-Verify }
    'detach'  { Do-Detach }
}
