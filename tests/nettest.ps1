# NovaOS network smoke test
# Boots with a SCRATCH data disk (never touches data.img), drives the shell
# through the QEMU monitor on 127.0.0.1:4444 and checks serial-nettest.log.
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)

$q = 'qemu-system-i386'
$qcmd = Get-Command $q -ErrorAction SilentlyContinue
if ($qcmd) { $q = $qcmd.Source }
if (-not $qcmd) {
    foreach ($c in @('D:\qemu\qemu-system-i386.exe','C:\Program Files\qemu\qemu-system-i386.exe')) {
        if (Test-Path $c) { $q = $c; break }
    }
}

Copy-Item data-seed.img data-nettest.img -Force
Remove-Item serial-nettest.log -Force -ErrorAction SilentlyContinue

Write-Host "[NetTest] booting QEMU (headless)..." -ForegroundColor Cyan
$proc = Start-Process -FilePath $q -PassThru -ArgumentList @(
    '-drive','format=raw,file=disk.img,if=ide,index=0,media=disk',
    '-drive','format=raw,file=data-nettest.img,if=ide,index=1,media=disk',
    '-m','256','-rtc','base=localtime,clock=host',
    '-serial','file:serial-nettest.log',
    '-monitor','tcp:127.0.0.1:4444,server,nowait',
    '-nic','user,model=e1000,hostfwd=udp::7777-:7777',
    '-display','none'
)

Start-Sleep -Seconds 9           # boot + selftests + login prompt
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
$s = $c.GetStream()
Start-Sleep -Milliseconds 600

function sk([string]$keys) {
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus' }
    foreach ($ch in $keys.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
        $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
        $s.Write($b, 0, $b.Length)
        Start-Sleep -Milliseconds 90
    }
    $b = [Text.Encoding]::ASCII.GetBytes("sendkey ret`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 250
}

sk "root";  Start-Sleep -Milliseconds 500       # login
sk "nova";  Start-Sleep -Milliseconds 500

sk "netinfo";       Start-Sleep -Seconds 5      # auto-dhcp + status
sk "ping 10.0.2.2"; Start-Sleep -Seconds 10     # 4 ICMP echoes to the gateway
sk "dns example.com"; Start-Sleep -Seconds 8    # SLIRP DNS forwarder
sk "udpecho";       Start-Sleep -Seconds 2      # UDP echo server on :7777

# host -> guest -> host UDP roundtrip through the 7777 forward
$udpOk = $false
$u = New-Object Net.Sockets.UdpClient
$u.Client.ReceiveTimeout = 6000
$b = [Text.Encoding]::ASCII.GetBytes("hello novaos")
$u.Connect('127.0.0.1', 7777); [void]$u.Send($b, $b.Length)
$ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
try {
    $r = $u.Receive([ref]$ep)
    $txt = [Text.Encoding]::ASCII.GetString($r)
    $udpOk = ($txt -eq "hello novaos")
    Write-Host ("UDP ROUNDTRIP: " + $txt) -ForegroundColor $(if($udpOk){'Green'}else{'Yellow'})
} catch {
    Write-Host "UDP ROUNDTRIP FAILED (no reply in 6s)" -ForegroundColor Red
}
$u.Close()

Start-Sleep -Seconds 1
sk "q"                                          # leave udpecho
Start-Sleep -Milliseconds 800

$b = [Text.Encoding]::ASCII.GetBytes("quit`n")  # stop QEMU via monitor
$s.Write($b, 0, $b.Length)
Start-Sleep -Seconds 2
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 500

# ---- report ----
$log = (Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue)
Write-Host "`n===== serial-nettest.log (tail) =====" -ForegroundColor Cyan
if ($log) {
    $lines = $log -split "`n"
    $lines | Select-Object -Last 60 | ForEach-Object { Write-Host $_ }
    Write-Host "===== summary =====" -ForegroundColor Cyan
    $checks = @(
        @{ name = 'boot net tag';    pat = '\[\s*OK\s*\] net' },
        @{ name = 'DHCP lease';      pat = 'dhcp: IP 10\.0\.2\.15' },
        @{ name = 'ICMP replies';    pat = 'Reply from 10\.0\.2\.2' },
        @{ name = 'DNS answer';      pat = 'example\.com -> ' },
        @{ name = 'UDP echo logged'; pat = '\[udp\] echoed' }
    )
    foreach ($k in $checks) {
        $hit = $log -match $k.pat
        Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.name) -ForegroundColor $(if ($hit) {'Green'} else {'Yellow'})
    }
    if ($udpOk) { Write-Host "  [OK] host->guest->host UDP roundtrip" -ForegroundColor Green }
    else        { Write-Host "  [--] host->guest->host UDP roundtrip" -ForegroundColor Yellow }
} else {
    Write-Host "(no serial log produced)" -ForegroundColor Red
}
