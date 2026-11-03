# Tests for tools/write-usb.ps1 (Windows PowerShell 5.1, no Pester).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-write-usb.ps1
#
# Nothing here touches a real disk. Every run of the writer passes -FakeDisks,
# which replaces the machine's disks with descriptions whose Path is a regular
# file in a scratch folder; the writer's "file" kind refuses anything starting
# with \\. The device-path checks below call only the path validators, which
# reject a bad path before CreateFileW is reached.
#
# Needs no elevation and about 150 MB of space in %TEMP%, removed at the end.
# Runs in CI's Windows job. Exit code 0 when every check passes.

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$writer = Join-Path $PSScriptRoot 'write-usb.ps1'
. $writer

$script:Failed = 0
function Ok([string]$What) { Write-Host ('WRITE-USB-PS ok    ' + $What) }
function Bad([string]$What) { Write-Host ('WRITE-USB-PS FAIL  ' + $What); $script:Failed = 1 }
function Expect([bool]$Cond, [string]$What) { if ($Cond) { Ok $What } else { Bad $What } }

$MiB = 1048576
$work = Join-Path ([IO.Path]::GetTempPath()) ('sonora-write-usb-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

function New-FilledFile([string]$Path, [int64]$Bytes, [int]$Seed) {
    $fs = [IO.File]::Create($Path)
    try {
        # A negative seed means the 0xAB pattern: a stick's old contents.
        $buf = [byte[]](@(0xAB) * $MiB)
        $rng = New-Object Random ([math]::Abs($Seed))
        $left = $Bytes
        while ($left -gt 0) {
            if ($Seed -ge 0) { $rng.NextBytes($buf) }
            $n = [int][math]::Min($left, $buf.Length)
            $fs.Write($buf, 0, $n)
            $left -= $n
        }
    } finally { $fs.Dispose() }
}

function Get-Sha([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

# Runs the writer as the owner would, in its own process, non-interactive.
function Invoke-Writer([string[]]$Arguments) {
    # Under Stop, Windows PowerShell 5.1 turns the child's first stderr line
    # into a terminating error here, and the test would stop instead of
    # reporting what the writer said.
    $ErrorActionPreference = 'Continue'
    $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -NonInteractive -File $writer @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $out }
}

try {
    Write-Host '--- device paths ---'
    $p = Get-DevicePath 3
    $codes = @($p.ToCharArray() | ForEach-Object { [int]$_ })
    Expect ($p.Length -eq 18 -and $codes[0] -eq 92 -and $codes[1] -eq 92 -and $codes[2] -eq 46 -and $codes[3] -eq 92 -and $p.Substring(4) -ceq 'PhysicalDrive3') `
        'Get-DevicePath 3 is backslash, backslash, dot, backslash, PhysicalDrive3 (18 chars)'
    $one = [string][char]92 + '.' + [string][char]92 + 'PhysicalDrive3'
    Expect (Test-DevicePath $p) 'the built path is accepted'
    Expect (-not (Test-DevicePath $one)) 'one leading backslash (the 2026-10-12 bug) is refused'
    Expect (-not (Test-DevicePath 'C:\PhysicalDrive3')) 'C:\PhysicalDrive3 is refused'
    Expect (-not (Test-DevicePath ($p + 'x'))) 'a trailing non-digit is refused'
    Expect (-not (Test-DevicePath ($p.Substring(0, 17)))) 'PhysicalDrive without a number is refused'
    Expect ([SonoraUsb.Native]::DevicePrefix -ceq $p.Substring(0, 17)) 'the C# device prefix is byte for byte the same'
    foreach ($case in @(@($one, 'device'), @('C:\PhysicalDrive3', 'device'), @($p, 'file'), @($p, 'volume'))) {
        $threw = $false
        try { [SonoraUsb.Native]::Open($case[0], $case[1], $false, $false).Dispose() } catch [ArgumentException] { $threw = $true } catch { $threw = $_.Exception.InnerException -is [ArgumentException] }
        Expect $threw ('Native.Open refuses ' + $case[0] + ' as kind ' + $case[1] + ' before opening anything')
    }

    Write-Host '--- refusals ---'
    $good = [pscustomobject]@{ Number = 3; BusType = 'USB'; IsSystem = $false; IsBoot = $false; HasPageFile = $false
        Size = [int64]32e9; LogicalSectorSize = 512; SerialNumber = '0401 aa0012bc34'; Partitions = @([pscustomobject]@{ DriveLetter = 'E' }) }
    function Variant([hashtable]$Change) { $c = $good.PSObject.Copy(); foreach ($k in $Change.Keys) { $c.$k = $Change[$k] }; return $c }
    $img = [int64]1500 * $MiB
    Expect ((Get-DiskRefusal $good $img $false 'C') -eq '') 'a 32 GB USB stick is accepted'
    Expect ((Get-DiskRefusal (Variant @{ IsSystem = $true }) $img $false 'C') -like '*system disk*') 'the system disk is refused'
    Expect ((Get-DiskRefusal (Variant @{ IsBoot = $true }) $img $false 'C') -like '*boot disk*') 'the boot disk is refused'
    Expect ((Get-DiskRefusal (Variant @{ HasPageFile = $true }) $img $false 'C') -like '*page file*') 'a disk holding a page file is refused'
    Expect ((Get-DiskRefusal $good $img $false 'E') -like '*image itself*') 'the disk the image is on is refused'
    foreach ($bus in @('NVMe', 'SATA', 'SD', 'MMC', 'SAS', 'Virtual', '')) {
        Expect ((Get-DiskRefusal (Variant @{ BusType = $bus }) $img $false 'C') -like '*not USB*') ('bus ' + $bus + ' is refused')
    }
    $big = Variant @{ Size = [int64]300 * 1024 * 1024 * 1024 }
    Expect ((Get-DiskRefusal $big $img $false 'C') -like '*-AllowLarge*') 'a 300 GiB USB disk is refused'
    Expect ((Get-DiskRefusal $big $img $true 'C') -eq '') '... and accepted with -AllowLarge'
    Expect ((Get-DiskRefusal (Variant @{ Size = [int64]256 * 1024 * 1024 * 1024 }) $img $false 'C') -eq '') 'exactly 256 GiB is accepted'
    Expect ((Get-DiskRefusal (Variant @{ Size = $img }) $img $false 'C') -like '*needs*') 'a disk the image fills to the last MiB is refused'
    Expect ((Get-DiskRefusal (Variant @{ LogicalSectorSize = 4096 }) $img $false 'C') -like '*4096-byte*') '4K sectors are refused'
    Expect ((Get-DiskRefusal (Variant @{ IsSystem = $true; BusType = 'USB' }) $img $true 'C') -like '*system disk*') 'a system disk on USB (Windows To Go) is refused even with -AllowLarge'

    Write-Host '--- confirmation token, sidecar ---'
    Expect ((Get-ConfirmToken $good) -ceq 'ERASE 3 BC34') 'token is ERASE, the number and the serial tail'
    Expect ((Get-ConfirmToken (Variant @{ SerialNumber = '' })) -ceq ('ERASE 3 ' + [math]::Floor(32e9 / $MiB))) 'without a serial the tail is the size in MiB'
    $sc = Join-Path $work 'x.sha256'
    Set-Content -LiteralPath $sc -Value ('ab' * 32 + ' *sonora.iso') -Encoding Ascii
    Expect ((Read-Sidecar $sc) -ceq ('AB' * 32)) 'sidecar with a binary marker (Git Bash sha256sum) is read'
    Set-Content -LiteralPath $sc -Value ('cd' * 32 + '  sonora.iso') -Encoding Ascii
    Expect ((Read-Sidecar $sc) -ceq ('CD' * 32)) 'sidecar in text mode is read'
    Set-Content -LiteralPath $sc -Value 'not a hash' -Encoding Ascii
    Expect ((Read-Sidecar $sc) -eq '') 'a sidecar without a hash reads as empty'

    Write-Host '--- the whole flow against fake disks (regular files) ---'
    # 70 MiB plus 2 KiB: not a MiB multiple, so the last chunk is short and
    # the tail range does not start on the head's boundary.
    $imageBytes = [int64]70 * $MiB + 2048
    $image = Join-Path $work 'sonora-stick.img'
    New-FilledFile $image $imageBytes 7
    $imageHash = Get-Sha $image
    Set-Content -LiteralPath ($image + '.sha256') -Value ($imageHash.ToLowerInvariant() + ' *sonora-stick.img') -Encoding Ascii

    $files = @{}
    foreach ($n in 1, 2, 3, 4, 5) { $files[$n] = Join-Path $work ('disk' + $n + '.bin') }
    foreach ($n in 1, 2, 4, 5) { New-FilledFile $files[$n] (4 * $MiB) (-1) }
    $stickBytes = [int64]128 * $MiB
    New-FilledFile $files[3] $stickBytes (-1)
    $before = @{}
    foreach ($n in 1, 2, 3, 4, 5) { $before[$n] = Get-Sha $files[$n] }

    function FakeDisk($n, $bus, $size, $serial, $extra) {
        $d = [ordered]@{ Number = $n; FriendlyName = ('Fake ' + $bus + ' ' + $n); SerialNumber = $serial; Size = $size
            BusType = $bus; IsSystem = $false; IsBoot = $false; HasPageFile = $false; LogicalSectorSize = 512
            UniqueId = ('fake-' + $n); Path = $files[$n]
            Partitions = @([ordered]@{ Number = 1; DriveLetter = ''; Size = $size - $MiB; Type = 'Basic'; AccessPaths = @() }) }
        foreach ($k in $extra.Keys) { $d[$k] = $extra[$k] }
        return $d
    }
    $json = Join-Path $work 'disks.json'
    $fake = @(
        (FakeDisk 1 'NVMe' ([int64]512e9) 'SYS0001' @{ IsSystem = $true; IsBoot = $true; HasPageFile = $true }),
        (FakeDisk 2 'USB' ([int64]2e12) 'BACKUP9999' @{}),
        (FakeDisk 3 'USB' $stickBytes 'AA0012BC34' @{}),
        (FakeDisk 4 'SATA' ([int64]64e9) 'SATA4444' @{}),
        (FakeDisk 5 'USB' ([int64]64e9) 'PAGE5555' @{ HasPageFile = $true })
    )
    ConvertTo-Json -InputObject $fake -Depth 5 | Set-Content -LiteralPath $json -Encoding Ascii
    $token = 'ERASE 3 BC34'
    $common = @('-Image', $image, '-FakeDisks', $json)

    $r = Invoke-Writer ($common + @('-List'))
    Expect ($r.Code -eq 0 -and $r.Text -match '3 .*can be written' -and $r.Text -match '2 other disks not shown') '-List shows only the USB disks, and says which may be written'
    Expect ($r.Text -match 'matches sonora-stick.img.sha256') 'the image is checked against its sidecar'

    $cases = @(
        @(@('-DiskNumber', '1', '-Confirm', 'ERASE 1 0001'), 'system disk', 1),
        @(@('-DiskNumber', '4', '-Confirm', 'ERASE 4 4444'), 'not USB', 4),
        @(@('-DiskNumber', '2', '-Confirm', 'ERASE 2 9999'), 'AllowLarge', 2),
        @(@('-DiskNumber', '5', '-Confirm', 'ERASE 5 5555'), 'page file', 5),
        @(@('-DiskNumber', '9', '-Confirm', 'ERASE 9 0000'), 'no disk 9', 3),
        @(@('-DiskNumber', '3', '-Confirm', 'ERASE 3 WRONG'), 'does not match', 3),
        @(@('-DiskNumber', '3', '-Confirm', 'erase 3 bc34'), 'does not match', 3),
        @(@('-DiskNumber', '3'), 'not interactive', 3),
        @(@(), 'nobody to ask', 3)
    )
    foreach ($c in $cases) {
        $r = Invoke-Writer ($common + $c[0])
        Expect ($r.Code -eq 2 -and $r.Text -like ('*' + $c[1] + '*')) ('refused, exit 2, "' + $c[1] + '": ' + ($c[0] -join ' '))
    }
    $r = Invoke-Writer ($common + @('-DiskNumber', '2', '-Confirm', 'ERASE 2 9999', '-AllowLarge', '-DryRun'))
    Expect ($r.Code -eq 0 -and $r.Text -like '*dry run*') '-AllowLarge lets the 2 TB USB disk through (dry run)'

    $r = Invoke-Writer ($common + @('-DiskNumber', '3', '-Confirm', $token, '-DryRun'))
    Expect ($r.Code -eq 0 -and $r.Text -like '*dry run: would write*' -and $r.Text -like '*WILL BE ERASED*') '-DryRun goes through the checks and the warning'
    $r = Invoke-Writer ($common + @('-DiskNumber', '3', '-Confirm', $token, '-WhatIf'))
    Expect ($r.Code -eq 0 -and $r.Text -like '*dry run: would write*') '-WhatIf is the same dry run'

    Set-Content -LiteralPath ($image + '.sha256') -Value (('0' * 64) + ' *sonora-stick.img') -Encoding Ascii
    $r = Invoke-Writer ($common + @('-DiskNumber', '3', '-Confirm', $token))
    Expect ($r.Code -eq 2 -and $r.Text -like '*does not match*sha256*') 'an image that does not match its sidecar is refused'
    Set-Content -LiteralPath ($image + '.sha256') -Value ($imageHash + '  sonora-stick.img') -Encoding Ascii

    $changed = @(1, 2, 3, 4, 5 | Where-Object { (Get-Sha $files[$_]) -ne $before[$_] })
    Expect ($changed.Count -eq 0) ('no refusal or dry run changed a byte of any fake disk' + $(if ($changed.Count) { ' (changed: ' + ($changed -join ',') + ')' } else { '' }))

    $r = Invoke-Writer ($common + @('-DiskNumber', '3', '-Confirm', $token))
    Expect ($r.Code -eq 0 -and $r.Text -like '*Done: disk 3*') 'the write to fake disk 3 succeeds'
    Expect (([regex]::Matches($r.Text, 'read back ok')).Count -eq 3) 'the read-back checks the head, the tail and the zeroed last MiB'
    Expect ((Get-Item -LiteralPath $files[3]).Length -eq $stickBytes) 'the fake disk keeps its size'
    $h = [SonoraUsb.Native]::Open($files[3], 'file', $false, $true)
    try {
        $whole = [SonoraUsb.Native]::HashRange($h, 0, $imageBytes)
        $gap = [SonoraUsb.Native]::HashRange($h, 72 * $MiB, $MiB)
        $last = [SonoraUsb.Native]::HashRange($h, $stickBytes - $MiB, $MiB)
    } finally { $h.Dispose() }
    $zero = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash((New-Object byte[] $MiB))).Replace('-', '')
    Expect ($whole -eq $imageHash) 'every byte of the image is on the fake disk'
    Expect ($last -eq $zero) 'the last MiB of the fake disk is zero'
    Expect ($gap -ne $zero) 'between the image and the last MiB the old contents are left as they were'
    foreach ($n in 1, 2, 4, 5) { Expect ((Get-Sha $files[$n]) -eq $before[$n]) ('fake disk ' + $n + ' is untouched') }

    Write-Host '--- read-back catches a bad write ---'
    $disk = [pscustomobject]@{ Path = $files[3]; Kind = 'file'; Size = $stickBytes }
    Expect (Test-WrittenDisk $disk $image $imageBytes) 'the written fake disk verifies'
    foreach ($at in @([int64]5 * $MiB, $imageBytes - 100, $stickBytes - 10)) {
        $fs = [IO.File]::Open($files[3], 'Open', 'ReadWrite')
        try { $fs.Position = $at; $b = $fs.ReadByte(); $fs.Position = $at; $fs.WriteByte(($b -bxor 0xFF)) } finally { $fs.Dispose() }
        Expect (-not (Test-WrittenDisk $disk $image $imageBytes)) ('one flipped byte at ' + $at + ' fails verification')
        $fs = [IO.File]::Open($files[3], 'Open', 'ReadWrite')
        try { $fs.Position = $at; $fs.WriteByte($b) } finally { $fs.Dispose() }
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($script:Failed -ne 0) { Write-Host 'WRITE-USB-PS TEST FAIL'; exit 1 }
Write-Host 'WRITE-USB-PS TEST PASS (fake disks only; no real disk was opened)'
exit 0
