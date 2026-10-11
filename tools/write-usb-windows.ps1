<#
.SYNOPSIS
  Write sonora.iso to a USB stick from Windows.

.DESCRIPTION
  tools/write-usb.sh needs /dev/sdX, parted and mkfs.ext4, none of which exist
  on Windows. This writes the hybrid ISO to a physical drive with dd, which is
  enough to boot: ADR-0005 documents the ISO alone as the supported fallback.

  The ext4 persistence partition is NOT created here, because Windows cannot
  make an ext4 filesystem. Create it from inside the booted system instead,
  which has parted and mkfs.ext4 already -- see the note printed at the end.
  Until then, sessions live in RAM and are lost on shutdown, which is fine for
  a first latency measurement and not fine for real work.

.PARAMETER DiskNumber
  The disk to erase, as reported by Get-Disk. Required, and never guessed.

.EXAMPLE
  # List candidates first. Always.
  powershell -File tools\write-usb-windows.ps1 -List

.EXAMPLE
  powershell -File tools\write-usb-windows.ps1 -DiskNumber 2
#>
[CmdletBinding()]
param(
  [int] $DiskNumber = -1,
  [switch] $List,
  [string] $IsoPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $IsoPath) { $IsoPath = Join-Path $repoRoot 'iso\sonora.iso' }

function Show-Disks {
  Write-Host ''
  Write-Host 'Disks on this machine:' -ForegroundColor Cyan
  Get-Disk | Sort-Object Number | ForEach-Object {
    $size = [math]::Round($_.Size / 1GB, 1)
    $flag = if ($_.BusType -eq 'USB') { 'USB' } else { "$($_.BusType)" }
    $boot = if ($_.IsBoot) { '  <-- SYSTEM DISK, DO NOT TOUCH' } else { '' }
    Write-Host ("  {0}  {1,7} GB  {2,-8} {3}{4}" -f `
      $_.Number, $size, $flag, $_.FriendlyName, $boot)
  }
  Write-Host ''
  Write-Host 'Pick the USB one by its number, and check the size matches the' -ForegroundColor Yellow
  Write-Host 'stick you intend to erase. Writing to the wrong disk is not' -ForegroundColor Yellow
  Write-Host 'recoverable.' -ForegroundColor Yellow
  Write-Host ''
}

if ($List -or $DiskNumber -lt 0) {
  Show-Disks
  if (-not $List) {
    Write-Host 'No -DiskNumber given, so nothing was written.' -ForegroundColor Red
    Write-Host 'Re-run with: -DiskNumber <n>'
  }
  exit 0
}

# --- preconditions ---
if (-not (Test-Path $IsoPath)) {
  throw "$IsoPath not found. Build it first with tools/build-iso.sh."
}

$isAdmin = ([Security.Principal.WindowsPrincipal] `
  [Security.Principal.WindowsIdentity]::GetCurrent()
  ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  throw 'Raw disk access needs an elevated PowerShell. Run it as Administrator.'
}

$disk = Get-Disk -Number $DiskNumber
if ($disk.IsBoot -or $disk.IsSystem) {
  throw "Disk $DiskNumber is the system disk. Refusing."
}
if ($disk.BusType -ne 'USB') {
  # Not fatal -- an internal SSD could legitimately be a target -- but it is
  # overwhelmingly more likely to be a mistake.
  Write-Host ''
  Write-Host "WARNING: disk $DiskNumber is $($disk.BusType), not USB." -ForegroundColor Red
}

$isoSize = (Get-Item $IsoPath).Length
if ($disk.Size -lt $isoSize) {
  throw ("Disk $DiskNumber is {0:N1} GB; the image needs {1:N1} GB." -f `
    ($disk.Size / 1GB), ($isoSize / 1GB))
}

# --- confirm, out loud, with the real device named ---
Write-Host ''
Write-Host 'About to ERASE:' -ForegroundColor Red
Write-Host ("  Disk {0}: {1}  {2:N1} GB  ({3})" -f `
  $disk.Number, $disk.FriendlyName, ($disk.Size / 1GB), $disk.BusType)
Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue |
  ForEach-Object {
    $letter = if ($_.DriveLetter) { "$($_.DriveLetter):" } else { '(no letter)' }
    Write-Host ("    partition {0}  {1,7:N1} GB  {2}" -f `
      $_.PartitionNumber, ($_.Size / 1GB), $letter)
  }
Write-Host ''
$reply = Read-Host 'Everything on this disk will be destroyed. Type ERASE to continue'
if ($reply -ne 'ERASE') { Write-Host 'Aborted.'; exit 1 }

# --- write ---
# Offline first: Windows holds handles on mounted volumes and the write fails
# partway with a sharing violation, leaving a stick that half boots.
Write-Host 'Taking the disk offline and clearing it...'
Set-Disk -Number $DiskNumber -IsOffline $true
try {
  Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false
} catch {
  Write-Host "  (clear reported: $($_.Exception.Message))"
}
Set-Disk -Number $DiskNumber -IsOffline $false
Start-Sleep -Seconds 2
Set-Disk -Number $DiskNumber -IsOffline $true

Write-Host "Writing $([math]::Round($isoSize / 1MB)) MB. This takes a while."

$target = "\.\PhysicalDrive$DiskNumber"
$bufferSize = 4MB
$source = $null
$dest = $null
try {
  $source = [System.IO.File]::OpenRead($IsoPath)
  $dest = New-Object System.IO.FileStream(
    $target, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::None)

  $buffer = New-Object byte[] $bufferSize
  $written = 0L
  $lastReport = 0L
  while (($read = $source.Read($buffer, 0, $bufferSize)) -gt 0) {
    $dest.Write($buffer, 0, $read)
    $written += $read
    if ($written - $lastReport -ge 64MB) {
      $pct = [math]::Round(100.0 * $written / $isoSize)
      Write-Progress -Activity 'Writing SonoraOS' `
        -Status ("{0} MB of {1} MB" -f [math]::Round($written/1MB), [math]::Round($isoSize/1MB)) `
        -PercentComplete $pct
      $lastReport = $written
    }
  }
  $dest.Flush()
  Write-Progress -Activity 'Writing SonoraOS' -Completed
  Write-Host ("Wrote {0} MB." -f [math]::Round($written / 1MB)) -ForegroundColor Green
} finally {
  if ($dest) { $dest.Dispose() }
  if ($source) { $source.Dispose() }
  Set-Disk -Number $DiskNumber -IsOffline $false -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host 'Done. Before booting:' -ForegroundColor Cyan
Write-Host '  - Disable Secure Boot. The bootloader is unsigned (ADR-0005).'
Write-Host '  - Connect the audio interface before powering on, so PipeWire'
Write-Host '    sees it at startup. The engine waits 60s for it anyway.'
Write-Host ''
Write-Host 'No persistence partition was created: Windows cannot make ext4.' -ForegroundColor Yellow
Write-Host 'Sessions will live in RAM and vanish on shutdown. To add it, boot'
Write-Host 'the stick and run, inside SonoraOS:'
Write-Host ''
Write-Host '    sudo tools/add-persistence.sh /dev/sdX' -ForegroundColor White
Write-Host ''
