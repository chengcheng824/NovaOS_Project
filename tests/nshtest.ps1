# NovaOS nsh.nxp network command test (sysop path from Ring3)
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
    '-nic','user,model=e1000,hostfwd=udp::7777-:7777',
    '-display','none'
)

Start-Sleep -Seconds 9
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
$s = $c.GetStream()
Start-Sleep -Milliseconds 600

function sk([string]$keys) {
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus' }
    foreach ($ch in $keys.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
        $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
        $s.Write($b, 0, $b.Length)
        Start-Sleep -Milliseconds 120
    }
    $b = [Text.Encoding]::ASCII.GetBytes("sendkey ret`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 300
}

sk "root"; sk "nova"; Start-Sleep -Seconds 1
sk "run nsh.nxp";  Start-Sleep -Seconds 3      # enter the Ring3 shell
sk "netinfo";      Start-Sleep -Seconds 5      # sysop 12 (auto-dhcp inside)
sk "ping 10.0.2.2"; Start-Sleep -Seconds 10   # sysop 13
sk "exit";         Start-Sleep -Seconds 2      # back to kernel shell

$b = [Text.Encoding]::ASCII.GetBytes("quit`n")
$s.Write($b, 0, $b.Length)
Start-Sleep -Seconds 2
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
$lines = $log -split "`n"
Write-Host "===== nsh test result =====" -ForegroundColor Cyan
$lines | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
