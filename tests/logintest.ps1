# NovaOS passwordless-root login test
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

# 1) root logs in with NO password prompt
sk "root"; Start-Sleep -Seconds 1
sk "whoami"
# 2) set a root password
sk "passwd"; sk "123"; sk "123"
sk "logout"; Start-Sleep -Seconds 1
# 3) root now REQUIRES the password
sk "root"; sk "123"; Start-Sleep -Seconds 1
sk "whoami"
# 4) create u1, u1 tries su root with a wrong password -> refused
sk "useradd u1"; sk "1"; sk "1"
sk "su u1"; Start-Sleep -Seconds 1
sk "su root"; Start-Sleep -Milliseconds 600
sk "zzz"                                  # wrong guess for root's password
Start-Sleep -Milliseconds 400
sk "whoami"
sk "logout"; Start-Sleep -Seconds 1
# 5) root clears password again (old 123, empty new twice)
sk "root"; sk "123"; Start-Sleep -Seconds 1
sk "passwd"; sk "123"; sk ""; sk ""
sk "logout"; Start-Sleep -Seconds 1
# 6) root is passwordless again (no prompt)
sk "root"; Start-Sleep -Seconds 1
sk "whoami"
sk "shutdown"
Start-Sleep -Seconds 3
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
Write-Host "===== login checks =====" -ForegroundColor Cyan
$checks = @(
    @{ n = 'no password prompt for root';   pat = 'NovaOS login: root\r?\nroot\r?\n\r?\nWelcome, root' },
    @{ n = 'passwd set ok';                 pat = 'Password updated' },
    @{ n = 'root+123 login works';          pat = 'Login incorrect' },
    @{ n = 'u1 created';                    pat = "User 'u1' created" },
    @{ n = 'su root refused (no backdoor)'; pat = 'su: wrong password' },
    @{ n = 'password removed';              pat = 'root password removed' },
    @{ n = 'passwordless again';            pat = 'NovaOS login: root\r?\nroot\r?\n\r?\nWelcome, root' }
)
$fail = 0
foreach ($k in $checks) {
    if ($k.n -eq 'root+123 login works') {
        $hit = ([regex]::Matches($log, 'Welcome, root')).Count -ge 2
    } else {
        $hit = $log -match $k.pat
    }
    if (-not $hit) { $fail++ }
    Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.n) -ForegroundColor $(if ($hit) {'Green'} else {'Red'})
}
if ($fail -eq 0) { Write-Host "LOGINTEST: ALL PASS" -ForegroundColor Green }
else { Write-Host "LOGINTEST: $fail FAILURES" -ForegroundColor Red }
