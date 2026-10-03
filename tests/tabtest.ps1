# NovaSh tab-completion test: command + filename + directory slash
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
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus'; '/' = 'slash'; '_' = 'shift-minus' }
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
function ty([string]$keys) {                  # type WITHOUT Enter (for tab)
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus'; '/' = 'slash'; '_' = 'shift-minus' }
    foreach ($ch in $keys.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
        $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
        $s.Write($b, 0, $b.Length)
        Start-Sleep -Milliseconds 100
    }
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

# 1) command completion: wh + tab -> whoami (runs, prints root)
ty "wh"; rawkey "tab"
shot "tab_cmd"
rawkey "ret"; Start-Sleep -Seconds 1

# 2) multi-match LCP: cat he + tab -> cat hello. (hello.nxp/.1/.2/.3)
ty "cat he"; rawkey "tab"
shot "tab_file"
rawkey "ret"; Start-Sleep -Seconds 1

# 3) directory slash: mkdir docs, then cd do + tab -> cd docs/
sk "mkdir docs"; Start-Sleep -Seconds 1
ty "cd do"; rawkey "tab"
shot "tab_dir"
rawkey "ret"; Start-Sleep -Seconds 1
sk "shutdown"
Start-Sleep -Seconds 3
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
Write-Host "===== tab checks =====" -ForegroundColor Cyan
$checks = @(
    @{ n = 'whoami completed + ran (no unknown cmd)'; pat = 'whoami ' },
    @{ n = 'multi-match LCP completed (cat hello.)';  pat = 'cat hello\.' },
    @{ n = 'dir completed with slash (cd docs/)';     pat = 'cd docs/' }
)
foreach ($k in $checks) {
    $hit = $log -match $k.pat
    Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.n) -ForegroundColor $(if ($hit) {'Green'} else {'Yellow'})
}
foreach ($n in @('tab_cmd','tab_file','tab_dir')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In "build\$n.ppm" -Out "build\$n.png"
}
