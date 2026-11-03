# Write the SonoraOS image to a USB stick from Windows (Windows PowerShell 5.1).
#
# THIS ERASES THE WHOLE TARGET DISK. Run from an elevated PowerShell:
#
#   powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -List
#   powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -DiskNumber 3 -DryRun
#   powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -DiskNumber 3
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
# never through a .NET path API; tools/test-write-usb.ps1 asserts both. Once
# open, the handle itself is asked for its length, bus and serial, which must
# be the disk that was shown.
#
# -FakeDisks FILE.json replaces the machine's disks with descriptions whose
# Path is a regular file. Every step but locking volumes runs against that
# file, which is how tools/test-write-usb.ps1 exercises the whole flow
# without a real disk anywhere near it.
#
# Exit codes: 0 done (or dry run), 1 error, 2 refused (nothing was written).
# A misspelt parameter is an error before anything runs: [CmdletBinding()]
# makes it one, so "-DryRn" cannot turn a rehearsal into a real write.

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Image = '',
    [int]$DiskNumber = -1,
    [string]$Token = '',
    [switch]$AllowLarge,
    [switch]$AllowDataVolumes,
    [switch]$DryRun,
    [switch]$List,
    [switch]$SelfTest,
    [string]$FakeDisks = ''
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

# Belt and braces for the same reason as [CmdletBinding()]: an argument that
# bound to nothing is a typo, and a typo here can erase a disk.
if ((Test-Path variable:args) -and @($args).Count -gt 0) {
    Write-Host ('refused: unknown arguments: ' + (@($args) -join ' ') + ' Nothing was written.')
    exit 2
}
# -WhatIf and -Confirm are not parameters here (-DryRun and -Token are), and
# without SupportsShouldProcess [CmdletBinding()] does not add them, so either
# fails parameter binding before this line runs; tools/test-write-usb.ps1
# checks that.

# 256 GiB rather than GB, so a stick sold as "256 GB" still passes; same
# limit as tools/write-usb.sh. Above it the disk is more likely a backup drive.
$script:MaxBytesWithoutOverride = [int64]256 * 1024 * 1024 * 1024
$script:MiB = [int64]1048576
$script:VerifyBytes = 64 * $script:MiB
$script:ChunkBytes = 4 * 1048576
# A partition this small is an EFI system partition or a reserved one, never
# somewhere data lives.
$script:SmallPartition = 64 * $script:MiB

