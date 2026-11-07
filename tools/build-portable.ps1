# The portable modules under MSVC (ADR-0008 section 4), on this machine: what
# CI's "Portable modules under MSVC" job ran on a GitHub Windows runner
# (owner, 2026-11-05: every build runs locally). tools/local-ci.sh runs it
# last, after the Linux steps.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-portable.ps1
#
# Needs Visual Studio 2022 Build Tools with the C++ workload (MSVC, the
# Windows SDK, and the CMake and Ninja that ship with it). The tree stays in
# build-portable\ so the next build is incremental.
#
# -Package [-OutDir dir] also builds the Windows VST3 package afterwards
# (tools\package-plugins.ps1, build-plugins-win\): the portable target holds
# no bundle, so the zip is a build of its own.
param(
    [switch]$Package,
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools are not installed (no vswhere.exe).' }
$vs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath
if (-not $vs) { throw 'No Visual Studio install with the MSVC x64 tools.' }

# The developer environment (cl, link, the SDK, cmake, ninja) in this process.
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
Set-Location $root

function Step([string]$name, [scriptblock]$body) {
    Write-Host "== $name"
    & $body
    if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE)" }
}

$env:SONORA_DSP_QUICK = '1'
Step 'configure' { cmake -S . -B build-portable -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONORA_PLATFORM=neutral }
Step 'build' { cmake --build build-portable --parallel --target sonora_portable }
Step 'test' { ctest --test-dir build-portable --output-on-failure -R 'dsp_|native_' }
Step 'write-usb.ps1 against fake disks' { powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-write-usb.ps1 }
Step 'umc-wsl.ps1 against fake usbipd and wsl' { powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-umc-wsl.ps1 }
if ($Package) {
    $pkgArgs = @()
    if ($OutDir) { $pkgArgs = @('-OutDir', $OutDir) }
    Step 'plugin package (VST3 zip)' { powershell -NoProfile -ExecutionPolicy Bypass -File tools\package-plugins.ps1 @pkgArgs }
}
Write-Host 'portable: all passed'
