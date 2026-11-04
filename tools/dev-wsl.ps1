# The Windows side of tools/dev-wsl.sh: makes sure a Debian WSL2 distro
# exists, then runs tools/dev-wsl.sh inside it on this checkout.
#
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1              # deps, build, run
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1 run          # engine + UI only
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1 stop         # stop engine + UI
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1 status       # what is running
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1 selftest     # any dev-wsl.sh step
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1 shortcut     # Sonora icon on the Desktop
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1 user [name]  # opt in to a non-root user
#
# `-Step run` works too. Docker Desktop runs on WSL2 beside this distro.
# Nothing here shuts WSL down, changes the default distro, or touches
# docker-desktop or docker-desktop-data. If the distro has no non-root user
# the steps run as root (that works; `user` creates a normal one, but its
# home is empty, so the build starts over).
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Step)

$ErrorActionPreference = 'Stop'
$Distro = 'Debian'
$Repo = Split-Path -Parent $PSScriptRoot

function Fail([string]$what, [string]$fix) {
    Write-Host "dev-wsl: $what" -ForegroundColor Red
    if ($fix) { Write-Host "         fix: $fix" }
    exit 1
}

# wsl.exe lists distros in UTF-16 but passes a distro's own output as UTF-8.
function Invoke-Wsl([string[]]$WslArgs, [switch]$Listing) {
    $prev = [Console]::OutputEncoding
    [Console]::OutputEncoding = if ($Listing) { [System.Text.Encoding]::Unicode } else { [System.Text.Encoding]::UTF8 }
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $out = @(& wsl.exe @WslArgs) } finally {
        $ErrorActionPreference = $old
        [Console]::OutputEncoding = $prev
    }
    $script:WslExit = $LASTEXITCODE
    return ($out | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
}

# Runs a command in the distro as root; returns its output, exit code in $WslExit.
function InDistro([string]$Cmd) {
    Invoke-Wsl @('-d', $Distro, '-u', 'root', '--', 'sh', '-c', $Cmd)
}

# --- steps that need no distro ---------------------------------------------

# `-Step run` binds by name, so "-Step" itself never reaches this array.
[string[]]$Step = @($Step | Where-Object { $_ })

if ($Step.Count -gt 0 -and $Step[0] -eq 'shortcut') {
    $cmd = Join-Path $PSScriptRoot 'sonora-wsl.cmd'
    $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Sonora.lnk'
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($lnk)
    $s.TargetPath = $cmd
    $s.WorkingDirectory = $Repo
    $s.WindowStyle = 1
    $s.Description = 'Start Sonora (engine + UI) in WSL2 Debian'
    $s.Save()
    Write-Host "dev-wsl: shortcut created: $lnk"
    exit 0
}

# --- preflight -------------------------------------------------------------

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Fail 'wsl.exe is not on this machine: WSL is not installed.' 'run `wsl --install` from an elevated PowerShell, reboot, then run this again.'
}
$null = Invoke-Wsl @('--status')
if ($WslExit -ne 0) {
    Fail "WSL is installed but does not answer (wsl --status exit $WslExit)." 'run `wsl --update`; if it still fails, enable "Virtual Machine Platform" and "Windows Subsystem for Linux" in Windows Features and reboot.'
}

$installed = @(Invoke-Wsl @('--list', '--quiet') -Listing)
if ($installed -notcontains $Distro) {
    Write-Host "dev-wsl: installing the $Distro distro (WSL is already present for Docker)"
    # Installing a distro on an existing WSL2 needs neither admin rights nor a reboot.
    wsl.exe --install --distribution $Distro --no-launch
    if ($LASTEXITCODE -ne 0) {
        Fail "wsl --install -d $Distro failed ($LASTEXITCODE)." 'run `wsl --update` and `wsl --list --online`, then try again.'
    }
    $installed = @(Invoke-Wsl @('--list', '--quiet') -Listing)
    if ($installed -notcontains $Distro) { Fail "$Distro is still not listed after installing." 'run `wsl --list --verbose` and check the distro name.' }
}

$ver = InDistro 'cat /etc/debian_version'
if ($WslExit -ne 0 -or -not $ver) {
    Fail "the $Distro distro does not start (exit $WslExit)." "run `wsl -d $Distro -u root -- echo ok` to see the error; `wsl --update` and `wsl --terminate $Distro` often cure it."
}
if (-not ("$ver" -like '13*' -or "$ver" -like 'trixie*')) {
    Fail "the $Distro distro is Debian '$ver', not 13 (trixie); the packages in tools/dev-wsl.sh are pinned to trixie." 'use a trixie distro (wsl --install Debian gives the current stable).'
}

$null = InDistro 'test -S /mnt/wslg/PulseServer'
if ($WslExit -ne 0) {
    Fail 'WSLg is missing: /mnt/wslg/PulseServer does not exist, so there is no window and no sound.' 'needs Windows 11 (or Windows 10 21H2+ with a WSLg-capable `wsl --update`), and no `guiApplications=false` in %UserProfile%\.wslconfig. Then `wsl --shutdown` is NOT needed for Docker users: just `wsl --terminate Debian` and run this again.'
}

# --- the non-root user ------------------------------------------------------

# Who `wsl -d Debian` logs in as: the [user] default of wsl.conf as read at
# the distro's last start, root when there is none.
function DefaultUser {
    $u = @(Invoke-Wsl @('-d', $Distro, '--', 'id', '-un'))
    if ($u.Count -gt 0) { return $u[-1] }
    return ''
}

