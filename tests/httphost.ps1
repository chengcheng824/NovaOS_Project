# minimal HTTP/1.0 server on 127.0.0.1:8080 for NovaOS wget testing.
# Serves ~10 KB of text (multi-segment TCP flow), logs to build\httpserver.log,
# exits after MaxConns requests.
param([int]$Port = 8080, [int]$MaxConns = 8)
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)
Remove-Item build\httpserver.log -Force -ErrorAction SilentlyContinue

$l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $Port)
$l.Start()

$body = New-Object Text.StringBuilder
[void]$body.AppendLine("NovaOS wget test page")
for ($k = 1; $k -le 150; $k++) {
    [void]$body.AppendLine("line $k : 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ the quick brown fox jumps over")
}
$b = [Text.Encoding]::ASCII.GetBytes($body.ToString())

for ($i = 0; $i -lt $MaxConns; $i++) {
    $c = $l.AcceptTcpClient()
    $s = $c.GetStream()
    $s.ReadTimeout = 5000
    $buf = New-Object byte[] 4096
    $req = ''
    try { $n = $s.Read($buf, 0, $buf.Length); $req = [Text.Encoding]::ASCII.GetString($buf, 0, $n) } catch {}
    $hdr = "HTTP/1.0 200 OK`r`nContent-Type: text/plain`r`nContent-Length: $($b.Length)`r`nConnection: close`r`n`r`n"
    $h = [Text.Encoding]::ASCII.GetBytes($hdr)
    $s.Write($h, 0, $h.Length)
    $s.Write($b, 0, $b.Length)
    $s.Close(); $c.Close()
    $first = ($req -split "`n")[0].Trim()
    Add-Content -Path build\httpserver.log -Value "$first -> sent $($b.Length) B"
}
$l.Stop()
Add-Content -Path build\httpserver.log -Value "server done"
