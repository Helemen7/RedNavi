$ErrorActionPreference = "Stop"
$disk = Get-Disk -Number 1
Write-Host "Target Disk: $($disk.FriendlyName) - BusType: $($disk.BusType)"
if ($disk.BusType -ne "USB") {
    throw "Disk 1 is not a USB drive!"
}

# Remove existing partitions if any
$partitions = Get-Partition -DiskNumber 1 -ErrorAction SilentlyContinue
if ($partitions) {
    $partitions | Remove-Partition -Confirm:$false
}

try {
    Initialize-Disk -Number 1 -PartitionStyle GPT -ErrorAction Stop
} catch {
    Write-Host "Disk already initialized as GPT."
}

Write-Host "Creating 16GB FAT32 partition..."
$p1 = New-Partition -DiskNumber 1 -Size 16GB -AssignDriveLetter
$v1 = Format-Volume -Partition $p1 -FileSystem FAT32 -NewFileSystemLabel "OPENCORE" -Confirm:$false

Write-Host "Creating remaining exFAT partition..."
$p2 = New-Partition -DiskNumber 1 -UseMaximumSize -AssignDriveLetter
$v2 = Format-Volume -Partition $p2 -FileSystem exFAT -NewFileSystemLabel "DATA" -Confirm:$false

Write-Host "Done. Volumes:"
Get-Volume | Where-Object DriveLetter -in @($p1.DriveLetter, $p2.DriveLetter) | Select-Object DriveLetter, FileSystemLabel, FileSystem, Size | Format-Table -AutoSize
