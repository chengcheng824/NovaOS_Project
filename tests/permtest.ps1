# NovaOS multi-user permission test (headless, sendkey over monitor 4444)
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
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus'; '/' = 'slash' }
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

sk "root"; sk "nova"; Start-Sleep -Seconds 1

# root creates a private file
sk "write secret.txt"
sk "top secret line"
sk "."
sk "chmod secret.txt 60"                      # owner rw, others none
sk "useradd u1"; sk "123"; sk "123"           # create user u1 / 123
sk "logout"; Start-Sleep -Seconds 1

# log in as u1 - everything below should be DENIED or allowed as noted
sk "u1"; sk "123"; Start-Sleep -Seconds 1
sk "whoami"
sk "cd /"
sk "cat secret.txt"                           # DENIED (no r for others)
sk "chmod secret.txt 66"                      # DENIED (not the owner)
sk "write secret.txt"                         # DENIED (not the owner)
sk "x"
sk "."
sk "run ringok.nxp"                           # ALLOWED (others-x on legacy files)
sk "cat nova.cfg"                             # not on this disk -> no such file (ok)
sk "rm secret.txt"                            # DENIED
sk "logout"; Start-Sleep -Seconds 1

# back as root - owner can read and chmod works both ways
sk "root"; sk "nova"; Start-Sleep -Seconds 1
sk "cat secret.txt"                           # ALLOWED (owner)
sk "ls"                                       # shows the mode column
sk "shutdown"

Start-Sleep -Seconds 3
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

# ---- assertions ----
$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
Write-Host "===== permission checks =====" -ForegroundColor Cyan
$checks = @(
    @{ n = 'root created + chmodded file';  pat = 'Mode set\.' },
    @{ n = 'useradd u1';                    pat = "User 'u1' created" },
    @{ n = 'u1 cat denied';                 pat = 'cat: permission denied' },
    @{ n = 'u1 chmod denied';               pat = 'chmod: permission denied \(not the owner\)' },
    @{ n = 'u1 write denied';               pat = 'Write failed: permission denied' },
    @{ n = 'u1 run allowed (ring3 alive)';  pat = 'ring3 alive' },
    @{ n = 'u1 rm denied';                  pat = 'rm: permission denied' },
    @{ n = 'root cat allowed';              pat = 'top secret line' }
)
$fail = 0
foreach ($k in $checks) {
    $hit = $log -match $k.pat
    if (-not $hit) { $fail++ }
    Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.n) -ForegroundColor $(if ($hit) {'Green'} else {'Red'})
}
Write-Host "===== ls output (mode column) =====" -ForegroundColor Cyan
$lines = $log -split "`n"
$inLs = $false; $shown = 0
foreach ($l in $lines) {
    if ($l -match 'root@novaos:/# ls') { $inLs = $true; continue }
    if ($inLs -and $l -match 'root@novaos') { $inLs = $false }
    elseif ($inLs -and $shown -lt 6) { Write-Host "  $l"; $shown++ }
}
if ($fail -eq 0) { Write-Host "PERMTEST: ALL PASS" -ForegroundColor Green }
else { Write-Host "PERMTEST: $fail FAILURES" -ForegroundColor Red }
