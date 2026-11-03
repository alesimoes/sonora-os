# Tests for tools/write-usb.ps1 (Windows PowerShell 5.1, no Pester).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-write-usb.ps1
#
# Nothing here touches a real disk. Every run of the writer passes -FakeDisks,
# which replaces the machine's disks with descriptions whose Path is a regular
# file in a scratch folder; the writer's "file" kind accepts only a drive
# letter path, and only a handle that is a regular file. The device-path
# checks below call only the path validators, which reject a bad path before
# CreateFileW is reached.
#
# Needs no elevation and about 300 MB of space in %TEMP%, removed at the end.
# Runs in CI's Windows job. Exit code 0 when every check passes.

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$writer = Join-Path $PSScriptRoot 'write-usb.ps1'
. $writer

$script:Failed = 0
function Ok([string]$What) { Write-Host ('WRITE-USB-PS ok    ' + $What) }
function Bad([string]$What) { Write-Host ('WRITE-USB-PS FAIL  ' + $What); $script:Failed = 1 }
function Expect([bool]$Cond, [string]$What) { if ($Cond) { Ok $What } else { Bad $What } }

$MiB = [int64]1048576
$work = Join-Path ([IO.Path]::GetTempPath()) ('sonora-write-usb-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

# A negative seed means the 0xAB pattern: a stick's old contents.
function New-FilledFile([string]$Path, [int64]$Bytes, [int]$Seed) {
    $fs = [IO.File]::Create($Path)
    try {
        $buf = [byte[]](@(0xAB) * 1048576)
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

function Part($Number, $Size, $Letter, $Fs, $Used, $Extra) {
    $p = [ordered]@{ Number = $Number; DriveLetter = $Letter; Size = [int64]$Size; Type = 'Basic'; AccessPaths = @()
        FileSystem = $Fs; Label = ''; UsedBytes = [int64]$Used; BitLocker = $false; HasWindows = $false; IsSystemPartition = $false }
    foreach ($k in $Extra.Keys) { $p[$k] = $Extra[$k] }
    return [pscustomobject]$p
}

function Set-U32([byte[]]$B, [int]$At, [int64]$V) {
    $x = [BitConverter]::GetBytes([uint32]$V)
    [Array]::Copy($x, 0, $B, $At, 4)
}

# The first MiB of a stick tools/make-stick-image.sh writes: xorriso's MBR
# (entry 1 type 0 at 64, entry 2 EF at 540 for 6656), write-usb.sh's entry 3
# (83 at the MiB after the ISO), and the ISO's volume descriptor at 32768.
function New-SonoraHead([int64]$Size1, [string]$Label) {
    $b = New-Object byte[] 1048576
    $b[510] = 0x55; $b[511] = 0xAA
    $b[446] = 0x80
    Set-U32 $b 454 64; Set-U32 $b 458 $Size1
    $b[466] = 0xEF; Set-U32 $b 470 540; Set-U32 $b 474 6656
    $start3 = [int64]([math]::Ceiling((64 + $Size1) / 2048)) * 2048
    $b[482] = 0x83; Set-U32 $b 486 $start3; Set-U32 $b 490 100000
    $b[32768] = 1
    $ascii = [Text.Encoding]::ASCII
    [Array]::Copy($ascii.GetBytes('CD001'), 0, $b, 32769, 5)
    [Array]::Copy($ascii.GetBytes($Label.PadRight(32)), 0, $b, 32808, 32)
    return , $b
}

# The first 4 KiB of an ext4 partition: magic at 1024+56, label at 1024+120.
function New-Ext4Block([string]$Label) {
    $b = New-Object byte[] 4096
    $b[1080] = 0x53; $b[1081] = 0xEF
    $l = [Text.Encoding]::ASCII.GetBytes($Label)
    [Array]::Copy($l, 0, $b, 1144, $l.Length)
    return , $b
}

function Write-Bytes([string]$Path, [int64]$Offset, [byte[]]$Bytes) {
    $fs = [IO.File]::Open($Path, 'Open', 'ReadWrite')
    try { $fs.Position = $Offset; $fs.Write($Bytes, 0, $Bytes.Length) } finally { $fs.Dispose() }
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
    $existing = Join-Path $work 'exists.bin'
    New-FilledFile $existing $MiB (-1)
    $fwd = $existing.Replace([string][char]92, '/')
    $qm = [string][char]92 + [string][char]92 + '?' + [string][char]92 + $existing
    foreach ($case in @(@($one, 'device'), @('C:\PhysicalDrive3', 'device'), @($p, 'file'), @($p, 'volume'),
                        @($fwd, 'file'), @($qm, 'file'), @('exists.bin', 'file'), @($work, 'file'))) {
        $threw = $false
        try { [SonoraUsb.Native]::Open($case[0], $case[1], $false, $false).Dispose() } catch [ArgumentException] { $threw = $true } catch { $threw = $_.Exception.InnerException -is [ArgumentException] }
        Expect $threw ('Native.Open refuses ' + $case[0] + ' as kind ' + $case[1] + ' before opening anything')
    }
    $fh = [SonoraUsb.Native]::Open($existing, 'file', $false, $false)
    try { Expect ([SonoraUsb.Native]::FileLength($fh) -eq $MiB) 'a regular file on a drive letter opens as kind file' } finally { $fh.Dispose() }

    Write-Host '--- refusals ---'
    $good = [pscustomobject]@{ Number = 3; BusType = 'USB'; IsSystem = $false; IsBoot = $false; HasPageFile = $false; FriendlyName = 'Kingston DataTraveler 3.0'
        Size = [int64]32e9; LogicalSectorSize = 512; SerialNumber = '0401 aa0012bc34'; Partitions = @([pscustomobject]@{ Number = 1; DriveLetter = 'E'; Size = [int64]31e9 })
        PartitionQueryFailed = $false; Head = $null; P3Block = $null }
    function Variant([hashtable]$Change) { $c = $good.PSObject.Copy(); foreach ($k in $Change.Keys) { $c.$k = $Change[$k] }; return $c }
    $img = [int64]1500 * $MiB
    Expect ((Get-DiskRefusal $good $img $false 'C') -eq '') 'a 32 GB USB stick is accepted'
    Expect ((Get-DiskRefusal (Variant @{ IsSystem = $true }) $img $false 'C') -like '*system disk*') 'the system disk is refused'
    Expect ((Get-DiskRefusal (Variant @{ IsBoot = $true }) $img $false 'C') -like '*boot disk*') 'the boot disk is refused'
    Expect ((Get-DiskRefusal (Variant @{ HasPageFile = $true }) $img $false 'C') -like '*page file*') 'a disk holding a page file is refused'
    Expect ((Get-DiskRefusal $good $img $false 'E') -like '*image itself*') 'the disk the image is on is refused'
    Expect ((Get-DiskRefusal (Variant @{ Partitions = @((Part 1 31e9 'E' 'NTFS' 1e9 @{ BitLocker = $true })) }) $img $true 'C') -like '*BitLocker*') 'a BitLocker volume is refused, even with -AllowLarge'
    Expect ((Get-DiskRefusal (Variant @{ Partitions = @((Part 1 31e9 'E' 'NTFS' 9e9 @{ HasWindows = $true })) }) $img $false 'C') -like '*Windows installation*') 'a disk with a Windows install or boot loader is refused'
    Expect ((Get-DiskRefusal (Variant @{ Partitions = @((Part 1 100e6 '' 'FAT32' 1e6 @{ IsSystemPartition = $true })) }) $img $false 'C') -like '*system or boot partition*') 'a system or boot partition is refused'
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

    Write-Host '--- data volumes ---'
    $ntfs = Variant @{ Partitions = @((Part 1 250e9 'E' 'NTFS' 200e9 @{ Label = 'Recordings' })) }
    $dp = @(Get-DataPartitions $ntfs)
    Expect ($dp.Count -eq 1) 'an NTFS volume with files is data'
    Expect ((Format-Partition $dp[0]) -like '*E:*NTFS*"Recordings"*200000 MB used*') 'the data line names the letter, filesystem, label and used space'
    Expect ((Get-DataToken $dp) -ceq 'ERASE DATA E') 'the second token names the drive letter'
    Expect (@(Get-DataPartitions (Variant @{ Partitions = @((Part 1 31e9 'F' 'FAT32' 0 @{})) })).Count -eq 1) 'a factory FAT32 volume with a letter counts as data too'
    $noletter = @(Get-DataPartitions (Variant @{ Partitions = @((Part 1 16e6 '' '' 0 @{}), (Part 2 31e9 '' '' 0 @{})) }))
    Expect ($noletter.Count -eq 1 -and (Get-DataToken $noletter) -ceq 'ERASE DATA P2') 'a large unreadable partition is data, a 16 MB reserved one is not'
    Expect (@(Get-DataPartitions (Variant @{ Partitions = @(); Head = (New-Object byte[] $MiB) })).Count -eq 0) 'no partitions and a zero first MiB: blank, no data'
    $raw = New-Object byte[] $MiB
    $raw[1080] = 0x53
    $rawItems = @(Get-DataPartitions (Variant @{ Partitions = @(); Head = $raw }))
    Expect ($rawItems.Count -eq 1 -and (Get-DataToken $rawItems) -ceq 'ERASE DATA DISK') 'no partitions but a non-zero first MiB (a filesystem on the whole device): data, ERASE DATA DISK'
    Expect (@(Get-DataPartitions (Variant @{ Partitions = @() })).Count -eq 1) 'no partitions and a first MiB that could not be read: data'
    $failedList = @(Get-DataPartitions (Variant @{ Partitions = @(); PartitionQueryFailed = $true; Head = (New-Object byte[] $MiB) }))
    Expect ($failedList.Count -eq 1 -and (Format-Partition $failedList[0]) -like '*could not list*') 'partitions that could not be listed: data, even over a zero first MiB'
    Expect (@(Get-DataPartitions (Variant @{ Partitions = @((Part 1 16e6 '' '' 0 @{ VolumeQueryFailed = $true })) })).Count -eq 1) 'a partition whose volume could not be queried is data, however small'

    Write-Host '--- a previous SonoraOS stick, by its bytes ---'
    $sHead = New-SonoraHead 8128 'SONORA'
    $sP3 = New-Ext4Block 'persistence'
    Expect (Test-SonoraBytes $sHead $sP3 ([int64]64e9)) 'our MBR, ISO label SONORA and ext4 label persistence: recognised'
    Expect (-not (Test-SonoraBytes (New-SonoraHead 8128 'DEBIAN_LIVE') $sP3 ([int64]64e9))) 'another live ISO label (DEBIAN_LIVE) is not ours'
    Expect (-not (Test-SonoraBytes $sHead (New-Ext4Block 'home-rw') ([int64]64e9))) 'another ext4 label is not ours'
    Expect (-not (Test-SonoraBytes $sHead $null ([int64]64e9))) 'an unreadable partition 3 is not ours'
    $odd = New-SonoraHead 8128 'SONORA'
    $odd[494] = 0x83
    Expect (-not (Test-SonoraBytes $odd $sP3 ([int64]64e9))) 'a fourth MBR entry is not ours'
    $winView = @((Part 2 3407872 'R' 'FAT' 3e6 @{}), (Part 3 31e9 '' '' 0 @{}))
    $sonora = Variant @{ Partitions = $winView; Head = $sHead; P3Block = $sP3 }
    Expect ((Test-SonoraLayout $sonora) -and @(Get-DataPartitions $sonora).Count -eq 0) 'a previous SonoraOS stick is not a data disk'
    $ntfsP1 = Variant @{ Partitions = @((Part 1 909e6 'E' 'NTFS' 5e8 @{}), (Part 2 3407872 'R' 'FAT' 3e6 @{}), (Part 3 31e9 '' '' 0 @{})); Head = $sHead; P3Block = $sP3 }
    Expect (-not (Test-SonoraLayout $ntfsP1) -and @(Get-DataPartitions $ntfsP1).Count -ge 1) 'NTFS on partition 1 with a small 2 and an unreadable 3: data, whatever the bytes say'
    $debian = Variant @{ Partitions = $winView; Head = (New-SonoraHead 8128 'DEBIAN_LIVE'); P3Block = $sP3 }
    Expect (-not (Test-SonoraLayout $debian) -and @(Get-DataPartitions $debian).Count -ge 1) 'a Debian-live-like stick with persistence: data'
    $lookalike = Variant @{ Partitions = $winView; Head = $null; P3Block = $null }
    Expect (-not (Test-SonoraLayout $lookalike) -and @(Get-DataPartitions $lookalike).Count -ge 1) 'the same partition sizes without our bytes: data'

    Write-Host '--- confirmation tokens, sidecar ---'
    Expect ((Get-ConfirmToken $good) -ceq 'ERASE 3 BC34') 'token is ERASE, the number and the serial tail'
    $noSerial = Variant @{ SerialNumber = '' }
    Expect ((Get-ConfirmToken $noSerial) -ceq 'ERASE 3 KINGSTONDATATRAVELER30 32000000000') 'without a serial the token is the model and the exact size in bytes'
    Expect (-not (Test-HasSerial $noSerial) -and (Test-HasSerial $good)) 'Test-HasSerial tells them apart'
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
    $nums = @(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13)
    foreach ($n in $nums) { $files[$n] = Join-Path $work ('disk' + $n + '.bin') }
    $stickBytes = [int64]128 * $MiB
    foreach ($n in $nums) {
        if ($n -eq 3 -or $n -eq 6) { New-FilledFile $files[$n] $stickBytes (-1) } else { New-FilledFile $files[$n] (8 * $MiB) (-1) }
    }
    # Disk 3 is a blank stick: no partitions and a zero first MiB.
    Write-Bytes $files[3] 0 (New-Object byte[] $MiB)
    # 9 carries our image's bytes, 11 another live ISO's label, 12 our bytes
    # under a Windows view with NTFS on partition 1.
    foreach ($n in @(9, 12)) { Write-Bytes $files[$n] 0 $sHead; Write-Bytes $files[$n] (4 * $MiB) $sP3 }
    Write-Bytes $files[11] 0 (New-SonoraHead 8128 'DEBIAN_LIVE')
    Write-Bytes $files[11] (4 * $MiB) $sP3
    $before = @{}
    foreach ($n in $nums) { $before[$n] = Get-Sha $files[$n] }

    function FakeDisk($n, $bus, $size, $serial, $extra, $parts) {
        $d = [ordered]@{ Number = $n; FriendlyName = ('Fake ' + $bus + ' ' + $n); SerialNumber = $serial; Size = $size
            BusType = $bus; IsSystem = $false; IsBoot = $false; HasPageFile = $false; LogicalSectorSize = 512
            UniqueId = ('fake-' + $n); Path = $files[$n]; Partitions = @($parts) }
        foreach ($k in $extra.Keys) { $d[$k] = $extra[$k] }
        return $d
    }
    $blank = @()
    $json = Join-Path $work 'disks.json'
    $fake = @(
        (FakeDisk 1 'NVMe' ([int64]512e9) 'SYS0001' @{ IsSystem = $true; IsBoot = $true; HasPageFile = $true } @((Part 1 511e9 'C' 'NTFS' 100e9 @{}))),
        (FakeDisk 2 'USB' ([int64]2e12) 'BACKUP9999' @{} $blank),
        (FakeDisk 3 'USB' $stickBytes 'AA0012BC34' @{} $blank),
        (FakeDisk 4 'SATA' ([int64]64e9) 'SATA4444' @{} $blank),
        (FakeDisk 5 'USB' ([int64]64e9) 'PAGE5555' @{ HasPageFile = $true } $blank),
        (FakeDisk 6 'USB' $stickBytes 'DATA6666' @{} @((Part 1 (100 * $MiB) 'Q' 'NTFS' (50 * $MiB) @{ Label = 'Takes' }))),
        (FakeDisk 7 'USB' ([int64]64e9) 'LOCK7777' @{} @((Part 1 63e9 'K' 'NTFS' 1e9 @{ BitLocker = $true }))),
        (FakeDisk 8 'USB' ([int64]64e9) 'WIND8888' @{} @((Part 1 63e9 'W' 'NTFS' 20e9 @{ HasWindows = $true }))),
        (FakeDisk 9 'USB' ([int64]64e9) 'SONO9999' @{} @((Part 1 909e6 '' '' 0 @{}), (Part 2 3407872 'R' 'FAT' 3e6 @{}), (Part 3 60e9 '' '' 0 @{}))),
        (FakeDisk 10 'USB' ([int64]64e9) '' @{} $blank),
        (FakeDisk 11 'USB' ([int64]64e9) 'DEBI1111' @{} @((Part 1 909e6 '' '' 0 @{}), (Part 2 3407872 'R' 'FAT' 3e6 @{}), (Part 3 60e9 '' '' 0 @{}))),
        (FakeDisk 12 'USB' ([int64]64e9) 'NTFS1212' @{} @((Part 1 909e6 'E' 'NTFS' 5e8 @{}), (Part 2 3407872 'R' 'FAT' 3e6 @{}), (Part 3 60e9 '' '' 0 @{}))),
        (FakeDisk 13 'USB' ([int64]64e9) 'FAIL1313' @{ PartitionQueryFailed = $true } $blank)
    )
    ConvertTo-Json -InputObject $fake -Depth 6 | Set-Content -LiteralPath $json -Encoding Ascii
    $right = 'ERASE 3 BC34'
    $common = @('-Image', $image, '-FakeDisks', $json)

    $r = Invoke-Writer ($common + @('-List'))
    Expect ($r.Code -eq 0 -and $r.Text -match '(?m)^  3 .*can be written' -and $r.Text -match '2 other disks not shown') '-List shows only the USB disks, and says which may be written'
    Expect ($r.Text -match '(?m)^  6 .*has data: partition 1 \(Q:\)  NTFS "Takes"') '-List marks the disk with an NTFS volume as having data'
    Expect ($r.Text -match '(?m)^  9 .*can be written') '-List shows a previous SonoraOS stick (our bytes) as writable'
    Expect ($r.Text -match '(?m)^  11 .*has data') '-List marks a Debian-live-like stick with persistence as having data'
    Expect ($r.Text -match '(?m)^  12 .*has data: partition 1 \(E:\)  NTFS') '-List marks our layout with NTFS on partition 1 as having data'
    Expect ($r.Text -match '(?m)^  13 .*has data: the whole disk: Windows could not list') '-List marks a disk whose partitions could not be listed as having data'
    Expect ($r.Text -match '(?m)^  10 .*has data: the whole disk: no partitions') '-List marks a disk with no partitions but old contents as having data'
    Expect ($r.Text -match 'matches sonora-stick.img.sha256') 'the image is checked against its sidecar'

    $cases = @(
        @(@('-DiskNumber', '1', '-Token', 'ERASE 1 0001'), 'system disk'),
        @(@('-DiskNumber', '4', '-Token', 'ERASE 4 4444'), 'not USB'),
        @(@('-DiskNumber', '2', '-Token', 'ERASE 2 9999'), 'AllowLarge'),
        @(@('-DiskNumber', '5', '-Token', 'ERASE 5 5555'), 'page file'),
        @(@('-DiskNumber', '7', '-Token', 'ERASE 7 7777', '-AllowDataVolumes'), 'BitLocker'),
        @(@('-DiskNumber', '8', '-Token', 'ERASE 8 8888', '-AllowDataVolumes'), 'Windows installation'),
        @(@('-DiskNumber', '6', '-Token', 'ERASE 6 6666'), 'holds data; pass -AllowDataVolumes'),
        @(@('-DiskNumber', '11', '-Token', 'ERASE 11 1111'), 'holds data; pass -AllowDataVolumes'),
        @(@('-DiskNumber', '12', '-Token', 'ERASE 12 1212'), 'holds data; pass -AllowDataVolumes'),
        @(@('-DiskNumber', '13', '-Token', 'ERASE 13 1313'), 'holds data; pass -AllowDataVolumes'),
        @(@('-DiskNumber', '10', '-Token', 'ERASE 10 FAKEUSB10 64000000000'), 'reports no serial'),
        @(@('-DiskNumber', '99', '-Token', 'ERASE 99 0000'), 'no disk 99'),
        @(@('-DiskNumber', '3', '-Token', 'ERASE 3 WRONG'), 'does not match'),
        @(@('-DiskNumber', '3', '-Token', 'erase 3 bc34'), 'does not match'),
        @(@('-DiskNumber', '3'), 'not interactive'),
        @(@(), 'nobody to ask')
    )
    foreach ($c in $cases) {
        $r = Invoke-Writer ($common + $c[0])
        Expect ($r.Code -eq 2 -and $r.Text -like ('*' + $c[1] + '*')) ('refused, exit 2, "' + $c[1] + '": ' + ($c[0] -join ' '))
    }

    # A misspelt or unsupported switch, with the right token: the run must
    # stop before it describes, let alone writes, anything.
    foreach ($typo in @(@('-DryRn'), @('-WhatIf'), @('-Confirm', $right), @('3'))) {
        $r = Invoke-Writer ($common + @('-DiskNumber', '3', '-Token', $right) + $typo)
        Expect ($r.Text -notlike '*Target:*' -and $r.Text -notlike '*WILL BE ERASED*' -and $r.Text -notlike '*Writing*' -and (Get-Sha $files[3]) -eq $before[3]) `
            ('an unknown argument (' + ($typo -join ' ') + ') stops the run before anything (exit ' + $r.Code + ')')
    }

    $r = Invoke-Writer ($common + @('-DiskNumber', '3', '-DryRun'))
    $iDry = $r.Text.IndexOf('DRY RUN: nothing will be written')
    $iTarget = $r.Text.IndexOf('Target:')
    Expect ($r.Code -eq 0 -and $iDry -ge 0 -and $iTarget -gt $iDry) '-DryRun says DRY RUN before it shows the target'
    Expect ($r.Text -like ('*would ask you to type: ' + $right + '*') -and $r.Text -notlike '*WILL BE ERASED*' -and $r.Text -notlike '*Writing*') '-DryRun asks for no token, never says WILL BE ERASED, never reaches the write'
    $r = Invoke-Writer ($common + @('-DiskNumber', '6', '-DryRun'))
    Expect ($r.Code -eq 0 -and $r.Text -like '*THIS DISK HOLDS DATA*' -and $r.Text -like '*ERASE DATA Q*') 'a dry run on a data disk lists the volume and the second token'
    $r = Invoke-Writer ($common + @('-DiskNumber', '2', '-AllowLarge', '-DryRun'))
    Expect ($r.Code -eq 0 -and $r.Text -like '*DRY RUN: nothing was written*') '-AllowLarge lets the 2 TB USB disk through (dry run)'
    $r = Invoke-Writer ($common + @('-DiskNumber', '9', '-DryRun'))
    Expect ($r.Code -eq 0 -and $r.Text -like '*previous SonoraOS stick*' -and $r.Text -notlike '*ERASE DATA*') 'a previous SonoraOS stick needs only the one token'

    Set-Content -LiteralPath ($image + '.sha256') -Value (('0' * 64) + ' *sonora-stick.img') -Encoding Ascii
    $r = Invoke-Writer ($common + @('-DiskNumber', '3', '-Token', $right))
    Expect ($r.Code -eq 2 -and $r.Text -like '*does not match*sha256*') 'an image that does not match its sidecar is refused'
    Set-Content -LiteralPath ($image + '.sha256') -Value ($imageHash + '  sonora-stick.img') -Encoding Ascii

    $changed = @($nums | Where-Object { (Get-Sha $files[$_]) -ne $before[$_] })
    Expect ($changed.Count -eq 0) ('no refusal or dry run changed a byte of any fake disk (changed: ' + ($changed -join ',') + ')')

    $r = Invoke-Writer ($common + @('-DiskNumber', '3', '-Token', $right))
    Expect ($r.Code -eq 0 -and $r.Text -like '*Done: disk 3*') 'the write to fake disk 3 succeeds'
    Expect (([regex]::Matches($r.Text, 'read back ok')).Count -eq 3) 'the read-back checks the head, the tail and the zeroed last MiB'
    $r6 = Invoke-Writer ($common + @('-DiskNumber', '6', '-Token', 'ERASE 6 6666', '-AllowDataVolumes'))
    Expect ($r6.Code -eq 0 -and $r6.Text -like '*-AllowDataVolumes: the partitions listed above*' -and $r6.Text -like '*NTFS "Takes"*') '-AllowDataVolumes writes the data disk and still lists what it erases'
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
    foreach ($n in @(1, 2, 4, 5, 7, 8, 9, 10, 11, 12, 13)) { Expect ((Get-Sha $files[$n]) -eq $before[$n]) ('fake disk ' + $n + ' is untouched') }

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
