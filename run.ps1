$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$q = 'qemu-system-i386'
$qcmd = Get-Command $q -ErrorAction SilentlyContinue
if ($qcmd) { $q = $qcmd.Source }
if (-not $qcmd) {
    $cands = @('D:\qemu\qemu-system-i386.exe','C:\Program Files\qemu\qemu-system-i386.exe','C:\qemu\qemu-system-i386.exe')
    foreach ($c in $cands) { if (Test-Path $c) { $q = $c; break } }
}
if (-not (Test-Path $q)) { Write-Host "[ERR] qemu not found." -ForegroundColor Red; exit 1 }
Write-Host "[Run] Booting NovaOS..." -ForegroundColor Cyan
# NOTE: the QEMU builds on this box (6.2.0 and 8.0.0) both fault in ntdll
# during process *exit* on Win7 - cosmetic only (guest already powered off,
# exit code 0 via ACPI). The WER popup is suppressed via:
#   HKCU\Software\Microsoft\Windows\Windows Error Reporting\Excluded Applications
#   -> qemu-system-i386.exe = 1
& $q -drive format=raw,file=disk.img -m 256 -rtc base=localtime,clock=host -device isa-debug-exit,iobase=0x501,iosize=2 -serial file:serial.log
Write-Host "`n[QEMU exited] (code $LASTEXITCODE)" -ForegroundColor Green
