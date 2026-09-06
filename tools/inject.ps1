# inject.ps1 - add/reprogram .nxp files in an EXISTING NovaFS image without
# formatting (so accounts and user files survive a kernel/program update).
#   tools\inject.ps1 -Image data.img -Programs tui,nsh
# For each NAME it injects the four slot binaries from build\:
#   nxp_NAME0.bin -> NAME.nxp, nxp_NAME1.bin -> NAME.1.nxp, ...
# The image is backed up to IMAGE.bak unless -NoBackup is given.
param(
    [string]$Image = 'data.img',
    [string]$Programs = '',
    [switch]$NoBackup
)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

$SB  = 129 * 512
$INO = 130 * 512
$BMP = 162 * 512
$DAT = 170 * 512
$MAXBLOCKS = 32768
$MAXINODES = 256

if (-not (Test-Path $Image)) { Write-Host "[ERR] $Image not found" -ForegroundColor Red; exit 1 }
$bytes = [IO.File]::ReadAllBytes($Image)
if ($bytes.Length -lt ($DAT + $MAXBLOCKS * 512)) { Write-Host "[ERR] image too small" -ForegroundColor Red; exit 1 }
$magic = [BitConverter]::ToUInt32($bytes, $SB)
if ($magic -ne 0x4E584653) { Write-Host "[ERR] not a NovaFS image (bad magic)" -ForegroundColor Red; exit 1 }

if (-not $NoBackup) {
    Copy-Item $Image "$Image.bak" -Force
    Write-Host "[inject] backup -> $Image.bak"
}

function Get-Bit([int]$i) { return (($bytes[$BMP + ($i -shr 3)] -shr ($i -band 7)) -band 1) }
function Set-Bit([int]$i) { $bytes[$BMP + ($i -shr 3)] = $bytes[$BMP + ($i -shr 3)] -bor [byte](1 -shl ($i -band 7)) }
function Clear-Bit([int]$i) { $bytes[$BMP + ($i -shr 3)] = $bytes[$BMP + ($i -shr 3)] -band [byte]([byte]255 -bxor (1 -shl ($i -band 7))) }

function Get-FreeBlock {
    for ($b = 1; $b -lt $MAXBLOCKS; $b++) {
        if ((Get-Bit $b) -eq 0) { Set-Bit $b; return $b }
    }
    throw "no free blocks"
}

function Get-FreeInode {
    for ($i = 1; $i -lt $MAXINODES; $i++) {
        if ($bytes[$INO + $i * 64] -eq 0) { return $i }
    }
    throw "no free inodes"
}

function Find-Inode([string]$name) {
    $nb = [Text.Encoding]::ASCII.GetBytes($name)
    for ($i = 1; $i -lt $MAXINODES; $i++) {
        $base = $INO + $i * 64
        if ($bytes[$base] -ne 1) { continue }                       # T_FILE
        $parent = [BitConverter]::ToUInt16($bytes, $base + 2)
        if ($parent -ne 0) { continue }                             # root dir only
        $ok = $true
        for ($k = 0; $k -lt $nb.Length; $k++) {
            if ($bytes[$base + 4 + $k] -ne $nb[$k]) { $ok = $false; break }
        }
        if ($ok -and $bytes[$base + 4 + $nb.Length] -eq 0) { return $i }
    }
    return -1
}

function Free-Blocks([int]$ino) {
    $base = $INO + $ino * 64
    for ($b = 0; $b -lt 6; $b++) {
        $blk = [BitConverter]::ToUInt32($bytes, $base + 32 + $b * 4)
        if ($blk) { Clear-Bit $blk }
    }
    $ind = [BitConverter]::ToUInt32($bytes, $base + 56)
    if ($ind) {
        for ($e = 0; $e -lt 128; $e++) {
            $blk = [BitConverter]::ToUInt32($bytes, $DAT + $ind * 512 + $e * 4)
            if ($blk) { Clear-Bit $blk }
        }
        Clear-Bit $ind
    }
}

function Write-FileInode([int]$ino, [byte[]]$data) {
    $base = $INO + $ino * 64
    Free-Blocks $ino
    $nb = [Math]::Ceiling($data.Length / 512.0)
    for ($b = 0; $b -lt [Math]::Min(6, $nb); $b++) {
        $blk = Get-FreeBlock
        [BitConverter]::GetBytes([uint32]$blk).CopyTo($bytes, $base + 32 + $b * 4)
        $n = [Math]::Min(512, $data.Length - $b * 512)
        [Array]::Copy($data, $b * 512, $bytes, $DAT + $blk * 512, $n)
    }
    if ($nb -gt 6) {
        $ind = Get-FreeBlock
        [BitConverter]::GetBytes([uint32]$ind).CopyTo($bytes, $base + 56)
        [Array]::Clear($bytes, $DAT + $ind * 512, 512)
        for ($b = 6; $b -lt $nb; $b++) {
            $blk = Get-FreeBlock
            [BitConverter]::GetBytes([uint32]$blk).CopyTo($bytes, $DAT + $ind * 512 + ($b - 6) * 4)
            $n = [Math]::Min(512, $data.Length - $b * 512)
            [Array]::Copy($data, $b * 512, $bytes, $DAT + $blk * 512, $n)
        }
    }
    [BitConverter]::GetBytes([uint32]$data.Length).CopyTo($bytes, $base + 28)
}

function Inject-File([string]$fname, [byte[]]$data) {
    if ($fname.Length -gt 23) { throw "name too long: $fname" }
    $ino = Find-Inode $fname
    if ($ino -lt 0) {
        $ino = Get-FreeInode
        $base = $INO + $ino * 64
        $bytes[$base + 0] = 1                                   # T_FILE
        $bytes[$base + 1] = 0                                   # owner: root
        [BitConverter]::GetBytes([uint16]0).CopyTo($bytes, $base + 2)
        $nb = [Text.Encoding]::ASCII.GetBytes($fname)
        [Array]::Clear($bytes, $base + 4, 24)
        [Array]::Copy($nb, 0, $bytes, $base + 4, $nb.Length)
        Write-Host ("    + $fname (new inode {0})" -f $ino) -ForegroundColor Gray
    } else {
        Write-Host ("    + $fname (replacing inode {0})" -f $ino) -ForegroundColor Gray
    }
    Write-FileInode $ino $data
}

$names = $Programs -split ',' | Where-Object { $_ }
if (-not $names) { Write-Host "[ERR] -Programs tui,nsh (...) required" -ForegroundColor Red; exit 1 }

foreach ($n in $names) {
    for ($s = 0; $s -lt 4; $s++) {
        $src = "build\nxp_$n$s.bin"
        if (-not (Test-Path $src)) { Write-Host "[ERR] missing $src (run build.ps1)" -ForegroundColor Red; exit 1 }
        $code = [IO.File]::ReadAllBytes($src)
        $file = New-Object byte[] (4 + $code.Length)
        $file[0] = 0x4E; $file[1] = 0x58; $file[2] = 0x50; $file[3] = 0x01   # 'NXP\x01'
        [Array]::Copy($code, 0, $file, 4, $code.Length)
        if ($file.Length -gt 68608) { Write-Host "[ERR] $n too big" -ForegroundColor Red; exit 1 }
        $fname = if ($s -eq 0) { "$n.nxp" } else { "$n.$s.nxp" }
        Inject-File $fname $file
    }
}

[IO.File]::WriteAllBytes($Image, $bytes)
Write-Host "[inject] OK -> $Image" -ForegroundColor Green
