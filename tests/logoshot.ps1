# boot logo screenshot: boot, wait for banner, screendump before login keys
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)
$q = 'D:\qemu\qemu-system-i386.exe'
if (-not (Test-Path $q)) { $q = 'qemu-system-i386' }
Copy-Item data-seed.img data-nettest.img -Force
Remove-Item serial-nettest.log -Force -ErrorAction SilentlyContinue
$proc = Start-Process -FilePath $q -PassThru -ArgumentList @(
    '-drive','format=raw,file=disk.img,if=ide,index=0,media=disk',
    '-drive','format=raw,file=data-nettest.img,if=ide,index=1,media=disk',
    '-m','256','-rtc','base=localtime,clock=host',
    '-serial','file:serial-nettest.log',
    '-monitor','tcp:127.0.0.1:4444,server,nowait',
    '-nic','user,model=e1000',
    '-display','none'
)
Start-Sleep -Seconds 6
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
$s = $c.GetStream()
Start-Sleep -Milliseconds 600
$b = [Text.Encoding]::ASCII.GetBytes("screendump build\logo.ppm`n")
$s.Write($b, 0, $b.Length)
Start-Sleep -Milliseconds 900
# also capture the post-banner state (login + tagline visible)
Start-Sleep -Seconds 4
$b = [Text.Encoding]::ASCII.GetBytes("screendump build\logo_login.ppm`n")
$s.Write($b, 0, $b.Length)
Start-Sleep -Milliseconds 900
$b = [Text.Encoding]::ASCII.GetBytes("quit`n")
$s.Write($b, 0, $b.Length)
Start-Sleep -Seconds 2
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }
& powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In build\logo.ppm -Out build\logo.png
& powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In build\logo_login.ppm -Out build\logo_login.png
