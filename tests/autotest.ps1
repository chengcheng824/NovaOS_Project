# NovaOS automated network smoke test (requires QEMU monitor on 4444)
$ErrorActionPreference = 'Continue'
Start-Sleep -Seconds 8                                 # wait for the login prompt

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

sk "root";  Start-Sleep -Milliseconds 400              # login
sk "nova";  Start-Sleep -Milliseconds 400
sk "netinfo";      Start-Sleep -Seconds 2              # mac / ip / gateway
sk "ping 10.0.2.2"; Start-Sleep -Seconds 8             # 4 icmp echoes
sk "udpecho";      Start-Sleep -Seconds 2              # guest echo loop

# host -> guest -> host udp roundtrip through the 7777 forward
$u = New-Object Net.Sockets.UdpClient                 # ephemeral local port
$u.Client.ReceiveTimeout = 5000
$b = [Text.Encoding]::ASCII.GetBytes("hello novaos")
$u.Connect('127.0.0.1', 7777); [void]$u.Send($b, $b.Length)
$ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
try {
    $r = $u.Receive([ref]$ep)
    Write-Host ("UDP ROUNDTRIP OK: " + [Text.Encoding]::ASCII.GetString($r)) -ForegroundColor Green
} catch {
    Write-Host "UDP ROUNDTRIP FAILED (no reply in 5s)" -ForegroundColor Red
}
$u.Close()
Start-Sleep -Seconds 1

sk "q"                                                 # leave udpecho
Start-Sleep -Milliseconds 500
sk "run snake.nxp"; Start-Sleep -Seconds 4             # spawn the game
sk q                                                   # quit it
Start-Sleep -Milliseconds 800
sk "exit"                                              # leave nsh -> kernel shell... (nsh not started; exit is harmless at shell)
Start-Sleep -Milliseconds 400
$c.Close()
Write-Host "AUTOTEST DONE - read serial.log" -ForegroundColor Cyan