if ($Step.Count -gt 0 -and $Step[0] -eq 'user') {
    $name = 'sonora'
    if ($Step.Count -gt 1) { $name = $Step[1] }
    if ($name -notmatch '^[a-z][a-z0-9_-]*$') { Fail "'$name' is not a valid Linux user name." '' }
    # dev-wsl.sh does the work: it reuses uid 1000 if there is one, and edits
    # wsl.conf in place, so a second run changes nothing.
    $prev = [Console]::OutputEncoding
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = @(& wsl.exe -d $Distro -u root --cd "$Repo" -- bash tools/dev-wsl.sh user $name)
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $old
    [Console]::OutputEncoding = $prev
    $out | Where-Object { $_ -notmatch '^DEFAULT_USER=' } | ForEach-Object { Write-Host $_ }
    if ($rc -ne 0) { Fail "could not set up user '$name' (exit $rc)." "run it by hand: wsl -d $Distro -u root -- adduser $name" }
    $set = @($out | Where-Object { $_ -match '^DEFAULT_USER=' } | ForEach-Object { $_.Substring(13) })
    if ($set.Count -gt 0) { $name = $set[-1] }
    if ((DefaultUser) -eq $name) { Write-Host "dev-wsl: '$name' is already the default user; nothing to do"; exit 0 }
    # wsl.conf is read when the distro starts. Terminating it would kill a
    # running engine mid-take, so that is left to the operator.
    # pgrep exits 1 for "no match" only. Anything else (0 = running, 2/3 =
    # pgrep error, 255 = wsl.exe failed) means we do not know, so no terminate.
    # One pgrep, not a chain: `a || b` would turn an error of `a` into the
    # exit code of `b`.
    $null = InDistro "pgrep -x 'sonora-(engine|ui)'"
    if ($WslExit -eq 0) {
        Fail "'$name' is set as the default user, but Sonora is running." "run 'tools\dev-wsl.ps1 stop', then 'wsl --terminate $Distro' (only this distro; never 'wsl --shutdown')."
    }
    if ($WslExit -ne 1) {
        Fail "'$name' is set as the default user, but could not check whether Sonora is running (exit $WslExit), so $Distro was not restarted." "run 'tools\dev-wsl.ps1 status'; when Sonora is not running, 'wsl --terminate $Distro' (only this distro; never 'wsl --shutdown')."
    }
    $null = Invoke-Wsl @('--terminate', $Distro)  # only this distro
    Write-Host "dev-wsl: '$name' is now the default user. Run this script again (the first build starts from scratch)."
    exit 0
}
# With no normal user, root does the work: it needs no password and sudo is a no-op.
# Without a uid 1000 the distro is never started as its default user: a
# fresh distro may run its first-launch user prompt there.
$asUser = @('-u', 'root')
$uid1000 = InDistro 'getent passwd 1000 | cut -d: -f1'
if ($uid1000) {
    if ((DefaultUser) -ne 'root') { $asUser = @() }
}

# --- run --------------------------------------------------------------------

[string[]]$stepArgs = @($Step)
# No step is the default flow (deps, build, run): it ends the way `run` does.
$first = 'all'
if ($stepArgs.Count -gt 0) { $first = $stepArgs[0] }
$isSelftest = $stepArgs.Count -gt 0 -and $stepArgs[0] -eq 'selftest'

# The checkout is reached through /mnt/c; dev-wsl.sh builds into the
# distro's own disk (~/.cache/sonora/build-wsl), which is much faster.
$old = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$prevEnc = [Console]::OutputEncoding
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
if ($isSelftest) {
    # Keep the audio + playback numbers: they are the evidence that the
    # sound path works on this laptop. dev-wsl.sh writes the log itself
    # (--log): piping a native command into Tee-Object under PowerShell 5.1
    # turns its stderr into error records and writes the file in UTF-16.
    # Forward slashes, because the distro's shell eats unquoted backslashes.
    $dir = Join-Path $env:LOCALAPPDATA 'Sonora'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $log = Join-Path $dir ("selftest-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))
    $logArg = $log -replace '\\', '/'
    wsl.exe -d $Distro @asUser --cd "$Repo" -- bash tools/dev-wsl.sh --log "$logArg" @stepArgs
    $rc = $LASTEXITCODE
    Write-Host "dev-wsl: selftest output saved to $log"
} else {
    wsl.exe -d $Distro @asUser --cd "$Repo" -- bash tools/dev-wsl.sh @stepArgs
    $rc = $LASTEXITCODE
}
[Console]::OutputEncoding = $prevEnc
$ErrorActionPreference = $old
# A UI ended by `stop` already exits 0: dev-wsl.sh maps its 143 (SIGTERM) only
# when do_stop left its marker, so any other SIGTERM is reported here.
if ($rc -ne 0 -and $first -notin @('status', 'stop')) {
    $hint = switch -Regex ($first) {
        '^(run|all)$' { 'if the message above says the engine exited before listening, its log is printed there; if it says "no build", run this script without arguments first.' }
        '^selftest$' { 'no microphone? try: selftest --tone' }
        default { 'see the messages above.' }
    }
    Write-Host "dev-wsl: step '$first' failed (exit $rc). $hint" -ForegroundColor Red
}
exit $rc
