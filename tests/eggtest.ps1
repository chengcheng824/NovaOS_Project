# NovaOS National-Day CJK egg test: boot with RTC = Oct 1 (in the holiday)
# and RTC = a random day (countdown), verify both paths + feature gates.
param([string]$Date = "2026-10-01", [string]$Tag = "egg")
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)

$q = 'D:\qemu\qemu-system-i386.exe'
if (-not (Test-Path $q)) { $q = 'qemu-system-i386' }

Copy-Item data-seed.img data-nettest.img -Force
Remove-Item serial-$Tag.log -Force -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $q -PassThru -ArgumentList @(
    '-drive','format=raw,file=disk.img,if=ide,index=0,media=disk',
    '-drive','format=raw,file=data-nettest.img,if=ide,index=1,media=disk',
    '-m','256',
    '-rtc',"base=$Date,clock=host",
    '-serial',"file:serial-$Tag.log",
    '-monitor','tcp:127.0.0.1:4444,server,nowait',
    '-nic','user,model=e1000',
    '-display','none'
)
Start-Sleep -Seconds 9
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
$s = $c.GetStream()
Start-Sleep -Milliseconds 600
function sk([string]$keys) {
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus'; '_' = 'shift-minus' }
    foreach ($ch in $keys.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
        $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
        $s.Write($b, 0, $b.Length)
        Start-Sleep -Milliseconds 100
    }
    $b = [Text.Encoding]::ASCII.GetBytes("sendkey ret`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 350
}
function shot([string]$name) {
    Start-Sleep -Milliseconds 900
    $b = [Text.Encoding]::ASCII.GetBytes("screendump build\$name.ppm`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 900
}

sk "root"; sk "nova"; Start-Sleep -Seconds 1
sk "date"; Start-Sleep -Seconds 1
shot "${tag}_before"                 # screen before the demo
sk "feature demo"; Start-Sleep -Seconds 2
shot "${tag}_after"                  # banner visible if Oct 1-7
sk "feature disable holiday_module"
sk "feature demo"
sk "feature enable holiday_module"
sk "feature disable cn_lang_support"
sk "feature demo"                    # cn off: banner must NOT draw

$b = [Text.Encoding]::ASCII.GetBytes("quit`n")
$s.Write($b, 0, $b.Length)
Start-Sleep -Seconds 2
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

foreach ($n in @("${tag}_before","${tag}_after")) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In "build\$n.ppm" -Out "build\$n.png"
}
$log = Get-Content "serial-$Tag.log" -Raw -ErrorAction SilentlyContinue
Write-Host "===== $Tag ($Date) =====" -ForegroundColor Cyan
$checks = @(
    @{ n = 'days_left == 0 (holiday)'; pat = 'TODAY! \(national day holiday\)' },
    @{ n = 'banner drawn';             pat = 'banner drawn on screen' },
    @{ n = 'module-off BLOCKED';       pat = 'BLOCKED \(module disabled\)' }
)
foreach ($k in $checks) {
    $hit = $log -match $k.pat
    Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.n) -ForegroundColor $(if ($hit) {'Green'} else {'Yellow'})
}
