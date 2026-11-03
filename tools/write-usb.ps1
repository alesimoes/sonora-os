# Write the SonoraOS image to a USB stick from Windows (Windows PowerShell 5.1).
#
# THIS ERASES THE WHOLE TARGET DISK. Run from an elevated PowerShell:
#
#   powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -List
#   powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -DiskNumber 3
#   powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -DiskNumber 3 -DryRun
#   powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -SelfTest
#
# What it writes: iso\sonora-stick.img when it exists (the ISO plus the
# persistence partition, made by tools/make-stick-image.sh), else
# iso\sonora.iso, which boots but keeps nothing across a reboot. -Image picks
# another file.
#
# Why a raw write from Windows and not WSL: `wsl --mount` does not take USB
# sticks, and passing one through to WSL needs usbipd-win installed on the
# host. Windows itself can write a raw disk once every volume on it is locked
# and dismounted, which is what Rufus and Win32DiskImager do. The Linux-only
# part -- making the ext4 persistence partition -- is done in a container
# against a file, by tools/write-usb.sh itself, so this script only copies
# bytes and never partitions or formats anything.
#
# The previous Windows writer (removed 2026-10-12) opened the device with one
# leading backslash, which .NET resolved to C:\PhysicalDrive: it erased the
# stick and wrote nothing. Here the device path is built from character codes,
# checked character by character, and opened through CreateFileW directly,
# never through a .NET path API; tools/test-write-usb.ps1 asserts both.
#
# -FakeDisks FILE.json replaces the machine's disks with descriptions whose
# Path is a regular file. Every step but locking volumes runs against that
# file, which is how tools/test-write-usb.ps1 exercises the whole flow
# without a real disk anywhere near it.
#
# Exit codes: 0 done, 1 error, 2 refused (nothing was written).

