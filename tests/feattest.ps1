# NovaOS feature subsystem end-to-end test (headless, monitor 4444)
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
function shot([string]$name) {
    Start-Sleep -Milliseconds 900
    $b = [Text.Encoding]::ASCII.GetBytes("screendump build\$name.ppm`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 900
}

sk "root"; sk "nova"; Start-Sleep -Seconds 1

sk "feature"                    # list (both modules, defaults enable)
sk "feature demo"               # hooks: both active (skeleton ready)
sk "feature disable holiday_module"
sk "feature demo"               # holiday hook now BLOCKED
sk "feature check cn_lang_support"
sk "cat /"                      # not a dir command; just fill
sk "run featureui.nxp"; Start-Sleep -Seconds 3
shot "feat_ui"                  # TUI screenshot
sk "down"; sk " "               # select cn_lang_support, toggle -> disable
shot "feat_ui_off"
sk "q"; Start-Sleep -Seconds 1
sk "feature demo"               # both hooks BLOCKED now
sk "feature"                    # list shows both disabled
sk "reboot"                     # persistence check across reboot
Start-Sleep -Seconds 14
sk "root"; sk "nova"; Start-Sleep -Seconds 1
sk "feature"                    # should still show disabled states
sk "feature enable holiday_module"
sk "feature demo"               # holiday back to skeleton-ready

$b = [Text.Encoding]::ASCII.GetBytes("quit`n")
$s.Write($b, 0, $b.Length)
Start-Sleep -Seconds 2
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

foreach ($n in @('feat_ui','feat_ui_off')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In "build\$n.ppm" -Out "build\$n.png"
}

# ---- assertions ----
$log = Get-Content serial-nettest.log -Raw -ErrorAction SilentlyContinue
Write-Host "===== feature checks =====" -ForegroundColor Cyan
$checks = @(
    @{ n = 'boot feat tag';            pat = '\[\s*OK\s*\] feat' },
    @{ n = 'initial list (both)';      pat = 'holiday_module\s+enable' },
    @{ n = 'cn listed';                pat = 'cn_lang_support\s+enable' },
    @{ n = 'hooks active (real logic)'; pat = 'cn_lang_support skeleton' },
    @{ n = 'disable persisted msg';    pat = 'disabled \(saved to /etc/features.conf\)' },
    @{ n = 'hook BLOCKED after off';   pat = 'BLOCKED \(module disabled\)' },
    @{ n = 'gate check cn';            pat = 'enabled' },
    @{ n = 'list after reboot (off)';  pat = 'holiday_module\s+disable' },
    @{ n = 're-enable after reboot';   pat = 'enabled \(saved to /etc/features.conf\)' }
)
$fail = 0
foreach ($k in $checks) {
    $hit = $log -match $k.pat
    if (-not $hit) { $fail++ }
    Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.n) -ForegroundColor $(if ($hit) {'Green'} else {'Red'})
}
if ($fail -eq 0) { Write-Host "FEATTEST: ALL PASS" -ForegroundColor Green }
else { Write-Host "FEATTEST: $fail FAILURES" -ForegroundColor Red }
