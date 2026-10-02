# NovaSh command-history test: recall with up/down, draft restore, dedup
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
        Start-Sleep -Milliseconds 100
    }
    $b = [Text.Encoding]::ASCII.GetBytes("sendkey ret`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 350
}
function rawkey([string]$k, [int]$ms = 150) {
    $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds $ms
}
function shot([string]$name) {
    Start-Sleep -Milliseconds 700
    $b = [Text.Encoding]::ASCII.GetBytes("screendump build\$name.ppm`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 700
}

sk "root"; Start-Sleep -Seconds 1
sk "whoami"                        # hist: whoami
sk "ver"                           # hist: ver
sk "echo hi"                       # hist: echo hi
sk "echo hi"                       # duplicate: deduped in history
# recall whoami (up x3) and run it
rawkey "up"; rawkey "up"; rawkey "up"
shot "hist_whoami"                 # screen should show 'whoami' on the line
rawkey "ret"; Start-Sleep -Seconds 1
# recall echo hi (up x2) and run it
rawkey "up"; rawkey "up"
shot "hist_echo"
rawkey "ret"; Start-Sleep -Seconds 1
# up then down -> back to empty draft, enter = empty command
rawkey "up"; rawkey "down"
shot "hist_draft"
rawkey "ret"; Start-Sleep -Seconds 1
sk "shutdown"
Start-Sleep -Seconds 3
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
Write-Host "===== history checks =====" -ForegroundColor Cyan
$checks = @(
    @{ n = 'ver output (v0.5)';          pat = 'v0\.5' },
    @{ n = 'echo hi ran at least twice'; pat = 'echo hi[\s\S]*hi[\s\S]*echo hi[\s\S]*hi' }
)
foreach ($k in $checks) {
    $hit = $log -match $k.pat
    Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.n) -ForegroundColor $(if ($hit) {'Green'} else {'Yellow'})
}
foreach ($n in @('hist_whoami','hist_echo','hist_draft')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In "build\$n.ppm" -Out "build\$n.png"
}
