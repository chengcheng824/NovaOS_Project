# NovaOS mouse-wheel test: IntelliMouse enable + QMP-injected wheel events
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

# HMP channel (sendkey / screendump)
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
$s = $c.GetStream()
Start-Sleep -Milliseconds 600
function mon([string]$cmd) {
    $b = [Text.Encoding]::ASCII.GetBytes("$cmd`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 150
}

# QMP channel (input injection - this HMP build lacks input-send-event)
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
$null = qdrain                                              # greeting
function qmp([string]$json) {
    $b = [Text.Encoding]::ASCII.GetBytes($json + "`n")
    $qs.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 120
    return (qdrain)
}
$cap = qmp '{"execute":"qmp_capabilities"}'
if ($cap -notmatch 'return') { Write-Host "QMP capabilities failed: $cap" -ForegroundColor Red }

function wheel([int]$notches) {           # -1 = down, +1 = up
    for ($i = 0; $i -lt [Math]::Abs($notches); $i++) {
        $dir = if ($notches -gt 0) { 'wheel-up' } else { 'wheel-down' }
        $r1 = qmp ('{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"' + $dir + '"}}]}}')
        $r2 = qmp ('{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"' + $dir + '"}}]}}')
        if (($r1 + $r2) -match '"error"') {
            $clean = (($r1 + $r2) -replace '\s+', ' ')
            Write-Host ("  QMP wheel error: " + $clean.Substring(0, [Math]::Min(220, $clean.Length))) -ForegroundColor Yellow
            return
        }
        Start-Sleep -Milliseconds 150
    }
}
function qmove([int]$dx, [int]$dy) {      # plumbing check: plain move via QMP
    $r1 = qmp ('{"execute":"input-send-event","arguments":{"events":[{"type":"rel","data":{"axis":"x","value":' + $dx + '}}]}}')
    $r2 = qmp ('{"execute":"input-send-event","arguments":{"events":[{"type":"rel","data":{"axis":"y","value":' + $dy + '}}]}}')
    if (($r1 + $r2) -match '"error"') {
        $clean = (($r1 + $r2) -replace '\s+', ' ')
        Write-Host ("  QMP move error: " + $clean.Substring(0, [Math]::Min(220, $clean.Length))) -ForegroundColor Yellow
    }
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

sk "root"; Start-Sleep -Seconds 1
sk "run tui.nxp"; Start-Sleep -Seconds 3
shot "wheel_before"                       # selection on the first entry
qmove 100 0                               # plumbing check: plain move
Start-Sleep -Milliseconds 500
shot "dbg_move"
wheel -3                                   # scroll DOWN 3 entries
Start-Sleep -Seconds 1
shot "wheel_after"
sk "q"; Start-Sleep -Seconds 1
sk "shutdown"
Start-Sleep -Seconds 3
$c.Close(); $qc.Close()
if (-not $proc.HasExited) { $proc.Kill() }

$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
Write-Host "===== wheel checks =====" -ForegroundColor Cyan
$hit = $log -match '\[mouse\] id=3 wheel on'
Write-Host ("  [{0}] wheel mode enabled (id=3)" -f ($(if ($hit) {'OK'} else {'--'}))) -ForegroundColor $(if ($hit) {'Green'} else {'Yellow'})
foreach ($n in @('wheel_before','dbg_move','wheel_after')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In "build\$n.ppm" -Out "build\$n.png"
}
