# The Windows side of tools/dev-wsl.sh: makes sure a Debian WSL2 distro
# exists, then runs tools/dev-wsl.sh inside it on this checkout.
#
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1            # deps, build, run
#   powershell -ExecutionPolicy Bypass -File tools\dev-wsl.ps1 selftest   # any dev-wsl.sh step
#
# Docker Desktop runs on WSL2 beside this distro. Nothing here shuts WSL
# down, changes the default distro, or touches docker-desktop or
# docker-desktop-data.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Step)

$ErrorActionPreference = 'Stop'
$Distro = 'Debian'
$Repo = Split-Path -Parent $PSScriptRoot

# wsl.exe writes UTF-16; read it as such or the names do not compare.
$prev = [Console]::OutputEncoding
[Console]::OutputEncoding = [System.Text.Encoding]::Unicode
$installed = @(wsl.exe --list --quiet) | ForEach-Object { $_.Trim() } | Where-Object { $_ }
[Console]::OutputEncoding = $prev

if ($installed -notcontains $Distro) {
    Write-Host "dev-wsl: installing the $Distro distro (WSL is already present for Docker)"
    # Installing a distro on an existing WSL2 needs neither admin rights nor
    # a reboot. If WSL itself were missing, this would ask for both: then run
    # `wsl --install` from an elevated prompt and reboot first.
    wsl.exe --install --distribution $Distro --no-launch
    if ($LASTEXITCODE -ne 0) { throw "wsl --install -d $Distro failed ($LASTEXITCODE)" }
    Write-Host @"
dev-wsl: $Distro is installed. Open it once to create your Linux user:

    wsl -d $Distro

then exit and run this script again.
"@
    exit 0
}

# The checkout is reached through /mnt/c; dev-wsl.sh builds into the
# distro's own disk (~/.cache/sonora/build-wsl), which is much faster.
wsl.exe -d $Distro --cd "$Repo" -- bash tools/dev-wsl.sh @Step
exit $LASTEXITCODE
