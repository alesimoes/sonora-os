# The Windows plugin package: the native suite as VST3 bundles, built with MSVC
# (owner, 2026-11-05: the plugins are a product of their own), as
# sonora-plugins-<version>-windows-x64.zip. The Linux package, with LV2 as well,
# is tools/package-plugins.sh.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\package-plugins.ps1 [-OutDir dist]
#
# Needs Visual Studio 2022 Build Tools with the C++ workload. The tree is
# build-plugins-win\ (only common\* and plugins\native, SONORA_PLUGINS_ONLY),
# so the next build is incremental. Bundles go through the install rules of
# plugins/native/format, so a processor marked EXPERIMENTAL, or one over its
# CPU class (ADR-0011 3.5), is not packaged.
param(
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools are not installed (no vswhere.exe).' }
    $vs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath
    if (-not $vs) { throw 'No Visual Studio install with the MSVC x64 tools.' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    Set-Location $root
}

function Step([string]$name, [scriptblock]$body) {
    Write-Host "== $name"
    & $body
    if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE)" }
}

# The version: SONORA_VERSION, else git describe; a tree with no tag reads
# 0.0.0-<commits>-g<sha> (the same rule as package-plugins.sh).
$version = $env:SONORA_VERSION
if (-not $version) {
    $d = (& git describe --tags --always 2>$null)
    if ($d -match '^v?[0-9]') {
        $version = $d -replace '^v', ''
    } else {
        $n = (& git rev-list --count HEAD 2>$null)
        $version = "0.0.0-$n-g$d"
    }
}
$name = "sonora-plugins-$version-windows-x64"

$build = 'build-plugins-win'
Step 'configure' {
    cmake -S . -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONORA_PLATFORM=neutral `
        -DSONORA_PLUGINS_ONLY=ON -DSONORA_WINDOWS_VST3=ON
}
Step 'build' { cmake --build $build --parallel --target sonora_native_bundles }

$stage = Join-Path $root "$build\stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$fmt = Get-ChildItem -Path $build -Recurse -Filter cmake_install.cmake |
    Where-Object { $_.FullName -match 'plugins[\/]native[\/]format[\/]cmake_install.cmake$' } |
    Select-Object -First 1
if (-not $fmt) { throw 'no install script for the native bundles' }
Step 'install' {
    cmake "-DCMAKE_INSTALL_PREFIX=$stage" -DCMAKE_INSTALL_CONFIG_NAME=Release -P $fmt.FullName
}
$vst3 = Join-Path $stage 'lib\vst3'
$bundles = @(Get-ChildItem -Path $vst3 -Directory -Filter '*.vst3')
if ($bundles.Count -eq 0) { throw 'no VST3 bundle was installed' }

# A DLL a host cannot load is a failed package: the VST3 entry points must be
# exported, and the module must sit where a host looks (Contents\x86_64-win).
Write-Host '== exports'
foreach ($b in $bundles) {
    $dll = Join-Path $b.FullName ('Contents\x86_64-win\' + $b.Name)
    if (-not (Test-Path $dll)) { throw "$($b.Name): no module at Contents\x86_64-win" }
    $exports = (& dumpbin /exports $dll) -join "`n"
    foreach ($sym in 'GetPluginFactory', 'InitDll', 'ExitDll') {
        if ($exports -notmatch "\b$sym\b") { throw "$($b.Name): $sym is not exported" }
    }
    if (-not (Test-Path (Join-Path $b.FullName 'Contents\Resources\descriptor.json'))) {
        throw "$($b.Name): no descriptor.json"
    }
}

$pkg = Join-Path $stage $name
New-Item -ItemType Directory -Force -Path (Join-Path $pkg 'vst3'), (Join-Path $pkg 'licenses') | Out-Null
Copy-Item -Recurse -Force -Path (Join-Path $vst3 '*') -Destination (Join-Path $pkg 'vst3')
Copy-Item LICENSE (Join-Path $pkg 'licenses\Sonora-GPL-3.0.txt')
$dpf = 'plugins\native\format\third_party\dpf'
Copy-Item "$dpf\LICENSE" (Join-Path $pkg 'licenses\DPF-ISC.txt')
Copy-Item "$dpf\LICENSING.md" (Join-Path $pkg 'licenses\DPF-LICENSING.md')
$notice = @"
Sonora plugins $version

The plugins are licensed under the GNU General Public License, version 3
(Sonora-GPL-3.0.txt). They are built on DPF, the DISTRHO Plugin Framework,
Copyright (C) 2012-2025 Filipe Coelho, under the ISC licence (DPF-ISC.txt);
its VST3 part is ISC, and no part of Steinberg's VST3 SDK is used.
No sample, impulse response, preset or model of another product is included.
"@
Set-Content -Path (Join-Path $pkg 'licenses\NOTICE.txt') -Value $notice -Encoding ASCII
$readme = @"
Sonora plugins $version (Windows x64)

$($bundles.Count) VST3 bundles in vst3\. Each processor comes in a stereo build
and a "-mono" build (one input, one output).

Install: copy the folders in vst3\ into C:\Program Files\Common Files\VST3
(all users) and rescan in your host. Licences and notices: licenses\.
"@
Set-Content -Path (Join-Path $pkg 'README.txt') -Value $readme -Encoding ASCII

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $pkg -DestinationPath $zip
Write-Host "package: $zip ($($bundles.Count) VST3)"
