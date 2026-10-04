# NovaOS NXP-loader hardening test: junk rejection, busy-slot no-overwrite,
# permission gating (non-root), all through the real shell paths.
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
sk "root"; Start-Sleep -Seconds 1

# 1) junk file: garbage content, run -> clean "not a .nxp"
sk "write junk.nxp"
sk "this is not a real program"
sk "."
sk "run junk.nxp"; Start-Sleep -Seconds 1

# 2) busy slot must NOT be overwritten: nsh occupies slot 0 and stays alive;
#    running hello.nxp (also slot 0) from inside nsh must be refused
sk "chmod ringbad.nxp 60"                      # others: no r, no x (for step 4)
sk "useradd u1"; sk "1"; sk "1"
sk "run nsh.nxp"; Start-Sleep -Seconds 3       # nsh running in slot 0
sk "run hello.nxp"; Start-Sleep -Seconds 2     # slot 0 busy -> refused
sk "run ringok.1.nxp"; Start-Sleep -Seconds 3  # slot 1 -> runs + exits
sk "exit"; Start-Sleep -Seconds 2              # leave nsh -> kernel shell
sk "logout"; Start-Sleep -Seconds 1

# 3) permission: u1 runs ringbad (owner root, mode 60 = no x for others)
sk "u1"; sk "1"; Start-Sleep -Seconds 1
sk "cd /"; Start-Sleep -Milliseconds 500
sk "run ringbad.nxp"; Start-Sleep -Seconds 1   # permission denied
sk "shutdown"
Start-Sleep -Seconds 3
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
Write-Host "===== nxp loader checks =====" -ForegroundColor Cyan
$checks = @(
    @{ n = 'junk rejected (not a .nxp)';      pat = 'not a \.nxp program' },
    @{ n = 'busy slot refused inside nsh';    pat = 'slot busy - that program is already running' },
    @{ n = 'other-slot ringok still ran';     pat = 'ring3 alive' },
    @{ n = 'u1 run without x denied';         pat = 'run: permission denied' }
)
$fail = 0
foreach ($k in $checks) {
    $hit = $log -match $k.pat
    if (-not $hit) { $fail++ }
    Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.n) -ForegroundColor $(if ($hit) {'Green'} else {'Yellow'})
}
if ($fail -eq 0) { Write-Host "NXPTEST: ALL PASS" -ForegroundColor Green }
else { Write-Host "NXPTEST: $fail FAILURES" -ForegroundColor Red }