if (-not ('SonoraUsb.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
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
    static extern bool WriteFile(SafeFileHandle h, IntPtr buf, uint count, out uint written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle h, IntPtr buf, uint count, out uint read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FlushFileBuffers(SafeFileHandle h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint GetFileType(SafeFileHandle h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileSizeEx(SafeFileHandle h, out long size);

    public const string DevicePrefix = @"\\.\PhysicalDrive";
    const string VolumePrefix = @"\\?\Volume{";
    const uint IoctlDiskGetLengthInfo = 0x0007405Cu;
    const uint IoctlStorageQueryProperty = 0x002D1400u;
    const int Align = 4096;

    static void Fail(string what) {
        int err = Marshal.GetLastWin32Error();
        throw new Exception(what + ": " + new Win32Exception(err).Message + " (Win32 error " + err + ")");
    }

    static bool AllDigits(string s) {
        if (s.Length == 0) return false;
        foreach (char c in s) if (c < '0' || c > '9') return false;
        return true;
    }

    static IntPtr Aligned(IntPtr raw) { return new IntPtr((raw.ToInt64() + Align - 1) & ~(long)(Align - 1)); }

    // Answers only for a disk or a volume; a regular file has no length IOCTL.
    static bool TryDiskLength(SafeFileHandle h, out long length) {
        IntPtr o = Marshal.AllocHGlobal(8);
        try {
            uint r;
            bool ok = DeviceIoControl(h, IoctlDiskGetLengthInfo, IntPtr.Zero, 0, o, 8, out r, IntPtr.Zero) && r >= 8;
            length = ok ? Marshal.ReadInt64(o) : -1;
            return ok;
        } finally { Marshal.FreeHGlobal(o); }
    }

    // kind is "device", "volume" or "file". Each kind accepts only its own
    // path shape, so a file path can never reach the device branch or the
    // reverse, whatever the caller passed. A "file" must be an existing file
    // on a drive letter (File.Exists is false for devices and directories)
    // and the open handle must be a disk file, not a pipe or a console.
    public static SafeFileHandle Open(string path, string kind, bool write, bool unbuffered) {
        if (kind == "device") {
            if (!path.StartsWith(DevicePrefix, StringComparison.Ordinal)
                || !AllDigits(path.Substring(DevicePrefix.Length)))
                throw new ArgumentException("not a physical drive path: '" + path + "'");
        } else if (kind == "volume") {
            if (!path.StartsWith(VolumePrefix, StringComparison.Ordinal) || path.EndsWith("\\"))
                throw new ArgumentException("not a volume path: '" + path + "'");
        } else if (kind == "file") {
            if (!Regex.IsMatch(path, @"^[A-Za-z]:\\") || path.IndexOf('/') >= 0 || path.IndexOf('?') >= 0
                || !System.IO.File.Exists(path))
                throw new ArgumentException("not an existing regular file on a drive letter: '" + path + "'");
        } else {
            throw new ArgumentException("unknown kind " + kind);
        }
        uint access = 0x80000000u | (write ? 0x40000000u : 0u);          // GENERIC_READ | GENERIC_WRITE
        uint flags = (write ? 0x80000000u : 0u) | (unbuffered ? 0x20000000u : 0u); // WRITE_THROUGH, NO_BUFFERING
        SafeFileHandle h = CreateFileW(path, access, 3u, IntPtr.Zero, 3u, flags, IntPtr.Zero);
        if (h.IsInvalid) Fail("cannot open " + path);
        if (kind == "file") {
            if (GetFileType(h) != 1u) {
                h.Dispose();
                throw new ArgumentException("'" + path + "' opened as something other than a regular file");
            }
        }
        return h;
    }

    public static long DiskLength(SafeFileHandle h) {
        long n;
        if (!TryDiskLength(h, out n)) Fail("cannot read the disk's length");
        return n;
    }

    public static long FileLength(SafeFileHandle h) {
        long n;
        if (!GetFileSizeEx(h, out n)) Fail("cannot read the file's length");
        return n;
    }

    // STORAGE_DEVICE_DESCRIPTOR: SerialNumberOffset at 24, BusType at 28
    // (BusTypeUsb = 7).
    public static string DeviceSerial(SafeFileHandle h, out int busType) {
        IntPtr q = Marshal.AllocHGlobal(12);
        IntPtr o = Marshal.AllocHGlobal(4096);
        try {
            for (int i = 0; i < 12; i++) Marshal.WriteByte(q, i, 0);
            uint r;
            if (!DeviceIoControl(h, IoctlStorageQueryProperty, q, 12, o, 4096, out r, IntPtr.Zero) || r < 32)
                Fail("cannot query the device descriptor");
            busType = Marshal.ReadInt32(o, 28);
            int off = Marshal.ReadInt32(o, 24);
            if (off <= 0 || off >= (int)r) return "";
            string s = Marshal.PtrToStringAnsi(new IntPtr(o.ToInt64() + off));
            return s == null ? "" : s.Trim();
        } finally {
            Marshal.FreeHGlobal(q);
            Marshal.FreeHGlobal(o);
        }
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

    // From a page-aligned copy: some USB controllers reject a raw-disk write
    // from an unaligned buffer, and a managed array has no alignment.
    // A short write is a failure, never something to retry around.
    public static void WriteAt(SafeFileHandle h, long offset, byte[] buf, int count) {
        IntPtr raw = Marshal.AllocHGlobal(count + Align);
        try {
            IntPtr p = Aligned(raw);
            Marshal.Copy(buf, 0, p, count);
            Seek(h, offset);
            uint written;
            if (!WriteFile(h, p, (uint)count, out written, IntPtr.Zero)) Fail("write failed at byte " + offset);
            if (written != (uint)count)
                throw new Exception("short write at byte " + offset + ": " + written + " of " + count);
        } finally {
            Marshal.FreeHGlobal(raw);
        }
    }

    // count bytes at offset (both multiples of 4096), for a handle opened
    // read-only and unbuffered: what is on the medium, not in a cache.
    public static byte[] ReadAt(SafeFileHandle h, long offset, int count) {
        int ask = (count + Align - 1) & ~(Align - 1);
        IntPtr raw = Marshal.AllocHGlobal(ask + Align);
        try {
            IntPtr p = Aligned(raw);
            Seek(h, offset);
            uint got;
            if (!ReadFile(h, p, (uint)ask, out got, IntPtr.Zero)) Fail("read failed at byte " + offset);
            if (got < (uint)count) throw new Exception("short read at byte " + offset + ": " + got + " of " + count);
            byte[] b = new byte[count];
            Marshal.Copy(p, b, 0, count);
            return b;
        } finally {
            Marshal.FreeHGlobal(raw);
        }
    }

    public static bool AllZero(byte[] b) {
        foreach (byte x in b) if (x != 0) return false;
        return true;
    }

    public static void Flush(SafeFileHandle h) {
        if (!FlushFileBuffers(h)) Fail("flush failed");
    }

    // SHA-256 of [offset, offset+length). Meant for a handle opened
    // unbuffered, so a read-back comes from the medium and not from a cache:
    // the buffer is page-aligned and every request a multiple of 4096 bytes.
    public static string HashRange(SafeFileHandle h, long offset, long length) {
        const int Block = 4 << 20;
        IntPtr raw = Marshal.AllocHGlobal(Block + Align);
        try {
            IntPtr p = Aligned(raw);
            byte[] managed = new byte[Block];
            using (SHA256 sha = SHA256.Create()) {
                Seek(h, offset);
                long done = 0;
                while (done < length) {
                    long want = Math.Min((long)Block, length - done);
                    uint ask = (uint)((want + Align - 1) & ~(long)(Align - 1));
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

# A property, or $Default when the object has none: fake disks and the tests'
# hand-made disks carry only the fields a case needs.
function Get-Field($Object, [string]$Name, $Default) {
    if ($null -eq $Object) { return $Default }
    if ($Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $Default
}

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

function Get-PartitionName($Part) {
    $l = [string](Get-Field $Part 'DriveLetter' '')
    if ($l -ne '') { return ('partition ' + $Part.Number + ' (' + $l + ':)') }
    return ('partition ' + $Part.Number)
}

# Why this disk must not be written, or '' if it may be.
function Get-DiskRefusal($Disk, [int64]$ImageBytes, [bool]$AllowLarge, [string]$ImageDrive) {
    if ($Disk.IsSystem) { return 'it is the system disk (Windows runs from it).' }
    if ($Disk.IsBoot) { return 'it is the boot disk.' }
    if ($Disk.HasPageFile) { return 'it holds a page file.' }
    $parts = @(Get-Field $Disk 'Partitions' @())
    foreach ($p in $parts) {
        if ([bool](Get-Field $p 'BitLocker' $false)) { return ((Get-PartitionName $p) + ' is BitLocker-encrypted. Decrypt or erase it in Windows first if it really is the stick.') }
        if ([bool](Get-Field $p 'HasWindows' $false)) { return ((Get-PartitionName $p) + ' holds a Windows installation or boot loader.') }
        if ([bool](Get-Field $p 'IsSystemPartition' $false)) { return ((Get-PartitionName $p) + ' is a system or boot partition.') }
    }
    $letters = @($parts | ForEach-Object { [string](Get-Field $_ 'DriveLetter' '') } | Where-Object { $_ -ne '' })
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

# A byte array property, or $null. The comma keeps PowerShell from unrolling
# the array into the pipeline one byte at a time.
function Get-Bytes($Object, [string]$Name) {
    # Not through Get-Field, whose return would unroll the array.
    if ($null -eq $Object -or -not $Object.PSObject.Properties[$Name]) { return $null }
    $v = $Object.$Name
    if ($v -is [byte[]]) { return , $v }
    return $null
}

function Get-U32([byte[]]$B, [int]$At) { return [int64][BitConverter]::ToUInt32($B, $At) }

# The sector where partition 3 starts if this MBR is exactly the one our
# image carries, else -1. xorriso's hybrid table (tools/make-stick-image.sh
# prints it): entry 1 type 0x00 from sector 64 over the whole ISO, entry 2
# type 0xEF at 540 for 6656 sectors (the EFI image), and write-usb.sh's entry
# 3, type 0x83, at the first MiB boundary after the ISO; entry 4 empty.
function Get-SonoraP3Start([byte[]]$Head) {
    if ($null -eq $Head -or $Head.Length -lt 65536) { return -1 }
    if ($Head[510] -ne 0x55 -or $Head[511] -ne 0xAA) { return -1 }
    if ($Head[450] -ne 0x00 -or (Get-U32 $Head 454) -ne 64) { return -1 }
    $size1 = Get-U32 $Head 458
    if ($size1 -le 0) { return -1 }
    if ($Head[466] -ne 0xEF -or (Get-U32 $Head 470) -ne 540 -or (Get-U32 $Head 474) -ne 6656) { return -1 }
    $start3 = [int64]([math]::Ceiling((64 + $size1) / 2048)) * 2048
    if ($Head[482] -ne 0x83 -or (Get-U32 $Head 486) -ne $start3 -or (Get-U32 $Head 490) -le 0) { return -1 }
    for ($i = 494; $i -lt 510; $i++) { if ($Head[$i] -ne 0) { return -1 } }
    return $start3
}

# Our image, byte for byte where it identifies itself: the MBR above, the
# ISO's primary volume descriptor (byte 32768) with --iso-volume "SONORA"
# from iso/auto/config, and an ext4 superblock labelled "persistence" at the
# start of partition 3. Another distribution's live stick with persistence
# differs in the volume label at least.
function Test-SonoraBytes([byte[]]$Head, [byte[]]$P3, [int64]$DiskSize) {
    $start3 = Get-SonoraP3Start $Head
    if ($start3 -lt 0) { return $false }
    if ($start3 * 512 + 4096 -gt $DiskSize) { return $false }
    $ascii = [Text.Encoding]::ASCII
    if ($Head[32768] -ne 1 -or $ascii.GetString($Head, 32769, 5) -cne 'CD001') { return $false }
    if ($ascii.GetString($Head, 32808, 32).TrimEnd([char]32, [char]0) -cne 'SONORA') { return $false }
    if ($null -eq $P3 -or $P3.Length -lt 4096) { return $false }
    if ($P3[1080] -ne 0x53 -or $P3[1081] -ne 0xEF) { return $false }
    return ($ascii.GetString($P3, 1144, 16).TrimEnd([char]0) -ceq 'persistence')
}

# A stick this script or tools/write-usb.sh wrote before. Rewriting it loses
# the old persistence, which the prompt says, but it is not a stranger's data
# disk. Its content must be our image (Test-SonoraBytes) and Windows must see
# nothing it can read on it but the small EFI partition: no letter and no
# filesystem on anything larger, no volume it failed to query.
function Test-SonoraLayout($Disk) {
    if ([bool](Get-Field $Disk 'PartitionQueryFailed' $false)) { return $false }
    $parts = @(Get-Field $Disk 'Partitions' @())
    if ($parts.Count -gt 3) { return $false }
    foreach ($p in $parts) {
        if ([bool](Get-Field $p 'VolumeQueryFailed' $false)) { return $false }
        if ([int64]$p.Size -gt $script:SmallPartition -and ([string](Get-Field $p 'DriveLetter' '') -ne '' -or [string](Get-Field $p 'FileSystem' '') -ne '')) { return $false }
    }
    return (Test-SonoraBytes (Get-Bytes $Disk 'Head') (Get-Bytes $Disk 'P3Block') ([int64]$Disk.Size))
}

function New-WholeDiskItem($Disk, [string]$Why) {
    return [pscustomobject]@{ Number = 0; DriveLetter = ''; Size = [int64]$Disk.Size; FileSystem = ''; Label = ''
        UsedBytes = [int64]0; Description = ('the whole disk: ' + $Why) }
}

# What may hold someone's files. Partitions with a drive letter, a filesystem
# Windows recognises, a volume it failed to query, or over 64 MiB it cannot
# read; the whole disk when its partitions could not be listed, or when it
# has none but its first MiB is not provably blank (a filesystem or LUKS on
# the whole device has no partition table). Nothing on a blank stick or a
# previous SonoraOS stick.
function Get-DataPartitions($Disk) {
    if ([bool](Get-Field $Disk 'PartitionQueryFailed' $false)) {
        return @(New-WholeDiskItem $Disk 'Windows could not list its partitions')
    }
    if (Test-SonoraLayout $Disk) { return @() }
    $parts = @(Get-Field $Disk 'Partitions' @())
    if ($parts.Count -eq 0) {
        $head = Get-Bytes $Disk 'Head'
        if ($null -ne $head -and $head.Length -ge $script:MiB -and [SonoraUsb.Native]::AllZero($head)) { return @() }
        return @(New-WholeDiskItem $Disk 'no partitions, and its first MiB is not blank or could not be read (a filesystem on the whole device?)')
    }
    $out = @()
    foreach ($p in $parts) {
        $letter = [string](Get-Field $p 'DriveLetter' '')
        $fs = [string](Get-Field $p 'FileSystem' '')
        $failed = [bool](Get-Field $p 'VolumeQueryFailed' $false)
        if ($failed -or $letter -ne '' -or $fs -ne '' -or [int64]$p.Size -gt $script:SmallPartition) { $out += $p }
    }
    return $out
}

# One line per data item, compared before and after the prompt.
function Get-DataSignature($Items) {
    return (@($Items | ForEach-Object { [string]$_.Number + ':' + (Get-Field $_ 'DriveLetter' '') + ':' + (Get-Field $_ 'FileSystem' '') + ':' + $_.Size + ':' + (Get-Field $_ 'Description' '') }) -join '|')
}

function Format-Partition($Part) {
    $desc = [string](Get-Field $Part 'Description' '')
    if ($desc -ne '') { return $desc }
    $fs = [string](Get-Field $Part 'FileSystem' '')
    if ($fs -eq '') { $fs = 'unknown filesystem' }
    $label = [string](Get-Field $Part 'Label' '')
    $s = (Get-PartitionName $Part) + '  ' + $fs
    if ($label -ne '') { $s += ' "' + $label + '"' }
    $s += '  ' + [math]::Round([int64]$Part.Size / 1e6) + ' MB'
    $used = [int64](Get-Field $Part 'UsedBytes' 0)
    if ($used -gt 0) { $s += ', ' + [math]::Round($used / 1e6) + ' MB used' }
    return $s
}

# What the operator types. The serial's tail ties the answer to the disk that
# was shown: if the number moves to another disk, the same answer no longer
# fits it. A disk without a serial gets its model and exact size instead, and
# must be confirmed by hand (Test-TokenAllowed).
function Get-ConfirmToken($Disk) {
    $alnum = ([string]$Disk.SerialNumber -replace '[^A-Za-z0-9]', '').ToUpperInvariant()
    if ($alnum.Length -ge 4) {
        return ('ERASE ' + $Disk.Number + ' ' + $alnum.Substring($alnum.Length - 4))
    }
    $model = ([string](Get-Field $Disk 'FriendlyName' '') -replace '[^A-Za-z0-9]', '').ToUpperInvariant()
    if ($model -eq '') { $model = 'NOMODEL' }
    return ('ERASE ' + $Disk.Number + ' ' + $model + ' ' + [int64]$Disk.Size)
}

function Test-HasSerial($Disk) {
    return (([string]$Disk.SerialNumber -replace '[^A-Za-z0-9]', '').Length -ge 4)
}

# The second answer a disk holding data needs: its drive letters, or the
# partition numbers when it has none.
function Get-DataToken($Parts) {
    if (@($Parts | Where-Object { [int]$_.Number -eq 0 }).Count -gt 0) { return 'ERASE DATA DISK' }
    $letters = (@($Parts | ForEach-Object { [string](Get-Field $_ 'DriveLetter' '') } | Where-Object { $_ -ne '' }) -join '')
    if ($letters -ne '') { return ('ERASE DATA ' + $letters.ToUpperInvariant()) }
    return ('ERASE DATA ' + (@($Parts | ForEach-Object { 'P' + $_.Number }) -join ''))
}

# Everything that tells two disks apart, partitions included (offset, size,
# type, GUID): a disk without a serial is then still not interchangeable with
# another of the same model and size.
function Get-DiskIdentity($Disk) {
    $parts = @(@(Get-Field $Disk 'Partitions' @()) | ForEach-Object {
        [string]$_.Number + ':' + (Get-Field $_ 'Offset' '') + ':' + $_.Size + ':' + (Get-Field $_ 'Type' '') + ':' +
        (Get-Field $_ 'MbrType' '') + ':' + (Get-Field $_ 'GptType' '') + ':' + (Get-Field $_ 'Guid' '') })
    return ([string]$Disk.Number + '|' + $Disk.UniqueId + '|' + $Disk.SerialNumber + '|' + $Disk.Size + '|' + $Disk.BusType + '|' +
        (Get-Field $Disk 'FriendlyName' '') + '|' + [bool](Get-Field $Disk 'PartitionQueryFailed' $false) + '|' + ($parts -join ';'))
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
            $fake = [pscustomobject]@{
                Number = [int]$d.Number; FriendlyName = [string]$d.FriendlyName
                SerialNumber = [string]$d.SerialNumber; Size = [int64]$d.Size
                BusType = [string]$d.BusType; IsSystem = [bool]$d.IsSystem; IsBoot = [bool]$d.IsBoot
                HasPageFile = [bool]$d.HasPageFile; LogicalSectorSize = [int]$d.LogicalSectorSize
                UniqueId = [string]$d.UniqueId; Partitions = @($d.Partitions)
                PartitionQueryFailed = [bool](Get-Field $d 'PartitionQueryFailed' $false)
                Path = [string]$d.Path; Kind = 'file'; Head = $null; P3Block = $null
            }
            Add-DiskContent $fake
            $out += $fake
        }
        return $out
    }
    $pageLetters = @(Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue |
        ForEach-Object { ([string]$_.Name).Substring(0, 1).ToUpperInvariant() })
    # Volumes BitLocker has touched, by \\?\Volume{...} path. Needs elevation
    # and is absent on Windows Home; then a locked volume still shows as a
    # partition Windows cannot read, which counts as data below.
    $bitlocker = @{}
    try {
        foreach ($ev in @(Get-CimInstance -Namespace 'root/cimv2/Security/MicrosoftVolumeEncryption' -ClassName Win32_EncryptableVolume -ErrorAction Stop)) {
            if ([int]$ev.ConversionStatus -ne 0 -or [int]$ev.ProtectionStatus -ne 0) {
                $bitlocker[([string]$ev.DeviceID).TrimEnd([char]92).ToUpperInvariant()] = $true
            }
        }
    } catch {
        $bitlocker = @{}
    }
    $out = @()
    foreach ($d in @(Get-Disk)) {
        $parts = @()
        # Through the pipeline: a disk with no partitions yields none, without
        # the "no MSFT_Partition objects found" error -DiskNumber gives. Any
        # error is a disk whose contents are unknown, which counts as data.
        $partsFailed = $false
        $found = @()
        try { $found = @($d | Get-Partition -ErrorAction Stop) } catch { $partsFailed = $true; $found = @() }
        foreach ($p in $found) {
            $letter = ''
            if ($p.DriveLetter -and [int][char]$p.DriveLetter -ne 0) { $letter = ([string]$p.DriveLetter).ToUpperInvariant() }
            $fs = ''; $label = ''; $used = [int64]0
            $vol = $null
            $volFailed = $false
            try { $vol = $p | Get-Volume -ErrorAction Stop } catch { $vol = $null; $volFailed = $true }
            if ($null -ne $vol) {
                $fs = [string]$vol.FileSystem
                $label = [string]$vol.FileSystemLabel
                if ([int64]$vol.Size -gt 0) { $used = [int64]$vol.Size - [int64]$vol.SizeRemaining }
            }
            $locked = $false
            foreach ($ap in @($p.AccessPaths)) {
                if ($bitlocker.ContainsKey(([string]$ap).TrimEnd([char]92).ToUpperInvariant())) { $locked = $true }
            }
            $hasWindows = $false
            if ($letter -ne '') {
                foreach ($rel in @('Windows\System32', 'bootmgr', 'EFI\Microsoft\Boot')) {
                    if (Test-Path -LiteralPath ($letter + ':\' + $rel)) { $hasWindows = $true }
                }
            }
            $parts += [pscustomobject]@{
                Number = $p.PartitionNumber; DriveLetter = $letter; Size = [int64]$p.Size
                Type = [string]$p.Type; AccessPaths = @($p.AccessPaths)
                Offset = [int64]$p.Offset; Guid = [string]$p.Guid
                MbrType = [string]$p.MbrType; GptType = [string]$p.GptType
                FileSystem = $fs; Label = $label; UsedBytes = $used; VolumeQueryFailed = $volFailed
                BitLocker = $locked; HasWindows = $hasWindows
                IsSystemPartition = ([bool]$p.IsSystem -or [bool]$p.IsBoot)
            }
        }
        $hasPage = $false
        foreach ($p in $parts) { if ($p.DriveLetter -ne '' -and ($pageLetters -contains $p.DriveLetter)) { $hasPage = $true } }
        $disk = [pscustomobject]@{
            Number = [int]$d.Number; FriendlyName = [string]$d.FriendlyName
            SerialNumber = ([string]$d.SerialNumber).Trim(); Size = [int64]$d.Size
            BusType = [string]$d.BusType; IsSystem = [bool]$d.IsSystem; IsBoot = [bool]$d.IsBoot
            HasPageFile = $hasPage; LogicalSectorSize = [int]$d.LogicalSectorSize
            UniqueId = [string]$d.UniqueId; Partitions = $parts; PartitionQueryFailed = $partsFailed
            Path = (Get-DevicePath $d.Number); Kind = 'device'; Head = $null; P3Block = $null
        }
        Add-DiskContent $disk
        $out += $disk
    }
    return $out
}

# count bytes of a disk, read-only, or $null when they cannot be read (on a
# real disk that includes not being elevated).
function Read-DiskBytes($Disk, [int64]$Offset, [int]$Count) {
    try {
        $h = [SonoraUsb.Native]::Open($Disk.Path, $Disk.Kind, $false, $true)
        try { $b = [SonoraUsb.Native]::ReadAt($h, $Offset, $Count) } finally { $h.Dispose() }
        return , $b
    } catch {
        return $null
    }
}

# The first MiB (blank or not, and our MBR and ISO descriptor) and, when the
# MBR is ours, the first 4 KiB of partition 3 (the ext4 superblock). Read for
# USB disks only: an internal disk is never opened, not even to read.
function Add-DiskContent($Disk) {
    if ([string]$Disk.BusType -ne 'USB') { return }
    $head = Read-DiskBytes $Disk 0 ([int]$script:MiB)
    $Disk.Head = $head
    $start3 = Get-SonoraP3Start $head
    if ($start3 -ge 0 -and ($start3 * 512 + 4096) -le [int64]$Disk.Size) {
        $Disk.P3Block = Read-DiskBytes $Disk ($start3 * 512) 4096
    }
}

function Show-Disk($Disk) {
    Write-Host ('  Disk ' + $Disk.Number + ': ' + $Disk.FriendlyName)
    Write-Host ('    size    ' + [math]::Round($Disk.Size / 1e9, 1) + ' GB (' + $Disk.Size + ' bytes)')
    Write-Host ('    serial  ' + $Disk.SerialNumber + '    bus ' + $Disk.BusType)
    $parts = @(Get-Field $Disk 'Partitions' @())
    if ($parts.Count -eq 0) { Write-Host '    no partitions' }
    foreach ($p in $parts) { Write-Host ('    ' + (Format-Partition $p)) }
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

function Test-Elevated {
    $me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    return $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
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

# The open handle must be the disk that was confirmed: a raw device by its
# own length, bus and serial, a fake disk by being a regular file of its size.
function Assert-HandleIsDisk($Handle, $Disk) {
    if ($Disk.Kind -eq 'device') {
        $len = [SonoraUsb.Native]::DiskLength($Handle)
        $bus = 0
        $serial = [SonoraUsb.Native]::DeviceSerial($Handle, [ref]$bus)
        if ($len -ne [int64]$Disk.Size) { return ('the open device is ' + $len + ' bytes, not ' + $Disk.Size + '.') }
        if ($bus -ne 7) { return ('the open device reports bus type ' + $bus + ', not USB (7).') }
        # Exact whenever Windows listed a serial. Without one, the identity
        # compared after the prompt (model, size, unique id, every partition's
        # offset, size, type and GUID) is what ties the handle to the disk.
        $want = ([string]$Disk.SerialNumber).Trim()
        if ($want -ne '' -and $serial.Trim() -cne $want) {
            return ('the open device reports serial "' + $serial + '", not "' + $want + '" as listed. Unplug other USB disks and run again.')
        }
        return ''
    }
    $len = [SonoraUsb.Native]::FileLength($Handle)
    if ($len -ne [int64]$Disk.Size) { return ('fake disk file is ' + $len + ' bytes, not ' + $Disk.Size + '.') }
    return ''
}

# --- the write ---

# Order matters. The first MiB -- the partition table and boot code -- is
# zeroed first and written last, so a write that stops anywhere leaves a stick
# Windows and firmware see as blank, never a table pointing at half an image.
# The last MiB is zeroed too: a stale backup GPT there makes some firmware and
# Windows itself report a damaged disk. The image is hashed as it is read, so
# what goes to the stick is what matched the sidecar, not what the file held
# a minute earlier.
function Write-ImageToDisk($Handle, $Disk, [string]$ImagePath, [int64]$ImageBytes, [string]$ExpectedHash) {
    if ($DryRun) { throw 'internal: a dry run reached the write.' }
    $in = [IO.File]::OpenRead($ImagePath)
    $sha = [Security.Cryptography.SHA256]::Create()
    $zero = New-Object byte[] $script:MiB
    try {
        [SonoraUsb.Native]::WriteAt($Handle, 0, $zero, $zero.Length)
        [SonoraUsb.Native]::WriteAt($Handle, [int64]$Disk.Size - $script:MiB, $zero, $zero.Length)

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
            [SonoraUsb.Native]::WriteAt($Handle, $offset, $buf, $padded)
            $offset += $n
            if ($offset -ge $nextReport) {
                Write-Host ('  ' + [math]::Floor(100 * $offset / $ImageBytes) + '%  ' + [math]::Floor($offset / $script:MiB) + ' MiB')
                $nextReport += [int64]$ImageBytes / 10
            }
        }
        [void]$sha.TransformFinalBlock($buf, 0, 0)
        $got = [BitConverter]::ToString($sha.Hash).Replace('-', '')
        if ($got -ne $ExpectedHash) {
            [SonoraUsb.Native]::Flush($Handle)
            throw ('the image changed while it was being written (SHA-256 now ' + $got + '). Its first MiB was not written, so the stick will not boot. Rebuild the image and run again.')
        }
        $padded = [int]([math]::Ceiling($headLen / 512) * 512)
        if ($padded -gt $headLen) { [Array]::Clear($head, $headLen, $padded - $headLen) }
        [SonoraUsb.Native]::WriteAt($Handle, 0, $head, $padded)
        [SonoraUsb.Native]::Flush($Handle)
        if ($Disk.Kind -eq 'device') { [void][SonoraUsb.Native]::UpdateProperties($Handle) }
    } finally {
        $sha.Dispose()
        $in.Dispose()
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
    if ($DryRun) { Write-Host 'DRY RUN: nothing will be written, and no confirmation is asked for.' }
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
    if ($Image -notmatch '^[A-Za-z]:\\') { Stop-Refused 'the image must be on a local drive letter, not a network path.' }
    $imageBytes = (Get-Item -LiteralPath $Image).Length
    $imageDrive = $Image.Substring(0, 1).ToUpperInvariant()

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
        if ($why -ne '') {
            $state = 'refused: ' + $why
        } else {
            $data = @(Get-DataPartitions $d)
            if ($data.Count -gt 0) { $state = 'has data: ' + (@($data | ForEach-Object { Format-Partition $_ }) -join '; ') }
        }
        Write-Host ('  ' + $d.Number + '  ' + [math]::Round($d.Size / 1e9, 1) + ' GB  ' + $d.FriendlyName + '  serial ' + $d.SerialNumber + '  -- ' + $state)
    }
    if ($List) { return 0 }

    if ($DiskNumber -lt 0) {
        if (-not (Test-Interactive)) { Stop-Refused 'no -DiskNumber, and there is nobody to ask.' }
        $answer = Read-Host 'Disk number (Enter to stop)'
        if ($answer -notmatch '^[0-9]+$') { Write-Host 'aborted.'; return 2 }
        $DiskNumber = [int]$answer
    }
    $target = @($disks | Where-Object { $_.Number -eq $DiskNumber })
    if ($target.Count -ne 1) { Stop-Refused ('there is no disk ' + $DiskNumber + '.') }
    $disk = $target[0]
    $why = Get-DiskRefusal $disk $imageBytes ([bool]$AllowLarge) $imageDrive
    if ($why -ne '') { Stop-Refused ('disk ' + $disk.Number + ': ' + $why) }
    if ($disk.Kind -eq 'device' -and -not (Test-DevicePath $disk.Path)) { Stop-Refused ('internal: malformed device path ' + $disk.Path) }
    if ($disk.Kind -eq 'file' -and ($disk.Path -notmatch '^[A-Za-z]:\\' -or -not (Test-Path -LiteralPath $disk.Path -PathType Leaf))) {
        Stop-Refused ('fake disk ' + $disk.Number + ' must be an existing regular file on a drive letter, not ' + $disk.Path)
    }
    $data = @(Get-DataPartitions $disk)
    # Not "$token": PowerShell names are case-insensitive, and a local by that
    # name would shadow -Token, which would then always match itself.
    $expected = Get-ConfirmToken $disk
    $dataToken = ''
    if ($data.Count -gt 0) { $dataToken = Get-DataToken $data }

    Write-Host ''
    Write-Host 'Target:'
    Show-Disk $disk
    if ($data.Count -gt 0) {
        Write-Host ''
        Write-Host 'THIS DISK HOLDS DATA. Everything on these partitions will be lost:'
        foreach ($p in $data) { Write-Host ('    ' + (Format-Partition $p)) }
    } elseif (Test-SonoraLayout $disk) {
        Write-Host '  (a previous SonoraOS stick: its persistence partition, with any sessions on it, will be lost)'
    }
    Write-Host ''

    if ($DryRun) {
        Write-Host ('DRY RUN: a real run would erase disk ' + $disk.Number + ' (' + $disk.FriendlyName + ') and write ' + $imageBytes + ' bytes to ' + $disk.Path + '.')
        Write-Host ('  It would ask you to type: ' + $expected)
        if ($dataToken -ne '') { Write-Host ('  and then, for the data on it: ' + $dataToken + '   (or pass -AllowDataVolumes)') }
        Write-Host 'DRY RUN: nothing was written.'
        return 0
    }

    # Before any question, so nobody types the token and is then told no.
    if ($disk.Kind -eq 'device' -and -not (Test-Elevated)) {
        Stop-Refused 'writing a disk needs an elevated PowerShell (Run as administrator).'
    }

    Write-Host ('EVERYTHING ON DISK ' + $disk.Number + ' (' + $disk.FriendlyName + ', ' + [math]::Round($disk.Size / 1e9, 1) + ' GB) WILL BE ERASED.')
    if ($Token -ne '') {
        if (-not (Test-HasSerial $disk)) { Stop-Refused ('disk ' + $disk.Number + ' reports no serial, so it can only be confirmed by typing at the prompt, not with -Token.') }
        if ($Token -cne $expected) { Stop-Refused ('-Token does not match this disk; it expects exactly: ' + $expected) }
    } else {
        if (-not (Test-Interactive)) { Stop-Refused ('not interactive; pass -Token "' + $expected + '" to write without a prompt.') }
        $reply = Read-Host ('Type  ' + $expected + '  to continue')
        if ($reply -cne $expected) { Write-Host 'aborted. Nothing was written.'; return 2 }
    }
    if ($data.Count -gt 0) {
        if ($AllowDataVolumes) {
            Write-Host '-AllowDataVolumes: the partitions listed above will be erased.'
        } else {
            if (-not (Test-Interactive)) { Stop-Refused ('disk ' + $disk.Number + ' holds data; pass -AllowDataVolumes to erase it without a prompt.') }
            $reply = Read-Host ('This disk holds data. Type  ' + $dataToken + '  to erase it')
            if ($reply -cne $dataToken) { Write-Host 'aborted. Nothing was written.'; return 2 }
        }
    }

    # Everything again, now that a human has had time to unplug one stick and
    # plug in another: the number may now name a different disk. From here on
    # only the re-read description is used.
    $again = @(@(Get-Disks) | Where-Object { $_.Number -eq $DiskNumber })
    if ($again.Count -ne 1 -or (Get-DiskIdentity $again[0]) -ne (Get-DiskIdentity $disk)) {
        Stop-Refused ('disk ' + $DiskNumber + ' is no longer the disk that was shown.')
    }
    $disk = $again[0]
    $why = Get-DiskRefusal $disk $imageBytes ([bool]$AllowLarge) $imageDrive
    if ($why -ne '') { Stop-Refused ('disk ' + $DiskNumber + ': ' + $why) }
    if ((Get-DataSignature @(Get-DataPartitions $disk)) -ne (Get-DataSignature $data)) {
        Stop-Refused ('what disk ' + $DiskNumber + ' holds changed since it was shown.')
    }

    $h = $null
    $locks = @()
    try {
        $h = [SonoraUsb.Native]::Open($disk.Path, $disk.Kind, $true, $false)
        $why = Assert-HandleIsDisk $h $disk
        if ($why -ne '') { Stop-Refused $why }
        # Windows refuses writes into a mounted volume's sectors. Each volume
        # on the stick is locked and dismounted, and the handles stay open
        # until the write is done so nothing remounts it in between.
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
    } catch {
        Write-Host ('error: ' + $_.Exception.Message)
        Write-Host 'Nothing was written.'
        foreach ($vh in $locks) { $vh.Dispose() }
        if ($null -ne $h) { $h.Dispose() }
        return 1
    }
    try {
        Write-Host ('Writing ' + $Image + ' to ' + $disk.Path + '...')
        Write-ImageToDisk $h $disk $Image $imageBytes $hash
    } catch {
        Write-Host ('error: ' + $_.Exception.Message)
        Write-Host 'The stick was erased but not fully written; it will not boot. Run again.'
        return 1
    } finally {
        $h.Dispose()
        foreach ($vh in $locks) { $vh.Dispose() }
    }

    Write-Host 'Verifying...'
    if (-not (Test-WrittenDisk $disk $Image $imageBytes)) {
        Write-Host 'error: the stick does not read back as written. Try another port or stick.'
        return 1
    }
    if ($persistent) {
        Write-Host ('Done: disk ' + $disk.Number + ' holds the image and the persistence partition.')
        Write-Host 'If Windows offers to format a drive on the stick, say Cancel: that is the persistence partition.'
    } else {
        Write-Host ('Done: disk ' + $disk.Number + ' holds the image, without persistence.')
    }
    return 0
}

# Dot-sourced by tools/test-write-usb.ps1 for the functions alone.
if ($MyInvocation.InvocationName -eq '.') { return }
exit (Invoke-Main)