param(
    [string]$Image = '',
    [int]$DiskNumber = -1,
    [string]$Confirm = '',
    [switch]$AllowLarge,
    [switch]$DryRun,
    [switch]$WhatIf,
    [switch]$List,
    [switch]$SelfTest,
    [string]$FakeDisks = ''
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

# 256 GiB rather than GB, so a stick sold as "256 GB" still passes; same
# limit as tools/write-usb.sh. Above it the disk is more likely a backup drive.
$script:MaxBytesWithoutOverride = [int64]256 * 1024 * 1024 * 1024
$script:MiB = [int64]1048576
$script:VerifyBytes = 64 * $script:MiB
$script:ChunkBytes = 4 * 1048576

if (-not ('SonoraUsb.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;

namespace SonoraUsb {
public static class Native {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa,
                                             uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize,
                                       IntPtr outBuf, uint outSize, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFilePointerEx(SafeFileHandle h, long distance, out long newPos, uint method);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(SafeFileHandle h, byte[] buf, uint count, out uint written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle h, IntPtr buf, uint count, out uint read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FlushFileBuffers(SafeFileHandle h);

    public const string DevicePrefix = @"\\.\PhysicalDrive";
    const string VolumePrefix = @"\\?\Volume{";

    static void Fail(string what) {
        int err = Marshal.GetLastWin32Error();
        throw new Exception(what + ": " + new Win32Exception(err).Message + " (Win32 error " + err + ")");
    }

    static bool AllDigits(string s) {
        if (s.Length == 0) return false;
        foreach (char c in s) if (c < '0' || c > '9') return false;
        return true;
    }

    // kind is "device", "volume" or "file". Each kind accepts only its own
    // path shape, so a file path can never reach the device branch or the
    // reverse, whatever the caller passed.
    public static SafeFileHandle Open(string path, string kind, bool write, bool unbuffered) {
        if (kind == "device") {
            if (!path.StartsWith(DevicePrefix, StringComparison.Ordinal)
                || !AllDigits(path.Substring(DevicePrefix.Length)))
                throw new ArgumentException("not a physical drive path: '" + path + "'");
        } else if (kind == "volume") {
            if (!path.StartsWith(VolumePrefix, StringComparison.Ordinal) || path.EndsWith("\\"))
                throw new ArgumentException("not a volume path: '" + path + "'");
        } else if (kind == "file") {
            if (path.StartsWith("\\\\", StringComparison.Ordinal) || !System.IO.File.Exists(path))
                throw new ArgumentException("not an existing regular file: '" + path + "'");
        } else {
            throw new ArgumentException("unknown kind " + kind);
        }
        uint access = 0x80000000u | (write ? 0x40000000u : 0u);          // GENERIC_READ | GENERIC_WRITE
        uint flags = (write ? 0x80000000u : 0u) | (unbuffered ? 0x20000000u : 0u); // WRITE_THROUGH, NO_BUFFERING
        SafeFileHandle h = CreateFileW(path, access, 3u, IntPtr.Zero, 3u, flags, IntPtr.Zero);
        if (h.IsInvalid) Fail("cannot open " + path);
        return h;
    }

    public static void LockAndDismount(SafeFileHandle h, string path) {
        uint r;
        if (!DeviceIoControl(h, 0x00090018u, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero))
            Fail("cannot lock " + path + " -- close every window and program using the stick");
        if (!DeviceIoControl(h, 0x00090020u, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero))
            Fail("cannot dismount " + path);
    }

    // IOCTL_DISK_UPDATE_PROPERTIES: re-read the partition table just written.
    public static bool UpdateProperties(SafeFileHandle h) {
        uint r;
        return DeviceIoControl(h, 0x00070140u, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);
    }

    static void Seek(SafeFileHandle h, long offset) {
        long pos;
        if (!SetFilePointerEx(h, offset, out pos, 0) || pos != offset) Fail("cannot seek to " + offset);
    }

    // A short write is a failure, never something to retry around.
    public static void WriteAt(SafeFileHandle h, long offset, byte[] buf, int count) {
        Seek(h, offset);
        uint written;
        if (!WriteFile(h, buf, (uint)count, out written, IntPtr.Zero)) Fail("write failed at byte " + offset);
        if (written != (uint)count)
            throw new Exception("short write at byte " + offset + ": " + written + " of " + count);
    }

    public static void Flush(SafeFileHandle h) {
        if (!FlushFileBuffers(h)) Fail("flush failed");
    }

    // SHA-256 of [offset, offset+length). Meant for a handle opened
    // unbuffered, so a read-back comes from the medium and not from a cache:
    // the buffer is page-aligned and every request a multiple of 4096 bytes.
    public static string HashRange(SafeFileHandle h, long offset, long length) {
        const int Block = 4 << 20;
        IntPtr raw = Marshal.AllocHGlobal(Block + 4096);
        try {
            IntPtr p = new IntPtr((raw.ToInt64() + 4095) & ~4095L);
            byte[] managed = new byte[Block];
            using (SHA256 sha = SHA256.Create()) {
                Seek(h, offset);
                long done = 0;
                while (done < length) {
                    long want = Math.Min((long)Block, length - done);
                    uint ask = (uint)((want + 4095) & ~4095L);
                    uint got;
                    if (!ReadFile(h, p, ask, out got, IntPtr.Zero)) Fail("read failed at byte " + (offset + done));
                    if (got < want)
                        throw new Exception("short read at byte " + (offset + done) + ": " + got + " of " + want);
                    Marshal.Copy(p, managed, 0, (int)want);
                    sha.TransformBlock(managed, 0, (int)want, null, 0);
                    done += want;
                }
                sha.TransformFinalBlock(managed, 0, 0);
                return BitConverter.ToString(sha.Hash).Replace("-", "");
            }
        } finally {
            Marshal.FreeHGlobal(raw);
        }
    }
}
}
'@
}

# --- decisions, free of side effects so tools/test-write-usb.ps1 can call them ---

# \\.\PhysicalDriveN, from character codes: no escaping layer between this
# file and CreateFileW can drop a backslash.
function Get-DevicePath([int]$Number) {
    $bs = [string][char]92
    return ($bs + $bs + '.' + $bs + 'PhysicalDrive' + $Number)
}

function Test-DevicePath([string]$Path) {
    if ($Path.Length -lt 18) { return $false }
    if ([int][char]$Path[0] -ne 92) { return $false }
    if ([int][char]$Path[1] -ne 92) { return $false }
    if ([int][char]$Path[2] -ne 46) { return $false }
    if ([int][char]$Path[3] -ne 92) { return $false }
    return ($Path.Substring(4) -cmatch '^PhysicalDrive[0-9]+$')
}

# Why this disk must not be written, or '' if it may be.
function Get-DiskRefusal($Disk, [int64]$ImageBytes, [bool]$AllowLarge, [string]$ImageDrive) {
    if ($Disk.IsSystem) { return 'it is the system disk (Windows runs from it).' }
    if ($Disk.IsBoot) { return 'it is the boot disk.' }
    if ($Disk.HasPageFile) { return 'it holds a page file.' }
    $letters = @($Disk.Partitions | ForEach-Object { [string]$_.DriveLetter } | Where-Object { $_ -ne '' })
    if ($ImageDrive -ne '' -and ($letters -contains $ImageDrive)) {
        return ('the image itself is on it (drive ' + $ImageDrive + ':).')
    }
    # USB only. Windows reports a laptop's built-in card reader as SD and its
    # soldered eMMC as SD or MMC too, so neither is trusted.
    if ([string]$Disk.BusType -ne 'USB') {
        return ('it is on the ' + $Disk.BusType + ' bus, not USB. This script writes USB sticks only.')
    }
    if ([int64]$Disk.Size -gt $script:MaxBytesWithoutOverride -and -not $AllowLarge) {
        return ('it is ' + [math]::Floor([int64]$Disk.Size / 1e9) + ' GB, larger than any stick this is meant for. If it really is the stick, pass -AllowLarge.')
    }
    if ([int64]$Disk.Size -lt ($ImageBytes + $script:MiB)) {
        return ('it is ' + [math]::Floor([int64]$Disk.Size / $script:MiB) + ' MiB; the image needs ' + [math]::Ceiling(($ImageBytes + $script:MiB) / $script:MiB) + ' MiB.')
    }
    # The image's partition table counts 512-byte sectors.
    if ([int]$Disk.LogicalSectorSize -ne 512) {
        return ('it has ' + $Disk.LogicalSectorSize + '-byte sectors; the image assumes 512.')
    }
    return ''
}

# What the operator types. The serial's tail ties the answer to the disk that
# was shown: if the number moves to another disk, the same answer no longer
# fits it.
function Get-ConfirmToken($Disk) {
    $alnum = ([string]$Disk.SerialNumber -replace '[^A-Za-z0-9]', '').ToUpperInvariant()
    if ($alnum.Length -ge 4) {
        $tail = $alnum.Substring($alnum.Length - 4)
    } else {
        $tail = [string][math]::Floor([int64]$Disk.Size / $script:MiB)
    }
    return ('ERASE ' + $Disk.Number + ' ' + $tail)
}

function Get-DiskIdentity($Disk) {
    return ([string]$Disk.Number + '|' + $Disk.UniqueId + '|' + $Disk.SerialNumber + '|' + $Disk.Size + '|' + $Disk.BusType)
}

# Sidecar in sha256sum's format: "<hex>  name" or "<hex> *name".
function Read-Sidecar([string]$Path) {
    $first = @(Get-Content -LiteralPath $Path -TotalCount 1)
    if ($first.Count -lt 1) { return '' }
    $hex = ($first[0] -split '\s+')[0]
    if ($hex -notmatch '^[0-9A-Fa-f]{64}$') { return '' }
    return $hex.ToUpperInvariant()
}

# --- disks ---

function Get-Disks {
    if ($FakeDisks -ne '') {
        $raw = Get-Content -LiteralPath $FakeDisks -Raw | ConvertFrom-Json
        $out = @()
        foreach ($d in @($raw)) {
            $out += [pscustomobject]@{
                Number = [int]$d.Number; FriendlyName = [string]$d.FriendlyName
                SerialNumber = [string]$d.SerialNumber; Size = [int64]$d.Size
                BusType = [string]$d.BusType; IsSystem = [bool]$d.IsSystem; IsBoot = [bool]$d.IsBoot
                HasPageFile = [bool]$d.HasPageFile; LogicalSectorSize = [int]$d.LogicalSectorSize
                UniqueId = [string]$d.UniqueId; Partitions = @($d.Partitions)
                Path = [string]$d.Path; Kind = 'file'
            }
        }
        return $out
    }
    $pageLetters = @(Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue |
        ForEach-Object { ([string]$_.Name).Substring(0, 1).ToUpperInvariant() })
    $out = @()
    foreach ($d in @(Get-Disk)) {
        $parts = @()
        foreach ($p in @(Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue)) {
            $letter = ''
            if ($p.DriveLetter -and [int][char]$p.DriveLetter -ne 0) { $letter = ([string]$p.DriveLetter).ToUpperInvariant() }
            $parts += [pscustomobject]@{
                Number = $p.PartitionNumber; DriveLetter = $letter; Size = [int64]$p.Size
                Type = [string]$p.Type; AccessPaths = @($p.AccessPaths)
            }
        }
        $hasPage = $false
        foreach ($p in $parts) { if ($p.DriveLetter -ne '' -and ($pageLetters -contains $p.DriveLetter)) { $hasPage = $true } }
        $out += [pscustomobject]@{
            Number = [int]$d.Number; FriendlyName = [string]$d.FriendlyName
            SerialNumber = ([string]$d.SerialNumber).Trim(); Size = [int64]$d.Size
            BusType = [string]$d.BusType; IsSystem = [bool]$d.IsSystem; IsBoot = [bool]$d.IsBoot
            HasPageFile = $hasPage; LogicalSectorSize = [int]$d.LogicalSectorSize
            UniqueId = [string]$d.UniqueId; Partitions = $parts
            Path = (Get-DevicePath $d.Number); Kind = 'device'
        }
    }
    return $out
}

function Show-Disk($Disk) {
    Write-Host ('  Disk ' + $Disk.Number + ': ' + $Disk.FriendlyName)
    Write-Host ('    size    ' + [math]::Round($Disk.Size / 1e9, 1) + ' GB (' + $Disk.Size + ' bytes)')
    Write-Host ('    serial  ' + $Disk.SerialNumber + '    bus ' + $Disk.BusType)
    foreach ($p in @($Disk.Partitions)) {
        $l = '-'
        if ([string]$p.DriveLetter -ne '') { $l = [string]$p.DriveLetter + ':' }
        Write-Host ('    partition ' + $p.Number + '  ' + $l + '  ' + [math]::Round([int64]$p.Size / 1e6) + ' MB  ' + $p.Type)
    }
}

function Stop-Refused([string]$Why) {
    Write-Host ('refused: ' + $Why + ' Nothing was written.')
    exit 2
}

function Test-Interactive {
    if ([Console]::IsInputRedirected) { return $false }
    if (-not [Environment]::UserInteractive) { return $false }
    foreach ($a in [Environment]::GetCommandLineArgs()) { if ($a -like '-NonI*') { return $false } }
    return $true
}

function Read-Full($Stream, [byte[]]$Buf, [int]$Want) {
    $n = 0
    while ($n -lt $Want) {
        $r = $Stream.Read($Buf, $n, $Want - $n)
        if ($r -le 0) { break }
        $n += $r
    }
    return $n
}

# --- the write ---

# Order matters. The first MiB -- the partition table and boot code -- is
# zeroed first and written last, so a write that stops anywhere leaves a stick
# Windows and firmware see as blank, never a table pointing at half an image.
# The last MiB is zeroed too: a stale backup GPT there makes some firmware and
# Windows itself report a damaged disk. The image is hashed as it is read, so
# what goes to the stick is what matched the sidecar, not what the file held
# a minute earlier.
function Write-ImageToDisk($Disk, [string]$ImagePath, [int64]$ImageBytes, [string]$ExpectedHash) {
    $h = [SonoraUsb.Native]::Open($Disk.Path, $Disk.Kind, $true, $false)
    $in = [IO.File]::OpenRead($ImagePath)
    $sha = [Security.Cryptography.SHA256]::Create()
    $zero = New-Object byte[] $script:MiB
    try {
        [SonoraUsb.Native]::WriteAt($h, 0, $zero, $zero.Length)
        [SonoraUsb.Native]::WriteAt($h, [int64]$Disk.Size - $script:MiB, $zero, $zero.Length)

        $head = New-Object byte[] $script:MiB
        $headLen = Read-Full $in $head ([int]$script:MiB)
        [void]$sha.TransformBlock($head, 0, $headLen, $null, 0)

        $buf = New-Object byte[] $script:ChunkBytes
        $offset = [int64]$headLen
        $nextReport = [int64]0
        while ($offset -lt $ImageBytes) {
            $n = Read-Full $in $buf $script:ChunkBytes
            if ($n -le 0) { throw ('the image ended at byte ' + $offset + ', before its ' + $ImageBytes + ' bytes') }
            [void]$sha.TransformBlock($buf, 0, $n, $null, 0)
            # A raw disk takes whole sectors only.
            $padded = [int]([math]::Ceiling($n / 512) * 512)
            if ($padded -gt $n) { [Array]::Clear($buf, $n, $padded - $n) }
            [SonoraUsb.Native]::WriteAt($h, $offset, $buf, $padded)
            $offset += $n
            if ($offset -ge $nextReport) {
                Write-Host ('  ' + [math]::Floor(100 * $offset / $ImageBytes) + '%  ' + [math]::Floor($offset / $script:MiB) + ' MiB')
                $nextReport += [int64]$ImageBytes / 10
            }
        }
        [void]$sha.TransformFinalBlock($buf, 0, 0)
        $got = [BitConverter]::ToString($sha.Hash).Replace('-', '')
        if ($got -ne $ExpectedHash) {
            [SonoraUsb.Native]::Flush($h)
            throw ('the image changed while it was being written (SHA-256 now ' + $got + '). Its first MiB was not written, so the stick will not boot. Rebuild the image and run again.')
        }
        $padded = [int]([math]::Ceiling($headLen / 512) * 512)
        if ($padded -gt $headLen) { [Array]::Clear($head, $headLen, $padded - $headLen) }
        [SonoraUsb.Native]::WriteAt($h, 0, $head, $padded)
        [SonoraUsb.Native]::Flush($h)
        if ($Disk.Kind -eq 'device') { [void][SonoraUsb.Native]::UpdateProperties($h) }
    } finally {
        $sha.Dispose()
        $in.Dispose()
        $h.Dispose()
    }
}

# Read back the first and last 64 MiB of the image region, and the zeroed last
# MiB of the disk, through a fresh unbuffered handle.
function Test-WrittenDisk($Disk, [string]$ImagePath, [int64]$ImageBytes) {
    $headLen = [math]::Min($script:VerifyBytes, $ImageBytes)
    $tailStart = [int64]([math]::Floor([math]::Max(0, $ImageBytes - $script:VerifyBytes) / $script:MiB)) * $script:MiB
    $tailLen = $ImageBytes - $tailStart
    $img = [SonoraUsb.Native]::Open($ImagePath, 'file', $false, $true)
    $dev = [SonoraUsb.Native]::Open($Disk.Path, $Disk.Kind, $false, $true)
    $ok = $true
    try {
        $checks = @(
            @('first ' + [math]::Floor($headLen / $script:MiB) + ' MiB', [int64]0, [int64]$headLen),
            @('last ' + [math]::Floor($tailLen / $script:MiB) + ' MiB of the image', $tailStart, $tailLen)
        )
        foreach ($c in $checks) {
            $want = [SonoraUsb.Native]::HashRange($img, $c[1], $c[2])
            $got = [SonoraUsb.Native]::HashRange($dev, $c[1], $c[2])
            if ($want -eq $got) {
                Write-Host ('  read back ok    ' + $c[0] + '  ' + $got)
            } else {
                Write-Host ('  read back FAIL  ' + $c[0] + ': stick ' + $got + ', image ' + $want)
                $ok = $false
            }
        }
        $zeroHash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash((New-Object byte[] $script:MiB))).Replace('-', '')
        $got = [SonoraUsb.Native]::HashRange($dev, [int64]$Disk.Size - $script:MiB, $script:MiB)
        if ($got -eq $zeroHash) {
            Write-Host '  read back ok    last MiB of the disk is zero (no stale backup GPT)'
        } else {
            Write-Host '  read back FAIL  last MiB of the disk is not zero'
            $ok = $false
        }
    } finally {
        $img.Dispose()
        $dev.Dispose()
    }
    return $ok
}

function Invoke-Main {
    if ($SelfTest) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -NonInteractive -File (Join-Path $PSScriptRoot 'test-write-usb.ps1') | Out-Host
        return $LASTEXITCODE
    }
    $root = Split-Path -Parent $PSScriptRoot
    $iso = Join-Path $root 'iso\sonora.iso'
    $stick = Join-Path $root 'iso\sonora-stick.img'
    $persistent = $true
    if ($Image -eq '') {
        if (Test-Path -LiteralPath $stick -PathType Leaf) {
            $Image = $stick
            if ((Test-Path -LiteralPath $iso -PathType Leaf) -and ((Get-Item -LiteralPath $iso).LastWriteTimeUtc -gt (Get-Item -LiteralPath $stick).LastWriteTimeUtc)) {
                Stop-Refused ($stick + ' is older than ' + $iso + '. Rebuild it: tools/make-stick-image.sh')
            }
        } else {
            $Image = $iso
            $persistent = $false
        }
    } elseif (-not ($Image -like '*stick*.img')) {
        $persistent = $false
    }
    if (-not (Test-Path -LiteralPath $Image -PathType Leaf)) {
        Write-Host ('error: ' + $Image + ' not found. Build it: tools/build-iso.sh, then tools/make-stick-image.sh')
        return 1
    }
    $Image = (Resolve-Path -LiteralPath $Image).Path
    if ($Image.StartsWith('\\')) { Stop-Refused 'the image must be on a local drive, not a network path.' }
    $imageBytes = (Get-Item -LiteralPath $Image).Length
    $imageDrive = ([IO.Path]::GetPathRoot($Image)).Substring(0, 1).ToUpperInvariant()

    Write-Host ('Image: ' + $Image + ' (' + [math]::Round($imageBytes / 1e6) + ' MB)')
    if (-not $persistent) {
        Write-Host '  warning: no persistence partition in this image; the stick will boot but keep nothing across a reboot.'
    }
    Write-Host '  computing SHA-256...'
    $hash = (Get-FileHash -LiteralPath $Image -Algorithm SHA256).Hash.ToUpperInvariant()
    $sidecar = $Image + '.sha256'
    if (Test-Path -LiteralPath $sidecar -PathType Leaf) {
        $want = Read-Sidecar $sidecar
        if ($want -eq '') { Stop-Refused ($sidecar + ' does not hold a SHA-256.') }
        if ($want -ne $hash) { Stop-Refused ('the image does not match ' + $sidecar + ' (file ' + $hash + ', sidecar ' + $want + '). Copy or rebuild it again.') }
        Write-Host ('  SHA-256 ' + $hash + ' matches ' + (Split-Path -Leaf $sidecar))
    } else {
        Write-Host ('  SHA-256 ' + $hash + ' (no ' + (Split-Path -Leaf $sidecar) + ' to check it against; compare it with the build machine by hand)')
    }

    $disks = @(Get-Disks)
    $usb = @($disks | Where-Object { $_.BusType -eq 'USB' })
    Write-Host ''
    Write-Host ('USB disks (' + ($disks.Count - $usb.Count) + ' other disks not shown):')
    if ($usb.Count -eq 0) { Write-Host '  none. Plug the stick in and run again.' }
    foreach ($d in $usb) {
        $why = Get-DiskRefusal $d $imageBytes ([bool]$AllowLarge) $imageDrive
        $state = 'can be written'
        if ($why -ne '') { $state = 'refused: ' + $why }
        Write-Host ('  ' + $d.Number + '  ' + [math]::Round($d.Size / 1e9, 1) + ' GB  ' + $d.FriendlyName + '  serial ' + $d.SerialNumber + '  -- ' + $state)
    }
    if ($List) { return 0 }

    if ($DiskNumber -lt 0) {
        if (-not (Test-Interactive)) { Stop-Refused 'no -DiskNumber, and there is nobody to ask.' }
        $answer = Read-Host 'Disk number to erase (Enter to stop)'
        if ($answer -notmatch '^[0-9]+$') { Write-Host 'aborted.'; return 2 }
        $DiskNumber = [int]$answer
    }
    $target = @($disks | Where-Object { $_.Number -eq $DiskNumber })
    if ($target.Count -ne 1) { Stop-Refused ('there is no disk ' + $DiskNumber + '.') }
    $disk = $target[0]
    $why = Get-DiskRefusal $disk $imageBytes ([bool]$AllowLarge) $imageDrive
    if ($why -ne '') { Stop-Refused ('disk ' + $disk.Number + ': ' + $why) }
    if ($disk.Kind -eq 'device' -and -not (Test-DevicePath $disk.Path)) { Stop-Refused ('internal: malformed device path ' + $disk.Path) }
    if ($disk.Kind -eq 'file' -and ($disk.Path.StartsWith('\\') -or -not (Test-Path -LiteralPath $disk.Path -PathType Leaf))) {
        Stop-Refused ('fake disk ' + $disk.Number + ' must be an existing regular file, not ' + $disk.Path)
    }

    Write-Host ''
    Write-Host 'Target:'
    Show-Disk $disk
    Write-Host ''
    Write-Host ('EVERYTHING ON DISK ' + $disk.Number + ' (' + $disk.FriendlyName + ', ' + [math]::Round($disk.Size / 1e9, 1) + ' GB) WILL BE ERASED.')
    $token = Get-ConfirmToken $disk
    if ($Confirm -ne '') {
        if ($Confirm -cne $token) { Stop-Refused ('-Confirm does not match this disk; it expects exactly: ' + $token) }
    } else {
        if (-not (Test-Interactive)) { Stop-Refused ('not interactive; pass -Confirm "' + $token + '" to write without a prompt.') }
        $reply = Read-Host ('Type  ' + $token + '  to continue')
        if ($reply -cne $token) { Write-Host 'aborted. Nothing was written.'; return 2 }
    }

    if ($DryRun -or $WhatIf) {
        Write-Host ('dry run: would write ' + $imageBytes + ' bytes to ' + $disk.Path + ' and verify them. Nothing was written.')
        return 0
    }

    # Everything again, now that a human has had time to unplug one stick and
    # plug in another: the number may now name a different disk.
    if ($disk.Kind -eq 'device') {
        $me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            Stop-Refused 'writing a disk needs an elevated PowerShell (Run as administrator).'
        }
    }
    $again = @(@(Get-Disks) | Where-Object { $_.Number -eq $DiskNumber })
    if ($again.Count -ne 1 -or (Get-DiskIdentity $again[0]) -ne (Get-DiskIdentity $disk)) {
        Stop-Refused ('disk ' + $DiskNumber + ' is no longer the disk that was shown.')
    }
    $why = Get-DiskRefusal $again[0] $imageBytes ([bool]$AllowLarge) $imageDrive
    if ($why -ne '') { Stop-Refused ('disk ' + $DiskNumber + ': ' + $why) }

    # Windows refuses writes into a mounted volume's sectors. Each volume on
    # the stick is locked and dismounted, and the handles stay open until the
    # write is done so nothing remounts it in between.
    $locks = @()
    try {
        if ($disk.Kind -eq 'device') {
            foreach ($p in @($disk.Partitions)) {
                foreach ($ap in @($p.AccessPaths)) {
                    if ([string]$ap -like '\\?\Volume{*') {
                        $vp = ([string]$ap).TrimEnd([char]92)
                        $vh = [SonoraUsb.Native]::Open($vp, 'volume', $true, $false)
                        $locks += $vh
                        [SonoraUsb.Native]::LockAndDismount($vh, $vp)
                    }
                }
            }
        }
        Write-Host ('Writing ' + $Image + ' to ' + $disk.Path + '...')
        try {
            Write-ImageToDisk $disk $Image $imageBytes $hash
        } catch {
            Write-Host ('error: ' + $_.Exception.Message)
            Write-Host 'The stick was erased but not fully written; it will not boot. Run again.'
            return 1
        }
    } catch {
        Write-Host ('error: ' + $_.Exception.Message)
        Write-Host 'Nothing was written.'
        return 1
    } finally {
        foreach ($vh in $locks) { $vh.Dispose() }
    }

    Write-Host 'Verifying...'
    if (-not (Test-WrittenDisk $disk $Image $imageBytes)) {
        Write-Host 'error: the stick does not read back as written. Try another port or stick.'
        return 1
    }
    if ($persistent) {
        Write-Host ('Done: disk ' + $disk.Number + ' holds the image and the persistence partition.')
    } else {
        Write-Host ('Done: disk ' + $disk.Number + ' holds the image, without persistence.')
    }
    return 0
}

# Dot-sourced by tools/test-write-usb.ps1 for the functions alone.
if ($MyInvocation.InvocationName -eq '.') { return }
exit (Invoke-Main)
