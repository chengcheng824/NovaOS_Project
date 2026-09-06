# NovaOS wget end-to-end test: host HTTP server + guest TCP fetch.
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)

$q = 'D:\qemu\qemu-system-i386.exe'
if (-not (Test-Path $q)) { $q = 'qemu-system-i386' }

Copy-Item data-seed.img data-nettest.img -Force
Remove-Item serial-nettest.log -Force -ErrorAction SilentlyContinue

# host-side HTTP server (127.0.0.1:8080, guest reaches it as 10.0.2.2:8080)
$host_ = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile','-ExecutionPolicy','Bypass','-File','tests\httphost.ps1'
)
Start-Sleep -Seconds 2

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
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus'; ':' = 'shift-semicolon'; '/' = 'slash' }
    foreach ($ch in $keys.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
        $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
        $s.Write($b, 0, $b.Length)
        Start-Sleep -Milliseconds 110
    }
    $b = [Text.Encoding]::ASCII.GetBytes("sendkey ret`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 300
}

sk "root"; sk "nova"; Start-Sleep -Seconds 1
sk "netinfo";          Start-Sleep -Seconds 5    # auto-dhcp
sk "wget 10.0.2.2:8080 /page.htm"; Start-Sleep -Seconds 20
sk "cat page.htm";     Start-Sleep -Seconds 3    # print the fetched body

$b2 = [Text.Encoding]::ASCII.GetBytes("quit`n")
$s.Write($b2, 0, $b2.Length)
Start-Sleep -Seconds 2
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }
if (-not $host_.HasExited) { $host_.Kill() }

Write-Host "===== httpserver.log =====" -ForegroundColor Cyan
Get-Content build\httpserver.log -ErrorAction SilentlyContinue
Write-Host "===== serial (wget/cat) =====" -ForegroundColor Cyan
$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
$lines = $log -split "`n"
$inCat = $false; $catLines = 0
foreach ($l in $lines) {
    if ($l -match 'wget|HTTP:|connect') { Write-Host $l }
    if ($l -match '^root@novaos:/# cat page.htm') { $inCat = $true; continue }
    if ($inCat) {
        if ($l -match '^root@novaos') { $inCat = $false }
        elseif ($catLines -lt 8) { Write-Host "  cat: $l"; $catLines++ }
    }
}
