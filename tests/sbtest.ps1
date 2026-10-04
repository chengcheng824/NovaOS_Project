# NovaOS console scrollback test: fill the screen, wheel up, verify older
# lines redrawn; keypress returns to live view.
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
    '-qmp','tcp:127.0.0.1:4445,server,nowait',
    '-nic','user,model=e1000',
    '-display','none'
)
Start-Sleep -Seconds 9

$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
$s = $c.GetStream()
Start-Sleep -Milliseconds 600
function mon([string]$cmd) {
    $b = [Text.Encoding]::ASCII.GetBytes("$cmd`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 130
}
function sk([string]$keys) {
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus' }
    foreach ($ch in $keys.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
        mon "sendkey $k"
    }
    mon "sendkey ret"
    Start-Sleep -Milliseconds 350
}
function shot([string]$name) {
    Start-Sleep -Milliseconds 700
    mon "screendump build\$name.ppm"
    Start-Sleep -Milliseconds 700
}
$qc = New-Object Net.Sockets.TcpClient('127.0.0.1', 4445)
$qs = $qc.GetStream()
Start-Sleep -Milliseconds 600
function qdrain() {
    if ($qs.DataAvailable) {
        $buf = New-Object byte[] 8192
        try { $n = $qs.Read($buf, 0, $buf.Length)
              return [Text.Encoding]::ASCII.GetString($buf, 0, $n) } catch {}
    }
    return ''
}
$null = qdrain
function qmp([string]$json) {
    $b = [Text.Encoding]::ASCII.GetBytes($json + "`n")
    $qs.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 100
    return (qdrain)
}
$null = qmp '{"execute":"qmp_capabilities"}'
function wheel([int]$notches) {
    for ($i = 0; $i -lt [Math]::Abs($notches); $i++) {
        $dir = if ($notches -gt 0) { 'wheel-up' } else { 'wheel-down' }
        $null = qmp ('{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"' + $dir + '"}}]}}')
        $null = qmp ('{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"' + $dir + '"}}]}}')
        Start-Sleep -Milliseconds 120
    }
}

sk "root"; Start-Sleep -Seconds 1
sk "help"; Start-Sleep -Seconds 1        # screen 1 of content
sk "help"; Start-Sleep -Seconds 1        # screen 2 (older pushed to scrollback)
sk "ver"; Start-Sleep -Seconds 1
shot "sb_live"                           # bottom: ver output
wheel 15                                 # wheel UP 15 notches
Start-Sleep -Seconds 1
shot "sb_up"                             # should show earlier lines
wheel -15                                # back down to live
Start-Sleep -Seconds 1
shot "sb_back"
sk "shutdown"
Start-Sleep -Seconds 3
$c.Close(); $qc.Close()
if (-not $proc.HasExited) { $proc.Kill() }
foreach ($n in @('sb_live','sb_up','sb_back')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In "build\$n.ppm" -Out "build\$n.png"
}
Write-Host "shots done - read the PNGs"
